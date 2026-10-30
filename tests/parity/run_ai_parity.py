#!/usr/bin/env python3
"""AI parity: identical plans (angle, power, intended solution, think time, status text) and learning updates."""
import os, random, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools"))
import ttref  # noqa
tt = ttref.load()
PROBE = os.path.join(HERE, "..", "..", "build", "host", "probe")
MAPS = list(tt.MAPS) + list(tt.SPECIAL_MAPS)
DIFFS = [tt.Difficulty.EASY, tt.Difficulty.NORMAL, tt.Difficulty.HARD, tt.Difficulty.MASTER]


def py_case(mi, seed, wind, diff, aseed, prec, obs):
    st = tt.Settings(wind=wind)
    D = list(tt.ALL_DESIGNS)
    pls = [tt.PlayerSetup("P0", "X", tt.COLOR_CHOICES[0][1], tt.COLOR_CHOICES[4][1], False, D[0].key, "standard", 3, team=0),
           tt.PlayerSetup("P1", "X", tt.COLOR_CHOICES[1][1], tt.COLOR_CHOICES[5][1], True, D[3].key, "standard", 3, team=1)]
    w = tt.BattleWorld(128, 80, MAPS[mi], seed, pls, st)
    ai = tt.AIController(DIFFS[diff], random.Random(aseed), prec)
    out = ["G %r" % ai.gravity_bias]
    for dx, hit in obs:
        p = ai.plan(w.tanks[1], w.tanks[0], w)
        out.append("A %r %r %r %r %r %r" % (float(p.angle), float(p.power), float(p.intended_rel), float(p.intended_power), float(p.think_time), float(p.prob)))
        out.append("Q " + p.status)
        imp = tt.Impact(w.tanks[0].x + dx, 20.0, tt.ImpactKind.TERRAIN)
        ai.observe(p, imp, w.tanks[0], bool(hit))
        out.append("O %r %r" % (float(ai.gravity_bias), float(ai.correction)))
        w.drift_wind()
    return out


def c_case(mi, seed, wind, diff, aseed, prec, obs):
    args = [PROBE, "ai", str(mi), str(seed), repr(float(wind)), str(diff), str(aseed), repr(float(prec)), str(len(obs))]
    for dx, hit in obs:
        args += [repr(float(dx)), str(hit)]
    return subprocess.run(args, capture_output=True, text=True, check=True).stdout.split("\n")[:-1]


def same(a, b):
    if a.startswith("Q"):
        return a == b
    ta, tb = a.split(), b.split()
    return len(ta) == len(tb) and all(float(x) == float(y) for x, y in zip(ta[1:], tb[1:]))


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 24
    r = random.Random(99)
    fails = 0
    for case in range(n):
        mi, seed, wind = r.randrange(len(MAPS)), r.randrange(1 << 30), r.choice((0.0, 3.0, 6.0, 10.0))
        diff, aseed, prec = case % 4, r.randrange(1 << 30), r.choice((1.0, 0.78, 1.25, 1.4))
        obs = [(r.uniform(-30, 30), int(r.random() < 0.3)) for _ in range(3)]
        a, b = py_case(mi, seed, wind, diff, aseed, prec, obs), c_case(mi, seed, wind, diff, aseed, prec, obs)
        if len(a) != len(b) or not all(same(x, y) for x, y in zip(a, b)):
            fails += 1
            print(f"MISMATCH case {case} diff {diff}")
            for x, y in zip(a, b):
                if not same(x, y):
                    print("  py:", x); print("  c :", y); break
    print(f"AI parity: {n - fails}/{n} cases identical")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
