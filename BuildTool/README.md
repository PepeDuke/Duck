# Duck project build tool

The build tool owns `.dkp` project files, source discovery, module selection,
dependency ordering, object compilation, and linking. The compiler does not
read project files. The tool requires Python 3.11+, `duckc`, and Clang; it has
no third-party Python dependencies. Commands run as argument arrays without a shell.

From the repository root on Windows:

```powershell
cmake -S . -B build
cmake --build build --config Debug
.\duck.cmd build Examples\Projects\Modular\Modular.dkp
.\Examples\Projects\Modular\.duck\build\debug\ModularDemo.exe
```

The executable prints `42`. The portable invocation is
`python BuildTool/duck.py build path/to/project.dkp`. This implementation has
been exercised on Windows; compiler/platform support remains the bootstrap's.

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
Without settings the tool tries PATH, this repository's compiler build folders,
and the usual Windows LLVM installation.

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

Each module produces `<module>.ll` and `<module>.o`. Linking uses the selected
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
Interfaces are currently derived from dependency sources: standalone binary
interface files and distributing libraries without sources are future work.

`libraries` accepts linker names (`example` becomes `-lexample`) or explicit
`.lib`, `.a`, `.so`, `.dylib`, `.o`, or `.obj` paths. `library_paths` supplies
search directories. Objects precede module libraries in dependent-first order;
project-wide libraries come last. Explicit files/directories are checked before
building. Native declarations and calling conventions are separate compiler
features: linking a native library does not yet expose its APIs to Duck code.

This is build-time modularity. Enabling a module includes code but does not
automatically register or execute it. Runtime plugin discovery, stable ABI,
lifecycle hooks, and dynamic unloading need a separate design. Parallel jobs,
incremental caching, package downloads, and distribution commands are not yet
implemented; each invocation recompiles its selected modules.

## Tests

`Tests/build_tool.py` verifies graph validation, source ownership, selection,
direct dependency enforcement, separate object builds, native linking, paths
with spaces, and exclusion of stale disabled objects. CMake registers it when
Python 3.11+ is available. Run the full suite with:

```powershell
ctest --test-dir build -C Debug --output-on-failure
```
