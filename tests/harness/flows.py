"""Smoke flows: tournament level 1, mission 1 and the shop, each driven by A taps from the main menu,
with screenshots along the way (fresh profile, no cheats)."""
import sys
sys.path.insert(0, "tests/harness")
from scriptgen import Script
s = Script()
s.tap("A").wait(60)
# tournament
s.taps("DOWN", 3).tap("A").wait(60).shot("t_roadmap")
for i in range(6):
    s.tap("A").wait(70).shot(f"t_step{i}")
s.wait(300).shot("t_battle")
s.tap("START").wait(40).shot("t_pause")
s.taps("DOWN", 3).tap("A").wait(40).shot("t_leave")
s.tap("A").wait(90).shot("t_after_leave")
s.tap("B").wait(60).tap("B").wait(60).tap("B").wait(60).shot("t_back")
print(s.save(sys.argv[1]))
