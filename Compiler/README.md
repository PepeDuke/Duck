# Duck bootstrap compiler

`duckc` is a C++17 frontend that lexes and parses Duck, checks the supported
types and names, and emits textual LLVM IR. Clang compiles that IR and links
the native executable against the C runtime. LLVM C++ headers and libraries
are not required. This allows the bootstrap to work with the installed
Windows LLVM tools distribution.

## Source layout

`Public/` holds the headers other targets include: `Pipeline.h`, the stage
entry points and their artifacts, and `Syntax.h`, the tokens and AST. The
implementation and its internal headers live in `Private/`.

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
`Examples/Bootstrap/21_Arrays.dk` demonstrates fixed and dynamic arrays and literals.
`Examples/Bootstrap/22_QueuesStacksPairs.dk` demonstrates a bracket checker, a round-robin queue, and pairs.
`Examples/Bootstrap/23_BoundsChecks.dk` demonstrates `IsValid` and guarded `?a[i]` reads and writes.
`Examples/Bootstrap/24_CollectionMutations.dk` demonstrates undo history, removal, `Clear()`, and mutation results.
`Examples/Bootstrap/25_GridsAndTables.dk` demonstrates a table, Pascal's triangle, and a Game of Life step on a sparse grid.
`Examples/Bootstrap/26_Iteration.dk` demonstrates forward and reverse `for` loops over collections and removing while iterating.
`Examples/Bootstrap/27_Ranges.dk` demonstrates inclusive ascending and descending ranges in loops and as values.
`Examples/Bootstrap/28_LoopChaining.dk` demonstrates chained for clauses and the placement of outer bodies.
`Examples/Bootstrap/29_DoLoops.dk` demonstrates `do` without a trailing `while`.
`Examples/Bootstrap/30_ProgramArguments.dk` reads the command line, and `Examples/Bootstrap/Scripts` runs files with `Exec`.
`Examples/Collections/01_ArrayMutation.dk`, `02_BoundsChecks.dk`, `03_Maps.dk`, and `04_InclusiveRanges.dk` now compile unchanged.
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
list does not implement imports or directory discovery.

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
  passed to a mutable reference. Constants can be passed by value. A constant
  requires its initializer: `#int age = 5` is valid, `#int age` is rejected,
  and a later `age = 6` is rejected.
- A required entry point, `Main()` or `Main(string [] args)`, with no return value.
  `args` holds the arguments after the executable's name and is empty when there
  are none. It is a mutable array that `Main` borrows, and other functions take it
  as `string []&`. On Windows the arguments come from `wmain` as UTF-16, or as
  bytes through `main` when `duckc` itself was built with MinGW, so that build sees
  arguments in the system code page. Elsewhere they are decoded as UTF-8. Each
  unpaired surrogate or malformed byte becomes U+FFFD, because managed text must be
  valid.
- Executable statements at file scope, run by `Exec("file.dk")`. A file's statements
  become one hidden function, so their locals last for one run and other files
  cannot see them; `Exec` runs them again each time. Functions and containers in
  the same file are ordinary declarations.
  - The path must be a string literal. It resolves against the directory of the
    file containing the call, and `..` is allowed. Computed paths are rejected.
  - A path matching none of the module's input files is a compile error, so there
    is no missing-file case at run time. A file from another module is rejected
    too: its statements are private to its module.
  - `Exec` on a file with only declarations does nothing. A file whose statements
    no `Exec` targets is a compile error, since they could never run; this rule
    can relax when computed paths exist.
  - The compile-time table is `Symbols::scripts`, keyed by normalized path. It is
    the natural basis for a run-time table if computed paths are added.
- Native `quat` and `rot` share the vector storage and paths, flagged on
  `NativeMath`; their own operators and methods are in `nativeOperation`,
  `checkRotationMethod` and `lowerRotationMethod`.
- Native `vecN`, `ivecN`, `dvecN`, `matN` and `dmatN`. `nativeMath` in `Syntax.h`
  describes a type's shape from its reserved name. `addNativeMathTypes` registers
  each as an ordinary container, so storage, copies and references use the
  container paths, with `X`..`W` fields or unnameable column fields. Constructors,
  operators, `Print` and indexing are native: `slotIn` indexes the packed storage
  as a fixed array. They are not available in `#` compile-time evaluation.
- The reserved `Math` namespace. `checkMathCall` types `Math.X(...)` and records
  it in `FunctionAnalysis::mathCalls`; `Symbols` rejects any declaration inside a
  namespace named `Math`. Exact operations (`Sqrt`, `Floor`, ...) lower to float
  or double LLVM intrinsics. Transcendental ones run at double through an
  intrinsic or the C library and round once for float, which is also how the
  compile-time interpreter evaluates them. Calls are pure for task scheduling.
  Vector functions read a container whose only fields are `X`, `Y` and
  optionally `Z` and `W`.
