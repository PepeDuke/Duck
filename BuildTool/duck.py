#!/usr/bin/env python3
"""Duck project builder. Requires Python 3.11+; no third-party packages."""
from __future__ import annotations

import argparse
import glob
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import tomllib


class BuildError(Exception):
    pass


def table(value, allowed, location):
    if not isinstance(value, dict):
        raise BuildError(f"{location} must be a table")
    unknown = set(value) - set(allowed)
    if unknown:
        raise BuildError(f"{location}: unknown keys: {', '.join(sorted(unknown))}")
    return value


def strings(value, location):
    if not isinstance(value, list) or any(not isinstance(x, str) or not x for x in value):
        raise BuildError(f"{location} must be an array of nonempty strings")
    if len(set(value)) != len(value):
        raise BuildError(f"{location} contains duplicates")
    return value


def name(value, location):
    if not isinstance(value, str) or not re.fullmatch(r"[A-Za-z][A-Za-z0-9_-]*", value):
        raise BuildError(f"{location} must start with a letter and contain only letters, digits, _ or -")
    # Names become filenames on Windows, even when a project is prepared elsewhere.
    if value.upper() in {"CON", "PRN", "AUX", "NUL", *(f"COM{i}" for i in range(1, 10)), *(f"LPT{i}" for i in range(1, 10))}:
        raise BuildError(f"{location} is a reserved filename")
    return value


def path_string(value, location):
    if not isinstance(value, str) or not value:
        raise BuildError(f"{location} must be a nonempty string")
    return value


class Project:
    def __init__(self, path, enable=(), disable=()):
        self.path = Path(path).resolve()
        if self.path.suffix.lower() != ".dkp":
            raise BuildError("project file must have the .dkp extension")
        self.root = self.path.parent
        with self.path.open("rb") as stream:
            data = tomllib.load(stream)
        table(data, {"version", "project", "modules", "toolchain", "link"}, "project file")
        if type(data.get("version")) is not int or data["version"] != 1:
            raise BuildError("project file requires version = 1")
        config = table(data.get("project"), {"name", "entry", "build_dir"}, "project")
        self.name = name(config.get("name"), "project.name")
        self.entry = name(config.get("entry"), "project.entry")
        self.build_dir = (self.root / path_string(config.get("build_dir", ".duck/build"), "project.build_dir")).resolve()
        self.toolchain = table(data.get("toolchain", {}), {"compiler", "clang"}, "toolchain")
        for key, value in self.toolchain.items():
            path_string(value, f"toolchain.{key}")
        self.link = table(data.get("link", {}), {"libraries", "library_paths"}, "link")
        self.modules = data.get("modules")
        if not isinstance(self.modules, dict) or not self.modules:
            raise BuildError("modules must contain at least one module table")
        for module, value in self.modules.items():
            name(module, "module name")
            table(value, {"sources", "depends", "enabled", "libraries", "library_paths"}, f"modules.{module}")
            strings(value.get("sources"), f"{module}.sources")
            if not value["sources"]:
                raise BuildError(f"{module}.sources cannot be empty")
            value["depends"] = strings(value.get("depends", []), f"{module}.depends")
            if type(value.get("enabled", True)) is not bool:
                raise BuildError(f"{module}.enabled must be true or false")
            value["enabled"] = value.get("enabled", True)
        for location, config in [("link", self.link), *self.modules.items()]:
            for key in ("libraries", "library_paths"):
                config[key] = strings(config.get(key, []), f"{location}.{key}")
        if self.entry not in self.modules:
            raise BuildError(f"entry module '{self.entry}' does not exist")
        if set(enable) & set(disable):
            raise BuildError("a module cannot be both enabled and disabled")
        for enabled, names in ((True, enable), (False, disable)):
            for module in names:
                if module not in self.modules:
                    raise BuildError(f"unknown module '{module}'")
                self.modules[module]["enabled"] = enabled
        for module, config in self.modules.items():
            for dependency in config["depends"]:
                if dependency not in self.modules:
                    raise BuildError(f"module '{module}' depends on missing module '{dependency}'")
                if dependency == self.entry:
                    raise BuildError(f"module '{module}' cannot depend on entry module '{dependency}'")
        # Validate cycles even in disabled modules; enabling them must not hide a bad graph.
        self.order = []
        active, done = [], set()

        def visit(module):
            if module in active:
                raise BuildError("module dependency cycle: " + " -> ".join(active + [module]))
            if module in done:
                return
            active.append(module)
            for dependency in self.modules[module]["depends"]:
                visit(dependency)
            active.pop()
            done.add(module)
            self.order.append(module)

        for module in self.modules:
            visit(module)

    def plan(self, target=None):
        if target is not None and target not in self.modules:
            raise BuildError(f"unknown module '{target}'")
        roots = [target] if target else [m for m in self.order if self.modules[m]["enabled"]]
        if target is None and not self.modules[self.entry]["enabled"]:
            raise BuildError("entry module cannot be disabled for an executable build")
        selected = set()

        def select(module):
            if not self.modules[module]["enabled"]:
                raise BuildError(f"required module '{module}' is disabled; enable it or disable its dependents")
            if module in selected:
                return
            selected.add(module)
            for dependency in self.modules[module]["depends"]:
                select(dependency)

        for module in roots:
            select(module)
        sources, owners = {}, {}
        for module in self.order:
            if module not in selected:
                continue
            paths = set()
            for pattern in self.modules[module]["sources"]:
                matches = [Path(p).resolve() for p in glob.glob(str(self.root / pattern), recursive=True) if Path(p).is_file()]
                if not matches:
                    raise BuildError(f"module '{module}': source pattern matches no files: {pattern}")
                for path in matches:
                    if path.suffix.lower() != ".dk":
                        raise BuildError(f"module '{module}': source is not a .dk file: {path}")
                    if path in owners and owners[path] != module:
                        raise BuildError(f"source '{path}' belongs to both '{owners[path]}' and '{module}'")
                    owners[path] = module
                    paths.add(path)
            sources[module] = sorted(paths)
        return sources


