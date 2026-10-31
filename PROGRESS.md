# PROGRESS — Terminal Tanks DS

## 2.0 update (in progress, 2026-10-30)
Goal: bring the DS port from PC 1.5.0 to PC 2.0.1 content, then bulletproof wireless, then music, then a 2.0 release.

Done (all committed):
- `reference/` is the PC 2.0.1 package; `tools/ttref.py` loads it flat. Generator handles the new content.
- Core: 9 new ammo mechanics (seek, pierce, mirror, push, cdelay, burrow, slither, stars, volley), AI models its
  ammo's g/wind/speed, Martyr 2.0 balance, Atomic Breath push. Parity: world+shot 300/300, AI 24/24, battles 6/6,
  `run_battle_parity.py new` 10/10 (one match per new mechanic).
- Saves: format 2. DS 1.5.0 saves (format 1) are remapped BY NAME (tools/gen_legacy.py -> core/gen/legacy_v1.c);
  original kept as <profile>.v1.bak; unreadable saves kept as <profile>.bak before any write.
  tests/save_migration/run.sh: 200/200 random 1.5.0 saves identical by name (negative control fails as it should).
- Visuals: 10 skylines, Mariana jellyfish, new fx motes, 20 materials, 10 glows, 10 barrel styles, 12 tank effects,
  10 shells, 10 trails, 10 impact bursts + signature blasts, 12 animated paints (generic ANIM_PAINTS registry,
  compact swatches like 2.0's setup.py). All checked in the emulator (tests/visual/shoot.py).
- Known emulator-only glitch: NooDS occasionally shows the top screen horizontally stretched for 1-2 frames during
  PARADOX shots. Verified the game's canvas AND the top back buffer are correct in those frames -> display/emulator
  side. Needs a look on real hardware.

- 10 home-screen layers (source/home_layers2.inc): half-block _Px canvas, integer primitives, cached affine stamps
  for static sections; 18-44 ms per redraw on the emulated ARM9 (Biolume Trench 58 ms). DS shortcuts listed in
  docs/FEATURE_PARITY.md.
- 2.0 UI: LINKS screen (2.0.1 list), RPS "YOU WIN!", shop panel without the balance, v2.0.1 strings, banner "v2.0 port".
- STEP 1 DONE (2026-10-30, ROM md5 9e94e401...): all tests pass (parity 300/300, 24/24, 6/6, new 10/10; save migration
  200/200). Handed to the user for a hardware test.

- STEP 3 (sound, done before step 2 at the user's request): the PC soundtrack and every effect, rendered by the PC
  game's own mixer (tools/gen_audio.py -> nitrofiles/audio, ~240 MB) and played by source/audio.c (music: ADPCM
  stream decoded in the VBlank IRQ into a looping PCM ring on channel 0, synced to timers 0+1; effects: hardware
  ADPCM on channels 1-11 from a 256 KB cache). Director (source/director.c) mirrors app._direct_music, _ui_sound,
  the wallet chimes and every BattleScene sfx call (the core reports them as FX_SFX; parity unchanged: 300/300,
  24/24, 6/6, new 10/10; save migration 200/200). Settings: AUDIO ON/BEEP ONLY/OFF + MUSIC/SFX VOLUME, packed in
  the existing settings byte (no save-format change; old 0/1 values mean ON). Checked in NooDS with a sound capture.

- Saves fixed (the sound ROM's NitroFS made the ROM the current drive; tests/harness/save_persist.py) and STEP 2
  (wireless) done: link pumped every frame by the app, no message ever dropped (inbox backpressure, aim merging),
  waiting banner instead of a 6 s cut-off, automatic re-join + stream resume after a lost association, session id
  per packet. melonDS two-console match PASS with no faults, 20% random loss, a 10 s and a 33 s blackout.

- Jukebox (main menu), NOW PLAYING popup + SONG POPUP setting, setting descriptions wrap instead of overflowing.

Next:
1. User hardware test of step 1 + sound: save migration on their real card (backup first), new content, home screens,
   the NooDS-only stretched frame during PARADOX shots, LINKS screen layout (not captured in the emulator).
2. Step 2: wireless reconnect/resync + stress tests (melonDS two-console harness in tools/harness: needs building).
3. Step 3: music on the DS. Step 4: polish + 2.0 release (DS repo, website DS page, itch).

## Status
**Feature-complete port; first real-DS test done by the user.** Their report (2026-10-13): Master/Eternal home
screens cluttered and broken-looking, Astral Rift map laggy, and after a power-off the game showed a white screen
on launch. All three addressed (below) and re-verified in emulation; awaiting a second hardware test.

## Fixes after the first hardware test
- White screen on relaunch: ROOT CAUSE FOUND — the user had deleted the flashcart's `__rpg` folder (Wood R4
  kernel files on an Ace3DS+ clone running TWiLight Menu++); every game white-screened. Restoring `__rpg` from
  their backup fixed the cart. Not a game bug. The hardening below stays anyway: Saves are now two fixed-size slots overwritten in place
  (no FAT changes on save, newest valid slot wins), a boot screen appears before the card is touched, L+R at
  start = safe start (no save read/write), crashes show the exception screen. Old saves migrate.
- Home screens: integer colour/trig, table-driven corona, cheaper logo, backdrop redraw rate adapts to cost,
  unchanged screens are not re-rendered. All 91 screens now 18–37 ms (were 45–180 ms). Master and Eternal
  decluttered (FEATURE_PARITY #17). Missing glyphs (∙ ╱ ╲ ╵ ▂ ◂ ✿ ❀) added to the font.
- Astral Rift map: ring/rays/aurora/stars in integer maths, aurora bands staggered: 330 ms → 29 ms a frame.
  All 15 map effect types now 18–29 ms.

## Completed
- Original `terminaltanks.py` (v1.5.0, 13,455 lines) read end to end; kept unmodified in `reference/`.
- Content (all counts verified by importing the module): 113 tanks, 113 ammo + 18 Martyr ammo, 50 + 87 maps,
  90 home screens, 266 shop items, 20 levels, 50 missions, 10 tickets, 466 unlockables, 24 cheats — generated
  by `tools/gen_content.py` (re-running reproduces the committed files byte for byte).
- Toolchain from GitHub (`scripts/setup_toolchain.sh`): xPack GCC 14.2.1 + wf-picolibc + BlocksDS 1.17.1.
  Clean builds give an identical ROM (md5 checked twice).
- Portable C core (`core/`): rng, world, shot (every ammo mechanic), AI (resumable), battle, session (all modes),
  save (unlocks, wallet, shop, workshop, cheats, binary format), xmath (correctly rounded sin/cos/log/hypot/x³).
- DS frontend (`source/`): 51×48-cell terminal over both screens, 128×80 battlefield at 2×, all backdrops and
  home screens, UI kit, on-screen keyboard, touch on every screen, profiles + saves on FAT, PSG beeps.
- All scenes: boot/title/menu/notice/how-to/settings/links/cheats, rules/setup/map select/round map/RPS/battle/
  pause/results, tournament roadmap/results, cinematics/whispers, tickets, Martyr rewards/finale/outro, missions/
  co-op, shop, workshop + studio, profiles, tutorial + practice range, LAN menu/host/join.
- Local wireless (dswifi, no router): reliable stream over CMD/REPLY frames, original message set, lockstep battles.
- Performance pass: bit-exact fast AI trace + adaptive per-frame AI budget (think time overlaps the idle part of
  the aim animation), incremental terrain shading, integer scenery bakes, fixed-point explosions, cached font
  and RPS hand sprites, bottom screen redrawn only on change. Battle ≈ 14–20 ms/frame (30 fps held in melonDS).
- Tests (all last run on the final ROM):
  - Parity vs Python on Linux: world+shot 300/300, AI 24/24, battles 6/6 (glibc build); 298/300, 24/24, 6/6
    with the DS's correctly rounded math.
  - DS self-test ROM (core on the emulated ARM9 vs host): 86/86 identical.
  - Two emulated consoles over local wireless (`run_lan_versus.py`): PASS — identical state hashes, every shot
    delivered, round end + next round.
  - Emulator play: menu tour, single-player match, tournament level 1, cheat entry, save persistence on SD.
- Docs: README, docs/PORTING_SPEC, ARCHITECTURE, CONTROLS, BUILDING, TESTING, FEATURE_PARITY, MULTIPLAYER,
  ASSETS, DEVELOPMENT.

## Important decisions (do not reverse without reason)
- BlocksDS v1.17.1 pinned (newer libnds needs a newer picolibc than wf-picolibc provides).
- Battlefield fixed at 128×80 world px (a legal original field size); physics identical to a 128-col terminal.
- Simulation in `double` with Python's evaluation order, `-ffp-contract=off`; MT19937 + random.Random ported
  bit-exactly. DS build uses `TT_XMATH` (correctly rounded math) because picolibc differs from glibc more often.
- Lockstep netplay: only aim/fire/skip cross the link; both consoles simulate.

## Not verified / known issues
- Real hardware (DS, DSi, 3DS) — none available. Real radio conditions and mid-match disconnects untested.
- Not played end to end in the emulator: tournament levels 2–20, missions, LAN co-op, tickets, the Martyr,
  shop purchases, workshop editing (all ported; rules live in the parity-tested core).
- One-off hitches: battle entry ~75 ms, first RPS frame ~340 ms, ~220 ms after a round; ~20 fps for about a
  second at round end with heavy particles.

## Exact next action
User retests the 12:33 build on hardware (cart fixed): boot screen, relaunch after power-off, Master/Eternal
screens, Astral Rift. Then LAN on two consoles.
