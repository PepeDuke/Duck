#include "Pipeline.h"

namespace duck {
// A markup value is a queue of node pointers, in declaration order, so moves
// and cleanup follow the ordinary collection rules. A node owns one heap
// element and records how to clean it up and find its own children. The first
// node of a queue also holds the per-type index: one entry for each distinct
// child type, with that type's nodes in order. Lookup therefore costs one
// comparison per distinct type, and indexing a type's children is constant time.
std::string markupRuntimeIR() {
  return R"IR(
%duck.markup.node = type { ptr, ptr, ptr, ptr, i64, ptr }
%duck.markup.entry = type { ptr, ptr, ptr }
@duck.markup.none = internal global ptr null

define internal i1 @duck.markup$same(ptr %a, ptr %b) {
entry:
  br label %next
next:
  %i = phi i64 [ 0, %entry ], [ %following, %more ]
  %pa = getelementptr i8, ptr %a, i64 %i
  %pb = getelementptr i8, ptr %b, i64 %i
  %ca = load i8, ptr %pa
  %cb = load i8, ptr %pb
  %equal = icmp eq i8 %ca, %cb
  br i1 %equal, label %check, label %differ
check:
  %end = icmp eq i8 %ca, 0
  br i1 %end, label %match, label %more
more:
  %following = add i64 %i, 1
  br label %next
match:
  ret i1 true
differ:
  ret i1 false
}

define internal void @duck.markup$freeIndex(ptr %node) {
entry:
  %head = getelementptr %duck.markup.node, ptr %node, i32 0, i32 5
  %first = load ptr, ptr %head
  store ptr null, ptr %head
  br label %loop
loop:
  %current = phi ptr [ %first, %entry ], [ %after, %release ]
  %done = icmp eq ptr %current, null
  br i1 %done, label %finish, label %release
release:
  %nodesSlot = getelementptr %duck.markup.entry, ptr %current, i32 0, i32 1
  %nodes = load ptr, ptr %nodesSlot
  call void @duck.seq$free(ptr %nodes, ptr null)
  %nextSlot = getelementptr %duck.markup.entry, ptr %current, i32 0, i32 2
  %after = load ptr, ptr %nextSlot
  call void @free(ptr %current)
  br label %loop
finish:
  ret void
}

; Releases the node held in a queue slot, with the element it owns.
define internal void @duck.markup$node(ptr %slot) {
  %node = load ptr, ptr %slot
  call void @duck.markup$freeIndex(ptr %node)
  %word = getelementptr %duck.markup.node, ptr %node, i32 0, i32 1
  %cleanupSlot = getelementptr %duck.markup.node, ptr %node, i32 0, i32 2
  %cleanup = load ptr, ptr %cleanupSlot
  call void @duck.heap$drop(ptr %word, i1 true, ptr %cleanup)
  call void @free(ptr %node)
  ret void
}

; The index entry for a child type, or null.
define internal ptr @duck.markup$entry(ptr %c, ptr %name) {
entry:
  %length = call i64 @duck.seq$length(ptr %c)
  %empty = icmp eq i64 %length, 0
  br i1 %empty, label %none, label %start
start:
  %slot = call ptr @duck.seq$nth(ptr %c, i64 0)
  %node = load ptr, ptr %slot
  %head = getelementptr %duck.markup.node, ptr %node, i32 0, i32 5
  %first = load ptr, ptr %head
  br label %loop
loop:
  %current = phi ptr [ %first, %start ], [ %after, %advance ]
  %done = icmp eq ptr %current, null
  br i1 %done, label %none, label %compare
compare:
  %known = load ptr, ptr %current
  %same = call i1 @duck.markup$same(ptr %known, ptr %name)
  br i1 %same, label %found, label %advance
advance:
  %nextSlot = getelementptr %duck.markup.entry, ptr %current, i32 0, i32 2
  %after = load ptr, ptr %nextSlot
  br label %loop
found:
  ret ptr %current
none:
  ret ptr null
}

define internal void @duck.markup$link(ptr %holder, ptr %node) {
entry:
  %slot = call ptr @duck.seq$push(ptr %holder, i64 8)
  store ptr %node, ptr %slot
  %c = load ptr, ptr %holder
  %name = load ptr, ptr %node
  %existing = call ptr @duck.markup$entry(ptr %c, ptr %name)
  %absent = icmp eq ptr %existing, null
  br i1 %absent, label %create, label %append
create:
  %firstSlot = call ptr @duck.seq$nth(ptr %c, i64 0)
  %first = load ptr, ptr %firstSlot
  %head = getelementptr %duck.markup.node, ptr %first, i32 0, i32 5
  %previous = load ptr, ptr %head
  %made = call ptr @duck.heap$alloc(i64 24)
  store ptr %name, ptr %made
  %madeNodes = getelementptr %duck.markup.entry, ptr %made, i32 0, i32 1
  store ptr null, ptr %madeNodes
  %madeNext = getelementptr %duck.markup.entry, ptr %made, i32 0, i32 2
  store ptr %previous, ptr %madeNext
  store ptr %made, ptr %head
  br label %append
append:
  %target = phi ptr [ %existing, %entry ], [ %made, %create ]
  %nodes = getelementptr %duck.markup.entry, ptr %target, i32 0, i32 1
  %entrySlot = call ptr @duck.seq$push(ptr %nodes, i64 8)
  store ptr %node, ptr %entrySlot
  ret void
}

; Takes ownership of an element and appends it to the markup stored at holder.
define internal void @duck.markup$attach(ptr %holder, ptr %name, ptr %word, ptr %cleanup, ptr %descriptor, i64 %offset) {
  %node = call ptr @duck.heap$alloc(i64 48)
  store ptr %name, ptr %node
  %wordSlot = getelementptr %duck.markup.node, ptr %node, i32 0, i32 1
  store ptr %word, ptr %wordSlot
  %cleanupSlot = getelementptr %duck.markup.node, ptr %node, i32 0, i32 2
  store ptr %cleanup, ptr %cleanupSlot
  %descriptorSlot = getelementptr %duck.markup.node, ptr %node, i32 0, i32 3
  store ptr %descriptor, ptr %descriptorSlot
  %offsetSlot = getelementptr %duck.markup.node, ptr %node, i32 0, i32 4
  store i64 %offset, ptr %offsetSlot
  %indexSlot = getelementptr %duck.markup.node, ptr %node, i32 0, i32 5
  store ptr null, ptr %indexSlot
  call void @duck.markup$link(ptr %holder, ptr %node)
  ret void
}

; Moves every node of source to the end of the markup at holder, in order, and
; frees source. Each node's type is indexed again in its new parent.
define internal void @duck.markup$splice(ptr %holder, ptr %source) {
entry:
  %count = call i64 @duck.seq$length(ptr %source)
  br label %loop
loop:
  %i = phi i64 [ 0, %entry ], [ %following, %move ]
  %more = icmp ult i64 %i, %count
  br i1 %more, label %move, label %done
move:
  %slot = call ptr @duck.seq$nth(ptr %source, i64 %i)
  %node = load ptr, ptr %slot
  call void @duck.markup$freeIndex(ptr %node)
  call void @duck.markup$link(ptr %holder, ptr %node)
  %following = add i64 %i, 1
  br label %loop
done:
  call void @duck.seq$free(ptr %source, ptr null)
  ret void
}

; The owner word of the only child of a type. A missing or repeated child traps.
define internal ptr @duck.markup$find(ptr %c, ptr %name) {
entry:
  %entry0 = call ptr @duck.markup$entry(ptr %c, ptr %name)
  %absent = icmp eq ptr %entry0, null
  br i1 %absent, label %trap, label %check
check:
  %nodesSlot = getelementptr %duck.markup.entry, ptr %entry0, i32 0, i32 1
  %nodes = load ptr, ptr %nodesSlot
  %count = call i64 @duck.seq$length(ptr %nodes)
  %single = icmp eq i64 %count, 1
  br i1 %single, label %found, label %trap
trap:
  call void @llvm.trap()
  unreachable
found:
  %slot = call ptr @duck.seq$nth(ptr %nodes, i64 0)
  %node = load ptr, ptr %slot
  %word = getelementptr %duck.markup.node, ptr %node, i32 0, i32 1
  ret ptr %word
}

; The owner word of a type's child at index. Outside the children it traps,
; or with guarded gives null.
define internal ptr @duck.markup$nth(ptr %c, ptr %name, i64 %index, i1 %guarded) {
entry:
  %entry0 = call ptr @duck.markup$entry(ptr %c, ptr %name)
  %absent = icmp eq ptr %entry0, null
  br i1 %absent, label %miss, label %check
check:
  %nodesSlot = getelementptr %duck.markup.entry, ptr %entry0, i32 0, i32 1
  %nodes = load ptr, ptr %nodesSlot
  %count = call i64 @duck.seq$length(ptr %nodes)
  %inside = icmp ult i64 %index, %count
  br i1 %inside, label %found, label %miss
miss:
  br i1 %guarded, label %none, label %trap
none:
  ret ptr null
trap:
  call void @llvm.trap()
  unreachable
found:
  %slot = call ptr @duck.seq$nth(ptr %nodes, i64 %index)
  %node = load ptr, ptr %slot
  %word = getelementptr %duck.markup.node, ptr %node, i32 0, i32 1
  ret ptr %word
}

define internal ptr @duck.markup$payload(ptr %node) {
  %wordSlot = getelementptr %duck.markup.node, ptr %node, i32 0, i32 1
  %word = load ptr, ptr %wordSlot
  %payload = call ptr @duck.heap$payload(ptr %word, i1 true)
  ret ptr %payload
}

define internal ptr @duck.markup$descriptor(ptr %node) {
  %slot = getelementptr %duck.markup.node, ptr %node, i32 0, i32 3
  %descriptor = load ptr, ptr %slot
  ret ptr %descriptor
}

; Where a node's element stores its own children.
define internal ptr @duck.markup$children(ptr %node) {
entry:
  %slot = getelementptr %duck.markup.node, ptr %node, i32 0, i32 4
  %offset = load i64, ptr %slot
  %leaf = icmp slt i64 %offset, 0
  br i1 %leaf, label %none, label %host
none:
  ret ptr @duck.markup.none
host:
  %payload = call ptr @duck.markup$payload(ptr %node)
  %children = getelementptr i8, ptr %payload, i64 %offset
  ret ptr %children
}
)IR";
}
} // namespace duck
