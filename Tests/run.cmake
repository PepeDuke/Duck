file(MAKE_DIRECTORY "${WORK}")

function(run_program name source expected)
  file(WRITE "${WORK}/${name}.dk" "${source}")
  run_files("${name}" "${expected}" "${WORK}/${name}.dk")
endfunction()

function(trap_program name source)
  file(WRITE "${WORK}/${name}.dk" "${source}")
  execute_process(COMMAND "${COMPILER}" "${WORK}/${name}.dk" -o "${WORK}/${name}.ll" RESULT_VARIABLE status ERROR_VARIABLE errors)
  if(NOT status EQUAL 0)
    message(FATAL_ERROR "${name}: compilation failed: ${errors}")
  endif()
  file(READ "${WORK}/${name}.ll" ir)
  if(NOT ir MATCHES "call void @llvm.trap")
    message(FATAL_ERROR "${name}: missing runtime trap")
  endif()
  foreach(optimization IN ITEMS -O0 -O2)
    execute_process(COMMAND "${CLANG}" ${optimization} "${WORK}/${name}.ll" -o "${WORK}/${name}.exe" RESULT_VARIABLE status ERROR_VARIABLE errors)
    if(NOT status EQUAL 0)
      message(FATAL_ERROR "${name} ${optimization}: LLVM/link failure: ${errors}")
    endif()
    execute_process(COMMAND "${WORK}/${name}.exe" RESULT_VARIABLE status OUTPUT_VARIABLE actual ERROR_VARIABLE errors TIMEOUT 10)
    if("${status}" STREQUAL "0" OR "${status}" MATCHES "[Tt]imeout|[Nn]o such|[Pp]ermission")
      message(FATAL_ERROR "${name} ${optimization}: expected runtime trap, got ${status}: ${errors}")
    endif()
  endforeach()
endfunction()

function(run_files name expected)
  execute_process(COMMAND "${COMPILER}" ${ARGN} -o "${WORK}/${name}.ll" RESULT_VARIABLE status ERROR_VARIABLE errors)
  if(NOT status EQUAL 0)
    message(FATAL_ERROR "${name}: compilation failed: ${errors}")
  endif()
  foreach(optimization IN ITEMS -O0 -O2)
    execute_process(COMMAND "${CLANG}" ${optimization} "${WORK}/${name}.ll" -o "${WORK}/${name}.exe" RESULT_VARIABLE status ERROR_VARIABLE errors)
    if(NOT status EQUAL 0)
      message(FATAL_ERROR "${name} ${optimization}: LLVM/link failure: ${errors}")
    endif()
    execute_process(COMMAND "${WORK}/${name}.exe" RESULT_VARIABLE status OUTPUT_VARIABLE actual ERROR_VARIABLE errors TIMEOUT 10)
    string(REPLACE "\r\n" "\n" actual "${actual}")
    if(NOT status EQUAL 0 OR NOT actual STREQUAL expected)
      message(FATAL_ERROR "${name} ${optimization}: exit=${status}, expected=[${expected}], actual=[${actual}], stderr=${errors}")
    endif()
  endforeach()
endfunction()

function(reject name source diagnostic)
  file(WRITE "${WORK}/${name}.dk" "${source}")
  file(REMOVE "${WORK}/${name}.ll")
  execute_process(COMMAND "${COMPILER}" "${WORK}/${name}.dk" -o "${WORK}/${name}.ll" RESULT_VARIABLE status ERROR_VARIABLE errors)
  if(NOT status EQUAL 1 OR NOT errors MATCHES "${diagnostic}" OR NOT errors MATCHES ":[0-9]+:[0-9]+: error:" OR EXISTS "${WORK}/${name}.ll")
    message(FATAL_ERROR "${name}: expected rejection matching ${diagnostic}, got ${status}: ${errors}")
  endif()
endfunction()

file(READ "${SOURCE}/Examples/Tutorial/01_HelloWorld.dk" hello)
run_program(hello "${hello}" "Hello world\n")
file(READ "${SOURCE}/Examples/Bootstrap/01_Core.dk" core)
run_program(core "${core}" "42\n0\n1\n2\n120\n1\n100% Duck\n")
run_program(scopes [=[
Main() {
 int n = 7
 if true { int n = 3
 Print(n) }
 Print(n)
 Print(-2147483648)
 Print(2 + 3 * 4)
 Print((2 + 3) * 4)
 Print(!false)
 Print(-7 / 2)
 Print(7 % 3)
}
]=] "3\n7\n-2147483648\n14\n20\n1\n-3\n1\n")
run_program(early_return [=[
int Choose(bool yes) {
 if yes { return 10 } else { return 20 }
}
Main() { Print(Choose(false))
Print(Choose(true)) }
]=] "20\n10\n")

file(READ "${SOURCE}/Examples/Tutorial/06_ConditionsAndLoops.dk" conditions)
run_program(conditions "${conditions}" "small\n0\n1\n2\n")
file(READ "${SOURCE}/Examples/Tutorial/03_FunctionsAndReferences.dk" references)
run_program(references "${references}" "10\n6\n")
file(READ "${SOURCE}/Examples/Bootstrap/03_ReferencesAndConstants.dk" constants)
run_program(constants "${constants}" "12\n1\n10\n")
run_program(reference_aliases [=[
Change(int& first, int& second) { first = 10
second += first }
Forward(int& value) { Change(value, value) }
Copy(int value) { value = 99
Print(value) }
Count(int& value, int remaining) {
 if remaining > 0 { value++
 Count(value, remaining - 1) }
}
bool Flip(bool& flag) { flag = !flag
return flag }
Main() {
 n : 1
 Forward(n)
 Print(n)
 Copy(n)
 Print(n)
 Count(n, 3)
 Print(n)
 bool flag = false
 Print(false && Flip(flag))
 Print(flag)
 Print(true ? Flip(flag) | false)
 Print(flag)
}
]=] "20\n99\n20\n23\n0\n0\n1\n1\n")
run_program(reference_evaluation [=[
int Increment(int& value) { value++
return value }
Show(int first, int second) { Print(first)
Print(second) }
Main() {
 n : 1
 Show(Increment(n), Increment(n))
 Print(n)
 for int i = 0; i < 2; i++ { Increment(n) }
 Print(n)
}
]=] "2\n3\n3\n5\n")
run_program(constant_scopes [=[
int Value(int n) { return n + 1 }
Main() {
 #int limit = Value(4)
 #bool enabled = limit > 0
 if enabled { int limit = 2
 limit++
 Print(limit) }
 Print(limit)
 Print(Value(limit))
}
]=] "3\n5\n6\n")
file(READ "${SOURCE}/Examples/Bootstrap/02_ControlFlow.dk" control)
run_program(control "${control}" "9\nselected\n1\n")
run_program(short_circuit [=[
bool Mark(int n) { Print(n)
return true }
int Value(int n) { Print(n)
return n }
Main() {
 Print(false && Mark(100))
 Print(true || Mark(200))
 Print(true && Mark(3))
 Print(false || Mark(4))
 Print(false || true && false)
 Print((true || false) && (false || Mark(5)))
 Print(true ? Value(6) | Value(100))
 Print(false ? Value(100) | false ? Value(100) | Value(7))
 Print(true ? false ? 100 | 8 | 100)
 Print((true ? false | true) || (false ? true | Mark(9)))
}
]=] "0\n1\n3\n1\n4\n1\n0\n5\n1\n6\n6\n7\n7\n8\n9\n1\n")
run_program(nested_loops [=[
Main() {
 int sum = 0
 int i = 99
 for int i = 0; i < 3; i++ {
  for int j = 0; j < 4; j++ {
   if j == 1 { continue }
   if j == 3 { break }
   sum += i * 10 + j
  }
 }
 Print(sum)
 Print(i)
 int n = 0
 while n < 5 {
  n++
  if n < 3 { continue } else if n == 4 { break } else { Print(n) }
 }
 for ;; { break }
 for ; n < 6; { n++ }
 Print(n)
 for int skipped = 0; false; skipped++ { Print(100) }
}
]=] "66\n99\n3\n6\n")
run_program(compound [=[
Main() {
 n : 10
 n += 4
 n -= 2
 n *= 3
 n /= 4
 n %= 5
 Print(n)
 n += true ? 2 | 100
 Print(n)
 label : true ? "left" | "right"
 label = "updated"
 Print(label)
}
]=] "4\n6\nupdated\n")
run_program(loop_returns [=[
int Find() {
 for int i = 0; i < 3; i++ {
  if i == 1 { return i } else { continue }
 }
 return -1
}
Main() {
 Print(Find())
 for int i = 0; i < 3; i++ {
  if i == 0 { continue } else { break }
 }
 Print(2)
}
]=] "1\n2\n")

