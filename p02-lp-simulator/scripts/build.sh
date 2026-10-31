#!/usr/bin/env bash
# Incremental build for day-to-day iteration.
#
# CMake + CTest remains the documented harness (design doc 13.2, 18.5); this
# script exists because a full reconfigure per edit is slow on this toolchain.
# It compiles only what changed, archives the library and links the suites.
#
# usage: scripts/build.sh [--opt] [target ...]
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

CXX="${CXX:-g++}"
OPT="-O0 -g"
if [ "${1:-}" = "--opt" ]; then OPT="-O2"; shift; fi

FLAGS="-std=c++20 -Iinclude -Itests/support -Wall -Wextra -DLPSIM_ENABLE_FAILPOINTS $OPT"
LIBS="-lsqlite3 -lws2_32"
OBJ="build/obj"
mkdir -p "$OBJ" build

LIB_SRCS="result wire clock fixed_point framer codec domain quote_book matcher ids \
failpoint scheduler validator report_builder ownership sqlite_store order_actor \
session config metrics scenario"

fail=0

compile() { # compile <src> <obj>
  local src="$1" obj="$2"
  if [ -f "$obj" ] && [ "$obj" -nt "$src" ]; then
    # Recompile when any header is newer than the object: the dependency set
    # here is coarse on purpose, because a stale object is far worse than a
    # redundant rebuild.
    if [ -z "$(find include tests/support -newer "$obj" -name '*.hpp' -print -quit 2>/dev/null)" ]; then
      return 0
    fi
  fi
  printf '  CXX %s\n' "$src"
  $CXX $FLAGS -c "$src" -o "$obj" || return 1
}

echo "== library =="
for n in $LIB_SRCS; do
  compile "src/$n.cpp" "$OBJ/$n.o" || fail=1
done
[ $fail -eq 0 ] || { echo "library build FAILED"; exit 1; }

compile tests/support/tiny_test.cpp "$OBJ/tiny_test.o" || exit 1

LIB_OBJS=""
for n in $LIB_SRCS; do LIB_OBJS="$LIB_OBJS $OBJ/$n.o"; done
ar rcs build/liblpsim.a $LIB_OBJS

link() { # link <name> <src> [extra objs]
  local name="$1" src="$2"; shift 2
  printf '  LD  %s\n' "$name"
  $CXX $FLAGS -o "build/$name.exe" "$src" "$@" build/liblpsim.a $LIBS || return 1
}

SUITES="test_wire test_quote test_reports test_scheduler test_store test_business \
test_session test_model"
INTEGRATION="test_recovery test_playbooks test_crash"

echo "== binaries =="
targets="${*:-all}"
for t in $targets; do
  case "$t" in
    all)
      for s in $SUITES; do
        [ -f "tests/unit/$s.cpp" ] && { link "$s" "tests/unit/$s.cpp" "$OBJ/tiny_test.o" || fail=1; }
      done
      for s in $INTEGRATION; do
        [ -f "tests/integration/$s.cpp" ] && { link "$s" "tests/integration/$s.cpp" "$OBJ/tiny_test.o" || fail=1; }
      done
      link lp_simulator apps/lp_simulator/main.cpp || fail=1
      link scenario_runner apps/scenario_runner/main.cpp || fail=1
      ;;
    lp_simulator) link lp_simulator apps/lp_simulator/main.cpp || fail=1 ;;
    scenario_runner) link scenario_runner apps/scenario_runner/main.cpp || fail=1 ;;
    *)
      if [ -f "tests/unit/$t.cpp" ]; then
        link "$t" "tests/unit/$t.cpp" "$OBJ/tiny_test.o" || fail=1
      elif [ -f "tests/integration/$t.cpp" ]; then
        link "$t" "tests/integration/$t.cpp" "$OBJ/tiny_test.o" || fail=1
      else
        echo "  unknown target: $t"; fail=1
      fi
      ;;
  esac
done

[ $fail -eq 0 ] && echo "build OK" || { echo "build FAILED"; exit 1; }
