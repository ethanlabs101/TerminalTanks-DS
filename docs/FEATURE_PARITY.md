# Feature parity

Legend — **Verified**: checked by an automated parity test or played in the emulator as described.
**Ported**: implemented from the original's code and seen rendering, but not played through end to end.
Nothing below is "planned": every feature of the original is implemented. Real DS hardware was not available, so
nothing is verified on hardware (see `TESTING.md`).

## Gameplay

| Feature (original) | DS | Status |
|---|---|---|
| Terrain generation, all 137 maps (50 library + 87 special), spawns, scenery random draws | `core/world.c` | Verified (parity) |
| Ballistics: gravity, wind, wind drift per turn, analytic arcs, sub-stepped collision | `core/world.c` | Verified (parity) |
| All ammo mechanics (60 `mech` ammos: split, cluster, roll, bounce, dig, mound, napalm, wobble, jump, glass…), blasts, splash, craters, falling ground and tanks | `core/shot.c`, `core/battle.c` | Verified (parity, every ammo incl. 18 Martyr ammo) |
| Turn flow, intro, flights, impacts, destruction, round end, best-of-N | `core/battle.c`, `core/session.c` | Verified (parity: whole matches) |
| AI, 4 difficulties incl. MASTER ensemble solver and learning | `core/ai.c` | Verified (parity) |
| UFO tanks (scale, hover, hops), special attack + charge (X), MARTYR'S BLESSING | `core/battle.c` | Verified (parity) for UFO/special; Blessing ported |
| Hearts, glass HP, rule variants (streak, flawless, one-strike, glass, gale, back-to-back) | `core/session.c` | Ported (rules are in the parity-tested core; tournament L1 "back to back" played) |
| Coins, credits, payouts, shop prices, unlock rules (466 unlockables), cheat codes (24) | `core/save.c` | Ported; cheat `iamethanlabs101` verified in emulator |

## Modes and screens

| Original | Status |
|---|---|
| Boot, title, main menu with all 90 home screens (Classic, 65 shop, 6 mission, 10 ticket, 6 ancient, Master, Secret) | Ported; title/menu verified; home screens render via the same layer code |
| Single Player: rules → pilot setup (live preview) → map select → match → results, coins | Verified (played) |
| Two Player hot-seat incl. rock-paper-scissors | Ported (RPS verified over LAN; hot-seat RPS uses the same scene) |
| LAN: host/join, versus, co-op missions | Versus verified on two emulated consoles; co-op ported (`MULTIPLAYER.md`) |
| Tournament: 20 levels, roadmap, loadout, whispers, rewards, retry | Level 1 verified (played); other levels ported |
| Missions (50): marathon, 2v1, UFO, co-op (local, LAN, AI wingman), rush, mothership | Ported; menu verified |
| Shop: 5 pages, tiers, coins/credits, theme preview, ticket art, Time Machine | Ported; pages verified rendering |
| Terminal Tickets (10 vault bosses) | Ported |
| The Martyr: 3 phases, element ammo, cinematics, finale, outro, ancient rewards, Blessing | Ported |
| Workshop (custom tanks, 24 slots, name/lore entry) and TANK STUDIO | Ported; lock notice verified |
| Tutorial: 4 lessons, briefing, 3 challenges, practice range with coach | Ported; menu verified |
| How to play, Settings, Links, Cheat codes, Profiles (3) | Verified rendering; cheats verified working; profile save/reload verified |
| Pause / confirm-leave / round result / match result overlays | Verified (played) |

## Deliberate differences (and why)

**Display**
1. *Terminal size.* The original adapts to the terminal (field width = columns, up to 180×80 px). The DS is one
   fixed 51×48-cell terminal across both screens; every screen layout was re-flowed for 51 columns (e.g. shop
   cells are 9 columns wide instead of 10, the practice coach sits on the touch screen instead of over the field).
2. *Battlefield 128×80.* A legal original field size (a 128-column terminal), drawn 2× on the top screen. All
   physics scale with the field exactly as in the original, so gameplay equals the original on a 128-column terminal.
3. *Colour.* The DS shows 15-bit colour (RGB555); the original's 24-bit gradients band slightly. The original's
   **COLOR MODE** setting (24-bit/256/16 terminal palettes) does not apply and is not shown.