reject(type "Main() { int n = true }" "type mismatch")
reject(unknown "Main() { Print(missing) }" "unknown variable")
reject(case "Main() { print(1) }" "unknown function")
reject(return "int F() { if true { return 1 } }\nMain() {}" "not all paths return")
reject(duplicate "Main() { n : 1\nn : 2 }" "duplicate variable")
reject(parameter "F(int n, int n) {}\nMain() {}" "duplicate variable")
reject(arity "F(int n) {}\nMain() { F() }" "wrong argument count")
reject(condition "Main() { while 1 {} }" "type mismatch")
reject(scope "Main() { if true { n : 1 }\nPrint(n) }" "unknown variable")
reject(unreachable "Main() { return\nPrint(1) }" "unreachable")
reject(overflow "Main() { int n = 2147483648 }" "type mismatch")
reject(literal "Main() { Print(123abc) }" "invalid integer literal")
reject(string "Main() { Print(\"oops) }" "unterminated string")
reject(comment "Main() { /* oops" "unterminated comment")
reject(brace "Main() {" "expected '}'")
reject(semicolon_commands "Main() { Print(1); Print(2) }" "expected newline")
reject(semicolon_trailing "Main() { Print(1); }" "expected newline")
reject(semicolon_fields "Example { int first; int second }\nMain() {}" "member terminator")
reject(compound_without_newline "Main() { if true {} Print(1) }" "newline between statements")
run_program(instance_container [=[
Main()
{
    Example local()
    local.Increment()
    Example copy = local
    Example named = {Val : 9}
    Example* cloned = named
    Print(cloned.Val)
    Example* owner = {Val : 5}
    owner.Increment()
    Print(copy.Val)
    Print(owner.Val)
    inferred :: Example()
    Print(inferred.Val)
}
Example
{
    int Val
    Increment()
    {
        Val++
    }
}
]=] "9\n1\n6\n0\n")
reject(separator "Main() { Print(1) Print(2) }" "expected newline")
reject(entry "main() {}" "program requires Main")
reject(entry_signature "int Main() { return 0 }" "bootstrap entry point")
reject(unsupported "Main() { element n = 1 }" "syntax may not be supported")
reject(void_value "F() {}\nMain() { n : F() }" "locals currently support")
reject(break_outside "Main() { break }" "requires an enclosing loop")
reject(continue_outside "Main() { continue }" "requires an enclosing loop")
reject(loop_scope "Main() { for int i = 0; i < 1; i++ {} \nPrint(i) }" "unknown variable")
reject(loop_step_scope "Main() { for int i = 0; i < 1; n++ { n : 0 } }" "unknown variable")
reject(loop_step_declaration "Main() { for ;; int n = 0 {} }" "step cannot declare")
reject(loop_condition "Main() { for ; 1; {} }" "type mismatch")
reject(loop_separator "Main() { for int i = 0 i < 1; i++ {} }" "expected ';'")
reject(after_break "Main() { while true { break\nPrint(1) } }" "unreachable")
reject(compound_bool "Main() { b : true\nb += 1 }" "type mismatch")
reject(compound_operand "Main() { n : 1\nn += false }" "type mismatch")
reject(logical_operand "Main() { Print(true && 1) }" "type mismatch")
reject(logical_unselected "Main() { Print(false && Missing()) }" "unknown function")
reject(conditional_type "Main() { Print(true ? 1 | false) }" "type mismatch")
reject(conditional_void "Main() { true ? Print(1) | Print(2) }" "must produce values")
reject(conditional_condition "Main() { Print(1 ? 2 | 3) }" "type mismatch")
reject(conditional_separator "Main() { Print(true ? 2 : 3) }" "expected '\\|'")
reject(keyword_name "Main() { int break = 1 }" "reserved keyword")
reject(reference_literal "F(int& n) {}\nMain() { F(1) }" "must be a mutable variable")
reject(reference_expression "F(int& n) {}\nMain() { n : 1\nF(n + 1) }" "must be a mutable variable")
reject(reference_call "int Get() { return 1 }\nF(int& n) {}\nMain() { F(Get()) }" "must be a mutable variable")
reject(reference_type "F(int& n) {}\nMain() { b : true\nF(b) }" "type mismatch")
reject(reference_unknown "F(int& n) {}\nMain() { F(missing) }" "unknown variable")
reject(reference_constant "F(int& n) {}\nMain() { #int n = 1\nF(n) }" "cannot pass a constant")
reject(reference_duplicate "F(int& n, int& n) {}\nMain() {}" "duplicate variable")
reject(reference_local_duplicate "F(int& n) { int n = 1 }\nMain() {}" "duplicate variable")
reject(reference_return "int& F(int& n) { return n }\nMain() {}" "expected identifier")
reject(constant_assignment "Main() { #int n = 1\nn = 2 }" "cannot modify constant")
reject(constant_increment "Main() { #int n = 1\nn++ }" "cannot modify constant")
reject(constant_compound "Main() { #int n = 1\nn += 2 }" "cannot modify constant")
reject(constant_inference "Main() { #n : 1 }" "requires an explicit type")
reject(constant_initializer "Main() { #int n }" "requires an initializer")
# README: #int age = 5 is a constant, so a later age = 6 is rejected.
run_program(constant_readme "Main() {\n#int age = 5\nPrint(age)\n}" "5\n")
reject(constant_readme_reassign "Main() { #int age = 5\nage = 6 }" "cannot modify constant 'age'")
file(READ "${SOURCE}/Examples/Tutorial/02_ValuesAndInference.dk" values)
run_program(values "${values}" "54\n55\n8\n")
file(READ "${SOURCE}/Examples/Bootstrap/04_FloatingPoint.dk" floating)
run_program(floating "${floating}" "2.5\n5\n3\n1\n")
run_program(numeric_operations [=[
double ReturnInt() { return 3 }
double ReturnLiteral() { return -1.25 }
Main() {
 x : 1.5
 Print(x + 2)
 Print(2 * x)
 Print(x / 2)
 Print(-x)
 x += 2
 x -= 1
 x *= 2
 x /= 4
 x++
 x--
 Print(x)
 Print(1.25e+2)
 Print(125e-2)
 Print(ReturnInt())
 Print(ReturnLiteral())
 double d = 2
 d = x
 Print(d)
 Print(false ? 1 | x)
 Print(true ? d | 1)
}
]=] "3.5\n3\n0.75\n-1.5\n1.25\n125\n1.25\n3\n-1.25\n1.25\n1.25\n1.25\n")
run_program(numeric_precision [=[
double Identity(double n) { return n }
double Literal() { return 16777217.0 }
Main() {
 double precise = 16777217.0
 inferred : 16777217.0
 Print(precise == 16777217)
 Print(inferred == precise)
 Print(Identity(16777217.0) == precise)
 Print(Literal() == precise)
 double chosen = true ? 16777217.0 | 0
 Print(chosen == precise)
 double negative = -16777217.0
 Print(negative == -precise)
}
]=] "1\n0\n1\n1\n1\n1\n")
run_program(numeric_comparisons [=[
Main() {
 double zero = 0.0
 nan : zero / zero
 Print(nan == nan)
 Print(nan != nan)
 Print(nan < zero)
 Print(nan >= zero)
 Print(1.5 < 2)
 Print(2 > 1.5)
 Print(2.0 <= 2)
 Print(2 >= 2.0)
 double count = 0
 while count < 2 { count += 0.5 }
 Print(count)
}
]=] "0\n1\n0\n0\n1\n1\n1\n1\n2\n")
reject(narrow_int "Main() { int n = 1.5 }" "type mismatch")
reject(narrow_float "Main() { double d = 1.5\nfloat f = d }" "type mismatch")
reject(narrow_return "int F() { return 1.5 }\nMain() {}" "type mismatch")
reject(narrow_compound "Main() { n : 1\nn += 1.5 }" "type mismatch")
reject(float_bool "Main() { float f = true }" "type mismatch")
reject(float_condition "Main() { if 1.5 {} }" "type mismatch")
reject(float_remainder "Main() { Print(1.5 % 1.0) }" "type mismatch")
reject(float_remainder_assign "Main() { f : 1.5\nf %= 1 }" "type mismatch")
reject(float_reference "F(double& n) {}\nMain() { f : 1.0\nF(f) }" "type mismatch")
reject(float_constant "Main() { #double d = 1\nd++ }" "cannot modify constant")
reject(float_overflow "Main() { f : 1e100 }" "out-of-range floating literal")
reject(double_overflow "Main() { double d = 1e999 }" "out-of-range floating literal")
reject(float_suffix "Main() { f : 1.5abc }" "invalid or out-of-range")
reject(float_exponent "Main() { f : 1e+ }" "exponent requires digits")
file(READ "${SOURCE}/Examples/Bootstrap/05_DefiniteAssignment.dk" initialization)
run_program(initialization "${initialization}" "10\n2.5\n1\n")
run_program(initialization_branches [=[
int Choose(bool a, bool b) {
 int n
 if a {
  if b { n = 1 } else { n = 2 }
 } else { n = 3 }
 return n
}
int Guard(bool early) {
 int n
 if early { return 7 } else { n = 8 }
 return n
}
Main() {
 Print(Choose(true, false))
 Print(Choose(false, false))
 Print(Guard(true))
 Print(Guard(false))
 int n
 n = 5
 if true { int n
 n = 9
 Print(n) }
 Print(n)
}
]=] "2\n3\n7\n8\n9\n5\n")
run_program(initialization_loops [=[
Main() {
 int n
 for n = 0; n < 3; n++ { Print(n) }
 Print(n)
 int step
 for int i = 0; i < 3; i += step {
  if i == 0 { step = 1
  continue } else { step = 2 }
 }
 int count = 0
 while count < 3 {
  int local
  if count == 0 { count++
  continue } else { local = count }
  Print(local)
  count++
 }
 int j
 j = 0
 while j < 3 {
  int local
  if j == 2 { break } else { local = j + 10 }
  Print(local)
  j++
 }
}
]=] "0\n1\n2\n3\n1\n2\n10\n11\n")
run_program(initialization_types [=[
Increment(int& n) { n++ }
Main() {
 int n
 n = 4
 Increment(n)
 float f
 f = n
 double d
 d = f
 bool b
 b = d == 5
 Print(f)
 Print(d)
 Print(b)
 int unused
}
]=] "5\n5\n1\n")
reject(uninitialized_read "Main() { int n\nPrint(n) }" "read before assignment")
reject(uninitialized_self "Main() { int n\nn = n + 1 }" "read before assignment")
reject(uninitialized_increment "Main() { int n\nn++ }" "read before assignment")
reject(uninitialized_compound "Main() { int n\nn += 1 }" "read before assignment")
reject(uninitialized_reference "Set(int& n) { n = 1 }\nMain() { int n\nSet(n) }" "read before assignment")
reject(uninitialized_branch "Main() { int n\nif true { n = 1 }\nPrint(n) }" "read before assignment")
reject(uninitialized_else "Main() { int n\nif true { n = 1 } else { Print(n) } }" "read before assignment")
reject(uninitialized_shadow "Main() { int n\nif true { int n = 1 } else { int n = 2 }\nPrint(n) }" "read before assignment")
reject(uninitialized_loop "Main() { int n\nwhile false { n = 1 }\nPrint(n) }" "read before assignment")
reject(uninitialized_break "Main() { int n\nwhile true { n = 1\nbreak }\nPrint(n) }" "read before assignment")
reject(uninitialized_step "Main() { int step\nfor int i = 0; i < 3; i += step { if i == 0 { continue }\nstep = 1 } }" "read before assignment")
reject(uninitialized_after_step "Main() { int n\nfor int i = 0; i < 0; n = 1 { break }\nPrint(n) }" "read before assignment")
reject(uninitialized_next_iteration "Main() { int n\nfor int i = 0; i < 2; i++ { Print(n)\nn = 1 } }" "read before assignment")
reject(uninitialized_short_circuit "Main() { bool b\nPrint(false && b) }" "read before assignment")
reject(uninitialized_ternary "Main() { int n\nPrint(true ? 1 | n) }" "read before assignment")
reject(uninitialized_bool "Main() { bool b\nif b {} }" "read before assignment")
reject(uninitialized_duplicate "Main() { int n\nint n }" "duplicate variable")
file(READ "${SOURCE}/Examples/Bootstrap/06_LoopBodies.dk" loop_bodies)
run_program(loop_bodies "${loop_bodies}" "2\n3\n9\n4\n")
run_program(single_statements [=[
int Choose(bool first, bool second) {
 if first
  if second
   return 1
  else
   return 2
 else
  return 3
}
Main() {
 Print(Choose(true, true))
 Print(Choose(true, false))
 Print(Choose(false, true))
 if false Print(100)
 else Print(4)
 int count = 0
 for int i = 0; i < 3; i++
  count += i
 Print(count)
 do count++
 while count < 5
 Print(count)
 if false
  Print(100)
 Print(6)
}
]=] "1\n2\n3\n4\n3\n5\n6\n")
run_program(do_assignment [=[
int Choose(bool stop) {
 int n
 do {
  if stop { n = 7
  break }
  n = 8
 } while false
 return n
}
int Early(bool first) {
 do { if first return 9
 else return 10 } while false
}
Main() {
 int n
 do n = 1
 while false
 Print(n)
 int result
 bool again = true
 do {
  result = 5
  again = false
  continue
 } while again
 Print(result)
 Print(Choose(true))
 Print(Choose(false))
 Print(Early(true))
 Print(Early(false))
}
]=] "1\n5\n7\n8\n9\n10\n")
run_program(nested_do [=[
bool Check(int& calls) { calls++
return calls < 3 }
Main() {
 int calls = 0
 int bodies = 0
 do {
  bodies++
  do { break } while true
  continue
 } while Check(calls)
 Print(bodies)
 Print(calls)
 do { break } while Check(calls)
 Print(calls)
 int n
 { n = 12 }
 Print(n)
 { int n = 20
 Print(n) }
 Print(n)
}
]=] "3\n3\n3\n12\n20\n12\n")
reject(do_missing_while "Main() { do {} }" "expected 'while'")
reject(do_condition "Main() { do {} while 1 }" "type mismatch")
reject(do_condition_unassigned "Main() { bool again\ndo { continue } while again }" "read before assignment")
reject(do_break_unassigned "Main() { int n\ndo { if true break\nn = 1 } while false\nPrint(n) }" "read before assignment")
reject(do_body_scope "Main() { do { int n = 1 } while n > 0 }" "unknown variable")
reject(do_unreachable "Main() { do { return } while false\nPrint(1) }" "unreachable statement")
reject(single_body_scope "Main() { if true int n = 1\nPrint(n) }" "unknown variable")
reject(block_scope "Main() { { int n = 1 }\nPrint(n) }" "unknown variable")
reject(block_unreachable "Main() { { return }\nPrint(1) }" "unreachable statement")
reject(empty_body "Main() { if true; }" "expected identifier")
reject(do_keyword "Main() { int do = 1 }" "reserved keyword")
run_files(multiple_files "42\n1\n"
  "${SOURCE}/Examples/Bootstrap/MultiFile/Main.dk"
  "${SOURCE}/Examples/Bootstrap/MultiFile/Even.dk"
  "${SOURCE}/Examples/Bootstrap/MultiFile/Odd.dk")
run_files(multiple_files_reversed "42\n1\n"
  "${SOURCE}/Examples/Bootstrap/MultiFile/Odd.dk"
  "${SOURCE}/Examples/Bootstrap/MultiFile/Even.dk"
  "${SOURCE}/Examples/Bootstrap/MultiFile/Main.dk")
file(WRITE "${WORK}/helper with spaces.dk" "int Helper() { return 12 }")
file(WRITE "${WORK}/caller with spaces.dk" "Main() { Print(Helper()) }")
run_files(paths_with_spaces "12\n" "${WORK}/caller with spaces.dk" "${WORK}/helper with spaces.dk")

function(reject_file name source diagnostic)
  file(WRITE "${WORK}/${name}.dk" "${source}")
  file(REMOVE "${WORK}/${name}.ll")
  execute_process(COMMAND "${COMPILER}" "${WORK}/caller with spaces.dk" "${WORK}/${name}.dk" -o "${WORK}/${name}.ll" RESULT_VARIABLE status ERROR_VARIABLE errors)
  string(FIND "${errors}" "${WORK}/${name}.dk:" location)
  if(NOT status EQUAL 1 OR location LESS 0 OR NOT errors MATCHES "${diagnostic}" OR EXISTS "${WORK}/${name}.ll")
    message(FATAL_ERROR "${name}: expected file-specific ${diagnostic}, got ${status}: ${errors}")
  endif()
endfunction()
reject_file(file_lexical "/* unclosed" "unterminated comment")
reject_file(file_parse "int Helper() {" "expected '}'")
reject_file(file_semantic "int Helper() { return true }" "type mismatch")
reject_file(file_duplicate "Main() {}" "duplicate or reserved function")

# Container declarations merge across files in input order.
file(WRITE "${WORK}/merge_first.dk" [=[
Animal
{
    int Legs = 4
}
Cat : Animal
{
    string Name = "Tom"
    int Lives = 9
}
#{
    int Count
}
]=])
file(WRITE "${WORK}/merge_second.dk" [=[
Cat : Animal
{
    int Whiskers = 12
    int Total() { return Legs + Lives + Whiskers + Count }
}
Cat
#{
    Cat Make()
    {
        Count++
        Cat c()
        return c
    }
}
Main()
{
    Cat c = Cat.Make()
    Print(c.Total())
    Print(c.Name)
    Cat d = {Whiskers: 1, Name: "Kit"}
    Print(d.Whiskers + d.Lives)
}
]=])
run_files(merge_containers "26\nTom\n10\n" "${WORK}/merge_first.dk" "${WORK}/merge_second.dk")
run_files(merge_containers_reversed "26\nTom\n10\n" "${WORK}/merge_second.dk" "${WORK}/merge_first.dk")
file(READ "${WORK}/merge_containers.ll" merged_ir)
file(READ "${WORK}/merge_containers_reversed.ll" reversed_ir)
if(NOT merged_ir MATCHES "%struct.Cat = type { %struct.Animal, ptr, i32, i32 }" OR
   NOT reversed_ir MATCHES "%struct.Cat = type { %struct.Animal, i32, ptr, i32 }")
  message(FATAL_ERROR "merged container layout must follow input order")
endif()
# The second file's declaration is the one diagnosed, pointing back at the first.
function(reject_merge name first second diagnostic)
  file(WRITE "${WORK}/${name}_a.dk" "${first}")
  file(WRITE "${WORK}/${name}_b.dk" "${second}")
  execute_process(COMMAND "${COMPILER}" "${WORK}/${name}_a.dk" "${WORK}/${name}_b.dk" -o "${WORK}/${name}.ll"
                  RESULT_VARIABLE status ERROR_VARIABLE errors)
  string(FIND "${errors}" "${WORK}/${name}_b.dk:" location)
  string(FIND "${errors}" " at ${WORK}/${name}_a.dk:" earlier)
  if(NOT status EQUAL 1 OR location LESS 0 OR earlier LESS 0 OR NOT errors MATCHES "${diagnostic}")
    message(FATAL_ERROR "${name}: expected ${diagnostic} in the second file, got ${status}: ${errors}")
  endif()
endfunction()
reject_merge(merge_missing_parents "Animal { int L }\nCat : Animal { int a }\n" "Cat { int b }\nMain() {}\n" "is declared with parents 'Animal'")
reject_merge(merge_extra_parents "Animal { int L }\nCat { int a }\n" "Cat : Animal { int b }\nMain() {}\n" "is declared with no parents")
reject_merge(merge_parent_order "A { int a }\nB { int b }\nCat : A, B { int x }\n" "Cat : B, A { int y }\nMain() {}\n" "parents 'A, B'")
reject_merge(merge_duplicate_field "Cat { int a }\n" "Cat { int a }\nMain() {}\n" "'a' is already declared in container 'Cat'")
reject_merge(merge_duplicate_method "Cat { F() {} }\n" "Cat { F() {} }\nMain() {}\n" "'F' is already declared in container 'Cat'")
reject_merge(merge_field_after_method "Cat { F() {} }\n" "Cat { int F }\nMain() {}\n" "'F' is already declared in container 'Cat'")
reject_merge(merge_method_after_field "Cat { int F }\n" "Cat { F() {} }\nMain() {}\n" "'F' is already declared in container 'Cat'")
execute_process(COMMAND "${COMPILER}" "${WORK}/caller with spaces.dk" "${WORK}/nonexistent_input.dk" -o "${WORK}/missing.ll" RESULT_VARIABLE status ERROR_VARIABLE errors)
if(NOT status EQUAL 1 OR NOT errors MATCHES "nonexistent_input.dk: cannot open input file")
  message(FATAL_ERROR "Missing input diagnostic: ${errors}")
endif()
file(WRITE "${WORK}/protected_input.dk" "Main() {}")
execute_process(COMMAND "${COMPILER}" "${WORK}/protected_input.dk" -o "${WORK}/protected_input.dk" RESULT_VARIABLE status ERROR_VARIABLE errors)
file(READ "${WORK}/protected_input.dk" preserved)
if(NOT status EQUAL 1 OR NOT preserved STREQUAL "Main() {}")
  message(FATAL_ERROR "Input file was not protected from output overwrite")
