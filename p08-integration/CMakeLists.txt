cmake_minimum_required(VERSION 3.20)
project(fx_integration_lab LANGUAGES CXX)

# Project 08 is the only place P01-P07 exist as one system. Sections 1.1 and
# 22 of the design doc describe the result as a lab rather than a product: two
# processes facing each other across a FIX session, plus a control plane that
# drives and observes them.
#
# The seven components are consumed where they live. They are not copied in and
# not vendored, so a fix in p03-order-state-machine is a fix here, and the
# version pinning section 2 demands is a property of one checkout rather than
# of a sync step somebody forgets to run.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

set(FX_COMPONENT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." CACHE PATH
    "Directory holding p01-* .. p07-*")

# P05 statically links the C++ runtime by default, which is right for a
# standalone MinGW binary shipped on its own and wrong here: the flag is PUBLIC,
# so it would propagate into every target in the lab and decide the link mode
# for six components that never asked. The lab picks its own.
set(RISK_STATIC_RUNTIME OFF CACHE BOOL "" FORCE)

# Each component's CMakeLists returns early when its *_BUILD_TOOLS option is
# off, which is the default for a non-top-level build. That keeps seven test
# suites and a dozen CLIs out of this configure step, and it is also what
# resolves the one hard target-name collision between the seven: P03 and P05
# both define a target called `tiny_test` for their own suites.
foreach(component
    p01-fix-session-engine
    p02-lp-simulator
    p03-order-state-machine
    p04-order-routing-reconciliation
    p05-pretrade-risk-engine
    p06-durable-journaling
    p07-low-latency-execution-engine)
  if(NOT EXISTS "${FX_COMPONENT_ROOT}/${component}/CMakeLists.txt")
    message(FATAL_ERROR
      "component ${component} not found under ${FX_COMPONENT_ROOT}. "
      "Section 2 of the design doc: a missing component is BLOCKED, not a "
      "partial lab.")
  endif()
  add_subdirectory("${FX_COMPONENT_ROOT}/${component}" "components/${component}")
endforeach()

# The contracts that only exist once the seven are in one binary: the evidence
# ladder of section 2 and the compatibility matrix that section closes with.
add_library(gwcontract STATIC
  src/contract.cpp
  src/evidence.cpp
  src/lab_guard.cpp
  src/execution_pipeline.cpp
  src/fix_transport.cpp
  src/order_reducer.cpp
  src/risk_adapter.cpp
  src/session_adapter.cpp
  # Oracle A (§11.1): not a reducer of its own, but the thing that makes P04's
  # independent reducer and P03's answerable to the same question.
  harness/reference_model/order_oracle.cpp
)
target_include_directories(gwcontract PUBLIC include harness/reference_model)
target_link_libraries(gwcontract PUBLIC
  fixse
  lpsim
  order_core
  order_router
  risk_engine
  jrnl
  llx
)

enable_testing()

