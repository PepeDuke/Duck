# Worker task scheduling: each program is compiled with tasks and with
# --sequential. Every build must print the same output with one worker
# (all tasks inline) and with several workers.
file(MAKE_DIRECTORY "${WORK}")

function(compile_both name)
  foreach(mode IN ITEMS parallel sequential)
    set(flags)
    if(mode STREQUAL "sequential")
      set(flags --sequential)
    endif()
    execute_process(COMMAND "${COMPILER}" ${flags} "${WORK}/${name}.dk" -o "${WORK}/${name}-${mode}.ll"
      RESULT_VARIABLE status ERROR_VARIABLE errors)
    if(NOT status EQUAL 0)
      message(FATAL_ERROR "${name} ${mode}: compilation failed: ${errors}")
    endif()
  endforeach()
  file(READ "${WORK}/${name}-parallel.ll" parallel_ir)
  file(READ "${WORK}/${name}-sequential.ll" sequential_ir)
  if(NOT parallel_ir MATCHES "call ptr @duck\\.task\\$spawn")
    message(FATAL_ERROR "${name}: expected at least one worker task")
  endif()
  if(sequential_ir MATCHES "duck\\.task")
    message(FATAL_ERROR "${name}: --sequential build still uses worker tasks")
  endif()
endfunction()

# Runs <exe> with DUCK_WORKERS=<workers>; empty means the default (CPU count).
function(run_with_workers exe workers status_var output_var)
  set(command "${exe}")
  if(NOT workers STREQUAL "")
    set(command "${CMAKE_COMMAND}" -E env "DUCK_WORKERS=${workers}" "${exe}")
  endif()
  execute_process(COMMAND ${command} RESULT_VARIABLE status OUTPUT_VARIABLE actual
    ERROR_VARIABLE errors TIMEOUT 30)
  string(REPLACE "\r\n" "\n" actual "${actual}")
  set(${status_var} "${status}" PARENT_SCOPE)
  set(${output_var} "${actual}" PARENT_SCOPE)
endfunction()

function(run_scheduled name source expected)
  file(WRITE "${WORK}/${name}.dk" "${source}")
  compile_both(${name})
  foreach(optimization IN ITEMS -O0 -O2)
    foreach(mode IN ITEMS parallel sequential)
      set(exe "${WORK}/${name}-${mode}${optimization}.exe")
      execute_process(COMMAND "${CLANG}" ${optimization} "${WORK}/${name}-${mode}.ll" -o "${exe}"
        RESULT_VARIABLE status ERROR_VARIABLE errors)
      if(NOT status EQUAL 0)
        message(FATAL_ERROR "${name} ${mode} ${optimization}: LLVM/link failure: ${errors}")
      endif()
      foreach(workers IN ITEMS 1 4 "")
        run_with_workers("${exe}" "${workers}" status actual)
        if(NOT status EQUAL 0 OR NOT actual STREQUAL expected)
          message(FATAL_ERROR "${name} ${mode} ${optimization} workers=[${workers}]: exit=${status}, "
            "expected=[${expected}], actual=[${actual}]")
        endif()
      endforeach()
    endforeach()
  endforeach()
endfunction()

# A trapping task must stop the program before any later output, as it would sequentially.
function(trap_scheduled name source)
  file(WRITE "${WORK}/${name}.dk" "${source}")
  compile_both(${name})
  execute_process(COMMAND "${CLANG}" -O0 "${WORK}/${name}-parallel.ll" -o "${WORK}/${name}.exe"
    RESULT_VARIABLE status ERROR_VARIABLE errors)
  if(NOT status EQUAL 0)
    message(FATAL_ERROR "${name}: LLVM/link failure: ${errors}")
  endif()
  foreach(workers IN ITEMS 1 4)
    run_with_workers("${WORK}/${name}.exe" "${workers}" status actual)
    if("${status}" STREQUAL "0" OR "${status}" MATCHES "[Tt]imeout" OR NOT actual STREQUAL "")
      message(FATAL_ERROR "${name} workers=${workers}: expected a trap before output, "
        "got exit=${status}, output=[${actual}]")
    endif()
  endforeach()
endfunction()

file(READ "${SOURCE}/Examples/Bootstrap/15_WorkerTasks.dk" worker_tasks)
run_scheduled(worker_tasks "${worker_tasks}" "46368\n27000\n0\n557\n1\n17712\n")

run_scheduled(task_joins [=[
bool IsEven(int n) {
 if n == 0 { return true }
 return IsOdd(n - 1)
}
bool IsOdd(int n) {
 if n == 0 { return false }
 return IsEven(n - 1)
}
double Half(int n) { return n / 2.0 }
int Slow(int n) {
 int total = 0
 for int i = 0; i < n; i++ { total += i % 3 }
 return total
}
int First(int limit) {
 for int i = 0; i < limit; i++ {
  value : Slow(i * 1000)
  other : Slow(i)
  if value > 1000 { return value + other }
 }
 return -1
}
Main() {
 a : IsEven(10001)
 b : Slow(100000)
 h : Half(7)
 Print(a)
 Print(b)
 Print(h)
 int count = 0
 while true {
  x : Slow(count * 10)
  y : Slow(5)
  count++
  if count == 3 { break }
 }
 Print(count)
 Print(First(10))
 {
  inner : Slow(50)
  outer : Slow(60)
  { Print(inner) }
  Print(outer)
 }
 n : Slow(9)
 m : 3
 if m > 0 { int n = 4
 Print(n) }
 Print(n)
 total : Slow(10)
 total2 : Slow(20)
 total += total2
 Print(total)
}
]=] "0\n99999\n3.5\n3\n2000\n49\n60\n4\n9\n28\n")

# The task traps only after a delay, and output is flushed explicitly, so
# printing before the join would be visible despite pipe buffering.
trap_scheduled(task_trap_order [=[
extern "C" int _flushall()
int SlowDivide(int a, int b) {
 int spin = 0
 for int i = 0; i < 200000000; i++ { spin += i % 3 }
 return a / (b + spin - spin)
}
int Divide(int a, int b) { return a / b }
Main() {
 x : SlowDivide(1, 0)
 y : Divide(4, 2)
 Print(y)
 _flushall()
 Print(x)
}
]=])
message(STATUS "Duck worker task tests passed")
