#include "Pipeline.h"

namespace duck {
// A heap owner is one word: the payload address while no alias exists, or the
// address of a control block with its low bit set once one does. An alias, in
// a local, field, collection slot, or closure, holds the untagged block address.
// A function that may lend returns an owner word, or a block address with bit 1
// set for a borrowed result. The local receiving it keeps that word, so a slot
// of owner role tells at run time whether it owns the object.
//
// A block is { payload, references, uses, cleanup }. References count the
// owner, aliases, and active uses; the last one frees the block. Uses is the
// number of active uses, with bit 63 set once the owner has closed the object.
// Closing is one atomic step: no use can begin afterwards, and every alias
// compares equal to null. Whoever brings a closed object to zero uses, the
// closer or the last user, runs the cleanup and frees the payload, so that
// happens exactly once.
//
// Only an owner supplies the cleanup, which releases managed fields and runs
// the destructor. Promotion writes the owner's word and is not atomic: worker
// tasks never receive heap references, so only the owning thread promotes.
std::string heapRuntimeIR() {
  return R"IR(
%duck.heap.block = type { ptr, i64, i64, ptr }

define internal ptr @duck.heap$alloc(i64 %bytes) {
  %p = call ptr @malloc(i64 %bytes)
  %failed = icmp eq ptr %p, null
  br i1 %failed, label %trap, label %done
trap:
  call void @llvm.trap()
  unreachable
done:
  ret ptr %p
}

define internal ptr @duck.heap$payload(ptr %word, i1 %owner) {
  %bits = ptrtoint ptr %word to i64
  %flag = and i64 %bits, 3
  %direct = icmp eq i64 %flag, 0
  %raw = and i1 %owner, %direct
  %nil = icmp eq ptr %word, null
  %ready = or i1 %nil, %raw
  br i1 %ready, label %immediate, label %indirect
immediate:
  ret ptr %word
indirect:
  %address = and i64 %bits, -4
  %block = inttoptr i64 %address to ptr
  %uses = getelementptr %duck.heap.block, ptr %block, i32 0, i32 2
  %state = load atomic i64, ptr %uses seq_cst, align 8
  %closed = icmp slt i64 %state, 0
  br i1 %closed, label %expired, label %live
expired:
  ret ptr null
live:
  %p = load ptr, ptr %block
  ret ptr %p
}

define internal ptr @duck.heap$alias(ptr %slot, i1 %owner) {
  %word = load ptr, ptr %slot
  %nil = icmp eq ptr %word, null
  br i1 %nil, label %empty, label %classify
empty:
  ret ptr null
classify:
  %bits = ptrtoint ptr %word to i64
  %flag = and i64 %bits, 3
  %direct = icmp eq i64 %flag, 0
  %create = and i1 %owner, %direct
  br i1 %create, label %promote, label %retain
promote:
  %new = call ptr @duck.heap$alloc(i64 32)
  store ptr %word, ptr %new
  %count = getelementptr %duck.heap.block, ptr %new, i32 0, i32 1
  store i64 2, ptr %count
  %newUses = getelementptr %duck.heap.block, ptr %new, i32 0, i32 2
  store i64 0, ptr %newUses
  %newCleanup = getelementptr %duck.heap.block, ptr %new, i32 0, i32 3
  store ptr null, ptr %newCleanup
  %addr = ptrtoint ptr %new to i64
  %tag = or i64 %addr, 1
  %tagged = inttoptr i64 %tag to ptr
  store ptr %tagged, ptr %slot
  ret ptr %new
retain:
  %addr2 = and i64 %bits, -4
  %block = inttoptr i64 %addr2 to ptr
  %rc = getelementptr %duck.heap.block, ptr %block, i32 0, i32 1
  %old = atomicrmw add ptr %rc, i64 1 seq_cst
  ret ptr %block
}

; Takes one more reference on an alias word.
define internal void @duck.heap$retain(ptr %word) {
  %nil = icmp eq ptr %word, null
  br i1 %nil, label %done, label %count
count:
  %rc = getelementptr %duck.heap.block, ptr %word, i32 0, i32 1
  %old = atomicrmw add ptr %rc, i64 1 seq_cst
  br label %done
done:
  ret void
}

; Drops one reference on a block.
define internal void @duck.heap$unref(ptr %block) {
  %rc = getelementptr %duck.heap.block, ptr %block, i32 0, i32 1
  %old = atomicrmw sub ptr %rc, i64 1 seq_cst
  %last = icmp eq i64 %old, 1
  br i1 %last, label %free, label %done
free:
  call void @free(ptr %block)
  br label %done
done:
  ret void
}

; Drops one reference on an alias word.
define internal void @duck.heap$release(ptr %word) {
  %nil = icmp eq ptr %word, null
  br i1 %nil, label %done, label %drop
drop:
  call void @duck.heap$unref(ptr %word)
  br label %done
done:
  ret void
}

define internal void @duck.heap$clean(ptr %cleanup, ptr %payload) {
  %none = icmp eq ptr %cleanup, null
  br i1 %none, label %done, label %call
call:
  call void %cleanup(ptr %payload)
  br label %done
done:
  ret void
}

; Runs the cleanup of a closed object with no uses left.
define internal void @duck.heap$finish(ptr %block) {
  %p = load ptr, ptr %block
  %slot = getelementptr %duck.heap.block, ptr %block, i32 0, i32 3
  %cleanup = load ptr, ptr %slot
  call void @duck.heap$clean(ptr %cleanup, ptr %p)
  call void @free(ptr %p)
  ret void
}

; Closes the object to new uses. The cleanup is published first, because the
; last user may finish the object as soon as the closed bit is visible.
define internal void @duck.heap$close(ptr %block, ptr %cleanup) {
  %slot = getelementptr %duck.heap.block, ptr %block, i32 0, i32 3
  store ptr %cleanup, ptr %slot
  %uses = getelementptr %duck.heap.block, ptr %block, i32 0, i32 2
  %old = atomicrmw or ptr %uses, i64 -9223372036854775808 seq_cst
  %idle = icmp eq i64 %old, 0
  br i1 %idle, label %finish, label %done
finish:
  call void @duck.heap$finish(ptr %block)
  br label %done
done:
  ret void
}

define internal void @duck.heap$drop(ptr %slot, i1 %owner, ptr %cleanup) {
  %word = load ptr, ptr %slot
  store ptr null, ptr %slot
  %nil = icmp eq ptr %word, null
  br i1 %nil, label %done, label %classify
classify:
  %bits = ptrtoint ptr %word to i64
  %flag = and i64 %bits, 3
  %direct = icmp eq i64 %flag, 0
  %raw = and i1 %owner, %direct
  br i1 %raw, label %freeRaw, label %indirect
freeRaw:
  call void @duck.heap$clean(ptr %cleanup, ptr %word)
  call void @free(ptr %word)
  br label %done
indirect:
  %address = and i64 %bits, -4
  %block = inttoptr i64 %address to ptr
  %lent = and i64 %bits, 2
  %held = icmp eq i64 %lent, 0
  %closes = and i1 %owner, %held
  br i1 %closes, label %close, label %release
close:
  call void @duck.heap$close(ptr %block, ptr %cleanup)
  br label %release
release:
  call void @duck.heap$unref(ptr %block)
  br label %done
done:
  ret void
}

; Marks a counted reference as a borrowed function result.
define internal ptr @duck.heap$lend(ptr %block) {
  %nil = icmp eq ptr %block, null
  br i1 %nil, label %empty, label %mark
empty:
  ret ptr null
mark:
  %bits = ptrtoint ptr %block to i64
  %tag = or i64 %bits, 2
  %word = inttoptr i64 %tag to ptr
  ret ptr %word
}

define internal i1 @duck.heap$borrowed(ptr %word) {
  %bits = ptrtoint ptr %word to i64
  %lent = and i64 %bits, 2
  %is = icmp ne i64 %lent, 0
  ret i1 %is
}

; Taking ownership of a borrowed result is invalid.
define internal void @duck.heap$owned(ptr %word) {
  %borrowed = call i1 @duck.heap$borrowed(ptr %word)
  br i1 %borrowed, label %trap, label %done
trap:
  call void @llvm.trap()
  unreachable
done:
  ret void
}

; Cleans up a slot holding an alias: collection elements and boxed locals.
define internal void @duck.heap$slot(ptr %slot) {
  call void @duck.heap$drop(ptr %slot, i1 false, ptr null)
  ret void
}

; Begins a use of the object a tracked slot refers to, and gives its payload,
; or null when the slot is empty or the object is closed. A use also holds a
; reference, so the block outlives a slot cleared while the use is active.
; %record receives the block; heap$unpin ends the use.
define internal ptr @duck.heap$pin(ptr %slot, i1 %owner, ptr %record) {
  %block = call ptr @duck.heap$alias(ptr %slot, i1 %owner)
  %nil = icmp eq ptr %block, null
  br i1 %nil, label %absent, label %start
absent:
  ret ptr null
start:
  %uses = getelementptr %duck.heap.block, ptr %block, i32 0, i32 2
  br label %retry
retry:
  %state = load atomic i64, ptr %uses seq_cst, align 8
  %closed = icmp slt i64 %state, 0
  br i1 %closed, label %expired, label %claim
expired:
  call void @duck.heap$unref(ptr %block)
  ret ptr null
claim:
  %next = add i64 %state, 1
  %exchange = cmpxchg ptr %uses, i64 %state, i64 %next seq_cst seq_cst
  %won = extractvalue { i64, i1 } %exchange, 1
  br i1 %won, label %pinned, label %retry
pinned:
  store ptr %block, ptr %record
  %p = load ptr, ptr %block
  ret ptr %p
}

; Ends the use held in %record, if any, finishing a closed object at its last.
define internal void @duck.heap$unpin(ptr %record) {
  %block = load ptr, ptr %record
  %nil = icmp eq ptr %block, null
  br i1 %nil, label %done, label %end
end:
  store ptr null, ptr %record
  %uses = getelementptr %duck.heap.block, ptr %block, i32 0, i32 2
  %old = atomicrmw sub ptr %uses, i64 1 seq_cst
  %last = icmp eq i64 %old, -9223372036854775807
  br i1 %last, label %finish, label %release
finish:
  call void @duck.heap$finish(ptr %block)
  br label %release
release:
  call void @duck.heap$unref(ptr %block)
  br label %done
done:
  ret void
}
)IR";
}
} // namespace duck
