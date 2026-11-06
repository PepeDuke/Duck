"""Compile a C fixture independently, then call it from separate Duck modules."""
import argparse
import os
from pathlib import Path
import subprocess
import shutil
import tempfile
import unittest

parser = argparse.ArgumentParser()
parser.add_argument("--builder", required=True)
parser.add_argument("--compiler", required=True)
parser.add_argument("--clang", required=True)
options, rest = parser.parse_known_args()

C_SOURCE = r"""
#include <stdint.h>
#include <string.h>
static int token;
int32_t native_add(int32_t a, int32_t b) { return a + b; }
uint32_t native_unsigned(uint32_t value) { return value; }
int64_t native_signed64(int64_t value) { return value; }
uint64_t native_unsigned64(uint64_t value) { return value; }
float native_float(float value) { return value * 2; }
double native_double(double value) { return value * 2; }
void native_update(int64_t *value, double *scale) { *value += 5; *scale += 0.5; }
void native_small(int8_t *a, uint16_t *b) { *a = -8; *b = 65535; }
void *native_handle(void) { return &token; }
void native_clear(void **handle) { *handle = 0; }
int32_t native_check(void *handle) { return handle == &token; }
int32_t native_length(const char *text) { return (int32_t)strlen(text); }
"""
BINDINGS = '''
Native #{
    extern "C" int Add(int a, int b) = "native_add"
    extern "C" uint Unsigned(uint value) = "native_unsigned"
    extern "C" long Signed64(long value) = "native_signed64"
    extern "C" uint64 Unsigned64(uint64 value) = "native_unsigned64"
    extern "C" float Float(float value) = "native_float"
    extern "C" double Double(double value) = "native_double"
    extern "C" Update(long& value, double& scale) = "native_update"
    extern "C" Small(int8& a, uint16& b) = "native_small"
    extern "C" void* Handle() = "native_handle"
    extern "C" Clear(void*& handle) = "native_clear"
    extern "C" int Check(void* handle) = "native_check"
    extern "C" int Length(cstring text) = "native_length"
}
int Sum(int a, int b) { return Native.Add(a, b) }
void* Handle() { return Native.Handle() }
cstring Label() { return "Duck" }
'''
APP = '''
extern "C" int AddAlias(int a, int b) = "native_add"
extern "C" int native_length(cstring text)
void* Identity(void* handle) { return handle }
Clear(void*& handle) { Native.Clear(handle) }
Main() {
    Print(Sum(20, 22))
    Print(AddAlias(1, 2))
    Print(Native.Unsigned(4294967295))
    Print(Native.Signed64(-9223372036854775808))
    Print(Native.Unsigned64(18446744073709551615))
    Print(Native.Float(1.25))
    Print(Native.Double(1.5))
    long count = 7
    double scale = 1.0
    Native.Update(count, scale)
    Print(count)
    Print(scale)
    int8 small = 0
    uint16 wide = 0
    Native.Small(small, wide)
    Print(small)
    Print(wide)
    void* handle = null
    Print(handle == null)
    handle = Identity(Handle())
    Print(handle != null)
    Print(Native.Check(handle))
    void* selected = true ? handle | null
    Print(selected == handle)
    Clear(handle)
    Print(handle == null)
    #cstring label = Label()
    Print(label)
    Print(Native.Length(label))
    Print(native_length("100% Duck"))
}
'''
EXPECTED = "42\n3\n4294967295\n-9223372036854775808\n18446744073709551615\n2.5\n3\n12\n1.5\n-8\n65535\n1\n1\n1\n1\n1\nDuck\n4\n9\n"


