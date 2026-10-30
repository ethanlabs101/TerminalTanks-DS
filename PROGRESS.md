# PROGRESS — Terminal Tanks DS

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
