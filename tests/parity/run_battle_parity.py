#!/usr/bin/env python3
"""End-to-end battle parity: AI vs AI rounds driven frame by frame (dt = 1/30) through the ORIGINAL BattleScene
(headless, no drawing) and through the native core. Snapshots at every turn (AI plan + full state) and every
destruction must be identical, as must the round results."""
import os, random, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools"))
import ttref  # noqa
tt = ttref.load()
PROBE = os.path.join(HERE, "..", "..", "build", "host", "probe")
D, A = list(tt.ALL_DESIGNS), list(tt.ALL_AMMOS) + list(tt.MAGE_AMMO.values())
DIFFS = [tt.Difficulty.EASY, tt.Difficulty.NORMAL, tt.Difficulty.HARD, tt.Difficulty.MASTER]


class FakeScreen:
    cols, rows = 128, 46


class FakeApp:
    def __init__(self, wind):
        self.settings = tt.Settings(wind=wind)
        self.save = tt.SaveData(None)
        self.screen = FakeScreen()
        self.time = 0.0
        self.rng = random.Random(1)
        self.pushed = []
        class FB:
            def emit(self, k): pass
        self.feedback = FB()
        class Quiet:                                   # 2.0: scenes talk to the audio engine; the test needs silence
            def __getattr__(self, name):
                return lambda *a, **k: None
        self.audio = Quiet()
    def __getattr__(self, name):                       # any other 2.0 app hook (sfx, music, toasts...) is a no-op
        return lambda *a, **k: None
    def push(self, s): self.pushed.append(s)
    def goto(self, s): pass
    def apply_settings(self): pass


def py_run(seed, wind, da, db, dA, aA, dB, aB, fx, sp, frames):
    app = FakeApp(wind)
    pls = [tt.PlayerSetup("P0", "X", tt.COLOR_CHOICES[0][1], tt.COLOR_CHOICES[4][1], True, D[dA].key, A[aA].key, 3, diff=DIFFS[da]),
           tt.PlayerSetup("P1", "X", tt.COLOR_CHOICES[1][1], tt.COLOR_CHOICES[5][1], True, D[dB].key, A[aB].key, 3, diff=DIFFS[db])]
    cfg = tt.MatchConfig(pls, True, tt.Difficulty.NORMAL, "single", target=2, ammo_fx=bool(fx), special=bool(sp))
    sess = tt.MatchSession(cfg, None, seed)
    out = []
    rounds = 0
    sc = tt.BattleScene(app, sess)
    sc.enter()
    last = None
    for f in range(frames):
        app.time += 1 / 30
        sc.update(1 / 30)
        ph = sc.phase
        if ph is not last:
            last = ph
            if ph in (tt.Phase.AI_THINKING, tt.Phase.DESTRUCTION):
                w = sc.world
                hs = 0
                for h in w.terrain.heights:
                    hs = (hs * 31 + h) & ((1 << 64) - 1)
                hs = hs - (1 << 64) if hs >= 1 << 63 else hs
                pid = {tt.Phase.AI_THINKING: 2, tt.Phase.DESTRUCTION: 6}[ph]
                line = "S %d %d %r %d" % (pid, sc.turn, w.wind, hs) + "".join(" %r %r %d" % (t.x, t.y, t.hp) for t in w.tanks)
                if ph is tt.Phase.AI_THINKING:
                    line += " %r %r" % (float(sc.plan.angle), float(sc.plan.power))
                out.append(line)
        if app.pushed:
            out.append("R %d %d %d" % (sess.round_no, sess.wins[0], sess.wins[1]))
            app.pushed.clear()
            rounds += 1
            if sess.over or rounds >= 3:
                break
            sess.advance()
            sc = tt.BattleScene(app, sess)
            sc.enter()
            last = None
    return out


def c_run(*a):
    return subprocess.run([PROBE, "battle"] + [str(x) for x in a], capture_output=True, text=True, check=True).stdout.split("\n")[:-1]


def same(a, b):
    ta, tb = a.split(), b.split()
    return len(ta) == len(tb) and ta[0] == tb[0] and all(float(x) == float(y) for x, y in zip(ta[1:], tb[1:]))


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 and sys.argv[1].isdigit() else 6
    r = random.Random(7)
    mech = [i for i, a in enumerate(tt.ALL_AMMOS) if a.mech]
    # `new`: one match per 2.0 mechanic (seek, pierce, mirror, push, delayed clusters, burrow, slither, stars, volley),
    # both tanks firing it, special-ammo effects on
    new = [i for i, a in enumerate(tt.ALL_AMMOS) if set(dict(a.mech)) & {"seek", "pierce", "push", "cdelay", "burrow", "slither", "stars", "volley"}
           or dict(a.mech).get("apex", ("",))[0] == "mirror"]
    only_new = "new" in sys.argv
    if only_new:
        n = len(new)
    fails = 0
    for case in range(n):
        args = (r.randrange(1 << 30), r.choice((0.0, 3.0, 6.0, 10.0)), r.randrange(3), r.randrange(4), r.randrange(len(D)),
                r.choice(mech), r.randrange(len(D)), r.choice(mech), int(r.random() < 0.8), int(r.random() < 0.3), 30000)
        if only_new:
            args = args[:5] + (new[case],) + args[6:7] + (new[case],) + (1,) + args[9:]
        a, b = py_run(*args), c_run(*args)
        ok = len(a) == len(b) and all(same(x, y) for x, y in zip(a, b))
        print(f"case {case}: {len(a)} snapshots, {'identical' if ok else 'MISMATCH'}")
        if not ok:
            fails += 1
            for i, (x, y) in enumerate(zip(a, b)):
                if not same(x, y):
                    print("  at", i, "\n  py:", x[:240], "\n  c :", y[:240])
                    break
            else:
                print("  lengths", len(a), len(b), a[-1:], b[-1:])
    print(f"battle parity: {n - fails}/{n} matches identical")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
