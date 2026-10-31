#!/usr/bin/env bash
# Build and run under the checks this toolchain actually supports.
#
# mingw-w64 ships no libasan or libubsan runtime, so the portable option
# is trapping undefined-behaviour checks plus libstdc++ assertions. That
# is weaker than UBSan proper and is reported as what it is rather than
# as a sanitizer run.
set -euo pipefail
cd "$(dirname "$0")/.."
BUILD="${BUILD:-build-ubtrap}"
cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DJRNL_UBSAN_TRAP=ON
cmake --build "$BUILD"
export PATH="$(dirname "$(command -v g++)"):$PATH"
ctest --test-dir "$BUILD" --output-on-failure
