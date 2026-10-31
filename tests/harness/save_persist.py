"""Saves survive a reboot (regression test for the 2.0 sound ROM, where NitroFS moved the current drive to the ROM
and every save read/write missed the SD card). Two boots share one emulated SD card image:
  boot 1: SETTINGS > WIND: NORMAL -> STRONG (applying a setting writes the save)
  boot 2: open SETTINGS and screenshot it.
Pass: boot 2 shows the same panel as boot 1 after the change, and differs from a fresh card (negative control)."""
import os, subprocess, sys
sys.path.insert(0, "tests/harness")
from scriptgen import Script
from PIL import Image, ImageChops

out = sys.argv[1] if len(sys.argv) > 1 else "build/savetest"
rom = sys.argv[2] if len(sys.argv) > 2 else "terminaltanks.nds"
os.makedirs(out, exist_ok=True)

def settings(change):
    s = Script()
    s.tap("A").wait(90)                       # title -> main menu
    s.taps("DOWN", 11).tap("A").wait(90)      # SETTINGS (12th item)
    s.tap("DOWN").wait(20)                    # WIND row
    if change:
        s.tap("RIGHT").wait(120)              # NORMAL -> STRONG, written to the card
    s.shot("settings")
    return s

def boot(name, change, img):
    d = f"{out}/{name}"
    os.makedirs(d, exist_ok=True)
    settings(change).save(f"{d}/s.script")
    subprocess.run(["build/host/harness", "run", rom, f"{d}/s.script", d, img], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=900)
    return Image.open(f"{d}/settings.ppm").crop((120, 192 + 20, 256, 192 + 34))   # the WIND value

img = os.path.abspath(f"{out}/sd.img")
fresh = os.path.abspath(f"{out}/sd_fresh.img")
for p in (img, fresh):
    if os.path.exists(p): os.remove(p)
a = boot("boot1", True, img)
b = boot("boot2", False, img)
c = boot("fresh", False, fresh)
same = ImageChops.difference(a.convert("RGB"), b.convert("RGB")).getbbox() is None
ctrl = ImageChops.difference(b.convert("RGB"), c.convert("RGB")).getbbox() is not None
print(("OK   " if same else "FAIL ") + "boot 2 shows the setting changed in boot 1")
print(("OK   " if ctrl else "FAIL ") + "a fresh card shows the default (negative control)")
print("save persistence:", "PASS" if same and ctrl else "FAIL")
sys.exit(0 if same and ctrl else 1)
