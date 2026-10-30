# Terminal Tanks DS

A native Nintendo DS port of **Terminal Tanks v1.5.0** (`terminaltanks.py`, by ethanlabs101): the turn-based
terminal artillery game, rewritten in C for the DS with the BlocksDS SDK. The original Python file is kept,
unmodified, in `reference/terminaltanks.py` and is the source of truth for every rule, table and number.

```
terminaltanks.nds      the ROM (build it with `make`, see docs/BUILDING.md)
core/                  portable game simulation (C11, no DS code) – bit-exact with the Python original
source/                DS frontend: terminal renderer, scenes, input, saves, local wireless
tests/parity/          parity tests: the C core vs. the Python original, same seeds, compared value by value
tests/harness/         emulator tests: scripted play in headless melonDS, including two linked consoles
tools/                 content/font generators and the melonDS harness
docs/                  spec, architecture, controls, building, testing, parity, multiplayer, assets, development
```

## What works (and how it was checked)

| Area | State | Evidence |
|---|---|---|
| Simulation (terrain, ballistics, wind, every ammo mechanic, UFOs, specials, Blessing, AI) | ported, value-for-value | `tests/parity`: the C core is bit-exact with Python on Linux (world+shot 300/300, AI 24/24, whole AI-vs-AI matches 6/6); the DS build's correctly rounded math gives 298/300, 24/24, 6/6, and a self-test ROM shows the ARM9 computes exactly what the PC does (86/86) |
| All content (113 tanks, 113 ammo + 18 Martyr ammo, 137 maps, 90 home screens, 266 shop items, 20 levels, 50 missions, 10 tickets, 24 cheats, 466 unlockables) | generated from the Python objects | `tools/gen_content.py` |
| Every scene of the original (title → menus → battle → results, tournament, missions, co-op, shop, workshop + studio, tickets, Martyr, cinematics, profiles, tutorial/practice, settings, cheats, links, LAN) | ported | every main-menu screen rendered in melonDS (`tests/harness/tour.py`); single-player and tournament battles played in the emulator; tickets, the Martyr, cinematics and the studio were not reached in emulator runs |
| Local wireless, two DS consoles, no router | working in emulation | `tests/harness/run_lan_versus.py`: host, join, RPS, ~30 shots across 2 rounds, identical state hashes on both consoles |
| Saves (3 profiles) on the SD/flashcart FAT | working in emulation | profile written to an emulated SD card and read back after a reboot |
| Real hardware | **not tested** | no DS hardware was available; see docs/TESTING.md |

Known gaps are listed honestly in `docs/FEATURE_PARITY.md`.

## Quick start

```sh
scripts/setup_toolchain.sh                      # once: GCC 14 + picolibc + BlocksDS 1.17.1 (from GitHub)
export WONDERFUL_TOOLCHAIN=/opt/wonderful BLOCKSDS=$HOME/deps/blocksds
make -j8                                        # -> terminaltanks.nds
```

Copy `terminaltanks.nds` to a flashcart (or run it in melonDS / no$gba). Saves go to `/data/TerminalTanks/`
on the card. Controls: d-pad aims (← → angle, ↑ ↓ power), **A** fires, hold **L/R** for big steps,
**B** back, **START** menu, or use the touch screen. Full list in `docs/CONTROLS.md`.

## Documentation

- `docs/PORTING_SPEC.md` – what the original does (read from the source), and the DS mapping
- `docs/ARCHITECTURE.md` – how the port is put together
- `docs/CONTROLS.md` – buttons and touch
- `docs/BUILDING.md` – toolchain and reproducible build
- `docs/TESTING.md` – parity tests and emulator tests, what each proves
- `docs/FEATURE_PARITY.md` – feature-by-feature status and every deliberate deviation
- `docs/MULTIPLAYER.md` – DS-to-DS wireless design
- `docs/ASSETS.md` – where every asset comes from, licences
- `docs/DEVELOPMENT.md` – working on the code, profiling, adding things
- `PROGRESS.md` – status log

## Credits

Game design, content and original code: ethanlabs101 (Terminal Tanks). DS port: this repository.
Font: Spleen 5x8 by Frederic Cambus (BSD-2-Clause). Built with BlocksDS (libnds, dswifi), tested with melonDS.
