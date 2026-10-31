cmake_minimum_required(VERSION 3.20)
project(order_core LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ORDER_CORE_ENABLE_TESTS "Build the what-if scenario suite" ${PROJECT_IS_TOP_LEVEL})
option(ORDER_CORE_SANITIZE_ADDRESS "Build with AddressSanitizer and UBSan" OFF)
option(ORDER_CORE_SANITIZE_THREAD "Build with ThreadSanitizer" OFF)

# ASan and TSan must not share a binary; the design doc calls this out in 20.5.
if(ORDER_CORE_SANITIZE_ADDRESS AND ORDER_CORE_SANITIZE_THREAD)
  message(FATAL_ERROR "Enable ASan or TSan, never both in one binary")
endif()

add_library(order_core
  src/math.cpp
  src/decimal.cpp
  src/policy.cpp
  src/events.cpp
  src/aggregate.cpp
  src/invariants.cpp
  src/reducer.cpp
  src/store.cpp
  src/engine.cpp
)
target_include_directories(order_core PUBLIC include)
target_compile_options(order_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)

if(ORDER_CORE_SANITIZE_ADDRESS)
  target_compile_options(order_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(order_core PUBLIC -fsanitize=address,undefined)
endif()
if(ORDER_CORE_SANITIZE_THREAD)
  target_compile_options(order_core PUBLIC -fsanitize=thread -fno-omit-frame-pointer)
  target_link_options(order_core PUBLIC -fsanitize=thread)
endif()

if(ORDER_CORE_ENABLE_TESTS)
  enable_testing()

  add_library(tiny_test tests/support/tiny_test.cpp)
  target_include_directories(tiny_test PUBLIC tests/support)

  file(GLOB ORDER_CORE_SUITE
    tests/unit/*.cpp
    tests/property/*.cpp
    tests/model/*.cpp
    tests/integration/*.cpp
    tests/failure/*.cpp
  )
  add_executable(order_core_tests tests/test_main.cpp ${ORDER_CORE_SUITE})
  target_link_libraries(order_core_tests PRIVATE order_core tiny_test)
  target_include_directories(order_core_tests PRIVATE tests)

  # One ctest entry per catalog group so a failing group is named in CI output.
  foreach(group A B C D E F G H I J K L M N O P Q PB PROP MODEL)
    add_test(NAME group_${group}
             COMMAND order_core_tests --filter=${group}-
                     --artifact=${CMAKE_BINARY_DIR}/test-artifacts/group_${group}.json)
  endforeach()

  add_executable(order_trace_replay tools/order_trace_replay/main.cpp)
  target_link_libraries(order_trace_replay PRIVATE order_core)
  target_include_directories(order_trace_replay PRIVATE tests)

  add_executable(order_model_check tools/order_model_check/main.cpp)
  target_link_libraries(order_model_check PRIVATE order_core)
  target_include_directories(order_model_check PRIVATE tests)

  add_executable(order_lab_cli apps/order_lab_cli/main.cpp)
  target_link_libraries(order_lab_cli PRIVATE order_core)

  add_test(NAME model_check COMMAND order_model_check --max-orders 2 --max-depth 8)
endif()
