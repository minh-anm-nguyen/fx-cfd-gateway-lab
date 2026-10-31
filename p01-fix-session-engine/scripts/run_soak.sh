#!/usr/bin/env bash
# Soak run: the client against the scriptable acceptor for a stretch of time,
# collecting metrics so resource plateaus can be judged.
#
# Usage: scripts/run_soak.sh [--hours 24] [--port 19878] [--out soak-out]
#
# The duration is a TEST SUGGESTION from the design doc, not a measured claim.
# Read the metric snapshots afterwards: file descriptors, resident memory and
# the inbox backlog should plateau rather than track elapsed time.
set -euo pipefail

HOURS=1
PORT=19878
OUT="soak-out"
BUILD_DIR="build"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --hours) HOURS="$2"; shift 2 ;;
    --port) PORT="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    --build-dir) BUILD_DIR="$2"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 1 ;;
  esac
done

mkdir -p "$OUT/state"
SECONDS_TOTAL=$(( HOURS * 3600 ))

sed -e "s/^port = .*/port = $PORT/" \
    -e 's/^heartbeat_seconds = .*/heartbeat_seconds = 5/' \
    -e "s#^database_path = .*#database_path = \"$OUT/state/session.db\"#" \
    -e 's/^create_database_if_missing = .*/create_database_if_missing = true/' \
    config/session.example.toml > "$OUT/session.toml"

"$BUILD_DIR/fix_test_acceptor" --port "$PORT" --heartbeat 5 --quiet > "$OUT/acceptor.log" 2>&1 &
ACCEPTOR_PID=$!
trap 'kill $ACCEPTOR_PID 2>/dev/null || true' EXIT
sleep 1

"$BUILD_DIR/fix_client" --config "$OUT/session.toml" \
  --duration-s "$SECONDS_TOTAL" --send-every-ms 250 \
  --metrics-out "$OUT/metrics.txt" --audit-out "$OUT/audit.log"

echo "soak finished; evidence in $OUT"
grep -E 'next_out|next_in|heartbeat|faults_total|inbox_pending|unresolved' "$OUT/metrics.txt" || true
