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
      {"atoi", "declare i32 @atoi(ptr)\n"},
      {"atexit", "declare i32 @atexit(ptr)\n"}};

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

; Tasks started by P{ }. Each always gets a thread of its own: it never runs
; inline and does not count against DUCK_WORKERS, since its body may loop
; until asked to stop. A record is
;   { i32 finished, i32 stop requested, ptr thread handle, ptr body, ptr env, ptr next }
; and stays linked in one program-wide list until its handle is dropped, so
; the program can stop and wait for every task still running when it exits.
%duck.detached = type { i32, i32, ptr, ptr, ptr, ptr }
$duck.detached.list = comdat any
$duck.detached.lock = comdat any
$duck.detached.exit = comdat any
$duck$detached.self = comdat any
@duck.detached.list = linkonce_odr global ptr null, comdat
@duck.detached.lock = linkonce_odr global ptr null, comdat
@duck.detached.exit = linkonce_odr global i32 0, comdat
; The record of the task running on this thread, or null.
@duck$detached.self = linkonce_odr thread_local global ptr null, comdat

define internal i32 @duck.detached$run(ptr %task) {
entry:
  store ptr %task, ptr @duck$detached.self
  %f = getelementptr %duck.detached, ptr %task, i32 0, i32 3
  %e = getelementptr %duck.detached, ptr %task, i32 0, i32 4
  %fn = load ptr, ptr %f
  %env = load ptr, ptr %e
  ; A scheduler slot makes the task visible to P.Thread[i].Status.
  %slot = call ptr @duck.thread$reserve(ptr %fn, ptr %env)
  %tracked = icmp ne ptr %slot, null
  br i1 %tracked, label %enter, label %body
enter:
  store ptr %slot, ptr @duck$thread.self
  store atomic i32 1, ptr %slot release, align 4
  br label %body
body:
  %ignored = call i32 %fn(ptr %env)
  br i1 %tracked, label %leave, label %finish
leave:
  store ptr null, ptr @duck$thread.self
  store atomic i32 0, ptr %slot release, align 4
  br label %finish
finish:
  store ptr null, ptr @duck$detached.self
  store atomic i32 1, ptr %task release, align 4
  ret i32 0
}

define internal ptr @duck.detached$start(ptr %fn, ptr %env) {
entry:
  %size = ptrtoint ptr getelementptr (%duck.detached, ptr null, i32 1) to i64
  %task = call ptr @malloc(i64 %size)
  %none = icmp eq ptr %task, null
  br i1 %none, label %fail, label %fill
fill:
  store %duck.detached zeroinitializer, ptr %task
  %f = getelementptr %duck.detached, ptr %task, i32 0, i32 3
  %e = getelementptr %duck.detached, ptr %task, i32 0, i32 4
  store ptr %fn, ptr %f
  store ptr %env, ptr %e
  ; Whatever is still running when Main returns is stopped and waited for.
  %registered = atomicrmw xchg ptr @duck.detached.exit, i32 1 seq_cst
  %first = icmp eq i32 %registered, 0
  br i1 %first, label %hook, label %link
hook:
  %hooked = call i32 @atexit(ptr @duck.detached$exit)
  br label %link
link:
  call void @AcquireSRWLockExclusive(ptr @duck.detached.lock)
  %head = load ptr, ptr @duck.detached.list
  %n = getelementptr %duck.detached, ptr %task, i32 0, i32 5
  store ptr %head, ptr %n
  store ptr %task, ptr @duck.detached.list
  call void @ReleaseSRWLockExclusive(ptr @duck.detached.lock)
  %handle = call ptr @CreateThread(ptr null, i64 0, ptr @duck.detached$run, ptr %task, i32 0, ptr null)
  %created = icmp ne ptr %handle, null
  br i1 %created, label %started, label %fail
started:
  %h = getelementptr %duck.detached, ptr %task, i32 0, i32 2
  store atomic ptr %handle, ptr %h release, align 8
  ret ptr %task
fail:
  call void @llvm.trap()
  unreachable
}

; t.Stop(): asks the task to stop. Its body sees P.Stopping become true.
define internal void @duck.detached$stop(ptr %task) {
entry:
  %none = icmp eq ptr %task, null
  br i1 %none, label %done, label %ask
ask:
  %s = getelementptr %duck.detached, ptr %task, i32 0, i32 1
  store atomic i32 1, ptr %s release, align 4
  br label %done
done:
  ret void
}

; t.Done: true once the body has returned, and for a task that holds nothing.
define internal i1 @duck.detached$done(ptr %task) {
entry:
  %none = icmp eq ptr %task, null
  br i1 %none, label %yes, label %read
read:
  %state = load atomic i32, ptr %task acquire, align 4
  %finished = icmp ne i32 %state, 0
  ret i1 %finished
yes:
  ret i1 1
}

; P.Stopping: false outside a task.
define internal i1 @duck.detached$stopping() {
entry:
  %task = load ptr, ptr @duck$detached.self
  %none = icmp eq ptr %task, null
  br i1 %none, label %no, label %read
read:
  %s = getelementptr %duck.detached, ptr %task, i32 0, i32 1
  %asked = load atomic i32, ptr %s acquire, align 4
  %stopping = icmp ne i32 %asked, 0
  ret i1 %stopping
no:
  ret i1 0
}

