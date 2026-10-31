#!/usr/bin/env bash
# Sanitizer builds for release gate A, which requires zero sanitizer reports.
#
# Usage: scripts/run_sanitizers.sh [--asan] [--tsan]
#
# ASan and TSan are never linked into the same binary, which the design doc of
# Project 03 calls out explicitly. Each gets its own build directory, and a
# sanitizer build is never presented as a latency measurement.
set -euo pipefail

RUN_ASAN=0
RUN_TSAN=0
RUN_TRAP=0
JOBS="$(nproc 2>/dev/null || echo 4)"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --asan) RUN_ASAN=1; shift ;;
    --tsan) RUN_TSAN=1; shift ;;
    --ubsan-trap) RUN_TRAP=1; shift ;;
    --jobs) JOBS="$2"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 1 ;;
  esac
done
if [[ $RUN_ASAN -eq 0 && $RUN_TSAN -eq 0 && $RUN_TRAP -eq 0 ]]; then RUN_ASAN=1; fi

if [[ $RUN_TRAP -eq 1 ]]; then
  # For toolchains with no sanitizer runtime: undefined behaviour traps instead
  # of reporting. A trap shows up as an abnormal test exit.
  echo "== trapping undefined behaviour checks =="
  cmake -S . -B build-ubtrap -G Ninja -DCMAKE_BUILD_TYPE=Debug -DFIXSE_UBSAN_TRAP=ON
  cmake --build build-ubtrap -j "$JOBS"
  ctest --test-dir build-ubtrap --output-on-failure
fi

if [[ $RUN_ASAN -eq 1 ]]; then
  echo "== AddressSanitizer and UndefinedBehaviorSanitizer =="
  cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DFIXSE_ASAN=ON
  cmake --build build-asan -j "$JOBS"
  ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 \
    UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
    ctest --test-dir build-asan --output-on-failure
fi

if [[ $RUN_TSAN -eq 1 ]]; then
  echo "== ThreadSanitizer =="
  cmake -S . -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DFIXSE_TSAN=ON
  cmake --build build-tsan -j "$JOBS"
  TSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-tsan --output-on-failure
fi
