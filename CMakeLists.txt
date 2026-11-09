cmake_minimum_required(VERSION 3.20)
project(Duck VERSION 0.1.0 LANGUAGES CXX)

set(DUCK_COMPILER_SOURCES
  Lexer.cpp Preprocessor.cpp Parser.cpp Symbols.cpp
  Semantics.cpp Codegen.cpp Pipeline.cpp TextRuntime.cpp HeapRuntime.cpp
  CalendarRuntime.cpp WideRuntime.cpp BigRuntime.cpp
  DecimalRuntime.cpp CollectionRuntime.cpp
  Scheduling.cpp TaskRuntime.cpp FuncRuntime.cpp
  CompileTime.cpp TestRuntime.cpp MarkupRuntime.cpp ScopeRuntime.cpp)
list(TRANSFORM DUCK_COMPILER_SOURCES PREPEND Compiler/Private/)
add_library(duck_compiler ${DUCK_COMPILER_SOURCES})
target_compile_features(duck_compiler PUBLIC cxx_std_17)
target_include_directories(duck_compiler PUBLIC Compiler/Public PRIVATE Compiler/Private)
add_executable(duckc Compiler/Private/main.cpp)
target_link_libraries(duckc PRIVATE duck_compiler)
add_executable(duck BuildTool/main.cpp BuildTool/Process.cpp)
target_compile_features(duck PRIVATE cxx_std_17)
target_compile_definitions(duck PRIVATE DUCK_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
find_package(Threads REQUIRED)
target_link_libraries(duck PRIVATE Threads::Threads)
if(MSVC)
  target_compile_options(duck_compiler PRIVATE /W4 /permissive-)
  target_compile_options(duck PRIVATE /W4 /permissive- /utf-8)
else()
  target_compile_options(duck_compiler PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(duck PRIVATE -Wall -Wextra -Wpedantic)
  if(MINGW)
    target_link_options(duck PRIVATE -municode)
  endif()
endif()

include(CTest)
if(BUILD_TESTING)
  add_executable(duck_stage_tests Tests/stages.cpp)
  target_link_libraries(duck_stage_tests PRIVATE duck_compiler Threads::Threads)
  add_test(NAME compiler.stages COMMAND duck_stage_tests)
  find_program(DUCK_CLANG NAMES clang HINTS "C:/Program Files/LLVM/bin" REQUIRED)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_FOUND)
    add_test(NAME compiler.compile_time COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/Tests/compile_time.py
      --compiler $<TARGET_FILE:duckc> --clang ${DUCK_CLANG})
    add_test(NAME buildtool.integration COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/Tests/build_tool.py
      --builder $<TARGET_FILE:duck> --compiler $<TARGET_FILE:duckc> --clang ${DUCK_CLANG})
    add_test(NAME compiler.text COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/Tests/text.py
      --compiler $<TARGET_FILE:duckc> --clang ${DUCK_CLANG})
    add_test(NAME compiler.calendar COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/Tests/calendar.py
      --compiler $<TARGET_FILE:duckc> --clang ${DUCK_CLANG})
    add_test(NAME compiler.wide_integers COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/Tests/wide_integers.py
      --compiler $<TARGET_FILE:duckc> --clang ${DUCK_CLANG})
    add_test(NAME compiler.vectors COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/Tests/vectors.py
      --compiler $<TARGET_FILE:duckc> --clang ${DUCK_CLANG})
    add_test(NAME compiler.math COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/Tests/math.py
      --compiler $<TARGET_FILE:duckc> --clang ${DUCK_CLANG})
    add_test(NAME compiler.diagnostics COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/Tests/diagnostics.py
      --compiler $<TARGET_FILE:duckc>)
    add_test(NAME compiler.arbitrary_numbers COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/Tests/arbitrary_numbers.py
      --compiler $<TARGET_FILE:duckc> --clang ${DUCK_CLANG})
    add_test(NAME compiler.ownership COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/Tests/ownership.py
      --compiler $<TARGET_FILE:duckc> --clang ${DUCK_CLANG})
    add_test(NAME compiler.unit_tests COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/Tests/unit_tests.py
      --compiler $<TARGET_FILE:duckc> --clang ${DUCK_CLANG})
    add_test(NAME compiler.markup COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/Tests/markup.py
      --compiler $<TARGET_FILE:duckc> --clang ${DUCK_CLANG})
    add_test(NAME compiler.lifetimes COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/Tests/lifetimes.py
      --compiler $<TARGET_FILE:duckc> --clang ${DUCK_CLANG})
    add_test(NAME compiler.ordered COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/Tests/ordered.py
      --compiler $<TARGET_FILE:duckc> --clang ${DUCK_CLANG})
    add_test(NAME compiler.native COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/Tests/native_interop.py
      --builder $<TARGET_FILE:duck> --compiler $<TARGET_FILE:duckc> --clang ${DUCK_CLANG})
  endif()
  add_test(NAME compiler.integration COMMAND ${CMAKE_COMMAND}
    -DCOMPILER=$<TARGET_FILE:duckc> -DCLANG=${DUCK_CLANG}
    -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR} -DWORK=${CMAKE_CURRENT_BINARY_DIR}/tests
    -P ${CMAKE_CURRENT_SOURCE_DIR}/Tests/run.cmake)
  add_test(NAME compiler.tasks COMMAND ${CMAKE_COMMAND}
    -DCOMPILER=$<TARGET_FILE:duckc> -DCLANG=${DUCK_CLANG}
    -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR} -DWORK=${CMAKE_CURRENT_BINARY_DIR}/task-tests
    -P ${CMAKE_CURRENT_SOURCE_DIR}/Tests/tasks.cmake)
endif()