; t.Join(): waits until the body has returned. A task cannot wait for itself,
; and a thread inside ${ } must not wait on work that may need its locks.
define internal void @duck.detached$join(ptr %task) {
entry:
  %none = icmp eq ptr %task, null
  br i1 %none, label %done, label %check
check:
  %self = load ptr, ptr @duck$detached.self
  %itself = icmp eq ptr %self, %task
  %mode = load i32, ptr @duck$lock.mode
  %locked = icmp ne i32 %mode, 0
  %invalid = or i1 %itself, %locked
  br i1 %invalid, label %trap, label %finished
finished:
  %state = load atomic i32, ptr %task acquire, align 4
  %over = icmp ne i32 %state, 0
  br i1 %over, label %done, label %wait
wait:
  %h = getelementptr %duck.detached, ptr %task, i32 0, i32 2
  %handle = load atomic ptr, ptr %h acquire, align 8
  call void @duck.thread$mark(i32 2)
  %status = call i32 @WaitForSingleObject(ptr %handle, i32 -1)
  call void @duck.thread$mark(i32 1)
  %failed = icmp ne i32 %status, 0
  br i1 %failed, label %trap, label %done
trap:
  call void @llvm.trap()
  unreachable
done:
  ret void
}

; Ends a task's handle: asks it to stop, waits for it, and frees its record.
define internal void @duck.detached$drop(ptr %task) {
entry:
  %none = icmp eq ptr %task, null
  br i1 %none, label %done, label %end
end:
  call void @duck.detached$stop(ptr %task)
  call void @duck.detached$join(ptr %task)
  ; Wait for the thread itself too, so its handle can be closed.
  %h = getelementptr %duck.detached, ptr %task, i32 0, i32 2
  %handle = load atomic ptr, ptr %h acquire, align 8
  %waited = call i32 @WaitForSingleObject(ptr %handle, i32 -1)
  call void @AcquireSRWLockExclusive(ptr @duck.detached.lock)
  br label %scan
scan:
  %link = phi ptr [ @duck.detached.list, %end ], [ %next, %advance ]
  %at = load ptr, ptr %link
  %missing = icmp eq ptr %at, null
  br i1 %missing, label %unlinked, label %compare
compare:
  %found = icmp eq ptr %at, %task
  br i1 %found, label %unlink, label %advance
advance:
  %next = getelementptr %duck.detached, ptr %at, i32 0, i32 5
  br label %scan
unlink:
  %n = getelementptr %duck.detached, ptr %task, i32 0, i32 5
  %after = load ptr, ptr %n
  store ptr %after, ptr %link
  br label %unlinked
unlinked:
  call void @ReleaseSRWLockExclusive(ptr @duck.detached.lock)
  %closed = call i32 @CloseHandle(ptr %handle)
  call void @free(ptr %task)
  br label %done
done:
  ret void
}

; Drops the task held in a variable or field, leaving it null.
define internal void @duck.detached$dropSlot(ptr %slot) {
  %task = load ptr, ptr %slot
  store ptr null, ptr %slot
  call void @duck.detached$drop(ptr %task)
  ret void
}

; At exit: asks every task still running to stop, then waits until each body
; has returned, so none is cut off mid-step. Records are only read here, under
; the list lock; a task's owner may still drop it meanwhile, which unlinks it
; under the same lock first. Handles in static fields are never dropped, and
; end here.
define internal void @duck.detached$exit() {
entry:
  call void @AcquireSRWLockExclusive(ptr @duck.detached.lock)
  br label %ask
ask:
  %from = phi ptr [ @duck.detached.list, %entry ], [ %link, %asked ]
  %each = load ptr, ptr %from
  %end = icmp eq ptr %each, null
  br i1 %end, label %asking, label %asked
asked:
  call void @duck.detached$stop(ptr %each)
  %link = getelementptr %duck.detached, ptr %each, i32 0, i32 5
  br label %ask
asking:
  call void @ReleaseSRWLockExclusive(ptr @duck.detached.lock)
  br label %find
find:
  ; The first task still running, other than the one calling exit, if any.
  call void @AcquireSRWLockExclusive(ptr @duck.detached.lock)
  %self = load ptr, ptr @duck$detached.self
  br label %scan
scan:
  %at = phi ptr [ @duck.detached.list, %find ], [ %next, %skip ]
  %task = load ptr, ptr %at
  %none = icmp eq ptr %task, null
  br i1 %none, label %finished, label %inspect
inspect:
  %next = getelementptr %duck.detached, ptr %task, i32 0, i32 5
  %state = load atomic i32, ptr %task acquire, align 4
  %over = icmp ne i32 %state, 0
  %itself = icmp eq ptr %task, %self
  %pass = or i1 %over, %itself
  br i1 %pass, label %skip, label %wait
skip:
  br label %scan
wait:
  %h = getelementptr %duck.detached, ptr %task, i32 0, i32 2
  %handle = load atomic ptr, ptr %h acquire, align 8
  call void @ReleaseSRWLockExclusive(ptr @duck.detached.lock)
  %status = call i32 @WaitForSingleObject(ptr %handle, i32 -1)
  br label %find
finished:
  call void @ReleaseSRWLockExclusive(ptr @duck.detached.lock)
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
