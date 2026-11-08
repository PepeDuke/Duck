#include "Pipeline.h"
namespace duck {
// The compiler emits for its own host, so the host picks the native entry.
// wmain supplies UTF-16 directly, the same form a Duck string stores. MinGW
// links wmain only with -municode, so it keeps main and its byte arguments.
#if defined(_WIN32) && !defined(__MINGW32__)
constexpr bool wideEntry = true;
#else
constexpr bool wideEntry = false;
#endif
// Managed text must always be valid, because @duck.text.decode does not check
// it, while the operating system promises nothing about arguments. Both
// decoders write U+FFFD for each unpaired surrogate or malformed byte. Input
// ends in a zero unit, which is neither a low surrogate nor a continuation
// byte, so looking one unit ahead cannot pass the end.
std::string entryArgumentsIR() {
  const std::string unit = wideEntry ? "i16" : "i8";
  std::string ir = "define internal ptr @duck.args$text(ptr %native) {\nentry:\n  br label %scan\nscan:\n"
                   "  %n = phi i64 [0, %entry], [%following, %scan]\n"
                   "  %unitp = getelementptr " + unit + ", ptr %native, i64 %n\n"
                   "  %unit = load " + unit + ", ptr %unitp\n"
                   "  %following = add i64 %n, 1\n"
                   "  %ended = icmp eq " + unit + " %unit, 0\n"
                   "  br i1 %ended, label %ready, label %scan\n";
  ir += R"IR(ready:
  ; Every malformed unit or byte becomes one UTF-16 unit, so n units suffice.
  %out = call ptr @duck.text.alloc(i64 %n, i64 2)
  br label %loop
loop:
  %i = phi i64 [0, %ready], [%advance, %emit]
  %written = phi i64 [0, %ready], [%after, %emit]
  %more = icmp ult i64 %i, %n
  br i1 %more, label %lead, label %end
)IR";
  ir += wideEntry ? R"IR(lead:
  %leadp = getelementptr i16, ptr %native, i64 %i
  %lead16 = load i16, ptr %leadp
  %first = zext i16 %lead16 to i32
  %kind = and i32 %first, 64512
  %high = icmp eq i32 %kind, 55296
  br i1 %high, label %pair, label %single
pair:
  %secondi = add i64 %i, 1
  %secondp = getelementptr i16, ptr %native, i64 %secondi
  %second16 = load i16, ptr %secondp
  %second = zext i16 %second16 to i32
  %secondKind = and i32 %second, 64512
  %paired = icmp eq i32 %secondKind, 56320
  br i1 %paired, label %joined, label %invalid
joined:
  %h = sub i32 %first, 55296
  %shifted = shl i32 %h, 10
  %l = sub i32 %second, 56320
  %sum = add i32 %shifted, %l
  %cp = add i32 %sum, 65536
  br label %emit
single:
  %low = icmp eq i32 %kind, 56320
  br i1 %low, label %invalid, label %plain
plain:
  br label %emit
invalid:
  br label %emit
emit:
  %scalar = phi i32 [%first, %plain], [%cp, %joined], [65533, %invalid]
  %length = phi i64 [1, %plain], [2, %joined], [1, %invalid]
)IR" : R"IR(lead:
  %leadp = getelementptr i8, ptr %native, i64 %i
  %lead8 = load i8, ptr %leadp
  %first = zext i8 %lead8 to i32
  %ascii = icmp ult i32 %first, 128
  br i1 %ascii, label %plain, label %multi
plain:
  br label %emit
multi:
  %under = icmp ult i32 %first, 194
  %over = icmp ugt i32 %first, 244
  %badLead = or i1 %under, %over
  br i1 %badLead, label %invalid, label %start
start:
  %two = icmp ult i32 %first, 224
  %three = icmp ult i32 %first, 240
  %largeNeed = select i1 %three, i64 2, i64 3
  %need = select i1 %two, i64 1, i64 %largeNeed
  %largeMask = select i1 %three, i32 15, i32 7
  %mask = select i1 %two, i32 31, i32 %largeMask
  %largeLeast = select i1 %three, i32 2048, i32 65536
  %least = select i1 %two, i32 128, i32 %largeLeast
  %initial = and i32 %first, %mask
  br label %tail
tail:
  %k = phi i64 [1, %start], [%nextk, %accept]
  %cp = phi i32 [%initial, %start], [%combined, %accept]
  %complete = icmp ugt i64 %k, %need
  br i1 %complete, label %check, label %continuation
continuation:
  %offset = add i64 %i, %k
  %bytep = getelementptr i8, ptr %native, i64 %offset
  %byte8 = load i8, ptr %bytep
  %byte = zext i8 %byte8 to i32
  %tag = and i32 %byte, 192
  %follows = icmp eq i32 %tag, 128
  br i1 %follows, label %accept, label %invalid
accept:
  %bits = and i32 %byte, 63
  %raised = shl i32 %cp, 6
  %combined = or i32 %raised, %bits
  %nextk = add i64 %k, 1
  br label %tail
check:
  %small = icmp ult i32 %cp, %least
  %large = icmp ugt i32 %cp, 1114111
  %range = and i32 %cp, -2048
  %surrogate = icmp eq i32 %range, 55296
  %wrong = or i1 %small, %large
  %rejected = or i1 %wrong, %surrogate
  br i1 %rejected, label %invalid, label %valid
valid:
  %width = add i64 %need, 1
  br label %emit
invalid:
  br label %emit
emit:
  %scalar = phi i32 [%first, %plain], [%cp, %valid], [65533, %invalid]
  %length = phi i64 [1, %plain], [%width, %valid], [1, %invalid]
)IR";
  ir += R"IR(  %after = call i64 @duck.text.encode(ptr %out, i64 %written, i32 %scalar, i64 2)
  %advance = add i64 %i, %length
  br label %loop
