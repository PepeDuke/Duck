"""Build-tool validation and end-to-end module/link tests."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

parser = argparse.ArgumentParser()
parser.add_argument("--builder", required=True)
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
        self.temp = tempfile.TemporaryDirectory(prefix="duck project & ")
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
        result = subprocess.run([options.builder, "build",
                                 str(self.project), "--compiler", options.compiler, "--clang", options.clang, *args],
                                capture_output=True, text=True, timeout=60)
        if success:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout)
        return result

    def test_library_project_reference(self):
        engine = self.root / "separate engine"
        engine.mkdir()
        (engine / "core.dk").write_text("int Add(int a, int b) { return a + b }", encoding="utf-8")
        library = engine / "Engine.dkp"
        library.write_text('version = 1\n[project]\nname = "Engine"\nkind = "library"\n'
                           '[modules.core]\nsources = ["core.dk"]\n', encoding="utf-8")
        self.write('version = 1\nreferences = ["separate engine/Engine.dkp"]\n'
                   '[project]\nname = "Demo"\nentry = "app"\n'
                   '[modules.app]\nsources = ["app.dk"]\ndepends = ["core"]\n')
        self.invoke()
        output = self.root / ".duck/build/debug"
        exe = output / ("Demo.exe" if os.name == "nt" else "Demo")
        self.assertEqual(subprocess.check_output([str(exe)], text=True).strip(), "42")
        self.assertTrue((output / "core.o").exists())
        self.assertTrue((output / "app.o").exists())
        self.project = library
        self.invoke()
        self.assertTrue((engine / ".duck/build/debug/core.o").exists())
        self.assertFalse((engine / ".duck/build/debug/Engine.exe").exists())
        library.write_text(library.read_text().replace('version = 1',
                           'version = 1\nreferences = ["Engine.dkp"]'), encoding="utf-8")
        self.assertIn("project reference cycle", self.invoke(success=False).stderr)

    def test_reference_rejects_executable_and_duplicate_modules(self):
        other = self.root / "other.dkp"
        other.write_text(BASE, encoding="utf-8")
        self.write(BASE.replace('version = 1', 'version = 1\nreferences = ["other.dkp"]'))
        self.assertIn("must be a library", self.invoke(success=False).stderr)
        other.write_text(BASE.replace('entry = "app"', 'kind = "library"'), encoding="utf-8")
        self.assertIn("duplicate module", self.invoke(success=False).stderr)

    def test_library_manifest_validation(self):
        self.write(BASE.replace('entry = "app"', 'kind = "unknown"'))
        self.assertIn("project.kind", self.invoke(success=False).stderr)
        self.write(BASE.replace('entry = "app"', 'kind = "library"\nentry = "app"'))
        self.assertIn("cannot have an entry", self.invoke(success=False).stderr)
        self.write(BASE.replace('entry = "app"', 'kind = "library"'))
        self.assertIn("Main", self.invoke(success=False).stderr)

    def test_graph_and_overrides(self):
        self.invoke()
        output = self.root / ".duck/build/debug"
        self.assertTrue((output / "core.o").exists())
        self.assertTrue((output / "app.o").exists())
        self.assertFalse((output / "extra.o").exists())
        self.invoke("--enable", "extra")
        self.assertTrue((output / "extra.o").exists())
        self.invoke("--enable", "extra", "--module", "extra", "--profile", "release")
        self.assertFalse((self.root / ".duck/build/release/app.o").exists())
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
                self.assertIn(error, self.invoke(success=False).stderr)

    def test_bad_overrides(self):
        self.assertIn("both enabled and disabled", self.invoke("--enable", "extra", "--disable", "extra", success=False).stderr)
        self.assertIn("unknown module", self.invoke("--enable", "missing", success=False).stderr)
        self.assertIn("entry module cannot be disabled", self.invoke("--disable", "app", success=False).stderr)

    def test_manifest_validation(self):
        cases = [
            (BASE.replace('version = 1', 'version = 1.0'), "version = 1"),
            (BASE.replace('name = "Demo"', 'name = "CON"'), "reserved filename"),
            (BASE.replace('sources = ["core.dk"]', 'sources = []'), "cannot be empty"),
            (BASE.replace('sources = ["core.dk"]', 'sources = ["core.dk", "core.dk"]'), "duplicates"),
            (BASE.replace('sources = ["core.dk"]', 'sources = [1]'), "string"),
            (BASE.replace('sources = ["core.dk"]', 'sources = ["demo.dkp"]'), "not a .dk file"),
            (BASE + '\n[toolchain]\ncompiler = false\n', "nonempty string"),
            (BASE + '\n[link]\nlibraries = ["-bad"]\n', "must not be linker options"),
            (BASE + '\n[link]\nlibrary_paths = ["missing"]\n', "library directory not found"),
            (BASE + '\n[link]\nlibraries = ["x", "x"]\n', "duplicates"),
            (BASE + '\n[link]\nunknown = []\n', "unknown keys"),
            (BASE + '\n[link\n', "Error"),
        ]
        for text, error in cases:
            with self.subTest(error=error):
                self.write(text)
                self.assertIn(error, self.invoke(success=False).stderr)

    def test_recursive_globs_and_toml(self):
        # Exercise actual TOML syntax, zero-depth **, nested files, character
        # classes, overlapping patterns, and exclusion of hidden directories.
        directory = self.root / "src"
        (directory / "nested").mkdir(parents=True)
        (directory / ".hidden").mkdir()
        (self.root / "core.dk").rename(directory / "core.dk")
        (directory / "nested/helper.dk").write_text("int Helper() { return 1 }", encoding="utf-8")
        (directory / ".hidden/bad.dk").write_text("invalid code", encoding="utf-8")
        self.write(BASE.replace('version = 1', 'version = 0x1').replace(
            'sources = ["core.dk"]', "sources = [\n  'src/**/*.dk', # recursive\n  'src/[c]ore.?k',\n]"))
        self.invoke()
        ir = (self.root / ".duck/build/debug/core.ll").read_text()
        self.assertIn("define i32 @duck.Helper", ir)
        self.assertEqual(ir.count("define i32 @duck.Add"), 1)

    def test_disabled_sources_are_not_required(self):
        (self.root / "extra.dk").unlink()
        self.invoke("--module", "core")
        self.assertIn("matches no files", self.invoke("--enable", "extra", success=False).stderr)

    def test_invalid_cli(self):
        for args, error in [(("--module", "missing"), "unknown module"),
                            (("--profile", "fast"), "debug or release"),
                            (("--unknown",), "unknown option"),
                            (("--module",), "missing value")]:
            with self.subTest(args=args):
                self.assertIn(error, self.invoke(*args, success=False).stderr)

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
            "Main() { n : 4\nBump(n)\nPrint(n)\nPrint(Twice(1.25)) }", encoding="utf-8")
        self.invoke("--profile", "release")
        exe = self.root / ".duck/build/release" / ("Demo.exe" if os.name == "nt" else "Demo")
        self.assertEqual(subprocess.check_output([str(exe)], text=True).splitlines(), ["5", "2.5"])

    def test_static_container_interface(self):
        (self.root / "core.dk").write_text(
            "Calc #{ int Add(int a, int b) { return a + b } }", encoding="utf-8")
        (self.root / "app.dk").write_text(
            "Main() { Print(Calc.Add(20, 22)) }", encoding="utf-8")
        self.invoke()
        output = self.root / ".duck/build/debug"
        self.assertIn("declare i32 @duck.Calc.Add", (output / "app.ll").read_text())
        exe = output / ("Demo.exe" if os.name == "nt" else "Demo")
        self.assertEqual(subprocess.check_output([str(exe)], text=True).strip(), "42")

    def test_static_field_storage_is_shared(self):
        (self.root / "core.dk").write_text(
            "Stats #{\n int Hits = 40\n Hit() { Hits++ }\n}", encoding="utf-8")
        (self.root / "app.dk").write_text(
            "Main() { Stats.Hit()\nStats.Hits += 1\nPrint(Stats.Hits) }", encoding="utf-8")
        self.invoke()
        output = self.root / ".duck/build/debug"
        self.assertIn("@duck$static.Stats.Hits = global i32 40", (output / "core.ll").read_text())
        self.assertIn("@duck$static.Stats.Hits = external global i32", (output / "app.ll").read_text())
        exe = output / ("Demo.exe" if os.name == "nt" else "Demo")
        self.assertEqual(subprocess.check_output([str(exe)], text=True).strip(), "42")

    def test_imported_field_defaults_and_managed_statics(self):
        (self.root / "core.dk").write_text(
            "Badge\n{\n string Label = \"core\"\n int Level = 2\n}\n"
            "Shop #{\n Badge Featured\n name Owner = \"ann\"\n}", encoding="utf-8")
        (self.root / "app.dk").write_text(
            "Main() { Badge b()\nPrint(b.Label)\nPrint(Shop.Featured.Label + \"/\" + (string)Shop.Owner)\n"
            "Shop.Featured.Label = \"app\" + \"!\"\nPrint(Shop.Featured.Level)\nPrint(Shop.Featured.Label) }",
            encoding="utf-8")
        self.invoke()
        exe = self.root / ".duck/build/debug" / ("Demo.exe" if os.name == "nt" else "Demo")
        self.assertEqual(subprocess.check_output([str(exe)], text=True).splitlines(),
                         ["core", "core/ann", "2", "app!"])

    def test_imported_container_extensions(self):
        (self.root / "core.dk").write_text("Badge\n{\n int Level\n}", encoding="utf-8")
        # Static blocks for an imported container keep its layout and are allowed.
        (self.root / "app.dk").write_text(
            "Badge #{ int Twice(int n) { return n * 2 } }\n"
            "Main() { Badge b()\nb.Level = 21\nPrint(Badge.Twice(b.Level)) }", encoding="utf-8")
        self.invoke()
        exe = self.root / ".duck/build/debug" / ("Demo.exe" if os.name == "nt" else "Demo")
        self.assertEqual(subprocess.check_output([str(exe)], text=True).strip(), "42")
        (self.root / "app.dk").write_text("Badge\n{\n int Extra\n}\nMain() {}", encoding="utf-8")
        result = self.invoke(success=False)
        self.assertIn("cannot extend imported container 'Badge'", result.stderr)

    def test_static_initialization_order_across_modules(self):
        # app imports only mid; core's statics still initialize first, before Main.
        self.write(BASE.replace('depends = ["core"]', 'depends = ["mid"]', 1) +
                   '\n[modules.mid]\nsources = ["mid.dk"]\ndepends = ["core"]\n')
        (self.root / "core.dk").write_text(
            "Core #{\n int Base = Start()\n int Start() { Print(\"core\")\nreturn 40 }\n}", encoding="utf-8")
        (self.root / "mid.dk").write_text(
            "Mid #{\n int Value = Core.Base + 1\n}\nint Peek() { return Core.Base }", encoding="utf-8")
        (self.root / "app.dk").write_text(
            "App #{\n int Total = Mid.Value + 1\n}\nMain() { Print(\"main\")\nPrint(App.Total)\nPrint(Peek()) }",
            encoding="utf-8")
        self.invoke()
        exe = self.root / ".duck/build/debug" / ("Demo.exe" if os.name == "nt" else "Demo")
        self.assertEqual(subprocess.check_output([str(exe)], text=True).splitlines(),
                         ["core", "main", "42", "40"])

    def test_long_interface(self):
        (self.root / "core.dk").write_text(
            "Bump(int64& n) { n++ }\nlong Twice(long n) { return n * 2 }", encoding="utf-8")
        (self.root / "app.dk").write_text(
            "Main() { long n = 5000000000\nBump(n)\nPrint(Twice(n)) }", encoding="utf-8")
        self.invoke("--profile", "release")
        output = self.root / ".duck/build/release"
        self.assertIn("declare i64 @duck.Twice(i64)", (output / "app.ll").read_text())
        exe = output / ("Demo.exe" if os.name == "nt" else "Demo")
        self.assertEqual(subprocess.check_output([str(exe)], text=True).strip(), "10000000002")

    def test_unsigned_interface(self):
        (self.root / "core.dk").write_text(
            "Bump(byte& b, uint& n) { b++\nn++ }\n"
            "uint64 Divide(uint64 n) { return n / (uint64)3 }\n"
            "uint16 Small(uint16 n) { return n }\n"
            "int8 Signed(int8 n) { return n }", encoding="utf-8")
        (self.root / "app.dk").write_text(
            "Main() { uint8 b = 255\nuint32 n = 4294967295\nBump(b, n)\n"
            "Print(b)\nPrint(n)\nPrint(Divide(18446744073709551615))\n"
            "Print(Small(65535))\nPrint(Signed(-128)) }", encoding="utf-8")
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

    def test_failed_rebuild_preserves_artifacts(self):
        self.invoke()
        output = self.root / ".duck/build/debug"
        before = {path.name: path.read_bytes() for path in output.iterdir()}
        (self.root / "core.dk").write_text("int Add(int a, int b) { return 99 }", encoding="utf-8")
        (self.root / "bad.o").write_bytes(b"invalid native object")
        self.write(BASE + '\n[link]\nlibraries = ["bad.o"]\n')
        self.invoke(success=False)
        self.assertEqual({path.name: path.read_bytes() for path in output.iterdir()}, before)

    def test_toolchain_configuration_and_cli_precedence(self):
        compiler = os.path.relpath(options.compiler, self.root).replace("\\", "/")
        self.write(BASE + "\n[toolchain]\ncompiler = '" + compiler + "'\nclang = '" + options.clang + "'\n")
        result = subprocess.run([options.builder, "build", str(self.project)],
                                capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.write(BASE + '\n[toolchain]\ncompiler = "missing-compiler"\nclang = "missing-clang"\n')
        self.invoke()

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

    def duck_test(self, *args, status=0):
        result = subprocess.run([options.builder, "test", str(self.project), "--compiler", options.compiler,
                                 "--clang", options.clang, *args], capture_output=True, text=True, timeout=120)
        self.assertEqual(result.returncode, status, result.stdout + result.stderr)
        return result.stdout

    def add_tests(self):
        (self.root / "core.dk").write_text(
            "Core #{ int Add(int a, int b) { return a + b } }\n"
            "T{\n Adds() { T.AreEqual(Add(1, 2), 3) }\n Zero() { T.AreEqual(Add(0, 0), 0) }\n}\n", encoding="utf-8")
        (self.root / "app.dk").write_text(
            'App #{ int Twice(int n) { return Core.Add(n, n) } }\n'
            'T{\n Doubles() { T.AreEqual(Twice(2), 4) }\n}\n'
            'Main() { Print("program ran") }\n', encoding="utf-8")

    def test_duck_test_builds_and_runs_each_module_separately(self):
        self.add_tests()
        output = self.duck_test()
        self.assertIn("== module core\n", output)
        self.assertIn("PASS Core.Adds\n", output)
        self.assertIn("== module app\n", output)
        self.assertIn("PASS App.Doubles\n", output)
        self.assertNotIn("program ran", output)
        tests = self.root / ".duck/build/debug/tests"
        suffix = ".exe" if os.name == "nt" else ""
        self.assertTrue((tests / f"Demo.core.test{suffix}").exists())
        self.assertTrue((tests / f"Demo.app.test{suffix}").exists())
        # Testing leaves the program's own output alone.
        self.assertFalse((self.root / f".duck/build/debug/Demo{suffix}").exists())

    def test_program_build_is_independent_of_tests(self):
        self.add_tests()
        self.invoke()
        exe = self.root / ".duck/build/debug" / ("Demo.exe" if os.name == "nt" else "Demo")
        before = exe.read_bytes()
        self.duck_test()
        self.assertEqual(exe.read_bytes(), before)
        self.assertEqual(subprocess.check_output([str(exe)], text=True).strip(), "program ran")
        self.assertFalse((self.root / ".duck/build/debug/core.test.o").exists())

    def test_duck_test_selection(self):
        self.add_tests()
        output = self.duck_test("--module", "core")
        self.assertIn("PASS Core.Adds\n", output)
        self.assertNotIn("App.Doubles", output)
        output = self.duck_test("--filter", "Doubles")
        self.assertIn("PASS App.Doubles\n", output)
        self.assertNotIn("PASS Core", output)
        listed = self.duck_test("--list")
        self.assertIn("Core.Adds\nCore.Zero\n", listed)
        self.assertNotIn("RUN ", listed)
        self.assertIn("no test matches", self.run_failing("--filter", "Missing"))

    def run_failing(self, *args):
        result = subprocess.run([options.builder, "test", str(self.project), "--compiler", options.compiler,
                                 "--clang", options.clang, *args], capture_output=True, text=True, timeout=120)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        return result.stdout + result.stderr

    def test_duck_test_reports_failures(self):
        self.add_tests()
        (self.root / "app.dk").write_text(
            'App #{ Noop() {} }\nT{\n Bad() { T.Fail("nope") }\n}\nMain() { }\n', encoding="utf-8")
        output = self.run_failing()
        self.assertIn(f'{self.root / "app.dk"}:3:', output.replace("/", os.sep))
        self.assertIn("test App.Bad: T.Fail", output)
        self.assertIn("PASS Core.Adds\n", output)
        self.assertIn("1 module(s) failed, 1 passed", output)

    def test_duck_test_without_tests(self):
        self.assertIn("no tests found", self.duck_test())

    def test_duck_test_on_a_library_project(self):
        self.add_tests()
        self.write('version = 1\n[project]\nname = "Lib"\nkind = "library"\n[modules.core]\nsources = ["core.dk"]\n')
        self.assertIn("PASS Core.Adds\n", self.duck_test())

    SCHEDULED = (
        'extern "C" void* getenv(cstring key)\n'
        'App #{ int Slow(int n) { int t = 0\nfor i : 0...n { t += i }\nreturn t } }\n'
        'T{\n Sums() {\n  a : Slow(100)\n  b : Slow(10)\n  T.AreEqual(a + b, 5105)\n CHECK }\n}\n'
        'Main() { }\n')

    def scheduled_project(self, check=""):
        (self.root / "app.dk").write_text(self.SCHEDULED.replace("CHECK", check), encoding="utf-8")

    def test_compare_schedules_reports_every_candidate(self):
        self.scheduled_project()
        output = self.duck_test("--compare-schedules", "--samples", "3")
        self.assertIn("dependency analysis accepted", output)
        self.assertIn("== module app: 3 sample(s) per schedule", output)
        for name in ("sequential: median", "workers=2: median", "workers=4: median", "workers=auto: median"):
            self.assertIn(name, output)
        self.assertIn("preferred: ", output)
        self.assertNotIn("DISCARDED", output)

    def test_compare_schedules_discards_failing_candidates(self):
        self.scheduled_project('T.IsTrue(getenv("DUCK_WORKERS") == null)\n')
        output = self.duck_test("--compare-schedules", "--samples", "2")
        self.assertIn("workers=2: DISCARDED (test failed: App.Sums)", output)
        self.assertIn("workers=4: DISCARDED", output)
        self.assertIn("sequential: median", output)
        self.assertIn("workers=auto: median", output)

    def test_compare_schedules_requires_a_passing_baseline(self):
        self.scheduled_project("T.Fail()\n")
        self.assertIn("sequential baseline did not pass", self.run_failing("--compare-schedules", "--samples", "1"))

    def test_compare_schedules_without_tasks_has_nothing_to_compare(self):
        self.add_tests()
        self.assertIn("scheduled no tasks", self.duck_test("--compare-schedules", "--samples", "1"))

    def test_compare_schedules_filter_and_validation(self):
        self.scheduled_project()
        self.assertIn("no test matches", self.run_failing("--compare-schedules", "--samples", "1", "--filter", "Missing"))
        self.assertIn("positive whole number", self.run_failing("--compare-schedules", "--samples", "0"))
        self.assertIn("cannot be combined", self.run_failing("--compare-schedules", "--list"))

    def test_filter_is_only_for_duck_test(self):
        result = self.invoke("--filter", "x", success=False)
        self.assertIn("unknown option: --filter", result.stderr)

    def test_link_native_object(self):
        source = self.root / "native.c"
        source.write_text("int duck_native_marker(void) { return 7; }", encoding="utf-8")
        native = self.root / "native.o"
        subprocess.run([options.clang, "-c", str(source), "-o", str(native)], check=True, capture_output=True)
        self.write(BASE + '\n[link]\nlibraries = ["native.o"]\nlibrary_paths = ["."]\n')
        self.invoke()


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0], *rest])
