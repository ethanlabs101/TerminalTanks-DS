#!/usr/bin/env bash
# Builds a test ROM that boots with everything unlocked and home screen $1 (perf / layout tests of home screens).
set -e
cd "$(dirname "$0")/.."
make -s clean >/dev/null
make -s -j8 EXTRA_CFLAGS="-DTT_DEBUG_HOME=$1" >/dev/null
cp terminaltanks.nds build/home_$1.nds
make -s clean >/dev/null
