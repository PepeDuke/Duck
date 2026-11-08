#include "Pipeline.h"

namespace duck {
// A named lifetime scope. Every module refers to one descriptor per name, so
// the descriptors and the shared state below merge when modules link.
//
// The generation is even while the scope is inactive and odd while it is
// active; each @start or @end advances it. A scoped variable's slot records
// where its value lives and the generation that value belongs to, so a slot
// from an older generation is simply stale: the first use in a new generation
// allocates the variable in the arena and gives it its type's default. That
// covers instances created after @start with no registry of instances.
//
// @end runs the registered cleanups, newest first, while the variables are
// still readable, then advances the generation and frees the arena. A repeated
// @start does the same but leaves the scope active. @end on an inactive scope
// traps. A scope active at program exit is closed by an atexit handler.
//
// Scope state is process-wide. The start, end, and first-use paths take the
// descriptor's lock, and scheduled tasks never touch scopes, because a function
// using one is never isolated. Only code on the thread running a [@Name] loop
// may @end that scope; other threads must coordinate their own use with @end.
std::string scopeRuntimeIR(const std::set<std::string> &nativeSymbols) {
  std::string ir = nativeSymbols.count("atexit") ? "" : "declare i32 @atexit(ptr)\n";
  return ir + R"IR(
%duck.scope = type { i64, i32, i32, ptr, ptr, ptr, i32 }
%duck.scope.chunk = type { ptr, i64, i64, i64 }
%duck.scope.cleanup = type { ptr, ptr, ptr }

$duck$scope.registry = comdat any
$duck$scope.unwind = comdat any
$duck$scope.hooked = comdat any
@duck$scope.registry = linkonce_odr global ptr null, comdat
@duck$scope.unwind = linkonce_odr global ptr null, comdat
@duck$scope.hooked = linkonce_odr global i32 0, comdat

define internal void @duck.scope$lock(ptr %scope) {
entry:
  %lock = getelementptr %duck.scope, ptr %scope, i32 0, i32 1
  br label %spin
spin:
  %held = atomicrmw xchg ptr %lock, i32 1 acquire
  %taken = icmp eq i32 %held, 0
  br i1 %taken, label %done, label %spin
done:
  ret void
}

define internal void @duck.scope$unlock(ptr %scope) {
  %lock = getelementptr %duck.scope, ptr %scope, i32 0, i32 1
  store atomic i32 0, ptr %lock release, align 4
  ret void
}

; Bump allocation from the scope's arena. The caller holds the lock.
define internal ptr @duck.scope$alloc(ptr %scope, i64 %bytes) {
entry:
  %rounded = add i64 %bytes, 15
  %size = and i64 %rounded, -16
  %chunks = getelementptr %duck.scope, ptr %scope, i32 0, i32 3
  %head = load ptr, ptr %chunks
  %none = icmp eq ptr %head, null
  br i1 %none, label %grow, label %check
check:
  %usedSlot = getelementptr %duck.scope.chunk, ptr %head, i32 0, i32 1
  %used = load i64, ptr %usedSlot
  %capacitySlot = getelementptr %duck.scope.chunk, ptr %head, i32 0, i32 2
  %capacity = load i64, ptr %capacitySlot
  %after = add i64 %used, %size
  %fits = icmp ule i64 %after, %capacity
  br i1 %fits, label %take, label %grow
grow:
  %large = icmp ugt i64 %size, 65536
  %wanted = select i1 %large, i64 %size, i64 65536
  %total = add i64 %wanted, 32
  %chunk = call ptr @malloc(i64 %total)
  %failed = icmp eq ptr %chunk, null
  br i1 %failed, label %trap, label %link
trap:
  call void @llvm.trap()
  unreachable
link:
  store ptr %head, ptr %chunk
  %newUsed = getelementptr %duck.scope.chunk, ptr %chunk, i32 0, i32 1
  store i64 %size, ptr %newUsed
  %newCapacity = getelementptr %duck.scope.chunk, ptr %chunk, i32 0, i32 2
  store i64 %wanted, ptr %newCapacity
  store ptr %chunk, ptr %chunks
  %first = getelementptr i8, ptr %chunk, i64 32
  ret ptr %first
take:
  store i64 %after, ptr %usedSlot
  %offset = add i64 %used, 32
  %address = getelementptr i8, ptr %head, i64 %offset
  ret ptr %address
}

define internal void @duck.scope$bytes(ptr %to, ptr %from, i64 %n) {
entry:
  %zero = icmp eq ptr %from, null
  br label %loop
loop:
  %i = phi i64 [ 0, %entry ], [ %following, %store ]
  %more = icmp ult i64 %i, %n
  br i1 %more, label %read, label %done
read:
  br i1 %zero, label %store, label %load
load:
  %source = getelementptr i8, ptr %from, i64 %i
  %loaded = load i8, ptr %source
  br label %store
store:
  %value = phi i8 [ 0, %read ], [ %loaded, %load ]
  %target = getelementptr i8, ptr %to, i64 %i
  store i8 %value, ptr %target
  %following = add i64 %i, 1
  br label %loop
done:
  ret void
}

; The storage of a scoped variable in the current generation, created on first
; use. Outside the scope it traps, or with guarded gives null.
define internal ptr @duck.scope$data(ptr %scope, ptr %slot, i64 %size, ptr %initial, ptr %cleanup, i1 %guarded) {
entry:
  %generation = load atomic i64, ptr %scope acquire, align 8
  %bit = and i64 %generation, 1
  %active = icmp eq i64 %bit, 1
  br i1 %active, label %current, label %inactive
inactive:
  br i1 %guarded, label %none, label %trap
none:
  ret ptr null
trap:
  call void @llvm.trap()
  unreachable
current:
  %storedSlot = getelementptr [2 x i64], ptr %slot, i32 0, i32 1
  %stored = load i64, ptr %storedSlot
  %fresh = icmp eq i64 %stored, %generation
  br i1 %fresh, label %ready, label %create
ready:
  %existing = load ptr, ptr %slot
  ret ptr %existing
create:
  call void @duck.scope$lock(ptr %scope)
  %again = load i64, ptr %storedSlot
  %raced = icmp eq i64 %again, %generation
  br i1 %raced, label %release, label %build
release:
  call void @duck.scope$unlock(ptr %scope)
  %winner = load ptr, ptr %slot
  ret ptr %winner
build:
  %data = call ptr @duck.scope$alloc(ptr %scope, i64 %size)
  call void @duck.scope$bytes(ptr %data, ptr %initial, i64 %size)
  %plain = icmp eq ptr %cleanup, null
  br i1 %plain, label %publish, label %register
register:
  %entry0 = call ptr @duck.scope$alloc(ptr %scope, i64 24)
  %cleanups = getelementptr %duck.scope, ptr %scope, i32 0, i32 4
  %previous = load ptr, ptr %cleanups
  store ptr %previous, ptr %entry0
  %functionSlot = getelementptr %duck.scope.cleanup, ptr %entry0, i32 0, i32 1
  store ptr %cleanup, ptr %functionSlot
  %dataSlot = getelementptr %duck.scope.cleanup, ptr %entry0, i32 0, i32 2
  store ptr %data, ptr %dataSlot
  store ptr %entry0, ptr %cleanups
  br label %publish
publish:
  store ptr %data, ptr %slot
  store i64 %generation, ptr %storedSlot
  call void @duck.scope$unlock(ptr %scope)
  ret ptr %data
}

; Cleans up the current generation's variables, then advances by steps.
define internal void @duck.scope$close(ptr %scope, i64 %steps) {
entry:
  %cleanups = getelementptr %duck.scope, ptr %scope, i32 0, i32 4
  br label %clean
clean:
  %head = load ptr, ptr %cleanups
  %empty = icmp eq ptr %head, null
  br i1 %empty, label %advance, label %run
run:
  %rest = load ptr, ptr %head
  store ptr %rest, ptr %cleanups
  %functionSlot = getelementptr %duck.scope.cleanup, ptr %head, i32 0, i32 1
  %function = load ptr, ptr %functionSlot
  %dataSlot = getelementptr %duck.scope.cleanup, ptr %head, i32 0, i32 2
  %data = load ptr, ptr %dataSlot
  call void %function(ptr %data)
  br label %clean
advance:
  call void @duck.scope$lock(ptr %scope)
  %old = atomicrmw add ptr %scope, i64 %steps seq_cst
  %chunks = getelementptr %duck.scope, ptr %scope, i32 0, i32 3
  %first = load ptr, ptr %chunks
  store ptr null, ptr %chunks
  call void @duck.scope$unlock(ptr %scope)
  br label %free
free:
  %chunk = phi ptr [ %first, %advance ], [ %following, %release ]
  %done = icmp eq ptr %chunk, null
  br i1 %done, label %finish, label %release
release:
  %following = load ptr, ptr %chunk
  call void @free(ptr %chunk)
  br label %free
finish:
  ret void
}

define internal void @duck.scope$shutdown() {
entry:
  %first = load ptr, ptr @duck$scope.registry
  br label %loop
loop:
  %scope = phi ptr [ %first, %entry ], [ %following, %advance ]
  %done = icmp eq ptr %scope, null
  br i1 %done, label %finish, label %check
check:
  %generation = load atomic i64, ptr %scope acquire, align 8
  %bit = and i64 %generation, 1
  %active = icmp eq i64 %bit, 1
  br i1 %active, label %close, label %advance
close:
  call void @duck.scope$close(ptr %scope, i64 1)
  br label %advance
advance:
  %nextSlot = getelementptr %duck.scope, ptr %scope, i32 0, i32 5
  %following = load ptr, ptr %nextSlot
  br label %loop
finish:
  ret void
}

; Starts the scope; while active, it closes the old variables and stays active.
define internal void @duck.scope$start(ptr %scope) {
entry:
  %registeredSlot = getelementptr %duck.scope, ptr %scope, i32 0, i32 6
  %wasRegistered = atomicrmw xchg ptr %registeredSlot, i32 1 acq_rel
  %first = icmp eq i32 %wasRegistered, 0
  br i1 %first, label %register, label %hooked
register:
  %nextSlot = getelementptr %duck.scope, ptr %scope, i32 0, i32 5
  br label %push
push:
  %head = load atomic ptr, ptr @duck$scope.registry acquire, align 8
  store ptr %head, ptr %nextSlot
  %swap = cmpxchg ptr @duck$scope.registry, ptr %head, ptr %scope seq_cst seq_cst
  %swapped = extractvalue { ptr, i1 } %swap, 1
  br i1 %swapped, label %hook, label %push
hook:
  %wasHooked = atomicrmw xchg ptr @duck$scope.hooked, i32 1 acq_rel
  %needed = icmp eq i32 %wasHooked, 0
  br i1 %needed, label %install, label %hooked
install:
  call i32 @atexit(ptr @duck.scope$shutdown)
  br label %hooked
hooked:
  %generation = load atomic i64, ptr %scope acquire, align 8
  %bit = and i64 %generation, 1
  %active = icmp eq i64 %bit, 1
  br i1 %active, label %restart, label %open
restart:
  call void @duck.scope$close(ptr %scope, i64 2)
  ret void
open:
  %old = atomicrmw add ptr %scope, i64 1 seq_cst
  ret void
}

define internal void @duck.scope$end(ptr %scope) {
entry:
  %generation = load atomic i64, ptr %scope acquire, align 8
  %bit = and i64 %generation, 1
  %active = icmp eq i64 %bit, 1
  br i1 %active, label %close, label %trap
trap:
  call void @llvm.trap()
  unreachable
close:
  call void @duck.scope$close(ptr %scope, i64 1)
  ret void
}

define internal i1 @duck.scope$active(ptr %scope) {
  %generation = load atomic i64, ptr %scope acquire, align 8
  %bit = and i64 %generation, 1
  %active = icmp eq i64 %bit, 1
  ret i1 %active
}

; One iteration of a [@Name] loop begins.
define internal void @duck.scope$enterLoop(ptr %scope) {
  %loops = getelementptr %duck.scope, ptr %scope, i32 0, i32 2
  %count = load i32, ptr %loops
  %more = add i32 %count, 1
  store i32 %more, ptr %loops
  call void @duck.scope$start(ptr %scope)
  ret void
}

; It ends, on any path. An explicit @end has already closed the scope.
define internal void @duck.scope$leaveLoop(ptr %scope) {
entry:
  %loops = getelementptr %duck.scope, ptr %scope, i32 0, i32 2
  %count = load i32, ptr %loops
  %less = sub i32 %count, 1
  store i32 %less, ptr %loops
  %active = call i1 @duck.scope$active(ptr %scope)
  br i1 %active, label %close, label %done
close:
  call void @duck.scope$close(ptr %scope, i64 1)
  br label %done
done:
  ret void
}

; After an @end: when a [@Name] loop is running, leave everything up to it.
define internal void @duck.scope$requestUnwind(ptr %scope) {
entry:
  %loops = getelementptr %duck.scope, ptr %scope, i32 0, i32 2
  %count = load i32, ptr %loops
  %running = icmp sgt i32 %count, 0
  br i1 %running, label %request, label %done
request:
  store ptr %scope, ptr @duck$scope.unwind
  br label %done
done:
  ret void
}
)IR";
}
} // namespace duck