end:
  call void @duck.text.finish(ptr %out, i64 %written, i64 2)
  ret ptr %out
}
)IR";
  ir += std::string("define i32 @") + (wideEntry ? "wmain" : "main") + R"IR((i32 %argc, ptr %argv) {
entry:
  %args = alloca ptr
  store ptr null, ptr %args
  br label %loop
loop:
  ; args omits argv[0], the executable's own name.
  %i = phi i32 [1, %entry], [%next, %body]
  %more = icmp slt i32 %i, %argc
  br i1 %more, label %body, label %run
body:
  %from = getelementptr ptr, ptr %argv, i32 %i
  %native = load ptr, ptr %from
  %text = call ptr @duck.args$text(ptr %native)
  %slot = call ptr @duck.seq$push(ptr %args, i64 8)
  store ptr %text, ptr %slot
  %next = add i32 %i, 1
  br label %loop
run:
  call void @duck.Main(ptr %args)
  %owned = load ptr, ptr %args
  call void @duck.seq$free(ptr %owned, ptr @duck$slot.release)
  ret i32 0
}
)IR";
  return ir;
}
// Immutable text storage: {i64 referenceCount, i64 codeUnits, trailing data}.
// Literals have referenceCount = -1. Dynamic values are released deterministically.
// Module-local helpers share the same storage ABI without duplicate linker symbols.
// They are emitted with the program, so direct duckc -> clang builds need no SDK.
std::string textRuntimeIR() {
  return R"IR(
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
; One-character text from a validated char (Unicode scalar).
define internal ptr @duck.char.text(i32 %cp, i64 %width) {
entry:
  %p = call ptr @duck.text.alloc(i64 4, i64 %width)
  %length = call i64 @duck.text.encode(ptr %p, i64 0, i32 %cp, i64 %width)
  call void @duck.text.finish(ptr %p, i64 %length, i64 %width)
  ret ptr %p
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
