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
 if true { int n = 3; Print(n) }
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
Main() { Print(Choose(false)); Print(Choose(true)) }
]=] "20\n10\n")

file(READ "${SOURCE}/Examples/Tutorial/06_ConditionsAndLoops.dk" conditions)
run_program(conditions "${conditions}" "small\n0\n1\n2\n")
file(READ "${SOURCE}/Examples/Tutorial/03_FunctionsAndReferences.dk" references)
run_program(references "${references}" "10\n6\n")
file(READ "${SOURCE}/Examples/Bootstrap/03_ReferencesAndConstants.dk" constants)
run_program(constants "${constants}" "12\n1\n10\n")
run_program(reference_aliases [=[
Change(int& first, int& second) { first = 10; second += first }
Forward(int& value) { Change(value, value) }
Copy(int value) { value = 99; Print(value) }
Count(int& value, int remaining) {
 if remaining > 0 { value++; Count(value, remaining - 1) }
}
bool Flip(bool& flag) { flag = !flag; return flag }
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
int Increment(int& value) { value++; return value }
Show(int first, int second) { Print(first); Print(second) }
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
 if enabled { int limit = 2; limit++; Print(limit) }
 Print(limit)
 Print(Value(limit))
}
]=] "3\n5\n6\n")
file(READ "${SOURCE}/Examples/Bootstrap/02_ControlFlow.dk" control)
run_program(control "${control}" "9\nselected\n1\n")
run_program(short_circuit [=[
bool Mark(int n) { Print(n); return true }
int Value(int n) { Print(n); return n }
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
reject(duplicate "Main() { n : 1; n : 2 }" "duplicate variable")
reject(parameter "F(int n, int n) {}\nMain() {}" "duplicate variable")
reject(arity "F(int n) {}\nMain() { F() }" "wrong argument count")
reject(condition "Main() { while 1 {} }" "type mismatch")
reject(scope "Main() { if true { n : 1 }; Print(n) }" "unknown variable")
reject(unreachable "Main() { return; Print(1) }" "unreachable")
reject(overflow "Main() { int n = 2147483648 }" "type mismatch")
reject(literal "Main() { Print(123abc) }" "invalid integer literal")
reject(string "Main() { Print(\"oops) }" "unterminated string")
reject(comment "Main() { /* oops" "unterminated comment")
reject(brace "Main() {" "expected '}'")
reject(separator "Main() { Print(1) Print(2) }" "expected newline")
reject(entry "main() {}" "program requires Main")
reject(entry_signature "int Main() { return 0 }" "bootstrap entry point")
reject(unsupported "Main() { big n = 1 }" "syntax may not be supported")
reject(void_value "F() {}\nMain() { n : F() }" "locals currently support")
reject(break_outside "Main() { break }" "requires an enclosing loop")
reject(continue_outside "Main() { continue }" "requires an enclosing loop")
reject(loop_scope "Main() { for int i = 0; i < 1; i++ {} ; Print(i) }" "unknown variable")
reject(loop_step_scope "Main() { for int i = 0; i < 1; n++ { n : 0 } }" "unknown variable")
reject(loop_step_declaration "Main() { for ;; int n = 0 {} }" "step cannot declare")
reject(loop_condition "Main() { for ; 1; {} }" "type mismatch")
reject(loop_separator "Main() { for int i = 0 i < 1; i++ {} }" "expected ';'")
reject(after_break "Main() { while true { break; Print(1) } }" "unreachable")
reject(compound_bool "Main() { b : true; b += 1 }" "type mismatch")
reject(compound_operand "Main() { n : 1; n += false }" "type mismatch")
reject(logical_operand "Main() { Print(true && 1) }" "type mismatch")
reject(logical_unselected "Main() { Print(false && Missing()) }" "unknown function")
reject(conditional_type "Main() { Print(true ? 1 | false) }" "type mismatch")
reject(conditional_void "Main() { true ? Print(1) | Print(2) }" "must produce values")
reject(conditional_condition "Main() { Print(1 ? 2 | 3) }" "type mismatch")
reject(conditional_separator "Main() { Print(true ? 2 : 3) }" "expected '\\|'")
reject(keyword_name "Main() { int break = 1 }" "reserved keyword")
reject(reference_literal "F(int& n) {}\nMain() { F(1) }" "must be a mutable variable")
reject(reference_expression "F(int& n) {}\nMain() { n : 1; F(n + 1) }" "must be a mutable variable")
reject(reference_call "int Get() { return 1 }\nF(int& n) {}\nMain() { F(Get()) }" "must be a mutable variable")
reject(reference_type "F(int& n) {}\nMain() { b : true; F(b) }" "type mismatch")
reject(reference_unknown "F(int& n) {}\nMain() { F(missing) }" "unknown variable")
reject(reference_constant "F(int& n) {}\nMain() { #int n = 1; F(n) }" "cannot pass a constant")
reject(reference_duplicate "F(int& n, int& n) {}\nMain() {}" "duplicate variable")
reject(reference_local_duplicate "F(int& n) { int n = 1 }\nMain() {}" "duplicate variable")
reject(reference_return "int& F(int& n) { return n }\nMain() {}" "expected identifier")
reject(constant_assignment "Main() { #int n = 1; n = 2 }" "cannot modify constant")
reject(constant_increment "Main() { #int n = 1; n++ }" "cannot modify constant")
reject(constant_compound "Main() { #int n = 1; n += 2 }" "cannot modify constant")
reject(constant_inference "Main() { #n : 1 }" "requires an explicit type")
reject(constant_initializer "Main() { #int n }" "expected '='")
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
reject(narrow_float "Main() { double d = 1.5; float f = d }" "type mismatch")
reject(narrow_return "int F() { return 1.5 }\nMain() {}" "type mismatch")
reject(narrow_compound "Main() { n : 1; n += 1.5 }" "type mismatch")
reject(float_bool "Main() { float f = true }" "type mismatch")
reject(float_condition "Main() { if 1.5 {} }" "type mismatch")
reject(float_remainder "Main() { Print(1.5 % 1.0) }" "type mismatch")
reject(float_remainder_assign "Main() { f : 1.5; f %= 1 }" "type mismatch")
reject(float_reference "F(double& n) {}\nMain() { f : 1.0; F(f) }" "type mismatch")
reject(float_constant "Main() { #double d = 1; d++ }" "cannot modify constant")
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
 if true { int n; n = 9; Print(n) }
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
  if i == 0 { step = 1; continue } else { step = 2 }
 }
 int count = 0
 while count < 3 {
  int local
  if count == 0 { count++; continue } else { local = count }
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
reject(uninitialized_read "Main() { int n; Print(n) }" "read before assignment")
reject(uninitialized_self "Main() { int n; n = n + 1 }" "read before assignment")
reject(uninitialized_increment "Main() { int n; n++ }" "read before assignment")
reject(uninitialized_compound "Main() { int n; n += 1 }" "read before assignment")
reject(uninitialized_reference "Set(int& n) { n = 1 }\nMain() { int n; Set(n) }" "read before assignment")
reject(uninitialized_branch "Main() { int n; if true { n = 1 }; Print(n) }" "read before assignment")
reject(uninitialized_else "Main() { int n; if true { n = 1 } else { Print(n) } }" "read before assignment")
reject(uninitialized_shadow "Main() { int n; if true { int n = 1 } else { int n = 2 }; Print(n) }" "read before assignment")
reject(uninitialized_loop "Main() { int n; while false { n = 1 }; Print(n) }" "read before assignment")
reject(uninitialized_break "Main() { int n; while true { n = 1; break }; Print(n) }" "read before assignment")
reject(uninitialized_step "Main() { int step; for int i = 0; i < 3; i += step { if i == 0 { continue }; step = 1 } }" "read before assignment")
reject(uninitialized_after_step "Main() { int n; for int i = 0; i < 0; n = 1 { break }; Print(n) }" "read before assignment")
reject(uninitialized_next_iteration "Main() { int n; for int i = 0; i < 2; i++ { Print(n); n = 1 } }" "read before assignment")
reject(uninitialized_short_circuit "Main() { bool b; Print(false && b) }" "read before assignment")
reject(uninitialized_ternary "Main() { int n; Print(true ? 1 | n) }" "read before assignment")
reject(uninitialized_bool "Main() { bool b; if b {} }" "read before assignment")
reject(uninitialized_duplicate "Main() { int n; int n }" "duplicate variable")
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
 if false Print(100); else Print(4)
 int count = 0
 for int i = 0; i < 3; i++
  count += i
 Print(count)
 do count++; while count < 5
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
  if stop { n = 7; break }
  n = 8
 } while false
 return n
}
int Early(bool first) {
 do { if first return 9; else return 10 } while false
}
Main() {
 int n
 do n = 1; while false
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
bool Check(int& calls) { calls++; return calls < 3 }
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
 { int n = 20; Print(n) }
 Print(n)
}
]=] "3\n3\n3\n12\n20\n12\n")
reject(do_missing_while "Main() { do {} }" "expected 'while'")
reject(do_condition "Main() { do {} while 1 }" "type mismatch")
reject(do_condition_unassigned "Main() { bool again; do { continue } while again }" "read before assignment")
reject(do_break_unassigned "Main() { int n; do { if true break; n = 1 } while false; Print(n) }" "read before assignment")
reject(do_body_scope "Main() { do { int n = 1 } while n > 0 }" "unknown variable")
reject(do_unreachable "Main() { do { return } while false; Print(1) }" "unreachable statement")
reject(single_body_scope "Main() { if true int n = 1; Print(n) }" "unknown variable")
reject(block_scope "Main() { { int n = 1 }; Print(n) }" "unknown variable")
reject(block_unreachable "Main() { { return }; Print(1) }" "unreachable statement")
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
 bool Even(int n) { if n == 0 return true; return Odd(n - 1) }
 Advance(int& n) { n++ }
}
Numbers #{
 bool Odd(int n) { if n == 0 return false; return Even(n - 1) }
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
reject(container_instance "Math { int n = 1 }\nMain() {}" "instance containers are not implemented")
reject(container_field "Math #{ int n = 1 }\nMain() {}" "expected '\\('")
reject(container_separated_chain "Math #{}\n\n#{}\nMain() {}" "expected identifier")
reject(container_unclosed "Math #{ F() {}" "expected '}'")
reject(container_reference "Math #{ F(int& n) {} }\nMain() { Math.F(1) }" "must be a mutable variable")
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
 while index < 2 { Print(index); index++ }
}
]=] "7\n7\n7\n7.5\n5000000000\n0\n1\n")
reject(long_overflow "Main() { long n = 9223372036854775808 }" "integer literal out of range")
reject(long_negative_overflow "Main() { long n = -9223372036854775809 }" "integer literal out of range")
reject(long_narrowing "Main() { long n = 1; int small = n }" "type mismatch")
reject(long_reference_width "F(long& n) {}\nMain() { int n = 1; F(n) }" "type mismatch")
reject(long_reference_reverse "F(int& n) {}\nMain() { long n = 1; F(n) }" "type mismatch")
reject(long_float_narrowing "Main() { long n = 1.5 }" "type mismatch")
reject(long_bool "Main() { long n = true }" "type mismatch")
reject(long_compound_narrowing "Main() { int n = 1; n += 5000000000 }" "type mismatch")
reject(long_constant "Main() { #long n = 1; n++ }" "cannot modify constant")
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
reject(cast_reference "F(long& n) {}\nMain() { n : 1; F((long)n) }" "must be a mutable variable")
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
      "F(${type}& a, ${type} b) { a ${op}= b }\nMain() { ${type} n = 7; F(n, 0) }")
    trap_program(${type}_${operation}_assign_overflow
      "F(${type}& a, ${type} b) { a ${op}= b }\nMain() { ${type} n = ${minimum}; F(n, -1) }")
  endforeach()
  run_program(${type}_division_boundaries
    "Main() { ${type} n = ${minimum}; Print(n / 1); Print(n % 1); Print(n / n); Print(n % n); ${type} a = -7; Print(a / 3); Print(a % 3); Print(a / -3); Print(a % -3); a /= -1; Print(a); a %= 3; Print(a) }"
    "${minimum}\n0\n1\n0\n-2\n-1\n2\n-1\n7\n1\n")
