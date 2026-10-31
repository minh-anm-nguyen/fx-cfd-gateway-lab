cmake_minimum_required(VERSION 3.20)
project(p04_order_routing_reconciliation CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ENABLE_ASAN "Build with the address and undefined behaviour sanitizers" OFF)

# Project 08 links this router into its lab as a library. Rebuilding the suites and
# CLIs inside that superbuild compiles every component a second time for no
# added evidence, so they are a top-level concern only. A standalone
# build is unchanged: PROJECT_IS_TOP_LEVEL is true there.
option(ROUTER_BUILD_TOOLS "Build the test suites and CLIs" ${PROJECT_IS_TOP_LEVEL})

# Warnings are errors. Design doc 22.4 makes that a PR gate, so the build must
# not be able to pass with one.
if(MSVC)
  add_compile_options(/W4 /WX /permissive-)
else()
  add_compile_options(-Wall -Wextra -Werror)
endif()

if(ENABLE_ASAN AND NOT MSVC)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
elseif(MINGW)
  # Statically link the toolchain runtime, as the Makefile does. Without this the
  # binaries need libstdc++ and libgcc on PATH, and ctest fails every entry with
  # STATUS_ENTRYPOINT_NOT_FOUND rather than running anything.
  add_link_options(-static)
endif()

file(GLOB ROUTER_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)

add_library(order_router STATIC ${ROUTER_SOURCES})
target_include_directories(order_router PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

file(GLOB_RECURSE TEST_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/support/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/matrix/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/playbooks/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/property/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/main.cpp)

if(NOT ROUTER_BUILD_TOOLS)
  return()
endif()

add_executable(router_test ${TEST_SOURCES})
target_include_directories(router_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(router_test PRIVATE order_router)

add_executable(order_router_cli ${CMAKE_CURRENT_SOURCE_DIR}/apps/router_cli/main.cpp)
target_link_libraries(order_router_cli PRIVATE order_router)

add_executable(recon_operator_cli
  ${CMAKE_CURRENT_SOURCE_DIR}/apps/recon_operator_cli/main.cpp)
target_link_libraries(recon_operator_cli PRIVATE order_router)

enable_testing()

# One ctest entry per matrix group, so a failing group is named in the report
# rather than hidden inside one giant test.
foreach(group
    unit matrix-A matrix-B matrix-C matrix-D matrix-E matrix-F matrix-G matrix-H
    matrix-I matrix-J matrix-K matrix-L matrix-M matrix-N matrix-O matrix-P
    matrix-Q matrix-R playbook crash-window property load)
  add_test(NAME ${group}
           COMMAND router_test --filter=${group}
                   --artifact=${CMAKE_CURRENT_SOURCE_DIR}/test-artifacts/${group}.json)
  # The fixture cases read fixtures/fix44 relative to the working directory, so
  # every entry runs from the project root.
  set_tests_properties(${group} PROPERTIES
                       WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endforeach()
