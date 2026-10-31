#!/usr/bin/env python3
"""Renders the PC game's soundtrack and sound effects for the DS.

The PC game synthesizes everything at runtime in Python; the DS can't, so this runs the ORIGINAL mixer from reference/
(unmodified) and stores what it plays:

  nitrofiles/audio/music.pak   every song, mono IMA-ADPCM at 16.5 kHz in 2 KB blocks, with a seamless loop: the render
                               keeps going past the song's end with the PC's own loop (back to section A), so the notes
                               still ringing at the loop are in the data; the DS jumps from loop_end back to loop_start
                               (the same moment of the song, one pass earlier).
  nitrofiles/audio/sfx.pak     every named effect plus each ammo's launch and blast, hardware IMA-ADPCM.
  source/gen/audio_data.c/.h   song and effect ids, and which song plays where (home screens, battlefields, bosses...).

Rendering takes a while (about 7.6 hours of music), so each song is cached in build/audio_cache by a signature of its
recipe; a rebuild only renders what changed.
Run:  python3 tools/gen_audio.py [-j N] [--only SUBSTR]
"""
import argparse
import ctypes
import hashlib
import json
import math
import os
import struct
import subprocess
import sys
import time
from multiprocessing import Pool

import numpy as np
from scipy.signal import resample_poly

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
sys.path.insert(0, HERE)
GEN_VER = "a4"                          # bump to re-render everything
SR_IN = 22050
UP, DOWN = 3, 4                         # 22050 * 3/4 = 16537.5 Hz; the DS plays it at 16542 Hz (0.5 cent sharp)
BLK_S, BLK_B = 4088, 2048
TAIL_S = 4.0                            # seconds of ringing notes kept across the loop point
PC_MASTER, PC_MUSIC, PC_SFX = 0.85, 0.7, 0.9   # the PC game's default volumes (Settings)
CACHE = os.path.join(ROOT, "build", "audio_cache")
OUT_NITRO = os.path.join(ROOT, "nitrofiles", "audio")
OUT_GEN = os.path.join(ROOT, "source", "gen")

_lib = None


def lib():
    global _lib
    if _lib is None:
        so = os.path.join(ROOT, "build", "ima_lib.so")
        src = os.path.join(HERE, "ima_lib.c")
        if not os.path.exists(so) or os.path.getmtime(so) < max(os.path.getmtime(src), os.path.getmtime(os.path.join(ROOT, "core", "ima.h"))):
            os.makedirs(os.path.dirname(so), exist_ok=True)
            subprocess.check_call(["gcc", "-O2", "-shared", "-fPIC", "-o", so, src])
        _lib = ctypes.CDLL(so)
    return _lib


def ref():
    import ttref
    ttref.load()
    import tanks.audio.director as director
    import tanks.audio.mixer as mixer
    import tanks.audio.notebank as notebank
    import tanks.audio.sfx as sfx
    import tanks.audio.synth as synth
    import tanks.content.songs as songs
    return director, mixer, notebank, sfx, synth, songs


def to16(x):
    return np.clip(np.round(x), -32767, 32767).astype(np.int16)


def song_sig(sid):
    director, mixer, notebank, sfx, synth, songs = ref()
    s = mixer._song_sig(sid)
    return hashlib.sha1(f"{GEN_VER}|{sid}|{s}|{PC_MASTER}|{PC_MUSIC}".encode()).hexdigest()[:16]


