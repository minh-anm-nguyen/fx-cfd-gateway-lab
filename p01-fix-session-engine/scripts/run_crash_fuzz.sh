#!/usr/bin/env bash
# Crash windows plus a long fuzz run.
#
# Usage: scripts/run_crash_fuzz.sh [--iterations 1000000] [--seed 20261030]
#
# The crash suite spawns child processes and terminates them at exact durability
# boundaries. The fuzz driver runs the corpus and the mutation loop. Neither
# proves hardware durability: a real power cut (G-09) is out of lab scope.
set -euo pipefail

ITERATIONS=1000000
SEED=20261030
BUILD_DIR="build"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --iterations) ITERATIONS="$2"; shift 2 ;;
    --seed) SEED="$2"; shift 2 ;;
    --build-dir) BUILD_DIR="$2"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 1 ;;
  esac
done

cmake --build "$BUILD_DIR" -j "$(nproc 2>/dev/null || echo 4)" \
  --target test_crash_windows fuzz_parse_frame

"$BUILD_DIR/test_crash_windows" \
  --artifact="$BUILD_DIR/test-artifacts/crash_windows.json"
"$BUILD_DIR/fuzz_parse_frame" --iterations="$ITERATIONS" --seed="$SEED" \
  --artifact="$BUILD_DIR/test-artifacts/fuzz_parse_frame.json"
