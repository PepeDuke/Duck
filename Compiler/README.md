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

- Top-level functions with explicit `int`, `long`, `float`, `double`, or `bool` parameters and returns,
  or omitted/explicit `void` returns. Forward calls and recursion work.
- Mutable scalar reference parameters (`int&`, `long&`, `float&`, `double&`, `bool&`). Arguments must be
  mutable variables of exactly the declared type. References can be forwarded
  to other functions and multiple parameters can alias the same variable.
  Value parameters still receive copies. Arguments evaluate left to right.
- Initialized local constants (`#int`, `#long`, `#float`, `#double`, `#bool`). Initializers may be runtime
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
- Signed 32-bit `int` and 64-bit `long`, with exact aliases `int32` and `int64`.
  Decimal integer literals infer int when they fit, otherwise long. Signed
  minima (`-2147483648`, `-9223372036854775808`) are supported; values beyond
  signed 64-bit range are rejected. Aliases work with constants and references.
- Integer/boolean literals, parentheses, unary `-`
  and `!`, arithmetic `+ - * / %`, comparisons, and numeric/boolean equality.
- IEEE 754 32-bit `float` and 64-bit `double`, decimal literals such as `1.25`
  and exponent forms such as `1e3` or `1.25e-2`. Decimal points require digits
  on both sides; type suffixes are not yet supported.
- Explicit casts `(type)expression` among numeric and boolean types, including
  aliases. Casts bind with unary precedence and may be nested. Float-to-integer
  casts **floor**, so `(int)-9.99` is `-10`; NaN, infinity, and out-of-range
  floored results trap before conversion. Long-to-int casts retain the low
  32 bits. Numeric-to-bool tests nonzero (NaN is true); bool-to-numeric yields
  0 or 1. Text, object, pointer, and void casts are not implemented.
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
- `Print` with one numeric, boolean, or string literal value. Floating-point
  output uses `%g` (six significant digits). Booleans print
  as `0` or `1`. Strings support `\n`, `\r`, `\t`, `\\`, and `\"` escapes.
  Literal text is passed as data to `printf`, including percent characters.
  Inferred locals can hold and reassign these literal values, including
  conditional selections such as `label : ready ? "yes" | "no"`.
- Line and block comments, newline statement terminators, optional semicolons,
  and termination before `}`. Call arguments and parameter lists may span lines.
- Source-file, line, and column diagnostics. Invalid programs do not write IR.

Integer addition, subtraction, multiplication, and negation wrap at their
respective 32-bit or 64-bit width.
Division truncates toward zero. Division by zero and the signed division
overflow case are currently unchecked; avoid these inputs. String literals
are passed as UTF-8 bytes to the C runtime for this bootstrap, not Duck's
eventual two-byte `string` representation. Inferred text locals hold references
to immutable, program-lifetime literals. Explicit `string` declarations,
string operations, and string parameters/returns are not yet supported.

Numeric conversions widen through `int → long → float → double` (including
direct jumps, such as int to double) for initialization, assignment, value
arguments, and returns.
Mixed arithmetic/comparisons and numeric ternaries use the widest operand type.
Reference arguments still require exact types; an int cannot bind to a long
reference. Integer-to-floating conversion can round values beyond the target's
precision. Implicit narrowing long to int, floating values to integers, or double
to float is rejected; use an explicit cast. Integer
operations determine their width from their operands before assignment:
`long n = 2147483647 + 1` still performs wrapping 32-bit addition, then widens.
Use a long operand when 64-bit arithmetic is intended.

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
other numeric types, full strings, range-based loops,
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

## Implementation and validation

The compiler is a reusable `duck_compiler` library with explicit stage APIs in
`Pipeline.h`. `main.cpp` only handles file I/O and invokes the pipeline.

| Stage | Input → output | Implementation |
| --- | --- | --- |
| Tokenize | Source text → tokens with locations | `Lexer.cpp` |
| Parse | Tokens → syntax tree | `Parser.cpp` |
| Combine units | Per-file syntax trees in input order → program | `Pipeline.cpp` |
| Collect declarations | Syntax tree → function signatures | `Symbols.cpp` |
| Analyze a function | Function + signatures → types, conversions, literal constants | `Semantics.cpp` |
| Lower a function | Function + signatures + analysis → LLVM fragment | `Codegen.cpp` |
| Assemble | Fragments in source order → LLVM module | `Pipeline.cpp` |

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

`Tests/run.cmake` exercises the full frontend → LLVM → linker → executable
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
