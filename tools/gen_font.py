#!/usr/bin/env python3
"""Builds source/gen/font_data.c: a 5x8 terminal font for the DS.
ASCII + Latin glyphs come from Spleen 5x8 (BSD-2-Clause, assets/font/); box-drawing and block elements are generated
procedurally so they join seamlessly cell to cell; the game's symbols (coins, credits, hearts, stars, arrows...) are
drawn here by hand at 5x8. Each glyph row is a byte whose bit 4 is the leftmost pixel."""
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
W, H = 5, 8

glyphs = {}

# --- Spleen -------------------------------------------------------------------------------------------------------
src = open(os.path.join(ROOT, "assets", "font", "spleen-5x8.bdf"), encoding="latin-1").read()
for c in src.split("STARTCHAR")[1:]:
    m = re.search(r"^ENCODING (\d+)$", c, re.M)
    if not m:
        continue
    cp = int(m.group(1))
    bbx = list(map(int, re.search(r"^BBX (.*)$", c, re.M).group(1).split()))
    if bbx[0] != 5 or bbx[1] != 8:
        continue
    rows = c.split("BITMAP")[1].split("ENDCHAR")[0].split()
    glyphs[cp] = [(int(r, 16) >> 3) & 0x1F for r in rows]


def art(s):
    rows = [r for r in s.strip("\n").split("\n")]
    assert len(rows) == H, (s, len(rows))
    out = []
    for r in rows:
        r = r.ljust(W, ".")
        v = 0
        for i in range(W):
            if r[i] == "#":
                v |= 1 << (W - 1 - i)
        out.append(v)
    return out


SYM = {
    "◉": ".....\n.###.\n#...#\n#.#.#\n#...#\n.###.\n.....\n.....",
    "◈": "..#..\n.#.#.\n#.#.#\n.#.#.\n..#..\n.....\n.....\n.....",
    "♥": ".....\n##.##\n#####\n#####\n.###.\n..#..\n.....\n.....",
    "♡": ".....\n##.##\n#.#.#\n#...#\n.#.#.\n..#..\n.....\n.....",
    "►": ".....\n#....\n##...\n###..\n##...\n#....\n.....\n.....",
    "◄": ".....\n...#.\n..##.\n.###.\n..##.\n...#.\n.....\n.....",
    "▶": ".....\n#....\n##...\n###..\n##...\n#....\n.....\n.....",
    "◀": ".....\n...#.\n..##.\n.###.\n..##.\n...#.\n.....\n.....",
    "▸": ".....\n.....\n.#...\n.##..\n.#...\n.....\n.....\n.....",
    "▲": ".....\n.....\n..#..\n.###.\n#####\n.....\n.....\n.....",
    "▼": ".....\n.....\n#####\n.###.\n..#..\n.....\n.....\n.....",
    "●": ".....\n.###.\n#####\n#####\n#####\n.###.\n.....\n.....",
    "○": ".....\n.###.\n#...#\n#...#\n#...#\n.###.\n.....\n.....",
    "◦": ".....\n.....\n.###.\n.#.#.\n.###.\n.....\n.....\n.....",
    "•": ".....\n.....\n.##..\n.##..\n.....\n.....\n.....\n.....",
    "★": "..#..\n..#..\n#####\n.###.\n.#.#.\n#...#\n.....\n.....",
    "☆": "..#..\n..#..\n##.##\n.#.#.\n.#.#.\n#...#\n.....\n.....",
    "✦": "..#..\n..#..\n.###.\n#####\n.###.\n..#..\n..#..\n.....",
    "✧": "..#..\n..#..\n.#.#.\n##.##\n.#.#.\n..#..\n..#..\n.....",
    "✛": ".....\n..#..\n..#..\n#####\n..#..\n..#..\n.....\n.....",
    "✓": ".....\n....#\n...#.\n#.#..\n.#...\n.....\n.....\n.....",
    "■": ".....\n.....\n####.\n####.\n####.\n####.\n.....\n.....",
    "□": ".....\n.....\n####.\n#..#.\n#..#.\n####.\n.....\n.....",
    "◆": ".....\n..#..\n.###.\n#####\n.###.\n..#..\n.....\n.....",
    "◇": ".....\n..#..\n.#.#.\n#...#\n.#.#.\n..#..\n.....\n.....",
    "╳": "#...#\n.#.#.\n..#..\n.#.#.\n#...#\n.....\n.....\n.....",
    "—": ".....\n.....\n.....\n#####\n.....\n.....\n.....\n.....",
    "…": ".....\n.....\n.....\n.....\n.....\n.....\n#.#.#\n.....",
    "·": ".....\n.....\n.....\n..#..\n.....\n.....\n.....\n.....",
    "°": ".##..\n#..#.\n.##..\n.....\n.....\n.....\n.....\n.....",
    "⚡": "...#.\n..#..\n.###.\n..#..\n.#...\n.....\n.....\n.....",
    "∙": ".....\n.....\n.....\n..#..\n.....\n.....\n.....\n.....",
    "╱": "....#\n....#\n...#.\n..#..\n..#..\n.#...\n#....\n#....",
    "╲": "#....\n#....\n.#...\n..#..\n..#..\n...#.\n....#\n....#",
    "╵": "..#..\n..#..\n..#..\n..#..\n.....\n.....\n.....\n.....",
    "▂": ".....\n.....\n.....\n.....\n.....\n.....\n#####\n#####",
    "◂": ".....\n.....\n...#.\n..##.\n...#.\n.....\n.....\n.....",
    "✿": ".....\n.#.#.\n#####\n.#.#.\n#####\n.#.#.\n.....\n.....",
    "❀": ".....\n.#.#.\n#.#.#\n.#.#.\n#.#.#\n.#.#.\n.....\n.....",
}
for ch, a in SYM.items():
    glyphs[ord(ch)] = art(a)

