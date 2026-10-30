"""Scene tour: visits every main-menu destination (after the everything-cheat) and screenshots it."""
import sys
sys.path.insert(0, "tests/harness")
from scriptgen import Script

MENU = ["SINGLE", "TWO", "LAN", "TOURNAMENT", "MISSIONS", "SHOP", "WORKSHOP", "HOWTO", "TUTORIAL", "PROFILES", "SETTINGS", "CHEATS", "LINKS"]
s = Script()
s.tap("A").wait(60).shot("menu")
s.taps("DOWN", 11).tap("A").wait(50).shot("cheats")
s.osk_type("iamethanlabs101").osk_ok().shot("cheats_done", 20)
s.tap("B").wait(50)
for i, name in enumerate(MENU):
    if name == "CHEATS":
        continue
    s.taps("DOWN", i).tap("A").wait(60).shot(name.lower())
    if name == "SHOP":
        s.tap("R").shot("shop2", 20).tap("R").shot("shop3", 20).tap("R").tap("R").shot("shop5", 20)
    s.tap("B").wait(60)
    s.wait(10)
print(s.save(sys.argv[1]))
