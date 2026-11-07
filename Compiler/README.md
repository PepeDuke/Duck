# Duck bootstrap compiler

`duckc` is a C++17 frontend that lexes and parses Duck, checks the supported
types and names, and emits textual LLVM IR. Clang compiles that IR and links
the native executable against the C runtime. LLVM C++ headers and libraries
are not required. This allows the bootstrap to work with the installed
Windows LLVM tools distribution.

## Build and run on Windows

Prerequisites: CMake 3.20+, a C++17 compiler (the commands below use Visual
Studio 2022 with its C++ tools and Windows SDK), and LLVM/Clang on PATH or
installed at `C:\Program Files\LLVM`.

From the repository root in PowerShell:

```powershell
cmake -S . -B build
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure

.\build\Debug\duckc.exe Examples\Bootstrap\01_Core.dk -o build\core.ll
& 'C:\Program Files\LLVM\bin\clang.exe' build\core.ll -o build\core.exe
.\build\core.exe
```

The example prints `42`, `0`, `1`, `2`, `120`, `1`, and `100% Duck`, each on
its own line. `Examples/Tutorial/01_HelloWorld.dk` and
`Examples/Tutorial/06_ConditionsAndLoops.dk` also compile unchanged.
`Examples/Tutorial/03_FunctionsAndReferences.dk` compiles with mutable scalar
reference parameters.
`Examples/Bootstrap/02_ControlFlow.dk` demonstrates the additional control flow.
`Examples/Bootstrap/03_ReferencesAndConstants.dk` demonstrates references and
local constants.
`Examples/Tutorial/02_ValuesAndInference.dk` now compiles unchanged, and
`Examples/Bootstrap/04_FloatingPoint.dk` demonstrates float/double arithmetic.
`Examples/Bootstrap/05_DefiniteAssignment.dk` demonstrates declarations with
later assignment and checks across branches.
`Examples/Bootstrap/06_LoopBodies.dk` demonstrates do/while, single-statement
bodies, and standalone scopes.
`Examples/Bootstrap/07_StaticContainers.dk` demonstrates grouped static functions.
`Examples/Bootstrap/08_LongIntegers.dk` demonstrates signed 64-bit values.
`Examples/Bootstrap/09_TypeCasts.dk` demonstrates C-style explicit casts.
`Examples/Bootstrap/10_SmallIntegers.dk` demonstrates signed 8-bit and 16-bit values.
`Examples/Bootstrap/11_UnsignedIntegers.dk` demonstrates unsigned integers and mixed arithmetic.
`Examples/Bootstrap/12_IntegerLiterals.dk` demonstrates hexadecimal and binary literals.
`Examples/Bootstrap/13_Bitwise.dk` demonstrates bitwise operations and checked shifts.
`Examples/Bootstrap/15_WorkerTasks.dk` demonstrates calls running as worker tasks.
`Examples/Bootstrap/16_CharsAndDates.dk` demonstrates `char`, `date`, `time`, and `datetime`.
`Examples/Bootstrap/17_ByteWidthIntegers.dk` demonstrates `iN`/`uN` integers up to 512 bits.
`Examples/Bootstrap/18_BigAndTiny.dk` demonstrates `big`/`ubig` and 69-digit `tiny` decimals.
Other tutorial files still demonstrate the larger language design.

For another Clang location, configure with `-DDUCK_CLANG=<path-to-clang>`.
To build just the frontend without requiring Clang at configure time, use
`-DBUILD_TESTING=OFF`. `duckc --help` shows its command-line usage.

For a program split across files, list all input paths before `-o`:

```powershell
.\build\Debug\duckc.exe Examples\Bootstrap\MultiFile\Main.dk Examples\Bootstrap\MultiFile\Even.dk Examples\Bootstrap\MultiFile\Odd.dk -o build\multi.ll
& 'C:\Program Files\LLVM\bin\clang.exe' build\multi.ll -o build\multi.exe
.\build\multi.exe
```

This prints `42` and `1`. All supplied files form one program with a shared
top-level function namespace and exactly one `Main`. Function calls and mutual
recursion work across files regardless of input order. Duplicate names are
errors, including duplicates across files. Each file must contain complete
declarations; tokens and comments never continue into another file. Diagnostics
retain the originating filename, line, and column. Files are processed and
functions emitted in command-line order, making repeated builds with the same
input order deterministic. Quote paths containing spaces. This explicit input
list does not implement imports, directory discovery, or Duck's runtime `Exec`.

For separate object modules and project configuration, use the
[build tool](../BuildTool/README.md). It reads `.dkp` files and invokes the
compiler with `--library` for non-entry modules and repeated
`--dependency path.dk` options for direct dependency signatures. Dependency
bodies are not emitted into the importing module. The compiler continues to
accept explicit files/options only and never reads project configuration.

## Implemented subset

- Top-level functions with explicit supported numeric or `bool` parameters and returns,
  or omitted/explicit `void` returns. Forward calls and recursion work.
  `string`, `name`, `cstring`, and pointer parameters, locals, and returns
  are also supported. See the text and native interop sections below.
- Mutable scalar reference parameters for all supported numeric and boolean types. Arguments must be
  mutable variables of exactly the declared type. References can be forwarded
  to other functions and multiple parameters can alias the same variable.
  Value parameters still receive copies. Arguments evaluate left to right.
- Initialized local constants for all supported numeric and boolean types. Initializers may be runtime
  expressions; the resulting binding cannot be reassigned, incremented, or
  passed to a mutable reference. Constants can be passed by value.
- A required `Main()` entry point with no parameters or return value.
- Static container function groups, such as `Math #{ int Twice(int n) { return
  n * 2 } }`, called as `Math.Twice(3)`. Groups may be nested, repeated across
  files, or extended by adjacent `#{ ... }` blocks. Chained blocks allow at most
  one newline between them. Qualified function names must be unique.
  Inside a group, relative function names resolve from the innermost container
  outward, then at global scope. `Print(...)` remains the built-in; a member
  named Print must be called with a qualifier. Static calls support the same
  parameters, references, returns, and recursion as global functions.
