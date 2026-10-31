#!/usr/bin/env bash
# Run the what-if matrix, optionally filtered by priority.
#
#   ./scripts/run_matrix.sh --priority P0 --seed 20261030
#
# The seed is accepted and recorded for reproducibility; the suites that
# generate sequences use fixed seeds of their own, so this is the value a
# run report should quote rather than an input that changes behaviour.
set -euo pipefail
cd "$(dirname "$0")/.."

PRIORITY=""
SEED="20261030"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --priority) PRIORITY="$2"; shift 2 ;;
    --seed) SEED="$2"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

BUILD="${BUILD:-build}"
cmake -S . -B "$BUILD" -G Ninja >/dev/null
cmake --build "$BUILD"
export PATH="$(dirname "$(command -v g++)"):$PATH"

echo "matrix run, seed ${SEED}, priority ${PRIORITY:-all}"
ctest --test-dir "$BUILD" --output-on-failure
python scripts/report.py --artifacts "$BUILD/test-artifacts" --out docs/P06_TEST_REPORT.md

if [[ -n "$PRIORITY" ]]; then
  echo "scenarios at ${PRIORITY}:"
  grep -E "^\| [A-Z0-9-]+ \| ${PRIORITY} \|" docs/P06_TEST_REPORT.md | wc -l
fi
