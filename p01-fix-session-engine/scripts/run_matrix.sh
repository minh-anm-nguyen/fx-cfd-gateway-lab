#!/usr/bin/env bash
# Run the what-if matrix and produce the test report.
#
# Usage: scripts/run_matrix.sh [--priority P0] [--build-dir build] [--jobs N]
#
# The release condition in design doc section 18 is 100 percent of P0 passing,
# so this script exits non-zero when any P0 scenario fails.
set -euo pipefail

PRIORITY=""
BUILD_DIR="build"
JOBS="$(nproc 2>/dev/null || echo 4)"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --priority) PRIORITY="$2"; shift 2 ;;
    --build-dir) BUILD_DIR="$2"; shift 2 ;;
    --jobs) JOBS="$2"; shift 2 ;;
    -h|--help) sed -n '2,8p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 1 ;;
  esac
done

cmake -S . -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build "$BUILD_DIR" -j "$JOBS"
ctest --test-dir "$BUILD_DIR" --output-on-failure

ARGS=(--artifacts "$BUILD_DIR/test-artifacts" --out docs/P01_TEST_REPORT.md)
if [[ -n "$PRIORITY" ]]; then ARGS+=(--priority "$PRIORITY"); fi
python scripts/report.py "${ARGS[@]}"