endif()
file(READ "${SOURCE}/Examples/Bootstrap/07_StaticContainers.dk" static_containers)
run_program(static_containers "${static_containers}" "14\n28\n1\n")
run_program(container_lookup [=[
int Value() { return 1 }
int GlobalOnly() { return 9 }
Outer #{
 int Value() { return 2 }
 int Child() { return Inner.Value() }
 Inner #{
  int Value() { return 3 }
  int Local() { return Value() }
  int Parent() { return Outer.Value() }
  int Fallback() { return GlobalOnly() }
 }
}
Other #{ int Value() { return 4 } }
Main() {
 Print(Value())
 Print(Outer.Value())
 Print(Outer.Child())
 Print(Outer.Inner.Local())
 Print(Outer.Inner.Parent())
 Print(Outer.Inner.Fallback())
 Print(Other.Value())
}
]=] "1\n2\n3\n3\n2\n9\n4\n")
run_program(container_recursion [=[
Numbers #{
 bool Even(int n) { if n == 0 return true
 return Odd(n - 1) }
 Advance(int& n) { n++ }
}
Numbers #{
 bool Odd(int n) { if n == 0 return false
 return Even(n - 1) }
 double Scale(double n) { return n * 2 }
}
Main() {
 n : 5
 Numbers.Advance(n)
 Print(Numbers.Even(n))
 Print(Numbers.Scale(1.25))
}
]=] "1\n2.5\n")
reject(container_duplicate "Math #{ F() {} }\nMath #{ F() {} }\nMain() {}" "duplicate or reserved function")
reject(container_missing "Math #{ F() {} }\nMain() { Math.Missing() }" "unknown function")
reject(container_no_global_entry "Math #{ Main() {} }" "program requires Main")
reject(container_private_lookup "Math #{ F() {} }\nMain() { F() }" "unknown function")
reject(container_instance "Math { int n = true }\nMain() {}" "type mismatch")
reject(container_field "Math #{ #F() {} }\nMain() {}" "constant static field requires an explicit type")
run_program(static_fields [=[
Point
{
    int X
    int Y
    Move(int d) { X += d }
}
Size : int #@{ Small, Large }
Game #{
    int Score = 10
    #int Max = 100
    long Big = int.Max
    double Speed = 2
    float Gravity = -9.5
    bool Running = true
    Size Mode = Size.Large
    Point Origin
    int Zero
    -int Secret = 7
    void* Handle = null
    Add(int n) { Score += n }
    int Reveal() { return Secret }
    Level #{
        int Number = 3
        int Twice() { return Number * 2 + Score }
    }
}
Main()
{
    Game.Add(5)
    Print(Game.Score)
    Print(Game.Max)
    Print(Game.Big)
    Print(Game.Speed)
    Print(Game.Gravity)
    Print(Game.Running)
    Print(Game.Mode)
    Game.Origin.Move(4)
    Game.Origin.Y = 2
    Print(Game.Origin.X + Game.Origin.Y + Game.Zero)
    Print(Game.Reveal())
    Game.Level.Number++
    Print(Game.Level.Twice())
    Print(Game.Handle == null)
    int Score = 1
    Print(Score)
}
]=] "15\n100\n2147483647\n2\n-9.5\n1\nLarge\n6\n7\n23\n1\n1\n")
reject(static_constant_write "Game #{ #int Max = 1 }\nMain() { Game.Max = 2 }" "cannot modify constant")
reject(static_constant_missing "Game #{ #int Max }\nMain() {}" "requires an initializer")
reject(static_protected "Game #{ -int Secret }\nMain() { Print(Game.Secret) }" "'Secret' is a protected member of 'Game'")
reject(static_reads_later "Game #{ int B = A + 1\nint A = 1 }\nMain() {}" "static field 'Game.A' is initialized after 'Game.B'; static fields initialize in declaration order")
reject(static_reads_itself "Game #{ int A = A + 1 }\nMain() {}" "static field 'Game.A' cannot read itself")
reject(static_out_of_range "Game #{ byte A = 300 }\nMain() {}" "out of range")
reject(static_managed "Game #{ cstring Title }\nMain() {}" "static fields of type cstring are not supported yet")
run_program(field_defaults [=[
Named
{
    string Title = "Duck"
    name Tag = "tag"
    int Count = 3
    big Total
    string Note
    Rename(string s) { Title = s }
}
Pair
{
    Named Left
    Named Right
    double Weight = 1.5
}
Hero : Named
{
    string Motto = "quack"
}
Registry #{
    string Banner = "Welcome"
    name Code
    Named Last
}
Named Make(string s)
{
    Named n()
    n.Title = s + "!"
    return n
}
string Describe(Named n)
{
    return n.Title + "/" + (string)n.Tag
}
Main()
{
    Named a()
    Print(a.Title)
    Print(a.Count)
    Named b = a
    b.Rename("Goose" + "!")
    Print(a.Title)
    a = b
    b.Title = "Swan"
    Print(a.Title)
    Print(Describe(a))
    Named c = Make("Made")
    Print(c.Title)
    Make("Gone").Rename("x")
    Named d = {Count: 9}
    Print(d.Title)
    Print(d.Count)
    Print(Length(d.Note))
    Print(d.Total + 1)
    Pair p()
    p.Left.Title = "L" + "eft"
    p.Right = p.Left
    Print(p.Right.Title)
    Print(p.Weight)
    Hero h()
    Print(h.Title + " " + h.Motto)
    Registry.Banner = Registry.Banner + "!"
    Registry.Last = c
    Print(Registry.Banner + " " + Registry.Last.Title)
    Print(Length(Registry.Code))
    for i : 0; i < 1000; i++ {
        Named t = Make("loop")
        Pair q()
        q.Left = t
        q.Right = q.Left
        t = q.Right
    }
}
]=] "Duck\n3\nDuck\nGoose!\nGoose!/tag\nMade!\nDuck\n9\n0\n1\nLeft\n1.5\nDuck quack\nWelcome! Made!\n0\n")
reject(field_default_type "A { int n = \"x\" }\nMain() {}" "type mismatch")
reject(field_default_reads_field "A { int m = 1\nint n = m }\nMain() {}" "initializers cannot read fields; 'm' is a field of 'A'")
reject(field_default_calls_method "A { int n = Read()\nint Read() { return 1 } }\nMain() {}" "'Read' is an instance method")
reject(static_runtime_constant_write "G #{ #int A = 1 + 1 }\nMain() { G.A = 2 }" "cannot modify constant")
run_program(runtime_initializers [=[
Point
{
    int X = Seed()
    int Y = 2
    big Mass = 12345678901234567890
}
Named : Point
{
    string Title = Label("duck")
    Point Origin = {X: 100}
    int Count = Counter.Next()
}
Counter #{
    int Value = 10
    int Next()
    {
        Value++
        return Value
    }
}
Config #{
    int Base = 40
    int Answer = Base + 2
    #int Limit = Base * 10
    string Banner = "v" + Label("1")
    big Huge = 99999999999999999999
    Point Home = {X: Answer, Y: 7}
    Named Default
}
int Seed()
{
    Print("seed")
    return 5
}
string Label(string s) { return "<" + s + ">" }
Main()
{
    Print("main")
    Print(Config.Answer)
    Print(Config.Limit)
    Print(Config.Banner)
    Print(Config.Huge + 1)
    Print(Config.Home.X + Config.Home.Y)
    Print(Config.Default.Title)
    Print(Config.Default.Count)
    Named n()
    Print(n.X + n.Y)
    Print(n.Title)
    Print(n.Origin.X + n.Origin.Y)
    Print(n.Count)
    Print(n.Mass)
    Named m = {Title: "given", X: 1}
    Print(m.Title)
    Print(m.X)
    Print(m.Count)
    for i : 0; i < 1000; i++ {
        Named t = m
        t.Title = t.Title + "!"
    }
}
]=] "seed\nmain\n42\n400\nv<1>\n100000000000000000000\n49\n<duck>\n11\nseed\n7\n<duck>\n102\n12\n12345678901234567890\ngiven\n1\n13\n")
run_program(managed_heap_containers [=[
Tag
{
    name Label = "t"
}
Named
{
    string Title = "Duck"
    big Mass = 12345678901234567890
    Tag Inner
    Rename(string s) { Title = s }
}
Named* Make(string s)
{
    Named* p()
    p.Title = s + "!"
    return ::p
}
Main()
{
    for i : 0; i < 1000; i++ {
        Named* a()
        a.Rename("a" + "b")
        a.Inner.Label = (name)("x" + "y")
        a.Mass = a.Mass + 1
        Named local()
        local.Title = "l" + "m"
        Named* b = local
        c :: Make("made")
        alias : c
        d :: c
        e : d
        d = null
        Named* f()
        f = null
    }
    p :: Make("final")
    Print(p.Title)
    Print(p.Inner.Label)
    Print(p.Mass + 1)
    alias : p
    p = null
    Print(alias == null)
}
]=] "final!\nt\n12345678901234567891\n1\n")
file(READ "${WORK}/managed_heap_containers.ll" heap_ir)
if(NOT heap_ir MATCHES "call void @duck.heap\\$drop\\(ptr [^,]+, i1 true, ptr @duck\\$cleanup.Named\\)")
  message(FATAL_ERROR "managed heap owners must drop with the container's cleanup")
endif()
reject(managed_pointer_cast "A { string s }\nMain() { void* v = null\nA* p = (A*)v }" "cannot cast to a pointer to a container with string, name, or big fields")
reject(managed_inherited_cast "A { name s }\nB : A { int x }\nMain() { void* v = null\nB* p = (B*)v }" "cannot cast to a pointer")
reject(managed_native_reference "A { string s }\nextern \"C\" F(A& a)\nMain() {}" "native parameters cannot reference a container with string, name, or big fields")
reject(managed_native_nested "A { string s }\nB { A a }\nextern \"C\" F(B& b)\nMain() {}" "native parameters cannot reference")
reject(static_duplicate "Game #{ int A\nint A }\nMain() {}" "duplicate static field or member 'Game.A'")
reject(static_function_clash "Game #{ int A\nA() {} }\nMain() {}" "duplicate static field or member 'Game.A'")
reject(static_void "Game #{ void A }\nMain() {}" "static field cannot have void type")
reject(static_file_scope "int Count = 1\nMain() {}" "expected '\\('")
reject(container_separated_chain "Math #{}\n\n#{}\nMain() {}" "chained container bodies must be adjacent")
reject(container_separated_instance "Math #{}\n\n{ int x }\nMain() {}" "chained container bodies must be adjacent")
file(READ "${SOURCE}/Examples/Tutorial/04_ContainersAndStaticBlocks.dk" containers_tutorial)
run_program(containers_tutorial "${containers_tutorial}" "7\n14\n")
run_program(container_chain [=[
Animal
{
    Walk() { Print("walk") }
}
Example : Animal
#{
    int Created
    Foo() { Print("static foo") }
    Example Make()
    {
        Created++
        Foo()
        Example e()
        return e
    }
}
{
    int Value = 7
    Foo()
    {
        Print("instance foo")
        Walk()
    }
    int Read() { return Value + Created }
}
#{
    int Twice(int n) { return n * 2 }
}
#{}
{
    Bump() { Value++ }
}
Main()
{
    Example.Foo()
    Example e = Example.Make()
    e.Foo()
    e.Bump()
    Print(e.Read())
    Print(Example.Twice(e.Value))
}
]=] "static foo\nstatic foo\ninstance foo\nwalk\n9\n16\n")
reject(chain_static_calls_instance "A\n#{\n int S() { return Read() }\n}\n{\n int Read() { return 1 }\n}\nMain() {}" "'Read' is an instance method; call it on a 'A' object")
reject(chain_type_calls_instance "A { int Read() { return 1 } }\nMain() { Print(A.Read()) }" "'A.Read' is an instance method")
reject(chain_instance_calls_static "A\n#{\n int S() { return 1 }\n}\n{\n int x\n}\nMain() { A a()\nPrint(a.S()) }" "unknown instance method")
reject(chain_duplicate_instance_member "A { int x }\n{ int x }\nMain() {}" "duplicate container member")
reject(chain_duplicate_static_member "A #{ F() {} }\n#{ F() {} }\nMain() {}" "duplicate or reserved function")
reject(chain_parents_without_instance "A : B #{}\nB { int x }\nMain() {}" "a parent list requires an instance body")
# Instance containers nested in static bodies are named by their enclosing
# path. Type paths resolve from the innermost container outward, then at file
# scope, in declarations, casts, parent lists, and construction.
run_program(nested_instance_types [=[
Space { int Value = 1 }
Name
#{
    Space
    #{
        int Count = 2
        Space Make()
        {
            Space s = {Value: Count}
            return s
        }
    }
    {
        int Value = 5
        int Twice() { return Value * 2 }
        Space Copy()
        {
            Space c = {Value: Value + 1}
            return c
        }
    }
    -int Secret = 7
    A
    #{
        B
        #{
            C
            {
                int V = 4
                int Peek() { return Secret }
            }
        }
        int FromA()
        {
            B.C c()
            return c.V
        }
    }
    Base { int X = 10 }
    Derived : Base { int Y = 1 }
    Space Build()
    {
        Space t = Space()
        return t
    }
}
Outer
{
    int X = 1
}
#{
    Inner : Outer { int Y = 2 }
}
Main()
{
    Space top()
    Print(top.Value)
    Name.Space sp()
    Print(sp.Twice())
    Name.Space made = Name.Space.Make()
    Print(made.Value)
    Name.Space copy = sp.Copy()
    Print(copy.Value)
    Name.Space built = Name.Build()
    Print(built.Value)
    Name.Space* p = (Name.Space*)null
    Print(p == null)
    Name.Space* h()
    Print(h.Value)
    Name.A.B.C c()
    Print(c.V + Name.A.FromA())
    Print(c.Peek())
    Name.Derived d()
    Print(d.X + d.Y)
    Outer.Inner i()
    Print(i.X + i.Y)
}
]=] "1\n10\n2\n6\n5\n1\n5\n8\n7\n11\n3\n")
file(WRITE "${WORK}/nested_first.dk" "Name #{ Space { int a = 1 } }\n")
file(WRITE "${WORK}/nested_second.dk" "Name #{ Space { int b = 2 } }\nMain() { Name.Space s()\nPrint(s.a + s.b) }\n")
run_files(nested_merge "3\n" "${WORK}/nested_first.dk" "${WORK}/nested_second.dk")
# An importing module uses a nested managed type by its qualified name,
# including the heap cleanup generated for it.
file(WRITE "${WORK}/nested_library.dk" "Lib\n#{\n    Item\n    {\n        string Label = \"lib\"\n        int N = 3\n        int Twice() { return N * 2 }\n    }\n    Item Make()\n    {\n        Item i = {N: 4}\n        return i\n    }\n}\n")
file(WRITE "${WORK}/nested_client.dk" "Main()\n{\n    Lib.Item i = Lib.Make()\n    Print(i.Twice())\n    Lib.Item* h()\n    Print(h.Label)\n}\n")
execute_process(COMMAND "${COMPILER}" --library "${WORK}/nested_library.dk" -o "${WORK}/nested_library.ll" RESULT_VARIABLE status ERROR_VARIABLE errors)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "nested_library: compilation failed: ${errors}")
endif()
execute_process(COMMAND "${COMPILER}" --dependency "${WORK}/nested_library.dk" "${WORK}/nested_client.dk" -o "${WORK}/nested_client.ll" RESULT_VARIABLE status ERROR_VARIABLE errors)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "nested_client: compilation failed: ${errors}")
endif()
execute_process(COMMAND "${CLANG}" "${WORK}/nested_library.ll" "${WORK}/nested_client.ll" -o "${WORK}/nested_client.exe" RESULT_VARIABLE status ERROR_VARIABLE errors)
execute_process(COMMAND "${WORK}/nested_client.exe" RESULT_VARIABLE status OUTPUT_VARIABLE actual)
string(REPLACE "\r\n" "\n" actual "${actual}")
if(NOT status EQUAL 0 OR NOT actual STREQUAL "8\nlib\n")
  message(FATAL_ERROR "nested_client: exit=${status}, actual=[${actual}]")