class NativeInterop(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="duck native & ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def command(self, *args, success=True):
        result = subprocess.run(args, capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)
        return result

    def test_native_module_abi(self):
        (self.root / "native.c").write_text(C_SOURCE)
        self.command(options.clang, "-c", str(self.root / "native.c"), "-o", str(self.root / "native.o"))
        library = self.root / "platform"
        library.mkdir()
        (library / "bindings.dk").write_text(BINDINGS)
        (library / "platform.dkp").write_text('version = 1\n[project]\nname = "Platform"\nkind = "library"\n'
            '[modules.platform]\nsources = ["bindings.dk"]\nlibraries = ["../native.o"]\n')
        (self.root / "app.dk").write_text(APP)
        manifest = self.root / "app.dkp"
        manifest.write_text('version = 1\nreferences = ["platform/platform.dkp"]\n'
            '[project]\nname = "Interop"\nentry = "app"\n'
            '[modules.app]\nsources = ["app.dk"]\ndepends = ["platform"]\n')
        for profile in ("debug", "release"):
            with self.subTest(profile=profile):
                self.command(options.builder, "build", str(manifest), "--profile", profile,
                             "--compiler", options.compiler, "--clang", options.clang)
                output = self.root / ".duck/build" / profile
                exe = output / ("Interop.exe" if os.name == "nt" else "Interop")
                self.assertEqual(self.command(str(exe)).stdout, EXPECTED)
                ir = (output / "app.ll").read_text()
                self.assertEqual(ir.count("declare i32 @native_add("), 1)
                self.assertIn("call i32 @native_add(", ir)
                self.assertNotIn("define i32 @native_add(", ir)
                self.assertIn("declare i32 @native_add(", (output / "platform.ll").read_text())

    @unittest.skipUnless(os.name == "nt", "Windows platform API example")
    def test_windows_clock(self):
        source = Path(__file__).resolve().parents[1] / "Examples/Projects/NativeClock"
        example = self.root / "NativeClock"
        shutil.copytree(source, example, ignore=shutil.ignore_patterns(".duck"))
        for profile in ("debug", "release"):
            self.command(options.builder, "build", str(example / "NativeClock.dkp"),
                         "--profile", profile, "--compiler", options.compiler, "--clang", options.clang)
            exe = example / ".duck/build" / profile / "NativeClock.exe"
            self.assertEqual(self.command(str(exe)).stdout, "1\n1\n1\n1\n")

    def test_rejections(self):
        cases = [
            ('extern "C++" int F()', 'extern "C"'),
            ('extern "C" int F() {}', "cannot have a body"),
            ('extern "C" int F() = "bad.symbol"', "native symbol"),
            ('extern "C" int F() = "printf"', "reserved native symbol"),
            ('extern "C" int F() = "main"', "reserved native symbol"),
            ('extern "C" int F() = ""', "nonempty"),
            ('extern "C" bool F()', "native return"),
            ('extern "C" byte F()', "native return"),
            ('extern "C" cstring F()', "native return"),
            ('extern "C" F(bool flag)', "native parameter ABI"),
            ('extern "C" F(bool& flag)', "native parameter ABI"),
            ('extern "C" F(byte flag)', "native parameter ABI"),
            ('extern "C" F(cstring& text)', "native parameter ABI"),
            ('extern "C" int A(int x) = "same"\nextern "C" double B(int x) = "same"', "conflicting native"),
            ('extern "C" int A(int x) = "same"\nextern "C" int B(int& x) = "same"', "conflicting native"),
            ('extern "C" int F(int x, int x)', "duplicate variable"),
            ('Main() { void* p = 0 }', "type mismatch"),
            ('Main() { void* p = null; p++ }', "type mismatch"),
            ('Main() { void* p = null; Print(p + 1) }', "type mismatch"),
            ('Main() { void* p = null; Print(p < null) }', "type mismatch"),
            ('Main() { Print(null) }', "cannot print an opaque pointer"),
            ('Main() { void* p; Print(p == null) }', "read before assignment"),
            ('Main() { Print((long)null) }', "casts currently require"),
            ('Main() { cstring s = null }', "type mismatch"),
            ('extern "C" F(long& n)\nMain() { long n; F(n) }', "read before assignment"),
            ('extern "C" F(long& n)\nMain() { #long n = 1; F(n) }', "constant"),
            ('extern "C" F(long& n)\nMain() { F(1) }', "mutable variable"),
            ('extern "C" F(long& n)\nMain() { int n = 1; F(n) }', "type mismatch"),
            ('extern "C" Main()', "Main cannot be a native"),
        ]
        for source, error in cases:
            with self.subTest(source=source):
                if "Main()" not in source:
                    source += "\nMain() {}"
                path = self.root / "bad.dk"
                path.write_text(source)
                ir = self.root / "bad.ll"
                result = self.command(options.compiler, str(path), "-o", str(ir), success=False)
                self.assertIn(error, result.stderr)
                self.assertIn("bad.dk:", result.stderr)
                self.assertFalse(ir.exists())

    def test_imported_native_conflict(self):
        dependency = self.root / "dependency.dk"
        dependency.write_text('extern "C" int First(int x) = "same"')
        app = self.root / "app.dk"
        app.write_text('extern "C" double Second(int x) = "same"\nMain() {}')
        result = self.command(options.compiler, "--dependency", str(dependency), str(app),
                              "-o", str(self.root / "bad.ll"), success=False)
        self.assertIn("conflicting native", result.stderr)


if __name__ == "__main__":
    unittest.main(argv=[__file__, *rest])
