"""big/ubig and tiny/utiny integration tests.

Oracles: Python integers for big (with truncating division), and Python's
decimal module at 69 digits with ROUND_HALF_EVEN for tiny. Duck's tiny output
is compared as an exact decimal value, independent of formatting.
"""
import argparse
import decimal
from decimal import Decimal
import math
from pathlib import Path
import random
import subprocess
import tempfile
import unittest

parser = argparse.ArgumentParser()
parser.add_argument('--compiler', required=True)
parser.add_argument('--clang', required=True)
options, rest = parser.parse_known_args()

CONTEXT = decimal.Context(prec=69, rounding=decimal.ROUND_HALF_EVEN,
                          Emax=999999999, Emin=-999999999, traps=[])

ALLOCATIONS = r'''
#include <stdint.h>
#include <stdlib.h>
static int live;
void *count_alloc(uint64_t size) { void *p = malloc((size_t)size); if (p) ++live; return p; }
void count_free(void *p) { if (p) { --live; if (live < 0) abort(); } free(p); }
int Live(void) { return live; }
'''


def truncating(a, b):
    q = abs(a) // abs(b)
    return q if (a < 0) == (b < 0) else -q


def tiny_literal(value):
    """A Duck literal for a Decimal, using an exponent so it stays a real literal."""
    sign, digits, exponent = value.as_tuple()
    text = ''.join(map(str, digits)) + '.0e' + str(exponent)
    return ('-' if sign else '') + text


