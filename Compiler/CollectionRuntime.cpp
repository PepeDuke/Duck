#include "Pipeline.h"

namespace duck {
// Queues, stacks, and pairs. A collection variable holds a pointer to its
// storage, null while nothing has been added, so zeroed storage is a valid
// empty collection. Elements are copied in and out bytewise; the compiler
// stores and loads typed values through the slots these helpers return and
// passes a release function (or null) for reference-counted elements.
//
// A queue or stack is a ring buffer: { data, head, count, capacity, element
// size }. A queue adds at the back and removes at the head (first in, first
// out); a stack adds and removes at the back (last in, first out). Growing
// copies the elements into a new buffer in order, starting at head.
//
// A pair keeps its entries in insertion order: { entries, count, capacity,
// key size, entry stride, value offset }. Each entry is a key followed, at
// the value offset, by its value. Lookups compare keys with an equality
// function, or bytewise when it is null (keys are zero-filled before they are
// written, so padding compares equal). Replacing a key's value keeps its
// position, so the last entry is always the last key added.
std::string collectionRuntimeIR() {
  return R"IR(
%duck.seq = type { ptr, i64, i64, i64, i64 }
%duck.pair = type { ptr, i64, i64, i64, i64, i64 }

define internal void @duck.mem$copy(ptr %to, ptr %from, i64 %n) {
entry:
  br label %loop
loop:
  %i = phi i64 [0, %entry], [%next, %body]
  %done = icmp uge i64 %i, %n
  br i1 %done, label %exit, label %body
body:
  %s = getelementptr i8, ptr %from, i64 %i
  %d = getelementptr i8, ptr %to, i64 %i
  %b = load i8, ptr %s
  store i8 %b, ptr %d
  %next = add i64 %i, 1
  br label %loop
exit:
  ret void
}

define internal void @duck.mem$zero(ptr %to, i64 %n) {
entry:
  br label %loop
loop:
  %i = phi i64 [0, %entry], [%next, %body]
  %done = icmp uge i64 %i, %n
  br i1 %done, label %exit, label %body
body:
  %d = getelementptr i8, ptr %to, i64 %i
  store i8 0, ptr %d
  %next = add i64 %i, 1
  br label %loop
exit:
  ret void
}

define internal i1 @duck.mem$equal(ptr %a, ptr %b, i64 %n) {
entry:
  br label %loop
loop:
  %i = phi i64 [0, %entry], [%next, %same]
  %done = icmp uge i64 %i, %n
  br i1 %done, label %equal, label %body
body:
  %pa = getelementptr i8, ptr %a, i64 %i
  %pb = getelementptr i8, ptr %b, i64 %i
  %x = load i8, ptr %pa
  %y = load i8, ptr %pb
  %eq = icmp eq i8 %x, %y
  %next = add i64 %i, 1
  br i1 %eq, label %same, label %different
same:
  br label %loop
equal:
  ret i1 true
different:
  ret i1 false
}

define internal ptr @duck.collection$alloc(i64 %bytes) {
  %p = call ptr @malloc(i64 %bytes)
  %failed = icmp eq ptr %p, null
  br i1 %failed, label %trap, label %done
trap:
  call void @llvm.trap()
  unreachable
done:
  ret ptr %p
}

define internal ptr @duck.seq$push(ptr %holder, i64 %size) {
entry:
  %c0 = load ptr, ptr %holder
  %empty = icmp eq ptr %c0, null
  br i1 %empty, label %create, label %ready
create:
  %new = call ptr @duck.collection$alloc(i64 40)
  store ptr null, ptr %new
  %nh = getelementptr %duck.seq, ptr %new, i32 0, i32 1
  store i64 0, ptr %nh
  %nc = getelementptr %duck.seq, ptr %new, i32 0, i32 2
  store i64 0, ptr %nc
  %ncap = getelementptr %duck.seq, ptr %new, i32 0, i32 3
  store i64 0, ptr %ncap
  %ns = getelementptr %duck.seq, ptr %new, i32 0, i32 4
  store i64 %size, ptr %ns
  store ptr %new, ptr %holder
  br label %ready
ready:
  %c = phi ptr [%c0, %entry], [%new, %create]
  %datap = getelementptr %duck.seq, ptr %c, i32 0, i32 0
  %headp = getelementptr %duck.seq, ptr %c, i32 0, i32 1
  %countp = getelementptr %duck.seq, ptr %c, i32 0, i32 2
  %capp = getelementptr %duck.seq, ptr %c, i32 0, i32 3
  %sizep = getelementptr %duck.seq, ptr %c, i32 0, i32 4
  %count = load i64, ptr %countp
  %cap = load i64, ptr %capp
  %elsize = load i64, ptr %sizep
  %full = icmp eq i64 %count, %cap
  br i1 %full, label %grow, label %place
grow:
  %doubled = shl i64 %cap, 1
  %small = icmp ult i64 %doubled, 4
  %newcap = select i1 %small, i64 4, i64 %doubled
  %bytes = mul i64 %newcap, %elsize
  %data = call ptr @duck.collection$alloc(i64 %bytes)
  %old = load ptr, ptr %datap
  %head = load i64, ptr %headp
  br label %move
move:
  %i = phi i64 [0, %grow], [%inext, %moveBody]
  %moved = icmp uge i64 %i, %count
  br i1 %moved, label %moveDone, label %moveBody
moveBody:
  %src0 = add i64 %head, %i
  %src = urem i64 %src0, %cap
  %soff = mul i64 %src, %elsize
  %doff = mul i64 %i, %elsize
  %sp = getelementptr i8, ptr %old, i64 %soff
  %dp = getelementptr i8, ptr %data, i64 %doff
  call void @duck.mem$copy(ptr %dp, ptr %sp, i64 %elsize)
  %inext = add i64 %i, 1
  br label %move
moveDone:
  call void @free(ptr %old)
  store ptr %data, ptr %datap
  store i64 0, ptr %headp
  store i64 %newcap, ptr %capp
  br label %place
place:
  %cap2 = load i64, ptr %capp
  %head2 = load i64, ptr %headp
  %data2 = load ptr, ptr %datap
  %at0 = add i64 %head2, %count
  %at = urem i64 %at0, %cap2
  %off = mul i64 %at, %elsize
  %slot = getelementptr i8, ptr %data2, i64 %off
  %grown = add i64 %count, 1
  store i64 %grown, ptr %countp
  ret ptr %slot
}

; The slot at the head (fromBack false) or the back; traps when empty.
define internal ptr @duck.seq$at(ptr %c, i1 %fromBack) {
entry:
  %null = icmp eq ptr %c, null
  br i1 %null, label %trap, label %check
check:
  %countp = getelementptr %duck.seq, ptr %c, i32 0, i32 2
  %count = load i64, ptr %countp
  %empty = icmp eq i64 %count, 0
  br i1 %empty, label %trap, label %found
trap:
  call void @llvm.trap()
  unreachable
found:
  %data = load ptr, ptr %c
  %headp = getelementptr %duck.seq, ptr %c, i32 0, i32 1
  %head = load i64, ptr %headp
  %capp = getelementptr %duck.seq, ptr %c, i32 0, i32 3
  %cap = load i64, ptr %capp
  %sizep = getelementptr %duck.seq, ptr %c, i32 0, i32 4
  %size = load i64, ptr %sizep
  %end = add i64 %head, %count
  %last = sub i64 %end, 1
  %index0 = select i1 %fromBack, i64 %last, i64 %head
  %index = urem i64 %index0, %cap
  %off = mul i64 %index, %size
  %slot = getelementptr i8, ptr %data, i64 %off
  ret ptr %slot
}

define internal void @duck.seq$drop(ptr %c, i1 %fromBack, ptr %release) {
entry:
  %slot = call ptr @duck.seq$at(ptr %c, i1 %fromBack)
  %managed = icmp ne ptr %release, null
  br i1 %managed, label %free, label %remove
free:
  call void %release(ptr %slot)
  br label %remove
remove:
  %headp = getelementptr %duck.seq, ptr %c, i32 0, i32 1
  %countp = getelementptr %duck.seq, ptr %c, i32 0, i32 2
  %capp = getelementptr %duck.seq, ptr %c, i32 0, i32 3
  %count = load i64, ptr %countp
  %fewer = sub i64 %count, 1
  store i64 %fewer, ptr %countp
  br i1 %fromBack, label %done, label %advance
advance:
  %head = load i64, ptr %headp
  %cap = load i64, ptr %capp
  %next = add i64 %head, 1
  %wrapped = urem i64 %next, %cap
  store i64 %wrapped, ptr %headp
  br label %done
done:
  ret void
}

; A dynamic array's element i; its head stays 0, since it only adds and
; removes at the back. Traps outside 0 <= i < count.
define internal ptr @duck.seq$index(ptr %c, i64 %i) {
entry:
  %null = icmp eq ptr %c, null
  br i1 %null, label %trap, label %check
check:
  %countp = getelementptr %duck.seq, ptr %c, i32 0, i32 2
  %count = load i64, ptr %countp
  %inside = icmp ult i64 %i, %count
  br i1 %inside, label %found, label %trap
trap:
  call void @llvm.trap()
  unreachable
found:
  %data = load ptr, ptr %c
  %sizep = getelementptr %duck.seq, ptr %c, i32 0, i32 4
  %size = load i64, ptr %sizep
  %off = mul i64 %i, %size
  %slot = getelementptr i8, ptr %data, i64 %off
  ret ptr %slot
}

; The slot of element i, or null when i is outside the array.
define internal ptr @duck.seq$find(ptr %c, i64 %i) {
entry:
  %null = icmp eq ptr %c, null
  br i1 %null, label %missing, label %check
check:
  %countp = getelementptr %duck.seq, ptr %c, i32 0, i32 2
  %count = load i64, ptr %countp
  %inside = icmp ult i64 %i, %count
  br i1 %inside, label %found, label %missing
missing:
  ret ptr null
found:
  %slot = call ptr @duck.seq$index(ptr %c, i64 %i)
  ret ptr %slot
}

; Removes element i of a dynamic array, which the caller has already
; released or moved out, shifting the later elements down.
define internal void @duck.seq$removeAt(ptr %c, i64 %i) {
entry:
  %countp = getelementptr %duck.seq, ptr %c, i32 0, i32 2
  %count = load i64, ptr %countp
  %sizep = getelementptr %duck.seq, ptr %c, i32 0, i32 4
  %size = load i64, ptr %sizep
  %data = load ptr, ptr %c
  %next = add i64 %i, 1
  %rest = sub i64 %count, %next
  %bytes = mul i64 %rest, %size
  %toff = mul i64 %i, %size
  %foff = mul i64 %next, %size
  %to = getelementptr i8, ptr %data, i64 %toff
  %from = getelementptr i8, ptr %data, i64 %foff
  call void @duck.mem$copy(ptr %to, ptr %from, i64 %bytes)
  %fewer = sub i64 %count, 1
  store i64 %fewer, ptr %countp
  ret void
}

; Calls cleanup on each of count elements of a fixed array.
define internal void @duck.array$each(ptr %data, i64 %count, i64 %size, ptr %cleanup) {
entry:
  br label %loop
loop:
  %i = phi i64 [0, %entry], [%next, %body]
  %done = icmp uge i64 %i, %count
  br i1 %done, label %exit, label %body
body:
  %off = mul i64 %i, %size
  %slot = getelementptr i8, ptr %data, i64 %off
  call void %cleanup(ptr %slot)
  %next = add i64 %i, 1
  br label %loop
exit:
  ret void
}

; Copies the element at source into each of count elements.
define internal void @duck.array$fill(ptr %data, i64 %count, i64 %size, ptr %source) {
entry:
  br label %loop
loop:
  %i = phi i64 [0, %entry], [%next, %body]
  %done = icmp uge i64 %i, %count
  br i1 %done, label %exit, label %body
body:
  %off = mul i64 %i, %size
  %slot = getelementptr i8, ptr %data, i64 %off
  call void @duck.mem$copy(ptr %slot, ptr %source, i64 %size)
  %next = add i64 %i, 1
  br label %loop
exit:
  ret void
}

define internal i64 @duck.seq$length(ptr %c) {
entry:
  %null = icmp eq ptr %c, null
  br i1 %null, label %zero, label %count
zero:
  ret i64 0
count:
  %countp = getelementptr %duck.seq, ptr %c, i32 0, i32 2
  %n = load i64, ptr %countp
  ret i64 %n
}

define internal void @duck.seq$free(ptr %c, ptr %release) {
entry:
  %null = icmp eq ptr %c, null
  br i1 %null, label %exit, label %start
start:
  %managed = icmp ne ptr %release, null
  %data = load ptr, ptr %c
  %headp = getelementptr %duck.seq, ptr %c, i32 0, i32 1
  %head = load i64, ptr %headp
  %countp = getelementptr %duck.seq, ptr %c, i32 0, i32 2
  %count = load i64, ptr %countp
  %capp = getelementptr %duck.seq, ptr %c, i32 0, i32 3
  %cap = load i64, ptr %capp
  %sizep = getelementptr %duck.seq, ptr %c, i32 0, i32 4
  %size = load i64, ptr %sizep
  br i1 %managed, label %loop, label %freeAll
loop:
  %i = phi i64 [0, %start], [%next, %body]
  %done = icmp uge i64 %i, %count
  br i1 %done, label %freeAll, label %body
body:
  %at0 = add i64 %head, %i
  %at = urem i64 %at0, %cap
  %off = mul i64 %at, %size
  %slot = getelementptr i8, ptr %data, i64 %off
  call void %release(ptr %slot)
  %next = add i64 %i, 1
  br label %loop
freeAll:
  call void @free(ptr %data)
  call void @free(ptr %c)
  br label %exit
exit:
  ret void
}

)IR" // MSVC caps each string literal piece at 16 KB.
         R"IR(
; The slot at position i from the head of a queue or stack, for iteration.
; Traps outside 0 <= i < count.
define internal ptr @duck.seq$nth(ptr %c, i64 %i) {
entry:
  %null = icmp eq ptr %c, null
  br i1 %null, label %trap, label %check
check:
  %countp = getelementptr %duck.seq, ptr %c, i32 0, i32 2
  %count = load i64, ptr %countp
  %inside = icmp ult i64 %i, %count
  br i1 %inside, label %found, label %trap
trap:
  call void @llvm.trap()
  unreachable
found:
  %data = load ptr, ptr %c
  %headp = getelementptr %duck.seq, ptr %c, i32 0, i32 1
  %head = load i64, ptr %headp
  %capp = getelementptr %duck.seq, ptr %c, i32 0, i32 3
  %cap = load i64, ptr %capp
  %sizep = getelementptr %duck.seq, ptr %c, i32 0, i32 4
  %size = load i64, ptr %sizep
  %at0 = add i64 %head, %i
  %at = urem i64 %at0, %cap
  %off = mul i64 %at, %size
  %slot = getelementptr i8, ptr %data, i64 %off
  ret ptr %slot
}

; The entry at position i in insertion order, for iteration. Traps outside
; 0 <= i < count.
define internal ptr @duck.pair$nth(ptr %c, i64 %i) {
entry:
  %null = icmp eq ptr %c, null
  br i1 %null, label %trap, label %check
check:
  %countp = getelementptr %duck.pair, ptr %c, i32 0, i32 1
  %count = load i64, ptr %countp
  %inside = icmp ult i64 %i, %count
  br i1 %inside, label %found, label %trap
trap:
  call void @llvm.trap()
  unreachable
found:
  %entries = load ptr, ptr %c
  %stridep = getelementptr %duck.pair, ptr %c, i32 0, i32 4
  %stride = load i64, ptr %stridep
  %off = mul i64 %i, %stride
  %e = getelementptr i8, ptr %entries, i64 %off
  ret ptr %e
}

define internal ptr @duck.pair$find(ptr %c, ptr %key, ptr %equal) {
entry:
  %null = icmp eq ptr %c, null
  br i1 %null, label %missing, label %start
start:
  %entries = load ptr, ptr %c
  %countp = getelementptr %duck.pair, ptr %c, i32 0, i32 1
  %count = load i64, ptr %countp
  %ksizep = getelementptr %duck.pair, ptr %c, i32 0, i32 3
  %ksize = load i64, ptr %ksizep
  %stridep = getelementptr %duck.pair, ptr %c, i32 0, i32 4
  %stride = load i64, ptr %stridep
  %bytewise = icmp eq ptr %equal, null
  br label %loop
loop:
  %i = phi i64 [0, %start], [%next, %differ]
  %done = icmp uge i64 %i, %count
  br i1 %done, label %missing, label %body
body:
  %off = mul i64 %i, %stride
  %e = getelementptr i8, ptr %entries, i64 %off
  br i1 %bytewise, label %bytes, label %custom
bytes:
  %sameBytes = call i1 @duck.mem$equal(ptr %e, ptr %key, i64 %ksize)
  br label %compared
custom:
  %sameCustom = call i1 %equal(ptr %e, ptr %key)
  br label %compared
compared:
  %same = phi i1 [%sameBytes, %bytes], [%sameCustom, %custom]
  br i1 %same, label %found, label %differ
differ:
  %next = add i64 %i, 1
  br label %loop
found:
  ret ptr %e
missing:
  ret ptr null
}

; The value slot stored for key, or null when the key is missing.
define internal ptr @duck.pair$lookup(ptr %c, ptr %key, ptr %equal) {
entry:
  %e = call ptr @duck.pair$find(ptr %c, ptr %key, ptr %equal)
  %missing = icmp eq ptr %e, null
  br i1 %missing, label %none, label %found
none:
  ret ptr null
found:
  %v = call ptr @duck.pair$value(ptr %c, ptr %e)
  ret ptr %v
}

define internal ptr @duck.pair$append(ptr %holder, i64 %ksize, i64 %vsize) {
entry:
  %c0 = load ptr, ptr %holder
  %empty = icmp eq ptr %c0, null
  br i1 %empty, label %create, label %ready
create:
  %new = call ptr @duck.collection$alloc(i64 48)
  store ptr null, ptr %new
  %nc = getelementptr %duck.pair, ptr %new, i32 0, i32 1
  store i64 0, ptr %nc
  %ncap = getelementptr %duck.pair, ptr %new, i32 0, i32 2
  store i64 0, ptr %ncap
  %nk = getelementptr %duck.pair, ptr %new, i32 0, i32 3
  store i64 %ksize, ptr %nk
  %k15 = add i64 %ksize, 15
  %voffset = and i64 %k15, -16
  %v15 = add i64 %vsize, 15
  %vround = and i64 %v15, -16
  %stride0 = add i64 %voffset, %vround
  %nstride = getelementptr %duck.pair, ptr %new, i32 0, i32 4
  store i64 %stride0, ptr %nstride
  %nv = getelementptr %duck.pair, ptr %new, i32 0, i32 5
  store i64 %voffset, ptr %nv
  store ptr %new, ptr %holder
  br label %ready
ready:
  %c = phi ptr [%c0, %entry], [%new, %create]
  %entriesp = getelementptr %duck.pair, ptr %c, i32 0, i32 0
  %countp = getelementptr %duck.pair, ptr %c, i32 0, i32 1
  %capp = getelementptr %duck.pair, ptr %c, i32 0, i32 2
  %stridep = getelementptr %duck.pair, ptr %c, i32 0, i32 4
  %count = load i64, ptr %countp
  %cap = load i64, ptr %capp
  %stride = load i64, ptr %stridep
  %full = icmp eq i64 %count, %cap
  br i1 %full, label %grow, label %place
grow:
  %doubled = shl i64 %cap, 1
  %small = icmp ult i64 %doubled, 4
  %newcap = select i1 %small, i64 4, i64 %doubled
  %bytes = mul i64 %newcap, %stride
  %data = call ptr @duck.collection$alloc(i64 %bytes)
  %old = load ptr, ptr %entriesp
  %used = mul i64 %count, %stride
  call void @duck.mem$copy(ptr %data, ptr %old, i64 %used)
  call void @free(ptr %old)
  store ptr %data, ptr %entriesp
  store i64 %newcap, ptr %capp
  br label %place
place:
  %entries = load ptr, ptr %entriesp
  %off = mul i64 %count, %stride
  %e = getelementptr i8, ptr %entries, i64 %off
  %grown = add i64 %count, 1
  store i64 %grown, ptr %countp
  ret ptr %e
}

define internal ptr @duck.pair$value(ptr %c, ptr %entry) {
  %vp = getelementptr %duck.pair, ptr %c, i32 0, i32 5
  %voffset = load i64, ptr %vp
  %v = getelementptr i8, ptr %entry, i64 %voffset
  ret ptr %v
}

; The last entry added; traps when empty.
define internal ptr @duck.pair$last(ptr %c) {
entry:
  %null = icmp eq ptr %c, null
  br i1 %null, label %trap, label %check
check:
  %countp = getelementptr %duck.pair, ptr %c, i32 0, i32 1
  %count = load i64, ptr %countp
  %empty = icmp eq i64 %count, 0
  br i1 %empty, label %trap, label %found
trap:
  call void @llvm.trap()
  unreachable
found:
  %entries = load ptr, ptr %c
  %stridep = getelementptr %duck.pair, ptr %c, i32 0, i32 4
  %stride = load i64, ptr %stridep
  %last = sub i64 %count, 1
  %off = mul i64 %last, %stride
  %e = getelementptr i8, ptr %entries, i64 %off
  ret ptr %e
}

define internal void @duck.pair$dropLast(ptr %c, ptr %releaseKey, ptr %releaseValue) {
entry:
  %e = call ptr @duck.pair$last(ptr %c)
  %managedKey = icmp ne ptr %releaseKey, null
  br i1 %managedKey, label %key, label %value
key:
  call void %releaseKey(ptr %e)
  br label %value
value:
  %managedValue = icmp ne ptr %releaseValue, null
  br i1 %managedValue, label %releaseV, label %remove
releaseV:
  %v = call ptr @duck.pair$value(ptr %c, ptr %e)
  call void %releaseValue(ptr %v)
  br label %remove
remove:
  %countp = getelementptr %duck.pair, ptr %c, i32 0, i32 1
  %count = load i64, ptr %countp
  %fewer = sub i64 %count, 1
  store i64 %fewer, ptr %countp
  ret void
}

; An entry's position in insertion order.
define internal i64 @duck.pair$position(ptr %c, ptr %entry) {
  %entries = load ptr, ptr %c
  %stridep = getelementptr %duck.pair, ptr %c, i32 0, i32 4
  %stride = load i64, ptr %stridep
  %start = ptrtoint ptr %entries to i64
  %at = ptrtoint ptr %entry to i64
  %off = sub i64 %at, %start
  %index = udiv i64 %off, %stride
  ret i64 %index
}

; Removes an entry whose key and value the caller has already released or
; moved out, shifting the later entries down so insertion order holds.
define internal void @duck.pair$remove(ptr %c, ptr %entry) {
  %countp = getelementptr %duck.pair, ptr %c, i32 0, i32 1
  %count = load i64, ptr %countp
  %stridep = getelementptr %duck.pair, ptr %c, i32 0, i32 4
  %stride = load i64, ptr %stridep
  %index = call i64 @duck.pair$position(ptr %c, ptr %entry)
  %next = add i64 %index, 1
  %rest = sub i64 %count, %next
  %bytes = mul i64 %rest, %stride
  %from = getelementptr i8, ptr %entry, i64 %stride
  call void @duck.mem$copy(ptr %entry, ptr %from, i64 %bytes)
  %fewer = sub i64 %count, 1
  store i64 %fewer, ptr %countp
  ret void
}

define internal i64 @duck.pair$length(ptr %c) {
entry:
  %null = icmp eq ptr %c, null
  br i1 %null, label %zero, label %count
zero:
  ret i64 0
count:
  %countp = getelementptr %duck.pair, ptr %c, i32 0, i32 1
  %n = load i64, ptr %countp
  ret i64 %n
}

define internal void @duck.pair$free(ptr %c, ptr %releaseKey, ptr %releaseValue) {
entry:
  %null = icmp eq ptr %c, null
  br i1 %null, label %exit, label %start
start:
  %entries = load ptr, ptr %c
  %countp = getelementptr %duck.pair, ptr %c, i32 0, i32 1
  %count = load i64, ptr %countp
  %stridep = getelementptr %duck.pair, ptr %c, i32 0, i32 4
  %stride = load i64, ptr %stridep
  %vp = getelementptr %duck.pair, ptr %c, i32 0, i32 5
  %voffset = load i64, ptr %vp
  %managedKey = icmp ne ptr %releaseKey, null
  %managedValue = icmp ne ptr %releaseValue, null
  br label %loop
loop:
  %i = phi i64 [0, %start], [%next, %valueDone]
  %done = icmp uge i64 %i, %count
  br i1 %done, label %freeAll, label %body
body:
  %off = mul i64 %i, %stride
  %e = getelementptr i8, ptr %entries, i64 %off
  br i1 %managedKey, label %key, label %keyDone
key:
  call void %releaseKey(ptr %e)
  br label %keyDone
keyDone:
  br i1 %managedValue, label %value, label %valueDone
value:
  %v = getelementptr i8, ptr %e, i64 %voffset
  call void %releaseValue(ptr %v)
  br label %valueDone
valueDone:
  %next = add i64 %i, 1
  br label %loop
freeAll:
  call void @free(ptr %entries)
  call void @free(ptr %c)
  br label %exit
exit:
  ret void
}

; Reference-counted elements and keys: string, name, and big slots hold a
; pointer to shared storage.
define internal void @duck$slot.release(ptr %slot) {
  %p = load ptr, ptr %slot
  call void @duck.text.release(ptr %p)
  ret void
}

define internal i1 @duck$key.string(ptr %a, ptr %b) {
  %x = load ptr, ptr %a
  %y = load ptr, ptr %b
  %eq = call i1 @duck.text.equal(ptr %x, ptr %y, i64 2)
  ret i1 %eq
}

define internal i1 @duck$key.name(ptr %a, ptr %b) {
  %x = load ptr, ptr %a
  %y = load ptr, ptr %b
  %eq = call i1 @duck.text.equal(ptr %x, ptr %y, i64 1)
  ret i1 %eq
}
)IR";
}
// Big keys compare through the big-integer runtime, emitted only with it.
std::string collectionBigKeyIR() {
  return R"IR(
define internal i1 @duck$key.big(ptr %a, ptr %b) {
  %x = load ptr, ptr %a
  %y = load ptr, ptr %b
  %order = call i32 @duck.big.compare(ptr %x, ptr %y)
  %eq = icmp eq i32 %order, 0
  ret i1 %eq
}
)IR";
}
} // namespace duck
