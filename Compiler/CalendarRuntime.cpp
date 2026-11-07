#include "Pipeline.h"
namespace duck {
// Calendar values have no timezone and use the proleptic Gregorian calendar.
//   char     i32           Unicode scalar value (never a surrogate)
//   date     i32           days since 0001-01-01, range [0, 3652058] (9999-12-31)
//   time     i64           nanoseconds since midnight, range [0, 86399999999999]
//   datetime {i32, i64}    the date and time fields above
// Constructors validate their fields and trap on invalid values. Like the text
// helpers, these are emitted module-local so direct duckc -> clang builds work.
std::string calendarRuntimeIR() {
  return R"IR(
@duck.calendar.dateFormat = private unnamed_addr constant [15 x i8] c"%04d-%02d-%02d\00"
@duck.calendar.separator = private unnamed_addr constant [2 x i8] c"T\00"
@duck.calendar.timeFormat = private unnamed_addr constant [15 x i8] c"%02d:%02d:%02d\00"
@duck.calendar.fractionFormat = private unnamed_addr constant [6 x i8] c".%09d\00"
@duck.calendar.newline = private unnamed_addr constant [2 x i8] c"\0A\00"

define internal void @duck.calendar.check(i1 %valid) {
entry:
  br i1 %valid, label %ok, label %fail
ok:
  ret void
fail:
  call void @llvm.trap()
  unreachable
}
define internal i32 @duck.char.make(i64 %v) {
entry:
  %range = icmp ule i64 %v, 1114111
  %low = icmp ult i64 %v, 55296
  %high = icmp ugt i64 %v, 57343
  %scalar = or i1 %low, %high
  %valid = and i1 %range, %scalar
  call void @duck.calendar.check(i1 %valid)
  %cp = trunc i64 %v to i32
  ret i32 %cp
}
define internal i64 @duck.date.monthDays(i64 %y, i64 %m) {
entry:
  %r4 = srem i64 %y, 4
  %r100 = srem i64 %y, 100
  %r400 = srem i64 %y, 400
  %by4 = icmp eq i64 %r4, 0
  %not100 = icmp ne i64 %r100, 0
  %by400 = icmp eq i64 %r400, 0
  %century = or i1 %not100, %by400
  %leap = and i1 %by4, %century
  %february = select i1 %leap, i64 29, i64 28
  %isFebruary = icmp eq i64 %m, 2
  %m4 = icmp eq i64 %m, 4
  %m6 = icmp eq i64 %m, 6
  %m9 = icmp eq i64 %m, 9
  %m11 = icmp eq i64 %m, 11
  %a = or i1 %m4, %m6
  %b = or i1 %m9, %m11
  %short = or i1 %a, %b
  %other = select i1 %short, i64 30, i64 31
  %days = select i1 %isFebruary, i64 %february, i64 %other
  ret i64 %days
}
; Howard Hinnant's days_from_civil, counted from 0001-01-01 instead of 1970.
define internal i32 @duck.date.make(i64 %y, i64 %m, i64 %d) {
entry:
  %y1 = icmp sge i64 %y, 1
  %y2 = icmp sle i64 %y, 9999
  %m1 = icmp sge i64 %m, 1
  %m2 = icmp sle i64 %m, 12
  %yearValid = and i1 %y1, %y2
  %monthValid = and i1 %m1, %m2
  %fieldsValid = and i1 %yearValid, %monthValid
  call void @duck.calendar.check(i1 %fieldsValid)
  %limit = call i64 @duck.date.monthDays(i64 %y, i64 %m)
  %d1 = icmp sge i64 %d, 1
  %d2 = icmp sle i64 %d, %limit
  %dayValid = and i1 %d1, %d2
  call void @duck.calendar.check(i1 %dayValid)
  %early = icmp sle i64 %m, 2
  %shift = zext i1 %early to i64
  %yy = sub i64 %y, %shift
  %era = sdiv i64 %yy, 400
  %eraYears = mul i64 %era, 400
  %yoe = sub i64 %yy, %eraYears
  %m9 = add i64 %m, 9
  %mp = srem i64 %m9, 12
  %t1 = mul i64 %mp, 153
  %t2 = add i64 %t1, 2
  %t3 = sdiv i64 %t2, 5
  %t4 = add i64 %t3, %d
  %doy = sub i64 %t4, 1
  %y365 = mul i64 %yoe, 365
  %y4 = sdiv i64 %yoe, 4
  %y100 = sdiv i64 %yoe, 100
  %u1 = add i64 %y365, %y4
  %u2 = sub i64 %u1, %y100
  %doe = add i64 %u2, %doy
  %eraDays = mul i64 %era, 146097
  %v1 = add i64 %eraDays, %doe
  %v2 = sub i64 %v1, 306
  %days = trunc i64 %v2 to i32
  ret i32 %days
}
; Inverse of make: {year, month, day}.
define internal {i32, i32, i32} @duck.date.civil(i32 %days) {
entry:
  %wide = sext i32 %days to i64
  %z = add i64 %wide, 306
  %era = sdiv i64 %z, 146097
  %eraDays = mul i64 %era, 146097
  %doe = sub i64 %z, %eraDays
  %a1 = sdiv i64 %doe, 1460
  %a2 = sdiv i64 %doe, 36524
  %a3 = sdiv i64 %doe, 146096
  %b1 = sub i64 %doe, %a1
  %b2 = add i64 %b1, %a2
  %b3 = sub i64 %b2, %a3
  %yoe = sdiv i64 %b3, 365
  %eraYears = mul i64 %era, 400
  %y0 = add i64 %yoe, %eraYears
  %c1 = mul i64 %yoe, 365
  %c2 = sdiv i64 %yoe, 4
  %c3 = sdiv i64 %yoe, 100
  %c4 = add i64 %c1, %c2
  %c5 = sub i64 %c4, %c3
  %doy = sub i64 %doe, %c5
  %p1 = mul i64 %doy, 5
  %p2 = add i64 %p1, 2
  %mp = sdiv i64 %p2, 153
  %q1 = mul i64 %mp, 153
  %q2 = add i64 %q1, 2
  %q3 = sdiv i64 %q2, 5
  %q4 = sub i64 %doy, %q3
  %d = add i64 %q4, 1
  %spring = icmp slt i64 %mp, 10
  %mA = add i64 %mp, 3
  %mB = sub i64 %mp, 9
  %m = select i1 %spring, i64 %mA, i64 %mB
  %january = icmp sle i64 %m, 2
  %bump = zext i1 %january to i64
  %y = add i64 %y0, %bump
  %y32 = trunc i64 %y to i32
  %m32 = trunc i64 %m to i32
  %d32 = trunc i64 %d to i32
  %r0 = insertvalue {i32, i32, i32} poison, i32 %y32, 0
  %r1 = insertvalue {i32, i32, i32} %r0, i32 %m32, 1
  %r2 = insertvalue {i32, i32, i32} %r1, i32 %d32, 2
  ret {i32, i32, i32} %r2
}
; ISO weekday: Monday = 1 ... Sunday = 7. 0001-01-01 is a Monday.
define internal i32 @duck.date.weekday(i32 %days) {
entry:
  %r = srem i32 %days, 7
  %w = add i32 %r, 1
  ret i32 %w
}
define internal i32 @duck.date.add(i32 %days, i64 %n) {
entry:
  %n1 = icmp sge i64 %n, -3652058
  %n2 = icmp sle i64 %n, 3652058
  %countValid = and i1 %n1, %n2
  call void @duck.calendar.check(i1 %countValid)
  %wide = sext i32 %days to i64
  %sum = add i64 %wide, %n
  %s1 = icmp sge i64 %sum, 0
  %s2 = icmp sle i64 %sum, 3652058
  %valid = and i1 %s1, %s2
  call void @duck.calendar.check(i1 %valid)
  %result = trunc i64 %sum to i32
  ret i32 %result
}
define internal i64 @duck.time.make(i64 %h, i64 %m, i64 %s, i64 %ns) {
entry:
  %hv = icmp ult i64 %h, 24
  %mv = icmp ult i64 %m, 60
  %sv = icmp ult i64 %s, 60
  %nv = icmp ult i64 %ns, 1000000000
  %a = and i1 %hv, %mv
  %b = and i1 %sv, %nv
  %valid = and i1 %a, %b
  call void @duck.calendar.check(i1 %valid)
  %hn = mul i64 %h, 3600000000000
  %mn = mul i64 %m, 60000000000
  %sn = mul i64 %s, 1000000000
  %t1 = add i64 %hn, %mn
  %t2 = add i64 %t1, %sn
  %t3 = add i64 %t2, %ns
  ret i64 %t3
}
; (nanos / divisor) % modulus, for hour, minute, second, and nanosecond parts.
define internal i32 @duck.time.part(i64 %nanos, i64 %divisor, i64 %modulus) {
entry:
  %q = sdiv i64 %nanos, %divisor
  %r = srem i64 %q, %modulus
  %part = trunc i64 %r to i32
  ret i32 %part
}
; mode: 1 = date, 2 = time, 3 = datetime. ISO 8601 extended format.
define internal void @duck.calendar.print(i32 %days, i64 %nanos, i32 %mode) {
entry:
  %dateBit = and i32 %mode, 1
  %hasDate = icmp ne i32 %dateBit, 0
  br i1 %hasDate, label %date, label %afterDate
date:
  %civil = call {i32, i32, i32} @duck.date.civil(i32 %days)
  %y = extractvalue {i32, i32, i32} %civil, 0
  %m = extractvalue {i32, i32, i32} %civil, 1
  %d = extractvalue {i32, i32, i32} %civil, 2
  call i32 (ptr, ...) @printf(ptr @duck.calendar.dateFormat, i32 %y, i32 %m, i32 %d)
  %both = icmp eq i32 %mode, 3
  br i1 %both, label %separator, label %afterDate
separator:
  call i32 (ptr, ...) @printf(ptr @duck.calendar.separator)
  br label %afterDate
afterDate:
  %timeBit = and i32 %mode, 2
  %hasTime = icmp ne i32 %timeBit, 0
  br i1 %hasTime, label %time, label %end
time:
  %h = call i32 @duck.time.part(i64 %nanos, i64 3600000000000, i64 24)
  %mi = call i32 @duck.time.part(i64 %nanos, i64 60000000000, i64 60)
  %s = call i32 @duck.time.part(i64 %nanos, i64 1000000000, i64 60)
  %f = call i32 @duck.time.part(i64 %nanos, i64 1, i64 1000000000)
  call i32 (ptr, ...) @printf(ptr @duck.calendar.timeFormat, i32 %h, i32 %mi, i32 %s)
  %fraction = icmp ne i32 %f, 0
  br i1 %fraction, label %digits, label %end
digits:
  call i32 (ptr, ...) @printf(ptr @duck.calendar.fractionFormat, i32 %f)
  br label %end
end:
  call i32 (ptr, ...) @printf(ptr @duck.calendar.newline)
  ret void
}
)IR";
}
} // namespace duck
