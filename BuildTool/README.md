# Duck project build tool

The build tool owns `.dkp` project files, source discovery, module selection,
dependency ordering, object compilation, and linking. The compiler does not
read project files. The tool is a C++17 executable named `duck`, built alongside
`duckc` by CMake. Building Duck projects requires `duckc` and Clang. Commands run
as argument arrays without a shell. TOML parsing uses vendored toml++ v3.4.0
(`vendor/tomlplusplus`, MIT license); configuring and building needs no downloads.

From the repository root on Windows:

```powershell
cmake -S . -B build
cmake --build build --config Debug
.\duck.cmd build Examples\Projects\Modular\Modular.dkp
.\Examples\Projects\Modular\.duck\build\debug\ModularDemo.exe
```

The executable prints `42`. Invoke the build tool directly with
`build/Debug/duck.exe build path/to/project.dkp` on Windows, or
`build/duck build path/to/project.dkp` with a single-configuration Unix build.
`duck.cmd` searches `build/Debug`, `build/Release`, then `build` for the executable.
This implementation has been exercised on Windows; compiler/platform support
remains the bootstrap's.

## Project format

`.dkp` files contain TOML, with schema version 1:

```toml
version = 1

[project]
name = "Game"
entry = "app"
build_dir = ".duck/build" # optional

[modules.core]
sources = ["src/core/**/*.dk"]

[modules.app]
sources = ["src/app/**/*.dk"]
depends = ["core"]

[modules.editor]
sources = ["src/editor/**/*.dk"]
depends = ["core"]
enabled = false

# Optional tool overrides:
# [toolchain]
# compiler = "duckc"
# clang = "clang"

# Optional native link inputs; also allowed in individual module tables:
# [link]
# library_paths = ["native/lib"]
# libraries = ["example", "native/support.lib"]
```

Paths/globs are relative to the project file. Bare tool names resolve through
PATH; tool paths resolve relative to the project. `--compiler` and `--clang`
override them, with relative paths interpreted from the working directory.
Without settings the tool tries PATH, the build-tool executable's directory,
this repository's compiler build folders, and the usual Windows LLVM installation.

The entry module must define `Main`; other modules cannot. All enabled modules
are built and linked, even if not reachable from the entry module. Dependencies
must exist, be enabled, and form an acyclic graph. Modules cannot depend on the
entry module. Unknown keys, invalid types, duplicate list entries, unmatched
source patterns, and source files assigned to multiple selected modules are
rejected. Files within a module are sorted and deduplicated; only `.dk` source
files are accepted. Disabled modules need not have sources present, but their
declared dependency graph must still be valid.

## Separate modules and selection

```powershell
# Build core and its dependency closure as objects, without a final executable:
.\duck.cmd build Examples\Projects\Modular\Modular.dkp --module core

# Include the optional metrics module:
.\duck.cmd build Examples\Projects\Modular\Modular.dkp --enable metrics

# Build the optional module alone with optimization:
.\duck.cmd build Examples\Projects\Modular\Modular.dkp --module metrics --enable metrics --profile release

# Omit metrics from an executable build:
.\duck.cmd build Examples\Projects\Modular\Modular.dkp --disable metrics
```

`--enable` and `--disable` are repeatable and do not edit the manifest. A module
cannot appear in both. Disabled required dependencies produce an error, rather
than being silently enabled. Disable the dependent module too, or change its
implementation/declarations. The entry module must be enabled for a full build.
`--module` builds only the selected module and dependencies, and never links an
executable, even when selecting the entry module.

Each module produces `<module>.ll` and `<module>.o`, and a library module also
`<module>.dki` (see below). Linking uses the selected
object list, never a wildcard of output files. Disabled artifacts may remain
on disk but are excluded. Outputs live under `<build_dir>/debug` or
`<build_dir>/release`. These profiles select `-O0` and `-O2`; source-level debug
information is not implemented. Temporary artifacts are published only after
the requested compilation/link operations succeed. A failed build returns
nonzero and preserves previous successful artifacts, which are not new results.

## Interfaces and native libraries

Code can call its own module's functions and its **direct** dependencies'
functions. Transitive dependencies are linked but not implicitly imported.
Currently all top-level functions are exported in the existing shared Duck
symbol namespace; names must be unique across linked modules. Static container
groups can qualify functions (for example `Math.Add`), including calls through
module dependencies. Build-module names do not implicitly qualify symbols.
Public/private exports and module-specific visibility remain future work.

The compiler accepts `--library` to omit the entry wrapper and repeated
`--dependency path.dk` options to read signatures without lowering dependency
bodies. The build tool compiles those bodies into their own module objects.

