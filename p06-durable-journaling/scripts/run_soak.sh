#!/usr/bin/env bash
# Drive the chaos and playbook suites in a loop for a wall-clock duration.
#
#   ./scripts/run_soak.sh --hours 48 --seed 20261030
#
# G24-07 in the chaos suite is reported BLOCKED precisely because a soak
# belongs here rather than in a per-commit verdict. The result of this
# script is a run report, not a test verdict.
set -euo pipefail
cd "$(dirname "$0")/.."

HOURS=0.05
SEED=20261030
while [[ $# -gt 0 ]]; do
  case "$1" in
    --hours) HOURS="$2"; shift 2 ;;
    --seed) SEED="$2"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

BUILD="${BUILD:-build}"
cmake --build "$BUILD"
export PATH="$(dirname "$(command -v g++)"):$PATH"

END=$(( $(date +%s) + $(python -c "print(int(float('$HOURS')*3600))") ))
round=0
failures=0
while [[ $(date +%s) -lt $END ]]; do
  round=$((round + 1))
  for suite in test_chaos_e2e test_playbooks test_model_property; do
    if ! "$BUILD/$suite.exe" \
          --artifact="$BUILD/test-artifacts/soak-$suite-$round.json" >/dev/null; then
      echo "round $round: $suite FAILED"
      failures=$((failures + 1))
    fi
  done
done
echo "soak: seed $SEED, $round rounds, $failures failures"
exit $(( failures == 0 ? 0 : 1 ))
