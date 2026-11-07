"""char, date, time, and datetime integration tests.

Python's datetime module is an independent oracle for the proleptic Gregorian
calendar: date.toordinal() - 1 is Duck's day count from 0001-01-01.
"""
import argparse
import datetime
from pathlib import Path
import random
import subprocess
import tempfile
import unittest

parser = argparse.ArgumentParser()
parser.add_argument('--compiler', required=True)
parser.add_argument('--clang', required=True)
options, rest = parser.parse_known_args()

ALLOCATIONS = r'''
#include <stdint.h>
#include <stdlib.h>
static int live;
void *text_test_alloc(uint64_t size) { void *p = malloc((size_t)size); if (p) ++live; return p; }
void text_test_free(void *p) { if (p) { --live; if (live < 0) abort(); } free(p); }
int Live(void) { return live; }
'''


class CalendarTests(unittest.TestCase):
    def setUp(self):
        # A trapped executable can stay locked briefly on Windows after it exits.
        self.temp = tempfile.TemporaryDirectory(prefix='duck calendar ', ignore_cleanup_errors=True)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def command(self, *command, success=True):
        result = subprocess.run(command, capture_output=True, timeout=60)
        self.assertEqual(result.returncode == 0, success,
                         (result.stdout + result.stderr).decode('utf-8', errors='replace'))
        return result

    def compile(self, source, stem='main', flags=()):
        path = self.root / (stem + '.dk')
        path.write_text(source, encoding='utf-8')
        ir = self.root / (stem + '.ll')
        self.command(options.compiler, *flags, str(path), '-o', str(ir))
        return path, ir

    def run_source(self, source, expected, fixture=False):
        _, ir = self.compile(source)
        objects = []
        if fixture:
            ir.write_text(ir.read_text().replace('@malloc', '@text_test_alloc')
                          .replace('@free', '@text_test_free'))
            native = self.root / 'native.c'
            native.write_text(ALLOCATIONS)
            obj = self.root / 'native.o'
            self.command(options.clang, '-c', str(native), '-o', str(obj))
            objects.append(str(obj))
        for level in ('-O0', '-O2'):
            exe = self.root / 'test.exe'
            self.command(options.clang, level, str(ir), *objects, '-o', str(exe))
            actual = self.command(str(exe)).stdout.replace(b'\r\n', b'\n')
            expected_bytes = expected if isinstance(expected, bytes) else expected.encode('utf-8')
            self.assertEqual(actual, expected_bytes, level)

    def reject(self, source, message):
        path, ir = self.root / 'bad.dk', self.root / 'bad.ll'
        path.write_text(source, encoding='utf-8')
        result = self.command(options.compiler, str(path), '-o', str(ir), success=False)
        self.assertIn(message, result.stderr.decode('utf-8', errors='replace'))
        self.assertRegex(result.stderr.decode('utf-8', errors='replace'), r':\d+:\d+: error:')
        self.assertFalse(ir.exists())

    def trap(self, source):
        _, ir = self.compile(source, 'trap')
        self.assertIn('call void @llvm.trap()', ir.read_text())
        for level in ('-O0', '-O2'):
            exe = self.root / 'trap.exe'
            self.command(options.clang, level, str(ir), '-o', str(exe))
            self.command(str(exe), success=False)

    def test_example(self):
        source = (Path(__file__).parent.parent / 'Examples/Bootstrap/16_CharsAndDates.dk').read_text(encoding='utf-8')
        self.run_source(source, '🦆\n129414\n2\n4\n1\n1\n2024-02-29\n2023-03-01\n4\n366\n'
                                '12:30:00\n23:59:59.999999999\n2024-02-29T12:30:00\n12\n1\n')

    def test_char_is_a_unicode_scalar(self):
        # Encoding boundaries, including supplementary characters that need a
        # UTF-16 surrogate pair but are still one char.
        points = [0, 0x41, 0x7F, 0x80, 0x7FF, 0x800, 0xD7FF, 0xE000, 0xFFFF,
                  0x10000, 0x1F986, 0x10FFFF]
        lines, expected = [], b''
        for cp in points:
            literal = "'\\U%08X'" % cp
            text = '"\\U%08X"' % cp
            lines += [f'c : {literal}', 'Print(c)', 'Print((uint32)c)',
                      f'Print(char({cp}) == c)', 'Print(Length((string)c))',
                      'Print(Length((name)c))', f'Print((string)c == {text})', f'Print((name)c == {text})']
            ch = chr(cp)
            expected += ch.encode('utf-8') + b'\n' + (
                f'{cp}\n1\n{len(ch.encode("utf-16-le")) // 2}\n{len(ch.encode("utf-8"))}\n1\n1\n').encode()
        body = '\n'.join('{\n' + '\n'.join(lines[i:i + 8]) + '\n}' for i in range(0, len(lines), 8))
        self.run_source('Main() {\n' + body + '\n}', expected)

    def test_char_literals_and_order(self):
        self.run_source("Main() {\nPrint('🦆' == '\\U0001F986')\nPrint('\\'' == char(39))\n"
                        "Print('\"' == char(34))\nPrint('\\uFFFF' < '\\U00010000')\n"
                        "Print('\\uE000' > '\\uD7FF')\nPrint('a' != 'A')\nPrint('z' >= 'a')\n"
                        "string s = \"x🦆\"\nPrint(char(CodeUnitAt(s, 0)) == 'x')\n"
                        "c : true ? 'y' | 'n'\nPrint(c)\n}",
                        '1\n1\n1\n1\n1\n1\n1\n1\ny\n')

    def test_char_text_storage_is_released(self):
        self.run_source('extern "C" int Live()\n'
                        'Main() {\nint i = 0\nwhile i < 100 {\n'
                        "s : (string)'🦆' + (string)char(65 + i % 26)\n"
                        "n : (name)'Ž'\nPrint(Length(s) + Length(n) == 5)\ni += 50\n}\n"
                        "Print('🦆')\nPrint(Live())\n}",
                        '1\n1\n🦆\n0\n', fixture=True)

    def test_dates_match_python(self):
        rng = random.Random(1912)
        dates = [datetime.date(1, 1, 1), datetime.date(9999, 12, 31), datetime.date(1970, 1, 1),
                 datetime.date(1600, 2, 29), datetime.date(1700, 2, 28), datetime.date(1700, 3, 1),
                 datetime.date(1900, 2, 28), datetime.date(1900, 3, 1), datetime.date(2000, 2, 29),
                 datetime.date(2024, 2, 29), datetime.date(2100, 2, 28), datetime.date(2100, 3, 1)]
        dates += [datetime.date.fromordinal(rng.randint(1, datetime.date.max.toordinal()))
                  for _ in range(300)]
        lines, expected = [], ''
        for d in dates:
            lines.append(f'{{ d : date({d.year}, {d.month}, {d.day})\n'
                         'Print(d)\nPrint(DaysBetween(date(1, 1, 1), d))\nPrint(DayOfWeek(d))\n'
                         'Print(Year(d) * 10000 + Month(d) * 100 + Day(d)) }')
            expected += (f'{d.isoformat()}\n{d.toordinal() - 1}\n{d.isoweekday()}\n'
                         f'{d.year * 10000 + d.month * 100 + d.day}\n')
        self.run_source('Main() {\n' + '\n'.join(lines) + '\n}', expected)

    def test_every_day_round_trips(self):
        self.run_source('''Main() {
    date d = date(1, 1, 1)
    long count = 0
    long bad = 0
    int weekday = 1
    while d < date(9999, 12, 31) {
        next : AddDays(d, 1)
        if date(Year(next), Month(next), Day(next)) != next || next <= d { bad++ }
        weekday = weekday == 7 ? 1 | weekday + 1
        if DayOfWeek(next) != weekday { bad++ }
        d = next
        count++
    }
    Print(count)
    Print(bad)
}''', '3652058\n0\n')

    def test_leap_days(self):
        self.run_source('''Main() {
    Print(AddDays(date(2024, 2, 28), 1))
    Print(AddDays(date(2023, 2, 28), 1))
    Print(AddDays(date(1900, 2, 28), 1))
    Print(AddDays(date(2000, 2, 28), 1))
    Print(AddDays(date(2024, 3, 1), -1))
    Print(AddDays(date(2024, 2, 29), 365))
    Print(DaysBetween(date(2024, 1, 1), date(2025, 1, 1)))
    Print(DaysBetween(date(2023, 1, 1), date(2024, 1, 1)))
    Print(DaysBetween(date(2000, 1, 1), date(2400, 1, 1)))
    Print(DaysBetween(date(2025, 1, 1), date(2024, 1, 1)))
    long y = 2024
    Print(date(y, 2, 29))
    datetime last = datetime(2024, 2, 29, 23, 59, 59, 999999999)
    Print(AddDays(last, 1))
}''', '2024-02-29\n2023-03-01\n1900-03-01\n2000-02-29\n2024-02-29\n2025-02-28\n'
      '366\n365\n146097\n-366\n2024-02-29\n2024-03-01T23:59:59.999999999\n')

    def test_time_and_datetime(self):
        self.run_source('''datetime Later(datetime a, datetime b) { return a < b ? b | a }
Shift(datetime& when) { when = AddDays(when, 7) }
Event {
    date day
    time at
}
Main() {
    Print(time(0, 0, 0))
    Print(time(7, 5, 3, 120))
    t : time(13, 45, 30, 999)
    Print(Hour(t) * 10000 + Minute(t) * 100 + Second(t))
    Print(Nanosecond(t))
    Print(time(23, 59, 59, 999999999) > time(23, 59, 59, 999999998))
    late : datetime(2024, 2, 28, 23, 0, 0)
    early : datetime(2024, 2, 29, 1, 0, 0)
    Print(late < early)
    Print(late == datetime(date(2024, 2, 28), time(23, 0, 0)))
    Print(late != early)
    Print(Later(late, early))
    Print(DateOf(early))
    Print(TimeOf(early))
    Print(Year(early) + Month(early) + Day(early) + Hour(early))
    datetime w
    w = early
    Shift(w)
    Print(w)
    Event e = {day: date(2024, 2, 29), at: time(9, 0, 0)}
    e.day = AddDays(e.day, 366)
    Print(datetime(e.day, e.at))
    #date fixed = date(1999, 12, 31)
    Print(fixed >= date(1999, 12, 31))
}''', '00:00:00\n07:05:03.000000120\n134530\n999\n1\n1\n1\n1\n2024-02-29T01:00:00\n'
      '2024-02-29\n01:00:00\n2056\n2024-03-07T01:00:00\n2025-03-01T09:00:00\n1\n')

    def test_user_functions_take_precedence(self):
        self.run_source('int Day(int n) { return n * 2 }\nMain() { Print(Day(21))\n'
                        'Print(Month(date(2024, 5, 1))) }', '42\n5\n')

    def test_separate_modules(self):
        lib, lib_ir = self.compile('datetime Stamp(int d) { return datetime(2024, 1, d, 1, 2, 3) }\n'
                                   'char Initial() { return \'🦆\' }', 'library', ['--library'])
        _, app_ir = self.compile('Main() { Print(Stamp(5))\nPrint(Year(Stamp(9)))\nPrint(Initial()) }',
                                 'app', ['--dependency', str(lib)])
        exe = self.root / 'modules.exe'
        self.command(options.clang, str(lib_ir), str(app_ir), '-o', str(exe))
        self.assertEqual(self.command(str(exe)).stdout.replace(b'\r\n', b'\n'),
                         '2024-01-05T01:02:03\n2024\n🦆\n'.encode())

    def test_invalid_constants_are_rejected(self):
        cases = [
            ('date(2023, 2, 29)', 'day 29 is outside 1..28 for month 2 of year 2023'),
            ('date(1900, 2, 29)', 'day 29 is outside 1..28'),
            ('date(2024, 2, 30)', 'day 30 is outside 1..29'),
            ('date(2024, 4, 31)', 'day 31 is outside 1..30'),
            ('date(2024, 1, 0)', 'day 0 is outside 1..31'),
            ('date(2024, 13, 1)', 'month 13 is outside 1..12'),
            ('date(2024, 0, 1)', 'month 0 is outside'),
            ('date(0, 1, 1)', 'year 0 is outside 1..9999'),
            ('date(10000, 1, 1)', 'year 10000 is outside'),
            ('date(-1, 1, 1)', 'year -1 is outside'),
            ('time(24, 0, 0)', 'hour 24 is outside 0..23'),
            ('time(0, 60, 0)', 'minute 60 is outside'),
            ('time(23, 59, 60)', 'second 60 is outside 0..59'),
            ('time(0, 0, 0, 1000000000)', 'nanosecond 1000000000 is outside'),
            ('time(0, 0, -1)', 'second -1 is outside'),
            ('datetime(2023, 2, 29, 0, 0, 0)', 'invalid datetime: day 29'),
            ('datetime(2024, 2, 29, 24, 0, 0)', 'invalid datetime: hour 24'),
            ('char(0xD800)', 'surrogate code point 55296'),
            ('char(0xDFFF)', 'surrogate'),
            ('char(0x110000)', 'code point 1114112 is outside'),
            ('char(-1)', 'code point -1 is outside'),
            ('date(2024, 1)', 'wrong argument count for date'),
            ('time(1, 2)', 'wrong argument count for time'),
            ('datetime(2024, 1, 1)', 'wrong argument count for datetime'),
            ('char()', 'wrong argument count for char'),
            ('date(2024.0, 1, 1)', 'type mismatch'),
            ('char((uint64)65)', 'type mismatch'),
            ('datetime(1, 2)', 'type mismatch'),
            ("''", 'exactly one Unicode scalar'),
            ("'ab'", 'exactly one Unicode scalar'),
            ("'e\\u0301'", 'exactly one Unicode scalar'),
            ("'\\uD800'", 'scalar value'),
            ('(char)65', 'use char(...)'),
            ('(date)1', 'use date(...)'),
            ('(int)date(2024, 1, 1)', 'only char has explicit conversions'),
            ("(double)'a'", 'only char has explicit conversions'),
            ('date(2024, 1, 1) + 1', 'date values support only comparison'),
            ('1 - time(1, 0, 0)', 'time values support only comparison'),
            ("'a' + 1", 'char values support only comparison'),
            ('date(2024, 1, 1) == time(1, 0, 0)', 'type mismatch'),
            ("'a' == 97", 'type mismatch'),
            ('Year(time(1, 2, 3))', 'Year requires a date or datetime, got time'),
            ('Hour(date(2024, 1, 1))', 'Hour requires a time or datetime'),
            ('DateOf(date(2024, 1, 1))', 'DateOf requires a datetime'),
            ('DaysBetween(datetime(2024, 1, 1, 0, 0, 0), date(2024, 1, 1))', 'DaysBetween requires a date'),
            ('AddDays(date(2024, 1, 1))', "wrong argument count for 'AddDays'"),
        ]
        for expression, message in cases:
            with self.subTest(expression=expression):
                self.reject('Main() { x : ' + expression + ' }', message)
        self.reject("Main() { c : 'a\nPrint(c) }", 'newline in character literal')
        self.reject("Main() { c : 'a", 'unterminated character literal')
        self.reject('Main() { int time = 3 }', 'reserved keyword')
        self.reject("Main() { c : 'a'\nc++ }", 'type mismatch')
        self.reject('Main() { date d\nPrint(d) }', 'read before assignment')
        self.reject('extern "C" Bad(date d)\nMain() {}', 'native parameter ABI')

    def test_invalid_runtime_values_trap(self):
        for source in [
            'Main() { long y = 2023\nPrint(date(y, 2, 29)) }',
            'Main() { int m = 13\nPrint(date(2024, m, 1)) }',
            'Main() { int y = 0\nPrint(date(y, 1, 1)) }',
            'Main() { uint32 u = 0xD800\nPrint(char(u)) }',
            'Main() { long u = 0x110000\nPrint(char(u)) }',
            'Main() { int n = -1\nPrint(char(n)) }',
            'Main() { int h = 24\nPrint(time(h, 0, 0)) }',
            'Main() { long ns = 1000000000\nPrint(time(0, 0, 0, ns)) }',
            'Main() { int d = 30\nPrint(datetime(2024, 2, d, 0, 0, 0)) }',
            'Main() { Print(AddDays(date(9999, 12, 31), 1)) }',
            'Main() { Print(AddDays(date(1, 1, 1), -1)) }',
            'Main() { Print(AddDays(datetime(9999, 12, 31, 0, 0, 0), 1)) }',
            'Main() { Print(AddDays(date(1, 1, 1), -9223372036854775807)) }',
            'Main() { Print(AddDays(date(9999, 12, 31), 9223372036854775807)) }',
        ]:
            with self.subTest(source=source):
                self.trap(source)


if __name__ == '__main__':
    unittest.main(argv=[__file__, *rest])
