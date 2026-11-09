#!/usr/bin/env python3
"""Checks what duckc prints when it rejects a program."""
import argparse
import pathlib
import subprocess
import sys
import tempfile
import unittest


class Diagnostics(unittest.TestCase):
    compiler = ''

    def errors(self, source, name='case.dk'):
        with tempfile.TemporaryDirectory() as work:
            path = pathlib.Path(work, name)
            path.write_bytes(source.encode('utf-8'))
            result = subprocess.run([self.compiler, name, '-o', 'out.ll'], cwd=work, capture_output=True)
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertFalse(pathlib.Path(work, 'out.ll').exists())
            return result.stderr.decode('utf-8').replace('\r\n', '\n')

    def test_source_line_and_caret(self):
        self.assertEqual(self.errors('Main()\n{\n    int count = 1\n    Print(coutn)\n}\n'),
                         "case.dk:4:11: error: unknown variable 'coutn'; did you mean 'count'?\n"
                         '    4 |     Print(coutn)\n'
                         '      |           ^~~~~\n')

    def test_caret_covers_string_literal(self):
        self.assertIn('    3 |     int a = "text"\n      |             ^~~~~~\n',
                      self.errors('Main()\n{\n    int a = "text"\n}\n'))

    def test_caret_covers_member_of_qualified_call(self):
        self.assertIn('    |             ^~~\n',
                      self.errors('P { int Get() { return 1 } }\nMain()\n{\n    P p()\n    Print(p.Gte())\n}\n'))

    def test_caret_accounts_for_tabs_and_utf8(self):
        self.assertIn('    3 |     string s = "héllo" + 1\n      |                          ^\n',
                      self.errors('Main()\n{\n\tstring s = "héllo" + 1\n}\n'))

    def test_crlf_source(self):
        output = self.errors('Main()\r\n{\r\n    Print(a)\r\n}\r\n')
        self.assertIn('    3 |     Print(a)\n      |           ^\n', output)

    def test_macro_note_follows_source_line(self):
        output = self.errors('M\n#{\n    macro Bad()\n    {\n        Print(missing)\n    }\n}\nMain()\n{\n    M.Bad()\n}\n')
        self.assertRegex(output, r"(?s)error: unknown variable 'missing'\n.*\^~+\n  note: in expansion of macro 'M\.Bad'")

    def test_no_source_line_without_a_location(self):
        self.assertEqual(self.errors('int F() { return 1 }\n'),
                         'case.dk:1:1: error: program requires Main(); add Main() { ... }, or compile with --library\n')

    def test_suggestions(self):
        cases = [
            ('Sq(int n) { }\nMain() { Sx(1) }\n', "unknown function 'Sx'; did you mean 'Sq'?"),
            ('Main() { print(1) }\n', "unknown function 'print'; did you mean 'Print'?"),
            ('G #{ int Twice(int n) { return n } }\nMain() { Print(G.Twise(1)) }\n', "did you mean 'G.Twice'?"),
            ('G #{ int Score = 1 }\nMain() { Print(G.Scor) }\n',
             "container 'G' has no static field 'Scor'; did you mean 'Score'?"),
            ('G #{ int Score = 1 }\nMain() { G.Scor = 2 }\n',
             "container 'G' has no static field 'Scor'; did you mean 'Score'?"),
            ('P { int Xpos }\nMain() { P p()\n Print(p.Xps) }\n', "unknown container field 'Xps'; did you mean 'Xpos'?"),
            ('P { int Xpos }\nMain() { P p = {Xps: 1} }\n', "unknown container field 'Xps'; did you mean 'Xpos'?"),
            ('P { Tick() { } }\nMain() { P p()\n p.tick() }\n', "did you mean 'Tick'?"),
            ('D : byte\n#@{\n One, Two\n}\nMain() { Print(D.Onee) }\n', "did you mean 'One'?"),
            ('Point { int X }\nMain() { Pont() }\n', "unknown function 'Pont'; did you mean 'Point'?"),
        ]

        for source, message in cases:
            with self.subTest(source=source):
                self.assertIn(message, self.errors(source))

    def test_unrelated_names_get_no_suggestion(self):
        self.assertNotIn('did you mean', self.errors('Main() { int count = 1\n Print(zebra) }\n'))

    def test_generated_names_are_never_suggested(self):
        self.assertNotIn('$', self.errors('P { int X\n P(int x) { X = x } }\nMain() { P p(1)\n p.new() }\n'))

    def test_unknown_types(self):
        cases = [
            'Main() {\n itn a = 1\n}\n',
            'F(itn a) { }\nMain() { }\n',
            'itn F() { return 1 }\nMain() { }\n',
            'P { itn X }\nMain() { }\n',
        ]

        for source in cases:
            with self.subTest(source=source):
                self.assertIn("unknown type 'itn'; did you mean 'int'?", self.errors(source))

        self.assertIn("unknown type 'Foo' (syntax may not be supported yet)\n", self.errors('Main() {\n Foo f()\n}\n'))

    def test_syntax_messages(self):
        cases = [
            ('Main()\n{\n int a = 1;\n}\n', "expected newline after statement, found ';'; Duck statements end at a newline, so remove it"),
            ('Main()\n{\n}\n}\n', "unmatched '}': no block is open here"),
            ('Main()\n{\n Print((1 + 2)\n}\n', "expected ')', found '}' (the '(' at case.dk:3:7 is still open)"),
            ('Main()\n{\n if true {\n Print(1)\n}\n', "expected '}', found end of file (the '{' at case.dk:2:1 is still open)"),
            ('Main()\n{\n int a =\n}\n', 'expected identifier, found end of line'),
            ('Main()\n{\n int a = 1 ` 2\n}\n', "after statement, found '`'"),
        ]

        for source, message in cases:
            with self.subTest(source=source):
                self.assertIn(message, self.errors(source))

    def test_declaration_context(self):
        self.assertIn("expected 2, got 1; declared at case.dk:1:5 as int(int,int)",
                      self.errors('int Add(int a, int b) { return a + b }\nMain() { Print(Add(1)) }\n'))
        self.assertIn("already declared at case.dk:1:10",
                      self.errors('Main() { int a = 1\n int a = 2 }\n'))
        self.assertIn('already declared at case.dk:1:1', self.errors('F() { }\nF() { }\nMain() { }\n'))

    def test_operand_and_cast_messages(self):
        self.assertIn("operator '*' cannot combine string and int", self.errors('Main() { Print("a" * 2) }\n'))
        self.assertIn('write an explicit cast: (int)value', self.errors('Main() { double d = 1.5\n int a = d\n Print(a) }\n'))

    def test_every_function_reports_its_first_error(self):
        output = self.errors('A()\n{\n    Print(a)\n    Print(b)\n}\nC()\n{\n    Print(c)\n}\nMain() { }\n')
        self.assertIn("unknown variable 'a'", output)
        self.assertNotIn("unknown variable 'b'", output)
        self.assertIn("unknown variable 'c'", output)
        self.assertTrue(output.endswith('2 errors\n'))

    def test_error_count_is_limited(self):
        source = ''.join(f'F{i}() {{ Print(x) }}\n' for i in range(25)) + 'Main() { }\n'
        output = self.errors(source)
        self.assertEqual(output.count(': error: '), 20)
        self.assertTrue(output.endswith('25 errors (showing the first 20)\n'))

    def test_editor_pattern_matches_each_error_only(self):
        output = self.errors('A() { Print(a) }\nB() { Print(b) }\nMain() { }\n')
        located = [line for line in output.splitlines() if ': error: ' in line]
        self.assertEqual(len(located), 2)
        self.assertRegex(located[0], r'^.+\.dk:\d+:\d+: error: .+$')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--compiler', required=True)
    args, rest = parser.parse_known_args()
    Diagnostics.compiler = str(pathlib.Path(args.compiler).resolve())
    unittest.main(argv=[sys.argv[0]] + rest)
