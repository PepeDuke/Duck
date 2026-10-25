cmake_minimum_required(VERSION 3.20)
project(Duck VERSION 0.1.0 LANGUAGES CXX)

add_library(duck_compiler
  Compiler/Lexer.cpp Compiler/Parser.cpp Compiler/Symbols.cpp
  Compiler/Semantics.cpp Compiler/Codegen.cpp Compiler/Pipeline.cpp)
target_compile_features(duck_compiler PUBLIC cxx_std_17)
target_include_directories(duck_compiler PUBLIC Compiler)
add_executable(duckc Compiler/main.cpp)
target_link_libraries(duckc PRIVATE duck_compiler)
if(MSVC)
  target_compile_options(duck_compiler PRIVATE /W4 /permissive-)
else()
  target_compile_options(duck_compiler PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(CTest)
if(BUILD_TESTING)
  find_package(Threads REQUIRED)
  add_executable(duck_stage_tests Tests/stages.cpp)
  target_link_libraries(duck_stage_tests PRIVATE duck_compiler Threads::Threads)
  add_test(NAME compiler.stages COMMAND duck_stage_tests)
  find_program(DUCK_CLANG NAMES clang HINTS "C:/Program Files/LLVM/bin" REQUIRED)
  find_package(Python3 3.11 COMPONENTS Interpreter)
  if(Python3_FOUND)
    add_test(NAME buildtool.integration COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/Tests/build_tool.py
      --compiler $<TARGET_FILE:duckc> --clang ${DUCK_CLANG})
  endif()
  add_test(NAME compiler.integration COMMAND ${CMAKE_COMMAND}
    -DCOMPILER=$<TARGET_FILE:duckc> -DCLANG=${DUCK_CLANG}
    -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR} -DWORK=${CMAKE_CURRENT_BINARY_DIR}/tests
    -P ${CMAKE_CURRENT_SOURCE_DIR}/Tests/run.cmake)
endif()
