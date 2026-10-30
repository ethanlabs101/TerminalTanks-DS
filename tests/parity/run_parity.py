#!/usr/bin/env python3
"""Parity harness: drives the Python reference and the native core (tests/parity/probe) with identical inputs and
compares results. Exit code != 0 on any mismatch. Usage: python3 tests/parity/run_parity.py [cases]"""
import os, random, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools"))
import ttref  # noqa
tt = ttref.load()
PROBE = os.path.join(HERE, "..", "..", "build", "host", "probe")
DESIGNS = list(tt.ALL_DESIGNS)
AMMOS = list(tt.ALL_AMMOS) + list(tt.MAGE_AMMO.values())
MAPS = list(tt.MAPS) + list(tt.SPECIAL_MAPS)
TERR = {"blast": 0, "mound": 1, "trench": 2, "glass": 3, "fx": 4}
KIND = {tt.ImpactKind.TERRAIN: 1, tt.ImpactKind.TANK: 2, tt.ImpactKind.OUT: 3}


def py_case(mi, seed, wind, players, shots):
    st = tt.Settings(wind=wind)
    pls = []
    for i, (d, a, team, scale, hover) in enumerate(players):
        pls.append(tt.PlayerSetup(f"P{i}", "X", tt.COLOR_CHOICES[i][1], tt.COLOR_CHOICES[i + 4][1], False, DESIGNS[d].key, AMMOS[a].key, 3,
                                  team=team, scale=scale, hover=hover, kind="ufo" if scale != 1.0 else ""))
    w = tt.BattleWorld(128, 80, MAPS[mi], seed, pls, st)
    out = ["H " + " ".join(str(h) for h in w.terrain.heights), "W %r" % w.wind]
    for t in w.tanks:
        out.append("T %r %r %d %r" % (t.x, t.y, t.facing, float(t.angle)))
    out.append("S %d %d %r" % (len(w.scenery.stars), len(w.scenery.motes), w.streaks[-1][1]))
    for sh, ang, pw, fx, sp in shots:
        w.ammo_fx = bool(fx)
        w.tanks[sh].angle, w.tanks[sh].power = ang, pw
        fl = tt.plan_shot(w, w.tanks[sh], bool(sp))
        out.append("P %d" % len(fl))
        for f in fl:
            imp = f.trace.impact
            out.append("F %r %r %d %d %r %r %d" % (float(imp.x), float(imp.y), KIND[imp.kind], -1 if imp.tank is None else imp.tank,
                                                  float(f.trace.time), float(f.delay), len(f.events)))
            for e in f.events:
                out.append("E %r %r %d %d %r %d %d %r %d %r %d %d" % (float(e.x), float(e.y), KIND[e.kind], -1 if e.tank is None else e.tank,
                                                                     float(e.radius), e.dmg, TERR[e.mode], float(e.delay), e.first, float(e.pull), e.multi, e.big))
    return out


def c_case(mi, seed, wind, players, shots):
    args = [PROBE, "world", str(mi), str(seed), repr(float(wind)), str(len(players))]
    for d, a, team, scale, hover in players:
        args += [str(d), str(a), str(team), repr(float(scale)), repr(float(hover))]
    for sh, ang, pw, fx, sp in shots:
        args += [str(sh), repr(float(ang)), repr(float(pw)), str(fx), str(sp)]
    return subprocess.run(args, capture_output=True, text=True, check=True).stdout.split("\n")[:-1]


def same(a, b):
    ta, tb = a.split(), b.split()
    if len(ta) != len(tb) or ta[0] != tb[0]:
        return False
    for x, y in zip(ta[1:], tb[1:]):
        if float(x) != float(y):
            return False
    return True


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 300
    r = random.Random(4242)
    mech = [i for i, a in enumerate(AMMOS) if a.mech]
    fails = 0
    for case in range(n):
        mi = r.randrange(len(MAPS))
        seed = r.randrange(1 << 30)
        wind = r.choice((0.0, 3.0, 6.0, 10.0))
        if case % 10 == 9:   # UFO / multi-tank layouts
            players = [(0, 0, 0, 1.0, 0.0), (r.randrange(len(DESIGNS)), 0, 1, 3.0, 14.0), (1, 0, 1, 2.0, 10.0)]
        elif case % 7 == 3:
            players = [(r.randrange(len(DESIGNS)), r.choice(mech), 0, 1.0, 0.0), (r.randrange(len(DESIGNS)), 0, 0, 1.0, 0.0),
                       (r.randrange(len(DESIGNS)), 0, 1, 1.0, 0.0), (r.randrange(len(DESIGNS)), 0, 1, 1.0, 0.0)]
        else:
            players = [(r.randrange(len(DESIGNS)), r.choice(mech) if r.random() < 0.85 else r.randrange(len(AMMOS)), 0, 1.0, 0.0),
                       (r.randrange(len(DESIGNS)), r.randrange(len(AMMOS)), 1, 1.0, 0.0)]
        shots = []
        for _ in range(4):
            shots.append((0, r.uniform(2, 178), r.uniform(10, 100), int(r.random() < 0.85), int(r.random() < 0.1)))
        a = py_case(mi, seed, wind, players, shots)
        b = c_case(mi, seed, wind, players, shots)
        if len(a) != len(b) or not all(same(x, y) for x, y in zip(a, b)):
            fails += 1
            if fails <= 3:
                print(f"MISMATCH case {case}: map {mi} seed {seed} players {players}")
                for x, y in zip(a, b):
                    if not same(x, y):
                        print("  py:", x[:200]); print("  c :", y[:200]); break
                if len(a) != len(b):
                    print("  line counts", len(a), len(b))
    print(f"world+shot parity: {n - fails}/{n} cases identical")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
