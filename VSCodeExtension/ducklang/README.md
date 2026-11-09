# Duck for Visual Studio Code

Language support for Duck source files (`.dk`) and Duck
project files (`.dkp`).

## Features

**Syntax highlighting** follows what the bootstrap compiler accepts:

- Containers, parents, enums (`#@{`), getter and setter blocks (`@{`, `={`),
  static blocks (`#{`), ordered blocks (`${`), and test blocks (`T{`).
- Every built-in type, including `iN`/`uN`, `big`/`tiny`, `vecN`/`matN`/`quat`/
  `rot`, and `queue`/`stack`/`pair` collections with `[N]`, `[]`, `[*]`, `[50*]`,
  and `[-]` array forms.
- Markup tags and attributes inside `this{ }` and markup values.
- Attributes (`[Hidden]`, `[args]`), named scopes (`[@Play]`, `@start`, `@end`),
  lambdas, macros, `#define`/`#if`/`#else`, compile-time calls (`#Foo(5)`), and
  `extern "C"` declarations.
- Duck operators: `:`, `::`, `..`, `..?`, `...`, and `?` guards.
- The built-in `Math` namespace, `T` assertions, reflection queries, and
  `Print`, `Exec`, `IsValid`, `Length`, and `CodeUnitAt`.

**Snippets.** Type a prefix such as `main`, `container`, `inherit`, `static`,
`forr`, `forchain`, `switch`, `lambda`, `enum`, `test`, `markup`, `macro`,
`#if`, `extern`, or `scopedloop`. Project files have `project`, `module`,
`moduleoptional`, `link`, and `toolchain`.

**Editing.** Comment toggling, bracket matching, auto-closing pairs, and
indentation.

**Compiler diagnostics in the Problems panel**, through the `$duck` problem
matcher. It reports errors from `duckc` and `duck`, and failed assertions from
Duck test runs. Add it to a build task in `.vscode/tasks.json`:

```json
{
    "version": "2.0.0",
    "tasks": [
        {
            "label": "Build Duck project",
            "type": "shell",
            "command": "duck",
            "args": ["build", "${workspaceFolder}/Game.dkp"],
            "group": "build",
            "problemMatcher": "$duck"
        },
        {
            "label": "Test Duck project",
            "type": "shell",
            "command": "duck",
            "args": ["test", "${workspaceFolder}/Game.dkp"],
            "group": "test",
            "problemMatcher": "$duck"
        }
    ]
}
```

## Limits of highlighting

Duck declares containers and functions without keywords, and highlighting works
on one line at a time, so a few forms are decided by convention:

- `Name : Parent` with no body on that line is a container only when both names
  start with a capital letter or the parent is a built-in type. Otherwise it is
  highlighted as an inferred variable.
- `Type Name()` is a function when the type is built in or `Name` starts with a
  capital letter. `Example e()` is highlighted as a variable.
- A single word alone on a line is highlighted as a container, because that is
  how a container is declared. This includes a markup value spliced in by name.

The extension does not type-check. Compile errors appear when a task using
`$duck` runs.

## Install from source

From this folder, build a package and install it:

```powershell
npx --yes @vscode/vsce package --allow-missing-repository --skip-license --out "$env:TEMP\ducklang.vsix"
code --install-extension "$env:TEMP\ducklang.vsix"
```

Then reload VS Code. To try changes without installing, open this folder in
VS Code and press `F5`.