- Static container function groups, such as `Calc #{ int Twice(int n) { return
  n * 2 } }`, called as `Calc.Twice(3)`. Groups may be nested, repeated across
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
- Type limits `T.Min` and `T.Max` for every integer type, including custom
  widths (`i3.Max` is 8388607, `u16.Max` is 2^128 - 1), and for `float` and
  `double`, where `Min` is the lowest finite value, `-Max`. Each limit is a
  constant of type `T`, so `long n = int.Max` widens while `byte b = int.Max`
  is a type mismatch. `bool`, text, calendar, `big`, and `tiny` types have no
  limits. See [20_TypeLimits.dk](../Examples/Bootstrap/20_TypeLimits.dk).
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
  and `break` skips the condition. The condition ends at a newline or `}`.
  A `do` body without a trailing `while` repeats until `break` or `return`;
  `continue` starts its next iteration, facts after it come only from its
  `break`s, and without a `break` the code after it is unreachable. A `while`
  on the body's last line is always its condition; on a later line it is the
  condition unless a `{` block follows it, which makes it the next statement.
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
Floating-point `==` and `!=` are tolerant rather than bitwise (see
`floatEquality` in Codegen, mirrored by `floatsMatch` in the compile-time
interpreter); `a (t)== b` parses to a binary node with `t` as a third operand.
Other floating-point operations preserve IEEE behavior without fast-math assumptions:
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
[worker tasks](#worker-tasks) and [ordered blocks](#ordered-blocks).
Everything else executes sequentially. The compiler does not yet implement all
of Duck's ownership rules; see
[Owned heap locals](#owned-heap-locals) and [Owning fields and tracked
storage](#owning-fields-and-tracked-storage). Local reference declarations, reference returns, arrays,
mutable text buffers, range-based loops,
markup
remain future work. Unsupported constructs produce errors; the design
examples are not a claim of compiler coverage. Non-void functions must have
an explicit return on every structurally reachable path, and statements
after a guaranteed return are rejected.

Declare an instance container as `Example { ... }`. Fields and instance methods
share the body. Each member and each statement starts on its own line when
there is more than one. Instance methods can use field names directly or access
them through `this`. Declare and use the type by its name alone, such as `Example` or `Example*`.
Static function groups continue to use `#{}`.

A container name can be followed by a chain of bodies in any order: `{ ... }`
for instance members and `#{ ... }` for static ones, as in
`Meter { ... }` then `#{ ... }` on the next line. Bodies must be adjacent,
separated by at most one newline; a blank line between them is an error. A
parent list goes before the first body, and a chain that has a parent list
needs at least one instance body. Static and instance members are separate namespaces, so both kinds can
declare `Foo()`: `meter.Foo()` and a bare `Foo()` in an instance method call the
instance one, while `Meter.Foo()` and a bare `Foo()` in a static body call the
static one. Instance methods can also use the chain's static fields by their
bare names.

A container with an instance body can be declared inside a static body, as in
`Name #{ Space { int Value } }`. Its qualified name is its enclosing path,
`Name.Space`, which names the type everywhere: declarations (`Name.Space sp()`),
pointers and casts (`(Name.Space*)p`), parent lists, construction
(`Name.Space()`), and static calls (`Name.Space.Make()`). Inside `Name`, in its
nested containers, and in `Space`'s own methods, a type path resolves like a
relative function name: from the innermost enclosing container outward, then at
file scope, so a bare `Space` finds `Name.Space` before a file-scope `Space`.
Nested types merge across declarations by qualified name, and a type cannot
share its qualified name with a static function or field.

`using Name` at the top of a file or static body imports `Name`'s members into
that scope: after it, `Space.Foo()`, `Space sp()`, `Space()`, and
`Space.Count` mean `Name.Space.Foo()` and so on. Directives come before the
scope's other declarations, name a container by a path resolved like a type
(`using Name.Space`, or `using Space` inside `Name`), and apply only in the
file or body that declares them and in the bodies nested inside it, including
instance methods, field defaults, and static initializers written there.
Lookup visits each scope from the innermost outward; within a scope, its own
members come first, then its directives, so the file's own `Helper()` wins
over an imported one and an inner body's directive wins over the file's.
Locals still shadow everything. When two directives in the same scope both
provide a name, the reference is rejected and both candidates are reported.

Separate declarations of one container, in the same file or in different files
of a module, merge into one container. Instance fields join the layout in input
order: files in the order given to the compiler (the build tool sorts a
module's files by path), then source order within a file. Every declaration
must repeat the same parent list, in the same order, so a reader of any one
file can see what the container inherits. A field or method declared twice is
reported at the later declaration, along with where the first one is. A module
can add static blocks to a container it imports, but not instance members,
because the defining module has already fixed the layout that importers share.

Fields can declare defaults, as in `int Count = 3`, `string Title = Label("x")`,
or `Point Origin = {X: 1}`. A default is any expression of the field's type
that reads no fields and calls no instance methods; it may call functions and
read static fields. Default construction (`Named n()`, `Named()`) and the
fields a `{ Field: value }` initializer leaves out take these defaults, as do
inherited and nested container fields; other fields start at zero.

Construction starts from the constant defaults (literals, negated literals,
`null`, type limits, enum members, and `string`/`name` literals), kept as a
private constant copy in each module. It then evaluates the remaining defaults
in layout order: parents first, then the container's own fields, entering
nested containers. Each runs in a generated method (`Container.$default$Field`)
defined in the container's module. A field named in a `{ ... }` initializer
never evaluates its default; unnamed defaults run first, then the named values
in written order.

Fields of type `string`, `name`, and `big` make a container managed. Managed
storage is never null: such a field without a default starts as `""` or zero.
Copying a managed container retains its text and big values, assigning one
releases the values it replaces, and parameters, temporaries, and locals
release theirs when they go out of scope, through nested containers and
parents. A managed container's layout is compiler-private, so it cannot be
passed to native code by reference or reached through a raw pointer cast.

Managed containers can live on the heap (`Named* p()`, `p :: Make()`). Only an
owner frees a heap payload, and the owner's type names the payload's type, so
dropping an owner passes the heap runtime a generated cleanup function,
`@duck$cleanup.Named`. The runtime calls it to release the payload's string,
name, and big values, through nested containers and parents, just before
freeing the payload. Aliases never free it, so they pass no cleanup. Each
module defines the cleanup functions its owners use.

A container declares constructors as members named after it, `Gun(int ammo)
{ ... }`, and at most one destructor, `~Gun() { ... }`. Construct with
arguments as `Gun g(5)`, `Gun(5)`, or `Gun* p(5)` on the heap; `Gun g`,
`Gun g()`, and `Gun()` call the zero-argument constructor. An overload whose parameter
types match the arguments exactly wins; otherwise the single constructor the
arguments convert to is chosen, and two such candidates are reported as
ambiguous. A container with constructors cannot be built with a
`{ Field: value }` initializer. Constructors are not inherited and cannot be
protected: a `-` prefix declares a partial constructor instead.

A partial constructor, `-Entity(World& w) { ... }`, is a part of construction
that must run in addition to the regular one. A container has one partial part
and one regular part, each present when it declares such constructors; any
one overload completes a part, and a zero-argument overload runs
automatically when no other is chosen. A container's parts run in a fixed
order: its parents' parts (left to right, depth first), then its own partial
part, then its regular part, so `-Example()` runs before `Example()`.

Composite construction passes every part's arguments in one flat list in that
order: with `Gun : Entity`, `Gun g(w, 5)` runs `-Entity(w)` then `Gun(5)`. Each
part takes the arguments of one overload, or none; the split that leaves the
fewest parts missing wins, then one whose overloads match the argument types
exactly, and any other tie is reported as ambiguous. Parents with
constructors are constructed this way, so `W : G` with `G(int)` is built as
`W w(2)`.

Only a declared local may be left incomplete: `Gun g(5)` runs `Gun(5)` now
and leaves `-Entity` missing, and `g(w)` supplies it later. Each part runs as
soon as its arguments are given. Definite-initialization analysis tracks the
parts a local may or must still miss and merges them at branches. While a
part may be missing, the local can read and write fields but cannot call a
method, be passed by reference, have its address taken, be copied, moved,
returned, or assigned, or reach the end of its scope, including through
`return`, `break`, and `continue`. A completion must supply parts missing on
every path, so completing on one branch and again afterwards is rejected, as
is completing a local declared outside the loop that contains the call.
Temporaries (`Gun(5)`), heap constructions, fields without a default, and
static fields must be complete where they are built.

Calling a constructor on a complete object reinitializes it: `g(5)` on a local
or reference parameter, `p(5)` on a heap owner or alias (or a typed pointer),
and `box.Inner(5)` on a container field. The arguments go through the same
part split as a construction and must complete every part. The new state is
built first, so arguments, including a reference to the object itself as in
`a(a, 5)`, still see the old state. Then the old object is destroyed (its
destructor runs, and its string, name, and big fields are released), and the
new state moves into the same storage without a copy. References and heap
aliases to the object therefore see the new object, and a dead owner or alias
traps like any dereference. Constant locals, fields of constants or
temporaries, and the receiver inside its own method (`this(...)`) cannot be
reinitialized. A container without constructors reinitializes to its field
defaults with `x()`.

Construction follows the layout: each parent is defaulted and runs its own
parts first, then the container's fields get their defaults (a field without
one is fully built from its parts' zero-argument overloads), then the
container's own partial and regular parts run. Each constructor is an instance method keyed by its
parameter types, such as `@duck.Gun.$$new$int` or `@duck.Entity.$$part$World_r`.

A container with a destructor, or with a field or parent that has one, is
move-only so that each constructed object is destroyed exactly once. Reading
one out of a variable, field, or dereference as a value is rejected; pass it
by reference (`Use(Gun& g)`), and by-value parameters of such a type are
rejected. Call results and constructions move into their destination, and
`return local` moves the local out instead of destroying it. Static fields
cannot hold one, since static storage is never destroyed.

The destructor runs when a local leaves scope (locals in reverse declaration
order, including on `break`, `continue`, and `return`), when an assignment
replaces the value, when a temporary dies (`Make()` as a statement, or a method
called on a call result), and when an owner drops a heap payload, through the
heap cleanup function. A generated `@duck$destroy.Gun` runs the destructor's
body, then destroys fields in reverse layout order, so parents are destroyed
last; string, name, and big values are released after that.

A `#{}` block can also hold static data fields, such as `int Score = 10`,
`#int Max = 100` for a constant, or `-int Secret` for a protected field. Each
field has one storage location, defined in the module that declares it and
declared `external` in modules that import it. Read and write a field by its
qualified name (`Game.Score`, `Game.Level.Number`); functions in the block and
in blocks nested inside it can also use the bare name, which locals shadow.
Fields hold numbers, `bool`, enums, `string`, `name`, `big`, `void*`, or
containers, with any initializer of the field's type; a container-typed field
without one starts from the container's defaults. Constant initializers become
the storage's initial value. The others run before `Main` in this order: every
static of the imported modules, then this module's statics in declaration
order (input file order, then source order). An initializer may read any
imported static and any static declared earlier in its module; reading itself
or a later one is rejected. A function an initializer calls sees later statics
at their initial constant or zero value. A `#` constant may also take a
computed initializer, which is still written only once.

Each static has a `$init` function that runs at most once. It first runs the
previous static's `$init`, or, for a module's first static, every imported
static's, so the order holds whichever module's startup runs first. Each
module with static fields registers a startup routine in `llvm.global_ctors`.
`tiny` fields and pointer fields other than `void*` are not supported yet.
Static storage is one resource each to the scheduler, so work touching the
same static never runs at the same time.

Declare parents after a colon, as in `Cat : Animal, Human { ... }`. Each
parent's state is embedded at the front of the derived layout in list order,
so a derived value starts with its first parent exactly as a C struct whose
first member is that parent. Fields and methods are found in the container
itself, then in each parent left to right, depth first, so the leftmost parent
wins an overlap. Inherited fields can be named directly inside methods and in
`{ Field: value }` initializers; an inherited method receives the embedded
parent state as its receiver. `this.Human.Walk()` reaches a lower-priority
parent explicitly; that parent view is protected, so outside code cannot use
it. Unknown or repeated parents and inheritance cycles are rejected, as is a
field that redeclares an inherited field or method.

A derived container satisfies a parent's reference parameter: `Talk(c)`
passes a `Cat` to `Talk(Animal& a)`, and `Stroll(c)` passes it to
`Stroll(Walker& w)`. The callee receives the parent's embedded state, so
`a.Legs` reads and writes the cat's own field, and the object's dispatch
table, so `a.Speak()` and any call on `this` inside `Animal`'s methods reach
`Cat`'s overrides. Passing the reference on, as `Again(a)`, keeps the table.
A container that holds the parent through more than one parent is rejected
as ambiguous. By-value parameters still take only their exact type, since a
copy holds just the parent's state. Through such a reference, replacing the
whole object, by reinitialization (`a(...)`) or assignment (`a = Animal()`),
traps when the reference holds only part of a larger object, because the
derived state would be left behind; on a whole `Animal` it works as before.

An instance method that redeclares an inherited method overrides it, and
every inherited method of that name through any parent: `string Speak()` in
`Cat : Animal` overrides `Animal.Speak`. An override must keep the parameter
types, `&` markers, result type, and access of each method it overrides;
fields cannot be overridden, and a method cannot redeclare an inherited
field. Calling a method on the object (`cat.Speak()`) runs the version its
exact type sees. Inside a method, a call on `this` (`Speak()` or
`this.Speak()`) runs the final overrider for the whole object, so
`Animal.Describe()` calling `Speak()` prints `meow` for a `Cat`, including
when `Cat` lives in a module that imports `Animal`. The final overrider is the
most derived declaration on the path from the object to the receiving parent
state; overriding a method declared in two parents overrides both. A base
call, `this.Animal.Speak()`, runs that parent's version directly, bypassing
the overrides below it, while calls on `this` inside it still dispatch.
Constructors and destructors are not dispatched through: inside them, calls
on `this` run their own container's versions, since a derived part is not
built yet or is already destroyed.

A getter block `@{ ... }` and a setter block `={ ... }` chained to a container
declare properties, as in `Examples/Containers/04_Properties.dk`:

```duck
Person
{
    -int Years = 20
}
@{
    Age: Years
}
={
    Age: Years = value
}
```

Each entry is an optional type, the property name, `:`, and either a getter
expression or a setter statement, or a `{ ... }` body (a getter body needs a
type and returns the value). Entries are separated by newlines, or by commas
after getter expressions. A setter reads the assigned value as `value`. An
untyped property takes the type of its getter expression, and an untyped
setter takes its container's getter's type; a typed setter must agree with
the getter. A `-` prefix makes an accessor protected.

`p.Age` calls the getter, `p.Age = 25` calls the setter, and `p.Age += 5`
and `p.Age++` call the getter and then the setter on a receiver evaluated
once. Inside a method, a bare `Age` names the property when no local or field
has that name. A property may share its name with a protected field of its
own container: inside the container (and derived ones) the name means the
field, and outside code, which cannot see the field, reaches the property.
Other clashes with fields or methods are rejected. A property without a
setter is read-only and one without a getter can only be assigned. A property
is not storage, so it cannot be passed by reference, have its address taken,
or be reinitialized; a setter cannot run on a temporary, and neither accessor
runs on a constant, since accessors may modify their receiver.

Accessors are instance methods named `get-Age` and `set-Age`, so they inherit,
override (keeping the property's type and access), dispatch through `this`
and container references, and cross module boundaries like other methods. A
derived container that redeclares a getter overrides it, and a field cannot
redeclare an inherited property. Static properties are not supported.

Each source instance method takes a hidden dispatch table after its
receiver, and each container reference parameter of a non-native function
takes one after the reference. A table describes an object of one container as
seen through one of its parent states: that state's offset within the object,
whether it is the whole object, the tables of its own parents, and, for each overridable method name it can see, the final
overrider, its receiver's offset, and its table. Calls on `this` load those
slots and call indirectly, as do calls on a container reference parameter;
calls on an object of known type, and base calls, are direct. Every module emits private copies of the tables it uses, as
`@"duck$vt.Cat"` and `@"duck$vt.Cat$1"` for the `Walker` state of
`Cat : Animal, Walker`. Objects carry no hidden pointer, so container layouts
are unchanged.

Prefix a field, instance method, or static-container function with `-` to make
it protected or `+` to state the default public access. The sign must touch the
declaration (`-int Cash`, not `- int Cash`). Protected members can be read,
written, called, named in a `{ Field: value }` initializer, or taken as a
callback only from inside their declaring container, static containers
nested in it, and containers that derive from it. File-scope functions and
nested containers take no prefix.

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

## Arrays

```duck
int [4] a            // fixed: four elements stored inline, starting at 0
a[0] = 5
a[3] += 4
Print(a.Len)         // 4
string [2] words     // elements start at their defaults: "" here
Point [3] points     // and a container's field defaults
points[1].Move(3)

int [] squares       // dynamic: grows as elements are added
squares += 9         // append
squares--            // remove the last element
Print(squares.Len)   // a long
```

`a[i]` names an element: it can be read, assigned, used with compound
assignment, passed by reference (from a fixed array), and reached through
(`points[1].X`). The array and the index are evaluated once, and an index
outside `0 <= i < Len`, or `--` on an empty array, traps. Indexes are
integers. Elements are numbers, `bool`, `char`, enums, `string`, `name`,
`big`, `tiny`, dates and times, containers, heap references (`Item* []` holds
tracked aliases), and other arrays (see
[Multidimensional arrays and grids](#multidimensional-arrays-and-grids));
arrays of collections are a later item. See [Indexing and bounds
checks](#indexing-and-bounds-checks) for `IsValid` and `?a[i]`, and
[Collection mutations](#collection-mutations) for `++`, `-= [i]`,
`Clear()`, and the results of `(a += x)` and `(a--)`.

A fixed array is a value of `[N x T]`. Declared without an initializer, it
starts zeroed and then copies each element's default into place: empty text,
a zero `big`, or a container's constant default. A container whose elements
need running code (constructors or computed defaults) cannot be defaulted
this way; use a dynamic array and add elements. A fixed array of plain
elements copies like any value; one whose elements need cleanup (text, `big`,
or containers that are managed or have destructors) is move-only and cleans
up every element when it dies. As a container field, a fixed array must hold
elements whose default is all zero bits.

A dynamic array is one pointer to heap storage, null while empty, so it can
be a container field (making its container move-only). It shares the queue
and stack runtime: elements are element-sized slots, `+=` moves the new value
into the next slot, and growing doubles the buffer and moves the elements
bytewise, which is a valid move for every Duck value. `--` cleans up the last
element, and freeing the array cleans up each element in index order:
releasing text and `big` values, and running a container's heap cleanup
(`@duck$cleanup.Name`: its destructor, then its field releases). An element
of a dynamic array cannot be passed by reference or have its address taken,
because growing the array would move it. Dynamic arrays are move-only: pass
them by reference and return locals to move them.

### Size hints

`T [50*]` and `T [*]` declare ordinary dynamic arrays and add an estimate of
how many elements to expect: about fifty, or a huge number. Each dimension of
a multidimensional array can have its own hint. A hint is not part of the
type, so hinted arrays are interchangeable with `T []`, and it is not a
capacity: `int [3*] a` grows past three elements like any other array. Only the
[cost model](#worker-tasks) reads it. `[0*]` and a hint on a `[-]` grid are
rejected. See [06_SizeHints.dk](../Examples/Collections/06_SizeHints.dk).

## Collection literals

```duck
int [] values = {5, 1, 2}
int [3] fixed = {7, 8, 9}             // exactly as many as the length
int queue waiting = {2, 3, 4}         // 2 comes out first
int stack pile = {2, 3, 4}            // 4 comes off first
string, string pair town = {"name" : "Bob", "city" : "York"}
string pair person = {name: "Bob", role: "admin"}   // bare words are name keys
Point [] points = {{X: 1}, {X: 4, Y: 5}}
values = {9}                          // replaces (and frees) the old contents

guesses : {5, 1, 2}                   // int []
mixed : {1, 2.5}                      // double []
ages : {"Ada" : 36, "Alan" : 41}      // string, int pair
```

A `{ ... }` literal takes its meaning from the type it initializes, assigns,
passes, or returns: `Name: value` entries initialize a container's fields,
plain values fill an array, queue, or stack in order, and `key : value`
entries fill a pair in order (a repeated key replaces its value in place).
In a pair keyed by `name`, a bare word before `:` is the key itself, so
`{name: "Bob"}` works even though `name` is a type; in other pairs it is a
variable holding the key. A word followed by `:` is always read as such a
key, so a variable used as a key in an untyped literal needs another form.

Without an expected type, as in `x : { ... }`, plain values make a dynamic
array and `key : value` entries a pair (bare-word keys are names), of the
entries' common type: equal types, or numbers widened as arithmetic would
widen them. Queues and stacks are never inferred. An empty literal, a mix of
values and entries, and entries whose types have no common type are rejected;
declare the type instead. Nested literals take their element type, so
`{{X: 1}}` builds `Point` elements of a `Point []`.

## Queues, stacks, and pairs

```duck
int queue q          // first in, first out
int stack s          // last in, first out
string, int pair ages // keyed by string, in insertion order
int pair scores      // keyed by name: `T pair` means `name, T pair`

q += 2               // add at the back
q += 3
Print(q.Front)       // 2
q--                  // remove the front
s += 2
s += 3
Print(s.Top)         // 3
s--                  // remove the top
ages += "Ada" : 36   // add, or replace the value of an existing key
Print(ages["Ada"])   // 36; a missing key traps
Print(ages.LastKey)  // the last key added, and ages.Last its value
ages--               // remove the last key added
Print(q.Len)         // number of elements, as a long
```

A collection starts empty and is a single pointer to heap storage, null until
something is added, so it can also be a container field. `q.Front`, `s.Top`,
`p.Last`, `p.LastKey`, and `p[key]` read a copy of an element, and trap when
the collection is empty or the key is missing, as does `--` on an empty
collection. Replacing a key's value keeps the key's place, so `p.LastKey` and
`p--` always refer to the most recently added key that is still present.

Elements are numbers, `bool`, `char`, enums, `string`, `name`, `big`, `tiny`,
dates and times, and heap references, which are tracked aliases; keys are
integers, `bool`, `char`, enums, `string`, `name`, `big`, and dates and times
(floating and `tiny` keys would compare by representation, so they are
rejected). Containers, raw pointers, and nested collections are not elements
yet, and grids cannot hold heap references.

Collections are move-only: pass one by reference (`Fill(int queue& q)`),
return a local to move it out, and receive a call's result in a declaration
or assignment. By-value parameters, copies, and static fields of collection
type are rejected, and a container with a collection field is move-only and
frees it when destroyed. Elements that are `string`, `name`, or `big` are
released when removed, replaced, or freed with their collection.

A queue or stack is a ring buffer of element-sized slots that doubles when
full, copying its elements in order from the head; a queue removes at the
head and a stack at the back. A pair keeps entries (key, then value at a
16-byte-aligned offset) in insertion order and finds keys by a linear scan,
comparing `string`, `name`, and `big` keys by value and other keys bytewise
after zero-filling them. The runtime is emitted module-locally from
`CollectionRuntime.cpp`.

## Multidimensional arrays and grids

```duck
int [2][3] table          // 2 arrays of 3 ints, stored inline: table[1][2] is the last
int [][] rows             // a dynamic array of dynamic arrays
int [3][] buckets         // 3 dynamic arrays, inline
string [][4] lines        // a dynamic array of string [4]
int [-][-] grid           // a sparse grid indexed by any int coordinates

table[1][2] += 10
rows++                    // adds an empty row
rows[0] += 7              // appends to row 0
rows += {1, 2, 3}         // adds a row
grid[-2][1] = 5           // stores the cell
Print(grid[100][100])     // 0: an unwritten cell reads as the default
nested : {{1}, {2, 3}}    // int [][]
```

Dimensions are written type-first, in index order: `T [5][10]` is 5
arrays of `T [10]`. Each `[N]` dimension is fixed and each `[]` dynamic,
and they mix freely. An array of arrays is an ordinary array whose element
is an array, so everything above applies at every level. `a[i]` names a
whole inner array: `a[i].Len`, `a[i] += x`, `a[i] -= [j]`, and passing a
fixed `a[i]` by reference all work. Each index is checked against its own
dimension before the next is applied, so a fixed `int [2][3]` traps on
`a[0][3]` instead of reading `a[1][0]`. A fixed array of fixed arrays is
one contiguous `[2 x [3 x i32]]`; its defaults and cleanup cover all six
elements. A dynamic row is a pointer, so an array holding one is
move-only. Freeing it frees each row and each row's elements, through a
generated per-module `@duck$cleanup.array.*` helper.

`?a[i][j]` and `IsValid(a[i][j])` cover the whole chain: a missing outer
element gives a miss without evaluating the inner index. A container field
may hold a nested array only when every dimension is fixed and its elements
are plain, as in `int [3][4]`; other nested fields are a later item, as
are arrays of collections.

### Signed grids

`T [-]`, `T [-][-]`, and so on are sparse grids. Every dimension must be
`[-]`, coordinates are `int` values from `int.Min` to `int.Max`, and
elements are the same plain values a queue or pair holds. A grid stores
only the cells that were written, in a pair keyed by an `[N x int]`
coordinate tuple compared bytewise, so lookups scan the stored cells
linearly. A grid is a collection: it starts empty, is move-only, can be a
container field, and is freed with its owner.

- `g[x][y]` takes exactly as many coordinates as the grid has dimensions.
  Reading an unwritten cell gives the element default without storing it.
  Writing a cell, including `g[x][y] += 1`, stores it first with the
  default if needed.
- `IsValid(g[x][y])` tells whether a cell is stored. `?g[x][y] = v` writes
  only a stored cell, and `?g[x][y]` reads like `g[x][y]`.
- `g -= [x][y]` removes a stored cell (a missing one traps), and
  `(g -= [x][y])` gives its value. `g.Len` counts the stored cells and
  `g.Clear()` removes them all.
- A cell cannot be passed by reference, because storing other cells can
  move it. `+=`, `++`, `--`, literals, and pair members such as `LastKey`
  do not apply to grids.

## Collection mutations

```duck
int [] s = {1, 1}
index : (s += 4)         // 2: the index the value was inserted at
s++                      // appends a default element, 0 here
s -= [0]                 // removes element 0; later elements shift down
last : (s--)             // removes the last element and gives it
s.Clear()                // frees every element; s is empty again

string, int pair ages = {"Ada" : 36}
ages -= ["Ada"]          // removes the entry for a present key
int, string pair lines
lines++                  // adds key 0 with a default value, then 1, ...
```

| Mutation | Array `T []` | Queue | Stack | Pair |
| --- | --- | --- | --- | --- |
| `+= x` | appends | adds at the back | adds on top | `+= key : value` adds, or replaces a present key's value in place |
| `++` | appends a default | adds a default at the back | adds a default on top | adds the next integer key with a default value |
| `--` | removes the last | removes the front | removes the top | removes the last key added |
| `-= [i]` | removes element `i` | rejected | rejected | `-= [key]` removes that key |
| `.Clear()` | empties | empties | empties | empties |

Fixed arrays have a fixed length and reject all of these. A default element
is what a guarded read gives for a missing one: zero, `false`, empty text, a
zero `big`, or a container's default, built as a declared local would be.

Inside parentheses a mutation gives a result. `(s += x)`, `(s++)`, and
`(p += key : value)` give the inserted element's index as a `long`, the
same index `s[i]` reads it back with; for a queue that is its position from
the front, for a stack from the bottom, and for a pair the entry's position
in insertion order, which a replaced key keeps. `(s--)` and `(s -= [i])`
give the removed element (a pair gives the removed value). It moves out of
the collection, so nothing is copied or released; a container's destructor
runs once, when the result dies. Without parentheses the removed element is
released or destroyed at once. `(x += 1)` on a number is rejected: only
collection mutations give a result.

`p++` needs an integer key type. It adds key 0 to an empty pair and
otherwise the last key added plus 1; it traps when that key would overflow
the key type or is already present, so it never overwrites an entry.

`-= [i]` traps outside `0 <= i < Len`, `-= [key]` traps on a missing key,
and `--` traps on an empty collection, as does its result form. Removing an
element shifts the later ones down by one, keeping their order, so it costs
time proportional to what follows it. `Clear()` releases every element in
order and frees the storage; the collection is then empty and null, like a
new one, so clearing an empty collection does nothing and the next `+=`
gives index 0.

## Iterating collections

```duck
int [] scores = {10, 20, 30}
for score : scores       // first to last
    Print(score)
for -score : scores      // last to first
    if score == 20
        scores -= score  // removes this element

string, int pair ages = {"Ada" : 36}
for person, age : ages   // or `for age : ages` for the values alone
    age += 1
```

| Collection | `for x` order | `for -x` order | `x` names | `c -= x` |
| --- | --- | --- | --- | --- |
| `T [N]` | index 0 up | last index down | the element | rejected: fixed length |
| `T []` | index 0 up | last index down | the element | removes it |
| Queue | front to back | back to front | the element | rejected: drain with `--` |
| Stack | bottom to top | top to bottom | the element | rejected: drain with `--` |
| Pair | insertion order | reverse insertion | the value, and in `for k, v` a constant key | removes the entry |
| Grid | storage order | reverse storage order | the cell, and in `for k, v` its constant coordinates `k[0]`, `k[1]` | removes the cell |

The loop variable names the current element's storage, so `x = v`, `x += 1`,
and `x.Method()` change the collection. It is a new variable in each
iteration's scope, is constant when the collection is, and, for anything but
a fixed array, cannot be passed by reference or have its address taken, like
any element in storage that can move. The collection must be a named local,
parameter, or field chain such as `this.items`; store a call's result in a
local first, and use a counted loop for `a[i]`. A body cannot declare a local
with the collection's name.

The loop lowers to an index cursor over the live length: the array or pair
position, or the distance from a queue's head. The collection is named again
at each use of the variable, so growth that moves the storage cannot leave
the variable dangling, and each use is bounds checked.

**Removing while iterating.** The only change a body can make to the length
of the collection it iterates is `c -= x`, where `x` (or the key in
`for k, v`) is that loop's variable:

- The rest of the loop visits every remaining element exactly once, forward
  or reverse. A forward loop stays at the position the next element shifted
  into; a reverse loop's next element did not move.
- After the removal `x` names nothing: reading it, writing it, or removing it
  again traps. `(c -= x)` gives the removed element if it is still needed.
- `+=`, `++`, `--`, `-= [i]`, `Clear()`, assigning the collection, and writing
  a grid cell (which can store a new one) are rejected in the body, as is
  removing while a second loop iterates the same collection.

**Changes made through a call** cannot be seen by the compiler and stay
memory safe, but positions are not adjusted. A forward loop runs while its
position is below the live length, so it visits appended elements and stops
early after a shrink. A reverse loop continues from the next lower position,
moved down to the last element if the collection has shrunk below it, and
does not visit appended elements. Using `x` after a call removed its element
traps if the position is now outside the collection, and otherwise names
whichever element is at that position.

## Loop chaining

```duck
for row : 0...1 |             // two nested loops; Print is the inner body
    column : 0...2
    Print(row * 10 + column)

for n : 0...3 |               // the | comes first: this body runs after the inner loop
{
    n++
}
    i : 0...5
    Print(n)

for n : 0...3                 // the | comes last: this body runs before the inner loop
{
    n++
}|
    i : 0...5
    Print(n)
```

A `|` nests the following clause, written without `for`, inside the clause
before it. Any for clause can be chained, to any depth: counted, range, or
collection, as in `for row : rows | value : row`. The last clause must have
a body. An outer loop has at most one body of its own, either before or after
its inner loop; giving it both is rejected.

The form is exactly the nested loops it stands for. An outer body and its
inner loop are one statement list, so a local declared in a body that runs
before the inner loop is visible in it, `break` and `continue` apply to the
innermost loop around them, and a `continue` in a body that runs first skips
the inner loop. Because `|` also means bitwise or, parenthesize a bitwise or
at the end of a for header, as in `i = (i | 1)`.

## Ranges

```duck
for i : 0...5                 // six values, 0 through 5
for i : 5...0                 // six values, 5 down to 0
for i : 0...values.Len-1      // ... binds looser than arithmetic

n : 0...3                     // int [] {0, 1, 2, 3}
long [] down = 2...0          // {2, 1, 0}
```

`a...b` includes both endpoints. Each endpoint is an integer and is evaluated
exactly once, `a` first, so changing a variable used in `b` does not move the
end of a running loop. The range's type is the endpoints' common type, as
arithmetic would widen them, and a literal endpoint takes the other
endpoint's type; mixed signed and unsigned values that have no common type
need a cast.

The direction comes from comparing the endpoints, once: `a <= b` counts up,
otherwise down. A range is never empty, so `0...values.Len-1` on an empty
array runs for 0 and then -1; use a counted loop or `for x : values` when a
collection may be empty. `for -i` does not apply: write the high endpoint first.

The loop variable is the counter itself, a new mutable local scoped to the
loop. Writing to it changes which values follow. After each iteration, the
loop ends if the counter has reached or passed `b`; only then does it step.
So a range ending at `int.Max` or `uint.Min` stops without overflowing, and a
body that moves the counter past `b` ends the loop.

Anywhere else, a range is a value: a new `T []` holding every value in order,
accepted wherever a dynamic array of those integers is, and the expected
element type sets the endpoints' type, as in `long [] w = 1...2`. A fixed
array or any other target is rejected. The array is built eagerly, so a huge
range traps when memory runs out. An unparenthesized `|` after the second
endpoint is not part of the range, so write `0...(a | b)` for a bitwise or.

## Indexing and bounds checks

```duck
int [3] f = {1, 2, 3}
string, int pair ages = {"Ada" : 36}

ages["Ada"] = 40         // writes the value of a present key
ages["Ada"] += 1
if IsValid(f[6])         // false: 6 is outside 0 <= i < 3
    f[6] = 5
?f[6] = 5                // skipped: the element is missing
?ages["Zed"] = Count()   // skipped, and Count() is not called
Print(?f[6])             // 0, the default of int
Print(?ages["Ada"])      // 41
```

`a[i]` and `p[key]` both name storage: they can be read, assigned, and
used with compound assignment. Writing `p[key]` replaces the value of a key
that is already present (releasing the old value) and traps when the key is
missing; add new keys with `p += key : value`. A pair value cannot be
passed by reference or have its address taken, because adding entries can
move it, just like an element of a dynamic array.

`IsValid(a[i])` and `IsValid(p[key])` give a `bool`: whether the index is
inside the array or the key is present. They do not read the element. The
argument must be an indexed element.

A leading `?` guards an index. The collection and the index or key are
evaluated once, and that one lookup decides both the guard and the access:

- **Guarded writes.** `?a[i] = v`, `?a[i] += v`, `?a[i]++`, and the other
  assignments run only when the element exists. When it is missing, the
  whole statement is skipped, so `v` is not evaluated.
- **Guarded reads.** `?a[i]` has the element type. A missing element reads
  as that type's default: zero, `false`, empty text, a zero `big`, or a
  container's field defaults. Use `IsValid` to tell a stored default from
  a missing element. A container whose default is built at run time
  (constructors or computed defaults) cannot be read this way; check
  `IsValid` first.

`?` guards only a trailing index: `?points[0].X` and guarded call chains
such as `?o.ex.Foo()` are later items. The runtime helpers
`@duck.seq$find` and `@duck.pair$lookup` return the element's slot, or null
when it is missing.

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
library implementation. Pointer arithmetic, structures passed to C by
value, varargs, C++ name mangling, and alternate calling conventions remain
unsupported.

### Reflection, duck calls, and func values

Each container used through a dispatch table, reflection, or a duck call gets a
private descriptor, `@duck$type.<container>`: its names and a member table of
fields then methods, each with its names, signature text, declaring container,
flags (1 method, 2 protected), attributes, and for a method the final overrider
with its receiver offset and table. Every dispatch table points at the
descriptor of its dynamic type, so `this` and container reference parameters
find the runtime type; every other receiver's static type is already exact.

`e..Foo(args)` takes its shape from the methods the receiver could dispatch to,
when they agree on it, and otherwise from the arguments. `@duck.meta$find`
compares name and signature text against the public methods, which makes a
signature mismatch the same as a missing method.

A `func` is one word: null, or a reference-counted closure
`{ count, dispose, fn, captures... }`. `fn` takes the closure, then the call's
arguments. A count below zero is an immortal constant, used for lambdas without
captures and functions used as values (-1) and for member lambdas (-2), whose
first argument is the object instead of the closure.

Each captured local lives in a box, `{ count, dispose, cleanup, pad, payload }`,
which the scope and each closure hold a reference to, so assignments are shared
and the payload is destroyed with the last reference. A captured tracked pointer
takes a `heap$alias` slot in the closure. `this` and reference parameters are
stored as raw addresses, which is why those lambdas stay in a local of the frame.

Fixed and dynamic arrays hold `func` values, releasing each closure with its
element. Queues, stacks, pairs, and static fields cannot hold them yet.

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
compiler proves only access the pointee, including from fields and collection
entries. Unproven escapes, raw casts, and transfers from aliases are rejected.
Heap references cross a function return as described in [Owning and lending
returns](#owning-and-lending-returns). Native pointers obtained from `&local` or a platform API continue
to use the existing raw ABI.

### Owning and lending returns

Before analyzing callers, `resolveReturnRoles` works out what each function
with a `T*` result can return, on every path and repeating until functions
that return each other's results settle. `return ::x` gives ownership, `return x`
of a tracked reference lends it, and `return Other()` passes on whatever
`Other` can give. A function with any of these has an owner-role result; one
that only returns raw pointers keeps its raw result, and may not mix the two.
Accessors stay raw. The result is part of a method's signature, so an override
must return the same kinds of reference as what it overrides.

The returned word carries the role. An owning result is an ordinary owner word;
a borrowed one is its control block address with bit 1 set, taken before local
cleanup, so an alias to the function's own local arrives closed. A local
received with `:` keeps that word, in an owner-role slot, and the runtime reads
bit 1 to decide whether clearing the slot closes the object or only drops a
reference. Such a local is also treated like an alias for active uses, since
its owner may be elsewhere.

`::` checks the bit when the function can lend, and traps on a borrowed result;
a function that only lends is a compile error instead. `(a.b :: Make())`
reports a borrowed result as `false` and releases it. Results still need a
destination (`:`, `::`, or `return`), including a function that only lends, and
`Item* x = Make()` is rejected in favor of those two forms.

### Owning fields and tracked storage

The declaration decides what a pointer field is. `Item* Part()` owns an object
built with the container, through a generated default method, and may pass
constructor arguments. `Item* Next :: null` owns and starts empty, and `Item*
Next :: value` owns the result of evaluating value at construction. Plain
`Item* Ref` is a tracked alias. `void*` and pointers to scalars such as `int*`
stay raw pointers, in C layout. A raw value cannot be stored in an alias field,
and a container with tracked or owning fields has compiler-private layout, as
managed containers do, so it cannot be passed to native code. To keep a native
pointer to a container in a field, store it as `void*` and cast it when reading.
Owning fields whose construction would lead back to their container are
rejected.

An owning field is move-only storage holding the same word as a local owner:
the payload address, or the tagged control block. An alias slot, in a field,
array element, queue, stack, pair value, or closure, holds the untagged block
address, one reference. Copying a container takes a reference for each alias it
holds, and overwriting or destroying one releases it, through the same paths as
`string` and `func` fields. Reading an alias slot gives the payload, or null
once it has expired. Removing an entry releases its alias, so `(list--)` is
rejected. An expired entry stays in place and compares equal to null.

`target :: source` runs the cycle check before changing anything. Instead of
walking up from the destination, it searches the moved object's subtree for the
destination slot: a by-value container, parent state, array element, or `this`
has no header from which to find its enclosing object, and a search downward
needs no parent links to keep correct as objects move. Each container that can
be searched gets a generated `@duck$reach.<Container>`. Moves the types show to
be safe skip it, as does a move into a local's own storage, which needs at most
one pointer comparison: nothing owns a local, so it has no ancestors. The search
costs time proportional to the moved subtree, and otherwise only happens for
self-referencing types.

`if p` accepts a pointer. For a tracked reference named by a local or field
path, the branch holds a use of the object: dereferences of that same path are
bound to the evaluated reference, and every exit ends the use. A local owner
needs no use, since only its own function can clear it. A branch that assigns or
moves the path gets a plain null test, as do conditions on array elements.

Every other condition accepts a pointer as well: `while p`, `do ... while p`,
the condition of a `for`, `!p`, `p && q`, `p || q`, and `p ? a | b`. A pointer
is true when it is not null, and these results are `bool`. They are plain tests
that hold no use of the object: only `if p` binds its branch, so a later
dereference in a `while p` body traps if the object has expired since the test.

`?p.q.Foo(x)` lowers to one check per link, root first, before the arguments.
Dereferences, array and pair lookups, and calls returning pointers all count
as links, and each is evaluated once: the call reuses the payload the check
found. A tracked link takes a use until the statement ends, except a local
owner, as elsewhere. The guard applies to methods and func fields, and a
result takes the same default as a missing `?a[i]`, so a container built at run
time cannot be read through one. Field reads such as `?p.q.X` are not guarded
yet.

### Active uses

A control block is `{ payload, references, uses, cleanup }`, where bit 63 of
`uses` means closed. Dropping an owner publishes the cleanup and sets that bit
with one atomic operation. `heap$pin` increments `uses` unless the object is
closed, and takes a reference so the block outlives a cleared slot; `heap$unpin`
undoes both. Whichever operation leaves a closed object with no uses runs the
cleanup, so it runs exactly once.

A statement that can run Duck code, through a call, property, or destructor,
pins each tracked object it dereferences until the statement ends; a statement
that cannot run code has nothing that could release the object, and stays a
plain load. A local owner is not pinned, and so takes no control block. A
statement pinning an owner held in a field promotes that owner to a control
block. Worker tasks do not receive heap references yet, so the atomic paths are
only exercised on one thread today.

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

Each function gets a read/write summary: the static fields, and the fields of
its reference parameters and receiver, that it may read or write. A summary is
a least fixpoint over the call graph, so a recursive group shares its effects.
At a call the callee's summary is rewritten in the caller's terms, so `Add(x)`
writes `x`, and `Move(a.Left)` writes that field of `a`. Two resources conflict
when they share a root and one field path is a prefix of the other; every array
element counts as its whole array. Effects are *unknown* when they cannot be
named: native and imported functions, pointers, lambdas and duck calls, and
calls on `this` or a reference parameter whose method is overridden. In a
library those calls are always unknown, because an importer may override them.

A statement whose initializer or whole body is a direct call can become a
worker task when the callee's effects are known, it never calls `Print`, its
body exchanges only plain data (numbers, `bool`, enums, and containers or arrays
of those), and the [cost model](#the-cost-model) says it is worth a thread.
Arguments are evaluated on the calling thread, in source order, when the
statement is reached. A task joins:

- before the first later statement in the same list that conflicts with it,
  including one that uses its result;
- before a statement with an external or unknown effect;
- before a statement containing `return`, `break`, or `continue`, or a loop
  that never exits;
- at the end of its statement list, before scope cleanup.

```c++
tally.FillEvens(n)   // Runs as a task.
tally.FillOdds(n)    // Overlaps it: the two touch different fields.
Print(tally.Evens)   // Print waits for every earlier task.
```

A call is not spawned when the very next statement would join it. Only
conflicting work is ordered, and joins precede every external effect, so program
output, runtime traps and nontermination occur in the same order as sequential
execution. See [15_WorkerTasks.dk](../Examples/Bootstrap/15_WorkerTasks.dk) and
[05_TasksWaitForConflicts.dk](../Examples/Concurrency/05_TasksWaitForConflicts.dk).

At run time, `DUCK_WORKERS=n` limits the number of threads executing Duck code,
including the main thread; it defaults to the active processor count. When the
limit is reached the task runs inline on the caller, so nested tasks never wait
for a free worker. `DUCK_WORKERS=1` runs every task inline, in source order.
`duckc --sequential` disables task lowering entirely. Each task currently
starts a Windows thread (`CreateThread`), so the runtime targets Windows x64.
For-loop initializers and steps, assignments to existing variables, and calls
inside larger expressions still run on the caller.

### The cost model

A task must pay for its thread, so the scheduler estimates how many simple
operations a call performs and spawns only at 1000 or more. A loop runs for its
literal bounds when it has them, or else for the element count of the
collection it visits: a fixed array's length, or a [size hint](#size-hints),
which follows reference arguments into the callee. A loop with neither is
assumed to run 1000 times, and recursion is assumed to be large. The estimate
only ever changes how fast a program runs, never what it does.

`Tests/tasks.cmake` compiles each scheduled program with and without
`--sequential`, links both at `-O0` and `-O2`, and requires identical output
with one worker, four workers, and the default. It also checks that a trapping
task stops the program before later output.

## Ordered blocks

A statement `${ ... }` runs its body in order and atomically with respect to
every other `${ }` block touching the same resources:

```c++
int Withdraw(int amount)
{
    ${
        if Cash < amount
            return 0
        Cash -= amount
        return amount
    }
}
```

Its lock set is what its body and all of its callees read and write, computed
from the same [summaries](#worker-tasks) that schedule tasks. Before the first
statement runs, earlier conflicting tasks are joined, then the locks are taken.
Nothing inside a block is spawned, and a task started by one of its callees
runs inline, so a thread holding locks never waits for other work. Every exit
releases them: reaching the end, `return`, `break`, `continue`, and an `@end`
unwinding through it.

Each scalar field and static is a lock identified by its address, hashed to one
of 64 stripes that are always acquired in ascending order, so blocks cannot
deadlock on each other. Locks are reentrant through nesting: an enclosing block's set
includes everything its callees lock, so an inner block finds them held. A
block whose only resources are its own locals takes no lock at all.

Some effects cannot be named before the block starts: unknown calls, array
elements (an index is only known while the block runs), and a callee locking
something the block itself created. Such a block takes the scheduler-wide
exclusion instead. Every block holds one reader/writer lock: shared when it
named its resources, exclusive when it could not, so an exclusive block excludes
every other block in the program, and the need for one propagates to the
blocks enclosing it. See [Examples/Concurrency](../Examples/Concurrency).

The locks exist for threads the scheduler cannot see, such as those started by
native code; work the compiler starts itself is already ordered by its
conflicts. Lock state is shared across modules, and blocks work the same under
`--sequential`. Blocks are also allowed in `#` compile-time calls, where they
simply run their body. `Tests/ordered.py` races native threads against
each other to check this exclusion.

## Explicit compile-time calls

Prefix a direct function call with `#` to evaluate it during compilation:

```c++
int Square(int n) { return n * n }
Main() {
    #int size = 7
    Print(#Square(size + 1)) // Emits the constant 64, with no Square call.
}
```

The interpreter runs after all function bodies have been type-checked, before
task scheduling and LLVM lowering. It uses the resolved call targets and the
same typed conversions as lowering. Forward calls, qualified static calls,
mutual recursion, and nested calls are supported when their bodies are in the
current module. Imported signatures and native declarations cannot be evaluated.

Arguments may use numeric/bool literals, type limits, arithmetic, casts,
conditionals, and earlier local `#` constants with evaluable initializers.
Mutable locals and runtime parameters cannot supply compile-time arguments.
An ordinary `#int` declaration still permits a runtime initializer; it becomes
usable as a compile-time argument only when that initializer can be evaluated.

Allowed function effects are scalar local declarations and mutations, value
parameters, returns (including void), direct calls to other allowed functions,
branches, Switch, while/do/counted-for loops, inclusive integer range loops, and
break/continue. Values are fixed-width integers up to 512 bits, float/double,
and bool. The complete transitive body is checked, including untaken branches.
I/O, Exec, native and indirect calls, instance methods, static storage reads or
writes, references, pointers, closures, containers, managed text, enums,
calendar values, big/ubig, and tiny/utiny are rejected by this interpreter.

Arithmetic follows the runtime rules: integers wrap to their declared width;
signed division truncates toward zero; zero divisors and signed MIN/-1 division
or remainder fail. Shifts check the original count before converting it.
Floats round at their declared precision and retain IEEE infinity/NaN behavior;
NaN payloads and signs are unspecified. Float-to-integer casts floor and reject
NaN, infinity, and results outside the destination range. Integer casts truncate
or sign/zero extend exactly as at runtime.

## Conditional compilation

`preprocess` (`Preprocessor.cpp`) runs over one file's tokens right after `lex`
and before name collection. It removes `#define`, evaluates `#if`, and copies
only the active branch, without its braces. An inactive branch is skipped by
brace matching alone, so it publishes no names and need not be valid Duck.

Defines are file-scoped: a file's environment starts empty, so the result does
not depend on the order of input files. Values are `bool`, int64, or string.
Conditions are type-checked, and both sides of `&&` and `||` are always checked
so a misspelled define cannot hide behind a short circuit. `#define` values
never reach the parser, which keeps them out of ordinary expressions.

## Macro expansion

`collectMacros` captures each `macro` at file scope or in a container body as
tokens, across all units, so calls can precede definitions and cross files. A
macro inside a function body is rejected. An unused body is never
validated. The parser then replaces each call with generated tokens and keeps
parsing, so the normal grammar for that position validates the output:

- Statement position: a `{ ... }` block holding one `#T name = argument` binding
  per parameter, then the body.
- Directly in a Switch body: the bindings are parsed onto the Switch
  (`Stmt::alternate`), then the body is spliced so it can open sections. A
  statement-position macro after a label stays a block unless its output opens
  with a label, directly or through the macro it calls first.
- Inside an expression: a `macroValue` node holding the same bindings
  (`Expr::bindings`) and the body as its result. The bindings get their own
  scope and run only when the expression is reached, so short-circuiting and
  loop conditions behave as written. A macro with no parameters is just
  `( body )`, which keeps its result usable as a constant label.

Copied tokens carry `Token::origin`, the expansion that produced them, and
`fail` walks that chain to print a note per expansion. Copied tokens also carry
`Token::mark`, unique per expansion. A local declared by a marked token is
renamed `name$mark`, which source text cannot spell, and the parser's `hygiene`
scope stack resolves later references in order. Argument tokens keep their
original mark, so they still mean what they meant at the call.

Recursion is rejected by walking the origin chain; macros have no conditions,
so a cycle could never end. Nesting is limited to 32 levels and output to 200000
tokens per file. Macros cannot produce declarations yet.

## Test blocks

`Parser.cpp` recognizes `T{` chained to a container and stores its tests in
`Program::tests`, away from the program's functions. Each test is a static
function keyed `Container.$test$Name`, which `$` keeps out of reach of source
code, and carries its stable name in `Function::testName`. An ordinary build
never looks at that list, so its LLVM IR is identical with or without tests.

`duckc --test` calls `enableTests`, which moves the tests into the program and
rejects duplicate names, and passes `tests = true` to `compileModule`. The
module is compiled as a non-entry module that may contain `Main`, and
`TestRuntime.cpp` appends a native `main`. Static initializers still run from
the module's constructor. A dependency's tests are always dropped, because each
module's test build owns its own tests.

An assertion lowers in the parser to ordinary statements: operands bound once
to hidden locals, an `if`, and a body of report calls and `return`. Using
`return` gives a failed test the same joins and scope cleanup as any other exit.
`$T.fail` prints the location and test name and sets `@duck$test.failed`;
`$T.show` prints a labeled operand through the `Print` lowering when its type
is printable. The runner clears the flag before each test and reads it after.

## Markup

Markup is parsed only inside a `this{ }` block or a markup literal, and lowers to
ordinary statements: `Tag* $element()`, a field assignment per attribute, then a
`markupAdd` that moves the element into its parent. A literal is a `macroValue`
whose statements build a hidden markup local and whose result takes it. A
`this{ }` block is a generated method, `$markup`, that returns such a literal, and
the hidden `$children` field is initialized by calling it during construction.

A `markup` value is a queue of element nodes (`MarkupRuntime.cpp`), so moves and
cleanup follow the collection rules. A node holds its element's owner word, the
container's name, cleanup, and type descriptor, and the offset of the element's
own `$children`. The first node of a queue also holds the per-type index.
`x.Label` is a location of owner role, over the word in the node, so reads,
aliases, and method calls use the ordinary tracked-reference paths. The parser
records which tags hold children or literal values, and `collectSymbols` adds
their storage before layouts are computed.

## Named scopes

`ScopeRuntime.cpp` holds the runtime: its comment specifies generations, the
arena, repeated starts, and threads. A marked field or static stores a two-word
slot, typed as `long [2]` so that layout, copying, and cleanup need no special
case; `scopedPlaces` records the uses, and each lowers through `scope$data`.

A marked loop's body is wrapped in a `scopedBody` statement. It binds a marker
in the scope stack whose cleanup is `scope$leaveLoop`, so every exit that
already runs `cleanupScopes` also leaves the iteration exactly once.

Leaving through a callee uses a shared pending-end word rather than unwinding
tables. `@end` sets it when a marked loop of that scope is running, and each call
that may have ended a scope tests it. On a hit the caller exits through that
loop if it is in this function, and otherwise cleans up and returns its result
type's default. `compileModule` computes those calls: functions reaching an
`@end`, calls into other modules, and calls through values. A module with no
`@end` and no imports has no such checks. Constructors and property accessors
are not checked, so an `@end` in one takes effect at the next checked call.

## Switch

`Switch` parses to a `switch` statement holding one `case` statement per
section. The selector is stored in a hidden `$switch` local (`Stmt::initializer`)
so it runs once, and `Stmt::target` reads it back. Stacked labels and `default`
merge into one section at parse time.

Analysis checks each label against the selector's type and rejects non-constant
and duplicate labels, following `#` locals so that macro-supplied labels are
compared too. Every section must end control, so each one starts from the same
entry facts and only breaks and a missing `default` reach the following code.
A Switch where neither happens is recorded in `terminatingLoops`.

Lowering compares labels in source order, using `@duck.text.equal` for `string`
and `name` and `icmp` for everything else. A Switch is a break target but not a
continue target: `LoopFlow::isSwitch` and `Loop::isSwitch` make `continue` pass
through to the enclosing loop.

Each marked call has a budget of 100,000 interpreter/effect-check steps, 32
nested calls, and 64 nested expressions. Wide division charges its bit width
against the step budget. Exhaustion, disallowed effects, unavailable bodies,
nonconstant arguments, and traps are compile errors at the `#` location; no
runtime fallback is emitted. A successful call is replaced by its typed result,
or removed for a void result. Field-default and static-initializer calls use
the same evaluation stage. `Tests/compile_time.py` compares interpreter results
with runtime execution at both optimization levels and with Python integer
arithmetic, and checks diagnostics and budgets.

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
| Evaluate marked calls | Typed bodies and constant arguments to typed results | `CompileTime.cpp` |
| Summarize effects | All analyses to read/write summaries and `${ }` lock sets | `Scheduling.cpp` |
| Schedule tasks | Summaries to task spawns, conflicts, and joins | `Scheduling.cpp` |
| Lower a function | Function + signatures + analysis â†’ LLVM fragment | `Codegen.cpp` |
| Assemble | Fragments in source order â†’ LLVM module | `Pipeline.cpp` |

Semantic analysis checks names, types, references, constants, definite assignment, and control flow
without creating LLVM instructions. All functions are analyzed successfully
before the sequential driver evaluates marked calls and starts lowering. The backend consumes the analysis
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
promote them to registers. A native `main` wrapper calls Duck's `Main`, building
and then freeing the argument array for `Main(string [] args)`.

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
