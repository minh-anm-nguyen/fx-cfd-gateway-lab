cmake_minimum_required(VERSION 3.20)
project(low_latency_execution_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# Build variants.
#
# ADR-08 of the design doc: a clean performance run, a profiling run and a
# sanitizer run are three separate builds. Combining them and then quoting one
# p99 confounds the result. Concretely that means two libraries from the same
# sources:
#
#   llx           lab build, failpoints and the SPSC ownership guard compiled
#                 in. Every test links this, because the 36 FI playbooks and 24
#                 crash windows are unreachable without the failpoints.
#   llx_measured  measurement build, no failpoints and no ownership guard, so
#                 the hot path carries no instrumentation branch. The benchmark
#                 app links this, and the manifest records which variant ran.
# ---------------------------------------------------------------------------
option(LLX_ASAN "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(LLX_TSAN "Build with ThreadSanitizer" OFF)
# Trapping UB checks need no sanitizer runtime, so they work on toolchains that
# ship no libubsan -- which is the case for mingw-w64. Less informative than
# UBSan proper (a trap instead of a report) but it still turns UB into a crash
# rather than silence. Paired with _GLIBCXX_ASSERTIONS for container checks.
option(LLX_UBSAN_TRAP "Build with trapping undefined behaviour checks" OFF)

# Project 08 links this engine into its lab as a library. Rebuilding the suites and
# CLIs inside that superbuild compiles every component a second time for no
# added evidence, so they are a top-level concern only. A standalone
# build is unchanged: PROJECT_IS_TOP_LEVEL is true there.
option(LLX_BUILD_TOOLS "Build the test suites and CLIs" ${PROJECT_IS_TOP_LEVEL})

if(LLX_ASAN AND LLX_TSAN)
  message(FATAL_ERROR "LLX_ASAN and LLX_TSAN cannot be enabled together: "
                      "design doc 23 layer 3 forbids linking both runtimes "
                      "into one binary")
endif()

set(LLX_SOURCES
  src/hash.cpp
  src/event.cpp
  src/clock.cpp
  src/failpoint.cpp
  src/histogram.cpp
  src/telemetry.cpp
  src/affinity.cpp
  src/domain.cpp
  src/store.cpp
  src/dispatch.cpp
  src/backpressure.cpp
  src/owner.cpp
  src/engine.cpp
  src/reference_pipeline.cpp
  src/config.cpp
  src/manifest.cpp
)

function(llx_add_library target)
  cmake_parse_arguments(ARG "FAILPOINTS;OWNERSHIP_GUARD" "" "" ${ARGN})
  add_library(${target} STATIC ${LLX_SOURCES})
  target_include_directories(${target} PUBLIC include)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion
                                           -Wsign-conversion -Wshadow)
  if(ARG_FAILPOINTS)
    target_compile_definitions(${target} PUBLIC LLX_ENABLE_FAILPOINTS)
  endif()
  if(NOT ARG_OWNERSHIP_GUARD)
    target_compile_definitions(${target} PUBLIC LLX_NO_OWNERSHIP_GUARD)
  endif()
  if(WIN32)
    # Affinity and topology discovery use the Win32 fallbacks documented in
    # affinity.cpp when the host is not Linux.
    target_link_libraries(${target} PUBLIC kernel32)
  else()
    find_package(Threads REQUIRED)
    target_link_libraries(${target} PUBLIC Threads::Threads)
  endif()
  if(LLX_ASAN)
    target_compile_options(${target} PUBLIC -fsanitize=address,undefined
                                            -fno-omit-frame-pointer -g)
    target_link_options(${target} PUBLIC -fsanitize=address,undefined)
  endif()
  if(LLX_TSAN)
    target_compile_options(${target} PUBLIC -fsanitize=thread
                                            -fno-omit-frame-pointer -g)
    target_link_options(${target} PUBLIC -fsanitize=thread)
  endif()
  if(LLX_UBSAN_TRAP)
    target_compile_options(${target} PUBLIC -fsanitize=undefined
                                            -fsanitize-undefined-trap-on-error
                                            -fno-omit-frame-pointer -g)
    target_compile_definitions(${target} PUBLIC _GLIBCXX_ASSERTIONS
                                                LLX_UBSAN_TRAP_BUILD)
  endif()
endfunction()

llx_add_library(llx FAILPOINTS OWNERSHIP_GUARD)
llx_add_library(llx_measured)

if(NOT LLX_BUILD_TOOLS)
  return()
endif()

# ---------------------------------------------------------------------------
# Test harness and suites.
# ---------------------------------------------------------------------------
add_library(llx_testing STATIC tests/support/tiny_test.cpp)
target_include_directories(llx_testing PUBLIC tests/support)
target_link_libraries(llx_testing PUBLIC llx)

# Global operator new/delete counters. Linked only into the suites that assert
# "no dynamic allocation in the measured region" (FR-03, LI-11); keeping it out
# of the other binaries means its bookkeeping cannot perturb them.
add_library(llx_alloc_probe STATIC tests/support/alloc_probe.cpp)
target_include_directories(llx_alloc_probe PUBLIC tests/support)

enable_testing()

# Each suite is one executable: it prints a verdict per catalog case and writes
# the JSON artifact scripts/report.py turns into the release-gate report.
set(LLX_SUITES
  unit/test_spsc_ring
  unit/test_pool
  unit/test_clock_hash
  unit/test_histogram_telemetry
  unit/test_affinity_config
  unit/test_backpressure
  unit/test_domain_store
  concurrency/test_spsc_stress
  concurrency/test_actor_fencing
  model/test_model_based
  differential/test_baseline_vs_optimized
  integration/test_pipeline
  fault/test_fault_playbooks
  crash/test_crash_windows
)

# The toolchain's runtime libraries have to be on PATH for a test to start at
# all on Windows, where libstdc++ and libwinpthread are DLLs next to the
# compiler rather than part of the system.
get_filename_component(_llx_cxx_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)

foreach(suite IN LISTS LLX_SUITES)
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/${suite}.cpp)
    get_filename_component(name ${suite} NAME)
    add_executable(${name} tests/${suite}.cpp)
    target_link_libraries(${name} PRIVATE llx_testing)
    target_compile_definitions(${name} PRIVATE
      LLX_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
    add_test(NAME ${name}
             COMMAND ${name} --artifact=${CMAKE_BINARY_DIR}/test-artifacts/${name}.json)
    set_tests_properties(${name} PROPERTIES TIMEOUT 900)
    if(WIN32)
      set_tests_properties(${name} PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_llx_cxx_dir}")
    endif()
  endif()
endforeach()

# Negative-compilation checks. LL-025 and LL-026 are enforced by static_assert,
# which no passing build can observe from the inside, so their evidence is a
# script that compiles the rejected cases and asserts the build fails. Doc 23
# forbids scoring a case that was never exercised, which is why those catalog
# entries are BLOCKED in the C++ suite and verified here instead.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_static_asserts.sh)
  find_program(LLX_BASH bash)
  if(LLX_BASH)
    add_test(NAME static_assert_negatives
             COMMAND ${LLX_BASH} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_static_asserts.sh)
    set_tests_properties(static_assert_negatives PROPERTIES
      TIMEOUT 600 ENVIRONMENT "CXX=${CMAKE_CXX_COMPILER}")
  else()
    message(STATUS "bash not found: the static_assert negative checks will not run")
  endif()
endif()

# Allocation accounting needs the probe linked in.
foreach(name test_pool test_pipeline)
  if(TARGET ${name})
    target_link_libraries(${name} PRIVATE llx_alloc_probe)
  endif()
endforeach()

# Micro-benchmark suite. Built against the measured library and NOT part of the
# correctness gates: doc 15.3 keeps measurement runs separate from sanitizer and
# failpoint builds, and a timing assertion in CI is a flaky test, not evidence.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/benchmark/micro_bench.cpp)
  add_executable(micro_bench tests/benchmark/micro_bench.cpp)
  target_link_libraries(micro_bench PRIVATE llx_measured)
  target_include_directories(micro_bench PRIVATE tests/support)
endif()

# ---------------------------------------------------------------------------
# Applications.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/apps/replay_benchmark/main.cpp)
  add_executable(replay_benchmark apps/replay_benchmark/main.cpp)
  target_link_libraries(replay_benchmark PRIVATE llx_measured)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/apps/latency_fault_harness/main.cpp)
  add_executable(latency_fault_harness apps/latency_fault_harness/main.cpp)
  target_link_libraries(latency_fault_harness PRIVATE llx)
endif()
