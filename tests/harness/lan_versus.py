"""Two-console LAN versus over melonDS local wireless (harness mp).
Host: LAN PLAY > HOST A GAME > rules (defaults) > wait for guest > start.  Guest: LAN PLAY > JOIN A GAME > join.
Then rock-paper-scissors, then both consoles press A every few seconds (only the side whose turn it is can fire).
Screenshots are taken on both consoles at the same frames: h_*.ppm and c_*.ppm."""
import sys
sys.path.insert(0, "tests/harness")
from scriptgen import Script

out = sys.argv[1]
CYCLES = int(sys.argv[2]) if len(sys.argv) > 2 else 10
h = Script()
h.tap("A").wait(60)                        # title -> menu
h.taps("DOWN", 2).tap("A").wait(60)        # LAN PLAY
h.tap("A").wait(60)                        # HOST A GAME -> rules
h.tap("A").wait(90)                        # rules confirm -> hosting
h.shot("h_hosting", 0)
c = Script()
c.tap("A").wait(60)
c.taps("DOWN", 2).tap("A").wait(60)
c.tap("DOWN").tap("A").wait(240)           # JOIN A GAME -> scanning
c.shot("c_scan", 0)
# guest joins (a few attempts), host starts
for f in (1100, 1250, 1400):
    c.lines.append(f"{f} tap A")
c.lines.append("1600 shot c_joined")
h.lines.append("1600 shot h_lobby")
for f in (1700, 1800):
    h.lines.append(f"{f} tap A")
# RPS: host rock, guest paper -> guest wins and chooses "GO FIRST"
h.lines.append("2000 shot h_rps")
h.lines.append("2050 tap A")
c.lines.append("2050 tap RIGHT")
c.lines.append("2070 tap A")
c.lines.append("2250 shot c_rps_reveal")
for f in (2500, 2560):
    c.lines.append(f"{f} tap A")
# battle: both sides hold UP briefly then fire every 7 s
f = 2900
for k in range(CYCLES):
    for who in (h, c):
        # the guest keeps the aim that lands (63%) so rounds end; the host keeps walking its shots
        if who is h or k == 0:
            for j in range(3):
                who.lines.append(f"{f + j * 8} tap UP")
        who.lines.append(f"{f + 40} tap A")
    if k % 5 == 0 or k == CYCLES - 1:
        h.lines.append(f"{f + 400} shot h_turn{k}")
        c.lines.append(f"{f + 400} shot c_turn{k}")
    f += 420
h.lines.append(f"{f + 60} shot h_end")
c.lines.append(f"{f + 60} shot c_end")
for who, name in ((h, "host"), (c, "guest")):
    who.lines.sort(key=lambda l: int(l.split()[0]))
    who.save(f"{out}/{name}.script")
print(f)
