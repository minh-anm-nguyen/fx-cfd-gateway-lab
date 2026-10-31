#!/usr/bin/env bash
# Thin wrapper over the Makefile, so "bash scripts/build.sh" works the same way
# on a machine with no CMake generator configured.
#
#   JOBS=12 bash scripts/build.sh          # parallel build
#   ASAN=1  bash scripts/build.sh          # sanitizers, as design doc 22.3 asks
set -euo pipefail
cd "$(dirname "$0")/.."
exec make -j "${JOBS:-8}" ASAN="${ASAN:-0}" "$@"
