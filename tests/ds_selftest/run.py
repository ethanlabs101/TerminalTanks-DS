#!/usr/bin/env python3
"""Runs the DS self-test ROM in headless melonDS and compares the ARM9's probe output with the host probe's
(build/ds_selftest/expected.txt from gen_cases.py). Numbers are compared as values (float('...') ==), other
tokens as text. Usage: run.py [frames]   (needs: make selftest, scripts/build_harness.sh)"""
import os, subprocess, sys
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
OUT = os.path.join(ROOT, "build", "ds_selftest")
frames = int(sys.argv[1]) if len(sys.argv) > 1 else 40000
open(os.path.join(OUT, "run.script"), "w").write(f"{frames} end\n")
subprocess.run([os.path.join(ROOT, "build", "host", "harness"), "run", os.path.join(ROOT, "build", "selftest.nds"),
                os.path.join(OUT, "run.script"), OUT], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

def cases(text):
    out, cur = [], []
    for line in text.splitlines():
        if line == "CASE_END":
            out.append(cur); cur = []
        elif line == "SELFTEST_DONE":
            break
        else:
            cur.append(line)
    return out

def tok_same(a, b):
    if a == b:
        return True
    try:
        return float(a) == float(b)
    except ValueError:
        return False

log = open(os.path.join(OUT, "log_0.txt"), errors="replace").read()
begin = log.find("SELFTEST_BEGIN\n")
ds = cases(log[begin + len("SELFTEST_BEGIN\n"):] if begin >= 0 else "")
host = cases(open(os.path.join(OUT, "expected.txt")).read())
done = "SELFTEST_DONE" in log
fails = 0
for i, h in enumerate(host):
    if i >= len(ds):
        break
    d = ds[i]
    same = len(d) == len(h) and all(len(x.split()) == len(y.split()) and all(tok_same(p, q) for p, q in zip(x.split(), y.split())) for x, y in zip(d, h))
    if not same:
        fails += 1
        for x, y in zip(d, h):
            if x != y:
                print(f"case {i} differs:\n  ds  : {x[:200]}\n  host: {y[:200]}")
                break
        else:
            print(f"case {i} differs in length: ds {len(d)} lines, host {len(h)} lines")
print(f"DS vs host probe: {len(ds)}/{len(host)} cases ran on the ARM9{'' if done else ' (did not finish: raise frames)'}, {len(ds) - fails} identical, {fails} different")
sys.exit(0 if done and fails == 0 and len(ds) == len(host) else 1)