# --- procedural box drawing (centre column 2, centre rows 3/4) -----------------------------------------------------
CX, CY = 2, 3


def box(up, down, left, right, heavy=False, double=False, dash=0):
    g = [[0] * W for _ in range(H)]
    def hline(y, x0, x1):
        for x in range(x0, x1 + 1):
            if not dash or (x % 2 == 0):
                g[y][x] = 1
    def vline(x, y0, y1):
        for y in range(y0, y1 + 1):
            if not dash or (y % 2 == 0):
                g[y][x] = 1
    ys = [CY - 1, CY + 1] if double else ([CY, CY + 1] if heavy else [CY])
    xs = [CX - 1, CX + 1] if double else ([CX, CX + 1] if heavy else [CX])
    if left:
        for y in ys:
            hline(y, 0, CX if not double else (xs[0] if (up or down) and not right else CX + 1))
    if right:
        for y in ys:
            hline(y, CX if not double else (xs[0] if (up or down) and not left else CX - 1), W - 1)
    if up:
        for x in xs:
            vline(x, 0, CY if not double else (ys[0] if (left or right) else CY + 1))
    if down:
        for x in xs:
            vline(x, CY if not double else (ys[0] if (left or right) else CY - 1), H - 1)
    return [sum(1 << (W - 1 - x) for x in range(W) if g[y][x]) for y in range(H)]


BOX = {
    "─": (0, 0, 1, 1), "│": (1, 1, 0, 0), "┌": (0, 1, 0, 1), "┐": (0, 1, 1, 0), "└": (1, 0, 0, 1), "┘": (1, 0, 1, 0),
    "├": (1, 1, 0, 1), "┤": (1, 1, 1, 0), "┬": (0, 1, 1, 1), "┴": (1, 0, 1, 1), "┼": (1, 1, 1, 1),
}
for ch, (u, d, l, r) in BOX.items():
    glyphs[ord(ch)] = box(u, d, l, r)
for ch, base in (("╭", "┌"), ("╮", "┐"), ("╰", "└"), ("╯", "┘")):
    glyphs[ord(ch)] = box(*BOX[base])
for ch, base in (("━", "─"), ("┃", "│"), ("┏", "┌"), ("┓", "┐"), ("┗", "└"), ("┛", "┘")):
    glyphs[ord(ch)] = box(*BOX[base], heavy=True)
DOUBLE = {
    "═": ".....\n.....\n#####\n.....\n#####\n.....\n.....\n.....",
    "║": ".#.#.\n.#.#.\n.#.#.\n.#.#.\n.#.#.\n.#.#.\n.#.#.\n.#.#.",
    "╔": ".....\n.....\n.####\n.#...\n.#.##\n.#.#.\n.#.#.\n.#.#.",
    "╗": ".....\n.....\n####.\n...#.\n##.#.\n.#.#.\n.#.#.\n.#.#.",
    "╚": ".#.#.\n.#.#.\n.#.##\n.#...\n.####\n.....\n.....\n.....",
    "╝": ".#.#.\n.#.#.\n##.#.\n...#.\n####.\n.....\n.....\n.....",
}
for ch, a in DOUBLE.items():
    glyphs[ord(ch)] = art(a)
glyphs[ord("┄")] = box(0, 0, 1, 1, dash=1)
glyphs[ord("┈")] = box(0, 0, 1, 1, dash=1)
glyphs[ord("╎")] = box(1, 1, 0, 0, dash=1)
glyphs[ord("┆")] = box(1, 1, 0, 0, dash=1)
# blocks
full = [0x1F] * H
glyphs[ord("█")] = full
glyphs[ord("▀")] = [0x1F] * 4 + [0] * 4
glyphs[ord("▄")] = [0] * 4 + [0x1F] * 4
glyphs[ord("▌")] = [0x1C] * H
glyphs[ord("▐")] = [0x07] * H
glyphs[ord("░")] = [0x15 if y % 2 else 0x0A for y in range(H)]
glyphs[ord("▒")] = [0x15 if y % 2 else 0x0A for y in range(H)]
glyphs[ord("▓")] = [0x1F if y % 2 else 0x15 for y in range(H)]

cps = sorted(cp for cp in glyphs if cp >= 32)
os.makedirs(os.path.join(ROOT, "source", "gen"), exist_ok=True)
with open(os.path.join(ROOT, "source", "gen", "font_data.c"), "w") as f:
    f.write("/* GENERATED by tools/gen_font.py - Spleen 5x8 (BSD-2-Clause, (c) Frederic Cambus) + procedural box drawing\n"
            " * + hand-drawn game symbols. Do not edit. */\n#include <stdint.h>\n")
    f.write(f"const int FONT_N = {len(cps)};\n")
    f.write("const uint16_t FONT_CP[] = {" + ",".join(str(c) for c in cps) + "};\n")
    f.write("const uint8_t FONT_BITS[][8] = {\n")
    for c in cps:
        f.write("  {" + ",".join(str(v) for v in glyphs[c]) + "},\n")
    f.write("};\n")
print("glyphs", len(cps))