def render_song(sid):
    """-> (ima bytes, loop_end, loop_start) using the PC mixer itself."""
    path = os.path.join(CACHE, f"{sid}_{song_sig(sid)}.bin")
    if os.path.exists(path):
        with open(path, "rb") as f:
            le, ls = struct.unpack("<II", f.read(8))
            return sid, f.read(), le, ls, 0.0
    t0 = time.time()
    director, mixer, notebank, sfx, synth, songs = ref()
    mx = mixer.Mixer(cache=False)
    mx.vol = {"m": PC_MASTER, "mus": PC_MUSIC, "sfx": PC_SFX}
    mx.load_song(sid)
    while mx.partial:
        mx._fill(sid)
    mx.command(f"music {sid} 0.05")
    song = notebank.song_of(sid)
    T, a = song.total, song.secs.get("A", 0)
    C = mixer.AU_CHUNK
    R = -(-T // C) * C                         # the mixer restarts at the first chunk boundary at or after the end
    r = UP / DOWN
    Lo = int(math.ceil((a + TAIL_S * SR_IN) * r / BLK_S)) * BLK_S
    if Lo / r < T and a < T:
        E = Lo + int(round((R - a) * r))
    else:                                      # too short to loop past the intro: loop the whole thing
        Lo, E = 0, int(round(R * r))
    need = int(math.ceil(E / r)) + 4096
    chunks = []
    n = 0
    while n < need:
        chunks.append(mx.render())
        n += C
    pcm = np.frombuffer(b"".join(chunks), dtype="<i2").astype(np.float64)
    mono = (pcm[0::2] + pcm[1::2]) * 0.5
    out = resample_poly(mono, UP, DOWN)
    nb = -(-E // BLK_S)
    buf = np.zeros(nb * BLK_S)
    k = min(len(out), nb * BLK_S)
    buf[:k] = out[:k]
    F = min(1024, Lo, E - Lo)                  # blend the last moments before the jump into what precedes loop_start,
    if F > 0:                                  # so the sample after the jump continues the waveform (no click)
        w = (np.arange(F) + 1) / F
        buf[E - F:E] = buf[E - F:E] * (1 - w) + buf[Lo - F:Lo] * w
    s16 = np.ascontiguousarray(to16(buf))
    enc = ctypes.create_string_buffer(nb * BLK_B)
    lib().enc_blocks(s16.ctypes.data_as(ctypes.c_void_p), nb, enc)
    data = enc.raw
    os.makedirs(CACHE, exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(struct.pack("<II", E, Lo) + data)
    os.replace(tmp, path)
    return sid, data, E, Lo, time.time() - t0


def _sfx_sig():
    src = b"".join(open(os.path.join(ROOT, "reference", "tanks", *p), "rb").read()
                   for p in (("audio", "sfx.py"), ("audio", "synth.py"), ("content", "ammo.py")))
    return hashlib.sha1(GEN_VER.encode() + src).hexdigest()[:16]


def build_sfx(name):
    path = os.path.join(CACHE, "sfx", f"{name.replace(':', '_')}_{_sfx_sig()}.bin")
    if os.path.exists(path):
        with open(path, "rb") as f:
            return name, f.read()
    name, data = _build_sfx(name)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path + ".tmp", "wb") as f:
        f.write(data)
    os.replace(path + ".tmp", path)
    return name, data


def _build_sfx(name):
    director, mixer, notebank, sfx, synth, songs = ref()
    x = np.array(synth.au_finish(sfx.sfx_build(name)), dtype=np.float64)
    x = np.clip(x, -1, 1) * 30000
    y = resample_poly(x, UP, DOWN)
    n = len(y) + (len(y) & 1)
    n = max(8, -(-n // 8) * 8)                 # whole words of nibbles
    s16 = np.zeros(n, dtype=np.int16)
    s16[:len(y)] = to16(y)
    s16 = np.ascontiguousarray(s16)
    enc = ctypes.create_string_buffer(4 + n // 2)
    lib().enc_hw(s16.ctypes.data_as(ctypes.c_void_p), n, enc)
    return name, enc.raw


def cident(s):
    return "".join(c if c.isalnum() else "_" for c in s).upper()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-j", type=int, default=os.cpu_count() or 1)
    ap.add_argument("--tables", action="store_true", help="only rewrite source/gen/audio_data.c/.h (no rendering, packs untouched)")
    ap.add_argument("--only", default="", help="render only songs whose id contains this (for testing); the pack still lists every song")
    args = ap.parse_args()
    director, mixer, notebank, sfx, synth, songs = ref()
    import ttref
    tt = ttref.load()
    SONGS = list(songs.SONGS)
    SIDX = {s: i for i, s in enumerate(SONGS)}

    # ---- effects: named ones, then fire:/hit: for every ammo in the DS's AMMOS order
    all_ammo = list(tt.ALL_AMMOS) + list(tt.MAGE_AMMO.values())
    names = list(sfx.SFX_RECIPES) + ["fire:" + a.key for a in all_ammo] + ["hit:" + a.key for a in all_ammo]
    if not args.tables:
        render_packs(args, SONGS, names)
    write_tables(director, sfx, songs, tt, SONGS, SIDX, all_ammo, names)


def render_packs(args, SONGS, names):
    t0 = time.time()
    with Pool(args.j) as p:
        fx = dict(p.map(build_sfx, names))
    print(f"sfx: {len(names)} effects, {sum(len(v) for v in fx.values()) / 1024:.0f} KB ({time.time() - t0:.0f}s)", flush=True)

    # ---- music
    want = [s for s in SONGS if args.only in s] if args.only else SONGS
    res = {}
    t0 = time.time()
    with Pool(args.j) as p:
        for i, (sid, data, le, ls, dt) in enumerate(p.imap_unordered(render_song, want)):
            res[sid] = (data, le, ls)
            if dt:
                print(f"  {i + 1:3d}/{len(want)} {sid:28s} {le / 16537.5:6.1f}s loop@{ls / 16537.5:5.1f}s  {dt:.1f}s", flush=True)
    print(f"music: {len(res)} songs ({time.time() - t0:.0f}s)", flush=True)

    os.makedirs(OUT_NITRO, exist_ok=True)
    # music.pak: magic, count, then count * MusEntry, then the blocks (songs that were not rendered get an empty entry)
    hdr = 8 + 16 * len(SONGS)
    body, ents = bytearray(), []
    for s in SONGS:
        if s in res:
            data, le, ls = res[s]
            ents.append((hdr + len(body), len(data) // BLK_B, le, ls))
            body += data
        else:
            ents.append((0, 0, 0, 0))
    with open(os.path.join(OUT_NITRO, "music.pak"), "wb") as f:
        f.write(struct.pack("<II", 0x554D5454, len(SONGS)))
        for e in ents:
            f.write(struct.pack("<IIII", *e))
        f.write(body)
    hdr = 8 + 8 * len(names)
    body, ents = bytearray(), []
    for nm in names:
        d = fx[nm]
        ents.append((hdr + len(body), len(d)))
        body += d + b"\0" * (-len(d) % 4)
    with open(os.path.join(OUT_NITRO, "sfx.pak"), "wb") as f:
        f.write(struct.pack("<II", 0x46535454, len(names)))
        for e in ents:
            f.write(struct.pack("<II", *e))
        f.write(body)
    print(f"packs: music {os.path.getsize(os.path.join(OUT_NITRO, 'music.pak')) / 1e6:.1f} MB, sfx {os.path.getsize(os.path.join(OUT_NITRO, 'sfx.pak')) / 1e6:.2f} MB")


def write_tables(director, sfx, songs, tt, SONGS, SIDX, all_ammo, names):
    H, C = [], []
    H.append("/* Generated by tools/gen_audio.py from reference/ -- do not edit. */\n#ifndef TT_AUDIO_DATA_H\n#define TT_AUDIO_DATA_H")
    H.append(f"#define N_SONGS {len(SONGS)}")
    H.append("enum {\n" + "\n".join(f"    SONG_{cident(s)} = {i}," for i, s in enumerate(SONGS)) + "\n};")
    named = list(sfx.SFX_RECIPES)
    H.append("enum {\n" + "\n".join(f"    SFX_{cident(n)} = {i}," for i, n in enumerate(named)) + "\n};")
    H.append(f"#define SFX_FIRE_FIRST {len(named)}\n#define SFX_HIT_FIRST {len(named) + len(all_ammo)}\n#define N_SFX {len(names)}")
    H.append("extern const char *const SONG_NAMES[N_SONGS];\nextern const unsigned char SONG_GROUP[N_SONGS];")
    H.append("extern const char *const SONG_DESC[N_SONGS];\nextern const char *const SONG_INFO[N_SONGS];   /* \"120 BPM \xC2\xB7 4/4 \xC2\xB7 STYLE\" */")
    H.append("#define N_SONG_GROUPS %d\nextern const char *const SONG_GROUP_NAMES[N_SONG_GROUPS];" % len(director.JUKEBOX_GROUPS))
    H.append("extern const short HOME_SONG[];\nextern const short MAP_SONG[];\nextern const short TOURN_SONG[20];\nextern const short MARTYR_SONG[3];")
    H.append("extern const short TICKET_SONG[];\nextern const unsigned char SFX_DUCKS[N_SFX];")
    groups = [g for _, g in director.JUKEBOX_GROUPS]
    H.append("enum { " + ", ".join(f"SG_{g.upper()}" for g in groups) + " };")
    H.append("#endif")
    C.append('/* Generated by tools/gen_audio.py from reference/ -- do not edit. */\n#include "audio_data.h"')
    esc = lambda s: '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'
    C.append("const char *const SONG_NAMES[N_SONGS] = {" + ", ".join(esc(songs.SONGS[s].name) for s in SONGS) + "};")
    C.append("const char *const SONG_DESC[N_SONGS] = {" + ", ".join(esc(songs.SONGS[s].desc) for s in SONGS) + "};")
    meters = {"44": "4/4", "34": "3/4", "68": "6/8", "78": "7/8", "54": "5/4"}
    C.append("const char *const SONG_INFO[N_SONGS] = {" + ", ".join(
        esc(f"{songs.SONGS[s].bpm} BPM \u00b7 {meters.get(songs.SONGS[s].meter, songs.SONGS[s].meter)} \u00b7 {songs.SONGS[s].style.upper()}") for s in SONGS) + "};")
    C.append("const char *const SONG_GROUP_NAMES[N_SONG_GROUPS] = {" + ", ".join(esc(t) for t, _ in director.JUKEBOX_GROUPS) + "};")
    C.append("const unsigned char SONG_GROUP[N_SONGS] = {" + ", ".join(str(groups.index(director.song_group(s))) for s in SONGS) + "};")
    C.append("const short HOME_SONG[%d] = {" % len(tt.HOME_ORDER) + ", ".join(str(SIDX[director.HOME_SONG.get(k, "main_theme")]) for k in tt.HOME_ORDER) + "};")
    all_maps = list(tt.MAPS) + list(tt.SPECIAL_MAPS)
    C.append("const short MAP_SONG[%d] = {" % len(all_maps) + ", ".join(str(SIDX[director.map_song(m)]) for m in all_maps) + "};")
    C.append("const short TOURN_SONG[20] = {" + ", ".join(str(SIDX[s]) for s in director.TOURNAMENT_SONGS) + "};")
    C.append("const short MARTYR_SONG[3] = {" + ", ".join(str(SIDX[s]) for s in director.MARTYR_SONGS) + "};")
    C.append("const short TICKET_SONG[%d] = {" % len(tt.TICKET_BOSSES) + ", ".join(str(SIDX["t_" + b.key]) for b in tt.TICKET_BOSSES) + "};")
    C.append("const unsigned char SFX_DUCKS[N_SFX] = {" + ", ".join("1" if n in sfx.SFX_DUCK else "0" for n in names) + "};")
    os.makedirs(OUT_GEN, exist_ok=True)
    with open(os.path.join(OUT_GEN, "audio_data.h"), "w") as f:
        f.write("\n".join(H) + "\n")
    with open(os.path.join(OUT_GEN, "audio_data.c"), "w") as f:
        f.write("\n".join(C) + "\n")
    print("tables: source/gen/audio_data.c/.h")


if __name__ == "__main__":
    main()
