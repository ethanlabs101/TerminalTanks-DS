"""Single-player match: the player fires every few seconds with small aim changes; screenshots and perf log."""
import sys
sys.path.insert(0, "tests/harness")
from scriptgen import Script
s = Script()
s.tap("A").wait(60)            # title -> menu
s.tap("A").wait(60)            # SINGLE PLAYER -> rules
s.tap("A").wait(60)            # rules -> setup
s.tap("A").wait(120)           # setup -> battle
s.shot("intro", 60)
for k in range(14):
    s.taps("UP", 3, 6).tap("A").wait(420)
    if k in (0, 3, 7, 13):
        s.shot(f"turn{k}", 0)
print(s.save(sys.argv[1]))
