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
  `string`, `name`, `cstring`, and opaque `void*` parameters, locals, and returns
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
  `byte` for `uint8` and `uint` for `uint32`. All integer types support locals,
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
  0 or 1. Explicit casts between `string` and `name` transcode text. Object, pointer,
  `cstring`, and void casts are not implemented.
- Short-circuit `&&` / `||` and Duck's `condition ? value | value` expression.
  Conditions require booleans; conditional branches must have matching
  non-void types or numeric types that can widen to a common type.
  Only the selected branch executes. Ternaries associate to
  the right and have lower precedence than logical operators.
- `if` / `else if` / `else`, `while`, `do body while condition`, and counted
  `for initializer; condition; step` loops with braced or single-statement
  bodies. Single-statement bodies also introduce a scope. `else` attaches to
  the nearest unmatched `if`; a simple body ends with a newline or semicolon.
  Function bodies still require braces. Standalone `{ ... }` statements create
  lexical scopes and propagate assignments to outer variables.
  Any for-header clause may be omitted; an omitted condition is true.
  `break` exits the innermost loop; `continue` runs the for-loop step or
  checks a while/do-while condition. A do-while body runs before its condition,
  and `break` skips the condition. A trailing semicolon after the do-while
  condition is optional at newline or `}`. The bare `do { ... }` form without
  `while` shown in the broader design is not yet supported.
  For-initializer locals belong to the loop
  scope. The step cannot declare a variable.
- `Print` with one numeric, boolean, `string`, `name`, or `cstring` value. Floating-point
  output uses `%g` (six significant digits). Booleans print
  as `0` or `1`. Text supports `\n`, `\r`, `\t`, `\\`, `\"`, `\0`,
  `\uXXXX`, and `\UXXXXXXXX` escapes.
  Text is printed as data, so percent characters never become format specifiers.
  Inferred locals can hold and reassign these literal values, including
  conditional selections such as `label : ready ? "yes" | "no"`.
- Line and block comments, newline statement terminators, optional semicolons,
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
`int8 n = 127; n += 1` wraps to `-128`. Integer casts may deliberately narrow
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

Execution is sequential. The compiler does not yet implement Duck's automatic
concurrency, ownership rules, resource locking, or runtime. Instance containers,
container references, local reference declarations, reference returns, arrays,
custom byte-width `iN`/`uN` types, other numeric types, mutable text buffers, range-based loops,
markup and compile-time evaluation
remain future work. Unsupported constructs produce errors; the design
examples are not a claim of compiler coverage. Non-void functions must have
an explicit return on every structurally reachable path, and statements
after a guaranteed return are rejected.

Static containers currently group functions only: fields, access modifiers,
instances, constructors, and properties are not implemented. The mixed instance
and static tutorial (`04_ContainersAndStaticBlocks.dk`) therefore remains a
design example. Qualified names currently refer to static functions, not object
member access. The executable entry point must still be a global `Main`.

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
semicolon, enclosing brace, or EOF. The compiler rejects bodies, conflicting
signatures for the same native symbol, and native Main declarations. Compatible
aliases share one LLVM declaration. Native declarations can be exported through
ordinary direct module dependencies; their bodies are never emitted.

Native signatures currently support:

- By value: signed/unsigned 32-bit and 64-bit integers, float, double, `void*`,
  and `string`, `name`, or `cstring` arguments. Native returns allow the supported
  numeric/pointer types or void, but exclude all text types.
- By reference: all supported numeric types and `void*&`. References pass the
  address of an initialized, mutable, exactly typed variable. Initialize output
  arguments explicitly; native calls do not bypass definite assignment checks.
- `bool`, 8/16-bit integers by value, native text returns, and text references are
  rejected until their ABI/lifetime behavior is implemented. For Windows BOOL,
  declare `int` and compare the return with zero. Duck `long` is always 64-bit;
  it is not necessarily the host C `long` type.

`void*` is an opaque handle type. It supports null, assignment, function arguments
and returns, mutable references, conditionals, and equality/inequality. It does
not support dereferencing, arithmetic, ordering, numeric casts, or Print. Use
`handle != null` to check a handle. A local handle is not initialized implicitly.
There is no automatic native resource ownership: explicitly call the native
library's destroy/close function when required.

`cstring` refers to immutable UTF-8 bytes with a trailing zero, suitable for
native `const char*` input. Duck functions can accept and return it, and Print
accepts it. It is distinct from an opaque handle and cannot be null or cast to
`void*`. Native callees must not modify its storage. Native references borrow
Duck storage only during the call and must not be retained. These native
contracts are the binding author's responsibility; Duck does not inspect the
library implementation. General pointers, buffers, callbacks, structures by
value, varargs, C++ name mangling, and alternate calling conventions remain
unsupported.

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
ownership analysis, and then dependency scheduling with a runtime. Those
semantics need dedicated analysis before the concurrency design can be claimed
as implemented.