- Initialized numeric and boolean locals, `name : expression` inference,
  assignment, numeric `++` / `--`, and compound assignments `+=`, `-=`, `*=`,
  `/=`. `%` and `%=` currently require integers.
- Explicitly typed declarations without initializers, such as `int count` or
  `double amount`. Semantic analysis requires assignment before a read.
  Inferred declarations and constants still require an initializer.
- Signed 8-bit `int8`, 16-bit `short`, 32-bit `int`, and 64-bit `long`, with
  exact aliases `int16`, `int32`, and `int64` for the latter three.
- Unsigned `uint8`, `uint16`, `uint32`, and `uint64`, with exact aliases
  `byte` for `uint8` and `uint` for `uint32`. Custom byte-width integers
  `i1`–`i64` and `u1`–`u64` are also supported; see
  [byte-width integers](#byte-width-integers-in-and-un). All integer types support locals,
  constants, parameters, references, returns, arithmetic, casts, and printing.
  Decimal, hexadecimal (`0x`/`0X`), and binary (`0b`/`0B`) integer literals
  infer `int`, then `long`, then `uint64`, choosing
  the first type that fits. Values above `18446744073709551615` or below
  `-9223372036854775808` are rejected. Signed minima are supported.
  Direct integer literals (including negation and conditional branches) in a
  typed initializer, assignment, argument, or return must fit that
  destination type: `int8 n = -128` is valid; `int8 n = 128` is rejected.
  Unsigned destinations reject negative literals except `-0`; use an explicit
  cast for deliberate wrapping, such as `(uint)-1`.
  Prefixes must be followed by digits valid for their base. Hex digits are
  case-insensitive; `e`/`E` is a hex digit rather than an exponent marker.
  Leading zeroes on decimal literals do not imply octal. Literal separators,
  suffixes, octal prefixes, and hexadecimal floating-point forms are unsupported.
  Prefixed literals describe magnitudes: `0xFF` is positive 255, whereas
  `(int8)0xFF` is -1. Negative signed minima such as `-0x8000000000000000`
  are accepted.
- Integer/boolean literals, parentheses, unary `-`
  and `!`, arithmetic `+ - * / %`, comparisons, and numeric/boolean equality.
- Integer bitwise `~`, `&`, `|`, `^`, shifts `<<` / `>>`, and compound
  assignments `&=`, `|=`, `^=`, `<<=`, `>>=`. Bitwise binary operations use
  the same common integer type as arithmetic. Shifts preserve the left
  operand's type and accept any integer count type. Counts below zero or
  greater than or equal to the left operand's width trap before shifting,
  including compound assignments. Left shifts discard high bits; signed
  right shifts extend the sign bit, and unsigned right shifts insert zeros.
  Booleans and floating-point operands are rejected.
  Precedence follows C: arithmetic, shifts, comparisons, equality, bitwise
  AND, XOR, OR, logical AND, logical OR, then conditional expressions.
  In `condition ? yes | no`, the first unparenthesized `|` in the true arm
  is the separator; parenthesize bitwise OR there: `ready ? (a | b) | c`.
  The false arm may contain OR directly. Nested conditionals remain
  right-associative, and call arguments have their own expression context.
- IEEE 754 32-bit `float` and 64-bit `double`, decimal literals such as `1.25`
  and exponent forms such as `1e3` or `1.25e-2`. Decimal points require digits
  on both sides; type suffixes are not yet supported.
- Explicit casts `(type)expression` among numeric and boolean types, including
  aliases. Casts bind with unary precedence and may be nested. Float-to-integer
  casts **floor**, so `(int)-9.99` is `-10`; NaN, infinity, and out-of-range
  floored results trap before conversion. Narrowing integer casts retain the
  destination's low bits. Integer widening sign-extends signed sources and
  zero-extends unsigned sources; equal-width casts reinterpret the bits.
  Numeric-to-bool tests nonzero (NaN is true); bool-to-numeric yields
  0 or 1. Explicit casts between `string` and `name` transcode text. Raw pointers support explicit pointer and 64-bit integer casts. Object,
  `cstring`, and void casts are not implemented.
- Short-circuit `&&` / `||` and Duck's `condition ? value | value` expression.
  Conditions require booleans; conditional branches must have matching
  non-void types or numeric types that can widen to a common type.
  Only the selected branch executes. Ternaries associate to
  the right and have lower precedence than logical operators.
- `if` / `else if` / `else`, `while`, `do body while condition`, and counted
  `for initializer; condition; step` loops with braced or single-statement
  bodies. Single-statement bodies also introduce a scope. `else` attaches to
  the nearest unmatched `if`; a simple body ends with a newline or closing brace.
  Function bodies still require braces. Standalone `{ ... }` statements create
  lexical scopes and propagate assignments to outer variables.
  Any for-header clause may be omitted; an omitted condition is true.
  `break` exits the innermost loop; `continue` runs the for-loop step or
  checks a while/do-while condition. A do-while body runs before its condition,
  and `break` skips the condition. The condition ends at a newline or `}`. The bare `do { ... }` form without
  `while` shown in the broader design is not yet supported.
  For-initializer locals belong to the loop
  scope. The step cannot declare a variable.
- `char`, `date`, `time`, and `datetime` values with validated constructors;
  see [characters, dates, and times](#characters-dates-and-times).
- `Print` with one numeric, boolean, `string`, `name`, `cstring`, `char`, or calendar value. Floating-point
  output uses `%g` (six significant digits). Booleans print
  as `0` or `1`. Text supports `\n`, `\r`, `\t`, `\\`, `\"`, `\0`,
  `\uXXXX`, and `\UXXXXXXXX` escapes.
  Text is printed as data, so percent characters never become format specifiers.
  Inferred locals can hold and reassign these literal values, including
  conditional selections such as `label : ready ? "yes" | "no"`.
- Line and block comments, newline statement terminators,
  and termination before `}`. Call arguments and parameter lists may span lines.
- Source-file, line, and column diagnostics. Invalid programs do not write IR.

Integer addition, subtraction, multiplication, and negation wrap at their
respective 8-bit, 16-bit, 32-bit, or 64-bit width.
Division truncates toward zero, and remainder has the dividend's sign.
Integer `/`, `%`, `/=`, and `%=` trap at runtime for a zero divisor, and signed
operations also trap for the signed minimum divided by `-1` (including
remainder). Unsigned division and comparisons interpret values as nonnegative.
Checks use the operand
width after numeric promotion; unselected short-circuit and conditional
branches do not trap. Floating-point division retains IEEE behavior.
Text literals default to UTF-16 `string`. In a typed text context the same
literal can initialize UTF-8 `name` or native `cstring`. Literals are validated
as Unicode; neither the target OS nor host `wchar_t` changes their encoding.
See [text values](#string-and-name-values) for operations, lifetime and interop.

Numeric conversions widen through `int8 â†’ short â†’ int â†’ long â†’ float â†’ double` (including
direct jumps, such as int to double) for initialization, assignment, value
arguments, and returns.
Unsigned conversions widen through `uint8 â†’ uint16 â†’ uint32 â†’ uint64`, and
all integers can convert implicitly to float/double. Unsigned-to-signed
conversion is implicit only when the signed destination is strictly wider;
signed-to-unsigned conversion requires a cast.
Arithmetic, comparisons, and numeric ternaries with matching signedness use
the widest operand type. Mixed signed/unsigned integers use the smallest
signed type that contains both complete ranges: `int8 + uint8` uses `short`,
and `int + uint` uses `long`. Mixing `uint64` with any signed integer requires
an explicit cast, including `uint64Value + 1`; use `uint64Value + (uint64)1`
or `uint64Value += 1` for unsigned arithmetic. Mixing integers and floating
values continues to use the widest floating type.
Reference arguments still require exact types; an int cannot bind to a long
reference. Integer-to-floating conversion can round values beyond the target's
precision. Implicit narrowing between integer types, floating values to integers, or double
to float is rejected; use an explicit cast. Integer
operations determine their width from their operands before assignment:
`long n = 2147483647 + 1` still performs wrapping 32-bit addition, then widens.
Use a long operand when 64-bit arithmetic is intended.
Two small operands retain their common width: `(int8)127 + (int8)1` wraps to
`-128`. A plain literal in arithmetic still infers `int`, so `small + 1` has
type `int`; assigning it to `int8` or `short` requires a cast. Compound
assignments accept direct in-range literals at the target width, so
incrementing an `int8` holding 127 wraps to `-128`. Integer casts may deliberately narrow
out-of-range values: `(int8)255` is `-1`. Unary negation of an unsigned value
wraps at its width. Floating casts to unsigned types floor first and trap
if the result is negative, non-finite, or at least 2 raised to the type's width.

An inferred decimal literal has type float. A decimal literal directly used as
a double initializer, assignment, argument, or return is parsed at double
precision, including unary negation and ternary branches. Other arithmetic
subexpressions determine their type from their operands before destination
conversion; assigning a float calculation to double does not recover precision.
Floating-point operations preserve IEEE behavior without fast-math assumptions:
NaN compares unequal, ordered comparisons with NaN are false, and division by
zero may produce infinities or NaN. Out-of-range literal overflow is rejected;
very small literals may round to zero. The older conversion examples still need
string concatenation support. Casts do not supply a destination type to their
operand: `(double)9.99` widens an already-rounded float literal; use a directly
initialized double when double-precision literal parsing is required.

Uninitialized locals have no implicit zero/default value in this bootstrap.
Every path reaching a read must assign the variable first. Branches ending in
`return`, `break`, or `continue` do not participate in the following statement's
merge. A loop-body assignment does not establish initialization after a loop,
because it may execute zero times; assignments in a for initializer do. A
do-while loop runs at least once: assignments must hold on both condition-exit
and break paths before use after the loop. Its condition merges normal body
exits and continue edges. Body-local declarations are out of scope in the
condition. An unreachable condition is still checked conservatively using
entry facts, and constant conditions are not used to prove reachability. The
for increment merges normal body exits and `continue` exits. Shadowed locals
are tracked independently. Increment and compound assignment read their target,
and reference arguments must already be initialized (references are not output
parameters). These checks are deliberately conservative: they do not prove
constant conditions, guaranteed loop iterations, or unreachable expression
branches, so `if true { n = 1 }` alone does not establish initialization after
the if. These bootstrap rules do not settle default initialization of future
containers or fields.

Calls whose effects are fully known can run on worker threads; see
[worker tasks](#worker-tasks). Everything else executes sequentially. The
compiler does not yet track resource reads and writes across calls, implement
`${ ... }` resource locking, or Duck's complete ownership rules. Local reference declarations, reference returns, arrays,
mutable text buffers, range-based loops,
markup and compile-time evaluation
remain future work. Unsupported constructs produce errors; the design
examples are not a claim of compiler coverage. Non-void functions must have
an explicit return on every structurally reachable path, and statements
after a guaranteed return are rejected.

Declare an instance container as `Example { ... }`. Fields and instance methods
share the body. Each member and each statement starts on its own line when
there is more than one. Instance methods can use field names directly or access
them through `this`. Declare and use the type by its name alone, such as `Example` or `Example*`.
Static function groups continue to use `#{}`. Custom constructors, field
defaults, properties, and inheritance remain future work.

## String and name values

`string` stores UTF-16 code units (always 16 bits); `name` stores UTF-8 bytes.
They are distinct, immutable text value types with reassignable variables.
`name` currently has content equality, not an interned identifier table.

```c++
string title = "Hellfeather \U0001F986"
name utf8 = (name)title
string restored = (string)utf8
Print(restored == title)
Print(Length(title))  // 14 UTF-16 code units
Print(Length(utf8))   // 16 UTF-8 bytes
Print(CodeUnitAt(title, 12)) // 55358: high surrogate
```

Supported operations:

- Locals, inference, constants, assignment, parameters, returns, mutable reference
  parameters, and conditional expressions. Uninitialized variables still require
  assignment before reading. A reference can reassign a value, not edit its bytes.
- `+` and `+=` concatenate values of the same type. `==` and `!=` compare content,
  including embedded NULs. Equality does not perform Unicode normalization.
- `(name)value` and `(string)value` explicitly convert between encodings at runtime.
  Conversion preserves Unicode scalars, including supplementary characters.
  Named values never change encoding implicitly. Literals adopt a typed destination
  (including arguments/returns) or an adjacent text operand; otherwise they infer
  `string`. Both branches of a conditional must have compatible types.
- `Length(value)` returns a `long` code-unit count; it is not a grapheme/character
  count. `CodeUnitAt(value, index)` returns a `uint32` code unit and traps for a
  negative/out-of-range index. Code units cannot be modified individually.
- `Print` emits UTF-8 bytes with a newline for either type, independent of locale
  and `wchar_t` width. Terminal display still depends on its UTF-8/font support.

Source literals must contain valid UTF-8. Escapes include `\0`, `\uXXXX`, and
`\UXXXXXXXX`; Unicode escapes must name scalar values, so isolated surrogate
escapes are rejected. Use `\U0001F986` for a supplementary character. UTF-16
surrogate pairs are generated automatically. Length, equality, conversion and
printing preserve embedded NUL code units.

Literals use immutable program-lifetime storage. Concatenation/conversion create
immutable shared storage with deterministic reference counting. Assignment,
argument evaluation, returns and scope exits retain/release it; this includes
break/continue and early returns. No tracing garbage collector is involved.
The current compiler executes sequentially; this reference count implementation
does not provide future concurrent sharing semantics. `Tests/text.py` instruments
text allocations and checks that loop/return/reference cases release all storage.

The internal ABI is an opaque pointer to a reference count, an i64 code-unit
length, and terminated data. This layout is compiler-private, not a stable native
or distribution ABI. Direct `duckc` → Clang builds work without an extra runtime
library: required helpers are emitted as module-local LLVM IR and use the C
allocator. Allocation failure and size overflow trap.

Native declarations may take `string` or `name` by value. The compiler passes a
read-only pointer to terminated UTF-16 or UTF-8 data, respectively, and keeps the
owner alive through the call. This is suitable for a Windows Unicode title or a
UTF-8 native API. The native signature must match that encoding; do not assume a
host `wchar_t*` is always UTF-16. A native C API may stop at an embedded NUL even
though Duck's length-aware operations preserve it. Native code must neither
modify nor retain the pointer. Native text returns and mutable text references
are rejected until copying/validation/lifetime rules are implemented.

`cstring` remains a distinct, immutable UTF-8 literal-pointer interop type.
It supports typed literal assignment, Duck parameters/returns and Print, but not
managed text operations or casts to/from `string`/`name`. Its lack of ownership
prevents safely borrowing arbitrary dynamically created text into a lasting
`cstring` variable. Use a `name` native parameter for a read-only UTF-8 borrow.

See [14_Text.dk](../Examples/Bootstrap/14_Text.dk) for an executable example.
Substring, search, formatting, normalization, grapheme iteration and mutable
buffers remain future work.

## Byte-width integers (iN and uN)

`iN` is a signed and `uN` an unsigned two's-complement integer of **N bytes**
(8·N bits), for N from 1 to 64, so the widest types hold 512 bits. N counts
bytes, not bits: `i8` is 64-bit, whereas `int8` is 8-bit.

```c++
i3 pixel = -8388608        // 24-bit minimum
i16 big = 170141183460469231731687303715884105727
Print(big + 1)             // wraps to -170141183460469231731687303715884105728
u64 all = 0xFFFF...FFFF    // 128 hex digits: 2^512 - 1
```

- `i1`, `i2`, `i4`, `i8` are exactly `int8`, `short`, `int`, `long`, and
  `u1`, `u2`, `u4`, `u8` are exactly `uint8`, `uint16`, `uint32`, `uint64`.
  Diagnostics name these by their host spelling. Every other width is a
  distinct type, such as `i3`, `u5`, `i9`, or `u64`.
- Every word made of `i` or `u` followed by digits is reserved. Widths outside
  1..64 or with a leading zero (`i0`, `u65`, `i08`) are rejected.
- They follow the existing integer rules: wrapping `+ - * ~` and negation,
  truncating division, remainder with the dividend's sign, traps on a zero
  divisor and on the signed minimum divided by `-1`, bitwise operators, and
  shifts whose counts trap outside `0..width-1` (a `u64` shift accepts counts
  up to 511, including from an `int8` count variable).
- Implicit conversions widen within signed or unsigned types and from unsigned
  to strictly wider signed types, regardless of width; narrowing requires a
  cast, which keeps the low bits. Mixed signed/unsigned arithmetic uses the
  smallest signed type holding both ranges. Host widths are tried first, and
  custom widths (byte multiples up to 512 bits) only when an operand already has
  a custom width: `u9 + int8` is `i10`, `u3 + int8` is `int`. `uint64` mixed
  with a signed host type still requires a cast, and nothing is wider than
  `u64` (512-bit), so mixing it with a signed value requires one too.
- Literal bounds are arbitrary precision. A literal beyond `uint64` is accepted
  only where a destination supplies a wide type: a typed declaration,
  assignment, argument, return, or the right operand of a custom type wider
  than 64 bits (`big * 100000000000000000000000`). A cast does not supply that
  context, as with floating literals: use a typed local instead of
  `(u16)340282366920938463463374607431768211455`.
- Floating conversions floor and range-check exactly like the host widths.
  Integer-to-float conversion rounds to nearest, so values near the top of a
  wide type can become `inf` in a `float`.
- `Print` writes exact decimal values for every width. Widths above 64 bits use a
  module-local helper (`WideRuntime.cpp`) that divides 32-bit limbs by 10^9;
  narrower custom widths use the 32- or 64-bit printf formats.
- LLVM lowers division and float conversions of 65–128-bit integers to
  compiler-rt calls (`__divti3`, `__floattidf`, …) that are not linked for the
  MSVC target, while it expands wider types inline. Those operations on `i9`–`i16`
  and `u9`–`u16` therefore run at 256 bits and truncate the exact result, so
  direct `duckc` → Clang builds still need no extra runtime library.
- Custom widths cannot appear in `extern "C"` signatures; their C ABI is not
  defined. `int.Max`-style limits are still future work.

`Tests/wide_integers.py` checks arithmetic, shifts, comparisons, and division
for 14 byte widths, signed and unsigned, plus cross-width and floating casts,
against Python's unbounded integers, and checks the traps and diagnostics.

## Arbitrary-precision numbers (big, ubig, tiny, utiny)

```c++
big deck = Factorial(52)           // exact, any size
ubig apples = 8
Print(apples - 9)                  // traps: ubig cannot go below zero
tiny dime = 0.1                    // exactly one tenth
Print(dime + 0.2 == 0.3)           // 1
tiny third = 1.0 / 3               // 0.333… (69 threes)
```

### big and ubig

- Arbitrary-precision two's-complement semantics without a width: `+ - * / %`,
  unary `-` (for `big`), comparisons, `+= -= *= /= %=`, and `++`/`--`. Results
  are exact; there is no overflow. Division truncates toward zero and the
  remainder takes the dividend's sign, as for fixed-width integers. Division or
  remainder by zero traps. Bitwise operators and shifts are rejected.
- **`ubig` never holds a negative value.** A subtraction (including `--`)
  whose exact result would be negative traps instead of wrapping, because there
  is no width to wrap at. Negating a `ubig` is a compile error, as are negative
  `ubig` literals.
- Implicit conversions: integers of at most 64 bits widen to `big`; unsigned
  ones also widen to `ubig`; `ubig` widens to `big`. Mixed arithmetic uses
  `ubig` when both operands are nonnegative types and `big` otherwise.
  Floating values, `tiny`, and `iN`/`uN` wider than 64 bits do not mix.
- Explicit casts: `(ubig)value` traps on a negative value. Casting `big`/`ubig` to
  an integer of at most 64 bits keeps the low two's-complement bits, like other
  narrowing integer casts: `(int8)(big)300` is 44.
- Literals of any size are exact where a `big`/`ubig` destination supplies the
  type, including a typed declaration, assignment, argument, return, the right
  operand of a `big` value, or the left literal operand of an expression whose
  destination is `big`.
- Representation: an immutable heap block `{refcount, limb count, sign, 32-bit
  limbs…}` in sign-magnitude form. It shares managed text's reference count
  word, so locals, parameters, returns, assignment and scope exits retain and
  release values exactly like `string`; literals are constant storage. Values
  cannot be container fields, pointers, `&` targets, native arguments, or worker
  task values yet. Multiplication is schoolbook and division is binary long
  division: correct for any size, but quadratic in the number of digits.
- `Print` writes the exact decimal value.

### tiny and utiny

- Decimal floating point with **69 significant decimal digits**. A value is
  `coefficient × 10^exponent` with `|coefficient| < 10^69`, stored as a value
  type `{i256, i32}`. The adjusted exponent (the power of ten of the leading
  digit) must stay within -999999999..999999999.
- **Rounding:** every literal and every `+ - * /` result is the exact result
  rounded once to 69 significant digits, **ties to even** (IEEE 754
  roundTiesToEven, matching Python's `decimal` at 69 digits). Comparisons are
  exact; `1.0` and `1.00` are equal. A result whose adjusted exponent leaves the
  range traps (there are no infinities, NaNs, or subnormals). Division by zero traps.
- **`utiny` never holds a negative value.** A subtraction whose rounded result
  would be negative traps, as does `(utiny)` of a negative value. Negating a
  `utiny` and negative `utiny` literals are compile errors.
- Decimal literals are parsed exactly, so `tiny x = 0.1` is one tenth. Literals
  take the `tiny` type from a typed destination, from a `tiny` left operand, or,
  for a real literal on the left, from the expression's `tiny` destination.
  Integer literals keep integer arithmetic: `tiny x = 1 / 3` is 0, as for
  `double`; write `1.0 / 3`.
- Integers of at most 64 bits convert implicitly (unsigned ones also to `utiny`)
  and mix in arithmetic. Floating values, `big`, and conversions from `tiny` to
  integers or floating types are not supported yet. `%` is rejected.
- `Print` writes the shortest exact form: integers below 10^69 without a point
  (`1200`), plain decimals down to 10^-7 (`0.00000012`), and otherwise scientific
  notation (`1.2e-8`, `-1.5e+100`).
- The arithmetic uses LLVM `i512` intermediates, which LLVM expands inline, and is
  emitted module-locally from `DecimalRuntime.cpp`.

`Tests/arbitrary_numbers.py` checks random `big` arithmetic against Python
integers, random `tiny` arithmetic against Python's `decimal` module, rounding
ties, formatting, release of all `big` storage, traps, and diagnostics.

## Enums

A file-scope `Name : type #@{ ... }` block declares an enum whose members
carry an integer, `float`, `double`, `string`, or `name` value. Members are
separated by commas or newlines, and a trailing comma is allowed. See
[19_Enums.dk](../Examples/Bootstrap/19_Enums.dk).

```c++
Day : byte
#@{
    Sunday, Monday, Tuesday
}
Day #@{ Wednesday: 9, Thursday }   // extension: Thursday is 10

Print(Day.Sunday)                  // Sunday
Print(Day.Thursday + 0)            // 10
```

- **Extension.** Further `Name #@{ ... }` blocks, in the same file or another
  input file, add members. All fragments of an enum are collected in input
  order before any implicit value is assigned. The underlying type may be
  stated on any fragment, must be stated on at least one, and must agree
  wherever it is stated. Member names must be unique across fragments.
- **Implicit values.** An integer member is the previous member's value plus 1,
  starting at 0. A `float` or `double` member is the previous value plus 0.5,
  starting at 0. A text member defaults to its own name. Explicit values are
  literals of the enum's kind (an optionally negated integer, a decimal number,
  or a string). Every value must fit the underlying type, including implicit
  ones; `byte` stops at 255. Duplicate values are allowed.
- **Values.** An enum is a type: it works for locals, constants, parameters,
  returns, container fields, conditional results, and worker tasks. A value is
  stored as its member's index (`i32`), so `Print` writes the member name,
  even when two members share a value. Arithmetic, comparisons, casts,
  concatenation, and implicit conversions use the underlying value: `int n =
  Day.Monday`, `Names.Bob + " and " + Names.Tom`, and `day == 0` all work.
  Two enum values compare by underlying value.
- **No conversion into an enum.** `Day d = 0` is a type mismatch, enum members
  are constants, and compound assignment such as `d += 1` is rejected.
- **Modules.** Each module emits private name and value tables. An importing
  module may use an imported enum, including passing it to the library, but
  cannot extend it; add members in the defining module instead.
- Not yet supported: enums inside static containers, values computed from
  expressions or other members, conversion from a value back to a member,
  enum `name` lookup at run time other than `Print`, `[args]` attribute tags,
  and native `extern "C"` enum parameters.

## Characters, dates, and times

`char`, `date`, `time`, and `datetime` are distinct value types. They are not
numeric: arithmetic, bitwise operators, `++`, and implicit conversions are
rejected, while `==`, `!=`, `<`, `<=`, `>`, and `>=` compare two values of the
same type. They work as locals, constants, parameters, returns, mutable
references, container fields, and conditional results. Declarations without an
initializer still require assignment before a read; there is no default value.

```c++
char duck = '🦆'                       // one char: U+1F986
Print(Length((string)duck))            // 2 UTF-16 code units
date leap = date(2024, 2, 29)
datetime meeting = datetime(leap, time(12, 30, 0))
Print(AddDays(meeting, 1))             // 2024-03-01T12:30:00
```

### char

- A `char` is one **Unicode scalar value**: U+0000–U+D7FF or U+E000–U+10FFFF.
  It is not a UTF-16 or UTF-8 code unit, so a supplementary character such as
  U+1F986 is one `char`, even though it needs a surrogate pair in a `string` and
  four bytes in a `name`. It is also not a grapheme: `é` written as `e` plus
  U+0301 is two scalars and cannot be one `char` literal.
- Literals use single quotes and the text escapes (`'\n'`, `'\''`, `'\u00E9'`,
  `'\U0001F986'`) or a UTF-8 source character. A literal must contain exactly one
  scalar; `''` and `'ab'` are rejected. `\'` is also accepted in text literals.
- `char(value)` takes an integer scalar value and validates it. Surrogates
  (U+D800–U+DFFF), negative values, and values above U+10FFFF are compile errors
  for literal arguments and trap at runtime otherwise. `(char)value` is rejected
  so that every conversion into `char` is validated.
- `(uint32)c` (or any other integer cast) yields the scalar value. `(string)c`
  and `(name)c` create one-character text. `char(CodeUnitAt(s, i))` validates a
  code unit, so it traps on half of a surrogate pair.
- Comparison is by scalar value, which differs from UTF-16 code-unit order:
  `'\uFFFF' < '\U00010000'` is true. `Print` writes the character as UTF-8.

### date, time, and datetime

All three are **civil (wall-clock) values without a timezone or UTC offset**. They
never consult the host's local zone or daylight-saving rules, and two values
compare by their fields alone. Zone- or offset-aware instants, the current
clock, parsing, and formatting options remain future work.

| Type | Range | Precision | Storage |
| --- | --- | --- | --- |
| `date` | 0001-01-01 to 9999-12-31 | 1 day | `i32` days since 0001-01-01 |
| `time` | 00:00:00 to 23:59:59.999999999 | 1 ns | `i64` nanoseconds since midnight |
| `datetime` | 0001-01-01T00:00:00 to 9999-12-31T23:59:59.999999999 | 1 ns | `{ i32, i64 }`: the date and the time |

- Dates use the **proleptic Gregorian calendar** for every year, including years
  before 1582. A leap year is divisible by 4, except century years not
  divisible by 400: 2000 and 2024 have February 29; 1900 and 2023 do not.
- Leap seconds are not representable: second 60 is invalid.
- Constructors take integer fields (any type that widens to `long`; cast `uint64`
  explicitly), evaluate them left to right, and then validate them:
  - `date(year, month, day)`: year 1–9999, month 1–12, day 1 to the month's length.
  - `time(hour, minute, second)` or `time(hour, minute, second, nanosecond)`:
    hour 0–23, minute 0–59, second 0–59, nanosecond 0–999999999.
  - `datetime(year, month, day, hour, minute, second[, nanosecond])`, or
    `datetime(date, time)` from already validated parts.
  When every field is an integer literal, invalid values are compile errors
  naming the field, such as `day 29 is outside 1..28 for month 2 of year 2023`.
  Otherwise an invalid value traps at runtime. Casts into these types are rejected.
- Built-in functions:
  - `Year`, `Month`, `Day`, and `DayOfWeek` (ISO: Monday = 1 … Sunday = 7) take
    a `date` or `datetime` and return `int`.
  - `Hour`, `Minute`, `Second`, and `Nanosecond` take a `time` or `datetime` and
    return `int`.
  - `DateOf(datetime)` returns its `date`; `TimeOf(datetime)` returns its `time`.
  - `AddDays(value, long days)` returns a `date` or `datetime` of the same type,
    keeping the time of day. Leaving the supported range traps.
  - `DaysBetween(from, to)` returns `to - from` as a `long` number of days.
  These names are built-ins only when no user function or container of the same
  name is visible, so existing functions named, for example, `Day` keep working.
- `Print` uses ISO 8601 extended format: `2024-02-29`, `12:30:00`, and
  `2024-02-29T12:30:00`. A nonzero fraction prints nine digits:
  `23:59:59.999999999`.
- `char`, `date`, `time`, and `datetime` are now reserved type keywords and
  cannot be used as identifiers.

The calendar helpers are emitted module-locally, like the text helpers, from
`CalendarRuntime.cpp`. They are not native-ABI types: native declarations
reject them, and the worker scheduler does not yet move them between threads.
Heap allocation (`own`/`T*` initialization) is also not supported for them.
`Tests/calendar.py` checks 312 dates against Python's `datetime` module, walks
every day from 0001-01-01 to 9999-12-31, and covers supplementary characters,
leap days, invalid constants, and runtime traps.

## Native API calls and opaque handles

The engine can now call native libraries directly from Duck. Declarations have
no body and use the platform C calling convention:

```c++
Platform
#{
    extern "C" int Counter(long& ticks) = "QueryPerformanceCounter"
    extern "C" int Frequency(long& value) = "QueryPerformanceFrequency"
}
extern "C" int puts(cstring message)

Main()
{
    long ticks = 0
    Print(Platform.Counter(ticks) != 0)
    puts("Called from Duck")
}
```

An optional `= "symbol"` maps a Duck function name to a native linker symbol.
Without an alias, the unqualified function name is used, even inside a static
container. Symbols must be ASCII identifiers; `main` and `printf` are reserved
by the bootstrap (as are `malloc`, `free`, and `putchar`, used by text helpers). Only `extern "C"` is accepted. Declarations end at a newline,
enclosing brace, or EOF. The compiler rejects bodies, conflicting
signatures for the same native symbol, and native Main declarations. Compatible
aliases share one LLVM declaration. Native declarations can be exported through
ordinary direct module dependencies; their bodies are never emitted.

Native signatures currently support:

- By value: signed/unsigned 32-bit and 64-bit integers, float, double, raw pointers,
  and `string`, `name`, or `cstring` arguments. Native returns allow the supported
  numeric/pointer types or void, but exclude all text types.
- By reference: numeric types up to 64 bits, raw pointer variables, and structs. References pass the
  address of an initialized, mutable, exactly typed variable. Initialize output
  arguments explicitly; native calls do not bypass definite assignment checks.
- `bool`, 8/16-bit integers by value, native text returns, and text references are
  rejected until their ABI/lifetime behavior is implemented. For Windows BOOL,
  declare `int` and compare the return with zero. Duck `long` is always 64-bit;
  it is not necessarily the host C `long` type.

`void*` is an opaque handle type. It supports null, assignment, function arguments
and returns, mutable references, conditionals, and equality/inequality. It does
not support dereferencing, arithmetic, ordering, or Print. Use
`handle != null` to check a handle. A local handle is not initialized implicitly.
There is no automatic native resource ownership: explicitly call the native
library's destroy/close function when required.

`cstring` refers to immutable UTF-8 bytes with a trailing zero, suitable for
native `const char*` input. Duck functions can accept and return it, and Print
accepts it. It is distinct from an opaque handle and cannot be null or cast to
`void*`. Native callees must not modify its storage. Native references borrow
Duck storage only during the call and must not be retained. These native
contracts are the binding author's responsibility; Duck does not inspect the
library implementation. Pointer arithmetic, indirect Duck calls, structures passed to C by
value, varargs, C++ name mangling, and alternate calling conventions remain
unsupported.

### Owned heap locals

```duck
Entity { int score }

int ReadScore(Entity* e) { return e.score }

Entity* Make() {
    Entity* owner()
    owner.score = 42
    return ::owner
}

Main() {
    entity :: Make()
    Print(ReadScore(entity))
    alias : entity
    {
        temporary :: entity
        Print(alias.score)
    }
    Print(alias == null)
}
```

Use `int* n = 5` or `n :: 5` to allocate a scalar. A move clears its source.
The compiler can keep a local alias as a direct borrow when its remaining
lexical uses preserve the owner. More general local aliases trigger lazy
promotion on the executed path. An expired alias compares equal to null and
dereferencing it traps. Scope exit, early return, break, and continue clean up
owned locals.

Tracked arguments may borrow into Duck pointer parameters whose bodies the
compiler proves only access the pointee. Unproven escapes, raw casts, storing
tracked aliases in native-layout fields, and transfers from aliases are rejected.
Owners cross a function return via `return ::owner` and a receiving `::`
declaration. Native pointers obtained from `&local` or a platform API continue
to use the existing raw ABI.

### Typed pointers and window state

Declare a file-scope container with fields in native declaration order. Struct
locals start zero-initialized. LLVM supplies the target's ordinary C field
alignment and padding. Fields can be scalars, pointers, or embedded structs. Use fixed-width types that match the native header.

```duck
WindowState
{
    int visits
    void* window
}

int Visit(void* window, long context) {
    WindowState* state = (WindowState*)context
    state.window = window
    state.visits++
    return 1
}
```

`&value` takes the address of initialized mutable storage. `*p` reads or
writes the pointee. Use `value.field` or `p.field` for fields. Each dot follows at most one pointer
level, so a pointer-to-pointer needs an explicit dereference, as in `(*pp).field`. A typed pointer
can pass directly to a `void*` parameter. Recover its type with an explicit
cast. On x64 Windows, `(long)&state` carries the address in an `LPARAM`.
The binding author must keep the storage alive and check nullable pointers
before accessing their pointees.

Pass `&Visit` to a native callback parameter declared as `void*`. The compiler
checks the callback uses supported scalar ABI types. The binding author must
match the actual parameter list, return type, and calling convention.
[WindowState](../Examples/Projects/WindowState/Main.dk) creates two hidden
windows and stores a state pointer in the child's `GWLP_USERDATA` slot. The
enumeration callback retrieves that pointer, passes a Duck container to
`GetClientRect`, and updates the caller's state.

Add libraries to the `.dkp` module's `libraries` array or pass them to Clang
when linking IR directly. Library names and prototype correctness must match
the selected platform; the compiler does not read C headers. The current
integration target is Windows x64. In particular, these bindings do not claim
support for 32-bit Windows stdcall APIs.

[Examples/Projects/NativeClock](../Examples/Projects/NativeClock/NativeClock.dkp)
is a Duck-only Windows clock example using Kernel32 (no handwritten C++ shim):

```powershell
./duck.cmd build Examples/Projects/NativeClock/NativeClock.dkp
./Examples/Projects/NativeClock/.duck/build/debug/NativeClock.exe
```

It prints four `1` values after querying frequency and checking two ordered
readings. The signatures follow Microsoft's
[QueryPerformanceCounter](https://learn.microsoft.com/en-us/windows/win32/api/profileapi/nf-profileapi-queryperformancecounter)
and [QueryPerformanceFrequency](https://learn.microsoft.com/en-us/windows/win32/api/profileapi/nf-profileapi-queryperformancefrequency)
documentation; native calls use LLVM's
[default C calling convention](https://llvm.org/docs/LangRef.html#calling-conventions).

`Tests/native_interop.py` compiles an independent C fixture, links it through a
referenced Duck library project, and checks scalar/ref/handle/string ABI behavior
at both optimization profiles. It also verifies declaration deduplication,
imported conflicts, diagnostics and the real Windows clock example.

## Worker tasks

The compiler starts with calls whose effects are known. A function is
*isolated* when it has a body, is not an instance method, takes and returns
only numeric or `bool` values (no references, pointers, text or containers),
uses only such locals, never calls `Print`, and calls only other isolated
functions. Recursive groups qualify together; a single call to a native,
imported, printing, or otherwise unknown function excludes the caller.

A declaration in a statement list whose initializer is a direct call to an
isolated function, such as `a : Fib(30)` or `long a = Fib(30)`, becomes a
worker task. Its arguments are evaluated on the calling thread, in source order,
when the declaration is reached. The task joins and stores its result:

- before the first later statement in the same list that mentions its name,
  anywhere inside that statement;
- before a statement with an external effect: `Print`, or a call to a function
  that is not isolated;
- before a statement containing `return`, `break`, or `continue`, or a loop
  that never exits;
- at the end of its statement list, before scope cleanup.

```c++
int Fib(int n)
{
    if n < 2 { return n }
    left : Fib(n - 1)     // Runs as a task.
    right : Fib(n - 2)    // Runs here, overlapping the task.
    return left + right   // Joins left first.
}
```

A declaration is not spawned when the very next statement would join it, so
`right` above runs on the caller. Tasks only exchange values with their caller,
and joins precede every external effect, so program output, runtime traps and
nontermination occur in the same order as sequential execution. See
[15_WorkerTasks.dk](../Examples/Bootstrap/15_WorkerTasks.dk).

At run time, `DUCK_WORKERS=n` limits the number of threads executing Duck code,
including the main thread; it defaults to the active processor count. When the
limit is reached the task runs inline on the caller, so nested tasks never wait
for a free worker. `DUCK_WORKERS=1` runs every task inline, in source order.
`duckc --sequential` disables task lowering entirely. Each task currently
starts a Windows thread (`CreateThread`), so the runtime targets Windows x64,
and small calls can cost more than they save; a cost model is future work.
For-loop initializers and steps, assignments to existing variables, and calls
inside larger expressions still run on the caller.

`Tests/tasks.cmake` compiles each scheduled program with and without
`--sequential`, links both at `-O0` and `-O2`, and requires identical output
with one worker, four workers, and the default. It also checks that a trapping
task stops the program before later output.

## Implementation and validation

The compiler is a reusable `duck_compiler` library with explicit stage APIs in
`Pipeline.h`. `main.cpp` only handles file I/O and invokes the pipeline.

| Stage | Input â†’ output | Implementation |
| --- | --- | --- |
| Tokenize | Source text â†’ tokens with locations | `Lexer.cpp` |
| Parse | Tokens â†’ syntax tree | `Parser.cpp` |
| Combine units | Per-file syntax trees in input order â†’ program | `Pipeline.cpp` |
| Collect declarations | Syntax tree â†’ function signatures | `Symbols.cpp` |
| Analyze a function | Function + signatures â†’ types, conversions, literal constants | `Semantics.cpp` |
| Schedule tasks | All analyses â†’ isolated functions, task spawns and joins | `Scheduling.cpp` |
| Lower a function | Function + signatures + analysis â†’ LLVM fragment | `Codegen.cpp` |
| Assemble | Fragments in source order â†’ LLVM module | `Pipeline.cpp` |

Semantic analysis checks names, types, references, constants, definite assignment, and control flow
without creating LLVM instructions. All functions are analyzed successfully
before the sequential driver starts lowering. The backend consumes the analysis
artifact and treats it as a precondition; callers must not mutate the AST or
signatures between analysis and lowering. Analysis stores expression pointers,
so the input AST must remain alive. Stage outputs are owned by the caller.
Function call resolution is recorded in the analysis artifact; LLVM lowering
uses the resolved qualified target without repeating name lookup rules.

The driver still runs sequentially. Future scheduling can tokenize and parse
independent files, collect their declarations at a synchronization point, then
analyze and lower independent function bodies against read-only signatures.
Multi-file input is implemented; the worker scheduler is not. Each analyzer
and lowerer has private state; LLVM string constants use a function-specific
prefix, and module assembly preserves source order regardless of task completion
order. Diagnostics should likewise be collected in source order when workers
are introduced. Tokenizing arbitrary chunks within a file needs extra handling
because strings and comments can span chunk boundaries.

Locals use entry-block allocations, allowing later LLVM optimization to
promote them to registers. A native `main` wrapper calls Duck's `Main`.

`Tests/stages.cpp` tests the boundaries directly, including semantic rejection
without lowering, forward/mutually recursive calls, concurrent analysis and
lowering, and byte-for-byte deterministic assembly across scheduling orders.

`Tests/run.cmake` exercises the full frontend â†’ LLVM â†’ linker â†’ executable
pipeline at both `-O0` and `-O2` and verifies output. Tests include nested loops,
short-circuit side effects, conditional branches, and compound assignments.
Reference tests cover caller mutation, aliasing, forwarding, recursion, value
copies, and rejection of temporary or constant arguments.
Initialization tests cover branch joins, early exits, loop increments and
continue edges, shadowing, and reads before assignment.
Control-flow tests also cover do-while condition side effects, break/continue
targets, nested single-statement branches, and standalone scope boundaries.
Negative cases cover types, name resolution,
scope, entry signatures, arity, missing returns, unreachable statements,
literal bounds, malformed source, and unsupported syntax.

Next milestones are a richer type system and references, containers and
ownership analysis, and resource read/write summaries that let calls touching
shared state run as tasks. Those semantics need dedicated analysis before the
full concurrency design can be claimed as implemented.
