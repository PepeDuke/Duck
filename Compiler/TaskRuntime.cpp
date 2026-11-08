#include "Pipeline.h"

namespace duck {
// Worker tasks for calls with known effects (Windows x64 threads).
// DUCK_WORKERS limits how many threads may run Duck code at once, including
// the caller; it defaults to the active processor count. When the limit is
// reached, or a thread cannot be created, the task runs inline on the caller,
// so nested spawns never wait for a free worker. DUCK_WORKERS=1 runs every
// task inline, in source order. A task record lives in the spawning frame;
// the compiler joins it before that frame can exit.
std::string taskRuntimeIR(const std::set<std::string> &nativeSymbols) {
  std::string ir;
  // Call sites state complete function types, so an existing program
  // declaration of the same symbol can be reused.
  const std::pair<const char *, const char *> imports[] = {
      {"CreateThread", "declare ptr @CreateThread(ptr, i64, ptr, ptr, i32, ptr)\n"},
      {"WaitForSingleObject", "declare i32 @WaitForSingleObject(ptr, i32)\n"},
      {"CloseHandle", "declare i32 @CloseHandle(ptr)\n"},
      {"GetActiveProcessorCount", "declare i32 @GetActiveProcessorCount(i16)\n"},
      {"getenv", "declare ptr @getenv(ptr)\n"},
      {"atoi", "declare i32 @atoi(ptr)\n"}};
  for (const auto &[symbol, declaration] : imports)
    if (!nativeSymbols.count(symbol)) ir += declaration;
  return ir + R"IR(
@duck.task.limit = internal global i32 0
@duck.task.active = internal global i32 0
@duck.task.variable = private unnamed_addr constant [13 x i8] c"DUCK_WORKERS\00"

define internal i32 @duck.task$limit() {
entry:
  %cached = load atomic i32, ptr @duck.task.limit monotonic, align 4
  %known = icmp sgt i32 %cached, 0
  br i1 %known, label %done, label %read
read:
  %text = call ptr @getenv(ptr @duck.task.variable)
  %unset = icmp eq ptr %text, null
  br i1 %unset, label %hardware, label %parse
parse:
  %requested = call i32 @atoi(ptr %text)
  %valid = icmp sgt i32 %requested, 0
  br i1 %valid, label %store, label %hardware
hardware:
  %cores = call i32 @GetActiveProcessorCount(i16 -1)
  %some = icmp sgt i32 %cores, 0
  %count = select i1 %some, i32 %cores, i32 1
  br label %store
store:
  %limit = phi i32 [ %requested, %parse ], [ %count, %hardware ]
  store atomic i32 %limit, ptr @duck.task.limit monotonic, align 4
  ret i32 %limit
done:
  ret i32 %cached
}

; Returns the thread handle, or null when the task already ran inline.
define internal ptr @duck.task$spawn(ptr %function, ptr %task) {
entry:
  ; A thread holding ${ } locks must not wait on work that may need them.
  %ordered = load i32, ptr @duck$lock.mode
  %locked = icmp ne i32 %ordered, 0
  br i1 %locked, label %direct, label %measure
measure:
  %limit = call i32 @duck.task$limit()
  ; Saturated workers are the common case in recursive work; check without writing.
  %seen = load atomic i32, ptr @duck.task.active monotonic, align 4
  %projected = add i32 %seen, 2
  %room = icmp sle i32 %projected, %limit
  br i1 %room, label %claim, label %direct
direct:
  %result = call i32 %function(ptr %task)
  ret ptr null
claim:
  %previous = atomicrmw add ptr @duck.task.active, i32 1 seq_cst
  %running = add i32 %previous, 2
  %allowed = icmp sle i32 %running, %limit
  br i1 %allowed, label %create, label %inline
create:
  %handle = call ptr @CreateThread(ptr null, i64 0, ptr %function, ptr %task, i32 0, ptr null)
  %created = icmp ne ptr %handle, null
  br i1 %created, label %started, label %inline
started:
  ret ptr %handle
inline:
  %undo = atomicrmw sub ptr @duck.task.active, i32 1 seq_cst
  %ignored = call i32 %function(ptr %task)
  ret ptr null
}

; Waiting for thread exit orders the worker's result store before the caller's load.
define internal void @duck.task$join(ptr %handle) {
entry:
  %inline = icmp eq ptr %handle, null
  br i1 %inline, label %done, label %wait
wait:
  %status = call i32 @WaitForSingleObject(ptr %handle, i32 -1)
  %failed = icmp ne i32 %status, 0
  br i1 %failed, label %trap, label %close
trap:
  call void @llvm.trap()
  unreachable
close:
  %closed = call i32 @CloseHandle(ptr %handle)
  %finished = atomicrmw sub ptr @duck.task.active, i32 1 seq_cst
  br label %done
done:
  ret void
}
)IR";
}

