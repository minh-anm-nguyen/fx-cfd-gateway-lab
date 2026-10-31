#!/usr/bin/env bash
# Builds and runs the P05 suite.
#
# Usage:
#   scripts/run-tests.sh            # everything, including the Python oracle
#   scripts/run-tests.sh unit       # only cases whose "suite.name" contains "unit"
#
# The binaries link the C++ runtime statically, so no PATH setup is needed and
# the crash harness can launch its child process from any shell.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="${BUILD_DIR:-$here/build}"
filter="${1:-}"

cmake -S "$here" -B "$build" -G Ninja >/dev/null
cmake --build "$build"

mkdir -p "$here/test-artifacts"
export RISK_CRASH_CHILD="$build/risk_crash_child.exe"
export RISK_MODEL_ARTIFACT="$here/test-artifacts/model_traces.json"

cd "$here"
if [[ -n "$filter" ]]; then
  "$build/risk_tests.exe" "--filter=$filter"
  exit $?
fi

"$build/risk_tests.exe" --report="$here/test-artifacts/report.json"

echo
echo "=== differential oracle (independent Python implementation) ==="
python -I "$here/tests/oracle/reference_model.py" "$RISK_MODEL_ARTIFACT"

echo
echo "=== deterministic scenarios ==="
if compgen -G "$here/fixtures/*.scenario" >/dev/null; then
  "$build/risk_scenario.exe" --quiet "$here"/fixtures/*.scenario
else
  echo "no scenario fixtures found"
fi
