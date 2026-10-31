#!/usr/bin/env bash
# Negative-compilation checks.
#
# LL-025 and LL-026 of the catalog require a non-power-of-two capacity, N=0 and
# N=1 to be refused. They are refused by static_assert, which is a hard error
# and therefore invisible from inside a passing build: no C++ test can observe
# it. Doc 23 forbids reporting a pass for a case that was never exercised, so
# those entries are BLOCKED in the suite and this script is their evidence.
#
# Each case below must FAIL to compile. The script fails if any of them builds.
set -u

CXX="${CXX:-g++}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

pass=0
fail=0

expect_failure() {
  local id="$1" description="$2" body="$3"
  cat > "$TMP/case.cpp" <<EOF
#include <cstdint>
#include "llx/spsc_ring.hpp"
$body
int main() { return 0; }
EOF
  if "$CXX" -std=c++20 -I"$ROOT/include" -fsyntax-only "$TMP/case.cpp" 2>"$TMP/err.txt"; then
    echo "[FAIL]    $id  $description"
    echo "          it compiled, but the constraint should have rejected it"
    fail=$((fail + 1))
  else
    # The failure has to come from our static_assert, not from an unrelated
    # mistake in the test case itself.
    if grep -q "static assertion failed" "$TMP/err.txt"; then
      echo "[PASS]    $id  $description"
      pass=$((pass + 1))
    else
      echo "[FAIL]    $id  $description"
      echo "          it failed to compile, but not on a static_assert:"
      sed 's/^/          /' "$TMP/err.txt" | head -5
      fail=$((fail + 1))
    fi
  fi
}

expect_compilation() {
  local id="$1" description="$2" body="$3"
  cat > "$TMP/case.cpp" <<EOF
#include <cstdint>
#include "llx/spsc_ring.hpp"
$body
int main() { return 0; }
EOF
  if "$CXX" -std=c++20 -I"$ROOT/include" -fsyntax-only "$TMP/case.cpp" 2>"$TMP/err.txt"; then
    echo "[PASS]    $id  $description"
    pass=$((pass + 1))
  else
    echo "[FAIL]    $id  $description"
    sed 's/^/          /' "$TMP/err.txt" | head -5
    fail=$((fail + 1))
  fi
}

echo "negative-compilation checks (LL-025, LL-026 and the slot-type contract)"
echo

expect_failure "LL-025a" "capacity 6 is not a power of two" \
  'llx::SpscRing<std::uint64_t, 6> r;'
expect_failure "LL-025b" "capacity 1000 is not a power of two" \
  'llx::SpscRing<std::uint64_t, 1000> r;'
expect_failure "LL-026a" "capacity 0 is refused" \
  'llx::SpscRing<std::uint64_t, 0> r;'
expect_failure "LL-026b" "capacity 1 is refused" \
  'llx::SpscRing<std::uint64_t, 1> r;'
expect_failure "LL-027c" "capacity beyond half the counter range is refused" \
  'llx::SpscRing<std::uint64_t, 256, std::uint8_t> r;'
expect_failure "LL-038b" "a non-trivially-copyable slot type is refused" \
  'struct WithDtor { ~WithDtor() {} };
   llx::SpscRing<WithDtor, 4> r;'
expect_failure "LL-038c" "a slot type holding a std::string is refused" \
  '#include <string>
   llx::SpscRing<std::string, 4> r;'

# And the valid cases must still build, or the checks above would pass for the
# wrong reason.
expect_compilation "LL-025c" "capacity 2 is accepted" \
  'llx::SpscRing<std::uint64_t, 2> r;'
expect_compilation "LL-025d" "capacity 8192 is accepted" \
  'llx::SpscRing<std::uint64_t, 8192> r;'
expect_compilation "LL-027d" "an 8-bit counter with capacity 4 is accepted" \
  'llx::SpscRing<std::uint64_t, 4, std::uint8_t> r;'

echo
echo "static_assert_negatives: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