// The locks behind `${ }` blocks (Windows slim reader/writer locks, which need
// no setup). A block hashes each resource's address to one of 64 stripes and
// takes them in ascending order, so two blocks can never wait on each other.
// Every block also takes one scheduler-wide lock: shared when it named its
// resources, exclusive when it could not, so the latter excludes all others.
//
// The transitive resource set of a block covers its callees, so a nested block
// finds its stripes already held and has nothing to acquire; everything is
// released when the outermost block exits. State is shared across modules.
std::string lockRuntimeIR(const std::set<std::string> &nativeSymbols) {
  std::string ir;
  const std::pair<const char *, const char *> imports[] = {
      {"AcquireSRWLockExclusive", "declare void @AcquireSRWLockExclusive(ptr)\n"},
      {"ReleaseSRWLockExclusive", "declare void @ReleaseSRWLockExclusive(ptr)\n"},
      {"AcquireSRWLockShared", "declare void @AcquireSRWLockShared(ptr)\n"},
      {"ReleaseSRWLockShared", "declare void @ReleaseSRWLockShared(ptr)\n"}};
  for (const auto &[symbol, declaration] : imports)
    if (!nativeSymbols.count(symbol)) ir += declaration;
  return ir + R"IR(
declare i64 @llvm.cttz.i64(i64, i1)
$duck$lock.global = comdat any
$duck$lock.stripes = comdat any
$duck$lock.mode = comdat any
$duck$lock.held = comdat any
@duck$lock.global = linkonce_odr global ptr null, comdat
@duck$lock.stripes = linkonce_odr global [64 x ptr] zeroinitializer, comdat
; 0 outside any block, 1 holding stripes under the shared lock, 2 exclusive.
@duck$lock.mode = linkonce_odr thread_local global i32 0, comdat
@duck$lock.held = linkonce_odr thread_local global i64 0, comdat

define internal i64 @duck.lock$bit(ptr %address) {
entry:
  %bits = ptrtoint ptr %address to i64
  %word = lshr i64 %bits, 3
  %mixed = mul i64 %word, -7046029254386353131
  %index = lshr i64 %mixed, 58
  %bit = shl i64 1, %index
  ret i64 %bit
}

define internal void @duck.lock$acquire(i64 %mask) {
entry:
  br label %test
test:
  %left = phi i64 [ %mask, %entry ], [ %rest, %take ]
  %more = icmp ne i64 %left, 0
  br i1 %more, label %take, label %done
take:
  %index = call i64 @llvm.cttz.i64(i64 %left, i1 true)
  %stripe = getelementptr [64 x ptr], ptr @duck$lock.stripes, i64 0, i64 %index
  call void @AcquireSRWLockExclusive(ptr %stripe)
  %less = sub i64 %left, 1
  %rest = and i64 %left, %less
  br label %test
done:
  ret void
}

define internal void @duck.lock$release(i64 %mask) {
entry:
  br label %test
test:
  %left = phi i64 [ %mask, %entry ], [ %rest, %drop ]
  %more = icmp ne i64 %left, 0
  br i1 %more, label %drop, label %done
drop:
  %index = call i64 @llvm.cttz.i64(i64 %left, i1 true)
  %stripe = getelementptr [64 x ptr], ptr @duck$lock.stripes, i64 0, i64 %index
  call void @ReleaseSRWLockExclusive(ptr %stripe)
  %less = sub i64 %left, 1
  %rest = and i64 %left, %less
  br label %test
done:
  ret void
}

; Returns 1 for the outermost block, which is the one that releases.
define internal i32 @duck.lock$enter(i64 %mask, i1 %exclusive) {
entry:
  %mode = load i32, ptr @duck$lock.mode
  switch i32 %mode, label %within [ i32 0, label %outermost
                                    i32 2, label %covered ]
covered:
  ret i32 0
outermost:
  br i1 %exclusive, label %alone, label %named
alone:
  call void @AcquireSRWLockExclusive(ptr @duck$lock.global)
  store i32 2, ptr @duck$lock.mode
  ret i32 1
named:
  call void @AcquireSRWLockShared(ptr @duck$lock.global)
  store i32 1, ptr @duck$lock.mode
  call void @duck.lock$acquire(i64 %mask)
  store i64 %mask, ptr @duck$lock.held
  ret i32 1
within:
  ; An enclosing block's resource set covers its callees, exclusion included.
  br i1 %exclusive, label %invariant, label %subset
invariant:
  call void @llvm.trap()
  unreachable
subset:
  %held = load i64, ptr @duck$lock.held
  %inverse = xor i64 %held, -1
  %needed = and i64 %mask, %inverse
  %none = icmp eq i64 %needed, 0
  br i1 %none, label %covered, label %grow
grow:
  call void @duck.lock$acquire(i64 %needed)
  %wider = or i64 %held, %needed
  store i64 %wider, ptr @duck$lock.held
  ret i32 0
}

define internal void @duck.lock$exit(i32 %outermost) {
entry:
  %last = icmp ne i32 %outermost, 0
  br i1 %last, label %leave, label %done
leave:
  %mode = load i32, ptr @duck$lock.mode
  store i32 0, ptr @duck$lock.mode
  %alone = icmp eq i32 %mode, 2
  br i1 %alone, label %whole, label %stripes
whole:
  call void @ReleaseSRWLockExclusive(ptr @duck$lock.global)
  ret void
stripes:
  %held = load i64, ptr @duck$lock.held
  store i64 0, ptr @duck$lock.held
  call void @duck.lock$release(i64 %held)
  call void @ReleaseSRWLockShared(ptr @duck$lock.global)
  ret void
done:
  ret void
}
)IR";
}
} // namespace duck
