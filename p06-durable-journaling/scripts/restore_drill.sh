#!/usr/bin/env bash
# Run the backup, archive and restore scenarios on their own.
#
# Section 17.2: a backup that has never been restored is not a backup, so
# this is meant to run on a schedule and have its result recorded, not
# only as part of a commit check.
set -euo pipefail
cd "$(dirname "$0")/.."
BUILD="${BUILD:-build}"
cmake --build "$BUILD"
export PATH="$(dirname "$(command -v g++)"):$PATH"
"$BUILD/test_backup_restore.exe" --artifact="$BUILD/test-artifacts/restore_drill.json"
