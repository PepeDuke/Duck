#include "Pipeline.h"
namespace duck {
// tiny/utiny: decimal floating point with 69 significant digits.
//   {i256 coefficient, i32 exponent}  value = coefficient * 10^exponent
// |coefficient| < 10^69 and the adjusted exponent (the power of ten of the
// leading digit) lies in -999999999..999999999. Every result is rounded once
// to 69 significant digits, ties to even; leaving the exponent range traps.
// Intermediates use i512, which LLVM expands inline (no compiler-rt calls).
// Representations are not canonical: 1.0 and 1.00 compare equal.
std::string decimalRuntimeIR() {
  const auto e68 = "1" + std::string(68, '0');
  return R"IR(
@duck.tiny.zero = private unnamed_addr constant [3 x i8] c"0\0A\00"
@duck.tiny.text = private unnamed_addr constant [5 x i8] c"%.*s\00"
@duck.tiny.minus = private unnamed_addr constant [2 x i8] c"-\00"
@duck.tiny.point = private unnamed_addr constant [2 x i8] c".\00"
@duck.tiny.digit0 = private unnamed_addr constant [2 x i8] c"0\00"
@duck.tiny.leading = private unnamed_addr constant [3 x i8] c"0.\00"
@duck.tiny.exponent = private unnamed_addr constant [6 x i8] c"e%+d\0A\00"
@duck.tiny.newline = private unnamed_addr constant [2 x i8] c"\0A\00"

define internal i512 @duck.tiny.pow10(i64 %n) {
entry:
  br label %loop
loop:
  %i = phi i64 [0, %entry], [%next, %body]
  %p = phi i512 [1, %entry], [%q, %body]
  %end = icmp eq i64 %i, %n
  br i1 %end, label %done, label %body
body:
  %q = mul i512 %p, 10
  %next = add i64 %i, 1
  br label %loop
done:
  ret i512 %p
}
; Decimal digits of a nonnegative value below 10^150; zero has none.
define internal i64 @duck.tiny.digits(i512 %m) {
entry:
  br label %loop
loop:
  %d = phi i64 [0, %entry], [%next, %body]
  %p = phi i512 [1, %entry], [%q, %body]
  %more = icmp ule i512 %p, %m
  br i1 %more, label %body, label %done
body:
  %q = mul i512 %p, 10
  %next = add i64 %d, 1
  br label %loop
done:
  ret i64 %d
}
; Rounds c * 10^e to 69 significant digits, ties to even. %sticky marks a
; nonzero fraction already discarded below c's units (from division).
define internal {i256, i32} @duck.tiny.round(i512 %c, i64 %e, i1 %sticky) {
entry:
  %negative = icmp slt i512 %c, 0
  %flip = sub i512 0, %c
  %m = select i1 %negative, i512 %flip, i512 %c
  %d = call i64 @duck.tiny.digits(i512 %m)
  %long = icmp ugt i64 %d, 69
  br i1 %long, label %shorten, label %exact
shorten:
  %k = sub i64 %d, 69
  %p = call i512 @duck.tiny.pow10(i64 %k)
  %q = udiv i512 %m, %p
  %r = urem i512 %m, %p
  %half = udiv i512 %p, 2
  %above = icmp ugt i512 %r, %half
  %tie = icmp eq i512 %r, %half
  %odd = trunc i512 %q to i1
  %tieBreak = or i1 %sticky, %odd
  %tieUp = and i1 %tie, %tieBreak
  %up = or i1 %above, %tieUp
  %increment = zext i1 %up to i512
  %q2 = add i512 %q, %increment
  %limit = call i512 @duck.tiny.pow10(i64 69)
  %carried = icmp eq i512 %q2, %limit
  %q3 = select i1 %carried, i512 )IR" + e68 + R"IR(, i512 %q2
  %extra = zext i1 %carried to i64
  %k2 = add i64 %k, %extra
  %e2 = add i64 %e, %k2
  br label %check
exact:
  br label %check
check:
  %value = phi i512 [%q3, %shorten], [%m, %exact]
  %exponent = phi i64 [%e2, %shorten], [%e, %exact]
  %isZero = icmp eq i512 %value, 0
  br i1 %isZero, label %zero, label %range
zero:
  ret {i256, i32} zeroinitializer
range:
  %digits = call i64 @duck.tiny.digits(i512 %value)
  %top = add i64 %exponent, %digits
  %adjusted = sub i64 %top, 1
  %low = icmp sge i64 %adjusted, -999999999
  %high = icmp sle i64 %adjusted, 999999999
  %valid = and i1 %low, %high
  br i1 %valid, label %build, label %fail
build:
  %negated = sub i512 0, %value
  %signed = select i1 %negative, i512 %negated, i512 %value
  %coefficient = trunc i512 %signed to i256
  %e32 = trunc i64 %exponent to i32
  %r0 = insertvalue {i256, i32} poison, i256 %coefficient, 0
  %r1 = insertvalue {i256, i32} %r0, i32 %e32, 1
  ret {i256, i32} %r1
fail:
  call void @llvm.trap()
  unreachable
}
; Scales a nonzero coefficient to exactly 69 digits: {coefficient, exponent}.
define internal {i512, i64} @duck.tiny.normalize({i256, i32} %v) {
entry:
  %c = extractvalue {i256, i32} %v, 0
  %e = extractvalue {i256, i32} %v, 1
  %wide = sext i256 %c to i512
  %e64 = sext i32 %e to i64
  %negative = icmp slt i512 %wide, 0
  %flip = sub i512 0, %wide
  %m = select i1 %negative, i512 %flip, i512 %wide
  %d = call i64 @duck.tiny.digits(i512 %m)
  %isZero = icmp eq i64 %d, 0
  %s0 = sub i64 69, %d
  %s = select i1 %isZero, i64 0, i64 %s0
  %p = call i512 @duck.tiny.pow10(i64 %s)
  %scaled = mul i512 %wide, %p
  %e2 = sub i64 %e64, %s
  %r0 = insertvalue {i512, i64} poison, i512 %scaled, 0
  %r1 = insertvalue {i512, i64} %r0, i64 %e2, 1
  ret {i512, i64} %r1
}
; a + b, or a - b when %subtract is set. After normalizing both to 69 digits,
; an operand 72 or more decimal places below the other is less than half a
; unit in the last place of the result and cannot affect its rounding.
define internal {i256, i32} @duck.tiny.add({i256, i32} %a, {i256, i32} %b0, i1 %subtract) {
entry:
  %cb0 = extractvalue {i256, i32} %b0, 0
  %cbNeg = sub i256 0, %cb0
  %cb = select i1 %subtract, i256 %cbNeg, i256 %cb0
  %b = insertvalue {i256, i32} %b0, i256 %cb, 0
  %ca = extractvalue {i256, i32} %a, 0
  %aZero = icmp eq i256 %ca, 0
  br i1 %aZero, label %returnB, label %checkB
returnB:
  ret {i256, i32} %b
checkB:
  %bZero = icmp eq i256 %cb, 0
  br i1 %bZero, label %returnA, label %sum
returnA:
  ret {i256, i32} %a
sum:
  %na = call {i512, i64} @duck.tiny.normalize({i256, i32} %a)
  %nb = call {i512, i64} @duck.tiny.normalize({i256, i32} %b)
  %A = extractvalue {i512, i64} %na, 0
  %Ea = extractvalue {i512, i64} %na, 1
  %B = extractvalue {i512, i64} %nb, 0
  %Eb = extractvalue {i512, i64} %nb, 1
  %aHigher = icmp sge i64 %Ea, %Eb
  %hiC = select i1 %aHigher, i512 %A, i512 %B
  %hiE = select i1 %aHigher, i64 %Ea, i64 %Eb
  %loC = select i1 %aHigher, i512 %B, i512 %A
  %loE = select i1 %aHigher, i64 %Eb, i64 %Ea
  %diff = sub i64 %hiE, %loE
  %far = icmp sge i64 %diff, 72
  br i1 %far, label %dominant, label %aligned
dominant:
  %r1 = call {i256, i32} @duck.tiny.round(i512 %hiC, i64 %hiE, i1 false)
  ret {i256, i32} %r1
aligned:
  %p = call i512 @duck.tiny.pow10(i64 %diff)
  %scaled = mul i512 %hiC, %p
  %s = add i512 %scaled, %loC
  %r2 = call {i256, i32} @duck.tiny.round(i512 %s, i64 %loE, i1 false)
  ret {i256, i32} %r2
}
define internal {i256, i32} @duck.tiny.multiply({i256, i32} %a, {i256, i32} %b) {
entry:
  %ca = extractvalue {i256, i32} %a, 0
  %cb = extractvalue {i256, i32} %b, 0
  %ea = extractvalue {i256, i32} %a, 1
  %eb = extractvalue {i256, i32} %b, 1
  %x = sext i256 %ca to i512
  %y = sext i256 %cb to i512
  %product = mul i512 %x, %y
  %ea64 = sext i32 %ea to i64
  %eb64 = sext i32 %eb to i64
  %e = add i64 %ea64, %eb64
  %r = call {i256, i32} @duck.tiny.round(i512 %product, i64 %e, i1 false)
  ret {i256, i32} %r
}
; The 69-digit dividend is scaled by 10^70, so the quotient has at least 70
; digits and the division remainder only acts as a sticky bit for ties.
define internal {i256, i32} @duck.tiny.divide({i256, i32} %a, {i256, i32} %b) {
entry:
  %cb = extractvalue {i256, i32} %b, 0
  %byZero = icmp eq i256 %cb, 0
  br i1 %byZero, label %fail, label %check
check:
  %ca = extractvalue {i256, i32} %a, 0
  %aZero = icmp eq i256 %ca, 0
  br i1 %aZero, label %zero, label %divide
zero:
  ret {i256, i32} zeroinitializer
divide:
  %na = call {i512, i64} @duck.tiny.normalize({i256, i32} %a)
  %A = extractvalue {i512, i64} %na, 0
  %Ea = extractvalue {i512, i64} %na, 1
  %aNeg = icmp slt i512 %A, 0
  %aFlip = sub i512 0, %A
  %absA = select i1 %aNeg, i512 %aFlip, i512 %A
  %B = sext i256 %cb to i512
  %bNeg = icmp slt i512 %B, 0
  %bFlip = sub i512 0, %B
  %absB = select i1 %bNeg, i512 %bFlip, i512 %B
  %scale = call i512 @duck.tiny.pow10(i64 70)
  %numerator = mul i512 %absA, %scale
  %q = udiv i512 %numerator, %absB
  %r = urem i512 %numerator, %absB
  %sticky = icmp ne i512 %r, 0
  %negative = xor i1 %aNeg, %bNeg
  %qFlip = sub i512 0, %q
  %signed = select i1 %negative, i512 %qFlip, i512 %q
  %eb = extractvalue {i256, i32} %b, 1
  %eb64 = sext i32 %eb to i64
  %e1 = sub i64 %Ea, %eb64
  %e = sub i64 %e1, 70
  %result = call {i256, i32} @duck.tiny.round(i512 %signed, i64 %e, i1 %sticky)
  ret {i256, i32} %result
fail:
  call void @llvm.trap()
  unreachable
}
define internal i32 @duck.tiny.sign(i256 %c) {
entry:
  %positive = icmp sgt i256 %c, 0
  %negative = icmp slt i256 %c, 0
  %p = zext i1 %positive to i32
  %n = zext i1 %negative to i32
  %s = sub i32 %p, %n
  ret i32 %s
}
define internal i32 @duck.tiny.compare({i256, i32} %a, {i256, i32} %b) {
entry:
  %ca = extractvalue {i256, i32} %a, 0
  %cb = extractvalue {i256, i32} %b, 0
  %sa = call i32 @duck.tiny.sign(i256 %ca)
  %sb = call i32 @duck.tiny.sign(i256 %cb)
  %differ = icmp ne i32 %sa, %sb
  br i1 %differ, label %bySign, label %same
bySign:
  %greater = icmp sgt i32 %sa, %sb
  %r0 = select i1 %greater, i32 1, i32 -1
  ret i32 %r0
same:
  %bothZero = icmp eq i32 %sa, 0
  br i1 %bothZero, label %equal, label %magnitude
equal:
  ret i32 0
magnitude:
  %na = call {i512, i64} @duck.tiny.normalize({i256, i32} %a)
  %nb = call {i512, i64} @duck.tiny.normalize({i256, i32} %b)
  %Ea = extractvalue {i512, i64} %na, 1
  %Eb = extractvalue {i512, i64} %nb, 1
  %sameExponent = icmp eq i64 %Ea, %Eb
  br i1 %sameExponent, label %coefficients, label %exponents
exponents:
  %higher = icmp sgt i64 %Ea, %Eb
  %byExponent = select i1 %higher, i32 1, i32 -1
  %positive = icmp sgt i32 %sa, 0
  %flipped = sub i32 0, %byExponent
  %r1 = select i1 %positive, i32 %byExponent, i32 %flipped
  ret i32 %r1
coefficients:
  %A = extractvalue {i512, i64} %na, 0
  %B = extractvalue {i512, i64} %nb, 0
  %less = icmp slt i512 %A, %B
  %more = icmp sgt i512 %A, %B
  %r2 = select i1 %more, i32 1, i32 0
  %r3 = select i1 %less, i32 -1, i32 %r2
  ret i32 %r3
}
define internal {i256, i32} @duck.tiny.requireUnsigned({i256, i32} %v) {
entry:
  %c = extractvalue {i256, i32} %v, 0
  %negative = icmp slt i256 %c, 0
  br i1 %negative, label %fail, label %ok
ok:
  ret {i256, i32} %v
fail:
  call void @llvm.trap()
  unreachable
}
; Shortest exact digits, then: integer form when the exponent is >= 0 and the
; value is below 10^69; plain decimal when the leading digit is at 10^-7 or
; above; otherwise scientific notation such as 1.5e-30.
define internal void @duck.tiny.print({i256, i32} %v) {
entry:
  %buffer = alloca [72 x i8]
  %c = extractvalue {i256, i32} %v, 0
  %e32 = extractvalue {i256, i32} %v, 1
  %e0 = sext i32 %e32 to i64
  %isZero = icmp eq i256 %c, 0
  br i1 %isZero, label %zero, label %nonzero
zero:
  call i32 (ptr, ...) @printf(ptr @duck.tiny.zero)
  ret void
nonzero:
  %negative = icmp slt i256 %c, 0
  %flip = sub i256 0, %c
  %m0 = select i1 %negative, i256 %flip, i256 %c
  br label %strip
strip:
  %m = phi i256 [%m0, %nonzero], [%mq, %stripBody]
  %e = phi i64 [%e0, %nonzero], [%eNext, %stripBody]
  %rem = urem i256 %m, 10
  %trailing = icmp eq i256 %rem, 0
  br i1 %trailing, label %stripBody, label %digitLoop
stripBody:
  %mq = udiv i256 %m, 10
  %eNext = add i64 %e, 1
  br label %strip
digitLoop:
  %pos = phi i64 [72, %strip], [%posNext, %digitBody]
  %rest = phi i256 [%m, %strip], [%restNext, %digitBody]
  %doneDigits = icmp eq i256 %rest, 0
  br i1 %doneDigits, label %format, label %digitBody
digitBody:
  %posNext = sub i64 %pos, 1
  %digit = urem i256 %rest, 10
  %restNext = udiv i256 %rest, 10
  %d8 = trunc i256 %digit to i8
  %ch = add i8 %d8, 48
  %slot = getelementptr i8, ptr %buffer, i64 %posNext
  store i8 %ch, ptr %slot
  br label %digitLoop
format:
  %start = getelementptr i8, ptr %buffer, i64 %pos
  %n = sub i64 72, %pos
  %n32 = trunc i64 %n to i32
  %top = add i64 %e, %n
  %adjusted = sub i64 %top, 1
  br i1 %negative, label %minus, label %layout
minus:
  call i32 (ptr, ...) @printf(ptr @duck.tiny.minus)
  br label %layout
layout:
  %eNonnegative = icmp sge i64 %e, 0
  %small = icmp slt i64 %adjusted, 69
  %integer = and i1 %eNonnegative, %small
  br i1 %integer, label %integerForm, label %fractionCheck
integerForm:
  call i32 (ptr, ...) @printf(ptr @duck.tiny.text, i32 %n32, ptr %start)
  br label %zeroLoop
zeroLoop:
  %z = phi i64 [0, %integerForm], [%zNext, %zeroBody]
  %zDone = icmp eq i64 %z, %e
  br i1 %zDone, label %newline, label %zeroBody
zeroBody:
  call i32 (ptr, ...) @printf(ptr @duck.tiny.digit0)
  %zNext = add i64 %z, 1
  br label %zeroLoop
fractionCheck:
  %eNegative = icmp slt i64 %e, 0
  %notTooSmall = icmp sge i64 %adjusted, -7
  %plain = and i1 %eNegative, %notTooSmall
  br i1 %plain, label %plainForm, label %scientific
plainForm:
  %hasInteger = icmp sge i64 %adjusted, 0
  br i1 %hasInteger, label %splitPoint, label %leadingZeros
splitPoint:
  %intLen = add i64 %adjusted, 1
  %intLen32 = trunc i64 %intLen to i32
  call i32 (ptr, ...) @printf(ptr @duck.tiny.text, i32 %intLen32, ptr %start)
  call i32 (ptr, ...) @printf(ptr @duck.tiny.point)
  %fracStart = getelementptr i8, ptr %start, i64 %intLen
  %fracLen = sub i64 %n, %intLen
  %fracLen32 = trunc i64 %fracLen to i32
  call i32 (ptr, ...) @printf(ptr @duck.tiny.text, i32 %fracLen32, ptr %fracStart)
  br label %newline
leadingZeros:
  call i32 (ptr, ...) @printf(ptr @duck.tiny.leading)
  %zeros = sub i64 -1, %adjusted
  br label %lzLoop
lzLoop:
  %y = phi i64 [0, %leadingZeros], [%yNext, %lzBody]
  %lzDone = icmp eq i64 %y, %zeros
  br i1 %lzDone, label %lzEnd, label %lzBody
lzBody:
  call i32 (ptr, ...) @printf(ptr @duck.tiny.digit0)
  %yNext = add i64 %y, 1
  br label %lzLoop
lzEnd:
  call i32 (ptr, ...) @printf(ptr @duck.tiny.text, i32 %n32, ptr %start)
  br label %newline
scientific:
  call i32 (ptr, ...) @printf(ptr @duck.tiny.text, i32 1, ptr %start)
  %multi = icmp ugt i64 %n, 1
  br i1 %multi, label %sciFraction, label %sciExponent
sciFraction:
  call i32 (ptr, ...) @printf(ptr @duck.tiny.point)
  %restStart = getelementptr i8, ptr %start, i64 1
  %restLen = sub i64 %n, 1
  %restLen32 = trunc i64 %restLen to i32
  call i32 (ptr, ...) @printf(ptr @duck.tiny.text, i32 %restLen32, ptr %restStart)
  br label %sciExponent
sciExponent:
  %adj32 = trunc i64 %adjusted to i32
  call i32 (ptr, ...) @printf(ptr @duck.tiny.exponent, i32 %adj32)
  ret void
newline:
  call i32 (ptr, ...) @printf(ptr @duck.tiny.newline)
  ret void
}
)IR";
}
} // namespace duck