# On Windows the toolchain's libstdc++ and libwinpthread are DLLs that sit next
# to the compiler rather than in the system directory, so a test binary will not
# even start without them on PATH. P01, P06 and P07 each do this for their own
# suites; the lab has to do it for its own.
get_filename_component(_gw_cxx_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
function(gw_add_test name)
  add_test(NAME ${name} COMMAND ${ARGN})
  # Section 19: a lab refuses to start unless TRADING_MODE says it is a
  # simulation. Declaring it per test rather than exporting it once is the
  # point -- an operator shell that has not opted in gets the refusal.
  set(_gw_env "TRADING_MODE=set:SIM_ONLY")
  if(WIN32)
    list(APPEND _gw_env "PATH=path_list_prepend:${_gw_cxx_dir}")
  endif()
  set_tests_properties(${name} PROPERTIES ENVIRONMENT_MODIFICATION "${_gw_env}")
endfunction()

# M1's gate. It runs before the lab starts and before any container is built,
# because a mismatch is BLOCKED and there is nothing to observe.
add_executable(contract_probe apps/contract_probe/main.cpp)
target_link_libraries(contract_probe PRIVATE gwcontract)
gw_add_test(contract_gate contract_probe)
set_tests_properties(contract_gate PROPERTIES TIMEOUT 60)

# Seam tests. Each one drives two components against each other rather than
# against either one's own fake, which is the only place the translations
# between them are actually exercised.
foreach(seam risk session)
  add_executable(test_${seam}_seam tests/test_${seam}_seam.cpp)
  target_link_libraries(test_${seam}_seam PRIVATE gwcontract)
  gw_add_test(${seam}_seam test_${seam}_seam)
  set_tests_properties(${seam}_seam PROPERTIES TIMEOUT 120)
endforeach()

# The assembled gateway: one order through P04 admission, real P05 risk and real
# P06 durable send, with P04's in-tree LP and reducer still standing in for P02
# and P03.
add_executable(gateway apps/gateway/main.cpp)
target_link_libraries(gateway PRIVATE gwcontract)
gw_add_test(gateway_smoke gateway)
set_tests_properties(gateway_smoke PROPERTIES TIMEOUT 120)

# The same gateway with P04's in-tree LP removed: P01's real session over TCP
# to P02 as a separate process. Not a ctest entry, because it needs that second
# process listening; scripts/interop.sh drives both.
add_executable(interop_gateway apps/interop_gateway/main.cpp)
target_link_libraries(interop_gateway PRIVATE gwcontract)

# Oracle A against the owner. Not a seam: both reducers already exist, and this
# is the differential that makes them answerable to the same event stream.
add_executable(test_order_oracle tests/test_order_oracle.cpp)
target_link_libraries(test_order_oracle PRIVATE gwcontract)
gw_add_test(order_oracle test_order_oracle)
set_tests_properties(order_oracle PROPERTIES TIMEOUT 120)

# P03 on the write path: order_core owns status and the fill ledger, and the
# unauthorised fill P04's reducer credited is now refused.
add_executable(test_order_write_path tests/test_order_write_path.cpp)
target_link_libraries(test_order_write_path PRIVATE gwcontract)
gw_add_test(order_write_path test_order_write_path)
set_tests_properties(order_write_path PROPERTIES TIMEOUT 120)

# The P04 <-> P01 frame bridge. Two strict parsers that disagree, so the
# assertion is that P04's own parser accepts what the bridge produces.
add_executable(test_fix_transport tests/test_fix_transport.cpp)
target_link_libraries(test_fix_transport PRIVATE gwcontract)
gw_add_test(fix_transport test_fix_transport)
set_tests_properties(fix_transport PROPERTIES TIMEOUT 60)

# P07's ring and pool carrying the gateway's work on two threads. The
# invariants are the subject, not a particular interleaving.
add_executable(test_execution_pipeline tests/test_execution_pipeline.cpp)
target_link_libraries(test_execution_pipeline PRIVATE gwcontract)
gw_add_test(execution_pipeline test_execution_pipeline)
set_tests_properties(execution_pipeline PROPERTIES TIMEOUT 180)

# EI-22 across components. P05's write-set is atomic and its own suite proves
# it; this asks whether P04 and P06 agree once a risk commit rolls back.
add_executable(test_admission_atomicity tests/test_admission_atomicity.cpp)
target_link_libraries(test_admission_atomicity PRIVATE gwcontract)
gw_add_test(admission_atomicity test_admission_atomicity)
set_tests_properties(admission_atomicity PROPERTIES TIMEOUT 180)

# EI-22 is not held: there are three durable files and no transaction spans
# them. This measures what the gap can produce rather than asserting it away.
add_executable(test_two_store_gap tests/test_two_store_gap.cpp)
target_link_libraries(test_two_store_gap PRIVATE gwcontract)
gw_add_test(two_store_gap test_two_store_gap)
set_tests_properties(two_store_gap PROPERTIES TIMEOUT 120)

# The guard that keeps the lab off a real venue, tested on its own because its
# failure mode is silence: a guard that accepts everything looks exactly like a
# guard that is working.
add_executable(test_lab_guard tests/test_lab_guard.cpp)
target_link_libraries(test_lab_guard PRIVATE gwcontract)
gw_add_test(lab_guard test_lab_guard)
set_tests_properties(lab_guard PROPERTIES TIMEOUT 60)
