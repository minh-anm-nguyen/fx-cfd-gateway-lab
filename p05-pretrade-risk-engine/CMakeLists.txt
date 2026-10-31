cmake_minimum_required(VERSION 3.20)
project(p05_pretrade_risk_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(RISK_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
# The crash harness launches a child process via std::system. On MinGW that
# child cannot find libstdc++/libgcc unless the toolchain's bin directory is on
# a Windows-style PATH, which a POSIX shell does not provide -- and a child that
# fails to start looks exactly like a child that crashed. Linking the runtime
# statically makes every binary self-contained, so "the child never ran" can
# never masquerade as "the child crashed where we asked it to".
option(RISK_STATIC_RUNTIME "Statically link the C++ runtime (MinGW)" ON)
option(RISK_FORCE_PORTABLE_I128 "Build the portable 128-bit backend" OFF)
option(RISK_SANITIZE_ADDRESS "Build with ASan+UBSan" OFF)
option(RISK_SANITIZE_THREAD "Build with TSan" OFF)

# Project 08 links this risk engine into its lab as a library. Rebuilding the suites and
# CLIs inside that superbuild compiles every component a second time for no
# added evidence and collides with P03 over the `tiny_test` target name, so they are a top-level concern only. A standalone
# build is unchanged: PROJECT_IS_TOP_LEVEL is true there.
option(RISK_BUILD_TOOLS "Build the test suites and CLIs" ${PROJECT_IS_TOP_LEVEL})

if(RISK_SANITIZE_ADDRESS AND RISK_SANITIZE_THREAD)
  # §24.4: ASan and TSan must not share a binary.
  message(FATAL_ERROR "RISK_SANITIZE_ADDRESS and RISK_SANITIZE_THREAD are mutually exclusive")
endif()

add_library(risk_engine STATIC
  src/result.cpp
  src/types.cpp
  src/wide.cpp
  src/rational.cpp
  src/hash.cpp
  src/clock.cpp
  src/policy.cpp
  src/config.cpp
  src/market.cpp
  src/valuation.cpp
  src/exposure.cpp
  src/reservation.cpp
  src/failpoint.cpp
  src/metrics.cpp
  src/store.cpp
  src/memory_store.cpp
  src/journal_store.cpp
  src/audit.cpp
  src/coordinator.cpp
  src/coordinator_events.cpp
  src/recovery.cpp
  src/scenario.cpp
)
target_include_directories(risk_engine PUBLIC include)

if(MSVC)
  target_compile_options(risk_engine PRIVATE /W4 /permissive-)
  if(RISK_WARNINGS_AS_ERRORS)
    target_compile_options(risk_engine PRIVATE /WX)
  endif()
else()
  target_compile_options(risk_engine PRIVATE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
    -Wold-style-cast -Wnon-virtual-dtor -Woverloaded-virtual -Wdouble-promotion)
  if(RISK_WARNINGS_AS_ERRORS)
    target_compile_options(risk_engine PRIVATE -Werror)
  endif()
endif()

if(RISK_FORCE_PORTABLE_I128)
  target_compile_definitions(risk_engine PUBLIC RISK_FORCE_PORTABLE_I128=1)
endif()

if(RISK_SANITIZE_ADDRESS)
  target_compile_options(risk_engine PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(risk_engine PUBLIC -fsanitize=address,undefined)
endif()
if(RISK_SANITIZE_THREAD)
  target_compile_options(risk_engine PUBLIC -fsanitize=thread -fno-omit-frame-pointer)
  target_link_options(risk_engine PUBLIC -fsanitize=thread)
endif()

find_package(Threads REQUIRED)
target_link_libraries(risk_engine PUBLIC Threads::Threads)

if(RISK_STATIC_RUNTIME AND MINGW AND NOT RISK_SANITIZE_ADDRESS AND NOT RISK_SANITIZE_THREAD)
  target_link_options(risk_engine PUBLIC -static -static-libgcc -static-libstdc++)
endif()

if(NOT RISK_BUILD_TOOLS)
  return()
endif()

# ---------------------------------------------------------------------------
# Applications
# ---------------------------------------------------------------------------
add_executable(risk_cli apps/risk_cli/main.cpp)
target_link_libraries(risk_cli PRIVATE risk_engine)
# �25.2: a benchmark number is meaningless without the build it came from.
target_compile_definitions(risk_cli PRIVATE
  RISK_BUILD_TYPE="${CMAKE_BUILD_TYPE}")

add_executable(risk_scenario apps/risk_scenario/main.cpp)
target_link_libraries(risk_scenario PRIVATE risk_engine)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()

add_library(tiny_test STATIC tests/support/tiny_test.cpp)
target_include_directories(tiny_test PUBLIC tests/support)

file(GLOB RISK_TEST_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/property/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/model/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/scenario/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/crash/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/concurrency/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/chaos/*.cpp
)

add_executable(risk_tests ${RISK_TEST_SOURCES})
target_link_libraries(risk_tests PRIVATE risk_engine tiny_test)
target_include_directories(risk_tests PRIVATE tests/support tests)

# One ctest entry per suite directory so a failure names the area it came from.
foreach(suite unit property model scenario crash concurrency chaos)
  add_test(NAME ${suite} COMMAND risk_tests --filter=${suite})
  set_tests_properties(${suite} PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endforeach()

# The differential check against the independent Python oracle (§24.1). It
# consumes the artifact the `model` suite writes, so it must run after it.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  add_test(NAME oracle
    COMMAND ${Python3_EXECUTABLE} -I
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracle/reference_model.py
            ${CMAKE_CURRENT_SOURCE_DIR}/test-artifacts/model_traces.json)
  set_tests_properties(oracle PROPERTIES
    DEPENDS model
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
else()
  message(STATUS "Python3 not found: the differential oracle test is unavailable")
endif()

# The crash harness re-executes this binary as a child and SIGKILLs it at a
# named failpoint; it must be a separate program so the parent survives.
add_executable(risk_crash_child tests/support/crash_child.cpp)
target_link_libraries(risk_crash_child PRIVATE risk_engine)
target_include_directories(risk_crash_child PRIVATE tests/support)
