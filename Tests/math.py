"""The compiler's Math namespace: scalar functions, vectors and reservation."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

parser = argparse.ArgumentParser()
parser.add_argument('--compiler', required=True)
parser.add_argument('--clang', required=True)
options, rest = parser.parse_known_args()

# Rounded to thousandths, so the expected output does not depend on libm's last bit.
MILLI = 'int Milli(double value) { return (int)Math.Round(value * 1000.0) }\n'


class MathTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='duck math ', ignore_cleanup_errors=True)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def command(self, *command, success=True):
        result = subprocess.run(command, capture_output=True, timeout=120)
        self.assertEqual(result.returncode == 0, success,
                         (result.stdout + result.stderr).decode('utf-8', errors='replace')[-3000:])
        return result

    def run_source(self, source, expected):
        path, ir = self.root / 'main.dk', self.root / 'main.ll'
        path.write_text(source, encoding='utf-8')
        self.command(options.compiler, str(path), '-o', str(ir))
        libraries = [] if os.name == 'nt' else ['-lm']
        for level in ('-O0', '-O2'):
            exe = self.root / 'test.exe'
            self.command(options.clang, level, str(ir), '-o', str(exe), *libraries)
            output = self.command(str(exe)).stdout.decode().replace('\r\n', '\n').strip().split('\n')
            self.assertEqual(output, expected, level)

    def reject(self, source, message):
        path, ir = self.root / 'bad.dk', self.root / 'bad.ll'
        path.write_text(source, encoding='utf-8')
        result = self.command(options.compiler, str(path), '-o', str(ir), success=False)
        self.assertIn(message, result.stderr.decode('utf-8', errors='replace'))

    def test_real_functions(self):
        calls = {
            'Math.Sqrt(2.0)': 1414, 'Math.Cbrt(27.0)': 3000, 'Math.Exp(1.0)': 2718, 'Math.Exp2(3.0)': 8000,
            'Math.Log(10.0)': 2303, 'Math.Log2(8.0)': 3000, 'Math.Log10(1000.0)': 3000,
            'Math.Sin(1.0)': 841, 'Math.Cos(1.0)': 540, 'Math.Tan(1.0)': 1557,
            'Math.Asin(0.5)': 524, 'Math.Acos(0.5)': 1047, 'Math.Atan(1.0)': 785,
            'Math.Sinh(1.0)': 1175, 'Math.Cosh(1.0)': 1543, 'Math.Tanh(1.0)': 762,
            'Math.Floor(-1.5)': -2000, 'Math.Ceil(-1.5)': -1000, 'Math.Round(2.5)': 3000, 'Math.Trunc(-1.5)': -1000,
            'Math.Pow(2.0, 10.0)': 1024000, 'Math.Atan2(1.0, -1.0)': 2356, 'Math.Hypot(3.0, 4.0)': 5000,
            'Math.Mod(7.5, 2.0)': 1500, 'Math.Lerp(2.0, 4.0, 0.25)': 2500,
        }
        body = ''.join(f'  Print(Milli({call}))\n' for call in calls)
        self.run_source(MILLI + 'Main() {\n' + body + '}\n', [str(value) for value in calls.values()])

    def test_result_follows_operand_type(self):
        self.run_source('''
Main() {
  float f = Math.Sqrt(2.0)
  double d = Math.Sqrt(0.01)
  double tenth = 0.1
  Print(d (0.0)== tenth)
  double wide = 2.0
  double root = Math.Sqrt(wide)
  Print(root * root > 2.0 - 0.000000001)
  Print(f > 1.41)
  Print((int)Math.Sqrt(16))
  float fromInteger = Math.Sqrt(16)
  Print((int)fromInteger)
  double near = Math.Max(wide, 0.1)
  Print(near (0.0)== wide)
}
''', ['1', '1', '1', '4', '4', '1'])

    def test_number_functions(self):
        self.run_source('''
Main() {
  Print(Math.Abs(-5))
  Print(Math.Abs(5))
  long large = -9000000000
  Print(Math.Abs(large))
  uint64 huge = 18446744073709551615
  Print(Math.Abs(huge))
  Print(Math.Min(3, -4))
  Print(Math.Max(3, -4))
  Print(Math.Max(huge, 0))
  Print(Math.Clamp(9, 0, 5))
  Print(Math.Clamp(-9, 0, 5))
  Print(Math.Clamp(3, 0, 5))
  Print(Math.Sign(-7))
  Print(Math.Sign(0))
  Print(Math.Sign(7))
  i16 wide = -170141183460469231731687303715884105727
  Print(Math.Abs(wide))
  Print(Math.Min(wide, 1) == wide)
  int mixed = 2
  Print((int)(Math.Max(mixed, 2.5) * 2.0))
  Print((int)Math.Abs(-3.5))
  Print((int)Math.Sign(-3.5))
}
''', ['5', '5', '9000000000', '18446744073709551615', '-4', '3', '18446744073709551615',
      '5', '0', '3', '-1', '0', '1', '170141183460469231731687303715884105727', '1', '5', '3', '-1'])

    def test_vectors(self):
        self.run_source(MILLI + '''
V3 {
  float X
  float Y
  float Z

  float Square() { return Math.Dot(this, this) }
}
D2 {
  double X
  double Y
}
V4 {
  float X
  float Y
  float Z
  float W
}
V3 Make(float x, float y, float z) {
  V3 v
  v.X = x
  v.Y = y
  v.Z = z
  return v
}
double LengthOf(V3& v) { return Math.Length(v) }
Main() {
  V3 a = Make(1.0, 2.0, 3.0)
  V3 b = Make(4.0, 5.0, 6.0)
  Print(Milli(Math.Dot(a, b)))
  V3 c = Math.Cross(Make(1.0, 0.0, 0.0), Make(0.0, 1.0, 0.0))
  Print(Milli(c.X))
  Print(Milli(c.Y))
  Print(Milli(c.Z))
  Print(Milli(a.Square()))
  V3 three = Make(3.0, 0.0, 4.0)
  Print(Milli(LengthOf(three)))
  Print(Milli(Math.Distance(a, b)))
  V3 unit = Math.Normalize(three)
  Print(Milli(unit.X))
  Print(Milli(unit.Z))
  V3 none = Math.Normalize(Make(0.0, 0.0, 0.0))
  Print(Milli(none.X))
  D2 p
  p.X = 3.0
  p.Y = 4.0
  Print(Milli(Math.Length(p)))
  V4 q
  q.X = 1.0
  q.Y = 1.0
  q.Z = 1.0
  q.W = 1.0
  Print(Milli(Math.Length(q)))
}
''', ['32000', '0', '0', '1000', '14000', '5000', '5196', '600', '800', '0', '5000', '2000'])

    def test_compile_time_matches_runtime(self):
        self.run_source('''
int Root(int n) { return (int)Math.Sqrt((double)n) }
Main() {
  Print(#Root(49))
  Print(#Math.Abs(-12))
  Print(#Math.Clamp(9, 0, 5))
  Print(#Math.Sign(-3))
  Print((int)#Math.Pow(2.0, 8.0))
  Print((int)#Math.Lerp(2.0, 4.0, 0.5))
}
''', ['7', '12', '5', '-1', '256', '3'])

    def test_program_may_import_the_same_c_function(self):
        self.run_source(MILLI + '''
extern "C" double tan(double value)
extern "C" double sqrt(double value)
Main() {
  Print(Milli(Math.Tan(1.0)))
  Print(Milli(tan(1.0)))
  Print(Milli(Math.Sqrt(4.0)))
  Print(Milli(sqrt(4.0)))
}
''', ['1557', '1557', '2000', '2000'])

    def test_math_is_reserved(self):
        message = "'Math' is a compiler namespace"
        self.reject('Math #{ int F() { return 1 } }\nMain() {}', message)
        self.reject('Math #{ int Sqrt(int n) { return n } }\nMain() {}', message)
        self.reject('Math { int x }\nMain() {}', message)
        self.reject('Math #{ int Count = 1 }\nMain() {}', message)
        self.reject('Outer #{ Math #{ int F() { return 1 } } }\nMain() {}', message)

    def test_rejected_calls(self):
        vectors = 'A { float X\n float Y\n float Z }\nB { float X\n float Y\n float Z }\nP { float X\n float Y }\n'
        self.reject('Main() { Print(Math.Missing(1)) }', "the Math namespace has no function 'Missing'")
        self.reject('Main() { Print(Math.Sqrt(1.0, 2.0)) }', "wrong argument count for 'Math.Sqrt'")
        self.reject('Main() { Print(Math.Abs(true)) }', 'Math.Abs requires numbers')
        self.reject('Main() { uint64 a = 1\n int b = 1\n Print(Math.Max(a, b)) }', 'mixed signed and unsigned')
        self.reject('Main() { Print(Math.Dot(1.0, 2.0)) }', 'requires a vector container')
        self.reject(vectors + 'Main() { A a\n B b\n Print(Math.Dot(a, b)) }', "requires both vectors")
        self.reject(vectors + 'Main() { P p\n P q\n P r = Math.Cross(p, q) }', 'with the fields X, Y and Z')
        self.reject('Mixed { float X\n int Y }\nMain() { Mixed m\n Print(Math.Length(m)) }', 'does not qualify')


if __name__ == '__main__':
    unittest.main(argv=[__file__] + rest)
