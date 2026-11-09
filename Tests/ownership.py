#!/usr/bin/env python3
"""Owning fields, tracked aliases in storage, deferred destruction, and pointer
conditions. Duck heap allocations are counted, so each scenario also proves
that it releases every payload and control block."""
import argparse
import pathlib
import subprocess
import sys
import tempfile
import unittest

parser = argparse.ArgumentParser()
parser.add_argument('--compiler', required=True)
parser.add_argument('--clang', required=True)
options, rest = parser.parse_known_args()

COUNTERS = r'''
#include <stdint.h>
#include <stdlib.h>
static int live;
void *heap_test_alloc(uint64_t size) { void *p = malloc((size_t)size); if (p) ++live; return p; }
void heap_test_free(void *p) { if (p) { --live; if (live < 0) abort(); } free(p); }
int Live(void) { return live; }
'''

# Shared by the scenarios. Scenario() runs in its own frame so that Main can
# report the allocations left after every local has been cleaned up.
PRELUDE = '''
extern "C" int Live()
Item
{
    int Value = 42
    ~Item() { Print("item gone") }
    Show() { Print(Value) }
}
Holder { Item* Ref }
Node
{
    int Id
    Node* Next :: null
    ~Node() { Print("node gone") }
}
'''
ENTRY = '\nMain() {\n Scenario()\n Print(Live())\n}\n'


