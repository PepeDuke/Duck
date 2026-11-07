"""Custom byte-width integer (iN/uN) integration tests.

Python's unbounded integers are the oracle: results are reduced to the
destination width, and division truncates toward zero like Duck's.
"""
import argparse
from pathlib import Path
import random
import subprocess
import tempfile
import unittest

parser = argparse.ArgumentParser()
parser.add_argument('--compiler', required=True)
parser.add_argument('--clang', required=True)
options, rest = parser.parse_known_args()

# Byte widths covering 8/16/32/64-bit aliases, sub-host, 65..128 (computed at
# 256 bits for division), and wider widths up to the 64-byte maximum.
WIDTHS = [1, 2, 3, 5, 7, 8, 9, 15, 16, 17, 24, 32, 63, 64]


def wrap(value, bits, signed):
    value &= (1 << bits) - 1
    return value - (1 << bits) if signed and value >> (bits - 1) else value


def truncating(a, b):
    q = abs(a) // abs(b)
    return q if (a < 0) == (b < 0) else -q


class WideIntegerTests(unittest.TestCase):
    def setUp(self):
        # A trapped executable can stay locked briefly on Windows after it exits.
        self.temp = tempfile.TemporaryDirectory(prefix='duck wide ', ignore_cleanup_errors=True)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def command(self, *command, success=True):
        result = subprocess.run(command, capture_output=True, timeout=120)
        self.assertEqual(result.returncode == 0, success,
                         (result.stdout + result.stderr).decode('utf-8', errors='replace')[-3000:])
        return result

    def compile(self, source, stem='main'):
        path = self.root / (stem + '.dk')
        path.write_text(source, encoding='utf-8')
        ir = self.root / (stem + '.ll')
        self.command(options.compiler, str(path), '-o', str(ir))
        return ir

    def run_source(self, source, expected):
        ir = self.compile(source)
        for level in ('-O0', '-O2'):
            exe = self.root / 'test.exe'
            self.command(options.clang, level, str(ir), '-o', str(exe))
            actual = self.command(str(exe)).stdout.decode().replace('\r\n', '\n').split('\n')
            wanted = expected.split('\n')
            for line, (got, want) in enumerate(zip(actual, wanted)):
                self.assertEqual(got, want, f'{level} line {line + 1}')
            self.assertEqual(len(actual), len(wanted), level)

    def reject(self, source, message):
        path, ir = self.root / 'bad.dk', self.root / 'bad.ll'
        path.write_text(source, encoding='utf-8')
        ir.unlink(missing_ok=True)
        result = self.command(options.compiler, str(path), '-o', str(ir), success=False)
        stderr = result.stderr.decode('utf-8', errors='replace')
        self.assertIn(message, stderr)
        self.assertRegex(stderr, r':\d+:\d+: error:')
        self.assertFalse(ir.exists())

    def trap(self, source):
        ir = self.compile(source, 'trap')
        self.assertIn('call void @llvm.trap()', ir.read_text())
        for level in ('-O0', '-O2'):
            exe = self.root / 'trap.exe'
            self.command(options.clang, level, str(ir), '-o', str(exe))
            self.command(str(exe), success=False)

    def test_example(self):
        source = (Path(__file__).parent.parent / 'Examples/Bootstrap/17_ByteWidthIntegers.dk').read_text(encoding='utf-8')
        import math
        self.run_source(source, '\n'.join(map(str, [
            -8388608, 16777215, 2**127 - 1, -2**127, math.factorial(50), 2**255,
            2**512 - 1, -1, 1, 1])) + '\n')

    def test_arithmetic_matches_python(self):
        rng = random.Random(64)
        lines, expected = [], []
        for bytes_ in WIDTHS:
            bits = 8 * bytes_
            for signed in (True, False):
                name = ('i' if signed else 'u') + str(bytes_)
                low, high = (-(1 << (bits - 1)), (1 << (bits - 1)) - 1) if signed else (0, (1 << bits) - 1)
                samples = [low, high, 0, 1, -1 if signed else 2] + [rng.randint(low, high) for _ in range(6)]
                for _ in range(8):
                    a, b = rng.choice(samples), rng.choice(samples)
                    shift = rng.randrange(bits)
                    lines.append(f'{{ {name} a = {a}\n{name} b = {b}')
                    results = [a + b, a - b, a * b, a & b, a | b, a ^ b, a << shift, ~a]
                    lines += ['Print(a + b)', 'Print(a - b)', 'Print(a * b)', 'Print(a & b)',
                              'Print(a | b)', 'Print(a ^ b)', f'Print(a << {shift})', 'Print(~a)']
                    expected += [wrap(r, bits, signed) for r in results]
                    lines.append(f'Print(a >> {shift})')
                    expected.append(a >> shift)  # Python >> is arithmetic for negatives.
                    lines += ['Print(a < b)', 'Print(a == b)', 'Print(-a)']
                    expected += [int(a < b), int(a == b), wrap(-a, bits, signed)]
                    if b != 0 and not (signed and a == low and b == -1):
                        q = truncating(a, b)
                        lines += ['Print(a / b)', 'Print(a % b)']
                        expected += [q, a - q * b]
                    lines.append('}')
        self.run_source('Main() {\n' + '\n'.join(lines) + '\n}', '\n'.join(map(str, expected)) + '\n')

    def test_casts_match_python(self):
        rng = random.Random(8)
        lines, expected = [], []
        for _ in range(120):
            source_bytes, target_bytes = rng.choice(WIDTHS), rng.choice(WIDTHS)
            source_signed, target_signed = rng.random() < 0.5, rng.random() < 0.5
            sbits = 8 * source_bytes
            value = wrap(rng.getrandbits(sbits), sbits, source_signed)
            source = ('i' if source_signed else 'u') + str(source_bytes)
            target = ('i' if target_signed else 'u') + str(target_bytes)
            lines.append(f'{{ {source} v = {value}\nPrint(({target})v)\nPrint((double)v) }}')
            expected += [str(wrap(value, 8 * target_bytes, target_signed)), '%g' % float(value)]
        for value in ['-12345.75', '1e30', '-1e30', '0.5', '6.02214076e23']:
            for target in ('i16', 'i9', 'i64', 'u17'):
                number = float(value)
                bits = 8 * int(target[1:])
                floor = int(number // 1)
                fits = -(1 << (bits - 1)) <= floor < (1 << (bits - 1)) if target[0] == 'i' else 0 <= floor < (1 << bits)
                if fits:
                    lines.append(f'{{ double d = {value}\nPrint(({target})d) }}')
                    expected.append(str(floor))
        self.run_source('Main() {\n' + '\n'.join(lines) + '\n}', '\n'.join(expected) + '\n')

    def test_types_and_conversions(self):
        self.run_source('''i16 Twice(i16 n) { return n * 2 }
Bump(u9& n) { n++ }
Main() {
    i4 a = 7
    int b = a
    i2 s = (short)3
    u1 c = (byte)255
    i8 l = 9223372036854775807
    long same = l
    Print(b + s + c)
    Print(same)
    u9 n = 4722366482869645213695
    Bump(n)
    Print(n)
    Print(Twice(85070591730234615865843651857942052864))
    i3 small = -1
    Print(small + 1000000000)
    u3 x = 16777215
    long wide = x
    Print(wide)
    i10 mixed = (u9)1 + (i1)-2
    Print(mixed)
    u9 top = 4722366482869645213695
    i10 promoted = top + (i1)1
    Print(promoted)
    u16 most = 170141183460469231731687303715884105728
    float f = most
    Print(f)
    i32 negative = -5
    Print(negative >> 1)
    u32 shifted = 1
    Print(shifted << (int8)100)
    i5 forty = -549755813888
    Print(forty)
    Print(true ? (i16)1 | (i16)2)
}''', '\n'.join(map(str, [7 + 3 + 255, 2**63 - 1, 0, -2**127, 999999999, 2**24 - 1,
                          -1, 2**72, '1.70141e+38', -3, 2**100, -2**39, 1])) + '\n')

    def test_rejections(self):
        cases = [
            ('i0 x = 1', "integer byte width must be 1 to 64: 'i0'"),
            ('u65 x = 1', "integer byte width must be 1 to 64: 'u65'"),
            ('i100 x = 1', 'integer byte width must be 1 to 64'),
            ('i08 x = 1', 'integer byte width must be 1 to 64'),
            ('int i2 = 3', 'reserved keyword'),
            ('i3 x = 8388608', 'out of range for i3'),
            ('i3 x = -8388609', 'out of range for i3'),
            ('u3 x = 16777216', 'out of range for u3'),
            ('u16 x = -1', 'out of range for u16'),
            ('i16 x = 170141183460469231731687303715884105728', 'out of range for i16'),
            ('u64 x = 0x1' + '0' * 128, 'out of range for u64'),
            ('x : 18446744073709551616', 'needs a wider iN or uN'),
            ('long x = -9223372036854775809', 'out of range'),
            ('int a = 1\ni3 b = a', 'type mismatch'),
            ('i3 a = 1\nint b = a', None),
            ('uint64 a = 1\ni9 b = a + (i9)1', None),
            ('u9 a = 1\nlong b = a', 'type mismatch'),
            ('i9 a = 1\nu9 b = a', 'type mismatch'),
            ('u64 a = 1\nPrint(a + (int8)1)', 'no common integer type'),
            ('u9 a = 1\ni9 b = a + (int8)1', 'type mismatch: expected i9, got i10'),
            ('i9 a = 1\nPrint(a + 1.5)', None),
        ]
        for statement, message in cases:
            with self.subTest(statement=statement):
                if message is None:
                    self.compile('Main() { ' + statement + ' }')
                else:
                    self.reject('Main() { ' + statement + ' }', message)
        self.reject('extern "C" Bad(i16 x)\nMain() {}', 'native parameter ABI')

    def test_runtime_traps(self):
        for source in [
            'Main() { i16 a = 1\ni16 b = 0\nPrint(a / b) }',
            'Main() { u9 a = 1\nu9 b = 0\nPrint(a % b) }',
            'Main() { i64 a = 1\ni64 b = 0\nPrint(a / b) }',
            'Main() { i16 a = -170141183460469231731687303715884105728\ni16 b = -1\nPrint(a / b) }',
            'Main() { i9 a = -2361183241434822606848\ni9 b = -1\nPrint(a % b) }',
            'Main() { i64 a = -1\nint s = 512\nPrint(a << s) }',
            'Main() { u64 a = 1\nint8 s = -1\nPrint(a >> s) }',
            'Main() { i16 a = 1\nPrint(a << 128) }',
            'Main() { u3 a = 1\nPrint(a << 24) }',
            'Main() { double d = 1e40\nPrint((i16)d) }',
            'Main() { double d = -1.0\nPrint((u17)d) }',
            'Main() { double d = 1e300\nPrint((u64)d) }',
        ]:
            with self.subTest(source=source):
                self.trap(source)


if __name__ == '__main__':
    unittest.main(argv=[__file__, *rest])