4. *Big logos* are drawn as a pixel overlay (3×6 px per glyph); 51 columns cannot hold the block-letter logo.
5. *Home-screen heroes* (the tanks lobbing shells on menu backdrops) are drawn at scale 1, and their shell speed is
   scaled by 0.72 so the arcs stay on the narrower screen.
6. *Theme building/tree layouts* are seeded with a FNV hash of the theme name; Python seeds `random.Random(str)`
   via SHA-512. Purely decorative; the layouts differ from the PC.
7. *Match result digits*: the original only defines big digits 0–3 and would raise a `KeyError` on a final score of
   4–9 (reachable with CUSTOM ROUNDS); the DS shows plain digits there instead of crashing.

**Input** (see `CONTROLS.md`)
8. Keys are mapped to DS buttons; letter keys become: R (retry / rename) → **Y**, G (studio) → **Y**,
   X (special / delete) → **X**, home-screen toggle → **SELECT**. Text is typed on an on-screen keyboard.
   Everything is also reachable by touch.

**System**
9. *QUIT* saves and returns to the title screen (a DS program has nowhere to quit to).
10. *Links* screen: the DS has no web browser, so choosing a link shows the address to type elsewhere.
11. *Sound*: the original's optional terminal bell is a short PSG beep (setting SOUND OFF/BEEP).
12. *Saves*: same fields as the original's JSON save, stored as a compact versioned binary per profile on the SD
    card. PC save files cannot be imported, so the original's v1–v3 save migrations are not needed.
13. *LAN* uses DS local wireless instead of TCP/UDP; same messages and semantics (`MULTIPLAYER.md`).

**The ten 2.0 home screens (Biolume Trench .. Ghost Armada)** are drawn exactly as the original's half-block pixel
layers, with these DS shortcuts to stay within ~20-45 ms a redraw on the ARM9 (no FPU): parts of each scene that do not
move are cached as per-pixel affine stamps; the Biolume Trench's god-rays keep their rest positions (their brightness
still breathes) and its jellyfish trail five tendrils instead of ten; the Tesseract's nebula, the Jade Temple's sun
disc, the Deep Desert's far dunes and the Holo City's window lights are frozen at rest (they drift over minutes on the
PC); the Glowshroom Grove's stems and caps are at rest and mid-glow (their gills and spots still pulse).

**Busy home screens (after testing on a real DS)**
17. *Master*: one aurora band instead of three, no golden god-rays, fewer embers. *Eternal*: the sanctum keeps the
    mage, the artifact and the three orbiting orbs, but drops the rune column and the 36-rune ring. Every themed
    home screen shows at most 60 drifting motes (the original allows 160 on a wide terminal). The title's credit
    lines sit on an opaque strip so busy layers behind them stay readable.
18. *Backdrop frame rate*: home-screen backdrops are redrawn every frame when cheap and every 2nd–4th frame when
    expensive (their animation still advances every frame); the logo shimmer steps with them.
19. *Battle sky effects* (aurora, Astral Rift ring, storms, inferno glow) are computed in integer maths; the aurora
    is refreshed at 6 Hz, one band per frame. Same shapes and colours, small rounding differences.

**Timing and performance**
14. *AI thinking* is computed over several frames (about 14 ms of each 33 ms frame) instead of in one blocking call.
    The chosen shot is identical; the time spent is credited to the first 30 % of the original's think animation,
    during which the barrel does not move anyway. On the hardest settings the AI can still take about a second
    longer than on a PC before it starts aiming.
15. *Particles* are capped at 900 alive at once (the original has no cap). Normal play stays far below it.
16. *Math library*: the DS uses correctly rounded `sin/cos/log/hypot/x³` (`core/xmath.c`). Python on Linux uses
    glibc, which is off by one unit in the last place on ~0.1–0.6 % of inputs; parity with Python-on-Linux is
    298/300 world cases, 24/24 AI, 6/6 matches (see `TESTING.md`). Python on Windows/macOS differs from Linux in the
    same way.

## Known issues

* One-off frame hitches: entering a battle (~75 ms, scenery bake), the first rock-paper-scissors frame (~340 ms,
  hand sprites, cached afterwards), and a ~220 ms hitch when the match moves on after a round.
* Round end with heavy particle effects runs at ~20 fps for about a second.
* Expensive home screens (Worldtree, Eternal, …) still take 50–70 ms on the frames where the backdrop is redrawn
  (every 3rd–4th frame); the average stays under 33 ms.
* Not verified on hardware; flashcart/DLDI compatibility depends on the card.
