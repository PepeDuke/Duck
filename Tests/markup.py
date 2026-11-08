"""Markup: element trees, component lookup, enumeration, and markup values."""
import argparse
from pathlib import Path
import subprocess
import tempfile
import unittest

parser = argparse.ArgumentParser()
parser.add_argument('--compiler', required=True)
parser.add_argument('--clang', required=True)
options, rest = parser.parse_known_args()

COMMON = '''Label
{
    string Title
    int Age
    Hello() { Print("hello " + Title) }
}
Hat { int Size }
'''


class Markup(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='duck markup ', ignore_cleanup_errors=True)
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

    def run_program(self, source, level='-O0'):
        ir, exe = self.root / 'main.ll', self.root / 'main.exe'
        self.duckc(self.write(source), '-o', ir)
        subprocess.run([options.clang, '-Wno-override-module', level, str(ir), '-o', str(exe)], check=True,
                       capture_output=True, timeout=60)
        result = subprocess.run([str(exe)], capture_output=True, timeout=60)
        return result.returncode, result.stdout.decode().replace('\r\n', '\n')

    def output(self, source):
        results = {self.run_program(COMMON + source, level) for level in ('-O0', '-O2')}
        self.assertEqual(len(results), 1, results)
        status, text = results.pop()
        self.assertEqual(status, 0, text)
        return text

    def traps(self, source):
        status, _ = self.run_program(COMMON + source)
        self.assertNotIn(status, (0, 1), 'expected a trap')

    def rejected(self, source, message):
        self.assertIn(message, self.duckc(self.write(COMMON + source, 'bad.dk'), '-o', self.root / 'bad.ll', success=False))

    def test_children_are_built_with_their_container(self):
        self.assertEqual(self.output('''Panel {
    this{
        <Label Title="Bob" Age=5/>
    }
    Show() {
        Print(this.Label.Title)
        Print(this.Label.Age)
    }
}
Main() {
    Panel a()
    Panel b()
    a.Label.Age = 9
    a.Show()
    b.Show()
}
'''), 'Bob\n9\nBob\n5\n')

    def test_a_field_with_the_tag_name_takes_priority(self):
        self.assertEqual(self.output('''Panel {
    int Label = 4
    this{ <Hat Size=1/> }
}
Main() {
    Panel a()
    Print(a.Label)
}
'''), '4\n')

    def test_the_root_may_be_written_out(self):
        self.assertEqual(self.output('''Panel {
    int Width
    this{
    <Panel Width=3>
        <Hat Size=2/>
    </Panel>
    }
}
Main() {
    Panel a()
    Print(a.Width)
    Print(a.Hat.Size)
}
'''), '3\n2\n')

    def test_attribute_values_are_expressions(self):
        self.assertEqual(self.output('''Panel {
    int Base = 20
    this{
        <Hat Size=(Base + 1)/>
    }
}
Main() {
    Panel a()
    Print(a.Hat.Size)
}
'''), '21\n')

    def test_nesting_builds_a_tree(self):
        self.assertEqual(self.output('''Row { }
Grid {
    this{
        <Row>
            <Hat Size=1/>
            <Hat Size=2/>
        </Row>
        <Row>
            <Hat Size=3/>
        </Row>
    }
    Show() {
        Print(this.Row[0].Hat[1].Size)
        Print(this.Row[1].Hat.Size)
    }
}
Main() {
    Grid g()
    g.Show()
}
'''), '2\n3\n')

    def test_literal_content_becomes_a_typed_value(self):
        self.assertEqual(self.output('''A { }
B { }
Panel {
    this{
        <A>"words"</A>
        <B>42</B>
    }
    Show() {
        Print(this.A.value)
        Print(this.B.value + 1)
    }
}
Main() {
    Panel p()
    p.Show()
}
'''), 'words\n43\n')

    def test_content_can_set_a_declared_value_from_a_variable(self):
        self.assertEqual(self.output('''A { string value }
Make #{
    markup Build(string text) {
        return {
            <A>text</A>
        }
    }
}
Panel {
    this{ Make.Build("given") }
}
Main() {
    Panel p()
    Print(p.A.value)
}
'''), 'given\n')

    def test_escapes_in_literal_values(self):
        self.assertEqual(self.output('''A { }
Panel { this{ <A>"a\\"b"</A> } }
Main() {
    Panel p()
    Print(p.A.value)
}
'''), 'a"b\n')

    def test_duplicates_are_indexed_and_counted(self):
        self.assertEqual(self.output('''Panel {
    this{
        <Label Title="Bob"/>
        <Label Title="Rob"/>
        <Hat Size=7/>
    }
    Show() {
        Print(this.GetChildren().Len)
        Print(this.Label[0].Title)
        Print(this.Label[1].Title)
        Print(this.Hat[0].Size)
    }
}
Main() {
    Panel p()
    p.Show()
}
'''), '3\nBob\nRob\n7\n')

    def test_children_enumerate_in_declaration_order(self):
        self.assertEqual(self.output('''Panel {
    this{
        <Label Title="Bob"/>
        <Hat Size=7/>
        <Label Title="Rob"/>
    }
    Show() {
        for c : this.GetChildren() {
            Print(c.GetName())
            ?c..Hello()
        }
        Print(this.GetChildren()[1].GetName())
    }
}
Main() {
    Panel p()
    p.Show()
}
'''), 'Label\nhello Bob\nHat\nLabel\nhello Rob\nHat\n')

    def test_elements_enumerate_their_own_children(self):
        self.assertEqual(self.output('''Row { }
Panel {
    this{
        <Row><Hat Size=1/><Hat Size=2/></Row>
        <Hat Size=3/>
    }
    Show() {
        for c : this.GetChildren() {
            Print(c.GetChildren().Len)
        }
    }
}
Main() {
    Panel p()
    p.Show()
}
'''), '2\n0\n')

    RETURNED = '''Guy {
    string Title
}
#{
    markup Make(string who) {
        return {
            <Guy Title=who>
                "note"
                <Hat Size=7/>
            </Guy>
        }
    }
}
'''

    def test_returned_markup_transfers_children_in_order(self):
        self.assertEqual(self.output(self.RETURNED + '''Panel {
    this{
        <Hat Size=1/>
        Guy.Make("bob")
        Guy.Make("rob")
    }
    Show() {
        Print(this.GetChildren().Len)
        Print(this.GetChildren()[1].GetName())
        Print(this.Guy[0].value)
        Print(this.Guy[1].Title)
        Print(this.Guy[1].Hat.Size)
        Print(this.Hat.Size)
    }
}
Main() {
    Panel p()
    p.Show()
}
'''), '3\nGuy\nnote\nrob\n7\n1\n')

    def test_markup_values_move_when_spliced(self):
        self.assertEqual(self.output(self.RETURNED + '''Panel {
    this{
        Build()
    }
    markup Build() {
        markup first = { <Hat Size=1/> <Hat Size=2/> }
        Print(first.Len)
        return {
            first
            first
            <Hat Size=3/>
        }
    }
    Show() { Print(this.GetChildren().Len) }
}
Main() {
    Panel p()
    p.Show()
}
'''), '2\n3\n')

    def test_a_markup_value_can_be_passed_along(self):
        self.assertEqual(self.output(self.RETURNED + '''Wrap #{
    markup Twice() {
        return {
            Guy.Make("a")
            Guy.Make("b")
        }
    }
}
Panel { this{ Wrap.Twice() } }
Main() {
    Panel p()
    Print(p.Guy[1].Title)
}
'''), 'b\n')

    def test_children_release_with_their_parent(self):
        self.assertEqual(self.output('''Part {
    int Id
    ~Part() { Print(Id) }
}
Panel {
    this{
        <Part Id=1/>
        <Part Id=2/>
    }
}
Main() {
    {
        Panel p()
        Print("built")
    }
    Print("gone")
}
'''), 'built\n1\n2\ngone\n')

    def test_an_unused_markup_value_releases_its_elements(self):
        self.assertEqual(self.output('''Part {
    ~Part() { Print("released") }
}
Main() {
    {
        markup m = { <Part/> }
        Print(m.Len)
    }
    Print("after")
}
'''), '1\nreleased\nafter\n')

    def test_an_alias_to_a_child_expires_with_its_parent(self):
        self.assertEqual(self.output('''Holder { Hat* Kept }
Main() {
    Holder h
    {
        Panel p()
        h.Kept = p.Hat
        Print(h.Kept.Size)
    }
    Print(h.Kept == null)
}
Panel { this{ <Hat Size=6/> } }
'''), '6\n1\n')

    def test_guarded_index_skips_missing_children(self):
        self.assertEqual(self.output('''Part { Run() { Print("ran") } }
Panel {
    this{ <Part/> }
    Show() {
        ?this.Part[0].Run()
        ?this.Part[5].Run()
        Print("done")
    }
}
Main() {
    Panel p()
    p.Show()
}
'''), 'ran\ndone\n')

    def test_dynamic_lookup_failures_trap(self):
        spliced = '''Make #{ markup Two() { return { <Hat Size=1/> <Hat Size=2/> } } }
Panel { this{ Make.Two() } }
'''
        self.traps(spliced + 'Main() { Panel p()\nPrint(p.Hat.Size) }\n')
        self.traps(spliced + 'Main() { Panel p()\nPrint(p.Hat[2].Size) }\n')
        self.traps(spliced + 'Main() { Panel p()\nPrint(p.Label.Age) }\n')
        self.assertEqual(self.output(spliced + 'Main() { Panel p()\nPrint(p.Hat[1].Size) }\n'), '2\n')

    def test_tags_may_be_qualified_and_nested_containers(self):
        self.assertEqual(self.output('''Ui #{
    Button { int Id }
}
Panel {
    this{ <Ui.Button Id=4/> }
    Show() { Print(this.GetChildren()[0].GetName()) }
}
Main() {
    Panel p()
    p.Show()
}
'''), 'Ui.Button\n')

    def test_markup_stays_usable_as_a_name(self):
        self.assertEqual(self.output('Main() {\n int markup = 3\n Print(markup + 1)\n}\n'), '4\n')

    def test_this_blocks_do_not_change_other_containers(self):
        self.assertEqual(self.output('Plain { int V = 2 }\nMain() {\n Plain a\n Plain b = a\n Print(b.V)\n}\n'), '2\n')

    def test_markup_links_across_modules(self):
        core = self.write(COMMON + 'Panel {\n this{ <Hat Size=8/> }\n}\nMake #{ markup One() { return { <Hat Size=9/> } } }\n', 'core.dk')
        app = self.write('Main() {\n Panel p()\n Print(p.Hat.Size)\n markup m = Make.One()\n Print(m.Len)\n}\n', 'app.dk')
        core_ir, app_ir = self.root / 'core.ll', self.root / 'app.ll'
        self.duckc('--library', core, '-o', core_ir)
        self.duckc('--dependency', core, app, '-o', app_ir)
        exe = self.root / 'linked.exe'
        subprocess.run([options.clang, '-Wno-override-module', str(app_ir), str(core_ir), '-o', str(exe)], check=True,
                       capture_output=True, timeout=60)
        result = subprocess.run([str(exe)], capture_output=True, timeout=60)
        self.assertEqual((result.returncode, result.stdout.decode().replace('\r\n', '\n')), (0, '8\n1\n'))

    def test_imported_containers_keep_their_layout(self):
        core = self.write('Box { }\nHat { int Size }\n', 'core.dk')
        app = self.write('Panel { this{ <Box><Hat Size=1/></Box> } }\nMain() { }\n', 'app.dk')
        self.assertIn('defined in another module', self.duckc('--dependency', core, app, '-o', self.root / 'x.ll', success=False))
        app = self.write('Panel { this{ <Box>"x"</Box> } }\nMain() { }\n', 'app.dk')
        self.assertIn('declare its value field', self.duckc('--dependency', core, app, '-o', self.root / 'x.ll', success=False))

    def test_diagnostics(self):
        panel = 'Panel {\n this{ %s }\n Show() { %s }\n}\nMain() { }\n'
        self.rejected(panel % ('<Label Title="a"/><Label Title="b"/>', 'Print(this.Label.Age)'), "component 'Label' has duplicates")
        self.rejected(panel % ('<Label Title="a"/>', 'Print(this.Hat.Size)'), "has no component 'Hat'")
        self.rejected(panel % ('<Nope/>', ''), "unknown component 'Nope'")
        self.rejected(panel % ('<Hat Missing=1/>', ''), "unknown container field 'Missing'")
        self.rejected(panel % ('<Hat Size="text"/>', ''), 'type mismatch')
        self.rejected(panel % ('<Label>"a"</Hat>', ''), 'expected closing tag </Label>')
        self.rejected(panel % ('<Hat Size=1/>', 'this.Hat = null'), '')
        self.rejected(panel % ('<Hat Size=1/>', 'markup m = this.GetChildren()'), 'is a view of stored children')
        self.rejected(panel % ('<Hat Size=1/>', 'this.GetChildren() += 1'), '')
        self.rejected('Panel {\n this{ <Hat/> }\n this{ <Hat/> }\n}\nMain() { }\n', 'already has a this{ } block')
        self.rejected('Panel { int V }\nMain() {\n Panel p\n Print(p.GetChildren().Len)\n}\n', 'has no markup children')
        self.rejected('A { }\nP {\n this{ <A>"x"</A>\n <A>1</A> }\n}\nMain() { }\n', 'value here; declare its value field')
        self.rejected('Main() {\n markup m = { <Hat/> "x" }\n}\n', 'belongs inside an element')
        self.rejected('A { int value }\nM #{ int N() { return 1 } }\nP { this{ M.N() } }\nMain() { }\n', 'must be an element or a markup value')
        self.rejected(panel % ('<Hat Size=1/>', 'Panel q = this'), '')

    def test_markup_is_only_parsed_in_a_markup_context(self):
        self.assertEqual(self.output('Main() {\n int a = 1\n int b = 2\n Print(a < b)\n if a < b { Print(3) }\n}\n'), '1\n3\n')


if __name__ == '__main__':
    unittest.main(argv=[__file__, *rest])
