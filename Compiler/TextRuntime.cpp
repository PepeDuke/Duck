#include "Pipeline.h"
namespace duck {
// Immutable text storage: {i64 referenceCount, i64 codeUnits, trailing data}.
// Literals have referenceCount = -1. Dynamic values are released deterministically.
// Module-local helpers share the same storage ABI without duplicate linker symbols.
// They are emitted with the program, so direct duckc -> clang builds need no SDK.
std::string textRuntimeIR() {
  return R"IR(
declare ptr @malloc(i64)
declare void @free(ptr)
declare i32 @putchar(i32)
declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)

define internal void @duck.text.retain(ptr %p) {
entry:
  %nil = icmp eq ptr %p, null
  br i1 %nil, label %done, label %read
read:
  %rc = load i64, ptr %p
  %dynamic = icmp ne i64 %rc, -1
  br i1 %dynamic, label %inc, label %done
inc:
  %next = add i64 %rc, 1
  store i64 %next, ptr %p
  br label %done
done:
  ret void
}
define internal void @duck.text.release(ptr %p) {
entry:
  %nil = icmp eq ptr %p, null
  br i1 %nil, label %done, label %read
read:
  %rc = load i64, ptr %p
  %dynamic = icmp ne i64 %rc, -1
  br i1 %dynamic, label %dec, label %done
dec:
  %next = sub i64 %rc, 1
  store i64 %next, ptr %p
  %last = icmp eq i64 %next, 0
  br i1 %last, label %destroy, label %done
destroy:
  call void @free(ptr %p)
  br label %done
done:
  ret void
}
define internal i64 @duck.text.length(ptr %p) {
  %address = getelementptr i8, ptr %p, i64 8
  %length = load i64, ptr %address
  ret i64 %length
}
define internal void @duck.text.finish(ptr %p, i64 %length, i64 %width) {
  %lp = getelementptr i8, ptr %p, i64 8
  store i64 %length, ptr %lp
  %bytes = mul i64 %length, %width
  %offset = add i64 %bytes, 16
  %end = getelementptr i8, ptr %p, i64 %offset
  %wide = icmp eq i64 %width, 2
  br i1 %wide, label %utf16, label %utf8
utf16:
  store i16 0, ptr %end
  ret void
utf8:
  store i8 0, ptr %end
  ret void
}
define internal ptr @duck.text.alloc(i64 %capacity, i64 %width) {
  %max = udiv i64 9223372036854775790, %width
  %valid = icmp ult i64 %capacity, %max
  br i1 %valid, label %allocate, label %fail
allocate:
  %units = add i64 %capacity, 1
  %bytes = mul i64 %units, %width
  %total = add i64 %bytes, 16
  %p = call ptr @malloc(i64 %total)
  %ok = icmp ne ptr %p, null
  br i1 %ok, label %init, label %fail
init:
  store i64 1, ptr %p
  call void @duck.text.finish(ptr %p, i64 %capacity, i64 %width)
  ret ptr %p
fail:
  call void @llvm.trap()
  unreachable
}
define internal ptr @duck.text.concat(ptr %a, ptr %b, i64 %width) {
  %al = call i64 @duck.text.length(ptr %a)
  %bl = call i64 @duck.text.length(ptr %b)
  %length = add i64 %al, %bl
  %p = call ptr @duck.text.alloc(i64 %length, i64 %width)
  %ad = getelementptr i8, ptr %a, i64 16
  %bd = getelementptr i8, ptr %b, i64 16
  %pd = getelementptr i8, ptr %p, i64 16
  %ab = mul i64 %al, %width
  %bb = mul i64 %bl, %width
  call void @llvm.memcpy.p0.p0.i64(ptr %pd, ptr %ad, i64 %ab, i1 false)
  %tail = getelementptr i8, ptr %pd, i64 %ab
  call void @llvm.memcpy.p0.p0.i64(ptr %tail, ptr %bd, i64 %bb, i1 false)
  ret ptr %p
}
define internal i1 @duck.text.equal(ptr %a, ptr %b, i64 %width) {
entry:
  %al = call i64 @duck.text.length(ptr %a)
  %bl = call i64 @duck.text.length(ptr %b)
  %same = icmp eq i64 %al, %bl
  br i1 %same, label %setup, label %no
setup:
  %bytes = mul i64 %al, %width
  %ad = getelementptr i8, ptr %a, i64 16
  %bd = getelementptr i8, ptr %b, i64 16
  br label %loop
loop:
  %i = phi i64 [0, %setup], [%next, %compare]
  %end = icmp eq i64 %i, %bytes
  br i1 %end, label %yes, label %compare
compare:
  %ap = getelementptr i8, ptr %ad, i64 %i
  %bp = getelementptr i8, ptr %bd, i64 %i
  %av = load i8, ptr %ap
  %bv = load i8, ptr %bp
  %eq = icmp eq i8 %av, %bv
  %next = add i64 %i, 1
  br i1 %eq, label %loop, label %no
yes:
  ret i1 true
no:
  ret i1 false
}
define internal i32 @duck.text.unit(ptr %p, i64 %index, i64 %width) {
  %length = call i64 @duck.text.length(ptr %p)
  %valid = icmp ult i64 %index, %length
  br i1 %valid, label %read, label %fail
read:
  %bytes = mul i64 %index, %width
  %offset = add i64 %bytes, 16
  %at = getelementptr i8, ptr %p, i64 %offset
  %wide = icmp eq i64 %width, 2
  br i1 %wide, label %utf16, label %utf8
utf16:
  %v16 = load i16, ptr %at
  %wideValue = zext i16 %v16 to i32
  ret i32 %wideValue
utf8:
  %v8 = load i8, ptr %at
  %byteValue = zext i8 %v8 to i32
  ret i32 %byteValue
fail:
  call void @llvm.trap()
  unreachable
}
; Decode storage that is known valid: only validated literals, concatenation and
; encoding conversion can construct managed text. Native calls receive read-only borrows.
define internal {i32, i64} @duck.text.decode(ptr %p, i64 %i, i64 %width) {
entry:
  %first = call i32 @duck.text.unit(ptr %p, i64 %i, i64 %width)
  %next = add i64 %i, 1
  %wide = icmp eq i64 %width, 2
  br i1 %wide, label %utf16, label %utf8
utf16:
  %hi = and i32 %first, 64512
  %pair = icmp eq i32 %hi, 55296
  br i1 %pair, label %surrogate, label %single
surrogate:
  %low = call i32 @duck.text.unit(ptr %p, i64 %next, i64 2)
  %h = sub i32 %first, 55296
  %shift = shl i32 %h, 10
  %l = sub i32 %low, 56320
  %sum = add i32 %shift, %l
  %scalar = add i32 %sum, 65536
  %after = add i64 %next, 1
  %r0 = insertvalue {i32, i64} poison, i32 %scalar, 0
  %r1 = insertvalue {i32, i64} %r0, i64 %after, 1
  ret {i32, i64} %r1
utf8:
  %ascii = icmp ult i32 %first, 128
  br i1 %ascii, label %single, label %multi
multi:
  %two = icmp ult i32 %first, 224
  %three = icmp ult i32 %first, 240
  %largeCount = select i1 %three, i64 3, i64 4
  %count = select i1 %two, i64 2, i64 %largeCount
  %largeMask = select i1 %three, i32 15, i32 7
  %mask = select i1 %two, i32 31, i32 %largeMask
  %initial = and i32 %first, %mask
  %limit = add i64 %i, %count
  br label %loop
loop:
  %index = phi i64 [%next, %multi], [%advance, %body]
  %cp = phi i32 [%initial, %multi], [%combined, %body]
  %done = icmp eq i64 %index, %limit
  br i1 %done, label %decoded, label %body
body:
  %byte = call i32 @duck.text.unit(ptr %p, i64 %index, i64 1)
  %bits = and i32 %byte, 63
  %high = shl i32 %cp, 6
  %combined = or i32 %high, %bits
  %advance = add i64 %index, 1
  br label %loop
decoded:
  %r2 = insertvalue {i32, i64} poison, i32 %cp, 0
  %r3 = insertvalue {i32, i64} %r2, i64 %index, 1
  ret {i32, i64} %r3
single:
  %r4 = insertvalue {i32, i64} poison, i32 %first, 0
  %r5 = insertvalue {i32, i64} %r4, i64 %next, 1
  ret {i32, i64} %r5
}
define internal i64 @duck.text.encode(ptr %p, i64 %i, i32 %cp, i64 %width) {
entry:
  %data = getelementptr i8, ptr %p, i64 16
  %wide = icmp eq i64 %width, 2
  br i1 %wide, label %utf16, label %utf8
utf16:
  %at16 = getelementptr i16, ptr %data, i64 %i
  %bmp = icmp ult i32 %cp, 65536
  br i1 %bmp, label %single16, label %pair16
single16:
  %v16 = trunc i32 %cp to i16
  store i16 %v16, ptr %at16
  %next16 = add i64 %i, 1
  ret i64 %next16
pair16:
  %base = sub i32 %cp, 65536
  %h = lshr i32 %base, 10
  %high = or i32 %h, 55296
  %l = and i32 %base, 1023
  %low = or i32 %l, 56320
  %h16 = trunc i32 %high to i16
  %l16 = trunc i32 %low to i16
  store i16 %h16, ptr %at16
  %second = getelementptr i16, ptr %at16, i64 1
  store i16 %l16, ptr %second
  %after16 = add i64 %i, 2
  ret i64 %after16
utf8:
  %ascii = icmp ult i32 %cp, 128
  br i1 %ascii, label %single8, label %multi
single8:
  %at8 = getelementptr i8, ptr %data, i64 %i
  %v8 = trunc i32 %cp to i8
  store i8 %v8, ptr %at8
  %next8 = add i64 %i, 1
  ret i64 %next8
multi:
  %two = icmp ult i32 %cp, 2048
  %three = icmp ult i32 %cp, 65536
  %largeCount = select i1 %three, i64 3, i64 4
  %count = select i1 %two, i64 2, i64 %largeCount
  %largePrefix = select i1 %three, i32 224, i32 240
  %prefix = select i1 %two, i32 192, i32 %largePrefix
  %rest = sub i64 %count, 1
  %shift64 = mul i64 %rest, 6
  %shift = trunc i64 %shift64 to i32
  %top = lshr i32 %cp, %shift
  %first = or i32 %top, %prefix
  %first8 = trunc i32 %first to i8
  %at = getelementptr i8, ptr %data, i64 %i
  store i8 %first8, ptr %at
  br label %loop
loop:
  %n = phi i64 [1, %multi], [%next, %body]
  %done = icmp eq i64 %n, %count
  br i1 %done, label %end, label %body
body:
  %remaining = sub i64 %rest, %n
  %bits64 = mul i64 %remaining, 6
  %bits = trunc i64 %bits64 to i32
  %part = lshr i32 %cp, %bits
  %masked = and i32 %part, 63
  %byte = or i32 %masked, 128
  %out = trunc i32 %byte to i8
  %offset = add i64 %i, %n
  %dest = getelementptr i8, ptr %data, i64 %offset
  store i8 %out, ptr %dest
  %next = add i64 %n, 1
  br label %loop
end:
  %after = add i64 %i, %count
  ret i64 %after
}
define internal ptr @duck.text.convert(ptr %p, i64 %from, i64 %to) {
entry:
  %length = call i64 @duck.text.length(ptr %p)
  %narrow = icmp eq i64 %to, 1
  %factor = select i1 %narrow, i64 3, i64 1
  %max = udiv i64 9223372036854775790, %factor
  %valid = icmp ult i64 %length, %max
  br i1 %valid, label %setup, label %fail
setup:
  %capacity = mul i64 %length, %factor
  %out = call ptr @duck.text.alloc(i64 %capacity, i64 %to)
  br label %loop
loop:
  %i = phi i64 [0, %setup], [%next, %body]
  %written = phi i64 [0, %setup], [%after, %body]
  %done = icmp eq i64 %i, %length
  br i1 %done, label %end, label %body
body:
  %decoded = call {i32, i64} @duck.text.decode(ptr %p, i64 %i, i64 %from)
  %cp = extractvalue {i32, i64} %decoded, 0
  %next = extractvalue {i32, i64} %decoded, 1
  %after = call i64 @duck.text.encode(ptr %out, i64 %written, i32 %cp, i64 %to)
  br label %loop
end:
  call void @duck.text.finish(ptr %out, i64 %written, i64 %to)
  ret ptr %out
fail:
  call void @llvm.trap()
  unreachable
}
define internal void @duck.text.print(ptr %p, i64 %width) {
entry:
  %wide = icmp eq i64 %width, 2
  br i1 %wide, label %convert, label %borrow
convert:
  %converted = call ptr @duck.text.convert(ptr %p, i64 2, i64 1)
  br label %setup
borrow:
  call void @duck.text.retain(ptr %p)
  br label %setup
setup:
  %text = phi ptr [%converted, %convert], [%p, %borrow]
  %length = call i64 @duck.text.length(ptr %text)
  br label %loop
loop:
  %i = phi i64 [0, %setup], [%next, %body]
  %done = icmp eq i64 %i, %length
  br i1 %done, label %end, label %body
body:
  %byte = call i32 @duck.text.unit(ptr %text, i64 %i, i64 1)
  call i32 @putchar(i32 %byte)
  %next = add i64 %i, 1
  br label %loop
end:
  call i32 @putchar(i32 10)
  call void @duck.text.release(ptr %text)
  ret void
}
)IR";
}
} // namespace duck
