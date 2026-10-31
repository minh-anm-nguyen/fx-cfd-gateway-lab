#!/usr/bin/env bash
# Run every suite, then assemble the matrix report.
#
# The report is the artifact that matters (design doc 18.4, 19): it is what
# decides the gate, and it counts BLOCKED and NOT_IMPLEMENTED separately from
# PASS so that a skipped case cannot be read as a passing one.
#
# usage: scripts/run_tests.sh [--build] [--csv FILE] [suite ...]
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BUILD=0
CSV=""
while [ $# -gt 0 ]; do
  case "$1" in
    --build) BUILD=1; shift ;;
    --csv) CSV="$2"; shift 2 ;;
    *) break ;;
  esac
done

if [ $BUILD -eq 1 ]; then
  ./scripts/build.sh all || exit 1
fi

SUITES="${*:-test_wire test_quote test_reports test_scheduler test_store \
test_business test_session test_model test_recovery test_playbooks test_crash}"

# Old artifacts would make a suite that no longer builds look like it passed.
rm -f test-artifacts/*.json

rc=0
for s in $SUITES; do
  if [ ! -x "build/$s.exe" ] && [ ! -x "build/$s" ]; then
    echo "MISSING  $s (run with --build)"
    rc=1
    continue
  fi
  printf '== %s\n' "$s"
  if [ -x "build/$s.exe" ]; then "./build/$s.exe" --artifacts test-artifacts; else "./build/$s" --artifacts test-artifacts; fi
  [ $? -eq 0 ] || rc=1
done

PY=python
command -v python >/dev/null 2>&1 || PY=python3

# The live socket checks, including A-08, which needs two real sockets.
if [ -x build/lp_simulator.exe ] || [ -x build/lp_simulator ]; then
  EXE=build/lp_simulator.exe
  [ -x "$EXE" ] || EXE=build/lp_simulator
  printf '== %s
' "test_smoke"
  $PY scripts/smoke_tcp.py --exe "$EXE" --artifacts test-artifacts --port 19898     || rc=1
else
  echo "MISSING  lp_simulator (run with --build); A-08 will not run"
  rc=1
fi

echo
if [ -n "$CSV" ]; then
  $PY scripts/report.py --artifacts test-artifacts --csv "$CSV" || rc=1
else
  $PY scripts/report.py --artifacts test-artifacts || rc=1
fi

exit $rc
