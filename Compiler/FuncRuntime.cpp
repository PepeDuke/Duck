#include "Pipeline.h"

namespace duck {
// A cell is reference-counted heap storage: { i64 count, ptr dispose, ... }.
// A count below zero marks an immortal constant. -1 is an ordinary closure
// without captures, and -2 a member lambda, whose env is the receiver.
//
// A closure is a cell { count, dispose, ptr fn, captures... }. Its function
// takes the closure (or the receiver) first, then the call's arguments.
// A box is a cell { count, dispose, ptr cleanup, pad, payload at byte 32 },
// holding a local that lambdas share by reference.
std::string funcRuntimeIR() {
  return R"IR(
define internal void @duck.cell$retain(ptr %c) {
  %nil = icmp eq ptr %c, null
  br i1 %nil, label %done, label %check
check:
  %count = load i64, ptr %c
  %immortal = icmp slt i64 %count, 0
  br i1 %immortal, label %done, label %bump
bump:
  %next = add i64 %count, 1
  store i64 %next, ptr %c
  br label %done
done:
  ret void
}

define internal void @duck.cell$release(ptr %c) {
  %nil = icmp eq ptr %c, null
  br i1 %nil, label %done, label %check
check:
  %count = load i64, ptr %c
  %immortal = icmp slt i64 %count, 0
  br i1 %immortal, label %done, label %drop
drop:
  %next = sub i64 %count, 1
  store i64 %next, ptr %c
  %last = icmp eq i64 %next, 0
  br i1 %last, label %dispose, label %done
dispose:
  %slot = getelementptr i8, ptr %c, i64 8
  %disposer = load ptr, ptr %slot
  %none = icmp eq ptr %disposer, null
  br i1 %none, label %free, label %run
run:
  call void %disposer(ptr %c)
  br label %free
free:
  call void @free(ptr %c)
  br label %done
done:
  ret void
}

define internal ptr @duck.cell$new(i64 %bytes, ptr %dispose) {
  %c = call ptr @malloc(i64 %bytes)
  %failed = icmp eq ptr %c, null
  br i1 %failed, label %trap, label %ready
trap:
  call void @llvm.trap()
  unreachable
ready:
  store i64 1, ptr %c
  %slot = getelementptr i8, ptr %c, i64 8
  store ptr %dispose, ptr %slot
  ret ptr %c
}

define internal void @duck.box$dispose(ptr %box) {
  %slot = getelementptr i8, ptr %box, i64 16
  %cleanup = load ptr, ptr %slot
  %none = icmp eq ptr %cleanup, null
  br i1 %none, label %done, label %run
run:
  %payload = getelementptr i8, ptr %box, i64 32
  call void %cleanup(ptr %payload)
  br label %done
done:
  ret void
}

define internal ptr @duck.box$new(i64 %bytes, ptr %cleanup) {
  %total = add i64 %bytes, 32
  %box = call ptr @duck.cell$new(i64 %total, ptr @duck.box$dispose)
  %slot = getelementptr i8, ptr %box, i64 16
  store ptr %cleanup, ptr %slot
  ret ptr %box
}

define internal void @duck$slot.func(ptr %slot) {
  %c = load ptr, ptr %slot
  call void @duck.cell$release(ptr %c)
  ret void
}
)IR";
}
// Finds a public method in a type descriptor by name and, unless null, by
// signature text. Gives its member descriptor, or null.
std::string metaRuntimeIR() {
  return R"IR(
define internal i1 @duck.meta$same(ptr %a, ptr %b) {
entry:
  br label %loop
loop:
  %i = phi i64 [ 0, %entry ], [ %next, %advance ]
  %pa = getelementptr i8, ptr %a, i64 %i
  %pb = getelementptr i8, ptr %b, i64 %i
  %ca = load i8, ptr %pa
  %cb = load i8, ptr %pb
  %equal = icmp eq i8 %ca, %cb
  br i1 %equal, label %check, label %differ
check:
  %end = icmp eq i8 %ca, 0
  br i1 %end, label %match, label %advance
advance:
  %next = add i64 %i, 1
  br label %loop
match:
  ret i1 true
differ:
  ret i1 false
}

define internal ptr @duck.meta$find(ptr %type, ptr %key, ptr %signature) {
entry:
  %countSlot = getelementptr i8, ptr %type, i64 16
  %count = load i64, ptr %countSlot
  %membersSlot = getelementptr i8, ptr %type, i64 24
  %members = load ptr, ptr %membersSlot
  br label %loop
loop:
  %i = phi i64 [ 0, %entry ], [ %next, %advance ]
  %more = icmp ult i64 %i, %count
  br i1 %more, label %body, label %missing
body:
  %member = getelementptr { ptr, ptr, ptr, ptr, ptr, i32, i32, ptr, ptr, i64, ptr }, ptr %members, i64 %i
  %flagsSlot = getelementptr { ptr, ptr, ptr, ptr, ptr, i32, i32, ptr, ptr, i64, ptr }, ptr %member, i32 0, i32 5
  %flags = load i32, ptr %flagsSlot
  %visible = icmp eq i32 %flags, 1
  br i1 %visible, label %compare, label %advance
compare:
  %nameSlot = getelementptr { ptr, ptr, ptr, ptr, ptr, i32, i32, ptr, ptr, i64, ptr }, ptr %member, i32 0, i32 1
  %name = load ptr, ptr %nameSlot
  %bytes = getelementptr i8, ptr %name, i64 16
  %same = call i1 @duck.meta$same(ptr %bytes, ptr %key)
  br i1 %same, label %shape, label %advance
shape:
  %any = icmp eq ptr %signature, null
  br i1 %any, label %found, label %exact
exact:
  %sigSlot = getelementptr { ptr, ptr, ptr, ptr, ptr, i32, i32, ptr, ptr, i64, ptr }, ptr %member, i32 0, i32 2
  %sig = load ptr, ptr %sigSlot
  %fits = call i1 @duck.meta$same(ptr %sig, ptr %signature)
  br i1 %fits, label %found, label %advance
advance:
  %next = add i64 %i, 1
  br label %loop
found:
  ret ptr %member
missing:
  ret ptr null
}
)IR";
}
} // namespace duck
