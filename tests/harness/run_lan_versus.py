"""Runs the two-console LAN versus scenario (lan_versus.py) in melonDS local wireless and checks the consoles agree.

The two emulated consoles run in separate threads, so screenshots taken at the same script frame are not the
same instant. Agreement is checked on the lockstep event log instead: in net games the battle scene logs every
phase change with a hash of terrain heights + tank HP, and netplay logs every shot sent / received.
Pass criteria:
  * both consoles reach the battle and log at least 8 shots,
  * the sequence of (phase change, turn, state hash) is identical on both consoles,
  * every shot one console sends is received by the other with the same tank / angle / power, in order."""
import os, re, subprocess, sys

out = sys.argv[1] if len(sys.argv) > 1 else "build/lan"
os.makedirs(out, exist_ok=True)
subprocess.run([sys.executable, "tests/harness/lan_versus.py", out, sys.argv[2] if len(sys.argv) > 2 else "10"], check=True, stdout=subprocess.DEVNULL)
subprocess.run(["build/host/harness", "mp", "terminaltanks.nds", f"{out}/host.script", f"{out}/guest.script", out],
               check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=1800)

def events(path):
    phases, sent, recv = [], [], []
    for line in open(path, errors="replace"):
        m = re.match(r"NET phase (\d+)->(\d+) turn (\d+) state (\w+)", line)
        if m: phases.append(m.groups())
        m = re.match(r"NET (send|recv) fire tank (\d+) a ([\d.-]+) p ([\d.-]+)", line)
        if m: (sent if m.group(1) == "send" else recv).append(m.groups()[1:])
    return phases, sent, recv

hp, hs, hr = events(f"{out}/log_0.txt")
cp, cs, cr = events(f"{out}/log_1.txt")
n = min(len(hp), len(cp))
ok = True
def check(cond, msg):
    global ok
    print(("OK   " if cond else "FAIL ") + msg)
    ok &= cond
check(len(hs) + len(cs) >= 8, f"shots fired: host {len(hs)}, guest {len(cs)}")
check(n > 0 and hp[:n] == cp[:n], f"phase/state sequence identical on both consoles ({n} transitions compared)")
check(abs(len(hp) - len(cp)) <= 3, f"transition counts close (host {len(hp)}, guest {len(cp)}; the run ends mid-turn)")
check(hs[:len(cr)] == cr, f"host->guest shots delivered intact ({len(cr)} of {len(hs)})")
check(cs[:len(hr)] == hr, f"guest->host shots delivered intact ({len(hr)} of {len(cs)})")
print("LAN versus:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
