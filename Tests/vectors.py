"""Native vecN, ivecN, dvecN, matN and dmatN types."""
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


class VectorTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='duck vectors ', ignore_cleanup_errors=True)
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

    def traps(self, source):
        path, ir = self.root / 'main.dk', self.root / 'main.ll'
        path.write_text(source, encoding='utf-8')
        self.command(options.compiler, str(path), '-o', str(ir))
        exe = self.root / 'test.exe'
        self.command(options.clang, '-O0', str(ir), '-o', str(exe))
        self.assertNotEqual(subprocess.run([str(exe)], capture_output=True, timeout=120).returncode, 0)

    def test_constructors(self):
        self.run_source('''
Main() {
  Print(vec2(1.0, 2.0))
  Print(vec3(1.5))
  Print(vec4(vec2(1.0, 2.0), 3.0, 4.0))
  Print(vec3(vec4(1.0, 2.0, 3.0, 4.0)))
  Print(ivec3(1, 2, 3))
  Print(dvec2(0.5, 0.25))
  Print(vec3())
  vec3 direct(7.0, 8.0, 9.0)
  Print(direct)
  Print(vec2(1, 2))
  dvec3 wide = dvec3(vec3(1.5, 2.5, 3.5))
  Print(wide)
  Print(ivec3(vec3(1.5, 2.5, -1.5)))
  Print(mat3(2.0))
  Print(mat3(vec3(1.0, 2.0, 3.0), vec3(4.0, 5.0, 6.0), vec3(7.0, 8.0, 9.0)))
  Print(mat3(1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0))
  Print(mat3(mat4(2.0)))
  Print(mat4(mat3(2.0)))
  Print(dmat3(mat3(1.0)))
}
''', ['(1, 2)', '(1.5, 1.5, 1.5)', '(1, 2, 3, 4)', '(1, 2, 3)', '(1, 2, 3)', '(0.5, 0.25)', '(0, 0, 0)',
      '(7, 8, 9)', '(1, 2)', '(1.5, 2.5, 3.5)', '(1, 2, -2)',
      '[(2, 0, 0), (0, 2, 0), (0, 0, 2)]',
      '[(1, 2, 3), (4, 5, 6), (7, 8, 9)]',
      '[(1, 2, 3), (4, 5, 6), (7, 8, 9)]',
      '[(2, 0, 0), (0, 2, 0), (0, 0, 2)]',
      '[(2, 0, 0, 0), (0, 2, 0, 0), (0, 0, 2, 0), (0, 0, 0, 1)]',
      '[(1, 0, 0), (0, 1, 0), (0, 0, 1)]'])

    def test_components_and_indexing(self):
        self.run_source('''
Bump(vec3& v) { v.Y += 10.0 }
vec3 Twice(vec3 v) { return v * 2.0 }
Main() {
  vec3 v = vec3(1.0, 2.0, 3.0)
  Print(v.X)
  v.Z = 5.0
  Print(v[2])
  v[0] = 9.0
  Print(v)
  Bump(v)
  Print(Twice(v))
  int i = 1
  Print(v[i])
  mat3 m = mat3(1.0)
  m[1][2] = 7.0
  Print(m[1])
  m[0] = vec3(4.0, 5.0, 6.0)
  Print(m[0].Y)
  ivec2 p = ivec2(3, 4)
  Print(p.X + p.Y)
  vec3 [2] pair
  pair[1].X = 6.0
  Print(pair[1])
  vec3 zeroed
  Print(zeroed)
}
''', ['1', '5', '(9, 2, 5)', '(18, 24, 10)', '12', '(0, 1, 7)', '5', '7', '(6, 0, 0)', '(0, 0, 0)'])

    def test_vector_operators(self):
        self.run_source('''
Main() {
  vec3 a = vec3(1.0, 2.0, 3.0)
  vec3 b = vec3(4.0, 5.0, 6.0)
  Print(a + b)
  Print(b - a)
  Print(a * b)
  Print(b / a)
  Print(a * 2.0)
  Print(2.0 * a)
  Print(a / 2.0)
  Print(2.0 / vec2(1.0, 4.0))
  Print(a + 1.0)
  Print(-a)
  Print(a == a)
  Print(a == b)
  Print(a != b)
  Print(vec3(1.0, 2.0, 4.0) == a)
  ivec3 i = ivec3(7, 8, 9)
  Print(i / 2)
  Print(-i)
  Print(i * i)
  vec3 c = a
  c += b
  c -= a
  c *= 2.0
  c /= 4.0
  c *= vec3(2.0, 1.0, 1.0)
  Print(c)
}
''', ['(5, 7, 9)', '(3, 3, 3)', '(4, 10, 18)', '(4, 2.5, 2)', '(2, 4, 6)', '(2, 4, 6)', '(0.5, 1, 1.5)',
      '(2, 0.5)', '(2, 3, 4)', '(-1, -2, -3)', '1', '0', '1', '0', '(3, 4, 4)', '(-7, -8, -9)',
      '(49, 64, 81)', '(4, 2.5, 3)'])

    def test_double_literals_keep_precision(self):
        self.run_source('''
Main() {
  dvec2 v = dvec2(0.1, 0.2)
  double tenth = 0.1
  Print(v.X (0.0)== tenth)
  dvec2 scaled = v * 0.1
  double expected = tenth * tenth
  Print(scaled.X (0.0)== expected)
  dvec2 left = 0.1 * v
  Print(left.X (0.0)== expected)
}
''', ['1', '1', '1'])

    def test_matrix_operators(self):
        self.run_source('''
Main() {
  mat3 a = mat3(1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0)
  mat3 id = mat3(1.0)
  Print(a * id == a)
  Print(id * a == a)
  Print(a * vec3(1.0, 0.0, 0.0))
  Print(a * vec3(0.0, 1.0, 0.0))
  mat4 t = mat4(1.0)
  t[3] = vec4(5.0, 6.0, 7.0, 1.0)
  Print(t * vec4(1.0, 1.0, 1.0, 1.0))
  mat4 s = mat4(2.0)
  s[3][3] = 1.0
  Print(t * s * vec4(1.0, 1.0, 1.0, 1.0))
  Print(s * t * vec4(1.0, 1.0, 1.0, 1.0))
  Print(a + a == a * 2.0)
  Print(2.0 * a == a + a)
  mat3 none = a - a
  Print(none[0])
  mat3 half = a / 2.0
  Print(half[2])
  mat3 product = a
  product *= id
  product += a
  Print(product == a * 2.0)
  dmat4 d = dmat4(1.0)
  Print(d * dvec4(1.0, 2.0, 3.0, 4.0))
}
''', ['1', '1', '(1, 2, 3)', '(4, 5, 6)', '(6, 7, 8, 1)', '(7, 8, 9, 1)', '(12, 14, 16, 1)', '1', '1',
      '(0, 0, 0)', '(3.5, 4, 4.5)', '1', '(1, 2, 3, 4)'])

    def test_math_functions_accept_vectors(self):
        self.run_source('''
Main() {
  vec3 a = vec3(1.0, 0.0, 0.0)
  vec3 b = vec3(0.0, 1.0, 0.0)
  Print(Math.Dot(a, b))
  Print(Math.Cross(a, b))
  Print(Math.Length(vec2(3.0, 4.0)))
  Print(Math.Distance(dvec3(1.0, 1.0, 1.0), dvec3(1.0, 4.0, 5.0)))
  Print(Math.Normalize(vec2(0.0, 2.0)))
}
''', ['0', '(0, 0, 1)', '5', '5', '(0, 1)'])

    def test_layout_is_tightly_packed(self):
        self.run_source('''
extern "C" void* memcpy(void* target, void* source, long byteCount)
Holder {
  vec3 Position
  dmat4 Transform
}
Main() {
  mat4 m = mat4(1.0)
  m[3][0] = 5.0
  float [16] flat
  memcpy(&flat[0], &m, 64)
  Print(flat[0])
  Print(flat[1])
  Print(flat[5])
  Print(flat[12])
  vec3 [2] two
  two[1] = vec3(1.0, 2.0, 3.0)
  float [6] packed
  memcpy(&packed[0], &two[0], 24)
  Print(packed[3])
  Print(packed[5])
  Holder h
  h.Transform[2][2] = 3.0
  Print(h.Transform[2])
}
''', ['1', '0', '1', '5', '1', '3', '(0, 0, 3, 0)'])

    def test_static_field_defaults_and_tests(self):
        self.run_source('''
Camera {
  vec3 Up = vec3(0.0, 1.0, 0.0)
  vec3 Eye
}
Main() {
  Camera c
  Print(c.Up)
  Print(c.Eye)
}
''', ['(0, 1, 0)', '(0, 0, 0)'])

    def test_vector_methods(self):
        self.run_source('''
Main() {
  vec3 v = vec3(3.0, 0.0, 4.0)
  Print(v.Length())
  Print(v.LengthSquared())
  vec3 copy = v
  Print(copy.Normalize())
  Print(v)
  vec3 x = vec3(1.0, 0.0, 0.0)
  vec3 y = vec3(0.0, 1.0, 0.0)
  Print(x.Dot(y))
  Print(x.Cross(y))
  Print(x.Distance(y) * x.Distance(y))
  Print(x.Lerp(y, 0.5))
  Print(vec3(-1.0, 2.0, -3.0).Abs())
  Print(x.Min(y))
  Print(x.Max(y))
  Print((x + y).LengthSquared())
  Print(vec3(0.0).Normalize())
  Print(ivec2(-1, 2).Abs())
  Print(ivec2(1, 5).Min(ivec2(3, 2)))
  Print(dvec2(3.0, 4.0).Length())
  Print(x.Cross(y).Normalize().Length())
  vec3* p = &v
  Print(p.Length())
}
''', ['5', '25', '(0.6, 0, 0.8)', '(3, 0, 4)', '0', '(0, 0, 1)', '2', '(0.5, 0.5, 0)', '(1, 2, 3)',
      '(0, 0, 0)', '(1, 1, 0)', '2', '(0, 0, 0)', '(1, 2)', '(1, 2)', '5', '1', '5'])

    def test_normalize_updates_its_receiver(self):
        self.run_source('''
Unit(vec3& v) { v.Normalize() }
Body {
  vec3 Direction
  Settle() { Direction.Normalize() }
}
int Pick() {
  Print(99)
  return 0
}
vec3 ByValue(vec3 v) {
  v.Normalize()
  return v
}
Main() {
  vec3 v = vec3(3.0, 0.0, 4.0)
  v.Normalize()
  Print(v)
  vec3 u = vec3(0.0, 2.0, 0.0)
  vec3 returned = u.Normalize()
  Print(returned)
  Print(u)
  vec3 viaReference = vec3(0.0, 0.0, 5.0)
  Unit(viaReference)
  Print(viaReference)
  Body b
  b.Direction = vec3(4.0, 0.0, 0.0)
  b.Settle()
  Print(b.Direction)
  b.Direction = vec3(0.0, 7.0, 0.0)
  b.Direction.Normalize()
  Print(b.Direction)
  vec3 [2] several
  several[0] = vec3(0.0, 0.0, 9.0)
  several[Pick()].Normalize()
  Print(several[0])
  vec3 pointed = vec3(6.0, 0.0, 0.0)
  vec3* p = &pointed
  p.Normalize()
  Print(pointed)
  vec3 original = vec3(2.0, 0.0, 0.0)
  Print(ByValue(original))
  Print(original)
  vec3 math = vec3(2.0, 0.0, 0.0)
  Print(Math.Normalize(math))
  Print(math)
  dvec2 d = dvec2(0.0, 3.0)
  d.Normalize()
  Print(d)
  vec3 zero = vec3(0.0)
  zero.Normalize()
  Print(zero)
}
''', ['(0.6, 0, 0.8)', '(0, 1, 0)', '(0, 1, 0)', '(0, 0, 1)', '(1, 0, 0)', '(0, 1, 0)', '99', '(0, 0, 1)',
      '(1, 0, 0)', '(1, 0, 0)', '(2, 0, 0)', '(1, 0, 0)', '(2, 0, 0)', '(0, 1)', '(0, 0, 0)'])

    def test_tasks_see_normalize_as_a_write(self):
        self.run_source('''
int Shrink(vec3& v) {
  v.Normalize()
  return 1
}
int Read(vec3& v) { return (int)(v.Length() * 10.0) }
Main() {
  vec3 v = vec3(30.0, 40.0, 0.0)
  first : Shrink(v)
  second : Read(v)
  Print(first)
  Print(second)
}
''', ['1', '10'])

    def test_matrix_methods(self):
        self.run_source('''
Main() {
  mat3 a = mat3(1.0, 2.0, 3.0, 0.0, 1.0, 4.0, 5.0, 6.0, 0.0)
  Print(a.Determinant())
  Print(a.Inverse())
  Print(a.Transpose())
  Print(a)
  mat4 t = mat4(1.0)
  t[3] = vec4(5.0, 6.0, 7.0, 1.0)
  Print(t.Determinant())
  mat4 back = t.Inverse()
  Print(back[3])
  Print(t * back == mat4(1.0))
  mat4 r = mat4(0.0)
  r[0][1] = 1.0
  r[1][0] = -1.0
  r[2][2] = 2.0
  r[3][3] = 1.0
  r[3][0] = 3.0
  Print(r.Determinant())
  Print(r * r.Inverse() == mat4.Identity())
  Print(r.Transpose().Transpose() == r)
  dmat3 d = dmat3(2.0)
  Print(d.Determinant())
  dmat3 inverse = d.Inverse()
  Print(inverse[0][0])
}
''', ['1', '[(-24, 18, 5), (20, -15, -4), (-5, 4, 1)]', '[(1, 0, 5), (2, 1, 6), (3, 4, 0)]',
      '[(1, 2, 3), (0, 1, 4), (5, 6, 0)]', '1', '(-5, -6, -7, 1)', '1', '2', '1', '1', '8', '0.5'])

    def test_singular_inverse_is_not_finite(self):
        self.run_source('''
Main() {
  mat3 zero = mat3.Zero()
  mat3 bad = zero.Inverse()
  float value = bad[0][0]
  Print(value == value)
}
''', ['0'])

    def test_type_functions(self):
        self.run_source('''
Main() {
  Print(vec3.Identity())
  Print(vec3.Zero())
  Print(vec2.One())
  Print(ivec4.Identity())
  Print(dvec2.Zero())
  Print(mat3.Identity())
  Print(mat3.Zero())
  Print(mat4.Identity() == mat4(1.0))
  Print(dmat3.Identity() == dmat3(1.0))
  vec3 v = vec3(2.0, 3.0, 4.0)
  Print(v * vec3.Identity() == v)
  Print(vec3.One().Length() > 1.0)
  Print(int.Max)
}
''', ['(1, 1, 1)', '(0, 0, 0)', '(1, 1)', '(1, 1, 1, 1)', '(0, 0)', '[(1, 0, 0), (0, 1, 0), (0, 0, 1)]',
      '[(0, 0, 0), (0, 0, 0), (0, 0, 0)]', '1', '1', '1', '1', '2147483647'])

    def test_methods_in_tasks_and_fields(self):
        self.run_source('''
Body {
  vec3 Velocity
  float Speed() { return Velocity.Length() }
}
float Measure(vec3 v) { return v.Length() }
Main() {
  Body b
  b.Velocity = vec3(3.0, 4.0, 0.0)
  Print(b.Speed())
  Print(b.Velocity.Normalize())
  first : Measure(vec3(6.0, 8.0, 0.0))
  second : Measure(vec3(5.0, 12.0, 0.0))
  Print(first + second)
}
''', ['5', '(0.6, 0.8, 0)', '23'])

    def test_rejected_methods(self):
        unused = 'returns a new value and changes nothing'
        self.reject('Main() { vec3 v\n v.Length() }', unused)
        self.reject('vec3 Make() { return vec3(1.0) }\nMain() { Make().Normalize() }', unused)
        self.reject('Main() { mat4 m\n m.Inverse() }', unused)
        self.reject('Main() { #vec3 v = vec3(1.0)\n Print(v.Normalize()) }', 'cannot call a mutable method on a constant')
        self.reject('Main() { vec3.Identity() }', unused)
        self.reject('Main() { vec3 v\n Print(v.Missing()) }', "vec3 has no method 'Missing'")
        self.reject('Main() { vec2 v\n Print(v.Cross(v)) }', "vec2 has no method 'Cross'")
        self.reject('Main() { ivec2 v\n Print(v.Length()) }', "ivec2 has no method 'Length'")
        self.reject('Main() { vec3 v\n Print(v.Inverse()) }', "vec3 has no method 'Inverse'")
        self.reject('Main() { mat3 m\n Print(m.Length()) }', "mat3 has no method 'Length'")
        self.reject('Main() { vec3 v\n Print(v.Dot()) }', "wrong argument count for 'Dot'")
        self.reject('Main() { vec3 v\n Print(v.Length(1.0)) }', "wrong argument count for 'Length'")
        self.reject('Main() { vec3 v\n dvec3 d\n Print(v.Dot(d)) }', 'Dot requires vec3')
        self.reject('Main() { vec3 v\n Print(v.Lerp(v, true)) }', 'Lerp requires a number')
        self.reject('Main() { Print(vec3.Missing()) }', "vec3 has no function 'Missing'")
        self.reject('Main() { Print(mat3.One()) }', "mat3 has no function 'One'")
        self.reject('Main() { Print(vec3.Zero(1.0)) }', 'wrong argument count')

    def test_float_equality_tolerates_rounding(self):
        self.run_source('''
Main() {
  float a = 0.1
  float b = 0.2
  float c = 0.3
  Print(a + b == c)
  double x = 0.1
  double y = 0.2
  double z = 0.3
  Print(x + y == z)
  Print(x + y != z)
  Print(x + y (0.0)== z)
  Print(x + y (0.0)!= z)
  Print(1.0 == 1.1)
  float large = 100000.0
  Print(large + 0.25 == large)
  Print(large + 5.0 == large)
  double little = 0.0000000001
  Print(little == 0.0)
  Print(little (0.0)== 0.0)
  T1()
}
T1() {
  float zero = 0.0
  float nan = zero / zero
  float inf = 1.0 / zero
  Print(nan == nan)
  Print(nan != nan)
  Print(inf == inf)
  Print(inf == 5.0)
  Print(inf (1.0)== inf)
  Print(inf (1.0)== -inf)
  Print(nan (1.0)== nan)
  Print(1.5 < 1.5 + 0.000001)
}
''', ['1', '1', '0', '0', '1', '0', '1', '0', '1', '0', '0', '1', '1', '0', '1', '0', '0', '1'])

    def test_written_tolerance(self):
        self.run_source('''
float Near(float value) { return value }
Main() {
  Print(1.0 (0.5)== 1.4)
  Print(1.0 (0.5)== 1.5)
  Print(1.0 (0.5)== 1.6)
  Print(1.0 (0.5)!= 1.6)
  float step = 0.25
  Print(1.0 (step * 2.0)== 1.4)
  Print(1.0 (1)== 1.9)
  Print(1.0 + 0.1 (0.5)== 1.4 && true)
  Print(Near(0.5)== 0.5)
  Print(Near(0.5) (0.1)== 0.55)
  int whole = 2
  Print(whole (0.5)== 2.25)
  if 1.0 (0.5)== 1.2 { Print(7) }
  double d = 1.0
  Print(d (0.0000000001)== 1.00000000001)
}
''', ['1', '1', '0', '1', '1', '1', '1', '1', '1', '1', '7', '1'])

    def test_vector_and_matrix_tolerance(self):
        self.run_source('''
Main() {
  vec3 a = vec3(1.0, 2.0, 3.0)
  Print(a == vec3(1.000001, 2.0, 3.0))
  Print(a == vec3(1.1, 2.0, 3.0))
  Print(a (0.5)== vec3(1.4, 2.4, 2.6))
  Print(a (0.5)== vec3(1.4, 2.6, 3.0))
  Print(a (0.5)!= vec3(1.4, 2.6, 3.0))
  Print(a (0.0)== vec3(1.000001, 2.0, 3.0))
  mat3 m = mat3(1.0)
  Print(m == mat3(1.000001))
  Print(m (0.5)== mat3(1.4))
  Print(m (0.5)== mat3(1.6))
  dvec2 d = dvec2(1.0, 2.0)
  Print(d (0.25)== dvec2(1.2, 2.2))
}
''', ['1', '0', '1', '0', '1', '0', '1', '1', '0', '1'])

    def test_compile_time_tolerance(self):
        self.run_source('''
bool Same(double a, double b) { return a == b }
bool Within(double a, double b) { return a (0.5)== b }
Main() {
  Print(#Same(0.3, 0.30000000001))
  Print(#Same(0.3, 0.4))
  Print(#Within(1.0, 1.4))
  Print(#Within(1.0, 1.6))
}
''', ['1', '0', '1', '0'])

    def test_rejected_tolerances(self):
        message = 'a comparison tolerance needs float or double'
        self.reject('Main() { int a = 1\n Print(a (1)== a) }', message)
        self.reject('Main() { Print(ivec2(1) (1)== ivec2(1)) }', message)
        self.reject('Main() { bool a = true\n Print(a (1)== a) }', message)
        self.reject('Main() { Print(1.0 (true)== 1.0) }', 'a comparison tolerance must be a number')
        self.reject('Main() { double d = 0.5\n float f = 1.0\n Print(f (d)== f) }', 'mismatch')

    def test_only_used_types_are_defined(self):
        path, ir = self.root / 'main.dk', self.root / 'main.ll'
        path.write_text('Holder { mat3 M }\nMain() { Print(1) }\n', encoding='utf-8')
        self.command(options.compiler, str(path), '-o', str(ir))
        text = ir.read_text(encoding='utf-8')
        self.assertIn('%struct.mat3 = type', text)
        self.assertIn('%struct.vec3 = type', text)
        self.assertNotIn('%struct.mat4 = type', text)
        self.assertNotIn('%struct.dvec3 = type', text)

    def test_out_of_range_index_traps(self):
        self.traps('Main() {\n vec3 v\n int i = 3\n Print(v[i]) }\n')
        self.traps('Main() {\n mat3 m\n int i = 3\n Print(m[i]) }\n')

    def test_integer_division_by_zero_traps(self):
        self.traps('Main() {\n int zero = 0\n Print(ivec2(1, 2) / zero) }\n')

    def test_rejected(self):
        self.reject('vec3 { float X }\nMain() {}', 'reserved keyword')
        self.reject('Main() { int mat4 = 1 }', 'reserved keyword')
        self.reject('Main() { Print(vec3(1.0, 2.0)) }', 'vec3 needs 3 components, got 2')
        self.reject('Main() { Print(vec2(1.0, 2.0, 3.0)) }', 'vec2 needs 2 components, got 3')
        self.reject('Main() { Print(vec3(true)) }', 'requires numbers or vectors')
        self.reject('Main() { Print(mat3(vec2(1.0, 2.0), vec2(1.0, 2.0), vec2(1.0, 2.0))) }', 'takes columns of 3')
        self.reject('Main() { Print(vec3(mat3(1.0))) }', 'a matrix can only be passed alone')
        self.reject('Main() { double d = 1.0\n Print(vec3(d)) }', 'mismatch')
        self.reject('Main() { Print(vec3(1.0) + dvec3(1.0)) }', "operator '+' does not apply")
        self.reject('Main() { Print(vec3(1.0) + vec2(1.0)) }', "operator '+' does not apply")
        self.reject('Main() { Print(vec3(1.0) < vec3(1.0)) }', "operator '<' does not apply")
        self.reject('Main() { Print(ivec2(1) % ivec2(1)) }', "operator '%' does not apply")
        self.reject('Main() { Print(ivec2(1) * 1.5) }', 'mismatch')
        self.reject('Main() { Print(mat3(1.0) / mat3(1.0)) }', "operator '/' does not apply")
        self.reject('Main() { Print(mat3(1.0) + 1.0) }', "operator '+' does not apply")
        self.reject('Main() { Print(mat4(1.0) * vec3(1.0)) }', "operator '*' does not apply")
        self.reject('Main() { vec3 v\n v++ }', 'does not apply')
        self.reject('Main() { vec3 v\n v *= mat3(1.0) }', 'does not apply')
        self.reject('Main() { vec3 v\n Print(v[1.5]) }', 'must be an integer')
        self.reject('Main() { vec3 v\n Print(v.Q) }', 'Q')
        self.reject('Main() { Print(Math.Length(ivec2(1, 1))) }', 'does not qualify')


if __name__ == '__main__':
    unittest.main(argv=[__file__] + rest)
