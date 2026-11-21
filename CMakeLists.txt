cmake_minimum_required(VERSION 3.20)
project(Duck VERSION 0.1.0 LANGUAGES CXX)

set(DUCK_COMPILER_SOURCES
  Lexer.cpp Preprocessor.cpp Parser.cpp Symbols.cpp
  Semantics.cpp Codegen.cpp Pipeline.cpp TextRuntime.cpp HeapRuntime.cpp
  CalendarRuntime.cpp WideRuntime.cpp BigRuntime.cpp
  DecimalRuntime.cpp CollectionRuntime.cpp
  Scheduling.cpp TaskRuntime.cpp FuncRuntime.cpp
  CompileTime.cpp TestRuntime.cpp MarkupRuntime.cpp ScopeRuntime.cpp FileRuntime.cpp
  Interface.cpp)
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

add_executable(duck_std_compiler Standard/Compile.cpp)
target_link_libraries(duck_std_compiler PRIVATE duck_compiler)
set(net_src "${CMAKE_CURRENT_SOURCE_DIR}/Standard/Net")
set(net_gen "${CMAKE_CURRENT_BINARY_DIR}/standard/$<CONFIG>")
add_custom_command(OUTPUT "${net_gen}/WebTransport.ll" "${net_gen}/WebTransport.dki" "${net_gen}/WebInterface.h"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${net_gen}"
  COMMAND duck_std_compiler "${net_src}/WebTransport.dk" "${net_gen}/WebTransport.ll" "${net_gen}/WebTransport.dki" wasm64
  COMMAND ${CMAKE_COMMAND} "-DINPUT=${net_gen}/WebTransport.dki" "-DLIBRARY=${net_gen}/WebTransport.ll"
    "-DTEMPLATE=${net_src}/WebInterface.h.in" "-DOUTPUT=${net_gen}/WebInterface.h" -P "${net_src}/Embed.cmake"
  DEPENDS duck_std_compiler "${net_src}/WebTransport.dk" "${net_src}/WebInterface.h.in" "${net_src}/Embed.cmake"
  VERBATIM)
add_custom_target(duck_web_net
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${net_gen}/WebTransport.ll" "${net_src}/WebTransport.js" "$<TARGET_FILE_DIR:duckc>"
  DEPENDS "${net_gen}/WebInterface.h"
  VERBATIM)
add_dependencies(duckc duck_web_net)
target_include_directories(duckc PRIVATE "${net_gen}")

if(WIN32)
  find_program(DUCK_CLANG NAMES clang HINTS "C:/Program Files/LLVM/bin" REQUIRED)
  add_custom_command(OUTPUT "${net_gen}/Net.obj" "${net_gen}/Net.dki"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${net_gen}"
    COMMAND duck_std_compiler "${net_src}/Net.dk" "${net_gen}/Net.ll" "${net_gen}/Net.dki"
    COMMAND "${DUCK_CLANG}" -O2 -c "${net_gen}/Net.ll" -o "${net_gen}/Net.obj"
    DEPENDS duck_std_compiler "${net_src}/Net.dk"
    VERBATIM)
  add_library(duck_net STATIC Standard/Net/Windows.cpp "${net_gen}/Net.obj")
  set_property(TARGET duck_net PROPERTY MSVC_RUNTIME_LIBRARY MultiThreaded)
  add_custom_command(OUTPUT "${net_gen}/NetInterface.h"
    COMMAND ${CMAKE_COMMAND} "-DINPUT=${net_gen}/Net.dki" "-DLIBRARY=$<TARGET_FILE:duck_net>"
      "-DTEMPLATE=${net_src}/Interface.h.in" "-DOUTPUT=${net_gen}/NetInterface.h" -P "${net_src}/Embed.cmake"
    DEPENDS duck_net "${net_gen}/Net.dki" "${net_src}/Interface.h.in" "${net_src}/Embed.cmake"
    VERBATIM)
  target_sources(duckc PRIVATE "${net_gen}/NetInterface.h")
  target_compile_definitions(duckc PRIVATE DUCK_NET_LIBRARY="$<TARGET_FILE_NAME:duck_net>")
endif()

include(CTest)
if(BUILD_TESTING)
  add_executable(duck_stage_tests Tests/stages.cpp)
  target_link_libraries(duck_stage_tests PRIVATE duck_compiler Threads::Threads)
  add_test(NAME compiler.stages COMMAND duck_stage_tests)
  find_program(DUCK_CLANG NAMES clang HINTS "C:/Program Files/LLVM/bin" REQUIRED)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_FOUND)
    if(DEFINED ENV{EMSDK})
      find_program(DUCK_EMCC NAMES emcc HINTS "$ENV{EMSDK}/upstream/emscripten")
      find_program(DUCK_WASM_DIS NAMES wasm-dis HINTS "$ENV{EMSDK}/upstream/bin")
      find_program(DUCK_NODE NAMES node)
      if(DUCK_EMCC AND DUCK_WASM_DIS AND DUCK_NODE)
        add_test(NAME buildtool.wasm64 COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/Tests/wasm64.py --builder $<TARGET_FILE:duck>
          --compiler $<TARGET_FILE:duckc> --emcc ${DUCK_EMCC} --node ${DUCK_NODE}
          --wasm-dis ${DUCK_WASM_DIS})
      endif()
    endif()
    if(WIN32)
      add_test(NAME compiler.net COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/net.py --compiler $<TARGET_FILE:duckc> --clang ${DUCK_CLANG})
    endif()
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
