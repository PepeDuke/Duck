"""Build-tool validation and end-to-end module/link tests."""
import argparse
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("duck_builder", ROOT / "BuildTool/duck.py")
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)
parser = argparse.ArgumentParser()
parser.add_argument("--compiler", required=True)
parser.add_argument("--clang", required=True)
options, rest = parser.parse_known_args()

BASE = '''version = 1
[project]
name = "Demo"
entry = "app"
[modules.core]
sources = ["core.dk"]
[modules.app]
sources = ["app.dk"]
depends = ["core"]
[modules.extra]
sources = ["extra.dk"]
depends = ["core"]
enabled = false
'''


class Projects(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="duck project ")
        self.root = Path(self.temp.name)
        self.project = self.root / "demo.dkp"
        self.project.write_text(BASE, encoding="utf-8")
        (self.root / "core.dk").write_text("int Add(int a, int b) { return a + b }", encoding="utf-8")
        (self.root / "app.dk").write_text("Main() { Print(Add(20, 22)) }", encoding="utf-8")
        (self.root / "extra.dk").write_text("int Extra() { return Add(1, 2) }", encoding="utf-8")
        self.addCleanup(self.temp.cleanup)

    def write(self, text):
        self.project.write_text(text, encoding="utf-8")

    def invoke(self, *args, success=True):
        result = subprocess.run([sys.executable, str(ROOT / "BuildTool/duck.py"), "build",
                                 str(self.project), "--compiler", options.compiler, "--clang", options.clang, *args],
                                capture_output=True, text=True, timeout=60)
        if success:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout)
        return result

    def test_graph_and_overrides(self):
        self.assertEqual(list(builder.Project(self.project).plan()), ["core", "app"])
        self.assertEqual(list(builder.Project(self.project, enable=["extra"]).plan()), ["core", "app", "extra"])
        self.assertEqual(list(builder.Project(self.project, enable=["extra"]).plan("extra")), ["core", "extra"])
        self.assertEqual(self.project.read_text(), BASE)

    def test_invalid_graphs(self):
        cases = [
            (BASE.replace('depends = ["core"]', 'depends = ["missing"]', 1), "missing module"),
            (BASE.replace('[modules.core]', '[modules.core]\ndepends = ["extra"]'), "cycle"),
            (BASE.replace('[modules.core]', '[modules.core]\nenabled = false'), "disabled"),
            (BASE.replace('[modules.core]', '[modules.core]\ndepends = ["app"]'), "entry module"),
            (BASE.replace('sources = ["core.dk"]', 'sources = ["absent*.dk"]'), "matches no files"),
            (BASE.replace('sources = ["app.dk"]', 'sources = ["core.dk"]'), "belongs to both"),
            (BASE.replace('enabled = false', 'enabled = "false"'), "must be true or false"),
            (BASE.replace('entry = "app"', 'entry = "unknown"'), "does not exist"),
            (BASE.replace('version = 1', 'version = true'), "version = 1"),
            (BASE.replace('[modules.core]', '[modules.core]\ndepnds = []'), "unknown keys"),
        ]
        for text, error in cases:
            with self.subTest(error=error):
                self.write(text)
                with self.assertRaisesRegex(builder.BuildError, error):
                    builder.Project(self.project).plan()

    def test_bad_overrides(self):
        with self.assertRaisesRegex(builder.BuildError, "both enabled and disabled"):
            builder.Project(self.project, enable=["extra"], disable=["extra"])
        with self.assertRaisesRegex(builder.BuildError, "unknown module"):
            builder.Project(self.project, enable=["missing"])
        with self.assertRaisesRegex(builder.BuildError, "entry module cannot be disabled"):
            builder.Project(self.project, disable=["app"]).plan()

    def test_separate_objects_and_optional_module(self):
        self.invoke("--enable", "extra")
        output = self.root / ".duck/build/debug"
        for module in ("core", "app", "extra"):
            self.assertTrue((output / (module + ".o")).is_file())
        app_ir = (output / "app.ll").read_text()
        self.assertIn("declare i32 @duck.Add", app_ir)
        self.assertNotIn("define i32 @duck.Add", app_ir)
        self.assertNotIn("@main", (output / "core.ll").read_text())
        exe = output / ("Demo.exe" if os.name == "nt" else "Demo")
        self.assertEqual(subprocess.check_output([str(exe)], text=True).strip(), "42")
        # A stale optional object remains on disk but must not participate in linking.
        (output / "extra.o").write_bytes(b"not an object file")
        self.invoke("--disable", "extra")
        self.assertEqual(subprocess.check_output([str(exe)], text=True).strip(), "42")

    def test_individual_module(self):
        self.invoke("--module", "extra", "--enable", "extra", "--profile", "release")
        output = self.root / ".duck/build/release"
        self.assertTrue((output / "core.o").is_file())
        self.assertTrue((output / "extra.o").is_file())
        self.assertFalse((output / "app.o").exists())
        self.assertFalse((output / "Demo.exe").exists())

    def test_undeclared_dependency(self):
        self.write(BASE.replace('depends = ["core"]', 'depends = []', 1))
        result = self.invoke(success=False)
        self.assertIn("unknown function 'Add'", result.stderr)

    def test_module_reference_and_float_abi(self):
        (self.root / "core.dk").write_text(
            "Bump(int& n) { n++ }\ndouble Twice(double value) { return value * 2 }",
            encoding="utf-8")
        (self.root / "app.dk").write_text(
            "Main() { n : 4; Bump(n); Print(n); Print(Twice(1.25)) }", encoding="utf-8")
        self.invoke("--profile", "release")
        exe = self.root / ".duck/build/release" / ("Demo.exe" if os.name == "nt" else "Demo")
        self.assertEqual(subprocess.check_output([str(exe)], text=True).splitlines(), ["5", "2.5"])

    def test_static_container_interface(self):
        (self.root / "core.dk").write_text(
            "Math #{ int Add(int a, int b) { return a + b } }", encoding="utf-8")
        (self.root / "app.dk").write_text(
            "Main() { Print(Math.Add(20, 22)) }", encoding="utf-8")
        self.invoke()
        output = self.root / ".duck/build/debug"
        self.assertIn("declare i32 @duck.Math.Add", (output / "app.ll").read_text())
        exe = output / ("Demo.exe" if os.name == "nt" else "Demo")
        self.assertEqual(subprocess.check_output([str(exe)], text=True).strip(), "42")

    def test_long_interface(self):
        (self.root / "core.dk").write_text(
            "Bump(int64& n) { n++ }\nlong Twice(long n) { return n * 2 }", encoding="utf-8")
        (self.root / "app.dk").write_text(
            "Main() { long n = 5000000000; Bump(n); Print(Twice(n)) }", encoding="utf-8")
        self.invoke("--profile", "release")
        output = self.root / ".duck/build/release"
        self.assertIn("declare i64 @duck.Twice(i64)", (output / "app.ll").read_text())
        exe = output / ("Demo.exe" if os.name == "nt" else "Demo")
        self.assertEqual(subprocess.check_output([str(exe)], text=True).strip(), "10000000002")

    def test_unsigned_interface(self):
        (self.root / "core.dk").write_text(
            "Bump(byte& b, uint& n) { b++; n++ }\n"
            "uint64 Divide(uint64 n) { return n / (uint64)3 }\n"
            "uint16 Small(uint16 n) { return n }\n"
            "int8 Signed(int8 n) { return n }", encoding="utf-8")
        (self.root / "app.dk").write_text(
            "Main() { uint8 b = 255; uint32 n = 4294967295; Bump(b, n); "
            "Print(b); Print(n); Print(Divide(18446744073709551615)); "
            "Print(Small(65535)); Print(Signed(-128)) }", encoding="utf-8")
        for profile in ("debug", "release"):
            with self.subTest(profile=profile):
                self.invoke("--profile", profile)
                output = self.root / ".duck/build" / profile
                self.assertIn("declare i64 @duck.Divide(i64)", (output / "app.ll").read_text())
                exe = output / ("Demo.exe" if os.name == "nt" else "Demo")
                self.assertEqual(subprocess.check_output([str(exe)], text=True).splitlines(),
                                 ["0", "0", "6148914691236517205", "65535", "-128"])

    def test_transitive_dependency_is_not_implicitly_imported(self):
        self.write(BASE.replace('depends = ["core"]', 'depends = ["extra"]', 1).replace('enabled = false', 'enabled = true'))
        result = self.invoke(success=False)
        self.assertIn("unknown function 'Add'", result.stderr)

    def test_library_cannot_define_entry(self):
        (self.root / "core.dk").write_text("Main() {}", encoding="utf-8")
        result = self.invoke("--module", "core", success=False)
        self.assertIn("library module cannot define Main", result.stderr)

    def test_failure_does_not_publish(self):
        (self.root / "app.dk").write_text("Main() { Missing() }", encoding="utf-8")
        self.invoke(success=False)
        self.assertFalse((self.root / ".duck/build/debug/core.o").exists())

    def test_missing_library(self):
        self.write(BASE + '\n[link]\nlibraries = ["missing.lib"]\n')
        result = self.invoke(success=False)
        self.assertIn("library file not found", result.stderr)

    def test_invalid_float_casts_trap(self):
        # Suppress Windows crash dialogs for deliberate LLVM traps in child processes.
        previous = None
        if os.name == "nt":
            import ctypes
            previous = ctypes.windll.kernel32.SetErrorMode(0x0001 | 0x0002)
        try:
            for cast in ("(int)(0.0 / 0.0)", "(long)(1.0 / 0.0)",
                         "(int)2147483648.0", "(long)9223372036854775808.0",
                         "(int)Exact(-2147483648.5)"):
                with self.subTest(cast=cast):
                    (self.root / "app.dk").write_text("double Exact(double n) { return n }\nMain() { Print(" + cast + ") }", encoding="utf-8")
                    self.invoke("--profile", "release")
                    exe = self.root / ".duck/build/release" / ("Demo.exe" if os.name == "nt" else "Demo")
                    result = subprocess.run([str(exe)], capture_output=True, timeout=10)
                    self.assertNotEqual(result.returncode, 0)
        finally:
            if previous is not None:
                ctypes.windll.kernel32.SetErrorMode(previous)

    def test_link_native_object(self):
        source = self.root / "native.c"
        source.write_text("int duck_native_marker(void) { return 7; }", encoding="utf-8")
        native = self.root / "native.o"
        subprocess.run([options.clang, "-c", str(source), "-o", str(native)], check=True, capture_output=True)
        self.write(BASE + '\n[link]\nlibraries = ["native.o"]\nlibrary_paths = ["."]\n')
        self.invoke()


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0], *rest])
