#include "Pipeline.h"

namespace duck {
// Heap bindings are function-local. The low bit on an owner distinguishes
// malloc-aligned payloads from promoted blocks. An alias holds the untagged
// block address. The count includes the owner until it closes the block.
std::string heapRuntimeIR() {
  return R"IR(
%duck.heap.block = type { ptr, i64 }

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
  %flag = and i64 %bits, 1
  %direct = icmp eq i64 %flag, 0
  %raw = and i1 %owner, %direct
  %nil = icmp eq ptr %word, null
  %ready = or i1 %nil, %raw
  br i1 %ready, label %immediate, label %indirect
immediate:
  ret ptr %word
indirect:
  %address = and i64 %bits, -2
  %block = inttoptr i64 %address to ptr
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
  %flag = and i64 %bits, 1
  %direct = icmp eq i64 %flag, 0
  %create = and i1 %owner, %direct
  br i1 %create, label %promote, label %retain
promote:
  %new = call ptr @duck.heap$alloc(i64 16)
  store ptr %word, ptr %new
  %count = getelementptr %duck.heap.block, ptr %new, i32 0, i32 1
  store i64 2, ptr %count
  %addr = ptrtoint ptr %new to i64
  %tag = or i64 %addr, 1
  %tagged = inttoptr i64 %tag to ptr
  store ptr %tagged, ptr %slot
  ret ptr %new
retain:
  %addr2 = and i64 %bits, -2
  %block = inttoptr i64 %addr2 to ptr
  %rc = getelementptr %duck.heap.block, ptr %block, i32 0, i32 1
  %old = load i64, ptr %rc
  %next = add i64 %old, 1
  store i64 %next, ptr %rc
  ret ptr %block
}

define internal void @duck.heap$drop(ptr %slot, i1 %owner) {
  %word = load ptr, ptr %slot
  store ptr null, ptr %slot
  %nil = icmp eq ptr %word, null
  br i1 %nil, label %done, label %classify
classify:
  %bits = ptrtoint ptr %word to i64
  %flag = and i64 %bits, 1
  %direct = icmp eq i64 %flag, 0
  %raw = and i1 %owner, %direct
  br i1 %raw, label %freeRaw, label %indirect
freeRaw:
  call void @free(ptr %word)
  br label %done
indirect:
  %address = and i64 %bits, -2
  %block = inttoptr i64 %address to ptr
  br i1 %owner, label %close, label %release
close:
  %p = load ptr, ptr %block
  store ptr null, ptr %block
  call void @free(ptr %p)
  br label %release
release:
  %rc = getelementptr %duck.heap.block, ptr %block, i32 0, i32 1
  %old = load i64, ptr %rc
  %next = sub i64 %old, 1
  store i64 %next, ptr %rc
  %last = icmp eq i64 %next, 0
  br i1 %last, label %freeBlock, label %done
freeBlock:
  call void @free(ptr %block)
  br label %done
done:
  ret void
}
)IR";
}
} // namespace duck
