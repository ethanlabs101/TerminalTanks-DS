# Updating the DS port when the Python game changes

The workflow when a new `terminaltanks.py` arrives:

1. Diff the new file against `reference/terminaltanks.py` to see exactly what changed, then replace the reference.
2. Re-run `python3 tools/gen_content.py`: every content table (tanks, ammo, maps, themes, shop, missions, levels,
   cheats, unlocks, story text) is regenerated from the Python objects automatically.
3. Port what the generator cannot: new ammo *mechanics* (`core/shot.c`), rule/AI/progression changes
   (`core/*.c`), new drawing code (`source/render.c`, `backdrop.c`, `sprites.c`), new screens (`source/scn_*.c`).
4. Rebuild (`make`), run the parity tests (`tests/parity/*.py`), the DS self-test and the emulator tests.
5. Bump `SAVE_VERSION` only if the save format changed, with a migration so old DS saves still load.

| Python change | DS work |
|---|---|
| New tank design / ammo with existing mechanics / map / home theme / shop item | automatic (regenerate + rebuild) |
| Ammo with a brand-new mechanic, new rule, AI tweak | small C port + parity check |
| Balance numbers (damage, prices, physics constants) | usually automatic (they live in the tables) |
| New screen, new visual effect | C port of that piece |
| Sound effects engine | DS version on the ARM7 sound hardware (PSG tones, or samples via maxmod) |

Keep a copy of the DS source (the zip or a git repo) together with the Python file it was ported from.