def executable(value, root):
    if "/" in value or "\\" in value or Path(value).is_absolute():
        path = (root / value).resolve()
        if path.is_file():
            return str(path)
    else:
        found = shutil.which(value)
        if found:
            return found
    raise BuildError(f"tool not found: {value}")


def tool(project, key, override):
    if override:
        return executable(override, Path.cwd())
    if key in project.toolchain:
        return executable(project.toolchain[key], project.root)
    repo = Path(__file__).resolve().parent.parent
    candidates = (["duckc", str(repo / "build/Debug/duckc.exe"), str(repo / "build/Release/duckc.exe"), str(repo / "build/duckc")]
                  if key == "compiler" else ["clang", "C:/Program Files/LLVM/bin/clang.exe"])
    for candidate in candidates:
        try:
            return executable(candidate, project.root)
        except BuildError:
            pass
    raise BuildError(f"cannot find {key}; set --{key} or toolchain.{key}")


def run(arguments):
    print(subprocess.list2cmdline([str(a) for a in arguments]), flush=True)
    result = subprocess.run([str(a) for a in arguments], check=False)
    if result.returncode:
        raise BuildError(f"tool failed with exit code {result.returncode}: {arguments[0]}")


def link_flags(project, configs):
    flags = []
    for config in configs:
        for directory in config["library_paths"]:
            path = (project.root / directory).resolve()
            if not path.is_dir():
                raise BuildError(f"library directory not found: {path}")
            flags.extend(["-L", str(path)])
        for library in config["libraries"]:
            if "/" in library or "\\" in library or Path(library).suffix.lower() in {".a", ".lib", ".so", ".dylib", ".o", ".obj"}:
                path = (project.root / library).resolve()
                if not path.is_file():
                    raise BuildError(f"library file not found: {path}")
                flags.append(str(path))
            else:
                if library.startswith("-"):
                    raise BuildError("library names must not be linker options")
                flags.append("-l" + library)
    return flags


def build(args):
    project = Project(args.project, args.enable, args.disable)
    sources = project.plan(args.module)
    compiler, clang = tool(project, "compiler", args.compiler), tool(project, "clang", args.clang)
    flags = link_flags(project, [*(project.modules[m] for m in reversed(sources)), project.link]) if not args.module else []
    destination = project.build_dir / args.profile
    destination.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".work-", dir=destination) as temporary:
        work = Path(temporary)
        artifacts, objects = [], []
        for module, files in sources.items():
            ir, obj = work / (module + ".ll"), work / (module + ".o")
            command = [compiler]
            if module != project.entry:
                command.append("--library")
            for dependency in project.modules[module]["depends"]:
                for source in sources[dependency]:
                    command.extend(["--dependency", source])
            run([*command, *files, "-o", ir])
            run([clang, "-c", "-O0" if args.profile == "debug" else "-O2", ir, "-o", obj])
            objects.append(obj)
            artifacts.extend([ir, obj])
        if not args.module:
            output = work / (project.name + (".exe" if os.name == "nt" else ""))
            run([clang, *objects, *flags, "-o", output])
            artifacts.append(output)
        # Publish only after every requested compilation/link operation succeeds.
        for artifact in artifacts:
            os.replace(artifact, destination / artifact.name)
    print(f"Built {'module ' + args.module if args.module else project.name}: {destination}")


def main(argv=None):
    parser = argparse.ArgumentParser(description="Build modular Duck projects (.dkp, TOML syntax)")
    commands = parser.add_subparsers(dest="command", required=True)
    command = commands.add_parser("build")
    command.add_argument("project", type=Path, help="path to a .dkp project")
    command.add_argument("--module", help="build only this module and its dependencies, without linking an executable")
    command.add_argument("--enable", action="append", default=[], metavar="MODULE")
    command.add_argument("--disable", action="append", default=[], metavar="MODULE")
    command.add_argument("--profile", choices=["debug", "release"], default="debug")
    command.add_argument("--compiler")
    command.add_argument("--clang")
    args = parser.parse_args(argv)
    try:
        build(args)
        return 0
    except (BuildError, OSError, tomllib.TOMLDecodeError) as error:
        print(f"duck build: error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
