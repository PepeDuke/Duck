"""T{ } blocks: discovery, the generated runner, assertions, and build separation."""
import argparse
from pathlib import Path
import subprocess
import tempfile
import unittest

parser = argparse.ArgumentParser()
parser.add_argument('--compiler', required=True)
parser.add_argument('--clang', required=True)
options, rest = parser.parse_known_args()


class UnitTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='duck tests ', ignore_cleanup_errors=True)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.count = 0

    def write(self, source, name='main.dk'):
        path = self.root / name
        path.write_text(source, encoding='utf-8')
        return path

    def duckc(self, *args, success=True):
        result = subprocess.run([options.compiler, *map(str, args)], capture_output=True, timeout=60)
        self.assertEqual(result.returncode == 0, success, result.stderr.decode(errors='replace'))
        return result.stderr.decode(errors='replace')

    def link(self, ir, level='-O0'):
        self.count += 1
        exe = self.root / f'program{self.count}.exe'
        result = subprocess.run([options.clang, '-Wno-override-module', level, str(ir), '-o', str(exe)],
                                capture_output=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors='replace'))
        return exe

    def execute(self, exe, *args):
        result = subprocess.run([str(exe), *args], capture_output=True, timeout=60)
        return result.returncode, result.stdout.decode().replace('\r\n', '\n')

    def runner(self, source, *flags, level='-O0'):
        path = source if isinstance(source, Path) else self.write(source)
        ir = self.root / f'runner{self.count}.ll'
        self.duckc('--test', *flags, path, '-o', ir)
        return self.link(ir, level)

    def run_tests(self, source, *args):
        return self.execute(self.runner(source), *args)

    def passes(self, body, setup='Box #{ Noop() {} }\n'):
        """body holds the statements of one test that must pass."""
        status, output = self.run_tests(setup + 'T{\n Check() {\n' + body + '\n }\n}\n')
        self.assertEqual((status, output), (0, 'RUN  Box.Check\nPASS Box.Check\n1 passed, 0 failed, 1 total\n'))

    def fails(self, body, *expected, setup='Box #{ Noop() {} }\n'):
        status, output = self.run_tests(setup + 'T{\n Check() {\n' + body + '\n }\n}\n')
        self.assertEqual(status, 1, output)
        self.assertIn('FAIL Box.Check\n0 passed, 1 failed, 1 total\n', output)
        for text in expected:
            self.assertIn(text, output)
        return output

    def rejected(self, source, message, *flags):
        path = self.write(source, 'bad.dk')
        error = self.duckc('--test', *flags, path, '-o', self.root / 'bad.ll', success=False)
        self.assertIn(message, error)

    SAMPLE = '''Arithmetic
#{
    int Double(int value)
    {
        return value * 2
    }
}
T{
    DoublesPositive()
    {
        T.AreEqual(Arithmetic.Double(3), 6)
    }

    DoublesZero()
    {
        T.IsTrue(Arithmetic.Double(0) == 0)
    }
}
Main() { Print(1) }
'''

    def test_runner_reports_each_test_and_a_summary(self):
        self.assertEqual(self.run_tests(self.SAMPLE), (0, (
            'RUN  Arithmetic.DoublesPositive\nPASS Arithmetic.DoublesPositive\n'
            'RUN  Arithmetic.DoublesZero\nPASS Arithmetic.DoublesZero\n'
            '2 passed, 0 failed, 2 total\n')))

    def test_optimized_and_sequential_builds_agree(self):
        expected = self.run_tests(self.SAMPLE)
        self.assertEqual(self.execute(self.runner(self.SAMPLE, level='-O2')), expected)
        self.assertEqual(self.execute(self.runner(self.SAMPLE, '--sequential')), expected)

    def test_failure_names_the_test_and_source_location(self):
        path = self.write('Box #{ int One() { return 1 } }\nT{\n    Wrong() {\n        T.AreEqual(One(), 2)\n    }\n}\n')
        status, output = self.execute(self.runner(path))
        self.assertEqual(status, 1)
        self.assertEqual(output, (
            f'RUN  Box.Wrong\n{path}:4:9: test Box.Wrong: T.AreEqual failed\n'
            '  actual:   1\n  expected: 2\nFAIL Box.Wrong\n0 passed, 1 failed, 1 total\n'))

    def test_failing_test_does_not_stop_the_run(self):
        status, output = self.run_tests('Box #{ Noop() {} }\nT{\n A() { T.Fail() }\n B() { T.IsTrue(true) }\n}\n')
        self.assertEqual(status, 1)
        self.assertIn('FAIL Box.A\nRUN  Box.B\nPASS Box.B\n1 passed, 1 failed, 2 total\n', output)

    def test_list_and_filters_select_tests_by_stable_name(self):
        exe = self.runner('Box #{ Noop() {} }\nT{\n Alpha() { }\n Beta() { }\n}\nOther #{ Noop() {} }\nT{\n Alpha() { }\n}\n')
        self.assertEqual(self.execute(exe, '--list'), (0, 'Box.Alpha\nBox.Beta\nOther.Alpha\n'))
        self.assertEqual(self.execute(exe, '--list', 'Alpha'), (0, 'Box.Alpha\nOther.Alpha\n'))
        self.assertEqual(self.execute(exe, 'Box.Beta')[0], 0)
        self.assertEqual(self.execute(exe, 'Box.Beta')[1].splitlines()[-1], '1 passed, 0 failed, 1 total')
        self.assertEqual(self.execute(exe, 'Beta', 'Other')[1].splitlines()[-1], '2 passed, 0 failed, 2 total')
        self.assertEqual(self.execute(exe, 'Missing'), (2, 'no test matches the filter\n'))
        self.assertEqual(self.execute(exe, '--list', 'Missing')[0], 2)
        self.assertEqual(self.execute(exe, '--nope')[0], 2)

    def test_timing_reports_each_test(self):
        exe = self.runner(self.SAMPLE)
        status, output = self.execute(exe, '--timing', 'Positive')
        self.assertEqual(status, 0)
        lines = output.splitlines()
        self.assertEqual(lines[1], 'PASS Arithmetic.DoublesPositive')
        kind, nanos, name = lines[2].split(' ')
        self.assertEqual((kind, name), ('TIME', 'Arithmetic.DoublesPositive'))
        self.assertGreaterEqual(int(nanos), 0)
        self.assertEqual(self.execute(exe, '--timing', 'Missing')[0], 2)

    def test_no_tests_is_an_empty_success(self):
        self.assertEqual(self.run_tests('Main() { Print(1) }'), (0, '0 passed, 0 failed, 0 total\n'))
        self.assertEqual(self.run_tests('int Add(int a, int b) { return a + b }'), (0, '0 passed, 0 failed, 0 total\n'))

    def test_order_does_not_depend_on_input_order(self):
        one = self.write('A #{ Noop() {} }\nT{\n Zed() { }\n}\n', 'one.dk')
        two = self.write('B #{ Noop() {} }\nT{\n Abe() { }\n}\n', 'two.dk')
        ir = self.root / 'order.ll'
        self.duckc('--test', one, two, '-o', ir)
        forward = self.execute(self.link(ir), '--list')
        self.duckc('--test', two, one, '-o', ir)
        self.assertEqual(self.execute(self.link(ir), '--list'), forward)
        self.assertEqual(forward, (0, 'A.Zed\nB.Abe\n'))

    def test_main_is_compiled_but_never_run(self):
        status, output = self.run_tests(self.SAMPLE)
        self.assertNotIn('\n1\n', '\n' + output)
        # The program's Main may take arguments; its test build still links.
        args = self.SAMPLE.replace('Main() { Print(1) }', 'Main(string [] args) { Print(args.Len) }')
        self.assertEqual(self.run_tests(args)[0], 0)

    def test_ordinary_build_is_unchanged_by_tests(self):
        program = 'Arithmetic\n#{\n int Double(int value) { return value * 2 }\n}\n'
        entry = 'Main() { Print(Arithmetic.Double(4)) }\n'
        tests = 'T{\n Check() { T.AreEqual(Arithmetic.Double(1), 2) }\n}\n'
        plain, tested = self.root / 'plain.ll', self.root / 'tested.ll'
        # Same source name, so the only possible difference is the T block.
        self.duckc(self.write(program + entry), '-o', plain)
        self.duckc(self.write(program + tests + entry), '-o', tested)
        self.assertEqual(plain.read_bytes(), tested.read_bytes())
        self.assertEqual(self.execute(self.link(tested)), (0, '8\n'))
        self.assertNotIn(b'duck$test', tested.read_bytes())

    def test_ordinary_build_does_not_analyze_test_bodies(self):
        source = 'Box #{ Noop() {} }\nT{\n Check() { int x = "not an int" }\n}\nMain() { Print(1) }\n'
        self.duckc(self.write(source), '-o', self.root / 'ok.ll')
        self.rejected(source, 'type mismatch')

    def test_dependency_tests_stay_with_their_module(self):
        core = self.write('Core #{ int One() { return 1 } }\nT{\n CoreTest() { T.Fail() }\n}\n', 'core.dk')
        app = self.write('App #{ Noop() {} }\nT{\n AppTest() { T.AreEqual(Core.One(), 1) }\n}\n', 'app.dk')
        core_ir, app_ir = self.root / 'core.ll', self.root / 'app.ll'
        core_o, app_o = self.root / 'core.o', self.root / 'app.o'
        self.duckc('--library', core, '-o', core_ir)
        self.duckc('--test', '--dependency', core, app, '-o', app_ir)
        for ir, obj in ((core_ir, core_o), (app_ir, app_o)):
            subprocess.run([options.clang, '-Wno-override-module', '-c', str(ir), '-o', str(obj)], check=True, timeout=60)
        exe = self.root / 'linked.exe'
        subprocess.run([options.clang, str(app_o), str(core_o), '-o', str(exe)], check=True, timeout=60)
        self.assertEqual(self.execute(exe), (0, 'RUN  App.AppTest\nPASS App.AppTest\n1 passed, 0 failed, 1 total\n'))

    def test_library_and_test_modes_conflict(self):
        self.duckc('--library', '--test', self.write(self.SAMPLE), '-o', self.root / 'x.ll', success=False)

    def test_blocks_attach_anywhere_in_a_chain(self):
        status, output = self.run_tests('''Example
{
    int Foo()
    {
        return 3
    }
}
#{
    int Foo2()
    {
        return 5
    }
}
T{
    Test1()
    {
        T.AreEqual(Example.Foo2(), 5)
    }

    Test2()
    {
        Example e()
        T.IsTrue(e.Foo() == 3)
    }
}
Later {
    int Value = 7
}
T{
    Field() {
        Later l
        T.AreEqual(l.Value, 7)
    }
}
#{
    int After() { return 2 }
}
T{
    Second() { T.AreEqual(After(), 2) }
}
''')
        self.assertEqual(status, 0, output)
        self.assertEqual(output.splitlines()[-1], '4 passed, 0 failed, 4 total')
        self.assertIn('PASS Later.Second', output)

    def test_nested_containers_qualify_test_names(self):
        exe = self.runner('Outer #{\n Inner #{ int Two() { return 2 } }\n T{\n  Deep() { T.AreEqual(Two(), 2) }\n }\n}\n')
        self.assertEqual(self.execute(exe, '--list'), (0, 'Outer.Inner.Deep\n'))
        self.assertEqual(self.execute(exe)[0], 0)

    def test_tests_reach_protected_members_and_statics(self):
        self.passes('T.AreEqual(Secret(), 9)\nT.AreEqual(Count, 42)',
                    setup='Box #{\n int Count = 40 + 2\n -int Secret() { return 9 }\n}\n')

    def test_each_assertion_passes_and_fails(self):
        self.passes('T.AreEqual(2 + 2, 4)\nT.AreNotEqual(2 + 2, 5)\nT.IsTrue(1 < 2)\nT.IsFalse(2 < 1)')
        self.fails('T.AreEqual(2 + 2, 5)', 'T.AreEqual failed', '  actual:   4\n  expected: 5\n')
        self.fails('T.AreNotEqual(2 + 2, 4)', 'T.AreNotEqual failed', '  both:     4\n')
        self.fails('T.IsTrue(2 < 1)', 'T.IsTrue failed')
        self.fails('T.IsFalse(1 < 2)', 'T.IsFalse failed')
        self.fails('T.Fail()', 'T.Fail\n')
        self.fails('T.Fail("the reason")', 'T.Fail\n', '  message:  the reason\n')

    def test_first_failure_ends_the_test(self):
        self.assertNotIn('unreached', self.fails('T.Fail()\nPrint("unreached")'))
        self.assertNotIn('unreached', self.fails('T.AreEqual(1, 2)\nPrint("unreached")'))

    def test_operands_are_evaluated_once(self):
        output = self.fails('T.AreEqual(Next(), 5)', '  actual:   1\n',
                            setup='Box #{\n int Calls = 0\n int Next() { Calls += 1\nPrint(Calls)\nreturn Calls }\n}\n')
        self.assertIn('RUN  Box.Check\n1\n', output)
        self.assertNotIn('\n2\n', output)

    def test_values_of_each_printable_kind_compare_and_report(self):
        setup = 'Hue : int #@{ Red, Green }\nBox #{\n string Hi() { return "hi" }\n name Nm() { return "nm" }\n big Huge() { return 5 }\n}\n'
        self.passes('T.AreEqual(Hi(), "hi")\nT.AreEqual(Nm(), "nm")\nT.AreEqual(Huge(), 5)\nT.AreEqual(Hue.Red, Hue.Red)\n'
                    'T.AreEqual(1.5, 1.5)\nT.AreEqual(true, 1 == 1)', setup=setup)
        self.fails('T.AreEqual(Hi(), "yo")', '  actual:   hi\n  expected: yo\n', setup=setup)
        self.fails('T.AreEqual(Hue.Red, Hue.Green)', '  actual:   Red\n  expected: Green\n', setup=setup)
        self.fails('T.AreEqual(Huge(), 6)', '  actual:   5\n  expected: 6\n', setup=setup)
        self.fails('T.AreEqual(1.5, 2.5)', '  actual:   1.5\n  expected: 2.5\n', setup=setup)

    def test_comparison_accepts_what_equality_accepts(self):
        self.passes('uint count = 5\nbyte small = 3\nfloat ratio = 1.5\nT.AreEqual(count, 5)\nT.AreEqual(small, 3)\n'
                    'T.AreEqual(5, count)\nT.AreEqual(ratio, 1.5)')
        self.rejected('Box #{ Noop() {} }\nT{\n Check() { uint64 wide = 5\nT.AreEqual(wide, 5) }\n}\n',
                      'mixed signed and unsigned')

    def test_pointers_compare_without_being_shown(self):
        setup = 'Item { int V }\nBox #{ Noop() {} }\n'
        self.passes('Item* p()\nT.AreNotEqual(p, null)\nT.IsTrue(p)', setup=setup)
        output = self.fails('Item* p()\nT.AreEqual(p, null)', 'T.AreEqual failed', setup=setup)
        self.assertNotIn('actual', output)

    def test_incomparable_values_are_compile_errors(self):
        self.rejected('Item { int V }\nBox #{ Noop() {} }\nT{\n Check() { Item a\nItem b\nT.AreEqual(a, b) }\n}\n', 'error')

    def test_assertions_work_inside_control_flow_and_macros(self):
        self.fails('for i : 0...3 {\n if i == 2 T.Fail("two")\n}', '  message:  two\n')
        self.passes('Switch (3) {\n case 3:\n  T.IsTrue(true)\n  break\n default:\n  T.Fail()\n  break\n}')
        self.fails('Same(1, 2)', 'T.AreEqual failed',
                   setup='macro Same(int a, int b) { T.AreEqual(a, b) }\nBox #{ Noop() {} }\n')

    def test_failed_assertion_runs_scope_cleanup(self):
        setup = 'Item {\n ~Item() { Print("released") }\n}\nBox #{ Noop() {} }\n'
        output = self.fails('Item* p()\nT.Fail()', setup=setup)
        self.assertLess(output.index('T.Fail'), output.index('released'))
        self.assertLess(output.index('released'), output.index('FAIL Box.Check'))

    def test_return_ends_a_test_as_passing(self):
        self.passes('if true { return }\nT.Fail()')

    def test_scheduled_calls_join_before_assertions(self):
        setup = 'Box #{\n int Slow(int n) { int t = 0\nfor i : 0...n { t += i }\nreturn t }\n}\n'
        self.passes('a : Slow(100)\nb : Slow(10)\nT.AreEqual(a, 5050)\nT.AreEqual(b, 55)', setup=setup)

    def test_program_may_declare_runner_symbols(self):
        status, output = self.run_tests('extern "C" int strcmp(cstring a, cstring b)\nBox #{ Noop() {} }\nT{\n Check() { T.AreEqual(strcmp("a", "a"), 0) }\n}\n')
        self.assertEqual(status, 0, output)

    def test_trap_leaves_the_failing_test_visible(self):
        status, output = self.run_tests(
            'Box #{ Noop() {} }\nT{\n A() { }\n Crash() {\n int [3] xs\n int i = 3\n Print(xs[i])\n }\n Later() { }\n}\n')
        self.assertNotIn(status, (0, 1, 2))
        self.assertTrue(output.endswith('RUN  Box.Crash\n'), output)

    def test_user_container_named_t_still_works(self):
        ir = self.root / 'container.ll'
        path = self.write('Box {\n int V\n}\nT {\n int Field = 3\n}\nT2 #{ Noop() {} }\nMain() { T t\nPrint(t.Field) }\n')
        self.duckc(path, '-o', ir)
        self.assertEqual(self.execute(self.link(ir)), (0, '3\n'))
        static = self.write('T #{ int Five() { return 5 } }\nMain() { Print(T.Five()) }\n')
        self.duckc(static, '-o', ir)
        self.assertEqual(self.execute(self.link(ir)), (0, '5\n'))

    def test_user_container_t_alongside_test_block(self):
        status, output = self.run_tests(
            'T #{ int Five() { return 5 } }\nBox #{ Noop() {} }\nT{\n Check() { T.AreEqual(T.Five(), 5) }\n}\n')
        self.assertEqual(status, 0, output)

    def test_macros_in_test_blocks_are_collected(self):
        self.assertEqual(self.run_tests('Box #{ Noop() {} }\nT{\n macro Same() { T.IsTrue(true) }\n Check() { Same() }\n}\n')[0], 0)

    def test_diagnostics(self):
        self.rejected('T{\n Check() { }\n}\n', 'must directly follow a container')
        self.rejected('Box #{ Noop() {} }\n\nT{\n Check() { }\n}\n', 'must be adjacent')
        self.rejected('Box #{ Noop() {} }\nT{\n Check(int a) { }\n}\n', 'takes no parameters')
        self.rejected('Box #{ Noop() {} }\nT{\n int Check() { return 1 }\n}\n', 'holds only tests')
        self.rejected('Box #{ Noop() {} }\nT{\n int Field\n}\n', 'holds only tests')
        self.rejected('Box #{ Noop() {} }\nT{\n Check() { }\n Check() { }\n}\n', "test 'Box.Check' is already declared")
        self.rejected('Box #{ Noop() {} }\nT{\n Check() { }\n}\n#{ Other() {} }\nT{\n Check() { }\n}\n', 'already declared')
        self.rejected('Box #{ Noop() {} }\nT{\n Check() { T.Equal(1, 1) }\n}\n', "unknown assertion 'T.Equal'")
        self.rejected('Box #{ Noop() {} }\nT{\n Check() { T.AreEqual(1) }\n}\n', 'expects 2 arguments')
        self.rejected('Box #{ Noop() {} }\nT{\n Check() { T.IsTrue() }\n}\n', 'expects 1 argument')
        self.rejected('Box #{ Noop() {} }\nT{\n Check() { f : @() { T.Fail() } }\n}\n', 'only directly inside a test')
        self.rejected('Main() { T.AreEqual(1, 1) }\n', 'only directly inside a test')


if __name__ == '__main__':
    unittest.main(argv=[__file__, *rest])