class ArbitraryNumberTests(unittest.TestCase):
    def setUp(self):
        # A trapped executable can stay locked briefly on Windows after it exits.
        self.temp = tempfile.TemporaryDirectory(prefix='duck arbitrary ', ignore_cleanup_errors=True)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def command(self, *command, success=True):
        result = subprocess.run(command, capture_output=True, timeout=180)
        self.assertEqual(result.returncode == 0, success,
                         (result.stdout + result.stderr).decode('utf-8', errors='replace')[-3000:])
        return result

    def compile(self, source, stem='main'):
        path = self.root / (stem + '.dk')
        path.write_text(source, encoding='utf-8')
        ir = self.root / (stem + '.ll')
        self.command(options.compiler, str(path), '-o', str(ir))
        return ir

    def outputs(self, source, fixture=False):
        ir = self.compile(source)
        objects = []
        if fixture:
            ir.write_text(ir.read_text().replace('@malloc', '@count_alloc').replace('@free', '@count_free'))
            native = self.root / 'native.c'
            native.write_text(ALLOCATIONS)
            obj = self.root / 'native.o'
            self.command(options.clang, '-c', str(native), '-o', str(obj))
            objects.append(str(obj))
        results = []
        for level in ('-O0', '-O2'):
            exe = self.root / 'test.exe'
            self.command(options.clang, level, str(ir), *objects, '-o', str(exe))
            results.append(self.command(str(exe)).stdout.decode().replace('\r\n', '\n').split('\n')[:-1])
        return results

    def run_source(self, source, expected, fixture=False):
        for lines in self.outputs(source, fixture):
            self.assertEqual(len(lines), len(expected))
            for index, (got, want) in enumerate(zip(lines, expected)):
                self.assertEqual(got, str(want), f'line {index + 1}')

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
        for level in ('-O0', '-O2'):
            exe = self.root / 'trap.exe'
            self.command(options.clang, level, str(ir), '-o', str(exe))
            self.command(str(exe), success=False)

    def test_example(self):
        source = (Path(__file__).parent.parent / 'Examples/Bootstrap/18_BigAndTiny.dk').read_text(encoding='utf-8')
        self.run_source(source, [math.factorial(52), 2**200, 5, '0.1', '0.3', 1,
                                 '0.333333333333333333333333333333333333333333333333333333333333333333333',
                                 '1e-60'])

    def test_big_matches_python(self):
        rng = random.Random(1024)
        values = [0, 1, -1, 2**32 - 1, 2**32, -2**32, 2**64, -2**64 + 1, 10**40, -(10**40) + 7]
        values += [rng.choice((1, -1)) * rng.getrandbits(rng.randint(1, 600)) for _ in range(30)]
        lines, expected = [], []
        for _ in range(120):
            a, b = rng.choice(values), rng.choice(values)
            lines.append(f'{{ big a = {a}\nbig b = {b}')
            lines += ['Print(a + b)', 'Print(a - b)', 'Print(a * b)', 'Print(-a)',
                      'Print(a < b)', 'Print(a == b)', 'Print(a >= b)']
            expected += [a + b, a - b, a * b, -a, int(a < b), int(a == b), int(a >= b)]
            if b:
                lines += ['Print(a / b)', 'Print(a % b)']
                q = truncating(a, b)
                expected += [q, a - q * b]
            lines.append('}')
        self.run_source('Main() {\n' + '\n'.join(lines) + '\n}', expected)

    def test_big_conversions_and_mixed_types(self):
        self.run_source('''big Factorial(int n) {
    big result = 1
    for int i = 2; i <= n; i++ { result *= i }
    return result
}
Main() {
    Print(Factorial(200) / Factorial(198))
    big x = 9223372036854775807
    x += 1
    Print(x)
    Print((long)x)
    Print((uint64)(x * 2 - 1))
    Print((int8)(big)300)
    long l = -9223372036854775807 - 1
    big fromLong = l
    Print(fromLong)
    uint64 most = 18446744073709551615
    ubig fromUnsigned = most
    Print(fromUnsigned + most)
    ubig u = 7
    big signed = u
    Print(signed - 10)
    Print(u * 3 + (byte)1)
    i3 small = -5
    Print(small * x)
    big y = 0
    y--
    Print(y)
    Print(true ? x | 1)
    ubig counter = 3
    counter -= 3
    Print(counter)
    Print(x * 100000000000000000000000000000)
}''', [200 * 199, 2**63, -2**63, 2**64 - 1, 44, -2**63, 2 * (2**64 - 1), -3, 22,
       -5 * 2**63, -1, 2**63, 0, 10**29 * 2**63])

    def test_tiny_matches_python_decimal(self):
        rng = random.Random(69)
        values = [Decimal(0), Decimal(1), Decimal(-1), Decimal('0.1'), Decimal('1e-30'),
                  Decimal('-123456789.000000001'), Decimal(10) ** 68 - 1, Decimal('5e-69'),
                  CONTEXT.create_decimal(Decimal(1) / Decimal(3))]
        for _ in range(30):
            digits = rng.randint(1, 69)
            coefficient = rng.randrange(10 ** (digits - 1), 10 ** digits) * rng.choice((1, -1))
            values.append(Decimal(coefficient).scaleb(rng.randint(-200, 200)))
        lines, expected = [], []
        for _ in range(150):
            a, b = rng.choice(values), rng.choice(values)
            lines.append(f'{{ tiny a = {tiny_literal(a)}\ntiny b = {tiny_literal(b)}')
            lines += ['Print(a + b)', 'Print(a - b)', 'Print(a * b)', 'Print(a < b)', 'Print(a == b)']
            expected += [CONTEXT.add(a, b), CONTEXT.subtract(a, b), CONTEXT.multiply(a, b),
                         int(a < b), int(a == b)]
            if b:
                lines.append('Print(a / b)')
                expected.append(CONTEXT.divide(a, b))
            lines.append('}')
        for outputs in self.outputs('Main() {\n' + '\n'.join(lines) + '\n}'):
            self.assertEqual(len(outputs), len(expected))
            for index, (got, want) in enumerate(zip(outputs, expected)):
                if isinstance(want, int):
                    self.assertEqual(got, str(want), f'line {index + 1}')
                else:
                    self.assertEqual(Decimal(got), want, f'line {index + 1}: {got} != {want}')

    def test_tiny_rounding_ties_to_even(self):
        top = Decimal(10) ** 68  # 69 digits
        self.run_source(f'''Main() {{
    tiny even = {10**68}
    Print(even + 0.5)
    Print(even + 1.5)
    Print(even + 2.5)
    Print(even + 0.50000000000000000000000000000000000000000000001)
    Print(even - 0.5)
    tiny third = 1.0 / 3
    Print(third + third + third)
    tiny two = 2
    Print(two / 3)
    Print(-two / 3)
    tiny literal = 1.{'0' * 68}5
    Print(literal)
    tiny literalUp = 1.{'0' * 67}15
    Print(literalUp)
}}''', [10**68, 10**68 + 2, 10**68 + 2, 10**68 + 1,
        '99999999999999999999999999999999999999999999999999999999999999999999.5',
        '0.' + '9' * 69, '0.666666666666666666666666666666666666666666666666666666666666666666667',
        '-0.666666666666666666666666666666666666666666666666666666666666666666667',
        '1', '1.00000000000000000000000000000000000000000000000000000000000000000002'])

    def test_tiny_formatting(self):
        self.run_source('''Main() {
    tiny zero = 0
    Print(zero)
    tiny a = 1200
    Print(a)
    tiny b = 12.500
    Print(b)
    tiny c = 0.00000012
    Print(c)
    tiny d = 0.000000012
    Print(d)
    tiny e = -1.5e100
    Print(e)
    tiny f = 1e68
    Print(f)
    tiny g = 1e69
    Print(g)
    tiny h = 1.0e-999999999
    Print(h)
    tiny i = 9.99e999999999
    Print(i)
    utiny u = 3
    Print(u - 1.5)
    tiny j = 7
    Print(j / 2 * (byte)4)
    Print(j > 6.99999)
    tiny k = 1
    k /= 8
    Print(k)
}''', ['0', '1200', '12.5', '0.00000012', '1.2e-8', '-1.5e+100', '1' + '0' * 68, '1e+69',
       '1e-999999999', '9.99e+999999999', '1.5', '14', '1', '0.125'])

    def test_storage_is_released(self):
        self.run_source('''extern "C" int Live()
big Make(int n) {
    big v = 1
    for int i = 0; i < n; i++ { v = v * 1000000007 + i }
    return v
}
big Pick(bool first, big a, big b) {
    if first { return a }
    return b
}
Work() {
    big total = 0
    int i = 0
    while i < 50 {
        big step = Make(i)
        i++
        if i % 3 == 0 { continue }
        total += step / 7 % 1000000000000
        if i == 45 { break }
    }
    Print(total > 0)
    big kept = Pick(true, Make(3), Make(4))
    Print(kept == Make(3))
    ubig u = 5
    big copy = u
    copy = copy
    Print(copy)
    Print(-Make(2) < 0)
}
Main() {
    Work()
    Print(Live())
}''', [1, 1, 5, 1, 0], fixture=True)

    def test_rejections(self):
        cases = [
            ('big b = 1\nPrint(b + 1.5)', 'type mismatch: expected big, got float'),
            ('big b = 1\nfloat f = 1.5\nPrint(b + f)', 'combine only with their own kind'),
            ('big b = 1\ntiny t = 1\nPrint(b + t)', 'got big and tiny'),
            ('big b = 1\ni9 w = 1\nPrint(b + w)', 'integers up to 64 bits'),
            ('big b = 1\nPrint(b << 1)', 'big supports + - * / %'),
            ('big b = 1\nPrint(b & 1)', 'big supports'),
            ('tiny t = 1\nPrint(t % 2)', 'tiny supports + - * / and comparisons'),
            ('ubig u = 1\nPrint(-u)', 'cannot negate unsigned ubig'),
            ('utiny u = -0.5', 'cannot negate unsigned utiny'),
            ('ubig u = -1', 'out of range for ubig'),
            ('utiny u = -1', 'out of range for utiny'),
            ('int8 s = 1\nubig u = s', 'type mismatch'),
            ('ubig u = 1\nubig v = u + (int8)1', 'type mismatch'),
            ('big b = 1\nlong l = b', 'type mismatch'),
            ('tiny t = 1\ndouble d = t', 'type mismatch'),
            ('Print((double)(big)1)', 'big/ubig convert only'),
            ('Print((tiny)1.5)', 'tiny/utiny accept'),
            ('Print((int)(tiny)1)', 'big/ubig convert only'),
            ('i16 w = 1\nPrint((big)w)', 'integers up to 64 bits'),
            ('tiny t = 1e1000000000', 'decimal literal out of range for tiny'),
            ('tiny t = 1e-1000000000', 'decimal literal out of range'),
            ('big b = 1\nbig* p = &b', 'pointers to managed text or big integers'),
            ('big b = 1\nPrint(!b)', 'big supports only arithmetic negation'),
            ('big ubig = 1', 'reserved keyword'),
        ]
        for statement, message in cases:
            with self.subTest(statement=statement):
                self.reject('Main() { ' + statement + ' }', message)
        self.reject('Holder {\n big value\n}\nMain() {}', 'unmanaged value type')
        self.reject('extern "C" Bad(big b)\nMain() {}', 'native parameter ABI')
        self.reject('extern "C" Bad(tiny& t)\nMain() {}', 'native parameter ABI')
        self.reject('extern "C" Bad(i16& t)\nMain() {}', 'native parameter ABI')

    def test_runtime_traps(self):
        for source in [
            'Main() { big a = 1\nbig b = 0\nPrint(a / b) }',
            'Main() { big a = 1\nbig b = 0\nPrint(a % b) }',
            'Main() { ubig a = 3\nubig b = 5\nPrint(a - b) }',
            'Main() { ubig a = 0\na-- }',
            'Main() { big a = -1\nPrint((ubig)a) }',
            'Main() { int n = -1\nPrint((ubig)n) }',
            'Main() { tiny a = 1\ntiny b = 0\nPrint(a / b) }',
            'Main() { utiny a = 0.1\nutiny b = 0.2\nPrint(a - b) }',
            'Main() { tiny a = -0.5\nPrint((utiny)a) }',
            'Main() { tiny a = 9e999999999\nPrint(a * 10) }',
            'Main() { tiny a = 1e-999999999\nPrint(a / 10) }',
            'Main() { tiny a = 1e999999999\nPrint(a / 1e-999999999) }',
        ]:
            with self.subTest(source=source):
                self.trap(source)


if __name__ == '__main__':
    unittest.main(argv=[__file__, *rest])
