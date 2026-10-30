#!/usr/bin/env python3
"""Visual check of content on the real ROM in a headless DS emulator (NooDS, tools: a dsshot-style
interactive driver). Builds a test ROM with a fixed map / tank / ammo / paint (TT_DEBUG_* hooks, everything unlocked),
starts a single-player battle, saves the battlefield, fires and saves frames of the shot in flight and the impact.
    python3 tests/visual/shoot.py DSSHOT OUTDIR NAME [map=I] [tank=I] [ammo=I] [paint=I] [home=I] [frames=N]"""
import os, subprocess, sys, tempfile
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]


class DS:
    def __init__(self, rom, binary):
        self.tmp = tempfile.TemporaryDirectory()
        self.p = subprocess.Popen([binary, str(rom), "-", self.tmp.name], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
        self.n = 0

    def cmd(self, line):
        self.p.stdin.write(line + "\n"); self.p.stdin.flush(); self.p.stdout.readline()

    def run(self, n): self.cmd(f"run {n}")

    def press(self, key, after=30, hold=6):
        self.cmd(f"press {key}"); self.run(hold); self.cmd(f"release {key}"); self.run(after)

    def image(self):
        self.n += 1
        self.cmd(f"shot c{self.n}")
        return Image.open(Path(self.tmp.name) / f"c{self.n}.ppm").convert("RGB")

    def wait_for(self, test, step=20, limit=3000):
        for _ in range(limit // step):
            im = self.image()
            if test(im):
                return im
            self.run(step)
        raise RuntimeError("timed out")

    def close(self):
        try:
            self.cmd("quit")
        except Exception:
            pass
        self.p.wait(timeout=10); self.tmp.cleanup()


def count(im, box, fn):
    return sum(1 for px in im.crop(box).get_flattened_data() if fn(px))
def press_a(im): return count(im, (90, 250, 170, 268), lambda p: p[0] > 200 and p[1] > 200 and p[2] > 200) > 20
def menu_list(im): return count(im, (60, 205, 200, 218), lambda p: p[0] > 220 and p[1] > 220 and p[2] > 220) > 30
def fire_ready(im): return count(im, (152, 219, 244, 312), lambda p: p[0] > 180 and p[1] < 90 and p[2] < 90) > 150


def build(defs):
    tag = "_".join(f"{k}{v}" for k, v in sorted(defs.items())) or "plain"
    rom = ROOT / "build" / "visual" / f"tt_{tag}.nds"
    if not rom.exists():
        flags = " ".join(f"-DTT_DEBUG_{k.upper()}={v}" for k, v in defs.items()) + " -DTT_DEBUG_HOME=" + str(defs.get("home", 0))
        env = dict(os.environ, WONDERFUL_TOOLCHAIN=os.environ.get("WONDERFUL_TOOLCHAIN", "/opt/wonderful"),
                   BLOCKSDS=os.environ.get("BLOCKSDS", "/root/deps/blocksds"))
        bd = f"build/visual/{tag}"
        subprocess.run(["make", "-s", "-j8", f"BUILDDIR={bd}", f"ELF={bd}/tt.elf", f"MAP={bd}/tt.map", f"ROM={rom}", f"EXTRA_CFLAGS={flags}"],
                       cwd=ROOT, env=env, check=True, stdout=subprocess.DEVNULL)
    return rom


def main():
    dsshot, out, name = sys.argv[1], Path(sys.argv[2]), sys.argv[3]
    kv = dict(a.split("=") for a in sys.argv[4:])
    frames = int(kv.pop("frames", 0))
    step = int(kv.pop("step", 3))
    skip = int(kv.pop("skip", 0))
    battle = kv.pop("battle", "1") == "1"
    defs = {"map" if k == "map" else k: int(v) for k, v in kv.items()}
    out.mkdir(parents=True, exist_ok=True)
    rom = build(defs)
    for attempt in range(4):
        ds = DS(rom, dsshot)
        try:
            ds.run(300); ds.wait_for(press_a)
            ds.press("A", after=10)
            menu = ds.wait_for(menu_list, step=10)
            if not battle:
                ds.run(60); ds.image().save(out / f"{name}.png"); ds.close(); return 0
            ds.press("A", after=130)
            ds.press("A", after=150)
            ds.press("UP", after=20); ds.press("A", after=60)
            im = None
            for _ in range(6):
                try:
                    im = ds.wait_for(fire_ready, step=30, limit=600); break
                except RuntimeError:
                    ds.press("A", after=30)
            if im is None:
                raise RuntimeError("no battle")
        except RuntimeError as e:
            print("retry:", e); ds.close(); continue
        ds.run(170); ds.image().save(out / f"{name}_field.png")
        if frames:
            ds.press("A", after=6, hold=10)
            if fire_ready(ds.image()):                       # still aiming: tap FIRE on the touch screen
                ds.cmd("touch 198 73"); ds.run(12); ds.cmd("untouch"); ds.run(4)
            shots = []
            ds.run(skip) if skip else None
            for i in range(frames):
                ds.run(step); shots.append(ds.image())
            w = 256 * min(len(shots), 6)
            # contact sheet: top screens only, 6 per row
            tops = [s.crop((0, 0, 256, 192)) for s in shots]
            rows = (len(tops) + 5) // 6
            sheet = Image.new("RGB", (256 * 6, 192 * rows))
            for i, t in enumerate(tops):
                sheet.paste(t, ((i % 6) * 256, (i // 6) * 192))
            sheet.save(out / f"{name}_shot.png")
        ds.close()
        print("ok", name)
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