class Ownership(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.root = pathlib.Path(cls.temp.name)
        native = cls.root / 'counters.c'
        native.write_text(COUNTERS)
        cls.counters = cls.root / 'counters.o'
        subprocess.run([options.clang, '-c', str(native), '-o', str(cls.counters)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def compile(self, source):
        path = self.root / 'test.dk'
        ir = self.root / 'test.ll'
        ir.unlink(missing_ok=True)
        path.write_text(source, encoding='utf-8')
        return subprocess.run([options.compiler, str(path), '-o', str(ir)], capture_output=True, text=True), ir

    def build(self, source, level):
        result, ir = self.compile(source)
        self.assertEqual(result.returncode, 0, result.stderr)
        text = ir.read_text()
        ir.write_text(text.replace('@malloc', '@heap_test_alloc').replace('@free', '@heap_test_free'))
        exe = self.root / 'test.exe'
        subprocess.run([options.clang, level, str(ir), str(self.counters), '-o', str(exe)], check=True,
                       capture_output=True)
        return exe

    def check(self, body, *expected):
        source = PRELUDE + body + ENTRY
        wanted = ''.join(f'{line}\n' for line in (*expected, 0))
        for level in ('-O0', '-O2'):
            with self.subTest(level=level):
                done = subprocess.run([str(self.build(source, level))], capture_output=True, timeout=20)
                self.assertEqual(done.returncode, 0, done.stderr)
                self.assertEqual(done.stdout.decode().replace('\r\n', '\n'), wanted)

    def traps(self, body):
        # A trap loses buffered output, so only the failure itself is checked.
        source = PRELUDE + body + '\nMain() { Scenario() }\n'
        for level in ('-O0', '-O2'):
            with self.subTest(level=level):
                done = subprocess.run([str(self.build(source, level))], capture_output=True, timeout=20)
                self.assertNotEqual(done.returncode, 0)

    def rejects(self, body, message):
        result, ir = self.compile(PRELUDE + body + '\nMain() {}\n')
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn(message, result.stderr)
        self.assertFalse(ir.exists())

    # Owning fields ----------------------------------------------------------

    def test_eager_field_is_built_and_destroyed_with_its_container(self):
        self.check('''
Box { Item* Thing() }
Scenario() {
 Box b
 Print(b.Thing.Value)
}''', 42, 'item gone')

    def test_eager_field_passes_arguments(self):
        self.check('''
Part { int N
 Part(int n) { N = n } }
Box { Part* P(5) }
Scenario() {
 Box b
 Print(b.P.N)
}''', 5)

    def test_empty_field_and_scalar_owner(self):
        self.check('''
Box {
 Item* Thing :: null
 int* Count :: 3
}
Scenario() {
 Box b
 Print(b.Thing == null)
 Print(*b.Count)
 *b.Count = 4
 Print(*b.Count)
}''', 1, 3, 4)

    def test_field_takes_local_call_and_new_objects(self):
        self.check('''
Item* Make() { Item* made()
 made.Value = 9
 return ::made }
Box { Item* Thing :: null }
Scenario() {
 Box b
 local :: 1
 Item* owner()
 owner.Value = 7
 b.Thing :: owner
 Print(owner == null)
 Print(b.Thing.Value)
 b.Thing :: Make()
 Print(b.Thing.Value)
 b.Thing :: Item()
 Print(b.Thing.Value)
 Print(*local)
}''', 1, 7, 'item gone', 9, 'item gone', 42, 1, 'item gone')

    def test_move_out_of_a_field_keeps_aliases(self):
        self.check('''
Box { Item* Thing() }
Scenario() {
 Box b
 alias : b.Thing
 taken :: b.Thing
 Print(b.Thing == null)
 Print(alias.Value)
 taken = null
 Print(alias == null)
}''', 1, 42, 'item gone', 1)

    def test_clearing_a_field_destroys_and_expires_aliases(self):
        self.check('''
Box { Item* Thing() }
Scenario() {
 Box b
 alias : b.Thing
 b.Thing = null
 Print(alias == null)
}''', 'item gone', 1)

    def test_destroying_a_parent_expires_aliases_to_descendants(self):
        self.check('''
Scenario() {
 Node* root()
 root.Next :: Node()
 root.Next.Next :: Node()
 deep : root.Next.Next
 root = null
 Print(deep == null)
}''', 'node gone', 'node gone', 'node gone', 1)

    def test_container_with_owning_field_moves(self):
        self.check('''
Box { Item* Thing() }
Box Make() { Box b
 b.Thing.Value = 8
 return b }
Scenario() {
 Box [] boxes
 boxes += Make()
 boxes++
 Print(boxes[0].Thing.Value)
 Print(boxes[1].Thing.Value)
 boxes -= [0]
 Print(boxes.Len)
}''', 8, 42, 'item gone', 1, 'item gone')

    def test_field_moves_in_a_method_need_this(self):
        self.check('''
Box {
 Item* Thing :: null
 Fill() { this.Thing :: Item() }
}
Scenario() {
 Box b
 b.Fill()
 Print(b.Thing.Value)
}''', 42, 'item gone')

    def test_rejected_cycle_through_alias_leaves_owner_intact(self):
        self.check('''
Scenario() {
 Node* a()
 a.Id = 1
 a.Next :: Node()
 inner : a.Next
 Print((inner.Next :: a))
 Print(a == null)
 Print(a.Id)
 Print(a.Next == null)
}''', 0, 0, 1, 0, 'node gone', 'node gone')

    def test_longer_cycle_is_rejected(self):
        self.check('''
Scenario() {
 Node* a()
 a.Next :: Node()
 a.Next.Next :: Node()
 last : a.Next.Next
 Print((last.Next :: a))
 Print(a == null)
}''', 0, 0, 'node gone', 'node gone', 'node gone')

    def test_valid_moves_between_branches_succeed(self):
        self.check('''
Scenario() {
 Node* a()
 Node* b()
 b.Id = 2
 Print((a.Next :: b))
 Print(a.Next.Id)
 a.Next.Next :: Node()
 Print((a.Next.Next.Next :: Node()))
}''', 1, 2, 1, 'node gone', 'node gone', 'node gone', 'node gone')

    def test_cycle_found_through_this(self):
        self.check('''
Graph {
 Graph* Child :: null
 Wrap* Up :: null
 ~Graph() { Print("graph gone") }
}
Wrap {
 Graph* Root :: null
 bool Close() {
  alias : Root
  return (alias.Child :: Root)
 }
}
Scenario() {
 Wrap w
 w.Root :: Graph()
 Print(w.Close())
 Print(w.Root == null)
}''', 0, 0, 'graph gone')

    def test_cycle_through_embedded_container_and_array(self):
        self.check('''
Slot { Tree* Held :: null }
Tree {
 int Id
 Slot Inner
 Slot [] Many
 ~Tree() { Print("tree gone") }
}
Scenario() {
 Tree* a()
 a.Inner.Held :: Tree()
 first : a.Inner.Held
 Print((first.Inner.Held :: a))
 a.Many++
 a.Many[0].Held :: Tree()
 second : a.Many[0].Held
 Print((second.Inner.Held :: a))
 Print(a == null)
}''', 0, 0, 0, 'tree gone', 'tree gone', 'tree gone')

    def test_statement_move_traps_on_a_cycle(self):
        self.traps('''
Scenario() {
 Node* a()
 a.Next :: Node()
 inner : a.Next
 Print("before")
 inner.Next :: a
 Print("after")
}''')

    def test_rejections(self):
        for body, message in [
            ('Scenario() { Node* a()\na.Next :: a }', 'into its own field'),
            ('Scenario() { Node* a()\na.Next :: Node()\na.Next.Next :: a.Next }', 'into its own field'),
            ('Scenario() { Node* a()\nb : a\nNode* c()\nc.Next :: b }', 'mutable owning source'),
            ('Holder Make() { Holder h\nreturn h }\nScenario() { Node* a()\nHolder h\nh.Ref :: a }', 'needs an owning field'),
            ('Rec { Rec* Self() }', 'without end'),
            ('A { B* X() }\nB { A* Y() }', 'without end'),
            ('Box { Item* Thing() }\nScenario() { Box a\nBox b = a }', 'cannot copy'),
            ('Box { Item* Thing :: null\nF(Node* n) { Thing :: Item() } }', "write this.Thing"),
            ('Box { int* N() }', 'cannot be constructed'),
            ('Scenario() { Item* o()\nHolder h\nItem i\nh.Ref = &i }', 'tracked'),
            ('Box { Item* Thing() }\nextern "C" F(Box& b)', 'native parameters'),
            ('Scenario() { Item* o()\nvoid* [] p }', 'cannot hold'),
        ]:
            with self.subTest(body=body):
                self.rejects(body, message)

    # Aliases in storage -----------------------------------------------------

    def test_expired_array_entry_stays_and_is_null(self):
        self.check('''
Scenario() {
 Item* owner()
 Item* [] aliases
 aliases += owner
 Print(aliases[0].Value)
 owner = null
 Print(aliases.Len)
 Print(aliases[0] == null)
}''', 42, 'item gone', 1, 1)

    def test_inferred_literal_holds_aliases(self):
        self.check('''
Scenario() {
 Item* owner()
 other : owner
 aliases : {owner, other}
 owner = null
 Print(aliases.Len)
 Print(aliases[1] == null)
}''', 'item gone', 2, 1)

    def test_array_element_operations(self):
        self.check('''
Scenario() {
 Item* a()
 Item* b()
 b.Value = 2
 Item* [] list = {a}
 list[0] = b
 Print(list[0].Value)
 list += list[0]
 list++
 Print(list[2] == null)
 list[1] = null
 list--
 list -= [0]
 Print(list.Len)
 list += a
 list.Clear()
 Print(list.Len)
}''', 2, 1, 1, 0, 'item gone', 'item gone')

    def test_fixed_array_queue_stack_and_pair(self):
        self.check('''
Scenario() {
 Item* owner()
 Item* [2] fixed
 fixed[1] = owner
 Item* queue q
 q += owner
 Item* stack s
 s += owner
 string, Item* pair p
 p += "a" : owner
 p += "a" : owner
 Print(fixed[0] == null)
 Print(q.Front.Value)
 Print(s.Top.Value)
 Print(p["a"].Value)
 owner = null
 Print(fixed[1] == null)
 Print(q.Len)
 Print(q.Front == null)
 Print(s.Top == null)
 Print(p.Last == null)
}''', 1, 42, 42, 42, 'item gone', 1, 1, 1, 1, 1)

    def test_iteration_and_copying_from_storage(self):
        self.check('''
Scenario() {
 Item* owner()
 Item* [] list = {owner}
 for x : list
 {
  y : x
  Print(y.Value)
 }
 Holder h
 h.Ref = list[0]
 z : h.Ref
 owner = null
 Print(z == null)
 for x : list
  Print(x == null)
}''', 42, 'item gone', 1, 1)

    def test_alias_field_copy_overwrite_and_return(self):
        self.check('''
Holder Keep(Item* unused) { Holder h
 return h }
Holder Pass(Holder h) { return h }
Scenario() {
 Item* a()
 Item* b()
 Holder one
 one.Ref = a
 Holder two = one
 two.Ref = b
 Print(one.Ref == two.Ref)
 three : Pass(one)
 one = two
 a = null
 Print(three.Ref == null)
 Print(one.Ref.Value)
 Holder init = {Ref: b}
 Print(init.Ref.Value)
 init.Ref = null
 Print(init.Ref == null)
}''', 0, 'item gone', 1, 42, 42, 1, 'item gone')

    def test_heap_container_releases_alias_fields(self):
        self.check('''
Scenario() {
 Item* owner()
 Holder* h()
 h.Ref = owner
 Print(h.Ref.Value)
 h = null
 Print(owner.Value)
}''', 42, 42, 'item gone')

    def test_lambda_capture_expires(self):
        self.check('''
Scenario() {
 Item* owner()
 alias : owner
 check : @ bool() { return alias == null }
 Print(check())
 owner = null
 Print(check())
}''', 0, 'item gone', 1)

    def test_field_alias_passes_to_a_proven_borrow(self):
        self.check('''
int Read(Item* e) { return e.Value }
Scenario() {
 Item* owner()
 Holder h
 h.Ref = owner
 Item* [] list = {owner}
 Print(Read(h.Ref))
 Print(Read(list[0]))
}''', 42, 42, 'item gone')

    def test_expired_field_alias_traps(self):
        self.traps('''
Scenario() {
 Item* owner()
 Holder h
 h.Ref = owner
 owner = null
 h.Ref.Show()
}''')

    def test_removed_alias_is_not_a_value(self):
        self.rejects('Scenario() { Item* o()\nItem* [] l = {o}\nx : (l--) }', 'removed alias')

    def test_alias_needs_a_stored_source(self):
        self.rejects('Scenario() { Item* o()\nItem* queue q\nq += o\nx : q.Front }', 'named source')

    def test_raw_pointer_fields_keep_their_layout(self):
        self.check('''
Native {
 int* Number
 void* Handle
}
Scenario() {
 int n = 5
 Native s
 s.Number = &n
 Print(*s.Number)
 Print(s.Handle == null)
}''', 5, 1)

    # Deferred destruction ---------------------------------------------------

    MANAGER = '''
Worker {
 int Progress = 7
 ~Worker() { Print("gone") }
 Show() { Print(Progress) }
 Run(Manager& m) {
  Print("begin")
  m.Kill()
  Progress++
  Print(Progress)
 }
}
Manager {
 Worker* W()
 Kill() { W = null }
 Reset() { this.W :: Worker() }
}
'''

    def test_owner_cleared_during_call_waits_for_the_call(self):
        self.check(self.MANAGER + '''
Scenario() {
 Manager m
 m.W.Run(m)
 Print("after")
 Print(m.W == null)
}''', 'begin', 8, 'gone', 'after', 1)

    def test_alias_call_defers_and_new_uses_fail(self):
        self.check(self.MANAGER + '''
Scenario() {
 Manager m
 a : m.W
 a.Run(m)
 Print(a == null)
}''', 'begin', 8, 'gone', 1)

    def test_use_after_close_during_a_call_traps(self):
        self.traps(self.MANAGER + '''
Again(Worker& w, Manager& m, Holder2& h) {}
Holder2 { Worker* Ref }
Reader {
 Use(Manager& m, Holder2& h) {
  m.Kill()
  h.Ref.Show()
 }
}
Scenario() {
 Manager m
 Holder2 h
 h.Ref = m.W
 Reader r
 r.Use(m, h)
}''')

    def test_assignment_keeps_target_alive_while_value_is_computed(self):
        self.check(self.MANAGER + '''
int Kill(Manager& m) {
 m.Kill()
 return 3
}
Scenario() {
 Manager m
 a : m.W
 a.Progress = Kill(m)
 Print(a == null)
}''', 'gone', 1)

    def test_destructor_that_clears_another_owner(self):
        self.check('''
Inner { ~Inner() { Print("inner") } }
Outer {
 Inner* A()
 Inner* B()
 ~Outer() { Print("outer")
  A = null }
}
Scenario() {
 Outer* o()
 alias : o.B
 o = null
 Print(alias == null)
}''', 'outer', 'inner', 'inner', 1)

    # Pointer conditions -----------------------------------------------------

    def test_condition_on_each_kind_of_pointer(self):
        self.check('''
Scenario() {
 Item* owner()
 alias : owner
 Holder h
 void* raw = null
 if owner
  Print("owner")
 if alias
  Print("alias")
 if h.Ref
  Print("no")
 else if raw
  Print("no")
 else
  Print("neither")
 owner = null
 if alias
  Print("no")
 if owner
  Print("no")
}''', 'owner', 'alias', 'neither', 'item gone')

    def test_condition_keeps_reference_valid_through_the_branch(self):
        self.check(self.MANAGER + '''
Scenario() {
 Manager m
 a : m.W
 if a {
  m.Kill()
  Print(a == null)
  a.Show()
  a.Progress = 9
  a.Show()
 }
 Print("after")
}''', 1, 7, 9, 'gone', 'after')

    def test_condition_on_a_field_binds_it(self):
        self.check(self.MANAGER + '''
Scenario() {
 Manager m
 if m.W {
  m.Kill()
  m.W.Show()
 }
 Print("after")
 if m.W
  Print("no")
}''', 7, 'gone', 'after')

    def test_use_ends_on_every_exit(self):
        self.check(self.MANAGER + '''
int Early(Manager& m) {
 a : m.W
 if a {
  m.Kill()
  return a.Progress
 }
 return 0
}
Scenario() {
 Manager m
 Print(Early(m))
 for int i = 0; i < 3; i++ {
  m.Reset()
  a : m.W
  if a {
   m.Kill()
   if i == 0 { continue }
   if i == 1 { break }
  }
 }
 Print("after")
}''', 'gone', 7, 'gone', 'gone', 'after')

    def test_branch_that_replaces_the_reference_is_not_bound(self):
        self.traps('''
Scenario() {
 Item* owner()
 a : owner
 if a {
  a = null
  Print(a == null)
  a.Show()
 }
}''')

    # Owning and lending returns ---------------------------------------------

    FACTORY = '''
Item* Make(bool give) {
 Item* local()
 if give
  return ::local
 return local
}
Shelf {
 Item* Held()
 Item* Find() { return Held }
 Item* Pick(bool give) {
  if give
   return ::Held
  return Held
 }
}
'''

    def test_inferred_local_owns_an_owning_result(self):
        self.check(self.FACTORY + """
Scenario() {
 x : Make(true)
 Print(x.Value)
 alias : x
 x = null
 Print(alias == null)
}""", 42, 'item gone', 1)

    def test_alias_to_a_local_owner_arrives_expired(self):
        self.check(self.FACTORY + """
Scenario() {
 x : Make(false)
 Print(x == null)
 ?x.Show()
 if x
  Print("no")
}""", 'item gone', 1)

    def test_using_an_expired_result_traps(self):
        self.traps(self.FACTORY + """
Scenario() {
 x : Make(false)
 x.Show()
}""")

    def test_lent_result_follows_a_surviving_owner(self):
        self.check(self.FACTORY + """
Scenario() {
 Shelf s
 x : s.Find()
 x.Show()
 x = null
 Print(s.Held == null)
 y : s.Find()
 s.Held = null
 Print(y == null)
}""", 42, 0, 'item gone', 1)

    def test_clearing_or_leaving_scope_with_a_loan_does_not_destroy(self):
        self.check(self.FACTORY + """
Scenario() {
 Shelf s
 {
  x : s.Find()
  Print(x.Value)
 }
 Print(s.Held.Value)
}""", 42, 42, 'item gone')

    def test_a_mixed_method_gives_or_lends(self):
        self.check(self.FACTORY + """
Scenario() {
 Shelf s
 a : s.Pick(false)
 Print(s.Held == null)
 b : s.Pick(true)
 Print(s.Held == null)
 Print(a == null)
 b = null
 Print(a == null)
}""", 0, 1, 0, 'item gone', 1)

    def test_explicit_receive_takes_a_given_result(self):
        self.check(self.FACTORY + """
Scenario() {
 x :: Make(true)
 Print(x.Value)
}""", 42, 'item gone')

    def test_explicit_receive_traps_on_a_borrowed_result(self):
        self.traps(self.FACTORY + 'Scenario() { x :: Make(false) }')

    def test_moving_from_a_loan_traps(self):
        self.traps(self.FACTORY + """
Scenario() {
 Shelf s
 x : s.Pick(false)
 y :: x
}""")

    def test_returning_ownership_of_a_loan_traps(self):
        self.traps(self.FACTORY + """
Item* Again(Shelf& s) {
 x : s.Pick(false)
 return ::x
}
Scenario() {
 Shelf s
 y : Again(s)
}""")

    def test_receiving_into_a_field(self):
        self.check(self.FACTORY + """
Box { Item* Thing :: null }
Scenario() {
 Box b
 b.Thing :: Make(true)
 Print(b.Thing.Value)
 Print((b.Thing :: Make(false)))
 Print(b.Thing.Value)
}""", 42, 'item gone', 0, 42, 'item gone')

    def test_field_move_of_a_borrowed_result_traps(self):
        self.traps(self.FACTORY + """
Box { Item* Thing :: null }
Scenario() {
 Box b
 b.Thing :: Make(false)
}""")

    def test_returns_pass_through_other_functions_in_any_order(self):
        self.check("""
Item* Outer(Shelf& s) { return Middle(s) }
Item* Middle(Shelf& s) { return Inner(s) }
Item* Inner(Shelf& s) { return s.Held }
Item* Given() { return Fresh() }
Item* Fresh() { Item* i()
 return ::i }
Shelf { Item* Held() }
Scenario() {
 Shelf s
 x : Outer(s)
 Print(x.Value)
 y :: Given()
 Print(y.Value)
}""", 42, 42, 'item gone', 'item gone')

    def test_a_loan_can_be_null_and_is_copied_as_an_alias(self):
        self.check("""
Shelf {
 Item* Held()
 Item* Maybe(bool some) {
  if some
   return Held
  return null
 }
}
Scenario() {
 Shelf s
 none : s.Maybe(false)
 Print(none == null)
 x : s.Maybe(true)
 Item* [] list = {x}
 Holder h
 h.Ref = x
 check : @ bool() { return x == null }
 s.Held = null
 Print(list[0] == null)
 Print(h.Ref == null)
 Print(check())
}""", 1, 'item gone', 1, 1, 1)

    def test_a_loan_is_held_while_in_use(self):
        self.check(self.MANAGER + """
Worker* Peek(Manager& m) { return m.W }
Scenario() {
 Manager m
 x : Peek(m)
 x.Run(m)
 Print(x == null)
 m.Reset()
 y : Peek(m)
 if y {
  m.Kill()
  y.Show()
 }
 Print("after")
}""", 'begin', 8, 'gone', 1, 7, 'gone', 'after')

    def test_factory_across_modules(self):
        lib = self.root / 'lib.dk'
        lib.write_text('''
Part { int N = 3 }
Store {
 Part* Held()
 Part* Find() { return Held }
}
Part* Build() { Part* p()
 return ::p }
''')
        app = self.root / 'app.dk'
        app.write_text('''
Main() {
 Store s
 x : s.Find()
 Print(x.N)
 y : Build()
 Print(y.N)
 s.Held = null
 Print(x == null)
}
''')
        libIr, appIr, exe = self.root / 'lib.ll', self.root / 'app.ll', self.root / 'mod.exe'
        subprocess.run([options.compiler, '--library', str(lib), '-o', str(libIr)], check=True)
        subprocess.run([options.compiler, '--dependency', str(lib), str(app), '-o', str(appIr)], check=True)
        subprocess.run([options.clang, '-O1', str(libIr), str(appIr), '-o', str(exe)], check=True, capture_output=True)
        done = subprocess.run([str(exe)], capture_output=True, timeout=20)
        self.assertEqual(done.returncode, 0)
        self.assertEqual(done.stdout.decode().replace('\r\n', '\n'), '3\n3\n1\n')

    def test_factory_rejections(self):
        for body, message in [
            (self.FACTORY + 'Scenario() { Shelf s\nx :: s.Find() }', 'only lends'),
            (self.FACTORY + 'Box { Item* T :: null }\nScenario() { Shelf s\nBox b\nb.T :: s.Find() }', 'only lends'),
            (self.FACTORY + 'Scenario() { Make(true) }', 'receive an owning'),
            (self.FACTORY + 'Scenario() { Item* x = Make(true) }', 'receive an owning'),
            (self.FACTORY + 'Scenario() { Make(true).Show() }', 'receive an owning'),
            ('Item* F(Item* raw) { Item* o()\nif o\n return ::o\nreturn raw }', 'cannot return'),
            ('A { Item* Held()\nItem* Get() { return null } }\nB : A { Item* Get() { return Held } }', 'overrides'),
            (self.FACTORY + 'Scenario() { f : Make }', 'func value'),
        ]:
            with self.subTest(body=body):
                self.rejects(body, message)

    def test_raw_pointer_returns_stay_raw(self):
        self.check('''
Item* Same(Item* p) { return p }
Scenario() {
 Item local
 Item* raw = &local
 Print(Same(raw).Value)
}''', 42, 'item gone')

    # Guarded calls ----------------------------------------------------------

    def test_guard_runs_a_live_call_and_skips_a_null_one(self):
        self.check('''
Scenario() {
 Item* owner()
 alias : owner
 ?alias.Show()
 ?owner.Show()
 owner = null
 ?alias.Show()
 ?owner.Show()
 Print("end")
}''', 42, 42, 'item gone', 'end')

    def test_skipped_arguments_have_no_side_effects(self):
        self.check('''
int Side() {
 Print("side")
 return 1
}
Takes { Use(int n) { Print(n) } }
Scenario() {
 Takes* owner()
 alias : owner
 ?alias.Use(Side())
 owner = null
 ?alias.Use(Side())
 Print("end")
}''', 'side', 1, 'end')

    def test_chain_stops_at_the_first_invalid_link(self):
        self.check('''
Inner { Foo() { Print("foo") } }
Middle { Inner* Ex :: null }
Scenario() {
 Middle* o()
 ?o.Ex.Foo()
 o.Ex :: Inner()
 ?o.Ex.Foo()
 keep : o.Ex
 ?keep.Foo()
 o = null
 ?o.Ex.Foo()
 ?keep.Foo()
 Print("end")
}''', 'foo', 'foo', 'end')

    def test_chain_through_embedded_containers(self):
        self.check('''
Leaf { Foo() { Print("foo") } }
Slot { Leaf* Held :: null }
Tree { Slot Inner }
Scenario() {
 Tree t
 ?t.Inner.Held.Foo()
 t.Inner.Held :: Leaf()
 ?t.Inner.Held.Foo()
}''', 'foo')

    def test_guard_gives_the_default_when_skipped(self):
        self.check('''
Vals {
 int N = 5
 int Add(int x) { return N + x }
 string Name() { return "named" }
 Vals* Self() { return null }
}
Scenario() {
 Vals* owner()
 alias : owner
 Print(?alias.Add(2))
 Print(?alias.Name())
 Print(?alias.Self() == null)
 owner = null
 Print(?alias.Add(2))
 Print(?alias.Name() == "")
 Print(?alias.Self() == null)
 int n = ?alias.Add(1) + 1
 Print(n)
}''', 7, 'named', 1, 0, 1, 1, 1)

    def test_each_link_is_evaluated_once(self):
        self.check('''
Thing { Foo() { Print("foo") } }
Thing* Get(Thing* p) {
 Print("get")
 return p
}
int Index() {
 Print("index")
 return 0
}
Scenario() {
 Thing local
 ?Get(&local).Foo()
 Thing* owner()
 Thing* [] list = {owner}
 ?list[Index()].Foo()
}''', 'get', 'foo', 'index', 'foo')

    def test_guard_skips_missing_and_expired_entries(self):
        self.check('''
Scenario() {
 Item* owner()
 Item* [] list = {owner}
 ?list[0].Show()
 ?list[5].Show()
 owner = null
 ?list[0].Show()
 Print(list.Len)
}''', 42, 'item gone', 1)

    def test_guard_skips_an_empty_func_field(self):
        self.check('''
Hooked { func int() Hook }
Scenario() {
 Hooked* h()
 Print(?h.Hook())
 h.Hook = @ int() { return 3 }
 Print(?h.Hook())
}''', 0, 3)

    def test_guard_holds_each_live_link_through_the_call(self):
        self.check(self.MANAGER + '''
Scenario() {
 Manager m
 ?m.W.Run(m)
 Print("after")
 ?m.W.Run(m)
 a : m.W
 Print("end")
}''', 'begin', 8, 'gone', 'after', 'end')

    def test_guard_inside_a_bound_branch_uses_the_bound_reference(self):
        self.check(self.MANAGER + '''
Scenario() {
 Manager m
 a : m.W
 if a {
  m.Kill()
  ?a.Show()
 }
 Print("after")
}''', 7, 'gone', 'after')

    def test_guard_in_conditions_and_loops(self):
        self.check('''
Vals { bool Ready() { return true } }
Scenario() {
 Vals* owner()
 alias : owner
 if ?alias.Ready()
  Print("ready")
 owner = null
 if ?alias.Ready()
  Print("no")
 for int i = 0; i < 2; i++
  ?alias.Ready()
 Print("end")
}''', 'ready', 'end')

    def test_guard_rejections(self):
        for body, message in [
            ('Scenario() { Print(?Live()) }', "guards an index such as"),
            ('Calc #{ int F() { return 1 } }\nScenario() { Print(?Calc.F()) }', 'no reference to check'),
            ('Scenario() { int [] a\n?a.Clear() }', 'no reference to check'),
            ('Built { int V = Live() }\nP { Built Make() { Built b\nreturn b } }\nScenario() { P* p()\nx : ?p.Make() }', 'built at run time'),
            ('Scenario() { Node* n()\nPrint(?n.Id) }', "guards an index such as"),
        ]:
            with self.subTest(body=body):
                self.rejects(body, message)

    def test_condition_rejects_other_types(self):
        self.rejects('Scenario() { if 5 { } }', 'type mismatch')
        for body in ('while 5 { }', 'do { } while 5', 'for ; 5; { }', 'x : !5', 'x : 5 && true',
                     'x : true || 5', 'x : 5 ? 1 | 2'):
            with self.subTest(body=body):
                self.rejects('Scenario() { ' + body + ' }', 'type mismatch')

    def test_a_tested_pointer_is_a_bool_only_as_a_condition(self):
        self.rejects('Scenario() { Item* o()\nbool b = o }', 'type mismatch')

    def test_negation_on_each_kind_of_pointer(self):
        self.check('''
Scenario() {
 Item* owner()
 alias : owner
 Holder h
 void* raw = null
 Print(!owner)
 Print(!alias)
 Print(!h.Ref)
 Print(!raw)
 owner = null
 Print(!owner)
 Print(!alias)
}''', 0, 0, 1, 1, 'item gone', 1, 1)

    def test_logical_operators_short_circuit(self):
        self.check('''
Scenario() {
 Item* owner()
 Holder h
 Print(owner && h.Ref)
 Print(owner || h.Ref)
 Print(h.Ref && h.Ref.Value == 42)
 Print(owner && owner.Value == 42)
 h.Ref = owner
 Print(h.Ref && h.Ref.Value == 42)
 Print(!h.Ref || owner.Value == 0)
}''', 0, 1, 0, 1, 1, 0, 'item gone')

    def test_conditional_expression_selects_on_a_pointer(self):
        self.check('''
Scenario() {
 Item* owner()
 alias : owner
 Print(alias ? 1 | 2)
 owner = null
 Print(alias ? 1 | 2)
}''', 1, 'item gone', 2)

    def test_while_walks_a_chain_until_null(self):
        self.check('''
Scenario() {
 Node* head()
 head.Id = 1
 head.Next :: Node()
 head.Next.Id = 2
 cursor : head
 while cursor {
  Print(cursor.Id)
  cursor = cursor.Next
 }
 Print(cursor == null)
}''', 1, 2, 1, 'node gone', 'node gone')

    def test_do_and_for_conditions(self):
        self.check('''
Scenario() {
 Node* head()
 head.Next :: Node()
 cursor : head
 int visited = 0
 do {
  visited++
  cursor = cursor.Next
 } while cursor
 Print(visited)
 int counted = 0
 for Node* unused = null; head.Next; {
  counted++
  head.Next = null
 }
 Print(counted)
}''', 2, 'node gone', 1, 'node gone')

    def test_while_stops_when_the_object_expires(self):
        self.check('''
Scenario() {
 Item* owner()
 alias : owner
 int turns = 0
 while alias {
  turns++
  owner = null
 }
 Print(turns)
}''', 'item gone', 1)

    def test_tested_parameter_stays_a_proven_borrow(self):
        self.check('''
int Value(Item* e) {
 if !e
  return 0
 return e.Value
}
int Twice(Item* e) {
 int n = 0
 while e && n < 2
  n++
 return e ? n | 0
}
Scenario() {
 Item* o()
 Print(Value(o))
 Print(Twice(o))
}''', 42, 2, 'item gone')

    def test_compound_condition_in_if_is_a_plain_test(self):
        self.check('''
Scenario() {
 Item* a()
 Item* b()
 first : a
 second : b
 if first && second
  Print("both")
 a = null
 if first && second
  Print("no")
 if !first
  Print("first gone")
}''', 'both', 'item gone', 'first gone', 'item gone')

    def test_nonescaping_parameter_may_be_tested(self):
        self.check('''
int Read(Item* e) {
 if e
  return e.Value
 return 0
}
Scenario() {
 Item* o()
 Print(Read(o))
}''', 42, 'item gone')


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0]] + rest, verbosity=1)
