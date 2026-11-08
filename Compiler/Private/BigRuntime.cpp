#include "Pipeline.h"
namespace duck {
// Arbitrary-precision big/ubig values: immutable, reference-counted storage
//   {i64 referenceCount, i64 limbCount, i64 negative, i32 limbs[limbCount]}
// with little-endian 32-bit magnitude limbs, no leading zero limbs, and zero
// never negative. The first word matches managed text, so the text runtime's
// retain/release (and its literal count of -1) manage these values too.
// Every operation returns new storage; operands are never modified.
std::string bigRuntimeIR() {
  return R"IR(
declare void @llvm.memset.p0.i64(ptr, i8, i64, i1)
@duck.big.zero = private unnamed_addr constant [3 x i8] c"0\0A\00"

define internal ptr @duck.big.alloc(i64 %count) {
entry:
  %valid = icmp ult i64 %count, 268435456
  br i1 %valid, label %allocate, label %fail
allocate:
  %bytes = shl i64 %count, 2
  %total = add i64 %bytes, 24
  %p = call ptr @malloc(i64 %total)
  %ok = icmp ne ptr %p, null
  br i1 %ok, label %init, label %fail
init:
  store i64 1, ptr %p
  %countAt = getelementptr i8, ptr %p, i64 8
  store i64 %count, ptr %countAt
  %signAt = getelementptr i8, ptr %p, i64 16
  store i64 0, ptr %signAt
  %data = getelementptr i8, ptr %p, i64 24
  call void @llvm.memset.p0.i64(ptr %data, i8 0, i64 %bytes, i1 false)
  ret ptr %p
fail:
  call void @llvm.trap()
  unreachable
}
define internal i64 @duck.big.count(ptr %p) {
entry:
  %at = getelementptr i8, ptr %p, i64 8
  %n = load i64, ptr %at
  ret i64 %n
}
define internal i1 @duck.big.negative(ptr %p) {
entry:
  %at = getelementptr i8, ptr %p, i64 16
  %s = load i64, ptr %at
  %negative = icmp ne i64 %s, 0
  ret i1 %negative
}
define internal ptr @duck.big.at(ptr %p, i64 %i) {
entry:
  %data = getelementptr i8, ptr %p, i64 24
  %at = getelementptr i32, ptr %data, i64 %i
  ret ptr %at
}
; Limb i, or zero beyond the stored limbs.
define internal i64 @duck.big.limb(ptr %p, i64 %i) {
entry:
  %n = call i64 @duck.big.count(ptr %p)
  %inside = icmp ult i64 %i, %n
  br i1 %inside, label %read, label %zero
read:
  %at = call ptr @duck.big.at(ptr %p, i64 %i)
  %v = load i32, ptr %at
  %w = zext i32 %v to i64
  ret i64 %w
zero:
  ret i64 0
}
; Drops leading zero limbs and sets the sign; zero is never negative.
define internal ptr @duck.big.finish(ptr %p, i1 %negative) {
entry:
  %n0 = call i64 @duck.big.count(ptr %p)
  br label %loop
loop:
  %n = phi i64 [%n0, %entry], [%less, %drop]
  %empty = icmp eq i64 %n, 0
  br i1 %empty, label %done, label %check
check:
  %less = sub i64 %n, 1
  %top = call i64 @duck.big.limb(ptr %p, i64 %less)
  %isZero = icmp eq i64 %top, 0
  br i1 %isZero, label %drop, label %done
drop:
  br label %loop
done:
  %countAt = getelementptr i8, ptr %p, i64 8
  store i64 %n, ptr %countAt
  %nonzero = icmp ne i64 %n, 0
  %sign = and i1 %negative, %nonzero
  %signWord = zext i1 %sign to i64
  %signAt = getelementptr i8, ptr %p, i64 16
  store i64 %signWord, ptr %signAt
  ret ptr %p
}
; -1, 0, or 1 comparing magnitudes; tolerates leading zero limbs.
define internal i32 @duck.big.compareMagnitude(ptr %a, ptr %b) {
entry:
  %na = call i64 @duck.big.count(ptr %a)
  %nb = call i64 @duck.big.count(ptr %b)
  %aLonger = icmp ugt i64 %na, %nb
  %n = select i1 %aLonger, i64 %na, i64 %nb
  br label %loop
loop:
  %i = phi i64 [%n, %entry], [%next, %same]
  %end = icmp eq i64 %i, 0
  br i1 %end, label %equal, label %body
body:
  %next = sub i64 %i, 1
  %x = call i64 @duck.big.limb(ptr %a, i64 %next)
  %y = call i64 @duck.big.limb(ptr %b, i64 %next)
  %eq = icmp eq i64 %x, %y
  br i1 %eq, label %same, label %differ
same:
  br label %loop
differ:
  %greater = icmp ugt i64 %x, %y
  %r = select i1 %greater, i32 1, i32 -1
  ret i32 %r
equal:
  ret i32 0
}
define internal ptr @duck.big.addMagnitude(ptr %a, ptr %b, i1 %negative) {
entry:
  %na = call i64 @duck.big.count(ptr %a)
  %nb = call i64 @duck.big.count(ptr %b)
  %aLonger = icmp ugt i64 %na, %nb
  %longer = select i1 %aLonger, i64 %na, i64 %nb
  %n = add i64 %longer, 1
  %r = call ptr @duck.big.alloc(i64 %n)
  br label %loop
loop:
  %i = phi i64 [0, %entry], [%next, %body]
  %carry = phi i64 [0, %entry], [%carryOut, %body]
  %end = icmp eq i64 %i, %n
  br i1 %end, label %done, label %body
body:
  %x = call i64 @duck.big.limb(ptr %a, i64 %i)
  %y = call i64 @duck.big.limb(ptr %b, i64 %i)
  %s1 = add i64 %x, %y
  %s = add i64 %s1, %carry
  %low = trunc i64 %s to i32
  %at = call ptr @duck.big.at(ptr %r, i64 %i)
  store i32 %low, ptr %at
  %carryOut = lshr i64 %s, 32
  %next = add i64 %i, 1
  br label %loop
done:
  %f = call ptr @duck.big.finish(ptr %r, i1 %negative)
  ret ptr %f
}
; |a| - |b| for |a| >= |b|.
define internal ptr @duck.big.subtractMagnitude(ptr %a, ptr %b, i1 %negative) {
entry:
  %n = call i64 @duck.big.count(ptr %a)
  %r = call ptr @duck.big.alloc(i64 %n)
  br label %loop
loop:
  %i = phi i64 [0, %entry], [%next, %body]
  %borrow = phi i64 [0, %entry], [%borrowOut, %body]
  %end = icmp eq i64 %i, %n
  br i1 %end, label %done, label %body
body:
  %x = call i64 @duck.big.limb(ptr %a, i64 %i)
  %y = call i64 @duck.big.limb(ptr %b, i64 %i)
  %d1 = sub i64 %x, %y
  %d = sub i64 %d1, %borrow
  %low = trunc i64 %d to i32
  %at = call ptr @duck.big.at(ptr %r, i64 %i)
  store i32 %low, ptr %at
  %under = icmp slt i64 %d, 0
  %borrowOut = zext i1 %under to i64
  %next = add i64 %i, 1
  br label %loop
done:
  %f = call ptr @duck.big.finish(ptr %r, i1 %negative)
  ret ptr %f
}
; a + b, or a - b when %subtract is set.
define internal ptr @duck.big.add(ptr %a, ptr %b, i1 %subtract) {
entry:
  %sa = call i1 @duck.big.negative(ptr %a)
  %sb0 = call i1 @duck.big.negative(ptr %b)
  %sb = xor i1 %sb0, %subtract
  %same = icmp eq i1 %sa, %sb
  br i1 %same, label %sum, label %difference
sum:
  %s = call ptr @duck.big.addMagnitude(ptr %a, ptr %b, i1 %sa)
  ret ptr %s
difference:
  %c = call i32 @duck.big.compareMagnitude(ptr %a, ptr %b)
  %aLarger = icmp sge i32 %c, 0
  br i1 %aLarger, label %aFirst, label %bFirst
aFirst:
  %d1 = call ptr @duck.big.subtractMagnitude(ptr %a, ptr %b, i1 %sa)
  ret ptr %d1
bFirst:
  %d2 = call ptr @duck.big.subtractMagnitude(ptr %b, ptr %a, i1 %sb)
  ret ptr %d2
}
define internal ptr @duck.big.multiply(ptr %a, ptr %b) {
entry:
  %na = call i64 @duck.big.count(ptr %a)
  %nb = call i64 @duck.big.count(ptr %b)
  %n = add i64 %na, %nb
  %r = call ptr @duck.big.alloc(i64 %n)
  br label %outer
outer:
  %i = phi i64 [0, %entry], [%iNext, %innerDone]
  %endI = icmp eq i64 %i, %na
  br i1 %endI, label %done, label %outerBody
outerBody:
  %x = call i64 @duck.big.limb(ptr %a, i64 %i)
  br label %inner
inner:
  %j = phi i64 [0, %outerBody], [%jNext, %innerBody]
  %carry = phi i64 [0, %outerBody], [%carryOut, %innerBody]
  %endJ = icmp eq i64 %j, %nb
  br i1 %endJ, label %innerDone, label %innerBody
innerBody:
  %y = call i64 @duck.big.limb(ptr %b, i64 %j)
  %k = add i64 %i, %j
  %at = call ptr @duck.big.at(ptr %r, i64 %k)
  %old32 = load i32, ptr %at
  %old = zext i32 %old32 to i64
  %product = mul i64 %x, %y
  %t1 = add i64 %product, %old
  %t = add i64 %t1, %carry
  %low = trunc i64 %t to i32
  store i32 %low, ptr %at
  %carryOut = lshr i64 %t, 32
  %jNext = add i64 %j, 1
  br label %inner
innerDone:
  %top = add i64 %i, %nb
  %topAt = call ptr @duck.big.at(ptr %r, i64 %top)
  %carry32 = trunc i64 %carry to i32
  store i32 %carry32, ptr %topAt
  %iNext = add i64 %i, 1
  br label %outer
done:
  %sa = call i1 @duck.big.negative(ptr %a)
  %sb = call i1 @duck.big.negative(ptr %b)
  %sign = xor i1 %sa, %sb
  %f = call ptr @duck.big.finish(ptr %r, i1 %sign)
  ret ptr %f
}
; r = 2r + bit across r's stored limbs.
define internal void @duck.big.shiftInBit(ptr %r, i64 %bit) {
entry:
  %n = call i64 @duck.big.count(ptr %r)
  br label %loop
loop:
  %i = phi i64 [0, %entry], [%next, %body]
  %carry = phi i64 [%bit, %entry], [%carryOut, %body]
  %end = icmp eq i64 %i, %n
  br i1 %end, label %done, label %body
body:
  %x = call i64 @duck.big.limb(ptr %r, i64 %i)
  %doubled = shl i64 %x, 1
  %t = or i64 %doubled, %carry
  %low = trunc i64 %t to i32
  %at = call ptr @duck.big.at(ptr %r, i64 %i)
  store i32 %low, ptr %at
  %carryOut = lshr i64 %t, 32
  %next = add i64 %i, 1
  br label %loop
done:
  ret void
}
; r -= b in place, for |r| >= |b|.
define internal void @duck.big.subtractInPlace(ptr %r, ptr %b) {
entry:
  %n = call i64 @duck.big.count(ptr %r)
  br label %loop
loop:
  %i = phi i64 [0, %entry], [%next, %body]
  %borrow = phi i64 [0, %entry], [%borrowOut, %body]
  %end = icmp eq i64 %i, %n
  br i1 %end, label %done, label %body
body:
  %x = call i64 @duck.big.limb(ptr %r, i64 %i)
  %y = call i64 @duck.big.limb(ptr %b, i64 %i)
  %d1 = sub i64 %x, %y
  %d = sub i64 %d1, %borrow
  %low = trunc i64 %d to i32
  %at = call ptr @duck.big.at(ptr %r, i64 %i)
  store i32 %low, ptr %at
  %under = icmp slt i64 %d, 0
  %borrowOut = zext i1 %under to i64
  %next = add i64 %i, 1
  br label %loop
done:
  ret void
}
; Truncating division by binary long division. The quotient's sign is the
; xor of the operand signs; the remainder keeps the dividend's sign.
define internal ptr @duck.big.divide(ptr %a, ptr %b, i1 %wantRemainder) {
entry:
  %nb = call i64 @duck.big.count(ptr %b)
  %byZero = icmp eq i64 %nb, 0
  br i1 %byZero, label %fail, label %start
start:
  %na = call i64 @duck.big.count(ptr %a)
  %q = call ptr @duck.big.alloc(i64 %na)
  %rn = add i64 %nb, 1
  %r = call ptr @duck.big.alloc(i64 %rn)
  %bits = shl i64 %na, 5
  br label %loop
loop:
  %bit = phi i64 [%bits, %start], [%index, %next]
  %finished = icmp eq i64 %bit, 0
  br i1 %finished, label %done, label %step
step:
  %index = sub i64 %bit, 1
  %word = lshr i64 %index, 5
  %offset = and i64 %index, 31
  %limb = call i64 @duck.big.limb(ptr %a, i64 %word)
  %shifted = lshr i64 %limb, %offset
  %incoming = and i64 %shifted, 1
  call void @duck.big.shiftInBit(ptr %r, i64 %incoming)
  %c = call i32 @duck.big.compareMagnitude(ptr %r, ptr %b)
  %fits = icmp sge i32 %c, 0
  br i1 %fits, label %subtract, label %next
subtract:
  call void @duck.big.subtractInPlace(ptr %r, ptr %b)
  %qAt = call ptr @duck.big.at(ptr %q, i64 %word)
  %qOld = load i32, ptr %qAt
  %mask64 = shl i64 1, %offset
  %mask = trunc i64 %mask64 to i32
  %qNew = or i32 %qOld, %mask
  store i32 %qNew, ptr %qAt
  br label %next
next:
  br label %loop
done:
  %sa = call i1 @duck.big.negative(ptr %a)
  %sb = call i1 @duck.big.negative(ptr %b)
  %qs = xor i1 %sa, %sb
  br i1 %wantRemainder, label %remainder, label %quotient
quotient:
  call void @duck.text.release(ptr %r)
  %fq = call ptr @duck.big.finish(ptr %q, i1 %qs)
  ret ptr %fq
remainder:
  call void @duck.text.release(ptr %q)
  %fr = call ptr @duck.big.finish(ptr %r, i1 %sa)
  ret ptr %fr
fail:
  call void @llvm.trap()
  unreachable
}
define internal i32 @duck.big.compare(ptr %a, ptr %b) {
entry:
  %sa = call i1 @duck.big.negative(ptr %a)
  %sb = call i1 @duck.big.negative(ptr %b)
  %differ = icmp ne i1 %sa, %sb
  br i1 %differ, label %signs, label %same
signs:
  %bySign = select i1 %sa, i32 -1, i32 1
  ret i32 %bySign
same:
  %c = call i32 @duck.big.compareMagnitude(ptr %a, ptr %b)
  %flipped = sub i32 0, %c
  %r = select i1 %sa, i32 %flipped, i32 %c
  ret i32 %r
}
define internal ptr @duck.big.negate(ptr %a) {
entry:
  %n = call i64 @duck.big.count(ptr %a)
  %r = call ptr @duck.big.alloc(i64 %n)
  %from = call ptr @duck.big.at(ptr %a, i64 0)
  %to = call ptr @duck.big.at(ptr %r, i64 0)
  %bytes = shl i64 %n, 2
  call void @llvm.memcpy.p0.p0.i64(ptr %to, ptr %from, i64 %bytes, i1 false)
  %sa = call i1 @duck.big.negative(ptr %a)
  %flip = xor i1 %sa, true
  %f = call ptr @duck.big.finish(ptr %r, i1 %flip)
  ret ptr %f
}
; Unsigned subtraction and conversions to ubig trap instead of going negative.
define internal ptr @duck.big.requireUnsigned(ptr %p) {
entry:
  %negative = call i1 @duck.big.negative(ptr %p)
  br i1 %negative, label %fail, label %ok
ok:
  ret ptr %p
fail:
  call void @llvm.trap()
  unreachable
}
define internal ptr @duck.big.fromUnsigned(i64 %v, i1 %negative) {
entry:
  %r = call ptr @duck.big.alloc(i64 2)
  %low = trunc i64 %v to i32
  %high64 = lshr i64 %v, 32
  %high = trunc i64 %high64 to i32
  %lowAt = call ptr @duck.big.at(ptr %r, i64 0)
  store i32 %low, ptr %lowAt
  %highAt = call ptr @duck.big.at(ptr %r, i64 1)
  store i32 %high, ptr %highAt
  %f = call ptr @duck.big.finish(ptr %r, i1 %negative)
  ret ptr %f
}
define internal ptr @duck.big.fromSigned(i64 %v) {
entry:
  %negative = icmp slt i64 %v, 0
  %flipped = sub i64 0, %v
  %magnitude = select i1 %negative, i64 %flipped, i64 %v
  %r = call ptr @duck.big.fromUnsigned(i64 %magnitude, i1 %negative)
  ret ptr %r
}
; Low 64 bits of the two's-complement value; narrowing casts keep low bits.
define internal i64 @duck.big.low64(ptr %p) {
entry:
  %l0 = call i64 @duck.big.limb(ptr %p, i64 0)
  %l1 = call i64 @duck.big.limb(ptr %p, i64 1)
  %high = shl i64 %l1, 32
  %magnitude = or i64 %high, %l0
  %negative = call i1 @duck.big.negative(ptr %p)
  %flipped = sub i64 0, %magnitude
  %r = select i1 %negative, i64 %flipped, i64 %magnitude
  ret i64 %r
}
define internal void @duck.big.print(ptr %p) {
entry:
  %n = call i64 @duck.big.count(ptr %p)
  %isZero = icmp eq i64 %n, 0
  br i1 %isZero, label %zero, label %digits
zero:
  call i32 (ptr, ...) @printf(ptr @duck.big.zero)
  ret void
digits:
  %copy = call ptr @duck.big.alloc(i64 %n)
  %from = call ptr @duck.big.at(ptr %p, i64 0)
  %to = call ptr @duck.big.at(ptr %copy, i64 0)
  %bytes = shl i64 %n, 2
  call void @llvm.memcpy.p0.p0.i64(ptr %to, ptr %from, i64 %bytes, i1 false)
  %twice = shl i64 %n, 1
  %groupCount = add i64 %twice, 2
  %groups = call ptr @duck.big.alloc(i64 %groupCount)
  %groupData = call ptr @duck.big.at(ptr %groups, i64 0)
  %negative = call i1 @duck.big.negative(ptr %p)
  call void @duck.wide.digits(ptr %to, i64 %n, ptr %groupData, i1 %negative)
  call void @duck.text.release(ptr %copy)
  call void @duck.text.release(ptr %groups)
  ret void
}
)IR";
}
} // namespace duck
