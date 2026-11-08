"""Named and loop-scoped lifetimes: [@Name], @start, @end, and if @Name."""
import argparse
from pathlib import Path
import subprocess
import tempfile
import unittest

parser = argparse.ArgumentParser()
parser.add_argument('--compiler', required=True)
parser.add_argument('--clang', required=True)
options, rest = parser.parse_known_args()

REPORT = '''Scope #{
    Show() {
        if @Tick
            Print("active")
        else
            Print("inactive")
    }
}
Item {
    int Id
    ~Item() { Print("released") }
}
'''


class Lifetimes(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='duck lifetimes ', ignore_cleanup_errors=True)
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

    def run_program(self, source, level='-O0', *flags):
        ir, exe = self.root / 'main.ll', self.root / 'main.exe'
        self.duckc(*flags, self.write(source), '-o', ir)
        subprocess.run([options.clang, '-Wno-override-module', level, str(ir), '-o', str(exe)], check=True,
                       capture_output=True, timeout=60)
        result = subprocess.run([str(exe)], capture_output=True, timeout=60)
        return result.returncode, result.stdout.decode().replace('\r\n', '\n')

    def output(self, source):
        results = {self.run_program(REPORT + source, level) for level in ('-O0', '-O2')}
        results.add(self.run_program(REPORT + source, '-O0', '--sequential'))
        self.assertEqual(len(results), 1, results)
        status, text = results.pop()
        self.assertEqual(status, 0, text)
        return text

    def traps(self, source):
        status, _ = self.run_program(REPORT + source)
        self.assertNotIn(status, (0, 1), 'expected a trap')

    def rejected(self, source, message):
        self.assertIn(message, self.duckc(self.write(REPORT + source, 'bad.dk'), '-o', self.root / 'bad.ll', success=False))

    SESSION = '''Session {
    [@Play]
    float Time
    [@Play]
    string Who
    Add(float d) { ?Time += d }
    Read() { Print(Time) }
    Peek() { Print(?Time) }
}
'''

    def test_start_end_and_query(self):
        self.assertEqual(self.output('Main() {\n @start Tick\n Scope.Show()\n @end Tick\n Scope.Show()\n}\n'), 'active\ninactive\n')

    def test_guarded_writes_and_reads_skip_when_inactive(self):
        self.assertEqual(self.output(self.SESSION + '''Main() {
    Session s
    s.Add(1.5)
    s.Peek()
    @start Play
    s.Add(1.5)
    s.Read()
    s.Peek()
    @end Play
    s.Add(1.5)
    s.Peek()
}
'''), '0\n1.5\n1.5\n0\n')

    def test_unguarded_use_outside_the_scope_traps(self):
        self.traps(self.SESSION + 'Main() {\n Session s\n s.Read()\n}\n')

    def test_each_start_gives_fresh_variables(self):
        self.assertEqual(self.output(self.SESSION + '''Main() {
    Session s
    @start Play
    s.Add(2.0)
    s.Who = "bob"
    Print(s.Who)
    @end Play
    @start Play
    s.Read()
    Print(s.Who + "|")
}
'''), 'bob\n0\n|\n')

    def test_a_repeated_start_reinitializes_and_stays_active(self):
        self.assertEqual(self.output(self.SESSION + '''Main() {
    Session s
    @start Play
    s.Add(2.0)
    @start Play
    s.Read()
    if @Play
        Print("still")
}
'''), '0\nstill\n')

    def test_a_second_end_is_a_runtime_error(self):
        self.traps('Main() {\n @start Tick\n @end Tick\n @end Tick\n}\n')
        self.traps('Main() {\n @end Tick\n}\n')

    def test_instances_created_after_start_join_the_scope(self):
        self.assertEqual(self.output(self.SESSION + '''Main() {
    @start Play
    Session a
    Session b
    a.Add(1.0)
    b.Add(2.0)
    a.Read()
    b.Read()
}
'''), '1\n2\n')

    def test_end_cleans_up_scoped_values_while_still_readable(self):
        self.assertEqual(self.output('''Box {
    [@Play]
    Item Held
}
Main() {
    Box b
    @start Play
    b.Held.Id = 4
    Print(b.Held.Id)
    @end Play
    Print("after")
}
'''), '4\nreleased\nafter\n')

    def test_a_variable_never_used_in_a_generation_is_not_built(self):
        self.assertEqual(self.output('Box {\n [@Play]\n Item Held\n}\nMain() {\n @start Play\n @end Play\n Print("done")\n}\n'), 'done\n')

    def test_static_scoped_fields_and_collections(self):
        self.assertEqual(self.output('''Manager #{
    [@Play]
    int [] Scores
    [@Play]
    int Count
    Add(int s) {
        Scores += s
        Count += 1
    }
}
Main() {
    @start Play
    Manager.Add(4)
    Manager.Add(5)
    Print(Manager.Scores.Len)
    Print(Manager.Scores[1])
    Print(Manager.Count)
    @end Play
    @start Play
    Print(Manager.Scores.Len)
    Print(Manager.Count)
}
'''), '2\n5\n2\n0\n0\n')

    def test_a_copied_container_shares_scoped_values(self):
        self.assertEqual(self.output('''Box {
    [@Play]
    int V
}
Main() {
    @start Play
    Box a
    a.V = 3
    Box b = a
    b.V = 4
    Print(a.V)
}
'''), '4\n')

    def test_scopes_close_at_program_exit(self):
        self.assertEqual(self.output('''Box {
    [@Play]
    Item Held
}
Main() {
    Box b
    @start Play
    b.Held.Id = 1
    Print("end of main")
}
'''), 'end of main\nreleased\n')

    def test_loops_run_each_iteration_in_a_fresh_scope(self):
        self.assertEqual(self.output('''Box {
    [@Tick]
    int V
}
Main() {
    Box b
    [@Tick]
    for i : 0...2 {
        b.V += 1
        Print(b.V)
    }
    Scope.Show()
}
'''), '1\n1\n1\ninactive\n')

    def test_every_loop_form(self):
        self.assertEqual(self.output('''Main() {
    int n = 0
    [@Tick]
    while n < 1 {
        n += 1
        Scope.Show()
    }
    [@Tick]
    for int i = 0; i < 1; i++ {
        Scope.Show()
    }
    [@Tick]
    do {
        Scope.Show()
    } while n < 0
    int [] xs = {1}
    [@Tick]
    for x : xs {
        Scope.Show()
    }
    Scope.Show()
}
'''), 'active\nactive\nactive\nactive\ninactive\n')

    def test_conditions_are_outside_the_iteration(self):
        self.assertEqual(self.output('''Check #{
    bool Once = true
    bool Go() {
        if @Tick
            Print("in")
        else
            Print("out")
        bool was = Once
        Once = false
        return was
    }
}
Main() {
    [@Tick]
    while Check.Go() {
        Print("body")
    }
}
'''), 'out\nbody\nout\n')

    def test_break_continue_and_return_each_close_once(self):
        self.assertEqual(self.output('''Box {
    [@Tick]
    Item Held
}
F #{
    int Early(Box& b) {
        [@Tick]
        while true {
            b.Held.Id = 1
            return 7
        }
        return 0
    }
}
Main() {
    Box b
    [@Tick]
    while true {
        b.Held.Id = 1
        break
    }
    Print("a")
    [@Tick]
    for i : 0...1 {
        b.Held.Id = 1
        continue
    }
    Print("b")
    Print(F.Early(b))
    Scope.Show()
}
'''), 'released\na\nreleased\nreleased\nb\nreleased\n7\ninactive\n')

    def test_end_in_the_loop_body_escapes_it(self):
        self.assertEqual(self.output('''Main() {
    [@Tick]
    while true {
        Print("body")
        @end Tick
        Print("unreachable")
    }
    Print("after")
    Scope.Show()
}
'''), 'body\nafter\ninactive\n')

    def test_end_in_a_callee_escapes_to_the_loop(self):
        self.assertEqual(self.output('''Deep #{
    Inner() {
        Item local
        @end Tick
        Print("not inner")
    }
    int Middle() {
        Inner()
        Print("not middle")
        return 3
    }
    Top() {
        x : Middle()
        Print(x)
    }
}
Main() {
    [@Tick]
    while true {
        Deep.Top()
        Print("not loop")
    }
    Print("after")
}
'''), 'released\nafter\n')

    def test_end_of_an_outer_loop_closes_inner_ones(self):
        self.assertEqual(self.output('''Stop #{ Now() { @end Outer } }
Main() {
    [@Outer]
    while true {
        [@Tick]
        while true {
            Stop.Now()
            Print("not inner")
        }
        Print("not outer")
    }
    Scope.Show()
    if @Outer
        Print("outer active")
    Print("after")
}
'''), 'inactive\nafter\n')

    def test_end_of_an_inner_loop_keeps_the_outer_running(self):
        self.assertEqual(self.output('''Main() {
    int n = 0
    [@Outer]
    while n < 2 {
        n += 1
        [@Tick]
        while true {
            @end Tick
        }
        if @Outer
            Print(n)
    }
}
'''), '1\n2\n')

    def test_end_without_a_loop_continues(self):
        self.assertEqual(self.output('''S #{ Stop() { @end Tick
 Print("continued") } }
Main() {
    @start Tick
    S.Stop()
    Scope.Show()
}
'''), 'continued\ninactive\n')

    def test_end_after_the_loop_finished_is_an_error(self):
        self.traps('Main() {\n [@Tick]\n for i : 0...1 { }\n @end Tick\n}\n')

    def test_starting_a_scope_during_its_loop_does_not_outlive_it(self):
        self.assertEqual(self.output('''Main() {
    [@Tick]
    while true {
        @start Tick
        break
    }
    Scope.Show()
}
'''), 'inactive\n')

    def test_end_through_a_duck_call_and_lambda(self):
        self.assertEqual(self.output('''Thing { Stop() { @end Tick } }
Main() {
    Thing t
    [@Tick]
    while true {
        t..Stop()
        Print("not after duck")
    }
    Print("a")
    func f = @() { S.Stop() }
    [@Tick]
    while true {
        f()
        Print("not after lambda")
    }
    Print("b")
}
S #{ Stop() { @end Tick } }
'''), 'a\nb\n')

    def test_scoped_functions_are_never_tasks(self):
        self.assertEqual(self.output('''W #{
    int Pure(int n) { return n + 1 }
    int Touches(int n) {
        if @Tick
            return n + 2
        return n
    }
}
Main() {
    @start Tick
    a : W.Touches(1)
    b : W.Pure(1)
    Print(a + b)
}
'''), '5\n')

    def test_a_scope_ended_in_another_module_escapes_the_loop(self):
        core = self.write('Stop #{ Now() {\n @end Tick\n Print("not core")\n} }\n', 'core.dk')
        middle = self.write('Mid #{ Call() {\n Stop.Now()\n Print("not middle")\n} }\n', 'middle.dk')
        app = self.write('Main() {\n [@Tick]\n while true {\n Mid.Call()\n Print("not app")\n }\n Print("after")\n}\n', 'app.dk')
        irs = [self.root / 'core.ll', self.root / 'middle.ll', self.root / 'app.ll']
        self.duckc('--library', core, '-o', irs[0])
        self.duckc('--library', '--dependency', core, middle, '-o', irs[1])
        self.duckc('--dependency', middle, app, '-o', irs[2])
        exe = self.root / 'linked.exe'
        subprocess.run([options.clang, '-Wno-override-module', *map(str, irs), '-o', str(exe)], check=True,
                       capture_output=True, timeout=60)
        result = subprocess.run([str(exe)], capture_output=True, timeout=60)
        self.assertEqual((result.returncode, result.stdout.decode().replace('\r\n', '\n')), (0, 'after\n'))

    def test_scope_state_is_shared_between_modules(self):
        core = self.write('Box #{\n [@Play]\n int V\n Show() {\n if @Play\n Print(V)\n}\n}\n', 'core.dk')
        app = self.write('Main() {\n @start Play\n Box.V = 6\n Box.Show()\n}\n', 'app.dk')
        irs = [self.root / 'core.ll', self.root / 'app.ll']
        self.duckc('--library', core, '-o', irs[0])
        self.duckc('--dependency', core, app, '-o', irs[1])
        exe = self.root / 'linked.exe'
        subprocess.run([options.clang, '-Wno-override-module', *map(str, irs), '-o', str(exe)], check=True,
                       capture_output=True, timeout=60)
        result = subprocess.run([str(exe)], capture_output=True, timeout=60)
        self.assertEqual((result.returncode, result.stdout.decode().replace('\r\n', '\n')), (0, '6\n'))

    def test_marked_loops_work_at_file_scope(self):
        path = self.write('[@Tick]\nwhile true {\n break\n}\nPrint(1)\n', 'script.dk')
        main = self.write('Main() { Exec("script.dk") }\n')
        ir, exe = self.root / 'main.ll', self.root / 'main.exe'
        self.duckc(main, path, '-o', ir)
        subprocess.run([options.clang, '-Wno-override-module', str(ir), '-o', str(exe)], check=True, capture_output=True, timeout=60)
        result = subprocess.run([str(exe)], capture_output=True, timeout=60)
        self.assertEqual((result.returncode, result.stdout.decode().replace('\r\n', '\n')), (0, '1\n'))

    def test_lambdas_are_still_written_with_at(self):
        self.assertEqual(self.output('Main() {\n func int() f = @int() { return 4 }\n Print(f())\n}\n'), '4\n')

    def test_diagnostics(self):
        self.rejected('Box {\n [@Play]\n int V = 3\n}\nMain() { }\n', 'starts at its type')
        self.rejected('Box {\n [@Play]\n int* V\n}\nMain() { }\n', 'holds a value, not a pointer')
        self.rejected('Box {\n [@Play]\n Go() { }\n}\nMain() { }\n', 'not a method')
        self.rejected('Built {\n Built() { }\n}\nBox {\n [@Play]\n Built V\n}\nMain() { }\n', 'cannot live in a named scope')
        self.rejected('Main() {\n [@Tick]\n int x = 1\n}\n', 'applies to a loop')
        self.rejected('Main() {\n func f = @() { @end Tick }\n}\n', 'a lambda body cannot do')
        self.rejected('Main() {\n int a = 1\n ?a += 1\n}\n', 'or a scoped variable')
        self.rejected('Main() {\n int a = 1\n Print(?a)\n}\n', 'or a scoped variable')


if __name__ == '__main__':
    unittest.main(argv=[__file__, *rest])