endif()
reject(nested_type_unqualified "Name #{ Space { int x } }\nMain() { Space s() }" "unknown type 'Space'; did you mean 'Name.Space'")
reject(nested_type_function_clash "Name #{ Space() {}\n Space { int x } }\nMain() {}" "'Name.Space' is already declared as a container")
reject(nested_type_static_clash "Name #{ int Space\n Space { int x } }\nMain() {}" "duplicate static field or member 'Name.Space'")
reject(nested_type_protected "Name #{ Space { -int x } }\nMain() { Name.Space s()\nPrint(s.x) }" "'x' is a protected member of 'Name.Space'")
# `using` imports a container's members into the file or static body that
# declares it. Each scope searches its own members, then its directives, before
# the enclosing scope, so an inner directive wins over an outer one.
file(WRITE "${WORK}/using_library.dk" [=[
Name
#{
    Space
    #{
        int Foo() { return 1 }
        int Count = 5
    }
    {
        int Foo2() { return 2 }
    }
}
Other
#{
    int Helper() { return 30 }
    Space { int V = 7 }
}
X #{ int Helper() { return 1 } }
]=])
file(WRITE "${WORK}/using_main.dk" [=[
using Name

Holder
#{
    using Other
    int Total = Helper() + Space.Count
    int Pick()
    {
        Space s()
        return s.V
    }
}
Defaults
{
    int Seed = Space.Foo() + 40
}
Main()
{
    Print(Space.Foo())
    Space sp()
    Print(sp.Foo2())
    Print(Space.Count)
    Space d = Space()
    Print(d.Foo2())
    Print(Holder.Total)
    Print(Holder.Pick())
    Defaults f()
    Print(f.Seed)
}
]=])
run_files(using_directives "1\n2\n5\n2\n35\n7\n41\n" "${WORK}/using_library.dk" "${WORK}/using_main.dk")
file(WRITE "${WORK}/using_own.dk" "using X\nusing Other\nint Helper() { return 9 }\nMain()\n{\n    Print(Helper())\n    int Space = 4\n    Print(Space)\n}\n")
run_files(using_own_and_local_win "9\n4\n" "${WORK}/using_library.dk" "${WORK}/using_own.dk")
function(reject_using name source diagnostic)
  file(WRITE "${WORK}/${name}.dk" "${source}")
  execute_process(COMMAND "${COMPILER}" "${WORK}/using_library.dk" "${WORK}/${name}.dk" -o "${WORK}/${name}.ll"
                  RESULT_VARIABLE status ERROR_VARIABLE errors)
  if(NOT status EQUAL 1 OR NOT errors MATCHES "${diagnostic}")
    message(FATAL_ERROR "${name}: expected ${diagnostic}, got ${status}: ${errors}")
  endif()
endfunction()
reject_using(using_ambiguous_type "using Name\nusing Other\nMain() { Space s() }" "'Space' is ambiguous between 'Name.Space' \\(using Name\\) and 'Other.Space' \\(using Other\\)")
reject_using(using_ambiguous_function "using X\nusing Other\nMain() { Print(Helper()) }" "'Helper' is ambiguous between 'X.Helper' \\(using X\\) and 'Other.Helper' \\(using Other\\)")
reject_using(using_unknown "using Nope\nMain() {}" "unknown container 'Nope' in using directive")
reject_using(using_duplicate "using Name\nusing Name\nMain() {}" "duplicate using directive for 'Name'")
reject_using(using_after_declaration "Main() {}\nusing Name\n" "using directives must come before the other declarations")
reject_using(using_lexical_scope "Holder #{ using Name }\nMain() { Print(Space.Foo()) }" "unknown function 'Space.Foo'")
# Constructors are chosen by argument types; destructors run exactly once per
# constructed object: at scope exit in reverse declaration order, when an
# assignment replaces the value, for temporaries, and when an owner drops a
# heap payload. Construction runs parents, then fields, then the body;
# destruction runs the body, then fields in reverse, then parents.
run_program(constructors_destructors [=[
Probe
{
    int Id
    Probe() { Print(0) }
    Probe(int id)
    {
        Id = id
        Print(Id)
    }
    Probe(int a, int b)
    {
        Id = a * 10 + b
        Print(Id)
    }
    ~Probe() { Print(-Id) }
}
Base
{
    Base() { Print("base") }
    ~Base() { Print("~base") }
}
Holder : Base
{
    Probe Inner
    Probe Second = Probe(7)
    string Label = "h"
    Holder() { Print("holder") }
    ~Holder() { Print("~holder") }
}
N
{
    N(long a) { Print("long") }
    N(int a) { Print("int") }
}
Probe Make(int id)
{
    Probe p(id)
    return p
}
Probe Pick(bool early)
{
    Probe t(1)
    if early { return t }
    Probe u(2)
    return u
}
Peek(Probe& p) { Print(p.Id + 100) }
Main()
{
    Probe a(1)
    Probe b = Probe(2, 3)
    Peek(a)
    {
        Probe c
    }
    Probe m = Make(4)
    m = Make(5)
    Make(6)
    Print("holder")
    {
        Holder h()
    }
    Print("heap")
    {
        Probe* p(8)
        Probe* alias = p
    }
    Print("moves")
    Probe early = Pick(true)
    Probe late = Pick(false)
    Print("loop")
    for int i = 0; i < 3; i++
    {
        Probe step(10 + i)
        if i == 1 { break }
    }
    N n(3)
    Print("end")
}
]=] "1\n23\n101\n0\n0\n4\n5\n-4\n6\n-6\nholder\nbase\n0\n7\nholder\n~holder\n-7\n0\n~base\nheap\n8\n-8\nmoves\n1\n1\n2\n-1\nloop\n10\n-10\n11\n-11\nint\nend\n-2\n-1\n-5\n-23\n-1\n")
file(WRITE "${WORK}/ctor_library.dk" "Kit\n{\n    int V\n    Kit(int v)\n    {\n        V = v\n        Print(V)\n    }\n    ~Kit() { Print(-V) }\n}\n")
file(WRITE "${WORK}/ctor_client.dk" "Main()\n{\n    Kit k(4)\n    Kit* h(6)\n}\n")
execute_process(COMMAND "${COMPILER}" --library "${WORK}/ctor_library.dk" -o "${WORK}/ctor_library.ll" RESULT_VARIABLE status ERROR_VARIABLE errors)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "ctor_library: compilation failed: ${errors}")
endif()
execute_process(COMMAND "${COMPILER}" --dependency "${WORK}/ctor_library.dk" "${WORK}/ctor_client.dk" -o "${WORK}/ctor_client.ll" RESULT_VARIABLE status ERROR_VARIABLE errors)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "ctor_client: compilation failed: ${errors}")
endif()
execute_process(COMMAND "${CLANG}" "${WORK}/ctor_library.ll" "${WORK}/ctor_client.ll" -o "${WORK}/ctor_client.exe" RESULT_VARIABLE status ERROR_VARIABLE errors)
execute_process(COMMAND "${WORK}/ctor_client.exe" RESULT_VARIABLE status OUTPUT_VARIABLE actual)
string(REPLACE "\r\n" "\n" actual "${actual}")
if(NOT status EQUAL 0 OR NOT actual STREQUAL "4\n6\n-6\n-4\n")
  message(FATAL_ERROR "ctor_client: exit=${status}, actual=[${actual}]")
