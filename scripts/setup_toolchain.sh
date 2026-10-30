#!/usr/bin/env bash
# Reproducible BlocksDS toolchain setup WITHOUT wf-pacman (used when wonderful.asie.pl
# is unreachable). Everything comes from GitHub.
#   - xPack arm-none-eabi-gcc 14.2.1  (compiler + binutils + libgcc)
#   - wf-picolibc (branch "wonderful") as the C library, replacing newlib in the sysroot
#   - BlocksDS SDK v1.17.1 with its pinned submodules (libnds, dswifi, maxmod, ndstool, grit)
# Result: WONDERFUL_TOOLCHAIN=/opt/wonderful  BLOCKSDS=$DEPS/blocksds
set -euo pipefail
DEPS=${DEPS:-$HOME/deps}
WF=/opt/wonderful
GCCVER=14.2.1-1.1
mkdir -p "$DEPS" "$WF/toolchain"
cd "$DEPS"

# 1. ARM GCC
if [ ! -x "$WF/toolchain/gcc-arm-none-eabi/bin/arm-none-eabi-gcc" ]; then
  curl -sSL -o gcc.tgz "https://github.com/xpack-dev-tools/arm-none-eabi-gcc-xpack/releases/download/v$GCCVER/xpack-arm-none-eabi-gcc-$GCCVER-linux-x64.tar.gz"
  tar xzf gcc.tgz && rm gcc.tgz
  cp -a "xpack-arm-none-eabi-gcc-$GCCVER" "$WF/toolchain/gcc-arm-none-eabi"
fi
export PATH=$WF/toolchain/gcc-arm-none-eabi/bin:$PATH

# 2. meson (from git; pip index may be unavailable) + wf-picolibc
[ -d meson ] || git clone -q --depth 1 --branch 1.6.1 https://github.com/mesonbuild/meson.git
[ -d wf-picolibc ] || git clone -q --depth 1 -b wonderful https://github.com/WonderfulToolchain/wf-picolibc.git
cat > cross-ds.txt <<'XEOF'
[binaries]
c = ['arm-none-eabi-gcc', '-nostdlib', '-mcpu=arm7tdmi', '-mthumb', '-mthumb-interwork']
ar = 'arm-none-eabi-gcc-ar'
as = 'arm-none-eabi-as'
nm = 'arm-none-eabi-nm'
strip = 'arm-none-eabi-strip'
[host_machine]
system = 'none'
cpu_family = 'arm'
cpu = 'arm'
endian = 'little'
[properties]
skip_sanity_check = true
XEOF
rm -rf pb && mkdir pb && cd pb
python3 ../meson/meson.py setup --cross-file=../cross-ds.txt -Dmultilib=false -Dpicocrt=false \
  -Dpicolib=false -Dsemihost=false -Dspecsdir=none -Dtests=false -Dthread-local-storage=true \
  -Dfast-bufio=true -Dio-long-long=true -Dio-pos-args=true -Dio-percent-b=true -Dposix-console=true \
  -Dformat-default=double -Dnewlib-nano-malloc=false -Dprefix=/opt/picolibc -Dlibdir=lib \
  -Dincludedir=include ../wf-picolibc >/dev/null
ninja >/dev/null && ninja install >/dev/null
cd ..
G=$WF/toolchain/gcc-arm-none-eabi/arm-none-eabi
[ -d "$G/include.newlib" ] || mv "$G/include" "$G/include.newlib"
rm -rf "$G/include"; mkdir "$G/include"
cp -a /opt/picolibc/include/* "$G/include/"; cp -a "$G/include.newlib/c++" "$G/include/"
for d in "$G/lib" "$G/lib/thumb/nofp"; do
  for l in libc.a libm.a libg.a libnosys.a; do cp /opt/picolibc/lib/$l "$d/"; done
done
[ -d wonderful-packages ] || git clone -q --depth 1 https://github.com/WonderfulToolchain/wonderful-packages.git
cp wonderful-packages/patches/picolibc.specs "$G/lib/"

# 3. BlocksDS SDK pinned to v1.17.1 (matches wf-picolibc's API generation)
[ -d blocksds ] || git clone -q --branch v1.17.1 --recurse-submodules --shallow-submodules https://github.com/blocksds/sdk.git blocksds
cd blocksds
export WONDERFUL_TOOLCHAIN=$WF BLOCKSDS=$PWD
make -j"$(nproc)" -C tools bin2c grit ndstool dldipatch
make -j"$(nproc)" -C libs libnds dswifi maxmod
make -j"$(nproc)" -C sys || true   # libxm7-based ARM7 cores fail (not needed); arm7_dswifi*.elf build fine
ls sys/arm7/main_core/arm7_dswifi_maxmod.elf
echo "Toolchain ready: export WONDERFUL_TOOLCHAIN=$WF BLOCKSDS=$PWD"
# NOTE: xPack GCC uses emulated TLS, so link with LIBS += -lc -lgcc after -lnds9 (see Makefile).
