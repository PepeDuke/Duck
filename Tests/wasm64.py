"""Exercise a modular Memory64 application with an activated Emscripten SDK."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def run(*args):
    result = subprocess.run(args, capture_output=True, encoding="utf-8", errors="replace")
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    return result.stdout


def check(builder, compiler, emcc, node, wasm_dis, root):
    project = root / "Demo.dkp"
    project.write_text('''version = 1
[project]
name = "Demo"
entry = "app"
[modules.core]
sources = ["core.dk"]
[modules.app]
sources = ["app.dk"]
depends = ["core"]
''')
    (root / "core.dk").write_text("int Twice(int value) { return value * 2 }")
    (root / "app.dk").write_text('''Item {
    ~Item() { Print("released") }
}
Main(string [] args) {
    Print(Twice(21))
    Print(args[0])
    Item* owner()
    alias : owner
    owner = null
    Print(alias == null)
    int left = 0
    int right = 0
    P{ left++ }{ right++ }
    Print(left + right)
    Print(P.ThreadCount)
}
''', encoding="utf-8")
    command = (builder, "build", project, "--compiler", compiler, "--target", "wasm64", "--emcc", emcc)
    run(*command)
    directory = root / ".duck/build/wasm64/debug"
    loader = directory / "Demo.js"
    assert (directory / "Demo.html").is_file()
    # Check the final module's linear-memory address type, independently of
    # the target triple in the IR and whether its addresses happen to fit u32.
    # wasm-dis prints the address type on the module's memory declaration.
    wasm = directory / "Demo.wasm"
    assert any(
        line.lstrip().startswith("(memory ") and " i64 " in line
        for line in run(wasm_dis, wasm, "-o", "-").splitlines()
    )
    assert run(node, loader, "Kacsa 🦆").splitlines() == ["42", "Kacsa 🦆", "released", "1", "2", "1"]
    assert "0 steps" in run(*command)
    loader.unlink()
    assert "1 steps" in run(*command)
    assert loader.is_file()

    # Sequential execution still rejects conflicting explicit branches.
    (root / "app.dk").write_text("Main() { int x = 1\n P{ x++ }{ int y = x } }")
    rejected = subprocess.run(command, capture_output=True, text=True)
    assert rejected.returncode and "conflicting accesses" in rejected.stderr, rejected.stderr


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    for option in ("builder", "compiler", "emcc", "node", "wasm-dis"):
        parser.add_argument("--" + option, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as temporary:
        check(args.builder, args.compiler, args.emcc, args.node, args.wasm_dis, Path(temporary))
