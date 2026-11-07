#include "Pipeline.h"
namespace duck {
// Exact decimal printing for integers wider than printf's 64 bits: iN/uN above
// 64 bits and big/ubig. The caller supplies a mutable copy of the magnitude as
// little-endian 32-bit limbs and a groups buffer of at least count*2+2 words.
// The limbs are divided by 10^9 in place, collecting nine digits per step.
std::string wideRuntimeIR() {
  return R"IR(
@duck.wide.first = private unnamed_addr constant [3 x i8] c"%u\00"
@duck.wide.group = private unnamed_addr constant [5 x i8] c"%09u\00"
@duck.wide.minus = private unnamed_addr constant [2 x i8] c"-\00"
@duck.wide.newline = private unnamed_addr constant [2 x i8] c"\0A\00"

define internal void @duck.wide.digits(ptr %limbs, i64 %count, ptr %groups, i1 %negative) {
entry:
  br label %outer
outer:
  %g = phi i64 [0, %entry], [%gNext, %divided]
  br label %inner
inner:
  %i = phi i64 [%count, %outer], [%iNext, %step]
  %remainder = phi i64 [0, %outer], [%rest, %step]
  %more = phi i1 [false, %outer], [%moreNext, %step]
  %done = icmp eq i64 %i, 0
  br i1 %done, label %divided, label %step
step:
  %iNext = sub i64 %i, 1
  %at = getelementptr i32, ptr %limbs, i64 %iNext
  %limb = load i32, ptr %at
  %low = zext i32 %limb to i64
  %high = shl i64 %remainder, 32
  %current = or i64 %high, %low
  %quotient = udiv i64 %current, 1000000000
  %rest = urem i64 %current, 1000000000
  %q32 = trunc i64 %quotient to i32
  store i32 %q32, ptr %at
  %nonzero = icmp ne i64 %quotient, 0
  %moreNext = or i1 %more, %nonzero
  br label %inner
divided:
  %slot = getelementptr i32, ptr %groups, i64 %g
  %digits = trunc i64 %remainder to i32
  store i32 %digits, ptr %slot
  %gNext = add i64 %g, 1
  br i1 %more, label %outer, label %sign
sign:
  br i1 %negative, label %minus, label %leading
minus:
  call i32 (ptr, ...) @printf(ptr @duck.wide.minus)
  br label %leading
leading:
  %top = sub i64 %gNext, 1
  %topSlot = getelementptr i32, ptr %groups, i64 %top
  %topDigits = load i32, ptr %topSlot
  call i32 (ptr, ...) @printf(ptr @duck.wide.first, i32 %topDigits)
  br label %tail
tail:
  %j = phi i64 [%top, %leading], [%jNext, %print]
  %finished = icmp eq i64 %j, 0
  br i1 %finished, label %end, label %print
print:
  %jNext = sub i64 %j, 1
  %groupSlot = getelementptr i32, ptr %groups, i64 %jNext
  %group = load i32, ptr %groupSlot
  call i32 (ptr, ...) @printf(ptr @duck.wide.group, i32 %group)
  br label %tail
end:
  call i32 (ptr, ...) @printf(ptr @duck.wide.newline)
  ret void
}
; iN/uN values have at most 16 limbs (512 bits), so 34 groups always suffice.
define internal void @duck.wide.print(ptr %limbs, i64 %count, i1 %negative) {
entry:
  %groups = alloca [34 x i32]
  call void @duck.wide.digits(ptr %limbs, i64 %count, ptr %groups, i1 %negative)
  ret void
}
)IR";
}
} // namespace duck
