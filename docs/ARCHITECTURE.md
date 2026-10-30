# Architecture

The port has two halves with a hard line between them:

```
            reference/terminaltanks.py  (source of truth, never modified)
                        │ tools/gen_content.py imports it and dumps every table
                        ▼
 core/  ── portable C11 simulation, no DS headers ──────────────┐   also compiled for the host
   rng.c      bit-exact MT19937 + random.Random methods + round()│   (build/host/probe) and compared
   world.c    terrain, spawns, wind, collision, ballistics       │   against Python by tests/parity
   shot.c     every ammo mechanic (split, roll, bounce, dig...)  │
   ai.c       AIController, 4 difficulties, resumable planner    │
   battle.c   BattleScene's simulation half: turns, flights,     │
              impacts, UFOs, special, Blessing, practice range   │
   session.c  match / tournament / mission / ticket / Martyr /   │
              practice sessions, scoring, rewards, HUD text      │
   save.c     SaveState: wallet, unlocks, shop, workshop, cheats │
              + binary save format                               │
   gen/       generated content tables (content_data.c)          │
 ────────────────────────────────────────────────────────────────┘
                        ▲ reads state / calls battle_aim, battle_fire …
 source/  ── DS frontend (libnds, dswifi) ─────────────────────────
   platform.c  video/input/sound/FAT, 30 fps pacing, scanline clock
   term.c      the terminal: one 51×48 cell grid across both screens
   gfx.c       5×8 font, pixel surfaces, colour helpers, y-up canvas
   render.c    battlefield: scenery, tanks, shells, explosions, guide
   particles.c sparks/smoke/debris (the original's ParticleSystem)
   backdrop.c  the 90 home screens (Ambient/Master/Secret/Theme)
   sprites.c   artifacts, the Martyr mage
   preview.c   live tank preview stage, map thumbnails
   ui.c        menus, panels, keycaps, hearts, wallet, logo, keyboard
   app.c       scene stack, fades, input→Action events, profiles, main()
   netplay.c   dswifi local multiplayer + reliable stream + battle glue
   scn_*.c     one file per group of the original's scenes
```

## Why the split

Gameplay must match the original exactly, and that can only be proven by running the same code on a PC next
to Python. So everything that decides an outcome (random draws, physics, damage, AI choices, unlock rules,
rewards) lives in `core/` and is compiled twice: for the DS (ARM9) and for the host test probe. `core/` uses
`double` and the same evaluation order as Python, is compiled with `-ffp-contract=off` on both sides, and
the parity tests compare results value by value (`tests/parity/*.py`).

Everything that is only presentation (pixels, particles, backdrops, layout) lives in `source/` and may use
`float`, fixed-point and lookup tables freely: the DS has no FPU, and presentation does not need bit-exactness.
The original consumes some random numbers for purely visual things (stars, motes, scenery phases) from the
gameplay stream; `world_scenery_init` reproduces those draws in `core/` so the gameplay stream stays aligned.

## Screens: one terminal over two LCDs

The original draws into a terminal of `cols × rows` cells, with the battlefield as a pixel canvas rendered with
half-block characters (2 pixels per cell vertically). The DS port keeps that model:

* `term.c` holds one **51 × 48** cell grid (5×8 font → 255×384 px). Rows 0–23 are the top screen, rows 24–47
  the bottom (touch) screen. Scenes "draw to the terminal" exactly like the original; layouts were adapted from
  the original's 80+-column designs to 51 columns.
* The battlefield is the original's pixel canvas at **128 × 80 world pixels**, drawn 2× (256×160) on the top
  screen. 128×80 is a legal field size in the original (field width = terminal columns ≤ 180, height = 2 × rows ≤ 80),
  so physics, terrain generation and AI behave as the original does on a 128-column terminal.
* Cells can be transparent (`bg == 0`), so the HUD text composites over the battlefield like terminal glyphs over
  half-block pixels.
* Big gradient logos are a pixel overlay (3×6 px per glyph) because 51 columns cannot hold the original's
  block-letter logo at cell resolution.
* The bottom screen is re-rendered only when its cells change; the top screen is re-rendered every frame only when
  a pixel layer is active.

## Scenes

`app.c` is the original's `App` + `Scene` stack: a scene has `enter/exit/handle/update/update_ambient/draw/draw_px/
draw_over/destroy` hooks. `app_goto` replaces the scene through a fade, `app_push`/`app_pop` handle overlays
(pause, confirm, round result). Input is converted to the original's `Action` events (CONFIRM, BACK, PAUSE, d-pad,
FIRE...) plus touch hotspots (`hot_add`) that arrive as `A_TOUCH` events with the hotspot id, so every scene
handles buttons and touch through one path. A global `Session g_sess` carries the running match/tournament/
mission between scenes, as `App.session` does in Python.

## Frame loop and timing

30 fps, as the original (`FPS = 30`): `plat_present` waits for every second vblank and DMA-copies the back
buffers. The core is stepped with the real frame `dt`, exactly like the Python main loop. `plat_lines()` is a
scanline clock (63.56 µs per line) used for profiling; the debug console gets a `perf` line every 60 frames.

The AI planner is resumable (`ai_plan_step(budget)`); on the DS the battle scene sizes the per-frame budget so
planning takes ~14 ms a frame and the field keeps animating. Planning is budget-independent: the chosen shot is
identical to the one-shot planner (checked by the AI parity test). Time spent planning is credited to the first,
motionless 30 % of the original's `think_time`, so the AI does not feel slower than in Python.

## Saves

`save.c` serialises `SaveState` into a versioned, checksummed binary blob; `app.c` keeps one save per profile
(up to 3 profiles listed in `profiles.txt`) on the flashcart/SD under `/data/TerminalTanks/`. Saves are written
when the original saves (after results, purchases, settings changes) and every 60 s of play time.

Every file is stored as two fixed-size 8 KB slots, `<name>.0` and `<name>.1`, overwritten in place: a write goes
to the older slot with a sequence number and checksum, and loading takes the newest valid slot. The slot files
never change size after they are created, so a write only touches their data sectors — no FAT chain or
directory updates — and switching the DS off mid-write loses at most that one write, never the previous save or
the card's file system. Single-file saves from earlier builds are read once and migrated.

Boot order: video is set up and a boot screen is shown before the card is touched ("READING SD CARD…",
"LOADING PROFILE…"), so a card problem is visible instead of a blank screen. Holding **L + R** while the game
starts is a safe start: saves are neither loaded nor written for that session. A crash shows libnds's exception
screen.

## Networking

See `docs/MULTIPLAYER.md`. In short: dswifi local multiplayer (no router), a reliable ordered byte stream on top
of the host's CMD and client's REPLY frames, the original's LAN message set on top of that, and lockstep battles:
only aims/fires/skips cross the link, both consoles simulate.
