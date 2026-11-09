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

    def output(self, source, *more):
        """Compiles and runs without compile_time's no-call assertion."""
        paths = [self.root / 'main.dk']
        paths[0].write_text(source, encoding='utf-8')
        for index, extra in enumerate(more):
            paths.append(self.root / f'extra{index}.dk')
            paths[-1].write_text(extra, encoding='utf-8')
        results = []
        for flags in ((), ('--sequential',)):
            ir, exe = self.root / 'main.ll', self.root / 'main.exe'
            self.command(options.compiler, *flags, *map(str, paths), '-o', str(ir))
            self.command(options.clang, '-O0', str(ir), '-o', str(exe))
            results.append(self.command(str(exe)).stdout.decode().replace('\r\n', '\n'))
        self.assertEqual(results[0], results[1])
        return results[0]

    def error(self, source, *messages):
        path, ir = self.root / 'bad.dk', self.root / 'bad.ll'
        path.write_text(source, encoding='utf-8')
        text = self.command(options.compiler, str(path), '-o', str(ir), success=False).stderr.decode(errors='replace')
        for message in messages:
            self.assertIn(message, text)
        self.assertFalse(ir.exists())
        return text

    # Typed macro expansion.
    def test_macro_arguments_bind_once_in_order(self):
        self.assertEqual(self.output('''
Log #{
    int Trace(int n) { Print(n)
        return n }
    macro Pair(int a, int b) {
        Print(b)
        Print(a + a)
    }
    macro Unused(int a) { Print(99) }
}
Main() {
    Log.Pair(Log.Trace(1), Log.Trace(2))
    Log.Unused(Log.Trace(3))
}
'''), '1\n2\n2\n2\n3\n99\n')

    def test_macro_parameters_convert_and_are_read_only(self):
        self.assertEqual(self.output('''
M #{ macro Show(double d) { Print(d / 2) } }
Main() { M.Show(3) }
'''), '1.5\n')
        self.error('M #{ macro Show(int d) { Print(d) } }\nMain() { M.Show("x") }', 'type mismatch')
        self.error('M #{ macro Bump(int d) { d = 2 } }\nMain() { M.Bump(1) }', 'note: in expansion of macro')
        self.error('M #{ macro Show(int& d) { Print(d) } }\nMain() {}', 'cannot be a reference')
        self.error('M #{ macro Show(d) { Print(d) } }\nMain() {}', 'explicit type')
        self.error('M #{ macro Show(int d) { Print(d) } }\nMain() { M.Show() }', 'takes 1 argument(s), got 0')

    def test_macro_hygiene(self):
        self.assertEqual(self.output('''
M #{
    macro Declare() {
        int x = 1
        y : x + 1
        for i : 0...0 { Print(i + y) }
        Print(x)
    }
    macro ReadsCallerFirst() {
        Print(x)
        x : 5
        Print(x)
    }
    macro ArgumentNamedLikeLocal(int v) {
        x : 7
        Print(v + x)
    }
}
Main() {
    int x = 100
    int y = 200
    int i = 300
    M.Declare()
    M.ReadsCallerFirst()
    M.ArgumentNamedLikeLocal(x)
    Print(x + y + i)
}
'''), '2\n1\n100\n5\n107\n600\n')
        self.error('M #{ macro D() { int inner = 1 } }\nMain() { M.D()\nPrint(inner) }', 'inner')

    def test_macro_lambda_captures_stay_separate_per_expansion(self):
        self.assertEqual(self.output('''
M #{
    macro Run(int seed) {
        n : seed
        f : @int() { return n + 1 }
        n = n + 10
        Print(f())
    }
}
Main() {
    M.Run(1)
    M.Run(2)
}
'''), '12\n13\n')

    def test_macro_control_flow_acts_on_caller(self):
        self.assertEqual(self.output('''
M #{
    macro Stop() { break }
    macro Skip() { continue }
    macro Leave(int v) { return v }
}
int F() {
    M.Leave(4)
}
Main() {
    for i : 0...5 {
        if i == 1 { M.Skip() }
        if i == 3 { M.Stop() }
        Print(i)
    }
    Print(F())
}
'''), '0\n2\n4\n')

    def test_macro_lookup_nesting_forward_and_files(self):
        self.assertEqual(self.output('''
Main() {
    A.Outer()
    Other.Run()
}
A #{
    macro Outer() { Inner() }
    macro Inner() { Print(1) }
}
Other #{
    Run() { B.Remote() }
}
''', 'B #{ macro Remote() { Print(2) } }\n'), '1\n2\n')
        self.assertEqual(self.output('''
using N
N #{ macro Hi() { Print(3) } }
Main() { Hi() }
'''), '3\n')
        self.error('Main() { M.Nope() }\nM #{ macro Hi() {} }', 'M.Nope')

    def test_macro_argument_may_be_compile_time_input(self):
        self.assertEqual(self.output('''
int Twice(int n) { return n * 2 }
M #{ macro Show(int n) { Print(#Twice(n)) } }
Main() { M.Show(21) }
'''), '42\n')

    def test_macro_limits_and_locations(self):
        text = self.error('''M #{
    macro A() {
        B()
    }
    macro B() {
        int x = "s"
    }
}
Main() {
    M.A()
}''', 'type mismatch', "note: in expansion of macro 'M.B'", "note: in expansion of macro 'M.A'")
        self.assertIn('bad.dk:6:', text)
        self.assertIn('bad.dk:10:5', text)
        self.error('M #{ macro A() { B() }\nmacro B() { A() } }\nMain() { M.A() }', 'expands itself')
        deep = '\n'.join(f'macro L{i}() {{ L{i + 1}() }}' for i in range(40))
        self.error('M #{\n' + deep + '\nmacro L40() {}\n}\nMain() { M.L0() }', '32-level depth limit')
        wide = '\n'.join(f'macro L{i}() {{ L{i + 1}()\nL{i + 1}() }}' for i in range(30))
        self.error('M #{\n' + wide + '\nmacro L30() { Print(1) }\n}\nMain() { M.L0() }', '200000-token limit')

    def test_macro_declaration_positions(self):
        self.assertEqual(self.output('''
int add(int a, int b) { return foo(a, b) }
macro foo(int a, int b) { a + b }
T {
    macro Local() { Print(7) }
    Run() { Local() }
}
S #{
    Run() { top() }
}
macro top() { Print(8) }
Main() {
    Print(add(1, 2))
    T t
    t.Run()
    S.Run()
    T.Local()
}
'''), '3\n7\n8\n7\n')
        self.error('Main() { macro A() {} }', 'cannot be declared inside a function')
        self.error('macro A() { A() }\nMain() { A() }', 'expands itself')
        self.error('macro A(int n) { A(n) + 1 }\nMain() { Print(A(1)) }', 'expands itself')
        self.error('M #{ macro A() {}\nA()\nB() {} }\nMain() {}', 'not among declarations')
        self.error('M #{ macro A() {}\nmacro A() {} }\nMain() {}', 'already defined')
        # An unused macro's body is never checked.
        self.assertEqual(self.output('M #{ macro Junk() { ) ( not syntax } }\nMain() { Print(1) }'), '1\n')

    def test_value_macros(self):
        self.assertEqual(self.output('''
M #{
    macro Add(int a, int b) { a + b }
    macro Twice(int a) { a * 2 }
    macro Same(int a) { a * a }
    macro Mem(int x) { x }
}
T { int x = 5 }
Main() {
    int n = 3
    Print(M.Add(n, 1) * 2)
    if M.Twice(n + 1) == 8 { Print(1) }
    Print(M.Same(n))
    Print(M.Same(4))
    T t
    Print(t.x + M.Mem(1))
}
'''), '8\n1\n9\n16\n6\n')
        self.error('M #{ macro S() { Print(1)\nPrint(2) } }\nMain() { int x = M.S() }', 'cannot be used as a value')
        self.error('M #{ macro S(int a) { a } }\nMain() { string x = M.S("s") }', 'type mismatch')
        self.error('M #{ macro S(int a) { a } }\nMain() { Print(M.S("s")) }', 'type mismatch')
        self.error('macro S(int a) { a = 2 }\nMain() { Print(S(1) + 1) }', 'error:')

    def test_value_macro_arguments_bind_once_any_number_of_uses(self):
        self.assertEqual(self.output('''
macro Square(int a) { a * a }
macro Cube(int a) { a * a * a }
macro Ignore(int a) { 5 }
macro Half(double d) { d / 2 }
macro Greet(string who) { "hi " + who + who }
macro Nest(int a) { Square(a) + Square(a + 1) }
int Next() {
    Print(100)
    return 3
}
bool Mark(bool v) {
    Print(200)
    return v
}
macro Same(bool v) { v }
T { int field = Square(4) }
S #{ int value = Square(5) }
Main() {
    Print(Square(Next()))
    Print(Cube(Next()))
    Print(Ignore(Next()))
    Print(Half(3))
    Print(Greet("a"))
    Print(Nest(Next()))
    if false && Same(Mark(true)) { Print(0) }
    int i = 0
    while Square(i) < 4 { i++ }
    Print(i)
    T t
    Print(t.field)
    Print(S.value)
    f : @int(int x) { return Square(x + 1) }
    Print(f(2))
    a : 1
    Print(Square(a) + Square(a + 1))
}
'''), '100\n9\n100\n27\n100\n5\n1.5\nhi aa\n100\n25\n2\n16\n25\n9\n5\n')

    def test_value_macro_compile_time_and_labels(self):
        self.assertEqual(self.output('''
macro Square(int a) { a * a }
macro Two() { 2 }
int F(int n) { return Square(n + 1) }
Main() {
    Print(#F(2))
    Switch (2) {
        case Two(): Print(1)
            break
    }
}
'''), '9\n1\n')
        self.error('macro K(int a) { a }\nMain() { Switch (2) { case K(2): break } }', 'case label must be')

    # Conditional compilation.
    def test_conditions_select_declarations_and_statements(self):
        self.assertEqual(self.output('''
#define A true
#define B !A
#define N 0x10
#define NAME "duck"
#if (A && !B) || N < 2
{
    Picked #{ Show() { Print(1) } }
}
#else
{
    Picked #{ Show() { Print(2) } }
    Unbalanced( is fine here
}
#if NAME == "duck" {
    #define NESTED N == 16
}
Main() {
    Picked.Show()
    int x = 0
    #if NESTED
    {
        #if N >= 17 { x = 1 }
        #else { x = 2 }
    }
    Print(x)
}
'''), '1\n2\n')

    def test_inactive_branch_publishes_no_names(self):
        self.error('#define X false\n#if X { Gone #{ F() {} } }\nMain() { Gone.F() }', 'Gone.F')
        self.error('#define X false\n#if X { Gone { int v } }\nMain() { Print(Gone(1)) }', 'Gone')

    def test_define_scope_is_one_file(self):
        self.assertEqual(self.output('#define X true\n#if X { Main() { Print(1) } }\n',
                                     '#define X false\n#if X { Never() {} }\n'), '1\n')
        self.error('#if X { }\n#define X true\nMain() {}', 'not defined here')

    def test_define_rejections(self):
        for source, message in [
            ('#define X true\n#define X false\nMain() {}', 'already defined'),
            ('#define\nMain() {}', 'requires a name'),
            ('#define X\nMain() {}', 'requires a value'),
            ('#define X 1.5\nMain() {}', 'bool, integer, and string'),
            ("#define X 'c'\nMain() {}", 'not char'),
            ('#if Y { }\nMain() {}', 'not defined here'),
            ('#define X true\n#if X || Y { }\nMain() {}', 'not defined here'),
            ('#define X 1\n#if X { }\nMain() {}', 'bool condition'),
            ('#define X 1\n#if X == true { }\nMain() {}', 'cannot compare'),
            ('#define X true\n#if X\nMain() {}', 'requires a { ... } block'),
            ('#define X true\n#if X {\nMain() {}', 'unterminated #if'),
            ('#else { }\nMain() {}', 'without a matching #if'),
            ('#define X true\n#if X { }\n#else\nMain() {}', 'requires a { ... } block'),
            ('Main() { int x = 1 #if x { } }', 'must start a line'),
            ('#define X true\n#if X { } Main() {}', 'new line after'),
        ]:
            with self.subTest(source=source):
                self.error(source, message)

    # Switch.
    def test_switch_selects_once_with_all_selector_types(self):
        self.assertEqual(self.output('''
Color : int #@{ Red, Green }
int Next() { Print(9)
    return 2 }
Main() {
    Switch (Next()) { case 1: Print(1)
        break
        case 2: Print(2)
        break }
    Color c = Color.Green
    Switch (c) { case Color.Red: Print(10)
        break
        case Color.Green: Print(11)
        break }
    string s = "b"
    Switch (s + "c") { case "bc": Print(20)
        break }
    name n = "x"
    Switch (n) { case "x": Print(21)
        break }
    Switch ('q') { case 'q': Print(30)
        break }
    Switch (true) { case false: Print(40)
        break
        case true: Print(41)
        break }
    i20 wide = 123456789012345678901234567
    Switch (wide) { case 123456789012345678901234567: Print(50)
        break }
    byte small = 255
    Switch (small) { case 255: Print(60)
        break }
    Switch (-1) { case -1: Print(70)
        break }
}
'''), '9\n2\n11\n20\n21\n30\n41\n50\n60\n70\n')

    def test_switch_stacking_default_and_no_match(self):
        self.assertEqual(self.output('''
Pick(int v) {
    Switch (v) {
        default:
        case 1:
        case 2, 3:
            Print(1)
            break
        case 4:
            Print(4)
            break
    }
}
Main() {
    Pick(9)
    Pick(3)
    Pick(4)
    Switch (5) { case 1: Print(0)
        break }
    Print(7)
}
'''), '1\n1\n4\n7\n')

    def test_switch_break_and_continue_targets(self):
        self.assertEqual(self.output('''
Main() {
    for i : 0...3 {
        Switch (i) {
            case 0: continue
            case 2: break
            default:
                Switch (i) { case 1: break }
                Print(100 + i)
                break
        }
        Print(i)
    }
    int n = 0
    while n < 3 {
        n++
        Switch (n) { case 2: continue
            default: break }
        Print(n * 10)
    }
    do {
        Switch (1) { case 1: break }
        break
    }
    Print(5)
}
'''), '101\n1\n2\n103\n3\n10\n30\n5\n')

    def test_switch_flow_analysis(self):
        self.assertEqual(self.output('''
int All(int v) {
    Switch (v) {
        case 1: return 10
        default: return 20
    }
}
int Assigned(int v) {
    int n
    Switch (v) {
        case 1: n = 1
            break
        default: n = 2
            break
    }
    return n
}
Main() {
    Print(All(1))
    Print(All(2))
    Print(Assigned(1))
    Print(Assigned(5))
}
'''), '10\n20\n1\n2\n')
        self.error('int F(int v) { Switch (v) { case 1: return 1 } }\nMain() {}', 'not all')
        self.error('Main() { int n\nSwitch (1) { case 1: n = 1\nbreak }\nPrint(n) }', 'n')
        self.error('Main() { Switch (1) { default: return\nPrint(1) } }', 'unreachable')

    def test_switch_local_cleanup_on_every_exit(self):
        self.assertEqual(self.output('''
Res { int id
    ~Res() { Print(id) }
}
int F(int v) {
    for i : 0...1 {
        Switch (v) {
            case 1:
                Res a = {id: 1}
                continue
            case 2:
                Res b = {id: 2}
                return 7
            default:
                Res c = {id: 3}
                break
        }
        Print(50)
        return 8
    }
    return 9
}
Main() {
    Print(F(1))
    Print(F(2))
    Print(F(3))
}
'''.replace('Res a = {id: 1}', 'Res a\n a.id = 1').replace('Res b = {id: 2}', 'Res b\n b.id = 2')
   .replace('Res c = {id: 3}', 'Res c\n c.id = 3')), '1\n1\n9\n2\n7\n3\n50\n8\n')

    def test_switch_rejections(self):
        for source, message in [
            ('Main() { Switch (1) { case 1: Print(1) } }', 'run off its end'),
            ('Main() { Switch (1) { case 1:\n case 2: } }', 'run off its end'),
            ('Main() { Switch (1) { case 1: break\ncase 1: break } }', 'duplicate case label'),
            ('Main() { Switch (1) { case 1, 0x1: break } }', 'duplicate case label'),
            ('Main() { Switch (1) { default: break\ndefault: break } }', 'only one default'),
            ('Main() { Switch (1) { Print(1) } }', "expected 'case' or 'default'"),
            ('Main() { Switch (1) { } }', 'at least one'),
            ('Main() { Switch (1.5) { default: break } }', 'selector must be'),
            ('Main() { int x = 1\nSwitch (1) { case x: break } }', 'case label must be'),
            ('Main() { Switch (1) { case "a": break } }', 'type mismatch'),
            ('Main() { byte b = 1\nSwitch (b) { case 300: break } }', 'out of range'),
            ('E : int #@{ A }\nF : int #@{ B }\nMain() { Switch (E.A) { case F.B: break } }', 'type mismatch'),
            ('Main() { case 1: Print(1) }', 'only directly inside a Switch'),
            ('Main() { break }', 'enclosing loop or Switch'),
            ('Main() { Switch (1) { default: continue } }', 'continue requires an enclosing loop'),
            ('Switch() {}\nMain() {}', 'expression'),
        ]:
            with self.subTest(source=source):
                self.assertTrue(self.error(source, message if message != 'expression' else 'error:'))

    def test_case_names_stay_usable(self):
        self.assertEqual(self.output('''
Main() {
    int case = 1
    default : 2
    case = case + 1
    Switch (case) {
        case 2:
            Print(default)
            break
    }
}
'''), '2\n')

    def test_switch_cases_from_macros(self):
        self.assertEqual(self.output('''
M #{
    macro One() {
        case 1:
            Print(1)
            break
    }
    macro Labeled(int v, string text) {
        case v:
            Print(text)
            break
    }
    macro Both() {
        One()
        Labeled(2, "two")
    }
    macro Stacked() { case 7: }
    macro Finish() { Print(70)
        break }
}
int Effect() { Print(5)
    return 3 }
Show(int v) {
    Switch (v) {
        M.Both()
        M.Labeled(Effect(), "three")
        M.Stacked()
        case 8:
            M.Finish()
    }
}
Main() {
    Show(1)
    Show(2)
    Show(3)
    Show(7)
    Show(9)
}
'''), '5\n1\n5\ntwo\n5\nthree\n5\n70\n5\n')
        self.error('M #{ macro A() { Print(1) } }\nMain() { Switch (1) { M.A() } }', "expected 'case'", 'note: in expansion')
        self.error('M #{ macro A() { case 1: break } }\nMain() { M.A() }', 'only directly inside a Switch', 'note: in expansion')
        self.error('M #{ macro A(int v) { case v: break } }\nMain() { Switch (1) { M.A(2)\nM.A(2) } }',
                   'duplicate case label')

    def test_switch_at_compile_time(self):
        self.run_source('''
int Pick(int v) {
    int total = 0
    for i : 0...3 {
        Switch (i) {
            case 0: continue
            case 2: break
            default: total += v
                break
        }
        total += 100
    }
    Switch (v) { case 1, 2: return total
        default: return -1 }
}
Main() {
    Print(#Pick(1))
    Print(#Pick(9))
}
''', '302\n-1\n')

    def test_examples(self):
        examples = Path(__file__).resolve().parents[1] / 'Examples'
        for relative, expected in [
            ('CompileTime/01_Definitions.dk', 'enabled\n'),
            ('CompileTime/02_MacroExpansion.dk', 'Hello world\n'),
            ('CompileTime/03_TypedMacroArguments.dk', '6\n12\n1\n'),
            ('CompileTime/04_MacroHygiene.dk', '1005\n100\n'),
            ('CompileTime/05_ValueMacros.dk', '15\n16\n25\n1\n'),
            ('CompileTime/06_MacroGeneratedCases.dk', 'zero\none\nunknown\n'),
            ('CompileTime/07_ConditionalBlocks.dk', 'fast path\nlevel 3\n'),
            ('Features/11_Switch.dk', 'weekend\nweekday\n2\n3\n5\nbig\nsmall\n'),
        ]:
            with self.subTest(example=relative):
                self.assertEqual(self.output((examples / relative).read_text(encoding='utf-8')), expected)
        for relative, messages in [
            ('FailureCases/28_CaseMacroOutsideSwitch.dk', ('only directly inside a Switch', 'note: in expansion')),
            ('FailureCases/29_RecursiveMacro.dk', ('expands itself',)),
            ('FailureCases/30_MacroArgumentType.dk', ('type mismatch',)),
            ('FailureCases/31_SwitchFallthrough.dk', ('run off its end',)),
            ('FailureCases/32_DuplicateCaseLabel.dk', ('duplicate case label',)),
            ('FailureCases/33_DefineUsedAsValue.dk', ('LIMIT',)),
            ('FailureCases/34_InactiveBranchName.dk', ('Telemetry',)),
        ]:
            with self.subTest(example=relative):
                self.error((examples / relative).read_text(encoding='utf-8'), *messages)

    def test_control_flow_constants_forward_recursion_and_void(self):
        self.run_source('''
Main() {
    #int n = 6
    Print(#Calc.Factorial(n))
    Print(#Odd(13))
    Print(#Loops(8))
    #Nothing()
    { #int n = 3
      Print(#Calc.Factorial(n)) }
    Print(#Calc.Factorial(n))
}
Calc #{
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
