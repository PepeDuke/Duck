"""Resource effects across calls, ordered ${ } blocks, and collection size hints."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

parser = argparse.ArgumentParser()
parser.add_argument('--compiler', required=True)
parser.add_argument('--clang', required=True)
options, rest = parser.parse_known_args()

REPOSITORY = Path(__file__).resolve().parent.parent

THREADS = '''extern "C" void* CreateThread(void* security, long stack, void* start, void* argument, int flags, void* id)
extern "C" int WaitForSingleObject(void* handle, int milliseconds)
extern "C" int CloseHandle(void* handle)
Race(void* a, void* b) {
    WaitForSingleObject(a, -1)
    WaitForSingleObject(b, -1)
    CloseHandle(a)
    CloseHandle(b)
}
'''


class Ordered(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='duck ordered ', ignore_cleanup_errors=True)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def write(self, source, name='main.dk'):
        path = self.root / name
        path.write_text(source, encoding='utf-8')
        return path

    def duckc(self, *args, success=True):
        result = subprocess.run([options.compiler, *map(str, args)], capture_output=True, timeout=60)
        self.assertEqual(result.returncode == 0, success, result.stderr.decode(errors='replace'))
        return result.stderr.decode(errors='replace')

    def ir(self, source, *flags):
        output = self.root / 'ir.ll'
        self.duckc(*flags, self.write(source), '-o', output)
        return output.read_text(encoding='utf-8')

    def build(self, source, level, *flags):
        ir, exe = self.root / f'main{level}{"".join(flags)}.ll', self.root / f'main{level}{"".join(flags)}.exe'
        self.duckc(*flags, self.write(source), '-o', ir)
        subprocess.run([options.clang, '-Wno-override-module', level, str(ir), '-o', str(exe)], check=True,
                       capture_output=True, timeout=60)
        return exe

    def run_exe(self, exe, workers=None):
        env = dict(os.environ)
        if workers is not None:
            env['DUCK_WORKERS'] = str(workers)
        result = subprocess.run([str(exe)], capture_output=True, timeout=60, env=env)
        return result.returncode, result.stdout.decode().replace('\r\n', '\n')

    def check(self, source, expected):
        """Every schedule of a program must print the same thing."""
        for level in ('-O0', '-O2'):
            for flags in ((), ('--sequential',)):
                exe = self.build(source, level, *flags)
                for workers in (1, 4, None):
                    status, text = self.run_exe(exe, workers)
                    self.assertEqual((status, text), (0, expected), f'{level} {flags} workers={workers}')

    def example(self, path, expected):
        self.check((REPOSITORY / 'Examples' / path).read_text(encoding='utf-8'), expected)

    def rejected(self, source, diagnostic):
        self.assertIn(diagnostic, self.duckc(self.write(source), '-o', self.root / 'x.ll', success=False))

    def entered(self, source):
        return re.findall(r'call i32 @duck\.lock\$enter\(i64 [^,]+, i1 (true|false)\)', self.ir(source))

    # Examples ---------------------------------------------------------------
    def test_existing_concurrency_examples(self):
        self.example('Concurrency/01_ReadWaitsForWork.dk', '2\n')
        self.example('Concurrency/02_AtomicWithdrawal.dk', '0\n')
        self.example('Concurrency/03_LockIncludesCallees.dk', '2\n3\n')
        self.example('Concurrency/04_OrderedTransfers.dk', '10\n0\n')

    def test_task_examples(self):
        self.example('Concurrency/05_TasksWaitForConflicts.dk', '2500\n2500\n2501\n')
        self.example('Concurrency/06_EffectsThroughReferences.dk', '1\n10000\n20000\n')

    def test_block_examples(self):
        self.example('Concurrency/07_NestedOrderedBlocks.dk', '15\n0\n15\n')
        self.example('Concurrency/08_UnknownEffectsExcludeAll.dk', '42\n6\n')
        self.example('Concurrency/09_ExitsReleaseLocks.dk', '3\n2\n8\n')

    def test_racing_example(self):
        self.example('Concurrency/10_RacingThreads.dk', '40000\n')

    def test_size_hint_examples(self):
        self.example('Collections/06_SizeHints.dk', '100\n4950\n3\n1\n')
        self.example('Collections/07_SizeHintsGuideScheduling.dk', '3\n8\n0\n')

    def test_zero_size_hint_rejected(self):
        source = (REPOSITORY / 'Examples/FailureCases/39_ZeroSizeHint.dk').read_text(encoding='utf-8')
        self.rejected(source, 'a size hint needs at least one element')

    # Conflicts --------------------------------------------------------------
    def test_conflicting_tasks_run_in_order(self):
        # Doubling then adding gives a different answer in the other order.
        self.check('''Cell { int Value = 1
    Double(int rounds) { for i : 1...rounds { Value = Value % 1000 + Value % 1000 } }
    Add(int rounds) { for i : 1...rounds { Value += 3 } }
    int Get() { return Value } }
Main() {
    Cell c()
    c.Double(3000)
    c.Add(3000)
    c.Double(3000)
    x : c.Get()
    Print(x)
}''', self.sequential_answer())

    @staticmethod
    def sequential_answer():
        value = 1
        for _ in range(3000):
            value = value % 1000 + value % 1000
        value += 9000
        for _ in range(3000):
            value = value % 1000 + value % 1000
        return f'{value}\n'

    def test_references_map_to_the_caller(self):
        ir = self.ir('''Add(int& n, int c) { for i : 1...c { n++ } }
Main() {
    int a = 0
    int b = 0
    Add(a, 2000)
    Add(b, 2000)
    Add(a, 2000)
    Print(a + b)
}''')
        self.assertGreaterEqual(ir.count('call ptr @duck.task$spawn'), 2)

    def test_static_write_waits(self):
        self.check('''Game #{ int Score = 1 }
Scale(int n) { for i : 1...n { Game.Score = Game.Score * 3 % 1000 } }
Grow(int n) { for i : 1...n { Game.Score = Game.Score + 7 } }
Main() {
    Scale(2500)
    Grow(2500)
    Scale(2500)
    Print(Game.Score)
}''', self.static_answer())

    @staticmethod
    def static_answer():
        score = 1
        for _ in range(2500):
            score = score * 3 % 1000
        score += 7 * 2500
        for _ in range(2500):
            score = score * 3 % 1000
        return f'{score}\n'

    # A static's container name is not storage, so the checks on a reference
    # must stop at the static instead of walking on to it.
    def test_static_by_reference(self):
        self.check('''Game #{ int Score = 1 }
Step(int& n) { n = n + 1 }
Main() {
    Step(Game.Score)
    Print(Game.Score)
}''', '2\n')

    def test_static_record_and_its_field_by_reference(self):
        self.check('''Holder { int Value = 1 }
S #{ Holder H }
Whole(Holder& h) { h.Value = h.Value + 1 }
Part(int& n) { n = n * 10 }
Main() {
    Whole(S.H)
    Part(S.H.Value)
    Print(S.H.Value)
}''', '20\n')

    def test_address_of_static(self):
        self.check('''Game #{ int Score = 1 }
Main() {
    int* p = &Game.Score
    *p = 9
    Print(Game.Score)
}''', '9\n')

    def test_scoped_static_by_reference(self):
        self.check('''Game
#{
    [@Play]
    int Score
}
Step(int& n) { n = n + 1 }
Main() {
    @start Play
    Game.Score = 4
    Step(Game.Score)
    Print(Game.Score)
    @end Play
}''', '5\n')

    def test_constant_static_by_reference_is_rejected(self):
        self.rejected('''Game #{ #int Score = 1 }
Step(int& n) { n = n + 1 }
Main() { Step(Game.Score) }''', 'cannot pass a constant to a mutable reference')

    def test_write_through_static_reference_waits(self):
        # Reordering any of the three calls changes the answer.
        self.check('''Game #{ int Score = 1 }
Add(int& t, int n) { for i : 1...n { t++ } }
Scale(int n) { for i : 1...n { Game.Score = Game.Score * 3 % 1000 } }
Main() {
    Add(Game.Score, 5000)
    Scale(5000)
    Add(Game.Score, 5000)
    Print(Game.Score)
}''', '5001\n')

    def test_unknown_native_effects_are_a_barrier(self):
        ir = self.ir('''extern "C" int _flushall()
Spin(int n) { int t = 0
    for i : 1...n { t += i } }
Main() {
    Spin(5000)
    Spin(5000)
    _flushall()
    Print(1)
}''')
        body = ir[ir.index('define void @duck.Main'):]
        native = body.index('call i32 @_flushall')
        self.assertLess(body.index('call void @duck.task$join'), native)

    def test_task_uses_the_receiver_and_its_table(self):
        self.check('''Base { int N = 0
    Run(int c) { for i : 1...c { N++ } } }
Derived : Base { int M = 0 }
Main() {
    Derived d()
    Derived e()
    d.Run(4000)
    e.Run(4000)
    Print(d.N + e.N)
}''', '8000\n')

    # Ordered blocks ---------------------------------------------------------
    def race(self, setup, body, expected):
        """Runs `body` in a loop on two native threads."""
        self.check(THREADS + setup + '''
int Worker(void* unused) {
    for i : 1...15000 {
        ''' + body + '''
    }
    return 0
}
Main() {
    first : CreateThread(null, 0, &Worker, null, 0, null)
    second : CreateThread(null, 0, &Worker, null, 0, null)
    Race(first, second)
    Print(Show())
}''', expected)

    def test_named_blocks_exclude_each_other(self):
        self.race('S #{ int Total = 0 }\nint Show() { return S.Total }',
                  '${ int seen = S.Total\n S.Total = seen + 1 }', '30000\n')

    def test_exclusive_blocks_exclude_each_other(self):
        self.race('Holder { int [2] Slots }\nS #{ Holder H }\nint Show() { return S.H.Slots[1] }',
                  '${ int seen = S.H.Slots[1]\n S.H.Slots[1] = seen + 1 }', '30000\n')

    def test_named_and_exclusive_blocks_exclude_each_other(self):
        # One thread names the static, the other locks through unknown native code.
        self.check(THREADS + '''extern "C" int abs(int v)
S #{ int Total = 0 }
int Named(void* unused) { for i : 1...15000 { ${ int seen = S.Total
    S.Total = seen + 1 } }
    return 0 }
int Blind(void* unused) { for i : 1...15000 { ${ int seen = abs(S.Total)
    S.Total = seen + 1 } }
    return 0 }
Main() {
    a : CreateThread(null, 0, &Named, null, 0, null)
    b : CreateThread(null, 0, &Blind, null, 0, null)
    Race(a, b)
    Print(S.Total)
}''', '30000\n')

    def test_locks_are_released_by_every_exit(self):
        self.race('''S #{ int Total = 0 }
int Show() { return S.Total }
int Bump() { ${ S.Total++
    return S.Total } }
int Skip(int n) { for j : 1...2 { ${ if j == 1 { continue }
    if n < 0 { break }
    S.Total++ } }
    return 0 }''', 'Bump()\nSkip(1)', '60000\n')

    def test_callee_locks_are_part_of_the_block(self):
        self.race('''S #{ int Total = 0 }
Step() { int seen = S.Total
    S.Total = seen + 1 }
int Show() { return S.Total }''', '${ Step() }', '30000\n')

    def test_block_through_a_reference_locks_the_object(self):
        ir = self.ir('''Cell { int N = 0 }
Touch(Cell& c) { ${ c.N++ } }
Main() { Cell c()
    Touch(c)
    Print(c.N) }''')
        touch = ir[ir.index('define void @duck.Touch'):]
        self.assertRegex(touch, r'(?s)getelementptr %struct\.Cell, ptr %arg0.*?call i64 @duck\.lock\$bit\(.*?lock\$enter')

    def test_block_with_only_private_locals_takes_no_lock(self):
        self.assertEqual(self.entered('Main() { ${ int a = 1\n Print(a) } }'), [])

    def test_named_block_is_not_exclusive(self):
        self.assertEqual(self.entered('S #{ int T }\nMain() { ${ S.T = 1 }\n Print(S.T) }'), ['false'])

    def test_unknown_callee_is_exclusive(self):
        self.assertEqual(self.entered('extern "C" int abs(int v)\nMain() { ${ Print(abs(-1)) } }'), ['true'])

    def test_overridable_call_is_exclusive(self):
        self.assertEqual(self.entered('''Animal { int N
    Speak() { N++ }
    Say() { ${ Speak() } } }
Cat : Animal { Speak() { N += 2 } }
Main() { Cat c()
    c.Say()
    Print(c.N) }'''), ['true'])

    def test_lambda_body_block_is_exclusive(self):
        self.check('''S #{ int T }
Main() {
    f : @ int(int n) { ${ S.T += n
        return S.T } }
    Print(f(2))
    Print(f(3))
}''', '2\n5\n')

    def test_callee_lock_on_block_local_is_exclusive(self):
        self.assertEqual(self.entered('''Cell { int N
    Bump() { ${ N++ } } }
Main() { ${ Cell c()
    c.Bump()
    Print(c.N) } }''').count('true'), 1)

    def test_nested_exclusive_callee_makes_the_block_exclusive(self):
        self.assertEqual(self.entered('''Holder { int [2] A }
S #{ int T
    Holder H }
Touch() { ${ S.H.A[0] = 1 } }
Main() { ${ S.T = 1
    Touch() }
    Print(S.T + S.H.A[0]) }''')[-1], 'true')

    def test_block_contains_no_tasks(self):
        ir = self.ir('''Spin(int n) { int t = 0
    for i : 1...n { t += i } }
Main() { ${ Spin(5000)
    Spin(5000)
    Print(1) } }''')
        self.assertNotIn('call ptr @duck.task$spawn', ir)

    def test_tasks_started_under_a_lock_run_inline(self):
        # Left is a callee of the block; if its task waited for a worker that
        # needs the same lock, this would hang.
        self.check('''S #{ int T = 0 }
Spin(int n) { ${ for i : 1...n { S.T += 1 } } }
Left(int n) { Spin(n)
    Spin(n)
    Print(S.T) }
Main() { ${ Left(5000) }
    Print(S.T) }''', '10000\n10000\n')

    def test_earlier_conflicting_task_finishes_before_entry(self):
        self.check('''S #{ int T = 1 }
Spin(int n) { for i : 1...n { S.T = S.T * 2 % 1000 } }
Main() {
    Spin(4000)
    int unrelated = 1
    ${ S.T += 1 }
    Print(S.T + unrelated - 1)
}''', f'{self.doubled(1, 4000) + 1}\n')

    @staticmethod
    def doubled(value, rounds):
        for _ in range(rounds):
            value = value * 2 % 1000
        return value

    def test_ordered_block_at_compile_time(self):
        self.check('''int Twice(int n) { ${ return n * 2 } }
Main() { int value = #Twice(21)
    Print(value) }''', '42\n')

    def test_dollar_needs_a_block(self):
        self.duckc(self.write('Main() { $ 5 }'), '-o', self.root / 'x.ll', success=False)

    # Size hints -------------------------------------------------------------
    def test_hints_do_not_change_the_type(self):
        self.check('''int Len(int []& a) { return (int)a.Len }
Box { int [4*] Items }
Main() {
    int [1*] a = {1, 2, 3}
    Box b
    b.Items += 9
    b.Items += 8
    Print(Len(a))
    Print(Len(b.Items))
    a = {4}
    Print(a[0])
}''', '3\n2\n4\n')

    def test_hint_cannot_be_mixed_with_a_grid(self):
        self.rejected('Main() { int [*][-] g\n Print(1) }', 'cannot be mixed')

    def test_hint_must_be_a_decimal_literal(self):
        self.rejected('Main() { int [0x10*] g\n Print(1) }', 'size hint must be a decimal integer literal')

    def test_small_hint_stays_on_the_caller(self):
        source = '''int Sum(int []& a) { int t = 0
    for v : a { t += v }
    return t }
Main() {
    int [HINT] a
    x : Sum(a)
    y : Sum(a)
    Print(x + y)
}'''
        self.assertNotIn('call ptr @duck.task$spawn', self.ir(source.replace('HINT', '4*')))
        self.assertIn('call ptr @duck.task$spawn', self.ir(source.replace('HINT', '*')))
        self.assertIn('call ptr @duck.task$spawn', self.ir(source.replace('HINT', '5000*')))


if __name__ == '__main__':
    unittest.main(argv=['ordered', *rest])
