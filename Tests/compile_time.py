"""Explicit compile-time calls: compare evaluation with runtime at O0 and O2."""
import argparse
from pathlib import Path
import subprocess
import tempfile
import unittest
import random

parser = argparse.ArgumentParser()
parser.add_argument('--compiler', required=True)
parser.add_argument('--clang', required=True)
options, rest = parser.parse_known_args()


class CompileTimeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='duck comptime ', ignore_cleanup_errors=True)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def command(self, *args, success=True):
        result = subprocess.run(args, capture_output=True, timeout=60)
        self.assertEqual(result.returncode == 0, success,
                         (result.stdout + result.stderr).decode(errors='replace'))
        return result

    def compile(self, source, *flags):
        path, ir = self.root / 'main.dk', self.root / 'main.ll'
        path.write_text(source, encoding='utf-8')
        self.command(options.compiler, *flags, str(path), '-o', str(ir))
        return ir

    def run_source(self, source, expected=None):
        # Run both scheduling modes: no task may execute a folded call.
        for flags in ((), ('--sequential',)):
            ir = self.compile(source, *flags)
            text = ir.read_text()
            main = text.split('define void @duck.Main(', 1)[1].split('\n}', 1)[0]
            self.assertNotIn('call i32 @duck.', main)
            for level in ('-O0', '-O2'):
                exe = self.root / 'test.exe'
                self.command(options.clang, level, str(ir), '-o', str(exe))
                output = self.command(str(exe)).stdout.decode().replace('\r\n', '\n')
                if expected is not None:
                    self.assertEqual(output, expected)
                else:
                    values = output.splitlines()
                    self.assertEqual(values[::2], values[1::2])

    def reject(self, source, message, location=None):
        path, ir = self.root / 'bad.dk', self.root / 'bad.ll'
        path.write_text(source, encoding='utf-8')
        result = self.command(options.compiler, str(path), '-o', str(ir), success=False)
        error = result.stderr.decode(errors='replace')
        self.assertIn(message, error)
        if location:
            self.assertIn(str(path) + location + ' error: compile-time call failed:', error)
        self.assertFalse(ir.exists())

    def test_control_flow_constants_forward_recursion_and_void(self):
        self.run_source('''
Main() {
    #int n = 6
    Print(#Math.Factorial(n))
    Print(#Odd(13))
    Print(#Loops(8))
    #Nothing()
    { #int n = 3
      Print(#Math.Factorial(n)) }
    Print(#Math.Factorial(n))
}
Math #{
    int Factorial(int n) {
        if n <= 1 { return 1 }
        return n * Factorial(n - 1)
    }
}
bool Odd(int n) { return n == 0 ? false | Even(n - 1) }
bool Even(int n) { return n == 0 ? true | Odd(n - 1) }
Nothing() { return }
int Loops(int n) {
    int sum
    sum = 0
    for i : 0; i < n; i++ {
        if i == 2 { continue }
        if i == 6 { break }
        sum += i
    }
    do { sum++ } while sum < 20
    while sum < 23 { sum++ }
    for i : 3...1 { sum += i }
    do { sum++
         break }
    return sum
}
''', '720\n1\n30\n6\n720\n')

    def test_numeric_runtime_parity(self):
        functions = '''
int Wrap(int x) { return x + 1 }
int Remainder(int x) { return x % 7 }
int8 Narrow(int x) { return (int8)x }
long Floor(double x) { return (long)x }
float Round(float x) { return (x + 1) - x }
double Divide(double x) { return x / 0.0 }
int SignedShift(int x) { return x >> 3 }
uint64 UnsignedShift(uint64 x) { return x >> 3 }
i64 Wide(i64 x) { return ((~x * 17) / 3) % 997 }
u64 UnsignedWide(u64 x) { return (x * 19 + 3) / 7 }
float WideFloat(u64 x) { return (float)x }
u64 FloatWide(double x) { return (u64)x }
bool Lazy(bool x) { return x || (1 / 0 == 2) }
int Choose(bool x) { return x ? 9 | (1 << -1) }
'''
        calls = ['Wrap(int.Max)', 'Remainder(-29)', 'Narrow(255)', 'Floor(-2.1)',
                 'Round(16777216.0)', 'Divide(1.0)', 'Divide(0.0)', 'SignedShift(-97)',
                 'UnsignedShift(uint64.Max)', 'Wide(i64.Min)', 'Wide(123456789012345678901234567890)',
                 'UnsignedWide(u64.Max)', 'WideFloat(u64.Max)', 'FloatWide(1.0e100)',
                 'Lazy(true)', 'Choose(true)']
        # Runtime calls live in a helper so Main itself contains only folded calls.
        functions += '\nRuntime() {\n' + '\n'.join('Print(' + call + ')' for call in calls) + '\n}\n'
        source = functions + '\nMain() {\n' + '\n'.join('Print(#' + call + ')' for call in calls) + '\nRuntime()\n}\n'
        ir = self.compile(source, '--sequential')
        main = ir.read_text().split('define void @duck.Main(', 1)[1].split('\n}', 1)[0]
        self.assertNotIn('call i32 @duck.', main)
        for level in ('-O0', '-O2'):
            exe = self.root / 'parity.exe'
            self.command(options.clang, level, str(ir), '-o', str(exe))
            output = self.command(str(exe)).stdout.decode().splitlines()
            # IEEE NaN sign/payload (and printf spelling) need not survive LLVM
            # optimization; their comparisons and casts are tested separately.
            output = ['nan' if 'nan' in value.lower() else value for value in output]
            self.assertEqual(output[:len(calls)], output[len(calls):])

    def test_traps_and_budgets_point_to_marker(self):
        for body, argument, error in [
            ('return 1 / x', '0', 'division or remainder by zero'),
            ('return int.Min / x', '-1', 'signed minimum divided by -1'),
            ('return int.Min % x', '-1', 'signed minimum divided by -1'),
            ('return 1 << x', '-1', 'shift count'),
            ('return 1 >> x', '32', 'shift count'),
            ('return (int)((double)x / 0.0)', '1', 'conversion is out of range'),
            ('while true {}\nreturn x', '1', '100000-step limit'),
            ('return F(x)', '1', '32-call depth limit'),
        ]:
            with self.subTest(error=error, body=body):
                self.reject('int F(int x) { ' + body + ' }\nMain() { Print(#F(' + argument + ')) }',
                            error, ':' + str(2 + body.count('\n')) + ':16:')

    def test_effects_runtime_arguments_and_shadowing(self):
        for source, message in [
            ('int F() { Print(1)\nreturn 2 }\nMain() { Print(#F()) }', 'I/O and Exec are forbidden'),
            ('int F() { if false { Print(1) }\nreturn 2 }\nMain() { Print(#F()) }', 'I/O and Exec are forbidden'),
            ('State #{ int N = 3 }\nint F() { return State.N }\nMain() { Print(#F()) }', 'static storage'),
            ('int F(int& x) { return x }\nMain() { int x = 2\nPrint(#F(x)) }', 'reference'),
            ('extern "C" int Native()\nMain() { Print(#Native()) }', 'direct Duck functions'),
            ('int F(int x) { return x }\nMain() { int x = 2\nPrint(#F(x)) }', 'not a constant argument'),
            ('int F(int x) { return x }\nint G(int x) { return #F(x) }\nMain() {}', 'not a constant argument'),
            ('int F(int x) { return x }\nMain() { #int x = 2\n{ int x = 3\nPrint(#F(x)) } }', 'not a constant argument'),
            ('int F(int x) { return x }\nMain() { #int i = 2\nfor i : 0...2 { Print(#F(i)) } }', 'not a constant argument'),
            ('Main() { Print(#(1 + 2)) }', "'#' requires an explicit function call"),
        ]:
            with self.subTest(source=source):
                self.reject(source, message)

    def test_generated_initializers(self):
        self.run_source('''
int Twice(int n) { return n * 2 }
State #{ int Value = #Twice(21) }
Box { int Value = #Twice(9) }
Main() {
    Print(State.Value)
    Box b
    Print(b.Value)
}
''', '42\n18\n')

    def test_baking_across_files_and_unavailable_dependency_body(self):
        helper = self.root / 'helper.dk'
        helper.write_text('int Twice(int n) { return n * 2 }')
        main = self.root / 'entry.dk'
        main.write_text('Main() { Print(#Twice(21)) }')
        ir = self.root / 'multiple.ll'
        self.command(options.compiler, str(main), str(helper), '-o', str(ir))
        self.assertIn('i32 42', ir.read_text())
        # Separate modules expose signatures; a #call must not silently fall
        # back to calling the dependency at runtime.
        result = self.command(options.compiler, str(main), '--dependency', str(helper),
                              '-o', str(self.root / 'bad.ll'), success=False)
        self.assertIn('function body is unavailable', result.stderr.decode())

    def test_nested_marks_constants_and_conversion_of_results(self):
        self.run_source('''
int Add(int x, int y) { return x + y }
int8 Small() { return -1 }
double Real() { return -2.1 }
bool Equal(double x) { return x == x }
Main() {
    #int n = #Add(2, 3)
    Print(#Add(n, #Add(4, 5)))
    long a = #Small()
    Print(a)
    Print((int)#Real())
    Print(#Equal(0.0 / 0.0))
}
''', '14\n-1\n-3\n0\n')

    def test_integer_widths_against_python(self):
        rng = random.Random(76123)
        functions, calls, expected = [], [], []
        for size in (1, 2, 3, 4, 7, 8, 9, 16, 31, 64):
            for signed in (False, True):
                kind = ('i' if signed else 'u') + str(size)
                name = 'Mix' + kind
                bits = size * 8
                mask = (1 << bits) - 1
                functions.append(f'{kind} {name}({kind} x, {kind} y) {{ return ((~x * ({kind})19) + y) ^ (x >> 3) }}')
                x = rng.randrange(1 << (bits - (1 if signed else 0)))
                if signed:
                    x = -x
                y = rng.randrange(1 << (bits - (1 if signed else 0)))
                result = (((~x * 19) + y) ^ (x >> 3)) & mask
                if signed and result >= 1 << (bits - 1):
                    result -= 1 << bits
                calls.append(f'Print(#{name}({x}, {y}))')
                expected.append(str(result))
        source = '\n'.join(functions) + '\nMain() {\n' + '\n'.join(calls) + '\n}'
        self.run_source(source, '\n'.join(expected) + '\n')


if __name__ == '__main__':
    unittest.main(argv=[__file__, *rest])