Compiling a library module also writes `<module>.dki`, an interface file:
the module's preprocessed tokens with the function bodies emptied that no
importer reads (it keeps those that return a pointer, take a heap pointer by
value, or contain markup, since importers derive ownership, escape and field
facts from them). It records a key covering the compiler binary, the module's
source paths and text, and its dependencies' keys. When the published
interface's key matches, a dependent compiles with `--import <module>.dki`
instead of the module's sources, which skips lexing them and parsing most of
them; the build prints how many interfaces were up to date. A stale or missing
interface is never waited for: the dependent reads the sources, and the
module's own compile publishes a fresh interface for the next build. Modules
are still always recompiled, and distributing libraries without sources is
future work.

`libraries` accepts linker names (`example` becomes `-lexample`) or explicit
`.lib`, `.a`, `.so`, `.dylib`, `.o`, or `.obj` paths. `library_paths` supplies
search directories. Objects precede module libraries in dependent-first order;
project-wide libraries come last. Explicit files/directories are checked before
building. Native APIs are exposed using the compiler's bodyless `extern "C"` declarations,
optionally with a `= "symbol"` alias. See [native calls](../Compiler/README.md#native-api-calls-and-opaque-handles)
for supported types and ABI limits. Linking alone does not infer declarations.

This is build-time modularity. Enabling a module includes code but does not
automatically register or execute it. Runtime plugin discovery, stable ABI,
lifecycle hooks, and dynamic unloading need a separate design. Incremental
caching, package downloads, and distribution commands are not yet implemented;
each invocation recompiles its selected modules.

`duck build` compiles modules concurrently, up to one per CPU, and links once
they all succeed. Output still appears in build order and stops at the first
failing module, as in a serial build. `duck test` builds modules one at a time.

## Running Duck tests

`duck test` builds and runs the `T{ }` tests of a project without touching the
program's own build output:

```powershell
.\duck.cmd test path\to\project.dkp
.\duck.cmd test path\to\project.dkp --module core
.\duck.cmd test path\to\project.dkp --filter Inventory --filter Combat
.\duck.cmd test path\to\project.dkp --list
```

Each module with tests becomes its own executable,
`<build_dir>/<profile>/tests/<project>.<module>.test[.exe]`: that module's test
build linked with the ordinary objects of its dependencies. Those executables can
also be run directly, with `--list` and name filters. Library projects work the
same way, and the entry module's `Main` is not called. `--filter` selects tests
whose stable name contains the text. `--module`, `--enable`, `--disable`,
`--profile`, `--compiler`, and `--clang` behave as they do for `duck build`.

`--compare-schedules` evaluates scheduling with those same tests. Each module is
built with its tasks and with `--sequential`, and the tests run `--samples` times
(default 5) under the sequential baseline and at `DUCK_WORKERS` 2, 4, and the
default. Samples are interleaved with a rotating start, so slow drift during the
run does not favor one schedule. A schedule with any failing sample is discarded.
The others report the median, minimum, and maximum of the summed test time, and a
schedule counts as faster or slower only when its middle half does not overlap
the baseline's. A module whose code has no tasks is reported and skipped. The
command fails only when the sequential baseline itself fails.

Candidates only use less or more of the parallelism that dependency analysis
already accepted, so the result can reject or prefer a schedule but cannot make
a new one legal: a passing test says nothing about races it did not happen to hit.

The command exits with `0` when every module passed and `1` when any test failed
or crashed, or when filters matched no test in any module.

## Tests

`Tests/build_tool.py` invokes the C++ executable to verify graph validation,
source ownership, selection, direct dependency enforcement, separate object
builds, native linking, paths with spaces and shell metacharacters, recursive
globs, TOML validation, failed rebuilds, and exclusion of stale disabled objects.
CMake registers it when
Python is available. Python is used only by this test harness, not by the build
tool. Run the full suite with:

```powershell
ctest --test-dir build -C Debug --output-on-failure
```


## Library projects and project references

An executable can reference separately maintained library projects:

```toml
version = 1
references = ["../Quaxel-Engine/Quaxel.dkp"]
[project]
name = "Hellfeather"
entry = "game"
[modules.game]
sources = ["Source/**/*.dk"]
depends = ["quaxel_core"]
```

The referenced manifest owns its module definitions:

```toml
version = 1
[project]
name = "Quaxel"
kind = "library"
[modules.quaxel_core]
sources = ["Source/Core/**/*.dk"]
```

`project.kind` defaults to `executable`. Libraries must omit `entry` and compile
all enabled modules to objects without linking an executable. Every library
module is compiled with `--library`, so it cannot define Main. Executable
projects still require an entry module. References may only target libraries.

References resolve relative to their declaring manifest, recursively. Imported
sources and native link paths retain that manifest's base directory. Imported
modules retain their names; the root project's enabled/disabled overrides apply
to the combined module graph. Referenced toolchain/build-directory settings are
used only when building that library directly; the consuming root owns its build.
Root modules must explicitly depend on imported modules to call their functions.
Project reference cycles and duplicate module names are rejected (including a
shared library reached through multiple reference paths). This first version
requires a reference tree without shared dependencies. References do not provide
packages, version resolution, binary interfaces or a module namespace.
