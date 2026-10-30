#!/usr/bin/env bash
# Builds the headless melonDS test harness (tools/harness/harness.cpp) used by tests/harness/*.
# melonDS is built as a library only (no Qt/SDL frontend); it boots the ROM directly with its built-in
# FreeBIOS and a generated firmware, so no BIOS/firmware dumps are needed.
set -euo pipefail
DEPS=${DEPS:-$HOME/deps}
M=$DEPS/melonDS
ROOT=$(cd "$(dirname "$0")/.." && pwd)
if [ ! -d "$M" ]; then
  git clone -q https://github.com/melonDS-emu/melonDS.git "$M"
  git -C "$M" checkout -q 906e9eb
fi
if [ ! -f "$M/build/src/libcore.a" ]; then
  cmake -S "$M" -B "$M/build" -DBUILD_QT_SDL=OFF -DENABLE_JIT=ON -DENABLE_OGLRENDERER=OFF -DENABLE_GDBSTUB=OFF -DCMAKE_BUILD_TYPE=Release
  cmake --build "$M/build" -j"$(nproc)" --target core
fi
mkdir -p "$ROOT/build/host"
g++ -std=c++17 -O2 -DJIT_ENABLED -DARCHITECTURE_x86_64=1 -DNDEBUG -I"$M/src" -I"$M/build/src" -I"$M/src/net" \
    -o "$ROOT/build/host/harness" "$ROOT/tools/harness/harness.cpp" "$M/src/net/LocalMP.cpp" \
    "$M/build/src/libcore.a" "$M/build/src/teakra/src/libteakra.a" -lpthread -lrt
echo "built $ROOT/build/host/harness"
