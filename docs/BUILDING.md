# Building

## Requirements

* Linux x86-64 (the scripts were run on Ubuntu; any recent distro with `git`, `curl`, `make`, `python3`, a host
  `gcc`/`g++` and `cmake` should work).
* Network access to GitHub only. The usual BlocksDS installer (`wf-pacman` from wonderful.asie.pl) was not
  reachable when this port was made, so `scripts/setup_toolchain.sh` builds the same toolchain from GitHub sources.

## 1. Toolchain (once)

```sh
scripts/setup_toolchain.sh          # DEPS=$HOME/deps by default; needs write access to /opt/wonderful
```

It installs, pinned:

| Component | Version | Where |
|---|---|---|
| xPack `arm-none-eabi-gcc` | 14.2.1-1.1 | `/opt/wonderful/toolchain/gcc-arm-none-eabi` |
| wf-picolibc (C library) | branch `wonderful` | into the GCC sysroot |
| BlocksDS SDK (libnds, dswifi, maxmod, ndstool, grit) | v1.17.1 + its pinned submodules | `$DEPS/blocksds` |

BlocksDS is pinned at 1.17.1 because newer libnds needs a newer picolibc than wf-picolibc's branch provides.
If you already have an official BlocksDS install, it should also work: point `BLOCKSDS` at it.

## 2. The ROM

```sh
export WONDERFUL_TOOLCHAIN=/opt/wonderful
export BLOCKSDS=$HOME/deps/blocksds
make -j8                        # -> terminaltanks.nds  (≈ 0.8 MB)
make clean
```

Notes on the build (all in `Makefile`):

* ARM9 code is built `-O2 -marm` (soft-float runs faster as ARM than Thumb); the two big generated tables
  (`core/gen/content_data.c`, `source/gen/font_data.c`) are built as Thumb to save space.
* `-ffp-contract=off` keeps floating-point results identical to the host build used by the parity tests.
* ARM7 binary: BlocksDS's prebuilt `arm7_dswifi.elf` (wireless + sound + touch). ARM9 links
  `-ldswifi9_noip -lnds9 -lc -lgcc -lm` (dswifi without the IP stack: local multiplayer only).
* The banner (icon `assets/icon.bmp`, title "Terminal Tanks DS / v1.5.0 port / ethanlabs101") is added by ndstool.
* `TT_VERSION` is `git describe` of this repo.

Reproducibility: with the pinned toolchain the ROM depends only on the repository contents (and the git
description string baked into it).

## 3. Generated sources (only when the Python original or the font changes)

Both are committed, so a normal build does not need Python:

```sh
python3 tools/gen_content.py    # reference/terminaltanks.py -> core/gen/content_data.c, content_enums.h
python3 tools/gen_font.py       # assets/font/spleen-5x8.bdf + drawn symbols -> source/gen/font_data.c
```

`gen_content.py` imports the original module (no game loop runs) and dumps every content table, so the C data
always matches the Python objects exactly.

## 4. Test tools

```sh
make host-tests                 # build/host/probe   (the core compiled for the PC, used by tests/parity)
scripts/build_harness.sh        # build/host/harness (headless melonDS, used by tests/harness)
```

`build_harness.sh` clones melonDS (commit 906e9eb), builds its core library without a GUI, and links
`tools/harness/harness.cpp` against it. No BIOS or firmware dumps are needed (FreeBIOS + generated firmware).
See `docs/TESTING.md`.

## Running

* **Flashcart**: copy `terminaltanks.nds` to the card. The game creates `/data/TerminalTanks/` for saves (DLDI
  is patched by the flashcart loader, as for any homebrew).
* **DSi / 3DS (TWiLight Menu++, nds-bootstrap)**: should work like any BlocksDS homebrew — **not tested**.
* **Emulators**: melonDS (tested, headless), no$gba / DeSmuME (not tested). For saves in an emulator, enable
  its DLDI / SD-card emulation.