endif()
set(probe "Probe\n{\n    int Id\n    Probe() {}\n    Probe(int id) { Id = id }\n    ~Probe() { Print(Id) }\n}\n")
reject(ctor_copy_declaration "${probe}Main() { Probe a(1)\nProbe b = a }" "cannot copy 'Probe': it has a destructor")
reject(ctor_copy_assignment "${probe}Main() { Probe a(1)\nProbe b(2)\nb = a }" "cannot copy 'Probe'")
reject(ctor_copy_dereference "${probe}Main() { Probe* p(1)\nProbe c = *p }" "cannot copy 'Probe'")
reject(ctor_return_reference "${probe}Probe Get(Probe& p) { return p }\nMain() {}" "cannot copy 'Probe'")
reject(ctor_value_parameter "${probe}Use(Probe p) {}\nMain() {}" "parameter 'p' must take 'Probe' by reference")
reject(ctor_static_field "${probe}S #{ Probe Keep }\nMain() {}" "static field 'S.Keep' cannot hold 'Probe'")
reject(ctor_named_initializer "${probe}Main() { Probe a = {Id: 1} }" "'Probe' has constructors; construct it with Probe")
reject(ctor_no_match "${probe}Main() { Probe a(\"x\") }" "no constructor of 'Probe' accepts \\(string\\); candidates: Probe\\(\\), Probe\\(int\\)")
reject(ctor_ambiguous "N\n{\n    N(long a) {}\n    N(double a) {}\n}\nMain() { N n(3) }" "constructor call N\\(int\\) is ambiguous between N\\(double\\) and N\\(long\\)")
reject(ctor_no_default "G { G(int a) {} }\nMain() { G g }" "'g' may be incomplete when its scope ends: G\\(...\\) may still be missing")
reject(ctor_field_no_default "G { G(int a) {} }\nW { G Inner }\nMain() {}" "field 'Inner' needs a default")
reject(ctor_parent_no_default "G { G(int a) {} }\nW : G { int x }\nMain() { W w = W() }" "this construction of 'W' leaves G\\(...\\) missing")
reject(ctor_destructor_name "G { ~H() {} }\nMain() {}" "a destructor is written ~G\\(\\)")
reject(ctor_destructor_parameters "G { ~G(int a) {} }\nMain() {}" "a destructor takes no parameters")
reject(ctor_duplicate_destructor "G { ~G() {}\n ~G() {} }\nMain() {}" "already declares a destructor")
reject(ctor_duplicate_constructor "G { G() {}\n G() {} }\nMain() {}" "already declares a constructor with these parameter types")
reject(ctor_result_type "G { int G() {} }\nMain() {}" "a constructor has no result type")
# Partial constructors (-Name(...)) are a container's partial part; any one
# overload completes it and a zero-argument one runs automatically. Composite
# construction splits flat arguments across parents' parts, then the
# container's partial part, then its regular part. A declared local may be
# left incomplete and completed later with local(...); until then it cannot
# be used as a whole, and it must be complete on every path out of its scope.
run_program(partial_constructors [=[
World { int Size = 9 }
Example
{
    -Example() { Print("Hello") }
    Example() { Print("World") }
}
Entity
{
    int Size
    -Entity(World& w)
    {
        Size = w.Size
        Print("entity")
    }
    ~Entity() { Print("~entity") }
}
Gun : Entity
{
    int Ammo
    Gun(int ammo)
    {
        Ammo = ammo
        Print("gun")
    }
    Fire() { Print(Ammo + Size) }
}
G { G(int a) { Print(a) } }
W : G { int X = 4 }
Arm(bool ok, World& w)
{
    Gun g(1)
    if ok { g(w) }
    else { g(w) }
    g.Fire()
}
Main()
{
    Example e()
    World w()
    Gun a(w, 5)
    a.Fire()
    Gun b(6)
    b.Ammo = 7
    b(w)
    b.Fire()
    Gun c
    c(w, 3)
    c.Fire()
    W x(2)
    Print(x.X)
    Arm(true, w)
    Print("end")
}
]=] "Hello\nWorld\nentity\ngun\n14\ngun\nentity\n16\nentity\ngun\n12\n2\n4\ngun\nentity\n10\n~entity\nend\n~entity\n~entity\n~entity\n")
set(gun "World { int Size = 9 }\nEntity\n{\n    -Entity(World& w) {}\n}\nGun : Entity\n{\n    int Ammo\n    Gun(int ammo) {}\n    Fire() {}\n}\n")
reject(partial_method_call "${gun}Main() { Gun g(5)\ng.Fire() }" "'g' may be incomplete: -Entity\\(...\\) may still be missing")
reject(partial_scope_end "${gun}Main() { Gun g(5) }" "'g' may be incomplete when its scope ends")
reject(partial_one_branch "${gun}Run(bool ok) { World w()\nGun g(5)\nif ok { g(w) }\ng.Fire() }\nMain() {}" "'g' may be incomplete")
reject(partial_twice "${gun}Run(bool ok) { World w()\nGun g(5)\nif ok { g(w) }\ng(w) }\nMain() {}" "'g' was completed on some paths but not others")
reject(partial_return "${gun}Run(bool ok) { World w()\nGun g(5)\nif ok { return }\ng(w) }\nMain() {}" "'g' may be incomplete when its scope ends")
reject(partial_loop "${gun}Main() { World w()\nGun g(5)\nwhile true\n{\ng(w)\nbreak\n}\n}" "complete 'g' outside the loop")
reject(partial_complete_again "${gun}Main() { World w()\nGun g(w, 5)\ng(w) }" "reinitializing 'g' leaves Gun\\(...\\) missing")
reject(partial_temporary "${gun}Main() { Gun g = Gun(5) }" "this construction of 'Gun' leaves -Entity\\(...\\) missing")
reject(partial_heap "${gun}Main() { Gun* p(5) }" "this construction of 'Gun' leaves -Entity\\(...\\) missing")
reject(partial_reference "${gun}Use(Gun& g) {}\nMain() { Gun g(5)\nUse(g) }" "'g' may be incomplete")
reject(partial_field "${gun}Box { Gun Inner }\nMain() {}" "field 'Inner' needs a default")
reject(partial_ambiguous "A\n{\n    -A(int x) {}\n    A(int y) {}\n}\nMain() { A a(1) }" "constructor call A\\(int\\) is ambiguous between -A\\(int\\) and A\\(int\\)")
reject(partial_duplicate "A\n{\n    -A() {}\n    -A() {}\n}\nMain() {}" "already declares a partial constructor with these parameter types")
# Calling a constructor on a complete object reinitializes it in place: the
# new state is built first (so arguments may read the old one), then the old
# object is destroyed and releases its fields, and the new state moves into
# the same storage, so references and heap aliases see the new object.
run_program(reinitialization [=[
Probe
{
    int Id
    Probe(int id)
    {
        Id = id
        Print(id)
    }
    Probe(Probe& other, int add)
    {
        Id = other.Id + add
        Print(Id)
    }
    ~Probe() { Print(-Id) }
    Show() { Print(Id) }
}
Box
{
    Probe Inner = Probe(10)
    string Label = "box"
}
Point
{
    int X = 3
    int Y
}
Named
{
    string Text
    Named(string text) { Text = text }
}
Bump(Probe& p) { p(p, 100) }
Main()
{
    Probe a(1)
    a(2)
    a.Show()
    a(a, 5)
    Bump(a)
    Probe* h(20)
    alias : h
    h(21)
    alias.Show()
    alias(22)
    h.Show()
    Box b()
    b.Inner(11)
    b.Inner.Show()
    Point pt()
    pt.Y = 9
    pt.X = 8
    pt()
    Print(pt.X)
    Print(pt.Y)
    Named n("first")
    n(n.Text + " second")
    Print(n.Text)
    int i = 0
    while i < 2
    {
        a(i)
        i = i + 1
    }
    Print("end")
}
]=] "1\n2\n-1\n2\n7\n-2\n107\n-7\n20\n21\n-20\n21\n22\n-21\n22\n10\n11\n-10\n11\n3\n0\nfirst second\n0\n-107\n1\n0\nend\n-11\n-22\n-1\n")
reject(reinit_constant "P { P(int a) {} }\nMain() { #P c = P(1)\nc(2) }" "cannot reinitialize constant 'c'")
reject(reinit_incomplete "${gun}Main() { World w()\nGun g(w, 5)\ng(6) }" "reinitializing 'g' leaves -Entity\\(...\\) missing")
reject(reinit_receiver "P\n{\n    P(int a) {}\n    Reset() { this(1) }\n}\nMain() {}" "cannot reinitialize the receiver inside its own method")
reject(reinit_constant_field "P { P(int a) {} }\nB { P Inner = P(1) }\nMain() { #B b = B()\nb.Inner(2) }" "cannot reinitialize field 'Inner' of a constant")
reject(reinit_temporary_field "P { P(int a) {} }\nB { P Inner = P(1) }\nB Make() { return B() }\nMain() { Make().Inner(2) }" "cannot reinitialize field 'Inner' of a temporary")
reject(reinit_protected_field "P { P(int a) {} }\nB { -P Inner = P(1) }\nMain() { B b()\nb.Inner(2) }" "'Inner' is a protected member of 'B'")
trap_program(reinit_moved_owner "P { int X }
Main() { P* h()
other :: h
h() }")
reject(reinit_no_match "P { P(int a) {} }\nMain() { P p(1)\np(true) }" "no constructor of 'P' accepts \\(bool\\)")
run_program(chain_constant_after_nested "Outer #{\n Inner #{}\n #int Max = 1\n}\nMain() { Print(Outer.Max) }" "1\n")
reject(container_unclosed "Math #{ F() {}" "expected '}'")
reject(container_reference "Math #{ F(int& n) {} }\nMain() { Math.F(1) }" "must be a mutable variable")
run_program(protected_members [=[
Account
{
    -int Cash
    +int Limit
    -int Bonus()
    {
        return 5
    }
    Fill(int amount)
    {
        Cash = amount
        Limit = Bonus()
    }
    int Total()
    {
        return Cash + Bonus()
    }
}
Vault #{
    -int Secret()
    {
        return 41
    }
    +int Open()
    {
        return Secret() + 1
    }
    Inner #{
        int Peek()
        {
            return Secret()
        }
    }
}
Main()
{
    Account account()
    account.Fill(10)
    Print(account.Total())
    Print(account.Limit)
    Print(Vault.Open())
    Print(Vault.Inner.Peek())
}
]=] "15\n5\n42\n41\n")
reject(protected_field_read "Account { -int Cash }\nMain() { Account a()\nPrint(a.Cash) }" "'Cash' is a protected member of 'Account'")
reject(protected_field_write "Account { -int Cash }\nMain() { Account a()\na.Cash = 1 }" "'Cash' is a protected member of 'Account'")
reject(protected_field_initializer "Account { -int Cash }\nMain() { Account a = {Cash: 1} }" "'Cash' is a protected member of 'Account'")
reject(protected_method "Account { -int Bonus() { return 5 } }\nMain() { Account a()\nPrint(a.Bonus()) }" "'Bonus' is a protected member of 'Account'")
reject(protected_static "Vault #{ -int Secret() { return 1 } }\nMain() { Print(Vault.Secret()) }" "'Secret' is a protected member of 'Vault'")
reject(protected_static_sibling "A #{ -int S() { return 1 } }\nB #{ int T() { return A.S() } }\nMain() {}" "'S' is a protected member of 'A'")
reject(protected_callback "Vault #{ -int Secret() { return 1 } }\nMain() { void* p = &Vault.Secret }" "'Secret' is a protected member of 'Vault'")
reject(access_modifier_space "Account { - int Cash }\nMain() {}" "access modifier '-' must be written directly before the member")
reject(access_modifier_newline "Vault #{ +\nF() {} }\nMain() {}" "access modifier '\\+' must be written directly before the member")
reject(access_modifier_file_scope "-F() {}\nMain() {}" "access modifiers apply only to container members")
reject(access_modifier_nested_container "Vault #{ -Inner #{} }\nMain() {}" "access modifiers on nested containers")
run_program(inheritance [=[
Animal
{
    -int Legs
    Walk() { Print("animal") }
    SetLegs(int n) { Legs = n }
}
Human
{
    int Height
    Walk() { Print("human") }
    Eat() { Print("eat") }
}
Cat : Animal,
      Human
{
    int Lives
    Demonstrate()
    {
        Walk()
        Eat()
        this.Human.Walk()
        Legs = 4
        Lives = 9
        Print(Legs + Height + Lives)
    }
}
Kitten : Cat
{
    int ReadLegs() { return Legs }
}
Size : int #@{ Small }
Main()
{
    Cat cat = {Lives: 1, Height: 30}
    cat.Demonstrate()
    cat.Walk()
    Kitten kitten()
    kitten.SetLegs(3)
    kitten.Height = 7
    Print(kitten.ReadLegs())
    Print(kitten.Height)
    Print(Size.Small)
}
]=] "animal\neat\nhuman\n43\nanimal\n3\n7\nSmall\n")
reject(inheritance_cycle "A : B { int x }\nB : A { int y }\nMain() {}" "inheritance cycle through")
reject(inheritance_self "A : A { int x }\nMain() {}" "inheritance cycle through 'A'")
reject(inheritance_unknown_parent "A : Missing { int x }\nMain() {}" "unknown parent container 'Missing'")
reject(inheritance_enum_parent "E : int #@{ V }\nA : E { int x }\nMain() {}" "unknown parent container 'E'")
reject(inheritance_duplicate_parent "A { int x }\nB : A, A { int y }\nMain() {}" "duplicate parent 'A'")
reject(inheritance_field_redeclared "A { int x }\nB : A { int x }\nMain() {}" "'x' is already inherited from 'A' as a field; fields cannot be overridden")
# A method redeclaring an inherited one overrides it. Calls on this (implicit
# or this.M()) dispatch to the final overrider of the dynamic object, through
# a dispatch table passed with the receiver; this.Parent.M() is a direct base
# call; constructors and destructors run their own container's versions.
run_program(overrides [=[
Animal
{
    int Legs = 4
    string Speak() { return "..." }
    Describe()
    {
        Print(Speak())
        Print(Legs)
    }
    Twice() { Describe() }
}
Walker
{
    int Steps = 1
    Walk() { Print("walker walks") }
    Run()
    {
        Walk()
        Walk()
    }
}
Cat : Animal, Walker
{
    string Name = "cat"
    string Speak() { return "meow" }
    Walk() { Print("cat walks " + Name) }
    Loud()
    {
        Print(this.Animal.Speak())
        this.Walker.Walk()
        Print(this.Speak())
    }
}
Lion : Cat
{
    string Speak() { return "roar" }
}
Base
{
    Base() { Hello() }
    ~Base() { Hello() }
    Hello() { Print("base hello") }
}
Child : Base
{
    Hello() { Print("child hello") }
}
Main()
{
    Animal a()
    a.Describe()
    Cat c()
    c.Describe()
    c.Twice()
    c.Run()
    c.Loud()
    Lion l()
    l.Describe()
    l.Run()
    l.Loud()
    Print(l.Speak())
    Lion* h()
    h.Twice()
    Child k()
    k.Hello()
    Print("end")
}
]=] "...\n4\nmeow\n4\nmeow\n4\ncat walks cat\ncat walks cat\n...\nwalker walks\nmeow\nroar\n4\ncat walks cat\ncat walks cat\n...\nwalker walks\nroar\nroar\nroar\n4\nbase hello\nchild hello\nend\nbase hello\n")
reject(override_parameters "A { Go(int x) {} }\nB : A { Go(long x) {} }\nMain() {}" "'Go' overrides A.Go\\(int\\) but is declared as Go\\(long\\)")
reject(override_result "A { int Go() { return 1 } }\nB : A { Go() {} }\nMain() {}" "'Go' overrides A.Go\\(\\) -> int but is declared as Go\\(\\)")
reject(override_access "A { -Go() {} }\nB : A { Go() {} }\nMain() {}" "'Go' overrides -A.Go\\(\\) but is declared as Go\\(\\)")
reject(override_reference "A { Go(int& x) {} }\nB : A { Go(int x) {} }\nMain() {}" "'Go' overrides A.Go\\(int&\\) but is declared as Go\\(int\\)")
reject(override_every_parent "A { Go() {} }\nM { Go(int a) {} }\nB : A, M { Go() {} }\nMain() {}" "'Go' overrides M.Go\\(int\\) but is declared as Go\\(\\)")
reject(override_field_with_method "A { int X }\nB : A { X() {} }\nMain() {}" "'X' is already inherited from 'A' as a field")
reject(override_method_with_field "A { X() {} }\nB : A { int X }\nMain() {}" "'X' is already inherited from 'A' as a method; only a method can override it")
# A derived container passes for a parent reference parameter: the callee
# receives the parent's state and the object's dispatch table, so calls
# through the parameter reach the object's overrides.
run_program(interface_parameters [=[
Animal
{
    int Legs = 4
    string Speak() { return "..." }
    Describe() { Print(Speak() + " on " + "legs") }
}
Walker
{
    int Steps = 1
    Walk() { Print("walker walks") }
}
Cat : Animal, Walker
{
    string Name = "cat"
    string Speak() { return "meow " + Name }
    Walk() { Print("cat walks") }
}
Lion : Cat
{
    string Speak() { return "roar" }
}
Talk(Animal& a)
{
    Print(a.Speak())
    a.Describe()
    a.Legs = a.Legs + 1
    Again(a)
}
Again(Animal& a) { Print(a.Speak() + "!") }
Stroll(Walker& w) { w.Walk() }
Pet(Cat& c)
{
    Talk(c)
    Stroll(c)
}
Kennel
{
    Kennel(Animal& a) { Print(a.Speak()) }
}
Main()
{
    Animal a()
    Talk(a)
    Cat c()
    Talk(c)
    Print(c.Legs)
    Stroll(c)
    Lion l()
    Pet(l)
    Kennel k(l)
}
]=] "...\n... on legs\n...!\nmeow cat\nmeow cat on legs\nmeow cat!\n5\ncat walks\nroar\nroar on legs\nroar!\ncat walks\nroar\n")
set(sliced "P\n{\n    int X\n    P(int a) { X = a }\n}\nQ : P { int Y }\n")
run_program(interface_whole_object "${sliced}Reset(P& p) { p(1) }\nSet(P& p) { p = P(3) }\nMain() { P p(2)\nReset(p)\nPrint(p.X)\nSet(p)\nPrint(p.X) }" "1\n3\n")
trap_program(interface_reinitialize_part "${sliced}Reset(P& p) { p(1) }\nMain() { Q q(5)\nReset(q) }")
trap_program(interface_assign_part "${sliced}Set(P& p) { p = P(3) }\nMain() { Q q(5)\nSet(q) }")
reject(interface_ambiguous "B { int X }\nL : B { int Y }\nR : B { int Z }\nD : L, R { int W }\nUse(B& b) {}\nMain() { D d()\nUse(d) }" "'D' contains 'B' through more than one parent")
reject(interface_unrelated "B { int X }\nO { int X }\nUse(B& b) {}\nMain() { O o()\nUse(o) }" "type mismatch: expected B, got O")
reject(interface_by_value "B { int X }\nL : B { int Y }\nUse(B b) {}\nMain() { L l()\nUse(l) }" "type mismatch: expected B, got L")
# Getter blocks (@{ Name: expression }) and setter blocks (={ Name: statement },
# reading `value`) chained to a container declare properties. Untyped
# properties take their getter expression's type. Reads call the getter,
# assignments the setter, and compound assignments both on a receiver
# evaluated once; accessors override and dispatch like methods.
file(READ "${SOURCE}/Examples/Containers/04_Properties.dk" properties_example)
run_program(properties_example "${properties_example}" "20\n25\n")
run_program(properties [=[
Person
{
    -int Age = 20
    -string First = "Ada"
    -int Writes
    Birthday() { Age += 1 }
    Show() { Print(Label) }
}
@{
    Age: Age,
    Label: First + " " + "Lovelace"
    Twice: Age * 2
    long Big: Age
    -int Hidden: 7
}
={
    Age:
    {
        Writes += 1
        Age = value
    }
    Label: First = value
    Writes: Writes = value
}
@{
    Writes: Writes
}
Student : Person
{
    int School = 1
}
@{
    Label: "student"
}
Bump(Person& p) { p.Age += 10 }
Person Make() { return Person() }
Main()
{
    Person p()
    Print(p.Age)
    p.Age = 25
    Print(p.Age)
    p.Age += 5
    p.Age++
    Print(p.Age)
    Print(p.Writes)
    Print(p.Twice)
    Print(p.Big)
    p.Label = "Grace"
    Print(p.Label)
    p.Show()
    p.Birthday()
    Print(p.Age)
    Person* h()
    h.Age = 40
    Print(h.Age)
    Print(Make().Age)
    Student s()
    s.Show()
    Bump(s)
    Print(s.Age)
    Print(s.Writes)
}
]=] "20\n25\n31\n3\n62\n31\nGrace Lovelace\nGrace Lovelace\n32\n40\n20\nstudent\n30\n1\n")
set(property "P\n{\n    -int A = 1\n}\n@{\n    A: A\n    Two: A * 2\n}\n={\n    A: A = value\n}\n")
reject(property_read_only "${property}Main() { P p()\np.Two = 3 }" "property 'Two' of 'P' has no setter; it is read-only")
reject(property_write_only "P { int W }\n={ int Code: W = value }\nMain() { P p()\nPrint(p.Code) }" "property 'Code' of 'P' has no getter")
reject(property_untyped_setter "P { int W }\n={ Code: W = value }\nMain() {}" "setter of property 'Code' needs a type")
reject(property_types_disagree "P { int W }\n@{ W2: W }\n={ long W2: W = 1 }\nMain() {}" "the getter and setter of property 'W2' disagree on its type: int and long")
reject(property_public_field "P { int W }\n@{ W: W }\nMain() {}" "property 'W' has the name of a public field of 'P'")
reject(property_method "P\n{\n    int W\n    X() {}\n}\n@{ X: W }\nMain() {}" "property 'X' has the name of a method of 'P'")
reject(property_reference "${property}Use(int& x) {}\nMain() { P p()\nUse(p.A) }" "'A' is a property, not storage")
reject(property_protected "P { int W }\n@{ -Secret: W }\nMain() { P p()\nPrint(p.Secret) }" "'Secret' is a protected member of 'P'")
reject(property_temporary "${property}P Make() { return P() }\nMain() { Make().A = 3 }" "cannot set property 'A' of a temporary")
reject(property_constant "${property}Main() { #P c = P()\nPrint(c.A) }" "cannot read property 'A' of a constant")
reject(property_cycle "P { int W }\n@{\n    X: Y\n    Y: X\n}\nMain() {}" "cannot infer the type of property")
reject(property_override_type "${property}Q : P { int Z }\n@{ long Two: 1 }\nMain() {}" "property 'Two' overrides P.Two \\(getter of int\\) but is declared as Two \\(getter of long\\)")
reject(property_block_untyped "P { int W }\n@{ X: { return W } }\nMain() {}" "a getter with a block body needs a type")
reject(property_duplicate "P { int W }\n@{\n    X: W\n    X: W\n}\nMain() {}" "property 'X' already has a getter")
reject(property_field_override "${property}Q : P { int Two }\nMain() {}" "'Two' is already inherited from 'P' as a property")
# Queues (first in, first out), stacks (last in, first out), and pairs
# (keyed, in insertion order). `+=` adds, `--` removes the queue's front,
# the stack's top, or the pair's last added entry; replacing a key's value
# keeps its place. Collections are move-only and start empty.
run_program(collections [=[
Inbox
{
    string queue Mail
    int Count = 0
}
Fill(int queue& q, int from, int to)
{
    int i = from
    while i <= to
    {
        q += i
        i = i + 1
    }
}
int stack Build()
{
    int stack s
    s += 7
    s += 8
    return s
}
Main()
{
    int queue q
    Print(q.Len)
    q += 2
    q += 3
    q += 4
    Print(q.Front)
    q--
    Print(q.Front)
    Print(q.Len)
    Fill(q, 5, 12)
    q--
    q--
    Print(q.Front)
    Print(q.Len)
    int stack s
    s += 2
    s += 3
    s += 4
    Print(s.Top)
    s--
    Print(s.Top)
    int stack built = Build()
    Print(built.Top)
    built--
    Print(built.Top)
    string, string pair p
    p += "name" : "Bob"
    p += "city" : "York"
    p += "name" : "Tom"
    Print(p["name"])
    Print(p.LastKey)
    Print(p.Last)
    Print(p.Len)
    p--
    Print(p.LastKey)
    Print(p.Len)
    string pair codes
    codes += "a" : "first"
    Print(codes["a"])
    int, big pair totals
    totals += 1 : 10
    totals += 1 : 20
    Print(totals[1])
    Inbox box()
    box.Mail += "hello"
    box.Mail += "world"
    Print(box.Mail.Front)
    box.Mail--
    Print(box.Mail.Front)
    char queue letters
    int k = 0
    while k < 100
    {
        letters += 'x'
        letters--
        k = k + 1
    }
    Print(letters.Len)
}
]=] "0\n2\n3\n2\n5\n8\n4\n3\n8\n7\nTom\ncity\nYork\n2\nname\n1\nfirst\n20\nhello\nworld\n0\n")
trap_program(collection_empty_front "Main() { int queue q\nPrint(q.Front) }")
trap_program(collection_empty_drop "Main() { int stack s\ns-- }")
trap_program(collection_missing_key "Main() { string, int pair p\np += \"a\" : 1\nPrint(p[\"b\"]) }")
trap_program(collection_empty_last "Main() { int pair p\nPrint(p.Last) }")
reject(collection_copy "Main() { int queue a\nint queue b = a }" "cannot copy int queue 'a': collections are move-only")
reject(collection_by_value "Use(int queue q) {}\nMain() {}" "parameter 'q' must take int queue by reference")
reject(collection_static "S #{ int queue Q }\nMain() {}" "static fields of type int queue are not supported")
reject(collection_container_element "P { int X }\nMain() { P queue q }" "a collection cannot hold P")
reject(collection_float_key "Main() { double, int pair p }" "a pair cannot be keyed by double")
reject(collection_entry_in_queue "Main() { int queue q\nq += 1 : 2 }" "key : value entries belong in a pair, not in int queue")
reject(collection_pair_without_entry "Main() { string, int pair p\np += \"a\" }" "add to a pair as p \\+= key : value")
reject(collection_unsupported "Main() { int queue q\nq *= 2 }" "'\\*=' is not supported on int queue yet")
reject(collection_member "Main() { int queue q\nPrint(q.Top) }" "int queue has no member 'Top'; a queue has Front and Len")
reject(collection_read_only "Main() { int queue q\nq.Len = 3 }" "'Len' is a read-only member of int queue")
reject(collection_field_copy "B { int queue Q }\nMain() { B a()\nB b = a }" "cannot copy 'B': it holds a collection, so it is move-only")
reject(collection_index_queue "Main() { int queue q\nPrint(q[0]) }" "only arrays and pairs can be indexed; int queue cannot")
reject(collection_entry_elsewhere "Main() { int x = 1\nx += 1 : 2 }" "key : value entries can only be added to a pair")
# Fixed arrays (T [N]) store their elements inline and start at defaults;
# dynamic arrays (T []) grow on +=, moving elements bytewise, and clean up
# each element on --, on assignment, and when they leave scope. a[i] names an
# element and traps outside the array.
run_program(arrays [=[
Point
{
    int X
    int Y = 2
    Move(int d) { X += d }
}
Probe
{
    int Id
    string Tag = "probe"
    Probe(int id) { Id = id }
    ~Probe() { Print(-Id) }
}
Grid
{
    int [3] Cells
    string [] Names
}
int Sum(int [4]& values)
{
    int total = 0
    int i = 0
    while i < values.Len
    {
        total += values[i]
        i = i + 1
    }
    return total
}
int [] Range(int count)
{
    int [] result
    int i = 0
    while i < count
    {
        result += i * i
        i = i + 1
    }
    return result
}
Main()
{
    int [4] a
    Print(a[2])
    a[0] = 5
    a[1] = 6
    a[3] += 4
    a[3]++
    Print(Sum(a))
    Print(a.Len)
    int [4] copy = a
    copy[0] = 100
    Print(a[0])
    string [2] words
    Print(Length(words[1]))
    words[0] = "hello"
    Print(words[0])
    Point [2] points
    Print(points[1].Y)
    points[1].Move(3)
    Print(points[1].X)
    int [] squares = Range(6)
    Print(squares.Len)
    Print(squares[5])
    squares--
    Print(squares.Len)
    string [] names
    int k = 0
    while k < 10
    {
        names += "n"
        k = k + 1
    }
    names[9] = "last"
    Print(names[9])
    big [] bigs
    bigs += 12345678901234567890
    Print(bigs[0])
    Grid g()
    g.Cells[2] = 7
    g.Names += "grid"
    Print(g.Cells[2])
    Print(g.Names[0])
    Probe [] probes
    probes += Probe(1)
    probes += Probe(2)
    probes += Probe(3)
    probes += Probe(4)
    probes += Probe(5)
    Print(probes[4].Tag)
    probes--
    Print("end")
}
]=] "0\n16\n4\n5\n0\nhello\n2\n3\n6\n25\n5\nlast\n12345678901234567890\n7\ngrid\nprobe\n-5\nend\n-1\n-2\n-3\n-4\n")
trap_program(array_fixed_high "Main() { int [3] a\nPrint(a[3]) }")
trap_program(array_fixed_negative "Main() { int [3] a\nint i = -1\na[i] = 1 }")
trap_program(array_dynamic_high "Main() { int [] a\na += 1\nPrint(a[1]) }")
trap_program(array_dynamic_empty_remove "Main() { int [] a\na-- }")
reject(array_copy_dynamic "Main() { int [] a\nint [] b = a }" "cannot copy int \\[\\] 'a': dynamic arrays are move-only")
reject(array_copy_managed "Main() { string [2] a\nstring [2] b = a }" "cannot copy string \\[2\\] 'a': its elements cannot be copied implicitly")
reject(array_by_value "Use(int [] a) {}\nMain() {}" "parameter 'a' must take int \\[\\] by reference")
reject(array_fixed_append "Main() { int [3] a\na += 1 }" "int \\[3\\] has a fixed length")
reject(array_element_reference "Use(int& x) {}\nMain() { int [] a\na += 1\nUse(a[0]) }" "cannot pass an element of a dynamic array by reference")
reject(array_field_strings "G { string [2] Names }\nMain() {}" "field 'Names' cannot hold string \\[2\\] yet")
reject(array_runtime_elements "P\n{\n    int X\n    P() { X = 1 }\n}\nMain() { P [2] ps }" "'ps' cannot default its elements")
reject(array_pair_write "Main() { string, int pair p\np[\"a\"] = 1 }" "pair values are read-only so far")
reject(array_index_scalar "Main() { int x = 1\nPrint(x[0]) }" "only arrays and pairs can be indexed")
reject(array_float_index "Main() { int [3] a\nPrint(a[1.5]) }" "an array index must be an integer, not float")
reject(array_zero_length "Main() { int [0] a }" "a fixed array needs at least one element")
reject(array_of_collections "Main() { int queue [3] a }" "arrays of collections are not supported yet")
reject(array_multidimensional "Main() { int [3] [2] a }" "multidimensional arrays are a later item")
reject(array_constant "Main() { #int [3] a = Make()\na[0] = 1 }\nint [3] Make() { int [3] r\nreturn r }" "cannot modify constant 'a'")
reject(array_member "Main() { int [3] a\nPrint(a.Size) }" "int \\[3\\] has no member 'Size'; an array has Len")
file(WRITE "${WORK}/property_library.dk" "Person\n{\n    -int Years = 20\n    Show() { Print(Title) }\n}\n@{\n    Age: Years\n    Title: \"person\"\n}\n={\n    Age: Years = value\n}\n")
file(WRITE "${WORK}/property_client.dk" "Kid : Person { int Toys = 1 }\n@{ Title: \"kid\" }\nMain()\n{\n    Kid k()\n    k.Age += 2\n    Print(k.Age)\n    k.Show()\n}\n")
execute_process(COMMAND "${COMPILER}" --library "${WORK}/property_library.dk" -o "${WORK}/property_library.ll" RESULT_VARIABLE status ERROR_VARIABLE errors)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "property_library: compilation failed: ${errors}")
endif()
execute_process(COMMAND "${COMPILER}" --dependency "${WORK}/property_library.dk" "${WORK}/property_client.dk" -o "${WORK}/property_client.ll" RESULT_VARIABLE status ERROR_VARIABLE errors)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "property_client: compilation failed: ${errors}")
endif()
execute_process(COMMAND "${CLANG}" "${WORK}/property_library.ll" "${WORK}/property_client.ll" -o "${WORK}/property_client.exe" RESULT_VARIABLE status ERROR_VARIABLE errors)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "property_client: link failed: ${errors}")
endif()
execute_process(COMMAND "${WORK}/property_client.exe" RESULT_VARIABLE status OUTPUT_VARIABLE actual)
if(NOT status EQUAL 0 OR NOT actual STREQUAL "22\nkid\n")
  message(FATAL_ERROR "property_client: exit=${status}, actual=[${actual}]")
