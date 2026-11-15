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
$duck.task.limit = comdat any
$duck.task.active = comdat any
$duck.thread.registryLock = comdat any
@duck.task.limit = linkonce_odr global i32 0, comdat
@duck.task.active = linkonce_odr global i32 0, comdat
@duck.thread.registryLock = linkonce_odr global ptr null, comdat
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

; Slots stay at stable addresses until process exit. Only allocation and reuse
; take the registry lock. Readers follow atomically published next links.
define internal ptr @duck.thread$reserve(ptr %function, ptr %task) {
entry:
  call void @AcquireSRWLockExclusive(ptr @duck.thread.registryLock)
  br label %scan
scan:
  %previous = phi ptr [ @duck$thread.main, %entry ], [ %next, %inspect ]
  %link = getelementptr %duck.thread.slot, ptr %previous, i32 0, i32 1
  %next = load atomic ptr, ptr %link acquire, align 8
  %end = icmp eq ptr %next, null
  br i1 %end, label %allocate, label %inspect
inspect:
  %state = load atomic i32, ptr %next acquire, align 4
  %idle = icmp eq i32 %state, 0
  br i1 %idle, label %reuse, label %scan
allocate:
  %size = ptrtoint ptr getelementptr (%duck.thread.slot, ptr null, i32 1) to i64
  %new = call ptr @malloc(i64 %size)
  %failed = icmp eq ptr %new, null
  br i1 %failed, label %done, label %publish
publish:
  store %duck.thread.slot { i32 4, ptr null, ptr null, ptr null }, ptr %new
  store atomic ptr %new, ptr %link release, align 8
  br label %bind
reuse:
  store atomic i32 4, ptr %next release, align 4
  br label %bind
bind:
  %slot = phi ptr [ %new, %publish ], [ %next, %reuse ]
  %fn = getelementptr %duck.thread.slot, ptr %slot, i32 0, i32 2
  %arg = getelementptr %duck.thread.slot, ptr %slot, i32 0, i32 3
  store ptr %function, ptr %fn
  store ptr %task, ptr %arg
  br label %done
done:
  %result = phi ptr [ null, %allocate ], [ %slot, %bind ]
  call void @ReleaseSRWLockExclusive(ptr @duck.thread.registryLock)
  ret ptr %result
}

define internal i32 @duck.thread$run(ptr %slot) {
  store ptr %slot, ptr @duck$thread.self
  %f = getelementptr %duck.thread.slot, ptr %slot, i32 0, i32 2
  %a = getelementptr %duck.thread.slot, ptr %slot, i32 0, i32 3
  %fn = load ptr, ptr %f
  %arg = load ptr, ptr %a
  store atomic i32 1, ptr %slot release, align 4
  %result = call i32 %fn(ptr %arg)
  store ptr null, ptr @duck$thread.self
  store atomic i32 0, ptr %slot release, align 4
  ret i32 %result
}

; Sample each slot once. Mode 0 counts slots, 1 live threads, 2 running.
define internal i32 @duck.thread$count(i32 %mode) {
entry:
  br label %scan
scan:
  %slot = phi ptr [ @duck$thread.main, %entry ], [ %next, %scan ]
  %count = phi i32 [ 0, %entry ], [ %total, %scan ]
  %state = load atomic i32, ptr %slot acquire, align 4
  %running = icmp eq i32 %state, 1
  %positive = icmp uge i32 %state, 1
  %started = icmp ule i32 %state, 3
  %live = and i1 %positive, %started
  %all = icmp eq i32 %mode, 0
  %onlyLive = icmp eq i32 %mode, 1
  %matched = select i1 %onlyLive, i1 %live, i1 %running
  %included = or i1 %all, %matched
  %add = zext i1 %included to i32
  %total = add i32 %count, %add
  %link = getelementptr %duck.thread.slot, ptr %slot, i32 0, i32 1
  %next = load atomic ptr, ptr %link acquire, align 8
  %end = icmp eq ptr %next, null
  br i1 %end, label %done, label %scan
done:
  ret i32 %total
}

define internal i32 @duck.thread$status(i32 %index) {
entry:
  %negative = icmp slt i32 %index, 0
  br i1 %negative, label %invalid, label %scan
scan:
  %i = phi i32 [ 0, %entry ], [ %following, %advance ]
  %slot = phi ptr [ @duck$thread.main, %entry ], [ %next, %advance ]
  %found = icmp eq i32 %i, %index
  br i1 %found, label %read, label %advance
advance:
  %link = getelementptr %duck.thread.slot, ptr %slot, i32 0, i32 1
  %next = load atomic ptr, ptr %link acquire, align 8
  %end = icmp eq ptr %next, null
  %following = add i32 %i, 1
  br i1 %end, label %invalid, label %scan
read:
  %state = load atomic i32, ptr %slot acquire, align 4
  ret i32 %state
invalid:
  call void @llvm.trap()
  unreachable
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
  %slot = call ptr @duck.thread$reserve(ptr %function, ptr %task)
  %reserved = icmp ne ptr %slot, null
  br i1 %reserved, label %launch, label %inline
launch:
  %handle = call ptr @CreateThread(ptr null, i64 0, ptr @duck.thread$run, ptr %slot, i32 0, ptr null)
  %created = icmp ne ptr %handle, null
  br i1 %created, label %started, label %cancel
cancel:
  store atomic i32 0, ptr %slot release, align 4
  br label %inline
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
  call void @duck.thread$mark(i32 2)
  %status = call i32 @WaitForSingleObject(ptr %handle, i32 -1)
  call void @duck.thread$mark(i32 1)
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

define internal void @duck.thread$mark(i32 %state) {
  %slot = load ptr, ptr @duck$thread.self
  %tracked = icmp ne ptr %slot, null
  br i1 %tracked, label %mark, label %done
mark:
  store atomic i32 %state, ptr %slot release, align 4
  br label %done
done:
  ret void
}

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
  call void @duck.thread$mark(i32 3)
  br i1 %exclusive, label %alone, label %named
alone:
  call void @AcquireSRWLockExclusive(ptr @duck$lock.global)
  call void @duck.thread$mark(i32 1)
  store i32 2, ptr @duck$lock.mode
  ret i32 1
named:
  call void @AcquireSRWLockShared(ptr @duck$lock.global)
  store i32 1, ptr @duck$lock.mode
  call void @duck.lock$acquire(i64 %mask)
  call void @duck.thread$mark(i32 1)
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
  call void @duck.thread$mark(i32 3)
  call void @duck.lock$acquire(i64 %needed)
  call void @duck.thread$mark(i32 1)
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
