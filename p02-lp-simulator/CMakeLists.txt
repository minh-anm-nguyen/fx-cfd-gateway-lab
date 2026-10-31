# Project 02 - FX/CFD Liquidity Provider Simulator (FIX 4.4 acceptor)
#
# Build layout follows design doc 13.2: one simulator process built from several
# libraries, plus a scenario runner and a scripted raw peer that exist only for
# the lab. CMake + CTest is the required harness (design doc 13.2, 18.5).
#
# Two build options matter for the test plan:
#
#   LPSIM_ENABLE_FAILPOINTS  compiles in the W1..W10 cutpoints. On by default,
#                            because the crash suites score a run INVALID rather
#                            than PASS when the cutpoint cannot be proven
#                            (scenario M-15).
#   LPSIM_SANITIZE           address+undefined, or thread. Never both in one
#                            binary: ASan and TSan are mutually exclusive, and
#                            the design doc calls for separate pipelines.

cmake_minimum_required(VERSION 3.20)
project(lp_simulator CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

option(LPSIM_ENABLE_FAILPOINTS "Compile in the W1..W10 crash cutpoints" ON)

# Project 08 links this simulator into its lab as a library. Rebuilding the suites and
# CLIs inside that superbuild compiles every component a second time for no
# added evidence, so they are a top-level concern only. A standalone
# build is unchanged: PROJECT_IS_TOP_LEVEL is true there.
option(LPSIM_BUILD_TOOLS "Build the test suites and CLIs" ${PROJECT_IS_TOP_LEVEL})
set(LPSIM_SANITIZE "" CACHE STRING "Sanitizer: empty, address, or thread")

add_library(lpsim STATIC
  src/result.cpp
  src/wire.cpp
  src/clock.cpp
  src/fixed_point.cpp
  src/framer.cpp
  src/codec.cpp
  src/domain.cpp
  src/quote_book.cpp
  src/matcher.cpp
  src/ids.cpp
  src/failpoint.cpp
  src/scheduler.cpp
  src/validator.cpp
  src/report_builder.cpp
  src/ownership.cpp
  src/sqlite_store.cpp
  src/order_actor.cpp
  src/session.cpp
  src/config.cpp
  src/metrics.cpp
  src/scenario.cpp
)
target_include_directories(lpsim PUBLIC include)

if(LPSIM_ENABLE_FAILPOINTS)
  target_compile_definitions(lpsim PUBLIC LPSIM_ENABLE_FAILPOINTS)
endif()

if(MSVC)
  target_compile_options(lpsim PRIVATE /W4)
else()
  # -Wpedantic is deliberately absent. The weighted-notional arithmetic uses
  # __int128, which is a compiler extension, and the design doc (6.4) calls for
  # exactly that to keep AvgPx exact. Enabling -Wpedantic would bury every real
  # warning under dozens of copies of the same note about it.
  target_compile_options(lpsim PRIVATE -Wall -Wextra)
endif()

# Winsock, for the acceptor's listener on Windows.
if(WIN32)
  target_link_libraries(lpsim PUBLIC ws2_32)
endif()

# SQLite. Prefer a system library; fall back to a vendored amalgamation when one
# is dropped into third_party/, so the build does not silently succeed against
# no storage engine at all.
find_package(SQLite3 QUIET)
if(SQLite3_FOUND)
  target_link_libraries(lpsim PUBLIC SQLite3::SQLite3)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite3/sqlite3.c")
  add_library(sqlite3_amalgamation STATIC third_party/sqlite3/sqlite3.c)
  target_include_directories(sqlite3_amalgamation PUBLIC third_party/sqlite3)
  target_link_libraries(lpsim PUBLIC sqlite3_amalgamation)
else()
  message(FATAL_ERROR
    "SQLite3 not found. Install the development package or place the "
    "amalgamation in third_party/sqlite3/.")
endif()

if(LPSIM_SANITIZE STREQUAL "address")
  target_compile_options(lpsim PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(lpsim PUBLIC -fsanitize=address,undefined)
elseif(LPSIM_SANITIZE STREQUAL "thread")
  target_compile_options(lpsim PUBLIC -fsanitize=thread -fno-omit-frame-pointer)
  target_link_options(lpsim PUBLIC -fsanitize=thread)
endif()

if(NOT LPSIM_BUILD_TOOLS)
  return()
endif()

# ---------------------------------------------------------------------------
# Test support
# ---------------------------------------------------------------------------

add_library(lpsim_testsupport STATIC tests/support/tiny_test.cpp)
target_include_directories(lpsim_testsupport PUBLIC tests/support)
target_link_libraries(lpsim_testsupport PUBLIC lpsim)

enable_testing()

set(LPSIM_ARTIFACTS "${CMAKE_CURRENT_SOURCE_DIR}/test-artifacts")

# add_lpsim_suite(<name> <source>)
# Registers one suite and points it at the shared artifact directory so that
# scripts/report.py can assemble the matrix report from every suite at once.
function(add_lpsim_suite name source)
  add_executable(${name} ${source})
  target_link_libraries(${name} PRIVATE lpsim_testsupport)
  add_test(NAME ${name}
           COMMAND ${name} --artifacts ${LPSIM_ARTIFACTS}
           WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  # A suite that hangs is a failure, not a reason to wait forever.
  set_tests_properties(${name} PROPERTIES TIMEOUT 300)
endfunction()

add_lpsim_suite(test_wire       tests/unit/test_wire.cpp)
add_lpsim_suite(test_quote      tests/unit/test_quote.cpp)
add_lpsim_suite(test_reports    tests/unit/test_reports.cpp)
add_lpsim_suite(test_scheduler  tests/unit/test_scheduler.cpp)
add_lpsim_suite(test_store      tests/unit/test_store.cpp)
add_lpsim_suite(test_business   tests/unit/test_business.cpp)
add_lpsim_suite(test_session    tests/unit/test_session.cpp)
add_lpsim_suite(test_model      tests/unit/test_model.cpp)
add_lpsim_suite(test_recovery   tests/integration/test_recovery.cpp)
add_lpsim_suite(test_playbooks  tests/integration/test_playbooks.cpp)

# The crash suite re-executes itself as a child process so that a failpoint can
# terminate it at an exact cutpoint; it needs its own executable path.
add_executable(test_crash tests/integration/test_crash.cpp)
target_link_libraries(test_crash PRIVATE lpsim_testsupport)
add_test(NAME test_crash
         COMMAND test_crash --artifacts ${LPSIM_ARTIFACTS}
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
set_tests_properties(test_crash PROPERTIES TIMEOUT 600)

# ---------------------------------------------------------------------------
# Applications
# ---------------------------------------------------------------------------

add_executable(lp_simulator apps/lp_simulator/main.cpp)
target_link_libraries(lp_simulator PRIVATE lpsim)

add_executable(scenario_runner apps/scenario_runner/main.cpp)
target_link_libraries(scenario_runner PRIVATE lpsim)

# ---------------------------------------------------------------------------
# Live socket checks
# ---------------------------------------------------------------------------
#
# A-08 needs two real sockets against the listener and therefore cannot be done
# in process at all, so it lives here rather than in test_session. The script
# writes its verdicts in the same schema as the C++ suites, so the matrix report
# counts them in the gate.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(NAME test_smoke
           COMMAND ${Python3_EXECUTABLE}
                   ${CMAKE_CURRENT_SOURCE_DIR}/scripts/smoke_tcp.py
                   --exe $<TARGET_FILE:lp_simulator>
                   --config ${CMAKE_CURRENT_SOURCE_DIR}/configs/lp.example.yaml
                   --artifacts ${LPSIM_ARTIFACTS}
                   --port 19898
           WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  set_tests_properties(test_smoke PROPERTIES TIMEOUT 120 DEPENDS test_session)
else()
  message(WARNING
    "Python 3 not found: the live socket checks (A-08 among them) will not run, "
    "and the matrix report will show them as missing rather than passing.")
endif()