endif()
file(WRITE "${WORK}/interface_library.dk" "Animal\n{\n    string Speak() { return \"...\" }\n}\nTalk(Animal& a) { Print(a.Speak()) }\n")
file(WRITE "${WORK}/interface_client.dk" "Cat : Animal\n{\n    string Speak() { return \"meow\" }\n}\nMain()\n{\n    Animal a()\n    Talk(a)\n    Cat c()\n    Talk(c)\n}\n")
execute_process(COMMAND "${COMPILER}" --library "${WORK}/interface_library.dk" -o "${WORK}/interface_library.ll" RESULT_VARIABLE status ERROR_VARIABLE errors)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "interface_library: compilation failed: ${errors}")
endif()
execute_process(COMMAND "${COMPILER}" --dependency "${WORK}/interface_library.dk" "${WORK}/interface_client.dk" -o "${WORK}/interface_client.ll" RESULT_VARIABLE status ERROR_VARIABLE errors)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "interface_client: compilation failed: ${errors}")
endif()
execute_process(COMMAND "${CLANG}" "${WORK}/interface_library.ll" "${WORK}/interface_client.ll" -o "${WORK}/interface_client.exe" RESULT_VARIABLE status ERROR_VARIABLE errors)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "interface_client: link failed: ${errors}")
endif()
execute_process(COMMAND "${WORK}/interface_client.exe" RESULT_VARIABLE status OUTPUT_VARIABLE actual)
if(NOT status EQUAL 0 OR NOT actual STREQUAL "...\nmeow\n")
  message(FATAL_ERROR "interface_client: exit=${status}, actual=[${actual}]")
endif()
# An importing module derives from a library container and overrides a
# method the library's own code calls on this.
file(WRITE "${WORK}/override_library.dk" "Animal\n{\n    int Legs = 4\n    string Speak() { return \"...\" }\n    Describe()\n    {\n        Print(Speak())\n        Print(Legs)\n    }\n}\nShow()\n{\n    Animal a()\n    a.Describe()\n}\n")
file(WRITE "${WORK}/override_client.dk" "Cat : Animal\n{\n    string Speak() { return \"meow\" }\n}\nMain()\n{\n    Show()\n    Cat c()\n    c.Describe()\n}\n")
execute_process(COMMAND "${COMPILER}" --library "${WORK}/override_library.dk" -o "${WORK}/override_library.ll" RESULT_VARIABLE status ERROR_VARIABLE errors)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "override_library: compilation failed: ${errors}")
endif()
execute_process(COMMAND "${COMPILER}" --dependency "${WORK}/override_library.dk" "${WORK}/override_client.dk" -o "${WORK}/override_client.ll" RESULT_VARIABLE status ERROR_VARIABLE errors)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "override_client: compilation failed: ${errors}")
endif()
execute_process(COMMAND "${CLANG}" "${WORK}/override_library.ll" "${WORK}/override_client.ll" -o "${WORK}/override_client.exe" RESULT_VARIABLE status ERROR_VARIABLE errors)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "override_client: link failed: ${errors}")
endif()
execute_process(COMMAND "${WORK}/override_client.exe" RESULT_VARIABLE status OUTPUT_VARIABLE actual)
if(NOT status EQUAL 0 OR NOT actual STREQUAL "...\n4\nmeow\n4\n")
  message(FATAL_ERROR "override_client: exit=${status}, actual=[${actual}]")
endif()
reject(inheritance_parent_name_member "A { int x }\nB : A { int A }\nMain() {}" "has the name of parent container 'A'")
reject(inheritance_protected_outside "A { -int x }\nB : A { int y }\nMain() { B b()\nPrint(b.x) }" "'x' is a protected member of 'A'")
reject(inheritance_parent_view_outside "A { F() {} }\nB : A { int y }\nMain() { B b()\nb.A.F() }" "'A' is a protected member of 'B'")
reject(inheritance_view_initializer "A { int x }\nB : A { int y\nF() { B b = {A: {x: 1}} } }\nMain() {}" "initialize inherited fields by name")
file(READ "${SOURCE}/Examples/Bootstrap/08_LongIntegers.dk" long_integers)
run_program(long_integers "${long_integers}" "5000000001\n10000000002\n-9223372036854775808\n")
run_program(long_arithmetic [=[
Main() {
 long n = 5000000000
 Print(n + 2)
 Print(2 + n)
 Print(n - 2)
 Print(n * 2)
 Print(n / 2)
 Print(n % 3)
 Print(-n)
 n += 3
 n -= 1
 n *= 2
 n /= 2
 n %= 7
 n++
 n--
 Print(n)
 Print(n == 4)
 Print(5000000000 > 2)
 Print(2 < 5000000000)
 Print(5000000000 != 2)
 Print(true ? 5000000000 | 1)
}
]=] "5000000002\n5000000002\n4999999998\n10000000000\n2500000000\n2\n-5000000000\n4\n1\n1\n1\n1\n5000000000\n")
run_program(long_boundaries [=[
long Identity(long n) { return n }
long Maximum() { return 9223372036854775807 }
Main() {
 Print(2147483648)
 Print(-2147483648)
 Print(-2147483649)
 Print(Maximum())
 long minimum = -9223372036854775808
 Print(minimum)
 Print(Identity(-9223372036854775808))
 long wrapped = Maximum()
 wrapped++
 Print(wrapped)
 wrapped--
 Print(wrapped)
 long widened = -2147483648
 Print(widened)
}
]=] "2147483648\n-2147483648\n-2147483649\n9223372036854775807\n-9223372036854775808\n-9223372036854775808\n-9223372036854775808\n9223372036854775807\n-2147483648\n")
run_program(long_conversions [=[
Increment(int64& n) { n++ }
Increment32(int32& n) { n++ }
Main() {
 int small = 5
 Increment32(small)
 long large = small
 Increment(large)
 Print(large)
 double precise = large
 Print(precise)
 float approximate = large
 Print(approximate)
 Print(large + 0.5)
 int64 deferred
 deferred = 5000000000
 Print(deferred)
 long index = 0
 while index < 2 { Print(index)
 index++ }
}
]=] "7\n7\n7\n7.5\n5000000000\n0\n1\n")
reject(long_overflow "Main() { long n = 9223372036854775808 }" "integer literal out of range")
reject(long_negative_overflow "Main() { long n = -9223372036854775809 }" "integer literal out of range")
reject(long_narrowing "Main() { long n = 1\nint small = n }" "type mismatch")
reject(long_reference_width "F(long& n) {}\nMain() { int n = 1\nF(n) }" "type mismatch")
reject(long_reference_reverse "F(int& n) {}\nMain() { long n = 1\nF(n) }" "type mismatch")
reject(long_float_narrowing "Main() { long n = 1.5 }" "type mismatch")
reject(long_bool "Main() { long n = true }" "type mismatch")
reject(long_compound_narrowing "Main() { int n = 1\nn += 5000000000 }" "type mismatch")
reject(long_constant "Main() { #long n = 1\nn++ }" "cannot modify constant")
file(READ "${SOURCE}/Examples/Bootstrap/09_TypeCasts.dk" casts)
run_program(casts "${casts}" "9\n-10\n0\n1\n3.5\n")
run_program(cast_precedence [=[
Main() {
 double value = -9.99
 Print((int)value)
 Print((int)-0.1)
 Print((int)-0.0)
 Print((int)1.9 * 2)
 Print((int)(1.9 * 2))
 Print((double)(long)7 / 2)
 Print((int)(3 + 4))
 Print((float)1.25)
 Print(true ? (int)2.5 | (int)-2.5)
 Print(false ? (int)2.5 | (int)-2.5)
 Print(false && (bool)(int)(0.0 / 0.0))
}
]=] "-10\n-1\n0\n2\n3\n3.5\n7\n1.25\n2\n-3\n0\n")
run_program(cast_integer_bounds [=[
Main() {
 Print((int)4294967295)
 Print((int)-4294967297)
 Print((long)-2147483648)
 double low = -2147483647.75
 double high = 2147483647.75
 Print((int)low)
 Print((int)high)
 double longLow = -9223372036854775808.0
 Print((long)longLow)
 long wide = 4294967296
 int narrowed = (int32)wide
 Print(narrowed)
 Print((int64)narrowed)
}
]=] "-1\n-1\n-2147483648\n-2147483648\n2147483647\n-9223372036854775808\n0\n0\n")
run_program(cast_bool [=[
Main() {
 Print((bool)0)
 Print((bool)-1)
 Print((bool)0.0)
 Print((bool)-0.1)
 Print((bool)(0.0 / 0.0))
 Print((int)true)
 Print((long)false)
 Print((float)true)
 Print((double)false)
}
]=] "0\n1\n0\n1\n1\n1\n0\n1\n0\n")
reject(cast_void "Main() { Print((void)1) }" "casts currently require")
reject(cast_text "Main() { Print((int)\"12\") }" "casts currently require")
reject(cast_void_result "F() {}\nMain() { Print((int)F()) }" "casts currently require")
reject(cast_reference "F(long& n) {}\nMain() { n : 1\nF((long)n) }" "must be a mutable variable")
reject(cast_no_operand "Main() { Print((int)) }" "expected identifier")
foreach(type IN ITEMS int8 short int long)
  if(type STREQUAL "int8")
    set(minimum "-128")
  elseif(type STREQUAL "short")
    set(minimum "-32768")
  elseif(type STREQUAL "int")
    set(minimum "-2147483648")
  else()
    set(minimum "-9223372036854775808")
  endif()
  foreach(op IN ITEMS / %)
    if(op STREQUAL "/")
      set(operation divide)
    else()
      set(operation remainder)
    endif()
    trap_program(${type}_${operation}_zero
      "${type} F(${type} a, ${type} b) { return a ${op} b }\nMain() { Print(F(7, 0)) }")
    trap_program(${type}_${operation}_overflow
      "${type} F(${type} a, ${type} b) { return a ${op} b }\nMain() { Print(F(${minimum}, -1)) }")
    trap_program(${type}_${operation}_assign_zero
      "F(${type}& a, ${type} b) { a ${op}= b }\nMain() { ${type} n = 7\nF(n, 0) }")
    trap_program(${type}_${operation}_assign_overflow
      "F(${type}& a, ${type} b) { a ${op}= b }\nMain() { ${type} n = ${minimum}\nF(n, -1) }")
  endforeach()
  run_program(${type}_division_boundaries
    "Main() { ${type} n = ${minimum}\nPrint(n / 1)\nPrint(n % 1)\nPrint(n / n)\nPrint(n % n)\n${type} a = -7\nPrint(a / 3)\nPrint(a % 3)\nPrint(a / -3)\nPrint(a % -3)\na /= -1\nPrint(a)\na %= 3\nPrint(a) }"
    "${minimum}\n0\n1\n0\n-2\n-1\n2\n-1\n7\n1\n")
