#!/usr/bin/env bash
# Configure, build and run every suite, then write the report.
#
# On an MSYS2 toolchain the sqlite3 DLL lives beside the compiler, so the
# compiler prefix goes on PATH before anything is run.
set -euo pipefail
cd "$(dirname "$0")/.."

BUILD="${BUILD:-build}"
cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build "$BUILD"

CXX_DIR="$(dirname "$(command -v g++)")"
export PATH="$CXX_DIR:$PATH"

ctest --test-dir "$BUILD" --output-on-failure "$@"
python scripts/report.py --artifacts "$BUILD/test-artifacts" --out docs/P06_TEST_REPORT.md
