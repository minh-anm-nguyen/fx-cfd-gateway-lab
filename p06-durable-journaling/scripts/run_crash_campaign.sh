#!/usr/bin/env bash
# Repeat the crash-window suite. Each iteration relaunches a child process
# per window and kills it inside the window, so a campaign is the thing
# that gives the windows statistical weight rather than one run.
#
#   ./scripts/run_crash_campaign.sh --iterations 200
#
# Section 27 names 10,000 iterations per release gate. That is a scheduled
# campaign measured in hours, not something to run from a laptop by
# default, so the default here is small and the number is a flag.
set -euo pipefail
cd "$(dirname "$0")/.."

ITERATIONS=20
while [[ $# -gt 0 ]]; do
  case "$1" in
    --iterations) ITERATIONS="$2"; shift 2 ;;
    --windows) shift 2 ;;   # accepted for compatibility with the doc's example
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

BUILD="${BUILD:-build}"
cmake --build "$BUILD"
export PATH="$(dirname "$(command -v g++)"):$PATH"

failures=0
for ((i = 1; i <= ITERATIONS; i++)); do
  if ! "$BUILD/test_crash_windows.exe" \
        --artifact="$BUILD/test-artifacts/crash-campaign-$i.json" >/dev/null; then
    echo "iteration $i FAILED"
    failures=$((failures + 1))
  fi
done
echo "crash campaign: $ITERATIONS iterations, $failures failed"
exit $(( failures == 0 ? 0 : 1 ))
