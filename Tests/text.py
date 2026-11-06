"""Text encoding, native ABI, ownership and diagnostic integration tests."""
import argparse
from pathlib import Path
import subprocess
import tempfile
import unittest

parser = argparse.ArgumentParser()
parser.add_argument('--compiler', required=True)
parser.add_argument('--clang', required=True)
options, rest = parser.parse_known_args()

NATIVE = r'''
#include <stdint.h>
#include <stdlib.h>
static int live, peak;
void *text_test_alloc(uint64_t size) {
    void *p = malloc((size_t)size);
    if (p) { ++live; if (live > peak) peak = live; }
    return p;
}
void text_test_free(void *p) { if (p) { --live; if (live < 0) abort(); } free(p); }
int Live(void) { return live; }
int Peak(void) { return peak; }
int CheckWide(const uint16_t *p) {
    return p[0] == 65 && p[1] == 0x17D && p[2] == 0xD83E && p[3] == 0xDD86 && p[4] == 0;
}
int CheckBytes(const unsigned char *p) {
    return p[0] == 65 && p[1] == 0xC5 && p[2] == 0xBD && p[3] == 0xF0 &&
           p[4] == 0x9F && p[5] == 0xA6 && p[6] == 0x86 && p[7] == 0;
}
'''

OWNERSHIP = r'''
extern "C" int Live()
extern "C" int Peak()
string Make() { string part = "A" + "B"; return part + "C" }
string Change(string& target) { target = "changed" + " value"; return "!" }
string First(string first, string second) { return first }
string Early(bool option) {
    for string value = "x" + "y"; Length(value) < 9; value += "z" {
        string inner = value + "!"
        if option { return inner }
        break
    }
    return "fallback"
}
Work() {
    string s = Make()
    Print(s == "ABC")
    s = s
    Print(s == "ABC")
    string copy = s
    s += Change(s)
    Print(s == "ABC!")
    Print(copy == "ABC")
    Print(First(s, Change(s)) == "ABC!")
    Print(s == "changed value")
    Print(Early(true) == "xy!")
    Print(Early(false) == "fallback")
    string later
    if true { later = Make() } else { later = "other" }
    Print(later == "ABC")
    string conditional = false ? Change(s) | Make()
    Print(conditional == "ABC")
    Print(false && Length(Change(s)) > 0)
    Print(s == "changed value")
    int i = 0
    while i < 20 {
        string local = Make()
        i++
        if i < 10 { continue }
        if i == 15 { break }
        local += "!"
    }
    do {
        string local = Make()
        i--
        if i > 12 { continue }
        break
    } while i > 0
    for string value = "a" + "b"; Length(value) < 5; value += "c" {
        string local = Make()
    }
    { string s = Make(); s += "inner" }
    Make()
}
Cycle() {
    string current = "AŽ🦆" + "!"
    for int i = 0; i < 500; i++ {
        name encoded = (name)current
        current = (string)encoded
        string duplicate = current + ""
        if duplicate != current { Print("bad") }
    }
}
Main() { Work(); Print(Live()); Cycle(); Print(Live()); Print(Peak() < 20) }
'''


class TextTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='duck text & ')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def command(self, *command, success=True):
        result = subprocess.run(command, capture_output=True, timeout=60)
        self.assertEqual(result.returncode == 0, success,
                         (result.stdout + result.stderr).decode('utf-8', errors='replace'))
        return result

    def compile(self, source, stem='main', imports=(), library=False):
        path = self.root / (stem + '.dk')
        path.write_text(source, encoding='utf-8')
        ir = self.root / (stem + '.ll')
        flags = ['--library'] if library else []
        for dependency in imports:
            flags += ['--dependency', str(dependency)]
        self.command(options.compiler, *flags, str(path), '-o', str(ir))
        return path, ir

    def run_source(self, source, expected, fixture=False):
        _, ir = self.compile(source)
        objects = []
        if fixture:
            # Track only Duck text allocations, excluding C-runtime internals.
            ir.write_text(ir.read_text().replace('@malloc', '@text_test_alloc').replace('@free', '@text_test_free'))
            native = self.root / 'native.c'
            native.write_text(NATIVE)
            obj = self.root / 'native.o'
            self.command(options.clang, '-c', str(native), '-o', str(obj))
            objects.append(str(obj))
        for level in ('-O0', '-O2'):
            exe = self.root / 'test.exe'
            self.command(options.clang, level, str(ir), *objects, '-o', str(exe))
            actual = self.command(str(exe)).stdout.replace(b'\r\n', b'\n')
            self.assertEqual(actual, expected.encode('utf-8'))

    def test_encodings_and_native_borrows(self):
        self.run_source(r'''
extern "C" int CheckWide(string text)
extern "C" int CheckBytes(name text)
Main() {
    string wide = "AŽ🦆"
    name narrow = "AŽ🦆"
    Print(Length(wide)); Print(Length(narrow))
    Print(CodeUnitAt(wide, 2)); Print(CodeUnitAt(wide, 3)); Print(CodeUnitAt(narrow, 3))
    Print(CheckWide(wide)); Print(CheckBytes(narrow))
    Print(CheckWide("AŽ🦆")); Print(CheckBytes("AŽ🦆"))
    Print(wide); Print(narrow)
    Print((string)narrow == wide); Print((name)wide == narrow)
    Print(wide != "other"); Print("AŽ🦆" == narrow)
    name composed = "A" + "Ž🦆"
    composed += "!"
    Print(composed)
    Print(Length("")); Print(Length((name)""))
    #string escaped = "\u0041\u017D\U0001F986"
    Print(escaped == wide)
    string embedded = "a\0b"
    Print(Length(embedded)); Print(CodeUnitAt(embedded, 1)); Print(embedded)
    Print((string)(name)embedded == embedded)
}
''', '4\n7\n55358\n56710\n240\n1\n1\n1\n1\nAŽ🦆\nAŽ🦆\n1\n1\n1\n1\nAŽ🦆!\n0\n0\n1\n3\n0\na\0b\n1\n', fixture=True)

    def test_contextual_text_literals(self):
        self.run_source('Main() { name n = "a"; name joined = "b" + ("c" + "d"); '
                        'name nested = n + ("e" + "f"); '
                        'one : true ? n | "g"; two : false ? "h" | n; '
                        'Print(joined); Print(nested); Print(one == n); Print(two == n) }',
                        'bcd\naef\n1\n1\n')

    def test_unicode_boundaries(self):
        points = [0, 0x7F, 0x80, 0x7FF, 0x800, 0xD7FF, 0xE000, 0xFFFF, 0x10000, 0x10FFFF]
        literal = ''.join('\\U%08X' % cp for cp in points)
        text = ''.join(chr(cp) for cp in points)
        self.run_source('Main() { string s = "' + literal + '"; name n = (name)s; '
                        'Print(Length(s)); Print(Length(n)); Print((string)n == s); Print(s == (string)(name)s) }',
                        f'{len(text.encode("utf-16-le")) // 2}\n{len(text.encode("utf-8"))}\n1\n1\n')

    def test_ownership(self):
        self.run_source(OWNERSHIP, '1\n' * 10 + '0\n1\n0\n0\n1\n', fixture=True)

    def test_separate_modules(self):
        path, lib = self.compile('string Echo(string text) { return text + "!" }\n'
                                 'name Identity(name text) { return text }', 'library', library=True)
        _, app = self.compile('Main() { string s = Echo("Duck"); name n = "Ž🦆"; '
                              'Print(s); Print(Identity(n)); Print(Echo(s) == "Duck!!") }', imports=[path])
        for level in ('-O0', '-O2'):
            obj, exe = self.root / 'library.o', self.root / 'modules.exe'
            self.command(options.clang, level, '-c', str(lib), '-o', str(obj))
            self.command(options.clang, level, str(app), str(obj), '-o', str(exe))
            self.assertEqual(self.command(str(exe)).stdout.replace(b'\r\n', b'\n'), 'Duck!\nŽ🦆\n1\n'.encode())

    def test_rejections(self):
        cases = [
            ('Main() { string s = "x"; name n = s }', 'type mismatch'),
            ('Main() { name n = "x"; string s = n }', 'type mismatch'),
            ('Main() { string s = "x"; cstring c = s }', 'type mismatch'),
            ('Main() { Print((cstring)"x") }', 'casts currently require'),
            ('Main() { string s; Print(s) }', 'read before assignment'),
            ('Main() { string s = null }', 'type mismatch'),
            ('Main() { Print("x" - "y") }', 'text supports'),
            ('Main() { string s = "x"; s++ }', 'type mismatch'),
            ('Main() { #name n = "x"; n += "y" }', 'constant'),
            ('Main() { Print(Length(3)) }', 'requires string or name'),
            ('Main() { Print(CodeUnitAt("x", true)) }', 'type mismatch'),
            ('extern "C" string Bad()\nMain() {}', 'native return'),
            ('extern "C" name Bad()\nMain() {}', 'native return'),
            ('extern "C" Bad(string& s)\nMain() {}', 'native parameter ABI'),
            ('extern "C" Bad(name& s)\nMain() {}', 'native parameter ABI'),
            ('Set(string& s) {}\nMain() { name n = "x"; Set(n) }', 'type mismatch'),
            ('Main() { Print("\\uD800") }', 'scalar value'),
            ('Main() { Print("\\U00110000") }', 'scalar value'),
            ('Main() { Print("\\uQQQQ") }', 'invalid Unicode escape'),
        ]
        for source, message in cases:
            with self.subTest(source=source):
                path, ir = self.root / 'bad.dk', self.root / 'bad.ll'
                path.write_text(source, encoding='utf-8')
                result = self.command(options.compiler, str(path), '-o', str(ir), success=False)
                self.assertIn(message, result.stderr.decode())
                self.assertFalse(ir.exists())
        for invalid in (b'\xc0\x80', b'\xed\xa0\x80', b'\xf4\x90\x80\x80', b'\xe2\x82', b'\x80'):
            path = self.root / 'invalid.dk'
            path.write_bytes(b'Main() { Print("' + invalid + b'") }')
            self.command(options.compiler, str(path), '-o', str(self.root / 'invalid.ll'), success=False)

    def test_bounds_trap(self):
        for index in (-1, 0, 1):
            literal = '""' if index == 0 else '"a"'
            _, ir = self.compile(f'Main() {{ Print(CodeUnitAt({literal}, {index})) }}')
            for level in ('-O0', '-O2'):
                exe = self.root / 'bounds.exe'
                self.command(options.clang, level, str(ir), '-o', str(exe))
                self.command(str(exe), success=False)


if __name__ == '__main__':
    unittest.main(argv=[__file__, *rest])