endforeach()
run_program(division_control_flow [=[
int Next(int& n) { n++
return n }
int Divide(int a, int b) { return a / b }
Main() {
 n : 7
 Print(Next(n) / Next(n))
 Print(n)
 Print(false && Divide(1, 0) == 0)
 Print(true || Divide(-2147483648, -1) == 0)
 Print(true ? 42 | Divide(1, 0))
 Print(false ? Divide(1, 0) | 12 / 3)
 Print((12 / 3 == 4) && (10 % 3 == 1))
 Print(true ? 12 / 3 | 10 % 3)
 for int i = 6; i / 2 > 0; i /= 2 { Print(i % 4) }
 long wide = -2147483648
 Print(wide / -1)
 Print(-2147483648 / (long)-1)
 Print(1.0 / 0.0 > 0.0)
}
]=] "0\n9\n0\n1\n42\n4\n1\n4\n2\n3\n2147483648\n2147483648\n1\n")
file(READ "${SOURCE}/Examples/Bootstrap/10_SmallIntegers.dk" small_integers)
run_program(small_integers "${small_integers}" "-128\n302\n-32768\n127\n-1\n-10\n")
run_program(small_integer_operations [=[
int8 Identity(int8 n) { return n }
Change(int8& a, int8& b) { a = 12
b += a }
Forward(int8& n) { Change(n, n) }
Main() {
 int8 a = -128
 int8 b = 127
 Print(-a)
 Print(b + (int8)1)
 Print(a - (int8)1)
 Print((int8)64 * (int8)4)
 Print(a < b)
 Print(a == (short)-128)
 a--
 Print(a)
 a++
 Print(a)
 a = 10
 a *= 12
 a += 10
 Print(a)
 a -= 1
 Print(a)
 Forward(a)
 Print(a)
 short s = -32768
 Print(-s)
 s--
 Print(s)
 s++
 Print(s)
 Print((short)16384 * (short)4)
 Print(b + s)
 Print(b + 1)
 Print(Identity(-128))
 int8 n
 if true { n = 3 } else { n = -4 }
 inferred : n
 inferred++
 Print(inferred)
 Print(true ? n | s)
 Print(false ? n | 128)
 int32 i = n
 int64 l = s
 float f = n
 double d = s
 Print(i)
 Print(l)
 Print(f)
 Print(d)
}
]=] "-128\n-128\n127\n0\n1\n1\n127\n-128\n-126\n-127\n24\n-32768\n32767\n-32768\n0\n-32641\n128\n-128\n4\n3\n128\n3\n-32768\n3\n-32768\n")
run_program(small_integer_casts [=[
Main() {
 Print((int8)128)
 Print((short)65535)
 Print((int8)(long)511)
 Print((int8)(short)-129)
 Print((short)(int8)-128)
 Print((bool)(int8)-128)
 Print((bool)(short)0)
 Print((int8)true)
 Print((short)false)
 Print((float)(int8)-128)
 Print((double)(short)-32768)
 double hi8 = 127.99
 double lo8 = -128.0
 double hi16 = 32767.99
 double lo16 = -32768.0
 Print((int8)hi8)
 Print((int8)lo8)
 Print((short)hi16)
 Print((short)lo16)
}
]=] "-128\n-1\n-1\n127\n-128\n1\n0\n1\n0\n-128\n-32768\n127\n-128\n32767\n-32768\n")
foreach(type IN ITEMS int8 short)
  if(type STREQUAL "int8")
    set(above 128)
    set(below -129)
    set(fraction_below -128.01)
  else()
    set(above 32768)
    set(below -32769)
    set(fraction_below -32768.01)
  endif()
  reject(${type}_literal_high "Main() { ${type} n = ${above} }" "literal out of range")
  reject(${type}_literal_low "Main() { ${type} n = ${below} }" "literal out of range")
  reject(${type}_argument_range "F(${type} n) {}\nMain() { F(${above}) }" "literal out of range")
  reject(${type}_return_range "${type} F() { return ${below} }\nMain() {}" "literal out of range")
  reject(${type}_narrow "Main() { int n = 1\n${type} s = n }" "type mismatch")
  reject(${type}_arithmetic_narrow "Main() { ${type} s = 1\ns = s + 1 }" "type mismatch")
  reject(${type}_reference_mismatch "F(${type}& n) {}\nMain() { int n = 1\nF(n) }" "type mismatch")
  reject(${type}_constant_reference "F(${type}& n) {}\nMain() { #${type} n = 1\nF(n) }" "cannot pass a constant")
  reject(${type}_uninitialized "Main() { ${type} n\nPrint(n) }" "read before assignment")
  trap_program(${type}_cast_high "Main() { Print((${type})${above}.0) }")
  trap_program(${type}_cast_low "Main() { double d = ${fraction_below}\nPrint((${type})d) }")
  trap_program(${type}_cast_nan "Main() { Print((${type})(0.0 / 0.0)) }")
  trap_program(${type}_cast_inf "Main() { Print((${type})(1.0 / 0.0)) }")
endforeach()
reject(short_to_int8 "Main() { short s = 1\nint8 n = s }" "type mismatch")
file(READ "${SOURCE}/Examples/Bootstrap/11_UnsignedIntegers.dk" unsigned_integers)
run_program(unsigned_integers "${unsigned_integers}"
  "0\n4294967295\n6148914691236517205\n1\n4294967294\n-1\n1\n")
foreach(width IN ITEMS 8 16 32 64)
  set(type "uint${width}")
  if(width EQUAL 8)
    set(maximum 255)
    set(above 256)
    set(half 128)
    set(quotient 127)
  elseif(width EQUAL 16)
    set(maximum 65535)
    set(above 65536)
    set(half 32768)
    set(quotient 32767)
  elseif(width EQUAL 32)
    set(maximum 4294967295)
    set(above 4294967296)
    set(half 2147483648)
    set(quotient 2147483647)
  else()
    set(maximum 18446744073709551615)
    set(above 18446744073709551616)
    set(half 9223372036854775808)
    set(quotient 9223372036854775807)
  endif()
  run_program(${type}_operations
    "${type} Maximum() { return ${maximum} }\nChange(${type}& n) { n++\nn -= 1 }\nMain() { #${type} top = Maximum()\n${type} n = top\nPrint(n)\nn++\nPrint(n)\nn--\nPrint(n)\nn *= 2\nPrint(n / (${type})2)\nPrint(n % (${type})2)\nn = top\nPrint(n / (${type})2)\nPrint(n % (${type})2)\nPrint(n > (${type})${half})\nPrint(n >= top)\nPrint(n < (${type})1)\nPrint(n <= top)\nPrint(n == top)\nPrint(n != (${type})0)\nChange(n)\nPrint(n)\nn = ${half}\nPrint(-n)\nPrint(n / top)\nPrint(n % top)\nn /= ${half}\nPrint(n)\nn %= 1\nPrint(n)\nPrint((bool)n)\nPrint((${type})true)\nPrint((${type})false)\nPrint((${type})-1)\nPrint((${type})-0.0) }"
    "${maximum}\n0\n${maximum}\n${quotient}\n0\n${quotient}\n1\n1\n1\n0\n1\n1\n1\n${maximum}\n${half}\n0\n${half}\n1\n0\n0\n1\n0\n${maximum}\n0\n")
  reject(${type}_literal_overflow "Main() { ${type} n = ${above} }" "integer literal out of range")
  reject(${type}_negative_literal "Main() { ${type} n = -1 }" "integer literal out of range")
  reject(${type}_signed_assignment "Main() { int n = 1\n${type} u = n }" "type mismatch")
  reject(${type}_signed_reference "F(${type}& n) {}\nMain() { int${width} n = 1\nF(n) }" "type mismatch")
  reject(${type}_constant_reference "F(${type}& n) {}\nMain() { #${type} n = 1\nF(n) }" "cannot pass a constant")
  reject(${type}_uninitialized "Main() { ${type} n\nPrint(n) }" "read before assignment")
  reject(${type}_negative_return "${type} F() { return -1 }\nMain() {}" "integer literal out of range")
  reject(${type}_overflow_argument "F(${type} n) {}\nMain() { F(${above}) }" "integer literal out of range")
  foreach(op IN ITEMS / %)
    if(op STREQUAL "/")
      set(operation divide)
    else()
      set(operation remainder)
    endif()
    trap_program(${type}_${operation}_zero
      "${type} F(${type} a, ${type} b) { return a ${op} b }\nMain() { Print(F(7, 0)) }")
    trap_program(${type}_${operation}_assign_zero
      "F(${type}& a, ${type} b) { a ${op}= b }\nMain() { ${type} n = 7\nF(n, 0) }")
  endforeach()
  trap_program(${type}_cast_upper "Main() { double d = ${above}.0\nPrint((${type})d) }")
  trap_program(${type}_cast_negative "Main() { Print((${type})-0.01) }")
  trap_program(${type}_cast_nan "Main() { Print((${type})(0.0 / 0.0)) }")
  trap_program(${type}_cast_infinity "Main() { Print((${type})(1.0 / 0.0)) }")
endforeach()
run_program(unsigned_conversions [=[
Alias(byte& b, uint& u) { b++
u++ }
Forward(uint8& b, uint32& u) { Alias(b, u) }
uint64 Recurse(uint64 n) { if n == (uint64)0 { return 1 }
return Recurse(n - (uint64)1) * (uint64)2 }
Main() {
 byte b = 255
 uint u = 4294967295
 Forward(b, u)
 Print(b)
 Print(u)
 b = 255
 uint16 s = b
 uint32 i = s
 uint64 l = i
 Print(s)
 Print(i)
 Print(l)
 short ss = b
 int si = s
 long sl = i
 Print(ss)
 Print(si)
 Print(sl)
 Print((long)(uint32)4294967295)
 Print((uint64)(int8)-1)
 Print((int8)(uint8)255)
 Print((short)(uint16)65535)
 Print((int)(uint32)4294967295)
 Print((long)(uint64)18446744073709551615)
 Print((uint8)(uint64)18446744073709551615)
 Print((uint16)(long)-1)
 Print((int8)(uint16)256)
 Print(18446744073709551615)
 Print(-0009223372036854775808)
 Print(-0002147483648)
 Print(Recurse(4))
 Print((float)(uint32)4294967295 > 0.0)
 Print((double)(uint64)18446744073709551615 > 0.0)
 Print((uint8)255.99)
 Print((uint16)65535.99)
 double limit32 = 4294967295.99
 double limit64 = 18446744073709549568.0
 Print((uint32)limit32)
 Print((uint64)limit64)
 double power = 9223372036854775808.0
 Print((uint64)power)
}
]=] "0\n0\n255\n255\n255\n255\n255\n255\n4294967295\n18446744073709551615\n-1\n-1\n-1\n-1\n255\n65535\n0\n18446744073709551615\n-9223372036854775808\n-2147483648\n16\n1\n1\n255\n65535\n4294967295\n18446744073709549568\n9223372036854775808\n")
run_program(mixed_integers [=[
Main() {
 uint8 a = 255
 int8 b = -128
 Print(a + b)
 Print(b + a)
 Print(a > b)
 Print(b < a)
 Print(true ? a | b)
 Print(false ? a | b)
 uint16 c = 65535
 short d = -32768
 Print(c + d)
 Print(d + c)
 uint32 e = 4294967295
 int f = -2147483648
 Print(e + f)
 Print(f + e)
 Print(e > f)
 Print(f > e)
 Print(e == f)
 Print(e != f)
 Print(e + 1)
 Print(1 + e)
 Print(e / -1)
 Print(e % -2)
 uint64 huge = 18446744073709551615
 Print(false && huge / (uint64)0 == (uint64)0)
 Print(true ? huge / (uint64)3 | huge / (uint64)0)
 Print((huge / (uint64)3 > (uint64)0) && (huge % (uint64)2 == (uint64)1))
}
]=] "127\n127\n1\n1\n255\n-128\n32767\n32767\n2147483647\n2147483647\n1\n0\n0\n1\n4294967296\n4294967296\n-4294967295\n1\n0\n6148914691236517205\n1\n")
reject(uint64_mixed "Main() { uint64 n = 1\nPrint(n + 1) }" "explicit cast")
reject(uint64_mixed_reverse "Main() { uint64 n = 1\nPrint(-1 < n) }" "explicit cast")
reject(uint64_conditional "Main() { uint64 n = 1\nPrint(true ? n | -1) }" "explicit cast")
reject(uint64_to_long "Main() { uint64 n = 1\nlong s = n }" "type mismatch")
reject(uint32_to_int "Main() { uint32 n = 1\nint s = n }" "type mismatch")
reject(unsigned_narrow "Main() { uint64 n = 1\nuint32 s = n }" "type mismatch")
reject(uint64_literal_overflow_inferred "Main() { n : 18446744073709551616 }" "integer literal out of range")
reject(negative_literal_overflow_inferred "Main() { Print(-9223372036854775809) }" "integer literal out of range")
file(READ "${SOURCE}/Examples/Bootstrap/12_IntegerLiterals.dk" integer_literals)
run_program(integer_literals "${integer_literals}"
  "165\n16746496\n-128\n18446744073709551615\n57005\n10\n255\n")
