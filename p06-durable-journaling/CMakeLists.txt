cmake_minimum_required(VERSION 3.20)
project(durable_journaling LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

# The 24 crash windows in design doc section 15 are only testable if the
# process can be stopped inside them. Failpoints are compiled in for lab
# builds and must be OFF for anything resembling production (JI-13 names the
# hooks test-only).
option(JRNL_ENABLE_FAILPOINTS "Compile fault injection points" ON)
option(JRNL_ASAN "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(JRNL_TSAN "Build with ThreadSanitizer" OFF)
# Trapping UB checks need no sanitizer runtime, which matters because the
# mingw-w64 toolchain here ships no libubsan. Less informative than UBSan
# proper, but it runs. Paired with _GLIBCXX_ASSERTIONS for container checks.
option(JRNL_UBSAN_TRAP "Build with trapping undefined behaviour checks" OFF)

# Project 08 links this journal into its lab as a library. Rebuilding the suites and
# CLIs inside that superbuild compiles every component a second time for no
# added evidence, so they are a top-level concern only. A standalone
# build is unchanged: PROJECT_IS_TOP_LEVEL is true there.
option(JRNL_BUILD_TOOLS "Build the test suites and CLIs" ${PROJECT_IS_TOP_LEVEL})

if(JRNL_ASAN AND JRNL_TSAN)
  message(FATAL_ERROR "JRNL_ASAN and JRNL_TSAN cannot be enabled together")
endif()

# On an MSYS2 or MinGW toolchain the SDK lives in the compiler's own prefix,
# which is not on CMake's default search path.
get_filename_component(_jrnl_cxx_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
get_filename_component(_jrnl_prefix "${_jrnl_cxx_dir}/.." ABSOLUTE)
find_path(SQLITE3_INCLUDE_DIR sqlite3.h HINTS "${_jrnl_prefix}/include")
find_library(SQLITE3_LIBRARY NAMES sqlite3 HINTS "${_jrnl_prefix}/lib")
if(NOT SQLITE3_INCLUDE_DIR OR NOT SQLITE3_LIBRARY)
  message(FATAL_ERROR "sqlite3 development files not found")
endif()

add_library(jrnl STATIC
  src/hash.cpp
  src/canon.cpp
  src/result.cpp
  src/clock.cpp
  src/failpoint.cpp
  src/reducer.cpp
  src/sql.cpp
  src/schema.cpp
  src/sqlite_store.cpp
  src/replay.cpp
  src/snapshot.cpp
  src/stable_send.cpp
  src/outbox.cpp
  src/recovery.cpp
  src/service.cpp
  src/retention.cpp
  src/config.cpp
)
target_include_directories(jrnl PUBLIC include ${SQLITE3_INCLUDE_DIR})
target_link_libraries(jrnl PUBLIC ${SQLITE3_LIBRARY})
target_compile_options(jrnl PRIVATE -Wall -Wextra -Wpedantic)

if(JRNL_ENABLE_FAILPOINTS)
  target_compile_definitions(jrnl PUBLIC JRNL_ENABLE_FAILPOINTS)
endif()
if(JRNL_ASAN)
  target_compile_options(jrnl PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer -g)
  target_link_options(jrnl PUBLIC -fsanitize=address,undefined)
endif()
if(JRNL_TSAN)
  target_compile_options(jrnl PUBLIC -fsanitize=thread -fno-omit-frame-pointer -g)
  target_link_options(jrnl PUBLIC -fsanitize=thread)
endif()
if(JRNL_UBSAN_TRAP)
  target_compile_options(jrnl PUBLIC
    -fsanitize=undefined -fsanitize-undefined-trap-on-error
    -fno-omit-frame-pointer -g)
  target_compile_definitions(jrnl PUBLIC _GLIBCXX_ASSERTIONS JRNL_UBSAN_TRAP_BUILD)
endif()

if(NOT JRNL_BUILD_TOOLS)
  return()
endif()

# Shared test harness: the tiny runner plus the lab fixture and the
# independent LP oracle that section 23 insists the test oracle must not
# share code with the system under test.
add_library(jrnl_testing STATIC tests/support/tiny_test.cpp)
target_include_directories(jrnl_testing PUBLIC tests/support)
target_link_libraries(jrnl_testing PUBLIC jrnl)

enable_testing()

# One executable per suite. Each prints a verdict per matrix scenario and
# writes a JSON artifact that scripts/report.py turns into the release
# report in docs/P06_TEST_REPORT.md.
set(JRNL_SUITES
  unit/test_canonical_hash
  unit/test_journal_append
  unit/test_atomic_tx
  unit/test_inbox
  unit/test_outbox
  unit/test_command_idempotency
  unit/test_fix_session_recovery
  unit/test_exec_dedup
  unit/test_risk_reservation
  unit/test_cancel_replace
  unit/test_snapshot_replay
  unit/test_cross_stream
  unit/test_concurrency_load
  unit/test_fencing
  unit/test_storage_ha
  unit/test_backup_restore
  unit/test_schema_config
  unit/test_retention
  unit/test_security_ops
  crash/test_crash_windows
  scenario/test_chaos_e2e
  property/test_model_property
  playbook/test_playbooks
)

foreach(suite IN LISTS JRNL_SUITES)
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/${suite}.cpp)
    get_filename_component(name ${suite} NAME)
    add_executable(${name} tests/${suite}.cpp)
    target_link_libraries(${name} PRIVATE jrnl_testing)
    target_compile_definitions(${name} PRIVATE
      JRNL_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
      JRNL_CONFIG_DIR="${CMAKE_CURRENT_SOURCE_DIR}/configs")
    add_test(NAME ${name}
             COMMAND ${name} --artifact=${CMAKE_BINARY_DIR}/test-artifacts/${name}.json)
    set_tests_properties(${name} PROPERTIES TIMEOUT 900)
  endif()
endforeach()

foreach(app replay_verify recon_cli journal_bench)
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/apps/${app}/main.cpp)
    add_executable(${app} apps/${app}/main.cpp)
    target_link_libraries(${app} PRIVATE jrnl)
  endif()
endforeach()
