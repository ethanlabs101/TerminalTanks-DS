# Development

## Ground rules

* `reference/terminaltanks.py` is the specification. Never edit it. When behaviour is unclear, read it (line
  numbers are quoted in comments throughout `core/` and `source/`).
* Anything that can change an outcome (random draws, physics, damage, AI, rewards, unlocks, save rules) goes in
  `core/` and must keep the parity tests green. Presentation goes in `source/`.
* `core/` must stay portable C11 with no DS headers, so the host probe can compile it.

## Day-to-day loop

```sh
export WONDERFUL_TOOLCHAIN=/opt/wonderful BLOCKSDS=$HOME/deps/blocksds
make -j8                                   # ROM
make host-tests                            # probe + probe_xmath
python3 tests/parity/run_parity.py 100     # quick parity check (also run_ai_parity.py, run_battle_parity.py)
python3 tests/harness/tour.py build/t.script && build/host/harness run terminaltanks.nds build/t.script build/t
```

Screenshots land in the output directory as `.ppm` (both screens stacked, 256×384); the game's debug output
goes to `log_0.txt`. Convert with PIL: `Image.open('x.ppm').save('x.png')`.

### Writing an emulator test

`tests/harness/scriptgen.py` builds scripts:

```python
s = Script()                      # starts at frame 380 (after boot)
s.tap("A").wait(60)               # press A for 4 frames, then wait 60 frames
s.taps("DOWN", 3)                 # three taps
s.touch(120, 100)                 # touch (x, y) on the touch screen (256x192)
s.osk_type("hello").osk_ok()      # type on the on-screen keyboard
s.shot("name")                    # screenshot
s.save("build/x.script")
```

Two consoles: `harness mp rom scriptA scriptB outdir` (see `tests/harness/lan_versus.py`). Each console writes
`log_<n>.txt`. The consoles run in separate threads, so compare logs, not screenshots taken "at the same frame".

### Test builds for one home screen or one map

```sh
scripts/home_rom.sh 88                                  # build/home_88.nds: everything unlocked, home screen 88 (Master)
make clean && make EXTRA_CFLAGS=-DTT_DEBUG_MAP=136      # every battle on map 136 (Astral Rift); rebuild normally after
```

`TT_DEBUG_HOME` / `TT_DEBUG_MAP` exist only for these test builds; the release ROM never defines them.

## Adding things

* **Content** (a tank, ammo, map, theme…) comes from the original's tables via `tools/gen_content.py`. When a
  newer version of `terminaltanks.py` adds content, replace `reference/terminaltanks.py` with it and re-run the
  generator; the C side reads everything from the generated tables. A new ammo *mechanic* also needs code in
  `core/shot.c` and a parity run.
* **A scene**: implement a `SceneVT` (see `source/scenes.h` and any `scn_*.c`), draw into the `Term` with the
  `term_*`/`ui_*` helpers, register touch areas with `hot_add` and handle `A_TOUCH` with the same ids.
  Bottom-screen rows are `T_SPLIT`..`T_ROWS-1`.
* **A setting**: add it to `Settings` (core), the settings table in `scn_home.c`, and the save format in
  `core/save.c` (bump `SAVE_VERSION`).
* **A network message**: add the type to `netplay.h`, keep payloads ≤ 400 bytes, bump `NET_PROTO`.

## Performance

The ARM9 runs at 67 MHz with no FPU; every `double`/`float` operation is a library call. Budget per frame at
30 fps: 33 ms ≈ 526 scanlines.

* `plat_lines()` is a scanline clock. The debug console prints `perf avg … max … | upd draw px rtop rbot` every
  60 frames, and `slow update` / `slow draw` / `slow step` lines when one frame overruns.
* Keep per-pixel work integer or fixed-point (`pxmix_k`, `cmix_k`, `isqrt64` in `render.c`), cache anything that
  is redrawn identically (terrain columns, RPS hands, font lookups).
* The AI is the heaviest simulation work: `core/ai.c` `fast_trace` is the hot loop; it must stay bit-identical
  to `phys_trace` + `model_probe` (the AI parity test checks this).
* Do not change float evaluation order in `core/`: `a*b*c` and `a*(b*c)` round differently, and the parity tests
  will catch it.

## Debug logging

`plat_log(fmt, …)` prints to the no$gba/melonDS debug console (max ~120 characters, `%` sequences are
interpreted by the emulator). In LAN battles the game logs `NET phase …` / `NET send|recv fire …` lines, which the
LAN test uses to prove the consoles stayed in lockstep.

## Repository layout

```
core/            simulation (portable)        core/gen/      generated content tables
source/          DS frontend                  source/gen/    generated font
reference/       the original Python (read-only)
tests/parity/    host parity tests            tests/harness/ emulator tests   tests/ds_selftest/ ARM9 self-test
tools/           generators, harness source   scripts/       toolchain + harness setup
docs/            documentation                assets/        icon, font source
```
