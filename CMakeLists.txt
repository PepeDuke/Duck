cmake_minimum_required(VERSION 3.20)
project(Duck VERSION 0.1.0 LANGUAGES CXX)

add_library(duck_compiler
  Compiler/Lexer.cpp Compiler/Parser.cpp Compiler/Symbols.cpp
  Compiler/Semantics.cpp Compiler/Codegen.cpp Compiler/Pipeline.cpp Compiler/TextRuntime.cpp Compiler/HeapRuntime.cpp
  Compiler/CalendarRuntime.cpp Compiler/WideRuntime.cpp Compiler/BigRuntime.cpp
  Compiler/DecimalRuntime.cpp Compiler/CollectionRuntime.cpp
  Compiler/Scheduling.cpp Compiler/TaskRuntime.cpp Compiler/FuncRuntime.cpp)
target_compile_features(duck_compiler PUBLIC cxx_std_17)
target_include_directories(duck_compiler PUBLIC Compiler)
add_executable(duckc Compiler/main.cpp)
target_link_libraries(duckc PRIVATE duck_compiler)
add_executable(duck BuildTool/main.cpp BuildTool/Process.cpp)
target_compile_features(duck PRIVATE cxx_std_17)
target_compile_definitions(duck PRIVATE DUCK_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
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
  find_package(Threads REQUIRED)
  add_executable(duck_stage_tests Tests/stages.cpp)
  target_link_libraries(duck_stage_tests PRIVATE duck_compiler Threads::Threads)
  add_test(NAME compiler.stages COMMAND duck_stage_tests)
  find_program(DUCK_CLANG NAMES clang HINTS "C:/Program Files/LLVM/bin" REQUIRED)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_FOUND)
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
    add_test(NAME compiler.arbitrary_numbers COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/Tests/arbitrary_numbers.py
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