endforeach()
run_program(division_control_flow [=[
int Next(int& n) { n++; return n }
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
Change(int8& a, int8& b) { a = 12; b += a }
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
 a--; Print(a); a++; Print(a)
 a = 10; a *= 12; a += 10; Print(a); a -= 1; Print(a)
 Forward(a); Print(a)
 short s = -32768
 Print(-s)
 s--; Print(s); s++; Print(s)
 Print((short)16384 * (short)4)
 Print(b + s)
 Print(b + 1)
 Print(Identity(-128))
 int8 n
 if true { n = 3 } else { n = -4 }
 inferred : n
 inferred++; Print(inferred)
 Print(true ? n | s)
 Print(false ? n | 128)
 int32 i = n; int64 l = s; float f = n; double d = s
 Print(i); Print(l); Print(f); Print(d)
}
]=] "-128\n-128\n127\n0\n1\n1\n127\n-128\n-126\n-127\n24\n-32768\n32767\n-32768\n0\n-32641\n128\n-128\n4\n3\n128\n3\n-32768\n3\n-32768\n")
run_program(small_integer_casts [=[
Main() {
 Print((int8)128); Print((short)65535); Print((int8)(long)511)
 Print((int8)(short)-129); Print((short)(int8)-128)
 Print((bool)(int8)-128); Print((bool)(short)0)
 Print((int8)true); Print((short)false)
 Print((float)(int8)-128); Print((double)(short)-32768)
 double hi8 = 127.99; double lo8 = -128.0
 double hi16 = 32767.99; double lo16 = -32768.0
 Print((int8)hi8); Print((int8)lo8)
 Print((short)hi16); Print((short)lo16)
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
  reject(${type}_narrow "Main() { int n = 1; ${type} s = n }" "type mismatch")
  reject(${type}_arithmetic_narrow "Main() { ${type} s = 1; s = s + 1 }" "type mismatch")
  reject(${type}_reference_mismatch "F(${type}& n) {}\nMain() { int n = 1; F(n) }" "type mismatch")
  reject(${type}_constant_reference "F(${type}& n) {}\nMain() { #${type} n = 1; F(n) }" "cannot pass a constant")
  reject(${type}_uninitialized "Main() { ${type} n; Print(n) }" "read before assignment")
  trap_program(${type}_cast_high "Main() { Print((${type})${above}.0) }")
  trap_program(${type}_cast_low "Main() { double d = ${fraction_below}; Print((${type})d) }")
  trap_program(${type}_cast_nan "Main() { Print((${type})(0.0 / 0.0)) }")
  trap_program(${type}_cast_inf "Main() { Print((${type})(1.0 / 0.0)) }")
endforeach()
reject(short_to_int8 "Main() { short s = 1; int8 n = s }" "type mismatch")
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
    "${type} Maximum() { return ${maximum} }\nChange(${type}& n) { n++; n -= 1 }\nMain() { #${type} top = Maximum(); ${type} n = top; Print(n); n++; Print(n); n--; Print(n); n *= 2; Print(n / (${type})2); Print(n % (${type})2); n = top; Print(n / (${type})2); Print(n % (${type})2); Print(n > (${type})${half}); Print(n >= top); Print(n < (${type})1); Print(n <= top); Print(n == top); Print(n != (${type})0); Change(n); Print(n); n = ${half}; Print(-n); Print(n / top); Print(n % top); n /= ${half}; Print(n); n %= 1; Print(n); Print((bool)n); Print((${type})true); Print((${type})false); Print((${type})-1); Print((${type})-0.0) }"
    "${maximum}\n0\n${maximum}\n${quotient}\n0\n${quotient}\n1\n1\n1\n0\n1\n1\n1\n${maximum}\n${half}\n0\n${half}\n1\n0\n0\n1\n0\n${maximum}\n0\n")
  reject(${type}_literal_overflow "Main() { ${type} n = ${above} }" "integer literal out of range")
  reject(${type}_negative_literal "Main() { ${type} n = -1 }" "integer literal out of range")
  reject(${type}_signed_assignment "Main() { int n = 1; ${type} u = n }" "type mismatch")
  reject(${type}_signed_reference "F(${type}& n) {}\nMain() { int${width} n = 1; F(n) }" "type mismatch")
  reject(${type}_constant_reference "F(${type}& n) {}\nMain() { #${type} n = 1; F(n) }" "cannot pass a constant")
  reject(${type}_uninitialized "Main() { ${type} n; Print(n) }" "read before assignment")
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
      "F(${type}& a, ${type} b) { a ${op}= b }\nMain() { ${type} n = 7; F(n, 0) }")
  endforeach()
  trap_program(${type}_cast_upper "Main() { double d = ${above}.0; Print((${type})d) }")
  trap_program(${type}_cast_negative "Main() { Print((${type})-0.01) }")
  trap_program(${type}_cast_nan "Main() { Print((${type})(0.0 / 0.0)) }")
  trap_program(${type}_cast_infinity "Main() { Print((${type})(1.0 / 0.0)) }")
endforeach()
run_program(unsigned_conversions [=[
Alias(byte& b, uint& u) { b++; u++ }
Forward(uint8& b, uint32& u) { Alias(b, u) }
uint64 Recurse(uint64 n) { if n == (uint64)0 { return 1 }; return Recurse(n - (uint64)1) * (uint64)2 }
Main() {
 byte b = 255; uint u = 4294967295
 Forward(b, u); Print(b); Print(u)
 b = 255; uint16 s = b; uint32 i = s; uint64 l = i
 Print(s); Print(i); Print(l)
 short ss = b; int si = s; long sl = i
 Print(ss); Print(si); Print(sl)
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
 Print((uint32)limit32); Print((uint64)limit64)
 double power = 9223372036854775808.0
 Print((uint64)power)
}
]=] "0\n0\n255\n255\n255\n255\n255\n255\n4294967295\n18446744073709551615\n-1\n-1\n-1\n-1\n255\n65535\n0\n18446744073709551615\n-9223372036854775808\n-2147483648\n16\n1\n1\n255\n65535\n4294967295\n18446744073709549568\n9223372036854775808\n")
run_program(mixed_integers [=[
Main() {
 uint8 a = 255; int8 b = -128
 Print(a + b); Print(b + a); Print(a > b); Print(b < a)
 Print(true ? a | b); Print(false ? a | b)
 uint16 c = 65535; short d = -32768
 Print(c + d); Print(d + c)
 uint32 e = 4294967295; int f = -2147483648
 Print(e + f); Print(f + e); Print(e > f); Print(f > e)
 Print(e == f); Print(e != f)
 Print(e + 1); Print(1 + e)
 Print(e / -1); Print(e % -2)
 uint64 huge = 18446744073709551615
 Print(false && huge / (uint64)0 == (uint64)0)
 Print(true ? huge / (uint64)3 | huge / (uint64)0)
 Print((huge / (uint64)3 > (uint64)0) && (huge % (uint64)2 == (uint64)1))
}
]=] "127\n127\n1\n1\n255\n-128\n32767\n32767\n2147483647\n2147483647\n1\n0\n0\n1\n4294967296\n4294967296\n-4294967295\n1\n0\n6148914691236517205\n1\n")
reject(uint64_mixed "Main() { uint64 n = 1; Print(n + 1) }" "explicit cast")
reject(uint64_mixed_reverse "Main() { uint64 n = 1; Print(-1 < n) }" "explicit cast")
reject(uint64_conditional "Main() { uint64 n = 1; Print(true ? n | -1) }" "explicit cast")
reject(uint64_to_long "Main() { uint64 n = 1; long s = n }" "type mismatch")
reject(uint32_to_int "Main() { uint32 n = 1; int s = n }" "type mismatch")
reject(unsigned_narrow "Main() { uint64 n = 1; uint32 s = n }" "type mismatch")
reject(uint64_literal_overflow_inferred "Main() { n : 18446744073709551616 }" "integer literal out of range")
reject(negative_literal_overflow_inferred "Main() { Print(-9223372036854775809) }" "integer literal out of range")
message(STATUS "Duck integration tests passed")
