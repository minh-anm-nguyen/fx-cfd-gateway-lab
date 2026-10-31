cmake_minimum_required(VERSION 3.20)
project(fix_session_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

# Fault injection points are what make the crash-window tests possible. They are
# compiled in for lab builds and must be OFF for anything resembling production
# (design doc 17.3).
option(FIXSE_ENABLE_FAILPOINTS "Compile fault injection points" ON)
option(FIXSE_ASAN "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(FIXSE_TSAN "Build with ThreadSanitizer" OFF)
# Undefined behaviour checks that need no sanitizer runtime library: UB traps
# instead of producing a report. Less informative than UBSan proper, but it
# works on toolchains that ship no libubsan, which is the case for mingw-w64.
# Paired with _GLIBCXX_ASSERTIONS so container preconditions are checked too.
option(FIXSE_UBSAN_TRAP "Build with trapping undefined behaviour checks" OFF)

# Project 08 links this engine into its gateway as a library. Building the
# suites, the fuzzer and the CLIs again inside that superbuild costs a full
# second compile of every component for no evidence, so they are a top-level
# concern only. A standalone build is unchanged: PROJECT_IS_TOP_LEVEL is true.
option(FIXSE_BUILD_TOOLS "Build the test suites, fuzz driver and CLIs" ${PROJECT_IS_TOP_LEVEL})

if(FIXSE_ASAN AND FIXSE_TSAN)
  # Design doc 20 of project 03 is explicit: never link ASan and TSan into the
  # same binary.
  message(FATAL_ERROR "FIXSE_ASAN and FIXSE_TSAN cannot be enabled together")
endif()

# Look next to the compiler as well: on an MSYS2 or MinGW toolchain the SDK
# lives in the same prefix and is not on CMake's default search path.
get_filename_component(_fixse_cxx_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
get_filename_component(_fixse_prefix "${_fixse_cxx_dir}/.." ABSOLUTE)
find_path(SQLITE3_INCLUDE_DIR sqlite3.h HINTS "${_fixse_prefix}/include")
find_library(SQLITE3_LIBRARY NAMES sqlite3 HINTS "${_fixse_prefix}/lib")
if(NOT SQLITE3_INCLUDE_DIR OR NOT SQLITE3_LIBRARY)
  message(FATAL_ERROR "sqlite3 development files not found")
endif()

add_library(fixse STATIC
  src/wire.cpp
  src/result.cpp
  src/clock.cpp
  src/config.cpp
  src/framer.cpp
  src/codec.cpp
  src/failpoint.cpp
  src/ownership.cpp
  src/sqlite_store.cpp
  src/resend_planner.cpp
  src/metrics.cpp
  src/audit.cpp
  src/session_actor.cpp
  src/tcp_connector.cpp
)
target_include_directories(fixse PUBLIC include ${SQLITE3_INCLUDE_DIR})
target_link_libraries(fixse PUBLIC ${SQLITE3_LIBRARY})
target_compile_options(fixse PRIVATE -Wall -Wextra -Wpedantic)
if(WIN32)
  target_link_libraries(fixse PUBLIC ws2_32)
endif()
if(FIXSE_ENABLE_FAILPOINTS)
  target_compile_definitions(fixse PUBLIC FIXSE_ENABLE_FAILPOINTS)
endif()
if(FIXSE_ASAN)
  target_compile_options(fixse PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer -g)
  target_link_options(fixse PUBLIC -fsanitize=address,undefined)
endif()
if(FIXSE_TSAN)
  target_compile_options(fixse PUBLIC -fsanitize=thread -fno-omit-frame-pointer -g)
  target_link_options(fixse PUBLIC -fsanitize=thread)
endif()
if(FIXSE_UBSAN_TRAP)
  target_compile_options(fixse PUBLIC
    -fsanitize=undefined -fsanitize-undefined-trap-on-error
    -fno-omit-frame-pointer -g)
  target_compile_definitions(fixse PUBLIC _GLIBCXX_ASSERTIONS FIXSE_UBSAN_TRAP_BUILD)
endif()

if(NOT FIXSE_BUILD_TOOLS)
  return()
endif()

# Shared test harness.
add_library(fixse_testing STATIC tests/support/tiny_test.cpp)
target_include_directories(fixse_testing PUBLIC tests/support)
target_link_libraries(fixse_testing PUBLIC fixse)

enable_testing()

# Each suite is one executable. It prints a verdict per matrix scenario and
# writes a JSON artifact that scripts/report.py turns into the release report.
set(FIXSE_SUITES
  unit/test_framing_codec
  unit/test_golden_fixtures
  unit/test_store
  unit/test_resend_planner
  protocol/test_logon_identity
  protocol/test_heartbeat_timers
  protocol/test_sequence_gap
  protocol/test_resend_gapfill
  protocol/test_network_lifecycle
  protocol/test_backpressure
  protocol/test_security
  protocol/test_concurrency_lifecycle
  protocol/test_interop_boundary
  scenario/test_e2e_loopback
  property/test_model_property
  crash/test_crash_windows
)

foreach(suite IN LISTS FIXSE_SUITES)
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/${suite}.cpp)
    get_filename_component(name ${suite} NAME)
    add_executable(${name} tests/${suite}.cpp)
    target_link_libraries(${name} PRIVATE fixse_testing)
    # Golden fixtures are read from the source tree, not the build tree.
    target_compile_definitions(${name} PRIVATE
      FIXSE_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
    add_test(NAME ${name}
             COMMAND ${name} --artifact=${CMAKE_BINARY_DIR}/test-artifacts/${name}.json)
    set_tests_properties(${name} PROPERTIES TIMEOUT 600)
  endif()
endforeach()

# Fuzz driver: libFuzzer when the toolchain has it, otherwise a deterministic
# standalone driver so the corpus still runs in CI.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/fuzz_parse_frame.cpp)
  add_executable(fuzz_parse_frame tests/fuzz/fuzz_parse_frame.cpp)
  target_link_libraries(fuzz_parse_frame PRIVATE fixse_testing)
  add_test(NAME fuzz_parse_frame
           COMMAND fuzz_parse_frame --iterations=200000
                   --artifact=${CMAKE_BINARY_DIR}/test-artifacts/fuzz_parse_frame.json)
  set_tests_properties(fuzz_parse_frame PROPERTIES TIMEOUT 900)
endif()

foreach(app fix_client fix_test_acceptor fix_admin_cli)
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/apps/${app}/main.cpp)
    add_executable(${app} apps/${app}/main.cpp)
    target_link_libraries(${app} PRIVATE fixse)
  endif()
endforeach()