run_program(radix_boundaries [=[
Small(int8& n) { n++ }
Large(uint64& n) { n-- }
Main() {
 int8 a = -0b10000000
 short b = -0x8000
 int c = -0x80000000
 long d = -0x8000000000000000
 Print(a)
 Print(b)
 Print(c)
 Print(d)
 uint8 e = 0xFF
 uint16 f = 0xFFFF
 uint32 g = 0xFFFFFFFF
 uint64 h = 0xFFFFFFFFFFFFFFFF
 Print(e)
 Print(f)
 Print(g)
 Print(h)
 Print(0x7FFFFFFF)
 Print(0x80000000)
 Print(0x7FFFFFFFFFFFFFFF)
 Print(0x8000000000000000)
 Print(0b1111111111111111111111111111111111111111111111111111111111111111)
 Print(-0b1000000000000000000000000000000000000000000000000000000000000000)
 Print(0x0000000000000000000000000000000000000000FF)
 Print(-0X00008000000000000000)
 int8 small = -0x80
 Small(small)
 Print(small)
 huge : 0xFFFFFFFFFFFFFFFF
 Large(huge)
 Print(huge)
 #byte constant = 0b11111111
 Print(constant)
 e = true ? 0xFE | 0b1
 Print(e)
 e += 0x1
 Print(e)
 double number = 0xFF
 Print(number)
 Print((int8)0xFF)
 Print((bool)0b0)
}
]=] "-128\n-32768\n-2147483648\n-9223372036854775808\n255\n65535\n4294967295\n18446744073709551615\n2147483647\n2147483648\n9223372036854775807\n9223372036854775808\n18446744073709551615\n-9223372036854775808\n255\n-9223372036854775808\n-127\n18446744073709551614\n255\n254\n255\n255\n-1\n0\n")
run_program(radix_lexing [=[
Main() {
 Print(0x1e+2)
 Print(0x1E-2)
 Print(0xBEEF)
 Print(0xdead)
 Print(0b101+0B11)
 Print(0x10*0b10)
 Print(012)
 Print(1e2)
 Print(1.25e-2)
 Print(0xA/* comment */+0xB)
 Print(true ? 0xE | 0xF)
}
]=] "32\n28\n48879\n57005\n8\n32\n12\n100\n0.0125\n21\n14\n")
foreach(literal IN ITEMS 0x 0X 0b 0B)
  reject(radix_empty_${literal} "Main() { Print(${literal}) }" "requires digits")
endforeach()
foreach(literal IN ITEMS 0xG 0x1G 0xFFu 0b2 0b102 0b1e3 0b10f 0x_1 0b1_0 0x1_0)
  reject(radix_invalid_${literal} "Main() { Print(${literal}) }" "invalid integer literal")
endforeach()
reject(radix_hex_overflow "Main() { Print(0x10000000000000000) }" "integer literal out of range")
reject(radix_binary_overflow "Main() { Print(0b10000000000000000000000000000000000000000000000000000000000000000) }" "integer literal out of range")
reject(radix_negative_overflow "Main() { Print(-0x8000000000000001) }" "integer literal out of range")
reject(radix_unsigned_negative "Main() { uint n = -0x1 }" "integer literal out of range")
reject(radix_byte_overflow "Main() { byte n = 0x100 }" "integer literal out of range")
reject(radix_signed_byte_overflow "Main() { int8 n = 0x80 }" "integer literal out of range")
reject(radix_signed_byte_underflow "Main() { int8 n = -0x81 }" "integer literal out of range")
reject(radix_signed_long_overflow "Main() { long n = 0x8000000000000000 }" "integer literal out of range")
reject(radix_int_overflow "Main() { int n = 0x80000000 }" "type mismatch")
file(READ "${SOURCE}/Examples/Bootstrap/13_Bitwise.dk" bitwise)
run_program(bitwise "${bitwise}" "5\n250\n2\n-4\n1\n3\n6\n")
run_program(bitwise_widths [=[
Main() {
 Print(~(int8)0)
 Print(~(short)0)
 Print(~0)
 Print(~(long)0)
 Print(~(byte)0)
 Print(~(uint16)0)
 Print(~(uint)0)
 Print(~(uint64)0)
 Print((int8)64 << 1)
 Print((byte)128 >> 7)
 Print((short)-32768 >> 15)
 Print((uint16)32768 >> 15)
 Print((long)1 << 63)
 Print((uint64)1 << 63)
 Print((uint64)0x8000000000000000 >> 63)
 Print((byte)1 << (uint64)7)
 Print(1 << (byte)3)
 Print((int8)-1 & (byte)255)
 Print((uint64)1 | (uint64)2)
 byte b = 255
 b &= 3
 b |= 4
 b ^= 2
 Print(b)
 long s = -8
 s >>= 1
 s <<= 2
 Print(s)
 b <<= (uint64)7
 Print(b)
 b >>= (long)7
 Print(b)
 int n = 0
 n++
 n--
 Print(n)
}
]=] "-1\n-1\n-1\n-1\n255\n65535\n4294967295\n18446744073709551615\n-128\n1\n-1\n1\n-9223372036854775808\n9223372036854775808\n1\n128\n8\n255\n3\n5\n-16\n128\n1\n0\n")
run_program(bitwise_precedence [=[
int Mark(int& n, int value) { n = n * 10 + value
return value }
Main() {
 Print(1 | 2 ^ 3 & 6)
 Print(1 << 2 + 1)
 Print(16 >> 1 >> 1)
 Print(~1 + 3)
 Print((byte)~(byte)0)
 Print(true ? false ? (1 | 2) | 8 | 16)
 Print(true ? (false ? 1 | 2 | 4) | 8)
 Print(false ? 1 | true ? 2 | 4)
 Print((true ? 1 | 2) | 4)
 int n = 0
 Print(Mark(n, 1) | Mark(n, 2))
 Print(n)
 Print(true ? 7 | (1 << -1))
 Print(false && ((1 << 32) == 0))
 int count = 0
 byte flags = 3
 flags <<= Mark(count, 1)
 Print(flags)
 Print(count)
 Print(true ? Mark(count, 2) | Mark(count, 3))
 Print(count)
 Print(true ? Mark(count, 1 | 2) | 0)
 Print(count)
}
]=] "1\n8\n4\n1\n255\n8\n6\n2\n5\n3\n12\n7\n0\n6\n1\n2\n12\n3\n123\n")
reject(bitwise_equality_precedence "Main() { Print(1 & 3 == 3) }" "require integer operands")
reject(bitwise_relational_precedence "Main() { Print(2 | 1 < 2) }" "require integer operands")
foreach(op IN ITEMS "&" "|" "^" "<<" ">>")
  string(MAKE_C_IDENTIFIER "${op}" operator_id)
  reject(bitwise_float_${operator_id} "Main() { Print(1.0 ${op} 1) }" "require integer operands")
  reject(bitwise_bool_${operator_id} "Main() { Print(1 ${op} true) }" "require integer operands")
endforeach()
reject(bitwise_not_bool "Main() { Print(~true) }" "require integer operands")
reject(bitwise_not_text "Main() { Print(~\"text\") }" "require integer operands")
reject(bitwise_mixed_range "Main() { Print((uint64)1 | 1) }" "no common integer type")
reject(bitwise_constant "Main() { #int n = 1\nn |= 2 }" "cannot modify constant")
reject(bitwise_uninitialized "Main() { int n\nn <<= 1 }" "read before assignment")
reject(bitwise_float_assign "Main() { float n = 1.0\nn &= 1 }" "require integer operands")
reject(bitwise_count_float "Main() { int n = 1\nn <<= 1.0 }" "require integer operands")
reject(bitwise_narrow_assign "Main() { byte n = 1\nn |= 256 }" "out of range")
foreach(op IN ITEMS "<<" ">>")
  string(MAKE_C_IDENTIFIER "${op}" operator_id)
  trap_program(shift_negative_${operator_id} "Main() { Print(1 ${op} -1) }")
  trap_program(shift_width_${operator_id} "Main() { Print(1 ${op} 32) }")
  trap_program(shift_small_${operator_id} "Main() { Print((byte)1 ${op} 8) }")
  trap_program(shift_large_${operator_id} "Main() { Print((byte)1 ${op} (uint64)256) }")
  trap_program(shift_long_${operator_id} "Main() { Print((long)1 ${op} 64) }")
  trap_program(shift_assign_${operator_id} "Main() { byte n = 1\nn ${op}= -1\nPrint(n) }")
endforeach()
file(READ "${SOURCE}/Examples/Bootstrap/19_Enums.dk" enums)
run_program(enum_example "${enums}"
  "Sunday\n1\n10\nMonday\n1\n3\n1\nTom\nBob and Thomas\nDaniel\n")
# Runtime member indices decay through the value tables.
run_program(enum_runtime "Color : string #@{ Red, Green: \"verde\" }
Tag : name #@{ A: \"alpha\" }
Level : i3 #@{ Low: -5, Mid, High: 8388607 }
Holder { Level level }
Level Pick(int n) { return n == 0 ? Level.Low | Level.High }
Main() {
  Color c = Color.Green
  Print(c)
  string s = c
  Print(s)
  Print(c + \"!\")
  name n = Tag.A
  Print(n)
  Holder h
  h.level = Pick(1)
  Print(h.level)
  Print(h.level == 8388607)
  Print(Level.Mid)
  Print(Level.Mid + 0)
  long total = Pick(0)
  Print(total)
  int i = 0
  while i < 2 {
    Print(Pick(i))
    i++
  }
  Print((int)Pick(1) / 2)
}" "Green\nverde\nverde!\nalpha\nHigh\n1\nMid\n-4\n-5\nLow\nHigh\n4194303\n")
# Fragments merge in input order before implicit values are assigned, so the
# untyped extension may come first.
file(WRITE "${WORK}/enum_typed.dk" "Shade : int #@{ Dark }\n")
file(WRITE "${WORK}/enum_extension.dk" "Shade #@{ Light }\nMain() {\n  Print(Shade.Dark + 0)\n  Print(Shade.Light)\n}\n")
run_files(enum_fragments "0\nLight\n" "${WORK}/enum_typed.dk" "${WORK}/enum_extension.dk")
run_files(enum_fragments_reversed "1\nLight\n" "${WORK}/enum_extension.dk" "${WORK}/enum_typed.dk")
# An imported enum is usable, including as a parameter, but not extensible.
file(WRITE "${WORK}/enum_library.dk" "Shade : int #@{ Dark, Light }\nint Code(Shade s) { return s * 10 }\n")
file(WRITE "${WORK}/enum_client.dk" "Main() {\n  Print(Shade.Light)\n  Print(Code(Shade.Light))\n}\n")
execute_process(COMMAND "${COMPILER}" --library "${WORK}/enum_library.dk" -o "${WORK}/enum_library.ll" RESULT_VARIABLE status ERROR_VARIABLE errors)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "enum_library: compilation failed: ${errors}")
endif()
execute_process(COMMAND "${COMPILER}" --dependency "${WORK}/enum_library.dk" "${WORK}/enum_client.dk" -o "${WORK}/enum_client.ll" RESULT_VARIABLE status ERROR_VARIABLE errors)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "enum_client: compilation failed: ${errors}")
endif()
execute_process(COMMAND "${CLANG}" "${WORK}/enum_library.ll" "${WORK}/enum_client.ll" -o "${WORK}/enum_client.exe" RESULT_VARIABLE status ERROR_VARIABLE errors)
execute_process(COMMAND "${WORK}/enum_client.exe" RESULT_VARIABLE status OUTPUT_VARIABLE actual)
string(REPLACE "\r\n" "\n" actual "${actual}")
if(NOT status EQUAL 0 OR NOT actual STREQUAL "Light\n10\n")
  message(FATAL_ERROR "enum_client: exit=${status}, actual=[${actual}]")
endif()
file(WRITE "${WORK}/enum_import_extension.dk" "Shade #@{ Dim }\nMain() {}\n")
execute_process(COMMAND "${COMPILER}" --dependency "${WORK}/enum_library.dk" "${WORK}/enum_import_extension.dk" -o "${WORK}/enum_import_extension.ll" RESULT_VARIABLE status ERROR_VARIABLE errors)
if(status EQUAL 0 OR NOT errors MATCHES "cannot extend imported enum")
  message(FATAL_ERROR "enum_import_extension: expected rejection, got ${status}: ${errors}")
endif()
reject(enum_untyped "E #@{ A }\nMain() {}" "requires an underlying type")
reject(enum_type_conflict "E : int #@{ A }\nE : byte #@{ B }\nMain() {}" "declared as int")
reject(enum_duplicate "E : int #@{ A }\nE #@{ A }\nMain() {}" "duplicate member 'A'")
reject(enum_unknown_member "E : int #@{ A }\nMain() { Print(E.B) }" "unknown member 'B'")
reject(enum_implicit_range "E : byte #@{ A: 255, B }\nMain() {}" "implicit value 256")
reject(enum_explicit_range "E : int8 #@{ A: -129 }\nMain() {}" "out of range")
reject(enum_assign_member "E : int #@{ A }\nMain() { E.A = 1 }" "is a constant")
reject(enum_from_integer "E : int #@{ A }\nMain() { E e = 0 }" "type mismatch")
reject(enum_compound "E : int #@{ A }\nMain() { E e = E.A\ne += 1 }" "type mismatch")
reject(enum_text_value "E : string #@{ A: 1 }\nMain() {}" "string literal")
reject(enum_integer_value "E : int #@{ A: \"x\" }\nMain() {}" "integer literal")
reject(enum_float_value "E : float #@{ A: 0x10 }\nMain() {}" "decimal number literal")
reject(enum_bool "E : bool #@{ A }\nMain() {}" "underlying type must be")
reject(enum_nested "S #{ E : int #@{ A } }\nMain() {}" "file scope")
reject(enum_container_clash "E { int x }\nE : int #@{ A }\nMain() {}" "duplicate type name")
reject(enum_separator "E : int #@{ A B }\nMain() {}" "between enum members")
reject(enum_native "E : int #@{ A }\nextern \"C\" F(E e)\nMain() {}" "native parameter")
file(READ "${SOURCE}/Examples/Bootstrap/20_TypeLimits.dk" limits)
run_program(type_limits_example "${limits}"
  "2147483647\n-2147483648\n255\n8388607\n340282366920938463463374607431768211455\n2147483647\n1\n")
run_program(type_limits "Main() {
  Print(int8.Min)
  Print(short.Max)
  Print(uint.Min)
  Print(uint64.Max)
  Print(long.Min)
  Print(i9.Min)
  Print(u9.Max)
  Print(u64.Max)
  Print((int.Max))
  x : uint16.Max
  Print(x)
  Print(double.Min == -double.Max)
}" "-128\n32767\n0\n18446744073709551615\n-9223372036854775808\n-2361183241434822606848\n4722366482869645213695\n13407807929942597099574024998205846127479365820592393377723561443721764030073546976801874298166903427690031858186486050853753882811946569946433649006084095\n2147483647\n65535\n1\n")
reject(type_limit_member "Main() { Print(int.Size) }" "has no member 'Size'")
reject(type_limit_bool "Main() { Print(bool.Max) }" "has no Min or Max")
reject(type_limit_big "Main() { Print(big.Max) }" "unbounded")
reject(type_limit_narrow "Main() { byte b = int.Max }" "type mismatch")
reject(type_limit_assign "Main() { int.Max = 1 }" "mutable variable")
message(STATUS "Duck integration tests passed")

trap_program(expired_heap_alias "Main() { int* value = 5\nalias : value\nvalue = null\nPrint(*alias) }")
