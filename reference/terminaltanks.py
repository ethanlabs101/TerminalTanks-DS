#!/usr/bin/env python3
"""TerminalTanks - turn-based artillery for the terminal.

Credit: ethanlabs101 - https://github.com/ethanlabs101

Run:   python3 terminaltanks9.py
Needs: Python 3.10+, a UTF-8 terminal of at least 96x28 (100x30 recommended).
No third-party packages: rendering, input and animation are built on the
standard library and plain ANSI escape sequences.
"""
from __future__ import annotations

import argparse
import argparse
import atexit
import colorsys
import copy
import json
import math
import os
import random
import re
import select
import shutil
import signal
import socket
import struct
import subprocess
import sys
import textwrap
import time
import traceback
from dataclasses import dataclass, field, replace
from enum import Enum, auto
from pathlib import Path
from typing import Callable, Optional, Sequence

IS_WEB = sys.platform in ("emscripten", "wasi")      # running in a browser (Pyodide): see the WEB BACKEND below
if os.name == "nt":
    import msvcrt
elif not IS_WEB:
    import select
    import termios
    import tty

__version__ = "1.5.0"
CREDIT_NAME = "ethanlabs101"
CREDIT_URL = "github.com/ethanlabs101"

# ============================================================================
# Configuration
# ============================================================================
FPS = 30
MIN_SIZE = (96, 28)
RECOMMENDED_SIZE = (100, 30)
MAX_FIELD_COLS = 180
MAX_FIELD_ROWS = 40
HUD_TOP_ROWS = 3
HUD_BOTTOM_ROWS = 3
MAX_HP = 3
ROUNDS_TO_WIN = 2
GLASS_HP = 2                           # Glass Cannon levels: your tank has only this many hearts
BOSS_MAX_WINS = 3                      # a Tournament level is lost when the boss wins this many rounds...
STRIKES_ALLOWED = BOSS_MAX_WINS - 1    # ...so the first two boss wins are only warnings (strikes)
LAN_PORT, BEACON_PORT = 47474, 47475   # LAN: TCP game port and UDP discovery port
FINAL_BOSSES = 3                       # the last three Tournament levels are the "big bosses" (Master AI, richer rewards)
_CELL = struct.Struct("<HHI6B")             # one changed cell for the browser renderer
PAIR_CACHE_MAX = 6000                  # cap for the screen's colour-escape cache (see Screen.flush)

TANK_W, TANK_H = 11, 6
PIVOT_H = 4.5
BARREL_LEN = 7.5
COARSE_STEP = 5


@dataclass(frozen=True)
class PhysicsConfig:
    gravity: float = 32.0          # px / s^2
    range_factor: float = 1.16     # full-power 45deg shot covers this many field widths
    power_curve: float = 1.0       # speed = max_speed * (power/100) ** curve
    min_power: float = 10.0
    max_power: float = 100.0
    projectile_radius: float = 0.9
    substep: float = 0.6           # max px travelled between collision probes
    blast_radius: float = 5.5
    splash_reach: float = 2.5
    fall_speed: float = 45.0


PHYS = PhysicsConfig()

# ============================================================================
# Colour helpers
# ============================================================================
RGB = tuple[int, int, int]


def clamp(v: float, lo: float, hi: float) -> float:
    return lo if v < lo else hi if v > hi else v


def lerp(a: float, b: float, t: float) -> float:
    return a + (b - a) * t


def mix(a: RGB, b: RGB, t: float) -> RGB:
    if t <= 0:
        return a
    if t >= 1:
        return b
    return (int(a[0] + (b[0] - a[0]) * t), int(a[1] + (b[1] - a[1]) * t),
            int(a[2] + (b[2] - a[2]) * t))


def shade(c: RGB, k: float) -> RGB:
    return (min(255, int(c[0] * k)), min(255, int(c[1] * k)), min(255, int(c[2] * k)))


def ease_out(t: float) -> float:
    t = clamp(t, 0.0, 1.0)
    return 1.0 - (1.0 - t) ** 3


def ease_in_out(t: float) -> float:
    t = clamp(t, 0.0, 1.0)
    return t * t * (3 - 2 * t)


def gradient(stops: Sequence[tuple[float, RGB]], t: float) -> RGB:
    if t <= stops[0][0]:
        return stops[0][1]
    for (t0, c0), (t1, c1) in zip(stops, stops[1:]):
        if t <= t1:
            return mix(c0, c1, (t - t0) / (t1 - t0) if t1 > t0 else 1.0)
    return stops[-1][1]


class Palette:
    BG = (5, 9, 15)
    PANEL = (9, 16, 26)
    PANEL_HI = (16, 38, 52)
    LINE = (38, 118, 128)
    PRIMARY = (96, 232, 212)
    PRIMARY_DIM = (44, 116, 112)
    AMBER = (255, 184, 64)
    TEXT = (208, 222, 232)
    MUTED = (104, 124, 140)
    DANGER = (255, 92, 92)
    HEART = (255, 82, 108)
    HEART_OFF = (76, 52, 66)
    OK = (124, 240, 152)
    WHITE = (255, 255, 255)
    INK = (8, 12, 18)


COLOR_CHOICES: tuple[tuple[str, RGB], ...] = (
    ("CYAN", (70, 220, 240)), ("BLUE", (74, 122, 255)), ("PURPLE", (156, 106, 240)),
    ("MAGENTA", (240, 84, 204)), ("RED", (240, 72, 72)), ("ORANGE", (255, 150, 52)),
    ("YELLOW", (250, 222, 72)), ("GREEN", (92, 222, 112)), ("WHITE", (236, 241, 246)),
    ("AQUA", (60, 230, 190)), ("TEAL", (40, 170, 160)), ("LIME", (170, 235, 70)),
    ("OLIVE", (140, 160, 70)), ("GOLD", (240, 185, 50)), ("COPPER", (200, 110, 70)),
    ("CRIMSON", (190, 40, 70)), ("PINK", (255, 140, 190)), ("VIOLET", (110, 70, 200)),
    ("COBALT", (50, 80, 200)), ("SKY", (130, 190, 255)), ("SILVER", (180, 190, 200)),
    ("STEEL", (110, 125, 145)), ("SAND", (214, 190, 140)), ("OBSIDIAN", (70, 74, 92)),
)


class ColorMode(Enum):
    AUTO = "AUTO"
    TRUE = "24-BIT"
    ANSI256 = "256"
    ANSI16 = "16"


def resolve_color_mode(mode: ColorMode) -> ColorMode:
    if mode is not ColorMode.AUTO:
        return mode
    if IS_WEB:
        return ColorMode.TRUE
    env = os.environ
    if env.get("COLORTERM", "").lower() in ("truecolor", "24bit") or env.get("WT_SESSION"):
        return ColorMode.TRUE
    if os.name == "nt":
        return ColorMode.TRUE
    term = env.get("TERM", "")
    if any(k in term for k in ("kitty", "alacritty", "direct")):
        return ColorMode.TRUE
    if "256" in term or env.get("TERM_PROGRAM") in ("Apple_Terminal", "iTerm.app"):
        return ColorMode.ANSI256
    return ColorMode.ANSI16 if "color" in term or term in ("xterm", "linux") else ColorMode.ANSI256


_ANSI16 = [(0, 0, 0), (205, 49, 49), (13, 188, 121), (229, 229, 16), (36, 114, 200),
           (188, 63, 188), (17, 168, 205), (229, 229, 229), (102, 102, 102), (241, 76, 76),
           (35, 209, 139), (245, 245, 67), (59, 142, 234), (214, 112, 214), (41, 184, 219),
           (255, 255, 255)]


def _nearest16(c: RGB) -> int:
    return min(range(16), key=lambda i: sum((c[k] - _ANSI16[i][k]) ** 2 for k in range(3)))


def _to_256(c: RGB) -> int:
    r, g, b = c
    if abs(r - g) < 10 and abs(g - b) < 10:
        avg = (r + g + b) // 3
        if avg < 8:
            return 16
        if avg > 248:
            return 231
        return 232 + round((avg - 8) / 247 * 23)
    return 16 + 36 * round(r / 255 * 5) + 6 * round(g / 255 * 5) + round(b / 255 * 5)


# ============================================================================
# Terminal I/O
# ============================================================================
class TerminalError(RuntimeError):
    pass


class TerminalIO:
    """Owns the raw terminal: alternate screen, cbreak input, guaranteed restore."""

    ENTER = "\x1b[?1049h\x1b[?25l\x1b[?7l\x1b[2J\x1b[H"
    LEAVE = "\x1b[0m\x1b[?7h\x1b[?25h\x1b[?1049l"
    CSI_KEYS = {"A": "UP", "B": "DOWN", "C": "RIGHT", "D": "LEFT"}

    def __init__(self) -> None:
        self._active = False
        self._fd = -1
        self._saved = None
        self._buffer = ""
        self.redraw_requested = False
        self._out = getattr(sys.stdout, "buffer", None)

    def __enter__(self) -> "TerminalIO":
        self.start()
        return self

    def __exit__(self, *exc) -> None:
        self.stop()

    def start(self) -> None:
        if not (sys.stdin.isatty() and sys.stdout.isatty()):
            raise TerminalError("TerminalTanks needs an interactive terminal.")
        if os.name == "nt":
            self._start_windows()
        else:
            self._start_posix()
        self._active = True
        atexit.register(self.stop)
        self.write(self.ENTER)

    def _start_posix(self) -> None:
        self._fd = sys.stdin.fileno()
        self._saved = termios.tcgetattr(self._fd)
        tty.setcbreak(self._fd)
        signal.signal(signal.SIGTERM, self._terminate)
        if hasattr(signal, "SIGHUP"):
            signal.signal(signal.SIGHUP, self._terminate)
        if hasattr(signal, "SIGTSTP"):
            signal.signal(signal.SIGTSTP, self._on_suspend)
            signal.signal(signal.SIGCONT, self._on_resume)

    def _start_windows(self) -> None:
        try:
            import ctypes
            kernel = ctypes.windll.kernel32
            kernel.SetConsoleOutputCP(65001)
            handle = kernel.GetStdHandle(-11)
            mode = ctypes.c_ulong()
            kernel.GetConsoleMode(handle, ctypes.byref(mode))
            kernel.SetConsoleMode(handle, mode.value | 0x0004)
        except Exception:
            pass

    @staticmethod
    def _terminate(signum, frame) -> None:
        raise SystemExit(128 + signum)

    def _on_suspend(self, signum, frame) -> None:
        self.write(self.LEAVE)
        termios.tcsetattr(self._fd, termios.TCSADRAIN, self._saved)
        signal.signal(signal.SIGTSTP, signal.SIG_DFL)
        os.kill(os.getpid(), signal.SIGTSTP)

    def _on_resume(self, signum, frame) -> None:
        signal.signal(signal.SIGTSTP, self._on_suspend)
        tty.setcbreak(self._fd)
        self.write(self.ENTER)
        self.redraw_requested = True

    def stop(self) -> None:
        if not self._active:
            return
        self._active = False
        self.write(self.LEAVE)
        if os.name != "nt" and self._saved is not None:
            try:
                termios.tcsetattr(self._fd, termios.TCSAFLUSH, self._saved)
            except termios.error:
                pass

    def write(self, text: str) -> None:
        if self._out is None:
            return
        try:
            self._out.write(text.encode("utf-8", "replace"))
            self._out.flush()
        except (BrokenPipeError, OSError):
            pass

    def size(self) -> tuple[int, int]:
        sz = shutil.get_terminal_size((80, 24))
        return sz.columns, sz.lines

    # -- keyboard ------------------------------------------------------------
    @staticmethod
    def _map_char(ch: str) -> Optional[str]:
        if ch in "\r\n":
            return "ENTER"
        if ch == " ":
            return "SPACE"
        if ch in "\x7f\x08":
            return "BACKSPACE"
        if ch == "\t":
            return "TAB"
        return ch if ch >= " " else None

    def read_keys(self) -> list[str]:
        if os.name == "nt":
            return self._read_windows()
        chunks = []
        while select.select([self._fd], [], [], 0)[0]:
            data = os.read(self._fd, 4096)
            if not data:
                raise SystemExit(0)
            chunks.append(data)
        if chunks:
            self._buffer += b"".join(chunks).decode("utf-8", "ignore")
        return self._parse_buffer()

    def _parse_buffer(self) -> list[str]:
        keys: list[str] = []
        s, i, n = self._buffer, 0, len(self._buffer)
        while i < n:
            ch = s[i]
            if ch == "\x1b":
                if i + 1 >= n:
                    keys.append("ESC")
                    i += 1
                    continue
                if s[i + 1] in "[O":
                    j = i + 2
                    while j < n and not ("@" <= s[j] <= "~"):
                        j += 1
                    if j >= n:
                        break
                    name = self.CSI_KEYS.get(s[j])
                    if name:
                        keys.append(("SHIFT+" if ";2" in s[i + 2:j] else "") + name)
                    i = j + 1
                    continue
                keys.append("ESC")
                i += 1
                continue
            key = self._map_char(ch)
            if key:
                keys.append(key)
            i += 1
        self._buffer = s[i:]
        return keys

    def _read_windows(self) -> list[str]:
        keys: list[str] = []
        table = {"H": "UP", "P": "DOWN", "K": "LEFT", "M": "RIGHT"}
        while msvcrt.kbhit():
            ch = msvcrt.getwch()
            if ch in ("\x00", "\xe0"):
                name = table.get(msvcrt.getwch())
                if name:
                    keys.append(name)
            elif ch == "\x03":
                raise KeyboardInterrupt
            elif ch == "\x1b":
                keys.append("ESC")
            else:
                key = self._map_char(ch)
                if key:
                    keys.append(key)
        return keys


# >>> WEB BACKEND (browser / Pyodide) -----------------------------------------------------------------
# In the browser the page owns the screen: the game hands it structured cell updates (Screen._flush_cells) which a
# canvas renderer paints pixel-perfectly, and the page pushes keystrokes and the grid size in. No termios, select or
# signals are involved. Everything else in this file (scenes, renderer, physics, AI, save) is shared with the terminal.
WEB_API = 2


class WebTerminalIO(TerminalIO):
    """TerminalIO for the browser: cells out via callbacks, keys and size pushed in by the page."""
    cell_output = True

    def __init__(self, present: Callable, bell: Optional[Callable[[], None]] = None, size: tuple[int, int] = (110, 31)) -> None:
        super().__init__()
        self._present, self._bell = present, bell
        self._size = (max(1, int(size[0])), max(1, int(size[1])))

    def start(self) -> None:
        self._active = True

    def stop(self) -> None:
        self._active = False

    def write(self, text: str) -> None:
        if "\a" in text and self._bell is not None:      # the game's optional sound setting rings the terminal bell
            self._bell()

    def present(self, cols: int, rows: int, full: bool, data: bytes) -> None:
        self._present(cols, rows, full, data)

    def size(self) -> tuple[int, int]:
        return self._size

    def resize(self, cols: int, rows: int) -> None:
        self._size = (max(1, int(cols)), max(1, int(rows)))

    def push_keys(self, data: str) -> None:
        """Raw input as a terminal would send it (letters, \\r, \\x1b[A, \\x1b[1;2C ...)."""
        self._buffer += data

    def read_keys(self) -> list[str]:
        return self._parse_buffer()


_web_term: Optional[WebTerminalIO] = None


def web_feed(data: str) -> None:
    """Page -> game: keystrokes."""
    if _web_term is not None:
        _web_term.push_keys(data)


def web_resize(cols: int, rows: int) -> None:
    """Page -> game: the drawing grid changed size."""
    if _web_term is not None:
        _web_term.resize(cols, rows)


def web_redraw() -> None:
    """Page -> game: repaint every cell (tab visible again, canvas was recreated, ...)."""
    if _web_term is not None:
        _web_term.redraw_requested = True


def web_frames(argv: Optional[Sequence[str]] = None, present: Optional[Callable] = None,
               bell: Optional[Callable[[], None]] = None, size: tuple[int, int] = (110, 31)):
    """Generator entry point for the browser: one iteration per frame, so the page paces the game (it never blocks the
    tab). The exit code is the generator's return value."""
    global _web_term
    args, mode, save_path = _parse_cli(argv)
    if args.reset_progress:
        return _reset_progress(save_path)
    term = _web_term = WebTerminalIO(present, bell, size)
    try:
        with term:
            yield from Application(term, args.seed, mode, save_path).frames()
    except GeneratorExit:
        raise
    except Exception:
        traceback.print_exc()
        print("\nTerminalTanks crashed.", file=sys.stderr)
        return 1
    finally:
        _web_term = None
    return 0
# <<< WEB BACKEND ---------------------------------------------------------------------------------------


class Action(Enum):
    UP = auto()
    DOWN = auto()
    LEFT = auto()
    RIGHT = auto()
    CONFIRM = auto()
    FIRE = auto()
    BACK = auto()
    PAUSE = auto()
    OTHER = auto()


@dataclass(frozen=True)
class InputEvent:
    action: Action
    coarse: bool = False
    raw: str = ""

    @property
    def confirm(self) -> bool:
        return self.action in (Action.CONFIRM, Action.FIRE)


_BINDINGS = {
    "UP": Action.UP, "w": Action.UP, "DOWN": Action.DOWN, "s": Action.DOWN,
    "LEFT": Action.LEFT, "a": Action.LEFT, "RIGHT": Action.RIGHT, "d": Action.RIGHT,
    "ENTER": Action.CONFIRM, "SPACE": Action.FIRE, "ESC": Action.BACK, "p": Action.PAUSE,
}


class InputManager:
    """Turns raw key names into game actions; letters honour Shift as 'coarse'."""

    def __init__(self, term) -> None:
        self.term = term

    def poll(self) -> list[InputEvent]:
        events = []
        for raw in self.term.read_keys():
            coarse = raw.startswith("SHIFT+") or (len(raw) == 1 and raw.isupper())
            name = raw[6:] if raw.startswith("SHIFT+") else raw
            action = _BINDINGS.get(name if len(name) > 1 else name.lower(), Action.OTHER)
            events.append(InputEvent(action, coarse, raw))
        return events


# ============================================================================
# Pixel canvas + cell screen
# ============================================================================
class PixelCanvas:
    """Square-ish pixel grid (two pixels per terminal row); y grows upward."""
    __slots__ = ("w", "h", "rows")

    def __init__(self, w: int, h: int, fill: RGB = (0, 0, 0), rows=None) -> None:
        self.w, self.h = w, h
        self.rows = rows if rows is not None else [[fill] * w for _ in range(h)]

    def copy(self) -> "PixelCanvas":
        return PixelCanvas(self.w, self.h, rows=[r[:] for r in self.rows])

    def plot(self, x: int, y: int, c: RGB) -> None:
        py = self.h - 1 - y
        if 0 <= x < self.w and 0 <= py < self.h:
            self.rows[py][x] = c

    def blend(self, x: int, y: int, c: RGB, a: float) -> None:
        py = self.h - 1 - y
        if 0 <= x < self.w and 0 <= py < self.h and a > 0.02:
            row = self.rows[py]
            row[x] = mix(row[x], c, a)

    def plotf(self, x: float, y: float, c: RGB) -> None:
        self.plot(int(x // 1), int(y // 1), c)

    def blendf(self, x: float, y: float, c: RGB, a: float) -> None:
        self.blend(int(x // 1), int(y // 1), c, a)


Cell = tuple[str, RGB, RGB]
BOX_STYLES = {"double": "╔╗╚╝═║", "single": "┌┐└┘─│", "round": "╭╮╰╯─│", "heavy": "┏┓┗┛━┃"}


def _shift_row(row: list, dx: int) -> list:
    if dx > 0:
        return [row[0]] * dx + row[:-dx]
    if dx < 0:
        return row[-dx:] + [row[-1]] * (-dx)
    return row


class Screen:
    """Double-buffered cell grid with diffed truecolor/256/16-colour output."""

    def __init__(self, term, mode: ColorMode) -> None:
        self.term = term
        self.cols = self.rows = 0
        self.back: list[list[Cell]] = []
        self._front: Optional[list[list]] = None
        self._pairs: dict = {}
        self.mode = ColorMode.TRUE
        self.set_mode(mode)

    def set_mode(self, mode: ColorMode) -> None:
        self.mode = resolve_color_mode(mode)
        self._pairs.clear()
        self._front = None

    def resize(self, cols: int, rows: int) -> None:
        self.cols, self.rows = cols, rows
        self._front = None
        self.clear()

    def force_redraw(self) -> None:
        self._front = None

    def clear(self, color: RGB = Palette.BG) -> None:
        blank: Cell = (" ", color, color)
        self.back = [[blank] * self.cols for _ in range(self.rows)]

    # -- drawing -------------------------------------------------------------
    def put(self, x: int, y: int, ch: str, fg: RGB, bg: Optional[RGB] = None) -> None:
        if 0 <= x < self.cols and 0 <= y < self.rows:
            if bg is None:
                old = self.back[y][x]
                bg = mix(old[1], old[2], 0.5) if old[0] == "▀" else old[2]
            self.back[y][x] = (ch, fg, bg)

    def text(self, x: int, y: int, s: str, fg: RGB, bg: Optional[RGB] = None) -> None:
        for i, ch in enumerate(s):
            if ch != " " or bg is not None:
                self.put(x + i, y, ch, fg, bg)

    def center(self, y: int, s: str, fg: RGB, bg: Optional[RGB] = None,
               x0: int = 0, width: Optional[int] = None) -> None:
        width = self.cols if width is None else width
        self.text(x0 + (width - len(s)) // 2, y, s, fg, bg)

    def fill(self, x: int, y: int, w: int, h: int, bg: RGB, ch: str = " ", fg: RGB = Palette.TEXT) -> None:
        cell: Cell = (ch, fg, bg)
        for yy in range(max(0, y), min(self.rows, y + h)):
            x0, x1 = max(0, x), min(self.cols, x + w)
            if x1 > x0:
                self.back[yy][x0:x1] = [cell] * (x1 - x0)

    def box(self, x: int, y: int, w: int, h: int, style: str = "double",
            fg: RGB = Palette.LINE, bg: RGB = Palette.PANEL, title: str = "",
            title_fg: RGB = Palette.PRIMARY) -> None:
        tl, tr, bl, br, hz, vt = BOX_STYLES[style]
        self.fill(x, y, w, h, bg)
        for i in range(1, w - 1):
            self.put(x + i, y, hz, fg, bg)
            self.put(x + i, y + h - 1, hz, fg, bg)
        for j in range(1, h - 1):
            self.put(x, y + j, vt, fg, bg)
            self.put(x + w - 1, y + j, vt, fg, bg)
        for (cx, cy, ch) in ((x, y, tl), (x + w - 1, y, tr), (x, y + h - 1, bl), (x + w - 1, y + h - 1, br)):
            self.put(cx, cy, ch, fg, bg)
        if title:
            label = f" {title} "
            self.text(x + (w - len(label)) // 2, y, label, title_fg, bg)

    def blit(self, canvas: PixelCanvas, x: int, y: int, shift: tuple[int, int] = (0, 0)) -> None:
        dx, dy = shift
        rows, h = canvas.rows, canvas.h
        for r in range(h // 2):
            if dx == 0 and dy == 0:
                top, bot = rows[2 * r], rows[2 * r + 1]
            else:
                ty = int(clamp(2 * r - dy, 0, h - 1))
                by = int(clamp(2 * r + 1 - dy, 0, h - 1))
                top, bot = _shift_row(rows[ty], dx), _shift_row(rows[by], dx)
            if 0 <= y + r < self.rows:
                self.back[y + r][x:x + canvas.w] = [("▀", t, b) for t, b in zip(top, bot)]

    def tint(self, x: int, y: int, w: int, h: int, color: RGB, a: float) -> None:
        for yy in range(max(0, y), min(self.rows, y + h)):
            row = self.back[yy]
            for xx in range(max(0, x), min(self.cols, x + w)):
                ch, fg, bg = row[xx]
                row[xx] = (ch, mix(fg, color, a), mix(bg, color, a))

    def dim(self, k: float) -> None:
        for row in self.back:
            row[:] = [(ch, shade(fg, k), shade(bg, k)) for ch, fg, bg in row]

    def fade_rows(self, factors: Sequence[float]) -> None:
        for y, row in enumerate(self.back):
            k = factors[y]
            if k < 0.999:
                row[:] = [(ch, shade(fg, k), shade(bg, k)) for ch, fg, bg in row]

    # -- output --------------------------------------------------------------
    def _encode(self, fg: RGB, bg: RGB) -> str:
        m = self.mode
        if m is ColorMode.TRUE:
            return f"\x1b[38;2;{fg[0]};{fg[1]};{fg[2]};48;2;{bg[0]};{bg[1]};{bg[2]}m"
        if m is ColorMode.ANSI256:
            return f"\x1b[38;5;{_to_256(fg)};48;5;{_to_256(bg)}m"
        f, b = _nearest16(fg), _nearest16(bg)
        return f"\x1b[{30 + f if f < 8 else 82 + f};{40 + b if b < 8 else 92 + b}m"

    def _flush_cells(self) -> None:
        """Browser renderer: send only the changed cells as fixed-size records (x, y, codepoint, fg rgb, bg rgb)
        instead of ANSI text; the page paints them on a canvas."""
        full = self._front is None
        if full:
            self._front = [[None] * self.cols for _ in range(self.rows)]
        pack, recs = _CELL.pack, []
        for y in range(self.rows):
            brow, frow = self.back[y], self._front[y]
            if brow == frow:
                continue
            for x in range(self.cols):
                cell = brow[x]
                if cell != frow[x]:
                    ch, fg, bg = cell
                    try:
                        recs.append(pack(x, y, ord(ch[:1] or " "), fg[0], fg[1], fg[2], bg[0], bg[1], bg[2]))
                    except struct.error:                                   # a colour channel out of range: clamp it
                        c = [max(0, min(255, int(v))) for v in (*fg, *bg)]
                        recs.append(pack(x, y, ord(ch[:1] or " "), *c))
            self._front[y] = brow[:]
        if recs or full:
            self.term.present(self.cols, self.rows, full, b"".join(recs))

    def flush(self) -> None:
        if getattr(self.term, "cell_output", False):
            return self._flush_cells()
        if self._front is None:
            self._front = [[None] * self.cols for _ in range(self.rows)]
        out: list[str] = []
        last_pair = None
        cur_x = cur_y = -1
        for y in range(self.rows):
            brow, frow = self.back[y], self._front[y]
            if brow == frow:
                continue
            for x in range(self.cols):
                cell = brow[x]
                if cell == frow[x]:
                    continue
                if cur_y != y or cur_x != x:
                    out.append(f"\x1b[{y + 1};{x + 1}H")
                pair = (cell[1], cell[2])
                if pair != last_pair:
                    seq = self._pairs.get(pair)
                    if seq is None:
                        if len(self._pairs) >= PAIR_CACHE_MAX:     # animated skies mint endless new colours;
                            self._pairs.clear()                    # an unbounded cache here was a slow memory leak
                        seq = self._pairs[pair] = self._encode(*pair)
                    out.append(seq)
                    last_pair = pair
                out.append(cell[0])
                cur_x, cur_y = x + 1, y
            self._front[y] = brow[:]
        if out:
            self.term.write("".join(out))


# ============================================================================
# Settings
# ============================================================================
class Difficulty(Enum):
    EASY = "EASY"
    NORMAL = "NORMAL"
    HARD = "HARD"
    MASTER = "MASTER"


@dataclass
class Settings:
    difficulty: Difficulty = Difficulty.NORMAL
    anim_speed: float = 1.0
    effects: bool = True
    trail_life: float = 0.55
    particle_density: float = 1.0
    aim_guide: int = 1
    random_maps: bool = False
    color_mode: ColorMode = ColorMode.AUTO
    sound: bool = False
    wind: float = 6.0
    home_mode: str = "CLASSIC"


SHOP_THEMES = ('SUNSET', 'MEADOW', 'DESERT', 'HARBOR', 'AUTUMN', 'DAWN', 'PRAIRIE', 'HARVEST', 'LAKESIDE', 'CAMPFIRE', 'TWILIGHT', 'FOGBANK', 'SPRING', 'MARKET', 'MONSOON', 'ARCTIC', 'JUNGLE', 'NEON', 'DEEPSEA', 'STORMFRONT', 'SYNTHWAVE', 'TOKYO', 'PYRAMIDS', 'TUNDRA', 'CANYONS', 'LAGOON', 'THUNDERHEAD', 'STEAMTOWN', 'ORBITRING', 'BAMBOO', 'MOLTEN', 'CRYSTALCAVE', 'BLOODMOON', 'EMERALD', 'GOLDENDUNES', 'NIGHTCITY', 'LAVAFALLS', 'VAPORWAVE', 'DEADSEA', 'SKYFORT', 'CYBERPUNK', 'MAELSTROM', 'ICEPALACE', 'SOLARSTORM', 'GHOSTTOWN', 'ECLIPSE', 'VOIDRIFT', 'SOLARCROWN', 'STARFALL', 'CELESTIAL', 'QUASAR', 'GALAXY', 'NEBULAR', 'HYPERGATE', 'BLACKHOLE', 'AURORAKING', 'CRYSTALPEAK', 'DRAGONSKY', 'CELESTIA', 'ASTRALSEA',
               'GLASSDESERT', 'LANTERNFEST', 'WORLDTREE', 'SKYWHALE', 'SHATTERLINE')
MISSION_THEMES = ('OUTPOST', 'RADAR', 'WARZONE', 'ORBITAL', 'DEEPSPACE', 'INVASION')
TICKET_THEMES = ('VOLTCITY', 'CLOCKWORKS', 'FROSTCATHEDRAL', 'SUNTEMPLE', 'SUNKENCITY', 'PYRAMIDPLAIN', 'CHEMWORKS', 'GOTHICKEEP', 'ORBITALCITY', 'PAGODAPEAK')
ANCIENT_THEMES = ('EMBERSANCTUM', 'TEMPESTSPIRES', 'ASTRALRIFT', 'SOLARIS', 'CHRONOS', 'ETERNAL')
HOME_ORDER = ("CLASSIC",) + SHOP_THEMES + MISSION_THEMES + TICKET_THEMES + ANCIENT_THEMES + ("MASTER", "SECRET")   # every home screen, in cycle order

SETTING_ROWS = (
    ("AI DIFFICULTY", "difficulty", (("EASY", Difficulty.EASY), ("NORMAL", Difficulty.NORMAL), ("HARD", Difficulty.HARD), ("MASTER", Difficulty.MASTER)),
     "How well the computer solves its firing problem"),
    ("WIND", "wind", (("OFF", 0.0), ("LIGHT", 3.0), ("NORMAL", 6.0), ("STRONG", 10.0)),
     "Shifts every turn - read the arrows before you fire"),
    ("ANIMATION SPEED", "anim_speed", (("SLOW", 0.7), ("NORMAL", 1.0), ("FAST", 1.6)),
     "Speed of shells, blasts and AI decisions"),
    ("SCREEN EFFECTS", "effects", (("OFF", False), ("ON", True)), "Screen shake and blast flashes"),
    ("PROJECTILE TRAILS", "trail_life", (("OFF", 0.0), ("SHORT", 0.55), ("LONG", 1.3)), "Length of the fading tracer"),
    ("PARTICLE DENSITY", "particle_density", (("LOW", 0.45), ("MEDIUM", 1.0), ("HIGH", 1.8)), "Sparks, debris and smoke"),
    ("AIM GUIDE", "aim_guide", (("OFF", 0), ("SHORT", 1), ("FULL", 2)), "Dotted NO-WIND preview - you add the wind"),
    ("MAP SELECTION", "random_maps", (("CHOOSE", False), ("RANDOM", True)), "Pick the opening battlefield or roll the dice"),
    ("COLOR MODE", "color_mode", (("AUTO", ColorMode.AUTO), ("24-BIT", ColorMode.TRUE), ("256", ColorMode.ANSI256), ("16", ColorMode.ANSI16)),
     "Force a palette if colours look wrong"),
    ("SOUND", "sound", (("OFF", False), ("BELL", True)), "Terminal bell on impacts"),
    ("HOME SCREEN", "home_mode", tuple((k, k) for k in HOME_ORDER), "Pick any home screen you own"),
)


class Feedback:
    """Audio-ish feedback: optional terminal bell, kept behind one small interface."""

    def __init__(self, term, settings: Settings) -> None:
        self.term, self.settings, self._last = term, settings, 0.0

    def emit(self, kind: str) -> None:
        if self.settings.sound and kind in ("explosion", "destroy"):
            now = time.monotonic()
            if now - self._last > 0.15:
                self._last = now
                self.term.write("\a")


# ============================================================================
# Maps & terrain
# ============================================================================
@dataclass(frozen=True)
class Theme:
    sky_top: RGB
    sky_bottom: RGB
    far: RGB
    orb: RGB
    surface: RGB
    topsoil: RGB
    soil: RGB
    deep: RGB
    star_density: float
    aurora: bool = False
    fx: str = ""            # extra animated sky layer: "embers" | "motes" | "storm" (see Scenery.draw_fx)
    buildings: str = ""     # a key of BUILDING_STYLES: a silhouette skyline baked into the sky behind the terrain


@dataclass(frozen=True)
class MapDefinition:
    key: str
    name: str
    description: str
    profile: tuple[tuple[float, float], ...]
    smooth: bool
    roughness: float
    spawns: tuple[float, float]
    theme: Theme
    cover: int
    relief: int
    special: bool = False                    # Tournament-reward map (not part of the 50-map library)
    accent: RGB = (255, 206, 100)            # colour of its name in the battlefield list


MAPS: tuple[MapDefinition, ...] = (
    MapDefinition("valley", "VALLEY", "A wide bowl. Open lines of fire, but shells roll into the basin.",
                  ((0, .60), (.12, .48), (.30, .20), (.5, .10), (.70, .20), (.88, .48), (1, .60)), True, 0.0,
                  (.09, .91), Theme((14, 22, 60), (232, 140, 120), (60, 50, 100), (255, 220, 160), (110, 210, 120),
                                    (70, 140, 80), (92, 84, 70), (40, 36, 40), 0.003), 1, 2),
    MapDefinition("pass", "MOUNTAIN PASS", "Twin peaks wall off the centre. Lob it over or wear it down.",
                  ((0, .28), (.14, .34), (.30, .78), (.40, .52), (.50, .30), (.60, .52), (.70, .78), (.86, .34), (1, .28)),
                  False, 0.012, (.08, .92),
                  Theme((8, 14, 34), (90, 120, 170), (40, 60, 100), (220, 235, 255), (235, 240, 250),
                        (140, 150, 165), (86, 90, 104), (34, 36, 48), 0.005), 3, 3),
    MapDefinition("wasteland", "WASTELAND", "Broken dunes and blast scars. Nothing to hide behind but luck.",
                  ((0, .22), (.2, .30), (.35, .18), (.5, .34), (.65, .18), (.8, .30), (1, .22)), False, 0.05,
                  (.10, .90), Theme((40, 14, 20), (230, 120, 60), (110, 50, 50), (255, 130, 60), (220, 170, 90),
                                    (190, 130, 70), (140, 90, 60), (60, 36, 30), 0.002), 1, 1),
    MapDefinition("industrial", "INDUSTRIAL", "Steel terraces and a central bulwark. Angles matter.",
                  ((0, .30), (.10, .30), (.13, .44), (.28, .44), (.31, .18), (.42, .18), (.45, .62), (.55, .62),
                   (.58, .18), (.69, .18), (.72, .44), (.87, .44), (.90, .30), (1, .30)), False, 0.0,
                  (.205, .795), Theme((6, 14, 26), (30, 90, 110), (24, 50, 70), (120, 240, 255), (120, 150, 170),
                                      (84, 104, 120), (60, 72, 86), (26, 32, 40), 0.001), 3, 2),
    MapDefinition("canyon", "CANYON", "High cliffs, a deep chasm and a lone rock spire between you.",
                  ((0, .62), (.24, .62), (.32, .50), (.36, .12), (.44, .08), (.48, .40), (.52, .40), (.56, .08),
                   (.64, .12), (.68, .50), (.76, .62), (1, .62)), False, 0.015, (.13, .87),
                  Theme((30, 20, 50), (250, 170, 110), (120, 70, 80), (255, 240, 200), (232, 150, 90),
                        (190, 100, 60), (150, 72, 50), (70, 34, 34), 0.002), 2, 3),
)


def sample_profile(points: Sequence[tuple[float, float]], u: float, smooth: bool) -> float:
    n = len(points)
    for i in range(n - 1):
        x0, h0 = points[i]
        x1, h1 = points[i + 1]
        if u <= x1:
            t = (u - x0) / (x1 - x0) if x1 > x0 else 1.0
            if not smooth:
                return h0 + (h1 - h0) * t
            p0 = points[max(0, i - 1)][1]
            p3 = points[min(n - 1, i + 2)][1]
            return max(0.02, 0.5 * (2 * h0 + (-p0 + h1) * t + (2 * p0 - 5 * h0 + 4 * h1 - p3) * t * t
                                    + (-p0 + 3 * h0 - 3 * h1 + p3) * t ** 3))
    return points[-1][1]


def _mirror(half: Sequence[tuple[float, float]]) -> tuple[tuple[float, float], ...]:
    return tuple(half) + tuple((round(1 - x, 4), h) for x, h in reversed(list(half)[:-1]))


def _rate(points: Sequence[tuple[float, float]], spawns: tuple[float, float]) -> tuple[int, int]:
    hs = [h for _, h in points]
    span = max(hs) - min(hs)
    base = (sample_profile(points, spawns[0], False) + sample_profile(points, spawns[1], False)) / 2
    bump = max([h for x, h in points if 0.3 <= x <= 0.7] or hs) - base
    return 1 + (bump > 0.15) + (bump > 0.35), 1 + (span > 0.3) + (span > 0.55)


_THEMES: dict[str, Theme] = {
    "valley": MAPS[0].theme, "alpine": MAPS[1].theme, "waste": MAPS[2].theme, "steel": MAPS[3].theme, "canyon": MAPS[4].theme,
    "arctic": Theme((10, 20, 48), (150, 200, 230), (70, 100, 150), (235, 245, 255), (240, 248, 255), (190, 215, 235), (120, 150, 185), (50, 70, 100), 0.003),
    "jungle": Theme((8, 30, 30), (120, 200, 120), (30, 90, 70), (255, 240, 170), (90, 200, 90), (50, 140, 60), (86, 66, 44), (34, 30, 26), 0.001),
    "volcano": Theme((20, 6, 8), (200, 60, 30), (80, 24, 24), (255, 150, 60), (120, 100, 100), (78, 66, 68), (52, 42, 44), (22, 18, 20), 0.002),
    "moon": Theme((2, 3, 8), (40, 44, 60), (50, 54, 70), (210, 215, 230), (180, 182, 190), (140, 142, 150), (100, 102, 110), (46, 48, 54), 0.012),
    "cyber": Theme((8, 4, 26), (180, 40, 160), (60, 24, 110), (80, 255, 240), (90, 255, 240), (60, 170, 200), (56, 56, 110), (24, 22, 56), 0.004),
    "swamp": Theme((10, 20, 16), (120, 140, 80), (40, 60, 50), (230, 230, 160), (120, 160, 70), (84, 110, 60), (70, 60, 44), (30, 28, 24), 0.002),
    "beach": Theme((30, 50, 120), (255, 190, 120), (90, 110, 160), (255, 230, 170), (250, 225, 160), (225, 190, 120), (170, 135, 90), (84, 68, 50), 0.001),
    "midnight": Theme((3, 6, 24), (30, 50, 110), (20, 30, 70), (180, 200, 255), (90, 110, 180), (60, 76, 130), (44, 52, 96), (20, 22, 44), 0.008),
    "ember": Theme((24, 10, 16), (220, 110, 70), (90, 40, 48), (255, 200, 120), (200, 110, 70), (160, 80, 56), (110, 60, 50), (50, 30, 30), 0.002),
    "toxic": Theme((6, 18, 10), (140, 220, 60), (40, 70, 40), (220, 255, 120), (150, 230, 70), (100, 170, 60), (70, 80, 50), (28, 34, 24), 0.002),
    "steppe": Theme((20, 30, 70), (240, 200, 130), (100, 100, 110), (255, 230, 170), (190, 190, 100), (150, 150, 80), (120, 100, 70), (56, 48, 40), 0.002),
    "rust": Theme((12, 14, 22), (170, 100, 70), (70, 56, 60), (230, 200, 170), (180, 110, 70), (130, 80, 56), (94, 62, 50), (40, 30, 30), 0.002),
}

_S = (0.08, 0.92)
# (name, description, profile, theme, spawns, smooth, roughness)
_EXTRA_MAPS = (
    ("DUNES", "Soft rolling sand. Shells drop short in the dips.", _mirror([(0, .25), (.15, .36), (.3, .2), (.5, .4)]), "beach", _S, True, 0.015),
    ("TWIN TOWERS", "A narrow tower guards each tank. Go high.", _mirror([(0, .2), (.2, .2), (.22, .7), (.27, .7), (.29, .2), (.5, .2)]), "cyber", (.09, .91), False, 0.0),
    ("BASIN", "A deep bowl. Shells roll to the bottom.", _mirror([(0, .7), (.2, .5), (.4, .15), (.5, .1)]), "toxic", _S, True, 0.0),
    ("HILLTOP", "One fat hill owns the middle.", _mirror([(0, .15), (.3, .2), (.42, .55), (.5, .6)]), "jungle", _S, False, 0.01),
    ("SAWTOOTH", "Jagged teeth break every flat line.", _mirror([(0, .22), (.16, .22), (.2, .5), (.22, .22), (.3, .5), (.32, .22), (.4, .5), (.42, .22), (.5, .5)]), "rust", _S, False, 0.0),
    ("CRATER LAKE", "A huge impact basin between two rims.", _mirror([(0, .4), (.15, .4), (.25, .2), (.35, .05), (.5, .03)]), "midnight", _S, False, 0.01),
    ("PLATEAU", "A flat-topped mesa dominates the centre.", _mirror([(0, .2), (.2, .2), (.3, .5), (.5, .5)]), "steppe", _S, False, 0.0),
    ("BUNKER", "Sunken firing pits behind a central berm.", _mirror([(0, .35), (.15, .35), (.18, .2), (.3, .2), (.33, .5), (.5, .5)]), "steel", _S, False, 0.0),
    ("ROLLING HILLS", "Gentle swells hide low shots.", _mirror([(0, .3), (.12, .42), (.25, .22), (.38, .45), (.5, .28)]), "valley", _S, True, 0.0),
    ("THE NEEDLE", "A single needle of rock splits the field.", _mirror([(0, .18), (.4, .18), (.47, .85), (.5, .88)]), "alpine", _S, False, 0.0),
    ("FORTRESS WALLS", "A rampart stands in front of each tank.", _mirror([(0, .25), (.2, .25), (.22, .55), (.28, .55), (.3, .25), (.5, .25)]), "steel", _S, False, 0.0),
    ("LAUNCH RAMPS", "High launch pads fall to a low centre.", _mirror([(0, .55), (.22, .55), (.42, .1), (.5, .08)]), "ember", _S, False, 0.0),
    ("ISLANDS", "Stepping stones across a deep gap.", _mirror([(0, .3), (.16, .3), (.2, .05), (.3, .05), (.34, .35), (.4, .35), (.44, .05), (.5, .05)]), "beach", _S, False, 0.0),
    ("CALDERA", "A volcanic cone with a hollow heart.", _mirror([(0, .15), (.22, .2), (.38, .62), (.44, .72), (.47, .5), (.5, .4)]), "volcano", _S, False, 0.005),
    ("CENTRE STAIRS", "Terraces climb toward the middle.", _mirror([(0, .15), (.16, .15), (.18, .25), (.26, .25), (.28, .35), (.36, .35), (.38, .45), (.5, .45)]), "steel", _S, False, 0.0),
    ("CASCADE", "Terraces fall toward the middle.", _mirror([(0, .6), (.16, .6), (.18, .5), (.28, .5), (.3, .4), (.38, .4), (.4, .2), (.5, .2)]), "arctic", _S, False, 0.0),
    ("TRENCHES", "Narrow gaps swallow flat shots.", _mirror([(0, .3), (.16, .3), (.18, .08), (.26, .08), (.28, .3), (.42, .3), (.44, .08), (.5, .08)]), "swamp", _S, False, 0.0),
    ("BROKEN BRIDGE", "A central pier stands over two gaps.", _mirror([(0, .4), (.26, .4), (.3, .05), (.4, .05), (.42, .4), (.5, .4)]), "beach", _S, False, 0.0),
    ("DOUBLE HUMP", "A hump in front of each tank.", _mirror([(0, .2), (.14, .2), (.22, .65), (.3, .3), (.4, .22), (.5, .25)]), "jungle", _S, False, 0.01),
    ("BADLANDS", "Crumbling ground. Nothing is flat.", _mirror([(0, .3), (.2, .4), (.35, .25), (.5, .38)]), "rust", _S, False, 0.07),
    ("GLACIER", "Smooth ice swells with a central trough.", _mirror([(0, .45), (.2, .3), (.4, .38), (.5, .2)]), "arctic", _S, True, 0.02),
    ("CLIFFSIDE", "Tanks perch on high cliffs over a wide floor.", _mirror([(0, .7), (.16, .7), (.2, .22), (.5, .2)]), "canyon", _S, False, 0.0),
    ("PYRAMID", "A huge pyramid fills the middle.", _mirror([(0, .1), (.2, .1), (.5, .8)]), "waste", _S, False, 0.0),
    ("ZIGZAG", "Peaks and valleys in every direction.", _mirror([(0, .3), (.16, .3), (.24, .55), (.32, .2), (.4, .55), (.5, .25)]), "cyber", _S, False, 0.0),
    ("MOONSCAPE", "Pockmarked lunar ground.", _mirror([(0, .25), (.25, .2), (.5, .28)]), "moon", _S, False, 0.06),
    ("THE GORGE", "A narrow, bottomless-looking gorge.", _mirror([(0, .5), (.22, .5), (.32, .4), (.38, .05), (.5, .03)]), "canyon", _S, False, 0.0),
    ("TWIN MESAS", "Flat-topped mesas with a saddle between.", _mirror([(0, .15), (.16, .15), (.2, .5), (.3, .5), (.34, .15), (.42, .15), (.46, .4), (.5, .4)]), "steppe", _S, False, 0.0),
    ("RAMPARTS", "Stepped battlements in layers.", _mirror([(0, .3), (.16, .3), (.18, .45), (.24, .45), (.26, .3), (.34, .3), (.36, .6), (.42, .6), (.44, .3), (.5, .3)]), "midnight", _S, False, 0.0),
    ("QUARRY", "Cut terraces drop into a deep pit.", _mirror([(0, .55), (.2, .55), (.24, .3), (.3, .3), (.34, .1), (.5, .1)]), "rust", _S, False, 0.0),
    ("TUNDRA WAVES", "Long frozen waves. Mind the crests.", _mirror([(0, .3), (.1, .4), (.2, .3), (.3, .4), (.4, .3), (.5, .4)]), "arctic", _S, True, 0.015),
    ("SHARK FIN", "A lone fin rises close to player two.", ((0, .3), (.4, .3), (.56, .3), (.6, .8), (.66, .3), (1, .3)), "midnight", (.09, .91), False, 0.0),
    ("LOPSIDED PEAK", "One big peak off-centre. Shots curve around it.", ((0, .25), (.2, .3), (.35, .75), (.5, .3), (.7, .25), (.85, .3), (1, .25)), "alpine", _S, False, 0.01),
    ("DRAGON SPINE", "A ridge of spikes down the whole field.", ((0, .3), (.1, .3), (.2, .5), (.3, .3), (.4, .6), (.5, .3), (.6, .7), (.7, .3), (.8, .5), (.9, .3), (1, .3)), "volcano", _S, False, 0.0),
    ("LANDSLIDE", "A collapsed slope with a deep scar.", ((0, .4), (.2, .4), (.4, .2), (.5, .3), (.6, .15), (.8, .4), (1, .4)), "rust", _S, False, 0.02),
    ("ICE SHELF", "High ice with a crack off-centre.", ((0, .5), (.35, .5), (.38, .1), (.46, .1), (.48, .5), (1, .5)), "arctic", _S, False, 0.0),
    ("WATCHTOWER", "A tall tower on the left, a ledge on the right.", ((0, .2), (.2, .2), (.24, .65), (.27, .65), (.3, .2), (.72, .2), (.78, .4), (.9, .4), (1, .2)), "steel", (.08, .85), False, 0.0),
    ("SINKHOLES", "The ground is full of holes.", ((0, .35), (.18, .35), (.22, .05), (.3, .05), (.34, .35), (.56, .35), (.6, .05), (.68, .05), (.72, .35), (1, .35)), "swamp", _S, False, 0.0),
    ("HOGBACK", "A long ridge sloping away to the right.", ((0, .2), (.2, .3), (.4, .7), (.55, .6), (.75, .3), (1, .2)), "waste", _S, True, 0.0),
    ("CAUSEWAY", "Raised roads between flooded lowlands.", ((0, .3), (.12, .3), (.17, .1), (.4, .1), (.45, .3), (.55, .3), (.6, .1), (.83, .1), (.88, .3), (1, .3)), "toxic", _S, False, 0.0),
    ("FANG GAP", "Two uneven fangs guard the gap.", ((0, .25), (.2, .25), (.28, .75), (.31, .25), (.7, .25), (.73, .75), (.8, .25), (1, .25)), "ember", _S, False, 0.0),
    ("CAULDRON", "A boiling pit ringed by high walls.", ((0, .3), (.22, .3), (.28, .6), (.4, .6), (.45, .1), (.55, .1), (.6, .6), (.72, .6), (.78, .3), (1, .3)), "volcano", _S, False, 0.0),
    ("SWITCHBACK", "Sharp spikes and narrow valleys.", ((0, .2), (.14, .2), (.22, .6), (.3, .15), (.4, .6), (.5, .15), (.6, .6), (.7, .15), (.78, .6), (.86, .2), (1, .2)), "jungle", _S, False, 0.0),
    ("THE NOTCH", "A raised block with a notch in the middle.", ((0, .25), (.25, .25), (.3, .5), (.45, .5), (.5, .3), (.55, .5), (.7, .5), (.75, .25), (1, .25)), "cyber", _S, False, 0.0),
    ("BREAKWATER", "A chain of jetties across the bay.", ((0, .3), (.18, .3), (.22, .5), (.28, .2), (.34, .5), (.4, .2), (.5, .5), (.6, .2), (.66, .5), (.72, .2), (.78, .5), (.82, .3), (1, .3)), "midnight", _S, False, 0.0),
    ("LAST STAND", "Both tanks dig in on raised bluffs.", ((0, .4), (.12, .4), (.17, .6), (.35, .15), (.65, .15), (.83, .6), (.88, .4), (1, .4)), "ember", (.06, .94), False, 0.0),
)


def _build_maps(rows) -> tuple[MapDefinition, ...]:
    out = []
    for name, desc, pts, theme, spawns, smooth, rough in rows:
        cover, relief = _rate(pts, spawns)
        out.append(MapDefinition(name.lower().replace(" ", "-"), name, desc, tuple(pts), smooth, rough, spawns,
                                 _THEMES[theme], cover, relief))
    return tuple(out)


MAPS = MAPS + _build_maps(_EXTRA_MAPS)
assert len(MAPS) == 50 and len({m.key for m in MAPS}) == 50

# Tournament reward maps. They are not part of the 50-map library: Level 8, 9 and 10 each unlock one, and
# the level is fought on it. The three build up in spectacle: embers -> thunderstorm -> aurora citadel.
FORGE_MAP = MapDefinition(
    "forge", "THE FORGE", "A lava foundry: molten trench, tall battlements and embers in the air.",
    _mirror([(0, .40), (.10, .40), (.14, .58), (.20, .58), (.23, .30), (.31, .30), (.35, .66), (.40, .66), (.43, .14), (.5, .10)]),
    False, 0.008, (.07, .93),
    Theme((18, 4, 8), (196, 62, 22), (74, 22, 24), (255, 126, 44), (120, 96, 90), (86, 62, 60), (62, 44, 44), (22, 14, 16),
          0.002, False, "embers"),
    3, 3, True, (255, 140, 60))

SPIRE_MAP = MapDefinition(
    "spire", "STORM SPIRE", "Needle spires in a raging thunderstorm. Lightning splits the sky.",
    _mirror([(0, .38), (.10, .38), (.14, .24), (.19, .24), (.21, .72), (.24, .72), (.27, .26), (.35, .26), (.38, .54),
             (.42, .54), (.46, .20), (.5, .80)]),
    False, 0.006, (.06, .94),
    Theme((4, 6, 16), (46, 58, 96), (22, 28, 52), (176, 196, 255), (126, 136, 170), (88, 96, 126), (62, 68, 96), (22, 24, 38),
          0.002, False, "storm"),
    3, 3, True, (150, 205, 255))

# The exclusive Master Map and the finale: bastions, towers, an aurora sky and drifting golden motes.
MASTER_MAP = MapDefinition(
    "citadel", "THE CITADEL", "The Master's fortress: bastions, towers and a burning aurora sky.",
    _mirror([(0, .52), (.13, .52), (.17, .34), (.23, .34), (.26, .74), (.30, .74), (.33, .30), (.41, .30), (.45, .88), (.5, .92)]),
    False, 0.006, (.07, .93),
    Theme((6, 4, 18), (120, 50, 120), (40, 24, 70), (255, 214, 120), (255, 206, 90), (120, 90, 60), (64, 50, 60), (22, 18, 30), 0.008,
          True, "motes"),
    3, 3, True, (255, 206, 100))

# Shop maps: 5 cheap, 5 mid, 5 pro and 3 citadel-level (aurora skies, drifting particles, towering ground).
_TIER_ACCENT = ((170, 205, 175), (120, 190, 255), (255, 165, 90), (255, 206, 100))


def _shop_map(tier: int, name: str, desc: str, half, theme, smooth: bool = False, rough: float = 0.0,
              spawns: tuple = (0.08, 0.92), fx: str = "", aurora: bool = False, buildings: str = "") -> MapDefinition:
    pts = _mirror(half)
    th = _THEMES[theme] if isinstance(theme, str) else theme
    if fx or aurora or buildings:
        th = replace(th, fx=fx or th.fx, aurora=aurora or th.aurora, buildings=buildings or th.buildings)
    cover, relief = _rate(pts, spawns)
    return MapDefinition(name.lower().replace(" ", "-"), name, desc, pts, smooth, rough, spawns, th, cover, relief, True,
                         _TIER_ACCENT[tier])


_SANCTUM = Theme((4, 8, 26), (60, 110, 170), (30, 50, 100), (210, 240, 255), (190, 235, 255), (120, 180, 220), (70, 100, 150),
                 (24, 32, 60), 0.012, True, "snow")
_ECLIPSE = Theme((8, 2, 12), (150, 40, 70), (50, 16, 50), (255, 160, 90), (255, 150, 90), (130, 70, 56), (70, 40, 50),
                 (26, 14, 24), 0.01, True, "embers")
_ASTRAL = Theme((4, 2, 22), (90, 50, 150), (34, 22, 80), (190, 170, 255), (210, 190, 255), (110, 90, 170), (64, 52, 110),
                (22, 18, 44), 0.014, True, "stardust")

SHOP_MAPS: tuple[MapDefinition, ...] = (
    _shop_map(0, "HAYSTACK", "Soft mounds dot an open field.", [(0, .22), (.12, .32), (.24, .2), (.36, .34), (.5, .24)], "steppe", True, 0.01),
    _shop_map(0, "CULVERT", "A shallow drainage ditch splits the field.", [(0, .35), (.3, .35), (.38, .12), (.5, .1)], "swamp"),
    _shop_map(0, "PICNIC KNOLL", "One gentle knoll rises in the middle.", [(0, .2), (.25, .22), (.4, .42), (.5, .46)], "valley", True),
    _shop_map(0, "BOULDER FIELD", "Scattered boulders break every sightline.",
              [(0, .25), (.1, .25), (.13, .4), (.17, .4), (.2, .25), (.3, .25), (.33, .45), (.37, .45), (.4, .25), (.5, .3)], "rust"),
    _shop_map(0, "CROSSROADS", "Flat ground with a low berm at the junction.", [(0, .28), (.4, .28), (.44, .4), (.5, .4)], "waste"),
    _shop_map(1, "ANTHILL", "A terraced mound crawling with ledges.",
              [(0, .15), (.18, .15), (.2, .28), (.28, .28), (.3, .4), (.38, .4), (.4, .55), (.5, .55)], "jungle"),
    _shop_map(1, "TIDEPOOLS", "Rock pools at every stride.",
              [(0, .3), (.1, .3), (.13, .1), (.2, .1), (.23, .3), (.3, .3), (.33, .12), (.4, .12), (.43, .32), (.5, .32)], "beach"),
    _shop_map(1, "WATCH RIDGE", "A long ridge with lookout posts.",
              [(0, .2), (.2, .3), (.3, .55), (.34, .55), (.36, .4), (.46, .45), (.5, .7)], "alpine", False, 0.01),
    _shop_map(1, "SALT FLATS", "A blinding flat studded with posts.", [(0, .2), (.2, .2), (.22, .5), (.24, .5), (.26, .2), (.5, .2)], "steppe"),
    _shop_map(1, "COPPER MINE", "Cut shafts and a towering spoil heap.",
              [(0, .5), (.15, .5), (.2, .2), (.28, .2), (.32, .45), (.4, .2), (.5, .2)], "rust"),
    _shop_map(2, "SKYBRIDGE", "A narrow span high above the void.",
              [(0, .2), (.2, .2), (.24, .04), (.3, .04), (.34, .62), (.5, .62)], "cyber", fx="stardust"),
    _shop_map(2, "THE MAW", "A pit ringed with stone fangs, ash falling.",
              [(0, .35), (.14, .35), (.18, .7), (.2, .35), (.28, .5), (.3, .1), (.5, .05)], "volcano", fx="ash"),
    _shop_map(2, "CLOCKWORK", "Alternating towers tick across the field.",
              [(0, .3), (.1, .3), (.13, .7), (.17, .7), (.2, .3), (.28, .3), (.3, .55), (.34, .55), (.37, .3), (.5, .7)], "steel", fx="stardust"),
    _shop_map(2, "FROZEN FALLS", "Frozen terraces cascade into a deep pool.",
              [(0, .7), (.16, .7), (.2, .55), (.28, .55), (.32, .4), (.4, .4), (.44, .1), (.5, .08)], "arctic", fx="snow"),
    _shop_map(2, "OBELISK FIELD", "Slim obelisks stand in ranks under fireflies.",
              [(0, .2), (.1, .2), (.12, .6), (.14, .2), (.24, .2), (.26, .7), (.28, .2), (.38, .2), (.4, .55), (.42, .2), (.5, .8)],
              "midnight", fx="fireflies"),
    _shop_map(3, "CRYSTAL SANCTUM", "Gem spires under an aurora and falling snow.",
              [(0, .45), (.1, .45), (.14, .28), (.2, .28), (.23, .8), (.26, .8), (.3, .3), (.38, .3), (.42, .9), (.5, .95)],
              _SANCTUM, False, 0.004, (.07, .93)),
    _shop_map(3, "ECLIPSE GATE", "A colossal gate beneath a bleeding sky of embers.",
              [(0, .4), (.1, .4), (.14, .2), (.22, .2), (.24, .85), (.3, .85), (.32, .4), (.4, .4), (.44, .95), (.5, .2)],
              _ECLIPSE, False, 0.004, (.07, .93)),
    _shop_map(3, "ASTRAL BASTION", "Tiered ramparts adrift in a river of stars.",
              [(0, .55), (.12, .55), (.16, .4), (.22, .4), (.25, .7), (.3, .7), (.33, .35), (.4, .35), (.43, .95), (.5, .98)],
              _ASTRAL, False, 0.004, (.07, .93)),
    _shop_map(3, "VOID CATHEDRAL", "Twin spires over a bottomless nave of stars.",
              [(0, .5), (.1, .5), (.13, .3), (.2, .3), (.22, .9), (.25, .9), (.28, .35), (.36, .2), (.4, .2), (.44, .75), (.5, .1)],
              Theme((2, 0, 14), (80, 30, 140), (30, 12, 70), (200, 150, 255), (190, 140, 255), (100, 70, 150), (60, 40, 100),
                    (16, 10, 36), 0.014, True, "stardust"), False, 0.004, (.07, .93)),
    _shop_map(3, "SOLAR TEMPLE", "Stepped golden terraces beneath a burning sky.",
              [(0, .35), (.08, .35), (.11, .5), (.18, .5), (.21, .65), (.28, .65), (.31, .8), (.4, .8), (.44, .98), (.5, .98)],
              Theme((16, 4, 8), (230, 100, 50), (90, 30, 40), (255, 230, 140), (255, 210, 110), (150, 100, 60), (80, 56, 50),
                    (28, 14, 20), 0.008, True, "embers"), False, 0.004, (.07, .93)),
)
_SHOP_BASE_MAPS = SHOP_MAPS
_M = _shop_map
_NEW_MAPS = (
    (  # cheap
        _M(0, "SLEEPY HOLLOW", "A gentle bowl: shells roll to the middle.", [(0, .4), (.2, .25), (.35, .2), (.5, .18)], "valley", True),
        _M(0, "TWIN HILLS", "Two matched hills with a flat between.", [(0, .2), (.1, .35), (.2, .2), (.35, .2), (.45, .4), (.5, .45)], "steppe", True),
        _M(0, "FANG ROW", "A long row of teeth.", [(0, .3), (.05, .4), (.1, .28), (.15, .4), (.2, .28), (.25, .4), (.3, .28), (.35, .4), (.4, .28), (.45, .4), (.5, .3)], "rust"),
        _M(0, "THE DITCH", "Flat ground and one deep, narrow ditch.", [(0, .35), (.4, .35), (.45, .05), (.5, .03)], "swamp"),
        _M(0, "MESA", "Two high mesas and a step down.", [(0, .5), (.2, .5), (.25, .3), (.5, .3)], "canyon"),
        _M(0, "BUMPY ROAD", "Nothing but small, random bumps.", [(0, .3), (.5, .32)], "waste", False, 0.08),
        _M(0, "WAVES", "Rolling swells and troughs.", [(0, .3), (.06, .42), (.12, .3), (.18, .18), (.24, .3), (.3, .42), (.36, .3), (.42, .18), (.5, .3)], "beach", True),
        _M(0, "STAIRCASE", "Climbing steps toward the middle.", [(0, .15), (.1, .15), (.12, .25), (.22, .25), (.24, .35), (.34, .35), (.36, .45), (.5, .45)], "alpine"),
        _M(0, "FOXHOLES", "Small pits to duck into.", [(0, .3), (.08, .3), (.1, .15), (.14, .15), (.16, .3), (.28, .3), (.3, .15), (.34, .15), (.36, .3), (.5, .3)], "valley"),
        _M(0, "LONE TREE", "One tall spire in a flat meadow.", [(0, .2), (.45, .2), (.48, .7), (.5, .72)], "jungle", False, 0.0, (.08, .92), "fireflies")),
    (  # mid
        _M(1, "NAVE", "Twin arch towers flanking a long hall.", [(0, .25), (.12, .25), (.14, .6), (.17, .6), (.19, .25), (.3, .25), (.34, .5), (.5, .5)], "midnight"),
        _M(1, "HANGING GARDENS", "Alternating green terraces.", [(0, .5), (.1, .5), (.12, .3), (.2, .3), (.22, .55), (.3, .55), (.32, .35), (.5, .35)], "jungle", False, 0.0, (.08, .92), "fireflies"),
        _M(1, "CREVASSE", "A glacier split by a deep crack.", [(0, .6), (.15, .6), (.2, .1), (.24, .1), (.28, .5), (.5, .5)], "arctic", False, 0.0, (.08, .92), "snow"),
        _M(1, "SLUDGE PITS", "Toxic pools between low banks.", [(0, .3), (.1, .3), (.14, .08), (.22, .08), (.26, .3), (.34, .3), (.38, .1), (.5, .1)], "toxic", False, 0.0, (.08, .92), "ash"),
        _M(1, "VOLCANO RIM", "A central caldera and ember rain.", [(0, .2), (.2, .2), (.3, .55), (.38, .7), (.42, .35), (.5, .3)], "volcano", False, 0.0, (.08, .92), "embers"),
        _M(1, "OASIS", "Dunes around a sunken oasis.", [(0, .4), (.12, .3), (.22, .45), (.34, .25), (.4, .12), (.5, .1)], "steppe", True),
        _M(1, "RAIL YARD", "Low walls like freight cars.", [(0, .2), (.1, .2), (.11, .35), (.14, .35), (.15, .2), (.3, .2), (.31, .35), (.34, .35), (.35, .2), (.5, .2)], "steel"),
        _M(1, "MOON BASE", "Cratered grey regolith.", [(0, .3), (.1, .3), (.14, .1), (.22, .1), (.26, .32), (.36, .32), (.4, .15), (.5, .15)], "moon", False, 0.0, (.08, .92), "stardust"),
        _M(1, "WINDMILL FIELD", "Hills with tall thin windmills.", [(0, .25), (.1, .35), (.12, .55), (.13, .35), (.25, .3), (.3, .5), (.31, .3), (.5, .25)], "valley"),
        _M(1, "BONE YARD", "Ribs of some enormous creature.", [(0, .2), (.08, .4), (.1, .2), (.16, .45), (.18, .2), (.24, .5), (.26, .2), (.32, .4), (.34, .2), (.5, .22)], "rust", False, 0.0, (.08, .92), "ash")),
    (  # pro
        _M(2, "THE SPIRAL", "Terraces wind up to a central summit.", [(0, .1), (.1, .1), (.12, .2), (.2, .2), (.22, .3), (.3, .3), (.32, .45), (.4, .45), (.42, .6), (.5, .6)], "cyber", False, 0.0, (.08, .92), "stardust"),
        _M(2, "SKYSCRAPER ROW", "A downtown of sheer towers.", [(0, .3), (.08, .3), (.09, .7), (.14, .7), (.15, .3), (.2, .3), (.21, .9), (.26, .9), (.27, .3), (.32, .3), (.33, .6), (.38, .6), (.39, .3), (.5, .3)], "midnight", False, 0.0, (.08, .92), "stardust", False, "skyline"),
        _M(2, "THE FUNNEL", "A huge V: everything slides inward.", [(0, .7), (.2, .55), (.35, .25), (.5, .04)], "steel"),
        _M(2, "ICE SPIKES", "Needles of frozen rock.", [(0, .3), (.06, .65), (.09, .3), (.15, .8), (.18, .3), (.25, .55), (.28, .3), (.5, .3)], "arctic", False, 0.0, (.08, .92), "snow"),
        _M(2, "LAVA FLOW", "Cooled cliffs stepping down to the fire.", [(0, .8), (.1, .8), (.12, .55), (.25, .55), (.27, .3), (.4, .3), (.42, .1), (.5, .1)], "volcano", False, 0.0, (.08, .92), "embers"),
        _M(2, "TWIN FORTS", "Two fortress blocks and a killing field.", [(0, .2), (.05, .2), (.06, .7), (.2, .7), (.21, .5), (.25, .5), (.26, .15), (.5, .15)], "rust", False, 0.0, (.08, .92), "", False, "castle"),
        _M(2, "CORAL REEF", "Organic, lumpy cover.", [(0, .2), (.05, .45), (.1, .3), (.16, .55), (.22, .25), (.28, .5), (.34, .3), (.4, .45), (.5, .3)], "beach", False, 0.0, (.08, .92), "fireflies"),
        _M(2, "TWIN CLIFFS", "Sheer cliffs at both edges.", [(0, .9), (.12, .9), (.14, .3), (.5, .1)], "canyon"),
        _M(2, "PIPE ORGAN", "Columns of rising height.", [(0, .2), (.04, .2), (.05, .35), (.1, .35), (.11, .5), (.16, .5), (.17, .65), (.22, .65), (.23, .8), (.28, .8), (.29, .3), (.5, .3)], "cyber"),
        _M(2, "SHATTERED MOON", "Floating-looking islands between bottomless gaps.", [(0, .4), (.06, .4), (.08, .05), (.12, .05), (.14, .55), (.2, .55), (.22, .05), (.26, .05), (.28, .45), (.34, .45), (.36, .05), (.5, .05)], "moon", False, 0.0, (.08, .92), "stardust")),
    (  # master tier
        _M(3, "OBSIDIAN SPIRES", "Jagged black glass over a lava sky.", [(0, .4), (.06, .8), (.1, .35), (.16, .95), (.2, .3), (.28, .85), (.34, .3), (.42, .98), (.5, .5)], _ECLIPSE, False, 0.004, (.07, .93)),
        _M(3, "AURORA FJORD", "A narrow valley under a curtain of light.", [(0, .98), (.12, .98), (.16, .5), (.2, .08), (.5, .05)], replace(_THEMES["arctic"], aurora=True, fx="snow"), False, 0.0, (.07, .93)),
        _M(3, "CRYSTAL CAVERNS", "A field of glittering crystal teeth.", [(0, .3), (.04, .6), (.08, .25), (.12, .8), (.16, .3), (.22, .7), (.26, .25), (.32, .9), (.36, .3), (.44, .75), (.5, .4)], _SANCTUM, False, 0.0, (.07, .93)),
        _M(3, "STARFALL PLATEAU", "A high plateau with a single spire.", [(0, .6), (.35, .6), (.4, .55), (.44, .55), (.46, .98), (.5, .98)], _ASTRAL, False, 0.003, (.07, .93)),
        _M(3, "SOLAR FORGE", "Stepped furnace terraces under a burning sky.", [(0, .15), (.08, .15), (.1, .3), (.18, .3), (.2, .5), (.28, .5), (.3, .7), (.38, .7), (.4, .9), (.5, .9)], _ECLIPSE, False, 0.0, (.07, .93)),
        _M(3, "NEBULA GARDENS", "Floating gardens in a violet river of stars.", [(0, .45), (.08, .45), (.1, .25), (.2, .25), (.22, .6), (.3, .6), (.32, .3), (.42, .3), (.44, .75), (.5, .75)], _ASTRAL, False, 0.0, (.07, .93)),
        _M(3, "TITAN'S THRONE", "A colossal stone chair at the centre.", [(0, .3), (.15, .3), (.16, .55), (.22, .55), (.23, .3), (.3, .3), (.31, .7), (.4, .7), (.41, .95), (.5, .95)], replace(_THEMES["steel"], aurora=True), False, 0.0, (.07, .93)),
        _M(3, "GLITCH ZONE", "Corrupted, chaotic terrain.", [(0, .4), (.5, .4)], replace(_THEMES["cyber"], aurora=True, fx="stardust"), False, 0.22, (.07, .93)),
        _M(3, "THE LABYRINTH", "A maze of tall and short walls.", [(0, .2), (.05, .2), (.06, .7), (.08, .7), (.09, .2), (.14, .2), (.15, .7), (.17, .7), (.18, .2), (.23, .2), (.24, .7), (.26, .7), (.27, .2), (.32, .2), (.33, .7), (.35, .7), (.36, .2), (.5, .2)], replace(_THEMES["midnight"], aurora=True), False, 0.0, (.07, .93)),
        _M(3, "CELESTIAL STEPS", "Giant stairs that climb into the aurora.", [(0, .1), (.06, .1), (.08, .22), (.16, .22), (.18, .36), (.26, .36), (.28, .5), (.36, .5), (.38, .66), (.5, .66)], _SANCTUM, False, 0.0, (.07, .93))),
)
_PREMIUM_ACCENT = (255, 70, 140)
_PREMIUM_MAPS = tuple(replace(m, accent=_PREMIUM_ACCENT) for m in (
    _M(3, "GLASS DESERT", "Dunes of fused glass under twin suns: every shell throws prisms.",
       [(0, .3), (.06, .42), (.12, .28), (.2, .5), (.28, .3), (.36, .44), (.44, .26), (.5, .34)],
       Theme((30, 10, 66), (255, 150, 104), (116, 62, 110), (255, 232, 170), (150, 232, 244), (92, 182, 204), (62, 112, 142), (20, 40, 72), 0.004, False, "stardust"), True, 0.01),
    _M(3, "LANTERN TERRACES", "Stepped temple terraces under a sky of rising lanterns.",
       [(0, .15), (.06, .15), (.08, .28), (.16, .28), (.18, .42), (.26, .42), (.28, .56), (.4, .56), (.42, .62), (.5, .62)],
       Theme((8, 6, 30), (196, 76, 62), (62, 22, 42), (255, 204, 124), (232, 172, 84), (152, 84, 52), (82, 42, 42), (30, 16, 24), 0.006, False, "fireflies", "pagoda")),
    _M(3, "WORLDTREE ROOTS", "Colossal glowing roots arch over bottomless hollows.",
       [(0, .45), (.04, .6), (.1, .2), (.16, .52), (.22, .18), (.3, .55), (.38, .22), (.44, .5), (.5, .35)],
       Theme((2, 14, 24), (40, 164, 134), (10, 62, 62), (192, 255, 232), (112, 242, 172), (52, 152, 112), (30, 82, 70), (10, 30, 34), 0.008, True, "fireflies"), True),
    _M(3, "WHALEFALL", "The bleached ribs of a sky-whale, arching over the dark.",
       [(0, .2), (.04, .5), (.08, .2), (.12, .55), (.16, .2), (.2, .6), (.24, .2), (.28, .55), (.32, .2), (.36, .5), (.4, .2), (.5, .25)],
       Theme((2, 6, 24), (32, 92, 164), (16, 42, 92), (204, 232, 255), (224, 232, 242), (162, 176, 202), (92, 102, 132), (30, 34, 60), 0.01, True, "stardust")),
    _M(3, "SHATTERLINE", "Floating plates of cracked glass above a prismatic void.",
       [(0, .4), (.05, .4), (.07, .02), (.1, .02), (.12, .5), (.2, .5), (.22, .02), (.26, .02), (.28, .62), (.36, .62), (.38, .02), (.44, .02), (.46, .45), (.5, .45)],
       Theme((10, 4, 28), (122, 62, 202), (62, 32, 112), (255, 255, 255), (222, 202, 255), (152, 132, 232), (92, 72, 172), (30, 20, 70), 0.012, True, "stardust")),
))
SHOP_MAPS = (_SHOP_BASE_MAPS[0:5] + _NEW_MAPS[0] + _SHOP_BASE_MAPS[5:10] + _NEW_MAPS[1] + _SHOP_BASE_MAPS[10:15] + _NEW_MAPS[2]
             + _SHOP_BASE_MAPS[15:20] + _NEW_MAPS[3] + _PREMIUM_MAPS)
assert len(SHOP_MAPS) == 65

_SPACE = Theme((2, 4, 24), (40, 70, 170), (16, 24, 80), (190, 220, 255), (170, 210, 255), (90, 120, 180), (50, 70, 120),
               (14, 20, 44), 0.014, True, "stardust")
_ROCK = Theme((4, 2, 14), (60, 50, 80), (24, 20, 40), (200, 190, 230), (170, 160, 190), (110, 100, 120), (70, 64, 80),
              (20, 18, 30), 0.014, True, "stardust")
_ALIEN = Theme((2, 10, 16), (30, 150, 110), (10, 60, 70), (150, 255, 190), (120, 255, 170), (60, 140, 110), (30, 80, 70),
               (8, 26, 30), 0.012, True, "stardust")
MISSION_MAPS: tuple[MapDefinition, ...] = (
    _shop_map(1, "BUNKER LINE", "Trenches and a ruined bunker under falling ash.",
              [(0, .3), (.1, .3), (.13, .12), (.2, .12), (.23, .3), (.3, .3), (.33, .45), (.4, .2), (.5, .2)], "rust", fx="ash"),
    _shop_map(1, "RADAR HILL", "A lonely hill crowned by a humming radar.",
              [(0, .2), (.2, .3), (.35, .6), (.4, .62), (.45, .5), (.5, .55)], "alpine", fx="fireflies"),
    _shop_map(2, "SHELLED FIELD", "A cratered field, still smouldering.",
              [(0, .3), (.08, .3), (.12, .12), (.18, .12), (.22, .32), (.3, .32), (.34, .14), (.4, .14), (.44, .34), (.5, .34)],
              "waste", fx="embers"),
    _shop_map(3, "SPACE ELEVATOR", "A cable to the stars rises from the middle of the field.",
              [(0, .25), (.25, .25), (.3, .4), (.4, .4), (.43, .98), (.5, .98)], _SPACE, False, 0.004, (.07, .93)),
    _shop_map(3, "ASTEROID FIELD", "Jagged rock in an endless starfield.",
              [(0, .4), (.06, .55), (.1, .3), (.16, .6), (.22, .25), (.3, .7), (.36, .3), (.42, .65), (.5, .5)], _ROCK, False, 0.004, (.07, .93)),
    _shop_map(3, "MOTHERSHIP", "The alien flagship's flight deck, lit in green.",
              [(0, .3), (.1, .3), (.14, .5), (.2, .5), (.24, .3), (.3, .3), (.34, .55), (.38, .55), (.42, .4), (.5, .6)],
              _ALIEN, False, 0.004, (.07, .93), "", True),
)
_K = lambda name, desc, half, theme, fx="", aurora=False, b="": _shop_map(3, name, desc, half, theme, False, 0.0, (.07, .93), fx, aurora, b)
TICKET_MAPS: tuple[MapDefinition, ...] = (
    _K("NEON SKYLINE", "A glittering city of towers behind the battlefield.", [(0, .3), (.15, .3), (.17, .5), (.22, .5), (.24, .3), (.34, .3), (.36, .4), (.5, .4)], "midnight", "stardust", False, "skyline"),
    _K("CLOCK PLAZA", "A vast clock tower rises over the plaza.", [(0, .3), (.2, .3), (.3, .2), (.5, .2)], "steel", "", False, "clock"),
    _K("FROZEN CATHEDRAL", "A cathedral of ice under the aurora.", [(0, .4), (.1, .4), (.15, .55), (.2, .4), (.35, .25), (.5, .25)], "arctic", "snow", True, "cathedral"),
    _K("SUN TEMPLE", "A great stepped temple catches the last light.", [(0, .25), (.12, .25), (.14, .4), (.22, .4), (.24, .55), (.32, .55), (.34, .7), (.5, .7)], _ECLIPSE, "", False, "temple"),
    _K("SUNKEN METROPOLIS", "Broken towers of a drowned city.", [(0, .15), (.1, .15), (.12, .35), (.15, .12), (.25, .12), (.28, .4), (.5, .1)], "midnight", "fireflies", False, "ruins"),
    _K("PYRAMID PLAIN", "Three pyramids on the horizon.", [(0, .2), (.2, .2), (.3, .3), (.5, .3)], "steppe", "ash", False, "pyramids"),
    _K("CHEMICAL WORKS", "Chimneys and sawtooth roofs, smoke in the air.", [(0, .3), (.1, .3), (.12, .15), (.2, .15), (.22, .3), (.5, .3)], "toxic", "ash", False, "factory"),
    _K("GOTHIC KEEP", "A castle of towers and battlements.", [(0, .25), (.15, .25), (.17, .6), (.24, .6), (.26, .25), (.5, .25)], "midnight", "", False, "castle"),
    _K("ORBITAL CITY", "A space station city above a blue world.", [(0, .35), (.2, .35), (.25, .5), (.35, .5), (.4, .35), (.5, .35)], _SPACE, "stardust", True, "station"),
    _K("PAGODA PEAK", "Tiered pagodas on a snowy summit.", [(0, .5), (.15, .5), (.2, .35), (.3, .35), (.35, .6), (.5, .6)], "alpine", "snow", False, "pagoda"),
)
_EMBER = Theme((16, 2, 6), (196, 54, 18), (70, 16, 12), (255, 160, 70), (56, 44, 50), (255, 120, 40), (60, 30, 32), (22, 10, 12), 0.003,
               False, "inferno", "volcano")
_TEMPEST = Theme((4, 16, 30), (40, 110, 140), (14, 44, 70), (190, 240, 255), (70, 100, 120), (120, 200, 230), (40, 70, 90), (14, 26, 40), 0.004,
                 True, "tempest", "isles")
_RIFT = Theme((4, 0, 18), (210, 170, 110), (46, 24, 90), (255, 244, 205), (46, 38, 86), (255, 226, 150), (40, 28, 76), (12, 8, 34), 0.012,
              True, "rift", "rift")
ANCIENT_MAPS: tuple[MapDefinition, ...] = tuple(replace(m, accent=a) for m, a in (
    (_shop_map(3, "EMBER SANCTUM", "A volcanic temple over a lava chasm. Pillars of basalt, rivers of fire.",
               [(0, .34), (.1, .34), (.12, .52), (.17, .52), (.19, .30), (.26, .30), (.28, .60), (.32, .60), (.34, .40), (.4, .18), (.45, .06), (.5, .04)],
               _EMBER, False, 0.006, (.07, .93)), (255, 150, 60)),
    (_shop_map(3, "TEMPEST SPIRES", "Storm-lashed spires above floating isles. Rain, wind and lightning.",
               [(0, .30), (.08, .30), (.10, .58), (.15, .58), (.17, .30), (.22, .30), (.24, .62), (.27, .62), (.29, .25), (.36, .25), (.38, .50), (.42, .50), (.44, .12), (.5, .12)],
               _TEMPEST, False, 0.0, (.07, .93)), (120, 232, 255)),
    (_shop_map(3, "ASTRAL RIFT", "The end of time: a colossal ring of light and dark turns over a shattered world.",
               [(0, .40), (.06, .40), (.08, .55), (.14, .55), (.16, .32), (.22, .32), (.25, .50), (.28, .70), (.31, .50), (.34, .30), (.40, .20), (.44, .46), (.47, .46), (.48, .20), (.5, .20)],
               _RIFT, False, 0.0, (.07, .93)), (214, 170, 255)),
))
SPECIAL_MAPS: tuple[MapDefinition, ...] = (FORGE_MAP, SPIRE_MAP, MASTER_MAP) + SHOP_MAPS + MISSION_MAPS + TICKET_MAPS + ANCIENT_MAPS
SPECIAL_MAP_BY_KEY = {m.key: m for m in SPECIAL_MAPS}
assert len({m.key for m in MAPS + SPECIAL_MAPS}) == len(MAPS) + len(SPECIAL_MAPS)


class Terrain:
    """Column heightmap; craters carve columns, so the battlefield is destructible."""

    def __init__(self, width: int, height: int, mapdef: MapDefinition, rng: random.Random) -> None:
        self.width, self.height = width, height
        self.version = 0
        phases = [rng.uniform(0, math.tau) for _ in range(3)]
        scale = min(height, width * 0.55) * 0.74
        self.heights: list[int] = []
        for x in range(width):
            u = x / max(1, width - 1)
            h = sample_profile(mapdef.profile, u, mapdef.smooth)
            r = mapdef.roughness
            if r:
                h += r * (math.sin(u * math.tau * 7 + phases[0]) + 0.5 * math.sin(u * math.tau * 17 + phases[1])
                          + 0.25 * math.sin(u * math.tau * 41 + phases[2]))
            self.heights.append(int(clamp(h * scale + 2, 2, height - 10)))
        self.scorch = [0.0] * width

    def height_at(self, x: float) -> int:
        return self.heights[int(clamp(x, 0, self.width - 1))]

    def support_height(self, x: float) -> int:
        xi = int(x)
        return min(self.heights[max(0, xi - 5):min(self.width, xi + 6)])

    def flatten(self, cx: float, half: int = 7) -> None:
        xc = int(cx)
        target = self.heights[xc]
        for x in range(max(0, xc - half), min(self.width, xc + half + 1)):
            edge = half - abs(x - xc)
            weight = 1.0 if edge >= 3 else edge / 3
            self.heights[x] = int(round(lerp(self.heights[x], target, weight)))

    def carve(self, cx: float, cy: float, r: float) -> tuple[int, int]:
        x0, x1 = max(0, int(cx - r) - 1), min(self.width - 1, int(cx + r) + 1)
        for x in range(x0, x1 + 1):
            dx = x + 0.5 - cx
            if abs(dx) > r:
                continue
            dy = math.sqrt(r * r - dx * dx)
            low, h = cy - dy, self.heights[x]
            if low < h and cy + dy >= h - 1:
                self.heights[x] = max(0, int(low))
                self.scorch[x] = max(self.scorch[x], 1 - abs(dx) / r * 0.6)
        self.version += 1
        return x0, x1


def _terrain_mound(self, cx: float, r: float, height: float) -> tuple[int, int]:
    """Builds earth up in a bell shape (a shell that makes ground instead of destroying it)."""
    x0, x1 = max(0, int(cx - r) - 1), min(self.width - 1, int(cx + r) + 1)
    for x in range(x0, x1 + 1):
        u = (x + 0.5 - cx) / r
        if abs(u) <= 1:
            self.heights[x] = int(min(self.height - 12, self.heights[x] + height * (1 - u * u)))
    self.version += 1
    return x0, x1


def _terrain_trench(self, cx: float, half: float, depth: float) -> tuple[int, int]:
    """A narrow, deep shaft."""
    x0, x1 = max(0, int(cx - half) - 1), min(self.width - 1, int(cx + half) + 1)
    for x in range(x0, x1 + 1):
        u = abs(x + 0.5 - cx) / half
        if u <= 1:
            self.heights[x] = max(1, int(self.heights[x] - depth * (1 - 0.5 * u * u)))
            self.scorch[x] = max(self.scorch[x], 0.8)
    self.version += 1
    return x0, x1


def _terrain_smooth(self, cx: float, r: float) -> tuple[int, int]:
    """Glassing: melts the ground in the radius down to one flat level."""
    x0, x1 = max(0, int(cx - r) - 1), min(self.width - 1, int(cx + r) + 1)
    seg = [self.heights[x] for x in range(x0, x1 + 1)]
    level = sum(seg) // max(1, len(seg))
    for x in range(x0, x1 + 1):
        u = abs(x + 0.5 - cx) / r
        if u <= 1:
            self.heights[x] = int(lerp(self.heights[x], level, 1.0 if u < 0.7 else (1 - u) / 0.3))
            self.scorch[x] = max(self.scorch[x], 0.4)
    self.version += 1
    return x0, x1


Terrain.mound, Terrain.trench, Terrain.smooth = _terrain_mound, _terrain_trench, _terrain_smooth


# ---- Background buildings, baked once into a map's sky. Every style is a few primitives on a baseline; windows are lit
# deterministically so both LAN machines (and every redraw) show the same city.
def _b_rect(put, x0, y0, w, h, col):
    for x in range(x0, x0 + w):
        for y in range(y0, y0 + h):
            put(x, y, col)


def _b_tri(put, cx, y0, half, h, col):
    for k in range(h):
        hw = int(half * (1 - k / h))
        for x in range(cx - hw, cx + hw + 1):
            put(x, y0 + k, col)


def _b_dome(put, cx, y0, rx, ry, col):
    for x in range(cx - rx, cx + rx + 1):
        for y in range(y0, y0 + ry):
            if ((x - cx) / rx) ** 2 + ((y - y0) / ry) ** 2 <= 1:
                put(x, y, col)


def _b_windows(put, x0, y0, w, h, lit, seed):
    for x in range(x0 + 1, x0 + w - 1, 2):
        for y in range(y0 + 2, y0 + h - 1, 3):
            if (x * 7 + y * 13 + seed) % 5 < 3:
                put(x, y, lit)


def _bake_buildings(kind: str, w: int, h: int, base_y: int, col: RGB, lit: RGB, put) -> None:
    r = random.Random(w * 31 + h * 7 + sum(map(ord, kind)))
    if kind == "skyline":
        x = 2
        while x < w - 4:
            bw, bh = r.randint(5, 9), int(h * r.uniform(0.22, 0.62))
            _b_rect(put, x, base_y, bw, bh, col)
            _b_windows(put, x, base_y, bw, bh, lit, x)
            if r.random() < 0.4:
                _b_rect(put, x + bw // 2, base_y + bh, 1, r.randint(3, 7), col)
            x += bw + r.randint(0, 2)
    elif kind == "clock":
        cx = w // 2
        _b_rect(put, cx - 6, base_y, 12, int(h * 0.55), col)
        _b_tri(put, cx, base_y + int(h * 0.55), 8, int(h * 0.22), col)
        for dx in range(-4, 5):
            for dy in range(-4, 5):
                if dx * dx + dy * dy <= 16:
                    put(cx + dx, base_y + int(h * 0.42) + dy, lit if dx * dx + dy * dy > 6 else shade(lit, 0.5))
        for sx in (w // 6, w - w // 6):
            _b_rect(put, sx - 5, base_y, 10, int(h * 0.3), col)
            _b_windows(put, sx - 5, base_y, 10, int(h * 0.3), lit, sx)
    elif kind == "cathedral":
        cx = w // 2
        _b_rect(put, cx - 14, base_y, 28, int(h * 0.3), col)
        _b_tri(put, cx, base_y + int(h * 0.3), 14, int(h * 0.14), col)
        for sx in (cx - 18, cx + 18):
            _b_rect(put, sx - 3, base_y, 6, int(h * 0.55), col)
            _b_tri(put, sx, base_y + int(h * 0.55), 4, int(h * 0.2), col)
        _b_dome(put, cx, base_y + 3, 6, int(h * 0.2), lit)
    elif kind == "temple":
        cx = w // 2
        for i in range(6):
            _b_rect(put, cx - 24 + i * 4, base_y + i * 4, 48 - i * 8, 4, col)
        _b_rect(put, cx - 3, base_y + 24, 6, 5, lit)
        for sx in (w // 7, w - w // 7):
            for k in range(4):
                _b_rect(put, sx + k * 4 - 8, base_y, 2, int(h * 0.3), col)
    elif kind == "ruins":
        x = 4
        while x < w - 6:
            bh = int(h * r.uniform(0.1, 0.45))
            _b_rect(put, x, base_y, 3, bh, col)
            if r.random() < 0.5:
                _b_rect(put, x, base_y + bh, 9, 2, col)
            x += r.randint(8, 14)
    elif kind == "pyramids":
        for cx, ph in ((w // 5, 0.45), (w // 2, 0.6), (w - w // 4, 0.35)):
            _b_tri(put, cx, base_y, int(h * ph * 0.9), int(h * ph), col)
            put(cx, base_y + int(h * ph), lit)
    elif kind == "factory":
        x = 3
        while x < w - 8:
            bw = r.randint(10, 16)
            _b_rect(put, x, base_y, bw, int(h * 0.2), col)
            for sx in range(x, x + bw - 3, 5):
                _b_tri(put, sx + 2, base_y + int(h * 0.2), 2, 4, col)
            if r.random() < 0.7:
                _b_rect(put, x + bw - 3, base_y, 3, int(h * r.uniform(0.35, 0.6)), col)
            _b_windows(put, x, base_y, bw, int(h * 0.2), lit, x)
            x += bw + 2
    elif kind == "castle":
        _b_rect(put, 2, base_y, w - 4, int(h * 0.22), col)
        for x in range(2, w - 4, 4):
            _b_rect(put, x, base_y + int(h * 0.22), 2, 3, col)
        for sx in (w // 8, w // 3, w - w // 3, w - w // 8):
            _b_rect(put, sx - 4, base_y, 8, int(h * 0.5), col)
            _b_tri(put, sx, base_y + int(h * 0.5), 5, int(h * 0.16), col)
            _b_windows(put, sx - 4, base_y, 8, int(h * 0.5), lit, sx)
    elif kind == "station":
        cx = w // 2
        _b_dome(put, cx, base_y, 16, int(h * 0.3), col)
        _b_rect(put, cx - 30, base_y + int(h * 0.12), 60, 3, col)
        for sx in (cx - 30, cx + 30):
            _b_rect(put, sx - 1, base_y, 3, int(h * 0.4), col)
            put(sx, base_y + int(h * 0.4), lit)
        _b_windows(put, cx - 14, base_y, 28, int(h * 0.2), lit, 3)
    elif kind == "volcano":
        for cx, ph, hw in ((w // 4, 0.58, 0.30), (w - w // 3, 0.44, 0.24)):
            _b_tri(put, cx, base_y, int(h * hw * 2.6), int(h * ph), col)
            top = base_y + int(h * ph)
            for dx in range(-3, 4):
                put(cx + dx, top, lit)
            for k in range(7):
                put(cx + (k % 2) * 2 - 1, top - 2 - k * 3, lit)
    elif kind == "isles":
        for cx, cy, rw in ((w // 6, 0.55, 12), (w // 2, 0.7, 18), (w - w // 5, 0.5, 13), (w // 3, 0.38, 8), (w - w // 3, 0.36, 9)):
            y0 = base_y + int(h * cy)
            _b_rect(put, cx - rw, y0, 2 * rw, 2, col)
            for k in range(1, rw):
                _b_rect(put, cx - rw + k, y0 - k // 2 - 1, 2 * (rw - k), 1, col)
            _b_rect(put, cx - 1, y0 + 2, 3, 6 + rw // 3, col)
            put(cx, y0 + 7 + rw // 3, lit)
    elif kind == "rift":
        cx, cy, R = w // 2, base_y + int(h * 0.5), max(6, int(h * 0.34))
        for a in range(0, 720):
            ang = a * math.pi / 360
            for rr in (R - 1, R, R + 1):
                put(int(cx + math.cos(ang) * rr), int(cy + math.sin(ang) * rr), lit if rr == R else col)
        for sx in (w // 7, w - w // 7):
            _b_rect(put, sx - 3, base_y, 3, int(h * 0.5), col)
            _b_rect(put, sx + 4, base_y, 3, int(h * 0.35), col)
            _b_rect(put, sx - 3, base_y + int(h * 0.5), 10, 2, col)
        for _ in range(9):
            px, py = r.randint(4, max(5, w - 5)), base_y + int(h * r.uniform(0.3, 0.8))
            _b_tri(put, px, py, 2, r.randint(3, 6), col)
    elif kind == "pagoda":
        for cx, sc in ((w // 4, 0.8), (w // 2, 1.0), (w - w // 4, 0.8)):
            for tier in range(4):
                tw = int((14 - tier * 3) * sc)
                _b_rect(put, cx - tw // 2, base_y + tier * int(h * 0.1), tw, int(h * 0.07), col)
                _b_rect(put, cx - tw // 2 - 2, base_y + tier * int(h * 0.1) + int(h * 0.07), tw + 4, 2, col)
            _b_rect(put, cx, base_y + 4 * int(h * 0.1) + 2, 1, 5, col)


BUILDING_STYLES = ("skyline", "clock", "cathedral", "temple", "ruins", "pyramids", "factory", "castle", "station", "pagoda", "volcano", "isles", "rift")


class Scenery:
    """Sky, distant ridges, stars and the shaded terrain layer (cached, patched per crater)."""

    def __init__(self, width: int, height: int, theme: Theme, rng: random.Random) -> None:
        self.w, self.h, self.theme = width, height, theme
        self.sky = self._build_sky(rng)
        if theme.buildings:
            col = mix(theme.sky_bottom, (0, 0, 0), 0.62)
            lit = mix(theme.sun if hasattr(theme, "sun") else (255, 220, 140), (255, 230, 150), 0.5)

            def put(x, y, c, sky=self.sky, hh=height, ww=width):
                if 0 <= x < ww and 0 <= y < hh:
                    sky[hh - 1 - y][x] = c
            _bake_buildings(theme.buildings, width, height, int(height * 0.2), col, lit, put)
        self.rows = [r[:] for r in self.sky]
        self.stars = []
        for _ in range(int(width * height * theme.star_density * 3) + 8):
            self.stars.append((rng.randrange(width), rng.randrange(0, max(1, int(height * 0.38))),
                               rng.uniform(0, math.tau), rng.uniform(1.0, 3.0), rng.uniform(0.4, 1.0)))
        self._terrain: Optional[Terrain] = None
        self._aur_key: Optional[int] = None
        self._aur_cells: list = []
        self._bolt_key: Optional[int] = None
        self._bolt: list = []
        self.motes: list = []
        if theme.fx in _MOTE_STYLES:        # only fx maps draw extra random numbers, so other maps stay seed-identical
            n = max(16, int(width * height * _MOTE_STYLES[theme.fx][0]))
            self.motes = [(rng.uniform(0, width), rng.uniform(0, height), rng.uniform(0.6, 1.4), rng.uniform(0, math.tau),
                           rng.uniform(0.5, 2.5), rng.randrange(3)) for _ in range(n)]

    def _build_sky(self, rng: random.Random) -> list[list[RGB]]:
        th, w, h = self.theme, self.w, self.h
        rows = [[mix(th.sky_top, th.sky_bottom, (py / max(1, h - 1)) ** 1.4)] * w for py in range(h)]
        ox, oy, r = int(w * rng.uniform(0.35, 0.65)), int(h * 0.7), max(3, int(h * 0.09))
        for py in range(max(0, h - 1 - oy - 3 * r), min(h, h - 1 - oy + 3 * r)):
            for x in range(max(0, ox - 3 * r), min(w, ox + 3 * r)):
                d = math.hypot(x - ox, (h - 1 - py) - oy)
                if d <= r:
                    rows[py][x] = mix(rows[py][x], th.orb, 0.95)
                elif d < 3 * r:
                    rows[py][x] = mix(rows[py][x], th.orb, 0.35 * (1 - (d - r) / (2 * r)) ** 2)
        ph = [rng.uniform(0, math.tau) for _ in range(6)]
        for layer, (base, k, dark) in enumerate(((0.36, 0.55, 1.0), (0.24, 0.75, 0.7))):
            far = shade(th.far, dark)
            for x in range(w):
                u = x / w
                ridge = h * (base + 0.08 * math.sin(u * math.tau * 3 + ph[layer])
                             + 0.05 * math.sin(u * math.tau * 8 + ph[layer + 2]) + 0.02 * math.sin(u * math.tau * 21 + ph[layer + 4]))
                for py in range(max(0, h - 1 - int(ridge)), h):
                    rows[py][x] = mix(rows[py][x], far, k)
        return rows

    def _pixel(self, x: int, y: int, h: int, scorch: float) -> RGB:
        th = self.theme
        depth = h - 1 - y
        if depth == 0:
            c = th.surface
        elif depth < 3:
            c = mix(th.topsoil, th.soil, depth / 3)
        else:
            c = mix(th.soil, th.deep, min(1.0, (depth - 3) / (self.h * 0.5)))
        n = ((x * 374761393 + y * 668265263) ^ (x * y * 2246822519 + 1013904223)) & 0xFF
        if depth < 4 and scorch > 0.02:
            c = shade(c, 1 - 0.6 * scorch)
        return shade(c, 0.94 + n / 255 * 0.12)

    def apply_terrain(self, terrain: Terrain, x0: int = 0, x1: Optional[int] = None) -> None:
        self._terrain = terrain
        x1 = self.w - 1 if x1 is None else min(x1, self.w - 1)
        h = self.h
        for x in range(max(0, x0), x1 + 1):
            top = terrain.heights[x]
            sc = terrain.scorch[x]
            for py in range(h):
                y = h - 1 - py
                self.rows[py][x] = self._pixel(x, y, top, sc) if y < top else self.sky[py][x]

    def canvas(self) -> PixelCanvas:
        return PixelCanvas(self.w, self.h, rows=[r[:] for r in self.rows])

    def draw_stars(self, canvas: PixelCanvas, t: float) -> None:
        for x, py, ph, sp, b in self.stars:
            if canvas.rows[py][x] == self.sky[py][x]:
                canvas.rows[py][x] = mix(canvas.rows[py][x], (235, 240, 255), (0.3 + 0.5 * math.sin(t * sp + ph) ** 2) * b)

    def _build_aurora(self, c: float) -> list:
        """Aurora ribbon cells (row, column, final colour) for animation time c.

        Hue and strength are quantised so the sky only ever uses a few hundred distinct colours (it used to mint
        thousands of new ones per frame, which flooded the terminal and the colour cache), and the ribbons are only
        rebuilt ten times a second instead of every frame."""
        h, w, sky = self.h, self.w, self.sky
        cells = []
        for b in range(3):
            for x in range(w):
                mid = h * (0.66 + 0.08 * b) + h * 0.05 * math.sin(x * 0.07 + c * 0.6 + b * 2.1) + h * 0.03 * math.sin(x * 0.17 - c * 0.9 + b)
                thick = 3 + 2 * math.sin(x * 0.09 + c + b)
                col = hsv(round((0.46 + 0.22 * math.sin(x * 0.03 + c * 0.25 + b * 1.3)) * 24) / 24, 0.65, 1.0)
                for dy in range(-int(thick), int(thick) + 1):
                    py = h - 1 - (int(mid) + dy)
                    if 0 <= py < h:
                        a = round(0.32 * (1 - abs(dy) / (thick + 1)) ** 1.3 * 25) / 25
                        if a > 0.02:
                            cells.append((py, x, mix(sky[py][x], col, a)))
        return cells

    def draw_aurora(self, canvas: PixelCanvas, t: float) -> None:
        """Animated aurora ribbons (only on themes that ask for it, i.e. the Citadel)."""
        if not self.theme.aurora:
            return
        key = int(t * 10)
        if key != self._aur_key:
            self._aur_key, self._aur_cells = key, self._build_aurora(key / 10)
        rows, sky = canvas.rows, self.sky
        for py, x, col in self._aur_cells:
            if rows[py][x] == sky[py][x]:
                rows[py][x] = col

    # -- extra sky layers: embers (Forge), thunderstorm (Spire), golden motes (Citadel) -----------------------------
    def draw_fx(self, canvas: PixelCanvas, t: float, flashes: bool = True) -> None:
        fx = self.theme.fx
        if fx in _MOTE_STYLES:
            self._draw_motes(canvas, t, _MOTE_STYLES[fx])
        if fx in ("storm", "tempest"):
            self._draw_storm(canvas, t, flashes)
        elif fx == "inferno":
            self._draw_lavaglow(canvas, t)
        elif fx == "rift":
            self._draw_rift(canvas, t)

    def _draw_motes(self, canvas: PixelCanvas, t: float, style: tuple) -> None:
        _, speed, pal, alpha = style
        h, w, sky, rows = self.h, self.w, self.sky, canvas.rows
        top = h * 0.92
        for x0, y0, sp, ph, amp, ci in self.motes:
            xi = int((x0 + math.sin(t * 0.8 * sp + ph) * amp) % w)
            py = h - 1 - int((y0 + t * speed * sp) % top)
            if 0 <= py < h and rows[py][xi] == sky[py][xi]:
                lvl = (0.5, 0.75, 1.0)[int((math.sin(t * 5 * sp + ph) + 1) * 1.49)]     # 3 brightness steps only
                rows[py][xi] = mix(sky[py][xi], pal[ci], alpha * lvl)

    def _sky_blend(self, canvas: PixelCanvas, x: float, y: float, col: RGB, a: float) -> None:
        xi, py = int(x), self.h - 1 - int(y)
        if 0 <= xi < self.w and 0 <= py < self.h and canvas.rows[py][xi] == self.sky[py][xi]:
            canvas.rows[py][xi] = mix(self.sky[py][xi], col, a)

    def _draw_lavaglow(self, canvas: PixelCanvas, t: float) -> None:
        """Ember Sanctum: the horizon pulses with the heat of the lava below."""
        h, w, sky, rows = self.h, self.w, self.sky, canvas.rows
        pulse = 0.5 + 0.5 * math.sin(t * 1.7)
        y0 = int(h * 0.62)
        for py in range(y0, h):
            a = round(0.24 * pulse * ((py - y0) / max(1, h - y0)) ** 1.5 * 20) / 20
            if a < 0.03:
                continue
            row, srow = rows[py], sky[py]
            for x in range(w):
                if row[x] == srow[x]:
                    row[x] = mix(srow[x], (255, 110, 30), a)

    def _draw_rift(self, canvas: PixelCanvas, t: float) -> None:
        """Astral Rift: a colossal ring of light and dark turning in the sky, with spiralling arms and an eclipsed core."""
        w, h = self.w, self.h
        cx, cy, R = w * 0.5, h * 0.62, h * 0.36
        sb = lambda x, y, c, a: self._sky_blend(canvas, x, y, c, a)
        for rad, spd in ((R, 0.35), (R * 0.78, -0.55), (R * 1.2, 0.2)):
            n = int(math.tau * rad * 1.6)
            for i in range(n):
                ang = i / n * math.tau + t * spd
                col = (255, 240, 190) if math.sin(ang) > 0 else (150, 90, 255)
                sb(cx + math.cos(ang) * rad, cy + math.sin(ang) * rad, col, 0.55 + 0.35 * math.sin(ang * 6 + t * 3))
        for k in range(12):
            ang = k * math.tau / 12 + t * 0.15
            for j in range(int(R * 0.3), int(R * 1.6)):
                sb(cx + math.cos(ang) * j, cy + math.sin(ang) * j, (255, 236, 190) if k % 2 else (140, 90, 230), 0.28 * (1 - j / (R * 1.7)))
        for arm in range(3):
            for k in range(44):
                rr = k / 44 * R * 0.95
                ang = arm * math.tau / 3 + k * 0.17 + t * 1.1
                sb(cx + math.cos(ang) * rr, cy + math.sin(ang) * rr, (255, 250, 230), 0.8 * (1 - k / 50))
        r0 = R * 0.3
        for yy in range(int(cy - r0), int(cy + r0) + 1):
            for xx in range(int(cx - r0), int(cx + r0) + 1):
                d = math.hypot(xx - cx, yy - cy)
                if d <= r0:
                    sb(xx, yy, (6, 2, 16), 0.8)
                elif d <= r0 + 1.2:
                    sb(xx, yy, (255, 214, 120), 0.6 + 0.3 * math.sin(t * 5))

    def _make_bolt(self, k: int) -> list:
        """Deterministic jagged bolt for strike number k: a list of (x0, y0, x1, y1) segments plus its ground x."""
        r = random.Random(k * 7919 + self.w)
        x, y = r.uniform(0.12, 0.88) * self.w, float(self.h - 1)
        segs, ground = [], self._terrain
        stop = lambda px: (ground.heights[int(clamp(px, 0, self.w - 1))] if ground else 0) + 1

        def run(px, py, limit, branch):
            while py > limit:
                nx, ny = px + r.uniform(-4.5, 4.5), py - r.uniform(2.0, 5.0)
                segs.append((px, py, nx, ny))
                if not branch and r.random() < 0.16:
                    run(nx, ny, max(limit, ny - r.uniform(8, 18)), True)
                px, py = nx, ny
        run(x, y, stop(x), False)
        return segs

    def _draw_storm(self, canvas: PixelCanvas, t: float, flashes: bool) -> None:
        period = 3.4
        k = int(t / period)
        off = 0.5 + (((k * 2654435761) >> 8) & 255) / 255 * 1.6
        local = t - k * period - off
        if not 0 <= local < 0.42:
            return
        if k != self._bolt_key:
            self._bolt_key, self._bolt = k, self._make_bolt(k)
        flick = 1.0 if (local < 0.1 or int(local * 25) % 2 == 0) else 0.5
        inten = round((1 - local / 0.42) * flick * 4) / 4
        if inten <= 0 or not self._bolt:
            return
        h, w, sky, rows = self.h, self.w, self.sky, canvas.rows
        bx = self._bolt[0][0]
        if flashes:                                       # soft local glow around the strike, never the whole sky
            for py in range(h):
                row, srow = rows[py], sky[py]
                for x in range(max(0, int(bx) - 22), min(w, int(bx) + 23)):
                    if row[x] == srow[x]:
                        a = round(0.2 * inten * (1 - abs(x - bx) / 23) * 20) / 20
                        if a > 0.02:
                            row[x] = mix(srow[x], (170, 200, 255), a)
        for x0, y0, x1, y1 in self._bolt:
            n = int(max(abs(x1 - x0), abs(y1 - y0))) + 1
            for i in range(n + 1):
                u = i / n
                px, py = x0 + (x1 - x0) * u, y0 + (y1 - y0) * u
                canvas.blendf(px, py, (235, 245, 255), min(1.0, inten * 1.3))
                if inten > 0.5:
                    canvas.blendf(px + 1, py, (130, 170, 255), 0.35 * inten)
                    canvas.blendf(px - 1, py, (130, 170, 255), 0.35 * inten)


# (density per pixel, rise speed, three colours, strength) for the drifting-particle sky layers
_MOTE_STYLES = {
    "embers": (0.0085, 9.0, ((255, 236, 150), (255, 150, 50), (230, 70, 24)), 0.85),
    "motes": (0.0035, 4.0, ((255, 240, 190), (255, 206, 100), (255, 170, 80)), 0.7),
    "snow": (0.007, -6.0, ((255, 255, 255), (220, 238, 255), (190, 220, 250)), 0.85),
    "fireflies": (0.003, 1.2, ((220, 255, 140), (170, 255, 110), (255, 240, 150)), 0.8),
    "ash": (0.006, -3.5, ((170, 168, 170), (130, 126, 130), (200, 196, 200)), 0.6),
    "stardust": (0.005, 2.5, ((255, 255, 255), (190, 170, 255), (140, 220, 255)), 0.8),
    "inferno": (0.011, 14.0, ((255, 236, 150), (255, 140, 40), (220, 50, 20)), 0.9),
    "tempest": (0.010, -42.0, ((190, 220, 255), (130, 180, 240), (220, 240, 255)), 0.55),
    "rift": (0.007, 6.0, ((255, 244, 205), (160, 110, 255), (255, 255, 255)), 0.85),
}


# ============================================================================
# Tanks
# ============================================================================
# ============================================================================
# Tank designs & ammunition
# Every design shares one 11x6 hitbox and every ammo type shares the same
# ballistics and blast radius: kits differ only in looks and animation.
# ============================================================================
@dataclass(frozen=True)
class TankDesign:
    key: str
    name: str
    blurb: str
    mask: tuple
    barrel: str = "single"      # a key of BARRELS
    glow: str = "none"          # animation of 'l' pixels: a key of GLOW_STYLES
    ambient: str = "none"       # particle flavour(s): keys of AMBIENT_KINDS joined with '+', e.g. "fire+smoke"
    accent: RGB = (236, 242, 250)
    paint: Optional[RGB] = None  # intrinsic body colour: when set the design looks the same whatever hull colour is picked
    parts: tuple = ()           # (turret, hull, track) component ids when the mask was composed from components
    hover: float = 0.0          # >0: the tank floats this many pixels above the ground (shadow, bob and thruster are drawn)


_TURRETS = {
    "dome": ("...ttttt...", "..ttttttt.."), "box": ("..ttttttt..", ".ttttttttt."), "twin": ("..tt.t.tt..", ".ttttttttt."),
    "spike": ("a...ttt...a", ".a.ttttt.a."), "wide": (".ttttttttt.", "ttttttttttt"), "crown": ("a.a.ttt.a.a", ".attttttta."),
    "needle": ("....ttt....", "...ttttt..."), "visor": ("..ttttttt..", ".tlllllllt."), "coil": ("..a.ttt.a..", "..attltta.."),
}
_HULLS = {
    "plain": (".hhhhhhhhg.", "hhhhhhhhhhh"), "plate": ("hhhhhhhhhhg", "hdhhhdhhhdh"), "core": (".hhhhhhhhg.", "hhhllllhhhh"),
    "stripe": (".hdhdhdhdhg", "hdhdhdhdhdh"), "wedge": ("..hhhhhhhgg", ".hhhhhhhhh."), "armor": ("hhhhhhhhhhg", "dhhhlhhhdhh"),
    "jewel": (".hhhhhhhhg.", "hallhhhllah"), "vent": ("hhhhhhhhhhg", "hlhlhlhlhlh"), "slab": ("hhhhhhhhhhg", "hhhhdhdhhhh"),
    "royal": (".hhhhhhhhg.", "hallllllhah"),
}
_TRACKS = {
    "std": ("kwkwkwkwkwk", ".kkkkkkkkk."), "full": ("kwkwkwkwkwk", "kkkkkkkkkkk"), "round": (".kwkwkwkwk.", "..kkkkkkk.."),
    "spike": ("kdkwkwkwkdk", "kkkkkkkkkkk"), "heavy": ("kkwkkwkkwkk", "kkkkkkkkkkk"),
}


_TURRETS.update({
    "antenna": ("....t.t....", "..ttttttt.."), "mushroom": (".tttttttt..", "...ttttt..."), "stair": ("......ttt..", "..ttttttt.."),
    "split": ("..tt...tt..", "..tt.t.tt.."), "pillbox": ("..ttttttt..", "..ttttttt.."), "ridge": ("...t.t.t...", ".ttttttttt."),
    "helm": ("...ttttt...", ".tttlllttt."), "radar": ("..t.ttt.t..", "...ttttt..."), "fin": ("a.........a", ".attttttta."),
    "orb": ("...ttttt...", "..tlllllt.."), "horns": ("t.........t", ".tt.ttt.tt."), "dish": (".t.......t.", "..ttttttt.."),
    "sail": ("......t....", "..tttttt..."), "rear": ("..ttt......", "..tttttttt."), "bubble": ("....ttt....", "...ttttt..."),
    "tower": ("...ttttt...", "...ttttt..."),
    # premium vocabulary: crystal (p/q), furnace flame (f/e), void (v), storm (b), clockwork gold (y)
    "crystal": ("....ppp....", "..pqpppqp.."), "shard": ("....q......", "..pppqppp.."), "furnace": ("a..f.f.f..a", "..tteeett.."),
    "inferno": ("f.f.fff.f.f", ".ttteeettt."), "singularity": ("...vvvvv...", "..vvhvhvv.."), "tesla": ("b....b....b", ".tbttttttbt"),
    "clock": ("...yyyyy...", "..yttyttyy."),
})
_HULLS.update({
    "ramp": ("...hhhhhhgg", ".hhhhhhhhhh"), "wide": ("hhhhhhhhhhg", "hhhhhhhhhhh"), "hex": ("..hhhhhhhg.", ".hhhhhhhhhh"),
    "ribbed": (".hhhhhhhhg.", "hdhdhdhdhdh"), "boat": ("hhhhhhhhhgg", ".hhhhhhhhh."), "bunker": ("hhhhhhhhhhg", "hddhhhhhddh"),
    "lattice": (".hdhhdhhdhg", "hhdhhdhhdhh"), "keel": (".hhhhhhhhg.", "..hhhhhhh.."), "domehull": ("..hhhhhhhg.", "hhhhhhhhhhh"),
    "saddle": ("hh.hhhhh.hg", "hhhhhhhhhhh"), "spined": ("ahahahahahg", "hhhhhhhhhhh"), "glowband": ("hhhhhhhhhhg", "hlllllllllh"),
    "cradle": (".h.hhhhh.hg", "hhhhhhhhhhh"),
    "crystal_h": ("..ppqpppp.g", ".ppppqpppp."), "forge_h": ("hheehheehhg", "heeeheeehhh"), "event_h": (".vvvhhhvvg.", "vvhvvvvhvvv"),
    "storm_h": ("hhhbhhhbhhg", "hbhhhbhhhbh"), "chrono_h": ("hhyhhhhyhhg", "hyhhhhhhyhh"),
})
_TRACKS.update({
    "tread": ("kkkkkkkkkkk", "kwkwkwkwkwk"), "stilts": (".k.......k.", "kk.......kk"), "skids": ("kk.......kk", ".kkkkkkkkk."),
    "wheels": (".kk.kk.kk..", ".kk.kk.kk.."), "spiked": ("kdkdkdkdkdk", "kkkkkkkkkkk"), "pods": ("..kk...kk..", ".kkkk.kkkk."),
    "lowtread": (".kkkkkkkkk.", "kwkwkwkwkwk"), "hoverpad": ("..k.....k..", ".kkk...kkk."),
    "crystal_t": ("..pq...qp..", ".pppqpqppp."), "ember_t": ("kekekekekek", "kkkkkkkkkkk"), "void_t": ("..vv...vv..", ".vvvv.vvvv."),
    "arc_t": ("kbkwkwkbkwk", "kkkkkkkkkkk"), "gold_t": ("kykwkwkwkyk", "kkkkkkkkkkk"),
})
assert all(len(r) == 11 for part in (_TURRETS, _HULLS, _TRACKS) for rows in part.values() for r in rows) \
    and all(len(rows) == 2 for part in (_TURRETS, _HULLS, _TRACKS) for rows in part.values())
# Premium components are only offered by the workshop once a tank that uses them is owned (see workshop_components()).
PREMIUM_PARTS = frozenset({"crystal", "shard", "furnace", "inferno", "singularity", "tesla", "clock", "crystal_h", "forge_h", "event_h",
                           "storm_h", "chrono_h", "crystal_t", "ember_t", "void_t", "arc_t", "gold_t"})


def _tank(key: str, name: str, blurb: str, parts: str, barrel: str, glow: str, ambient: str, accent: RGB,
          paint: Optional[RGB] = None, hover: float = 0.0) -> TankDesign:
    """parts = 'turret/hull/track' component ids from the tables above."""
    t, h, k = parts.split("/")
    return TankDesign(key, name, blurb, _TURRETS[t] + _HULLS[h] + _TRACKS[k], barrel=barrel, glow=glow, ambient=ambient,
                      accent=accent, parts=(t, h, k), paint=paint, hover=hover)


def _tanks(rows: list) -> tuple:
    return tuple(_tank(*r[:8], **(r[8] if len(r) > 8 else {})) for r in rows)


DESIGNS: tuple[TankDesign, ...] = (
    TankDesign("ranger", "RANGER", "Balanced dome-turret all-rounder.",
               ("...ttttt...", "..ttttttt..", ".hhhhhhhhg.", "hhhhhhhhhhh", "kwkwkwkwkwk", ".kkkkkkkkk.")),
    TankDesign("bulwark", "BULWARK", "Boxy plated hull with a heavy gun.",
               ("..ttttttt..", ".tttdtdttt.", "hhhhhhhhhhg", "hdhhhdhhhdh", "kwkwkwkwkwk", "kkkkkkkkkkk"),
               barrel="heavy", accent=(255, 210, 90)),
    TankDesign("viper", "VIPER", "Low wedge hull with blinking sensors.",
               ("....ttt....", "..ttltltt..", "..hhhhhhhgg", ".hhhhhhhhh.", "kwkwkwkwkw.", ".kkkkkkkkk."),
               barrel="rail", glow="blink", accent=(140, 255, 230)),
    TankDesign("gemini", "GEMINI", "Twin cupolas and a split barrel.",
               ("..tt.t.tt..", ".ttttttttt.", ".hhhhhhhhg.", "hhhhhhhhhhh", "kwkwkwkwkwk", ".kkkkkkkkk."),
               barrel="double"),
    TankDesign("crawler", "CRAWLER", "Round-bellied hull on small wheels.",
               ("....ttt....", "..ttttttt..", ".ttttttttt.", "hhhhhhhhhhg", ".kwkwkwkwk.", "..kkkkkkk..")),
    TankDesign("hornet", "HORNET", "Warning stripes and a spark-spitting hull.",
               ("...ttttt...", "..tttdttt..", ".hdhdhdhdhg", "hdhdhdhdhdh", "kwkwkwkwkwk", ".kkkkkkkkk."),
               ambient="sparks"),
    TankDesign("titan", "TITAN", "Armoured giant with a pulsing core.",
               (".ttttttttt.", "tttdtltdttt", "hhhhhhhhhhg", "hdddhhhdddh", "kkwkkwkkwkk", "kkkkkkkkkkk"),
               barrel="heavy", glow="pulse", accent=(120, 200, 255)),
    TankDesign("phantom", "PHANTOM", "Angular stealth hull trailing violet mist.",
               ("....ttt....", "...ttttt...", ".dhhhhhhhg.", "dhhllllhhhh", "kdkdkdkdkdk", ".kkkkkkkkk."),
               barrel="rail", glow="pulse", ambient="mist", accent=(200, 140, 255)),
    TankDesign("juggernaut", "JUGGERNAUT", "Siege hull with a roaring exhaust.",
               ("..ttttttt..", ".ttttatttt.", "hhhhhhhhhhg", "lhhhhhhhhhh", "kwkwkwkwkwk", "kkkkkkkkkkk"),
               barrel="heavy", glow="flame", ambient="embers", accent=(255, 160, 60)),
    # --- Tournament levels 5-9: each one a step up in presence ------------------------------------------------
    TankDesign("sentinel", "SENTINEL", "Shield-plated guardian with a scanning visor.",
               ("..ttttttt..", ".tddtltddt.", "hhhhhhhhhhg", "hllllllllhh", "kwkwkwkwkwk", "kkkkkkkkkkk"),
               barrel="heavy", glow="scan", accent=(120, 255, 190)),
    TankDesign("glacier", "GLACIER", "Spiked ice armour shedding frost.",
               ("....ttt....", "..tttattt..", ".hhhhhhhhg.", "ahalhhhlaha", "kwkwkwkwkwk", ".kkkkkkkkk."),
               barrel="rail", glow="pulse", ambient="frost", accent=(170, 225, 255)),
    TankDesign("helios", "HELIOS", "A crowned solar hull burning with plasma.",
               ("a.a.ttt.a.a", ".attlltta..", ".hhhhhhhhg.", "hhllllllhhh", "kwkwkwkwkwk", "kkkkkkkkkkk"),
               barrel="double", glow="plasma", ambient="flare", accent=(255, 214, 90)),
    # --- Tournament levels 8-17: ten more steps up in presence ---------------------------------------------------
    _tank("rattler", "RATTLER", "Dusty desert wedge that spits sparks.", "wide/stripe/full", "heavy", "none", "sparks", (240, 200, 90)),
    _tank("miasma", "MIASMA", "Toxic hull trailing green mist.", "dome/vent/round", "rail", "blink", "mist", (140, 255, 100)),
    _tank("bastion", "BASTION", "A walking fortress of slab armour.", "box/armor/spike", "heavy", "pulse", "none", (210, 190, 150)),
    _tank("zephyr", "ZEPHYR", "Needle-nosed sprinter that rides the wind.", "needle/wedge/std", "rail", "scan", "frost", (170, 230, 255)),
    _tank("geode", "GEODE", "Crystal-spiked shell with a glowing heart.", "spike/core/round", "double", "pulse", "flare", (200, 140, 255)),
    _tank("mirage", "MIRAGE", "A shimmering hull that never sits still.", "twin/jewel/std", "double", "plasma", "mist", (255, 170, 210)),
    _tank("revenant", "REVENANT", "Ghost-iron war machine with a lava heart.", "visor/vent/spike", "heavy", "lava", "embers", (255, 120, 60)),
    _tank("corsair", "CORSAIR", "Raider with a crackling storm cannon.", "coil/plate/full", "double", "storm", "static", (120, 200, 255)),
    _tank("tyrant", "TYRANT", "Crowned siege lord, furnace-lit.", "crown/slab/heavy", "heavy", "flame", "embers", (255, 150, 50)),
    _tank("archon", "ARCHON", "Gleaming champion with a triple plasma gun.", "crown/core/spike", "triple", "plasma", "flare", (255, 230, 160)),
    # --- Tournament levels 18-20: the Master's road --------------------------------------------------------------
    TankDesign("obsidian", "OBSIDIAN", "Black glass hull split by molten cracks.",
               ("a...ttt...a", ".a.ttdtt.a.", "hdhhhhhhhhg", "dlhdlhhldhd", "kdkwkdkwkdk", "kkkkkkkkkkk"),
               barrel="heavy", glow="lava", ambient="embers", accent=(255, 96, 36)),
    TankDesign("tempest", "TEMPEST", "Storm-crowned coil tank with a triple barrel.",
               ("a....t....a", "a.tttltttt.", "hhhhhhhhhhg", "hlhlhlhlhlh", "kdkwkwkwkdk", "kkkkkkkkkkk"),
               barrel="triple", glow="storm", ambient="static", accent=(150, 222, 255)),
    # --- Tournament level 20: the Master's tank (prism barrel, crown, sparkle) --------------------------------
    TankDesign("sovereign", "SOVEREIGN", "The Master's crowned war machine.",
               ("..a.ttt.a..", "..attltta..", ".hhhhhhhhg.", "hallhhhllah", "kwkwkwkwkwk", ".kkkkkkkkk."),
               barrel="prism", glow="prism", ambient="sparkle", accent=(255, 214, 110)),
)
# Secret kits: never part of the Tournament, hidden until a cheat code unlocks them.
SECRET_DESIGNS: tuple[TankDesign, ...] = (
    TankDesign("asciibot", "ASCIIBOT", "A retro monitor on treads, running on pure code.",
               (".ttttttttt.", ".tlltlttlt.", ".hhhhhhhhg.", "hdhdhdhdhdh", "kwkwkwkwkwk", ".kkkkkkkkk."),
               barrel="single", glow="matrix", ambient="bits", accent=(80, 255, 120)),
)
# Shop kits: bought with coins (and credits at the top tier). 5 cheap, 5 mid, 5 pro, 3 sovereign-level.
_SHOP_BASE_DESIGNS: tuple[TankDesign, ...] = (
    _tank("scout", "SCOUT", "Light wedge built for quick shots.", "needle/wedge/tread", "single", "none", "none", (200, 225, 240)),
    _tank("mule", "MULE", "Stubby, stubborn workhorse.", "box/plain/full", "single", "none", "none", (230, 200, 140)),
    _tank("badger", "BADGER", "Low-slung and hard to shift.", "dome/stripe/round", "single", "none", "none", (210, 170, 120)),
    _tank("turtle", "TURTLE", "Domed shell on tiny wheels.", "dome/plain/round", "single", "none", "none", (150, 220, 150)),
    _tank("pioneer", "PIONEER", "Open-frame frontier tank.", "twin/slab/std", "single", "none", "none", (220, 190, 160)),
    _tank("raider", "RAIDER", "Striped raider with a spark exhaust.", "wide/stripe/std", "double", "none", "sparks", (255, 180, 80)),
    _tank("centurion", "CENTURION", "Plated legionnaire, blinking visor.", "box/plate/spike", "heavy", "blink", "none", (230, 210, 120)),
    _tank("warhound", "WARHOUND", "Fast hunter with a rail gun.", "needle/vent/full", "rail", "blink", "none", (120, 255, 200)),
    _tank("mantis", "MANTIS", "Green praying-mantis hull.", "spike/wedge/round", "rail", "pulse", "mist", (150, 255, 120)),
    _tank("paladin", "PALADIN", "Gilded defender with a pulsing core.", "dome/armor/std", "heavy", "pulse", "none", (255, 230, 140)),
    _tank("reaper", "REAPER", "Dark scythe-armed hunter.", "spike/armor/spike", "rail", "lava", "embers", (255, 80, 60)),
    _tank("leviathan", "LEVIATHAN", "Deep-sea hull glowing from within.", "wide/core/heavy", "heavy", "pulse", "mist", (80, 200, 255)),
    _tank("banshee", "BANSHEE", "Shrieking storm-wing tank.", "coil/wedge/std", "triple", "storm", "static", (180, 220, 255)),
    _tank("behemoth", "BEHEMOTH", "Colossal slab with a blazing core.", "box/slab/heavy", "heavy", "flame", "embers", (255, 140, 40)),
    _tank("nightshade", "NIGHTSHADE", "Violet stealth hull, glowing visor.", "visor/jewel/round", "rail", "plasma", "mist", (200, 120, 255)),
    _tank("emperor", "EMPEROR", "Crowned war engine with a prism gun.", "crown/royal/heavy", "prism", "prism", "sparkle", (255, 214, 110)),
    _tank("celestial", "CELESTIAL", "Star-forged hull wreathed in solar fire.", "coil/jewel/spike", "triple", "plasma", "flare", (255, 220, 120)),
    _tank("eternal", "ETERNAL", "Void-crowned titan lit by lightning.", "crown/armor/heavy", "prism", "storm", "static", (190, 160, 255)),
    _tank("dynasty", "DYNASTY", "A gilded throne on treads, sparkling.", "coil/royal/heavy", "triple", "prism", "sparkle", (255, 200, 120)),
    _tank("overlord", "OVERLORD", "Jewelled horned tyrant, plasma-lit.", "spike/jewel/heavy", "prism", "plasma", "flare", (255, 150, 220)),
)
# Mission-mode rewards (6 tanks, 6 ammo, 6 maps and 6 home screens; the last of each is the alien Grand Prize set).
MISSION_DESIGNS: tuple[TankDesign, ...] = (
    _tank("wardog", "WARDOG", "Battle-scarred hound with a heavy cannon.", "box/plate/heavy", "heavy", "none", "sparks", (180, 200, 150)),
    _tank("spitfire", "SPITFIRE", "Quick-firing wedge with a scanning visor.", "visor/wedge/std", "double", "scan", "none", (150, 255, 190)),
    _tank("blitz", "BLITZ", "Storm-lit raider with a triple cannon.", "coil/vent/full", "triple", "blink", "static", (255, 220, 120)),
    _tank("judge", "JUDGE", "A crowned slab of unforgiving armour.", "crown/armor/spike", "rail", "pulse", "flare", (230, 200, 255)),
    _tank("hydra", "HYDRA", "Many-headed plasma tank trailing mist.", "twin/jewel/heavy", "triple", "plasma", "mist", (120, 255, 150)),
    TankDesign("ufo", "U.F.O.", "An alien saucer on hover pods. The Grand Prize.",
               ("....lll....", "...tllllt..", ".hhhhhhhhg.", "hahahahahah", ".kk.kkk.kk.", "..k.....k.."),
               barrel="single", glow="plasma", ambient="mist", accent=(120, 255, 170), hover=3.0),
)
_N = lambda *r: r
_NEW_TANKS = (
    _tanks([  # cheap
        _N("pebble", "PEBBLE", "A smooth little river-stone of a tank.", "dome/ribbed/tread", "single", "none", "none", (190, 200, 205)),
        _N("rusty", "RUSTY", "Patched bunker on low treads.", "pillbox/bunker/lowtread", "howitzer", "none", "none", (200, 140, 90)),
        _N("hopper", "HOPPER", "Spindly antenna tank on wheels.", "antenna/boat/wheels", "single", "none", "none", (170, 220, 170)),
        _N("spud", "SPUD", "Round, wide and unbothered.", "bubble/wide/full", "single", "none", "none", (220, 200, 140)),
        _N("crate", "CRATE", "A box on pods. It works.", "pillbox/slab/pods", "single", "none", "none", (190, 170, 130)),
        _N("skiff", "SKIFF", "Sail-turret skimmer on skids.", "sail/keel/skids", "needle", "none", "none", (150, 200, 235)),
        _N("grunt", "GRUNT", "Rear-set gun on a ramped hull.", "rear/ramp/std", "howitzer", "none", "none", (160, 190, 140)),
        _N("tinker", "TINKER", "Radar-topped hex hull on spiked treads.", "radar/hex/spiked", "single", "none", "none", (210, 190, 120)),
        _N("drifter", "DRIFTER", "Mushroom cap over a saddle hull.", "mushroom/saddle/round", "single", "none", "none", (200, 170, 210)),
        _N("bucket", "BUCKET", "Bulbous cradle with a double barrel.", "bubble/cradle/heavy", "double", "none", "none", (180, 190, 200))]),
    _tanks([  # mid
        _N("stingray", "STINGRAY", "Dish-crowned keel gliding on pods.", "dish/keel/pods", "sniper", "scan", "bubbles", (100, 200, 230)),
        _N("marauder", "MARAUDER", "Horned hex hull bristling with barrels.", "horns/hex/spiked", "quad", "blink", "sparks", (235, 150, 90)),
        _N("harpoon", "HARPOON", "Helmeted lowtread with a very long gun.", "helm/glowband/lowtread", "sniper", "scan", "none", (120, 230, 200)),
        _N("trident", "TRIDENT", "A three-pronged gun on a bunker.", "split/bunker/tread", "triple", "pulse", "none", (110, 210, 230)),
        _N("redoubt", "REDOUBT", "A tower on a ribbed fortress hull.", "tower/ribbed/heavy", "howitzer", "pulse", "none", (210, 200, 170)),
        _N("sandstorm", "SANDSTORM", "Gritty ridge-top hull shedding dust.", "ridge/ramp/spiked", "gatling", "none", "embers", (230, 190, 110)),
        _N("cobra", "COBRA", "A hooded orb striking from a keel.", "orb/keel/hoverpad", "coil", "toxic", "mist", (140, 240, 110)),
        _N("barracuda", "BARRACUDA", "Finned predator with a needle gun.", "fin/boat/skids", "needle", "ice", "frost", (150, 220, 255)),
        _N("outrider", "OUTRIDER", "Radar scout on long stilts.", "radar/domehull/stilts", "sniper", "neon", "none", (240, 110, 220)),
        _N("brigadier", "BRIGADIER", "Stair-stepped command tank in gold.", "stair/lattice/lowtread", "heavy", "gold", "none", (235, 215, 130))]),
    _tanks([  # pro
        _N("typhoon", "TYPHOON", "A storm-cell turret on a glowing band.", "antenna/glowband/hoverpad", "bolt", "storm", "static", (130, 190, 255)),
        _N("hellcat", "HELLCAT", "Horned, ember-plated predator.", "horns/armor/spiked", "flame", "ember", "embers", (255, 110, 60)),
        _N("warmaster", "WARMASTER", "A quad-barrel siege lord.", "helm/bunker/heavy", "quad", "pulse", "sparks", (220, 190, 120)),
        _N("gravedigger", "GRAVEDIGGER", "Black spined hull, void-lit.", "ridge/spined/stilts", "void", "void", "void", (150, 100, 230)),
        _N("shatterer", "SHATTERER", "Crystal-cold hull crowned with ice.", "fin/hex/pods", "needle", "ice", "frost", (190, 235, 255)),
        _N("stormrider", "STORMRIDER", "A wide-bodied cannon on skids, crackling.", "radar/wide/skids", "gatling", "storm", "lightning", (110, 200, 255)),
        _N("ironclad", "IRONCLAD", "A tower of armour with a gold heart.", "tower/armor/heavy", "howitzer", "gold", "none", (200, 200, 215)),
        _N("bloodhound", "BLOODHOUND", "A fast red hunter trailing embers.", "rear/ramp/hoverpad", "sniper", "ember", "embers", (230, 70, 70)),
        _N("sandshark", "SANDSHARK", "A finned burrower with a toxic glow.", "fin/keel/spiked", "coil", "toxic", "petals", (220, 190, 100)),
        _N("tempestor", "TEMPESTOR", "An orb-topped war engine in neon.", "orb/lattice/heavy", "bolt", "neon", "orbit", (255, 90, 220))]),
    _tanks([  # master tier
        _N("ascendant", "ASCENDANT", "A rising gold spire over a glowing band.", "helm/glowband/hoverpad", "crystal", "gold", "orbit", (255, 225, 130)),
        _N("seraphim", "SERAPHIM", "A winged, haloed hull, bright and ethereal.", "fin/domehull/pods", "bolt", "refract", "sparkle", (255, 250, 200)),
        _N("doombringer", "DOOMBRINGER", "A spiked dread on burning treads.", "horns/armor/lowtread", "flame", "ember", "burn", (255, 80, 50)),
        _N("starforge", "STARFORGE", "A star-crowned forge of living gold.", "orb/royal/heavy", "coil", "gold", "sparkle+flare", (255, 200, 100)),
        _N("arcanist", "ARCANIST", "A mystic hull orbited by motes.", "coil/jewel/hoverpad", "void", "neon", "orbit", (200, 120, 255)),
        _N("voidwalker", "VOIDWALKER", "A black glass stalker leaking dark.", "ridge/lattice/stilts", "void", "void", "void+smoke", (160, 110, 240)),
        _N("sunbreaker", "SUNBREAKER", "A radiant dome of solar plasma.", "crown/glowband/heavy", "flame", "plasma", "fire+flare", (255, 190, 70)),
        _N("templar", "TEMPLAR", "A gilded tower-fortress with a halo.", "tower/royal/heavy", "heavy", "gold", "sparkle+orbit", (255, 235, 150)),
        _N("monolith", "MONOLITH", "A slab of glowing stone and static.", "pillbox/slab/heavy", "howitzer", "storm", "static+orbit", (180, 200, 230)),
        _N("phoenix", "PHOENIX", "A bird of fire on a gilded frame.", "fin/jewel/hoverpad", "flame", "ember", "fire+embers", (255, 160, 50))]),
)
# The premium tier: intrinsic designs built from the premium component set (crystal, furnace, void, storm, clockwork).
_PREMIUM_TANKS = _tanks([
    _N("prismatic_king", "PRISMATIC KING", "Grown from a single crystal: every facet splits the light.", "crystal/crystal_h/crystal_t", "crystal",
       "refract", "prism+sparkle", (255, 255, 255), {"paint": (206, 220, 246)}),
    _N("eviscerator", "EVISCERATOR", "Furnace-hearted and permanently ablaze, smoke pouring off white-hot armour.", "inferno/forge_h/ember_t",
       "flame", "ember", "burn", (255, 150, 40), {"paint": (205, 26, 26)}),
    _N("singularity", "SINGULARITY", "A collapsed star in tank form; light bends around the hull.", "singularity/event_h/void_t", "void", "void",
       "void+orbit", (170, 110, 255), {"paint": (34, 22, 70), "hover": 3.0}),
    _N("stormbringer", "STORMBRINGER", "Tesla spires and arc-wreathed tracks.", "tesla/storm_h/arc_t", "bolt", "storm", "lightning+static",
       (160, 220, 255), {"paint": (52, 72, 128)}),
    _N("chronos", "CHRONOS", "A clockwork of gold that never stops turning.", "clock/chrono_h/gold_t", "coil", "gold", "orbit+sparkle",
       (255, 230, 140), {"paint": (150, 112, 40)}),
])
SHOP_DESIGNS: tuple[TankDesign, ...] = (_SHOP_BASE_DESIGNS[0:5] + _NEW_TANKS[0] + _SHOP_BASE_DESIGNS[5:10] + _NEW_TANKS[1]
                                        + _SHOP_BASE_DESIGNS[10:15] + _NEW_TANKS[2] + _SHOP_BASE_DESIGNS[15:20] + _NEW_TANKS[3] + _PREMIUM_TANKS)
# Terminal Ticket bosses: ten unique premium-class tanks (intrinsic paint), reward-only.
TICKET_DESIGNS: tuple[TankDesign, ...] = _tanks([
    _N("voltsovereign", "VOLT SOVEREIGN", "A throne of arcing coils.", "tesla/glowband/hoverpad", "bolt", "storm", "lightning+static", (150, 210, 255), {"paint": (40, 60, 150)}),
    _N("magistrate", "IRON MAGISTRATE", "A clockwork judge in grey iron and gold.", "clock/ribbed/lowtread", "coil", "gold", "orbit+sparkle", (255, 220, 120), {"paint": (110, 104, 96)}),
    _N("regent", "GLACIER REGENT", "A frozen spire-tank breathing frost.", "shard/hex/pods", "needle", "ice", "frost+orbit", (200, 240, 255), {"paint": (170, 215, 245)}),
    _N("solarch", "SOLAR ARCHON", "A blazing gold tower-tank.", "tower/royal/spiked", "flame", "gold", "fire+flare+sparkle", (255, 220, 100), {"paint": (230, 170, 50)}),
    _N("abyssal", "ABYSS LEVIATHAN", "A deep-sea horror leaking bubbles and dark.", "wide/hex/hoverpad", "howitzer", "void", "bubbles+void", (100, 200, 255), {"paint": (20, 70, 110), "hover": 2.0}),
    _N("pharaoh", "SAND PHARAOH", "A gilded sun-priest on silent skids.", "dish/spined/skids", "sniper", "gold", "sparkle+embers", (255, 230, 150), {"paint": (205, 170, 70)}),
    _N("baron", "TOXIC BARON", "A noxious war-machine venting green smoke.", "stair/ribbed/lowtread", "quad", "toxic", "smoke+mist", (170, 255, 90), {"paint": (80, 130, 40)}),
    _N("shadowlord", "SHADOW EMPEROR", "A horned silhouette swallowing the light.", "horns/lattice/stilts", "void", "void", "void+smoke", (170, 120, 255), {"paint": (30, 24, 46)}),
    _N("overseer", "COSMIC OVERSEER", "A watching orb on a hovering throne.", "orb/jewel/hoverpad", "crystal", "refract", "orbit+prism", (200, 190, 255), {"paint": (60, 60, 140), "hover": 3.0}),
    _N("warlord", "DRAGON WARLORD", "A scaled, fire-breathing conqueror.", "furnace/slab/spiked", "flame", "ember", "fire+smoke", (255, 160, 60), {"paint": (170, 40, 30)}),
])
# --- Ancient Artifact vocabulary: o solar flame, r rune gold, s starlight, n nightglass, m aether, z chrono ------------------
_TURRETS.update({
    "sunhalo": ("..o.ooo.o..", ".ooosssooo."), "eclipsecrown": ("n.n.nnn.n.n", ".nnnsssnnn."), "tempestfan": ("m.m..m..m.m", ".mmmmmmmmm."),
    "emberspire": ("..f.fff.f..", ".efffffffe."), "hourglass": ("..zzzzzzz..", "...zszsz..."),
})
_HULLS.update({
    "sunplate": ("hoosshsso.g", "hsohhhhhosh"), "nightwing": ("nnhhnnnhhng", "nsnnhhhnnsn"), "stormdeck": ("hmhmhmhmhmg", "hmmmhhhmmmh"),
    "magmaslab": ("hefehfehefg", "heeefhfeeeh"), "chronocase": ("hzzhhzhhzzg", "zshzzhzzhsz"),
})
_TRACKS.update({
    "sunrunner": ("ororororor.", ".oooooooo.."), "voidskids": ("n.nn...nn.n", ".nsnn.nnsn."), "galetreads": (".mm.mm.mm..", "mmmmmmmmmmm"),
    "cinder_t": ("kekfkekfkek", ".kkkkkkkkk."), "sand_t": ("..zz...zz..", ".zzszzzszz."),
})
assert all(len(r) == 11 for part in (_TURRETS, _HULLS, _TRACKS) for rows in part.values() for r in rows) \
    and all(len(rows) == 2 for part in (_TURRETS, _HULLS, _TRACKS) for rows in part.values())
_TURRETS["martyrhood"] = ("...nnnnn...", "..nnrsrnn..")
_HULLS["martyrrobe"] = ("nnrnnnnnrng", "nrnnsnnsnrn")
_TRACKS["martyrstaff"] = ("r.rr.rr.rr.", ".rssrrrrss.")
assert all(len(r) == 11 for p in ("martyrhood", "martyrrobe", "martyrstaff") for part in (_TURRETS, _HULLS, _TRACKS) if p in part for r in part[p])
PREMIUM_PARTS = PREMIUM_PARTS | {"martyrhood", "martyrrobe", "martyrstaff", "sunhalo", "eclipsecrown", "tempestfan", "emberspire", "hourglass", "sunplate", "nightwing", "stormdeck",
                                 "magmaslab", "chronocase", "sunrunner", "voidskids", "galetreads", "cinder_t", "sand_t"}
ANCIENT_DESIGNS: tuple[TankDesign, ...] = (
    _tank("a_solaris", "ANCIENT SOLARIS", "Sunforged war-relic crowned in living light.", "sunhalo/sunplate/sunrunner", "sunlance", "plasma",
          "halo+flare+sparkle", (255, 226, 130), paint=(226, 176, 60), hover=3.0),
    _tank("a_umbral", "ANCIENT UMBRAL", "A shard of the first night, wearing a crown of stars.", "eclipsecrown/nightwing/voidskids", "umbra", "aeon",
          "riftmotes+void", (196, 146, 255), paint=(16, 10, 34), hover=3.0),
    _tank("a_tempest", "ANCIENT TEMPEST", "Storm and tide bound into one hovering fortress.", "tempestfan/stormdeck/galetreads", "gale", "storm",
          "glyphs+static+bubbles", (120, 232, 255), paint=(30, 92, 134), hover=3.0),
    _tank("a_emberheart", "ANCIENT EMBERHEART", "Mountain and fire: a furnace that never went out.", "emberspire/magmaslab/cinder_t", "magmacannon", "lava",
          "fire+embers+glyphs", (255, 146, 56), paint=(92, 32, 22), hover=2.0),
    _tank("a_chronos", "ANCIENT CHRONOS", "The hourglass itself. Time bends around its hull.", "hourglass/chronocase/sand_t", "ancient", "aeon",
          "halo+orbit+glyphs", (255, 236, 176), paint=(150, 108, 48), hover=4.0),
    _tank("a_martyr", "THE MARTYR", "The legend's own war-tank: robes of night and a crown of stars.", "martyrhood/martyrrobe/martyrstaff", "ancient", "aeon",
          "halo+riftmotes+glyphs", (255, 226, 150), paint=(20, 12, 44), hover=4.0),
)
ALL_DESIGNS = DESIGNS + SHOP_DESIGNS + MISSION_DESIGNS + TICKET_DESIGNS + SECRET_DESIGNS + ANCIENT_DESIGNS
assert all(len(d.mask) == TANK_H and all(len(r) == TANK_W for r in d.mask) for d in ALL_DESIGNS)
assert len(DESIGNS) == 25 and DESIGNS[-1].key == "sovereign"          # 5 starters + 20 Tournament unlocks
assert len({d.key for d in ALL_DESIGNS}) == len(ALL_DESIGNS) and len(SHOP_DESIGNS) == 65
DESIGN_BY_KEY = {d.key: d for d in ALL_DESIGNS}
DEFAULT_DESIGN = DESIGNS[0]
BASE_KITS = 5     # the first five designs / ammo types are available from the start


@dataclass(frozen=True)
class AmmoType:
    key: str
    name: str
    blurb: str
    proj: str       # projectile look
    trail: str      # tracer look
    boom: str       # explosion look
    mech: tuple = ()  # gameplay mechanics as (key, value) pairs - see plan_shot(); empty = a plain shell

    @property
    def mechanics(self) -> dict:
        return dict(self.mech)


AMMOS: tuple[AmmoType, ...] = (          # first five: starters; then one per Tournament level, in level order
    AmmoType("standard", "STANDARD", "Reliable high-explosive shell.", "shell", "plain", "classic"),
    AmmoType("tracer", "TRACER", "Dashed tracer streak, ray flash.", "streak", "dash", "rays"),
    AmmoType("plasma", "PLASMA", "Pulsing energy orb, ringed blast.", "orb", "glow", "ring"),
    AmmoType("flak", "FLAK", "Crackling sparks, clustered bursts.", "spark", "sparks", "cluster"),
    AmmoType("comet", "COMET", "Blazing head, long ember tail.", "comet", "embers", "embers"),
    AmmoType("venom", "VENOM", "Dripping toxin, lingering gas cloud.", "drip", "drip", "cloud"),
    AmmoType("starburst", "STARBURST", "Twinkling star, radiant ray burst.", "star", "twinkle", "starrays"),
    AmmoType("void", "VOID", "Dark core that implodes, then blasts.", "void", "mist", "implode"),
    AmmoType("inferno", "INFERNO", "Living flame, rising fire plume.", "flame", "fire", "fire"),
    AmmoType("pulse", "PULSE", "Rippling ring-wave, triple shockwave.", "ring", "ripple", "shock"),
    AmmoType("cryo", "CRYO", "Ice shard trailing frost, shattering blast.", "shard", "frost", "shatter"),
    AmmoType("nova", "NOVA", "A blinding solar orb and a white-hot flare.", "sun", "solar", "nova"),
    AmmoType("shrapnel", "SHRAPNEL", "Splintering shards, clustered blast.", "streak", "sparks", "cluster"),
    AmmoType("blight", "BLIGHT", "Creeping rot and a choking cloud.", "drip", "mist", "cloud"),
    AmmoType("siege", "SIEGE", "Heavy slug, ember wake, shockwave.", "shell", "embers", "shock"),
    AmmoType("cyclone", "CYCLONE", "Spinning ring on a rippling wake.", "ring", "ripple", "rays"),
    AmmoType("crystal", "CRYSTAL", "Faceted shard, twinkling dust.", "shard", "twinkle", "starrays"),
    AmmoType("echo", "ECHO", "Ringing orb, dashed afterimage.", "orb", "dash", "ring"),
    AmmoType("wraith", "WRAITH", "Ghost core, frost wake, shattering burst.", "void", "frost", "shatter"),
    AmmoType("barrage", "BARRAGE", "Blazing comet, sparking wake, solar burst.", "comet", "sparks", "nova"),
    AmmoType("wrath", "WRATH", "Roaring flame, lava trail, molten crater.", "flame", "lava", "magma"),
    AmmoType("zenith", "ZENITH", "Radiant star, solar wake, blinding nova.", "star", "solar", "nova"),
    AmmoType("magma", "MAGMA", "Molten blob, dripping lava, erupting crater.", "lava", "lava", "magma"),
    AmmoType("thunder", "THUNDER", "Crackling storm orb, forking lightning blast.", "bolt", "arc", "thunder"),
    AmmoType("aurora", "AURORA", "Prismatic shell, shimmering blast.", "prism", "prism", "prism"),
)
SECRET_AMMOS: tuple[AmmoType, ...] = (
    AmmoType("glyph", "GLYPH", "Cycling ASCII character, raining code.", "glyph", "code", "code"),
    AmmoType("blessing", "MARTYR'S BLESSING", "An impossible blessing inherited from the Martyr. Land a direct hit and time fractures, stars multiply, and the battlefield receives its final judgment.",
             "blessing", "blessing", "blessing"),        # no `mech`: its power is the ONE-SHOT KILL handled in BattleScene (see BlessingFX)
)
BLESSING_KEY = "blessing"
_SHOP_BASE_AMMOS: tuple[AmmoType, ...] = (
    AmmoType("blazer", "BLAZER", "A plain comet with a clean blast.", "comet", "plain", "classic"),
    AmmoType("dasher", "DASHER", "Dashed streak, clustered bursts.", "streak", "dash", "cluster"),
    AmmoType("fizz", "FIZZ", "Fizzing spark, twinkling wake.", "spark", "twinkle", "rays"),
    AmmoType("slag", "SLAG", "Dripping slag, ember burst.", "drip", "embers", "embers"),
    AmmoType("needle", "NEEDLE", "A thin streak that bursts into stars.", "streak", "plain", "starrays"),
    AmmoType("ripple", "RIPPLE", "Glowing ring and a ringed blast.", "ring", "glow", "ring"),
    AmmoType("cinder", "CINDER", "Living flame that sheds embers.", "flame", "embers", "embers"),
    AmmoType("frostbite", "FROSTBITE", "Ice shard, frost wake, chilling cloud.", "shard", "frost", "cloud"),
    AmmoType("photon", "PHOTON", "Pulsing orb on a solar wake.", "orb", "solar", "rays"),
    AmmoType("toxin", "TOXIN", "Dripping poison, imploding gas.", "drip", "mist", "implode"),
    AmmoType("stormfront", "STORMFRONT", "Crackling bolt, rippling wake, forked blast.", "bolt", "ripple", "thunder"),
    AmmoType("quasar", "QUASAR", "A tiny sun trailing stardust.", "sun", "twinkle", "starrays"),
    AmmoType("abyss", "ABYSS", "A black hole that detonates in light.", "void", "mist", "nova"),
    AmmoType("molten", "MOLTEN", "Lava blob, fire wake, fire plume.", "lava", "fire", "fire"),
    AmmoType("glacial", "GLACIAL", "Prismatic shard, shattering burst.", "shard", "prism", "shatter"),
    AmmoType("helix", "HELIX", "Twin prismatic orbs spiralling in flight.", "helix", "prism", "prism"),
    AmmoType("eclipse", "ECLIPSE", "A dark core with a blazing corona.", "void", "solar", "nova"),
    AmmoType("bloom", "BLOOM", "A prismatic star that opens like a flower.", "star", "prism", "bloom"),
    AmmoType("nebula", "NEBULA", "Spiralling orbs shedding stardust.", "helix", "twinkle", "starrays"),
    AmmoType("supernova", "SUPERNOVA", "A miniature sun that blooms on impact.", "sun", "prism", "bloom"),
)
MISSION_AMMOS: tuple[AmmoType, ...] = (
    AmmoType("bandit", "BANDIT", "Dashed rounds with clustered bursts.", "comet", "dash", "cluster"),
    AmmoType("salvo", "SALVO", "Heavy shell, sparking wake, shockwave.", "shell", "sparks", "shock"),
    AmmoType("hailstorm", "HAILSTORM", "Crackling spark, rippling wake, forked blast.", "spark", "ripple", "thunder"),
    AmmoType("meltdown", "MELTDOWN", "Molten core, solar wake, erupting crater.", "lava", "solar", "magma"),
    AmmoType("oblivion", "OBLIVION", "A prismatic void that implodes.", "void", "prism", "implode"),
    AmmoType("xenon", "XENON", "A caged star of alien plasma: orbiting ions, a comet of sparks, a crystalline bloom of rings. The Grand Prize.", "xenon", "xenon", "xenon"),
)
def _ammos(rows: list) -> tuple:
    return tuple(AmmoType(k, n, b, p, t, bm, tuple(m)) for k, n, b, p, t, bm, m in rows)


_A = lambda *r: r
_NEW_AMMOS = (
    _ammos([  # cheap: one clear quirk each
        _A("lob", "LOB", "Slow, floaty lob: wide blast, loves a tailwind.", "orb", "glow", "ring", (("g", 0.7), ("wind", 1.4), ("radius", 1.1))),
        _A("slug", "SLUG", "Flat, fast, wind-proof slug with a small blast.", "streak", "dash", "classic", (("g", 1.45), ("wind", 0.4), ("speed", 1.1), ("radius", 0.85))),
        _A("skipper", "SKIPPER", "Skips once off the ground before it blows.", "shell", "plain", "classic", (("bounce", 1), ("br", 0.5), ("radius", 0.9))),
        _A("twister", "TWISTER", "Corkscrews through the air.", "spark", "twinkle", "rays", (("wob", (3.0, 8.0)), ("radius", 0.95))),
        _A("pebbles", "PEBBLES", "A small blast, then three bomblets.", "shell", "sparks", "cluster", (("cluster", 3), ("cspace", 4.5), ("cr", 0.5), ("radius", 0.6))),
        _A("dud", "DUD", "A tiny pop... then the real blast.", "comet", "plain", "embers", (("fuse", 0.8), ("fr", 1.35), ("radius", 0.5))),
        _A("mudball", "MUDBALL", "Builds a mound of earth instead of digging.", "drip", "drip", "cloud", (("terrain", "mound"), ("dmg", 0), ("radius", 1.1))),
        _A("borer", "BORER", "Drills a narrow, deep shaft.", "streak", "sparks", "shatter", (("terrain", "trench"), ("radius", 0.9))),
        _A("scatter", "SCATTER", "Splits in two at the top of its arc.", "star", "twinkle", "starrays", (("apex", ("split", 2, 0.5)), ("subr", 0.85))),
        _A("breeze", "BREEZE", "Ignores the wind completely.", "ring", "ripple", "ring", (("wind", 0.0), ("radius", 0.95)))]),
    _ammos([  # mid
        _A("zigzag", "ZIGZAG", "A wild zigzag that is hard to read.", "spark", "arc", "thunder", (("wob", (6.0, 12.0)), ("radius", 1.0))),
        _A("triskip", "TRI-SKIP", "Three skips, three chances.", "shell", "dash", "shock", (("bounce", 3), ("br", 0.4), ("radius", 0.9))),
        _A("rainmaker", "RAINMAKER", "Rains four shells from the apex.", "drip", "mist", "cloud", (("apex", ("split", 4, 0.7)), ("subr", 0.6))),
        _A("fireworks", "FIREWORKS", "Bursts into six at the top of the arc.", "star", "twinkle", "starrays", (("apex", ("split", 6, 1.0)), ("subr", 0.5))),
        _A("walker", "WALKER", "A line of blasts marching forward.", "lava", "lava", "magma", (("chain", 4), ("cstep", 7.0), ("cr", 0.6), ("radius", 0.7))),
        _A("timecharge", "TIME CHARGE", "Small pop, then a delayed big blast.", "comet", "solar", "nova", (("fuse", 1.2), ("fr", 1.8), ("radius", 0.45))),
        _A("rockslide", "ROCKSLIDE", "Bomblets spread across the ground.", "shell", "embers", "cluster", (("cluster", 5), ("cspace", 6.0), ("cr", 0.6), ("radius", 0.7))),
        _A("fortify", "FORTIFY", "Raises a defensive hill, harms nobody.", "orb", "glow", "ring", (("terrain", "mound"), ("dmg", 0), ("radius", 1.3))),
        _A("tunneler", "TUNNELER", "Digs a deep trench.", "streak", "frost", "shatter", (("terrain", "trench"), ("radius", 1.2))),
        _A("glasser", "GLASSER", "Melts the ground perfectly flat.", "sun", "solar", "nova", (("terrain", "glass"), ("radius", 1.0)))]),
    _ammos([  # pro
        _A("warpshell", "WARP SHELL", "Teleports forward at the top of its arc.", "void", "mist", "implode", (("apex", ("jump", 28.0)),)),
        _A("gravmine", "GRAV MINE", "Pulls nearby tanks toward the blast.", "void", "mist", "implode", (("pull", 7.0), ("radius", 1.1))),
        _A("hailfall", "HAILFALL", "Eight shards fall from above.", "shard", "frost", "shatter", (("apex", ("split", 8, 1.2)), ("subr", 0.5))),
        _A("ricochet", "RICOCHET", "Four ricochets before the finale.", "bolt", "arc", "thunder", (("bounce", 4), ("br", 0.5), ("radius", 1.0))),
        _A("tremor", "TREMOR", "A ground-shaking line of blasts.", "lava", "embers", "magma", (("chain", 6), ("cstep", 6.5), ("cr", 0.65), ("radius", 0.8))),
        _A("detonator", "DETONATOR", "Bomblets, then a delayed blast.", "comet", "sparks", "cluster", (("cluster", 7), ("cspace", 4.0), ("cr", 0.5), ("fuse", 0.9), ("fr", 1.5), ("radius", 0.7))),
        _A("colossus", "COLOSSUS", "A huge, heavy, slow shell.", "shell", "embers", "fire", (("radius", 1.9), ("g", 1.25), ("speed", 0.9))),
        _A("phantomorb", "PHANTOM ORB", "A wind-proof orb that wanders.", "orb", "mist", "implode", (("wob", (7.0, 7.0)), ("wind", 0.0), ("radius", 1.2))),
        _A("meteor", "METEOR", "Plunges steeply and hits hard.", "sun", "fire", "fire", (("g", 1.8), ("speed", 1.15), ("radius", 1.5))),
        _A("sunder", "SUNDER", "A wide blast that hurts every tank in reach.", "prism", "prism", "prism", (("multi", 1), ("radius", 1.7)))]),
    _ammos([  # master
        _A("vortex", "VORTEX", "Drags tanks in, then detonates.", "void", "mist", "implode", (("pull", 10.0), ("radius", 1.6), ("fuse", 0.7), ("fr", 2.2))),
        _A("skyfall", "SKYFALL", "Ten shells rain from the apex.", "star", "twinkle", "starrays", (("apex", ("split", 10, 1.3)), ("subr", 0.55))),
        _A("wormhole", "WORMHOLE", "Warps far ahead and lands as a cluster.", "ring", "ripple", "cluster", (("apex", ("jump", 45.0)), ("cluster", 3), ("cr", 0.6))),
        _A("chainreact", "CHAIN REACTION", "A long chain of blasts.", "bolt", "arc", "thunder", (("chain", 8), ("cstep", 6.0), ("cr", 0.7))),
        _A("faultline", "FAULT LINE", "Splits the ground open and runs on.", "lava", "lava", "magma", (("terrain", "trench"), ("radius", 1.6), ("chain", 5), ("cstep", 8.0), ("cr", 0.6))),
        _A("mirrorshot", "MIRROR", "Bounces five times.", "prism", "prism", "prism", (("bounce", 5), ("br", 0.6), ("radius", 1.2))),
        _A("overkill", "OVERKILL", "An enormous multi-target blast.", "sun", "solar", "nova", (("radius", 2.1), ("multi", 1), ("g", 1.1))),
        _A("cataclysm", "CATACLYSM", "Bomblets, then a world-ending blast.", "flame", "fire", "fire", (("fuse", 1.5), ("fr", 2.6), ("cluster", 4), ("radius", 0.8))),
        _A("prismrain", "PRISM RAIN", "Seven prismatic shells from above.", "prism", "prism", "prism", (("apex", ("split", 7, 0.9)), ("subr", 0.75))),
        _A("maelstrom", "MAELSTROM", "A swirling storm that drags tanks about.", "helix", "prism", "bloom", (("wob", (9.0, 6.0)), ("pull", 6.0), ("radius", 1.4)))]),
)
# Premium: each of these is a different algorithm, not a bigger number.
_PREMIUM_AMMOS = _ammos([
    _A("eventhorizon", "EVENT HORIZON", "Drags everything in, then collapses.", "void", "mist", "implode", (("pull", 14.0), ("radius", 2.0), ("fuse", 1.0), ("fr", 2.6), ("multi", 1))),
    _A("phaseshift", "PHASE SHIFT", "Blinks across the field and carpet-bombs.", "phase", "alien", "alien", (("apex", ("jump", 60.0)), ("cluster", 5), ("cspace", 5.0), ("cr", 0.6))),
    _A("armageddon", "ARMAGEDDON", "Twelve shells from the sky.", "sun", "fire", "nova", (("apex", ("split", 12, 1.6)), ("subr", 0.7))),
    _A("tectonic", "TECTONIC FURY", "A fissure that races across the land.", "lava", "lava", "magma", (("terrain", "trench"), ("radius", 1.8), ("chain", 10), ("cstep", 9.0), ("cr", 0.75))),
    _A("quantumecho", "QUANTUM ECHO", "Six impossible bounces, then a cluster.", "helix", "prism", "bloom", (("bounce", 6), ("br", 0.6), ("radius", 1.0), ("cluster", 4))),
])
SHOP_AMMOS: tuple[AmmoType, ...] = (_SHOP_BASE_AMMOS[0:5] + _NEW_AMMOS[0] + _SHOP_BASE_AMMOS[5:10] + _NEW_AMMOS[1]
                                    + _SHOP_BASE_AMMOS[10:15] + _NEW_AMMOS[2] + _SHOP_BASE_AMMOS[15:20] + _NEW_AMMOS[3] + _PREMIUM_AMMOS)
TICKET_AMMOS: tuple[AmmoType, ...] = _ammos([
    _A("arcstorm", "ARC STORM", "A chain of lightning blasts.", "bolt", "arc", "thunder", (("chain", 6), ("cstep", 7.0), ("cr", 0.65), ("radius", 0.9))),
    _A("gavel", "GAVEL", "A small tap... then the verdict.", "comet", "solar", "nova", (("fuse", 1.0), ("fr", 2.0), ("radius", 0.6))),
    _A("coldfront", "COLD FRONT", "Six shards fall from the sky.", "shard", "frost", "shatter", (("apex", ("split", 6, 0.8)), ("subr", 0.65))),
    _A("sunburst", "SUNBURST", "A wide solar blast with bomblets.", "sun", "solar", "nova", (("radius", 1.5), ("multi", 1), ("cluster", 4), ("cr", 0.55))),
    _A("undertow", "UNDERTOW", "Drags tanks toward the blast.", "void", "mist", "implode", (("pull", 9.0), ("radius", 1.4))),
    _A("duneshot", "DUNE SHOT", "Skips across the sand, then scatters.", "shell", "embers", "cluster", (("bounce", 3), ("br", 0.45), ("cluster", 3))),
    _A("plague", "PLAGUE", "Spreading bomblets, then a cloud.", "drip", "mist", "cloud", (("cluster", 6), ("cr", 0.6), ("fuse", 0.9), ("fr", 1.6), ("radius", 0.6))),
    _A("umbra", "UMBRA", "Splits in the dark, then collapses.", "void", "mist", "implode", (("apex", ("split", 5, 0.8)), ("subr", 0.7), ("fuse", 0.8))),
    _A("starshower", "STARSHOWER", "Nine stars rain from the apex.", "star", "twinkle", "starrays", (("apex", ("split", 9, 1.1)), ("subr", 0.6))),
    _A("dragonfire", "DRAGONFIRE", "A river of flame along the ground.", "flame", "fire", "fire", (("chain", 5), ("cstep", 8.0), ("cr", 0.7), ("radius", 1.2))),
])
ANCIENT_AMMOS: tuple[AmmoType, ...] = _ammos([
    _A("sunfall", "SUNFALL", "A relic of light: a wide golden blast with falling stars.", "relic", "runes", "sigil",
       ((("radius", 1.5), ("apex", ("split", 4, 0.9)), ("subr", 0.7)))),
    _A("totality", "TOTALITY", "A watching eye that swallows light, then collapses.", "eye", "aeon", "eclipse",
       (("pull", 10.0), ("radius", 1.6), ("fuse", 0.8), ("fr", 1.8))),
    _A("tempest_a", "TEMPEST", "Wind and tide: chains of crashing waves.", "runes", "tides", "tidal",
       (("chain", 5), ("cstep", 7.5), ("cr", 0.7), ("radius", 1.0), ("wob", (3.0, 5.0)))),
    _A("cinderfall", "CINDERFALL", "A burning hexagram that rains molten stone.", "sigil", "cinders", "cinderstorm",
       (("cluster", 6), ("cr", 0.6), ("radius", 1.2), ("terrain", "mound"))),
    _A("timerift", "TIME RIFT", "Skips through time, then snaps back as a blast.", "sandglass", "chrono", "rewind",
       (("apex", ("jump", 26.0)), ("fuse", 0.9), ("fr", 1.9), ("radius", 1.1))),
])
ALL_AMMOS = AMMOS + SHOP_AMMOS + MISSION_AMMOS + TICKET_AMMOS + SECRET_AMMOS + ANCIENT_AMMOS
assert len(AMMOS) == 25 and AMMOS[-1].key == "aurora" and len(SHOP_AMMOS) == 65
assert len({a.key for a in ALL_AMMOS}) == len(ALL_AMMOS)
AMMO_BY_KEY = {a.key: a for a in ALL_AMMOS}
DEFAULT_AMMO = AMMOS[0]


@dataclass
class Tank:
    index: int
    name: str
    label: str
    color: RGB
    shot_color: RGB
    is_ai: bool
    x: float
    y: float
    facing: int
    angle: float
    power: float = 60.0
    hp: int = MAX_HP
    max_hp: int = MAX_HP
    hull_sentinel: tuple = (250, 3, 251)   # which animated colour (RGB / ASTRAL) the hull and shell use when the flags below are set
    shot_sentinel: tuple = (250, 3, 251)
    rgb_hull: bool = False       # RGB unlocks: the colour is re-rolled every frame by BattleWorld
    rgb_shot: bool = False
    hurt: float = 0.0
    recoil: float = 0.0
    destroyed: bool = False
    design: TankDesign = DEFAULT_DESIGN
    ammo: AmmoType = DEFAULT_AMMO
    team: int = 0                # tanks on the same team never end a round against each other
    owner: int = 0               # LAN: which machine controls this tank (0 = host)
    kind: str = ""               # "" = ordinary tank, "ufo" = large hovering saucer
    scale: float = 1.0           # hit-box / size multiplier (UFOs are big)
    hover: float = 0.0           # height above the ground it floats at
    shield: int = 0              # hits the shield still absorbs
    volley: int = 0              # extra shots left in the current turn
    turns: int = 0               # turns taken (drives UFO behaviour)
    charge: int = 0              # special attack: forfeited turns banked (2 = ready)
    guard: bool = False          # shielded for as long as another tank on its team is alive (the Mothership)
    vis_dx: float = 0.0          # visual-only offset that eases a teleporting UFO into place
    phase: int = 0               # UFO damage phases already triggered

    @property
    def width(self) -> float:
        return TANK_W * self.scale

    @property
    def height(self) -> float:
        return TANK_H * self.scale

    @property
    def alive(self) -> bool:
        return self.hp > 0 and not self.destroyed

    @property
    def rel_angle(self) -> float:
        return self.angle if self.facing > 0 else 180 - self.angle

    def set_rel_angle(self, rel: float) -> None:
        self.angle = rel if self.facing > 0 else 180 - rel

    @property
    def rect(self) -> tuple[float, float, float, float]:
        return self.x - self.width / 2, self.y, self.x + self.width / 2, self.y + self.height

    def launch_origin(self, angle: Optional[float] = None) -> tuple[float, float]:
        ang = self.angle if angle is None else angle
        if self.scale == 1.0:
            return muzzle_point(self.x, self.y, ang, self.design)
        a = math.radians(ang)                    # big units fire from their belly cannon
        d, ph = self.width * 0.45 + 1.0, self.height * 0.35
        return self.x + d * math.cos(a), self.y + ph + d * math.sin(a)


def hsv(h: float, s: float = 1.0, v: float = 1.0) -> RGB:
    r, g, b = colorsys.hsv_to_rgb(h % 1.0, s, v)
    return int(r * 255), int(g * 255), int(b * 255)


# A sentinel colour: whatever holds it is drawn as a slowly cycling rainbow (the RGB hull / RGB shell unlocks).
RAINBOW: RGB = (250, 3, 251)


ASTRAL: RGB = (251, 4, 250)          # second sentinel: the Astral Rift paint (Martyr reward), selectable for hull AND shell
PRISMATIC: RGB = (252, 6, 247)       # shop paint: the Prismatic King's refracting crystal, on ANY tank
VOID: RGB = (247, 2, 252)            # shop paint: the Singularity's light-eating void, on ANY tank
PIXEL_PAINTS = (ASTRAL, PRISMATIC, VOID)                  # paints that colour every hull pixel individually
LIVE_COLORS = (RAINBOW, ASTRAL, PRISMATIC, VOID)          # animated sentinels (re-rolled each frame by BattleWorld)


_GALAXY = ((0, (24, 14, 96)), (0.2, (96, 44, 190)), (0.4, (196, 64, 196)), (0.6, (56, 160, 220)), (0.8, (232, 232, 255)), (1.0, (24, 14, 96)))


def galaxy(t: float, c: float, r: float) -> RGB:
    """Per-pixel drifting nebula with twinkling stars: what the ASTRAL paint looks like on a hull."""
    v = (math.sin(c * 0.55 + t * 1.1) + math.sin(r * 0.9 - t * 0.8) + math.sin((c + r) * 0.45 + t * 0.6) + 3) / 6
    col = gradient(_GALAXY, round(v * 24) / 24)
    if _flick(t * 1.4, c * 7 + r * 3, 23) > 0.93:
        return (255, 255, 255)
    return col


def astral(t: float) -> RGB:
    """Slowly shifting Astral Rift colours: deep indigo -> violet -> starlight white -> gold -> back."""
    return gradient(_GALAXY, round((t * 0.18 % 1.0) * 48) / 48)


def prismatic(t: float) -> RGB:
    return hsv(round((t * 0.3 % 1.0) * 36) / 36, 0.35, 1.0)


def void_tint(t: float) -> RGB:
    return mix((90, 54, 190), (176, 120, 255), 0.5 + 0.5 * math.sin(t * 2.4))


def prismatic_px(t: float, c: float, r: float) -> RGB:
    """Per-pixel refraction: a pale crystal whose facets drift through the whole spectrum, with white glints."""
    if _flick(t * 1.6, c * 5 + r * 11, 29) > 0.94:
        return (255, 255, 255)
    hue = (c * 0.05 + r * 0.035 + t * 0.3 + 0.12 * math.sin(c * 0.9 - r * 0.6 + t * 1.7)) % 1
    return hsv(round(hue * 48) / 48, 0.40 + 0.2 * math.sin(r * 0.5 + t), 1.0)


def void_px(t: float, c: float, r: float) -> RGB:
    """Per-pixel void: near-black violet that ripples slowly and swallows light, with rare violet glints."""
    if _flick(t * 1.2, c * 3 + r * 7, 31) > 0.95:
        return (176, 126, 255)
    v = (math.sin(c * 0.7 + t * 1.3) + math.sin(r * 1.1 - t * 0.9) + 2) / 4
    return mix((6, 3, 18), (54, 30, 118), v)


def paint_shade(paint: RGB, gb: RGB, ch: str) -> RGB:
    if paint == VOID:
        return {"t": mix(gb, (120, 80, 210), 0.22), "g": mix(gb, (180, 130, 255), 0.55), "k": shade(gb, 0.6),
                "w": mix(gb, (90, 70, 150), 0.3), "d": shade(gb, 0.7)}.get(ch, gb)
    return {"t": mix(gb, (255, 255, 255), 0.15), "g": mix(gb, (255, 255, 255), 0.4), "k": shade(gb, 0.45),
            "w": mix(gb, (200, 210, 230), 0.2), "d": shade(gb, 0.5)}.get(ch, gb)


def live_color(c: RGB, t: float) -> RGB:
    return (rainbow(t) if c == RAINBOW else astral(t) if c == ASTRAL else prismatic(t) if c == PRISMATIC
            else void_tint(t) if c == VOID else c)


def rainbow(t: float) -> RGB:
    return hsv(round((t * 0.35 % 1.0) * 36) / 36, 0.85, 1.0)      # 36 steps keeps the colour cache tiny


# ---- Barrels: reusable muzzle hardware. `offsets` are lateral barrel positions, `length` is the muzzle distance from the
# pivot (it also decides where shells leave the gun), `style` picks the per-pixel colouring in barrel_color().
@dataclass(frozen=True)
class BarrelSpec:
    offsets: tuple = (0.0,)
    length: float = BARREL_LEN
    style: str = "plain"


BARRELS: dict[str, BarrelSpec] = {
    "single": BarrelSpec(), "double": BarrelSpec((-1.1, 1.1)), "heavy": BarrelSpec((-0.5, 0.5)),
    "rail": BarrelSpec((0.0,), BARREL_LEN, "rail"), "triple": BarrelSpec((-1.6, 0.0, 1.6), BARREL_LEN, "rail"),
    "prism": BarrelSpec((-0.5, 0.5), BARREL_LEN, "prism"),
    "sniper": BarrelSpec((0.0,), 10.5, "plain"), "howitzer": BarrelSpec((-0.7, 0.7), 6.0, "plain"),
    "needle": BarrelSpec((0.0,), 9.5, "rail"), "quad": BarrelSpec((-2.1, -0.7, 0.7, 2.1), 7.0, "plain"),
    "gatling": BarrelSpec((-0.9, 0.0, 0.9), 7.0, "spin"), "coil": BarrelSpec((0.0,), 8.0, "coil"),
    "crystal": BarrelSpec((-0.4, 0.4), 8.5, "crystal"), "flame": BarrelSpec((0.0,), 7.5, "flame"),
    "void": BarrelSpec((0.0,), 8.0, "void"), "bolt": BarrelSpec((-0.6, 0.6), 8.0, "bolt"),
    "sunlance": BarrelSpec((0.0,), 10.0, "flame"), "umbra": BarrelSpec((-0.6, 0.6), 9.0, "void"), "gale": BarrelSpec((-1.2, 0.0, 1.2), 8.5, "coil"),
    "magmacannon": BarrelSpec((-0.5, 0.5), 8.5, "flame"), "ancient": BarrelSpec((-0.9, 0.0, 0.9), 9.5, "rune"),
}


def barrel_spec(name: str) -> BarrelSpec:
    return BARRELS.get(name, BARRELS["single"])


def muzzle_point(x: float, y: float, angle: float, design: "TankDesign", scale: float = 1.0, recoil: float = 0.0) -> tuple[float, float]:
    """Where a shell leaves the gun for a tank drawn at (x, y) with the barrel at `angle` degrees. THE single source of
    truth: draw_tank, Tank.launch_origin and every title-screen / shop animation use it, so shells always start at the
    visible muzzle (a one-pixel margin keeps them clear of the barrel itself)."""
    a = math.radians(angle)
    d = (barrel_spec(design.barrel).length - recoil * 2.5 + 1.0) * scale
    return x + d * math.cos(a), y + PIVOT_H * scale + d * math.sin(a)


def visual_lift(design: "TankDesign", t: float, scale: float = 1.0) -> float:
    """How far draw_tank raises a hovering design (hover height + bob). Muzzle users add it so shells leave the barrel."""
    return design.hover + 0.6 * math.sin(t * 2.4) * scale if design.hover > 0 else 0.0


def barrel_color(style: str, base: RGB, accent: RGB, t: float, d: float, lane: int) -> RGB:
    if style == "rune":
        if int(d * 3 - t * 10) % 7 == 0:
            return (255, 252, 230)
        return mix(shade(base, 0.7), accent, 0.5 + 0.5 * math.sin(d * 1.4 - t * 7 + lane))
    if style == "rail":
        return accent if int(d * 2 - t * 14) % 6 == 0 else base
    if style == "coil":
        return accent if int(d * 2.2 - t * 9) % 3 == 0 else shade(base, 0.7)
    if style == "prism":
        return hsv(t * 0.5 + d * 0.04, 0.55, 1.0)
    if style == "crystal":
        f = 0.5 + 0.5 * math.sin(d * 1.7 - t * 5 + lane * 2.0)
        return mix(hsv(t * 0.45 + d * 0.07 + lane * 0.2, 0.5, 1.0), (255, 255, 255), 0.6 * f)
    if style == "flame":
        k = clamp(d / 8.0 + 0.2 * math.sin(t * 20 + d * 3), 0, 1)
        return gradient(((0, base), (0.55, (255, 120, 30)), (1, (255, 230, 120))), k)
    if style == "spin":
        return accent if (int(t * 18) + lane) % 3 == 0 else base
    if style == "void":
        return mix((20, 10, 40), (170, 90, 255), 0.5 + 0.5 * math.sin(d * 1.3 - t * 6))
    if style == "bolt":
        return (235, 245, 255) if _flick(t, int(d * 2), lane) > 0.8 else mix(base, accent, 0.5)
    return base


# ---- Material pixels in tank masks: letters beyond the basic t/h/g/k/w/d/a/l palette. A material is a function
# (body, accent, t, col, row, flash) -> colour, so any design can use crystal, fire, void or storm pixels.
def _mat_prism(body, a, t, c, r, f):          # a refracting crystal facet: hue shifts across the tank and in time
    return hsv(t * 0.35 + c * 0.085 + r * 0.14, 0.62, 1.0)


def _mat_prism_hi(body, a, t, c, r, f):       # a bright glint travelling across the crystal
    k = max(0.0, 1 - abs((c + r) - ((t * 7) % 18 - 3)) / 2.2)
    return mix(hsv(t * 0.35 + c * 0.085 + r * 0.14, 0.25, 1.0), (255, 255, 255), k)


def _mat_fire(body, a, t, c, r, f):           # licking flame
    k = clamp(0.55 + 0.45 * math.sin(t * 21 + c * 2.3 + r * 1.7) * math.sin(t * 9 + c), 0, 1)
    return gradient(((0, (150, 20, 8)), (0.5, (255, 110, 20)), (1, (255, 236, 120))), k)


def _mat_ember(body, a, t, c, r, f):          # glowing-hot armour that breathes
    return gradient(((0, shade(body, 0.4)), (1, (255, 90, 30))), 0.5 + 0.5 * math.sin(t * 3 + c * 0.9 + r))


def _mat_void(body, a, t, c, r, f):
    return mix((8, 4, 20), (110, 50, 200), 0.5 + 0.5 * math.sin(t * 4 + c * 0.8 - r))


def _mat_bolt(body, a, t, c, r, f):
    return (240, 248, 255) if _flick(t, c * 7 + r, 11) > 0.84 else mix(shade(body, 0.35), a, 0.45)


def _mat_gold(body, a, t, c, r, f):
    k = max(0.0, 1 - abs((c * 1.3 + r) - ((t * 6) % 20 - 4)) / 2.5)
    return mix((196, 150, 50), (255, 244, 180), k)


MATERIALS: dict[str, Callable] = {"p": _mat_prism, "q": _mat_prism_hi, "f": _mat_fire, "e": _mat_ember, "v": _mat_void,
                                  "b": _mat_bolt, "y": _mat_gold}


def _mat_solar(body, a, t, c, r, f):
    return gradient(((0, (255, 130, 30)), (0.5, (255, 222, 96)), (1, (255, 255, 236))), 0.5 + 0.5 * math.sin(t * 5 - r * 1.6 + c * 0.45))


def _mat_rune(body, a, t, c, r, f):
    if _flick(t * 0.7, c * 3 + r, 13) > 0.78:
        return (255, 244, 190)
    return mix((140, 100, 28), (226, 176, 70), 0.5 + 0.5 * math.sin(t * 2.4 + c * 0.9 - r))


def _mat_star(body, a, t, c, r, f):
    return mix((196, 212, 255), (255, 255, 255), 0.5 + 0.5 * math.sin(t * 9 + c * 2.1 + r))


def _mat_night(body, a, t, c, r, f):
    if _flick(t * 0.8, c * 5 + r, 17) > 0.93:
        return (226, 206, 255)
    return mix((10, 6, 26), (74, 32, 146), 0.5 + 0.5 * math.sin(t * 2 + c * 0.7 + r))


def _mat_aether(body, a, t, c, r, f):
    return mix((20, 110, 150), (150, 255, 240), 0.5 + 0.5 * math.sin(t * 3.5 - c * 0.9 + r * 0.6))


def _mat_chrono(body, a, t, c, r, f):
    k = max(0.0, 1 - abs((c + r * 2) - ((t * 8) % 24 - 6)) / 2.2)
    return mix(mix((120, 70, 30), (232, 172, 72), 0.5 + 0.5 * math.sin(t * 1.5 + c)), (255, 250, 222), k)


MATERIALS.update({"o": _mat_solar, "r": _mat_rune, "s": _mat_star, "n": _mat_night, "m": _mat_aether, "z": _mat_chrono})

# Glow styles recolour the 'l' pixels; ambient kinds are particle / overlay flavours (combine with '+').
GLOW_STYLES = ("none", "pulse", "blink", "flame", "prism", "scan", "plasma", "lava", "storm", "matrix",
               "refract", "ember", "void", "ice", "toxic", "neon", "gold", "aeon")
AMBIENT_KINDS = ("none", "sparks", "embers", "mist", "sparkle", "static", "frost", "flare", "bits",
                 "smoke", "fire", "burn", "prism", "orbit", "void", "lightning", "bubbles", "petals", "halo", "glyphs", "riftmotes")


def _flick(t: float, a: int, b: int) -> float:
    """Cheap deterministic flicker noise in 0..1 that changes ~30 times a second."""
    return (((int(t * 30) * 73856093) ^ (a * 19349663) ^ (b * 83492791)) & 255) / 255.0


def _glow_color(design: TankDesign, body: RGB, t: float, idx: int) -> RGB:
    g, a = design.glow, design.accent
    if g == "aeon":
        return gradient(((0, (90, 40, 190)), (0.5, (255, 214, 120)), (1, (255, 255, 244))), 0.5 + 0.5 * math.sin(t * 3.2 + idx * 0.8))
    if g == "pulse":
        return mix(shade(a, 0.3), a, 0.5 + 0.5 * math.sin(t * 4 + idx))
    if g == "blink":
        return a if int(t * 3 + idx) % 2 == 0 else shade(a, 0.25)
    if g == "flame":
        k = 0.5 + 0.5 * math.sin(t * 23 + idx * 2.1) * math.sin(t * 11 + idx)
        return gradient(((0, (120, 30, 10)), (0.5, (255, 120, 20)), (1, (255, 232, 130))), k)
    if g == "prism":
        return hsv(t * 0.4 + idx * 0.13, 0.7, 1.0)
    if g == "scan":          # a bright bar sweeping along the visor
        k = max(0.0, 1 - abs(idx - ((t * 9) % 15 - 2)) / 2.5)
        return mix(mix(shade(a, 0.28), a, 0.35), (255, 255, 255), k * 0.85)
    if g == "plasma":
        return gradient(((0, (255, 90, 20)), (0.5, a), (1, (255, 250, 205))), 0.5 + 0.5 * math.sin(t * 4.5 + idx * 0.7))
    if g == "lava":
        k = clamp(0.5 + 0.4 * math.sin(t * 2.2 + idx * 1.7) + 0.2 * (_flick(t, idx, 3) - 0.5), 0, 1)
        return gradient(((0, (90, 10, 6)), (0.6, (230, 70, 18)), (1, (255, 196, 80))), k)
    if g == "storm":
        f = _flick(t, idx, 5)
        return mix(shade(a, 0.22), (235, 245, 255), 0.85) if f > 0.86 else mix(shade(a, 0.22), a, f * 0.6)
    if g == "matrix":
        return (96, 255, 130) if _flick(t, idx, 9) > 0.5 else (18, 96, 44)
    if g == "refract":       # prism light with white glints sweeping through it
        k = max(0.0, 1 - abs(idx - ((t * 8) % 16 - 2)) / 2.0)
        return mix(hsv(t * 0.4 + idx * 0.09, 0.6, 1.0), (255, 255, 255), k * 0.8)
    if g == "ember":
        return gradient(((0, (110, 14, 8)), (0.6, (230, 60, 20)), (1, (255, 190, 90))),
                        clamp(0.5 + 0.35 * math.sin(t * 2.6 + idx * 1.3) + 0.3 * (_flick(t, idx, 7) - 0.5), 0, 1))
    if g == "void":
        return mix((10, 4, 24), a, 0.5 + 0.5 * math.sin(t * 3 + idx * 0.9))
    if g == "ice":
        return mix(shade(a, 0.5), (240, 252, 255), 0.5 + 0.5 * math.sin(t * 2.4 + idx * 0.8))
    if g == "toxic":
        return mix((20, 70, 20), (170, 255, 60), 0.5 + 0.5 * math.sin(t * 5 + idx * 1.1))
    if g == "neon":
        return (255, 70, 220) if (int(t * 4) + idx) % 2 == 0 else (60, 240, 255)
    if g == "gold":
        k = max(0.0, 1 - abs(idx - ((t * 6) % 15 - 2)) / 2.5)
        return mix((190, 140, 40), (255, 244, 170), k)
    return mix(body, (255, 255, 255), 0.6)


def draw_tank(canvas: PixelCanvas, x: float, y: float, facing: int, angle: float, body: RGB, scale: int = 1,
              flash: float = 0.0, wreck: bool = False, recoil: float = 0.0,
              design: TankDesign = DEFAULT_DESIGN, t: float = 0.0, lift: Optional[float] = None,
              hover: Optional[float] = None) -> None:
    """Draws a tank whose base is at (x, y). `lift` is extra height added here (None = the design's own hover, which is
    what previews want; battles pass 0 because the simulated tank already floats). `hover` is how far the body is
    above the ground, used for the shadow and thruster (None = same as lift)."""
    paint = body if (body in PIXEL_PAINTS and not wreck) else None
    galaxy_on = paint is not None                                 # per-pixel paint (ASTRAL / PRISMATIC / VOID) overrides a design's own paint
    if galaxy_on:
        body = live_color(paint, t)
    elif design.paint is not None:
        body = design.paint
    elif body == RAINBOW:
        body = live_color(body, t)
    lift = design.hover if lift is None else lift
    air = lift if hover is None else hover
    ground_y = y
    if air > 0 and not wreck:
        bob = 0.6 * math.sin(t * 2.4) * scale
        y = y + lift + bob
        ground_y = y - air * scale - bob if hover is not None else y - lift - bob
    elif lift:
        y = y + lift
    if air > 0 and not wreck:                                  # soft shadow on the ground + thruster glow under the hull
        sh = clamp(1.0 - air * scale / 16.0, 0.35, 0.9)
        for dx in range(int(-5.5 * scale), int(5.5 * scale) + 1):
            k = 1 - abs(dx) / (5.5 * scale)
            canvas.blendf(x + dx, ground_y, (0, 0, 0), 0.55 * sh * k)
        for k in range(3):
            canvas.blendf(x + (k - 1) * 2 * scale, y - 1, design.accent, 0.35 + 0.25 * math.sin(t * 17 + k * 2))
    if wreck:
        pal = {"t": (34, 34, 38), "h": (28, 28, 32), "g": (44, 40, 40), "k": (18, 18, 20), "w": (30, 30, 34),
               "d": (20, 20, 22), "a": (44, 40, 38), "l": (62, 34, 22)}
    else:
        pal = {"t": mix(body, (255, 255, 255), 0.28), "h": body, "g": mix(body, (255, 255, 255), 0.5),
               "k": mix(shade(body, 0.3), (36, 40, 48), 0.5), "w": mix(body, (200, 210, 220), 0.3),
               "d": shade(body, 0.38), "a": design.accent}
    if flash > 0:
        pal = {k: mix(v, (255, 255, 255), flash) for k, v in pal.items()}
    if not wreck:
        a = math.radians(angle)
        ca, sa = math.cos(a), math.sin(a)
        spec = barrel_spec(design.barrel)
        px, py = x, y + PIVOT_H * scale
        length = (spec.length - recoil * 2.5) * scale
        base = mix(body, (225, 232, 240), 0.4)
        if flash > 0:
            base = mix(base, (255, 255, 255), flash)
        for lane, off in enumerate(spec.offsets):
            for k in range(int(length * 2) + 1):
                d = k * 0.5
                bx, by = px + d * ca - off * scale * sa, py + d * sa + off * scale * ca
                col = barrel_color(spec.style, base, design.accent, t, d, lane)
                for ox in range(scale):
                    for oy in range(scale):
                        canvas.plotf(bx + ox * 0.9 - (scale - 1) * 0.45, by + oy * 0.9 - (scale - 1) * 0.45, col)
        canvas.plotf(px + length * ca, py + length * sa, mix(base, (255, 255, 255), 0.6))
    base_x, base_y = int(x // 1) - 5 * scale, int(round(y))
    for r, row in enumerate(design.mask):
        if wreck and r < 2 and (r == 0 or facing > 0):
            continue
        for c, ch in enumerate(row):
            if ch == ".":
                continue
            cc = c if facing > 0 else TANK_W - 1 - c
            if ch == "l" and not wreck:
                col = _glow_color(design, body, t, cc)
            elif ch in MATERIALS and not wreck:
                col = MATERIALS[ch](body, design.accent, t, cc, r, flash)
            elif galaxy_on and ch in "thgkwd":
                gb = (galaxy if paint == ASTRAL else prismatic_px if paint == PRISMATIC else void_px)(t, cc, r)
                col = paint_shade(paint, gb, ch)
            else:
                col = pal.get(ch, pal["h"])
            if flash > 0 and (ch == "l" or ch in MATERIALS) and not wreck:
                col = mix(col, (255, 255, 255), flash)
            wx, wy = base_x + cc * scale, base_y + (TANK_H - 1 - r) * scale
            for ox in range(scale):
                for oy in range(scale):
                    canvas.plot(wx + ox, wy + oy, col)
    if not wreck:
        _draw_ambient_overlay(canvas, x, y, scale, design, t)


def _draw_ambient_overlay(canvas: PixelCanvas, x: float, y: float, scale: int, design: TankDesign, t: float) -> None:
    """Draw-time halves of the ambient kinds (twinkles, orbiting motes, arcs). Particle halves live in EffectsFactory."""
    kinds = design.ambient.split("+")
    if "halo" in kinds:                                      # a slowly turning ring of gold light over the turret
        for i in range(14):
            ang = t * 1.2 + i * math.tau / 14
            hx, hy = x + math.cos(ang) * 6.5 * scale, y + 9 * scale + math.sin(ang) * 1.6 * scale
            canvas.blendf(hx, hy, (255, 232, 150), 0.35 + 0.5 * (0.5 + 0.5 * math.sin(ang * 2 - t * 4)))
    if "glyphs" in kinds:                                    # tiny rune crosses drifting around the hull
        for i in range(4):
            ang = -t * 0.9 + i * math.pi / 2
            gx, gy = x + math.cos(ang) * 8.5 * scale, y + 4 * scale + math.sin(ang) * 3.2 * scale
            a_ = 0.45 + 0.45 * math.sin(t * 5 + i * 1.9)
            canvas.blendf(gx, gy, design.accent, a_)
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                canvas.blendf(gx + dx, gy + dy, design.accent, a_ * 0.4)
    if "riftmotes" in kinds:                                 # light and dark sparks circling in opposition
        for i in range(8):
            ang = t * 1.5 + i * math.tau / 8
            rx, ry = x + math.cos(ang) * (6 + i % 3) * scale, y + 4 * scale + math.sin(ang) * 3 * scale
            canvas.blendf(rx, ry, (255, 244, 205) if i % 2 == 0 else (120, 60, 220), 0.85)
    if "sparkle" in kinds:
        for i in range(4):
            ang = t * 1.3 + i * math.pi / 2
            sx, sy = x + math.cos(ang) * 8 * scale, y + 3 * scale + math.sin(ang) * 5 * scale
            tw = 0.5 + 0.5 * math.sin(t * 9 + i * 1.7)
            canvas.blendf(sx, sy, (255, 230, 150), 0.85 * tw)
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                canvas.blendf(sx + dx, sy + dy, (255, 200, 110), 0.3 * tw)
    if "prism" in kinds:                                     # refraction glints: rainbow stars popping around the crystal
        for i in range(6):
            ph = (t * 0.9 + i * 0.37) % 1.0
            sx = x + math.cos(i * 2.4 + t * 0.6) * (5 + i % 3) * scale
            sy = y + (1.5 + 3.2 * ((i * 5) % 4) / 3) * scale + 2 * math.sin(t * 2 + i)
            col, a = hsv((i / 6 + t * 0.4) % 1, 0.5, 1.0), math.sin(ph * math.pi)
            canvas.blendf(sx, sy, (255, 255, 255), 0.95 * a)
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                canvas.blendf(sx + dx, sy + dy, col, 0.55 * a)
            for dx, dy in ((2, 0), (-2, 0), (0, 2), (0, -2)):
                canvas.blendf(sx + dx, sy + dy, col, 0.25 * a)
    if "orbit" in kinds:
        for i in range(5):
            ang = t * (1.6 + 0.2 * i) + i * 1.26
            sx, sy = x + math.cos(ang) * (7 + i % 2) * scale, y + 3.5 * scale + math.sin(ang) * 2.4 * scale
            canvas.blendf(sx, sy, design.accent, 0.9)
            canvas.blendf(sx - math.cos(ang + 1.5) * 0.9, sy - math.sin(ang + 1.5) * 0.9, design.accent, 0.35)
    if "lightning" in kinds:
        step = int(t * 14)
        for i in range(2):
            if _flick(step / 30.0, i, 33) > 0.55:
                px, py = x + (_flick(step / 30.0, i, 1) - 0.5) * 9 * scale, y + 6 * scale
                for s in range(5):
                    px += (_flick(step / 30.0, i, 10 + s) - 0.5) * 3
                    py += 1.6
                    canvas.blendf(px, py, (230, 245, 255), 0.9 - s * 0.14)
    if "void" in kinds:                                      # motes drawn inward to the hull
        for i in range(6):
            k = (t * 0.8 + i / 6) % 1.0
            ang = i * 1.05 + t * 0.7
            sx, sy = x + math.cos(ang) * (1 - k) * 9 * scale, y + 3 * scale + math.sin(ang) * (1 - k) * 5 * scale
            canvas.blendf(sx, sy, (170, 110, 255), 0.7 * k)


# 3x5 bitmaps for the ASCII easter-egg ammo (row 0 is the top of the glyph)
_GLYPH_BITS = (
    ("#.#", "###", "#.#", "###", "#.#"), ("###", "#.#", "#.#", "#.#", "###"), (".#.", "##.", ".#.", ".#.", "###"),
    ("...", ".#.", "###", ".#.", "..."), ("#.#", ".#.", "###", ".#.", "#.#"), ("#.#", "..#", ".#.", "#..", "#.#"),
    (".#.", "#.#", ".#.", "#.#", ".##"), ("..#", "..#", ".#.", "#..", "#.."),
)


def draw_glyph(canvas: PixelCanvas, x: float, y: float, k: int, col: RGB, a: float) -> None:
    for r, row in enumerate(_GLYPH_BITS[k % len(_GLYPH_BITS)]):
        for c, ch in enumerate(row):
            if ch == "#":
                canvas.blendf(x + c - 1, y + 2 - r, col, a)


def draw_ufo(canvas: PixelCanvas, t: Tank, now: float, flash: float = 0.0) -> None:
    """A big, glowing, bobbing flying saucer (drawn inside the tank's hit box so what you see is what you hit)."""
    cx = t.x + t.vis_dx
    w, h = t.width, t.height
    cy = t.y + 0.9 * math.sin(now * 2.0 + t.index)                   # bob is visual only; the hit box never moves
    if t.destroyed:
        return
    body = t.color
    firing = t.recoil > 0
    gy = t.y - t.hover                                                   # the ground under the saucer
    for dx in range(int(-w * 0.42), int(w * 0.42) + 1):                  # a shadow that shrinks/darkens with height
        k = 1 - abs(dx) / (w * 0.42)
        canvas.blendf(t.x + dx, gy, (0, 0, 0), 0.5 * k * clamp(1.2 - t.hover / 40.0, 0.35, 0.9))
        if k > 0.5:
            canvas.blendf(t.x + dx, gy + 1, (0, 0, 0), 0.25 * k)
    hull_a, hull_b = mix(body, (255, 255, 255), 0.35), shade(body, 0.45)
    for ix in range(int(-w / 2) - 1, int(w / 2) + 2):
        for iy in range(-1, int(h) + 2):
            u, v = ix / (w / 2), (iy - h * 0.40) / (h * 0.5)
            px, py = cx + ix, cy + iy
            col, a = None, 1.0
            # saucer disc
            if (u / 1.0) ** 2 + (v / 0.42) ** 2 <= 1.0:
                shade_k = 0.5 + 0.5 * (-v / 0.42)
                col = mix(hull_b, hull_a, clamp(shade_k, 0, 1))
                if abs(v) < 0.06:                                        # glowing rim ring
                    col = mix(col, (255, 255, 255), 0.5)
            # glass dome
            dv = (iy - h * 0.52) / (h * 0.42)
            if u * u / 0.25 * 1.0 + dv * dv <= 1.0 and dv >= -0.05:
                col = mix((60, 220, 200), (190, 255, 255), clamp(0.3 + 0.7 * dv, 0, 1))
                if dv > 0.45 and u < -0.05:
                    col = mix(col, (255, 255, 255), 0.55)                # highlight
            # underside cannon pod
            if (u / 0.28) ** 2 + ((iy - h * 0.16) / (h * 0.16)) ** 2 <= 1.0:
                col = mix(shade(body, 0.3), (255, 220, 120) if firing else (90, 100, 120), 0.7 if firing else 0.3)
            if col is not None:
                if flash > 0:
                    col = mix(col, (255, 255, 255), min(1.0, flash))
                canvas.plotf(px, py, col)
    # chasing rim lights
    n = 9
    for k in range(n):
        ang = k / n * math.tau
        lx, ly = cx + math.cos(ang) * w * 0.44, cy + h * 0.40 + math.sin(ang) * h * 0.14
        on = int(now * 6 + k) % 3 == 0
        canvas.plotf(lx, ly, hsv((k / n + now * 0.2) % 1, 0.5, 1.0) if on else shade(body, 0.4))
    for dy in range(1, int(min(8, t.hover)) + 1):                        # thruster glow separating it from the ground
        canvas.blendf(cx, cy + h * 0.1 - dy, (150, 255, 200), 0.38 - dy * 0.04 + 0.1 * math.sin(now * 14 + dy))
    if firing or int(now * 3) % 5 == 0:                                  # tractor-beam glow under the cannon
        for dy in range(1, 6):
            canvas.blendf(cx, cy + h * 0.08 - dy, (170, 255, 200), 0.5 - dy * 0.08)
    if t.shield > 0:                                                     # shimmering shield bubble
        for k in range(int(w * 4)):
            ang = k / (w * 4) * math.tau
            fl = 0.35 + 0.3 * math.sin(now * 9 + k * 0.7)
            canvas.blendf(cx + math.cos(ang) * w * 0.62, cy + h * 0.5 + math.sin(ang) * h * 0.78, (140, 230, 255), fl)


def draw_projectile(canvas: PixelCanvas, x: float, y: float, vx: float, vy: float, accent: RGB,
                    ammo: AmmoType, t: float) -> None:
    """Draws the shell head in the style of its ammo type (looks only)."""
    accent = live_color(accent, t)
    sp = math.hypot(vx, vy) or 1.0
    ux, uy = vx / sp, vy / sp
    white = (255, 255, 255)
    nb = ((1, 0), (-1, 0), (0, 1), (0, -1))
    style = ammo.proj
    if style == "streak":
        for i in range(6):
            canvas.blendf(x - ux * i, y - uy * i, mix(white, accent, i / 6), 1.0 - i / 7)
    elif style == "orb":
        r = 1.5 + 0.4 * math.sin(t * 14)
        for dx in range(-3, 4):
            for dy in range(-3, 4):
                d = math.hypot(dx, dy)
                if d <= r + 0.5:
                    canvas.blendf(x + dx, y + dy, mix(accent, white, 0.4), 0.9)
                elif d <= 3.2:
                    canvas.blendf(x + dx, y + dy, accent, 0.22)
        canvas.plotf(x, y, white)
    elif style == "spark":
        canvas.plotf(x, y, white)
        for k in range(3):
            canvas.blendf(x + (_flick(t, k, 1) - 0.5) * 3.5, y + (_flick(t, k, 2) - 0.5) * 3.5, accent, 0.9)
    elif style == "comet":
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                canvas.blendf(x + dx, y + dy, mix(accent, white, 0.3), 0.7)
        canvas.plotf(x, y, white)
    elif style == "drip":
        canvas.plotf(x, y, mix(accent, white, 0.35))
        for dx, dy in nb:
            canvas.blendf(x + dx, y + dy, accent, 0.6)
        canvas.blendf(x, y - 2, accent, 0.55)
        canvas.blendf(x, y - 3, accent, 0.25)
    elif style in ("star", "prism"):
        tw = 0.5 + 0.5 * math.sin(t * 12)
        cols = [accent] * 4 if style == "star" else [hsv(t * 1.2 + k * 0.25, 0.6, 1.0) for k in range(4)]
        canvas.plotf(x, y, white)
        for (dx, dy), c in zip(nb, cols):
            canvas.blendf(x + dx, y + dy, c, 0.95)
            canvas.blendf(x + dx * 2, y + dy * 2, c, 0.35 + 0.5 * tw)
        if tw > 0.55:
            for dx, dy in ((1, 1), (-1, 1), (1, -1), (-1, -1)):
                canvas.blendf(x + dx, y + dy, white, 0.5)
    elif style == "relic":                                   # spinning gold diamond inside a halo of light
        ang0 = t * 6
        for k in range(12):
            a_ = k * math.tau / 12 - ang0 * 0.5
            canvas.blendf(x + math.cos(a_) * 3.4, y + math.sin(a_) * 3.4, (255, 226, 140), 0.35)
        for k in range(4):
            a_ = ang0 + k * math.pi / 2
            canvas.blendf(x + math.cos(a_) * 2.2, y + math.sin(a_) * 2.2, mix(accent, white, 0.5), 0.9)
        for dx, dy in nb:
            canvas.blendf(x + dx, y + dy, accent, 0.95)
        canvas.plotf(x, y, white)
    elif style == "eye":                                     # a watching eye with a slit pupil
        for dx in range(-3, 4):
            for dy in range(-3, 4):
                d = math.hypot(dx * 0.8, dy * 1.25)
                if d <= 2.6:
                    canvas.blendf(x + dx, y + dy, mix(accent, white, 0.55) if d > 1.1 else shade(accent, 0.1), 0.95)
        for dy in (-1, 0, 1):
            canvas.blendf(x, y + dy, (10, 4, 20), 0.9)
        for k in range(10):
            a_ = k * math.tau / 10 + t * 3
            canvas.blendf(x + math.cos(a_) * 3.8, y + math.sin(a_) * 3.8, accent, 0.4)
    elif style == "runes":                                   # four rune spokes spinning (Tempest's shell; was shadowing GLYPH's digits)
        a0 = t * 9
        for k in range(4):
            a_ = a0 + k * math.pi / 2
            for sp in (1, 2, 3):
                canvas.blendf(x + math.cos(a_) * sp, y + math.sin(a_) * sp, accent if sp < 3 else white, 0.95 - 0.2 * sp)
        canvas.plotf(x, y, white)
    elif style == "sigil":                                   # a burning hexagram
        a0 = -t * 5
        for k in range(6):
            a_ = a0 + k * math.tau / 6
            canvas.blendf(x + math.cos(a_) * 2.6, y + math.sin(a_) * 2.6, (255, 150, 40) if k % 2 else white, 0.95)
            canvas.blendf(x + math.cos(a_) * 1.4, y + math.sin(a_) * 1.4, (255, 214, 120), 0.7)
        canvas.plotf(x, y, white)
    elif style == "sandglass":                               # two triangles and a falling grain
        for dy in range(-3, 4):
            w = abs(dy) * 0.8
            for dx in range(-3, 4):
                if abs(dx) <= w and abs(dy) >= 1:
                    canvas.blendf(x + dx, y + dy, mix(accent, white, 0.4), 0.85)
        canvas.plotf(x, y + 1 - (t * 14) % 3, white)
    elif style == "void":
        for dx in range(-3, 4):
            for dy in range(-3, 4):
                d = math.hypot(dx, dy)
                if d <= 1.2:
                    canvas.blendf(x + dx, y + dy, shade(accent, 0.08), 1.0)
                elif d <= 2.2:
                    canvas.blendf(x + dx, y + dy, mix(accent, white, 0.25), 0.95)
                elif d <= 3.2:
                    canvas.blendf(x + dx, y + dy, shade(accent, 0.2), 0.3)
    elif style == "ring":
        r = 2.4 + 1.1 * abs(math.sin(t * 9))
        for dx in range(-4, 5):
            for dy in range(-4, 5):
                d = math.hypot(dx, dy)
                if abs(d - r) < 0.75:
                    canvas.blendf(x + dx, y + dy, mix(accent, white, 0.3), 0.9)
                elif d < r - 0.75:
                    canvas.blendf(x + dx, y + dy, accent, 0.18)
        canvas.plotf(x, y, white)
    elif style == "shard":
        px, py = -uy, ux
        for i in range(-3, 4):
            k = 1 - abs(i) / 4
            canvas.blendf(x + ux * i, y + uy * i, mix(accent, (225, 248, 255), k), 0.95)
            if abs(i) < 2:
                canvas.blendf(x + ux * i + px * 0.9, y + uy * i + py * 0.9, accent, 0.7)
                canvas.blendf(x + ux * i - px * 0.9, y + uy * i - py * 0.9, accent, 0.7)
        if int(t * 14) % 3 == 0:
            canvas.blendf(x, y + 2, white, 0.6)
    elif style == "sun":
        for dx in range(-3, 4):
            for dy in range(-3, 4):
                d = math.hypot(dx, dy)
                if d <= 1.6:
                    canvas.blendf(x + dx, y + dy, mix((255, 250, 210), white, 0.5), 1.0)
                elif d <= 3.2:
                    canvas.blendf(x + dx, y + dy, accent, 0.28)
        for k in range(8):
            a = k * math.pi / 4 + t * 7
            for s in (2.4, 3.4, 4.4 if k % 2 == 0 else 3.4):
                canvas.blendf(x + math.cos(a) * s, y + math.sin(a) * s, mix(accent, white, 0.5), 0.85 - (s - 2.4) * 0.15)
    elif style == "lava":
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                hot = _flick(t, dx + 3, dy + 7) > 0.45
                canvas.blendf(x + dx, y + dy, (255, 190, 70) if hot else (200, 60, 18), 0.95)
        canvas.plotf(x, y, (255, 240, 170))
        canvas.blendf(x - ux * 2.2, y - uy * 2.2 - 1, (255, 120, 30), 0.8)
        canvas.blendf(x - ux * 3.4, y - uy * 3.4 - 2, (170, 40, 12), 0.6)
        canvas.blendf(x + 1.5, y - 2.5 - (t * 14) % 2, (255, 150, 40), 0.7)
    elif style == "bolt":
        canvas.plotf(x, y, white)
        for dx, dy in nb:
            canvas.blendf(x + dx, y + dy, mix(accent, white, 0.5), 0.9)
        for k in range(4):                      # crackling arcs leaping off the orb
            ang = _flick(t, k, 4) * math.tau
            px_, py_ = x, y
            for s in range(1, 4):
                nx = x + math.cos(ang) * s * 1.5 + (_flick(t, k, s + 10) - 0.5) * 2
                ny = y + math.sin(ang) * s * 1.5 + (_flick(t, k, s + 20) - 0.5) * 2
                canvas.blendf(nx, ny, mix(accent, white, 0.7), 0.85 - s * 0.2)
                px_, py_ = nx, ny
    elif style == "alien":
        pulse = 0.5 + 0.5 * math.sin(t * 11)
        for dx in range(-3, 4):
            for dy in range(-3, 4):
                d = math.hypot(dx, dy)
                if d <= 1.5:
                    canvas.blendf(x + dx, y + dy, mix((170, 255, 190), white, 0.5 * pulse), 1.0)
                elif d <= 2.8:
                    canvas.blendf(x + dx, y + dy, (60, 230, 150), 0.35 + 0.25 * pulse)
        for k in range(2):
            a = t * 10 + k * math.pi
            canvas.blendf(x + math.cos(a) * 3.4, y + math.sin(a) * 3.4, (200, 120, 255), 0.95)
            canvas.blendf(x + math.cos(a - 0.5) * 3.4, y + math.sin(a - 0.5) * 3.4, (150, 80, 230), 0.5)
    elif style == "blessing":                                # MARTYR'S BLESSING: a golden eight-point star, a ticking clock-hand, orbiting violet stars
        for k in range(8):
            a_ = t * 5 + k * math.pi / 4
            ln = 5 if k % 2 == 0 else 3
            for s in range(1, ln + 1):
                canvas.blendf(x + math.cos(a_) * s, y + math.sin(a_) * s, (255, 226, 120) if k % 2 == 0 else (255, 255, 255), 0.95 - s * 0.12)
        for dx in range(-2, 3):
            for dy in range(-2, 3):
                if math.hypot(dx, dy) <= 2.2:
                    canvas.blendf(x + dx, y + dy, (255, 250, 220), 0.9)
        for s in range(2, 8):
            canvas.blendf(x + math.cos(t * 3) * s, y + math.sin(t * 3) * s, (255, 200, 80), 0.9 - s * 0.08)
        for k in range(3):
            a_ = -t * 6 + k * math.tau / 3
            canvas.blendf(x + math.cos(a_) * 6.5, y + math.sin(a_) * 6.5, (176, 120, 255), 0.95)
    elif style == "phase":                                   # PHASE SHIFT: a plain flickering ghost that blinks between two positions
        flick = int(t * 18) % 2
        canvas.blendf(x, y, (170, 255, 200), 0.95 if flick else 0.5)
        canvas.blendf(x - ux * 3, y - uy * 3, (190, 130, 255), 0.45 if flick else 0.9)
        canvas.plotf(x, y, white)
    elif style == "xenon":                                   # XENON: a caged star - white core, 2 counter-rotating ion orbits, cross-flares
        pulse = 0.5 + 0.5 * math.sin(t * 9)
        for dx in range(-4, 5):
            for dy in range(-4, 5):
                d = math.hypot(dx, dy)
                if d <= 1.6:
                    canvas.blendf(x + dx, y + dy, mix((235, 255, 245), white, 0.6 * pulse), 1.0)
                elif d <= 3.6:
                    canvas.blendf(x + dx, y + dy, mix((60, 235, 160), (120, 80, 230), (d - 1.6) / 2.0), 0.3 + 0.25 * pulse)
        for k in range(6):                                   # outer orbit
            a_ = t * 8 + k * math.tau / 6
            canvas.blendf(x + math.cos(a_) * 4.6, y + math.sin(a_) * 4.6, (210, 140, 255) if k % 2 else (150, 255, 210), 0.95)
            canvas.blendf(x + math.cos(a_ - 0.35) * 4.6, y + math.sin(a_ - 0.35) * 4.6, (120, 70, 220), 0.5)
        for k in range(4):                                   # inner orbit, opposite way
            a_ = -t * 12 + k * math.pi / 2
            canvas.blendf(x + math.cos(a_) * 2.6, y + math.sin(a_) * 2.6, white, 0.9)
        for k in range(4):                                   # pulsing cross-flares
            a_ = math.pi / 4 + k * math.pi / 2 + t * 1.5
            for s in range(3, 3 + int(3 + 3 * pulse)):
                canvas.blendf(x + math.cos(a_) * s, y + math.sin(a_) * s, (180, 255, 220), 0.55 - s * 0.05)
    elif style == "helix":
        px, py = -uy, ux
        for k in range(8):
            ph, s = t * 14 - k * 0.9, k * 1.3
            off = math.sin(ph) * 2.2
            col, a = hsv(t * 0.8 + k * 0.1, 0.6, 1.0), 0.95 - k * 0.1
            canvas.blendf(x - ux * s + px * off, y - uy * s + py * off, col, a)
            canvas.blendf(x - ux * s - px * off, y - uy * s - py * off, mix(col, white, 0.4), a)
        canvas.plotf(x, y, white)
    elif style == "glyph":
        k = int(t * 12)
        draw_glyph(canvas, x, y, k, mix((90, 255, 130), white, 0.35), 1.0)
        draw_glyph(canvas, x - ux * 5, y - uy * 5 + 1, k + 3, (40, 190, 80), 0.45)
    elif style == "flame":
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                if _flick(t, dx + 5, dy + 5) > 0.25:
                    canvas.blendf(x + dx, y + dy, gradient(((0, (255, 236, 150)), (1, mix((255, 100, 20), accent, 0.3))),
                                                          _flick(t, dx, dy)), 0.95)
        for k in range(1, 3):
            canvas.blendf(x + (_flick(t, k, 9) - 0.5), y + k + 1, (255, 150, 40), 0.7 / k)
        canvas.plotf(x, y, (255, 250, 220))
    else:   # standard shell
        canvas.plotf(x, y, white)
        for dx, dy in nb:
            canvas.blendf(x + dx, y + dy, accent, 0.75)


# ============================================================================
# Particles & explosions
# ============================================================================
@dataclass(slots=True)
class Particle:
    x: float
    y: float
    vx: float
    vy: float
    life: float
    max_life: float
    c0: RGB
    c1: RGB
    gravity: float = 0.0
    drag: float = 0.0
    alpha: float = 1.0
    solid: bool = False
    size: int = 1
    windk: float = 0.0


class ParticleSystem:
    def __init__(self, cap: int = 2600) -> None:
        self.items: list[Particle] = []
        self.cap = cap
        self.wind = 0.0

    def emit(self, p: Particle) -> None:
        if len(self.items) < self.cap:
            self.items.append(p)

    def update(self, dt: float, terrain: Optional[Terrain] = None) -> None:
        alive = []
        for p in self.items:
            p.life -= dt
            if p.life <= 0:
                continue
            if p.drag:
                k = max(0.0, 1.0 - p.drag * dt)
                p.vx *= k
                p.vy *= k
            p.vy -= p.gravity * dt
            if p.windk:
                p.vx += self.wind * p.windk * dt
            p.x += p.vx * dt
            p.y += p.vy * dt
            if p.solid and terrain is not None:
                xi = int(p.x)
                if p.y < 0 or (0 <= xi < terrain.width and p.y < terrain.heights[xi]):
                    continue
            alive.append(p)
        self.items = alive

    def draw(self, canvas: PixelCanvas) -> None:
        for p in self.items:
            t = 1 - p.life / p.max_life
            col = mix(p.c0, p.c1, t)
            a = p.alpha * (1 - t)
            x, y = int(p.x // 1), int(p.y // 1)
            canvas.blend(x, y, col, a)
            if p.size > 1:
                for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    canvas.blend(x + dx, y + dy, col, a * 0.5)


class Explosion:
    """Expanding fireball with radial gradient, flash core and shockwave; `style` is the ammo's blast look."""

    def __init__(self, x: float, y: float, radius: float, accent: RGB, duration: float = 0.95,
                 style: str = "classic") -> None:
        self.x, self.y, self.radius, self.duration, self.age = x, y, radius, duration, 0.0
        self.accent, self.style = accent, style
        if style == "implode":
            stops = [(0.0, shade(accent, 0.12)), (0.45, shade(accent, 0.35)), (0.8, accent), (1.0, mix(accent, (255, 255, 255), 0.6))]
        elif style == "fire":
            stops = [(0.0, (255, 250, 220)), (0.25, (255, 214, 110)), (0.55, mix((255, 110, 24), accent, 0.25)),
                     (0.85, (150, 32, 10)), (1.0, (50, 14, 8))]
        elif style == "magma":
            stops = [(0.0, (255, 244, 190)), (0.25, (255, 176, 60)), (0.55, (226, 70, 18)), (0.85, (110, 22, 10)), (1.0, (34, 10, 8))]
        elif style == "nova":
            stops = [(0.0, (255, 255, 255)), (0.3, (255, 252, 214)), (0.55, mix((255, 200, 90), accent, 0.3)),
                     (0.85, (255, 110, 34)), (1.0, (80, 22, 12))]
        elif style == "sigil":
            stops = [(0.0, (255, 252, 230)), (0.3, (255, 226, 130)), (0.6, (230, 150, 40)), (0.85, (120, 60, 20)), (1.0, (30, 16, 8))]
        elif style == "eclipse":
            stops = [(0.0, (8, 3, 18)), (0.45, (56, 20, 110)), (0.75, (255, 200, 90)), (1.0, (255, 250, 222))]
        elif style == "tidal":
            stops = [(0.0, (240, 255, 255)), (0.3, (140, 230, 255)), (0.6, (30, 120, 200)), (0.85, (10, 40, 110)), (1.0, (4, 14, 40))]
        elif style == "cinderstorm":
            stops = [(0.0, (255, 240, 200)), (0.3, (255, 150, 50)), (0.6, (180, 40, 16)), (0.85, (60, 14, 10)), (1.0, (16, 6, 6))]
        elif style == "rewind":
            stops = [(0.0, (255, 255, 250)), (0.35, (255, 226, 150)), (0.65, (150, 100, 240)), (0.9, (50, 20, 110)), (1.0, (14, 6, 40))]
        elif style == "code":
            stops = [(0.0, (230, 255, 235)), (0.3, (120, 255, 150)), (0.6, (30, 170, 70)), (0.85, (10, 70, 30)), (1.0, (4, 24, 12))]
        elif style == "blessing":
            stops = [(0.0, (255, 255, 255)), (0.25, (255, 226, 120)), (0.6, (176, 120, 255)), (1.0, (30, 10, 70))]
        elif style == "xenon":
            stops = [(0.0, (255, 255, 255)), (0.2, (150, 255, 215)), (0.5, (150, 100, 255)), (0.8, (70, 30, 150)), (1.0, (14, 6, 40))]
        elif style == "alien":
            stops = [(0.0, (235, 255, 240)), (0.25, (130, 255, 190)), (0.55, (80, 90, 220)), (0.85, (60, 20, 120)), (1.0, (16, 6, 36))]
        else:
            stops = [(0.0, (255, 252, 238)), (0.28, mix((255, 255, 255), accent, 0.35)), (0.55, accent),
                     (0.8, shade(accent, 0.5)), (1.0, shade(accent, 0.2))]
        self.palette = [gradient(stops, i / 15) for i in range(16)]

    @property
    def done(self) -> bool:
        return self.age >= self.duration

    @property
    def progress(self) -> float:
        return self.age / self.duration

    def update(self, dt: float) -> None:
        self.age += dt

    def draw(self, canvas: PixelCanvas) -> None:
        p = self.progress
        if p >= 1:
            return
        R, style = self.radius, self.style
        grow = max(0.0, (p - 0.1) / 0.9) if style == "implode" else p
        rr = R * ease_out(grow / 0.25) if grow < 0.25 else R * (1 - 0.45 * (grow - 0.25) / 0.75)
        alpha = 1.0 if p < 0.5 else 1 - (p - 0.5) / 0.5
        span = int(max(rr, R * 2.2)) + 2
        cx, cy = int(self.x), int(self.y)
        ring = R * (0.4 + 1.6 * ease_out(p))
        flash = R * 1.9 * (1 - p / 0.12) if (p < 0.12 and style != "implode") else 0.0
        pull = R * (2.4 - 12 * p) if (style == "implode" and p < 0.2) else 0.0
        for yy in range(cy - span, cy + span + 1):
            for xx in range(cx - span, cx + span + 1):
                dx, dy = xx + 0.5 - self.x, yy + 0.5 - self.y
                d = math.hypot(dx, dy)
                if flash and d <= flash:
                    canvas.blend(xx, yy, (255, 255, 250), 0.75 * (1 - d / flash * 0.5))
                if pull and abs(d - pull) < 0.8:
                    canvas.blend(xx, yy, mix(self.accent, (255, 255, 255), 0.4), 0.8)
                if d <= rr:
                    f = min(1.0, d / max(rr, 0.1) + p * 0.55)
                    col = self.palette[int(f * 15)]
                    if style in ("prism", "bloom"):
                        col = mix(col, hsv(math.atan2(dy, dx) / math.tau + p * 0.7 + d * 0.03, 0.6, 1.0), 0.55)
                    canvas.blend(xx, yy, col, alpha * (1 - d / max(rr, 0.1) * 0.25))
                if abs(d - ring) < 0.75 and p < 0.9 and style != "implode":
                    canvas.blend(xx, yy, self.palette[3], (1 - p) * 0.55)
        if style in ("rays", "starrays", "prism"):
            self._rays(canvas, p)
        elif style == "cluster":
            self._cluster(canvas, p)
        elif style == "fire":
            self._plume(canvas, p)
        elif style == "ring":
            self._ring2(canvas, p)
        elif style == "cloud":
            self._cloud(canvas, p)
        elif style == "shock":
            self._shock(canvas, p)
        elif style == "shatter":
            self._shatter(canvas, p)
        elif style == "nova":
            self._nova(canvas, p)
        elif style == "magma":
            self._magma(canvas, p)
        elif style == "thunder":
            self._forks(canvas, p)
        elif style == "code":
            self._code(canvas, p)
        elif style == "bloom":
            self._bloom(canvas, p)
        elif style in ("sigil", "eclipse", "tidal", "cinderstorm", "rewind"):
            self._ancient(canvas, p)
        elif style == "blessing":
            self._blessing(canvas, p)
        elif style == "xenon":
            self._xenon(canvas, p)
        elif style == "alien":
            for k in range(3):
                q = clamp((p - k * 0.14) / 0.7, 0, 1)
                if 0 < q < 1:
                    self._ringline(canvas, self.radius * (0.5 + 3.0 * ease_out(q)), mix((120, 255, 190), (190, 120, 255), k / 2), (1 - q) * 0.85)

    def _blessing(self, canvas: PixelCanvas, p: float) -> None:        # BLESSING burst: white core, an 8-point golden star, 3 rings, orbiting stars
        R = self.radius
        q = ease_out(clamp(p / 0.6, 0, 1))
        if p < 0.3:
            for dx in range(-int(R), int(R) + 1):
                for dy in range(-int(R), int(R) + 1):
                    if math.hypot(dx, dy) <= R * (0.4 + 1.6 * p):
                        canvas.blendf(self.x + dx, self.y + dy, (255, 252, 235), (1 - p / 0.3) * 0.95)
        for k in range(8):
            a_ = k * math.pi / 4 + p * 1.2
            ln = R * (3.4 if k % 2 == 0 else 1.9) * q
            for s in range(1, int(ln) + 1):
                canvas.blendf(self.x + math.cos(a_) * s, self.y + math.sin(a_) * s, (255, 226, 120) if k % 2 == 0 else (255, 255, 255), (1 - p) * (0.95 - 0.4 * s / max(1, ln)))
        for k, col in enumerate(((255, 214, 90), (255, 255, 255), (176, 120, 255))):
            qq = clamp((p - k * 0.1) / 0.7, 0, 1)
            if 0 < qq < 1:
                self._ringline(canvas, R * (0.5 + 3.0 * ease_out(qq)), col, (1 - qq) * 0.9)
        for k in range(10):
            a_ = -p * 5 + k * math.tau / 10
            rr = R * (1.2 + 2.2 * q)
            canvas.blendf(self.x + math.cos(a_) * rr, self.y + math.sin(a_) * rr, (255, 240, 170) if k % 2 else (176, 120, 255), (1 - p) * 0.9)

    def _xenon(self, canvas: PixelCanvas, p: float) -> None:           # XENON bloom: white flash, 5 staggered rings, a rotating 12-spoke crystal, hex star
        R = self.radius
        if p < 0.25:
            for dx in range(-int(R), int(R) + 1):
                for dy in range(-int(R), int(R) + 1):
                    d = math.hypot(dx, dy)
                    if d <= R * (0.5 + 2 * p):
                        canvas.blendf(self.x + dx, self.y + dy, mix((255, 255, 255), (150, 255, 215), d / (R * 1.5)), (1 - p / 0.25) * 0.9)
        for k in range(5):
            q = clamp((p - k * 0.1) / 0.65, 0, 1)
            if 0 < q < 1:
                self._ringline(canvas, R * (0.4 + 3.4 * ease_out(q)), mix((120, 255, 190), (190, 120, 255), k / 4), (1 - q) * 0.9)
        if p < 0.8:
            q = p / 0.8
            for k in range(12):
                a_ = k * math.tau / 12 + p * 3
                ln = R * (0.5 + 3.2 * ease_out(q)) * (1.0 if k % 2 else 0.6)
                for s in range(2, int(ln) + 1, 2):
                    canvas.blendf(self.x + math.cos(a_) * s, self.y + math.sin(a_) * s, (230, 255, 240) if s > ln - 3 else (130, 255, 200), (1 - q) * 0.85)
            for k in range(6):                               # hex star core
                a_ = k * math.pi / 3 - p * 2
                for s in range(1, int(R * 0.9)):
                    canvas.blendf(self.x + math.cos(a_) * s, self.y + math.sin(a_) * s, (255, 255, 255), (1 - q) * 0.9)

    def _ancient(self, canvas: PixelCanvas, p: float) -> None:          # the Ancient Artifact blasts: rings, rays and rune geometry
        st, R = self.style, self.radius
        gold, white = (255, 226, 130), (255, 255, 255)
        if st == "sigil":                                                 # a runic circle with a rotating hexagram
            r0 = R * (0.9 + 1.7 * ease_out(p))
            self._ringline(canvas, r0, gold, (1 - p) * 0.9)
            self._ringline(canvas, r0 * 0.72, white, (1 - p) * 0.5)
            for k in range(6):
                a = k * math.tau / 6 + p * 2.4
                for j in range(int(r0)):
                    canvas.blendf(self.x + math.cos(a) * j, self.y + math.sin(a) * j, gold, (1 - p) * 0.7)
        elif st == "eclipse":                                             # dark disc with a flaring corona
            for k in range(16):
                a = k * math.tau / 16 + p * 0.8
                ln = R * (1.0 + 2.6 * ease_out(p)) * (0.6 + 0.4 * math.sin(k * 3.1))
                for j in range(int(ln)):
                    canvas.blendf(self.x + math.cos(a) * (R * 0.6 + j), self.y + math.sin(a) * (R * 0.6 + j), gold, (1 - p) * 0.8 * (1 - j / max(1, ln)))
            self._ringline(canvas, R * (0.7 + 2.0 * ease_out(p)), (170, 110, 255), (1 - p) * 0.7)
        elif st == "tidal":                                               # staggered water rings
            for k in range(4):
                q = clamp((p - k * 0.1) / 0.7, 0, 1)
                if 0 < q < 1:
                    self._ringline(canvas, R * (0.5 + 3.2 * ease_out(q)), mix((120, 220, 255), white, 0.3 * k / 3), (1 - q) * 0.85)
        elif st == "cinderstorm":                                         # sparks flung out on arcs
            for k in range(18):
                a = k * math.tau / 18 + 0.3
                s = R * (0.6 + 2.8 * ease_out(p)) * (0.7 + 0.3 * math.sin(k * 2.3))
                canvas.blendf(self.x + math.cos(a) * s, self.y + math.sin(a) * s - 5 * p * p * R * 0.4, (255, 190, 90), (1 - p) * 0.9)
            self._ringline(canvas, R * (0.5 + 1.8 * ease_out(p)), (255, 120, 40), (1 - p) * 0.6)
        else:                                                             # rewind: rings fall back into the blast
            for k in range(3):
                q = clamp(1 - (p - k * 0.1) / 0.8, 0, 1)
                if 0 < q < 1:
                    self._ringline(canvas, R * (0.4 + 2.6 * q), mix((255, 226, 150), (170, 110, 255), k / 2), min(1.0, 1.6 * q) * 0.8)
            for k in range(8):
                a = k * math.tau / 8 - p * 3
                ln = R * 2.4 * (1 - p)
                for j in range(int(ln)):
                    canvas.blendf(self.x + math.cos(a) * (ln - j), self.y + math.sin(a) * (ln - j), white, (1 - p) * 0.5)

    def _bloom(self, canvas: PixelCanvas, p: float) -> None:            # eight prismatic petals unfolding
        reach = self.radius * (1.0 + 2.6 * ease_out(p))
        for k in range(8):
            a = k * math.tau / 8 + p * 0.7
            ca, sa = math.cos(a), math.sin(a)
            col = hsv(k / 8 + p * 0.6, 0.55, 1.0)
            for s in range(1, int(reach) + 1):
                w = math.sin(s / reach * math.pi) * (1.2 + 2.2 * p)
                for side in (-1, 1):
                    canvas.blendf(self.x + ca * s - sa * w * side, self.y + sa * s + ca * w * side, col, (1 - p) * 0.85)
        self._ringline(canvas, self.radius * (0.5 + 2.0 * ease_out(p)), (255, 255, 255), (1 - p) * 0.5)

    def _ringline(self, canvas: PixelCanvas, r: float, col: RGB, a: float) -> None:
        for k in range(int(math.tau * r * 1.5) + 1):
            ang = k / max(1.0, r * 1.5)
            canvas.blendf(self.x + math.cos(ang) * r, self.y + math.sin(ang) * r, col, a)

    def _shock(self, canvas: PixelCanvas, p: float) -> None:            # three staggered shockwaves
        for k in range(3):
            q = clamp((p - k * 0.12) / 0.7, 0, 1)
            if 0 < q < 1:
                self._ringline(canvas, self.radius * (0.6 + 2.8 * ease_out(q)), mix(self.accent, (255, 255, 255), 0.5), (1 - q) * 0.8)

    def _shatter(self, canvas: PixelCanvas, p: float) -> None:          # ice crystals flung outward
        for k in range(14):
            a = k * math.tau / 14 + 0.2
            d0 = self.radius * (0.5 + 2.2 * ease_out(p)) * (0.7 + 0.5 * ((k * 7) % 5) / 4)
            for s in range(3):
                canvas.blendf(self.x + math.cos(a) * (d0 - s), self.y + math.sin(a) * (d0 - s),
                              mix((235, 250, 255), self.accent, s / 3), (1 - p) * (0.95 - s * 0.25))

    def _nova(self, canvas: PixelCanvas, p: float) -> None:             # white flash, double ring, long rays
        if p < 0.25:
            fr = self.radius * 2.4 * (1 - p / 0.25)
            for yy in range(int(self.y - fr) - 1, int(self.y + fr) + 2):
                for xx in range(int(self.x - fr) - 1, int(self.x + fr) + 2):
                    d = math.hypot(xx + 0.5 - self.x, yy + 0.5 - self.y)
                    if d <= fr:
                        canvas.blend(xx, yy, (255, 255, 245), 0.5 * (1 - d / fr))
        self._ringline(canvas, self.radius * (0.7 + 2.2 * ease_out(p)), (255, 244, 200), (1 - p) * 0.8)
        self._ringline(canvas, self.radius * (0.4 + 3.4 * ease_out(p)), self.accent, (1 - p) * 0.5)
        for k in range(12):
            a = k * math.tau / 12 + p * 0.5
            for s in range(1, int(self.radius * (1.4 + 2.2 * ease_out(p))) + 1):
                canvas.blendf(self.x + math.cos(a) * s, self.y + math.sin(a) * s, (255, 240, 190), (1 - p) * 0.7 * (1 - s / (self.radius * 3.6)))

    def _magma(self, canvas: PixelCanvas, p: float) -> None:            # lava globs thrown up in arcs + glowing pool
        R = self.radius
        for k in range(9):
            a = 0.35 + k * (math.pi - 0.7) / 8
            v, tau = R * (2.2 + 0.5 * ((k * 5) % 3)), p * 1.6
            gx, gy = self.x + math.cos(a) * v * tau * 0.9, self.y + math.sin(a) * v * tau - 4.5 * R * tau * tau
            if p < 0.85:
                for dx, dy in ((0, 0), (1, 0), (0, 1), (1, 1)):
                    canvas.blendf(gx + dx, gy + dy, (255, int(170 - 60 * p), 50), 0.9 * (1 - p))
        if p < 0.9:
            for dx in range(-int(R * 1.3), int(R * 1.3) + 1):
                canvas.blendf(self.x + dx, self.y - 1, (255, 120, 30), 0.45 * (1 - p) * (1 - abs(dx) / (R * 1.4)))

    def _forks(self, canvas: PixelCanvas, p: float) -> None:            # lightning forking out of the blast
        if p > 0.75:
            return
        R, step = self.radius, int(self.age * 22)
        for k in range(7):
            a = k * math.tau / 7 + 0.3
            x, y = self.x, self.y
            for s in range(int(R * 1.8)):
                a2 = a + (_flick(step * 0.033, k, s) - 0.5) * 1.1
                x, y = x + math.cos(a2) * 1.6, y + math.sin(a2) * 1.6
                canvas.blendf(x, y, (235, 245, 255), (1 - p / 0.75) * 0.95)
                if s % 3 == 0:
                    canvas.blendf(x + 1, y, self.accent, (1 - p / 0.75) * 0.4)

    def _code(self, canvas: PixelCanvas, p: float) -> None:             # a spray of ASCII glyphs
        R = self.radius
        for k in range(10):
            a = k * math.tau / 10 + 0.4
            d = R * (0.4 + 2.4 * ease_out(p))
            draw_glyph(canvas, self.x + math.cos(a) * d, self.y + math.sin(a) * d * 0.9, int(self.age * 10) + k,
                       (110, 255, 150), (1 - p) * 0.9)

    def _rays(self, canvas: PixelCanvas, p: float) -> None:
        n = {"rays": 8, "starrays": 12, "prism": 16}[self.style]
        length = self.radius * (1.1 + (1.0 if self.style == "rays" else 1.7) * ease_out(p))
        for k in range(n):
            a = k * math.tau / n + (0.3 if self.style == "rays" else p * 0.8 if self.style == "prism" else 0.0)
            ca, sa = math.cos(a), math.sin(a)
            col = hsv(k / n + p, 0.6, 1.0) if self.style == "prism" else mix((255, 255, 255), self.accent, 0.4)
            for s in range(1, int(length) + 1):
                canvas.blendf(self.x + ca * s, self.y + sa * s, col, (1 - p) * (1 - s / length) * 0.85)

    def _cluster(self, canvas: PixelCanvas, p: float) -> None:
        R = self.radius
        for k in range(6):
            a = k * math.tau / 6 + 0.4
            local = clamp((p - 0.12 - k * 0.05) / 0.5, 0, 1)
            if 0 < local < 1:
                r = R * 0.45 * math.sin(local * math.pi)
                bx, by = self.x + math.cos(a) * R * 1.45, self.y + math.sin(a) * R * 1.45
                for yy in range(int(by - r) - 1, int(by + r) + 2):
                    for xx in range(int(bx - r) - 1, int(bx + r) + 2):
                        d = math.hypot(xx + 0.5 - bx, yy + 0.5 - by)
                        if d <= r:
                            canvas.blend(xx, yy, mix((255, 255, 255), self.palette[2], d / max(r, 0.1)), (1 - local) * 0.9)

    def _plume(self, canvas: PixelCanvas, p: float) -> None:
        if p >= 0.8:
            return
        R = self.radius
        cols = int(R * 1.2)
        for dx in range(-cols, cols + 1):
            height = R * (1.0 + 1.4 * _flick(self.age, dx, 7)) * (1 - p / 0.8) * (1 - abs(dx) / (cols + 1))
            for dy in range(int(height)):
                col = gradient(((0, (255, 230, 140)), (0.5, (255, 120, 24)), (1, (120, 24, 8))), dy / max(1.0, height))
                canvas.blendf(self.x + dx, self.y + dy + 1, col, 0.8 * (1 - p))

    def _ring2(self, canvas: PixelCanvas, p: float) -> None:
        r = self.radius * (0.8 + 2.4 * ease_out(p))
        for k in range(int(math.tau * r * 1.6) + 1):
            a = k / max(1.0, r * 1.6)
            canvas.blendf(self.x + math.cos(a) * r, self.y + math.sin(a) * r, self.palette[2], (1 - p) * 0.7)

    def _cloud(self, canvas: PixelCanvas, p: float) -> None:
        r = self.radius * (1.0 + 1.2 * ease_out(p))
        col = mix(self.accent, (60, 90, 40), 0.4)
        for yy in range(int(self.y - r) - 1, int(self.y + r) + 2):
            for xx in range(int(self.x - r) - 1, int(self.x + r) + 2):
                if math.hypot(xx + 0.5 - self.x, yy + 0.5 - self.y) <= r:
                    canvas.blend(xx, yy, col, 0.22 * (1 - p))


class EffectsFactory:
    """Spawns the particle mixes behind muzzle flashes, trails, impacts, ambient flair and wrecks."""

    def __init__(self, particles: ParticleSystem, settings: Settings, rng: random.Random) -> None:
        self.ps, self.settings, self.rng = particles, settings, rng

    def _n(self, base: float) -> int:
        return max(1, int(base * self.settings.particle_density))

    def _burst(self, x: float, y: float, n: int, speed: tuple, life: tuple, c0, c1, gravity: float = 60.0,
               drag: float = 0.4, size: int = 1, alpha: float = 1.0, spread: tuple = (0.1, math.pi - 0.1),
               solid: bool = False, windk: float = 0.0) -> None:
        r = self.rng
        for _ in range(n):
            a, s, life_ = r.uniform(*spread), r.uniform(*speed), r.uniform(*life)
            ca, cb = (c0(), c1()) if callable(c0) else (c0, c1)
            self.ps.emit(Particle(x, y, math.cos(a) * s, math.sin(a) * s, life_, life_, ca, cb, gravity=gravity,
                                  drag=drag, alpha=alpha, size=size, solid=solid, windk=windk))

    def trail(self, x: float, y: float, accent: RGB, ammo: Optional[AmmoType] = None, n: int = 0, t: float = 0.0) -> None:
        life = self.settings.trail_life
        if life <= 0:
            return
        r, ps = self.rng, self.ps
        l = life * r.uniform(0.7, 1.0)
        white = (255, 255, 255)
        style = ammo.trail if ammo else "plain"
        plain = Particle(x, y, 0, 0, l, l, mix(white, accent, 0.5), shade(accent, 0.15), alpha=0.9)
        if style == "plain":
            ps.emit(plain)
        elif style == "dash":
            if n % 4 < 2:
                ps.emit(Particle(x, y, 0, 0, l * 0.8, l * 0.8, white, shade(accent, 0.3)))
        elif style == "glow":
            ps.emit(Particle(x, y, 0, 0, l * 1.1, l * 1.1, mix(accent, white, 0.35), shade(accent, 0.1), alpha=0.55, size=2))
        elif style == "sparks":
            ps.emit(plain)
            if r.random() < 0.5:
                ps.emit(Particle(x, y, r.uniform(-12, 12), r.uniform(-6, 14), 0.5, 0.5, white, shade(accent, 0.3), gravity=60))
        elif style == "embers":
            ps.emit(Particle(x, y, 0, 0, l * 1.4, l * 1.4, mix((255, 200, 120), accent, 0.4), (120, 30, 10), alpha=0.9))
            if r.random() < 0.4:
                ps.emit(Particle(x, y, r.uniform(-4, 4), r.uniform(3, 9), l, l, (255, 190, 100), (90, 20, 8), gravity=-6))
        elif style == "drip":
            ps.emit(Particle(x, y, 0, 0, l, l, mix(accent, white, 0.15), shade(accent, 0.2), alpha=0.9))
            if r.random() < 0.35:
                ps.emit(Particle(x, y, r.uniform(-2, 2), -8, 0.7, 0.7, accent, shade(accent, 0.2), gravity=40))
        elif style == "twinkle":
            c = white if r.random() < 0.5 else accent
            ps.emit(Particle(x + r.uniform(-0.7, 0.7), y + r.uniform(-0.7, 0.7), 0, 0, l * 0.9, l * 0.9, c, shade(accent, 0.2)))
        elif style == "runes":
            ps.emit(Particle(x + r.uniform(-1.2, 1.2), y + r.uniform(-1.2, 1.2), 0, r.uniform(0, 3), l, l,
                             (255, 240, 170) if r.random() < 0.5 else accent, (150, 100, 30), alpha=0.9))
            if r.random() < 0.25:
                ps.emit(Particle(x, y, 0, 0, l * 0.5, l * 0.5, white, accent, size=2, alpha=0.7))
        elif style == "aeon":
            for sgn in (-1, 1):
                ps.emit(Particle(x + sgn * math.sin(t * 14) * 1.6, y, 0, 0, l, l,
                                 (255, 250, 225) if sgn > 0 else (170, 110, 255), (30, 10, 60), alpha=0.8))
        elif style == "tides":
            ps.emit(Particle(x, y, r.uniform(-4, 4), r.uniform(2, 8), l, l, mix((190, 240, 255), accent, 0.3), (10, 60, 110), gravity=40, alpha=0.9))
            if r.random() < 0.3:
                ps.emit(Particle(x, y, 0, 0, l * 0.8, l * 0.8, (230, 250, 255), accent, size=2, alpha=0.5))
        elif style == "cinders":
            ps.emit(Particle(x + r.uniform(-0.8, 0.8), y, r.uniform(-4, 4), r.uniform(2, 12), l * 0.9, l * 0.9, (255, 200, 100), (60, 20, 10),
                             gravity=-12, alpha=0.9, size=2 if r.random() < 0.3 else 1))
            if r.random() < 0.4:
                ps.emit(Particle(x, y, r.uniform(-5, 5), r.uniform(-5, 5), 0.35, 0.35, (44, 22, 22), (10, 5, 5), alpha=0.8))
        elif style == "chrono":
            if n % 3 == 0:
                ps.emit(Particle(x, y, 0, 0, l * 1.4, l * 1.4, (255, 236, 170), (120, 80, 30), size=2, alpha=0.55))
            ps.emit(Particle(x, y, 0, 0, l * 0.6, l * 0.6, white, accent, alpha=0.9))
        elif style == "mist":
            ps.emit(Particle(x, y, r.uniform(-3, 3), r.uniform(-3, 3), l * 1.2, l * 1.2, shade(accent, 0.25), (8, 4, 16), alpha=0.75, size=2))
            if r.random() < 0.3:
                ps.emit(Particle(x, y, 0, 0, l * 0.6, l * 0.6, mix(accent, white, 0.3), shade(accent, 0.2)))
        elif style == "fire":
            ps.emit(Particle(x + r.uniform(-0.6, 0.6), y, r.uniform(-3, 3), r.uniform(4, 14), l * 0.8, l * 0.8,
                             mix((255, 236, 150), accent, 0.3), (120, 26, 8), gravity=-20, alpha=0.9,
                             size=2 if r.random() < 0.4 else 1))
        elif style == "ripple":
            for sgn in (-1, 1):
                ps.emit(Particle(x + sgn * 0.8, y, sgn * 3.5, 0, l, l, mix(accent, white, 0.4), shade(accent, 0.15), alpha=0.6, drag=1.2))
        elif style == "frost":
            ps.emit(Particle(x + r.uniform(-1, 1), y + r.uniform(-1, 1), r.uniform(-2, 2), -4, l * 1.1, l * 1.1,
                             mix((235, 250, 255), accent, 0.3), shade(accent, 0.2), alpha=0.9, gravity=10))
            if r.random() < 0.35:
                ps.emit(Particle(x, y, 0, 0, l * 0.7, l * 0.7, white, accent, size=2, alpha=0.6))
        elif style == "solar":
            ps.emit(Particle(x, y, 0, 0, l * 0.9, l * 0.9, (255, 250, 210), (255, 130, 30), alpha=0.95, size=2))
            a_ = r.uniform(0, math.tau)
            ps.emit(Particle(x, y, math.cos(a_) * 9, math.sin(a_) * 9, 0.4, 0.4, white, (255, 150, 40), drag=2.0))
        elif style == "lava":
            ps.emit(Particle(x, y, r.uniform(-3, 3), 0, l * 0.9, l * 0.9, (255, 170, 60), (70, 16, 8), alpha=0.95, gravity=70,
                             size=2 if r.random() < 0.3 else 1))
        elif style == "arc":
            for _ in range(2):
                ps.emit(Particle(x + r.uniform(-2.6, 2.6), y + r.uniform(-2.6, 2.6), 0, 0, r.uniform(0.1, 0.22), 0.22,
                                 (235, 245, 255), shade(accent, 0.3)))
            ps.emit(plain)
        elif style == "blessing":                            # gold stardust and violet sparks, with a white flare at the core
            for k in range(2):
                a_ = r.uniform(0, math.tau)
                ps.emit(Particle(x, y, math.cos(a_) * 10, math.sin(a_) * 10, l * 1.4, l * 1.4, (255, 220, 110), (120, 60, 200), alpha=0.95, drag=1.6))
            ps.emit(Particle(x, y, 0, 0, l * 1.1, l * 1.1, (255, 255, 255), (255, 200, 80), size=2, alpha=0.8))
        elif style == "xenon":                               # comet of ions: green-white core motes, violet sparks, rare white flare
            for k in range(2):
                a_ = r.uniform(0, math.tau)
                ps.emit(Particle(x, y, math.cos(a_) * 9, math.sin(a_) * 9, l * 1.3, l * 1.3, (160, 255, 215), (110, 50, 210), alpha=0.9, drag=1.8))
            ps.emit(Particle(x, y, 0, 0, l, l, (255, 255, 255), (90, 60, 200), size=2, alpha=0.7))
            if r.random() < 0.25:
                ps.emit(Particle(x, y, r.uniform(-14, 14), r.uniform(-14, 14), l * 0.7, l * 0.7, (230, 170, 255), (60, 20, 120), alpha=0.9, drag=2.0))
        elif style == "alien":
            a_ = r.uniform(0, math.tau)
            ps.emit(Particle(x, y, math.cos(a_) * 6, math.sin(a_) * 6 - 2, l * 1.2, l * 1.2, (150, 255, 190), (120, 60, 200), alpha=0.85,
                             drag=1.5))
            if r.random() < 0.3:
                ps.emit(Particle(x, y, 0, 0, l * 0.8, l * 0.8, (210, 150, 255), (60, 20, 120), size=2, alpha=0.6))
        elif style == "code":
            ps.emit(Particle(x, y, 0, -10, l * 1.1, l * 1.1, (150, 255, 175), (8, 56, 22), alpha=0.9))
            if r.random() < 0.4:
                ps.emit(Particle(x + r.uniform(-1, 1), y + 1, 0, -14, l, l, (80, 220, 110), (6, 40, 16), alpha=0.7))
        elif style == "prism":
            c = hsv(t * 0.8 + n * 0.05, 0.65, 1.0)
            ps.emit(Particle(x, y, 0, 0, l * 1.1, l * 1.1, c, shade(c, 0.15), alpha=0.95))
            if r.random() < 0.4:
                ps.emit(Particle(x + r.uniform(-1.5, 1.5), y + r.uniform(-1.5, 1.5), 0, 0, l * 0.6, l * 0.6, white, c))
        else:
            ps.emit(plain)

    def muzzle(self, x: float, y: float, angle: float, accent: RGB) -> None:
        r = self.rng
        for _ in range(self._n(12)):
            a = math.radians(angle + r.uniform(-18, 18))
            s = r.uniform(20, 55)
            self.ps.emit(Particle(x, y, math.cos(a) * s, math.sin(a) * s, r.uniform(0.15, 0.4), 0.4,
                                  (255, 250, 220), shade(accent, 0.3), drag=3.0))

    def smoke(self, x: float, y: float, n: int = 1, spread: float = 2.0, tint: Optional[RGB] = None) -> None:
        r = self.rng
        c0 = mix((118, 118, 126), tint, 0.6) if tint else (118, 118, 126)
        for _ in range(n):
            life = r.uniform(1.2, 2.4)
            self.ps.emit(Particle(x + r.uniform(-spread, spread), y, r.uniform(-4, 4), r.uniform(5, 14), life, life,
                                  c0, (34, 36, 44), gravity=-4, drag=0.8, alpha=0.55, size=2, windk=0.8))

    def impact(self, x: float, y: float, accent: RGB, soil: RGB, scale: float = 1.0,
               ammo: Optional[AmmoType] = None) -> None:
        r = self.rng
        style = ammo.boom if ammo else "classic"
        spark_c0 = mix((255, 255, 255), accent, 0.4)
        self._burst(x, y, self._n(46 * scale), (14 * scale, 55 * scale), (0.5, 1.1), spark_c0, shade(accent, 0.2),
                    gravity=70, drag=0.4)
        for _ in range(self._n(22 * scale)):
            a, s = r.uniform(0.3, math.pi - 0.3), r.uniform(10, 38) * scale
            life = r.uniform(0.8, 1.5)
            c = shade(soil, r.uniform(0.7, 1.2))
            self.ps.emit(Particle(x, y, math.cos(a) * s, math.sin(a) * s, life, life, c, shade(c, 0.5), gravity=90, solid=True))
        tint = accent if style in ("cloud", "implode") else None
        self.smoke(x, y + 1, self._n(16 * scale), 3.0 * scale, tint)
        if style == "cloud":
            self._burst(x, y + 1, self._n(26 * scale), (2, 9), (2.0, 3.5), mix(accent, (60, 90, 40), 0.3), (20, 30, 18),
                        gravity=-3, drag=0.8, size=2, alpha=0.5, windk=0.8)
        elif style == "embers":
            self._burst(x, y, self._n(30 * scale), (10, 40), (1.2, 2.2), (255, 190, 110), (120, 30, 10), gravity=30)
        elif style == "fire":
            self._burst(x, y, self._n(36 * scale), (15, 45), (0.5, 1.1), mix((255, 230, 140), accent, 0.25), (120, 26, 8),
                        gravity=-10, drag=0.6, size=2, spread=(1.0, 2.1))
        elif style in ("prism", "bloom"):
            self._burst(x, y, self._n(50 * scale), (14, 60), (0.7, 1.4), lambda: hsv(r.random(), 0.6, 1.0), lambda: (40, 30, 70),
                        gravity=40, drag=0.5)
        elif style == "cluster":
            self._burst(x, y, self._n(24 * scale), (30, 80), (0.3, 0.45), (255, 255, 255), shade(accent, 0.3), gravity=20)
        elif style == "implode":
            self._burst(x, y, self._n(24 * scale), (8, 30), (1.0, 1.8), shade(accent, 0.3), (10, 4, 20), gravity=20,
                        size=2, alpha=0.8)
        elif style in ("rays", "starrays"):
            self._burst(x, y, self._n(30 * scale), (30, 70), (0.5, 0.9), (255, 255, 255), shade(accent, 0.3), gravity=40)
        elif style == "shock":
            self._burst(x, y, self._n(26 * scale), (25, 60), (0.4, 0.8), (255, 255, 255), shade(accent, 0.3), gravity=30, drag=1.0)
        elif style == "shatter":
            self._burst(x, y, self._n(40 * scale), (20, 75), (0.6, 1.2), (240, 252, 255), shade(accent, 0.4), gravity=55, drag=0.3)
        elif style == "nova":
            self._burst(x, y, self._n(44 * scale), (20, 80), (0.6, 1.3), (255, 250, 220), (255, 120, 30), gravity=-6, drag=0.9,
                        spread=(0.0, math.pi))
        elif style == "sigil":
            self._burst(x, y, self._n(40 * scale), (18, 64), (0.7, 1.4), (255, 240, 180), (150, 90, 20), gravity=10, drag=0.7, size=2)
        elif style == "eclipse":
            self._burst(x, y, self._n(34 * scale), (10, 44), (0.8, 1.6), (255, 220, 120), (40, 12, 90), gravity=-8, drag=0.6, size=2)
        elif style == "tidal":
            self._burst(x, y, self._n(46 * scale), (16, 58), (0.7, 1.3), (200, 245, 255), (10, 50, 120), gravity=60, drag=0.4)
        elif style == "cinderstorm":
            self._burst(x, y, self._n(44 * scale), (14, 56), (1.0, 1.8), (255, 190, 90), (70, 16, 8), gravity=45, size=2)
        elif style == "rewind":
            self._burst(x, y, self._n(36 * scale), (8, 36), (0.9, 1.6), (255, 240, 200), (60, 30, 140), gravity=-14, drag=0.8, size=2)
        elif style == "magma":
            self._burst(x, y, self._n(36 * scale), (14, 52), (1.0, 1.9), (255, 190, 80), (120, 24, 8), gravity=85, size=2,
                        solid=True, spread=(0.5, math.pi - 0.5))
            self._burst(x, y, self._n(16 * scale), (6, 22), (1.4, 2.4), (255, 140, 40), (60, 16, 8), gravity=-8, drag=0.7)
        elif style == "thunder":
            self._burst(x, y, self._n(34 * scale), (40, 95), (0.2, 0.45), (235, 245, 255), shade(accent, 0.3), gravity=10, drag=1.5)
        elif style == "code":
            self._burst(x, y, self._n(34 * scale), (10, 48), (0.7, 1.4), (150, 255, 175), (8, 56, 22), gravity=45, drag=0.5, size=2)
        elif style == "blessing":
            self._burst(x, y, self._n(60 * scale), (14, 70), (0.9, 1.9), (255, 226, 120), (150, 90, 230), gravity=-6, drag=0.9, size=2)
            self._burst(x, y, self._n(30 * scale), (30, 90), (0.5, 1.2), (255, 255, 255), (255, 200, 80), gravity=0, drag=1.4, size=1)
            for k in range(20):
                a_ = k * math.tau / 20
                self.ps.emit(Particle(x, y, math.cos(a_) * 52, math.sin(a_) * 52, 1.0, 1.0, (255, 236, 160), (176, 120, 255), alpha=0.95, drag=1.5))
        elif style == "xenon":
            self._burst(x, y, self._n(46 * scale), (12, 58), (0.9, 1.8), (170, 255, 215), (120, 60, 220), gravity=-10, drag=0.8, size=2)
            self._burst(x, y, self._n(26 * scale), (30, 80), (0.5, 1.1), (255, 255, 255), (170, 110, 255), gravity=0, drag=1.4, size=1)
            for k in range(16):                              # a ring of ions flung outward
                a_ = k * math.tau / 16
                self.ps.emit(Particle(x, y, math.cos(a_) * 44, math.sin(a_) * 44, 0.9, 0.9, (190, 255, 230), (110, 60, 220), alpha=0.9, drag=1.6))
        elif style == "alien":
            self._burst(x, y, self._n(40 * scale), (12, 52), (0.8, 1.6), (150, 255, 200), (110, 50, 200), gravity=-10, drag=0.8, size=2)

    def ambient(self, design: TankDesign, x: float, y: float, body: RGB, dt: float) -> None:
        for kind in design.ambient.split("+"):
            if kind not in ("none", "sparkle", "orbit", "lightning"):
                self._ambient_one(kind, x, y, body, dt)

    def _ambient_one(self, kind: str, x: float, y: float, body: RGB, dt: float) -> None:
        r = self.rng
        rate = {"fire": 15.0, "burn": 15.0, "smoke": 7.0, "prism": 9.0}.get(kind, 6.0)
        if r.random() > dt * rate * self.settings.particle_density:
            return
        if kind == "burn":                                   # a burning machine: flames, smoke and embers together
            for k in ("fire", "smoke", "embers"):
                self._ambient_one(k, x, y, body, 1e9)
            return
        if kind == "sparks":
            self.ps.emit(Particle(x + r.uniform(-4, 4), y + 6, r.uniform(-8, 8), r.uniform(10, 24), 0.5, 0.5,
                                  (255, 230, 120), (255, 120, 20), gravity=40))
        elif kind == "embers":
            self.ps.emit(Particle(x + r.uniform(-4, 4), y + 5, r.uniform(-3, 3), r.uniform(6, 14), 1.2, 1.2,
                                  (255, 150, 40), (80, 20, 10), gravity=-8, alpha=0.9))
        elif kind == "mist":
            self.ps.emit(Particle(x + r.uniform(-5, 5), y + 2, r.uniform(-3, 3), r.uniform(2, 6), 1.8, 1.8,
                                  (150, 110, 230), (20, 10, 40), alpha=0.45, size=2, windk=0.5))
        elif kind == "static":
            a_ = r.uniform(0, math.tau)
            self.ps.emit(Particle(x + r.uniform(-5, 5), y + r.uniform(2, 8), math.cos(a_) * 14, math.sin(a_) * 14, 0.16, 0.16,
                                  (235, 245, 255), (90, 150, 255), drag=2.0))
        elif kind == "frost":
            self.ps.emit(Particle(x + r.uniform(-5, 5), y + 8, r.uniform(-3, 3), r.uniform(-6, -2), 1.6, 1.6,
                                  (235, 250, 255), (120, 170, 220), alpha=0.8, windk=0.6))
        elif kind == "flare":
            self.ps.emit(Particle(x + r.uniform(-5, 5), y + 6, r.uniform(-4, 4), r.uniform(8, 18), 1.0, 1.0,
                                  (255, 244, 190), (255, 130, 30), alpha=0.9, gravity=-6))
        elif kind == "bits":
            self.ps.emit(Particle(x + r.uniform(-5, 5), y + 7, 0, r.uniform(6, 14), 0.9, 0.9,
                                  (150, 255, 175), (8, 56, 22), alpha=0.9))
        elif kind == "fire":                                 # flame tongues licking up from the hull
            self.ps.emit(Particle(x + r.uniform(-4.5, 4.5), y + r.uniform(3, 6), r.uniform(-3, 3), r.uniform(14, 26), 0.55, 0.55,
                                  (255, 214, 90), (200, 30, 8), gravity=-24, alpha=0.95, size=2 if r.random() < 0.35 else 1))
        elif kind == "smoke":                                # dark smoke drifting up and away on the wind
            self.ps.emit(Particle(x + r.uniform(-4, 4), y + 7, r.uniform(-3, 3), r.uniform(5, 11), 2.2, 2.2,
                                  (88, 86, 92), (18, 18, 22), gravity=-4, alpha=0.55, size=2, windk=0.9))
        elif kind == "prism":                                # rainbow dust shed by a crystal
            c = hsv(r.random(), 0.5, 1.0)
            self.ps.emit(Particle(x + r.uniform(-5, 5), y + r.uniform(1, 7), r.uniform(-4, 4), r.uniform(3, 10), 1.1, 1.1,
                                  c, shade(c, 0.2), alpha=0.9, gravity=-3))
        elif kind == "void":
            self.ps.emit(Particle(x + r.uniform(-6, 6), y + r.uniform(1, 8), r.uniform(-3, 3), r.uniform(-2, 4), 1.4, 1.4,
                                  (150, 90, 240), (10, 4, 24), alpha=0.7, size=2, windk=0.2))
        elif kind == "bubbles":
            self.ps.emit(Particle(x + r.uniform(-5, 5), y + 2, r.uniform(-2, 2), r.uniform(5, 12), 1.5, 1.5,
                                  (190, 235, 255), (90, 150, 220), alpha=0.7, windk=0.4))
        elif kind == "petals":
            self.ps.emit(Particle(x + r.uniform(-5, 5), y + 8, r.uniform(-5, 5), r.uniform(-6, -1), 1.8, 1.8,
                                  (255, 170, 200), (200, 80, 130), alpha=0.85, windk=0.8))

    def tank_debris(self, x: float, y: float, color: RGB) -> None:
        r = self.rng
        for _ in range(self._n(40)):
            a, s = r.uniform(0.2, math.pi - 0.2), r.uniform(12, 50)
            life = r.uniform(1.0, 2.0)
            c = r.choice((color, shade(color, 0.5), (36, 38, 44)))
            self.ps.emit(Particle(x, y, math.cos(a) * s, math.sin(a) * s, life, life, c, shade(c, 0.4), gravity=80,
                                  solid=True, size=2))


# ============================================================================
# Physics
# ============================================================================
class ImpactKind(Enum):
    TERRAIN = auto()
    TANK = auto()
    OUT = auto()


@dataclass(frozen=True)
class Impact:
    x: float
    y: float
    kind: ImpactKind
    tank: Optional[int] = None


@dataclass(frozen=True)
class Launch:
    x0: float
    y0: float
    vx: float
    vy: float
    g: float
    ax: float = 0.0   # horizontal wind acceleration
    wa: float = 0.0   # corkscrew amplitude (px) - a sideways oscillation across the flight line
    wf: float = 0.0   # corkscrew angular frequency

    def at(self, t: float) -> tuple[float, float]:
        x, y = self.x0 + self.vx * t + 0.5 * self.ax * t * t, self.y0 + self.vy * t - 0.5 * self.g * t * t
        if self.wa:
            sp = math.hypot(self.vx, self.vy) or 1.0
            s = self.wa * math.sin(self.wf * t) * min(1.0, t * 2.5)
            x, y = x - self.vy / sp * s, y + self.vx / sp * s
        return x, y


@dataclass
class TraceResult:
    impact: Impact
    points: list
    time: float


class PhysicsEngine:
    def __init__(self, cfg: PhysicsConfig, field_width: int) -> None:
        self.cfg = cfg
        self.max_speed = math.sqrt(cfg.gravity * field_width * cfg.range_factor)

    def speed_for(self, power: float) -> float:
        return self.max_speed * clamp(power / self.cfg.max_power, 0, 1) ** self.cfg.power_curve

    def make_launch(self, origin: tuple[float, float], angle: float, power: float,
                    gravity: Optional[float] = None, wind: float = 0.0) -> Launch:
        a, v = math.radians(angle), self.speed_for(power)
        return Launch(origin[0], origin[1], v * math.cos(a), v * math.sin(a),
                      self.cfg.gravity if gravity is None else gravity, wind)

    def trace(self, launch: Launch, probe: Callable, dt: float = 0.02, spacing: float = 0.0,
              max_time: float = 12.0) -> TraceResult:
        pts, acc, t = [], 0.0, 0.0
        px, py = launch.x0, launch.y0
        while t < max_time:
            t += dt
            x, y = launch.at(t)
            hit = probe(x, y)
            if spacing:
                acc += math.hypot(x - px, y - py)
                if acc >= spacing:
                    pts.append((x, y))
                    acc = 0.0
            if hit:
                return TraceResult(Impact(x, y, hit[0], hit[1]), pts, t)
            px, py = x, y
        return TraceResult(Impact(px, py, ImpactKind.OUT), pts, t)


class Projectile:
    def __init__(self, launch: Launch, owner: int, probe: Callable) -> None:
        self.launch, self.owner, self.probe = launch, owner, probe
        self.t = 0.0
        self.x, self.y = launch.x0, launch.y0


def advance_projectile(proj: Projectile, dt: float, cfg: PhysicsConfig,
                       on_step: Optional[Callable[[float, float], None]] = None) -> Optional[Impact]:
    """Moves a shell along its analytic arc in sub-steps so it can never tunnel."""
    t0, t1 = proj.t, proj.t + dt
    x1, y1 = proj.launch.at(t1)
    n = max(1, int(math.hypot(x1 - proj.x, y1 - proj.y) / cfg.substep) + 1)
    for i in range(1, n + 1):
        tt = t0 + dt * i / n
        x, y = proj.launch.at(tt)
        hit = proj.probe(x, y)
        if hit:
            proj.t, proj.x, proj.y = tt, x, y
            return Impact(x, y, hit[0], hit[1])
        if on_step:
            on_step(x, y)
    proj.t, proj.x, proj.y = t1, x1, y1
    return None


# ============================================================================
# Battle world
# ============================================================================
class BattleWorld:
    def __init__(self, width: int, height: int, mapdef: MapDefinition, seed: int, players: Sequence["PlayerSetup"],
                 settings: Settings) -> None:
        self.width, self.height, self.mapdef, self.settings = width, height, mapdef, settings
        self.rng = random.Random(seed)
        self.terrain = Terrain(width, height, mapdef, self.rng)
        self.physics = PhysicsEngine(PHYS, width)
        self.vrng = random.Random(seed ^ 0x5EED)       # visuals only: effects must never touch the simulation's rng (LAN sync)
        self.tanks: list[Tank] = []
        slots: dict = {}
        for i, pl in enumerate(players):
            jitter = self.rng.uniform(-0.02, 0.02)
            team = pl.team if pl.team >= 0 else i
            k = slots[team] = slots.get(team, -1) + 1
            gap = 0.13 if all(p.scale == 1.0 for p in players) else 0.18
            frac = mapdef.spawns[0] + k * gap if team == 0 else mapdef.spawns[1] - k * gap
            half = int(TANK_W * pl.scale / 2)
            x = int(clamp((frac + jitter) * width, 9 + half, width - 10 - half))
            self.terrain.flatten(x + 0.5, 10)
            hover = pl.hover or DESIGN_BY_KEY[pl.design].hover
            self.tanks.append(Tank(i, pl.name, pl.label, pl.tank_color, pl.shot_color, pl.is_ai, x + 0.5,
                                   float(self.terrain.support_height(x)) + hover, 1 if team == 0 else -1,
                                   50.0 if team == 0 else 130.0,
                                   hp=pl.hp, max_hp=pl.hp, hull_sentinel=pl.tank_color, shot_sentinel=pl.shot_color, rgb_hull=pl.tank_color in LIVE_COLORS, rgb_shot=pl.shot_color in LIVE_COLORS,
                                   design=DESIGN_BY_KEY[pl.design], ammo=AMMO_BY_KEY[pl.ammo], team=team, owner=pl.owner,
                                   kind=pl.kind, scale=pl.scale, hover=hover, shield=pl.shield, guard=pl.guard))
        self.scenery = Scenery(width, height, mapdef.theme, self.rng)
        self.scenery.apply_terrain(self.terrain)
        self.particles = ParticleSystem()
        self.explosions: list[Explosion] = []
        self.fx = EffectsFactory(self.particles, settings, self.vrng)
        self._smoke_timer = 0.0
        self.clock = 0.0
        self.wind = 0.0
        self.roll_wind()
        self._make_streaks()

    # -- wind ----------------------------------------------------------------
    def roll_wind(self) -> None:
        m = self.settings.wind
        self.wind = round(self.rng.choice((-1, 1)) * self.rng.uniform(0.2, 1.0) * m, 1) if m > 0 else 0.0

    def drift_wind(self) -> None:
        m = self.settings.wind
        if m > 0:
            self.wind = round(clamp(self.wind + self.rng.uniform(-0.45, 0.45) * m, -m, m), 1)

    def _make_streaks(self) -> None:
        n = max(8, self.width * self.height // 450)
        self.streaks = [[self.rng.uniform(0, self.width), self.rng.uniform(self.height * 0.25, self.height - 2)] for _ in range(n)]

    def draw_wind(self, canvas: PixelCanvas) -> None:
        if abs(self.wind) < 0.3:
            return
        sgn = 1 if self.wind > 0 else -1
        length = 3 + int(abs(self.wind) * 0.9)
        sky = self.scenery.sky
        for x, y in self.streaks:
            py = self.height - 1 - int(y)
            if not 0 <= py < self.height:
                continue
            for i in range(length):
                px = int(x) - sgn * i
                if 0 <= px < self.width and canvas.rows[py][px] == sky[py][px]:
                    canvas.rows[py][px] = mix(sky[py][px], (235, 242, 255), 0.22 * (1 - i / length))

    # -- collision -----------------------------------------------------------
    def collision_at(self, x: float, y: float, skip: Optional[int]):
        if x < 0 or x >= self.width:
            return ImpactKind.OUT, None
        if y < self.terrain.heights[int(x)]:
            return ImpactKind.TERRAIN, None
        r = PHYS.projectile_radius
        for t in self.tanks:
            if t.index == skip or t.destroyed:
                continue
            x0, y0, x1, y1 = t.rect
            if x0 - r <= x <= x1 + r and y0 - r <= y <= y1 + r:
                return ImpactKind.TANK, t.index
        return None

    def inside_tank(self, idx: int, x: float, y: float) -> bool:
        x0, y0, x1, y1 = self.tanks[idx].rect
        return x0 - 1.5 <= x <= x1 + 1.5 and y0 - 1.5 <= y <= y1 + 1.5

    def owner_probe(self, owner: int) -> Callable:
        state = {"left": False}

        def probe(x: float, y: float):
            if not state["left"] and not self.inside_tank(owner, x, y):
                state["left"] = True
            return self.collision_at(x, y, None if state["left"] else owner)
        return probe

    def distance_to_tank(self, tank: Tank, x: float, y: float) -> float:
        x0, y0, x1, y1 = tank.rect
        return math.hypot(x - clamp(x, x0, x1), y - clamp(y, y0, y1))

    # -- state updates -------------------------------------------------------
    def settle(self, dt: float) -> bool:
        moving = False
        for t in self.tanks:
            if t.destroyed:
                continue
            target = float(self.terrain.support_height(t.x)) + t.hover
            if t.y > target:
                t.y = max(target, t.y - PHYS.fall_speed * dt)
                moving = True
            elif t.y < target:
                t.y = target
        return moving

    def carve(self, x: float, y: float, r: float) -> None:
        x0, x1 = self.terrain.carve(x, y, r)
        self.scenery.apply_terrain(self.terrain, x0 - 1, x1 + 1)

    def terrain_op(self, mode: str, x: float, y: float, r: float) -> None:
        """Shell-specific ground effects. 'blast' digs a crater; the others are what makes some ammo a different tool."""
        t = self.terrain
        if mode == "mound":
            x0, x1 = t.mound(x, r * 1.25, r * 0.9)
        elif mode == "trench":
            x0, x1 = t.trench(x, max(2.0, r * 0.45), r * 1.5)
        elif mode == "glass":
            x0, x1 = t.smooth(x, r * 1.4)
        else:
            return self.carve(x, y, r)
        self.scenery.apply_terrain(t, x0 - 1, x1 + 1)

    def update_effects(self, dt: float) -> None:
        self.clock += dt
        for t in self.tanks:
            if t.vis_dx:
                t.vis_dx = 0.0 if abs(t.vis_dx) < 0.05 else t.vis_dx * max(0.0, 1 - dt * 3.2)
        if any(t.rgb_hull or t.rgb_shot for t in self.tanks):
            for t in self.tanks:
                if t.rgb_hull:
                    t.color = live_color(t.hull_sentinel, self.clock)
                if t.rgb_shot:
                    t.shot_color = live_color(t.shot_sentinel, self.clock)
        self.particles.wind = self.wind
        drift = self.wind * 7 * dt
        for st in self.streaks:
            st[0] = (st[0] + drift) % self.width
        self.particles.update(dt, self.terrain)
        for e in self.explosions:
            e.update(dt)
        self.explosions = [e for e in self.explosions if not e.done]
        for t in self.tanks:
            t.hurt = max(0.0, t.hurt - dt)
            t.recoil = max(0.0, t.recoil - dt)
            if t.alive:
                if t.kind != "ufo":
                    self.fx.ambient(t.design, t.x, t.y, t.color, dt)
        self._smoke_timer -= dt
        if self._smoke_timer <= 0:
            self._smoke_timer = 0.09
            for t in self.tanks:
                if t.destroyed:
                    self.fx.smoke(t.x, t.y + 4, 1, 3.0)
                elif t.hp == 1 and self.vrng.random() < 0.35:
                    self.fx.smoke(t.x, t.y + 6, 1, 1.5)

    def resize(self, w: int, h: int) -> None:
        sx, sy = w / self.width, h / self.height
        old, oldsc = self.terrain.heights, self.terrain.scorch
        self.terrain.heights = [int(clamp(old[min(len(old) - 1, int(x / sx))] * sy, 2, h - 10)) for x in range(w)]
        self.terrain.scorch = [oldsc[min(len(oldsc) - 1, int(x / sx))] for x in range(w)]
        self.terrain.width, self.terrain.height = w, h
        self.width, self.height = w, h
        self.physics = PhysicsEngine(PHYS, w)
        self.scenery = Scenery(w, h, self.mapdef.theme, self.rng)
        self.scenery.apply_terrain(self.terrain)
        for t in self.tanks:
            t.x = int(clamp(t.x * sx, 9, w - 10)) + 0.5
            t.y = float(self.terrain.support_height(t.x)) + t.hover
        self.particles.items.clear()
        self.explosions.clear()
        self._make_streaks()


# ============================================================================
# AI
# ============================================================================
@dataclass(frozen=True)
class AIProfile:
    angle_sigma: float
    power_sigma: float
    gravity_error: float
    sim_dt: float
    terrain_aware: bool
    learn_rate: float
    think_time: float
    angle_step: int
    pick_from: int
    refine: bool
    wind_error: float          # how badly the AI misreads the wind (fraction)
    adaptive: bool = False     # Master: ensemble planning + physics-model calibration


AI_PROFILES = {
    Difficulty.EASY: AIProfile(10.0, 0.18, 0.30, 0.06, False, 0.35, 2.0, 6, 8, False, 0.65),
    Difficulty.NORMAL: AIProfile(5.5, 0.09, 0.16, 0.04, True, 0.65, 1.5, 3, 3, False, 0.30),
    Difficulty.HARD: AIProfile(4.2, 0.075, 0.10, 0.025, True, 0.92, 1.1, 3, 1, True, 0.12),
    # Same hands as a very good human (it still shakes), but it reasons much better:
    # it picks the shot that is most likely to land under its own execution noise and
    # fits its gravity model to every miss instead of nudging an aim offset.
    Difficulty.MASTER: AIProfile(1.8, 0.032, 0.06, 0.02, True, 0.9, 0.9, 3, 1, True, 0.03, True),
}


@dataclass
class AimPlan:
    angle: float
    power: float
    intended_rel: float
    intended_power: float
    think_time: float
    status: str
    prob: float = 0.0


@dataclass
class ShotRecord:
    rel: float
    power: float
    error_x: float
    hit: bool


class ModelProbe:
    """The AI's own (imperfect) world model used while solving for a shot."""

    def __init__(self, terrain: Terrain, foe: Tank, aware: bool) -> None:
        self.terrain, self.rect, self.aware, self.flat = terrain, foe.rect, aware, foe.y
        self.w = terrain.width

    def __call__(self, x: float, y: float):
        if x < 0 or x >= self.w:
            return ImpactKind.OUT, None
        x0, y0, x1, y1 = self.rect
        if x0 - 1 <= x <= x1 + 1 and y0 - 1 <= y <= y1 + 1:
            return ImpactKind.TANK, None
        ground = self.terrain.heights[int(x)] if self.aware else self.flat
        if y < ground:
            return ImpactKind.TERRAIN, None
        return None


class AIController:
    """Searches (angle, power) pairs against an imperfect ballistic model and learns from misses."""

    def __init__(self, difficulty: Difficulty, rng: random.Random, precision: float = 1.0) -> None:
        self.profile = AI_PROFILES[difficulty]
        if precision != 1.0:        # Tournament levels fine-tune the hands: <1 sharper, >1 sloppier
            self.profile = replace(self.profile, angle_sigma=self.profile.angle_sigma * precision,
                                   power_sigma=self.profile.power_sigma * precision)
        self.rng = rng
        self.gravity_bias = 1 + rng.uniform(-self.profile.gravity_error, self.profile.gravity_error)
        self.correction = 0.0
        self.last_wind: Optional[float] = None
        self.history: list[ShotRecord] = []
        self._ctx = None

    def _shoot(self, ctx, rel: float, power: float, g_scale: float) -> Impact:
        me, foe, world, wind, probe = ctx
        ang = rel if me.facing > 0 else 180 - rel
        phys = world.physics
        launch = phys.make_launch(me.launch_origin(ang), ang, power, phys.cfg.gravity * g_scale, wind)
        return phys.trace(launch, probe, dt=self.profile.sim_dt, max_time=10.0).impact

    @staticmethod
    def _is_hit(imp: Impact, foe: Tank, world: BattleWorld) -> bool:
        return imp.kind is ImpactKind.TANK or (
            imp.kind is ImpactKind.TERRAIN and world.distance_to_tank(foe, imp.x, imp.y) <= PHYS.splash_reach)

    def plan(self, me: Tank, foe: Tank, world: BattleWorld) -> AimPlan:
        p = self.profile
        aim_x = clamp(foe.x + self.correction, 3, world.width - 4)
        aim_y = foe.y + foe.height / 2
        wind = world.wind * (1 + self.rng.uniform(-p.wind_error, p.wind_error))
        if self.last_wind is not None and abs(world.wind - self.last_wind) > 0.5:
            self.correction *= 0.5     # old corrections were learned in different wind
        self.last_wind = world.wind
        ctx = self._ctx = (me, foe, world, wind, ModelProbe(world.terrain, foe, p.terrain_aware))

        def miss(rel: float, power: float) -> float:
            imp = self._shoot(ctx, rel, power, self.gravity_bias)
            d = abs(imp.x - foe.x) * 0.15 if imp.kind is ImpactKind.TANK else math.hypot(imp.x - aim_x, imp.y - aim_y)
            for rec in self.history:
                if not rec.hit and abs(rec.rel - rel) < 1.5 and abs(rec.power - power) < 3:
                    d += 12
            return d + 0.02 * abs(rel - 52)

        scored = []
        for rel in range(18, 83, p.angle_step):
            for power in range(22, 101, 3):
                scored.append((miss(rel, power) + self.rng.random() * 0.4, rel, power))
        scored.sort()
        prob = 0.0
        if p.adaptive:
            rel, power, prob = self._ensemble_pick(scored, miss, ctx, foe, world)
        else:
            _, rel, power = self.rng.choice(scored[:p.pick_from])
            if p.refine:
                best = (miss(rel, power), rel, power)
                for da in range(-2, 3):
                    for dp in range(-3, 4):
                        pw = clamp(power + dp, 15, 100)
                        cand = (miss(rel + da, pw), rel + da, pw)
                        if cand < best:
                            best = cand
                _, rel, power = best
        exec_rel = clamp(rel + self.rng.gauss(0, p.angle_sigma), 4, 176)
        exec_power = clamp(power * (1 + self.rng.gauss(0, p.power_sigma)), 12, 100)
        angle = exec_rel if me.facing > 0 else 180 - exec_rel
        if p.adaptive:
            status = f"ENSEMBLE SOLVE - P(HIT) {prob:.0%}"
            if self.history:
                status += f" - MODEL RECALIBRATED x{len(self.history)}"
        elif self.history:
            last = self.history[-1]
            long_shot = last.error_x * me.facing > 0
            status = f"LAST SHOT {'LONG' if long_shot else 'SHORT'} {abs(last.error_x):.0f}PX - RECALIBRATING"
            if last.hit:
                status = "TARGET LOCKED - REPEATING SOLUTION"
        else:
            status = "SOLVING FIRING SOLUTION"
        return AimPlan(angle, exec_power, rel, power, p.think_time * self.rng.uniform(0.85, 1.2), status, prob)

    def _ensemble_pick(self, scored, miss, ctx, foe, world) -> tuple:
        """Refine the best distinct candidates, then keep the one most likely to hit under execution noise."""
        p, picks, seen = self.profile, [], set()
        for _, rel, power in scored:
            key = (round(rel / 2), round(power / 3))
            if key not in seen:
                seen.add(key)
                picks.append((rel, power))
            if len(picks) == 8:
                break
        best = None
        for rel0, pow0 in picks:
            local = (miss(rel0, pow0), rel0, pow0)
            for da in (-1, 0, 1):
                for dp in (-2, 0, 2):
                    pw = clamp(pow0 + dp, 15, 100)
                    cand = (miss(rel0 + da, pw), rel0 + da, pw)
                    if cand < local:
                        local = cand
            dist, rel, power = local
            hits, n = 0, 14
            for _ in range(n):
                imp = self._shoot(ctx, rel + self.rng.gauss(0, p.angle_sigma),
                                  clamp(power * (1 + self.rng.gauss(0, p.power_sigma)), 12, 100), self.gravity_bias)
                hits += self._is_hit(imp, foe, world)
            score = hits / n - 0.003 * dist
            if best is None or score > best[0]:
                best = (score, rel, power, hits / n)
        return best[1], best[2], best[3]

    def observe(self, plan: AimPlan, impact: Impact, foe: Tank, hit: bool) -> None:
        err = impact.x - foe.x
        self.history.append(ShotRecord(plan.intended_rel, plan.intended_power, err, hit))
        if self.profile.adaptive:
            if not hit and self._ctx is not None:
                self._fit_gravity(plan, impact)
            return
        if not hit:
            self.correction = clamp(self.correction - err * self.profile.learn_rate, -30, 30)

    def _fit_gravity(self, plan: AimPlan, impact: Impact) -> None:
        """Bisect for the gravity scale that makes the model reproduce where the shell really landed."""
        ctx = self._ctx
        f = lambda g: self._shoot(ctx, plan.intended_rel, plan.intended_power, g).x - impact.x
        lo, hi = 0.75, 1.25
        flo, fhi = f(lo), f(hi)
        if flo * fhi > 0:
            return
        for _ in range(10):
            mid = (lo + hi) / 2
            fm = f(mid)
            if flo * fm <= 0:
                hi, fhi = mid, fm
            else:
                lo, flo = mid, fm
        self.gravity_bias = clamp(self.gravity_bias + ((lo + hi) / 2 - self.gravity_bias) * 0.6, 0.8, 1.2)


# ============================================================================
# Tournament definition & persistence
# ============================================================================
@dataclass(frozen=True)
class TournamentLevel:
    """One boss fight. `rule` selects the win/loss condition:
        ""          standard: the boss may win twice (two strikes); its third round win fails the level
        "streak"    win `wins` rounds IN A ROW - a boss round win resets the streak (no strikes, unlimited tries)
        "flawless"  win `wins` rounds without losing a single one - the first boss round win fails the level
        "onestrike" the boss may win once; its second round win fails the level
        "glass"     standard strikes, but your tank has only two hearts
        "gale"      standard strikes, but the wind is stuck at its strongest
    The last FINAL_BOSSES levels never use a rule."""
    number: int
    name: str
    boss: str
    difficulty: Difficulty
    wins: int                  # rounds the player must win
    tank: str                  # boss kit == the reward kit
    ammo: str
    boss_color: str
    boss_shot: str
    blurb: str
    map_key: Optional[str] = None       # a reward map the level is fought on (looked up in SPECIAL_MAP_BY_KEY)
    precision: float = 1.0              # AI aim-noise multiplier: <1 = sharper, >1 = sloppier (fine ramp inside a difficulty)
    rule: str = ""
    extra_rewards: tuple = ()           # unlockable ids beyond the kit (+ map) this level hands out

    # -- rules ---------------------------------------------------------------
    @property
    def consecutive(self) -> bool:
        return self.rule == "streak"

    @property
    def fail_at(self) -> Optional[int]:
        """Boss round wins that fail the level (None = it can never be failed, only retried within the run)."""
        return {"streak": None, "flawless": 1, "onestrike": 2}.get(self.rule, BOSS_MAX_WINS)

    @property
    def strikes_allowed(self) -> Optional[int]:
        return None if self.fail_at is None else self.fail_at - 1

    @property
    def player_hp(self) -> int:
        return GLASS_HP if self.rule == "glass" else MAX_HP

    @property
    def wind_override(self) -> Optional[float]:
        return 10.0 if self.rule == "gale" else None

    @property
    def is_final(self) -> bool:
        return self.number > len(TOURNAMENT) - FINAL_BOSSES

    @property
    def rule_tag(self) -> str:
        return {"": "STANDARD", "streak": "BACK TO BACK", "flawless": "FLAWLESS", "onestrike": "ONE STRIKE",
                "glass": "GLASS CANNON", "gale": "GALE FORCE"}[self.rule]

    @property
    def rule_text(self) -> str:
        return {"": "2 STRIKES - BOSS WIN #3 FAILS IT",
                "streak": "NO STRIKES - A LOSS RESETS STREAK",
                "flawless": "NO STRIKES - 1 LOST ROUND FAILS",
                "onestrike": "1 STRIKE - BOSS WIN #2 FAILS IT",
                "glass": f"{GLASS_HP} HEARTS - BOSS WIN #3 FAILS IT",
                "gale": "WIND LOCKED AT MAX - 2 STRIKES"}[self.rule]

    @property
    def objective(self) -> str:
        if self.rule == "streak":
            return f"WIN {self.wins} ROUNDS IN A ROW"
        if self.rule == "flawless":
            return f"WIN {self.wins} ROUNDS, LOSE NONE"
        return f"WIN {self.wins} ROUNDS"

    @property
    def short_objective(self) -> str:
        return f"{self.wins} IN A ROW" if self.rule == "streak" else f"{self.wins} WINS"

    def reward_ids(self) -> tuple:
        """Every unlockable id earned by clearing this level: its exact boss tank + ammo, its map, any extras."""
        ids = [f"tank:{self.tank}", f"ammo:{self.ammo}"]
        if self.map_key:
            ids.append(f"map:{self.map_key}")
        return tuple(ids) + tuple(self.extra_rewards)


# Twenty levels in four acts. Level N unlocks exactly one new tank + one new ammo type (kit number 5+N) and its boss
# fights with that very kit. Levels 1-17 climb Easy -> Normal -> Hard with a per-level `precision` ramp; some of them use
# a special win condition (`rule`). Levels 18-20 are the three big bosses: Master AI, standard rules only, each on its
# own reward map; 18 and 19 are shorter fights, 20 is the longest, on the Citadel, and the only one that unlocks the
# Master difficulty and Master home screen.
TOURNAMENT: tuple[TournamentLevel, ...] = (
    TournamentLevel(1, "BOOT CAMP", "STINGER", Difficulty.EASY, 3, "hornet", "venom", "YELLOW", "LIME",
                    "Land three rounds back to back.", rule="streak"),
    TournamentLevel(2, "PROVING GROUND", "WARDEN", Difficulty.NORMAL, 3, "titan", "starburst", "STEEL", "SKY",
                    "A patient gunner who learns fast.", precision=1.25),
    TournamentLevel(3, "GHOST FRONT", "SPECTER", Difficulty.NORMAL, 4, "phantom", "void", "VIOLET", "PURPLE",
                    "Quiet, steady and hard to shake.", precision=1.05),
    TournamentLevel(4, "IRON GAUNTLET", "COLOSSUS", Difficulty.NORMAL, 4, "juggernaut", "inferno", "CRIMSON", "ORANGE",
                    "A siege engine that never blinks.", precision=0.9),
    TournamentLevel(5, "STEEL SENTRY", "SENTINEL", Difficulty.HARD, 4, "sentinel", "pulse", "TEAL", "AQUA",
                    "Two hearts. Spend them wisely.", precision=1.4, rule="glass"),
    TournamentLevel(6, "DEEP FREEZE", "GLACIER", Difficulty.HARD, 5, "glacier", "cryo", "SKY", "WHITE",
                    "Cold, exact and in no hurry.", precision=1.3),
    TournamentLevel(7, "SOLAR FLARE", "HELIOS", Difficulty.HARD, 5, "helios", "nova", "ORANGE", "YELLOW",
                    "Hot hands, hotter shells.", precision=1.2),
    TournamentLevel(8, "DUST BOWL", "RATTLER", Difficulty.HARD, 3, "rattler", "shrapnel", "SAND", "COPPER",
                    "Win clean: lose a single round and it is over.", precision=1.25, rule="flawless"),
    TournamentLevel(9, "TOXIC MARSH", "MIASMA", Difficulty.HARD, 5, "miasma", "blight", "LIME", "GREEN",
                    "It gets in everywhere.", precision=1.1),
    TournamentLevel(10, "STONE WALL", "BASTION", Difficulty.HARD, 5, "bastion", "siege", "OLIVE", "SAND",
                    "A fortress that fires back.", precision=1.05),
    TournamentLevel(11, "WIND TUNNEL", "ZEPHYR", Difficulty.HARD, 5, "zephyr", "cyclone", "AQUA", "WHITE",
                    "The wind never drops. Read it or lose.", precision=1.15, rule="gale"),
    TournamentLevel(12, "CRYSTAL DEPTHS", "GEODE", Difficulty.HARD, 6, "geode", "crystal", "VIOLET", "PINK",
                    "Every facet is a firing line.", precision=0.95),
    TournamentLevel(13, "HALL OF MIRRORS", "MIRAGE", Difficulty.HARD, 6, "mirage", "echo", "PINK", "MAGENTA",
                    "Is that the real tank?", precision=0.9),
    TournamentLevel(14, "GRAVEYARD SHIFT", "REVENANT", Difficulty.HARD, 5, "revenant", "wraith", "OBSIDIAN", "CYAN",
                    "It only needs two wins.", precision=1.05, rule="onestrike"),
    TournamentLevel(15, "SKY RAID", "CORSAIR", Difficulty.HARD, 6, "corsair", "barrage", "COBALT", "SKY",
                    "Fast, loud and accurate.", precision=0.8),
    TournamentLevel(16, "BLOOD MOON", "TYRANT", Difficulty.HARD, 4, "tyrant", "wrath", "CRIMSON", "GOLD",
                    "Four in a row. Blink and it resets.", precision=0.85, rule="streak"),
    TournamentLevel(17, "THE GATEKEEPER", "ARCHON", Difficulty.HARD, 6, "archon", "zenith", "GOLD", "WHITE",
                    "The last guard on the Master's road.", precision=0.65),
    TournamentLevel(18, "THE FORGE", "OBSIDIAN", Difficulty.MASTER, 5, "obsidian", "magma", "OBSIDIAN", "ORANGE",
                    "Master-grade fire control in a lava foundry.", map_key="forge", precision=0.95),
    TournamentLevel(19, "STORM SPIRE", "TEMPEST", Difficulty.MASTER, 6, "tempest", "thunder", "BLUE", "CYAN",
                    "Lightning overhead, a Master across the gap.", map_key="spire", precision=0.88),
    TournamentLevel(20, "MASTER'S CITADEL", "THE MASTER", Difficulty.MASTER, 8, "sovereign", "aurora", "GOLD", "WHITE",
                    "The final duel, on ground few have seen.", map_key="citadel", precision=0.78,
                    extra_rewards=("master_difficulty", "theme:master")),
)
assert len(TOURNAMENT) == 20 and TOURNAMENT[-1].map_key == MASTER_MAP.key
assert all(l.number == i + 1 for i, l in enumerate(TOURNAMENT))
# the progression contract: level N unlocks kit number BASE_KITS+N (exactly one tank + one ammo) and its boss uses it
assert all(l.tank == DESIGNS[BASE_KITS + l.number - 1].key and l.ammo == AMMOS[BASE_KITS + l.number - 1].key for l in TOURNAMENT)
assert all(l.map_key in SPECIAL_MAP_BY_KEY for l in TOURNAMENT if l.map_key)
assert all(l.rule == "" for l in TOURNAMENT[-FINAL_BOSSES:]) and sum(1 for l in TOURNAMENT if l.rule) >= 5
assert all(l.rule != "streak" or l.difficulty is not Difficulty.MASTER for l in TOURNAMENT)
COLOR_BY_NAME = dict(COLOR_CHOICES)
COLOR_BY_NAME["RGB"] = RAINBOW        # the animated RGB colour, selectable once unlocked in the shop
COLOR_BY_NAME["ASTRAL"] = ASTRAL      # the Astral Rift colour, earned by beating the Martyr
COLOR_BY_NAME["PRISMATIC"] = PRISMATIC   # shop paint
COLOR_BY_NAME["VOID"] = VOID             # shop paint
PAINTS = list(COLOR_CHOICES) + [("RGB", RAINBOW), ("ASTRAL", ASTRAL), ("PRISMATIC", PRISMATIC), ("VOID", VOID)]
PAINT_BY_KEY = {"astral": ASTRAL, "prismatic": PRISMATIC, "void": VOID}
PAINT_UNLOCK = {"RGB": ("rgb:hull", "rgb:shell"), "ASTRAL": ("paint:astral", "paint:astral"),
                "PRISMATIC": ("paint:prismatic", "paint:prismatic"), "VOID": ("paint:void", "paint:void")}


def paint_index(name: str) -> int:
    return next((i for i, (n, _) in enumerate(PAINTS) if n == name), 0)


def color_index(name: str, default: int = 0) -> int:
    return next((i for i, (n, _) in enumerate(COLOR_CHOICES) if n == name), default)


def default_save_path() -> Path:
    env = os.environ.get("TERMINALTANKS_SAVE")
    if env:
        return Path(env).expanduser()
    if os.name == "nt":
        base = Path(os.environ.get("APPDATA", str(Path.home()))) / "TerminalTanks"
    else:
        base = Path(os.environ.get("XDG_CONFIG_HOME", str(Path.home() / ".config"))) / "terminaltanks"
    return base / "save.json"


@dataclass
class Loadout:
    tank: str = "ranger"
    tank_color: str = "CYAN"
    ammo: str = "standard"
    shot_color: str = "YELLOW"


# ---------------------------------------------------------------------------
# Economy. Two currencies: COINS (everything) and TOURNAMENT CREDITS (Tournament only, needed for Master-tier gear).
# Tweak the numbers here; the shop prices are in COIN_PRICES / CREDIT_PRICES further down. Cheat codes never touch
# the wallet - the only way to earn is to play.
#   TTT + $$$$   first clear of a big boss (levels 18-20)       T + $$   big boss, later clears
#   T + $$$      first clear of any other level                 $$       other levels, later clears
#   $            single-player win (x difficulty)               $.5      any loss (level failed / match lost)
# ---------------------------------------------------------------------------
COIN_HALF, COIN_LITTLE, COIN_SOME, COIN_DECENT, COIN_LOTS = 10, 20, 50, 120, 400
CREDITS_SOME, CREDITS_LOTS = 4, 15
SP_WIN_MULT = {Difficulty.EASY: 1.0, Difficulty.NORMAL: 1.25, Difficulty.HARD: 1.5, Difficulty.MASTER: 2.0}


@dataclass(frozen=True)
class Payout:
    coins: int = 0
    credits: int = 0

    def text(self) -> str:
        return f"+{self.coins} ◉" + (f"   +{self.credits} ◈" if self.credits else "")


def tournament_payout(level: TournamentLevel, won: bool, first: bool) -> Payout:
    if not won:
        return Payout(COIN_HALF)
    if level.is_final:
        return Payout(COIN_LOTS, CREDITS_LOTS) if first else Payout(COIN_SOME, CREDITS_SOME)
    return Payout(COIN_DECENT, CREDITS_SOME) if first else Payout(COIN_SOME)


def match_payout(won: bool, difficulty: Difficulty) -> Payout:
    return Payout(int(COIN_LITTLE * SP_WIN_MULT[difficulty])) if won else Payout(COIN_HALF)


# ---------------------------------------------------------------------------
# Home screens that can be bought (the Master and Secret screens are earned / cheat-only).
# ---------------------------------------------------------------------------
@dataclass(frozen=True)
class HomeTheme:
    key: str
    name: str
    blurb: str
    sky: tuple              # gradient stops, horizon (0) -> zenith (1)
    far: RGB                # distant ridge
    ground: RGB
    ground_top: RGB
    accent: RGB             # frame / highlight colour
    logo_a: RGB             # logo gradient, top -> bottom
    logo_b: RGB
    fx: str                 # drifting-particle layer (key of _MOTE_STYLES) or ""
    hero: str               # tank design shown firing on the title screen
    hero_ammo: str
    stars: bool = True
    aurora: bool = False
    corona: bool = False
    layers: tuple = ()      # scenery layers drawn by ThemeBackdrop: see LAYER_DRAWERS (moon, sun, city, mountains, grid, ...)


def _hx(key: str, name: str, blurb: str, sky4: tuple, far: RGB, ground: RGB, accent: RGB, fx: str, hero: str, ammo: str,
        layers: tuple, stars: bool = True, aurora: bool = False, corona: bool = False) -> HomeTheme:
    """Compact HomeTheme: derives the ground-top colour and the logo gradient from the accent."""
    return HomeTheme(key, name, blurb, tuple(zip((0, 0.28, 0.62, 1), sky4)), far, ground, mix(ground, accent, 0.55), accent,
                     mix(accent, (255, 255, 255), 0.65), accent, fx, hero, ammo, stars, aurora, corona, layers)


def _ht(key: str, name: str, blurb: str, sky4: tuple, far: RGB, ground: RGB, top: RGB, accent: RGB, la: RGB, lb: RGB, fx: str,
        hero: str, ammo: str, stars: bool = True, aurora: bool = False, corona: bool = False) -> HomeTheme:
    sky = tuple(zip((0, 0.28, 0.62, 1), sky4))
    return HomeTheme(key, name, blurb, sky, far, ground, top, accent, la, lb, fx, hero, ammo, stars, aurora, corona)


HOME_THEMES: dict[str, HomeTheme] = {t.key: t for t in (
    # --- shop, row 1 (cheap) ---------------------------------------------------------------------------------
    _ht("SUNSET", "SUNSET RIDGE", "A warm dusk with drifting fireflies.", ((255, 170, 100), (230, 100, 110), (90, 40, 110), (14, 10, 40)),
        (70, 36, 84), (30, 16, 34), (150, 100, 70), (255, 170, 110), (255, 225, 160), (230, 110, 90), "fireflies", "ranger", "blazer"),
    _ht("MEADOW", "MEADOW MORN", "Green hills under a soft morning haze.", ((200, 240, 200), (120, 200, 180), (40, 90, 130), (10, 20, 50)),
        (40, 90, 70), (24, 50, 34), (110, 170, 90), (150, 235, 150), (230, 255, 200), (90, 200, 120), "fireflies", "mule", "blazer"),
    _ht("DESERT", "DESERT NOON", "Hot sand and drifting dust.", ((255, 210, 140), (240, 150, 90), (120, 70, 90), (20, 14, 40)),
        (120, 70, 60), (80, 50, 36), (210, 160, 100), (255, 200, 120), (255, 240, 190), (220, 140, 70), "ash", "scout", "dasher", False),
    _ht("HARBOR", "BLUE HARBOR", "Dusk over still water.", ((255, 200, 170), (150, 160, 200), (50, 70, 130), (8, 12, 40)),
        (40, 56, 100), (20, 30, 56), (110, 140, 190), (150, 190, 255), (225, 240, 255), (100, 150, 230), "", "badger", "fizz"),
    _ht("AUTUMN", "AUTUMN LEAVES", "Amber light and falling ash.", ((255, 190, 110), (200, 110, 70), (80, 40, 60), (16, 8, 30)),
        (80, 40, 40), (40, 24, 24), (170, 100, 50), (255, 170, 80), (255, 230, 170), (210, 100, 50), "ash", "pioneer", "slag"),
    # --- shop, row 2 (mid) -----------------------------------------------------------------------------------
    _ht("ARCTIC", "ARCTIC NIGHT", "Falling snow under a freezing sky.", ((190, 230, 255), (100, 160, 220), (30, 56, 120), (6, 10, 36)),
        (40, 62, 104), (30, 48, 80), (190, 215, 235), (150, 220, 255), (230, 250, 255), (90, 170, 230), "snow", "glacier", "cryo"),
    _ht("JUNGLE", "JUNGLE CANOPY", "Humid green dusk full of fireflies.", ((190, 255, 170), (80, 190, 120), (20, 70, 80), (4, 16, 24)),
        (20, 70, 60), (10, 36, 30), (70, 150, 80), (120, 255, 150), (230, 255, 190), (60, 200, 110), "fireflies", "mantis", "toxin"),
    _ht("NEON", "NEON CITY", "A synthwave skyline in violet and pink.", ((255, 120, 200), (160, 60, 200), (50, 20, 110), (6, 4, 28)),
        (40, 20, 80), (14, 10, 34), (200, 60, 220), (255, 100, 230), (255, 230, 255), (220, 70, 230), "stardust", "warhound", "photon"),
    _ht("DEEPSEA", "DEEP SEA", "Cold currents and drifting plankton.", ((120, 230, 255), (40, 140, 200), (10, 50, 110), (2, 6, 30)),
        (10, 50, 100), (6, 24, 56), (60, 140, 190), (90, 220, 255), (210, 250, 255), (40, 170, 230), "snow", "leviathan", "ripple"),
    _ht("STORMFRONT", "STORM FRONT", "A bruised sky about to break.", ((190, 200, 220), (100, 110, 150), (36, 40, 70), (6, 6, 20)),
        (46, 50, 80), (22, 24, 40), (120, 130, 170), (170, 190, 255), (235, 240, 255), (120, 140, 230), "ash", "centurion", "ripple", False),
    # --- shop, row 3 (pro) -----------------------------------------------------------------------------------
    _ht("MOLTEN", "MOLTEN CORE", "A furnace sky raining embers.", ((255, 140, 50), (200, 50, 30), (70, 16, 24), (10, 2, 8)),
        (60, 16, 20), (24, 10, 12), (190, 70, 30), (255, 140, 60), (255, 240, 160), (220, 70, 30), "embers", "behemoth", "molten", False),
    _ht("CRYSTALCAVE", "CRYSTAL CAVE", "A violet aurora over glittering stone.", ((220, 180, 255), (140, 100, 220), (50, 30, 120), (6, 4, 30)),
        (60, 40, 120), (24, 16, 56), (180, 140, 255), (200, 160, 255), (245, 230, 255), (150, 100, 255), "stardust", "nightshade", "glacial",
        True, True),
    _ht("BLOODMOON", "BLOOD MOON", "A crimson night and drifting ash.", ((255, 120, 110), (190, 40, 60), (60, 10, 40), (8, 2, 14)),
        (50, 10, 30), (20, 6, 14), (150, 30, 50), (255, 90, 100), (255, 220, 200), (210, 40, 60), "ash", "reaper", "molten"),
    _ht("EMERALD", "EMERALD AURORA", "Green ribbons over snowy hills.", ((160, 255, 210), (40, 170, 150), (10, 60, 90), (2, 8, 26)),
        (10, 60, 80), (6, 26, 40), (60, 170, 150), (100, 255, 200), (220, 255, 240), (40, 220, 170), "snow", "banshee", "frostbite", True, True),
    _ht("GOLDENDUNES", "GOLDEN DUNES", "Sunset-lit dunes and rising embers.", ((255, 230, 150), (255, 150, 70), (120, 50, 70), (16, 6, 24)),
        (90, 40, 50), (30, 14, 20), (220, 140, 60), (255, 190, 90), (255, 245, 200), (240, 140, 50), "embers", "behemoth", "cinder"),
    # --- shop, row 4 (master tier) ---------------------------------------------------------------------------
    _ht("ECLIPSE", "TOTAL ECLIPSE", "A black sun, a prismatic corona and a river of stars.",
        ((255, 200, 120), (150, 60, 160), (40, 16, 90), (2, 2, 16)), (30, 14, 56), (10, 6, 20), (190, 150, 230), (210, 170, 255),
        (255, 236, 170), (190, 120, 255), "stardust", "emperor", "helix", True, True, True),
    _ht("VOIDRIFT", "VOID RIFT", "A tear in space, ringed by violet fire.", ((120, 60, 200), (50, 20, 120), (14, 6, 50), (0, 0, 10)),
        (20, 10, 50), (6, 4, 16), (120, 80, 220), (170, 120, 255), (230, 210, 255), (130, 70, 255), "stardust", "eternal", "eclipse",
        True, True, True),
    _ht("SOLARCROWN", "SOLAR CROWN", "A burning corona over a scorched world.", ((255, 250, 200), (255, 190, 90), (210, 80, 60), (30, 6, 30)),
        (110, 40, 40), (34, 10, 20), (255, 200, 90), (255, 220, 120), (255, 252, 220), (255, 170, 60), "embers", "celestial", "supernova",
        True, False, True),
    _ht("STARFALL", "STARFALL", "A river of stars falling through aurora.", ((150, 200, 255), (70, 90, 200), (20, 20, 100), (2, 2, 24)),
        (24, 24, 90), (8, 8, 36), (120, 160, 255), (160, 200, 255), (235, 245, 255), (100, 140, 255), "stardust", "dynasty", "nebula",
        True, True),
    _ht("CELESTIAL", "CELESTIAL GATE", "A gate of light beyond the clouds.", ((255, 230, 255), (220, 150, 255), (90, 50, 160), (8, 4, 36)),
        (70, 40, 130), (24, 14, 52), (240, 200, 255), (250, 200, 255), (255, 245, 255), (230, 140, 255), "fireflies", "overlord", "bloom",
        True, True, True),
    # --- new shop screens: each pairs a palette with scenery LAYERS so they differ in structure, not just colour ---------
    _hx("DAWN", "FIRST LIGHT", "A low sun over blue mountains.", ((255, 220, 160), (250, 160, 150), (110, 110, 170), (20, 24, 64)), (70, 80, 130), (28, 34, 60), (255, 190, 130), "", "scout", "slug", ("sun", "mountains")),
    _hx("PRAIRIE", "OPEN PRAIRIE", "Pine ridge and a warm evening sun.", ((255, 230, 170), (255, 180, 110), (130, 90, 120), (24, 20, 52)), (60, 70, 70), (26, 34, 30), (255, 200, 120), "fireflies", "mule", "lob", ("sun", "trees")),
    _hx("HARVEST", "HARVEST MOON", "A huge orange moon over dark pines.", ((255, 190, 100), (190, 100, 70), (70, 36, 60), (10, 8, 28)), (50, 30, 44), (20, 14, 22), (255, 160, 70), "ash", "badger", "dud", ("moon", "trees")),
    _hx("LAKESIDE", "QUIET LAKESIDE", "Still water below a mountain range.", ((190, 225, 255), (120, 170, 220), (50, 90, 150), (8, 16, 44)), (50, 80, 120), (18, 34, 60), (140, 200, 255), "", "turtle", "breeze", ("mountains", "ocean")),
    _hx("CAMPFIRE", "CAMPFIRE NIGHT", "Embers rising into a starry dark.", ((60, 50, 90), (40, 30, 70), (20, 16, 44), (4, 4, 18)), (24, 20, 40), (12, 10, 22), (255, 150, 60), "embers", "pioneer", "pebbles", ("trees", "moon")),
    _hx("TWILIGHT", "TWILIGHT TOWN", "A small town as the lights come on.", ((255, 170, 150), (190, 110, 150), (80, 60, 130), (12, 12, 44)), (50, 40, 80), (20, 18, 40), (255, 180, 160), "", "pebble", "skipper", ("moon", "city")),
    _hx("FOGBANK", "FOGBANK", "Rain through grey mist.", ((170, 180, 190), (120, 130, 150), (60, 70, 96), (14, 18, 32)), (70, 80, 100), (30, 36, 50), (190, 210, 230), "", "rusty", "twister", ("rain", "mountains"), False),
    _hx("SPRING", "SPRING MEADOW", "Green hills and a pale sun.", ((235, 250, 220), (170, 220, 190), (80, 150, 170), (20, 50, 90)), (60, 110, 90), (24, 52, 40), (170, 240, 150), "fireflies", "hopper", "borer", ("sun", "trees")),
    _hx("MARKET", "NIGHT MARKET", "Glowing windows in a crowded old town.", ((255, 200, 120), (180, 110, 110), (70, 50, 100), (10, 8, 32)), (60, 40, 70), (24, 18, 34), (255, 210, 110), "", "spud", "scatter", ("city", "moon")),
    _hx("MONSOON", "MONSOON", "Warm rain over a dark sea.", ((120, 150, 170), (70, 100, 130), (30, 50, 84), (6, 10, 28)), (30, 50, 80), (12, 22, 44), (130, 200, 240), "", "crate", "mudball", ("rain", "ocean"), False),
    _hx("SYNTHWAVE", "SYNTHWAVE", "A perspective grid under a striped sun.", ((255, 120, 190), (180, 60, 190), (60, 20, 120), (8, 4, 30)), (50, 20, 90), (14, 8, 34), (255, 90, 220), "", "outrider", "zigzag", ("sun", "grid")),
    _hx("TOKYO", "NEON RAIN", "A rain-soaked megacity.", ((90, 60, 150), (60, 36, 120), (30, 20, 80), (6, 4, 26)), (30, 24, 70), (12, 10, 30), (255, 110, 200), "", "marauder", "triskip", ("city", "rain")),
    _hx("PYRAMIDS", "PYRAMIDS", "Golden pyramids at dusk.", ((255, 220, 140), (250, 160, 90), (150, 80, 90), (24, 14, 44)), (110, 70, 60), (44, 28, 30), (255, 200, 110), "ash", "sandstorm", "rainmaker", ("sun", "pyramids"), False),
    _hx("TUNDRA", "TUNDRA", "Snow driven across pale mountains.", ((220, 235, 250), (160, 190, 225), (80, 110, 170), (14, 24, 56)), (110, 140, 180), (60, 80, 110), (190, 225, 255), "snow", "barracuda", "fireworks", ("mountains",)),
    _hx("CANYONS", "RED CANYONS", "Layered rock under a hot sun.", ((255, 210, 150), (240, 130, 90), (140, 60, 80), (24, 12, 40)), (130, 60, 56), (46, 24, 26), (255, 150, 90), "ash", "raider", "walker", ("sun", "mountains"), False),
    _hx("LAGOON", "MOON LAGOON", "A silver moon over a glassy lagoon.", ((150, 210, 235), (90, 150, 200), (30, 70, 130), (4, 10, 36)), (30, 60, 100), (10, 22, 46), (160, 230, 250), "fireflies", "stingray", "timecharge", ("moon", "ocean")),
    _hx("THUNDERHEAD", "THUNDERHEAD", "Lightning inside a boiling storm.", ((120, 120, 150), (70, 70, 110), (30, 30, 66), (6, 6, 22)), (36, 36, 70), (14, 14, 32), (200, 210, 255), "", "trident", "rockslide", ("lightning", "rain"), False),
    _hx("STEAMTOWN", "STEAMTOWN", "Smoking chimneys under a copper sky.", ((230, 170, 110), (170, 110, 90), (80, 56, 70), (14, 10, 26)), (66, 46, 54), (24, 18, 22), (255, 180, 100), "ash", "redoubt", "fortify", ("city",), False),
    _hx("ORBITRING", "RINGED WORLD", "A banded planet hangs overhead.", ((90, 100, 190), (50, 60, 140), (24, 24, 80), (4, 4, 24)), (30, 34, 90), (10, 12, 36), (150, 180, 255), "stardust", "harpoon", "tunneler", ("planet",)),
    _hx("BAMBOO", "BAMBOO RAIN", "Green stalks and soft rain.", ((190, 235, 200), (110, 190, 160), (40, 110, 110), (6, 24, 44)), (40, 90, 80), (14, 40, 34), (150, 240, 170), "", "cobra", "glasser", ("trees", "rain"), False),
    _hx("NIGHTCITY", "NIGHT CITY", "Towers, rain and a moon behind the clouds.", ((90, 70, 130), (50, 40, 100), (24, 20, 66), (4, 4, 22)), (30, 26, 64), (10, 10, 26), (170, 150, 255), "", "tempestor", "warpshell", ("moon", "city", "rain")),
    _hx("LAVAFALLS", "LAVA FALLS", "Burning peaks and a rain of embers.", ((255, 150, 60), (200, 60, 30), (80, 20, 24), (12, 2, 8)), (70, 20, 20), (24, 8, 10), (255, 130, 50), "embers", "hellcat", "tremor", ("mountains",), False),
    _hx("VAPORWAVE", "VAPORWAVE", "A pastel sun and a drifting planet.", ((255, 190, 230), (200, 140, 240), (110, 90, 200), (20, 20, 70)), (90, 70, 160), (24, 20, 56), (255, 150, 230), "", "typhoon", "gravmine", ("sun", "grid", "planet")),
    _hx("DEADSEA", "DEAD SEA", "A pale moon on black water.", ((120, 140, 170), (70, 90, 130), (30, 40, 80), (4, 6, 22)), (30, 40, 70), (8, 12, 28), (170, 200, 235), "ash", "gravedigger", "hailfall", ("moon", "ocean")),
    _hx("SKYFORT", "SKY FORTRESS", "A needle tower above the clouds.", ((200, 220, 255), (130, 160, 230), (60, 80, 160), (8, 12, 48)), (60, 80, 140), (22, 30, 60), (190, 220, 255), "snow", "warmaster", "ricochet", ("tower", "comet")),
    _hx("CYBERPUNK", "CYBERPUNK", "Glitching skyline in acid rain.", ((120, 255, 200), (50, 180, 170), (20, 70, 110), (2, 8, 28)), (20, 70, 90), (6, 22, 34), (80, 255, 200), "", "shatterer", "detonator", ("city", "rain", "glitch"), True, True),
    _hx("MAELSTROM", "MAELSTROM", "Black water lit by lightning.", ((90, 110, 140), (50, 70, 110), (20, 34, 70), (4, 6, 22)), (24, 34, 66), (8, 12, 30), (160, 200, 255), "", "stormrider", "colossus", ("ocean", "lightning", "rain"), False),
    _hx("ICEPALACE", "ICE PALACE", "A spire of ice under green aurora.", ((190, 250, 235), (100, 200, 200), (30, 100, 140), (4, 14, 48)), (40, 100, 130), (14, 36, 56), (160, 255, 230), "snow", "ironclad", "phantomorb", ("tower", "mountains"), True, True),
    _hx("SOLARSTORM", "SOLAR STORM", "A giant sun and crackling bolts.", ((255, 250, 190), (255, 190, 90), (210, 90, 60), (40, 10, 30)), (130, 50, 40), (36, 12, 20), (255, 220, 120), "embers", "bloodhound", "meteor", ("sun", "lightning")),
    _hx("GHOSTTOWN", "GHOST TOWN", "Empty windows under a cold moon.", ((150, 160, 190), (90, 100, 140), (40, 46, 84), (6, 8, 28)), (40, 46, 80), (12, 14, 30), (190, 205, 240), "ash", "sandshark", "sunder", ("moon", "city"), True),
    _hx("QUASAR", "QUASAR", "A ringed world and a cosmic jet.", ((200, 160, 255), (120, 80, 220), (50, 30, 140), (4, 2, 28)), (50, 36, 120), (12, 8, 40), (190, 150, 255), "stardust", "ascendant", "vortex", ("planet", "comet"), True, True, True),
    _hx("GALAXY", "SPIRAL GALAXY", "Comets crossing a violet galaxy.", ((130, 140, 255), (80, 70, 200), (36, 30, 120), (2, 2, 24)), (36, 34, 110), (10, 10, 40), (170, 180, 255), "stardust", "seraphim", "skyfall", ("planet", "comet"), True, True),
    _hx("NEBULAR", "NEBULA SEA", "Pink and teal clouds in deep space.", ((255, 160, 220), (160, 90, 210), (50, 70, 170), (2, 4, 30)), (50, 60, 150), (12, 14, 50), (255, 160, 230), "stardust", "doombringer", "wormhole", ("comet",), True, True),
    _hx("HYPERGATE", "HYPERGATE", "A tower of light and a streaming grid.", ((120, 255, 255), (50, 160, 220), (20, 60, 150), (2, 6, 34)), (24, 60, 130), (6, 14, 40), (120, 240, 255), "stardust", "starforge", "chainreact", ("tower", "grid", "comet"), True, True),
    _hx("BLACKHOLE", "EVENT HORIZON", "A black sun ringed in fire.", ((255, 200, 120), (180, 70, 90), (60, 24, 100), (2, 2, 18)), (30, 14, 56), (8, 6, 20), (255, 180, 120), "stardust", "arcanist", "faultline", ("planet", "comet"), True, True, True),
    _hx("AURORAKING", "AURORA KING", "Great ribbons over snowy peaks.", ((140, 255, 190), (60, 190, 170), (20, 90, 150), (2, 8, 40)), (24, 80, 120), (8, 24, 46), (130, 255, 190), "snow", "voidwalker", "mirrorshot", ("mountains", "comet"), True, True),
    _hx("CRYSTALPEAK", "CRYSTAL PEAK", "Glittering peaks and floating stars.", ((220, 190, 255), (150, 120, 240), (60, 50, 160), (6, 4, 36)), (60, 50, 150), (16, 14, 52), (210, 180, 255), "stardust", "sunbreaker", "overkill", ("mountains", "moon"), True, True),
    _hx("DRAGONSKY", "DRAGON SKY", "A storm over burning mountains.", ((255, 150, 110), (190, 70, 90), (70, 30, 100), (8, 4, 30)), (70, 28, 80), (20, 10, 34), (255, 150, 120), "embers", "templar", "cataclysm", ("mountains", "lightning"), True, True),
    _hx("CELESTIA", "CELESTIA", "A golden gate beyond the clouds.", ((255, 245, 200), (255, 200, 140), (200, 130, 190), (30, 20, 80)), (130, 90, 170), (40, 28, 70), (255, 230, 150), "fireflies", "monolith", "prismrain", ("tower", "sun", "comet"), True, True, True),
    _hx("ASTRALSEA", "ASTRAL SEA", "A starlit sea beneath ribbons of light.", ((140, 200, 255), (80, 130, 230), (30, 60, 160), (2, 6, 40)), (30, 60, 140), (8, 16, 50), (150, 210, 255), "stardust", "phoenix", "maelstrom", ("ocean", "comet", "planet"), True, True),
    # --- terminal ticket rewards ---------------------------------------------------------------------------------
    _hx("VOLTCITY", "VOLT CITY", "A storm over a city of towers.", ((90, 110, 190), (50, 60, 150), (24, 24, 90), (4, 4, 30)), (30, 34, 90), (10, 10, 36), (150, 210, 255), "", "voltsovereign", "arcstorm", ("city", "lightning", "rain")),
    _hx("CLOCKWORKS", "CLOCKWORKS", "A giant tower ticking over a copper town.", ((255, 220, 150), (210, 150, 90), (110, 76, 70), (20, 14, 30)), (90, 66, 60), (30, 22, 26), (255, 215, 120), "", "magistrate", "gavel", ("city", "tower")),
    _hx("FROSTCATHEDRAL", "FROST CATHEDRAL", "An ice cathedral beneath green aurora.", ((190, 250, 235), (100, 200, 210), (30, 100, 150), (4, 14, 50)), (40, 100, 140), (14, 36, 60), (160, 255, 235), "snow", "regent", "coldfront", ("tower", "mountains"), True, True),
    _hx("SUNTEMPLE", "SUN TEMPLE", "A temple at sunset.", ((255, 230, 150), (255, 160, 90), (200, 80, 90), (30, 12, 40)), (120, 56, 56), (36, 16, 24), (255, 210, 100), "embers", "solarch", "sunburst", ("sun", "pyramids")),
    _hx("SUNKENCITY", "SUNKEN CITY", "Drowned towers in black water.", ((120, 190, 220), (60, 120, 170), (20, 60, 120), (2, 8, 36)), (24, 60, 110), (8, 18, 44), (110, 210, 255), "fireflies", "abyssal", "undertow", ("city", "ocean")),
    _hx("PYRAMIDPLAIN", "PYRAMID PLAIN", "Pyramids and a huge moon.", ((255, 220, 160), (230, 150, 100), (130, 80, 110), (20, 14, 44)), (110, 70, 70), (40, 26, 34), (255, 215, 130), "ash", "pharaoh", "duneshot", ("moon", "pyramids")),
    _hx("CHEMWORKS", "CHEMICAL WORKS", "Smokestacks in acid rain.", ((170, 220, 120), (110, 160, 100), (50, 80, 80), (8, 18, 30)), (50, 80, 60), (18, 28, 24), (170, 255, 100), "ash", "baron", "plague", ("city", "rain"), False),
    _hx("GOTHICKEEP", "GOTHIC KEEP", "A castle against a stormy moon.", ((150, 130, 190), (90, 70, 150), (40, 30, 90), (6, 4, 30)), (40, 30, 80), (12, 10, 30), (190, 160, 255), "", "shadowlord", "umbra", ("city", "moon", "lightning")),
    _hx("ORBITALCITY", "ORBITAL CITY", "A space-station city and a distant world.", ((140, 200, 255), (70, 120, 230), (30, 50, 150), (2, 4, 36)), (30, 50, 140), (8, 14, 46), (150, 210, 255), "stardust", "overseer", "starshower", ("planet", "tower", "comet"), True, True),
    _hx("PAGODAPEAK", "PAGODA PEAK", "Pagodas above the clouds.", ((255, 200, 190), (210, 140, 160), (100, 80, 140), (14, 12, 44)), (80, 66, 120), (24, 20, 46), (255, 190, 170), "snow", "warlord", "dragonfire", ("mountains", "trees", "moon")),
    # --- mission rewards ---------------------------------------------------------------------------------------
    _ht("OUTPOST", "FORWARD OUTPOST", "A lonely radio mast on a cold ridge.", ((200, 215, 225), (110, 130, 150), (40, 54, 70), (8, 12, 22)),
        (44, 56, 70), (24, 32, 42), (130, 150, 160), (170, 210, 230), (235, 245, 250), (110, 160, 190), "ash", "wardog", "bandit"),
    _ht("RADAR", "RADAR STATION", "Green sweeps across a dark horizon.", ((120, 255, 150), (40, 160, 90), (10, 60, 50), (2, 10, 14)),
        (12, 56, 44), (6, 26, 22), (60, 170, 110), (100, 255, 150), (220, 255, 230), (40, 200, 110), "fireflies", "spitfire", "salvo"),
    _ht("WARZONE", "WARZONE", "Burning horizon, smoke and sparks.", ((255, 170, 90), (190, 70, 40), (60, 24, 24), (10, 6, 10)),
        (50, 24, 22), (22, 12, 12), (150, 80, 50), (255, 150, 80), (255, 235, 190), (210, 80, 40), "embers", "blitz", "hailstorm", False),
    _ht("ORBITAL", "ORBITAL RING", "A station ring above a blue world.", ((140, 200, 255), (60, 110, 220), (20, 30, 110), (2, 2, 20)),
        (30, 44, 100), (10, 14, 44), (130, 180, 255), (150, 210, 255), (230, 245, 255), (90, 150, 255), "stardust", "judge", "meltdown",
        True, False, True),
    _ht("DEEPSPACE", "DEEP SPACE", "A violet nebula and a million stars.", ((200, 150, 255), (110, 60, 190), (36, 20, 100), (2, 2, 18)),
        (40, 24, 90), (12, 8, 36), (170, 130, 255), (200, 160, 255), (245, 230, 255), (150, 100, 255), "stardust", "hydra", "oblivion",
        True, True),
    _ht("INVASION", "ALIEN INVASION", "Saucers over the horizon, green fire in the sky.",
        ((150, 255, 170), (50, 190, 110), (24, 70, 100), (2, 8, 20)), (16, 50, 60), (6, 20, 28), (80, 220, 140), (120, 255, 170),
        (230, 255, 220), (60, 220, 150), "stardust", "ufo", "xenon", True, True, True),
)}
HOME_THEMES.update({t.key: t for t in (
    _hx("GLASSDESERT", "GLASS DESERT", "Twin suns over dunes of fused glass.", ((255, 214, 150), (255, 140, 112), (96, 44, 130), (14, 6, 40)), (112, 62, 104), (36, 22, 48),
        (150, 236, 246), "stardust", "prismatic_king", "prismrain", ("twinsuns",), True),
    _hx("LANTERNFEST", "LANTERN FEST", "A night festival: a thousand lanterns rising.", ((255, 190, 120), (214, 84, 70), (70, 24, 56), (8, 6, 30)), (64, 24, 44), (28, 14, 22),
        (255, 190, 100), "fireflies", "phoenix", "cataclysm", ("lanterns",), True),
    _hx("WORLDTREE", "WORLDTREE", "A colossal tree whose veins glow with starlight.", ((150, 255, 220), (60, 190, 150), (14, 70, 70), (2, 12, 24)), (10, 54, 56), (8, 24, 28),
        (120, 250, 180), "fireflies", "stormbringer", "maelstrom", ("worldtree",), True, True),
    _hx("SKYWHALE", "SKY WHALE", "A sky-whale as big as a moon drifts through the stars.", ((190, 220, 255), (70, 120, 200), (20, 44, 100), (2, 6, 24)), (16, 40, 92), (8, 14, 36),
        (170, 220, 255), "stardust", "singularity", "eventhorizon", ("whale",), True, True),
    _hx("SHATTERLINE", "SHATTERLINE", "A fractured sky of drifting, prismatic glass.", ((255, 230, 255), (180, 120, 255), (70, 34, 140), (8, 2, 24)), (56, 28, 104), (16, 8, 40),
        (226, 190, 255), "stardust", "chronos", "quantumecho", ("shards",), True, True),

    replace(_ht("EMBERSANCTUM", "EMBER SANCTUM", "A volcanic temple above a river of fire.", ((255, 170, 70), (220, 70, 24), (90, 20, 16), (14, 4, 8)),
                (60, 14, 12), (22, 8, 8), (255, 120, 40), (255, 150, 60), (255, 236, 170), (255, 110, 40), "inferno", "a_emberheart", "cinderfall"),
            layers=("volcano",)),
    replace(_ht("TEMPESTSPIRES", "TEMPEST SPIRES", "Floating isles in a world-ending storm.", ((150, 230, 250), (40, 130, 170), (14, 50, 90), (2, 10, 24)),
                (14, 40, 70), (6, 18, 30), (110, 200, 230), (120, 232, 255), (230, 252, 255), (80, 190, 240), "tempest", "a_tempest", "tempest_a",
                True, True), layers=("isles", "rain", "lightning")),
    replace(_ht("ASTRALRIFT", "ASTRAL RIFT", "A ring of light and dark at the end of time.", ((255, 226, 160), (150, 90, 230), (40, 16, 100), (2, 0, 14)),
                (36, 20, 80), (8, 4, 26), (255, 226, 150), (214, 170, 255), (255, 248, 214), (170, 120, 255), "rift", "a_umbral", "totality",
                True, True, True), layers=("riftring",)),
    replace(_ht("SOLARIS", "SOLARIS", "A sun that never sets over a sea of gold.", ((255, 248, 210), (255, 196, 90), (200, 110, 40), (40, 16, 10)),
                (120, 60, 24), (30, 14, 8), (255, 210, 90), (255, 226, 130), (255, 252, 226), (255, 190, 70), "stardust", "a_solaris", "sunfall",
                True, False, True), layers=("sunburst",)),
    replace(_ht("CHRONOS", "CHRONOS", "Time itself, caught in an hourglass of gold.", ((255, 236, 190), (190, 130, 60), (70, 40, 40), (8, 4, 10)),
                (70, 40, 30), (16, 10, 8), (226, 176, 70), (255, 236, 176), (255, 250, 232), (230, 170, 70), "stardust", "a_chronos", "timerift",
                True, False, True), layers=("hourglass",)),
    replace(_ht("ETERNAL", "ETERNAL", "The legend's own sanctum. The universe is safe. Because of you.", ((255, 250, 230), (255, 206, 110), (120, 70, 190), (4, 2, 20)),
                (70, 40, 120), (14, 8, 30), (255, 226, 150), (255, 230, 150), (255, 255, 244), (255, 200, 90), "rift", "a_martyr", "timerift",
                True, True, False), layers=("sanctum",)),
)})
assert set(HOME_ORDER) == {"CLASSIC", "MASTER", "SECRET"} | set(HOME_THEMES) and len(HOME_ORDER) == len(set(HOME_ORDER))


# ---------------------------------------------------------------------------
# Shop catalogue: five pages, four tiers (cheap / mid / pro / master-tier). Master-tier gear costs coins AND credits.
# ---------------------------------------------------------------------------
@dataclass(frozen=True)
class ShopItem:
    uid: str
    page: int           # 0 tanks, 1 ammo, 2 main screens, 3 maps, 4 extras
    tier: int           # 0 cheap, 1 mid, 2 pro, 3 master-tier
    name: str
    blurb: str
    coins: int
    credits: int = 0


SHOP_PAGES = ("TANKS", "AMMO", "MAIN SCREEN", "MAPS", "EXTRAS")
TIER_COLORS = ((170, 205, 175), (120, 190, 255), (255, 165, 90), (255, 206, 100), (255, 70, 140))   # tier 4 = premium
TIER_SIZE = 15                                         # items per ordinary tier; anything after tier 3 is the premium tier
_TIER_RANGE = ((60, 190), (200, 650), (450, 1300), (800, 2400))          # cheapest..dearest coin price inside each tier
PREMIUM_COINS = (5000, 6500, 8000, 9500, 11000)
PREMIUM_CREDITS = (40, 50, 60, 70, 80)
RGB_PRICE = (1000, 14)
PRISMATIC_PRICE = (45000, 450)        # ultra expensive: the Prismatic King's refracting crystal as paint for ANY tank and shell
VOID_PRICE = (30000, 300)             # the Singularity's void as paint for ANY tank and shell
TICKET_PRICE = (14000, 120)
MARTYR_PRICE = (60000, 400)


def tier_of(index: int) -> tuple:
    """(tier, position-in-tier) of the n-th item of a shop page: 15 per ordinary tier, then the premium tier."""
    tier = min(index // TIER_SIZE, 4)
    return tier, index - tier * TIER_SIZE


def tier_price(tier: int, j: int) -> tuple:
    """(coins, credits) for item j of a tier. Only master-tier (3) and premium (4) gear costs credits."""
    if tier >= 4:
        return PREMIUM_COINS[j % 5], PREMIUM_CREDITS[j % 5]
    lo, hi = _TIER_RANGE[tier]
    return int(round((lo + (hi - lo) * j / (TIER_SIZE - 1)) / 5) * 5), (10 + 2 * j if tier == 3 else 0)


def _build_shop() -> tuple[ShopItem, ...]:
    items: list[ShopItem] = []

    def grid(page: int, rows: list) -> None:          # rows: [(uid, name, blurb)] in tier order
        for i, (uid, name, blurb) in enumerate(rows):
            tier, j = tier_of(i)
            items.append(ShopItem(uid, page, tier, name, blurb, *tier_price(tier, j)))
    grid(0, [(f"tank:{d.key}", d.name, d.blurb) for d in SHOP_DESIGNS])
    grid(1, [(f"ammo:{a.key}", a.name, a.blurb) for a in SHOP_AMMOS])
    grid(2, [(f"theme:{k.lower()}", HOME_THEMES[k].name, HOME_THEMES[k].blurb) for k in SHOP_THEMES])
    grid(3, [(f"map:{m.key}", m.name, m.description) for m in SHOP_MAPS])
    items.append(ShopItem("rgb:hull", 4, 3, "RGB HULL", "Your hull cycles through the whole rainbow.", *RGB_PRICE))
    items.append(ShopItem("rgb:shell", 4, 3, "RGB SHELL", "Your shells, trails and blasts cycle through the rainbow.", *RGB_PRICE))
    items.append(ShopItem("paint:prismatic", 4, 4, "PRISMATIC PAINT", "Paint ANY tank (and shells) in the Prismatic King's living crystal: "
                          "every facet splits the light.", *PRISMATIC_PRICE))
    items.append(ShopItem("paint:void", 4, 4, "VOID PAINT", "Paint ANY tank (and shells) in the Singularity's void: a hull that eats the light.", *VOID_PRICE))
    items.append(ShopItem("ticket", 4, 4, "TERMINAL TICKET", "Summons a random boss from the vault. Win to claim its tank, ammo, map and "
                          "main screen. Each boss appears once.", *TICKET_PRICE))
    items.append(ShopItem("timemachine", 4, 4, "MARTYR'S TIME MACHINE", "An ancient artifact that wakes only when every vault boss has fallen. "
                          "Something inside is waiting for you.", *MARTYR_PRICE))
    return tuple(items)


SHOP_ITEMS: tuple[ShopItem, ...] = _build_shop()
SHOP_BY_UID = {i.uid: i for i in SHOP_ITEMS}
assert len(SHOP_BY_UID) == len(SHOP_ITEMS)
assert all((i.credits > 0) == (i.tier >= 3) for i in SHOP_ITEMS)         # exactly the master-tier and premium gear costs credits


# ---------------------------------------------------------------------------
# Missions: 50 numbered challenges, each unlocked by clearing the one before (like the Tournament, but with variety).
#   marathon  win N matches in a row (losing any match fails it)        2v1    you against two AI tanks
#   ufo       one huge hovering saucer with a big health pool             coop   two players (local, LAN or + AI wingman)
#   rush      beat a string of Tournament bosses, one round each          mother the final saucer fleet (Grand Prize)
# Coins only (never credits). First clear pays much more than repeats; every 5th mission also hands out kit rewards.
# ---------------------------------------------------------------------------
@dataclass(frozen=True)
class Mission:
    number: int
    name: str
    kind: str                    # marathon | 2v1 | ufo | coop | rush | mother
    diff: Difficulty
    diff_end: Difficulty         # marathons ramp from diff to diff_end
    stages: int                  # matches (marathon) / bosses (rush) / 1
    target: int                  # rounds to win per stage
    fail_at: int                 # enemy round wins in a stage that fail the mission
    hp: int                      # hearts each human starts with
    enemies: int = 1
    enemy_hp: int = 3
    ufo_hp: int = 0
    prec: float = 1.0
    levels: tuple = ()           # rush: Tournament level numbers
    coop_ufo: bool = False
    map_key: Optional[str] = None
    blurb: str = ""
    rewards: tuple = ()

    @property
    def is_coop(self) -> bool:
        return self.kind == "coop"

    @property
    def type_name(self) -> str:
        return {"marathon": "MARATHON", "2v1": "2 vs 1", "ufo": "UFO ASSAULT", "coop": "CO-OP", "rush": "BOSS RUSH",
                "mother": "MOTHERSHIP"}[self.kind]

    @property
    def objective(self) -> str:
        k = self.kind
        if k == "marathon":
            return f"WIN {self.stages} MATCHES IN A ROW"
        if k == "rush":
            return f"BEAT {self.stages} BOSSES, NO LOSSES"
        if k == "mother":
            return "DESTROY THE MOTHERSHIP FLEET"
        if k == "coop":
            return "TEAM UP: " + ("DOWN THE SAUCER" if self.coop_ufo else "BEAT BOTH TANKS") + f" ({self.target} RDS)"
        if k == "ufo":
            return f"DOWN THE SAUCER ({self.target} {'RD' if self.target == 1 else 'RDS'})"
        return f"BEAT BOTH TANKS - WIN {self.target} ROUNDS"

    @property
    def rule_text(self) -> str:
        k = self.kind
        if k == "marathon":
            return "LOSE ONE MATCH AND IT'S OVER"
        if k == "rush":
            return "LOSE ONE ROUND AND IT'S OVER"
        return f"{self.fail_at} LOST ROUNDS FAIL IT  ·  {self.hp} HEARTS"

    @property
    def first_coins(self) -> int:
        base = 40 + 3 * self.number
        if self.kind == "rush":
            base = int(base * 1.5)
        if self.kind == "mother":
            base += 400
        return base

    @property
    def repeat_coins(self) -> int:
        return int(self.first_coins * 0.4)


MISSION_REWARDS = {
    5: ("tank:wardog", "ammo:bandit"), 10: ("map:bunker-line", "theme:outpost"), 15: ("tank:spitfire", "ammo:salvo"),
    20: ("map:radar-hill", "theme:radar"), 25: ("tank:blitz", "ammo:hailstorm"), 30: ("map:shelled-field", "theme:warzone"),
    35: ("tank:judge", "ammo:meltdown"), 40: ("map:space-elevator", "theme:orbital"),
    45: ("tank:hydra", "ammo:oblivion", "map:asteroid-field", "theme:deepspace"),
    50: ("tank:ufo", "ammo:xenon", "map:mothership", "theme:invasion"),
}
_MARATHONS = {1: 3, 5: 3, 6: 3, 11: 3, 16: 4, 20: 4, 21: 4, 26: 5, 30: 5, 31: 5, 36: 6, 41: 7, 46: 10}
_RUSH = {40: tuple(range(1, 8)), 45: tuple(range(8, 15)), 47: tuple(range(1, 21))}
_UFO_NAMES = ("SCOUT", "DRONE", "SKIMMER", "HUNTER", "REAPER", "WARSHIP", "DESTROYER", "CRUISER", "DREADNOUGHT", "LEVIATHAN", "OVERMIND",
              "ARCHON")
_MARA_NAMES = ("WARM-UP RUN", "ROAD TO NOWHERE", "LONG HAUL", "IRON LEGS", "UNBROKEN", "ENDURANCE", "THE GRIND", "NO BRAKES", "STREAKER",
               "LAST MILE", "THE LONG ROAD", "FULL DISTANCE", "ULTRA")
_OUT_NAMES = ("OUTNUMBERED", "TWO ON ONE", "PINCER", "CROSSFIRE", "THE AMBUSH", "SURROUNDED", "DOUBLE TROUBLE", "NO ESCAPE", "BAD ODDS",
              "LAST STAND")
_COOP_NAMES = ("BUDDY SYSTEM", "TAG TEAM", "SIDE BY SIDE", "BACK TO BACK", "DUAL STRIKE", "WINGMEN", "IN SYNC", "TWIN FIRE", "ONE TEAM",
               "SHOULDER TO SHOULDER")


def _build_missions() -> tuple:
    out, counts = [], {"marathon": 0, "2v1": 0, "ufo": 0, "coop": 0}
    ufo_ranks = [n for n in range(1, 50) if (n % 5 == 3 or n in (15, 35)) and n not in _RUSH]
    tiers = (Difficulty.EASY, Difficulty.NORMAL, Difficulty.HARD)

    def tier(n: int) -> Difficulty:
        return tiers[0 if n <= 8 else 1 if n <= 18 else 2]

    for n in range(1, 51):
        prec = round(1.45 - 0.75 * (n - 1) / 49, 2)
        d = tier(n)
        rewards = MISSION_REWARDS.get(n, ())
        if n == 50:
            m = Mission(n, "THE MOTHERSHIP", "mother", Difficulty.HARD, Difficulty.HARD, 1, 1, 3, 9, 3, 4, 26, 0.8,
                        map_key="mothership", blurb="The alien flagship and its two drones. Everything you have learned, at once.")
        elif n in _RUSH:
            lv = _RUSH[n]
            m = Mission(n, {40: "BOSS RUSH I", 45: "BOSS RUSH II", 47: "THE GAUNTLET"}[n], "rush", Difficulty.HARD, Difficulty.MASTER,
                        len(lv), 1, 1, 4, levels=lv,
                        blurb="Every boss in a row, one round each, no healing. One loss ends the run." if n != 47 else
                        "All twenty Tournament bosses, back to back. The end-game test.")
        elif n in _MARATHONS:
            i = counts["marathon"] = counts["marathon"] + 1
            end = tiers[min(2, tiers.index(d) + 1)]
            m = Mission(n, _MARA_NAMES[(i - 1) % len(_MARA_NAMES)], "marathon", d, end, _MARATHONS[n], 2, 2, 3, prec=prec,
                        blurb=f"{_MARATHONS[n]} matches against fresh opponents. A lost match ends the marathon.")
        elif n in ufo_ranks:
            rk = ufo_ranks.index(n)
            counts["ufo"] += 1
            m = Mission(n, f"UFO: {_UFO_NAMES[rk % len(_UFO_NAMES)]}", "ufo", Difficulty.NORMAL if rk < 3 else Difficulty.HARD,
                        Difficulty.HARD, 1, 1 if rk < 6 else 2, 2 if rk < 6 else 3, 6 if rk < 4 else 7 if rk < 8 else 9, 1, 3,
                        5 + int(round(0.9 * rk)), prec, map_key="space-elevator" if rk >= 6 else None,
                        blurb="A huge saucer hovers above the field: shields, hops and three-shot volleys. Direct hits count double.")
        elif n % 5 == 4 or n in (25,):
            i = counts["coop"] = counts["coop"] + 1
            ufo = n >= 34
            m = Mission(n, _COOP_NAMES[(i - 1) % len(_COOP_NAMES)], "coop", d, d, 1, 2, 3, 5 if not ufo else 7, 2, 4,
                        10 + (n - 34) if ufo else 0, prec, coop_ufo=ufo,
                        blurb="Two players, one team. Play local, over LAN, or bring an AI wingman.")
        else:
            i = counts["2v1"] = counts["2v1"] + 1
            m = Mission(n, _OUT_NAMES[(i - 1) % len(_OUT_NAMES)], "2v1", d, d, 1, 2, 3, 6, 2, 3,
                        prec=prec, blurb="Two AI tanks hunt you. Pick them off one at a time.")
        out.append(replace(m, rewards=rewards))
    return tuple(out)


MISSIONS: tuple = _build_missions()
assert len(MISSIONS) == 50 and all(m.number == i + 1 for i, m in enumerate(MISSIONS))
assert MISSIONS[-1].kind == "mother" and all(m.diff is not Difficulty.MASTER or m.kind == "rush" for m in MISSIONS)
assert {"marathon", "2v1", "ufo", "coop", "rush", "mother"} == {m.kind for m in MISSIONS}
assert all(uid.split(":")[1] in {*(d.key for d in MISSION_DESIGNS), *(a.key for a in MISSION_AMMOS), *(x.key for x in MISSION_MAPS),
                                 *(k.lower() for k in MISSION_THEMES)} for r in MISSION_REWARDS.values() for uid in r)


def payout_state(level: TournamentLevel, cleared: bool) -> tuple:
    """(label, Payout) for the single reward that applies to a level right now: first clear until cleared, then replay."""
    return ("REPLAY", tournament_payout(level, True, False)) if cleared else ("FIRST CLEAR", tournament_payout(level, True, True))


def mission_payout(m: Mission, won: bool, first: bool) -> Payout:
    if not won:
        return Payout(COIN_HALF)
    return Payout(m.first_coins if first else m.repeat_coins)        # missions never pay credits


# ---------------------------------------------------------------------------
# Terminal Tickets: ten vault bosses. A ticket draws one that has not been beaten yet; beating it grants its tank, ammo, map
# and main screen. Beaten bosses leave the pool; with all ten beaten the ticket is sold out.
# ---------------------------------------------------------------------------
@dataclass(frozen=True)
class TicketBoss:
    key: str
    name: str
    tank: str
    ammo: str
    map_key: str
    theme: str
    hp: int
    prec: float
    taunt: str

    def reward_ids(self) -> tuple:
        return (f"tank:{self.tank}", f"ammo:{self.ammo}", f"map:{self.map_key}", f"theme:{self.theme.lower()}")


TICKET_BOSSES: tuple = (
    TicketBoss("volt", "VOLT SOVEREIGN", "voltsovereign", "arcstorm", "neon-skyline", "VOLTCITY", 9, 0.95, "THE CITY RUNS ON MY LIGHTNING."),
    TicketBoss("magistrate", "IRON MAGISTRATE", "magistrate", "gavel", "clock-plaza", "CLOCKWORKS", 9, 0.9, "YOUR TIME IS UP."),
    TicketBoss("regent", "GLACIER REGENT", "regent", "coldfront", "frozen-cathedral", "FROSTCATHEDRAL", 9, 0.9, "KNEEL IN THE COLD."),
    TicketBoss("solar", "SOLAR ARCHON", "solarch", "sunburst", "sun-temple", "SUNTEMPLE", 10, 0.88, "THE SUN ANSWERS TO ME."),
    TicketBoss("abyssal", "ABYSS LEVIATHAN", "abyssal", "undertow", "sunken-metropolis", "SUNKENCITY", 10, 0.88, "THE DEEP WILL TAKE YOU."),
    TicketBoss("pharaoh", "SAND PHARAOH", "pharaoh", "duneshot", "pyramid-plain", "PYRAMIDPLAIN", 10, 0.85, "THE DESERT KEEPS ITS DEAD."),
    TicketBoss("baron", "TOXIC BARON", "baron", "plague", "chemical-works", "CHEMWORKS", 11, 0.85, "BREATHE DEEP."),
    TicketBoss("shadow", "SHADOW EMPEROR", "shadowlord", "umbra", "gothic-keep", "GOTHICKEEP", 11, 0.82, "THE DARK IS MY THRONE."),
    TicketBoss("overseer", "COSMIC OVERSEER", "overseer", "starshower", "orbital-city", "ORBITALCITY", 12, 0.8, "I SEE EVERY SHOT."),
    TicketBoss("warlord", "DRAGON WARLORD", "warlord", "dragonfire", "pagoda-peak", "PAGODAPEAK", 12, 0.78, "BURN, ALL OF YOU."),
)
TICKET_BOSS_BY_KEY = {b.key: b for b in TICKET_BOSSES}
assert len(TICKET_BOSSES) == 10 and all(b.tank in DESIGN_BY_KEY and b.ammo in AMMO_BY_KEY and b.map_key in SPECIAL_MAP_BY_KEY
                                        and b.theme in HOME_THEMES for b in TICKET_BOSSES)
TICKET_PLAYER_HP, TICKET_WIN_COINS = 6, 600


# ---------------------------------------------------------------------------
# Custom tanks. Stored as self-describing records with STABLE component ids (never array indexes) and a format version, so a
# saved tank still resolves after the game gains new components, tanks or reorders its lists. Anything that has gone missing
# falls back to a sensible default for that slot instead of corrupting the save, and unknown extra fields are preserved.
# ---------------------------------------------------------------------------
CUSTOM_FMT = 1
CUSTOM_TANK_FEE = 2500
HOVER_COINS, HOVER_CREDITS = 25000, 50          # the very pricey HOVER upgrade for a custom tank
CUSTOM_TANK_MAX = 24
CUSTOM_SLOTS = ("turret", "hull", "track", "barrel", "glow", "ambient", "accent")
CUSTOM_FALLBACK = {"turret": "dome", "hull": "plain", "track": "std", "barrel": "single", "glow": "none", "ambient": "none", "accent": "WHITE"}
FREE_PARTS = {"turret": {"dome", "box", "twin", "wide", "pillbox"}, "hull": {"plain", "plate", "ramp", "wide", "hex"},
              "track": {"std", "full", "round", "tread"}, "barrel": {"single", "double", "heavy"},
              "glow": {"none", "pulse", "blink"}, "ambient": {"none", "sparks", "mist"}}
CUSTOM_DESIGNS: dict[str, TankDesign] = {}        # runtime registry: key "custom:<id>" (also mirrored into DESIGN_BY_KEY)


def component_tables() -> dict:
    """Every component id by slot: the single registry the workshop, the cheat screen and the validators all read."""
    return {"turret": list(_TURRETS), "hull": list(_HULLS), "track": list(_TRACKS), "barrel": list(BARRELS),
            "glow": list(GLOW_STYLES), "ambient": list(AMBIENT_KINDS), "accent": [n for n, _ in COLOR_CHOICES]}


def design_from_custom(rec: dict) -> TankDesign:
    """Builds a TankDesign from a stored record, repairing anything that no longer exists."""
    tables, p = component_tables(), (rec.get("parts") if isinstance(rec.get("parts"), dict) else {})
    pick = lambda slot: p.get(slot) if p.get(slot) in tables[slot] else CUSTOM_FALLBACK[slot]
    t, h, k = pick("turret"), pick("hull"), pick("track")
    amb = "+".join(a for a in str(p.get("ambient", "none")).split("+") if a in AMBIENT_KINDS) or "none"
    name = "".join(c for c in str(rec.get("name", "CUSTOM")).upper() if c.isalnum() or c in " -")[:14].strip() or "CUSTOM"
    lore = str(rec.get("lore", ""))[:60] or "A tank of your own design."
    return TankDesign(f"custom:{rec.get('id', 'x')}", name, lore, _TURRETS[t] + _HULLS[h] + _TRACKS[k], barrel=pick("barrel"),
                      glow=pick("glow"), ambient=amb, accent=COLOR_BY_NAME.get(p.get("accent"), (236, 242, 250)), parts=(t, h, k),
                      hover=3.0 if rec.get("hover") else 0.0)


def register_custom(rec: dict) -> Optional[TankDesign]:
    if not isinstance(rec, dict) or not isinstance(rec.get("id"), str):
        return None
    d = design_from_custom(rec)
    CUSTOM_DESIGNS[d.key] = d
    DESIGN_BY_KEY[d.key] = d
    return d


def custom_designs(save: "SaveData") -> tuple:
    return tuple(d for d in (register_custom(r) for r in save.custom_tanks) if d is not None)


def workshop_components(save: "SaveData") -> dict:
    """What the player may use in the workshop: the free starter set plus every component of every tank they own."""
    have = {s: set(FREE_PARTS.get(s, ())) for s in CUSTOM_SLOTS}
    owned = save.unlocked_tanks()
    for d in ALL_DESIGNS:
        if d.key not in owned and d.key not in {x.key for x in DESIGNS[:BASE_KITS]}:
            continue
        for slot, cid in zip(("turret", "hull", "track"), d.parts):
            have[slot].add(cid)
        have["barrel"].add(d.barrel)
        have["glow"].add(d.glow)
        have["ambient"].update(d.ambient.split("+"))
    have["accent"] = {n for n, _ in COLOR_CHOICES}
    tables = component_tables()
    return {s: [c for c in tables[s] if c in have[s]] for s in CUSTOM_SLOTS}


def component_counts(save: "SaveData") -> tuple:
    """(unlocked, total) component counts, derived live from the registries."""
    have, tables = workshop_components(save), component_tables()
    return sum(len(have[s]) for s in CUSTOM_SLOTS if s != "accent"), sum(len(tables[s]) for s in CUSTOM_SLOTS if s != "accent")


def content_counts(save: "SaveData") -> list:
    """Every (label, owned, total) row the cheat screen shows - all computed from the current registries."""
    tanks, ammo = save.unlocked_tanks(), save.unlocked_ammo()
    official_t = [d for d in ALL_DESIGNS if d.key not in {x.key for x in SECRET_DESIGNS}]
    official_a = [a for a in ALL_AMMOS if a.key not in {x.key for x in SECRET_AMMOS}]
    comps = component_counts(save)
    return [("TANKS", sum(d.key in tanks for d in official_t), len(official_t)), ("AMMO", sum(a.key in ammo for a in official_a), len(official_a)),
            ("MAPS", len(MAPS) + len(save.unlocked_maps()), len(MAPS) + len(SPECIAL_MAPS)),
            ("SCREENS", len(save.home_modes()), len(HOME_ORDER)), ("PARTS", comps[0], comps[1]),
            ("CUSTOM", len(save.custom_tanks), CUSTOM_TANK_MAX)]


# ---------------------------------------------------------------------------
# Unlock registry. Everything that can be unlocked is listed here once; the 'iamethanlabs101' cheat simply
# grants every entry, so adding a future unlockable is: register it below (one line) and give it a rule in
# SaveData.is_unlocked if it isn't earned through a Tournament level's reward_ids() or bought in the shop.
# ---------------------------------------------------------------------------
@dataclass(frozen=True)
class Unlockable:
    uid: str        # "tank:<key>" | "ammo:<key>" | "map:<key>" | "level:<n>" | "theme:<name>" | "rgb:<hull|shell>" | ...
    kind: str       # tank | ammo | map | level | theme | feature
    label: str


_UNLOCKABLES: list[Unlockable] = []


def register_unlockable(uid: str, kind: str, label: str) -> None:
    if all(u.uid != uid for u in _UNLOCKABLES):
        _UNLOCKABLES.append(Unlockable(uid, kind, label))


def unlockables() -> tuple[Unlockable, ...]:
    return tuple(_UNLOCKABLES)


def _register_default_unlockables() -> None:
    for d in DESIGNS[BASE_KITS:] + SHOP_DESIGNS + MISSION_DESIGNS + TICKET_DESIGNS + SECRET_DESIGNS + ANCIENT_DESIGNS:
        register_unlockable(f"tank:{d.key}", "tank", d.name)
    for a in AMMOS[BASE_KITS:] + SHOP_AMMOS + MISSION_AMMOS + TICKET_AMMOS + SECRET_AMMOS + ANCIENT_AMMOS:
        register_unlockable(f"ammo:{a.key}", "ammo", a.name)
    for m in MISSIONS[1:]:                          # mission 1 is always open
        register_unlockable(f"mission:{m.number}", "mission", f"MISSION {m.number}")
    for m in SPECIAL_MAPS:
        register_unlockable(f"map:{m.key}", "map", m.name)
    for lv in TOURNAMENT[1:]:                       # level 1 is always open
        register_unlockable(f"level:{lv.number}", "level", f"LEVEL {lv.number}")
    register_unlockable("master_difficulty", "feature", "MASTER DIFFICULTY")
    for k in HOME_ORDER[1:]:
        register_unlockable(f"theme:{k.lower()}", "theme", f"{k} HOME SCREEN")
    register_unlockable("rgb:hull", "feature", "RGB HULL COLOR")
    register_unlockable("rgb:shell", "feature", "RGB SHELL COLOR")
    register_unlockable("paint:astral", "feature", "ASTRAL PAINT / AMMO COLOR")
    register_unlockable("paint:prismatic", "feature", "PRISMATIC PAINT / AMMO COLOR")
    register_unlockable("paint:void", "feature", "VOID PAINT / AMMO COLOR")


_register_default_unlockables()
# what a version-1 save that had cleared the old (Master) Level 5 keeps: the Master rewards now live at Level 20
LEGACY_MASTER_GRANTS = ("tank:sovereign", "ammo:aurora", "map:citadel", "master_difficulty", "theme:master")
OLD_FINALS = {8: 18, 9: 19, 10: 20}         # version-2 saves had only ten levels; their last three became levels 18-20


class SaveData:
    """Progress, unlocks, wallet, loadouts and settings, stored as one small JSON file (path=None: in-memory only).

    `completed` = Tournament levels actually cleared (the chain rule applies: a level only counts if it was reachable).
    `granted`   = unlockables handed out some other way (shop purchases, cheat codes, legacy saves). A granted level is
                  only *open to play*; it is never marked cleared, and it never opens the level after it."""
    VERSION = 5

    def __init__(self, path: Optional[Path] = None) -> None:
        self.path = path
        self.mission_completed: set[int] = set()   # missions cleared (chain rule applies)
        self.mission_paid: set[int] = set()        # missions whose FIRST-clear reward was already paid (also LAN co-op guests)
        self.custom_tanks: list[dict] = []         # workshop creations: stable, versioned records (see design_from_custom)
        self.tickets_won: set[str] = set()         # Terminal Ticket bosses already defeated (they leave the random pool)
        self.martyr_paid, self.martyr_phase, self.martyr_won, self.martyr_seen, self.martyr_open = False, 0, False, False, False   # the Time Machine story
        self.ticket_active: str = ""               # a drawn-but-unbeaten ticket boss: retry it freely, it is never wasted
        self.story: set[str] = set()               # one-time story whispers already heard (see STORY_LINES)
        self.playtime: float = 0.0                 # seconds spent in the game (this profile)
        self.completed: set[int] = set()
        self.granted: set[str] = set()
        self.coins = 0
        self.credits = 0
        self.loadouts = [Loadout(), Loadout("ranger", "ORANGE", "standard", "MAGENTA")]
        self.settings: dict[str, str] = {}
        self.warning = ""

    # -- persistence ---------------------------------------------------------
    @classmethod
    def load(cls, path: Optional[Path]) -> "SaveData":
        data = cls(path)
        if path is None or not path.exists():
            return data
        try:
            raw = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            data.warning = "save file unreadable - starting fresh"
            return data
        if not isinstance(raw, dict):
            return data
        version = raw.get("version", 1) if isinstance(raw.get("version", 1), int) else 1
        known = {u.uid for u in _UNLOCKABLES}
        given = raw.get("granted", [])
        data.granted = {g for g in given if isinstance(g, str) and g in known} if isinstance(given, list) else set()
        if version < 3:                  # cheat-opened levels 8-10 meant the old finals
            data.granted = {f"level:{OLD_FINALS[int(g[6:])]}" if g.startswith("level:") and int(g[6:]) in OLD_FINALS else g
                            for g in data.granted}
        done = raw.get("tournament_completed", [])
        if isinstance(done, list):
            done = [n for n in done if isinstance(n, int)]
            if version < 2:
                if 5 in done:           # the old Level 5 was the Master fight: keep its rewards, the new Level 5 is a fresh fight
                    data.granted |= {g for g in LEGACY_MASTER_GRANTS if g in known}
                done = [n for n in done if n != 5]
            elif version < 3:           # old 8/9/10 are now 18/19/20: keep their rewards, they must be re-earned as levels
                for old, new in OLD_FINALS.items():
                    if old in done:
                        data.granted |= {g for g in TOURNAMENT[new - 1].reward_ids() if g in known}
                done = [n for n in done if n < 8]
            ok: set[int] = set()
            for n in range(1, len(TOURNAMENT) + 1):          # a level only counts if it was reachable
                reachable = n == 1 or (n - 1) in ok or f"level:{n}" in data.granted
                if n in done and reachable:
                    ok.add(n)
            data.completed = ok
        mdone = raw.get("missions_completed", [])
        if isinstance(mdone, list):
            ok_m: set[int] = set()
            for n in range(1, len(MISSIONS) + 1):
                if n in mdone and (n == 1 or (n - 1) in ok_m or f"mission:{n}" in data.granted):
                    ok_m.add(n)
            data.mission_completed = ok_m
        ct = raw.get("custom_tanks")
        if isinstance(ct, list):
            seen: set = set()
            for rec in ct:
                if isinstance(rec, dict) and isinstance(rec.get("id"), str) and rec["id"] not in seen:
                    seen.add(rec["id"])
                    data.custom_tanks.append(rec)                  # kept verbatim: unknown fields from newer versions survive
                    register_custom(rec)
        tk = raw.get("tickets")
        if isinstance(tk, dict):
            data.tickets_won = {k for k in tk.get("won", []) if isinstance(k, str)}
            data.ticket_active = tk["active"] if isinstance(tk.get("active"), str) else ""
        if isinstance(raw.get("playtime"), (int, float)) and raw["playtime"] >= 0:
            data.playtime = float(raw["playtime"])
        sty = raw.get("story")
        if isinstance(sty, list):
            data.story = {s for s in sty if isinstance(s, str)}
        mt = raw.get("martyr")
        if isinstance(mt, dict):
            data.martyr_paid, data.martyr_won, data.martyr_seen = bool(mt.get("paid")), bool(mt.get("won")), bool(mt.get("seen"))
            data.martyr_open = bool(mt.get("open"))
            data.martyr_phase = int(mt["phase"]) if isinstance(mt.get("phase"), int) and 0 <= mt["phase"] <= 3 else 0
        if data.martyr_won:                                  # saves that beat the Martyr before new rewards existed get them now
            for uid in MARTYR_REWARD_IDS:
                data.granted.add(uid) if uid in known else None
        paid = raw.get("missions_paid", [])
        data.mission_paid = {n for n in paid if isinstance(n, int) and 1 <= n <= len(MISSIONS)} | data.mission_completed \
            if isinstance(paid, list) else set(data.mission_completed)
        wallet = raw.get("wallet")
        if isinstance(wallet, dict):
            for f in ("coins", "credits"):
                v = wallet.get(f, 0)
                setattr(data, f, max(0, v) if isinstance(v, int) else 0)
        for i, lo in enumerate((raw.get("loadouts") or [])[:2]):
            if isinstance(lo, dict):
                cur = data.loadouts[i]
                for f in ("tank", "tank_color", "ammo", "shot_color"):
                    if isinstance(lo.get(f), str):
                        setattr(cur, f, lo[f])
        st = raw.get("settings")
        if isinstance(st, dict):
            data.settings = {str(k): str(v) for k, v in st.items()}
        return data

    def flush(self) -> None:
        if self.path is None:
            return
        doc = {
            "version": self.VERSION,
            "tournament_completed": sorted(self.completed),
            "granted": sorted(self.granted),
            "missions_completed": sorted(self.mission_completed),
            "missions_paid": sorted(self.mission_paid),
            "tickets": {"won": sorted(self.tickets_won), "active": self.ticket_active},
            "story": sorted(self.story),
            "playtime": round(self.playtime, 1),
            "martyr": {"paid": self.martyr_paid, "phase": self.martyr_phase, "won": self.martyr_won, "seen": self.martyr_seen, "open": self.martyr_open},
            "custom_tanks": self.custom_tanks,
            "wallet": {"coins": self.coins, "credits": self.credits},
            "unlocked": {"tanks": sorted(self.unlocked_tanks()), "ammo": sorted(self.unlocked_ammo()),
                         "maps": [m.key for m in self.unlocked_maps()], "master_difficulty": self.master_unlocked,
                         "home_screens": self.home_modes()},
            "loadouts": [vars(l) for l in self.loadouts],
            "settings": self.settings,
        }
        try:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            tmp = self.path.with_suffix(".tmp")
            tmp.write_text(json.dumps(doc, indent=2), encoding="utf-8")
            os.replace(tmp, self.path)
        except OSError:
            self.warning = "could not write save file"

    # -- wallet & shop -------------------------------------------------------
    def earn(self, p: Payout) -> None:
        """The only way money enters the wallet (match / Tournament results). Cheat codes never call this."""
        self.coins += max(0, p.coins)
        self.credits += max(0, p.credits)
        self.flush()

    def shortfall(self, item: ShopItem) -> str:
        if self.coins < item.coins:
            return f"NEED {item.coins - self.coins} MORE COINS"
        if self.credits < item.credits:
            return f"NEED {item.credits - self.credits} MORE CREDITS"
        return ""

    def buy(self, item: ShopItem) -> str:
        """Buys a shop item. Returns '' on success, otherwise the reason it failed."""
        if self.is_unlocked(item.uid):
            return "ALREADY OWNED"
        why = self.shortfall(item)
        if why:
            return why
        self.coins -= item.coins
        self.credits -= item.credits
        self.granted.add(item.uid)
        self.sanitize_loadouts()
        self.flush()
        return ""

    # -- unlock rules --------------------------------------------------------
    def earned(self) -> set:
        """Unlockable ids earned by clearing Tournament levels."""
        out: set = set()
        for lv in TOURNAMENT:
            if lv.number in self.completed:
                out.update(lv.reward_ids())
        for m in MISSIONS:
            if m.number in self.mission_completed:
                out.update(m.rewards)
        return out

    def tickets_sold_out(self) -> bool:
        return len(self.tickets_won & set(TICKET_BOSS_BY_KEY)) >= len(TICKET_BOSS_BY_KEY)

    def buy_ticket(self, item: "ShopItem") -> str:
        """Pays for a Terminal Ticket and draws a random boss that has not been beaten. Returns the boss key, or '' if it
        could not be bought. A drawn boss stays active until beaten, so losing never wastes a ticket."""
        if not self.tickets_open() or self.ticket_active or self.tickets_sold_out() or self.shortfall(item):
            return ""
        pool = sorted(set(TICKET_BOSS_BY_KEY) - self.tickets_won)
        self.coins -= item.coins
        self.credits -= item.credits
        self.ticket_active = random.choice(pool)
        self.flush()
        return self.ticket_active

    # -- progression gates (derived from existing state: no new unlock flag, so old saves keep exactly what they earned) ----
    def missions_open(self) -> bool:
        """Missions open once the Master (Tournament level 20) falls. Saves that already touched Missions keep them."""
        return (len(TOURNAMENT) in self.completed or bool(self.mission_completed) or bool(self.mission_paid)
                or any(g.startswith("mission:") for g in self.granted))

    def tickets_open(self) -> bool:
        """Terminal Tickets open after the Master AND the Mothership (Mission 50) are beaten. Saves already deep in the vaults keep access."""
        return ((len(TOURNAMENT) in self.completed and len(MISSIONS) in self.mission_completed) or bool(self.tickets_won)
                or bool(self.ticket_active) or self.martyr_open or self.martyr_paid or self.martyr_won)

    def progress_tier(self) -> int:
        """0 none · 1 Tournament cleared (gold) · 2 Missions cleared (green) · 3 every vault boss beaten (void) · 4 the Martyr beaten (astral + gold)."""
        if self.martyr_won:
            return 4
        if self.tickets_sold_out():
            return 3
        if len(MISSIONS) in self.mission_completed:
            return 2
        return 1 if len(TOURNAMENT) in self.completed else 0

    def progress_counts(self) -> dict:
        """{'tanks': (owned, total), 'ammo': ..., 'maps': ..., 'screens': ...} for profile cards."""
        def count(prefix: str, base: int) -> tuple:
            us = [u for u in _UNLOCKABLES if u.uid.startswith(prefix)]
            return base + sum(1 for u in us if self.is_unlocked(u.uid)), base + len(us)
        return {"tanks": count("tank:", BASE_KITS), "ammo": count("ammo:", BASE_KITS), "maps": count("map:", len(MAPS)), "screens": count("theme:", 1)}

    def paint_ok(self, name: str, hull: bool) -> bool:
        """Animated paints need their unlock; the plain 24 colours are always fine."""
        need = PAINT_UNLOCK.get(name)
        return need is None or self.is_unlocked(need[0 if hull else 1])

    def workshop_open(self) -> bool:
        """The Workshop opens after Tournament level 5. Saves that already built a tank (or are past level 5) keep it."""
        return 5 in self.completed or bool(self.custom_tanks) or any(n > 5 for n in self.completed)

    def blessing_unlocked(self) -> bool:
        """Martyr's Blessing is earned only by defeating the Martyr (the flag is set together with the other Martyr rewards)."""
        return self.martyr_won

    def hear(self, key: str) -> bool:
        """True exactly once per story whisper: marks it heard and saves."""
        if key in self.story:
            return False
        self.story.add(key)
        self.flush()
        return True

    def martyr_available(self) -> bool:
        return self.tickets_sold_out() or self.martyr_open

    def buy_martyr(self, item: "ShopItem") -> bool:
        if not self.martyr_available() or self.martyr_paid or self.shortfall(item):
            return False
        self.coins -= item.coins
        self.credits -= item.credits
        self.martyr_paid = True
        self.flush()
        return True

    def mission_unlocked(self, n: int) -> bool:
        return n == 1 or (n - 1) in self.mission_completed or f"mission:{n}" in self.granted

    def complete_mission(self, n: int) -> bool:
        """Records a cleared mission. Returns True if this was its first clear."""
        first = n not in self.mission_completed
        self.mission_completed.add(n)
        self.mission_paid.add(n)
        self.sanitize_loadouts()
        self.flush()
        return first

    def level_unlocked(self, n: int) -> bool:
        return n == 1 or (n - 1) in self.completed or f"level:{n}" in self.granted

    def is_unlocked(self, uid: str) -> bool:
        if uid.startswith("level:"):
            return self.level_unlocked(int(uid[6:]))
        if uid.startswith("mission:"):
            return self.mission_unlocked(int(uid[8:]))
        if uid == "ticket":                                  # "owned" means sold out
            return self.tickets_sold_out()
        if uid == "timemachine":                             # "owned" means the Martyr has been beaten
            return self.martyr_won
        if uid == "ammo:" + BLESSING_KEY:                    # only ever the Martyr's reward - never granted, bought or cheated
            return self.blessing_unlocked()
        return uid in self.granted or uid in self.earned()

    def grant(self, uid: str) -> bool:
        """Hands out one unlockable (cheat codes). True only if it was new. Never touches the wallet."""
        if uid == "ammo:" + BLESSING_KEY or uid not in {u.uid for u in _UNLOCKABLES} or self.is_unlocked(uid):
            return False
        self.granted.add(uid)
        self.sanitize_loadouts()
        return True

    def grant_all(self) -> int:
        """Grants every registered unlockable (levels become open to play, none are marked cleared). No currency."""
        n = sum(self.grant(u.uid) for u in unlockables())
        self.flush()
        return n

    def _keys(self, kind: str) -> set:
        have = self.granted | self.earned()
        return {u[len(kind) + 1:] for u in have if u.startswith(kind + ":")}

    def unlocked_tanks(self) -> set:
        return {d.key for d in DESIGNS[:BASE_KITS]} | self._keys("tank") | {f"custom:{r['id']}" for r in self.custom_tanks}

    def create_custom(self, parts: dict, name: str, lore: str, hover: bool = False) -> str:
        """Builds a custom tank for CUSTOM_TANK_FEE coins (+ the HOVER upgrade if asked). Returns '' on success, else the reason."""
        if len(self.custom_tanks) >= CUSTOM_TANK_MAX:
            return "WORKSHOP FULL - DELETE A TANK FIRST"
        cost, cred = CUSTOM_TANK_FEE + (HOVER_COINS if hover else 0), HOVER_CREDITS if hover else 0
        if self.coins < cost:
            return f"NEED {cost - self.coins:,} MORE COINS"
        if self.credits < cred:
            return f"NEED {cred - self.credits} MORE CREDITS FOR HOVER"
        have = workshop_components(self)
        if any(parts.get(s) not in have[s] for s in CUSTOM_SLOTS if s != "ambient") or \
                any(a not in have["ambient"] for a in parts.get("ambient", "none").split("+")):
            return "A CHOSEN PART IS NOT UNLOCKED"
        rec = {"fmt": CUSTOM_FMT, "id": "c" + "".join(random.choice("0123456789abcdef") for _ in range(10)), "name": name, "lore": lore,
               "parts": dict(parts), "created": time.strftime("%Y-%m-%d"), "game": __version__}
        if hover:
            rec["hover"] = True
        self.custom_tanks.append(rec)
        register_custom(rec)
        self.coins -= cost
        self.credits -= cred
        self.flush()
        return ""

    def delete_custom(self, cid: str) -> None:
        self.custom_tanks = [r for r in self.custom_tanks if r.get("id") != cid]
        CUSTOM_DESIGNS.pop(f"custom:{cid}", None)
        self.sanitize_loadouts()
        self.flush()

    def unlocked_ammo(self) -> set:
        out = {a.key for a in AMMOS[:BASE_KITS]} | self._keys("ammo")
        out.discard(BLESSING_KEY)
        return out | ({BLESSING_KEY} if self.blessing_unlocked() else set())

    def unlocked_maps(self) -> list:
        return [m for m in SPECIAL_MAPS if self.is_unlocked(f"map:{m.key}")]

    @property
    def master_unlocked(self) -> bool:
        return self.is_unlocked("master_difficulty")

    def home_modes(self) -> list:
        return [k for k in HOME_ORDER if k == "CLASSIC" or self.is_unlocked(f"theme:{k.lower()}")]

    def complete_level(self, n: int) -> bool:
        """Records a cleared level. Returns True if this was its first clear."""
        first = n not in self.completed
        self.completed.add(n)
        self.sanitize_loadouts()
        self.flush()
        return first

    def loadout(self, i: int) -> Loadout:
        lo = self.loadouts[i]
        if lo.tank not in self.unlocked_tanks():
            lo.tank = "ranger"
        if lo.ammo not in self.unlocked_ammo():
            lo.ammo = "standard"
        if lo.tank_color not in COLOR_BY_NAME or not self.paint_ok(lo.tank_color, True):
            lo.tank_color = "CYAN"
        if lo.shot_color not in COLOR_BY_NAME or not self.paint_ok(lo.shot_color, False):
            lo.shot_color = "YELLOW"
        return lo

    def sanitize_loadouts(self) -> None:
        for i in range(2):
            self.loadout(i)

    # -- settings ------------------------------------------------------------
    def capture_settings(self, s: Settings) -> None:
        for name, attr, choices, _ in SETTING_ROWS:
            cur = getattr(s, attr)
            label = next((l for l, v in choices if v == cur), None)
            if label is not None:
                self.settings[name] = label

    def apply_settings(self, s: Settings) -> None:
        for name, attr, choices, _ in SETTING_ROWS:
            label = self.settings.get(name)
            for l, v in choices:
                if l == label:
                    setattr(s, attr, v)
        if not self.master_unlocked and s.difficulty is Difficulty.MASTER:   # locked content can never leak in via a settings file
            s.difficulty = Difficulty.NORMAL
        if s.home_mode not in self.home_modes():
            s.home_mode = "CLASSIC"


# ---------------------------------------------------------------------------
# Cheat codes
# ---------------------------------------------------------------------------
@dataclass(frozen=True)
class Cheat:
    code: str
    grants: tuple = ()          # unlockable ids this code hands out
    everything: bool = False    # True: grant every registered unlockable
    activate_theme: str = ""    # home screen to switch to once unlocked
    text: str = ""              # message when something new was unlocked


LEVEL_WORDS = ("one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten", "eleven", "twelve", "thirteen",
               "fourteen", "fifteen", "sixteen", "seventeen", "eighteen", "nineteen", "twenty")
assert len(LEVEL_WORDS) == len(TOURNAMENT)
CHEATS: dict[str, Cheat] = {}


def _register_cheats() -> None:
    def add(c: Cheat) -> None:
        CHEATS[c.code] = c
    add(Cheat("asciieasteregg", ("tank:asciibot", "ammo:glyph"), text="EASTER EGG UNLOCKED: ASCIIBOT TANK + GLYPH AMMO"))
    add(Cheat("iamethanlabs101", everything=True))
    add(Cheat("unlocktimemachine", text="MARTYR'S TIME MACHINE UNLOCKED"))
    add(Cheat("notasecrettheme", ("theme:secret",), activate_theme="SECRET", text="SECRET HOME SCREEN UNLOCKED AND ACTIVATED"))
    for n, word in enumerate(LEVEL_WORDS, 1):       # each level code opens that one level only
        add(Cheat(f"unlocklevel{word}", (f"level:{n}",)))


_register_cheats()


def redeem_cheat(app: "Application", code: str) -> tuple[bool, str]:
    """Runs a cheat code. Returns (success, message). Cheats unlock content only - never coins or credits."""
    cheat = CHEATS.get("".join(code.lower().split()))
    if cheat is None:
        return False, "UNKNOWN CODE"
    save = app.save
    if cheat.code == "unlocktimemachine":                 # opens the Time Machine in the shop and skips its price (no currency given)
        if save.martyr_open and save.martyr_paid:
            return True, "THE TIME MACHINE IS ALREADY UNLOCKED"
        save.martyr_open = save.martyr_paid = True
        save.flush()
        return True, "TIME MACHINE UNLOCKED - FIND IT IN THE SHOP (EXTRAS) AND PRESS ENTER"
    if cheat.everything:
        n = save.grant_all()
        return True, f"ALL UNLOCKABLES GRANTED - {n} NEW" if n else "EVERYTHING IS ALREADY UNLOCKED"
    if cheat.code.startswith("unlocklevel"):
        n = int(cheat.grants[0][6:])
        if save.level_unlocked(n):
            return True, f"LEVEL {n} IS ALREADY OPEN"
        save.grant(cheat.grants[0])
        save.flush()
        return True, f"LEVEL {n} OPENED - CLEAR IT TO EARN ITS REWARDS"
    new = [g for g in cheat.grants if save.grant(g)]
    if not new:
        return True, "ALREADY UNLOCKED"
    if cheat.activate_theme:
        app.settings.home_mode = cheat.activate_theme
        app.apply_settings()
    else:
        save.flush()
    return True, cheat.text or "UNLOCKED"


# ============================================================================
# Match
# ============================================================================
@dataclass(frozen=True)
class PlayerSetup:
    name: str
    label: str
    tank_color: RGB
    shot_color: RGB
    is_ai: bool
    design: str = "ranger"
    ammo: str = "standard"
    hp: int = MAX_HP
    team: int = -1               # -1: its own team (plain 1v1); otherwise tanks sharing a team fight together
    owner: int = 0               # LAN owner (0 = host machine)
    diff: Optional[Difficulty] = None    # per-tank AI difficulty (defaults to the match's)
    prec: float = 1.0
    kind: str = ""               # "ufo" for the big saucer enemies
    scale: float = 1.0
    hover: float = 0.0
    shield: int = 0
    guard: bool = False


@dataclass
class MatchConfig:
    players: list
    single: bool
    difficulty: Difficulty
    mode: str = "single"
    ai_precision: float = 1.0
    target: int = ROUNDS_TO_WIN  # rounds a side must win to take the match
    label: str = ""              # e.g. "BEST OF 5" / "FIRST TO 3"
    net: object = None           # a NetLink when this match is played over LAN
    my_owner: int = 0            # which LAN seat this machine plays (0 host, 1 client)
    world_size: Optional[tuple] = None   # LAN: fixed battlefield size shared by both machines
    ai_seed: int = 0
    special_wait: int = 2        # forfeited turns needed to bank the special attack
    pick_maps: bool = False      # the loser (single player: you) picks the next battlefield every round
    ammo_fx: bool = True         # special-ammo mechanics on/off (single player: on; tournament/missions: off; two player/LAN: rule)
    special: bool = False        # two-player / LAN: the forfeit-two-turns-for-a-3-damage special attack is enabled
    blessing: bool = False       # MARTYR'S BLESSING may be fired (Single/Two Player/LAN rules only; never Tournament, Missions, Tickets or the Martyr)


class MatchSession:
    """A best-of-three match. Tournament runs subclass this and swap the scoring rules."""

    exit_label = "MAIN MENU"

    def __init__(self, config: MatchConfig, first_map: Optional[MapDefinition], seed: Optional[int]) -> None:
        self.config = config
        self.rng = random.Random(seed)
        self.wins = [0, 0]
        self.round_no = 1
        self.played: list[MapDefinition] = []
        self.current_map = first_map or self._pick_map()
        self.played.append(self.current_map)

    def _pick_map(self) -> MapDefinition:
        pool = [m for m in MAPS if m not in self.played] or list(MAPS)
        return self.rng.choice(pool)

    @property
    def starting_player(self) -> int:
        return (self.round_no - 1 + getattr(self, "first_offset", 0)) % 2     # first_offset: set by the rock-paper-scissors winner

    def round_seed(self) -> int:
        return self.rng.randrange(1 << 30)

    def record_round(self, winner: int) -> None:
        self.wins[winner] += 1

    @property
    def over(self) -> bool:
        return max(self.wins) >= self.config.target

    @property
    def champion(self) -> int:
        return 0 if self.wins[0] > self.wins[1] else 1

    def team_players(self, team: int) -> list:
        return [p for i, p in enumerate(self.config.players) if (p.team if p.team >= 0 else i) == team]

    def team_name(self, team: int) -> str:
        return " + ".join(p.name for p in self.team_players(team)) or f"TEAM {team + 1}"

    pending_map: Optional[MapDefinition] = None          # the battlefield the round's loser picked

    def on_turn_start(self, scene, tank) -> None:        # hook: boss sessions change a tank's ammo here
        pass

    def dmg_mult(self, tank) -> int:                     # hook: boss fights scale damage (huge health pools)
        return 1

    def interlude(self, app):                            # hook: a scene to show between rounds (cinematics)
        return None


    def advance(self) -> None:
        self.round_no += 1
        self.current_map = self._pick_map()                  # always drawn, so both LAN machines keep their rngs in step
        if self.pending_map is not None:
            self.current_map, self.pending_map = self.pending_map, None
        self.played.append(self.current_map)

    # -- presentation hooks (overridden by tournament runs) -------------------
    def hud_wins(self, i: int) -> str:
        need = self.config.target
        if need > 5:
            return f"WINS {self.wins[i]}/{need}"
        return "WINS " + " ".join("●" if k < self.wins[i] else "○" for k in range(need))

    def hud_title(self) -> str:
        return f"ROUND {self.round_no}  ·  {self.current_map.name}"

    def banner_title(self) -> str:
        return f"ROUND {self.round_no}"

    def round_summary(self, winner: int) -> dict:
        names = [self.team_name(0), self.team_name(1)]
        return {"title": f"ROUND {self.round_no} COMPLETE", "headline": f"{names[winner]} WINS THE ROUND",
                "score": f"{self.wins[0]}   —   {self.wins[1]}", "sub": f"{names[0]}  vs  {names[1]}",
                "next": "MATCH DECIDED" if self.over else f"NEXT: ROUND {self.round_no + 1} - NEW BATTLEFIELD"}

    def banner_note(self) -> str:
        return ""

    payout: Optional[Payout] = None        # set once the match is over and paid out (single player only)
    paid = False
    wind_override: Optional[float] = None  # a Tournament rule can pin the wind (GALE FORCE)

    def commit(self, app: "Application") -> None:
        """Called after every round; pays out once when a single-player match ends (two-player earns nothing)."""
        if self.over and not self.paid and self.config.mode == "single":
            self.paid = True
            self.payout = match_payout(self.champion == 0, self.config.difficulty)
            app.save.earn(self.payout)

    def finish_scene(self, app: "Application") -> "Scene":
        return MatchResultScene(app, self)

    def exit_scene(self, app: "Application") -> "Scene":
        return MenuScene(app)

    def restart(self, app: "Application") -> None:
        app.start_match(self.config, self.played[0] if not app.settings.random_maps else None, self.wind_override, rps=False)


def mission_foe(rng: random.Random, diff: Difficulty, prec: float, avoid: set, name: str, hp: int) -> PlayerSetup:
    """A random AI opponent built only from the starter kits, so it never depends on what a player has unlocked
    (LAN machines must build exactly the same opponent from the same seed)."""
    colors = [n for n, _ in COLOR_CHOICES if n not in avoid]
    return PlayerSetup(name, "AI", COLOR_BY_NAME[rng.choice(colors)], COLOR_BY_NAME[rng.choice(colors)], True,
                       rng.choice(DESIGNS[:BASE_KITS]).key, rng.choice(AMMOS[:BASE_KITS]).key, hp, team=1, diff=diff, prec=prec)


def ufo_setup(name: str, hp: int, diff: Difficulty, prec: float, scale: float = 3.0, color: RGB = (90, 230, 160),
              guard: bool = False) -> PlayerSetup:
    return PlayerSetup(name, "UFO", color, (170, 255, 200), True, "ranger", "xenon", hp, team=1, diff=diff, prec=prec, kind="ufo",
                       scale=scale, hover=4.0 * scale + 2.0, guard=guard)


class MissionSession(MatchSession):
    """One run at a mission. Marathons and boss rushes are several stages; the session swaps the opposition between them."""
    exit_label = "MISSIONS"

    def __init__(self, mission: Mission, config: MatchConfig, seed: Optional[int], humans: list, restart_kw: Optional[dict] = None) -> None:
        self.mission, self.humans = mission, humans
        self.stage, self.stage_cleared = 0, False
        self.committed, self.first_clear = False, False
        self.restart_kw = restart_kw
        super().__init__(config, None, seed)
        config.players = self.stage_players(0)

    # -- stages --------------------------------------------------------------
    @property
    def stage_target(self) -> int:
        m = self.mission
        return 2 if m.kind == "marathon" else m.target

    @property
    def fail_target(self) -> int:
        m = self.mission
        return 2 if m.kind == "marathon" else m.fail_at

    def stage_diff(self) -> Difficulty:
        m = self.mission
        order = (Difficulty.EASY, Difficulty.NORMAL, Difficulty.HARD, Difficulty.MASTER)
        if m.kind == "marathon" and m.stages > 1:
            a, b = order.index(m.diff), order.index(m.diff_end)
            return order[a + (b - a) * self.stage // (m.stages - 1)] if a != b else m.diff
        return m.diff

    def stage_players(self, stage: int) -> list:
        m, rng = self.mission, random.Random(self.rng.randrange(1 << 30))
        avoid = {n for n, c in COLOR_CHOICES if any(c == h.tank_color for h in self.humans)}
        foes: list = []
        if m.kind == "marathon":
            foes = [mission_foe(rng, self.stage_diff(), m.prec, avoid, f"RIVAL {stage + 1}", 3)]
        elif m.kind == "rush":
            lv = TOURNAMENT[m.levels[stage] - 1]
            foes = [replace(boss_setup(lv, color_index(next((n for n, c in COLOR_CHOICES if c == self.humans[0].tank_color), "CYAN"))),
                            team=1, hp=3, diff=lv.difficulty, prec=lv.precision)]
        elif m.kind == "mother":
            foes = [ufo_setup("MOTHERSHIP", m.ufo_hp, m.diff, m.prec * 1.2, 4.0, (120, 255, 190), guard=True),
                    ufo_setup("DRONE A", m.enemy_hp, m.diff, m.prec * 1.5, 2.0, (200, 140, 255)),
                    ufo_setup("DRONE B", m.enemy_hp, m.diff, m.prec * 1.5, 2.0, (140, 200, 255))]
        elif m.kind == "ufo" or (m.kind == "coop" and m.coop_ufo):
            foes = [ufo_setup(m.name.replace("UFO: ", ""), m.ufo_hp, m.diff, m.prec * 1.4)]
        else:
            foes = [mission_foe(rng, m.diff, m.prec * 1.3, avoid, f"ENEMY {k + 1}", m.enemy_hp) for k in range(m.enemies)]
        return list(self.humans) + foes

    def _pick_map(self) -> MapDefinition:
        m = self.mission
        key = m.map_key
        if m.kind == "rush":
            lv = TOURNAMENT[m.levels[min(self.stage, len(m.levels) - 1)] - 1]
            key = lv.map_key
        return SPECIAL_MAP_BY_KEY[key] if key else super()._pick_map()

    # -- scoring -------------------------------------------------------------
    def record_round(self, winner: int) -> None:
        self.wins[winner] += 1
        if winner == 0 and self.wins[0] >= self.stage_target and self.stage < self.mission.stages - 1:
            self.stage_cleared = True

    def advance(self) -> None:
        if self.stage_cleared:
            self.stage, self.wins, self.stage_cleared = self.stage + 1, [0, 0], False
            self.config.players = self.stage_players(self.stage)
        super().advance()

    @property
    def completed(self) -> bool:
        return self.stage == self.mission.stages - 1 and self.wins[0] >= self.stage_target

    @property
    def failed(self) -> bool:
        return self.wins[1] >= self.fail_target and not self.completed

    @property
    def over(self) -> bool:
        return self.completed or self.failed

    @property
    def champion(self) -> int:
        return 0 if self.completed else 1

    def hud_wins(self, i: int) -> str:
        m = self.mission
        if i == 1:
            return f"LOST {self.wins[1]}/{self.fail_target}"
        if m.kind == "rush":
            return f"BOSS {self.stage + 1}/{m.stages}  ·  {m.stages - self.stage - 1} AFTER THIS"
        pre = f"MATCH {self.stage + 1}/{m.stages}  " if m.stages > 1 else ""
        return f"{pre}WINS {self.wins[0]}/{self.stage_target}"

    def hud_title(self) -> str:
        return f"MISSION {self.mission.number}  ·  {self.mission.type_name}  ·  {self.current_map.name}"

    def banner_title(self) -> str:
        m = self.mission
        if m.stages > 1:
            return f"{'MATCH' if m.kind == 'marathon' else 'BOSS'} {self.stage + 1} OF {m.stages}"
        return f"MISSION {m.number} - ROUND {self.round_no}"

    def banner_note(self) -> str:
        m = self.mission
        if m.kind == "mother" and self.round_no == 1:
            return "THE MOTHERSHIP IS SHIELDED - DESTROY THE DRONES FIRST"
        if m.kind in ("marathon", "rush"):
            foe = self.config.players[-1]
            return f"VS {foe.name}" + (f"  ·  {self.stage_diff().value}" if m.kind == "marathon" else "")
        if self.wins[1] and self.wins[1] >= self.fail_target - 1:
            return "FINAL WARNING - ONE MORE LOST ROUND FAILS THE MISSION"
        return ""

    def round_summary(self, winner: int) -> dict:
        m = self.mission
        if self.completed:
            nxt = "MISSION COMPLETE!"
        elif self.failed:
            nxt = "MISSION FAILED"
        elif self.stage_cleared:
            left = m.stages - self.stage - 1
            nxt = f"{'MATCH' if m.kind == 'marathon' else 'BOSS'} WON - {left} {'MORE' if m.kind == 'marathon' else 'BOSS' + ('ES' if left != 1 else '') + ' LEFT IN THE GAUNTLET'}".replace("MORE", "MORE TO GO")
        elif winner == 0:
            nxt = f"{self.stage_target - self.wins[0]} MORE TO GO"
        else:
            nxt = f"ROUND LOST - {self.fail_target - self.wins[1]} MORE FAILS THE MISSION"
        if m.kind == "rush":
            beaten = self.stage + (1 if (self.stage_cleared or self.completed) else 0)
            score = f"BOSSES BEATEN {beaten}/{m.stages}  ·  {m.stages - beaten} LEFT"
        elif m.kind == "marathon":
            score = f"MATCH {self.stage + 1}/{m.stages}  ·  ROUNDS {self.wins[0]}-{self.wins[1]}"
        else:
            score = f"{self.wins[0]}/{self.stage_target}  ·  LOST {self.wins[1]}/{self.fail_target}"
        return {"title": f"MISSION {m.number} - ROUND {self.round_no}",
                "headline": "YOUR TEAM WINS THE ROUND" if winner == 0 else "THE ENEMY WINS THE ROUND",
                "score": score, "sub": m.objective, "next": nxt}

    # -- results -------------------------------------------------------------
    def commit(self, app: "Application") -> None:
        if self.over and not self.committed:
            self.committed = True
            n, save = self.mission.number, app.save
            host = self.config.net is None or self.config.my_owner == 0
            if self.completed:
                self.first_clear = n not in save.mission_paid
                if host:
                    save.complete_mission(n)                 # progression counts only on the host's save
                else:                                        # LAN guest: gets the coins and the kit, not the progression step
                    save.mission_paid.add(n)
                    if self.first_clear:
                        for uid in self.mission.rewards:
                            save.grant(uid)
            self.payout = mission_payout(self.mission, self.completed, self.first_clear)
            save.earn(self.payout)

    def finish_scene(self, app: "Application") -> "Scene":
        return MissionResultScene(app, self)

    def exit_scene(self, app: "Application") -> "Scene":
        return MissionsScene(app, self.mission.number)

    def restart(self, app: "Application") -> None:
        build_mission_session(app, self.mission, **(self.restart_kw or {"loadouts": [app.save.loadout(0)]}))


def humans_for(app, mission: Mission, loadouts: list, wingman: bool = False, guest: Optional[Loadout] = None) -> list:
    """The player-controlled (and wingman) tanks for a mission. Co-op always has two seats on team 0."""
    hp = mission.hp
    mk = lambda lo, name, label, owner=0, col=None: PlayerSetup(name, label, COLOR_BY_NAME[col or lo.tank_color],
                                                                 COLOR_BY_NAME[lo.shot_color], False, lo.tank, lo.ammo, hp, team=0,
                                                                 owner=owner)
    first = mk(loadouts[0], "PLAYER 1" if mission.is_coop else "PLAYER", "P1" if mission.is_coop else "YOU")
    if not mission.is_coop:
        return [first]
    if guest is not None or len(loadouts) > 1:
        lo2 = guest or loadouts[1]
        col = lo2.tank_color
        if col == loadouts[0].tank_color:
            col = COLOR_CHOICES[(color_index(col) + 12) % len(COLOR_CHOICES)][0]
        return [first, mk(lo2, "PLAYER 2", "P2", 1 if guest is not None else 0, col)]
    col = COLOR_CHOICES[(color_index(loadouts[0].tank_color) + 12) % len(COLOR_CHOICES)][0]       # AI wingman
    wing = PlayerSetup("WINGMAN", "AI", COLOR_BY_NAME[col], (200, 255, 200), True, "gemini", "standard", hp, team=0,
                       diff=Difficulty.NORMAL, prec=1.0)
    return [first, wing]


def build_mission_session(app, mission: Mission, loadouts: list, wingman: bool = False, guest: Optional[Loadout] = None, net=None,
                          my_owner: int = 0, world: Optional[tuple] = None, seed: Optional[int] = None, humans: Optional[list] = None,
                          go: bool = True) -> MissionSession:
    humans = humans if humans is not None else humans_for(app, mission, loadouts, wingman, guest)
    cfg = MatchConfig(list(humans), False, mission.diff, "mission", target=mission.target, ammo_fx=False, net=net, my_owner=my_owner, world_size=world)
    seed = app.rng.randrange(1 << 30) if seed is None else seed
    kw = {"loadouts": loadouts, "wingman": wingman, "guest": guest}
    s = MissionSession(mission, cfg, seed, humans, restart_kw=kw if net is None else None)
    if go:
        app.goto(BattleScene(app, s))
    return s


class TicketSession(MatchSession):
    """A Terminal Ticket boss fight: first to two rounds against one vault boss. Losing keeps the ticket active."""
    exit_label = "SHOP"

    def __init__(self, boss: TicketBoss, config: MatchConfig, seed: Optional[int]) -> None:
        self.boss, self.committed, self.first_clear = boss, False, True
        super().__init__(config, None, seed)

    def _pick_map(self) -> MapDefinition:
        return SPECIAL_MAP_BY_KEY[self.boss.map_key]

    @property
    def completed(self) -> bool:
        return self.wins[0] >= 2

    @property
    def failed(self) -> bool:
        return self.wins[1] >= 2

    @property
    def over(self) -> bool:
        return self.completed or self.failed

    @property
    def champion(self) -> int:
        return 0 if self.completed else 1

    def hud_wins(self, i: int) -> str:
        return f"WINS {self.wins[0]}/2" if i == 0 else f"LOST {self.wins[1]}/2"

    def hud_title(self) -> str:
        return f"TERMINAL TICKET  ·  {self.boss.name}  ·  {self.current_map.name}"

    def banner_title(self) -> str:
        return f"{self.boss.name} - ROUND {self.round_no}"

    def banner_note(self) -> str:
        return self.boss.taunt if self.round_no == 1 else ""

    def round_summary(self, winner: int) -> dict:
        nxt = ("BOSS DEFEATED!" if self.completed else "TICKET BATTLE LOST - TRY AGAIN, THE TICKET IS KEPT" if self.failed
               else f"{2 - self.wins[0]} MORE TO GO" if winner == 0 else "ROUND LOST - ONE MORE FAILS THE BATTLE")
        return {"title": f"{self.boss.name} - ROUND {self.round_no}", "headline": "YOU WIN THE ROUND" if winner == 0 else f"{self.boss.name} WINS THE ROUND",
                "score": f"{self.wins[0]}/2  ·  LOST {self.wins[1]}/2", "sub": "WIN TWO ROUNDS", "next": nxt}

    def commit(self, app: "Application") -> None:
        if self.over and not self.committed:
            self.committed = True
            save = app.save
            if self.completed:
                save.tickets_won.add(self.boss.key)
                save.ticket_active = ""
                for uid in self.boss.reward_ids():
                    save.grant(uid)
                self.payout = Payout(TICKET_WIN_COINS)
            else:
                self.payout = Payout(COIN_HALF)
            save.earn(self.payout)

    def finish_scene(self, app: "Application") -> "Scene":
        return TicketResultScene(app, self)

    def exit_scene(self, app: "Application") -> "Scene":
        return ShopScene(app, 4)

    def restart(self, app: "Application") -> None:
        start_ticket(app, self.boss.key)


def start_ticket(app, key: str) -> None:
    if not key or not app.save.tickets_open():               # nothing drawn / still sealed: never open a vault encounter
        app.goto(ShopScene(app, 4))
        return
    boss = TICKET_BOSS_BY_KEY.get(key)
    if boss is None:                                         # an unknown id in a save: clear it rather than getting stuck
        app.save.ticket_active = ""
        app.save.flush()
        app.goto(ShopScene(app, 4))
        return
    lo = app.save.loadout(0)
    me = PlayerSetup("PLAYER", "YOU", COLOR_BY_NAME[lo.tank_color], COLOR_BY_NAME[lo.shot_color], False, lo.tank, lo.ammo,
                     TICKET_PLAYER_HP, team=0)
    foe = PlayerSetup(boss.name, "BOSS", DESIGN_BY_KEY[boss.tank].accent, (255, 255, 255), True, boss.tank, boss.ammo, boss.hp, team=1,
                      diff=Difficulty.MASTER, prec=boss.prec)
    cfg = MatchConfig([me, foe], False, Difficulty.MASTER, "ticket", target=2)
    app.goto(BattleScene(app, TicketSession(boss, cfg, app.rng.randrange(1 << 30))))


def ticket_art(save: "SaveData", t: float) -> PixelCanvas:
    """The shop preview for the Terminal Ticket: a mystery ticket, or the drawn boss once one is active."""
    cv = PixelCanvas(34, 20, (0, 0, 0))
    for py in range(20):
        cv.rows[py] = [mix((10, 6, 28), (60, 20, 80), py / 19)] * 34
    boss = TICKET_BOSS_BY_KEY.get(save.ticket_active)
    if boss:
        for x in range(34):
            for y in range(3):
                cv.plot(x, y, (40, 36, 60))
        draw_tank(cv, 17.0, 3, -1, 130, DESIGN_BY_KEY[boss.tank].accent, scale=2, design=DESIGN_BY_KEY[boss.tank], t=t)
        return cv
    for x in range(5, 29):                                   # a ticket with a notch on each side and a question mark
        for y in range(5, 15):
            if not ((x in (5, 28)) and y in (9, 10)):
                cv.plot(x, y, mix((230, 190, 90), (255, 240, 170), 0.5 + 0.5 * math.sin(t * 2 + x * 0.3)))
    for y in range(6, 14):
        cv.plot(8, y, (150, 110, 40))
    for (x, y) in ((16, 12), (17, 12), (18, 12), (18, 11), (18, 10), (17, 9), (16, 9), (16, 8), (17, 8), (18, 8)):
        pass
    for (x, y) in ((15, 11), (16, 12), (17, 12), (18, 11), (18, 10), (17, 9), (17, 8), (17, 6)):
        cv.plot(x + 4, y, (90, 40, 120))
    cv.plot(21, 6, (90, 40, 120))
    return cv


class TournamentSession(MatchSession):
    """One run at a tournament level: win `level.wins` rounds against the boss under the level's rule."""

    exit_label = "TOURNAMENT"

    def __init__(self, level: TournamentLevel, config: MatchConfig, seed: Optional[int]) -> None:
        self.level = level
        self.first_clear = False
        self.committed = False
        super().__init__(config, None, seed)

    @property
    def wind_override(self) -> Optional[float]:
        return self.level.wind_override

    def _pick_map(self) -> MapDefinition:
        return SPECIAL_MAP_BY_KEY[self.level.map_key] if self.level.map_key else super()._pick_map()

    @property
    def strikes(self) -> int:
        s = self.level.strikes_allowed
        return 0 if s is None else min(self.wins[1], s)

    def banner_note(self) -> str:
        lv, bw = self.level, self.wins[1]
        s = lv.strikes_allowed
        if s and bw >= s:
            return "FINAL WARNING - ONE MORE LOSS FAILS THE LEVEL"
        if s and bw:
            return f"{bw} OF {s} STRIKES USED"
        if lv.rule and self.round_no == 1:
            return f"{lv.rule_tag}: {lv.rule_text}"
        if lv.consecutive and bw:
            return f"STREAK BROKEN {bw}X - BACK TO ZERO"
        return ""

    def record_round(self, winner: int) -> None:
        self.wins[winner] += 1
        if winner == 1 and self.level.consecutive:
            self.wins[0] = 0                     # a loss breaks the streak

    @property
    def completed(self) -> bool:
        return self.wins[0] >= self.level.wins

    @property
    def failed(self) -> bool:
        f = self.level.fail_at
        return f is not None and self.wins[1] >= f and not self.completed

    @property
    def over(self) -> bool:
        return self.completed or self.failed

    @property
    def champion(self) -> int:
        return 0 if self.completed else 1

    def hud_wins(self, i: int) -> str:
        lv, need = self.level, self.level.wins
        if i == 1:
            if lv.fail_at is None:
                return f"BOSS WINS {self.wins[1]}"
            if lv.fail_at == 1:
                return "NO LOSSES ALLOWED"
            s = self.strikes
            return f"BOSS {self.wins[1]}/{lv.fail_at}  STRIKES {'■' * s}{'□' * (lv.strikes_allowed - s)}"
        pips = ("●" * self.wins[0] + "○" * (need - self.wins[0])) if need <= 5 else f"{self.wins[0]}"
        return f"{'STREAK' if lv.consecutive else 'WINS'} {pips} {self.wins[0]}/{need}"

    def hud_title(self) -> str:
        tag = f"  ·  {self.level.rule_tag}" if self.level.rule else ""
        return f"LEVEL {self.level.number}  ·  ROUND {self.round_no}  ·  {self.current_map.name}{tag}"

    def banner_title(self) -> str:
        return f"LEVEL {self.level.number} - ROUND {self.round_no}"

    def round_summary(self, winner: int) -> dict:
        lv, need = self.level, self.level.wins
        bw, f = self.wins[1], lv.fail_at
        if self.completed:
            nxt = "LEVEL CLEARED!"
        elif self.failed:
            nxt = "LEVEL FAILED - YOU LOST A ROUND" if f == 1 else f"LEVEL FAILED - {lv.boss} WON {f} ROUNDS"
        elif winner == 0:
            nxt = f"{need - self.wins[0]} MORE TO GO"
        elif lv.consecutive:
            nxt = "STREAK BROKEN - BACK TO ZERO"
        elif bw >= lv.strikes_allowed:
            nxt = f"STRIKE {lv.strikes_allowed} OF {lv.strikes_allowed} - ONE MORE LOSS FAILS THE LEVEL"
        else:
            nxt = f"STRIKE {bw} OF {lv.strikes_allowed} - WARNING"
        boss_score = f"{bw}/{f}" if f else f"{bw}"
        return {"title": f"LEVEL {lv.number} - ROUND {self.round_no}",
                "headline": "YOU WIN THE ROUND" if winner == 0 else f"{lv.boss} WINS THE ROUND",
                "score": f"{self.wins[0]}/{need}  ·  BOSS {boss_score}", "sub": lv.objective, "next": nxt}

    def commit(self, app: "Application") -> None:
        if self.over and not self.committed:
            self.committed = True
            if self.completed:
                self.first_clear = app.save.complete_level(self.level.number)
                if self.first_clear and self.level.number == len(TOURNAMENT):
                    app.settings.home_mode = "MASTER"      # reveal the Master home screen right away
                    app.apply_settings()
            self.payout = tournament_payout(self.level, self.completed, self.first_clear)
            app.save.earn(self.payout)

    def finish_scene(self, app: "Application") -> "Scene":
        return TournamentResultScene(app, self)

    def exit_scene(self, app: "Application") -> "Scene":
        return TournamentScene(app, self.level.number)

    def restart(self, app: "Application") -> None:
        app.start_tournament(self.level, self.config)


# ============================================================================
# UI helpers
# ============================================================================
_GLYPHS = {
    "T": ["████████╗", "╚══██╔══╝", "   ██║   ", "   ██║   ", "   ██║   ", "   ╚═╝   "],
    "E": ["███████╗", "██╔════╝", "█████╗  ", "██╔══╝  ", "███████╗", "╚══════╝"],
    "R": ["██████╗ ", "██╔══██╗", "██████╔╝", "██╔══██╗", "██║  ██║", "╚═╝  ╚═╝"],
    "M": ["███╗   ███╗", "████╗ ████║", "██╔████╔██║", "██║╚██╔╝██║", "██║ ╚═╝ ██║", "╚═╝     ╚═╝"],
    "I": ["██╗", "██║", "██║", "██║", "██║", "╚═╝"],
    "N": ["███╗   ██╗", "████╗  ██║", "██╔██╗ ██║", "██║╚██╗██║", "██║ ╚████║", "╚═╝  ╚═══╝"],
    "A": [" █████╗ ", "██╔══██╗", "███████║", "██╔══██║", "██║  ██║", "╚═╝  ╚═╝"],
    "L": ["██╗     ", "██║     ", "██║     ", "██║     ", "███████╗", "╚══════╝"],
    "K": ["██╗  ██╗", "██║ ██╔╝", "█████╔╝ ", "██╔═██╗ ", "██║  ██╗", "╚═╝  ╚═╝"],
    "S": ["███████╗", "██╔════╝", "███████╗", "╚════██║", "███████║", "╚══════╝"],
}
_DIGITS = {
    "0": ("███", "█ █", "█ █", "█ █", "███"), "1": (" █ ", "██ ", " █ ", " █ ", "███"),
    "2": ("███", "  █", "███", "█  ", "███"), "3": ("███", "  █", "███", "  █", "███"),
}


def big_word(word: str) -> list[str]:
    rows = [""] * 6
    for ch in word:
        g = _GLYPHS[ch]
        w = max(len(r) for r in g)
        for i in range(6):
            rows[i] += g[i].ljust(w)
    return rows


def draw_logo(screen: Screen, y: int, t: float, compact: bool) -> int:
    """Draws the gradient logo with a travelling shimmer. Returns the next free row."""
    words = [big_word("TERMINAL")] if compact else [big_word("TERMINAL"), big_word("TANKS")]
    shimmer = (t * 42) % 140 - 30
    for wi, rows in enumerate(words):
        width = len(rows[0])
        x0 = (screen.cols - width) // 2
        for r, line in enumerate(rows):
            top = mix((150, 255, 235), (50, 150, 235), (r + wi * 6) / 11)
            for i, ch in enumerate(line):
                if ch == " ":
                    continue
                if ch == "█":
                    b = max(0.0, 1 - abs(i + r * 2 + wi * 14 - shimmer) / 9)
                    screen.put(x0 + i, y, ch, mix(top, (255, 255, 255), b * 0.75), Palette.BG)
                else:
                    screen.put(x0 + i, y, ch, (30, 76, 100), Palette.BG)
            y += 1
        y += 0 if wi else 1
    if compact:
        screen.center(y, "▀▄▀  T  A  N  K  S  ▀▄▀", Palette.AMBER)
        y += 1
    return y


def draw_hp(screen: Screen, x: int, y: int, t: Tank, bg: RGB, right: bool = False) -> int:
    """Hearts for small health pools, a bar plus number for big ones. Returns the width drawn."""
    if t.max_hp <= 10:
        w = t.max_hp * 2 - 1
        x0 = x - w + 1 if right else x
        for k in range(t.max_hp):
            screen.put(x0 + k * 2, y, "♥" if k < t.hp else "♡", Palette.HEART if k < t.hp else Palette.HEART_OFF, bg)
        return w
    n = 0 if t.hp <= 0 else max(1, min(10, math.ceil(10 * t.hp / t.max_hp)))
    label = f" {max(t.hp, 0)}/{t.max_hp}"
    w = 10 + len(label)
    x0 = x - w + 1 if right else x
    for k in range(10):
        screen.put(x0 + k, y, "█" if k < n else "░", Palette.HEART if k < n else Palette.HEART_OFF, bg)
    screen.text(x0 + 10, y, label, Palette.TEXT, bg)
    return w


def hearts(hp: int, spaced: bool = True, total: int = MAX_HP) -> str:
    if total > 10:
        return f"♥{max(hp, 0)}/{total}"
    sep = " " if spaced else ""
    return sep.join("♥" if i < hp else "♡" for i in range(total))


def draw_bar(screen: Screen, x: int, y: int, w: int, frac: float) -> None:
    frac = clamp(frac, 0, 1)
    full = frac * w
    for i in range(w):
        if i + 1 <= full:
            ch, col = "█", gradient(((0, Palette.OK), (0.6, Palette.AMBER), (1, Palette.DANGER)), i / max(1, w - 1))
        elif i < full:
            ch, col = "▌", gradient(((0, Palette.OK), (0.6, Palette.AMBER), (1, Palette.DANGER)), i / max(1, w - 1))
        else:
            ch, col = "░", (52, 66, 78)
        screen.put(x + i, y, ch, col, Palette.PANEL)


class MenuList:
    def __init__(self, items: Sequence[str]) -> None:
        self.items, self.index = list(items), 0

    def handle(self, ev: InputEvent) -> Optional[int]:
        if ev.action is Action.UP:
            self.index = (self.index - 1) % len(self.items)
        elif ev.action is Action.DOWN:
            self.index = (self.index + 1) % len(self.items)
        elif ev.confirm:
            return self.index
        return None

    def draw(self, screen: Screen, cx: int, y: int, t: float, gap: int = 1, width: int = 30) -> None:
        for i, label in enumerate(self.items):
            row = y + i * gap
            x0 = cx - width // 2
            if i == self.index:
                pulse = 0.5 + 0.5 * math.sin(t * 6)
                screen.fill(x0, row, width, 1, Palette.PANEL_HI)
                screen.center(row, label, Palette.WHITE, Palette.PANEL_HI, x0, width)
                arrow = mix(Palette.PRIMARY_DIM, Palette.PRIMARY, pulse)
                screen.put(x0 + 1, row, "▶", arrow, Palette.PANEL_HI)
                screen.put(x0 + width - 2, row, "◀", arrow, Palette.PANEL_HI)
            else:
                screen.center(row, label, Palette.MUTED, None, x0, width)


def draw_keycaps(screen: Screen, x: int, y: int, pairs: Sequence[tuple[str, str]], bg: Optional[RGB] = None) -> None:
    for key, desc in pairs:
        screen.text(x, y, key, Palette.AMBER, bg)
        x += len(key) + 1
        screen.text(x, y, desc, Palette.MUTED, bg)
        x += len(desc) + 3


def panel(screen: Screen, w: int, h: int, title: str, y: Optional[int] = None, fg: RGB = Palette.LINE) -> tuple[int, int]:
    x = (screen.cols - w) // 2
    y = (screen.rows - h) // 2 if y is None else y
    screen.box(x, y, w, h, "double", fg, Palette.PANEL, title)
    return x, y


def wrap_center(screen: Screen, y: int, text: str, width: int, fg: RGB, bg: Optional[RGB], x: int, w: int, lines: int = 2) -> int:
    """Centred, word-wrapped text that never gets cut mid-word. Returns rows used."""
    parts = textwrap.wrap(text, width) or [""]
    if len(parts) > lines:
        parts = parts[:lines]
        parts[-1] = parts[-1][:max(0, width - 1)].rstrip() + "…"
    for k, ln in enumerate(parts):
        screen.center(y + k, ln, fg, bg, x, w)
    return len(parts)


def render_map_preview(cache: dict, mapdef: MapDefinition, w: int, h: int, t: float, tanks: list) -> PixelCanvas:
    """Terrain scan used by the rules screens. tanks = [(design, colour), (design, colour)]."""
    key = (mapdef.key, w, h)
    if key not in cache:
        rng = random.Random(1)
        terrain = Terrain(w, h, mapdef, rng)
        xs = []
        for i in range(2):
            x = int(mapdef.spawns[i] * w)
            terrain.flatten(x + 0.5, 10)
            xs.append(x + 0.5)
        scen = Scenery(w, h, mapdef.theme, rng)
        scen.apply_terrain(terrain)
        cache[key] = (scen, terrain, xs)
    scen, terrain, xs = cache[key]
    cv = scen.canvas()
    scen.draw_stars(cv, t)
    scen.draw_aurora(cv, t)
    scen.draw_fx(cv, t)
    for i, x in enumerate(xs):
        d, col = tanks[i]
        draw_tank(cv, x, terrain.support_height(x), 1 if i == 0 else -1, 50 if i == 0 else 130, col, design=d, t=t)
    return cv


# ============================================================================
# Ambient backdrop (menus) + attract-mode shells
# ============================================================================
class AmbientBackdrop:
    def __init__(self, seed: int = 11) -> None:
        self.rng = random.Random(seed)
        self.ps = ParticleSystem(1600)
        self.explosions: list[Explosion] = []
        self.shells: list[dict] = []
        self.size = (0, 0)
        self.layer: Optional[PixelCanvas] = None
        self.ridge: list[int] = []
        self.stars: list = []
        self.spawn_timer = 1.0
        self.celebrate: Optional[list] = None
        self.fw_timer = 0.0

    def ensure(self, cols: int, rows: int) -> None:
        if self.size == (cols, rows):
            return
        self.size = (cols, rows)
        w, h = cols, rows * 2
        r = self.rng
        ph = [r.uniform(0, math.tau) for _ in range(6)]
        self.ridge = [int(h * (0.22 + 0.06 * math.sin(x / w * math.tau * 2 + ph[0]) + 0.04 * math.sin(x / w * math.tau * 6 + ph[1])
                               + 0.015 * math.sin(x / w * math.tau * 19 + ph[2]))) for x in range(w)]
        far = [int(h * (0.34 + 0.08 * math.sin(x / w * math.tau * 3 + ph[3]) + 0.04 * math.sin(x / w * math.tau * 9 + ph[4]))) for x in range(w)]
        rows_px = []
        for py in range(h):
            y = h - 1 - py
            base = gradient(((0, (4, 8, 20)), (0.55, (8, 26, 44)), (0.85, (16, 70, 84)), (1, (30, 110, 120))),
                            1 - y / h) if False else gradient(((0, (30, 110, 120)), (0.25, (16, 62, 78)), (0.6, (8, 24, 42)), (1, (4, 8, 20))), y / h)
            row = []
            for x in range(w):
                c = base
                if y < self.ridge[x]:
                    c = mix((10, 26, 34), (4, 12, 18), 1 - y / max(1, self.ridge[x]))
                elif y < far[x]:
                    c = mix(base, (20, 48, 62), 0.75)
                vx, vy = (x / w - 0.5) * 2, (py / h - 0.5) * 2
                c = shade(c, max(0.25, 1 - 0.42 * (vx * vx + vy * vy) * 0.8))
                if (py // 2) % 2:
                    c = shade(c, 0.9)
                row.append(c)
            rows_px.append(row)
        self.layer = PixelCanvas(w, h, rows=rows_px)
        self.stars = [(r.randrange(w), r.randrange(int(h * 0.6)), r.uniform(0, 6), r.uniform(1, 3)) for _ in range(w // 3)]

    def _spawn_shell(self) -> None:
        r, w = self.rng, self.size[0]
        left = r.random() < 0.5
        x0 = r.uniform(0.05, 0.3) * w if left else r.uniform(0.7, 0.95) * w
        tx = r.uniform(0.3, 0.7) * w
        th = math.radians(r.uniform(48, 68))
        y0 = self.ridge[int(x0)] + 2
        hh = self.layer.h if self.layer is not None else 56
        tmax = 4 * (hh - 9 - y0) / max(4.0, abs(tx - x0))              # apex = range*tan(angle)/4 must stay inside the canvas
        if math.tan(th) > tmax:
            th = math.atan(max(0.45, tmax))
        v = math.sqrt(32 * abs(tx - x0) / math.sin(2 * th))
        self.shells.append({"l": Launch(x0, y0, math.copysign(v * math.cos(th), tx - x0), v * math.sin(th), 32),
                            "t": 0.0, "c": r.choice(COLOR_CHOICES)[1]})

    def celebrate_with(self, colors: Optional[list]) -> None:
        self.celebrate = colors

    def update(self, dt: float) -> None:
        if self.layer is None:
            return
        self.ps.update(dt)
        for e in self.explosions:
            e.update(dt)
        self.explosions = [e for e in self.explosions if not e.done]
        self.spawn_timer -= dt
        if self.spawn_timer <= 0 and len(self.shells) < 3 and not self.celebrate:
            self.spawn_timer = self.rng.uniform(1.0, 2.8)
            self._spawn_shell()
        for s in self.shells[:]:
            s["t"] += dt
            x, y = s["l"].at(s["t"])
            self.ps.emit(Particle(x, y, 0, 0, 0.6, 0.6, mix(Palette.WHITE, s["c"], 0.5), shade(s["c"], 0.15), alpha=0.9))
            if x < 0 or x >= self.size[0] or y < self.ridge[int(x)]:
                self.shells.remove(s)
                if 0 <= x < self.size[0]:
                    self.explosions.append(Explosion(x, y, 3.5, s["c"], 0.7))
                    for _ in range(18):
                        a, sp = self.rng.uniform(0.2, 2.9), self.rng.uniform(10, 34)
                        self.ps.emit(Particle(x, y, math.cos(a) * sp, math.sin(a) * sp, 0.8, 0.8, Palette.WHITE, shade(s["c"], 0.2), gravity=60))
        if self.celebrate:
            self.fw_timer -= dt
            if self.fw_timer <= 0:
                self.fw_timer = self.rng.uniform(0.25, 0.6)
                bx, by = self.rng.uniform(0.15, 0.85) * self.size[0], self.rng.uniform(0.45, 0.85) * self.size[1] * 2
                col = self.rng.choice(self.celebrate)
                for _ in range(60):
                    a, sp = self.rng.uniform(0, math.tau), self.rng.uniform(8, 34)
                    life = self.rng.uniform(0.9, 1.6)
                    self.ps.emit(Particle(bx, by, math.cos(a) * sp, math.sin(a) * sp, life, life, mix(Palette.WHITE, col, 0.5),
                                          shade(col, 0.2), gravity=26, drag=1.1))

    def draw(self, screen: Screen, t: float) -> None:
        self.ensure(screen.cols, screen.rows)
        canvas = self.layer.copy()
        for x, py, ph, sp in self.stars:
            canvas.rows[py][x] = mix(canvas.rows[py][x], (220, 240, 255), 0.25 + 0.5 * math.sin(t * sp + ph) ** 2)
        self.ps.draw(canvas)
        for e in self.explosions:
            e.draw(canvas)
        for s in self.shells:
            x, y = s["l"].at(s["t"])
            canvas.plotf(x, y, Palette.WHITE)
        screen.blit(canvas, 0, 0)


# ============================================================================
# Preview stage (tank + ammo demo used by setup, tournament and reward screens)
# ============================================================================
class PreviewStage:
    """A tiny looping range: the tank aims, fires one shell in its ammo's style and the shell detonates."""
    PERIOD = 3.4

    def __init__(self, settings: Settings, w: int = 46, h: int = 26, scale: int = 2, seed: int = 3) -> None:
        self.w, self.h, self.scale = w, h, scale
        self.ps = ParticleSystem(500)
        self.fx = EffectsFactory(self.ps, settings, random.Random(seed))
        self.explosions: list[Explosion] = []
        self.design, self.ammo = DEFAULT_DESIGN, DEFAULT_AMMO
        self.body, self.shot = COLOR_CHOICES[0][1], COLOR_CHOICES[6][1]
        self.t = self.clock = 0.0
        self.phase = 0
        self.n = 0
        self.recoil = 0.0
        self.sky = [[mix((14, 22, 60), (200, 120, 110), (py / h) ** 1.4)] * w for py in range(h)]
        self.dim = 0.0
        self.scn = None
        self.gy = self.ty = 4.0

    def set_map(self, mapdef: Optional[MapDefinition]) -> None:
        """Use a real battlefield (sky, terrain, aurora, weather) as the backdrop; None = the plain workshop range."""
        self.t, self.phase = 0.0, 0
        self.explosions.clear()
        if mapdef is None:
            self.scn, self.gy, self.ty = None, 4.0, 4.0
            return
        rng = random.Random(5)
        terrain = Terrain(self.w, self.h, mapdef, rng)
        terrain.flatten(self.tank_x, 10)
        terrain.flatten(self.target_x, 10)
        self.scn = Scenery(self.w, self.h, mapdef.theme, rng)
        self.scn.apply_terrain(terrain)
        self.gy, self.ty = float(terrain.support_height(int(self.tank_x))), float(terrain.support_height(self.target_x))

    def set_kit(self, design: TankDesign, body: RGB, ammo: AmmoType, shot: RGB, dim: float = 0.0) -> None:
        if (design, ammo, body, shot) != (self.design, self.ammo, self.body, self.shot):
            self.t, self.phase = 0.0, 0
            self.ps.items.clear()
            self.explosions.clear()
        self.design, self.ammo, self.body, self.shot, self.dim = design, ammo, body, shot, dim

    @property
    def target_x(self) -> int:
        return self.w - 6

    def update(self, dt: float) -> None:
        self.t += dt
        self.clock += dt
        self.recoil = max(0.0, self.recoil - dt)
        self.ps.update(dt)
        self.fx.ambient(self.design, self.tank_x, self.gy, live_color(self.body, self.clock), dt)     # particle ambients (embers, mist, sparks...)
        for e in self.explosions:
            e.update(dt)
        self.explosions = [e for e in self.explosions if not e.done]
        if self.t > self.PERIOD:
            self.t, self.phase = 0.0, 0
        if self.phase == 0 and self.t > 0.6:
            self.phase, self.recoil = 1, 0.2
            self._launch()
        if self.phase == 1:
            u = (self.t - 0.6) / 1.2
            x, y = self._shell(u)
            self.n += 1
            shot = live_color(self.shot, self.clock)
            self.fx.trail(x, y, shot, self.ammo, self.n, self.clock)
            if u >= 1:
                self.phase = 2
                self.explosions.append(Explosion(self.target_x, self.ty, 4.5, shot, 0.8, self.ammo.boom))
                self.fx.impact(self.target_x, self.ty, shot, (92, 84, 70), 0.6, self.ammo)

    def _angle(self) -> float:
        return 26 + 9 * math.sin(self.clock * 2.2)

    @property
    def tank_x(self) -> float:
        return 5.5 * self.scale + 6

    def _launch(self) -> None:
        """Latch the muzzle position and barrel angle at the instant of firing; the shell then arcs from there to the target."""
        ang = self._angle()
        self.fire_angle = ang
        self.fire_from = muzzle_point(self.tank_x, self.gy + visual_lift(self.design, self.clock, self.scale), ang, self.design, self.scale)

    def _shell(self, u: float) -> tuple[float, float]:
        x0, y0 = getattr(self, "fire_from", (self.tank_x, 8.0))
        x1, y1 = self.target_x, self.ty
        k = math.tan(math.radians(getattr(self, "fire_angle", 26.0))) * (x1 - x0) - (y1 - y0)    # start slope == barrel slope
        return x0 + (x1 - x0) * u, y0 + (y1 - y0) * u + k * u * (1 - u)

    def render(self) -> PixelCanvas:
        w, h, s = self.w, self.h, self.scale
        if self.scn is not None:
            cv = self.scn.canvas()
            self.scn.draw_stars(cv, self.clock)
            self.scn.draw_aurora(cv, self.clock)
            self.scn.draw_fx(cv, self.clock)
        else:
            cv = PixelCanvas(w, h, rows=[r[:] for r in self.sky])
            for x in range(w):
                for y in range(3):
                    cv.plot(x, y, shade((92, 84, 70), 0.75 + 0.1 * ((x + y) % 3)))
                cv.plot(x, 3, (110, 210, 120))
        draw_tank(cv, self.tank_x, self.gy, 1, self._angle() if self.phase != 1 else self.fire_angle, self.body, scale=s, recoil=self.recoil,
                  design=self.design, t=self.clock)
        if self.phase == 1:
            u = (self.t - 0.6) / 1.2
            x, y = self._shell(u)
            x2, y2 = self._shell(min(1.0, u + 0.02))
            draw_projectile(cv, x, y, x2 - x + 1e-3, y2 - y, self.shot, self.ammo, self.clock)
        self.ps.draw(cv)
        for e in self.explosions:
            e.draw(cv)
        if self.dim:
            for row in cv.rows:
                row[:] = [mix(c, (6, 10, 16), self.dim) for c in row]
        return cv


# ============================================================================
# Master home screen: aurora sky, citadel, a crowned tank firing prismatic shells
# ============================================================================
GOLD = (255, 206, 100)


class MasterBackdrop:
    """Same interface as AmbientBackdrop, but a far richer scene."""

    def __init__(self, settings: Settings, seed: int = 21) -> None:
        self.rng = random.Random(seed)
        self.settings = settings
        self.ps = ParticleSystem(2600)
        self.fx = EffectsFactory(self.ps, settings, self.rng)
        self.explosions: list[Explosion] = []
        self.shells: list[dict] = []
        self.size = (0, 0)
        self.layer: Optional[PixelCanvas] = None
        self.ridge: list[int] = []
        self.stars: list = []
        self.beacons: list = []
        self.shooting: list[list] = []
        self.timer = 1.5
        self.fire_timer = 2.0
        self.clock = 0.0
        self.celebrate: Optional[list] = None
        self.fw_timer = 0.0
        self.hero_x = 0.0
        self.hero_angle = 40.0
        self.hero_target = 40.0
        self.recoil = 0.0
        self.pending: Optional[tuple] = None
        self.n = 0

    def celebrate_with(self, colors: Optional[list]) -> None:
        self.celebrate = colors

    def ensure(self, cols: int, rows: int) -> None:
        if self.size == (cols, rows):
            return
        self.size = (cols, rows)
        w, h = cols, rows * 2
        r = self.rng
        sky = ((0, (255, 166, 96)), (0.16, (214, 78, 124)), (0.42, (74, 26, 116)), (1, (5, 3, 24)))
        ph = [r.uniform(0, math.tau) for _ in range(6)]
        far = [int(h * (0.30 + 0.07 * math.sin(x / w * math.tau * 3 + ph[0]) + 0.04 * math.sin(x / w * math.tau * 9 + ph[1]))) for x in range(w)]
        ground = [int(h * (0.15 + 0.03 * math.sin(x / w * math.tau * 2.4 + ph[2]) + 0.015 * math.sin(x / w * math.tau * 13 + ph[3]))) for x in range(w)]
        self.hero_x = w * 0.13
        hx = int(self.hero_x)
        plat = ground[hx]
        for x in range(max(0, hx - 10), min(w, hx + 11)):
            ground[x] = int(lerp(ground[x], plat + 2, 1.0 if abs(x - hx) <= 7 else 0.5))
        # the citadel: curtain wall, towers and spires on the right
        cx = int(w * 0.80)
        self.beacons = []
        wall = int(h * 0.26)
        towers = [(-0.17, 0.42, 5), (-0.09, 0.34, 4), (-0.02, 0.62, 6), (0.07, 0.38, 4), (0.15, 0.48, 5)]
        cit = [0] * w
        windows = []
        for off, th, tw in towers:
            tx = cx + int(off * w)
            top = int(h * th)
            for x in range(tx - tw, tx + tw + 1):
                if 0 <= x < w:
                    cit[x] = max(cit[x], top)
            for k in range(1, 5):                                    # spire
                for x in range(tx - tw + k, tx + tw - k + 1):
                    if 0 <= x < w and (k * 2 + top) > cit[x] and k < tw:
                        cit[x] = max(cit[x], top + k * 2)
            if 0 <= tx < w:
                self.beacons.append((tx, top + tw * 2))
            for _ in range(5):
                windows.append((tx + r.randint(-tw + 1, tw - 1), r.randint(int(h * 0.2), max(int(h * 0.21), top - 2))))
        for x in range(int(cx - 0.2 * w), int(cx + 0.2 * w)):
            if 0 <= x < w:
                cit[x] = max(cit[x], wall)               # the curtain wall rises from the ordinary ground; no raised plateau line
        self.ridge = [max(ground[x], cit[x] - 1) for x in range(w)]
        rows_px = []
        for py in range(h):
            y = h - 1 - py
            base = gradient(sky, y / h)
            row = []
            for x in range(w):
                c = base
                if y < far[x]:
                    c = mix(base, (46, 20, 78), 0.7)
                if y < cit[x]:
                    c = mix(c, (10, 5, 22), 0.94)
                if y < ground[x]:
                    c = mix((20, 10, 34), (8, 4, 16), 1 - y / max(1, ground[x]))
                    if y == ground[x] - 1:
                        c = (150, 104, 56)
                vx, vy = (x / w - 0.5) * 2, (py / h - 0.5) * 2
                c = shade(c, max(0.3, 1 - 0.38 * (vx * vx + vy * vy) * 0.8))
                if (py // 2) % 2:
                    c = shade(c, 0.92)
                row.append(c)
            rows_px.append(row)
        for wx, wy in windows:
            py = h - 1 - wy
            if 0 <= wx < w and 0 <= py < h and wy < cit[wx] - 2:
                rows_px[py][wx] = (255, 196, 96)
        self.layer = PixelCanvas(w, h, rows=rows_px)
        self.stars = [(r.randrange(w), r.randrange(int(h * 0.62)), r.uniform(0, 6), r.uniform(1, 4)) for _ in range(w // 2)]

    def _sky_ok(self, x: int, y: int) -> bool:
        return 0 <= x < self.size[0] and y > self.ridge[x] + 2

    def _fire(self) -> None:
        r, w = self.rng, self.size[0]
        tx = r.uniform(0.66, 0.92) * w
        th = r.uniform(42, 62)
        self.hero_target = th
        self.pending = (tx, th)

    def update(self, dt: float) -> None:
        if self.layer is None:
            return
        self.clock += dt
        self.recoil = max(0.0, self.recoil - dt)
        w, h = self.size[0], self.size[1] * 2
        self.ps.wind = 0.0
        self.ps.update(dt)
        for e in self.explosions:
            e.update(dt)
        self.explosions = [e for e in self.explosions if not e.done]
        r = self.rng
        # rising golden motes
        if r.random() < dt * 14 * self.settings.particle_density:
            self.ps.emit(Particle(r.uniform(0, w), r.uniform(0, h * 0.2), r.uniform(-1.5, 1.5), r.uniform(3, 8),
                                  r.uniform(3, 6), 5, (255, 214, 120), (140, 60, 120), alpha=0.85))
        # shooting stars
        self.timer -= dt
        if self.timer <= 0:
            self.timer = r.uniform(2.5, 6)
            self.shooting.append([r.uniform(0.1, 0.9) * w, r.uniform(0.6, 0.95) * h, r.uniform(40, 70), -r.uniform(10, 24), 0.0])
        for s in self.shooting:
            s[0] += s[2] * dt
            s[1] += s[3] * dt
            s[4] += dt
        self.shooting = [s for s in self.shooting if s[4] < 1.1]
        # the hero tank: aim, fire, recoil
        self.hero_angle += (self.hero_target - self.hero_angle) * min(1.0, dt * 4)
        self.fire_timer -= dt
        if self.fire_timer <= 0 and self.pending is None and not self.celebrate:
            self.fire_timer = r.uniform(2.2, 3.6)
            self._fire()
        if self.pending and abs(self.hero_angle - self.hero_target) < 1.5:
            tx, th = self.pending
            self.pending = None
            hy = self.ridge[int(self.hero_x)]
            th = self.hero_angle
            rad = math.radians(th)
            ox, oy = muzzle_point(int(self.hero_x) + 0.5, hy, th, DESIGN_BY_KEY["sovereign"], 2)
            reach = max(10.0, tx - ox)
            reach = min(reach, 4 * max(10.0, self.size[1] * 2 - 10 - oy) / max(0.3, math.tan(rad)))    # never let the arc leave the canvas
            v = math.sqrt(32 * reach / max(0.2, math.sin(2 * rad)))
            self.shells.append({"l": Launch(ox, oy, v * math.cos(rad), v * math.sin(rad), 32.0), "t": 0.0})
            self.recoil = 0.25
            self.fx.muzzle(ox, oy, th, (255, 214, 120))
        for s in self.shells[:]:
            s["t"] += dt
            x, y = s["l"].at(s["t"])
            self.n += 1
            self.fx.trail(x, y, GOLD, AMMO_BY_KEY["aurora"], self.n, self.clock)
            if x < 0 or x >= w or (y < self.ridge[int(x)] and s["t"] > 0.2):
                self.shells.remove(s)
                if 0 <= x < w:
                    self.explosions.append(Explosion(x, y, 6.0, GOLD, 0.95, "prism"))
                    self.fx.impact(x, y, GOLD, (90, 60, 80), 0.9, AMMO_BY_KEY["aurora"])
        if self.celebrate:
            self.fw_timer -= dt
            if self.fw_timer <= 0:
                self.fw_timer = r.uniform(0.2, 0.5)
                bx, by = r.uniform(0.15, 0.85) * w, r.uniform(0.45, 0.85) * h
                col = r.choice(self.celebrate)
                for _ in range(60):
                    a, sp = r.uniform(0, math.tau), r.uniform(8, 34)
                    life = r.uniform(0.9, 1.6)
                    self.ps.emit(Particle(bx, by, math.cos(a) * sp, math.sin(a) * sp, life, life, mix(Palette.WHITE, col, 0.5),
                                          shade(col, 0.2), gravity=26, drag=1.1))

    def draw(self, screen: Screen, t: float) -> None:
        self.ensure(screen.cols, screen.rows)
        cv = self.layer.copy()
        w, h, c = cv.w, cv.h, self.clock
        # aurora ribbons
        for b in range(3):
            for x in range(0, w):
                mid = h * (0.64 + 0.09 * b) + h * 0.06 * math.sin(x * 0.045 + c * 0.6 + b * 2.1) + h * 0.035 * math.sin(x * 0.11 - c * 0.9 + b)
                thick = 5 + 3 * math.sin(x * 0.07 + c + b)
                col = hsv(0.46 + 0.22 * math.sin(x * 0.02 + c * 0.25 + b * 1.3), 0.65, 1.0)
                for dy in range(-int(thick), int(thick) + 1):
                    y = int(mid) + dy
                    if self._sky_ok(x, y):
                        cv.blend(x, y, col, 0.30 * (1 - abs(dy) / (thick + 1)) ** 1.3)
        # stars + shooting stars
        for x, py, ph, sp in self.stars:
            if self._sky_ok(x, h - 1 - py):
                cv.rows[py][x] = mix(cv.rows[py][x], (240, 235, 255), 0.25 + 0.55 * math.sin(c * sp + ph) ** 2)
        for sx, sy, vx, vy, age in self.shooting:
            for k in range(10):
                cv.blendf(sx - vx * 0.03 * k, sy - vy * 0.03 * k, (255, 240, 220), (1 - k / 10) * (1 - age / 1.1))
        # rotating sunburst behind the logo
        cx, cy = w * 0.5, h * 0.74
        for k in range(16):
            a = k * math.tau / 16 + c * 0.05
            ca, sa = math.cos(a), math.sin(a)
            for s in range(6, int(h * 0.5), 1):
                x, y = cx + ca * s * 1.7, cy + sa * s * 0.8
                if self._sky_ok(int(x), int(y)):
                    cv.blendf(x, y, GOLD, 0.075 * (1 - s / (h * 0.5)))
        # beacons
        for bx, by in self.beacons:
            pulse = 0.5 + 0.5 * math.sin(c * 3 + bx)
            for dx in range(-2, 3):
                for dy in range(-2, 3):
                    cv.blendf(bx + dx, by + dy, (255, 150, 90), 0.5 * pulse / (1 + abs(dx) + abs(dy)))
        self.ps.draw(cv)
        hy = self.ridge[int(self.hero_x)]
        draw_tank(cv, int(self.hero_x) + 0.5, hy, 1, self.hero_angle, GOLD, scale=2, recoil=self.recoil, design=DESIGN_BY_KEY["sovereign"], t=c)
        for s in self.shells:
            x, y = s["l"].at(s["t"])
            draw_projectile(cv, x, y, s["l"].vx, s["l"].vy - 32 * s["t"], GOLD, AMMO_BY_KEY["aurora"], c)
        for e in self.explosions:
            e.draw(cv)
        screen.blit(cv, 0, 0)


def prism_text(screen: Screen, x: int, y: int, text: str, t: float, base: float = 0.1, spread: float = 0.04,
               sat: float = 0.55, val: float = 1.0, bg: Optional[RGB] = None) -> None:
    for i, ch in enumerate(text):
        if ch != " ":
            screen.put(x + i, y, ch, hsv(base + 0.09 * math.sin(t * 1.5 + i * spread * 3) + i * spread * 0.2, sat, val), bg)


def draw_master_logo(screen: Screen, y: int, t: float, tall: bool) -> int:
    """Gold logo with a travelling prismatic shimmer and animated ornament lines."""
    y += 1                                                   # (ornament removed; row kept for breathing room)
    words = [big_word("TERMINAL")] + ([big_word("TANKS")] if tall else [])
    shimmer = (t * 36) % 120 - 25
    for wi, rows in enumerate(words):
        x0 = (screen.cols - len(rows[0])) // 2
        for r, line in enumerate(rows):
            top = mix((255, 236, 150), (214, 120, 60), (r + wi * 6) / 11)
            for i, ch in enumerate(line):
                if ch == " ":
                    continue
                if ch == "█":
                    b = max(0.0, 1 - abs(i + r * 2 + wi * 14 - shimmer) / 10)
                    col = mix(top, hsv(0.55 + i * 0.01 + t * 0.3, 0.45, 1.0), b * 0.8)
                    screen.put(x0 + i, y, ch, col, Palette.BG)
                else:
                    screen.put(x0 + i, y, ch, (70, 32, 90), Palette.BG)
            y += 1
    if not tall:
        sub = "◆  T  A  N  K  S  ◆"
        prism_text(screen, (screen.cols - len(sub)) // 2, y, sub, t, 0.1, 0.05, 0.6)
        y += 1
    edition = "M A S T E R   E D I T I O N"
    screen.center(y + 1, edition, GOLD)
    prism_text(screen, (screen.cols - len(edition)) // 2, y + 1, edition, t, 0.1, 0.06, 0.45)
    return y + 2


def draw_master_menu(screen: Screen, menu: "MenuList", cx: int, y: int, t: float, gap: int = 2) -> None:
    items = menu.items
    w, h = 38, len(items) * gap + 1
    x0 = cx - w // 2
    screen.fill(x0, y - 1, w, h + 2, (10, 6, 22))
    per = 2 * (w + h)
    for i in range(w):                      # animated border, top and bottom
        for yy, k in ((y - 1, i), (y + h - 0, per // 2 - i)):
            screen.put(x0 + i, yy, "━", hsv(0.1 + 0.12 * math.sin((k / per) * math.tau * 2 + t * 1.4), 0.6, 1.0), (10, 6, 22))
    for j in range(h + 2):
        screen.put(x0, y - 1 + j, "┃", hsv(0.1 + 0.1 * math.sin(j * 0.4 + t * 1.4), 0.6, 1.0), (10, 6, 22))
        screen.put(x0 + w - 1, y - 1 + j, "┃", hsv(0.1 + 0.1 * math.sin(j * 0.4 - t * 1.4), 0.6, 1.0), (10, 6, 22))
    for (cxx, cyy, ch) in ((x0, y - 1, "◆"), (x0 + w - 1, y - 1, "◆"), (x0, y + h, "◆"), (x0 + w - 1, y + h, "◆")):
        screen.put(cxx, cyy, ch, GOLD, (10, 6, 22))
    for i, label in enumerate(items):
        row = y + i * gap
        if i == menu.index:
            for k in range(w - 4):
                glow = 0.5 + 0.5 * math.sin(t * 5 - k * 0.15)
                screen.put(x0 + 2 + k, row, " ", Palette.WHITE, mix((60, 34, 8), (120, 70, 16), glow))
            screen.center(row, label, (255, 248, 220), None, x0, w)
            screen.put(x0 + 3, row, "◆", GOLD, None)
            screen.put(x0 + w - 4, row, "◆", GOLD, None)
        else:
            screen.center(row, label, (178, 140, 110), (10, 6, 22), x0, w)


class SecretBackdrop:
    """The secret home theme: green code rain over a dark CRT, with an ASCIIBOT lobbing glyph shells along the bottom."""
    GLYPHS = "01ABCDEF{}[]<>/\\|#$%&*+=~;:?"

    def __init__(self, settings: Settings, seed: int = 33) -> None:
        self.rng = random.Random(seed)
        self.settings = settings
        self.size = (0, 0)
        self.drops: list = []
        self.clock = 0.0
        self.celebrate: Optional[list] = None
        self.shells: list = []
        self.explosions: list = []
        self.timer = 1.2
        self.recoil = 0.0

    def celebrate_with(self, colors: Optional[list]) -> None:
        self.celebrate = colors

    def ensure(self, cols: int, rows: int) -> None:
        if self.size == (cols, rows):
            return
        self.size = (cols, rows)
        r = self.rng
        self.drops = [[r.uniform(-rows, rows), r.uniform(8, 26), r.randint(6, 16)] for _ in range(cols)]

    def update(self, dt: float) -> None:
        self.clock += dt
        rows = self.size[1]
        for d in self.drops:
            d[0] += d[1] * dt
            if d[0] - d[2] > rows:
                d[0], d[1], d[2] = -self.rng.uniform(0, rows * 0.5), self.rng.uniform(8, 26), self.rng.randint(6, 16)
        self.recoil = max(0.0, self.recoil - dt)
        self.timer -= dt
        if self.timer <= 0:
            self.timer = self.rng.uniform(2.2, 3.4)
            self.recoil = 0.25
            v = self.rng.uniform(34, 44)
            design = DESIGN_BY_KEY["asciibot"]
            ang = 38 + 9 * math.sin(self.clock * 0.7)
            ox, oy = muzzle_point(11.5, 2 + visual_lift(design, self.clock, 2), ang, design, 2)
            a = math.radians(ang)
            cap = min(getattr(self, "size", (0, 20))[1] * 2, 72) - 10
            apex = oy + (v * math.sin(a)) ** 2 / 80
            if apex > cap > oy:
                v *= math.sqrt((cap - oy) / (apex - oy))
            self.shells.append({"l": Launch(ox, oy, v * math.cos(a), v * math.sin(a), 40.0), "t": 0.0})
        for s in self.shells[:]:
            s["t"] += dt
            x, y = s["l"].at(s["t"])
            if y < 3.5 and s["t"] > 0.2:
                self.shells.remove(s)
                self.explosions.append(Explosion(x, 3.5, 5.5, (110, 255, 150), 0.9, "code"))
        for e in self.explosions:
            e.update(dt)
        self.explosions = [e for e in self.explosions if not e.done]

    def draw(self, screen: Screen, t: float) -> None:
        self.ensure(screen.cols, screen.rows)
        cols, rows = self.size
        for y in range(rows):
            bg = mix((2, 14, 8), (0, 32, 16), y / max(1, rows - 1))
            screen.fill(0, y, cols, 1, shade(bg, 0.8) if y % 2 else bg)          # CRT scanlines
        tick = int(t * 8)
        for x, (head, _, trail) in enumerate(self.drops):
            for i in range(trail):
                y = int(head) - i
                if 0 <= y < rows:
                    ch = self.GLYPHS[(x * 7919 + y * 104729 + (tick if i < 2 else tick // 4) * 31) % len(self.GLYPHS)]
                    screen.put(x, y, ch, (205, 255, 215) if i == 0 else mix((70, 255, 120), (4, 40, 18), i / trail))
        # the hero: an ASCIIBOT on a ground strip, drawn on black and masked onto the screen
        w, h = cols, min(rows * 2, 72)
        cv = PixelCanvas(w, h, (0, 0, 0))
        for x in range(w):
            cv.plot(x, 0, (22, 96, 48))
            cv.plot(x, 1, (40, 150, 76))
        draw_tank(cv, 11.5, 2, 1, 38 + 9 * math.sin(self.clock * 0.7), (70, 220, 120), scale=2, recoil=self.recoil,
                  design=DESIGN_BY_KEY["asciibot"], t=self.clock)
        ammo = AMMO_BY_KEY["glyph"]
        for s in self.shells:
            x, y = s["l"].at(s["t"])
            draw_projectile(cv, x, y, s["l"].vx, s["l"].vy - 40 * s["t"], (110, 255, 150), ammo, self.clock)
        for e in self.explosions:
            e.draw(cv)
        top = rows - h // 2
        for r in range(h // 2):
            if not 0 <= top + r < rows:
                continue
            for c in range(w):
                a, b = cv.rows[2 * r][c], cv.rows[2 * r + 1][c]
                if a == (0, 0, 0) and b == (0, 0, 0):
                    continue
                bgc = screen.back[top + r][c][2]
                screen.back[top + r][c] = ("▀", bgc if a == (0, 0, 0) else a, bgc if b == (0, 0, 0) else b)


def _cell(screen: Screen, x: int, y: int, col: RGB, ch: str = " ", fg: Optional[RGB] = None) -> None:
    if 0 <= x < screen.cols and 0 <= y < screen.rows:
        screen.put(x, y, ch, fg or col, col)


def _L_disc(self, screen, cols, rows, cx, cy, r, col, glow=True):
    for y in range(int(cy - r * 1.8), int(cy + r * 1.8) + 1):
        for x in range(int(cx - r * 3.4), int(cx + r * 3.4) + 1):
            d = math.hypot((x - cx) / 2.0, y - cy)
            if d <= r:
                _cell(screen, x, y, col)
            elif glow and d <= r * 1.7 and 0 <= y < rows and 0 <= x < cols:
                bg = screen.back[y][x][2]
                screen.back[y][x] = (screen.back[y][x][0], screen.back[y][x][1], mix(bg, col, 0.32 * (1 - (d - r) / (r * 0.7))))


def _L_moon(self, screen, cols, rows, t):
    r = max(3, rows // 8)
    cx, cy = cols * 0.2, rows * 0.22
    _L_disc(self, screen, cols, rows, cx, cy, r, mix(self.th.accent, (255, 255, 255), 0.7))
    for dx, dy, cr in ((-0.3, -0.2, 0.28), (0.25, 0.25, 0.22), (0.1, -0.4, 0.15)):
        _L_disc(self, screen, cols, rows, cx + dx * r * 2, cy + dy * r, r * cr, mix(self.th.accent, (60, 60, 80), 0.4), False)


def _L_sun(self, screen, cols, rows, t):
    r = max(4, rows // 6)
    cx, cy = cols * 0.66, rows * 0.58 + math.sin(t * 0.2) * 0.4
    _L_disc(self, screen, cols, rows, cx, cy, r, mix(self.th.accent, (255, 255, 235), 0.5))
    for k in range(0, int(r * 2), 3):                                    # horizontal slits through the lower half (retro sun)
        for x in range(int(cx - r * 2), int(cx + r * 2) + 1):
            y = int(cy + r * 0.2 + k * 0.6)
            if 0 <= y < rows and 0 <= x < cols and math.hypot((x - cx) / 2.0, y - cy) <= r:
                screen.back[y][x] = (" ", (0, 0, 0), self.rowcol[y])


def _L_city(self, screen, cols, rows, t):
    r = random.Random(self.th.key)
    x, hz = 0, int(rows * 0.76)
    while x < cols:
        bw, bh = r.randint(4, 9), r.randint(rows // 8, rows // 3)
        for yy in range(hz - bh, hz):
            for xx in range(x, min(cols, x + bw)):
                lit = (xx * 7 + yy * 13 + x) % 5 < 2 and (xx - x) % 2 == 1 and (yy % 2 == 0) and (int(t * 0.4) + xx + yy) % 11 != 0
                _cell(screen, xx, yy, mix(self.th.far, (0, 0, 0), 0.45) if not lit else mix(self.th.accent, (255, 230, 160), 0.5))
        x += bw + r.randint(0, 2)


def _L_mountains(self, screen, cols, rows, t):
    hz = int(rows * 0.76)
    for x in range(cols):
        h = int(rows * 0.2 + rows * 0.12 * math.sin(x * 0.045 + 1) + rows * 0.07 * math.sin(x * 0.11) + 3 * abs(math.sin(x * 0.02)))
        for k in range(h):
            y = hz - k
            cap = k >= h - 2 and h > rows * 0.24
            _cell(screen, x, y, mix(self.th.far, (255, 255, 255), 0.65) if cap else mix(self.th.far, (0, 0, 0), 0.25 + 0.2 * k / max(1, h)))


def _L_grid(self, screen, cols, rows, t):
    hz = int(rows * 0.72)
    for y in range(hz, rows):
        k = (y - hz) / max(1, rows - hz)
        for x in range(cols):
            on_h = int((k * 8 + t * 0.9) * 2) % 4 == 0
            vx = (x - cols / 2) / max(0.3, 0.25 + k * 1.6)
            on_v = abs(vx - round(vx / 6) * 6) < 0.5
            bg = self.rowcol[y]
            if on_h or on_v:
                _cell(screen, x, y, mix(bg, self.th.accent, 0.45 + 0.4 * k))
            else:
                _cell(screen, x, y, mix(bg, (0, 0, 0), 0.55))


def _L_ocean(self, screen, cols, rows, t):
    hz = int(rows * 0.7)
    for y in range(hz, rows):
        k = (y - hz) / max(1, rows - hz)
        for x in range(cols):
            w = math.sin(x * 0.35 + t * 1.6 + y * 0.9) + math.sin(x * 0.13 - t * 0.7)
            base = mix(self.th.far, (0, 0, 0), 0.3 + 0.3 * k)
            _cell(screen, x, y, mix(base, self.th.accent, 0.35) if w > 1.25 else base, "~" if w > 1.25 else " ", mix(base, self.th.accent, 0.8))


def _L_planet(self, screen, cols, rows, t):
    r = max(5, rows // 5)
    cx, cy = cols * 0.74, rows * 0.28
    _L_disc(self, screen, cols, rows, cx, cy, r, mix(self.th.far, self.th.accent, 0.5), False)
    for x in range(int(cx - r * 3.3), int(cx + r * 3.3) + 1):
        y = int(cy + (x - cx) * 0.12)
        if abs((x - cx) / 2.0) > r * 0.4 and 0 <= y < rows and 0 <= x < cols:
            _cell(screen, x, y, mix(self.th.accent, (255, 255, 255), 0.4), "─")
            _cell(screen, x, y + 1, mix(self.th.accent, (255, 255, 255), 0.2), "─")


def _L_pyramids(self, screen, cols, rows, t):
    hz = int(rows * 0.76)
    for cx, h in ((cols * 0.25, rows * 0.28), (cols * 0.55, rows * 0.4), (cols * 0.82, rows * 0.22)):
        for k in range(int(h)):
            hw = int((h - k) * 1.9)
            for x in range(int(cx - hw), int(cx + hw) + 1):
                _cell(screen, x, hz - k, mix(self.th.far, (255, 255, 255), 0.18) if x >= cx else mix(self.th.far, (0, 0, 0), 0.3))


def _L_trees(self, screen, cols, rows, t):
    hz, r = int(rows * 0.77), random.Random(self.th.key + "t")
    x = 1
    while x < cols - 2:
        h = r.randint(rows // 8, rows // 4)
        for k in range(h):
            hw = int((h - k) * 0.55) + (1 if k % 3 == 0 else 0)
            for xx in range(x - hw, x + hw + 1):
                _cell(screen, xx, hz - k, mix(self.th.far, (0, 0, 0), 0.5))
        x += r.randint(3, 7)


def _L_rain(self, screen, cols, rows, t):
    for i in range(cols // 2):
        x0, sp = (i * 37) % cols, 14 + (i * 5) % 9
        y = int((t * sp + i * 11) % rows)
        x = int(x0 - y * 0.3) % cols
        if 0 <= y < rows:
            bg = screen.back[y][x][2]
            screen.back[y][x] = ("╱", mix(bg, (200, 220, 255), 0.55), bg)


def _L_lightning(self, screen, cols, rows, t):
    period = 3.2
    k = int(t / period)
    local = t - k * period
    if local > 0.3:
        return
    r = random.Random(k)
    x, y = r.randint(cols // 6, cols - cols // 6), 0
    while y < int(rows * 0.76):
        _cell(screen, x, y, (240, 245, 255), "█" if local < 0.15 else "▓", (240, 245, 255))
        x += r.choice((-1, 0, 1)) * 2
        y += 1


def _L_tower(self, screen, cols, rows, t):
    hz, cx = int(rows * 0.76), int(cols * 0.3)
    for k in range(int(rows * 0.7)):
        hw = max(0, int(3 - k * 3.0 / (rows * 0.7)) + (1 if k % 6 == 0 else 0))
        for x in range(cx - hw, cx + hw + 1):
            _cell(screen, x, hz - k, mix(self.th.far, (0, 0, 0), 0.35))
        if k % 5 == 2:
            _cell(screen, cx, hz - k, mix(self.th.accent, (255, 255, 255), 0.4))
    if int(t * 2) % 2 == 0:
        _cell(screen, cx, hz - int(rows * 0.7), (255, 80, 80))


def _L_comet(self, screen, cols, rows, t):
    for i in range(3):
        k = ((t * 0.18 + i * 0.37) % 1.0)
        x, y = int(cols * (1.1 - k * 1.3)), int(rows * (0.05 + k * 0.5 + i * 0.07))
        for s in range(10):
            _cell(screen, x + s * 2, y - s // 2, mix(self.rowcol[min(rows - 1, max(0, y))], (255, 255, 255), 0.9 * (1 - s / 10)), "─", mix(self.rowcol[min(rows - 1, max(0, y))], (255, 255, 255), 0.9 * (1 - s / 10)))


def _L_glitch(self, screen, cols, rows, t):
    r = random.Random(int(t * 6))
    for _ in range(3):
        y, w, x0 = r.randrange(rows), r.randint(cols // 8, cols // 3), r.randrange(cols)
        for x in range(x0, min(cols, x0 + w)):
            ch, fg, bg = screen.back[y][x]
            screen.back[y][x] = (ch, bg, mix(fg, self.th.accent, 0.5))


def _L_volcano(self, screen, cols, rows, t):
    hz, pulse = int(rows * 0.78), 0.5 + 0.5 * math.sin(t * 1.6)
    for cx, h, wd in ((cols * 0.28, rows * 0.46, 2.4), (cols * 0.74, rows * 0.36, 2.0)):
        for k in range(int(h)):
            hw = int((h - k) * wd) + 1
            for x in range(int(cx - hw), int(cx + hw) + 1):
                base = mix(self.th.far, (0, 0, 0), 0.35)
                if k > h - 4 and abs(x - cx) < 4:
                    base = mix((255, 90, 20), (255, 210, 90), pulse)
                _cell(screen, x, hz - k, base)
        for k in range(int(h * 0.7)):
            f = 0.5 + 0.5 * math.sin(t * 3 + k * 0.6)
            _cell(screen, int(cx + math.sin(k * 0.5) * 1.3), hz - int(h) + 3 + k, mix((200, 50, 14), (255, 190, 80), f))


def _L_isles(self, screen, cols, rows, t):
    for fx_, fy, rw in ((0.15, 0.30, 9), (0.5, 0.22, 13), (0.82, 0.34, 10)):
        cx = int(cols * fx_)
        cy = int(rows * fy + math.sin(t * 0.7 + fx_ * 9) * 1.0)
        for dx in range(-rw, rw + 1):
            _cell(screen, cx + dx, cy, mix(self.th.accent, (255, 255, 255), 0.2))
        for k in range(1, rw // 2 + 1):
            half = rw - k * 2
            for dx in range(-half, half + 1):
                _cell(screen, cx + dx, cy + k, mix(self.th.far, self.th.accent, 0.25))
        for k in range(1, 7):
            f = 0.5 + 0.5 * math.sin(t * 8 + k)
            _cell(screen, cx, cy + rw // 2 + k, mix((120, 200, 240), (230, 250, 255), f), "│" if k % 2 else "╎", (200, 240, 255))


def _L_riftring(self, screen, cols, rows, t):
    cx, cy, R = cols * 0.5, rows * 0.42, rows * 0.34
    for rad, spd in ((R, 0.35), (R * 0.72, -0.55)):
        n = int(rad * 14)
        for i in range(n):
            ang = i / n * math.tau + t * spd
            col = (255, 240, 190) if math.sin(ang) > 0 else (170, 110, 255)
            _cell(screen, int(cx + math.cos(ang) * rad * 2.0), int(cy + math.sin(ang) * rad), col)
    for k in range(12):
        ang = k * math.tau / 12 + t * 0.15
        for j in range(int(R * 0.4), int(R * 1.5), 2):
            x, y = int(cx + math.cos(ang) * j * 2.0), int(cy + math.sin(ang) * j)
            if 0 <= x < cols and 0 <= y < rows:
                screen.put(x, y, "·", (255, 236, 190) if k % 2 else (150, 100, 240))
    r0 = R * 0.28
    for yy in range(int(cy - r0), int(cy + r0) + 1):
        for xx in range(int(cx - r0 * 2), int(cx + r0 * 2) + 1):
            d = math.hypot((xx - cx) / 2.0, yy - cy)
            if d <= r0:
                _cell(screen, xx, yy, (6, 2, 16))
            elif d <= r0 + 0.9:
                _cell(screen, xx, yy, mix((255, 200, 100), (255, 250, 220), 0.5 + 0.5 * math.sin(t * 5)))


def _L_hourglass(self, screen, cols, rows, t):
    cx, top, H = int(cols * 0.5), int(rows * 0.1), int(rows * 0.6)
    Wd = max(6, int(rows * 0.3))
    level = (t * 0.06) % 1.0
    for k in range(H):
        f = abs(H / 2 - k) / (H / 2)                      # 1 at the ends, 0 at the waist
        hw = int(3 + f * Wd)
        y = top + k
        sand = (k < H / 2 and k > (H / 2) * level) or (k >= H / 2 and (k - H / 2) / (H / 2) > 1 - level)
        for dx in range(-hw, hw + 1):
            edge = abs(dx) >= hw - 1 or k in (0, H - 1)
            if edge:
                _cell(screen, cx + dx * 2 // 1, y, (255, 226, 150))
            elif sand:
                _cell(screen, cx + dx * 2, y, mix((200, 140, 50), (255, 230, 150), 0.5 + 0.5 * math.sin(t * 2 + dx + k)))
        if abs(H / 2 - k) < H / 2 * 0.5 and 0 < level < 1:
            screen.put(cx, y, "│" if int(t * 12 + k) % 2 else "╎", (255, 236, 180))
    for i in range(80):
        ang = i / 80 * math.tau + t * 0.4
        _cell(screen, int(cx + math.cos(ang) * Wd * 3.2), int(top + H / 2 + math.sin(ang) * H * 0.6), mix((255, 226, 150), (255, 255, 255), 0.5 + 0.5 * math.sin(ang * 5 + t * 3)))


def _L_sunburst(self, screen, cols, rows, t):
    cx, cy, R = cols * 0.5, rows * 0.45, rows * 0.28
    for k in range(18):
        ang = k * math.tau / 18 + t * 0.12
        for j in range(int(R * 1.1), int(R * 3.2), 2):
            x, y = int(cx + math.cos(ang) * j * 2.0), int(cy + math.sin(ang) * j)
            if 0 <= x < cols and 0 <= y < rows:
                screen.put(x, y, "·" if k % 2 else "∙", (255, 226, 130) if k % 3 else (255, 255, 235))
    for yy in range(int(cy - R), int(cy + R) + 1):
        for xx in range(int(cx - R * 2), int(cx + R * 2) + 1):
            d = math.hypot((xx - cx) / 2.0, yy - cy)
            if d <= R:
                _cell(screen, xx, yy, mix((255, 250, 220), (255, 190, 70), d / R * 0.8 + 0.1 * math.sin(t * 2 + d)))
            elif d <= R + 1.0:
                _cell(screen, xx, yy, mix((255, 210, 90), (255, 255, 255), 0.5 + 0.5 * math.sin(t * 4)))
    n = int(R * 12)
    for i in range(n):
        ang = i / n * math.tau - t * 0.5
        _cell(screen, int(cx + math.cos(ang) * R * 1.45 * 2.0), int(cy + math.sin(ang) * R * 1.45), (255, 226, 130))


def _L_arcane(self, screen, cols, rows, t):
    cx, cy, R = cols * 0.5, rows * 0.42, rows * 0.36
    def P(a, rad):
        return int(cx + math.cos(a) * rad * 2.0), int(cy + math.sin(a) * rad)
    for rad, spd in ((R * 1.08, 0.15), (R * 0.92, -0.2)):
        n = int(rad * 16)
        for i in range(n):
            ang = i / n * math.tau + t * spd
            x, y = P(ang, rad)
            if 0 <= x < cols and 0 <= y < rows and i % 3 != 0:
                screen.put(x, y, "◦" if i % 2 else "·", (255, 226, 150))
    for tri in (0, 1):                                              # a slowly turning hexagram
        pts = [P(t * 0.25 + tri * math.pi / 3 + k * math.tau / 3, R * 0.9) for k in range(3)]
        for k in range(3):
            (x0, y0), (x1, y1) = pts[k], pts[(k + 1) % 3]
            for j in range(40):
                f = j / 40
                x, y = int(x0 + (x1 - x0) * f), int(y0 + (y1 - y0) * f)
                if 0 <= x < cols and 0 <= y < rows:
                    screen.put(x, y, "·", (255, 240, 190) if tri else (190, 140, 255))
    for i, el in enumerate(_ELEM):                                  # the six elements circling
        ang = t * 0.7 + i * math.tau / 6
        x, y = P(ang, R * 1.08)
        col = _ELEM[el][4]
        for dx, dy in ((0, 0), (1, 0), (-1, 0)):
            _cell(screen, x + dx, y + dy, col)


def _L_sanctum(self, screen, cols, rows, t):
    """PROTECTOR OF THE UNIVERSE: the Martyr (the cutscene's archmage) stands on the ridge, and above him hangs the Arcane Time
    Machine itself (the cutscene's own artifact: a glowing hourglass wrapped in turning rune rings). A thin thread of light
    ties the two together; three elemental orbs circle the machine and a quiet rune circle glows at his feet."""
    rc, G, V = self.rowcol, (255, 214, 100), (176, 120, 255)
    mx = int(cols * 0.88)
    rid = self.ridge[min(cols - 1, mx)]
    sc = clamp(rows * 2 * 0.40 / 32.0, 0.9, 2.2)                      # the mage is ~40% of the screen height
    tm_s = clamp(rows * 2 * 0.02, 0.9, 1.6)                          # the Time Machine hangs above him
    tcy = rows * 2 - int(rows * 2 * 0.20)                             # its centre, in canvas pixels (y up)
    head = rid * 2 + 2 + int(32 * sc)                                 # top of the mage, canvas pixels
    for yy in range(head + 3, tcy - int(7 * tm_s), 3):               # the thread of light between them
        r_ = rows - 1 - yy // 2
        if 0 <= r_ < rows - rid:
            screen.put(mx, r_, "·", shade(mix(G, V, 0.5 + 0.5 * math.sin(yy * 0.4 - t * 3)), 0.8), rc[r_])
    for k, col in enumerate(((255, 120, 70), (110, 190, 255), (150, 255, 170))):               # fire, water, earth orbs round the machine
        ang = t * 0.8 + k * math.tau / 3
        x = mx + int(math.cos(ang) * 9 * tm_s)
        y = rows - 1 - int(tcy / 2) + int(math.sin(ang) * 3 * tm_s)
        if 0 <= x < cols - 1 and 0 <= y < rows - rid:
            screen.put(x, y, "●", col, rc[y])
    cv = PixelCanvas(cols, rows * 2, (0, 0, 0))
    draw_artifact(cv, mx, tcy, tm_s, t)
    draw_mage(cv, mx, rid * 2 + 2, sc, t, 1.0, True)
    for r in range(rows):
        for c in range(max(0, mx - 34), min(cols, mx + 34)):
            a_, b_ = cv.rows[2 * r][c], cv.rows[2 * r + 1][c]
            if a_ == (0, 0, 0) and b_ == (0, 0, 0):
                continue
            bgc = screen.back[r][c][2]
            screen.back[r][c] = ("▀", bgc if a_ == (0, 0, 0) else a_, bgc if b_ == (0, 0, 0) else b_)
    gy = rows - rid                                                   # a quiet rune circle at his feet
    for i in range(36):
        ang = i * math.tau / 36 - t * 0.35
        x, y = mx + int(math.cos(ang) * 11), gy + int(math.sin(ang) * 1.2)
        if 0 <= x < cols and 0 <= y < rows:
            screen.put(x, y, "✦" if i % 9 == 0 else "·", shade(mix(G, V, 0.5 + 0.5 * math.sin(ang * 3 + t)), 0.8), rc[min(rows - 1, y)])


def _L_twinsuns(self, screen, cols, rows, t):
    """GLASS DESERT: two suns of different size sinking into the same haze, with sun-pillars of light on the dune-glass."""
    for cx_f, cy_f, r, col in ((0.26, 0.34, max(3, rows // 6), (255, 236, 176)), (0.44, 0.45, max(2, rows // 10), (255, 150, 110))):
        _L_disc(self, screen, cols, rows, int(cols * cx_f), int(rows * cy_f), r, col)
    for k, cx_f in enumerate((0.26, 0.44)):                                  # the sun-pillars: a slow shimmering column down to the dunes
        x = int(cols * cx_f)
        for y in range(int(rows * 0.5), rows - 4):
            if (y + int(t * 6)) % 3 == 0 and 0 <= x < cols:
                screen.put(x + int(math.sin(y * 0.5 + t * 2) * (1 + k)), y, "│", mix(self.rowcol[min(rows - 1, y)], (255, 230, 170), 0.5))
    for i in range(14):                                                      # glints: glass catching the light
        gx = int((i * 53 + 11) % cols)
        gy = rows - 3 - int((i * 7) % 6)
        if (int(t * 3) + i) % 5 == 0 and 0 <= gy < rows:
            screen.put(gx, gy, "✦", rainbow(t * 0.7 + i * 0.3))


def _L_lanterns(self, screen, cols, rows, t):
    """LANTERN FEST: solid paper lanterns (red, gold, white) drifting up from the temple roofs, swaying, with soft warm halos."""
    rc = self.rowcol
    for i in range(30):
        ph = (t * (0.04 + (i % 5) * 0.01) + i * 0.173) % 1.0
        x = int((i * 37 + 9) % cols + math.sin(t * 0.8 + i) * 2)
        y = int(rows * 0.9 - ph * rows * 0.88)
        if not (1 <= x < cols - 3 and 2 <= y < rows - 4):
            continue
        glow = 0.7 + 0.3 * math.sin(t * 3 + i * 1.7)
        base = ((255, 90, 50), (255, 190, 80), (255, 236, 200))[i % 3]
        col = shade(base, 0.7 + 0.3 * glow)
        for dy in (-2, -1, 0, 1, 2):                                     # soft halo
            for dx in range(-3, 4):
                if abs(dx) + abs(dy) * 2 <= 4 and (dx, dy) != (0, 0):
                    cell = screen.back[y + dy][x + dx] if 0 <= y + dy < rows and 0 <= x + dx < cols else None
                    if cell is not None:
                        _cell(screen, x + dx, y + dy, mix(rc[y + dy], col, 0.14 * glow))
        for dx in (0, 1):                                                # the lantern body: a solid 2x2 block with a top cap and tassel
            _cell(screen, x + dx, y, col, "▀", mix(col, (255, 255, 255), 0.3))
            _cell(screen, x + dx, y + 1, shade(col, 0.8), "▄", shade(col, 0.55))
        screen.put(x, y - 1, "▂", (90, 40, 20))
        screen.put(x, y + 2, "╵", shade(col, 0.6))



def _L_worldtree(self, screen, cols, rows, t):
    """WORLDTREE: a colossal solid trunk at the left, a canopy of glowing leaf-masses across the whole sky, veins of light pulsing
    up the bark and petals drifting down."""
    rc = self.rowcol
    tx = int(cols * 0.13)
    for y in range(0, rows - 3):
        w_ = 5 + int((y / max(1, rows)) * 9) + int(math.sin(y * 0.5) * 1.2)
        for dx in range(-w_, w_ + 1):
            u = abs(dx) / max(1, w_)
            if 0 <= tx + dx < cols:
                _cell(screen, tx + dx, y, mix((26, 60, 50), (6, 20, 22), u ** 0.7 * 0.9 + (0.12 if (dx + y // 3) % 4 == 0 else 0)))
        pulse = (y * 0.35 - t * 3) % 9
        for k in range(-2, 3):
            if pulse < 1.5 and 0 <= tx + k * 2 < cols:
                _cell(screen, tx + k * 2 + int(math.sin(y * 0.5) * 1.5), y, mix((30, 120, 90), (150, 255, 210), 1 - pulse / 1.5))
    for y in range(0, max(4, int(rows * 0.3))):                          # the canopy: overlapping leaf-masses
        for x in range(0, cols):
            v = math.sin(x * 0.16 + y * 0.9 + t * 0.15) + math.sin(x * 0.06 - y * 0.55 + 1.3) * 0.9 + math.cos(x * 0.11 + t * 0.1) * 0.5
            if v > 0.45 - y * 0.05:
                k = clamp((v - 0.2) / 2.2, 0, 1)
                _cell(screen, x, y, mix(rc[y], mix((14, 90, 70), (80, 230, 160), k), 0.55 + 0.35 * k), "▒" if k > 0.5 else "░", mix((120, 255, 200), (255, 255, 255), k * 0.5))
    for i in range(22):                                                  # glowing petals
        ph = (t * 0.06 + i * 0.137) % 1.0
        x = int((i * 41 + 7) % cols + math.sin(t + i) * 3)
        y = int(ph * (rows - 4))
        if 0 <= x < cols and 0 <= y < rows - 3:
            screen.put(x, y, "✿" if i % 2 else "❀", mix((150, 255, 200), (255, 255, 255), 0.5 + 0.5 * math.sin(t * 4 + i)))



def _L_whale(self, screen, cols, rows, t):
    """SKY WHALE: a vast solid whale sailing slowly through the stars (belly lighter than its back, flukes sweeping, glowing spots,
    a calf drifting far behind it)."""
    rc = self.rowcol
    for scale, yf, spd, off, dim in ((1.0, 0.30, 0.010, 0.25, 0.0), (0.4, 0.62, 0.016, 0.55, 0.5)):
        L_ = max(20, int(cols * 0.98 * scale))
        x0 = int((cols + L_) * ((off + t * spd) % 1.0) - L_)
        cy = int(rows * yf + math.sin(t * 0.3) * 1.3)
        for i in range(L_):
            u = i / L_
            hgt = max(0.0, math.sin(min(1.0, u * 1.08) * math.pi)) ** 0.7 * (rows * 0.26 * scale) * (1.0 - 0.6 * max(0.0, u - 0.7) / 0.3)
            if u > 0.86:
                hgt += (u - 0.86) * 20 * scale * (0.5 + 0.5 * math.sin(t * 1.6))      # the flukes sweeping
            x = x0 + i
            for dy in range(-int(hgt), int(hgt * 0.75) + 1):
                if 0 <= x < cols and 0 <= cy + dy < rows - 3:
                    f = (dy + hgt) / max(1.0, hgt * 1.75)                                 # 0 at the back, 1 at the belly
                    col = mix((18, 34, 84), (150, 190, 235), f ** 1.5)
                    _cell(screen, x, cy + dy, mix(rc[cy + dy], col, 1.0 - dim * 0.6))
            if i % 6 == 0 and 0 <= x < cols and 0 <= cy - int(hgt * 0.35) < rows:
                screen.put(x, cy - int(hgt * 0.35), "•", shade((150, 235, 255), (0.45 + 0.55 * math.sin(t * 2 + i)) * (1 - dim * 0.5)))
            if int(hgt * 0.7) > 1 and i % 3 == 0 and 0 <= x < cols and 0 <= cy + int(hgt * 0.45) < rows:        # throat grooves
                screen.put(x, cy + int(hgt * 0.45), "─", (170, 205, 245))
        ex = x0 + int(L_ * 0.13)
        if 0 <= ex < cols - 1 and 0 <= cy < rows:
            _cell(screen, ex, cy - 1, (240, 250, 255), "●", (20, 40, 90))



def _L_shards(self, screen, cols, rows, t):
    """SHATTERLINE: big slabs of translucent glass drifting over a cracked moon; each facet flashes its own prism colour as it turns."""
    rc = self.rowcol
    mx_, my_ = int(cols * 0.5), int(rows * 0.34)
    for a in range(9):                                                   # cracks radiating from the moon across the whole sky
        ang = a * math.tau / 9 + 0.4
        for s in range(3, int(rows * 0.62)):
            x, y = mx_ + int(math.cos(ang) * s * 2), my_ + int(math.sin(ang) * s)
            if 0 <= x < cols and 0 <= y < rows - 3 and (s + a) % 3:
                screen.put(x, y, "╱" if (a % 2) else "╲", (214, 204, 255))
    for i in range(12):
        cx_ = int((i * 61 + 5) % cols + math.sin(t * 0.3 + i) * 3)
        cy_ = int(rows * (0.1 + (i * 29 % 50) / 100) + math.sin(t * 0.5 + i * 2) * 1.5)
        w_ = 5 + (i * 7) % 7
        tilt = math.sin(t * 0.6 + i)
        for dx in range(-w_, w_ + 1):
            for dy in range(-2, 3):
                if abs(dx) + abs(dy) * 3 <= w_ + 1 and 0 <= cx_ + dx < cols and 0 <= cy_ + dy < rows - 3:
                    k = (dx * tilt * 0.25 + dy * 0.4) + t * 0.35 + i * 0.2
                    edge = abs(dx) + abs(dy) * 3 >= w_
                    _cell(screen, cx_ + dx, cy_ + dy, mix(rc[cy_ + dy], rainbow(k), 0.30 if not edge else 0.7),
                          "◆" if (dx + dy + i) % 5 == 0 else " ", (255, 255, 255))


LAYER_DRAWERS = {"twinsuns": _L_twinsuns, "lanterns": _L_lanterns, "worldtree": _L_worldtree, "whale": _L_whale, "shards": _L_shards, "sanctum": _L_sanctum, "moon": _L_moon, "sun": _L_sun, "city": _L_city, "mountains": _L_mountains, "grid": _L_grid, "ocean": _L_ocean,
                 "planet": _L_planet, "pyramids": _L_pyramids, "trees": _L_trees, "rain": _L_rain, "lightning": _L_lightning,
                 "tower": _L_tower, "comet": _L_comet, "glitch": _L_glitch, "volcano": _L_volcano, "isles": _L_isles,
                 "riftring": _L_riftring, "hourglass": _L_hourglass, "sunburst": _L_sunburst, "arcane": _L_arcane}


class ThemeBackdrop:
    """Home-screen backdrop for the shop's themed screens: gradient sky, stars / aurora / eclipse, drifting particles,
    a far ridge and a hero tank lobbing shells of its own ammo along the bottom edge. Driven entirely by a HomeTheme."""

    def __init__(self, theme: HomeTheme) -> None:
        self.th = theme
        self.rng = random.Random(sum(map(ord, theme.key)))
        self.size = (0, 0)
        self.clock = 0.0
        self.rowcol: list = []
        self.stars: list = []
        self.motes: list = []
        self.ridge: list = []
        self.shells: list = []
        self.explosions: list = []
        self.timer = 1.2
        self.recoil = 0.0

    def celebrate_with(self, colors: Optional[list]) -> None:
        pass

    def ensure(self, cols: int, rows: int) -> None:
        if self.size == (cols, rows):
            return
        self.size, r, th = (cols, rows), self.rng, self.th
        self.rowcol = [gradient(th.sky, clamp(1 - y / max(1, rows - 1), 0, 1)) for y in range(rows)]
        self.stars = [(r.randrange(cols), r.randrange(0, max(1, int(rows * 0.62))), r.uniform(0, math.tau))
                      for _ in range(cols * rows // 55)] if th.stars else []
        style = _MOTE_STYLES.get(th.fx)
        n = int(cols * rows * style[0] * 3) + 8 if style else 0
        self.motes = [(r.uniform(0, cols), r.uniform(0, rows), r.uniform(0.6, 1.4), r.uniform(0, math.tau),
                       r.uniform(0.5, 2.5), r.randrange(3)) for _ in range(n)]
        self.ridge = [int(rows * 0.27 + 1.6 * math.sin(x * 0.06 + 1) + 1.1 * math.sin(x * 0.17)) for x in range(cols)]

    def update(self, dt: float) -> None:
        self.clock += dt
        self.recoil = max(0.0, self.recoil - dt)
        self.timer -= dt
        if self.timer <= 0:
            self.timer, self.recoil = self.rng.uniform(2.2, 3.4), 0.25
            v = self.rng.uniform(34, 44)
            design = DESIGN_BY_KEY[self.th.hero]
            ang = 38 + 9 * math.sin(self.clock * 0.7)                      # the angle the hero's barrel is showing right now
            ox, oy = muzzle_point(15.0, 3 + visual_lift(design, self.clock, 2), ang, design, 2)
            a = math.radians(ang)
            cap = min(self.size[1] * 2, 72) - 10
            apex = oy + (v * math.sin(a)) ** 2 / 80
            if apex > cap > oy:
                v *= math.sqrt((cap - oy) / (apex - oy))                 # keep the whole arc inside the canvas
            self.shells.append({"l": Launch(ox, oy, v * math.cos(a), v * math.sin(a), 40.0), "t": 0.0})
        for s in self.shells[:]:
            s["t"] += dt
            x, y = s["l"].at(s["t"])
            if y < 3.0 and s["t"] > 0.2:
                self.shells.remove(s)
                self.explosions.append(Explosion(x, 3.0, 5.0, self.th.accent, 0.9, AMMO_BY_KEY[self.th.hero_ammo].boom))
        for e in self.explosions:
            e.update(dt)
        self.explosions = [e for e in self.explosions if not e.done]

    def draw(self, screen: Screen, t: float) -> None:
        self.ensure(screen.cols, screen.rows)
        cols, rows = self.size
        th, rc = self.th, self.rowcol
        for y in range(rows):
            screen.fill(0, y, cols, 1, rc[y])
        for x, y, ph in self.stars:
            lvl = int((math.sin(t * 1.6 + ph) + 1) * 1.49)
            screen.put(x, y, "·" if lvl < 2 else "+", mix(rc[y], (255, 255, 255), (0.25, 0.5, 0.85)[lvl]), rc[y])
        if th.corona:
            self._corona(screen, t)
        if th.aurora:
            for b in range(2):
                for x in range(cols):
                    y = int(rows * (0.2 + 0.11 * b) + math.sin(x * 0.07 + t * 0.5 + b * 2.1) * rows * 0.05)
                    if 0 <= y < rows:
                        hue = round((0.45 + 0.2 * math.sin(x * 0.03 + t * 0.2 + b)) * 24) / 24
                        screen.put(x, y, "▒", mix(rc[y], hsv(hue, 0.6, 1.0), 0.45), rc[y])
        style = _MOTE_STYLES.get(th.fx)
        if style:
            _, speed, pal, alpha = style
            for x0, y0, sp, ph, amp, ci in self.motes:
                x = int((x0 + math.sin(t * 0.8 * sp + ph) * amp) % cols)
                y = int(rows - 1 - ((y0 + t * speed * sp * 0.45) % rows))
                if 0 <= y < rows:
                    lvl = (0.5, 0.75, 1.0)[int((math.sin(t * 5 * sp + ph) + 1) * 1.49)]
                    screen.put(x, y, "•" if sp > 1.0 else "∙", mix(rc[y], pal[ci], alpha * lvl), rc[y])
        for name in th.layers:
            LAYER_DRAWERS[name](self, screen, cols, rows, t)
        for x in range(cols):
            h = self.ridge[x]
            screen.fill(x, rows - h, 1, h, th.far)
        self._hero(screen, cols, rows)

    def _corona(self, screen: Screen, t: float) -> None:
        cols, rows = self.size
        cx, cy, R = cols - 10, int(rows * 0.3), max(4, rows // 6)
        for dy in range(-R - 3, R + 4):
            for dx in range(-2 * (R + 3), 2 * (R + 3) + 1):
                x, y = cx + dx, cy + dy
                if not (0 <= x < cols and 0 <= y < rows):
                    continue
                d = math.hypot(dx / 2, dy)
                if d <= R:
                    screen.put(x, y, " ", (0, 0, 0), (2, 1, 8))
                elif d <= R + 2.6:
                    ang = math.atan2(dy, dx / 2)
                    k = round((1 - (d - R) / 2.6) * 6) / 6
                    hue = round((ang / math.tau + t * 0.08) % 1 * 24) / 24
                    screen.put(x, y, "▓" if k > 0.5 else "░", mix(self.rowcol[y], hsv(hue, 0.55, 1.0), 0.35 + 0.6 * k),
                               mix(self.rowcol[y], (255, 244, 210), 0.4 * k))

    def _hero(self, screen: Screen, cols: int, rows: int) -> None:
        th, h = self.th, min(rows * 2, 72)
        cv = PixelCanvas(cols, h, (0, 0, 0))
        for x in range(cols):
            cv.plot(x, 0, th.ground)
            cv.plot(x, 1, th.ground)
            cv.plot(x, 2, th.ground_top)
        ammo = AMMO_BY_KEY[th.hero_ammo]
        draw_tank(cv, 15.0, 3, 1, 38 + 9 * math.sin(self.clock * 0.7), th.accent, scale=2, recoil=self.recoil,
                  design=DESIGN_BY_KEY[th.hero], t=self.clock)
        for s in self.shells:
            x, y = s["l"].at(s["t"])
            draw_projectile(cv, x, y, s["l"].vx, s["l"].vy - 40 * s["t"], th.accent, ammo, self.clock)
        for e in self.explosions:
            e.draw(cv)
        top = rows - h // 2
        for r in range(h // 2):
            if not 0 <= top + r < rows:
                continue
            for c in range(cols):
                a, b = cv.rows[2 * r][c], cv.rows[2 * r + 1][c]
                if a == (0, 0, 0) and b == (0, 0, 0):
                    continue
                bgc = screen.back[top + r][c][2]
                screen.back[top + r][c] = ("▀", bgc if a == (0, 0, 0) else a, bgc if b == (0, 0, 0) else b)


def draw_theme_logo(screen: Screen, y: int, t: float, tall: bool, th: HomeTheme) -> int:
    """Gradient logo in the theme's colours with a travelling shimmer."""
    y += 1
    words = [big_word("TERMINAL")] + ([big_word("TANKS")] if tall else [])
    shimmer = (t * 36) % 140 - 25
    for wi, rows_ in enumerate(words):
        x0 = (screen.cols - len(rows_[0])) // 2
        for r, line in enumerate(rows_):
            base = mix(th.logo_a, th.logo_b, (r + wi * 6) / (11 if tall else 5))
            for i, ch in enumerate(line):
                if ch == " ":
                    continue
                if ch == "█":
                    b = max(0.0, 1 - abs(i + r * 2 + wi * 14 - shimmer) / 9)
                    screen.put(x0 + i, y, ch, mix(base, (255, 255, 255), b * 0.75), Palette.BG)
                else:
                    screen.put(x0 + i, y, ch, shade(base, 0.4), Palette.BG)
            y += 1
    if not tall:
        screen.center(y, "▌▌ T A N K S ▐▐", th.logo_b)
        y += 1
    edition = "   ".join(" ".join(w) for w in th.name.split()) + "   ·   E D I T I O N"
    if th.key == "ETERNAL":
        edition = "P R O T E C T O R   O F   T H E   U N I V E R S E"
    screen.center(y + 1, edition, th.accent)
    return y + 2


def draw_theme_menu(screen: Screen, menu: "MenuList", cx: int, y: int, t: float, th: HomeTheme, gap: int = 2) -> None:
    items = menu.items
    w, h = 40, len(items) * gap + 1
    x0, bg = cx - w // 2, shade(mix(th.ground, th.accent, 0.08), 1.0)
    screen.box(x0, y - 1, w, h + 2, "double", shade(th.accent, 0.8), bg, th.name, th.logo_a)
    sel_bg = mix(bg, th.accent, 0.28)
    for i, label in enumerate(items):
        row = y + i * gap
        if i == menu.index:
            screen.fill(x0 + 2, row, w - 4, 1, sel_bg)
            screen.center(row, f"◆ {label} ◆", Palette.WHITE, sel_bg, x0, w)
        else:
            screen.center(row, label, shade(th.accent, 0.62), bg, x0, w)


def draw_secret_logo(screen: Screen, y: int, t: float, tall: bool) -> int:
    """Terminal-green logo with glitching rows and a chroma-split edge."""
    head = "┌─[ root@tanks ]─[ ~/secret ]─┐"
    screen.center(y, head, (80, 230, 130))
    y += 1
    glitch = int(t * 6) % 9 == 0
    words = [big_word("TERMINAL")] + ([big_word("TANKS")] if tall else [])
    shimmer = (t * 40) % 130 - 25
    for wi, rows in enumerate(words):
        x0 = (screen.cols - len(rows[0])) // 2
        for r, line in enumerate(rows):
            jitter = (1 if (r + wi + int(t * 6)) % 2 else -1) if glitch and (r * 7 + int(t * 6)) % 4 == 0 else 0
            top = mix((190, 255, 205), (30, 170, 80), (r + wi * 6) / 11)
            for i, ch in enumerate(line):
                if ch == " ":
                    continue
                if ch == "█":
                    b = max(0.0, 1 - abs(i + r * 2 + wi * 14 - shimmer) / 9)
                    screen.put(x0 + i + jitter, y, ch, mix(top, (235, 255, 240), b * 0.8), Palette.BG)
                else:
                    ghost = (255, 60, 120) if glitch and i % 2 else (20, 90, 54)
                    screen.put(x0 + i + jitter, y, ch, ghost, Palette.BG)
            y += 1
    if not tall:
        screen.center(y, "▌▌ T A N K S ▐▐", (110, 255, 150))
        y += 1
    edition = "S E C R E T   E D I T I O N"
    shown = edition[:max(1, int((t % 6) * 12))] if int(t) % 6 < 3 else edition
    screen.text((screen.cols - len(edition)) // 2, y + 1, shown, (130, 255, 170))
    if int(t * 2) % 2 == 0:
        screen.put((screen.cols - len(edition)) // 2 + len(shown), y + 1, "█", (130, 255, 170))
    return y + 2


def draw_secret_menu(screen: Screen, menu: "MenuList", cx: int, y: int, t: float, gap: int = 2) -> None:
    items = menu.items
    w, h = 40, len(items) * gap + 1
    x0, bg = cx - w // 2, (2, 18, 10)
    screen.box(x0, y - 1, w, h + 2, "single", (50, 190, 95), bg, "ROOT MENU", (150, 255, 180))
    for i, label in enumerate(items):
        row = y + i * gap
        if i == menu.index:
            screen.fill(x0 + 2, row, w - 4, 1, (8, 62, 30))
            screen.center(row, f"> {label} <", (200, 255, 215), (8, 62, 30), x0, w)
            if int(t * 3) % 2 == 0:
                screen.put(x0 + w - 5, row, "_", (150, 255, 180), (8, 62, 30))
        else:
            screen.center(row, label, (60, 150, 90), bg, x0, w)


# ============================================================================
# Scenes
# ============================================================================
def wallet_text(save: "SaveData") -> str:
    return f"◉ {save.coins:,}   ◈ {save.credits:,}"


def draw_wallet(screen: Screen, save: "SaveData", flash: float = 0.0) -> None:
    """Coins (◉) and Tournament Credits (◈) in the top-right corner of every screen."""
    text = wallet_text(save)
    x = screen.cols - len(text) - 3
    bg = mix((16, 24, 34), (60, 52, 20), clamp(flash, 0, 1))
    screen.fill(x - 1, 0, len(text) + 3, 1, bg)
    coin, _, cred = text.partition("   ")
    screen.text(x, 0, coin, GOLD, bg)
    screen.text(x + len(coin) + 3, 0, cred, (140, 225, 255), bg)


class Scene:
    overlay = False
    ambient = False
    show_wallet = True

    def __init__(self, app: "Application") -> None:
        self.app = app

    def enter(self) -> None: ...
    def exit(self) -> None: ...
    def handle(self, ev: InputEvent) -> None: ...
    def update(self, dt: float) -> None: ...
    def update_ambient(self, dt: float) -> None: ...
    def draw(self, screen: Screen) -> None: ...


class BackdropScene(Scene):
    home = False

    def backdrop(self):
        return self.app.home_backdrop() if self.home else self.app.backdrop

    def update(self, dt: float) -> None:
        self.backdrop().update(dt)

    def draw(self, screen: Screen) -> None:
        self.backdrop().draw(screen, self.app.time)


class BootScene(BackdropScene):
    show_wallet = False
    LINES = ("TERMINALTANKS // TACTICAL ARTILLERY SYSTEM", f"author: {CREDIT_NAME} ({CREDIT_URL})",
             "display link .............. online", "ballistics core ........... online",
             "terrain engine ............ online", "particle system ........... online",
             "fire-control AI ........... online", "tournament registry ....... online",
             "all systems nominal")

    def __init__(self, app) -> None:
        super().__init__(app)
        self.t = 0.0

    def handle(self, ev: InputEvent) -> None:
        self.t = max(self.t, 9.0)

    def update(self, dt: float) -> None:
        self.t += dt
        if self.t > 3.8 or (self.t >= 9.0):
            self.app.goto(TitleScene(self.app))

    def draw(self, screen: Screen) -> None:
        screen.clear(Palette.BG)
        x, y = max(2, screen.cols // 2 - 24), screen.rows // 2 - 9
        shown = int(self.t / 0.38)
        for i, line in enumerate(self.LINES[:shown]):
            head = i == 0
            screen.text(x, y + i * 2, line, Palette.PRIMARY if head else Palette.TEXT)
            if not head and i != 1:
                screen.text(x + 41, y + i * 2, "[ OK ]", Palette.OK)
        w = 40
        frac = clamp(self.t / 3.4, 0, 1)
        screen.text(x, y + 20, "LOADING", Palette.MUTED)
        for i in range(w):
            screen.put(x + 8 + i, y + 20, "█" if i < frac * w else "░", Palette.PRIMARY if i < frac * w else (36, 52, 62), Palette.BG)


class HomeScene(BackdropScene):
    """Shared behaviour of the title and main-menu scenes: switching between the unlocked home screens."""
    home = True
    THEME_COLORS = {"CLASSIC": Palette.PRIMARY_DIM, "MASTER": GOLD, "SECRET": (110, 255, 150),
                    **{k: v.accent for k, v in HOME_THEMES.items()}}

    def __init__(self, app) -> None:
        super().__init__(app)
        self.note = ""
        self.note_t = 0.0

    def toggle_home(self) -> None:
        modes = self.app.save.home_modes()
        if len(modes) < 2:
            self.note, self.note_t = "NO OTHER HOME SCREENS YET - BUY ONE IN THE SHOP", 3.0
            return
        s = self.app.settings
        s.home_mode = modes[(modes.index(self.theme) + 1) % len(modes)]
        self.app.apply_settings()
        self.app.goto(type(self)(self.app))

    @property
    def theme(self) -> str:
        return self.app.home_theme()

    @property
    def master(self) -> bool:
        return self.theme == "MASTER"

    def update(self, dt: float) -> None:
        super().update(dt)
        self.note_t = max(0.0, self.note_t - dt)

    def draw_footer(self, screen: Screen, row: int) -> None:
        if self.note_t > 0:
            screen.center(row, self.note, Palette.AMBER)
            return
        if len(self.app.save.home_modes()) > 1:
            screen.center(row, f"[H] HOME SCREEN: {self.theme}  (press H to switch)", self.THEME_COLORS[self.theme])
        else:
            screen.center(row, "[H] MORE HOME SCREENS IN THE SHOP  (Master screen: clear Level 20)", Palette.MUTED)


class TitleScene(HomeScene):
    def handle(self, ev: InputEvent) -> None:
        if ev.raw in ("h", "H", "TAB"):
            self.toggle_home()
        elif ev.confirm:
            self.app.goto(MenuScene(self.app))

    def draw(self, screen: Screen) -> None:
        super().draw(screen)
        t = self.app.time
        if self.theme == "MASTER":
            y = max(0, (screen.rows - (21 if screen.rows >= 34 else 15)) // 2)
            y = draw_master_logo(screen, y, t, screen.rows >= 34)
            if int(t * 2) % 2 == 0:
                screen.center(y + 2, "[ PRESS ENTER ]", (255, 248, 220))
        elif self.theme == "SECRET":
            y = max(0, (screen.rows - (21 if screen.rows >= 34 else 15)) // 2)
            y = draw_secret_logo(screen, y, t, screen.rows >= 34)
            if int(t * 2) % 2 == 0:
                screen.center(y + 2, "[ PRESS ENTER ]", (200, 255, 215))
        elif self.theme in HOME_THEMES:
            th = HOME_THEMES[self.theme]
            y = max(1, (screen.rows - (21 if screen.rows >= 34 else 15)) // 2)
            y = draw_theme_logo(screen, y, t, screen.rows >= 34, th)
            if int(t * 2) % 2 == 0:
                screen.center(y + 2, "[ PRESS ENTER ]", mix(th.accent, (255, 255, 255), 0.6))
        else:
            y = max(1, (screen.rows - 18) // 2)
            y = draw_logo(screen, y, t, compact=False)
            screen.center(y + 1, "T A C T I C A L   A R T I L L E R Y   S I M U L A T I O N", Palette.AMBER)
            if int(t * 2) % 2 == 0:
                screen.center(y + 4, "[ PRESS ENTER ]", Palette.WHITE)
        screen.center(screen.rows - 4, f"v{__version__}  -  single file, zero dependencies", Palette.MUTED)
        screen.center(screen.rows - 3, f"created by {CREDIT_NAME}  ·  {CREDIT_URL}", Palette.PRIMARY_DIM)
        self.draw_footer(screen, screen.rows - 2)


class MenuScene(HomeScene):
    def __init__(self, app) -> None:
        super().__init__(app)
        self.menu = MenuList(["SINGLE PLAYER", "TWO PLAYER", "LAN PLAY", "TOURNAMENT", "MISSIONS", "SHOP", "WORKSHOP", "HOW TO PLAY", "TUTORIAL", "PROFILES", "SETTINGS", "CHEAT CODES", "LINKS", "QUIT"])

    def handle(self, ev: InputEvent) -> None:
        if ev.raw in ("h", "H", "TAB"):
            self.toggle_home()
            return
        if ev.action is Action.BACK:
            self.app.goto(TitleScene(self.app))
            return
        choice = self.menu.handle(ev)
        if choice is None:
            return
        app, label = self.app, self.menu.items[choice]
        if label == "SINGLE PLAYER":
            def single_next(r):
                sc = SetupScene(app, "single", rules=r)
                sc.back = RulesScene(app, "SINGLE PLAYER RULES", single_next, kind="single")
                app.goto(sc)
            app.goto(RulesScene(app, "SINGLE PLAYER RULES", single_next, kind="single"))
        elif label == "TWO PLAYER":
            app.goto(RulesScene(app, "TWO PLAYER RULES", lambda r: app.goto(SetupScene(app, "two", rules=r)), kind="two"))
        elif label == "LAN PLAY":
            if IS_WEB:
                app.goto(NetNoticeScene(app, "LAN DISABLED ON WEB", WEB_LAN_TEXT))
            else:
                app.goto(LanMenuScene(app))
        elif label == "TOURNAMENT":
            app.goto(TournamentScene(app))
        elif label == "MISSIONS":
            if app.save.missions_open():
                app.goto(MissionsScene(app))
            else:
                app.goto(NetNoticeScene(app, "MISSIONS LOCKED", "Missions open once you defeat the Master in Tournament mode (Level 20). "
                                                                "Your Tournament progress is on the TOURNAMENT screen."))
        elif label == "SHOP":
            app.goto(ShopScene(app))
        elif label == "WORKSHOP":
            if app.save.workshop_open():
                app.goto(WorkshopScene(app))
            else:
                app.goto(NetNoticeScene(app, "WORKSHOP LOCKED", "The Tank Workshop opens once you defeat Level 5 in Tournament mode."))
        elif label == "TUTORIAL":
            app.goto(TutorialScene(app))
        elif label == "PROFILES":
            app.goto(ProfilesScene(app))
        elif label == "HOW TO PLAY":
            app.goto(HowToScene(app))
        elif label == "SETTINGS":
            app.goto(SettingsScene(app))
        elif label == "CHEAT CODES":
            app.goto(CheatScene(app))
        elif label == "LINKS":
            app.goto(LinksScene(app))
        elif label == "QUIT":
            app.running = False

    def _view(self, screen: Screen, y0: int, gap: int):
        """The menu may have more entries than a short terminal has rows: show a window that follows the selection."""
        items, idx = self.menu.items, self.menu.index
        fit = max(5, (screen.rows - 4 - y0) // gap)
        if len(items) <= fit:
            return self.menu
        start = max(0, min(idx - fit // 2, len(items) - fit))
        view = copy.copy(self.menu)
        view.items = list(items[start:start + fit])
        view.index = idx - start
        return view

    def draw(self, screen: Screen) -> None:
        super().draw(screen)
        t = self.app.time
        if self.theme == "MASTER":
            tall = screen.rows >= 42
            y = draw_master_logo(screen, 0, t, tall)
            gap = 2 if y + 4 + len(self.menu.items) * 2 <= screen.rows - 3 else 1
            draw_master_menu(screen, self._view(screen, y + 2, gap), screen.cols // 2, y + 2, t, gap)
        elif self.theme == "SECRET":
            tall = screen.rows >= 42
            y = draw_secret_logo(screen, 0, t, tall)
            gap = 2 if y + 4 + len(self.menu.items) * 2 <= screen.rows - 3 else 1
            draw_secret_menu(screen, self._view(screen, y + 2, gap), screen.cols // 2, y + 2, t, gap)
        elif self.theme in HOME_THEMES:
            th = HOME_THEMES[self.theme]
            tall = screen.rows >= 42
            y = draw_theme_logo(screen, 1, t, tall, th)
            gap = 2 if y + 4 + len(self.menu.items) * 2 <= screen.rows - 3 else 1
            draw_theme_menu(screen, self._view(screen, y + 2, gap), screen.cols // 2, y + 2, t, th, gap)
        else:
            y = draw_logo(screen, 2, t, compact=screen.rows < 38)
            screen.center(y, "SELECT MISSION", Palette.PRIMARY_DIM)
            gap = 2 if screen.rows >= 32 and y + 4 + len(self.menu.items) * 2 <= screen.rows - 3 else 1
            self.menu.draw(screen, screen.cols // 2, y + 2, t, gap=gap) if False else self._view(screen, y + 2, gap).draw(screen, screen.cols // 2, y + 2, t, gap=gap)
        draw_keycaps(screen, screen.cols // 2 - 22, screen.rows - 3, (("W/S", "NAVIGATE"), ("ENTER", "SELECT"), ("ESC", "BACK")))
        self.draw_footer(screen, screen.rows - 2)
        pt = int(self.app.save.playtime)
        pn = next((p["name"] for p in self.app.profiles.profiles if p["id"] == self.app.profiles.active), "")
        screen.text(2, screen.rows - 1, f"{pn}  ·  PLAYTIME {fmt_playtime(pt)}", Palette.MUTED)


class HowToScene(BackdropScene):
    page = 0
    AMMO_LINES = [
        ("WHAT IT IS", Palette.AMBER),
        ("Ammo marked ✦ in pilot setup has a real effect: it can bounce, split in flight, drop cluster bomblets, chain blasts, change gravity, speed, wind pull or blast size, and more. Plain ammo (STANDARD, TRACER, PLASMA...) only changes how shots look.", Palette.TEXT),
        ("WHEN IT WORKS", Palette.AMBER),
        ("Single Player: on by default - switch it ON/OFF in the Single Player rules. Two Player / LAN: when SPECIAL AMMO is ON in the match rules (the host decides in LAN). Terminal Tickets: always.", Palette.TEXT),
        ("WHEN IT DOES NOT", Palette.AMBER),
        ("Tournament and Missions: always off, so boss fights stay fair. Two Player / LAN with SPECIAL AMMO OFF. The 3-damage SPECIAL ATTACK (X) also skips the ammo's effect. SPECIAL ATTACK itself is a rule toggle in Single Player, Two Player and LAN.", Palette.TEXT),
        ("HOW YOU CAN TELL", Palette.AMBER),
        ("When it is off, the match banner says SPECIAL AMMO OFF and a note pops up over your tank on your first shot. The AI follows the same rules.", Palette.TEXT),
    ]

    def handle(self, ev: InputEvent) -> None:
        if ev.action is Action.BACK or (self.page == 1 and ev.action in (Action.CONFIRM, Action.FIRE)):
            self.app.goto(MenuScene(self.app))
        elif self.page == 0 and (ev.action in (Action.CONFIRM, Action.FIRE, Action.RIGHT) or ev.confirm):
            self.page = 1
        elif ev.action is Action.LEFT:
            self.page = 0

    def draw(self, screen: Screen) -> None:
        super().draw(screen)
        if self.page == 1:
            x, y = panel(screen, 76, 24, "HOW TO PLAY  ·  SPECIAL AMMO")
            row = y + 2
            for t_, c in self.AMMO_LINES:
                for ln in textwrap.wrap(t_, 66):
                    screen.text(x + 4, row, ln, c, Palette.PANEL)
                    row += 1
            screen.center(y + 22, "[ ◄ ] PAGE 1     [ ENTER ] BACK", Palette.PRIMARY_DIM, Palette.PANEL, x, 76)
            return
        x, y = panel(screen, 76, 24, "HOW TO PLAY")
        lines = [
            ('OBJECTIVE', Palette.AMBER),
            ('Destroy the enemy tank; every hit costs a heart. Terrain breaks.', Palette.TEXT),
            ('CONTROLS', Palette.AMBER),
            ('A / D  or  ◄ ►   rotate barrel (hold SHIFT for big steps)', Palette.TEXT),
            ('W / S  or  ▲ ▼   firing power        SPACE / ENTER   fire', Palette.TEXT),
            ('ESC / P   pause        SHIFT+A/D in pilot setup jumps 5 kits', Palette.TEXT),
            ('TIPS', Palette.AMBER),
            ('WIND pushes every shell: read the arrows. The guide ignores wind.', Palette.TEXT),
            ('A blast right beside a tank still counts as a hit.', Palette.TEXT),
            ('TWO PLAYER / LAN', Palette.AMBER),
            ('Pick BEST OF or FIRST TO n rounds and your hearts. LAN: one hosts,', Palette.TEXT),
            ("one joins on the same network; the host's rules apply.", Palette.TEXT),
            ('TOURNAMENT', Palette.AMBER),
            ('Twenty levels, each opened by the one before. Beat a boss to unlock', Palette.TEXT),
            ('its exact tank + ammo. Usually 2 strikes; some levels bend the rules.', Palette.TEXT),
            ('MISSIONS', Palette.AMBER),
            ('50 missions (open after you beat the Master): marathons, 2v1, UFO, co-op, rushes.', Palette.TEXT),
            ('Coins only. Co-op: local, LAN or an AI wingman. Grand prize: a UFO.', Palette.TEXT),
            ('SHOP', Palette.AMBER),
            ('Spend coins (◉) and tournament credits (◈) on kits, maps and screens.', Palette.TEXT),
        ]
        for i, (s, c) in enumerate(lines):
            screen.text(x + 4, y + 2 + i, s, c, Palette.PANEL)
        screen.center(y + 22, "[ ENTER / ► ] NEXT: SPECIAL AMMO     [ ESC ] BACK", Palette.PRIMARY_DIM, Palette.PANEL, x, 76)


class SettingsScene(BackdropScene):
    def __init__(self, app) -> None:
        super().__init__(app)
        self.index = 0

    def _choices(self, row) -> tuple:
        save = self.app.save
        if row[1] == "difficulty" and not save.master_unlocked:
            return tuple(c for c in row[2] if c[1] is not Difficulty.MASTER)
        if row[1] == "home_mode":
            return tuple(c for c in row[2] if c[1] in save.home_modes())
        return row[2]

    def _choice_index(self, row) -> int:
        cur = getattr(self.app.settings, row[1])
        return next((i for i, (_, v) in enumerate(self._choices(row)) if v == cur), 0)

    def handle(self, ev: InputEvent) -> None:
        if ev.action is Action.BACK:
            self.app.goto(MenuScene(self.app))
        elif ev.action is Action.UP:
            self.index = (self.index - 1) % len(SETTING_ROWS)
        elif ev.action is Action.DOWN:
            self.index = (self.index + 1) % len(SETTING_ROWS)
        elif ev.action in (Action.LEFT, Action.RIGHT, Action.CONFIRM, Action.FIRE):
            row = SETTING_ROWS[self.index]
            step = -1 if ev.action is Action.LEFT else 1
            choices = self._choices(row)
            i = (self._choice_index(row) + step) % len(choices)
            setattr(self.app.settings, row[1], choices[i][1])
            self.app.apply_settings()

    def draw(self, screen: Screen) -> None:
        super().draw(screen)
        step = 2 if screen.rows >= len(SETTING_ROWS) * 2 + 10 else 1
        h = len(SETTING_ROWS) * step + 6
        x, y = panel(screen, 64, h, "SETTINGS")
        for i, row in enumerate(SETTING_ROWS):
            ry = y + 2 + i * step
            sel = i == self.index
            bg = Palette.PANEL_HI if sel else Palette.PANEL
            screen.fill(x + 2, ry, 60, 1, bg)
            screen.text(x + 4, ry, row[0], Palette.WHITE if sel else Palette.MUTED, bg)
            val = self._choices(row)[self._choice_index(row)][0]
            s = f"◄ {val} ►" if sel else val
            screen.text(x + 60 - len(s), ry, s, Palette.PRIMARY if sel else Palette.TEXT, bg)
        row = SETTING_ROWS[self.index]
        desc = row[3]
        if not self.app.save.master_unlocked and row[1] in ("difficulty", "home_mode"):
            desc = "MASTER unlocks after Tournament Level 10"
        screen.center(y + h - 3, desc, Palette.MUTED, Palette.PANEL, x, 64)
        screen.center(y + h - 2, "ESC  BACK", Palette.PRIMARY_DIM, Palette.PANEL, x, 64)


def draw_swatches(screen: Screen, x: int, y: int, idx: int, taken: Optional[int] = None, rgb: bool = False,
                  rgb_sel: bool = False, t: float = 0.0, astral_on: bool = False, astral_sel: bool = False,
                  cur: str = "", prism_on: bool = False, void_on: bool = False) -> None:
    """24 colours as a 12x2 grid (+ animated RGB / ASTRAL cells once unlocked); the selection is framed in white."""
    def cell(sx: int, name: str, label: str, col: RGB, sel: bool) -> None:
        if sel:
            screen.put(sx, y + 2, "▐", Palette.WHITE, Palette.PANEL)
            screen.put(sx + 1, y + 2, "█", col, Palette.PANEL)
            screen.put(sx + 2, y + 2, "▌", Palette.WHITE, Palette.PANEL)
            screen.text(sx + 4, y + 2, label, col, Palette.PANEL)
        else:
            screen.text(sx, y + 2, "███", col, Palette.PANEL)
            screen.text(sx + 4, y + 2, label, shade(col, 0.6), Palette.PANEL)
    if rgb:
        cell(x, "RGB", "RGB", rainbow(t), rgb_sel)
    if astral_on:
        cell(x + 8, "ASTRAL", "ASTRAL", astral(t), astral_sel)
    if prism_on:
        cell(x + 19, "PRISMATIC", "PRISM", prismatic(t), cur == "PRISMATIC")
    if void_on:
        cell(x + 29, "VOID", "VOID", void_tint(t), cur == "VOID")
    for k, (_, col) in enumerate(COLOR_CHOICES):
        sx, sy = x + (k % 12) * 3, y + k // 12
        if k == idx:
            screen.put(sx, sy, "▐", Palette.WHITE, Palette.PANEL)
            screen.put(sx + 1, sy, "█", col, Palette.PANEL)
            screen.put(sx + 2, sy, "▌", Palette.WHITE, Palette.PANEL)
        elif k == taken:
            screen.text(sx, sy, "╳╳╳", shade(col, 0.35), Palette.PANEL)
        else:
            screen.text(sx, sy, "███", col, Palette.PANEL)


def pick_ai_kit(save: SaveData, rng: random.Random, difficulty: Difficulty, player_color: int, ammo_pool: Optional[list] = None) -> PlayerSetup:
    """Opponent for a normal Single Player match. Master difficulty always fields the Master kit."""
    if difficulty is Difficulty.MASTER:
        lv = TOURNAMENT[-1]
        body, shot, design, ammo = lv.boss_color, lv.boss_shot, lv.tank, lv.ammo
    else:
        design = rng.choice(DESIGNS[:BASE_KITS]).key
        ammo = rng.choice(ammo_pool) if ammo_pool else DEFAULT_AMMO.key
        body = rng.choice([n for n, _ in COLOR_CHOICES])
        shot = rng.choice([n for n, _ in COLOR_CHOICES])
    bi = color_index(body)
    if bi == player_color:
        bi = (bi + 5) % len(COLOR_CHOICES)
    return PlayerSetup("AI", "CPU", COLOR_CHOICES[bi][1], COLOR_BY_NAME[shot], True, design, ammo)


def boss_setup(level: TournamentLevel, player_color: int) -> PlayerSetup:
    bi = color_index(level.boss_color)
    if bi == player_color:
        bi = (bi + 5) % len(COLOR_CHOICES)
    return PlayerSetup(level.boss, "BOSS", COLOR_CHOICES[bi][1], COLOR_BY_NAME[level.boss_shot], True, level.tank, level.ammo)


def unlock_hint(kind: str, key: str) -> str:
    for lv in TOURNAMENT:
        if (kind == "tank" and lv.tank == key) or (kind == "ammo" and lv.ammo == key):
            return f"LOCKED - CLEAR TOURNAMENT LEVEL {lv.number}"
    for m in MISSIONS:
        if f"{kind}:{key}" in m.rewards:
            return f"LOCKED - CLEAR MISSION {m.number}"
    if any(f"{kind}:{key}" in b.reward_ids() for b in TICKET_BOSSES):
        return "LOCKED - WIN IT FROM A TERMINAL TICKET"
    item = SHOP_BY_UID.get(f"{kind}:{key}")
    if item:
        return f"LOCKED - SHOP: {item.coins} COINS" + (f" + {item.credits} CREDITS" if item.credits else "")
    return "LOCKED"


class SetupScene(BackdropScene):
    """Loadout screen: tank design, hull colour, ammo, shell colour (+ difficulty) with a live preview.
    Modes: 'single', 'two' (two pages) and 'tournament' (one page, then straight into the run)."""

    def __init__(self, app, mode: str = "single", level: Optional[TournamentLevel] = None, rules: "Optional[Rules]" = None,
                 done: Optional[Callable] = None, pages: Optional[int] = None) -> None:
        super().__init__(app)
        self.mode, self.level, self.rules, self.done = mode, level, rules, done
        self.back: Optional[Scene] = None
        self.pages = pages or (2 if mode == "two" else 1)
        self.page = 0
        self.row = 0
        self.sel = [Loadout(**vars(app.save.loadout(i))) for i in range(2)]
        self.msg, self.msg_t = "", 0.0
        self.stage = PreviewStage(app.settings, 46, 26, 2)

    # -- rows ----------------------------------------------------------------
    def rows(self) -> list[str]:
        r = ["TANK", "HULL COLOR", "AMMO", "SHELL COLOR"]
        if self.mode == "single" and self.page == 0:
            r.append("AI DIFFICULTY")
        return r + ["CONFIRM"]

    def _cur(self) -> Loadout:
        return self.sel[self.page]

    def _locked(self, lo: Loadout) -> str:
        if lo.tank not in self.app.save.unlocked_tanks():
            return unlock_hint("tank", lo.tank)
        if lo.ammo not in self.app.save.unlocked_ammo():
            return unlock_hint("ammo", lo.ammo)
        return ""

    def _kits(self, name: str) -> tuple:
        """Every Tournament kit (locked ones are shown as locked) plus any secret kit that has been unlocked."""
        if name == "TANK":
            return (DESIGNS + SHOP_DESIGNS + MISSION_DESIGNS + TICKET_DESIGNS + ANCIENT_DESIGNS + custom_designs(self.app.save)
                    + tuple(d for d in SECRET_DESIGNS if d.key in self.app.save.unlocked_tanks()))
        return AMMOS + SHOP_AMMOS + MISSION_AMMOS + TICKET_AMMOS + ANCIENT_AMMOS + tuple(a for a in SECRET_AMMOS if a.key in self.app.save.unlocked_ammo())

    def _cycle_kit(self, items: tuple, attr: str, d: int) -> None:
        lo = self._cur()
        i = next(k for k, it in enumerate(items) if it.key == getattr(lo, attr))
        setattr(lo, attr, items[(i + d) % len(items)].key)

    def _color_names(self, attr: str) -> list:
        names = [n for n, _ in COLOR_CHOICES]
        if self.app.save.is_unlocked("rgb:hull" if attr == "tank_color" else "rgb:shell"):
            names.append("RGB")
        for nm in ("ASTRAL", "PRISMATIC", "VOID"):
            if self.app.save.is_unlocked(PAINT_UNLOCK[nm][0]):
                names.append(nm)
        return names

    def _cycle_color(self, attr: str, d: int) -> None:
        lo = self._cur()
        names = self._color_names(attr)
        cur = getattr(lo, attr)
        i = names.index(cur) if cur in names else 0
        other = self.sel[0].tank_color if (attr == "tank_color" and self.page == 1) else None
        for _ in range(len(names)):
            i = (i + d) % len(names)
            if names[i] != other:
                break
        setattr(lo, attr, names[i])

    def handle(self, ev: InputEvent) -> None:
        rows = self.rows()
        if ev.action is Action.BACK:
            if self.page > 0:
                self.page -= 1
                self.row = 0
            else:
                self.app.goto(TournamentScene(self.app, self.level.number) if self.mode == "tournament" else
                              self.back or MenuScene(self.app))
        elif ev.action is Action.UP:
            self.row = (self.row - 1) % len(rows)
        elif ev.action is Action.DOWN:
            self.row = (self.row + 1) % len(rows)
        elif ev.action in (Action.LEFT, Action.RIGHT):
            d = -1 if ev.action is Action.LEFT else 1
            name = rows[self.row]
            if name in ("TANK", "AMMO") and getattr(ev, "coarse", False):
                d *= 5                                   # SHIFT + A/D jumps five kits (there are a lot now)
            if name == "TANK":
                self._cycle_kit(self._kits("TANK"), "tank", d)
            elif name == "AMMO":
                self._cycle_kit(self._kits("AMMO"), "ammo", d)
            elif name == "HULL COLOR":
                self._cycle_color("tank_color", d)
            elif name == "SHELL COLOR":
                self._cycle_color("shot_color", d)
            elif name == "AI DIFFICULTY":
                order = [x for x in Difficulty if x is not Difficulty.MASTER or self.app.save.master_unlocked]
                s = self.app.settings
                s.difficulty = order[(order.index(s.difficulty) + d) % len(order)]
                self.app.apply_settings()
        elif ev.confirm:
            if rows[self.row] == "CONFIRM" or ev.action is Action.CONFIRM:
                lock = self._locked(self._cur())
                if lock:
                    self.msg, self.msg_t = lock, 2.5
                    self.row = 0
                elif self.page + 1 < self.pages:
                    self.page += 1
                    self.row = 0
                else:
                    self._finish()
            else:
                self.row = (self.row + 1) % len(rows)

    def _finish(self) -> None:
        app = self.app
        for i in range(self.pages):
            app.save.loadouts[i] = Loadout(**vars(self.sel[i]))
        app.save.flush()
        if self.mode in ("lan", "mission"):                  # these flows decide the match themselves
            self.done([Loadout(**vars(l)) for l in self.sel[:self.pages]])
            return
        mk = lambda i, name, label, ai=False: PlayerSetup(
            name, label, COLOR_BY_NAME[self.sel[i].tank_color], COLOR_BY_NAME[self.sel[i].shot_color], ai,
            self.sel[i].tank, self.sel[i].ammo,
            self.level.player_hp if (self.mode == "tournament" and i == 0) else (self.rules.hp if self.rules else MAX_HP))
        pcol = color_index(self.sel[0].tank_color)
        if self.mode == "tournament":
            cfg = MatchConfig([mk(0, "PLAYER", "YOU"), boss_setup(self.level, pcol)], True, self.level.difficulty, "tournament",
                              self.level.precision, ammo_fx=False)
            app.start_tournament(self.level, cfg)
            return
        if self.mode == "single":
            diff = app.settings.difficulty
            r = self.rules or Rules()
            me_setup = mk(0, "PLAYER", "YOU")
            pool = None
            if r.ammo_fx and me_setup.ammo != DEFAULT_AMMO.key:       # you chose a special ammo: the CPU fights with special ammo too
                banned = {BLESSING_KEY, DEFAULT_AMMO.key} | {a.key for a in ANCIENT_AMMOS + TICKET_AMMOS + SECRET_AMMOS}
                pool = sorted(k for k in app.save.unlocked_ammo() if k not in banned) or None
            players = [me_setup, replace(pick_ai_kit(app.save, app.rng, diff, pcol, pool), hp=r.hp)]    # otherwise it uses plain standard shells
            cfg = MatchConfig(players, True, diff, "single", target=r.target, label=r.label, ammo_fx=r.ammo_fx, special=r.special, pick_maps=r.pick_maps,
                              blessing=r.blessing and app.save.blessing_unlocked())
            app.start_match(cfg, r.mapdef, r.wind_value(app.settings))        # the rules screen already chose map + wind
            return
        else:
            r = self.rules or Rules()
            cfg = MatchConfig([mk(0, "PLAYER 1", "P1"), mk(1, "PLAYER 2", "P2")], False, app.settings.difficulty, "two",
                              target=r.target, label=r.label, special=r.special, ammo_fx=r.ammo_fx, pick_maps=r.pick_maps,
                              blessing=r.blessing and app.save.blessing_unlocked())
            app.start_match(cfg, r.mapdef, r.wind_value(app.settings))          # the rules screen already chose map + wind
            return
        if app.settings.random_maps:
            app.start_match(cfg, None)
        else:
            app.goto(MapSelectScene(app, cfg))

    def update(self, dt: float) -> None:
        super().update(dt)
        self.stage.update(dt)
        self.msg_t = max(0.0, self.msg_t - dt)

    def draw(self, screen: Screen) -> None:
        super().draw(screen)
        total_w = 92
        x, y = (screen.cols - total_w) // 2, (screen.rows - 24) // 2
        lo = self._cur()
        title = ("PILOT SETUP" if self.mode in ("single", "lan") or (self.mode == "mission" and self.pages == 1)
                 else f"PLAYER {self.page + 1} SETUP" if self.mode in ("two", "mission") else f"LEVEL {self.level.number} LOADOUT")
        screen.box(x, y, 44, 24, "double", Palette.LINE, Palette.PANEL, title)
        rows = self.rows()
        other = color_index(self.sel[0].tank_color) if self.page == 1 else None
        design, ammo = DESIGN_BY_KEY[lo.tank], AMMO_BY_KEY[lo.ammo]
        ry = y + 2
        for i, name in enumerate(rows):
            sel = i == self.row
            bg = Palette.PANEL_HI if sel else Palette.PANEL
            screen.fill(x + 2, ry, 40, 1, bg)
            screen.text(x + 3, ry, name, Palette.WHITE if sel else Palette.MUTED, bg)
            if name in ("TANK", "AMMO"):
                kit = design if name == "TANK" else ammo
                items = self._kits(name)
                pos = next((k for k, it in enumerate(items) if it.key == kit.key), 0) + 1
                bad = (kit.key not in (self.app.save.unlocked_tanks() if name == "TANK" else self.app.save.unlocked_ammo()))
                label = f"{pos}/{len(items)} {kit.name}" + (" ✦" if getattr(kit, "mech", ()) else "") + (" [LOCKED]" if bad else "")
                label = f"◄ {label} ►" if sel else label
                screen.text(x + 41 - len(label), ry, label, Palette.DANGER if bad else Palette.AMBER, bg)
                hint = unlock_hint("tank" if name == "TANK" else "ammo", kit.key) if bad else kit.blurb
                for k2, ln in enumerate(textwrap.wrap(hint, 39)[:2]):
                    screen.text(x + 3, ry + 1 + k2, ln, Palette.DANGER if bad else Palette.MUTED, Palette.PANEL)
                ry += 4
            elif name in ("HULL COLOR", "SHELL COLOR"):
                cur = lo.tank_color if name == "HULL COLOR" else lo.shot_color
                idx = color_index(cur, -1)
                label = f"◄ {cur} ►" if sel else cur
                screen.text(x + 41 - len(label), ry, label, live_color(COLOR_BY_NAME[cur], self.app.time), bg)
                draw_swatches(screen, x + 3, ry + 1, idx, other if name == "HULL COLOR" else None,
                              self.app.save.is_unlocked("rgb:hull" if name == "HULL COLOR" else "rgb:shell"), cur == "RGB",
                              self.app.time, self.app.save.is_unlocked("paint:astral"), cur == "ASTRAL", cur,
                              self.app.save.is_unlocked("paint:prismatic"), self.app.save.is_unlocked("paint:void"))
                ry += 4
            elif name == "AI DIFFICULTY":
                d = self.app.settings.difficulty.value
                screen.text(x + 41 - len(d) - 4, ry, f"◄ {d} ►" if sel else f"  {d}  ", GOLD if d == "MASTER" else Palette.AMBER, bg)
                ry += 2
            else:
                last = self.page + 1 == self.pages
                text = ("[ START RUN ]" if self.mode == "tournament" else "[ READY ]") if last else "[ NEXT PLAYER ]"
                screen.center(ry, text, Palette.PRIMARY if sel else Palette.MUTED, bg, x + 2, 40)
        draw_keycaps(screen, x + 2, y + 22, (("W/S", "ROW"), ("A/D", "PICK"), ("SHIFT", "x5"), ("ENTER", "OK")), Palette.PANEL)
        px = x + 46
        screen.box(px, y, 48, 24, "double", Palette.LINE, Palette.PANEL, "LIVE PREVIEW")
        lock = self._locked(lo)
        self.stage.set_kit(design, COLOR_BY_NAME[lo.tank_color], ammo, COLOR_BY_NAME[lo.shot_color], 0.45 if lock else 0.0)
        screen.blit(self.stage.render(), px + 1, y + 2)
        screen.center(y + 16, f"{design.name}  +  {ammo.name}", Palette.TEXT, Palette.PANEL, px, 48)
        screen.center(y + 17, f"{lo.tank_color} HULL  /  {lo.shot_color} SHELL", Palette.MUTED, Palette.PANEL, px, 48)
        if self.mode == "tournament":
            lv = self.level
            screen.center(y + 19, f"VS {lv.boss}  ·  {lv.difficulty.value} AI", GOLD if lv.difficulty is Difficulty.MASTER else Palette.AMBER, Palette.PANEL, px, 48)
            screen.center(y + 20, lv.objective, Palette.TEXT, Palette.PANEL, px, 48)
            screen.center(y + 21, lv.rule_text, GOLD if lv.rule else Palette.MUTED, Palette.PANEL, px, 48)
            screen.center(y + 18, "SPECIAL AMMO IS OFF IN TOURNAMENT", Palette.MUTED, Palette.PANEL, px, 48)
        else:
            if self.mode == "mission":
                notes, nc = ["SPECIAL AMMO IS OFF IN MISSIONS", "Every kit is cosmetic: all ammo plays alike."], Palette.MUTED
            elif self.mode == "lan" and self.rules is None:
                notes, nc = ["✦ = ammo with a special effect", "LAN: the host's SPECIAL AMMO rule decides."], Palette.AMBER
            elif self.rules is not None and not self.rules.ammo_fx:
                notes, nc = ["SPECIAL AMMO IS OFF (match rule)", "✦ ammo plays like plain ammo this match."], Palette.MUTED
            else:
                notes, nc = ["SPECIAL AMMO IS ON  ✦ = has an effect", "Bounce, split, cluster... see HOW TO PLAY."], Palette.AMBER
            for k, nt in enumerate(notes):
                screen.center(y + 19 + k, nt, nc, Palette.PANEL, px, 48)
            if self.rules is not None and self.rules.special:
                screen.center(y + 21, "SPECIAL ATTACK: X skips a turn; after two,", GOLD, Palette.PANEL, px, 48)
                screen.center(y + 22, "your next shot deals 3 damage." if self.msg_t <= 0 else self.msg, GOLD if self.msg_t <= 0 else Palette.DANGER, Palette.PANEL, px, 48)
        if self.msg_t > 0:
            screen.center(y + 22, self.msg, Palette.DANGER, Palette.PANEL, px, 48)
        elif not (self.rules is not None and self.rules.special and self.mode != "tournament"):
            screen.center(y + 22, "ESC  BACK", Palette.MUTED, Palette.PANEL, px, 48)


class MapSelectScene(BackdropScene):
    VISIBLE = 17

    def __init__(self, app, cfg: MatchConfig) -> None:
        super().__init__(app)
        self.cfg = cfg
        self.maps = list(MAPS) + app.save.unlocked_maps()
        self.index = 0          # 0 = RANDOM, 1..N = maps
        self.cache: dict = {}

    def handle(self, ev: InputEvent) -> None:
        n = len(self.maps) + 1
        if ev.action is Action.BACK:
            self.app.goto(SetupScene(self.app, self.cfg.mode))
        elif ev.action is Action.UP:
            self.index = (self.index - 1) % n
        elif ev.action is Action.DOWN:
            self.index = (self.index + 1) % n
        elif ev.action is Action.LEFT:
            self.index = max(0, self.index - 8)
        elif ev.action is Action.RIGHT:
            self.index = min(n - 1, self.index + 8)
        elif ev.confirm:
            self.app.start_match(self.cfg, None if self.index == 0 else self.maps[self.index - 1])

    def _preview(self, mapdef: MapDefinition) -> PixelCanvas:
        if mapdef.key not in self.cache:
            rng = random.Random(1)
            terrain = Terrain(46, 26, mapdef, rng)
            tanks = []
            for i in range(2):
                x = int(mapdef.spawns[i] * 46)
                terrain.flatten(x + 0.5, 10)
                tanks.append(x + 0.5)
            scen = Scenery(46, 26, mapdef.theme, rng)
            scen.apply_terrain(terrain)
            self.cache[mapdef.key] = (scen, terrain, tanks)
        scen, terrain, tanks = self.cache[mapdef.key]
        cv = scen.canvas()
        scen.draw_stars(cv, self.app.time)
        scen.draw_aurora(cv, self.app.time)
        scen.draw_fx(cv, self.app.time)
        for i, x in enumerate(tanks):
            p = self.cfg.players[i]
            draw_tank(cv, x, terrain.support_height(x), 1 if i == 0 else -1, 50 if i == 0 else 130, p.tank_color,
                      design=DESIGN_BY_KEY[p.design], t=self.app.time)
        return cv

    def draw(self, screen: Screen) -> None:
        super().draw(screen)
        x, y = (screen.cols - 88) // 2, (screen.rows - 22) // 2
        n = len(self.maps) + 1
        screen.box(x, y, 34, 22, "double", Palette.LINE, Palette.PANEL, "BATTLEFIELD")
        top = int(clamp(self.index - self.VISIBLE // 2, 0, n - self.VISIBLE))
        for row in range(self.VISIBLE):
            i = top + row
            sel = i == self.index
            bg = Palette.PANEL_HI if sel else Palette.PANEL
            if i == 0:
                name = "RANDOM"
            else:
                m = self.maps[i - 1]
                name = f"{i:02d} {m.name}" if not m.special else f"★  {m.name}"
            col = self.maps[i - 1].accent if (i and self.maps[i - 1].special) else Palette.AMBER if i == 0 and not sel else Palette.WHITE if sel else Palette.MUTED
            screen.fill(x + 2, y + 2 + row, 30, 1, bg)
            screen.text(x + 3, y + 2 + row, ("▶ " if sel else "  ") + name, col, bg)
        if top > 0:
            screen.put(x + 31, y + 2, "▲", Palette.PRIMARY_DIM, Palette.PANEL)
        if top + self.VISIBLE < n:
            screen.put(x + 31, y + 2 + self.VISIBLE - 1, "▼", Palette.PRIMARY_DIM, Palette.PANEL)
        screen.center(y + 20, f"{self.index}/{n - 1}  ·  A/D PAGE", Palette.MUTED, Palette.PANEL, x, 34)
        px = x + 36
        screen.box(px, y, 52, 22, "double", Palette.LINE, Palette.PANEL, "TERRAIN SCAN")
        m = self.maps[self.index - 1] if self.index else MAPS[int(self.app.time / 1.2) % len(MAPS)]
        screen.blit(self._preview(m), px + 3, y + 2)
        screen.center(y + 16, m.name if self.index else "RANDOM", m.accent if m.special else Palette.AMBER, Palette.PANEL, px, 52)
        desc = m.description if self.index else "A fresh battlefield every round."
        cut = desc.rfind(" ", 0, 48) if len(desc) > 48 else len(desc)
        screen.center(y + 17, desc[:cut], Palette.TEXT, Palette.PANEL, px, 52)
        screen.center(y + 18, desc[cut:].strip(), Palette.TEXT, Palette.PANEL, px, 52)
        if self.index:
            dots = lambda k: "●" * k + "○" * (3 - k)
            screen.center(y + 19, f"COVER {dots(m.cover)}     RELIEF {dots(m.relief)}", Palette.PRIMARY, Palette.PANEL, px, 52)
        screen.center(y + 20, "Opening map only - later rounds rotate", Palette.MUTED, Palette.PANEL, px, 52)


# ---------------------------------------------------------------------------
# Battle
# ---------------------------------------------------------------------------
class Phase(Enum):
    INTRO = auto()
    AIMING = auto()
    AI_THINKING = auto()
    FLIGHT = auto()
    IMPACT = auto()
    SETTLE = auto()
    DESTRUCTION = auto()
    BLESSING = auto()          # MARTYR'S BLESSING: the cosmic-clock detonation (see BlessingFX)


@dataclass
class Layout:
    cols: int
    rows: int
    fx: int
    fy: int
    fcols: int
    frows: int
    top: int


@dataclass
class FloatText:
    x: float
    y: float
    text: str
    color: RGB
    age: float = 0.0
    life: float = 1.4


@dataclass
class Banner:
    text: str
    sub: str
    color: RGB
    life: float = 1.2
    age: float = 0.0


class ScreenShake:
    def __init__(self) -> None:
        self.mag = 0.0

    def kick(self, m: float) -> None:
        self.mag = max(self.mag, m)

    def update(self, dt: float) -> None:
        self.mag = max(0.0, self.mag - dt * 7)

    def offset(self, rng: random.Random, enabled: bool) -> tuple[int, int]:
        if not enabled or self.mag < 0.35:
            return 0, 0
        return round(rng.uniform(-1, 1) * self.mag), round(rng.uniform(-1, 1) * self.mag * 0.6)


# ============================================================================
# Ammo mechanics. A shot is planned once, at fire time, as a list of FLIGHTS (arcs) that end in impact EVENTS. Everything
# is computed on the fixed physics grid from the world state, so LAN machines plan the very same shot.
#   mech keys (all optional)      g, wind, speed : scale gravity / wind pull / muzzle speed
#   wob=(amp, freq) corkscrew     bounce=n, br   : skip off the ground n times (small blasts), radius br
#   radius=r  dmg=n               apex=("split", n, spread) | ("jump", dist)   : act at the top of the arc
#   cluster=n, cspace, cr         chain=n, cstep : bomblets / a walking line of blasts after the main impact
#   fuse=seconds, fr              terrain="blast"|"mound"|"trench"|"glass"       pull=px : drag tanks toward the impact
#   multi=1 : the blast can hurt every tank in reach
# ============================================================================
@dataclass
class Ev:
    x: float
    y: float
    kind: "ImpactKind"
    tank: int = -1
    radius: float = 1.0
    dmg: int = 1
    mode: str = "blast"          # blast | mound | trench | glass | fx
    delay: float = 0.0
    first: bool = False
    pull: float = 0.0
    multi: bool = False
    big: bool = False            # the special attack: bigger visuals everywhere


SPECIAL_DMG, SPECIAL_RADIUS = 3, 2.0


@dataclass
class FlightSpec:
    launch: Launch
    trace: TraceResult
    delay: float
    events: list = field(default_factory=list)


def make_ammo_launch(w: "BattleWorld", tank: Tank, angle: float, power: float, wind: Optional[float] = None,
                     special: bool = False) -> Launch:
    M = {} if (special or not getattr(w, "ammo_fx", True)) else tank.ammo.mechanics
    L = w.physics.make_launch(tank.launch_origin(angle), angle, power, gravity=w.physics.cfg.gravity * M.get("g", 1.0),
                              wind=(w.wind if wind is None else wind) * M.get("wind", 1.0))
    if M.get("speed", 1.0) != 1.0:
        L = replace(L, vx=L.vx * M["speed"], vy=L.vy * M["speed"])
    if "wob" in M:
        L = replace(L, wa=M["wob"][0], wf=M["wob"][1])
    return L


def plan_shot(w: "BattleWorld", tank: Tank, special: bool = False) -> list:
    """A special attack is a plain arc ending in one huge 3-damage blast (the ammo's own looks, scaled up)."""
    M, phys = ({} if (special or not getattr(w, "ammo_fx", True)) else tank.ammo.mechanics), w.physics
    L = make_ammo_launch(w, tank, tank.angle, tank.power, special=special)
    probe = lambda: w.owner_probe(tank.index)
    tr = phys.trace(L, probe(), 0.02, 0.0, 12.0)
    flights: list = []

    def main_ev(imp: Impact, first: bool, radius: float, mode: Optional[str] = None) -> Ev:
        if special:
            return Ev(imp.x, imp.y, imp.kind, imp.tank, SPECIAL_RADIUS, SPECIAL_DMG, "blast", 0.0, first, 0.0, False, True)
        return Ev(imp.x, imp.y, imp.kind, imp.tank, radius, int(M.get("dmg", 1)), mode or M.get("terrain", "blast"), 0.0, first,
                  float(M.get("pull", 0.0)), bool(M.get("multi", 0)))

    def finale(imp: Impact, first: bool, vx: float) -> list:
        evs = [main_ev(imp, first, M.get("radius", 1.0))]
        if imp.kind is ImpactKind.OUT:
            return evs
        if M.get("cluster"):
            n, sp = int(M["cluster"]), float(M.get("cspace", 5.5))
            for k in range(n):
                bx = clamp(imp.x + (k - (n - 1) / 2) * sp, 2, w.width - 3)
                evs.append(Ev(bx, w.terrain.height_at(bx) + 1.0, ImpactKind.TERRAIN, -1, M.get("cr", 0.55), 1, "blast", 0.09 + 0.07 * k))
        if M.get("chain"):
            n, st = int(M["chain"]), float(M.get("cstep", 7.0))
            d = 1.0 if vx >= 0 else -1.0
            for k in range(1, n + 1):
                bx = clamp(imp.x + d * k * st, 2, w.width - 3)
                evs.append(Ev(bx, w.terrain.height_at(bx) + 1.0, ImpactKind.TERRAIN, -1, M.get("cr", 0.6), 1, "blast", 0.1))
        if M.get("fuse"):
            evs.append(Ev(imp.x, imp.y, ImpactKind.TERRAIN, -1, M.get("fr", 1.5), int(M.get("dmg", 1)), "blast", float(M["fuse"])))
        return evs

    apex = M.get("apex")
    t_ap = L.vy / L.g if L.g > 0 else 0.0
    if apex and 0.35 < t_ap < tr.time - 0.2:
        ax, ay = L.at(t_ap)
        vxa, vya = L.vx + L.ax * t_ap, L.vy - L.g * t_ap
        portal = [Ev(ax, ay, ImpactKind.OUT, -1, 0.5, 0, "fx")] if apex[0] == "jump" else []
        flights.append(FlightSpec(L, TraceResult(Impact(ax, ay, ImpactKind.OUT), [], t_ap), 0.0, portal))
        if apex[0] == "split":
            n, spread = int(apex[1]), float(apex[2])
            for k in range(n):
                f = (k - (n - 1) / 2) / max(1.0, (n - 1) / 2)
                sub = replace(L, x0=ax + f * 1.5, y0=ay, vx=vxa * (1 + spread * f) + f * spread * 6, vy=vya + 1.0 - abs(f) * 3.0, wa=0.0)
                st = phys.trace(sub, probe(), 0.02, 0.0, 12.0)
                flights.append(FlightSpec(sub, st, t_ap + 0.04 * k, [main_ev(st.impact, k == 0, M.get("subr", 0.6), "blast")]))
            return flights
        dirx = 1.0 if vxa >= 0 else -1.0
        nx = clamp(ax + dirx * float(apex[1]), 4, w.width - 5)
        sub = replace(L, x0=nx, y0=ay, vx=vxa, vy=vya, wa=0.0)
        st = phys.trace(sub, probe(), 0.02, 0.0, 12.0)
        flights.append(FlightSpec(sub, TraceResult(Impact(nx, ay, ImpactKind.OUT), [], 0.0), t_ap + 0.1,
                                  [Ev(nx, ay, ImpactKind.OUT, -1, 0.5, 0, "fx")]))
        flights.append(FlightSpec(sub, st, t_ap + 0.12, finale(st.impact, True, vxa)))
        return flights
    bounces, cur, st, tcum = int(M.get("bounce", 0)), L, tr, 0.0
    for b in range(bounces + 1):
        imp = st.impact
        vxi, vyi = cur.vx + cur.ax * st.time, cur.vy - cur.g * st.time
        last = b == bounces or imp.kind is not ImpactKind.TERRAIN or vyi > -4
        evs = finale(imp, b == 0, vxi) if last else [Ev(imp.x, imp.y, ImpactKind.TERRAIN, -1, M.get("br", 0.4), 1, "blast", 0.0, b == 0)]
        flights.append(FlightSpec(cur, st, tcum, evs))
        tcum += st.time
        if last:
            break
        cur = replace(cur, x0=imp.x, y0=imp.y + 1.5, vx=vxi * 0.82, vy=-vyi * 0.55)
        st = phys.trace(cur, probe(), 0.02, 0.0, 12.0)
    return flights


class BlessingFX:
    """MARTYR'S BLESSING, a four-act spectacle drawn on terminal cells (no raw output, nothing to restore):
      I   THE CLOCK  - an enormous Roman-numeral dial opens around the impact; hands sweep, rings turn.
      II  THE SUN    - 24 stars (12 that duplicate into 24) spiral in from the dial and fuse into a swelling golden sun.
      III THE JUDGMENT - the sun cracks; a reticle marks each victim, then a column of light falls on it. Tanks are struck first,
                         then pillars sweep outward across half the battlefield, each one carving a real crater.
      IV  THE FINALE - the dial implodes into the centre and a colossal white nova erases what is left.
    Everything is bounded (<=12 numerals, 4 rings, 24 stars, <=MAXT pillars) and `full=False` (SCREEN EFFECTS off) keeps the same
    gameplay beats on a short, calm timeline. Gameplay (kills, craters) is applied by BattleScene from the events below."""
    ROMAN = ("XII", "I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX", "X", "XI")
    GOLDC, VIOLET, PURPLE, WHITEC = (255, 214, 90), (176, 120, 255), (104, 46, 180), (255, 255, 255)
    CHAIN_N, MAXT = 12, 14

    def __init__(self, wx: float, wy: float, targets: list, full: bool = True, wipes: tuple = ()) -> None:
        self.wx, self.wy, self.full, self.t = wx, wy, full, 0.0
        self.targets = list(targets)[:self.MAXT]                     # (world x, world y, tank index or None): victims first
        self.wipes = list(wipes)[:16]                                # (world x, world y): the final firewall's craters across the whole field
        if full:
            self.T_SUN, self.T_BANG, self.first, self.gap, self.tel, tail = 1.6, 3.0, 0.7, 0.1, 0.6, 2.6
        else:
            self.T_SUN, self.T_BANG, self.first, self.gap, self.tel, tail = 99.0, 0.4, 0.2, 0.04, 0.2, 0.5
        self.arrive = [self.T_BANG + self.first + self.gap * i for i in range(len(self.targets))]
        self.T_CONV = (max(self.arrive) if self.arrive else self.T_BANG) + 0.45        # the finale (the nuke) starts here
        self.wipe_t = [self.T_CONV + 0.05 + abs(x - wx) / (55.0 if full else 400.0) for x, _ in self.wipes]
        self.T_END = max([self.T_CONV + tail] + [w_ + 0.7 for w_ in self.wipe_t])
        self.killed, self.skipped, self._fired = False, False, set()
        self.cell = (0, 0, 6, 3)

    def skip(self) -> None:
        """Jump to the end. Every hit and the finale still resolve (the scene applies them instantly, without the spectacle)."""
        self.skipped = True
        self.t = max(self.t, self.T_END)

    def events(self) -> list:
        keys = [("bang", self.T_BANG)] + [(f"hit{i}", a) for i, a in enumerate(self.arrive)] + [("finale", self.T_CONV)] \
            + [(f"wipe{i}", w_) for i, w_ in enumerate(self.wipe_t)]
        out = []
        for key, when in keys:
            if key not in self._fired and self.t >= when:
                self._fired.add(key)
                if not self.skipped or key.startswith(("hit", "wipe")) or key == "finale":
                    out.append(key)
        return out

    @staticmethod
    def _c(v: int, lo: int, hi: int) -> int:
        return (lo + hi) // 2 if lo > hi else max(lo, min(hi, v))

    def draw(self, screen: "Screen", L: "Layout", cell_of: Callable) -> None:
        t, full = self.t, self.full
        ry = max(3, min(L.frows // 2 - 1, 12))
        rx = max(6, min(ry * 2 + 2, L.fcols // 2 - 3))
        ix, iy = cell_of(self.wx, self.wy)
        cx = self._c(ix, L.fx + rx + 2, L.fx + L.fcols - rx - 3)
        cy = self._c(iy, L.fy + ry + 1, L.fy + L.frows - ry - 2)
        self.cell = (cx, cy, rx, ry)
        compact = L.fcols < 46 or L.frows < 16                      # narrow/short terminals: four numerals instead of twelve
        x0, y0, x1, y1 = L.fx, L.fy, L.fx + L.fcols, L.fy + L.frows
        G, V, P, W = self.GOLDC, self.VIOLET, self.PURPLE, self.WHITEC

        def put(x: int, y: int, ch: str, col: RGB) -> None:
            if x0 <= x < x1 and y0 <= y < y1:
                screen.put(x, y, ch, col)

        def text(x: int, y: int, s: str, col: RGB) -> None:
            for j, ch in enumerate(s):
                put(x + j, y, ch, col)

        def ellipse(px: int, py: int, ax: float, ay: float, fade: float, hot: RGB) -> None:      # filled glowing blob
            for dy in range(-int(ay) - 1, int(ay) + 2):
                for dx in range(-int(ax) - 1, int(ax) + 2):
                    d = math.hypot(dx / max(0.5, ax), dy / max(0.5, ay))
                    if d <= 1.0:
                        put(px + dx, py + dy, "█▓▒░"[min(3, int(d * 4))], mix(gradient(((0.0, W), (0.35, hot), (0.75, V), (1.0, P)), d), P, 0.8 * (1 - fade)))

        def ring(px: int, py: int, ax: float, ay: float, n: int, col: RGB, fade: float, glyphs: str = "○◦") -> None:
            for j in range(n):
                a = j * math.tau / n
                put(px + int(round(math.cos(a) * ax)), py + int(round(math.sin(a) * ay)), glyphs[j % len(glyphs)], shade(col, fade))

        conv = clamp((t - self.T_CONV) / max(0.01, self.T_END - self.T_CONV), 0, 1) if full else 0.0
        k = ease_out(clamp(t / 1.0, 0, 1)) * (1 - conv * conv)
        screen.tint(x0, y0, L.fcols, L.frows, (46, 12, 104), 0.40 * clamp(t / 0.8, 0, 1) * (1 - 0.6 * conv))

        # I - the clock: two turning rings, 12 numerals (4 on small screens), sweeping hands
        for ri, (f, col, ch, dr) in enumerate(((1.0, G, "·", 1), (0.86, V, "•", -1), (0.62, G, "·", 1), (0.3, P, "◦", -1))):
            n = int(clamp(math.tau * rx * f * 0.9, 24, 140))
            for j in range(n):
                a = j * math.tau / n + dr * t * (0.35 + 0.2 * ri)
                mark = "✧" if j % 10 == 0 else ch
                put(cx + int(round(math.sin(a) * rx * f * k)), cy - int(round(math.cos(a) * ry * f * k)), mark,
                    shade(col, 0.55 + 0.45 * math.sin(t * 5 + j * 0.7) ** 2) if mark == ch else W)
        for i, rn in enumerate(self.ROMAN):
            if (compact and i % 3) or t < 0.25 + i * 0.06:
                continue
            a = i * math.tau / 12 + t * 0.18
            lit = (int(t * 3) % 12) == i and conv == 0
            text(cx + int(round(math.sin(a) * rx * 0.93 * k)) - len(rn) // 2, cy - int(round(math.cos(a) * ry * 0.93 * k)), rn, W if lit else G)
        for length, spd, col in ((0.55, 2.4, G), (0.82, 0.8, W)):
            ha = t * spd * (1 - conv)
            for s in range(1, 13):
                f = s / 12 * length * k
                put(cx + int(round(math.sin(ha) * rx * f)), cy - int(round(math.cos(ha) * ry * f)), "•" if s % 3 else "◦", col)
        if rx >= 8 and L.frows >= 10:
            title = "M A R T Y R ' S   B L E S S I N G"
            if len(title) > L.fcols - 2:
                title = "MARTYR'S BLESSING"[:max(0, L.fcols - 2)]
            text(x0 + (L.fcols - len(title)) // 2, y0 + 1, title, mix(P, G, 0.5 + 0.5 * math.sin(t * 6)))
            if t >= self.T_BANG and conv < 1 and L.frows >= 14:
                sub = "TIME RECEIVES ITS FINAL JUDGMENT"[:max(0, L.fcols - 2)]
                text(x0 + (L.fcols - len(sub)) // 2, y0 + 2, sub, W)

        # II - the sun: one clean spiral of stars (12, then 24 after the duplication) winds into a swelling golden sun
        if full and self.T_SUN <= t < self.T_BANG:
            u_all = clamp((t - self.T_SUN) / (self.T_BANG - self.T_SUN), 0, 1)
            twin = 2 if t >= self.T_SUN + 0.6 else 1
            for j in range(12 * twin):
                u = clamp(u_all * 1.15 - (j % 12) * 0.02, 0, 1)
                ang = (j % 12) * math.tau / 12 + (math.pi / 12 if j >= 12 else 0) + u * 5.5
                r = (1 - u) ** 1.4
                put(cx + int(round(math.cos(ang) * rx * 0.95 * r)), cy + int(round(math.sin(ang) * ry * 0.95 * r)),
                    "✦" if j < 12 else "✧", G if j < 12 else W)
            sr = ease_out(u_all)
            ellipse(cx, cy, rx * 0.34 * sr, ry * 0.34 * sr, 1.0, G)
        # III - the judgment
        if full and t >= self.T_BANG:
            b = t - self.T_BANG
            if b < 0.9:                                                          # the crack: flash, flare, shock ring
                ellipse(cx, cy, rx * 0.34 * (1 - b / 0.9) + 1, ry * 0.34 * (1 - b / 0.9) + 1, 1 - b / 0.9, W)
                rs = ease_out(clamp(b / 0.7, 0, 1))
                ring(cx, cy, rx * 1.05 * rs, ry * 1.05 * rs, 64, mix(W, G, b / 0.9), 1 - b / 0.9)
                for j in range(8):
                    a = j * math.pi / 4
                    for s in range(2, int(2 + 8 * rs)):
                        put(cx + int(round(math.cos(a) * s * 2)), cy - int(round(math.sin(a) * s)), "✦" if s == int(7 * rs) else "·", G if j % 2 else W)
            for j in range(40):                                                          # shooting stars fly out of the clock in every direction
                age = t - (self.T_BANG + 0.12 * j)
                if 0 <= age < 1.0:
                    a = j * 2.399 + 0.4                                                  # golden-angle spread: even coverage, never a clump
                    spd = 34 + (j % 5) * 9
                    hx, hy = cx + math.cos(a) * spd * age * 1.7, cy + math.sin(a) * spd * age * 0.55 - age * age * 10
                    for tr in range(7):
                        a2 = max(0.0, age - tr * 0.025)
                        put(int(round(cx + math.cos(a) * spd * a2 * 1.7)), int(round(cy + math.sin(a) * spd * a2 * 0.55 - a2 * a2 * 10)),
                            "✦" if tr == 0 else "·", shade(G if j % 3 else W if j % 3 == 0 else V, 1 - tr * 0.13))
            top = y0 + 3
            for i, (tgt, arr) in enumerate(zip(self.targets, self.arrive)):
                ex, ey = cell_of(tgt[0], tgt[1])
                ex, ey = int(clamp(ex, x0 + 2, x1 - 3)), int(clamp(ey, y0 + 4, y1 - 2))
                vic = tgt[2] is not None
                age = t - arr
                if -self.tel <= age < -0.12:                                         # the reticle contracts onto the victim
                    p = 1 - (age + self.tel) / (self.tel - 0.12)
                    rr = 2 + (1 - p) * 7
                    ring(ex, ey, rr * 2, rr * 0.7, 16, G if int(t * 12) % 2 else W, 0.5 + 0.5 * p)
                    put(ex, ey, "✛", W)
                if -0.12 <= age < 0.45:                                              # the pillar of light
                    drop = clamp((age + 0.12) / 0.12, 0, 1)
                    head = int(top + (ey - top) * drop)
                    wide = 2 if vic else 1
                    fade = 1.0 if age < 0.12 else clamp(1 - (age - 0.12) / 0.33, 0, 1)
                    for yy in range(top, head + 1):
                        put(ex, yy, "┃", shade(W, 0.5 + 0.5 * fade))
                        for sdx in range(1, wide + 1):
                            c = mix(G, V, (sdx - 1) / max(1, wide))
                            put(ex - sdx, yy, "│" if sdx == 1 else "╎", shade(c, 0.35 + 0.5 * fade))
                            put(ex + sdx, yy, "│" if sdx == 1 else "╎", shade(c, 0.35 + 0.5 * fade))
                    put(ex, head, "▼", W)
                if 0 <= age < 0.9:                                                   # the strike: twin shock rings, rising sparks
                    sw = (2 + age * (22 if vic else 15)) * ease_out(clamp(age / 0.5, 0, 1) + 0.2)
                    ring(ex, ey, sw, sw * 0.4, 24, G, 1 - age / 0.9)
                    if vic and age > 0.08:
                        ring(ex, ey, sw * 0.62, sw * 0.25, 16, V, 1 - age / 0.9, "◦•")
                    for j in range(6 if vic else 4):
                        a = j * math.pi / (3 if vic else 2) + 0.3
                        put(ex + int(round(math.cos(a) * age * 11)), ey - int(round(abs(math.sin(a)) * age * 9 - age * age * 12)), "✦", W if j % 2 else G)
        # IV - the finale, a nuclear end of the world: the dial implodes, white-out, a rising mushroom cloud and a firewall racing
        #      across the whole field (each wall-front crater is a real one: see the wipe events)
        if full and t >= self.T_CONV:
            f = t - self.T_CONV
            if conv > 0:
                for j in range(24):
                    a = j * math.tau / 24
                    put(cx + int(round(math.sin(a) * rx * (1 - conv))), cy - int(round(math.cos(a) * ry * (1 - conv))), "✦", W)
            base = min(y1 - 2, cy + ry + 2)                                        # the ground the cloud grows from
            if f < 0.5:                                                            # white-out fireball
                ellipse(cx, base, rx * (0.6 + 2.2 * f), ry * (0.5 + 1.6 * f), 1 - f / 0.5 * 0.4, W)
            p = ease_out(clamp(f / 2.2, 0, 1))
            fade = 1 - clamp((f - 2.0) / 1.2, 0, 1)
            top_y = int(base - (base - (y0 + 4)) * 0.8 * p)
            if f > 0.15 and fade > 0:
                for yy in range(top_y, base + 1):                                  # the stem: hot at the core, rolling smoke at the edges
                    sw = max(2, int(2 + (1 - (yy - top_y) / max(1, base - top_y)) * 2 + (yy - top_y) / max(1, base - top_y) * 5))
                    for dx in range(-sw, sw + 1):
                        u = abs(dx) / max(1, sw)
                        put(cx + dx, yy, "█▓▒░"[min(3, int(u * 4))], shade(gradient(((0, (255, 226, 150)), (0.5, (255, 110, 30)), (1, (120, 28, 18))), u), 0.5 + 0.5 * fade))
                cap_rx, cap_ry = rx * 1.25 * p + 4, ry * 0.95 * p + 2                  # the cap: a boiling mushroom of fire
                for dy in range(-int(cap_ry) - 1, int(cap_ry) + 2):
                    for dx in range(-int(cap_rx) - 1, int(cap_rx) + 2):
                        d = math.hypot(dx / cap_rx, dy / cap_ry + 0.25 * math.sin(dx * 0.4 + t * 6) * 0.3)
                        if d <= 1.0:
                            put(cx + dx, top_y + dy, "█▓▒░"[min(3, int(d * 4))],
                                shade(gradient(((0, (255, 240, 190)), (0.3, (255, 130, 36)), (0.7, (210, 48, 20)), (1, (60, 18, 30))), d), 0.4 + 0.6 * fade))
            for sgn in (-1, 1):                                                   # the firewall sweeping the whole field
                wx_ = cx + sgn * int(f * 55)
                if x0 <= wx_ < x1 and f < 2.5:
                    for yy in range(y0 + 2, y1 - 1):
                        k_ = (yy * 7 + int(t * 20)) % 5
                        for th_ in range(7):                                         # a wall of fire: white-hot front, cooling trail behind it
                            put(wx_ - sgn * th_, yy - (th_ + k_) % 2, "█▓▓▒▒░░"[th_],
                                shade(mix((255, 245, 200), (190, 50, 20), th_ / 6), 0.55 + 0.45 * ((k_ + th_) % 3) / 2))
        if t > self.T_END - 0.3:                                                    # cosmic flash marks the end
            screen.tint(x0, y0, L.fcols, L.frows, W, clamp((t - (self.T_END - 0.3)) / 0.3, 0, 1) * 0.85)


class LiveFlight:
    """A flight currently animating on screen."""
    def __init__(self, spec: FlightSpec, owner: int) -> None:
        self.spec, self.delay = spec, spec.delay
        self.proj = Projectile(spec.launch, owner, lambda x, y: None)
        self.last = (spec.launch.x0, spec.launch.y0)
        self.n = 0


class BattleScene(Scene):
    show_wallet = False
    ambient = True
    INTRO_TIME = 1.8

    def __init__(self, app, session: MatchSession) -> None:
        super().__init__(app)
        self.session = session
        self.world: Optional[BattleWorld] = None
        self.phase = Phase.INTRO
        self.t = 0.0
        self.turn = 0
        self.order: list = []
        self.trace: Optional[TraceResult] = None
        self.ais: dict = {}
        self.inbox_wait = 0.0
        self._aim_sent = None
        self._aim_timer = 0.0
        self.proj: Optional[Projectile] = None
        self.flights: list = []
        self.special_shot = False
        self.blessing_shot, self.bless, self._bless_blocked = False, None, False
        self._off: list = []
        self._fx_warned: set = set()
        self.pending: list = []
        self.ev: Optional[Ev] = None
        self.victims: list = []
        self.doomed_more: list = []
        self.shake = ScreenShake()
        self.floaters: list[FloatText] = []
        self.banner: Optional[Banner] = None
        self.flash = 0.0
        self.guide: list = []
        self.guide_key = None
        self.plan: Optional[AimPlan] = None
        self.plan_from = (0.0, 0.0)
        self.victim: Optional[Tank] = None
        self.doomed: Optional[Tank] = None
        self.impact: Optional[Impact] = None
        self.flags: set = set()
        self.last_trail = (0.0, 0.0)
        self.trail_n = 0
        self.size = (0, 0)
        self.wiped = False
        self.finished = False
        self.layout: Optional[Layout] = None

    # -- setup ---------------------------------------------------------------
    def enter(self) -> None:
        self._build()
        cfg = self.session.config
        for t, pl in zip(self.world.tanks, cfg.players):
            if t.is_ai:
                self.ais[t.index] = AIController(pl.diff or cfg.difficulty, random.Random(self.session.rng.randrange(1 << 30)),
                                                 cfg.ai_precision * pl.prec)
        m = self.session.current_map
        wt = self._wind_text()
        sub = m.name + (f"   ·   {wt}" if wt else "")
        if not cfg.ammo_fx and any(t.ammo.mechanics for t in self.world.tanks if not t.is_ai):
            sub += "   ·   SPECIAL AMMO OFF"
        if self._bless_blocked:
            sub += "   ·   MARTYR'S BLESSING OFF HERE"
        note = self.session.banner_note()
        warn = note.startswith("FINAL")
        self.banner = Banner(self.session.banner_title(), f"{note}   ·   {sub}" if note else sub,
                             Palette.DANGER if warn else Palette.PRIMARY, self.INTRO_TIME * (1.4 if warn else 1.0))

    def _layout(self) -> Layout:
        s = self.app.screen
        fc = min(s.cols, MAX_FIELD_COLS)
        fr = max(8, min(s.rows - HUD_TOP_ROWS - HUD_BOTTOM_ROWS, MAX_FIELD_ROWS))
        if self.session.config.world_size:                  # LAN: both machines draw the same fixed-size battlefield
            fc, fr = self.session.config.world_size[0], self.session.config.world_size[1] // 2
        top = max(0, (s.rows - (fr + HUD_TOP_ROWS + HUD_BOTTOM_ROWS)) // 2)
        return Layout(s.cols, s.rows, (s.cols - fc) // 2, top + HUD_TOP_ROWS, fc, fr, top)

    def _build(self) -> None:
        self.layout = L = self._layout()
        self.size = (self.app.screen.cols, self.app.screen.rows)
        s = self.app.settings
        if self.session.wind_override is not None:           # GALE FORCE: a private settings copy with the wind pinned high
            s = replace(s, wind=self.session.wind_override)
        self.cfg_settings = s
        size = self.session.config.world_size or (L.fcols, L.frows * 2)
        seed = self.session.round_seed()
        self.world = BattleWorld(size[0], size[1], self.session.current_map, seed, self.session.config.players, s)
        self.world.ammo_fx = self.session.config.ammo_fx
        if not self.session.config.blessing:
            for i, (tk, pl) in enumerate(zip(self.world.tanks, self.session.config.players)):
                if pl.ammo == BLESSING_KEY:                              # PlayerSetup is frozen: swap in a corrected copy
                    self.session.config.players[i] = replace(pl, ammo=DEFAULT_AMMO.key)
                    tk.ammo = DEFAULT_AMMO
                    self._bless_blocked = True
        self.srng = random.Random(seed ^ 0xA11E)              # scene-level decisions every LAN machine must repeat identically
        self._make_order()

    def _check_resize(self) -> None:
        if self.size != (self.app.screen.cols, self.app.screen.rows):
            self.layout = L = self._layout()
            self.size = (self.app.screen.cols, self.app.screen.rows)
            if self.session.config.world_size is None and (L.fcols, L.frows * 2) != (self.world.width, self.world.height):
                self.world.resize(L.fcols, L.frows * 2)
                self.guide_key = None
                if self.phase in (Phase.FLIGHT, Phase.IMPACT):
                    self.proj = None
                    self.victim = None
                    self._end_turn()

    # -- helpers -------------------------------------------------------------
    def _turn_text(self, i: int) -> str:
        t = self.world.tanks[i]
        if t.kind == "ufo":
            return "UFO TURN"
        if t.is_ai:
            n = sum(1 for x in self.world.tanks if x.is_ai)
            return "AI TURN" if n == 1 else f"AI {sum(1 for x in self.world.tanks[:i + 1] if x.is_ai)} TURN"
        humans = [x for x in self.world.tanks if not x.is_ai]
        if len(humans) == 1:
            return "PLAYER TURN"
        return f"PLAYER {humans.index(t) + 1} TURN"

    @property
    def current(self) -> Tank:
        return self.world.tanks[self.turn]

    def _make_order(self) -> None:
        """Turn order: teams alternate, tanks inside a team take turns in seat order."""
        tanks = self.world.tanks
        lists = [[t.index for t in tanks if t.team == tm] for tm in sorted({t.team for t in tanks})]
        start = self.session.starting_player % len(lists)
        lists = lists[start:] + lists[:start]
        self.order = [l[k] for k in range(max(map(len, lists))) for l in lists if k < len(l)]
        self.turn = self.order[0]

    def _control(self, t: Tank) -> str:
        """'local' = this keyboard, 'ai' = computed here, 'remote' = driven by the LAN peer's messages."""
        cfg = self.session.config
        if cfg.net is None:
            return "ai" if t.is_ai else "local"
        if t.is_ai:
            return "ai" if cfg.my_owner == 0 else "remote"
        return "local" if t.owner == cfg.my_owner else "remote"

    def _target_for(self, me: Tank) -> Tank:
        foes = [t for t in self.world.tanks if t.team != me.team and t.alive]
        return min(foes, key=lambda t: (abs(t.x - me.x), t.hp)) if foes else self.world.tanks[0]

    def _begin_turn(self, volley: bool = False) -> None:
        tank = self.current
        self.guide_key = None
        if not volley and self._turn_start(tank):
            self.banner = Banner("UFO CHARGING", "", tank.color, 0.8)
            self._end_turn()
            return
        if not volley:
            self.session.on_turn_start(self, tank)
        ready = self.session.config.special and tank.charge >= self.session.config.special_wait
        self.banner = Banner(self._turn_text(self.turn), "SPECIAL READY - FIRE FOR 3 DAMAGE!" if ready else self._wind_text(), tank.color,
                             0.9 if volley else 1.3)
        ctl = self._control(tank)
        self._aim_sent = None
        cfg = self.session.config
        if (ctl == "ai" and cfg.mode == "single" and cfg.special and cfg.net is None and not volley and tank.index in self.ais
                and tank.kind != "ufo" and tank.charge < cfg.special_wait and self.srng.random() < 0.2):
            self._forfeit(tank, announce=False)                     # the CPU banks a charge now and then and later fires its special
            return
        if ctl == "ai" and tank.index in self.ais:
            self.plan = self.ais[tank.index].plan(tank, self._target_for(tank), self.world)
            self.plan_from = (tank.angle, tank.power)
            self.phase, self.t = Phase.AI_THINKING, 0.0
        else:
            self.phase, self.t = Phase.AIMING, 0.0

    def _turn_start(self, tank: Tank) -> bool:
        """Per-turn behaviour of special units. The saucer hops to a new spot, raises shields as it is hurt and, every few
        turns, fires a three-shot volley. Everything here runs off the shared scene rng so LAN machines stay in step."""
        if tank.kind != "ufo":
            return False
        w = self.world
        tank.turns += 1
        lo, hi = (0.52, 0.93) if tank.team == 1 else (0.07, 0.48)
        old, half = tank.x, int(tank.width / 2)
        others = [t for t in w.tanks if t is not tank and t.alive and t.kind == "ufo"]
        for _ in range(8):                               # a few tries to land clear of the other saucers
            cand = int(clamp(self.srng.uniform(lo, hi) * w.width, 12 + half, w.width - 13 - half)) + 0.5
            if all(abs(cand - o.x) >= (tank.width + o.width) / 2 + 3 for o in others):
                break
        tank.x = cand
        tank.vis_dx = old - tank.x
        w.terrain.flatten(tank.x, 10)
        tank.y = float(w.terrain.support_height(tank.x)) + tank.hover
        frac = tank.hp / tank.max_hp
        thresholds = (0.66, 0.33)
        drone = tank.name.startswith("DRONE")             # Mothership fight: only the mothership ever shields
        while tank.phase < 2 and frac <= thresholds[tank.phase]:
            tank.phase += 1
            if drone:
                continue
            tank.shield = 2
            self.floaters.append(FloatText(tank.x, tank.y + tank.height + 8, "SHIELDS UP", (140, 230, 255)))
        if tank.turns % 2 == 1:                          # charging turn: the saucer only repositions - a breather for the player
            self.floaters.append(FloatText(tank.x, tank.y + tank.height + 14, "CHARGING...", (150, 255, 200)))
            return True
        period = 3 if frac > 0.5 else 2
        if (tank.turns // 2) % period == 0:
            tank.volley = 2
            self.floaters.append(FloatText(tank.x, tank.y + tank.height + 14, "VOLLEY!", Palette.DANGER))
        return False

    def _end_turn(self) -> None:
        pos = self.order.index(self.turn)
        for k in range(1, len(self.order) + 1):
            i = self.order[(pos + k) % len(self.order)]
            if self.world.tanks[i].alive:
                self.turn = i
                break
        self.world.drift_wind()
        self._begin_turn()

    def _wind_text(self) -> str:
        w, m = self.world.wind, self.cfg_settings.wind
        if m <= 0:
            return ""
        if abs(w) < 0.05:
            return "WIND CALM"
        n = 1 + int(min(abs(w) / m, 0.999) * 4)
        return f"WIND {'◄' * n if w < 0 else '►' * n} {abs(w):.1f}"

    def _fx_on(self) -> bool:
        return self.app.settings.effects

    def _fire(self, announce: bool = True) -> None:
        w, tank = self.world, self.current
        origin = tank.launch_origin()
        # The whole shot (arcs, bounces, splits, bomblets...) is decided once, on a fixed 20ms grid, so every LAN machine
        # produces the exact same events no matter its frame rate; the screen just animates the planned flights.
        if tank.ammo.key == BLESSING_KEY and not self.session.config.blessing:      # stale selection / forged loadout: never fires
            tank.ammo = DEFAULT_AMMO
        self.blessing_shot = tank.ammo.key == BLESSING_KEY
        self.special_shot = (not self.blessing_shot) and self.session.config.special and tank.charge >= self.session.config.special_wait
        if self.blessing_shot:
            self.floaters.append(FloatText(tank.x, tank.y + tank.height + 8, "MARTYR'S BLESSING!", GOLD))
        if self.special_shot:
            tank.charge = 0
            self.floaters.append(FloatText(tank.x, tank.y + tank.height + 8, "SPECIAL ATTACK!", GOLD))
            if tank.ammo.mechanics:
                self.floaters.append(FloatText(tank.x, tank.y + tank.height + 14, "AMMO EFFECT SKIPPED", Palette.MUTED))
        elif tank.ammo.mechanics and not w.ammo_fx and not tank.is_ai and tank.index not in self._fx_warned:
            self._fx_warned.add(tank.index)
            self.floaters.append(FloatText(tank.x, tank.y + tank.height + 8, "SPECIAL AMMO IS OFF HERE", Palette.MUTED))
        self.flights = [LiveFlight(s, tank.index) for s in plan_shot(w, tank, self.special_shot)]
        self.pending = []
        self.proj = self.flights[0].proj if self.flights else None
        net = self.session.config.net
        if net is not None and announce:
            net.send({"t": "fire", "i": tank.index, "a": tank.angle, "p": tank.power})
        tank.recoil = 0.2
        w.fx.muzzle(origin[0], origin[1], tank.angle, tank.shot_color)
        if self._fx_on():
            self.shake.kick(0.9)
        self.phase, self.t = Phase.FLIGHT, 0.0

    def _advance_flights(self, sdt: float) -> None:
        for f in list(self.flights):
            if f.delay > 0:
                f.delay -= sdt
                continue
            advance_projectile(f.proj, sdt, PHYS, lambda x, y, f=f: self._flight_step(f, x, y))
            if f.proj.t >= f.spec.trace.time:
                self.flights.remove(f)
                self.pending.extend(f.spec.events)

    def _flight_step(self, f: "LiveFlight", x: float, y: float) -> None:
        if math.hypot(x - f.last[0], y - f.last[1]) >= 1.0:
            f.last = (x, y)
            f.n += 1
            for k in range(3 if self.special_shot else 1):
                self.world.fx.trail(x + (k - 1) * 1.5, y + (k - 1), self.current.shot_color, self.current.ammo, f.n + k, self.app.time)

    def _begin_blessing(self, ev: Ev) -> None:
        """A direct hit on an opposing tank: the clock opens and the judgment is planned. Every opposing tank is struck first, then
        nine pillars sweep outward across half the battlefield around the impact, each carving a real crater."""
        w, shooter = self.world, self.current
        self.proj, self.ev, self.flags = None, ev, set()
        self.impact = Impact(ev.x, ev.y, ev.kind, ev.tank)
        self.victims = [t for t in w.tanks if t.alive and t.team != shooter.team]
        self.blessing_shot, self.pending, self.flights = False, [], []
        W, n = w.width, 10
        span = W * 0.55
        c0 = clamp(ev.x - span / 2, 4, max(4, W - 4 - span))
        ground = sorted((c0 + span * (k + 0.5) / n for k in range(n)), key=lambda x: abs(x - ev.x))
        targets = [(v.x, v.y + v.height * 0.5, v.index) for v in self.victims]
        targets += [(x, float(w.terrain.heights[int(clamp(x, 0, W - 1))]), None) for x in ground if abs(x - shooter.x) >= 14]
        wipes = [(x, float(w.terrain.heights[int(x)])) for x in (4 + (W - 9) * k / 11 for k in range(12)) if abs(x - shooter.x) >= 14]
        self.bless = BlessingFX(ev.x, ev.y, targets, full=self._fx_on(), wipes=tuple(wipes))
        self.phase, self.t = Phase.BLESSING, 0.0

    def _bless_kill(self, v: "Tank") -> None:
        if v.hp <= 0 and v in ([self.doomed] + self.doomed_more):
            return
        v.shield = 0
        v.hp = 0
        v.hurt = 0.6
        self.floaters.append(FloatText(v.x, v.y + v.height + 6, "JUDGED", BlessingFX.GOLDC))
        if self.doomed is None:
            self.doomed = v
        elif v is not self.doomed and v not in self.doomed_more:
            self.doomed_more.append(v)

    def _blessing_update(self, sdt: float) -> None:
        w, b, imp = self.world, self.bless, self.impact
        b.t += sdt
        R = PHYS.blast_radius
        for key in b.events():
            if key == "bang":
                w.explosions.append(Explosion(imp.x, imp.y, R * 3.0, BlessingFX.GOLDC, 1.2, "blessing"))
                self.flash = 1.0
                if self._fx_on():
                    self.shake.kick(5.0)
                self.app.feedback.emit("explosion")
            elif key.startswith("hit"):                                      # a pillar lands: real crater, real kill if it is a tank's
                i = int(key[3:])
                if i >= len(b.targets):
                    continue
                tx, ty, ti = b.targets[i]
                w.terrain_op("blast", tx, ty, R * (1.7 if ti is not None else 2.6))
                if not b.skipped:
                    w.explosions.append(Explosion(tx, ty, R * 3.2, BlessingFX.GOLDC if i % 2 == 0 else BlessingFX.VIOLET, 1.1, "blessing"))
                    w.fx.impact(tx, ty, BlessingFX.GOLDC, w.mapdef.theme.soil, 1.8, ammo=self.current.ammo)
                    self.flash = max(self.flash, 0.25 if ti is None else 0.5)
                    if self._fx_on():
                        self.shake.kick(3.0 if ti is None else 5.0)
                if ti is not None:
                    self._bless_kill(w.tanks[ti])
            elif key == "finale":                                            # the nuke at the heart of the clock
                w.terrain_op("blast", imp.x, imp.y, R * 4.2)
                if not b.skipped:
                    w.explosions.append(Explosion(imp.x, imp.y, R * 7.5, (255, 255, 255), 2.0, "blessing"))
                    self.flash = 1.0
                    if self._fx_on():
                        self.shake.kick(11.0)
                    self.app.feedback.emit("explosion")
            elif key.startswith("wipe"):                                     # the firewall front reaches a column: the ground is gone
                i = int(key[4:])
                if i < len(b.wipes):
                    wx_, wy_ = b.wipes[i]
                    w.terrain_op("blast", wx_, wy_, R * 3.4)
                    if not b.skipped:
                        w.explosions.append(Explosion(wx_, wy_, R * 3.4, (255, 150, 60), 1.2, "blessing"))
                        if self._fx_on():
                            self.shake.kick(2.0)
        if b.t >= b.T_END:
            for v in self.victims:                                           # safety net: nobody survives the Blessing
                self._bless_kill(v)
            self.bless = None
            self.phase, self.t = Phase.SETTLE, 0.0

    def _start_event(self, ev: Ev) -> None:
        if self.blessing_shot and ev.first:
            if ev.kind is ImpactKind.TANK and ev.tank is not None and self.world.tanks[ev.tank].team != self.current.team:
                self._begin_blessing(ev)
                return
            self.blessing_shot = False                                       # a miss is just an ordinary shell: the Blessing needs a DIRECT HIT
            if ev.kind is not ImpactKind.OUT:
                self.floaters.append(FloatText(ev.x, ev.y + 8, "THE BLESSING NEEDS A DIRECT HIT", BlessingFX.GOLDC))
        w, shooter = self.world, self.current
        self.proj, self.ev, self.flags = None, ev, set()
        self.impact = Impact(ev.x, ev.y, ev.kind, ev.tank)
        victims: list = []
        reach = PHYS.splash_reach * ev.radius
        if ev.dmg > 0 and ev.kind is ImpactKind.TANK and ev.tank >= 0:
            victims = [w.tanks[ev.tank]]
        elif ev.dmg > 0 and ev.kind is ImpactKind.TERRAIN:
            near = sorted((w.distance_to_tank(t, ev.x, ev.y), t.index) for t in w.tanks if t.alive)
            victims = [w.tanks[i] for d, i in near if d <= reach][:(len(near) if ev.multi else 1)]
        self.victims = victims
        ai = self.ais.get(shooter.index)
        if ev.first and ai and shooter.is_ai and self.plan and self._control(shooter) == "ai":
            foe = self._target_for(shooter)
            ai.observe(self.plan, self.impact, foe, foe in victims)
        if ev.kind is ImpactKind.OUT and ev.mode != "fx":
            self.floaters.append(FloatText(clamp(ev.x, 6, w.width - 6), w.height - 6, "OUT OF BOUNDS", Palette.MUTED))
            self.flags |= {"boom", "burst", "carve", "damage"}
            self.t = 0.6
        else:
            self.t = -ev.delay
        self.phase = Phase.IMPACT

    def _impact_update(self, sdt: float) -> None:
        w, ev, imp = self.world, self.ev, self.impact
        self._advance_flights(sdt)
        self.t += sdt
        if self.t < 0:
            return
        shooter = self.current
        R = PHYS.blast_radius * ev.radius
        if "boom" not in self.flags:
            self.flags.add("boom")
            w.explosions.append(Explosion(imp.x, imp.y, max(2.0, R), shooter.shot_color, style=shooter.ammo.boom))
            if self._fx_on() and ev.radius >= 0.5:
                self.shake.kick(5.0 if ev.big else 1.6 * min(1.4, ev.radius))
            self.app.feedback.emit("explosion")
        if self.t >= 0.06 and "burst" not in self.flags:
            self.flags.add("burst")
            w.fx.impact(imp.x, imp.y, shooter.shot_color, w.mapdef.theme.soil, 2.6 if ev.big else min(1.4, 0.4 + 0.6 * ev.radius), ammo=shooter.ammo)
            if ev.big:                                               # the special: two more rings of the ammo's own blast
                for k, sc in enumerate((1.5, 2.1)):
                    w.explosions.append(Explosion(imp.x, imp.y, R * sc, mix(shooter.shot_color, (255, 255, 255), 0.3 * k), style=shooter.ammo.boom))
        if self.t >= 0.28 and "carve" not in self.flags:
            self.flags.add("carve")
            if ev.mode != "fx":
                w.terrain_op(ev.mode, imp.x, imp.y, R * (0.85 if imp.kind is ImpactKind.TANK else 1.0))
            self.guide_key = None
        if self.t >= 0.42 and "damage" not in self.flags:
            self.flags.add("damage")
            direct = imp.kind is ImpactKind.TANK
            for v in self.victims:
                dmg = ev.dmg * (2 if (direct and v.scale > 1.0) else 1) * self.session.dmg_mult(shooter)   # saucers: direct hits double
                if v.shield > 0:
                    v.shield -= 1
                    dmg = 0
                v.hp -= dmg
                v.hurt = 0.6
                txt = ("SHIELDED - KILL THE DRONES" if v.guard else "SHIELD ABSORBED") if dmg == 0 else (f"DIRECT HIT -{dmg}" if direct else f"HIT -{dmg}")
                self.floaters.append(FloatText(v.x, v.y + v.height + 6, txt, Palette.AMBER if dmg == 0 else Palette.DANGER))
                if self._fx_on():
                    self.shake.kick(2.6 if dmg else 1.0)
                if v.hp <= 0:
                    if self.doomed is None:
                        self.doomed = v
                    elif v is not self.doomed and v not in self.doomed_more:
                        self.doomed_more.append(v)
            if ev.pull > 0:                                                     # gravity well: drag tanks toward the blast
                for t in w.tanks:
                    dx = imp.x - t.x
                    if t.alive and t.kind != "ufo" and abs(dx) <= ev.pull * 2.2:
                        t.x = clamp(t.x + clamp(dx * 0.6, -ev.pull, ev.pull), 9, w.width - 10) + 0.0
                        t.y = max(t.y, float(w.terrain.support_height(t.x)) + t.hover)
        end_t = 1.0 if not (self.flights or self.pending) else 0.5
        if self.t >= end_t:
            if self.pending:
                self._start_event(self.pending.pop(0))
            elif self.flights:
                self.phase, self.t = Phase.FLIGHT, 0.0
            else:
                self.phase, self.t = Phase.SETTLE, 0.0

    def _destruction_update(self, sdt: float) -> None:
        w, d = self.world, self.doomed
        self.t += sdt
        if "start" not in self.flags:
            self.flags.add("start")
            self.wiped = not any(t.alive for t in w.tanks if t.team == d.team)   # round ends only when a whole team is down
            if self.wiped:
                self.session.record_round(1 - d.team)
                self.session.commit(self.app)
        if self.t < 0.8:
            d.hurt = 0.6
        if self.t >= 0.8 and "boom" not in self.flags:
            self.flags.add("boom")
            d.destroyed = True
            for o in self.doomed_more:
                o.destroyed = True
            d.hurt = 0.0
            w.explosions.append(Explosion(d.x, d.y + 3, 12, d.shot_color, 1.3, self.current.ammo.boom))
            w.fx.tank_debris(d.x, d.y + 3, d.color)
            w.fx.impact(d.x, d.y + 3, d.shot_color, w.mapdef.theme.soil, 1.8, self.current.ammo)
            self.flash = 1.0
            if self._fx_on():
                self.shake.kick(4.5)
            self.app.feedback.emit("destroy")
        if self.t >= 1.6 and "banner" not in self.flags:
            self.flags.add("banner")
            self.banner = Banner("TARGET DESTROYED", d.name + " IS OUT", d.color, 2.0)
        if self.wiped and self.t >= 3.4 and not self.finished:
            self.finished = True
            self.app.push(RoundResultScene(self.app, self))
        elif not self.wiped and self.t >= 2.4:
            if self.doomed_more:                           # another tank fell in the same volley
                self.doomed, self.flags, self.t = self.doomed_more.pop(0), set(), 0.0
            else:
                self.doomed, self.flags = None, set()      # a teammate fell but the team fights on
                self._end_turn()

    # -- input ---------------------------------------------------------------
    def handle(self, ev: InputEvent) -> None:
        if ev.action in (Action.BACK, Action.PAUSE):
            self.app.push(PauseScene(self.app, self))
            return
        if self.phase is Phase.BLESSING:
            if self.bless is not None and (ev.confirm or ev.action in (Action.CONFIRM, Action.FIRE)):
                self.bless.skip()                                # the kill still lands; only the spectacle is cut short
            return
        if self.phase is not Phase.AIMING or self._control(self.current) != "local":
            return
        t, step = self.current, COARSE_STEP if ev.coarse else 1
        if ev.raw in ("x", "X") and self.session.config.special:
            self._forfeit(t)
        elif ev.action is Action.LEFT:
            t.angle = clamp(t.angle + step, 2, 178)
        elif ev.action is Action.RIGHT:
            t.angle = clamp(t.angle - step, 2, 178)
        elif ev.action is Action.UP:
            t.power = clamp(t.power + step, PHYS.min_power, PHYS.max_power)
        elif ev.action is Action.DOWN:
            t.power = clamp(t.power - step, PHYS.min_power, PHYS.max_power)
        elif ev.confirm:
            self._fire()

    def _forfeit(self, tank: Tank, announce: bool = True) -> None:
        """Special attack charging: give up this turn to bank one charge (two charges make your next shot the special)."""
        need = self.session.config.special_wait
        if tank.charge >= need:
            self.floaters.append(FloatText(tank.x, tank.y + tank.height + 8, "SPECIAL ALREADY READY - FIRE!", Palette.AMBER))
            return
        tank.charge += 1
        self.floaters.append(FloatText(tank.x, tank.y + tank.height + 8, "SPECIAL READY!" if tank.charge >= need else f"CHARGING {tank.charge}/{need}", GOLD))
        net = self.session.config.net
        if net is not None and announce:
            net.send({"t": "skip", "i": tank.index})
        self._end_turn()

    # -- LAN -------------------------------------------------------------------
    def _net_tick(self, dt: float) -> None:
        net = self.session.config.net
        if net is None or self.world is None:
            return
        net.pump()
        if net.take("quit"):
            net.close()
            self.app.goto(NetNoticeScene(self.app, "OPPONENT LEFT", "The other player left the match."))
            return
        if net.dead:
            self.app.goto(NetNoticeScene(self.app, "CONNECTION LOST", "The other player disconnected."))
            return
        cur = self.current
        ctl = self._control(cur)
        if self.phase is Phase.AIMING and ctl == "local" or self.phase is Phase.AI_THINKING:
            self._aim_timer -= dt
            key = (round(cur.angle, 2), round(cur.power, 2))
            if self._aim_timer <= 0 and key != self._aim_sent:
                self._aim_timer, self._aim_sent = 0.08, key
                net.send({"t": "aim", "i": cur.index, "a": cur.angle, "p": cur.power})
        if self.phase is Phase.AIMING and ctl == "remote":
            while True:
                m = net.peek("aim", "fire", "skip")
                if m is None:
                    break
                if m.get("i") != cur.index:
                    if m["t"] == "aim":                   # a stale aim from an earlier turn: discard it
                        net.drop(m)
                        continue
                    break
                net.drop(m)
                if m["t"] == "skip":
                    self._forfeit(cur, announce=False)
                    break
                cur.angle = clamp(float(m["a"]), 2, 178)
                cur.power = clamp(float(m["p"]), PHYS.min_power, PHYS.max_power)
                if m["t"] == "fire":
                    self._fire(announce=False)
                    break

    # -- update --------------------------------------------------------------
    def update_ambient(self, dt: float) -> None:
        if self.world:
            self.world.update_effects(dt)
            self._tick_visuals(dt)

    def _tick_visuals(self, dt: float) -> None:
        self.shake.update(dt)
        self.flash = max(0.0, self.flash - dt * 2.2)
        for f in self.floaters:
            f.age += dt
        self.floaters = [f for f in self.floaters if f.age < f.life]
        if self.banner:
            self.banner.age += dt
            if self.banner.age >= self.banner.life:
                self.banner = None

    def _refresh_guards(self) -> None:
        """A guarded tank (the Mothership) keeps its shield up while any teammate lives; it drops for good once they are all down."""
        for t in self.world.tanks:
            if t.guard and t.alive:
                if any(o.alive and o is not t and o.team == t.team for o in self.world.tanks):
                    t.shield = max(t.shield, 2)
                else:
                    t.guard, t.shield = False, 0
                    self.floaters.append(FloatText(t.x, t.y + t.height + 8, "SHIELD DOWN!", (140, 230, 255)))

    def update(self, dt: float) -> None:
        self._check_resize()
        self._net_tick(dt)
        if self.world is not None:
            self._refresh_guards()
        sdt = dt * self.app.settings.anim_speed
        w = self.world
        w.update_effects(sdt)
        self._tick_visuals(dt)
        ph = self.phase
        if ph is Phase.INTRO:
            self.t += dt
            if self.t >= self.INTRO_TIME:
                self._begin_turn()
        elif ph is Phase.AI_THINKING:
            self.t += sdt
            p = self.t / self.plan.think_time
            tank = self.current
            k = ease_in_out((p - 0.3) / 0.6)
            tank.angle = lerp(self.plan_from[0], self.plan.angle, k)
            tank.power = lerp(self.plan_from[1], self.plan.power, k)
            if p >= 1:
                tank.angle, tank.power = self.plan.angle, self.plan.power
                self._fire()
        elif ph is Phase.FLIGHT:
            self._advance_flights(sdt)
            if self.pending:
                self._start_event(self.pending.pop(0))
        elif ph is Phase.IMPACT:
            self._impact_update(sdt)
        elif ph is Phase.SETTLE:
            self.t += sdt
            moving = w.settle(sdt)
            if self.t > 0.25 and not moving:
                if self.doomed:
                    pool = [self.doomed] + self.doomed_more          # an enemy goes down first if both sides lost a tank
                    foe_first = [v for v in pool if v.team != self.current.team]
                    self.doomed = (foe_first or pool)[0]
                    self.doomed_more = [v for v in pool if v is not self.doomed]
                    self.phase, self.t, self.flags = Phase.DESTRUCTION, 0.0, set()
                elif self.current.alive and self.current.volley > 0:
                    self.current.volley -= 1
                    self._begin_turn(volley=True)
                else:
                    self._end_turn()
        elif ph is Phase.DESTRUCTION:
            self._destruction_update(sdt)
        elif ph is Phase.BLESSING:
            self._blessing_update(sdt)

    # -- drawing -------------------------------------------------------------
    def _cell_of(self, x: float, y: float) -> tuple[int, int]:
        L = self.layout
        return L.fx + int(x), L.fy + (self.world.height - 1 - int(y)) // 2

    def draw(self, screen: Screen) -> None:
        if self.world is None:
            return
        L, w = self.layout, self.world
        self._off = []
        canvas = w.scenery.canvas()
        w.scenery.draw_stars(canvas, self.app.time)
        w.scenery.draw_aurora(canvas, self.app.time)
        w.scenery.draw_fx(canvas, self.app.time, self._fx_on())
        w.draw_wind(canvas)
        cur = self.current
        if self.phase is Phase.AIMING and self.app.settings.aim_guide:
            key = (cur.angle, cur.power, w.terrain.version, cur.x, cur.y)
            if key != self.guide_key:
                self.guide_key = key
                launch = make_ammo_launch(w, cur, cur.angle, cur.power, wind=0.0)
                self.guide = w.physics.trace(launch, w.owner_probe(cur.index), 0.02, 2.6, 8.0).points
            pts = self.guide[:10] if self.app.settings.aim_guide == 1 else self.guide
            for i, (x, y) in enumerate(pts):
                canvas.blendf(x, y, cur.shot_color, 0.85 * (1 - i / (len(pts) + 4)))
        for t in w.tanks:
            flash = (t.hurt / 0.6) * (0.4 + 0.4 * math.sin(self.app.time * 40)) if t.hurt > 0 else 0.0
            if t.kind == "ufo":
                draw_ufo(canvas, t, self.app.time, max(0.0, flash))
                continue
            draw_tank(canvas, t.x, t.y, t.facing, t.angle, t.hull_sentinel if (t.rgb_hull and t.hull_sentinel in PIXEL_PAINTS) else t.color, flash=max(0.0, flash), wreck=t.destroyed, recoil=t.recoil,
                      design=t.design, t=self.app.time, lift=0.0, hover=t.hover)
        w.particles.draw(canvas)
        for f in self.flights:
            if f.delay > 0 or f.spec.trace.time <= 0:
                continue
            x, y = f.proj.x, f.proj.y
            col = self.current.shot_color
            if y >= w.height:
                pr, lc = f.proj, f.proj.launch
                draw_projectile(canvas, x, w.height - 5, lc.vx, 0.0, col, self.current.ammo, self.app.time)     # the shell, kept on screen
                self._off.append((x, y - w.height))
            else:
                pr, lc = f.proj, f.proj.launch
                vx, vy = lc.vx + lc.ax * pr.t, lc.vy - lc.g * pr.t
                if self.blessing_shot:                     # temporal invocation: gold/violet halo and orbiting stars on the shell
                    tt = self.app.time
                    for k in range(30):
                        a = k / 30 * math.tau - tt * 5
                        rr = 4.5 + 1.5 * math.sin(tt * 8 + k)
                        canvas.blendf(x + math.cos(a) * rr, y + math.sin(a) * rr, (255, 214, 90) if k % 2 else (176, 120, 255), 0.7)
                    for k in range(6):
                        a = tt * 6 + k * math.tau / 6
                        canvas.blendf(x + math.cos(a) * 2.6, y + math.sin(a) * 2.6, (255, 255, 255), 0.9)
                if self.special_shot:                      # scaled-up: a pulsing halo and three orbiting copies of the ammo's shell
                    tt, sp = self.app.time, math.hypot(vx, vy) or 1.0
                    for k in range(26):
                        a = k / 26 * math.tau + tt * 6
                        canvas.blendf(x + math.cos(a) * (5 + math.sin(tt * 9)), y + math.sin(a) * (5 + math.sin(tt * 9)), col, 0.55)
                    for k in range(3):
                        a = tt * 7 + k * math.tau / 3
                        draw_projectile(canvas, x + math.cos(a) * 3.2, y + math.sin(a) * 3.2, vx, vy, col, self.current.ammo, tt)
                draw_projectile(canvas, x, y, vx, vy, col, self.current.ammo, self.app.time)
        for e in w.explosions:
            e.draw(canvas)
        screen.blit(canvas, L.fx, L.fy, self.shake.offset(w.rng, self._fx_on()))
        for ox, oh in self._off:                                                  # off-screen marker: how far above the field it is
            cx = clamp(L.fx + int(ox), L.fx + 3, L.fx + L.fcols - 8)
            screen.put(cx, L.fy, "▲", Palette.WHITE)
            screen.text(cx + 1, L.fy, f"{int(oh)}", Palette.WHITE)
        if self.flash > 0 and self._fx_on():
            screen.tint(L.fx, L.fy, L.fcols, L.frows, (255, 255, 255), self.flash * 0.45)
        if self.phase is Phase.BLESSING and self.bless is not None:
            self.bless.draw(screen, L, self._cell_of)
        self._draw_labels(screen)
        self._draw_overlays(screen)
        self._draw_hud(screen)

    def _draw_labels(self, screen: Screen) -> None:
        L, w = self.layout, self.world
        for t in w.tanks:
            if t.destroyed:
                continue
            cx, cy = self._cell_of(t.x, t.y + t.height + 8)
            cy = max(L.fy, cy)
            text = f"{t.label} {hearts(t.hp, False, t.max_hp)}"
            screen.text(cx - len(text) // 2, cy, text, t.color if t.hp > 0 else Palette.MUTED)
            if t is self.current and self.phase in (Phase.AIMING, Phase.AI_THINKING) and cy > L.fy:
                bob = int(self.app.time * 3) % 2
                screen.put(cx, cy - 1 - bob + (1 if cy - 1 - bob < L.fy else 0), "▼", Palette.WHITE)

    def _draw_overlays(self, screen: Screen) -> None:
        L = self.layout
        for f in self.floaters:
            x, y = self._cell_of(f.x, f.y + f.age * 6)
            a = 1 - f.age / f.life
            s = f.text
            screen.text(x - len(s) // 2, max(L.fy, y), s, mix(Palette.BG, f.color, clamp(a * 2, 0, 1)))
        b = self.banner
        if b:
            a = clamp(min(b.age * 6, (b.life - b.age) * 4), 0, 1)
            y = L.fy + L.frows // 3
            screen.tint(L.fx, y - 1, L.fcols, 4, (0, 0, 0), 0.6 * a)
            spaced = " ".join(b.text)
            screen.center(y, spaced, mix(Palette.BG, b.color, a), None, L.fx, L.fcols)
            if b.sub:
                screen.center(y + 2, b.sub, mix(Palette.BG, Palette.TEXT, a), None, L.fx, L.fcols)

    def _state_text(self) -> str:
        p = self.phase
        dots = "." * (int(self.app.time * 3) % 4)
        if p is Phase.INTRO:
            return "DEPLOYING"
        if p is Phase.AIMING:
            return "AIMING"
        if p is Phase.AI_THINKING:
            return "AI THINKING" + dots
        if p is Phase.FLIGHT:
            return "PROJECTILE AWAY"
        if p is Phase.IMPACT:
            return "IMPACT"
        if p is Phase.SETTLE:
            return "TERRAIN SETTLING"
        if p is Phase.BLESSING:
            return "THE MARTYR'S BLESSING"
        return "TARGET DESTROYED"

    def _draw_hud(self, screen: Screen) -> None:
        L, s = self.layout, self.session
        top, cols = L.top, L.cols
        screen.fill(0, top, cols, 2, Palette.PANEL)
        pl = self.world.tanks
        teams = sorted({t.team for t in pl})[:2]
        for side, team in enumerate(teams):
            everyone = [t for t in pl if t.team == team]
            members = everyone[:2]
            wins_text = s.hud_wins(team)
            for r, t in enumerate(members):
                row, nm = top + (r if len(members) > 1 else 0), t.name[:12]
                if side == 0:
                    screen.text(1, row, "▌", t.color, Palette.PANEL)
                    screen.text(3, row, nm, t.color, Palette.PANEL)
                    wd = draw_hp(screen, 4 + len(nm), row, t, Palette.PANEL)
                    if s.config.special:
                        screen.text(5 + len(nm) + wd, row, "✦" * t.charge + "·" * (s.config.special_wait - t.charge), GOLD if t.charge >= s.config.special_wait else Palette.MUTED, Palette.PANEL)
                else:
                    x = cols - 2
                    screen.text(x, row, "▐", t.color, Palette.PANEL)
                    screen.text(x - 1 - len(nm), row, nm, t.color, Palette.PANEL)
                    wd = draw_hp(screen, x - 3 - len(nm), row, t, Palette.PANEL, True)
                    if s.config.special:
                        screen.text(x - 5 - len(nm) - wd, row, "✦" * t.charge + "·" * (s.config.special_wait - t.charge), GOLD if t.charge >= s.config.special_wait else Palette.MUTED, Palette.PANEL)
            if len(members) == 1:
                if side == 0:
                    screen.text(3, top + 1, wins_text, Palette.MUTED, Palette.PANEL)
                else:
                    screen.text(cols - 2 - len(wins_text), top + 1, wins_text, Palette.MUTED, Palette.PANEL)
            else:
                pass
        screen.center(top, s.hud_title(), Palette.TEXT, Palette.PANEL)
        cur = self.current
        badge = f" {self._turn_text(self.turn)} "
        state = self._state_text()
        total = len(badge) + 2 + len(state)
        bx = (cols - total) // 2
        if self.phase is Phase.DESTRUCTION:
            screen.center(top + 1, state, Palette.DANGER, Palette.PANEL)
        else:
            screen.text(bx, top + 1, badge, Palette.INK, cur.color)
            screen.text(bx + len(badge) + 2, top + 1, state, Palette.AMBER, Palette.PANEL)
        screen.fill(0, top + 2, cols, 1, Palette.BG, "─", mix(Palette.LINE, cur.color, 0.5))
        wt = wallet_text(self.app.save)                  # the wallet rides on the HUD divider, always visible in battle
        screen.text(cols - len(wt) - 3, top + 2, f" {wt} ", Palette.TEXT, Palette.BG)
        for side, team in enumerate(teams):
            if len([t for t in pl if t.team == team]) > 1:
                wtx = s.hud_wins(team)
                extra = len([t for t in pl if t.team == team]) - 2
                if extra > 0:
                    wtx += f"  +{extra} MORE"
                ww = len(wallet_text(self.app.save)) + 6
                screen.text(1 if side == 0 else cols - ww - len(wtx), top + 2, wtx, Palette.MUTED, Palette.BG)
        by = L.fy + L.frows
        screen.fill(0, by, cols, 3, Palette.PANEL)
        screen.fill(0, by, cols, 1, Palette.BG, "─", mix(Palette.LINE, cur.color, 0.5))
        x = 2
        rel = cur.rel_angle
        screen.text(x, by + 1, "ANGLE", Palette.MUTED, Palette.PANEL)
        screen.text(x + 6, by + 1, f"{rel:>3.0f}°", cur.color, Palette.PANEL)
        gx, gw = x + 12, 25
        screen.put(gx, by + 1, "├", Palette.LINE, Palette.PANEL)
        for i in range(gw):
            screen.put(gx + 1 + i, by + 1, "─", (52, 70, 82), Palette.PANEL)
        marker = int((180 - cur.angle) / 180 * (gw - 1))
        screen.put(gx + 1 + marker, by + 1, "◆", cur.color, Palette.PANEL)
        screen.put(gx + gw + 1, by + 1, "┤", Palette.LINE, Palette.PANEL)
        px = gx + gw + 5
        screen.text(px, by + 1, "POWER", Palette.MUTED, Palette.PANEL)
        draw_bar(screen, px + 6, by + 1, 20, (cur.power - PHYS.min_power) / (PHYS.max_power - PHYS.min_power))
        screen.text(px + 27, by + 1, f"{cur.power:>3.0f}%", cur.color, Palette.PANEL)
        sx = px + 34
        if sx + 8 < cols:
            screen.text(sx, by + 1, "SHELL", Palette.MUTED, Palette.PANEL)
            screen.text(sx + 6, by + 1, "●●", cur.shot_color, Palette.PANEL)
        if self.phase is Phase.AI_THINKING:
            screen.text(2, by + 2, "AI FIRE CONTROL: " + self.plan.status, Palette.AMBER, Palette.PANEL)
        else:
            caps = ([("◄►", "AIM"), ("▲▼", "POWER"), ("SHIFT", "COARSE"), ("SPACE", "FIRE")] if isinstance(self, PracticeScene)
                    else [("A/D", "AIM"), ("W/S", "POWER"), ("SHIFT", "COARSE"), ("SPACE", "FIRE")])
            if self.session.config.special:
                caps.append(("X", "CHARGE"))
            draw_keycaps(screen, 2, by + 2, tuple(caps + [("ESC", "MENU")]), Palette.PANEL)
        wmax, wind = self.cfg_settings.wind, self.world.wind
        if wmax <= 0:
            wt, wc = "WIND OFF", Palette.MUTED
        elif abs(wind) < 0.05:
            wt, wc = "WIND CALM", Palette.TEXT
        else:
            wt = self._wind_text()
            wc = gradient(((0, Palette.OK), (0.6, Palette.AMBER), (1, Palette.DANGER)), abs(wind) / wmax)
        screen.text(cols - 2 - len(wt), by + 2, wt, wc, Palette.PANEL)


class ConfirmLeaveScene(Scene):
    """'Leave this match?' - makes the consequence clear before anything is closed or discarded."""
    overlay = True
    show_wallet = False

    def __init__(self, app, session: "MatchSession") -> None:
        super().__init__(app)
        self.session = session
        self.net = session.config.net

    def handle(self, ev: InputEvent) -> None:
        if ev.action is Action.BACK or ev.raw in ("n", "N"):
            self.app.pop()
        elif ev.confirm or ev.raw in ("y", "Y"):
            if self.net is not None:                           # tell the other player, then release the socket
                self.net.send({"t": "quit"})
                self.net.close()
            self.app.save.flush()
            self.app.goto(self.session.exit_scene(self.app))

    def draw(self, screen: Screen) -> None:
        screen.dim(0.55)
        s = self.session
        x, y = panel(screen, 52, 10, "LEAVE THE MATCH?", fg=Palette.DANGER)
        line = ("The other player will be told you left." if self.net else "This match will end and its progress is lost.")
        screen.center(y + 2, line, Palette.TEXT, Palette.PANEL, x, 52)
        screen.center(y + 3, f"You return to {s.exit_label if s.exit_label != 'MENU' else 'the menu'}.", Palette.MUTED, Palette.PANEL, x, 52)
        screen.center(y + 6, "[ENTER] LEAVE      [ESC] KEEP PLAYING", Palette.AMBER, Palette.PANEL, x, 52)


class PauseScene(Scene):
    overlay = True
    show_wallet = False

    def __init__(self, app, battle: BattleScene) -> None:
        super().__init__(app)
        self.battle = battle
        self.net = battle.session.config.net
        self.menu = MenuList(["RESUME", "EXIT MATCH"] if self.net else ["RESUME", "RESTART", "EXIT MATCH"])

    def handle(self, ev: InputEvent) -> None:
        if ev.action in (Action.BACK, Action.PAUSE):
            self.app.pop()
            return
        c = self.menu.handle(ev)
        if c is None:
            return
        label = self.menu.items[c]
        if label == "RESUME":
            self.app.pop()
        elif label == "RESTART":
            self.battle.session.restart(self.app)
        else:
            self.app.push(ConfirmLeaveScene(self.app, self.battle.session))

    def draw(self, screen: Screen) -> None:
        screen.dim(0.45)
        x, y = panel(screen, 34, 9, "MATCH MENU")
        self.menu.draw(screen, screen.cols // 2, y + 3, self.app.time, gap=1, width=24)
        if self.net:
            screen.center(y + 7, "The match keeps running for the other player.", Palette.MUTED, Palette.PANEL, x, 34)


# ---------------------------------------------------------------------------
# Interactive tutorial: a practice range (one passive, unbreakable target) + coaching panel. Reached from HOW TO PLAY (T).
# Lessons 1-4 are live practice, 5 is a rules briefing built from the game's real level/mission text, 6 is optional challenges.
# Nothing here touches the wallet, unlocks or the Tournament; the only thing saved is a "tut:N" tick in the story list.
# ---------------------------------------------------------------------------
TUT_LESSONS = (
    ("1  AIMING & POWER", "Rotate the barrel with ◄ ► (hold SHIFT for big steps), set the power with ▲ ▼, fire with SPACE or ENTER. A higher angle lobs the shell; more power sends it farther.",
     "HIT THE TARGET ONCE", "Too short? add power. Too long? lower it. Change one thing at a time."),
    ("2  WIND", "Wind pushes every shell and changes each turn. The HUD arrows show its direction and strength; the dotted aim guide ignores it.",
     "FIRE 3 SHOTS AND LAND ONE HIT", "Fire the same aim twice and compare. Aim INTO the wind to compensate."),
    ("3  AMMUNITION", "Ammo types change how a shell flies and bursts. PEBBLES makes a small blast, then three bomblets. Marked ✦ in pilot setup = real effects.",
     "LAND A HIT WITH PEBBLES", "Effects are live in Single/Two Player and LAN (SPECIAL AMMO rule); Tournament and Missions keep ammo cosmetic."),
    ("4  TERRAIN", "Every blast digs a crater and the ground falls away. Dig a lane through a hill, or undermine a tank so it drops.",
     "DIG 2 CRATERS, THEN HIT THE TARGET", "Craters change future shots: a shell now rolls into the hole. Some ammo (MUDBALL, BORER) builds or drills instead."),
)
TUT_CHALLENGES = (                 # (name, ammo, wind, shots-in-a-row needed)
    ("GALE SNIPER: 3 hits in a row in a strong wind", "standard", 10.0, 3),
    ("WIND-PROOF: SLUG ammo, 3 hits in a row", "slug", 10.0, 3),
    ("LOB SHOT: floaty LOB ammo, 2 hits in a row", "lob", 6.0, 2),
)


class PracticeSession(MatchSession):
    exit_label = "THE TUTORIAL"

    def __init__(self, config: MatchConfig, mapdef, seed, lesson: int, adv: Optional[int]) -> None:
        super().__init__(config, mapdef, seed)
        self.lesson, self.adv = lesson, adv

    def exit_scene(self, app: "Application") -> "Scene":
        return TutorialScene(app, adv=self.adv is not None)

    def restart(self, app: "Application") -> None:
        app.goto(PracticeScene(app, self.lesson, self.adv))


class PracticeScene(BattleScene):
    """Practice range. The target never fires, never dies and nobody earns anything; the coach panel explains each lesson."""

    def __init__(self, app, lesson: int, adv: Optional[int] = None) -> None:
        lo = app.save.loadout(0)
        ammo = lo.ammo if lo.ammo != BLESSING_KEY else DEFAULT_AMMO.key
        wind = 0.0
        if adv is not None:
            ammo, wind = TUT_CHALLENGES[adv][1], TUT_CHALLENGES[adv][2]
        elif lesson == 2:
            wind = 8.0
        elif lesson == 3:
            ammo = "pebbles"
        elif lesson != 1 and lesson != 4:
            ammo = DEFAULT_AMMO.key
        if adv is None and lesson in (1, 2, 4):
            ammo = DEFAULT_AMMO.key
        me = PlayerSetup("PLAYER", "YOU", COLOR_BY_NAME[lo.tank_color], COLOR_BY_NAME[lo.shot_color], False, "ranger" if lo.tank not in app.save.unlocked_tanks() else lo.tank,
                         ammo, 9)
        tgt = PlayerSetup("TARGET", "TGT", COLOR_BY_NAME["ORANGE"], COLOR_BY_NAME["PINK"], True, "ranger", DEFAULT_AMMO.key, 9)
        cfg = MatchConfig([me, tgt], True, Difficulty.EASY, "single", target=99, ammo_fx=True, blessing=False)
        sess = PracticeSession(cfg, MAPS[0], app.rng.randrange(1 << 30), lesson, adv)
        sess.wind_override = wind
        super().__init__(app, sess)
        self.lesson, self.adv = lesson, adv
        self.shots = self.hits = self.streak = self.craters = 0
        self.note, self.first_x, self.coach, self.done = "FIRE WHEN READY", None, True, False
        self._ver = None

    # --- turn flow: the player always shoots again; the target is restored after every shot ---
    def _begin_turn(self, volley: bool = False) -> None:
        self.turn = 0
        super()._begin_turn(volley)

    def _start_event(self, ev: Ev) -> None:
        if ev.first and ev.kind is not ImpactKind.OUT:
            self.first_x = ev.x
        super()._start_event(ev)

    def _end_turn(self) -> None:
        w = self.world
        me, tgt = w.tanks[0], w.tanks[1]
        hit = tgt.hp < tgt.max_hp
        self.shots += 1
        self.hits += int(hit)
        self.streak = self.streak + 1 if hit else 0
        ver = w.terrain.version
        if self._ver is not None and ver != self._ver:
            self.craters += 1
        self._ver = ver
        if hit:
            self.note = "DIRECT HIT!"
        elif self.first_x is None:
            self.note = "OUT OF BOUNDS - try less power"
        else:
            err = (self.first_x - tgt.x) * (1 if tgt.x >= me.x else -1)
            self.note = f"{'LONG' if err > 0 else 'SHORT'} by {abs(err):.0f} - " + ("lower the power or angle" if err > 0 else "add power")
        self.first_x = None
        me.hp, tgt.hp = me.max_hp, tgt.max_hp
        tgt.charge = me.charge = 0
        self.doomed, self.doomed_more = None, []
        w.drift_wind()
        self._begin_turn()

    # --- goals ---
    def _goal_met(self) -> bool:
        if self.adv is not None:
            return self.streak >= TUT_CHALLENGES[self.adv][3]
        return {1: self.hits >= 1, 2: self.shots >= 3 and self.hits >= 1, 3: self.hits >= 1, 4: self.craters >= 2 and self.hits >= 1}[self.lesson]

    def update(self, dt: float) -> None:
        super().update(dt)
        if not self.done and self._goal_met():
            self.done = True
            self.app.save.hear(f"tut:{self.lesson if self.adv is None else 6}")

    def handle(self, ev: InputEvent) -> None:
        if self.phase is Phase.AIMING and ev.raw in ("t", "T"):
            self.coach = not self.coach
        elif self.phase is Phase.AIMING and self.done and ev.raw in ("n", "N"):
            if self.adv is not None or self.lesson >= 4:
                self.app.goto(TutorialScene(self.app, adv=self.adv is not None))
            else:
                self.app.goto(PracticeScene(self.app, self.lesson + 1))
        else:
            super().handle(ev)

    def draw(self, screen: Screen) -> None:
        super().draw(screen)
        if not self.coach or self.layout is None:
            return
        L = self.layout
        if self.adv is not None:
            name, goal, tip = "CHALLENGE", TUT_CHALLENGES[self.adv][0].upper(), "Streaks reset on a miss. Wind shifts every turn."
            body, prog = "Practice what you learned. No rewards - just bragging rights.", f"STREAK {self.streak}/{TUT_CHALLENGES[self.adv][3]}"
        else:
            name, body, goal, tip = TUT_LESSONS[self.lesson - 1]
            prog = f"SHOTS {self.shots}  HITS {self.hits}" + (f"  CRATERS {self.craters}" if self.lesson == 4 else "")
        w = min(L.fcols - 2, 74)
        lines = textwrap.wrap(body, w - 4)[:3] + ["GOAL: " + goal + "   " + prog, ("DONE!  [N] " + ("BACK TO LESSONS" if self.adv is not None or self.lesson >= 4 else "NEXT LESSON")
                                                                              + "  ·  keep practising if you like") if self.done
                                                else self.note + "   ·   " + tip[:max(0, w - len(self.note) - 9)]]
        h = len(lines) + 2
        x, y = L.fx + 1, L.fy + 1
        screen.fill(x, y, w, h, Palette.PANEL)
        screen.text(x + 2, y, f" TUTORIAL · {name} ", GOLD, Palette.PANEL)
        for k, ln in enumerate(lines):
            screen.text(x + 2, y + 1 + k, ln[:w - 3], (Palette.OK if self.done and k == len(lines) - 1 else Palette.WHITE if k < len(lines) - 2 else GOLD if k == len(lines) - 2 else Palette.AMBER), Palette.PANEL)
        screen.text(x + w - 22, y + h - 1, " [T] HIDE  [ESC] MENU ", Palette.MUTED, Palette.PANEL)


class TutorialScene(BackdropScene):
    """Lesson picker. ESC always returns to HOW TO PLAY / the main menu - nothing here is mandatory."""

    def __init__(self, app, adv: bool = False) -> None:
        super().__init__(app)
        self.adv, self.sel, self.page5 = adv, 0, False

    def _items(self) -> list:
        if self.adv:
            return [c[0] for c in TUT_CHALLENGES] + ["BACK"]
        return [l[0] for l in TUT_LESSONS] + ["5  BOSS MECHANICS", "6  ADVANCED PRACTICE (OPTIONAL)", "BACK"]

    def handle(self, ev: InputEvent) -> None:
        items = self._items()
        if self.page5:
            if ev.confirm or ev.action is Action.BACK:
                self.page5 = False
                self.app.save.hear("tut:5")
            return
        if ev.action is Action.BACK:
            self.app.goto(TutorialScene(self.app) if self.adv else MenuScene(self.app))
        elif ev.action is Action.UP:
            self.sel = (self.sel - 1) % len(items)
        elif ev.action is Action.DOWN:
            self.sel = (self.sel + 1) % len(items)
        elif ev.confirm or ev.action in (Action.CONFIRM, Action.FIRE):
            lab = items[self.sel]
            if lab == "BACK":
                self.app.goto(TutorialScene(self.app) if self.adv else MenuScene(self.app))
            elif self.adv:
                self.app.goto(PracticeScene(self.app, 6, self.sel))
            elif self.sel < 4:
                self.app.goto(PracticeScene(self.app, self.sel + 1))
            elif self.sel == 4:
                self.page5 = True
            else:
                self.app.goto(TutorialScene(self.app, adv=True))

    def draw(self, screen: Screen) -> None:
        super().draw(screen)
        sv = self.app.save
        if self.page5:
            x, y = panel(screen, 84, 24, "TUTORIAL  ·  5  BOSS MECHANICS")
            lines = [("Some opponents break the normal rules. Read what the game tells you:", Palette.TEXT),
                     ("• The ROUND BANNER names the fight and any special rule before you fire.", Palette.TEXT),
                     ("• Floating messages explain events: SHIELD ABSORBED, SHIELDS UP, VOLLEY!, CHARGING...", Palette.TEXT),
                     ("• Saucers hop between turns, raise shields as they are hurt and fire 3-shot volleys; direct hits count double.", Palette.TEXT),
                     ("TOURNAMENT RULE TAGS (real examples from the level cards):", Palette.AMBER)]
            seen: set = set()
            for lv in TOURNAMENT:
                if lv.rule not in seen and lv.rule:
                    seen.add(lv.rule)
                    lines.append((f"  {lv.rule_tag:<13} {lv.rule_text}", GOLD))
            lines += [("  STANDARD      " + TOURNAMENT[1].rule_text, GOLD),
                      ("EXERCISE: open TOURNAMENT and read the rule line on any level card before you start it.", Palette.AMBER),
                      ("Boss kits are the kit you unlock by winning. Later bosses add their own mechanics - the banner always tells you.", Palette.TEXT)]
            row = y + 2
            for t_, c in lines:
                for ln in textwrap.wrap(t_, 76):
                    screen.text(x + 4, row, ln, c, Palette.PANEL)
                    row += 1
            screen.center(y + 22, "[ ENTER ] BACK", Palette.PRIMARY_DIM, Palette.PANEL, x, 84)
            return
        x, y = panel(screen, 70, 20, "TUTORIAL  ·  ADVANCED PRACTICE" if self.adv else "TUTORIAL")
        screen.center(y + 1, "Optional. Practice range: no rewards, nothing can be lost." if not self.adv else "Optional challenges for a keen eye.", Palette.MUTED, Palette.PANEL, x, 70)
        for i, lab in enumerate(self._items()):
            sel = i == self.sel
            done = (not self.adv and i < 6 and f"tut:{i + 1}" in sv.story) or (self.adv and i < len(TUT_CHALLENGES) and "tut:6" in sv.story and False)
            screen.text(x + 6, y + 3 + i * 2, ("▶ " if sel else "  ") + lab, GOLD if sel else Palette.TEXT, Palette.PANEL)
            if done:
                screen.text(x + 62, y + 3 + i * 2, "✓", Palette.OK, Palette.PANEL)
        draw_keycaps(screen, x + 8, y + 18, (("▲▼", "CHOOSE"), ("ENTER", "START"), ("ESC", "BACK")), Palette.PANEL)


# ===========================================================================
# ROCK-PAPER-SCISSORS: decides who shoots first in Two Player and LAN (winner picks FIRST or SECOND)
# ===========================================================================
RPS_NAMES = ("ROCK", "PAPER", "SCISSORS")
RPS_ART = ((" ▄███▄ ", "███████", " ▀███▀ "), ("┌─────┐", "│     │", "└─────┘"), (" ╲   ╱ ", "   ╳   ", " ○   ○ "))


def _hp_ell(cv, cx: float, cy: float, rx: float, ry: float, col: RGB, y_shade: float = 0.0) -> None:
    for dx in range(-int(rx) - 1, int(rx) + 2):
        for dy in range(-int(ry) - 1, int(ry) + 2):
            if (dx / max(0.5, rx)) ** 2 + (dy / max(0.5, ry)) ** 2 <= 1.0:
                cv.plot(int(cx + dx), int(cy + dy), shade(col, 1.0 - y_shade * (0.5 - dy / (2 * max(0.5, ry)))))


def _hp_cap(cv, x0: float, y0: float, x1: float, y1: float, w: float, col: RGB, y_shade: float = 0.0) -> None:
    n = int(max(abs(x1 - x0), abs(y1 - y0)) * 2) + 1
    for i in range(n + 1):
        u = i / n
        _hp_ell(cv, x0 + (x1 - x0) * u, y0 + (y1 - y0) * u, w / 2, w / 2, col, y_shade)


def hand_sprite(kind: int, skin: RGB, sleeve: RGB, bob: float = 0.0, mirror: bool = False) -> "PixelCanvas":
    """A 32x36 pixel-art hand (0 rock, 1 paper, 2 scissors), outlined and lit from above, wearing a coloured cuff.
    Drawn twice (a dark expanded pass, then the skin) so every shape gets a clean outline."""
    W, H = 32, 36
    cv = PixelCanvas(W, H, (0, 0, 0))
    ol = (46, 26, 24)

    def parts(grow: float, col: RGB, sh: float) -> None:
        g = grow
        if kind == 0:                                                       # ROCK: a clenched fist
            _hp_ell(cv, 16, 13, 9 + g, 7.5 + g, col, sh)
            for i in range(4):
                _hp_ell(cv, 9.6 + i * 4.3, 19.2, 2.7 + g, 3.4 + g, col, sh)
            _hp_cap(cv, 7, 11, 15, 14.5, 4.6 + 2 * g, col, sh)               # the thumb across the front
        elif kind == 1:                                                     # PAPER: a flat open hand, fingers fanned
            _hp_ell(cv, 16, 12, 8.2 + g, 6.6 + g, col, sh)
            for i, ang in enumerate((-13, -4, 5, 14)):
                a = math.radians(ang)
                ln = (14, 16, 15.5, 12)[i]
                bx = 11 + i * 3.3
                _hp_cap(cv, bx, 16, bx + math.sin(a) * ln, 16 + math.cos(a) * ln, 3.3 + 2 * g, col, sh)
            _hp_cap(cv, 8, 10, 2.5, 19, 3.8 + 2 * g, col, sh)                # thumb out to the side
        else:                                                               # SCISSORS: index + middle in a V, the rest curled
            _hp_ell(cv, 16, 11, 8.2 + g, 6.2 + g, col, sh)
            _hp_cap(cv, 12, 15, 6.5, 31, 3.6 + 2 * g, col, sh)
            _hp_cap(cv, 19, 15, 25, 31, 3.6 + 2 * g, col, sh)
            for cx_ in (11.5, 16.5):
                _hp_ell(cv, cx_, 14.5, 2.7 + g, 2.5 + g, col, sh)            # ring + little finger folded down
            _hp_cap(cv, 8, 10, 15, 13, 4.0 + 2 * g, col, sh)
    parts(1.0, ol, 0.0)
    # sleeve / cuff (outlined)
    for x in range(8, 25):
        for y in range(0, 7):
            cv.plot(x, y, ol)
    for x in range(9, 24):
        for y in range(1, 6):
            cv.plot(x, y, shade(sleeve, 0.7 + 0.3 * (y / 5)))
    for x in range(9, 24):
        cv.plot(x, 6, mix(sleeve, (255, 255, 255), 0.45))
    parts(0.0, skin, 0.45)
    # knuckle / finger lines for a hand-drawn feel
    if kind == 0:
        for i in range(3):
            for y in range(16, 19):
                cv.plot(int(11.9 + i * 4.3), y, shade(skin, 0.62))
    elif kind == 1:
        for i in range(3):
            cv.plot(int(13.6 + i * 3.3), 18, shade(skin, 0.6))
    if mirror:
        cv.rows = [row[::-1] for row in cv.rows]
    return cv


def blit_sprite(dst: "PixelCanvas", src: "PixelCanvas", x: int, y: int, flip_y: bool = False) -> None:
    """Copies every non-black pixel of src onto dst at (x, y) (both y-up)."""
    for sy in range(src.h):
        for sx in range(src.w):
            c = src.rows[src.h - 1 - sy][sx]
            if c != (0, 0, 0):
                dst.plot(x + sx, y + sy, c)



class RPSScene(BackdropScene):
    """Local: players pick one after the other (the other looks away). LAN: both pick on their own machine; picks travel as
    {"t":"rps","c":n} and the winner's choice as {"t":"rpsfirst","p":seat}. Ties repeat. Sets session.first_offset, then battle."""

    def __init__(self, app, session: "MatchSession", seat: Optional[int] = None) -> None:
        super().__init__(app)
        self.s, self.seat, self.net = session, seat, session.config.net
        self.picks: list = [None, None]
        self.stage, self.turn, self.sel, self.t, self.winner, self.note = "pick", 0, 0, 0.0, None, ""
        pl = [p for p in session.config.players if not p.is_ai][:2]
        self.names = [(p.name or f"PLAYER {i + 1}")[:14] for i, p in enumerate(pl)] + ["PLAYER 1", "PLAYER 2"][len(pl):]
        self.choice = 0                                 # FIRST (0) / SECOND (1)

    @staticmethod
    def beats(a: int, b: int) -> bool:
        return (a - b) % 3 == 1

    def _finish(self, first_seat: int) -> None:
        self.s.first_offset = first_seat % 2
        self.app.goto(BattleScene(self.app, self.s))

    def _both(self) -> None:
        a, b = self.picks
        self.stage, self.t = "count", 0.0                 # both hands shake: ROCK... PAPER... SCISSORS... SHOOT!
        self.winner = None if a == b else (0 if self.beats(a, b) else 1)

    def update(self, dt: float) -> None:
        super().update(dt)
        self.t += dt
        if self.net is not None:
            self.net.pump()
            if self.net.take("quit") or self.net.dead:
                self.net.close()
                self.app.goto(NetNoticeScene(self.app, "OPPONENT LEFT", "The other player left the match."))
                return
            peer = 1 - self.seat
            m = self.net.peek("rps")
            if m is not None and self.picks[peer] is None and isinstance(m.get("c"), int):
                self.net.drop(m)
                self.picks[peer] = int(m["c"]) % 3
                if self.picks[self.seat] is not None:
                    self._both()
            f = self.net.peek("rpsfirst")
            if f is not None and self.stage == "wait":
                self.net.drop(f)
                self._finish(int(f.get("p", 0)))
                return
        if self.stage == "count" and self.t > 2.3:
            self.stage, self.t = "reveal", 0.0
        elif self.stage == "reveal" and self.t > 2.0:
            if self.winner is None:
                self.picks, self.stage, self.turn, self.note = [None, None], "pick", 0, "TIE! PICK AGAIN"
            elif self.net is None:
                self.stage = "choose"
            else:
                self.stage = "choose" if self.winner == self.seat else "wait"

    def handle(self, ev: InputEvent) -> None:
        if ev.action is Action.BACK and self.net is None:
            self.app.goto(MenuScene(self.app))
            return
        if ev.action in (Action.LEFT, Action.RIGHT, Action.UP, Action.DOWN):
            d = -1 if ev.action in (Action.LEFT, Action.UP) else 1
            if self.stage == "pick":
                self.sel = (self.sel + d) % 3
            elif self.stage == "choose":
                self.choice = 1 - self.choice
        elif ev.raw in ("1", "2", "3") and self.stage == "pick":
            self.sel = int(ev.raw) - 1
        elif ev.confirm:
            if self.stage == "pick":
                if self.net is not None:
                    if self.picks[self.seat] is None:
                        self.picks[self.seat] = self.sel
                        self.net.send({"t": "rps", "c": self.sel})
                        if self.picks[1 - self.seat] is not None:
                            self._both()
                else:
                    self.picks[self.turn] = self.sel
                    if self.turn == 0:
                        self.turn, self.sel = 1, 0
                    else:
                        self._both()
                self.note = ""
            elif self.stage == "choose":
                w = self.winner
                first_seat = w if self.choice == 0 else 1 - w
                if self.net is not None:
                    self.net.send({"t": "rpsfirst", "p": first_seat})
                self._finish(first_seat)

    SKINS = ((244, 204, 170), (176, 118, 84))

    def _hands(self, screen: Screen, x: int, y: int, kinds: tuple, bobs: tuple, glow: Optional[int] = None) -> None:
        """Two pixel-art hands facing each other, half-block rendered into the panel."""
        W_PX, H_PX = 84, 38
        cv = PixelCanvas(W_PX, H_PX, (0, 0, 0))
        cols = [live_color(p.tank_color, self.app.time) for p in self.s.config.players if not p.is_ai][:2]
        cols += [(90, 200, 255), (255, 150, 90)][len(cols):]
        for k in range(2):
            sp = hand_sprite(kinds[k], self.SKINS[k], cols[k], mirror=bool(k))
            hx = 6 if k == 0 else W_PX - 6 - sp.w
            blit_sprite(cv, sp, hx, int(bobs[k]))
        top = y
        for r in range(H_PX // 2):
            for c in range(W_PX):
                a_, b_ = cv.rows[2 * r][c], cv.rows[2 * r + 1][c]
                if a_ == (0, 0, 0) and b_ == (0, 0, 0):
                    continue
                bgc = Palette.PANEL
                screen.put(x + c, top + r, "▀", bgc if a_ == (0, 0, 0) else a_, bgc if b_ == (0, 0, 0) else b_)

    def draw(self, screen: Screen) -> None:
        super().draw(screen)
        W, H = 92, 26
        x, y = panel(screen, W, H, "WHO SHOOTS FIRST?")
        me = self.seat
        gx = x + (W - 84) // 2                                        # the hands' stage, centred
        if self.stage == "pick":
            who = (f"{self.names[me]}: CHOOSE YOUR HAND" if me is not None else f"{self.names[self.turn]}: CHOOSE  (the other player look away!)")
            screen.center(y + 1, who, GOLD, Palette.PANEL, x, W)
            locked = self.net is not None and self.picks[me] is not None
            for i in range(3):
                bx = x + 10 + i * 27
                sel = i == self.sel and not locked
                sp = hand_sprite(i, self.SKINS[(me if me is not None else self.turn) % 2], (90, 200, 255) if (me or self.turn) == 0 else (255, 150, 90))
                cv = PixelCanvas(32, 36, (0, 0, 0))
                blit_sprite(cv, sp, 0, 3 if sel and int(self.app.time * 4) % 2 else 0)
                for r in range(18):
                    for c in range(32):
                        a_, b_ = cv.rows[2 * r][c], cv.rows[2 * r + 1][c]
                        if a_ == (0, 0, 0) and b_ == (0, 0, 0):
                            continue
                        dim = 1.0 if (sel or locked is False) else 0.55
                        screen.put(bx + c - 2, y + 4 + r, "▀", shade(Palette.PANEL, 1) if a_ == (0, 0, 0) else shade(a_, dim if sel else 0.6),
                                   Palette.PANEL if b_ == (0, 0, 0) else shade(b_, dim if sel else 0.6))
                screen.center(y + 22, "", Palette.PANEL, Palette.PANEL, x, W) if False else None
                screen.text(bx + 8 - len(RPS_NAMES[i]) // 2 + 3, y + 21, ("▶ " if sel else "  ") + RPS_NAMES[i], Palette.WHITE if sel else Palette.MUTED, Palette.PANEL)
            if locked:
                screen.center(y + 23, "LOCKED IN - WAITING FOR THE OTHER PLAYER...", Palette.MUTED, Palette.PANEL, x, W)
            elif self.note:
                screen.center(y + 23, self.note, Palette.AMBER, Palette.PANEL, x, W)
            draw_keycaps(screen, x + 22, y + H - 2, (("◄ ►", "CHOOSE"), ("ENTER", "LOCK IN"), ("1 2 3", "QUICK PICK")), Palette.PANEL)
        elif self.stage == "count":
            beat = int(self.t / 0.5)
            word = ("ROCK...", "PAPER...", "SCISSORS...", "SHOOT!", "SHOOT!")[min(4, beat)]
            bobs = tuple(max(0, 14 * abs(math.sin(self.t * math.pi / 0.5)) - 2) if beat < 3 else 0 for _ in range(2))
            self._hands(screen, gx, y + 4, (0, 0), bobs)
            screen.center(y + 22, word, GOLD if beat >= 3 else Palette.WHITE, Palette.PANEL, x, W)
            names = f"{self.names[0]}     vs     {self.names[1]}"
            screen.center(y + 1, names, Palette.AMBER, Palette.PANEL, x, W)
        elif self.stage == "reveal":
            a, b = self.picks
            self._hands(screen, gx, y + 4, (a, b), (4 * ease_out(clamp(self.t / 0.2, 0, 1)),) * 2, self.winner)
            screen.center(y + 1, f"{self.names[0]}     vs     {self.names[1]}", Palette.AMBER, Palette.PANEL, x, W)
            if self.t < 0.12:
                screen.tint(x, y, W, H, (255, 255, 255), 0.5 * (1 - self.t / 0.12))
            screen.center(y + 21, f"{RPS_NAMES[a]}   vs   {RPS_NAMES[b]}", Palette.WHITE, Palette.PANEL, x, W)
            screen.center(y + 23, "IT'S A TIE!" if self.winner is None else f"{self.names[self.winner]} WINS!", Palette.OK if self.winner is not None else Palette.AMBER, Palette.PANEL, x, W)
        elif self.stage == "choose":
            screen.center(y + 3, f"{self.names[self.winner]} WON", GOLD, Palette.PANEL, x, W)
            screen.center(y + 5, "CHOOSE WHO SHOOTS FIRST", Palette.WHITE, Palette.PANEL, x, W)
            for i, lab in enumerate(("GO FIRST", "GO SECOND")):
                sel = i == self.choice
                screen.fill(x + 30, y + 9 + i * 4, 32, 3, Palette.PANEL_HI if sel else Palette.PANEL)
                screen.center(y + 10 + i * 4, ("▶ " if sel else "  ") + lab + (" ◀" if sel else "  "), GOLD if sel else Palette.TEXT, Palette.PANEL_HI if sel else Palette.PANEL, x + 30, 32)
            draw_keycaps(screen, x + 28, y + H - 2, (("◄ ►", "CHOOSE"), ("ENTER", "CONFIRM")), Palette.PANEL)
        else:
            screen.center(y + 10, f"{self.names[self.winner]} WON - CHOOSING WHO GOES FIRST...", Palette.AMBER, Palette.PANEL, x, W)


# ===========================================================================
# PROFILES: up to three named saves in one game instance (profile 1 is the original save file: nothing to migrate)
# ===========================================================================
PROFILE_MAX = 3


def fmt_playtime(sec: float) -> str:
    """12345678 s -> '142D 21H 21M'; shows years and months once they exist (365-day years, 30-day months)."""
    m = max(0, int(sec)) // 60
    y, m = divmod(m, 525600)
    mo, m = divmod(m, 43200)
    d, m = divmod(m, 1440)
    h, mi = divmod(m, 60)
    parts = []
    for val, lab in ((y, "Y"), (mo, "MO"), (d, "D"), (h, "H")):
        if parts or val:
            parts.append(f"{val}{lab}")
    parts.append(f"{mi:02d}M" if parts else f"{mi}M")
    return " ".join(parts)


class ProfileBook:
    def __init__(self, base: Optional[Path]) -> None:
        self.base = base
        self.index_path = base.parent / "profiles.json" if base is not None else None
        self.profiles: list[dict] = [{"id": "1", "name": "PROFILE 1", "file": base.name if base else ""}]
        self.active = "1"
        if self.index_path is not None and self.index_path.exists():
            try:
                raw = json.loads(self.index_path.read_text(encoding="utf-8"))
                profs = [p for p in raw.get("profiles", []) if isinstance(p, dict) and isinstance(p.get("id"), str)
                         and isinstance(p.get("file"), str) and "/" not in p["file"] and "\\" not in p["file"]]
                if profs:
                    self.profiles = [{"id": p["id"], "name": str(p.get("name", "PROFILE"))[:14] or "PROFILE", "file": p["file"]} for p in profs[:PROFILE_MAX]]
                    self.active = raw.get("active") if raw.get("active") in {p["id"] for p in self.profiles} else self.profiles[0]["id"]
            except (OSError, ValueError, AttributeError):
                pass

    def path_of(self, pid: str) -> Optional[Path]:
        p = next((q for q in self.profiles if q["id"] == pid), None)
        return self.base.parent / p["file"] if (p and self.base is not None) else None

    def active_path(self) -> Optional[Path]:
        return self.path_of(self.active)

    def write(self) -> None:
        if self.index_path is None:
            return
        try:
            self.index_path.parent.mkdir(parents=True, exist_ok=True)
            tmp = self.index_path.with_suffix(".tmp")
            tmp.write_text(json.dumps({"active": self.active, "profiles": self.profiles}, indent=1), encoding="utf-8")
            os.replace(tmp, self.index_path)
        except OSError:
            pass

    def create(self, name: str) -> Optional[str]:
        if len(self.profiles) >= PROFILE_MAX or self.base is None:
            return None
        pid = next(str(i) for i in range(1, 10) if str(i) not in {p["id"] for p in self.profiles})
        self.profiles.append({"id": pid, "name": (name.strip() or f"PROFILE {pid}")[:14], "file": f"save_profile{pid}.json"})
        self.write()
        return pid

    def rename(self, pid: str, name: str) -> None:
        for p in self.profiles:
            if p["id"] == pid and name.strip():
                p["name"] = name.strip()[:14]
        self.write()

    def delete(self, pid: str) -> bool:
        if len(self.profiles) <= 1:
            return False
        p = next((q for q in self.profiles if q["id"] == pid), None)
        if p is None:
            return False
        path = self.path_of(pid)
        self.profiles = [q for q in self.profiles if q["id"] != pid]
        if self.active == pid:
            self.active = self.profiles[0]["id"]
        self.write()
        try:
            if path is not None and path.exists() and path != self.base:      # the original save file is never deleted by accident
                path.unlink()
            elif path is not None and path == self.base and path.exists():
                path.rename(path.with_suffix(".deleted.json"))
        except OSError:
            pass
        return True


class ProfilesScene(BackdropScene):
    """ENTER = switch (or create in an empty slot) · R = rename · X = delete (press twice) · ESC = back."""

    def __init__(self, app) -> None:
        super().__init__(app)
        self.sel, self.mode, self.buf, self.msg, self.confirm = 0, "", "", "", False

    def _rows(self) -> list:
        return list(self.app.profiles.profiles) + ([None] if len(self.app.profiles.profiles) < PROFILE_MAX else [])

    def _save_of(self, p: dict) -> "SaveData":
        return self.app.save if p["id"] == self.app.profiles.active else SaveData.load(self.app.profiles.path_of(p["id"]))

    def handle(self, ev: InputEvent) -> None:
        book, rows = self.app.profiles, self._rows()
        self.sel = min(self.sel, len(rows) - 1)
        if self.mode:                                            # typing a name
            if ev.action is Action.BACK:
                self.mode = ""
            elif ev.confirm:
                if self.mode == "new":
                    pid = book.create(self.buf)
                    if pid:
                        self.app.switch_profile(pid)
                        return
                elif self.mode == "rename" and rows[self.sel]:
                    book.rename(rows[self.sel]["id"], self.buf)
                self.mode = ""
            elif ev.raw == "BACKSPACE":
                self.buf = self.buf[:-1]
            elif ev.raw == "SPACE" and len(self.buf) < 14:
                self.buf += " "
            elif len(ev.raw) == 1 and ev.raw.isprintable() and len(self.buf) < 14 and (ev.raw.isalnum() or ev.raw in "-_."):
                self.buf += ev.raw.upper()
            return
        if ev.action is Action.BACK:
            self.app.goto(MenuScene(self.app))
        elif ev.action is Action.UP:
            self.sel, self.confirm = (self.sel - 1) % len(rows), False
        elif ev.action is Action.DOWN:
            self.sel, self.confirm = (self.sel + 1) % len(rows), False
        elif ev.confirm:
            p = rows[self.sel]
            if p is None:
                self.mode, self.buf = "new", ""
            elif p["id"] != book.active:
                self.app.switch_profile(p["id"])
            else:
                self.msg = "THAT PROFILE IS ALREADY ACTIVE"
        elif ev.raw in ("r", "R") and rows[self.sel]:
            self.mode, self.buf = "rename", rows[self.sel]["name"]
        elif ev.raw in ("x", "X", "DELETE") and rows[self.sel]:
            if len(book.profiles) <= 1:
                self.msg = "YOU NEED AT LEAST ONE PROFILE"
            elif not self.confirm:
                self.confirm, self.msg = True, "PRESS X AGAIN TO DELETE THIS PROFILE AND ITS PROGRESS FOR GOOD"
            else:
                self.app.profiles_delete(rows[self.sel]["id"])
                self.confirm, self.msg, self.sel = False, "PROFILE DELETED", 0

    TIER_NAMES = ("NEW GAME", "TOURNAMENT CLEARED", "MISSIONS CLEARED", "VAULT CLEARED", "LEGEND")

    @staticmethod
    def outline_color(tier: int, i: int, t: float) -> RGB:
        """Border colour of the i-th perimeter cell: gold / pulsing green / void / astral+gold, so progress reads at a glance."""
        if tier == 1:
            return mix((190, 140, 40), (255, 222, 120), 0.5 + 0.5 * math.sin(t * 1.2 + i * 0.1))
        if tier == 2:
            return mix((14, 80, 36), (110, 255, 150), 0.5 + 0.5 * math.sin(t * 3.0 + i * 0.05))
        if tier == 3:
            return mix(void_tint(t), (10, 4, 28), 0.5 + 0.5 * math.sin(t * 1.4 + i * 0.12))
        if tier == 4:
            return (255, 214, 90) if (i + int(t * 6)) % 6 < 3 else astral(t + i * 0.03)
        return Palette.LINE

    def draw(self, screen: Screen) -> None:
        super().draw(screen)
        W, H = 84, 26
        x, y = panel(screen, W, H, "PROFILES")
        book, rows, t = self.app.profiles, self._rows(), self.app.time
        screen.center(y + 1, "Separate saves in one game. Switching saves your current profile first.", Palette.MUTED, Palette.PANEL, x, W)
        for i, p in enumerate(rows):
            sel, ry = i == self.sel, y + 3 + i * 5
            bw, bx = W - 6, x + 3
            screen.fill(bx, ry, bw, 5, Palette.PANEL_HI if sel else Palette.PANEL)
            bg = Palette.PANEL_HI if sel else Palette.PANEL
            if p is None:
                screen.text(bx + 3, ry + 2, "+ NEW PROFILE", GOLD if sel else Palette.MUTED, bg)
                continue
            sv = self._save_of(p)
            tier, cnt = sv.progress_tier(), sv.progress_counts()
            per = [(bx + k, ry) for k in range(bw)] + [(bx + bw - 1, ry + k) for k in range(1, 5)] + [(bx + bw - 1 - k, ry + 4) for k in range(1, bw)] + [(bx, ry + 4 - k) for k in range(1, 4)]
            for n, (px, py) in enumerate(per):
                ch = "═" if py in (ry, ry + 4) else "║"
                if (px, py) == (bx, ry): ch = "╔"
                elif (px, py) == (bx + bw - 1, ry): ch = "╗"
                elif (px, py) == (bx, ry + 4): ch = "╚"
                elif (px, py) == (bx + bw - 1, ry + 4): ch = "╝"
                screen.put(px, py, ch, self.outline_color(tier, n, t), bg)
            act = p["id"] == book.active
            screen.text(bx + 3, ry + 1, ("▶ " if sel else "  ") + p["name"] + ("   ★ ACTIVE" if act else ""), GOLD if act else Palette.WHITE if sel else Palette.TEXT, bg)
            tn = self.TIER_NAMES[tier]
            screen.text(bx + bw - 3 - len(tn), ry + 1, tn, self.outline_color(tier, int(t * 10), t), bg)
            line = f"PLAYTIME {fmt_playtime(sv.playtime)}    ◉ {sv.coins:,}    ◈ {sv.credits:,}"
            screen.text(bx + 5, ry + 2, line[:bw - 8], Palette.AMBER, bg)
            c = cnt
            screen.text(bx + 5, ry + 3, f"TANKS {c['tanks'][0]}/{c['tanks'][1]}   AMMO {c['ammo'][0]}/{c['ammo'][1]}   MAPS {c['maps'][0]}/{c['maps'][1]}   SCREENS {c['screens'][0]}/{c['screens'][1]}"[:bw - 8], Palette.MUTED, bg)
        if self.mode:
            screen.fill(x + 6, y + 18, W - 12, 4, Palette.PANEL_HI)
            screen.text(x + 8, y + 18, "NAME THE NEW PROFILE:" if self.mode == "new" else "RENAME PROFILE:", Palette.AMBER, Palette.PANEL_HI)
            screen.text(x + 8, y + 19, self.buf + ("█" if int(self.app.time * 2) % 2 == 0 else " "), Palette.WHITE, Palette.PANEL_HI)
            screen.text(x + 8, y + 20, "ENTER OK   ESC CANCEL", Palette.MUTED, Palette.PANEL_HI)
        elif self.msg:
            wrap_center(screen, y + 21, self.msg, W - 8, Palette.AMBER, Palette.PANEL, x, W, 2)
        draw_keycaps(screen, x + 8, y + H - 2, (("▲▼", "CHOOSE"), ("ENTER", "SWITCH/NEW"), ("R", "RENAME"), ("X", "DELETE"), ("ESC", "BACK")), Palette.PANEL)


class RoundMapScene(BackdropScene):
    """After a round the loser picks the next battlefield: numbered list + live terrain scan. LAN: sent to the opponent."""
    VISIBLE = 17

    def __init__(self, app, session: MatchSession, picker: int) -> None:
        super().__init__(app)
        self.session, self.picker, self.cache = session, picker, {}
        self.maps = [None] + list(MAPS) + app.save.unlocked_maps()          # None = RANDOM
        self.index = 0

    def handle(self, ev: InputEvent) -> None:
        n = len(self.maps)
        if ev.action is Action.UP:
            self.index = (self.index - 1) % n
        elif ev.action is Action.DOWN:
            self.index = (self.index + 1) % n
        elif ev.action is Action.LEFT:
            self.index = max(0, self.index - 8)
        elif ev.action is Action.RIGHT:
            self.index = min(n - 1, self.index + 8)
        elif ev.confirm:
            m = self.maps[self.index]
            net = self.session.config.net
            if net is not None:
                net.send({"t": "map", "key": m.key if m else ""})
            self.session.pending_map = m
            self.session.advance()
            self.app.goto(BattleScene(self.app, self.session))

    def draw(self, screen: Screen) -> None:
        super().draw(screen)
        s = self.session
        who = s.team_name(self.picker) if s.config.mode != "single" else "YOU"
        n = len(self.maps)
        x, y = (screen.cols - 88) // 2, max(0, (screen.rows - 24) // 2)
        screen.box(x, y, 34, 24, "double", Palette.LINE, Palette.PANEL, "NEXT BATTLEFIELD")
        screen.center(y + 1, f"{who[:20]} PICK{'S' if who != 'YOU' else ''}", Palette.AMBER, Palette.PANEL, x, 34)
        top = int(clamp(self.index - self.VISIBLE // 2, 0, max(0, n - self.VISIBLE)))
        for row in range(min(self.VISIBLE, n)):
            i = top + row
            sel, m = i == self.index, self.maps[i]
            bg = Palette.PANEL_HI if sel else Palette.PANEL
            name = "RANDOM" if m is None else (f"{i:03d} {m.name}" if not m.special else f"{i:03d} ★ {m.name}")
            col = Palette.WHITE if sel else (m.accent if (m and m.special) else Palette.AMBER if m is None else Palette.MUTED)
            screen.fill(x + 2, y + 3 + row, 30, 1, bg)
            screen.text(x + 3, y + 3 + row, ("▶ " if sel else "  ") + name[:27], col, bg)
        if top > 0:
            screen.put(x + 31, y + 3, "▲", Palette.PRIMARY_DIM, Palette.PANEL)
        if top + self.VISIBLE < n:
            screen.put(x + 31, y + 2 + self.VISIBLE, "▼", Palette.PRIMARY_DIM, Palette.PANEL)
        screen.center(y + 21, f"{self.index:03d}/{n - 1}  ·  A/D PAGE  ·  ENTER PICK", Palette.MUTED, Palette.PANEL, x, 34)
        px = x + 36
        screen.box(px, y, 52, 24, "double", Palette.LINE, Palette.PANEL, "TERRAIN SCAN")
        m = self.maps[self.index] or MAPS[int(self.app.time / 1.2) % len(MAPS)]
        pl = [(DESIGN_BY_KEY[p.design], p.tank_color) for p in s.config.players[:2]]
        screen.blit(render_map_preview(self.cache, m, 46, 24, self.app.time, pl), px + 3, y + 2)
        screen.center(y + 15, (f"{self.index:03d}/{n - 1}  " if self.index else "") + (m.name if self.index else "RANDOM"),
                      m.accent if (self.index and m.special) else Palette.AMBER, Palette.PANEL, px, 52)
        wrap_center(screen, y + 16, m.description if self.index else "A fresh battlefield.", 48, Palette.TEXT, Palette.PANEL, px, 52, 3)
        if self.index:
            dots = lambda k: "●" * k + "○" * (3 - k)
            screen.center(y + 20, f"COVER {dots(m.cover)}     RELIEF {dots(m.relief)}", Palette.PRIMARY, Palette.PANEL, px, 52)


class RoundMapWaitScene(BackdropScene):
    """LAN: the other player lost the round and is choosing the next battlefield."""

    def __init__(self, app, session: MatchSession, picker: int) -> None:
        super().__init__(app)
        self.session, self.picker, self.net = session, picker, session.config.net

    def update(self, dt: float) -> None:
        super().update(dt)
        self.net.pump()
        if self.net.take("quit") or self.net.dead:
            self.net.close()
            self.app.goto(NetNoticeScene(self.app, "OPPONENT LEFT", "The other player left the match."))
            return
        msgs = self.net.take("map")
        if msgs:
            self.session.pending_map = all_maps().get(str(msgs[-1].get("key", "")))
            self.session.advance()
            self.app.goto(BattleScene(self.app, self.session))

    def handle(self, ev: InputEvent) -> None:
        if ev.action is Action.BACK:
            self.app.push(ConfirmLeaveScene(self.app, self.session))

    def draw(self, screen: Screen) -> None:
        super().draw(screen)
        x, y = panel(screen, 52, 9, "NEXT BATTLEFIELD")
        screen.center(y + 3, f"{self.session.team_name(self.picker)} IS CHOOSING THE NEXT MAP", Palette.AMBER, Palette.PANEL, x, 52)
        dots = "." * (1 + int(self.app.time * 2) % 3)
        screen.center(y + 5, "WAITING" + dots, Palette.MUTED, Palette.PANEL, x, 52)


class RoundResultScene(Scene):
    overlay = True

    def __init__(self, app, battle: BattleScene) -> None:
        super().__init__(app)
        self.battle = battle
        self.session = battle.session

        self.net = battle.session.config.net
        self.ready_me = self.ready_peer = self.moved = False

    def _proceed(self) -> None:
        if self.moved:
            return
        self.moved = True
        if self.session.over:
            self.app.goto(self.session.finish_scene(self.app))
            return
        s, cfg = self.session, self.session.config
        if cfg.pick_maps and type(s) is MatchSession and cfg.mode in ("single", "two", "lan"):
            loser = self.battle.doomed.team
            if cfg.mode == "lan":
                if loser == cfg.my_owner:
                    self.app.goto(RoundMapScene(self.app, s, loser))
                else:
                    self.app.goto(RoundMapWaitScene(self.app, s, loser))
                return
            if cfg.mode in ("two", "single") or loser == 0:
                self.app.goto(RoundMapScene(self.app, s, loser))
                return
        s.advance()
        self.app.goto(s.interlude(self.app) or BattleScene(self.app, s))

    def update(self, dt: float) -> None:
        if self.net is not None:
            self.net.pump()
            if self.net.take("quit") or self.net.dead:
                self.net.close()
                self.app.goto(NetNoticeScene(self.app, "OPPONENT LEFT", "The other player left the match."))
                return
            if self.net.take("ready"):
                self.ready_peer = True
            if self.ready_me and self.ready_peer:
                self._proceed()

    def handle(self, ev: InputEvent) -> None:
        if ev.action is Action.BACK and self.session.config.mode in ("two", "lan"):
            self.app.push(ConfirmLeaveScene(self.app, self.session))
            return
        if ev.action is Action.CONFIRM or ev.action is Action.FIRE:
            if self.net is None:
                self._proceed()
            elif not self.ready_me:
                self.ready_me = True
                self.net.send({"t": "ready"})

    def draw(self, screen: Screen) -> None:
        screen.dim(0.5)
        s = self.session
        winner = 1 - self.battle.doomed.team
        info = s.round_summary(winner)
        pl = (s.team_players(winner) or s.config.players)[0]
        x, y = panel(screen, 52, 13, info["title"], fg=pl.tank_color)
        screen.center(y + 2, info["headline"], pl.tank_color, Palette.PANEL, x, 52)
        screen.center(y + 4, info["score"], Palette.WHITE, Palette.PANEL, x, 52)
        screen.center(y + 5, info["sub"], Palette.MUTED, Palette.PANEL, x, 52)
        screen.center(y + 8, info["next"], Palette.AMBER if s.over else Palette.TEXT, Palette.PANEL, x, 52)
        if self.net is not None and self.ready_me and not self.ready_peer:
            screen.center(y + 10, "WAITING FOR THE OTHER PLAYER...", Palette.AMBER, Palette.PANEL, x, 52)
        elif int(self.app.time * 2) % 2 == 0:
            screen.center(y + 10, "[ ENTER ]", Palette.PRIMARY, Palette.PANEL, x, 52)


# ============================================================================
# Match rules (Two Player + LAN) and LAN networking
# ============================================================================
ROUND_PRESETS = (3, 5, 7, 10, 0)          # 0 = custom
HP_PRESETS = (1, 3, 5, 7, 10, 0)


@dataclass
class Rules:
    """Two Player / LAN match rules: BEST OF or FIRST TO n rounds, hearts, the battlefield, wind and the special attack."""
    fmt: str = "best"                     # "best" | "first"
    n_idx: int = 0
    n_custom: int = 4
    hp_idx: int = 1
    hp_custom: int = 4
    map_key: str = ""                     # "" = random battlefield
    wind: bool = True
    special: bool = False
    ammo_fx: bool = True                  # special ammo effects ON/OFF
    pick_maps: bool = True                # choose the battlefield every round (loser picks) instead of a random one
    blessing: bool = False                # MARTYR'S BLESSING allowed in this mode (independent of `ammo_fx`; needs the unlock)

    @property
    def n(self) -> int:
        return ROUND_PRESETS[self.n_idx] or self.n_custom

    @property
    def hp(self) -> int:
        return HP_PRESETS[self.hp_idx] or self.hp_custom

    @property
    def target(self) -> int:
        """Rounds one side must win. Best of n: more than half (best of 10 = first to 6, so a tie can never survive)."""
        return self.n // 2 + 1 if self.fmt == "best" else self.n

    @property
    def label(self) -> str:
        return f"{'BEST OF' if self.fmt == 'best' else 'FIRST TO'} {self.n}"

    @property
    def mapdef(self) -> Optional[MapDefinition]:
        return all_maps().get(self.map_key)

    def wind_value(self, settings: "Settings") -> float:
        """Wind strength for this match: your Settings strength when ON (a medium default if that is OFF), 0 when OFF."""
        return 0.0 if not self.wind else (settings.wind if settings.wind > 0 else 5.0)

    def summary(self) -> str:
        m = self.mapdef.name if self.mapdef else "RANDOM MAP"
        return f"{self.label} · {self.hp} HP · {m} · WIND {'ON' if self.wind else 'OFF'}" + (" · SPECIAL" if self.special else "") + ("" if self.ammo_fx else " · AMMO FX OFF") + (" · BLESSING" if self.blessing else "")

    def dump(self) -> str:
        return f"{self.fmt},{self.n_idx},{self.n_custom},{self.hp_idx},{self.hp_custom},{self.map_key},{int(self.wind)},{int(self.special)},{int(self.ammo_fx)},{int(self.pick_maps)},{int(self.blessing)}"

    @classmethod
    def load(cls, text: str) -> "Rules":
        try:
            parts = text.split(",")
            f, a, b, c, d = parts[:5]                         # older saves only had the first five fields
            r = cls(f if f in ("best", "first") else "best", int(a), int(b), int(c), int(d))
            if len(parts) >= 8:
                r.map_key, r.wind, r.special = parts[5], parts[6] == "1", parts[7] == "1"
            if len(parts) >= 9:
                r.ammo_fx = parts[8] == "1"
            if len(parts) >= 10:
                r.pick_maps = parts[9] == "1"
            if len(parts) >= 11:
                r.blessing = parts[10] == "1"
            if not (0 <= r.n_idx < len(ROUND_PRESETS) and 0 <= r.hp_idx < len(HP_PRESETS)):
                raise ValueError
            r.n_custom, r.hp_custom = int(clamp(r.n_custom, 1, 25)), int(clamp(r.hp_custom, 1, 50))
            if r.map_key and r.map_key not in all_maps():
                r.map_key = ""
            return r
        except ValueError:
            return cls()


def all_maps() -> dict:
    return {m.key: m for m in MAPS + SPECIAL_MAPS}


class RulesScene(BackdropScene):
    """Pick BEST OF / FIRST TO, rounds, hearts, the battlefield (live terrain scan), WIND, SPECIAL AMMO and the special attack. Remembered."""

    def __init__(self, app, title: str, on_done: Callable, kind: str = "two") -> None:
        super().__init__(app)
        self.title, self.on_done, self.kind = title, on_done, kind
        self.rkey = "rules_single" if kind == "single" else "rules_lan" if kind == "lan" else "rules"
        old = app.save.settings.get(self.rkey)
        self.rules = Rules.load(old if old is not None else app.save.settings.get("rules", "") if kind == "lan" else "")
        if old is None and kind == "lan":
            self.rules.blessing = False                      # LAN used to share the two-player entry: never inherit its Blessing choice
        if not app.save.blessing_unlocked():
            self.rules.blessing = False
        self.row = 0
        self.maps = [None] + list(MAPS) + app.save.unlocked_maps()           # None = RANDOM
        self.cache: dict = {}

    def keys(self) -> list:
        r = self.rules
        k = ["fmt", "n"] + (["n_custom"] if ROUND_PRESETS[r.n_idx] == 0 else [])
        k += ["hp"] + (["hp_custom"] if HP_PRESETS[r.hp_idx] == 0 else [])
        return k + ["map", "next", "wind", "ammo", "blessing", "special", "go"]

    def _map_index(self) -> int:
        return next((i for i, m in enumerate(self.maps) if m is not None and m.key == self.rules.map_key), 0)

    def handle(self, ev: InputEvent) -> None:
        keys, r = self.keys(), self.rules
        self.row = min(self.row, len(keys) - 1)
        if ev.action is Action.BACK:
            self.app.goto(LanMenuScene(self.app) if self.kind == "lan" else MenuScene(self.app))
        elif ev.action is Action.UP:
            self.row = (self.row - 1) % len(keys)
        elif ev.action is Action.DOWN:
            self.row = (self.row + 1) % len(keys)
        elif ev.action in (Action.LEFT, Action.RIGHT):
            d, k = (-1 if ev.action is Action.LEFT else 1), keys[self.row]
            step = d * (5 if ev.coarse else 1)
            if k == "fmt":
                r.fmt = "first" if r.fmt == "best" else "best"
            elif k == "n":
                r.n_idx = (r.n_idx + d) % len(ROUND_PRESETS)
            elif k == "hp":
                r.hp_idx = (r.hp_idx + d) % len(HP_PRESETS)
            elif k == "n_custom":
                r.n_custom = int(clamp(r.n_custom + step, 1, 25))
            elif k == "hp_custom":
                r.hp_custom = int(clamp(r.hp_custom + step, 1, 50))
            elif k == "map":
                m = self.maps[(self._map_index() + step) % len(self.maps)]
                r.map_key = m.key if m else ""
            elif k == "next":
                r.pick_maps = not r.pick_maps
            elif k == "wind":
                r.wind = not r.wind
            elif k == "ammo":
                r.ammo_fx = not r.ammo_fx
            elif k == "blessing":
                r.blessing = (not r.blessing) and self.app.save.blessing_unlocked()      # locked: stays OFF
            elif k == "special":
                r.special = not r.special
        elif ev.confirm:
            r.blessing = r.blessing and self.app.save.blessing_unlocked()
            self.app.save.settings[self.rkey] = r.dump()
            self.app.save.flush()
            self.on_done(r)

    def draw(self, screen: Screen) -> None:
        super().draw(screen)
        r, keys, t = self.rules, self.keys(), self.app.time
        self.row = min(self.row, len(keys) - 1)
        PW, VW = 50, 44
        sp = 2 if 9 + len(keys) * 2 <= screen.rows - 1 else 1
        H = max(22, 9 + len(keys) * sp)
        x, y = (screen.cols - (PW + 1 + VW)) // 2, max(0, (screen.rows - H) // 2)
        screen.box(x, y, PW, H, "double", Palette.LINE, Palette.PANEL, self.title)
        names = {"fmt": "FORMAT", "n": "ROUNDS", "n_custom": "  CUSTOM ROUNDS", "hp": "HEALTH", "hp_custom": "  CUSTOM HEALTH",
                 "map": "BATTLEFIELD", "next": "LATER ROUNDS", "wind": "WIND", "ammo": "SPECIAL AMMO", "blessing": "MARTYR'S BLESSING", "special": "SPECIAL ATTACK", "go": "CONTINUE"}
        mp = self.maps[self._map_index()]
        for i, k in enumerate(keys):
            ry, sel = y + 2 + i * sp, i == self.row
            bg = Palette.PANEL_HI if sel else Palette.PANEL
            screen.fill(x + 2, ry, PW - 4, 1, bg)
            screen.text(x + 4, ry, names[k], Palette.WHITE if sel else Palette.MUTED, bg)
            val = {"fmt": "BEST OF" if r.fmt == "best" else "FIRST TO", "n": str(r.n) if ROUND_PRESETS[r.n_idx] else "CUSTOM",
                   "n_custom": str(r.n_custom), "hp": str(r.hp) if HP_PRESETS[r.hp_idx] else "CUSTOM",
                   "hp_custom": str(r.hp_custom), "map": (f"{self._map_index():03d}/{len(self.maps) - 1} {mp.name}" if mp else "RANDOM"),
                   "next": ("PICK EACH ROUND" if r.pick_maps else "RANDOM"), "wind": "ON" if r.wind else "OFF",
                   "ammo": "ON" if r.ammo_fx else "OFF", "special": "ON" if r.special else "OFF", "go": "▶",
                   "blessing": ("LOCKED" if not self.app.save.blessing_unlocked() else "ON" if r.blessing else "OFF")}[k]
            val = f"◄ {val[:25]} ►" if sel and k != "go" else val[:29]
            screen.text(x + PW - 4 - len(val), ry, val, GOLD if sel else Palette.TEXT, bg)
        screen.center(y + H - 6, f"{r.label}  ·  {r.hp} {'HEART' if r.hp == 1 else 'HEARTS'} EACH  ·  WIND {'ON' if r.wind else 'OFF'}",
                      Palette.AMBER, Palette.PANEL, x, PW)
        extra = ("SPECIAL: press X to forfeit a turn; after two, next shot deals 3" if r.special
                 else f"A side wins the match with {r.target} round wins")
        screen.center(y + H - 5, extra[:PW - 2], Palette.MUTED, Palette.PANEL, x, PW)
        if keys[self.row] == "ammo":
            tip = ("ON: ✦ ammo really works (bounce, split, cluster, gravity, wind)."
                   if r.ammo_fx else "OFF: every ammo only changes looks - no special-ammo edge.")
            for k, ln in enumerate(textwrap.wrap(tip, PW - 6)[:2]):
                screen.center(y + H - 4 + k, ln, Palette.PRIMARY, Palette.PANEL, x, PW)
        elif keys[self.row] == "blessing":
            tip = ("Defeat the Martyr to inherit this ultimate ammo." if not self.app.save.blessing_unlocked()
                   else "ON: MARTYR'S BLESSING may be loaded - one direct hit erases every foe. Separate from SPECIAL AMMO." if r.blessing
                   else "OFF: MARTYR'S BLESSING cannot be selected or fired in this mode.")
            for k, ln in enumerate(textwrap.wrap(tip, PW - 6)[:2]):
                screen.center(y + H - 4 + k, ln, GOLD, Palette.PANEL, x, PW)
        else:
            screen.center(y + H - 4, "SPECIAL AMMO ON: effects active" if r.ammo_fx else "SPECIAL AMMO OFF: all ammo is cosmetic",
                          Palette.PRIMARY if r.ammo_fx else Palette.MUTED, Palette.PANEL, x, PW)
        draw_keycaps(screen, x + 3, y + H - 2, (("W/S", "ROW"), ("A/D", "CHANGE"), ("SHIFT", "x5"), ("ENTER", "OK")), Palette.PANEL)
        # live terrain scan
        vx = x + PW + 1
        screen.box(vx, y, VW, H, "double", Palette.LINE, Palette.PANEL, "TERRAIN SCAN")
        m = mp or MAPS[int(t / 1.2) % len(MAPS)]
        pl = []
        for i in range(2):
            lo = self.app.save.loadout(i)
            pl.append((DESIGN_BY_KEY.get(lo.tank, DESIGNS[0]), COLOR_BY_NAME[lo.tank_color]))
        screen.blit(render_map_preview(self.cache, m, VW - 4, 24, t, pl), vx + 2, y + 2)
        screen.center(y + 14, (f"{self._map_index():03d}/{len(self.maps) - 1}  " if mp else "") + (m.name if mp else "RANDOM"),
                      m.accent if (mp and m.special) else Palette.AMBER, Palette.PANEL, vx, VW)
        desc = m.description if mp else "A fresh battlefield every round."
        n = wrap_center(screen, y + 15, desc, VW - 4, Palette.TEXT, Palette.PANEL, vx, VW, 3)
        if mp:
            dots = lambda k: "●" * k + "○" * (3 - k)
            screen.center(y + 15 + n + 1, f"COVER {dots(m.cover)}     RELIEF {dots(m.relief)}", Palette.PRIMARY, Palette.PANEL, vx, VW)
        screen.center(y + H - 2, "Opening map only - later rounds rotate", Palette.MUTED, Palette.PANEL, vx, VW)


def field_size(cols: int, rows: int) -> tuple:
    """Battlefield pixel size a terminal of this size can show (what BattleScene would pick for itself)."""
    fc = min(cols, MAX_FIELD_COLS)
    fr = max(8, min(rows - HUD_TOP_ROWS - HUD_BOTTOM_ROWS, MAX_FIELD_ROWS))
    return fc, fr * 2


class NetLink:
    """One TCP connection speaking newline-delimited JSON, fully non-blocking (polled from the game loop)."""

    def __init__(self, sock: socket.socket) -> None:
        sock.setblocking(False)
        try:
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        except OSError:
            pass
        self.sock, self.rbuf, self.wbuf, self.inbox, self.dead = sock, b"", b"", [], False

    def send(self, obj: dict) -> None:
        if not self.dead:
            self.wbuf += (json.dumps(obj, separators=(",", ":")) + "\n").encode()
            self._flush()

    def _flush(self) -> None:
        while self.wbuf and not self.dead:
            try:
                n = self.sock.send(self.wbuf)
            except (BlockingIOError, InterruptedError):
                return
            except OSError:
                self.dead = True
                return
            self.wbuf = self.wbuf[n:]

    def pump(self) -> None:
        self._flush()
        while not self.dead:
            try:
                data = self.sock.recv(65536)
            except (BlockingIOError, InterruptedError):
                break
            except OSError:
                self.dead = True
                break
            if not data:
                self.dead = True
                break
            self.rbuf += data
        while b"\n" in self.rbuf:
            line, self.rbuf = self.rbuf.split(b"\n", 1)
            try:
                m = json.loads(line.decode("utf-8"))
            except ValueError:
                continue
            if isinstance(m, dict) and isinstance(m.get("t"), str):
                self.inbox.append(m)

    def take(self, *types: str) -> list:
        got = [m for m in self.inbox if m["t"] in types]
        if got:
            self.inbox = [m for m in self.inbox if m["t"] not in types]
        return got

    def peek(self, *types: str) -> Optional[dict]:
        return next((m for m in self.inbox if m["t"] in types), None)

    def drop(self, m: dict) -> None:
        for i, x in enumerate(self.inbox):
            if x is m:
                del self.inbox[i]
                return

    def close(self) -> None:
        try:
            self._flush()
            self.sock.close()
        except OSError:
            pass
        self.dead = True


WEB_LAN_TEXT = ("LAN play needs direct network access, which browsers do not allow, so it is disabled in the web version. "
                "Download the game to play LAN - the installed version works fully.")


def local_ips() -> list:
    ips: list = []
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("10.255.255.255", 1))             # no packet is sent; it just asks the OS which interface would be used
        ips.append(s.getsockname()[0])
        s.close()
    except OSError:
        pass
    try:
        for ip in socket.gethostbyname_ex(socket.gethostname())[2]:
            if ip not in ips and not ip.startswith("127."):
                ips.append(ip)
    except OSError:
        pass
    return ips or ["127.0.0.1"]


class LanHost:
    """Listens for one player and shouts 'game here' over UDP broadcast once a second so joiners can find it."""

    def __init__(self, name: str, port: int = LAN_PORT, beacon: bool = True, bind: str = "") -> None:
        self.name, self.t = name, 0.0
        self.srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        for p in ([port] if port == 0 else range(port, port + 10)):
            try:
                self.srv.bind((bind, p))
                break
            except OSError:
                continue
        else:
            raise OSError("no free LAN port")
        self.srv.listen(1)
        self.srv.setblocking(False)
        self.port = self.srv.getsockname()[1]
        self.udp: Optional[socket.socket] = None
        if beacon:
            try:
                self.udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                self.udp.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
            except OSError:
                self.udp = None

    def poll(self, dt: float) -> Optional[NetLink]:
        self.t -= dt
        if self.udp is not None and self.t <= 0:
            self.t = 1.0
            msg = json.dumps({"g": "TT", "name": self.name, "port": self.port, "ver": __version__}).encode()
            try:
                self.udp.sendto(msg, ("255.255.255.255", BEACON_PORT))
            except OSError:
                pass
        try:
            conn, _ = self.srv.accept()
        except (BlockingIOError, InterruptedError):
            return None
        except OSError:
            return None
        return NetLink(conn)

    def close(self) -> None:
        for s in (self.srv, self.udp):
            if s is not None:
                try:
                    s.close()
                except OSError:
                    pass


class LanBrowser:
    """Collects host beacons: {(ip, port): {'name', 'seen'}}."""

    def __init__(self, port: int = BEACON_PORT) -> None:
        self.hosts: dict = {}
        self.sock: Optional[socket.socket] = None
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            if hasattr(socket, "SO_REUSEPORT"):
                try:
                    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
                except OSError:
                    pass
            s.bind(("", port))
            s.setblocking(False)
            self.sock = s
        except OSError:
            self.sock = None

    def poll(self) -> list:
        now = time.monotonic()
        while self.sock is not None:
            try:
                data, (ip, _) = self.sock.recvfrom(2048)
                m = json.loads(data.decode("utf-8"))
                if m.get("g") == "TT":
                    self.hosts[(ip, int(m["port"]))] = {"name": str(m.get("name", "HOST"))[:16], "seen": now}
            except (BlockingIOError, InterruptedError):
                break
            except (OSError, ValueError, KeyError, TypeError):
                break
        self.hosts = {k: v for k, v in self.hosts.items() if now - v["seen"] < 4.0}
        return sorted(self.hosts.items())

    def close(self) -> None:
        if self.sock is not None:
            self.sock.close()


class Connector:
    """Non-blocking TCP connect (so the menu never freezes while a host is unreachable)."""

    def __init__(self, host: str, port: int, timeout: float = 6.0) -> None:
        self.error, self.t0, self.timeout = "", time.monotonic(), timeout
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setblocking(False)
        try:
            self.sock.connect_ex((host, port))
        except (OSError, UnicodeError) as e:
            self.error = str(e) or "bad address"

    def poll(self) -> Optional[NetLink]:
        if self.error:
            return None
        if time.monotonic() - self.t0 > self.timeout:
            self.error = "timed out"
            return None
        try:
            _, w, x = select.select([], [self.sock], [self.sock], 0)
        except (OSError, ValueError) as e:
            self.error = str(e)
            return None
        if w or x:
            err = self.sock.getsockopt(socket.SOL_SOCKET, socket.SO_ERROR)
            if err or x and not w:
                self.error = "connection refused" if err else "failed"
                return None
            return NetLink(self.sock)
        return None


def _custom_rec_for(key: str) -> Optional[dict]:
    d = CUSTOM_DESIGNS.get(key)
    return None if d is None else {"id": key[7:], "name": d.name, "lore": d.blurb, "fmt": CUSTOM_FMT,
                                   "parts": dict(zip(("turret", "hull", "track"), d.parts), barrel=d.barrel, glow=d.glow, ambient=d.ambient,
                                                  accent=next((n for n, c in COLOR_CHOICES if c == d.accent), "WHITE"))}


def player_to_json(p: PlayerSetup) -> dict:
    d = dict(vars(p))
    if p.design.startswith("custom:"):
        d["custom_rec"] = _custom_rec_for(p.design)
    d["tank_color"], d["shot_color"] = list(p.tank_color), list(p.shot_color)
    d["diff"] = p.diff.name if p.diff else None
    return d


def player_from_json(d: dict) -> PlayerSetup:
    d = dict(d)
    if isinstance(d.get("custom_rec"), dict):
        register_custom(d["custom_rec"])                           # the peer's own tank design, usable for this match only
    if d.get("design") not in DESIGN_BY_KEY:
        d["design"] = "ranger"
    d["tank_color"], d["shot_color"] = tuple(d["tank_color"]), tuple(d["shot_color"])
    d["diff"] = Difficulty[d["diff"]] if d.get("diff") else None
    return PlayerSetup(**{k: v for k, v in d.items() if k in PlayerSetup.__dataclass_fields__})


def loadout_json(lo: Loadout) -> dict:
    d = dict(vars(lo))
    if lo.tank.startswith("custom:"):
        d["custom_rec"] = _custom_rec_for(lo.tank)
    return d


def loadout_from_json(d: dict, save: "SaveData", owner: int) -> Loadout:
    """Accepts the peer's loadout but never trusts it blindly (unknown kits fall back to the starters)."""
    lo = Loadout()
    if isinstance(d.get("custom_rec"), dict):
        register_custom(d["custom_rec"])
    if d.get("tank") in DESIGN_BY_KEY:
        lo.tank = d["tank"]
    if d.get("ammo") in AMMO_BY_KEY:
        lo.ammo = d["ammo"]
    for f in ("tank_color", "shot_color"):
        if d.get(f) in COLOR_BY_NAME:
            setattr(lo, f, d[f])
    return lo


def lan_players(host_lo: Loadout, guest_lo: Loadout, hp: int) -> list:
    """Two human seats for a LAN versus match (the guest's hull colour is nudged if it matches the host's)."""
    gcol = guest_lo.tank_color
    if gcol == host_lo.tank_color:
        gcol = COLOR_CHOICES[(color_index(gcol) + 12) % len(COLOR_CHOICES)][0]
    mk = lambda lo, col, name, label, owner: PlayerSetup(name, label, COLOR_BY_NAME[col], COLOR_BY_NAME[lo.shot_color], False,
                                                         lo.tank, lo.ammo, hp, owner=owner)
    return [mk(host_lo, host_lo.tank_color, "PLAYER 1", "P1", 0), mk(guest_lo, gcol, "PLAYER 2", "P2", 1)]


class NetNoticeScene(BackdropScene):
    show_wallet = True

    def __init__(self, app, title: str, text: str) -> None:
        super().__init__(app)
        self.title, self.text = title, text

    def handle(self, ev: InputEvent) -> None:
        if ev.confirm or ev.action is Action.BACK:
            self.app.goto(MenuScene(self.app))

    def draw(self, screen: Screen) -> None:
        super().draw(screen)
        x, y = panel(screen, 56, 10, self.title, fg=Palette.DANGER)
        for k, line in enumerate(textwrap.wrap(self.text, 50)[:4]):
            screen.center(y + 2 + k, line, Palette.TEXT, Palette.PANEL, x, 56)
        if int(self.app.time * 2) % 2 == 0:
            screen.center(y + 7, "[ ENTER ]", Palette.PRIMARY, Palette.PANEL, x, 56)


class LanMenuScene(BackdropScene):
    def __init__(self, app) -> None:
        super().__init__(app)
        self.menu = MenuList(["HOST A GAME", "JOIN A GAME", "PILOT SETUP", "BACK"])

    def handle(self, ev: InputEvent) -> None:
        if ev.action is Action.BACK:
            self.app.goto(MenuScene(self.app))
            return
        c = self.menu.handle(ev)
        app = self.app
        if c == 0:
            app.goto(RulesScene(app, "LAN MATCH RULES", lambda r: app.goto(LanHostScene(app, r)), kind="lan"))
        elif c == 1:
            app.goto(LanJoinScene(app))
        elif c == 2:
            app.goto(SetupScene(app, "lan", done=lambda _: app.goto(LanMenuScene(app)), pages=1))
        elif c == 3:
            app.goto(MenuScene(app))

    def draw(self, screen: Screen) -> None:
        super().draw(screen)
        x, y = panel(screen, 56, 14, "LAN PLAY")
        screen.center(y + 2, "Play a friend on the same network.", Palette.TEXT, Palette.PANEL, x, 56)
        screen.center(y + 3, "One hosts, one joins - same rules as Two Player.", Palette.MUTED, Palette.PANEL, x, 56)
        self.menu.draw(screen, screen.cols // 2, y + 6, self.app.time, gap=1, width=26)
        screen.center(y + 11, "Your PLAYER 1 loadout is the one you bring.", Palette.MUTED, Palette.PANEL, x, 56)


class LanHostScene(BackdropScene):
    """Waiting room: shows the address, the rules and the guest; ENTER starts once someone has joined.
    `launch` (optional) builds the match for non-versus games such as co-op missions: launch(app, link, guest_lo) -> None."""

    def __init__(self, app, rules: Optional[Rules], launch: Optional[Callable] = None, title: str = "HOSTING A LAN GAME") -> None:
        super().__init__(app)
        self.rules, self.launch, self.title = rules, launch, title
        self.link: Optional[NetLink] = None
        self.guest: Optional[Loadout] = None
        self.guest_term = None
        self.err = ""
        self.ips = local_ips()
        try:
            self.host: Optional[LanHost] = LanHost("TERMINAL TANKS")
        except OSError as e:
            self.host, self.err = None, str(e)

    def exit(self) -> None:
        if self.host:
            self.host.close()

    def update(self, dt: float) -> None:
        super().update(dt)
        if self.host and self.link is None:
            self.link = self.host.poll(dt)
        if self.link is not None:
            self.link.pump()
            for m in self.link.take("hello"):
                self.guest = loadout_from_json(m.get("lo", {}), self.app.save, 1)
                self.guest_term = tuple(m.get("term", (80, 24)))
                self.link.send({"t": "welcome", "rules": self.rules.summary() if self.rules else self.title})
            if self.link.dead:
                self.link, self.guest = None, None

    def handle(self, ev: InputEvent) -> None:
        if ev.action is Action.BACK:
            if self.link:
                self.link.send({"t": "quit"})
                self.link.close()
            self.app.goto(LanMenuScene(self.app))
        elif ev.confirm and self.link and self.guest:
            self._start()

    def _start(self) -> None:
        app, link = self.app, self.link
        self.link = None
        if self.launch:
            self.launch(app, link, self.guest, self.guest_term)
            return
        r, s = self.rules, app.save
        players = lan_players(s.loadout(0), self.guest, r.hp)
        bless = bool(r.blessing and s.blessing_unlocked())              # host-authoritative: the host's save and rules decide
        if not bless:
            players = [replace(p, ammo=DEFAULT_AMMO.key) if p.ammo == BLESSING_KEY else p for p in players]
        mine = field_size(app.screen.cols, app.screen.rows)
        world = (min(mine[0], self.guest_term[0]), min(mine[1], self.guest_term[1]))
        seed = app.rng.randrange(1 << 30)
        wind = r.wind_value(app.settings)
        cfg = MatchConfig(players, False, app.settings.difficulty, "lan", target=r.target, label=r.label, net=link, my_owner=0,
                          world_size=world, special=r.special, ammo_fx=r.ammo_fx, pick_maps=r.pick_maps, blessing=bless)
        link.send({"t": "start", "kind": "versus", "players": [player_to_json(p) for p in players], "target": r.target,
                   "label": r.label, "seed": seed, "world": list(world), "wind": wind, "map": r.map_key, "special": r.special, "ammo_fx": r.ammo_fx, "pick": r.pick_maps, "blessing": bless})
        session = MatchSession(cfg, r.mapdef, seed)
        session.wind_override = wind
        app.goto(RPSScene(app, session, 0))

    def draw(self, screen: Screen) -> None:
        super().draw(screen)
        x, y = panel(screen, 60, 17, self.title)
        if self.err:
            screen.center(y + 4, "Could not open a LAN port:", Palette.DANGER, Palette.PANEL, x, 60)
            screen.center(y + 5, self.err[:50], Palette.MUTED, Palette.PANEL, x, 60)
            return
        screen.center(y + 2, "TELL YOUR FRIEND TO JOIN:", Palette.MUTED, Palette.PANEL, x, 60)
        for k, ip in enumerate(self.ips[:2]):
            screen.center(y + 3 + k, f"{ip}:{self.host.port}", GOLD, Palette.PANEL, x, 60)
        screen.center(y + 6, "(or pick this game from their JOIN list)", Palette.MUTED, Palette.PANEL, x, 60)
        if self.rules:
            screen.center(y + 8, f"{self.rules.label}  ·  {self.rules.hp} HEARTS EACH", Palette.AMBER, Palette.PANEL, x, 60)
        if self.guest:
            screen.center(y + 10, "PLAYER 2 CONNECTED", Palette.OK, Palette.PANEL, x, 60)
            screen.center(y + 11, f"{DESIGN_BY_KEY[self.guest.tank].name} + {AMMO_BY_KEY[self.guest.ammo].name}", Palette.TEXT, Palette.PANEL, x, 60)
            if int(self.app.time * 2) % 2 == 0:
                screen.center(y + 13, "[ ENTER ] START", Palette.PRIMARY, Palette.PANEL, x, 60)
        else:
            dots = "." * (int(self.app.time * 2) % 4)
            screen.center(y + 10, f"WAITING FOR PLAYER 2{dots}", Palette.AMBER, Palette.PANEL, x, 60)
        screen.center(y + 15, "ESC  CANCEL", Palette.MUTED, Palette.PANEL, x, 60)


class LanJoinScene(BackdropScene):
    """Lists games found on the network (or type an IP address), connects, then waits for the host to start."""

    def __init__(self, app) -> None:
        super().__init__(app)
        self.browser = LanBrowser()
        self.found: list = []
        self.index = 0
        self.text = ""
        self.conn: Optional[Connector] = None
        self.link: Optional[NetLink] = None
        self.msg = ""
        self.rules_text = ""

    def exit(self) -> None:
        self.browser.close()

    def _connect(self, ip: str, port: int) -> None:
        self.conn, self.msg = Connector(ip, port), ""

    def handle(self, ev: InputEvent) -> None:
        raw = ev.raw
        if ev.action is Action.BACK:
            if self.link:
                self.link.send({"t": "quit"})
                self.link.close()
            self.app.goto(LanMenuScene(self.app))
        elif self.link or self.conn:
            return
        elif raw == "BACKSPACE":
            self.text = self.text[:-1]
        elif len(raw) == 1 and (raw.isdigit() or raw in ".:") and len(self.text) < 21:
            self.text += raw
        elif ev.action is Action.UP:
            self.index = max(0, self.index - 1)
        elif ev.action is Action.DOWN:
            self.index = min(max(0, len(self.found) - 1), self.index + 1)
        elif ev.confirm:
            if self.text:
                host, _, port = self.text.partition(":")
                self._connect(host, int(port) if port.isdigit() else LAN_PORT)
            elif self.found:
                (ip, port), _ = self.found[min(self.index, len(self.found) - 1)]
                self._connect(ip, port)

    def update(self, dt: float) -> None:
        super().update(dt)
        self.found = self.browser.poll()
        if self.conn and not self.link:
            link = self.conn.poll()
            if link:
                self.link, self.conn = link, None
                lo = self.app.save.loadout(0)
                s = self.app.screen
                link.send({"t": "hello", "lo": loadout_json(lo), "term": list(field_size(s.cols, s.rows)), "ver": __version__})
            elif self.conn.error:
                self.msg, self.conn = f"COULD NOT CONNECT: {self.conn.error.upper()}", None
        if self.link:
            self.link.pump()
            for m in self.link.take("welcome"):
                self.rules_text = str(m.get("rules", ""))
            if self.link.take("quit") or self.link.dead:
                self.msg, self.link = "THE HOST CLOSED THE GAME", None
            for m in (self.link.take("start") if self.link else []):
                self._begin(m)

    def _begin(self, m: dict) -> None:
        app, link = self.app, self.link
        players = [player_from_json(p) for p in m["players"]]
        if m.get("kind") == "mission":
            mission = MISSIONS[int(m["n"]) - 1]
            sess = build_mission_session(app, mission, [], net=link, my_owner=1, world=tuple(m["world"]), seed=int(m["seed"]),
                                         humans=players, go=False)
            sess.wind_override = float(m["wind"])
            app.goto(BattleScene(app, sess))
            return
        world = tuple(m["world"])
        cfg = MatchConfig(players, False, app.settings.difficulty, "lan", target=int(m["target"]), label=str(m["label"]), net=link,
                          my_owner=1, world_size=world, special=bool(m.get("special", False)), ammo_fx=bool(m.get("ammo_fx", True)), pick_maps=bool(m.get("pick", False)),
                          blessing=bool(m.get("blessing", False)))
        session = MatchSession(cfg, all_maps().get(str(m.get("map", ""))), int(m["seed"]))
        session.wind_override = float(m["wind"])
        app.goto(RPSScene(app, session, 1))

    def draw(self, screen: Screen) -> None:
        super().draw(screen)
        x, y = panel(screen, 60, 18, "JOIN A LAN GAME")
        if self.link:
            screen.center(y + 6, "CONNECTED", Palette.OK, Palette.PANEL, x, 60)
            if self.rules_text:
                screen.center(y + 8, self.rules_text, Palette.AMBER, Palette.PANEL, x, 60)
            dots = "." * (int(self.app.time * 2) % 4)
            screen.center(y + 10, f"WAITING FOR THE HOST TO START{dots}", Palette.TEXT, Palette.PANEL, x, 60)
            screen.center(y + 15, "ESC  LEAVE", Palette.MUTED, Palette.PANEL, x, 60)
            return
        screen.text(x + 4, y + 2, "GAMES ON YOUR NETWORK", Palette.AMBER, Palette.PANEL)
        if not self.found:
            screen.text(x + 4, y + 4, "searching..." if int(self.app.time * 2) % 2 else "searching", Palette.MUTED, Palette.PANEL)
        for i, ((ip, port), info) in enumerate(self.found[:5]):
            sel = i == self.index and not self.text
            bg = Palette.PANEL_HI if sel else Palette.PANEL
            screen.fill(x + 3, y + 4 + i, 54, 1, bg)
            screen.text(x + 4, y + 4 + i, f"{'▸ ' if sel else '  '}{info['name']:<16} {ip}:{port}", Palette.WHITE if sel else Palette.TEXT, bg)
        screen.text(x + 4, y + 10, "OR TYPE AN ADDRESS", Palette.MUTED, Palette.PANEL)
        screen.box(x + 4, y + 11, 40, 3, "single", Palette.LINE, Palette.PANEL_HI)
        cursor = "█" if int(self.app.time * 2) % 2 == 0 else " "
        screen.text(x + 6, y + 12, self.text + cursor, Palette.WHITE, Palette.PANEL_HI)
        if self.conn:
            screen.center(y + 15, "CONNECTING...", Palette.AMBER, Palette.PANEL, x, 60)
        elif self.msg:
            screen.center(y + 15, self.msg[:54], Palette.DANGER, Palette.PANEL, x, 60)
        else:
            screen.center(y + 15, "W/S PICK   ENTER JOIN   ESC BACK", Palette.MUTED, Palette.PANEL, x, 60)


def reward_lines(level: TournamentLevel, compact: bool = False) -> list[tuple[str, str]]:
    """(label, name) pairs describing everything a level unlocks (compact = short labels for the roadmap panel)."""
    tank, ammo = DESIGN_BY_KEY[level.tank], AMMO_BY_KEY[level.ammo]
    if level.number == len(TOURNAMENT):
        if compact:
            return [(f"TANK {DESIGNS.index(tank) + 1}", tank.name), (f"AMMO {AMMOS.index(ammo) + 1}", ammo.name),
                    ("MAP", SPECIAL_MAP_BY_KEY[level.map_key].name.replace("THE ", "")), ("MASTER", "DIFFICULTY"), ("MASTER", "HOME SCREEN")]
        return [("MASTER TANK", tank.name), ("MASTER AMMO", ammo.name), ("MASTER MAP", SPECIAL_MAP_BY_KEY[level.map_key].name),
                ("MASTER DIFFICULTY", "SINGLE PLAYER"), ("MASTER HOME SCREEN", "TITLE + MENU")]
    out = [(f"TANK {DESIGNS.index(tank) + 1}", tank.name), (f"AMMO {AMMOS.index(ammo) + 1}", ammo.name)]
    if level.map_key:
        out.append(("MAP", SPECIAL_MAP_BY_KEY[level.map_key].name))
    return out


ACTS = ((1, "ACT I", "THE PROVING GROUNDS", (120, 225, 150)), (6, "ACT II", "THE IRON ROAD", (255, 190, 90)),
        (11, "ACT III", "THE ELITE", (130, 190, 255)), (18, "FINALE", "THE MASTER'S ROAD", GOLD))


def act_of(n: int) -> tuple:
    return [a for a in ACTS if a[0] <= n][-1]


class TournamentScene(BackdropScene):
    """The roadmap as a carousel: a progress ribbon for all 20 levels, five big cards centred on the selected level,
    and a detail panel. Each level stays locked until the one before it is cleared; cleared levels can be replayed."""
    W = 94
    CARD = ((36, 10), (14, 8), (10, 6))          # (width, height) by distance from the selection

    def __init__(self, app, selected: Optional[int] = None) -> None:
        super().__init__(app)
        save = app.save
        if selected is None:
            open_levels = [l.number for l in TOURNAMENT if save.level_unlocked(l.number) and l.number not in save.completed]
            selected = open_levels[0] if open_levels else len(TOURNAMENT)
        self.sel = selected - 1
        self.stage = PreviewStage(app.settings, 34, 16, 1)
        self.msg, self.msg_t = "", 0.0
        self.moved = 0.0

    def handle(self, ev: InputEvent) -> None:
        n = len(TOURNAMENT)
        step = 5 if ev.coarse else 1
        if ev.action is Action.BACK:
            self.app.goto(MenuScene(self.app))
        elif ev.action in (Action.LEFT, Action.UP):
            self.sel, self.moved = max(0, self.sel - step), 1.0
        elif ev.action in (Action.RIGHT, Action.DOWN):
            self.sel, self.moved = min(n - 1, self.sel + step), 1.0
        elif ev.confirm:
            lv = TOURNAMENT[self.sel]
            if self.app.save.level_unlocked(lv.number):
                self.app.goto(SetupScene(self.app, "tournament", lv))
            else:
                self.msg, self.msg_t = f"LEVEL {lv.number} IS LOCKED - CLEAR LEVEL {lv.number - 1} FIRST", 2.5

    def update(self, dt: float) -> None:
        super().update(dt)
        self.stage.update(dt)
        self.msg_t = max(0.0, self.msg_t - dt)
        self.moved = max(0.0, self.moved - dt * 4)

    def _state(self, n: int) -> str:
        save = self.app.save
        return "cleared" if n in save.completed else "open" if save.level_unlocked(n) else "locked"

    @staticmethod
    def _diff_color(lv: TournamentLevel) -> RGB:
        return GOLD if lv.difficulty is Difficulty.MASTER else {Difficulty.EASY: Palette.OK, Difficulty.NORMAL: Palette.AMBER,
                                                               Difficulty.HARD: Palette.DANGER}[lv.difficulty]

    def _edge(self, st: str, act_col: RGB, selected: bool) -> RGB:
        edge = {"cleared": act_col, "open": mix(Palette.PRIMARY, (255, 255, 255), 0.25), "locked": (62, 76, 90)}[st]
        return mix(edge, Palette.WHITE, 0.35 + 0.25 * math.sin(self.app.time * 5)) if selected and st != "locked" else edge

    def _ribbon(self, screen: Screen, x0: int, y: int) -> None:
        n, t = len(TOURNAMENT), self.app.time
        px = [x0 + 3 + round(i * (self.W - 7) / (n - 1)) for i in range(n)]
        for i in range(n - 1):
            col = act_of(i + 1)[3] if (i + 1) in self.app.save.completed else (50, 62, 74)
            for x in range(px[i] + 1, px[i + 1]):
                screen.put(x, y + 1, "─" if col != (50, 62, 74) else "┄", col)
        for i, lv in enumerate(TOURNAMENT):
            st, act = self._state(lv.number), act_of(lv.number)
            final = lv.is_final
            if st == "cleared":
                ch, col = ("★" if final else "◆"), act[3]
            elif st == "open":
                ch, col = ("☆" if final else "◇"), mix(act[3], Palette.WHITE, 0.4 + 0.3 * math.sin(t * 4 + i))
            else:
                ch, col = "·", (70, 84, 98)
            sel = i == self.sel
            screen.put(px[i], y + 1, ch, Palette.WHITE if sel else col)
            if sel:
                screen.put(px[i], y, "▼", mix(act[3], Palette.WHITE, 0.5 + 0.4 * math.sin(t * 6)))
            num = str(lv.number)
            screen.text(px[i] - len(num) // 2, y + 2, num, Palette.WHITE if sel else (110, 124, 138) if st == "locked" else col)
            if lv.rule:
                screen.put(px[i], y + 3, "!", GOLD if st != "locked" else (96, 86, 50))

    def _mini(self, lv: TournamentLevel, locked: bool) -> PixelCanvas:
        cv = PixelCanvas(12, 8, Palette.PANEL)
        draw_tank(cv, 6.0, 1, 1, 45, (70, 82, 94) if locked else COLOR_BY_NAME[lv.boss_color],
                  design=DESIGN_BY_KEY[lv.tank], t=self.app.time)
        return cv

    def _cards(self, screen: Screen, x0: int, cy: int) -> None:
        n, t = len(TOURNAMENT), self.app.time
        cx = x0 + (self.W - 36) // 2
        spots = {0: cx, -1: cx - 15, 1: cx + 37, -2: cx - 26, 2: cx + 52}
        for d in (-2, 2, -1, 1, 0):
            i = self.sel + d
            if not 0 <= i < n:
                continue
            lv, st = TOURNAMENT[i], self._state(i + 1)
            act = act_of(lv.number)
            w, h = self.CARD[abs(d)]
            x, y = spots[d], cy + (self.CARD[0][1] - h) // 2
            locked = st == "locked"
            edge = self._edge(st, act[3], d == 0)
            if d == 0:
                screen.box(x, y, w, h, "double", edge, Palette.PANEL, f"LEVEL {lv.number}", Palette.WHITE if not locked else Palette.MUTED)
                self.stage.set_kit(DESIGN_BY_KEY[lv.tank], COLOR_BY_NAME[lv.boss_color], AMMO_BY_KEY[lv.ammo],
                                   COLOR_BY_NAME[lv.boss_shot], 0.5 if locked else 0.0)
                screen.blit(self.stage.render(), x + 1, y + 1)
                badge = {"cleared": " ✓ CLEARED ", "open": " ▶ OPEN ", "locked": " ░ LOCKED "}[st]
                screen.text(x + w - len(badge) - 2, y + h - 1, badge, edge, Palette.PANEL)
                screen.text(x + 2, y + h - 1, f" {lv.boss} ", Palette.WHITE if not locked else Palette.MUTED, Palette.PANEL)
            elif abs(d) == 1:
                screen.box(x, y, w, h, "single", edge, Palette.PANEL)
                screen.center(y + 1, f"LV {lv.number}", Palette.TEXT if not locked else Palette.MUTED, Palette.PANEL, x, w)
                screen.blit(self._mini(lv, locked), x + 1, y + 2)
                screen.center(y + 6, lv.boss[:12], COLOR_BY_NAME[lv.boss_color] if not locked else (80, 92, 104), Palette.PANEL, x, w)
            else:
                screen.box(x, y, w, h, "single", edge, Palette.PANEL)
                screen.center(y + 1, f"LV {lv.number}", Palette.TEXT if not locked else Palette.MUTED, Palette.PANEL, x, w)
                dc = self._diff_color(lv)
                screen.center(y + 2, lv.difficulty.value[:4], shade(dc, 0.45) if locked else dc, Palette.PANEL, x, w)
                screen.center(y + 3, {"cleared": "✓", "open": "▶", "locked": "░"}[st], edge, Palette.PANEL, x, w)
        mid = cy + 5
        for gx in (cx - 16, cx - 1, cx + 36, cx + 51):
            near_i = {cx - 16: self.sel - 2, cx - 1: self.sel - 1, cx + 36: self.sel, cx + 51: self.sel + 1}[gx]
            if 0 <= near_i < n - 1 or (gx == cx + 36 and near_i < n - 1):
                done = (near_i + 1) in self.app.save.completed
                screen.put(gx, mid, "━" if done else "┈", act_of(near_i + 1)[3] if done else (70, 84, 98))
        if self.sel > 2:
            screen.text(x0, mid, f"◂{self.sel - 2}", Palette.MUTED)
        if self.sel < n - 3:
            right = f"{n - 3 - self.sel}▸"
            screen.text(x0 + self.W - len(right), mid, right, Palette.MUTED)

    def draw(self, screen: Screen) -> None:
        super().draw(screen)
        save, t, W = self.app.save, self.app.time, self.W
        x0, y0 = (screen.cols - W) // 2, max(1, (screen.rows - 26) // 2)
        lv = TOURNAMENT[self.sel]
        st = self._state(lv.number)
        act = act_of(lv.number)
        gold = lv.difficulty is Difficulty.MASTER
        screen.text(x0 + 1, y0, "T O U R N A M E N T", GOLD)
        title = f"{act[1]}  ·  {act[2]}"
        screen.text(x0 + (W - len(title)) // 2, y0, title, act[3])
        done = f"{len(save.completed)}/{len(TOURNAMENT)} CLEARED"
        screen.text(x0 + W - 1 - len(done), y0, done, Palette.OK if len(save.completed) == len(TOURNAMENT) else Palette.TEXT)
        self._ribbon(screen, x0, y0 + 1)
        self._cards(screen, x0, y0 + 5)
        # detail panel
        py = y0 + 16
        screen.box(x0, py, W, 9, "double", mix(act[3], (0, 0, 0), 0.25) if not gold else GOLD, Palette.PANEL,
                   f"LEVEL {lv.number} - {lv.name}", act[3])
        ax, boss_col = x0 + 3, COLOR_BY_NAME[lv.boss_color]
        rows = [("BOSS", lv.boss, boss_col), ("AI", lv.difficulty.value, self._diff_color(lv)), ("GOAL", lv.objective, Palette.TEXT),
                ("RULE", lv.rule_tag, GOLD if lv.rule else Palette.MUTED)]
        for k, (lab, val, col) in enumerate(rows):
            screen.text(ax, py + 1 + k, f"{lab:<5}", Palette.MUTED, Palette.PANEL)
            screen.text(ax + 6, py + 1 + k, val, col, Palette.PANEL)
        screen.text(ax, py + 5, lv.rule_text[:32], GOLD if lv.rule else Palette.MUTED, Palette.PANEL)
        status = {"cleared": ("✓ CLEARED - REPLAY ANY TIME", Palette.OK), "open": ("▶ READY TO FIGHT", Palette.PRIMARY),
                  "locked": (f"░ LOCKED - CLEAR LEVEL {lv.number - 1} FIRST", Palette.MUTED)}[st]
        screen.text(ax, py + 7, status[0], status[1], Palette.PANEL)
        mx = x0 + 37
        for k, line in enumerate(textwrap.wrap(lv.blurb, 28)[:2]):
            screen.text(mx, py + 1 + k, line, Palette.TEXT, Palette.PANEL)
        tank, ammo = DESIGN_BY_KEY[lv.tank], AMMO_BY_KEY[lv.ammo]
        screen.text(mx, py + 3, f"TANK  {tank.name}", Palette.TEXT, Palette.PANEL)
        screen.text(mx, py + 4, f"AMMO  {ammo.name}", Palette.AMBER, Palette.PANEL)
        if lv.map_key:
            m = SPECIAL_MAP_BY_KEY[lv.map_key]
            screen.text(mx, py + 5, f"MAP   {m.name}", m.accent, Palette.PANEL)
        label, pay = payout_state(lv, lv.number in save.completed)
        screen.text(mx, py + 6, f"{label} {pay.coins} ◉" + (f" {pay.credits} ◈" if pay.credits else ""),
                    GOLD if label == "FIRST CLEAR" else Palette.MUTED, Palette.PANEL)
        rx = x0 + 68
        screen.text(rx, py + 1, "REWARDS", GOLD, Palette.PANEL)
        earned = lv.number in save.completed
        for k, (lab, name) in enumerate(reward_lines(lv, True)):
            screen.text(rx, py + 2 + k, ("✓ " if earned else "· ") + f"{lab:<8}" + name[:13],
                        Palette.OK if earned else (Palette.TEXT if st != "locked" else Palette.MUTED), Palette.PANEL)
        if self.msg_t > 0:
            screen.center(y0 + 25, self.msg, Palette.DANGER)
        else:
            draw_keycaps(screen, screen.cols // 2 - 28, y0 + 25,
                         (("A/D", "LEVEL"), ("SHIFT", "JUMP 5"), ("ENTER", "FIGHT / REPLAY"), ("ESC", "BACK")))


def theme_preview(th: HomeTheme, t: float, hull: RGB, w: int = 34, h: int = 20) -> PixelCanvas:
    """Miniature of a home screen for the shop: sky, stars, ridge, ground, hero tank and (for Eclipse) the black sun."""
    cv = PixelCanvas(w, h, (0, 0, 0))
    for py in range(h):
        col = gradient(th.sky, clamp(1 - py / (h - 1), 0, 1))
        cv.rows[py] = [col] * w
    if th.stars:
        for k in range(26):
            sx, sy = (k * 37) % w, (k * 11) % int(h * 0.55)
            cv.blendf(sx, h - 1 - sy, (255, 255, 255), 0.35 + 0.45 * math.sin(t * 2 + k))
    if th.corona:
        for dy in range(-8, 9):
            for dx in range(-8, 9):
                d = math.hypot(dx, dy)
                if d <= 4.5:
                    cv.plot(24 + dx, 13 + dy, (2, 1, 8)) if 0 <= 24 + dx < w and 0 <= 13 + dy < h else None
                elif d <= 7:
                    cv.blendf(24 + dx, 13 + dy, hsv((math.atan2(dy, dx) / math.tau + t * 0.1) % 1, 0.55, 1.0), 0.8 * (1 - (d - 4.5) / 2.5))
    style = _MOTE_STYLES.get(th.fx)
    if style:
        for k in range(14):
            mx = (k * 53 + math.sin(t + k) * 2) % w
            my = (k * 29 + t * abs(style[1]) * 0.6 * (1 if style[1] < 0 else -1)) % h
            cv.blendf(mx, my, style[2][k % 3], 0.85)
    for x in range(w):
        rh = int(3 + 2 * math.sin(x * 0.3) + math.sin(x * 0.7))
        for y in range(rh + 3):
            cv.plot(x, y, th.far)
        for y in range(3):
            cv.plot(x, y, th.ground if y < 2 else th.ground_top)
    draw_tank(cv, 8.0, 3, 1, 40, hull, design=DESIGN_BY_KEY[th.hero], t=t)
    return cv


class ShopScene(BackdropScene):
    """Spend coins (and credits on master-tier gear). Five pages; tier rows; live preview; ENTER twice to buy."""
    CW, GAP, W = 10, 1, 94

    def __init__(self, app, page: int = 0) -> None:
        super().__init__(app)
        self.page = page
        self.pos = {p: (0, 0) for p in range(len(SHOP_PAGES))}
        self.scroll: dict = {}
        self.pending = False
        self.msg, self.msg_ok, self.msg_t = "", True, 0.0
        self.stage = PreviewStage(app.settings, 34, 20, 1)
        self.maps: dict = {}

    # -- data ----------------------------------------------------------------
    def grid(self) -> list:
        """Rows of up to five items; a row never mixes tiers, so each row has exactly one colour."""
        its = [i for i in SHOP_ITEMS if i.page == self.page]
        rows: list = []
        for t in sorted({i.tier for i in its}):
            tier_items = [i for i in its if i.tier == t]
            rows += [tier_items[k:k + 5] for k in range(0, len(tier_items), 5)]
        return rows

    def _view_rows(self, screen_rows: int) -> tuple:
        """(panel height, rows visible at once) for this terminal; the grid scrolls when there are more rows than that."""
        ph = max(21, min(41, screen_rows - 8))
        return ph, max(1, (ph - 3) // 4)

    def _fit(self, vis: int) -> None:
        r = min(self.pos[self.page][0], len(self.grid()) - 1)
        top = self.scroll.get(self.page, 0)
        if r < top:
            top = r
        elif r >= top + vis:
            top = r - vis + 1
        self.scroll[self.page] = max(0, min(top, max(0, len(self.grid()) - vis)))

    @property
    def item(self) -> ShopItem:
        g = self.grid()
        r, c = self.pos[self.page]
        r = min(r, len(g) - 1)
        return g[r][min(c, len(g[r]) - 1)]

    def _go_page(self, p: int) -> None:
        self.page, self.pending = p % len(SHOP_PAGES), False

    def handle(self, ev: InputEvent) -> None:
        g = self.grid()
        r, c = self.pos[self.page]
        if ev.action is Action.BACK:
            if self.pending:
                self.pending = False
            else:
                self.app.goto(MenuScene(self.app))
            return
        if len(ev.raw) == 1 and ev.raw in "12345":
            self._go_page(int(ev.raw) - 1)
            return
        if ev.raw == "TAB":
            self._go_page(self.page + 1)
            return
        if ev.action in (Action.LEFT, Action.RIGHT, Action.UP, Action.DOWN):
            self.pending = False
            if ev.action is Action.LEFT:
                c = max(0, c - 1)
            elif ev.action is Action.RIGHT:
                c = min(len(g[r]) - 1, c + 1)
            else:
                jump = self._view_rows(self.app.screen.rows)[1] if ev.coarse else 1      # SHIFT+W/S pages through the rows
                r = max(0, r - jump) if ev.action is Action.UP else min(len(g) - 1, r + jump)
            self.pos[self.page] = (r, min(c, len(g[r]) - 1))
            self._fit(self._view_rows(self.app.screen.rows)[1])
            return
        if ev.confirm and self.item.uid == "ticket":
            self._ticket()
            return
        if ev.confirm and self.item.uid == "timemachine":
            self._martyr()
            return
        if ev.confirm:
            it, save = self.item, self.app.save
            if save.is_unlocked(it.uid):
                self.msg, self.msg_ok, self.msg_t = "YOU ALREADY OWN THIS", False, 2.0
            elif save.shortfall(it):
                self.pending = False
                self.msg, self.msg_ok, self.msg_t = save.shortfall(it), False, 2.5
            elif not self.pending:
                self.pending = True
            else:
                why = save.buy(it)
                self.pending = False
                self.msg, self.msg_ok, self.msg_t = (why, False, 2.5) if why else (f"PURCHASED: {it.name}", True, 3.0)
                if not why:
                    self.app._wallet_flash = 1.4

    def _martyr(self) -> None:
        save, it = self.app.save, self.item
        if save.martyr_won:
            self.msg, self.msg_ok, self.msg_t = "THE MARTYR RESTS. REPLAY THE DUEL? PRESS ENTER AGAIN", True, 2.5
            if self.pending:
                self.pending = False
                start_martyr(self.app, replay=True)
            else:
                self.pending = True
        elif not save.martyr_available():
            self.msg, self.msg_ok, self.msg_t = "DEFEAT ALL 10 VAULT BOSSES FIRST", False, 2.5
        elif save.martyr_paid:
            start_martyr(self.app)
        elif save.shortfall(it):
            self.pending = False
            self.msg, self.msg_ok, self.msg_t = save.shortfall(it), False, 2.5
        elif not self.pending:
            self.pending = True
        else:
            self.pending = False
            self.app._wallet_flash = 1.4
            if save.buy_martyr(it):
                start_martyr(self.app, intro=True)

    def _ticket(self) -> None:
        save, it = self.app.save, self.item
        if not save.tickets_open():
            self.pending = False
            self.msg, self.msg_ok, self.msg_t = "SEALED - BEAT THE MASTER AND THE FINAL MISSION", False, 3.0
        elif save.tickets_sold_out():
            self.msg, self.msg_ok, self.msg_t = "SOLD OUT - EVERY BOSS IS DEFEATED", False, 2.5
        elif save.ticket_active:                              # a drawn boss is waiting: no new charge
            start_ticket(self.app, save.ticket_active)
        elif save.shortfall(it):
            self.pending = False
            self.msg, self.msg_ok, self.msg_t = save.shortfall(it), False, 2.5
        elif not self.pending:
            self.pending = True
        else:
            self.pending = False
            self.app._wallet_flash = 1.4
            start_ticket(self.app, save.buy_ticket(it))

    def update(self, dt: float) -> None:
        super().update(dt)
        self.stage.update(dt)
        self.msg_t = max(0.0, self.msg_t - dt)

    # -- preview -------------------------------------------------------------
    def _map_preview(self, m: MapDefinition) -> PixelCanvas:
        if m.key not in self.maps:
            rng = random.Random(1)
            terrain = Terrain(34, 20, m, rng)
            tanks = []
            for i in range(2):
                x = int(m.spawns[i] * 34)
                terrain.flatten(x + 0.5, 10)
                tanks.append(x + 0.5)
            scen = Scenery(34, 20, m.theme, rng)
            scen.apply_terrain(terrain)
            self.maps[m.key] = (scen, terrain, tanks)
        scen, terrain, tanks = self.maps[m.key]
        t, cv = self.app.time, scen.canvas()
        scen.draw_stars(cv, t)
        scen.draw_aurora(cv, t)
        scen.draw_fx(cv, t)
        for i, x in enumerate(tanks):
            draw_tank(cv, x, terrain.support_height(x), 1 if i == 0 else -1, 50 if i == 0 else 130, (230, 190, 90) if i == 0 else (90, 190, 230),
                      t=t)
        return cv

    def _preview(self, it: ShopItem) -> PixelCanvas:
        """Previews always use the game's first tank, first ammo and default colours - never the player's loadout - so an
        RGB shell is not hidden by an RGB-coloured current setup, and every ammo is judged on the same stage."""
        design, ammo = DESIGN_BY_KEY["ranger"], AMMO_BY_KEY["standard"]
        hull, shot = COLOR_BY_NAME["CYAN"], COLOR_BY_NAME["YELLOW"]
        kind, _, key = it.uid.partition(":")
        if it.uid == "ticket":
            return ticket_art(self.app.save, self.app.time)
        if it.uid == "timemachine":
            return martyr_art(self.app.save, self.app.time)
        if kind == "map":
            return self._map_preview(SPECIAL_MAP_BY_KEY[key])
        if kind == "theme":
            return theme_preview(HOME_THEMES[key.upper()], self.app.time, hull)
        if kind == "tank":
            design = DESIGN_BY_KEY[key]
        elif kind == "ammo":
            ammo = AMMO_BY_KEY[key]
        elif kind == "paint":
            hull = shot = PAINT_BY_KEY[key]
        elif key == "hull":
            hull = RAINBOW
        elif key == "shell":
            shot = RAINBOW
        self.stage.set_kit(design, hull, ammo, shot, 0.0)
        return self.stage.render()

    # -- drawing -------------------------------------------------------------
    def _price(self, it: ShopItem) -> str:
        return f"{it.coins}◉" + (f" {it.credits}◈" if it.credits else "")

    KIND = {"tank": "TANK", "ammo": "AMMO", "map": "MAP", "theme": "MAIN SCREEN", "rgb": "EXTRA", "paint": "EXTRA", "ticket": "TICKET", "timemachine": "ARTIFACT"}

    def draw(self, screen: Screen) -> None:
        super().draw(screen)
        save, t, W = self.app.save, self.app.time, self.W
        ph, vis = self._view_rows(screen.rows)
        x0, y0 = (screen.cols - W) // 2, max(1, (screen.rows - ph - 3) // 2)
        screen.text(x0 + 1, y0, "S H O P", GOLD)
        tx = x0 + 14
        for i, name in enumerate(SHOP_PAGES):
            lab = f" {i + 1} {name} "
            sel = i == self.page
            screen.text(tx, y0, lab, Palette.INK if sel else Palette.MUTED, GOLD if sel else Palette.PANEL)
            tx += len(lab) + 1
        screen.fill(x0, y0 + 1, W, 1, Palette.BG, "─", Palette.LINE)
        grid = self.grid()
        self._fit(vis)
        top = self.scroll.get(self.page, 0)
        sel_r, sel_c = self.pos[self.page]
        sel_r = min(sel_r, len(grid) - 1)
        title = SHOP_PAGES[self.page] + (f"  ·  ROW {sel_r + 1}/{len(grid)}" if len(grid) > vis else "")
        screen.box(x0, y0 + 2, 57, ph, "double", Palette.LINE, Palette.PANEL, title)
        for k in range(vis):                                  # the visible window of rows; tier colour alone marks the tier
            r = top + k
            if r >= len(grid):
                break
            row, gy = grid[r], y0 + 4 + k * 4
            col = TIER_COLORS[row[0].tier]
            for dy in (0, 1):
                bar = rainbow(t + r * 0.1 + dy * 0.05) if row[0].tier >= 4 else col      # the premium rows shimmer
                screen.put(x0 + 1, gy + dy, "▌", bar, Palette.PANEL)
            for c, it in enumerate(row):
                x = x0 + 2 + c * (self.CW + self.GAP)
                sel = (r, c) == (sel_r, sel_c)
                owned = save.is_unlocked(it.uid)
                bg = Palette.PANEL_HI if sel else Palette.PANEL
                screen.fill(x, gy, self.CW, 2, bg)
                name = it.name if len(it.name) <= self.CW else it.name[:self.CW - 1] + "…"
                screen.text(x, gy, name, Palette.WHITE if sel else (Palette.OK if owned else col), bg)
                if it.uid == "ticket":
                    price, pc = (("SOLD OUT", Palette.MUTED) if owned else ("SEALED", Palette.MUTED) if not save.tickets_open()
                                 else (("RESUME" if save.ticket_active else self._price(it)), Palette.AMBER))
                elif it.uid == "timemachine":
                    price, pc = (("✓ BEATEN", Palette.OK) if owned else ("LOCKED", Palette.MUTED) if not save.martyr_available()
                                 else (("RESUME", Palette.AMBER) if save.martyr_paid else (self._price(it), Palette.AMBER)))
                elif owned:
                    price, pc = "✓ OWNED", Palette.OK
                else:
                    price = self._price(it)
                    pc = Palette.AMBER if not save.shortfall(it) else (200, 90, 90)
                screen.text(x, gy + 1, price, pc, bg)
                if sel:
                    screen.put(x - 1, gy, "▸", GOLD, Palette.PANEL)
        if len(grid) > vis:                                   # scroll bar
            track = ph - 3
            thumb = max(1, track * vis // len(grid))
            pos = (track - thumb) * top // max(1, len(grid) - vis)
            for k in range(track):
                screen.put(x0 + 56, y0 + 3 + k, "█" if pos <= k < pos + thumb else "│", GOLD if pos <= k < pos + thumb else Palette.LINE, Palette.PANEL)
            if top > 0:
                screen.put(x0 + 56, y0 + 2, "▲", GOLD, Palette.PANEL)
            if top + vis < len(grid):
                screen.put(x0 + 56, y0 + 2 + ph - 1, "▼", GOLD, Palette.PANEL)
        # detail panel (works for the selected item even when its row is scrolled out of view)
        it = self.item
        px, py = x0 + 58, y0 + 2
        col = TIER_COLORS[it.tier]
        owned = save.is_unlocked(it.uid)
        screen.box(px, py, 36, 21, "double", rainbow(t) if it.tier >= 4 else col, Palette.PANEL, "ITEM")
        screen.blit(self._preview(it), px + 1, py + 1)
        screen.center(py + 11, it.name, Palette.WHITE, Palette.PANEL, px, 36)
        screen.center(py + 12, self.KIND[it.uid.split(":")[0]] + ("  ·  PREMIUM" if it.tier >= 4 else ""), col, Palette.PANEL, px, 36)
        for k, line in enumerate(textwrap.wrap(it.blurb, 32)[:4]):
            screen.center(py + 13 + k, line, Palette.TEXT, Palette.PANEL, px, 36)
        if it.uid == "ticket":
            sub = "SOLD OUT" if owned else "SEALED: BEAT THE MASTER + FINAL MISSION" if not save.tickets_open() else (f"ACTIVE BOSS: {TICKET_BOSS_BY_KEY[save.ticket_active].name}" if save.ticket_active
                                          else f"PRICE  {it.coins} ◉  {it.credits} ◈")
            screen.center(py + 17, sub, Palette.OK if owned else GOLD, Palette.PANEL, px, 36)
        elif it.uid == "timemachine":
            sub = ("DUEL COMPLETE" if owned else "DEFEAT ALL 10 VAULT BOSSES TO UNLOCK" if not save.martyr_available()
                   else f"PHASE {save.martyr_phase + 1}/3 - RESUME" if save.martyr_paid else f"PRICE  {it.coins:,} ◉  {it.credits} ◈")
            screen.center(py + 17, sub, Palette.OK if owned else GOLD if save.martyr_available() else Palette.MUTED, Palette.PANEL, px, 36)
        else:
            screen.center(py + 17, "OWNED" if owned else f"PRICE  {it.coins} ◉" + (f"  {it.credits} ◈" if it.credits else ""),
                          Palette.OK if owned else GOLD, Palette.PANEL, px, 36)
        screen.center(py + 18, f"YOU HAVE  {save.coins:,} ◉  {save.credits:,} ◈", Palette.MUTED, Palette.PANEL, px, 36)
        if self.msg_t > 0:
            screen.center(py + 19, self.msg, Palette.OK if self.msg_ok else Palette.DANGER, Palette.PANEL, px, 36)
        elif it.uid == "timemachine":
            if self.pending and int(t * 3) % 2 == 0:
                screen.center(py + 19, "ENTER AGAIN TO CONTINUE  ·  ESC CANCEL", Palette.AMBER, Palette.PANEL, px, 36)
            elif not self.pending:
                screen.center(py + 19, "[ENTER] " + ("REPLAY" if owned else "RESUME" if save.martyr_paid else "BUY"), Palette.PRIMARY, Palette.PANEL, px, 36)
        elif it.uid == "ticket" and not owned and not save.tickets_open():
            screen.center(py + 19, "[ENTER] WHY IS IT SEALED?", Palette.MUTED, Palette.PANEL, px, 36)
        elif it.uid == "ticket" and not owned:
            n = len(save.tickets_won & set(TICKET_BOSS_BY_KEY))
            if self.pending and int(t * 3) % 2 == 0:
                screen.center(py + 19, "ENTER AGAIN TO BUY  ·  ESC CANCEL", Palette.AMBER, Palette.PANEL, px, 36)
            elif not self.pending:
                screen.center(py + 19, f"[ENTER] {'RESUME FIGHT' if save.ticket_active else 'BUY'}   {n}/{len(TICKET_BOSS_BY_KEY)} BEATEN",
                              Palette.PRIMARY, Palette.PANEL, px, 36)
        elif owned:
            hint = {"tank": "PICK IT IN PILOT SETUP", "ammo": "PICK IT IN PILOT SETUP", "map": "PICK IT IN BATTLEFIELD",
                    "theme": "SET IT IN SETTINGS OR PRESS H", "rgb": "PICK IT IN PILOT SETUP", "paint": "PICK IT IN PILOT SETUP OR THE WORKSHOP", "ticket": "ALL BOSSES DEFEATED"}[it.uid.split(":")[0]]
            screen.center(py + 19, hint, Palette.MUTED, Palette.PANEL, px, 36)
        elif self.pending:
            if int(t * 3) % 2 == 0:
                screen.center(py + 19, "ENTER AGAIN TO BUY  ·  ESC CANCEL", Palette.AMBER, Palette.PANEL, px, 36)
        else:
            why = save.shortfall(it)
            screen.center(py + 19, why if why else "[ENTER] BUY", (200, 90, 90) if why else Palette.PRIMARY, Palette.PANEL, px, 36)
        draw_keycaps(screen, x0 + 2, y0 + ph + 2, (("1-5", "PAGE"), ("TAB", "NEXT"), ("WASD", "SELECT"), ("SHIFT+W/S", "PAGE UP/DN"), ("ENTER", "BUY"), ("ESC", "BACK")))


# ============================================================================
# Missions screens
# ============================================================================
M_ACTS = ((1, "ACT I", "BASIC TRAINING", (120, 225, 150)), (11, "ACT II", "THE FIELD", (255, 190, 90)),
          (21, "ACT III", "HOT ZONES", (255, 120, 100)), (31, "ACT IV", "THE ELITE", (130, 190, 255)),
          (41, "ACT V", "THE ALIEN THREAT", (120, 255, 200)))
M_TYPE_COLORS = {"marathon": (130, 225, 140), "2v1": (255, 180, 90), "ufo": (120, 255, 200), "coop": (130, 190, 255),
                 "rush": (255, 110, 100), "mother": GOLD}
M_TYPE_SHORT = {"marathon": "RUN", "2v1": "2v1", "ufo": "UFO", "coop": "CO-OP", "rush": "RUSH", "mother": "BOSS"}


def reward_label(uid: str) -> tuple:
    kind, _, key = uid.partition(":")
    if kind == "tank":
        return "TANK", DESIGN_BY_KEY[key].name
    if kind == "ammo":
        return "AMMO", AMMO_BY_KEY[key].name
    if kind == "map":
        return "MAP", SPECIAL_MAP_BY_KEY[key].name
    if kind == "theme":
        return "SCREEN", HOME_THEMES[key.upper()].name if key.upper() in HOME_THEMES else key.upper()
    return kind.upper(), key.upper()


def _fake_tank(i: int, color: RGB, x: float, y: float, facing: int, **kw) -> Tank:
    return Tank(i, "", "", color, (255, 255, 255), False, x, y, facing, 50.0 if facing > 0 else 130.0, **kw)


def mission_art(m: Mission, t: float, locked: bool) -> PixelCanvas:
    """The picture on a mission's roadmap card: the kind of fight it is."""
    cv = PixelCanvas(34, 16, (0, 0, 0))
    for py in range(16):
        cv.rows[py] = [mix((6, 10, 24), (30, 40, 70), py / 15)] * 34
    for x in range(34):
        for y in range(3):
            cv.plot(x, y, (40, 60, 50) if not locked else (40, 46, 52))
    dim = (70, 80, 92)
    pc = lambda c: dim if locked else c
    ally, foe = (110, 200, 235), (235, 120, 100)
    if m.kind in ("ufo", "mother") or (m.kind == "coop" and m.coop_ufo):
        big = 2.2 if m.kind == "mother" else 2.0
        u = _fake_tank(0, pc((90, 230, 160)), 25.0, 7.0, -1, kind="ufo", scale=big, hover=4.0, hp=5, max_hp=5,
                       shield=1 if m.kind != "ufo" else 0)
        draw_ufo(cv, u, t)
        if m.kind == "mother":
            for k in range(2):
                d = _fake_tank(k + 1, pc((190, 140, 255)), 8.0 + k * 9, 9.0, -1, kind="ufo", scale=1.3, hover=0, hp=1, max_hp=1)
                draw_ufo(cv, d, t + k)
        if m.is_coop:
            draw_tank(cv, 4.0, 3, 1, 55, pc(ally), t=t)
            draw_tank(cv, 11.0, 3, 1, 55, pc((235, 200, 110)), t=t)
        elif m.kind == "ufo":
            draw_tank(cv, 6.0, 3, 1, 60, pc(ally), t=t)
    elif m.kind == "marathon":
        for k in range(3):
            draw_tank(cv, 5.0 + k * 8, 3, 1, 50, pc(mix(ally, (255, 255, 255), 0.15 * k)), t=t)
        draw_tank(cv, 29.0, 3, -1, 130, pc(foe), t=t)
        cv.plotf(18, 12, pc(GOLD))
    elif m.kind == "rush":
        for k, lvn in enumerate(m.levels[:4] if len(m.levels) > 4 else m.levels):
            lv = TOURNAMENT[lvn - 1]
            draw_tank(cv, 5.0 + k * 8.5, 3, -1 if k else 1, 130, pc(COLOR_BY_NAME[lv.boss_color]), design=DESIGN_BY_KEY[lv.tank], t=t)
    else:
        draw_tank(cv, 5.0, 3, 1, 50, pc(ally), t=t)
        if m.is_coop:
            draw_tank(cv, 13.0, 3, 1, 50, pc((235, 200, 110)), t=t)
        for k in range(m.enemies):
            draw_tank(cv, 29.0 - k * 9, 3, -1, 130, pc(foe if k == 0 else (235, 170, 90)), t=t)
    return cv


class MissionsScene(BackdropScene):
    """The mission roadmap: a 50-pip progress ribbon, five cards around the selected mission and a detail panel."""
    W = 94
    CARD = ((36, 10), (14, 8), (10, 6))

    def __init__(self, app, selected: Optional[int] = None) -> None:
        super().__init__(app)
        save = app.save
        if selected is None:
            todo = [m.number for m in MISSIONS if save.mission_unlocked(m.number) and m.number not in save.mission_completed]
            selected = todo[0] if todo else len(MISSIONS)
        self.sel = selected - 1
        self.msg, self.msg_t = "", 0.0

    def handle(self, ev: InputEvent) -> None:
        n, step = len(MISSIONS), (5 if ev.coarse else 1)
        if ev.action is Action.BACK:
            self.app.goto(MenuScene(self.app))
        elif ev.action in (Action.LEFT, Action.UP):
            self.sel = max(0, self.sel - step)
        elif ev.action in (Action.RIGHT, Action.DOWN):
            self.sel = min(n - 1, self.sel + step)
        elif ev.confirm:
            m = MISSIONS[self.sel]
            if not self.app.save.mission_unlocked(m.number):
                self.msg, self.msg_t = f"MISSION {m.number} IS LOCKED - CLEAR MISSION {m.number - 1} FIRST", 2.5
            elif m.is_coop:
                self.app.goto(CoopChoiceScene(self.app, m))
            else:
                scene = SetupScene(self.app, "mission", pages=1, done=lambda los, m=m: build_mission_session(self.app, m, los))
                scene.back = MissionsScene(self.app, m.number)
                self.app.goto(scene)

    def update(self, dt: float) -> None:
        super().update(dt)
        self.msg_t = max(0.0, self.msg_t - dt)

    def _state(self, n: int) -> str:
        s = self.app.save
        return "cleared" if n in s.mission_completed else "open" if s.mission_unlocked(n) else "locked"

    @staticmethod
    def _act(n: int) -> tuple:
        return [a for a in M_ACTS if a[0] <= n][-1]

    def _ribbon(self, screen: Screen, x0: int, y: int) -> None:
        """50 pips in two evenly spaced rows of 25 (3 columns apart); the selected one is bracketed."""
        n, t = len(MISSIONS), self.app.time
        per, stride = (n + 1) // 2, 3
        start = x0 + (self.W - per * stride) // 2 + 1
        for i, m in enumerate(MISSIONS):
            st = self._state(m.number)
            col = M_TYPE_COLORS[m.kind]
            if st == "cleared":
                ch, c = "◆", col
            elif st == "open":
                ch, c = "◇", mix(col, Palette.WHITE, 0.4 + 0.3 * math.sin(t * 4 + i))
            else:
                ch, c = "·", (70, 84, 98)
            if m.rewards and st != "locked":
                ch = "★" if st == "cleared" else "☆"
            row, k = divmod(i, per)
            px, py = start + k * stride, y + row
            if i == self.sel:
                hl = mix(col, Palette.WHITE, 0.5 + 0.4 * math.sin(t * 6))
                screen.put(px - 1, py, "[", hl)
                screen.put(px + 1, py, "]", hl)
                screen.put(px, py, ch, Palette.WHITE)
            else:
                screen.put(px, py, ch, c)
        for row, (a, b) in enumerate(((1, per), (per + 1, n))):
            screen.text(x0 + 1, y + row, f"{a:02d}", (110, 124, 138))
            screen.text(x0 + self.W - 3, y + row, f"{b:02d}", (110, 124, 138))

    def _cards(self, screen: Screen, x0: int, cy: int) -> None:
        n, t = len(MISSIONS), self.app.time
        cx = x0 + (self.W - 36) // 2
        spots = {0: cx, -1: cx - 15, 1: cx + 37, -2: cx - 26, 2: cx + 52}
        for d in (-2, 2, -1, 1, 0):
            i = self.sel + d
            if not 0 <= i < n:
                continue
            m, st = MISSIONS[i], self._state(i + 1)
            col, locked = M_TYPE_COLORS[m.kind], st == "locked"
            w, h = self.CARD[abs(d)]
            x, y = spots[d], cy + (self.CARD[0][1] - h) // 2
            edge = {"cleared": col, "open": mix(Palette.PRIMARY, (255, 255, 255), 0.25), "locked": (62, 76, 90)}[st]
            if d == 0 and st != "locked":
                edge = mix(edge, Palette.WHITE, 0.35 + 0.25 * math.sin(t * 5))
            if d == 0:
                screen.box(x, y, w, h, "double", edge, Palette.PANEL, f"MISSION {m.number}", Palette.WHITE if not locked else Palette.MUTED)
                screen.blit(mission_art(m, t, locked), x + 1, y + 1)
                badge = {"cleared": " ✓ CLEARED ", "open": " ▶ OPEN ", "locked": " ░ LOCKED "}[st]
                screen.text(x + w - len(badge) - 2, y + h - 1, badge, edge, Palette.PANEL)
                screen.text(x + 2, y + h - 1, f" {m.type_name} ", col if not locked else Palette.MUTED, Palette.PANEL)
            else:
                screen.box(x, y, w, h, "single", edge, Palette.PANEL)
                screen.center(y + 1, f"M{m.number}", Palette.TEXT if not locked else Palette.MUTED, Palette.PANEL, x, w)
                screen.center(y + 2, M_TYPE_SHORT[m.kind], shade(col, 0.45) if locked else col, Palette.PANEL, x, w)
                screen.center(y + 3, {"cleared": "✓", "open": "▶", "locked": "░"}[st], edge, Palette.PANEL, x, w)
                if abs(d) == 1:
                    screen.center(y + 5, m.name[:12], (80, 92, 104) if locked else Palette.TEXT, Palette.PANEL, x, w)
                    if m.rewards:
                        screen.center(y + 6, "★ REWARD", GOLD if not locked else (96, 86, 50), Palette.PANEL, x, w)
        if self.sel > 2:
            screen.text(x0, cy + 5, f"◂{self.sel - 2}", Palette.MUTED)
        if self.sel < n - 3:
            right = f"{n - 3 - self.sel}▸"
            screen.text(x0 + self.W - len(right), cy + 5, right, Palette.MUTED)

    def draw(self, screen: Screen) -> None:
        super().draw(screen)
        save, t, W = self.app.save, self.app.time, self.W
        x0, y0 = (screen.cols - W) // 2, max(1, (screen.rows - 26) // 2)
        m = MISSIONS[self.sel]
        st, act, col = self._state(m.number), self._act(m.number), M_TYPE_COLORS[m.kind]
        screen.text(x0 + 1, y0, "M I S S I O N S", GOLD)
        title = f"{act[1]}  ·  {act[2]}"
        screen.text(x0 + (W - len(title)) // 2, y0, title, act[3])
        done = f"{len(save.mission_completed)}/{len(MISSIONS)} CLEARED"
        screen.text(x0 + W - 1 - len(done), y0, done, Palette.OK if len(save.mission_completed) == len(MISSIONS) else Palette.TEXT)
        self._ribbon(screen, x0, y0 + 1)
        self._cards(screen, x0, y0 + 4)
        py = y0 + 14
        screen.box(x0, py, W, 11, "double", mix(col, (0, 0, 0), 0.3), Palette.PANEL, f"MISSION {m.number} - {m.name}", col)
        ax = x0 + 3
        foe = {"marathon": f"{m.stages} AI RIVALS", "2v1": "2 AI TANKS", "ufo": "1 ALIEN SAUCER", "coop": "2 AI TANKS" if not m.coop_ufo else "1 ALIEN SAUCER",
               "rush": f"{m.stages} TOURNAMENT BOSSES", "mother": "MOTHERSHIP + 2 DRONES"}[m.kind]
        rows = [("TYPE", m.type_name, col), ("FOES", foe, Palette.TEXT)]
        for k, (lab, val, c) in enumerate(rows):
            screen.text(ax, py + 1 + k, f"{lab:<5}", Palette.MUTED, Palette.PANEL)
            screen.text(ax + 6, py + 1 + k, val, c, Palette.PANEL)
        goal = textwrap.wrap(m.objective, 30)[:2]
        screen.text(ax, py + 3, "GOAL", Palette.MUTED, Palette.PANEL)
        for k, ln in enumerate(goal):
            screen.text(ax + 6, py + 3 + k, ln, Palette.TEXT, Palette.PANEL)
        ai = m.diff.value if m.kind != "marathon" or m.diff is m.diff_end else f"{m.diff.value} > {m.diff_end.value}"
        screen.text(ax, py + 5, "AI", Palette.MUTED, Palette.PANEL)
        screen.text(ax + 6, py + 5, ai, Palette.AMBER, Palette.PANEL)
        for k, ln in enumerate(textwrap.wrap(m.rule_text, 34)[:2]):
            screen.text(ax, py + 6 + k, ln, GOLD, Palette.PANEL)
        status = {"cleared": ("✓ CLEARED - REPLAY ANY TIME", Palette.OK), "open": ("▶ READY", Palette.PRIMARY),
                  "locked": (f"░ LOCKED - CLEAR MISSION {m.number - 1} FIRST", Palette.MUTED)}[st]
        screen.text(ax, py + 9, status[0][:36], status[1], Palette.PANEL)
        mx = x0 + 40
        for k, line in enumerate(textwrap.wrap(m.blurb, 28)[:5]):
            screen.text(mx, py + 1 + k, line, Palette.TEXT, Palette.PANEL)
        first_left = m.number not in save.mission_paid
        screen.text(mx, py + 7, f"{'FIRST CLEAR' if first_left else 'REPLAY'}  {m.first_coins if first_left else m.repeat_coins} ◉",
                    GOLD if first_left else Palette.MUTED, Palette.PANEL)
        if m.is_coop:
            screen.text(mx, py + 9, "LOCAL · LAN · AI WINGMAN", (130, 190, 255), Palette.PANEL)
        rx = x0 + 70
        screen.text(rx, py + 1, "GRAND PRIZE" if m.number == len(MISSIONS) else "REWARDS", GOLD, Palette.PANEL)
        earned = m.number in save.mission_completed
        for k, uid in enumerate(m.rewards[:5]):
            lab, name = reward_label(uid)
            screen.text(rx, py + 2 + k, ("✓ " if earned else "· ") + f"{lab:<7}" + name[:11], Palette.OK if earned else Palette.TEXT, Palette.PANEL)
        if not m.rewards:
            screen.text(rx, py + 2, "· COINS ONLY", Palette.MUTED, Palette.PANEL)
        if self.msg_t > 0:
            screen.center(y0 + 25, self.msg, Palette.DANGER)
        else:
            draw_keycaps(screen, screen.cols // 2 - 28, y0 + 25,
                         (("A/D", "MISSION"), ("SHIFT", "JUMP 5"), ("ENTER", "START"), ("ESC", "BACK")))


class CoopChoiceScene(BackdropScene):
    """How to play a co-op mission: two players on this machine, hosting a LAN game, or alone with an AI wingman."""

    def __init__(self, app, mission: Mission) -> None:
        super().__init__(app)
        self.m = mission
        self.menu = MenuList(["LOCAL TWO PLAYER", "HOST OVER LAN", "SOLO + AI WINGMAN", "BACK"])

    def handle(self, ev: InputEvent) -> None:
        app, m = self.app, self.m
        if ev.action is Action.BACK:
            app.goto(MissionsScene(app, m.number))
            return
        c = self.menu.handle(ev)
        if c == 0:
            sc = SetupScene(app, "mission", pages=2, done=lambda los: build_mission_session(app, m, los))
            sc.back = CoopChoiceScene(app, m)
            app.goto(sc)
        elif c == 1 and IS_WEB:
            app.goto(NetNoticeScene(app, "LAN DISABLED ON WEB", WEB_LAN_TEXT))
        elif c == 1:
            def after(los):
                def launch(app_, link, guest, guest_term):
                    mine = field_size(app_.screen.cols, app_.screen.rows)
                    world = (min(mine[0], guest_term[0]), min(mine[1], guest_term[1]))
                    seed = app_.rng.randrange(1 << 30)
                    humans = humans_for(app_, m, los, guest=guest)
                    link.send({"t": "start", "kind": "mission", "n": m.number, "players": [player_to_json(h) for h in humans],
                               "seed": seed, "world": list(world), "wind": app_.settings.wind})
                    build_mission_session(app_, m, los, net=link, my_owner=0, world=world, seed=seed, humans=humans)
                app.goto(LanHostScene(app, None, launch, f"CO-OP MISSION {m.number}"))
            sc = SetupScene(app, "mission", pages=1, done=after)
            sc.back = CoopChoiceScene(app, m)
            app.goto(sc)
        elif c == 2:
            sc = SetupScene(app, "mission", pages=1, done=lambda los: build_mission_session(app, m, los, wingman=True))
            sc.back = CoopChoiceScene(app, m)
            app.goto(sc)
        elif c == 3:                                  # (MenuList returns None for plain navigation keys: never treat that as BACK)
            app.goto(MissionsScene(app, m.number))

    def draw(self, screen: Screen) -> None:
        super().draw(screen)
        x, y = panel(screen, 60, 16, f"CO-OP MISSION {self.m.number}: {self.m.name}")
        screen.center(y + 2, self.m.objective, Palette.TEXT, Palette.PANEL, x, 60)
        self.menu.draw(screen, screen.cols // 2, y + 5, self.app.time, gap=1, width=30)
        screen.center(y + 10, "LAN: both players earn coins and the kit;", Palette.MUTED, Palette.PANEL, x, 60)
        screen.center(y + 11, "only the host's mission progress advances.", Palette.MUTED, Palette.PANEL, x, 60)
        screen.center(y + 12, "LOCAL: the one save on this machine earns it all.", Palette.MUTED, Palette.PANEL, x, 60)


class MissionResultScene(BackdropScene):
    def __init__(self, app, session: MissionSession) -> None:
        super().__init__(app)
        self.session, self.m, self.t = session, session.mission, 0.0

    def enter(self) -> None:
        self.app._wallet_flash = 1.6
        if self.session.completed:
            self.app.backdrop.celebrate_with([M_TYPE_COLORS[self.m.kind], GOLD, Palette.WHITE])

    def exit(self) -> None:
        self.app.backdrop.celebrate_with(None)

    def handle(self, ev: InputEvent) -> None:
        s, net = self.session, self.session.config.net
        if ev.raw in ("r", "R") and not s.completed and net is None:
            s.restart(self.app)
        elif ev.confirm:
            if net is not None:
                net.send({"t": "quit"})
                net.close()
            nxt = self.m.number + 1 if (s.completed and self.m.number < len(MISSIONS) and net is None) else self.m.number
            go = lambda: self.app.goto(MissionsScene(self.app, nxt))
            m = self.m
            if net is not None:
                go()
            elif m.kind == "mother" and s.completed:
                whisper(self.app, "mothership", go)
            elif m.kind == "ufo" or (m.kind == "coop" and m.coop_ufo):
                whisper(self.app, "alien", go)
            else:
                go()

    def update(self, dt: float) -> None:
        super().update(dt)
        self.t += dt

    def draw(self, screen: Screen) -> None:
        super().draw(screen)
        s, m, t = self.session, self.m, self.t
        ok = s.completed
        col = M_TYPE_COLORS[m.kind]
        x, y = panel(screen, 64, 20, f"MISSION {m.number}", fg=col if ok else Palette.DANGER)
        head = "M I S S I O N   C O M P L E T E" if ok else "M I S S I O N   F A I L E D"
        if ok:
            prism_text(screen, x + (64 - len(head)) // 2, y + 2, head, t, 0.12, 0.05, 0.55, 1.0, Palette.PANEL)
        else:
            screen.center(y + 2, head, Palette.DANGER, Palette.PANEL, x, 64)
        screen.center(y + 3, m.name, col, Palette.PANEL, x, 64)
        if s.payout:
            screen.center(y + 5, ("FIRST CLEAR  " if s.first_clear else "REPLAY  " if ok else "CONSOLATION  ") + s.payout.text(), GOLD,
                          Palette.PANEL, x, 64)
        if ok and s.first_clear and m.rewards:
            screen.center(y + 7, "GRAND PRIZE UNLOCKED" if m.number == len(MISSIONS) else "UNLOCKED", GOLD if m.number == len(MISSIONS) else Palette.OK,
                          Palette.PANEL, x, 64)
            for k, uid in enumerate(m.rewards):
                a = clamp((t - 0.6 - k * 0.5) / 0.4, 0, 1)
                if a > 0:
                    lab, name = reward_label(uid)
                    screen.center(y + 9 + k, f"{lab}: {name}", mix(Palette.PANEL, GOLD, a), Palette.PANEL, x, 64)
        elif ok:
            screen.center(y + 8, "ALREADY CLEARED - NO NEW REWARDS", Palette.MUTED, Palette.PANEL, x, 64)
        else:
            screen.center(y + 8, "ONLY THIS MISSION RESTARTS", Palette.AMBER, Palette.PANEL, x, 64)
        if int(t * 2) % 2 == 0 and t > 0.6:
            text = "[ ENTER ] CONTINUE" if ok else ("[ ENTER ] MISSIONS" if s.config.net else "[ ENTER ] MISSIONS     [ R ] RETRY")
            screen.center(y + 17, text, Palette.PRIMARY, Palette.PANEL, x, 64)


class LinksScene(BackdropScene):
    """Where to find the source, the website and the other storefronts."""
    LINKS = (("SOURCE CODE", "github.com/ethanlabs101/TerminalTanks", True),
             ("WEBSITE REPO", "github.com/ethanlabs101/terminaltanks-web", True),
             ("WEBSITE", "ethanlabs101.github.io/terminaltanks-web", True),
             ("ALL PROJECTS", "github.com/ethanlabs101", True),
             ("ITCH.IO", "COMING SOON", False),
             ("STEAM", "COMING SOON", False))

    def __init__(self, app) -> None:
        super().__init__(app)
        self.row, self.msg, self.msg_t = 0, "", 0.0

    def handle(self, ev: InputEvent) -> None:
        if ev.action is Action.BACK:
            self.app.goto(MenuScene(self.app))
        elif ev.action is Action.UP:
            self.row = (self.row - 1) % len(self.LINKS)
        elif ev.action is Action.DOWN:
            self.row = (self.row + 1) % len(self.LINKS)
        elif ev.confirm:
            _, url, live = self.LINKS[self.row]
            if not live:
                self.msg, self.msg_t = "NOT LIVE YET - CHECK BACK SOON", 2.5
                return
            try:
                if IS_WEB:
                    import js                                    # Pyodide: let the page open the tab
                    js.window.open("https://" + url, "_blank")
                    ok = True
                else:
                    import webbrowser
                    ok = webbrowser.open("https://" + url)
            except Exception:
                ok = False
            self.msg, self.msg_t = ("OPENED IN YOUR BROWSER" if ok else "COULD NOT OPEN A BROWSER - TYPE THE ADDRESS"), 2.5

    def update(self, dt: float) -> None:
        super().update(dt)
        self.msg_t = max(0.0, self.msg_t - dt)

    def draw(self, screen: Screen) -> None:
        super().draw(screen)
        w, h = 70, 21
        x, y = panel(screen, w, h, "TERMINAL TANKS  ·  FIND US")
        screen.center(y + 2, "Free, open source and built by one developer.", Palette.TEXT, Palette.PANEL, x, w)
        screen.center(y + 3, "Play it, read it, fork it, share it.", Palette.MUTED, Palette.PANEL, x, w)
        for i, (lab, url, live) in enumerate(self.LINKS):
            ry, sel = y + 5 + i * 2, i == self.row
            bg = Palette.PANEL_HI if sel else Palette.PANEL
            screen.fill(x + 2, ry, w - 4, 1, bg)
            screen.text(x + 4, ry, lab, Palette.WHITE if sel else Palette.MUTED, bg)
            screen.text(x + 20, ry, url, (GOLD if sel else Palette.PRIMARY) if live else Palette.AMBER, bg)
        screen.center(y + 18, self.msg if self.msg_t > 0 else "More games from ethanlabs101 are on the way.",
                      Palette.OK if self.msg_t > 0 and "OPENED" in self.msg else (Palette.AMBER if self.msg_t > 0 else Palette.MUTED), Palette.PANEL, x, w)
        draw_keycaps(screen, x + 14, y + 19, (("W/S", "SELECT"), ("ENTER", "OPEN"), ("ESC", "BACK")), Palette.PANEL)


# ===========================================================================
# TANK STUDIO: a preview-only booth. Any tank, any battlefield, paint, ammo and shell colour, firing on loop. Nothing is saved or exported.
# ===========================================================================
class StudioScene(BackdropScene):
    """TANK STUDIO (preview only): pick a real battlefield, a paint, an ammo and a shell colour and watch the tank fire on a loop."""
    ROWS = ("map", "paint", "ammo", "shot", "back")
    SW, SH = 46, 44                                        # preview pixels: 46 x 22 terminal rows

    def __init__(self, app, design: "TankDesign", body_name: str, back: Callable) -> None:
        super().__init__(app)
        self.design, self.back = design, back
        self.row, self.msg, self.msg_ok = 0, "", True
        self.maps = list(all_maps().values())
        self.ammos = [a for a in AMMOS + ANCIENT_AMMOS if a.key in app.save.unlocked_ammo()] or [DEFAULT_AMMO]
        lo = app.save.loadout(0)
        self.mi = 0
        self.ai = next((i for i, a in enumerate(self.ammos) if a.key == lo.ammo), 0)
        self.pi, self.si = paint_index(body_name), paint_index(lo.shot_color)
        self.stage = PreviewStage(app.settings, self.SW, self.SH, 2)
        self._map_applied = None

    def _apply(self, stage: "PreviewStage") -> None:
        if self._map_applied is not self.maps[self.mi] or stage is not self.stage:
            stage.set_map(self.maps[self.mi])
            if stage is self.stage:
                self._map_applied = self.maps[self.mi]
        stage.set_kit(self.design, PAINTS[self.pi][1], self.ammos[self.ai], PAINTS[self.si][1], 0.0)

    def handle(self, ev: InputEvent) -> None:
        key = self.ROWS[self.row]
        if ev.action is Action.BACK:
            self.app.goto(self.back())
        elif ev.action is Action.UP:
            self.row = (self.row - 1) % len(self.ROWS)
        elif ev.action is Action.DOWN:
            self.row = (self.row + 1) % len(self.ROWS)
        elif ev.action in (Action.LEFT, Action.RIGHT):
            d = (-1 if ev.action is Action.LEFT else 1) * (5 if ev.coarse else 1)
            if key == "map":
                self.mi = (self.mi + d) % len(self.maps)
            elif key == "paint":
                self.pi = (self.pi + d) % len(PAINTS)
            elif key == "ammo":
                self.ai = (self.ai + d) % len(self.ammos)
            elif key == "shot":
                self.si = (self.si + d) % len(PAINTS)
        elif ev.confirm and key == "back":
            self.app.goto(self.back())

    def update(self, dt: float) -> None:
        super().update(dt)
        self.stage.update(dt)

    def draw(self, screen: Screen) -> None:
        super().draw(screen)
        H = 26
        x, y = (screen.cols - 96) // 2, max(0, (screen.rows - H) // 2)
        screen.box(x, y, 44, H, "double", Palette.LINE, Palette.PANEL, "TANK STUDIO")
        sv, t = self.app.save, self.app.time
        pn, sn = PAINTS[self.pi][0], PAINTS[self.si][0]
        lock = lambda n: "" if sv.paint_ok(n, True) else "  (LOCKED)"
        vals = {"map": self.maps[self.mi].name, "paint": pn + lock(pn), "ammo": self.ammos[self.ai].name, "shot": sn + lock(sn),
                "back": "◄  BACK"}
        labels = {"map": "BATTLEFIELD", "paint": "TANK PAINT", "ammo": "AMMO", "shot": "SHELL COLOR", "back": ""}
        for i, key in enumerate(self.ROWS):
            ry, sel = y + 2 + i * 2, i == self.row
            bg = Palette.PANEL_HI if sel else Palette.PANEL
            screen.fill(x + 2, ry, 40, 1, bg)
            screen.text(x + 3, ry, labels[key], Palette.WHITE if sel else Palette.MUTED, bg)
            v = f"◄ {vals[key]} ►" if key in ("map", "paint", "ammo", "shot") and sel else vals[key]
            screen.text(x + 41 - len(v), ry, v[:30], GOLD if sel else Palette.TEXT, bg)
            if key in ("paint", "shot"):
                col = live_color(PAINTS[self.pi if key == "paint" else self.si][1], t)
                screen.text(x + 3, ry + 1, "█" * 8, col, Palette.PANEL)
        pr = y + 15
        tip = ("Paints marked LOCKED are preview-only until you own them." if (not sv.paint_ok(pn, True) or not sv.paint_ok(sn, False))
               else "Preview your tank on any battlefield with any paint, ammo and shell colour. Nothing is saved.")
        wrap_center(screen, pr, tip, 40, Palette.MUTED, Palette.PANEL, x, 44, 4)
        draw_keycaps(screen, x + 3, y + H - 2, (("▲▼", "ROW"), ("◄►", "CHANGE"), ("ENTER", "DO"), ("ESC", "BACK")), Palette.PANEL)
        px = x + 45
        screen.box(px, y, 50, H, "double", Palette.LINE, Palette.PANEL, "LIVE PREVIEW")
        self._apply(self.stage)
        screen.blit(self.stage.render(), px + 2, y + 1)
        screen.center(y + 23, self.design.name, Palette.WHITE, Palette.PANEL, px, 50)
        screen.center(y + 24, f"{self.ammos[self.ai].name}  ·  {self.maps[self.mi].name}"[:46], Palette.MUTED, Palette.PANEL, px, 50)


class WorkshopScene(BackdropScene):
    """Tank Workshop: combine unlocked components, name the tank, write its lore, preview it and build it for a fee."""
    ROWS = ("load", "turret", "hull", "track", "barrel", "glow", "ambient", "accent", "color", "hover", "name", "lore", "build", "equip", "delete")
    LABEL = {"load": "TANK", "turret": "TURRET", "hull": "HULL", "track": "TRACKS", "barrel": "BARREL", "glow": "GLOW", "ambient": "AMBIENT",
             "accent": "ACCENT", "color": "COLOR", "hover": "HOVER", "name": "NAME", "lore": "LORE", "build": "BUILD", "equip": "EQUIP", "delete": "DELETE"}

    def __init__(self, app) -> None:
        super().__init__(app)
        self.row, self.msg, self.msg_ok, self.msg_t = 0, "", True, 0.0
        self.stage = PreviewStage(app.settings, 46, 26, 2)
        self.load = 0                                   # 0 = a new design, n = saved tank n
        self.confirm_delete = False
        self.draft = {"parts": dict(CUSTOM_FALLBACK), "name": "MY TANK", "lore": "Built in the workshop.", "hover": False}
        self.opts = workshop_components(app.save)
        self.pcolor = paint_index(app.save.loadout(0).tank_color)        # preview body colour (applied to Player 1 on EQUIP)

    def _saved(self) -> list:
        return self.app.save.custom_tanks

    def _sync_draft(self) -> None:
        if self.load > 0 and self.load <= len(self._saved()):
            rec = self._saved()[self.load - 1]
            self.draft = {"parts": dict(CUSTOM_FALLBACK, **{k: v for k, v in (rec.get("parts") or {}).items() if k in CUSTOM_SLOTS}),
                          "name": str(rec.get("name", "")), "lore": str(rec.get("lore", "")), "hover": bool(rec.get("hover"))}

    def _design(self) -> TankDesign:
        return design_from_custom({"id": "preview", "name": self.draft["name"] or "CUSTOM", "lore": self.draft["lore"], "parts": self.draft["parts"],
                                   "hover": self.draft["hover"]})

    def _cycle(self, slot: str, d: int) -> None:
        opts = self.opts[slot]
        cur = self.draft["parts"].get(slot, CUSTOM_FALLBACK[slot])
        self.draft["parts"][slot] = opts[(opts.index(cur) + d) % len(opts)] if cur in opts else opts[0]

    def handle(self, ev: InputEvent) -> None:
        key, raw, save = self.ROWS[self.row], ev.raw, self.app.save
        if raw in ("g", "G") and key not in ("name", "lore"):                         # TANK STUDIO: preview booth
            self.app.goto(StudioScene(self.app, self._design(), PAINTS[self.pcolor][0], lambda: WorkshopScene(self.app)))
            return
        if ev.action is Action.BACK:
            self.confirm_delete = False if self.confirm_delete else self.confirm_delete
            if not self.confirm_delete:
                self.app.goto(MenuScene(self.app))
            return
        if key in ("name", "lore") and len(raw) == 1 and raw.isprintable():       # typing: letters never navigate here
            cap = 14 if key == "name" else 60
            if len(self.draft[key]) < cap and (raw.isalnum() or raw in " -.,!'"):
                self.draft[key] += raw.upper() if key == "name" else raw
            return
        if key in ("name", "lore") and raw == "SPACE":
            if len(self.draft[key]) < (14 if key == "name" else 60):
                self.draft[key] += " "
            return
        if raw == "BACKSPACE" and key in ("name", "lore"):
            self.draft[key] = self.draft[key][:-1]
            return
        if ev.action is Action.UP:
            self.row, self.confirm_delete = (self.row - 1) % len(self.ROWS), False
        elif ev.action is Action.DOWN:
            self.row, self.confirm_delete = (self.row + 1) % len(self.ROWS), False
        elif ev.action in (Action.LEFT, Action.RIGHT):
            d = -1 if ev.action is Action.LEFT else 1
            if key == "load":
                self.load = (self.load + d) % (len(self._saved()) + 1)
                self._sync_draft()
            elif key == "color":
                self.pcolor = (self.pcolor + d * (4 if ev.coarse else 1)) % len(PAINTS)
            elif key in self.opts:
                self._cycle(key, d * (5 if ev.coarse else 1))
            elif key == "hover":
                self.draft["hover"] = not self.draft["hover"]
        elif ev.confirm:
            if key == "hover":
                self.draft["hover"] = not self.draft["hover"]
            elif key == "build":
                hv = self.draft["hover"]
                why = save.create_custom(self.draft["parts"], self.draft["name"].strip() or "CUSTOM", self.draft["lore"].strip(), hv)
                paid = f"-{CUSTOM_TANK_FEE + (HOVER_COINS if hv else 0):,} COINS" + (f", -{HOVER_CREDITS} CREDITS" if hv else "")
                self.msg_ok, self.msg, self.msg_t = (not why), why or f"BUILT: {(self.draft['name'].strip() or 'CUSTOM')}  ({paid})", 3.0
                if not why:
                    self.load = len(save.custom_tanks)
                    self.app._wallet_flash = 1.4
            elif key == "equip" and self.load > 0:
                save.loadouts[0].tank = f"custom:{self._saved()[self.load - 1]['id']}"
                if save.paint_ok(PAINTS[self.pcolor][0], True):
                    save.loadouts[0].tank_color = PAINTS[self.pcolor][0]
                save.flush()
                self.msg_ok, self.msg, self.msg_t = True, "EQUIPPED AS YOUR PLAYER 1 TANK", 2.5
            elif key == "equip":
                self.msg_ok, self.msg, self.msg_t = False, "BUILD IT FIRST - THEN EQUIP", 2.5
            elif key == "delete" and self.load > 0:
                if not self.confirm_delete:
                    self.confirm_delete = True
                    self.msg_ok, self.msg, self.msg_t = False, "ENTER AGAIN TO DELETE FOR GOOD", 3.0
                else:
                    save.delete_custom(self._saved()[self.load - 1]["id"])
                    self.load, self.confirm_delete = 0, False
                    self.msg_ok, self.msg, self.msg_t = True, "TANK DELETED", 2.0
            elif key == "delete":
                self.msg_ok, self.msg, self.msg_t = False, "NOTHING SAVED TO DELETE", 2.0

    def update(self, dt: float) -> None:
        super().update(dt)
        self.stage.update(dt)
        self.msg_t = max(0.0, self.msg_t - dt)

    HELP = ("HOW IT WORKS: you only get parts from tanks you own - every tank you unlock adds its parts. Mix parts, name it, "
            "add lore, BUILD for {fee:,} coins. HOVER (a floating tank): +{hc:,} coins and +{hr} credits.")

    def draw(self, screen: Screen) -> None:
        super().draw(screen)
        save, t = self.app.save, self.app.time
        W, H = 93, 26
        x, y = (screen.cols - W) // 2, max(0, (screen.rows - H) // 2)
        screen.box(x, y, 44, H, "double", Palette.LINE, Palette.PANEL, "TANK WORKSHOP")
        for i, key in enumerate(self.ROWS):
            ry, sel = y + 2 + i, i == self.row
            bg = Palette.PANEL_HI if sel else Palette.PANEL
            screen.fill(x + 2, ry, 40, 1, bg)
            screen.text(x + 3, ry, f"{self.LABEL[key]:<8}", Palette.WHITE if sel else Palette.MUTED, bg)
            if key == "load":
                val = "NEW DESIGN" if self.load == 0 else f"{self.load}/{len(self._saved())}  {self._saved()[self.load - 1].get('name', '')[:14]}"
            elif key in self.opts:
                val = self.draft["parts"].get(key, "-").upper().replace("_", " ")
            elif key == "hover":
                val = "ON" if self.draft["hover"] else "OFF"
            elif key == "color":
                nm, cc = PAINTS[self.pcolor]
                val = nm + ("" if self.app.save.paint_ok(nm, True) else " (LOCKED)")
                screen.text(x + 12, ry, "████", live_color(cc, self.app.time), bg)
            elif key == "name":
                val = self.draft["name"] + ("█" if sel and int(t * 2) % 2 == 0 else "")
            elif key == "lore":
                val = (self.draft["lore"] + ("█" if sel and int(t * 2) % 2 == 0 else ""))[-26:]
            elif key == "build":
                val = f"▶ {CUSTOM_TANK_FEE + (HOVER_COINS if self.draft['hover'] else 0):,} COINS" + (f" +{HOVER_CREDITS}CR" if self.draft["hover"] else "")
            elif key == "equip":
                val = "▶ USE AS P1"
            else:
                val = "▶ REMOVE" if not self.confirm_delete else "▶ SURE?"
            val = f"◄ {val} ►" if sel and key in ("load", "hover", "color") + tuple(self.opts) else val
            screen.text(x + 41 - len(val), ry, val[:30], GOLD if sel else Palette.TEXT, bg)
        my = y + 3 + len(self.ROWS)
        if self.msg_t > 0:
            wrap_center(screen, my, self.msg, 40, Palette.OK if self.msg_ok else Palette.DANGER, Palette.PANEL, x, 44, 2)
        else:
            screen.center(my, f"{len(save.custom_tanks)}/{CUSTOM_TANK_MAX} SAVED", Palette.MUTED, Palette.PANEL, x, 44)
        for k, ln in enumerate(textwrap.wrap(self.HELP.format(fee=CUSTOM_TANK_FEE, hc=HOVER_COINS, hr=HOVER_CREDITS), 40)[:4]):
            screen.text(x + 2, my + 1 + k, ln, Palette.MUTED, Palette.PANEL)
        draw_keycaps(screen, x + 3, y + H - 2, (("▲▼", "ROW"), ("◄►", "PART"), ("ENTER", "DO"), ("ESC", "BACK")), Palette.PANEL)
        # preview
        px = x + 45
        screen.box(px, y, 48, H, "double", Palette.LINE, Palette.PANEL, "PREVIEW  ·  [G] STUDIO")
        d = self._design()
        lo = save.loadout(0)
        self.stage.set_kit(d, PAINTS[self.pcolor][1], AMMO_BY_KEY[lo.ammo], COLOR_BY_NAME[lo.shot_color], 0.0)
        screen.blit(self.stage.render(), px + 1, y + 1)
        screen.center(y + 14, d.name, Palette.WHITE, Palette.PANEL, px, 48)
        wrap_center(screen, y + 15, d.blurb, 44, Palette.TEXT, Palette.PANEL, px, 48, 2)
        screen.center(y + 17, f"{d.barrel.upper()}  ·  {d.glow.upper()}  ·  {d.ambient.upper()}"[:44], Palette.MUTED, Palette.PANEL, px, 48)
        screen.center(y + 18, f"YOU HAVE {save.coins:,} ◉  {save.credits} CR", GOLD if save.coins >= CUSTOM_TANK_FEE else Palette.DANGER, Palette.PANEL, px, 48)
        # browsable list of every custom tank
        saved = self._saved()
        screen.text(px + 2, y + 19, f"MY TANKS  {len(saved)}/{CUSTOM_TANK_MAX}", Palette.AMBER, Palette.PANEL)
        hint = "◄► ON TANK ROW TO BROWSE"
        screen.text(px + 46 - len(hint), y + 19, hint, Palette.MUTED, Palette.PANEL)
        total, vis = len(saved) + 1, 5
        top = int(clamp(self.load - vis // 2, 0, max(0, total - vis)))
        equipped = save.loadouts[0].tank
        for r in range(vis):
            i = top + r
            if i >= total:
                break
            cur = i == self.load
            bg = Palette.PANEL_HI if cur else Palette.PANEL
            screen.fill(px + 2, y + 20 + r, 44, 1, bg)
            if i == 0:
                label = "+ NEW DESIGN"
            else:
                rec = saved[i - 1]
                label = f"{i:02d} {str(rec.get('name', ''))[:22]}" + ("  ★ EQUIPPED" if equipped == f"custom:{rec['id']}" else "")
            screen.text(px + 3, y + 20 + r, ("▶ " if cur else "  ") + label, Palette.WHITE if cur else Palette.TEXT, bg)
        if top > 0:
            screen.put(px + 45, y + 20, "▲", Palette.PRIMARY_DIM, Palette.PANEL)
        if top + vis < total:
            screen.put(px + 45, y + 24, "▼", Palette.PRIMARY_DIM, Palette.PANEL)


# ============================================================================
# THE MARTYR'S ARCANE TIME MACHINE: the true final boss (unlocked once every Terminal Ticket boss is beaten)
# ============================================================================
MARTYR_PLAYER_HP, MARTYR_PLAYER_MULT = 175, 10                 # huge health pools: every hit is worth 10 (special: 30)
MARTYR_MAGE_HP = (250, 200, 260)
MARTYR_MAGE_MULT = (4, 5, 6)                                    # his hits scale up every phase...
MARTYR_RADIUS = (1.0, 1.15, 1.3)                                # ...and so do his blasts
MARTYR_PREC = (0.8, 0.68, 0.55)                                 # ...and his aim
MARTYR_PHASES = ("PHASE I  -  FIRE & EARTH", "PHASE II  -  WIND & WATER", "PHASE III  -  LIGHT & DARK")
MARTYR_TAUNT = ("LET US BEGIN WHERE ALL POWER BEGINS.", "THE SKY ITSELF WILL FIGHT YOU.", "THIS IS THE END OF TIME. HOLD NOTHING BACK.")
MARTYR_REWARD_IDS = tuple([f"tank:{d.key}" for d in ANCIENT_DESIGNS] + [f"ammo:{a.key}" for a in ANCIENT_AMMOS]
                          + [f"map:{m.key}" for m in ANCIENT_MAPS] + [f"theme:{k.lower()}" for k in ANCIENT_THEMES] + ["paint:astral"])
MARTYR_WIN_COINS = 25000
_ELEM = {   # element: (name, projectile, trail, blast, colour, mechanics)
    "fire": ("FIRE", "flame", "fire", "fire", (255, 130, 40), (("cluster", 5), ("cr", 0.6), ("radius", 1.2), ("dmg", 2))),
    "earth": ("EARTH", "shell", "cinders", "magma", (176, 134, 84), (("bounce", 2), ("br", 0.5), ("radius", 1.3), ("dmg", 3))),
    "wind": ("WIND", "ring", "ripple", "shock", (190, 240, 222), (("wob", (4.0, 9.0)), ("apex", ("split", 3, 1.2)), ("subr", 0.7), ("radius", 0.9), ("dmg", 1))),
    "water": ("WATER", "glyph", "tides", "tidal", (80, 170, 255), (("chain", 5), ("cstep", 7.0), ("cr", 0.7), ("radius", 1.0), ("dmg", 2))),
    "dark": ("DARK", "eye", "aeon", "eclipse", (160, 90, 240), (("pull", 12.0), ("radius", 1.5), ("fuse", 0.8), ("fr", 1.8), ("dmg", 2))),
    "light": ("LIGHT", "relic", "runes", "sigil", (255, 240, 170), (("apex", ("split", 5, 1.0)), ("subr", 0.7), ("radius", 1.3), ("dmg", 2))),
}
_PHASE_ELEMS = (("fire", "earth", "wind"), ("fire", "earth", "wind", "water"), ("fire", "earth", "wind", "water", "dark", "light"))
MAGE_AMMO: dict = {}
for _el, (_n, _pj, _tr, _bm, _col, _mech) in _ELEM.items():
    for _ph in range(3):
        _m = tuple((k, (round(v * MARTYR_RADIUS[_ph], 2) if k == "radius" else v)) for k, v in _mech)
        _a = AmmoType(f"m_{_el}{_ph}", _n, f"The Martyr's {_n.lower()} magic.", _pj, _tr, _bm, _m)
        MAGE_AMMO[(_el, _ph)] = _a
        AMMO_BY_KEY[_a.key] = _a


def draw_artifact(cv: PixelCanvas, cx: float, cy: float, s: float, t: float) -> None:
    """The Time Machine: a glowing hourglass wrapped in two turning rings of runes."""
    H, Wd = max(8, int(14 * s)), max(5, int(8 * s))
    for k in range(H):
        f = abs(H / 2 - k - 0.5) / (H / 2)
        hw = int(1 + f * Wd / 2)
        y = int(cy - H / 2 + k)
        for dx in range(-hw, hw + 1):
            edge = abs(dx) >= hw or k in (0, H - 1)
            col = (255, 226, 150) if edge else mix((60, 40, 110), (150, 110, 230), 0.5 + 0.5 * math.sin(t * 2 + k * 0.4))
            cv.blend(int(cx) + dx, y, col, 0.95 if edge else 0.55)
    top = int(cy - H / 2)
    for k in range(H // 2 - 2, H // 2 + 3):
        cv.plot(int(cx), top + k, (255, 236, 180))
    for k in range(int(H * 0.22)):
        for dx in range(-(k // 2), k // 2 + 1):
            cv.blend(int(cx) + dx, top + 1 + (H // 2 - 1 - k), (255, 210, 120), 0.6)
    for rx, ry, sp in ((Wd * 1.4, Wd * 0.5, 0.8), (Wd * 1.1, Wd * 0.42, -1.2)):
        for i in range(70):
            ang = i / 70 * math.tau + t * sp
            cv.blendf(cx + math.cos(ang) * rx, cy + math.sin(ang) * ry * (1.4 if sp < 0 else 1.0) + math.sin(ang * 3 + t) * 0.5,
                      (255, 226, 150) if i % 2 else (170, 120, 255), 0.55 + 0.35 * math.sin(ang * 5 + t * 3))


def _mage_sprite() -> list:
    """A 24x32 cell sprite built from shapes: pointed hood, shadowed face, long beard, gold-trimmed robe, staff."""
    g = [["."] * 24 for _ in range(32)]
    def put(x, y, c):
        if 0 <= x < 24 and 0 <= y < 32:
            g[y][x] = c
    for y in range(13, 32):                                          # robe, flaring toward the hem
        w = 4 + int((y - 13) * 0.33)
        for x in range(12 - w, 12 + w + 1):
            edge = x in (12 - w, 12 + w)
            put(x, y, "G" if edge else "L" if (x - 12) % 5 == 0 else "M" if abs(x - 12) < w - 1 else "D")
    for x in range(5, 20):                                            # shoulders and belt
        put(x, 13, "H"), put(x, 14, "D")
    for x in range(8, 17):
        put(x, 19, "G")
    for k in range(5):                                                # right arm reaching for the staff
        put(15 + k, 16 - (k // 2), "M"), put(15 + k, 17 - (k // 2), "D")
    put(20, 15, "g"), put(19, 15, "g")
    for y in range(4, 32):                                            # staff with gold rings
        put(20, y, "g" if (y - 4) % 7 == 0 else "S")
    for y in range(1, 14):                                            # pointed hood
        w = 1 + int((y - 1) * 0.5)
        for x in range(12 - w, 12 + w + 1):
            put(x, y, "H" if x in (12 - w, 12 + w) else "D")
    for y in range(7, 13):                                            # the face in shadow
        w = 2 if y < 11 else 1
        for x in range(12 - w, 12 + w + 1):
            put(x, y, "F")
    put(11, 9, "E"), put(13, 9, "E")
    for y in range(11, 22):                                           # a long white beard
        w = max(0, 3 - (y - 11) // 3)
        for x in range(12 - w, 12 + w + 1):
            put(x, y, "B")
    for (x, y) in ((8, 24), (15, 26), (10, 28), (13, 22), (7, 20), (16, 30)):
        put(x, y, "g")
    return g


_MAGE = _mage_sprite()


def draw_mage(cv: PixelCanvas, cx: float, by: float, s: float, t: float, alpha: float = 1.0, aura: bool = True) -> None:
    """The Martyr: a hooded archmage with a glowing-eyed hood, long beard, gold-trimmed robes and a staff whose orb cycles through
    his six elements, surrounded by orbiting element sparks. s = pixels per sprite cell."""
    bob = math.sin(t * 1.5) * 0.8 * s
    sz = max(1, int(math.ceil(s)))
    pal = {"D": (30, 14, 66), "M": (66, 34, 130), "L": (112, 70, 204), "H": (50, 24, 100), "G": (232, 180, 70), "g": (255, 240, 170),
           "F": (6, 2, 16), "E": (255, 250, 214), "B": (226, 226, 246), "S": (140, 96, 52)}
    for row in range(32):
        for col in range(24):
            ch = _MAGE[row][col]
            if ch == ".":
                continue
            c = pal[ch]
            if ch in "ML":                                              # shimmering robe
                c = mix(c, (150, 100, 240), 0.25 + 0.25 * math.sin(t * 2 + col * 0.6 + row * 0.35))
            elif ch == "E":
                c = mix(c, (255, 255, 255), 0.5 + 0.5 * math.sin(t * 6))
            elif ch == "B":
                c = mix(c, (190, 190, 230), 0.3 + 0.3 * math.sin(t * 1.3 + row))
            sway = math.sin(t * 2 + row * 0.25) * 0.5 * (row / 32) * s if row > 18 else 0.0
            px = cx + (col - 12) * s + sway
            py = by + bob + (31 - row) * s
            for ox in range(sz):
                for oy in range(sz):
                    cv.blend(int(px) + ox, int(py) + oy, c, alpha)
    ox_, oy_ = cx + 8 * s, by + bob + 30 * s                           # the orb at the top of the staff
    orb = _ELEM[list(_ELEM)[int(t * 0.8) % 6]][4]
    r = max(2.0, 2.6 * s)
    for dx in range(-int(r) - 1, int(r) + 2):
        for dy in range(-int(r) - 1, int(r) + 2):
            d = math.hypot(dx, dy)
            if d <= r:
                cv.blend(int(ox_) + dx, int(oy_) + dy, mix(orb, (255, 255, 255), 0.6 * (1 - d / r)), alpha * 0.95)
            elif d <= r + 1.5:
                cv.blend(int(ox_) + dx, int(oy_) + dy, orb, alpha * 0.3)
    if aura:
        for i, el in enumerate(_ELEM):
            ang = t * 1.1 + i * math.tau / 6
            ex, ey = cx + math.cos(ang) * 14 * s, by + 16 * s + math.sin(ang) * 7 * s
            col = _ELEM[el][4]
            for dx, dy in ((0, 0), (1, 0), (-1, 0), (0, 1), (0, -1)):
                cv.blendf(ex + dx, ey + dy, col, alpha * (0.95 if dx == 0 and dy == 0 else 0.5))


def martyr_art(save: "SaveData", t: float) -> PixelCanvas:
    cv = PixelCanvas(34, 20, (0, 0, 0))
    for py in range(20):
        cv.rows[py] = [mix((6, 2, 20), (70, 30, 110), py / 19)] * 34
    rng = random.Random(7)
    for _ in range(16):
        cv.blendf(rng.uniform(0, 33), rng.uniform(4, 19), (255, 255, 255), 0.4 + 0.4 * math.sin(t * 3 + rng.uniform(0, 6)))
    draw_artifact(cv, 17, 10, 0.95, t)
    if save.martyr_available() and not save.martyr_won:
        draw_mage(cv, 17, 2, 0.5, t, 0.35 + 0.25 * math.sin(t * 2), False)
    return cv


def start_martyr(app, intro: bool = False, replay: bool = False) -> None:
    save = app.save
    ph = 0 if replay else min(2, save.martyr_phase)
    lo = save.loadout(0)
    me = PlayerSetup("PLAYER", "YOU", COLOR_BY_NAME[lo.tank_color], COLOR_BY_NAME[lo.shot_color], False, lo.tank, lo.ammo, MARTYR_PLAYER_HP, team=0)
    mage = PlayerSetup("THE MARTYR", "MAGE", (190, 150, 255), (255, 240, 170), True, "a_martyr", "timerift", MARTYR_MAGE_HP[ph], team=1,
                       diff=Difficulty.MASTER, prec=MARTYR_PREC[ph])
    cfg = MatchConfig([me, mage], False, Difficulty.MASTER, "martyr", target=3, ammo_fx=True, special=True, special_wait=1)
    sess = MartyrSession(cfg, app.rng.randrange(1 << 30), ph, replay)

    def go():
        save.martyr_seen = True
        save.flush()
        app.goto(BattleScene(app, sess))
    pages = (MARTYR_INTRO if not save.martyr_seen else []) + (MARTYR_BETWEEN[ph] if ph > 0 else MARTYR_PHASE0)
    if not replay and not save.martyr_seen:
        whisper(app, "hoarders", lambda: app.goto(CinematicScene(app, pages, go)))     # saves that finished the vaults earlier still hear it
    else:
        app.goto(CinematicScene(app, pages, go))


class MartyrSession(MatchSession):
    """Three escalating phases against the Martyr. Losing a phase repeats it; clearing one is saved."""
    exit_label = "SHOP"

    def __init__(self, config: MatchConfig, seed: Optional[int], phase: int = 0, replay: bool = False) -> None:
        self.phase_i, self.won_phase, self.committed, self.replay = phase, False, False, replay
        super().__init__(config, None, seed)
        self.wins[0] = phase
        self._set_mage()

    def _set_mage(self) -> None:
        p = min(self.phase_i, 2)
        self.config.players[1] = replace(self.config.players[1], hp=MARTYR_MAGE_HP[p], prec=MARTYR_PREC[p])

    def _pick_map(self) -> MapDefinition:
        return ANCIENT_MAPS[min(self.phase_i, 2)]

    @property
    def completed(self) -> bool:
        return self.wins[0] >= 3

    @property
    def failed(self) -> bool:
        return False

    @property
    def over(self) -> bool:
        return self.completed

    @property
    def champion(self) -> int:
        return 0

    def record_round(self, winner: int) -> None:
        self.won_phase = winner == 0
        if winner == 0:
            self.wins[0] += 1

    def advance(self) -> None:
        self.round_no += 1
        if self.won_phase:
            self.phase_i = min(2, self.phase_i + 1)
        self._set_mage()
        self.current_map = self._pick_map()
        self.played.append(self.current_map)

    def interlude(self, app):
        if not self.won_phase:
            return None
        return CinematicScene(app, MARTYR_BETWEEN[self.phase_i], lambda: app.goto(BattleScene(app, self)))

    def on_turn_start(self, scene, tank) -> None:
        if tank.index != 1:
            return
        el = scene.srng.choice(_PHASE_ELEMS[min(self.phase_i, 2)])
        a = MAGE_AMMO[(el, min(self.phase_i, 2))]
        name = _ELEM[el][0]
        if scene.srng.random() < 0.7:                               # most turns he draws from EVERY special ammo there is
            pool = [x for x in AMMOS[BASE_KITS:] + TICKET_AMMOS + ANCIENT_AMMOS if x.key != BLESSING_KEY]
            a = scene.srng.choice(pool)
            name = a.name
        tank.ammo, tank.shot_color = a, _ELEM[el][4]
        scene.floaters.append(FloatText(tank.x, tank.y + tank.height + 14, name + "!", _ELEM[el][4]))

    def dmg_mult(self, tank) -> int:
        return MARTYR_PLAYER_MULT if tank.index == 0 else MARTYR_MAGE_MULT[min(self.phase_i, 2)]

    def hud_wins(self, i: int) -> str:
        return f"PHASE {min(3, self.phase_i + 1)}/3" if i == 0 else "THE MARTYR"

    def hud_title(self) -> str:
        return f"MARTYR'S TIME MACHINE  ·  {MARTYR_PHASES[min(self.phase_i, 2)]}  ·  {self.current_map.name}"

    def banner_title(self) -> str:
        return MARTYR_PHASES[min(self.phase_i, 2)]

    def banner_note(self) -> str:
        return MARTYR_TAUNT[min(self.phase_i, 2)]

    def round_summary(self, winner: int) -> dict:
        nxt = ("THE MARTYR FALLS." if self.completed else f"NEXT: {MARTYR_PHASES[min(self.phase_i + 1, 2)]}" if winner == 0
               else "YOU FALL - THE PHASE IS REPEATED. YOUR PROGRESS IS SAVED.")
        return {"title": MARTYR_PHASES[min(self.phase_i, 2)], "headline": "PHASE CLEARED" if winner == 0 else "THE MARTYR PREVAILS",
                "score": f"PHASES CLEARED {self.wins[0]}/3", "sub": "ESCALATING: HIS POWER GROWS EACH PHASE", "next": nxt}

    def commit(self, app: "Application") -> None:
        save = app.save
        if self.won_phase and not self.completed and not self.replay:
            save.martyr_phase = max(save.martyr_phase, self.wins[0])
            save.flush()
        if self.completed and not self.committed:
            self.committed = True
            if not self.replay:
                save.martyr_won, save.martyr_phase = True, 3
                for uid in MARTYR_REWARD_IDS:
                    save.grant(uid)
                self.payout = Payout(MARTYR_WIN_COINS)
                save.earn(self.payout)
                app.settings.home_mode = "ETERNAL"            # the masterpiece is waiting on the main screen
                app.apply_settings()
            save.flush()

    def finish_scene(self, app: "Application") -> "Scene":
        if self.replay:
            return ShopScene(app, 4)
        return CinematicScene(app, MARTYR_OUTRO, lambda: app.goto(MartyrRewardsScene(app)))

    def exit_scene(self, app: "Application") -> "Scene":
        return ShopScene(app, 4)

    def restart(self, app: "Application") -> None:
        start_martyr(app, replay=self.replay)


# ---- the script --------------------------------------------------------------------------------------------------------
STORY_LINES = {          # one-time whispers from an unseen voice (the Martyr, never named): heard once, remembered in the save
    "first_battle": ("That firing angle... I have not seen it in an age.",),
    "master": ("You carry old iron into a new war. Perhaps that is why you survived.",),
    "alien": ("Another world, another hunger. Why must scarcity always end in conquest?",),
    "mothership": ("You have ended a war that was not yours to inherit.", "There is another, older war waiting beneath your feet."),
    "hoarders": ("Ten prisons opened. Ten old sins answered for. And still, you stand.",
                 "Steady hands. Stubborn aim. I knew them once. They run in the blood."),
}


def whisper(app, key: str, nxt: Callable) -> None:
    """Plays a story whisper once (a short CinematicScene), then continues with `nxt`. Already heard / unknown key: just `nxt`."""
    if key in STORY_LINES and app.save.hear(key):
        app.goto(CinematicScene(app, [("A VOICE", ln, "voice", False) for ln in STORY_LINES[key]], nxt))
    else:
        nxt()


_N, _M = "NARRATOR", "THE MARTYR"
MARTYR_INTRO = [
    (_N, "Ten vaults opened. Ten ancient hoarders fell. The last seal shatters... and the world holds its breath.", "artifact", False),
    (_N, "In your hands, the Martyr's Arcane Time Machine begins to hum. Its sand has been falling for ten thousand years.", "artifact", False),
    (_N, "Then the sand stops.", "awake", True),
    (_M, "So. The last of the Hoarders is beaten.", "mage", False),
    (_M, "I felt every seal break. Each one was a thread of my own life, pulled tight for ten thousand years... and finally cut.", "mage", False),
    (_M, "Do not be afraid, child. I am the one the legends say died sealing the Hoarders away.", "mage", False),
    (_M, "The legends are half right. The world believed I gave my life to bind them. In truth, I bound them... and then I bound myself.", "mage", False),
    (_M, "No mortal body can hold that much magic and live. So I tore myself out of the flow of time, one heartbeat folded inside this hourglass. I did not die. I became the sand.", "mage", False),
    (_M, "I could never step out. Had I stayed human, the power I carried would have burned me to ash before the first century ended. So I waited, inside the artifact, until someone worthy broke the last seal.", "mage", False),
    (_M, "You fought well. You and that tank. You saved the universe. Peace has returned to the people, and their sanctuary stands. Because of you.", "mage", False),
    (_M, "Tell me... that rusted tank you started with. The five hulls, the five shells. Did you ever wonder where they came from?", "mage", False),
    (_M, "Spoils of the first crisis. I handed them down myself, parent to child, for a thousand generations, until they reached you. Old rusty heirlooms.", "mage", False),
    (_M, "I am your many-times-great-grandfather. Everything you are, I set in motion.", "mage", True),
    (_M, "So there is only one thing left to ask of a living legend. Duel me.", "duel", False),
    (_M, "Not for the universe. For the truth. Are you stronger than the one who saved it before you?", "duel", False),
    (_M, "Hold nothing back. I will not.", "duel", True),
]
MARTYR_PHASE0 = [
    (_N, "PHASE I.  THE EMBER SANCTUM.", "map0", True),
    (_M, "Let us begin where all power begins: fire, and the stone it forges. Wait one turn, and your special strikes for triple. Use it.", "map0", False),
]
MARTYR_BETWEEN = {
    1: [(_M, "Hah! Good. The mountain bows to you.", "map0", False),
        (_N, "PHASE II.  THE TEMPEST SPIRES.", "map1", True),
        (_M, "Now the sky itself. Wind to carry the shot, water to drown it. Let us see you stand.", "map1", False)],
    2: [(_M, "Wind and water, tamed. You are better than I dreamed.", "map1", False),
        (_N, "PHASE III.  THE ASTRAL RIFT.", "map2", True),
        (_M, "Then I will stop holding back. Light and dark. The end of time itself. Come, descendant. Show me everything.", "map2", False)],
}
MARTYR_OUTRO = [
    (_M, "...Magnificent.", "win", False),
    (_M, "Ten thousand years I waited. I thought I was waiting to be saved. I was waiting for you.", "win", False),
    (_M, "Take the relics of all six elements. Take the sanctuaries where we fought. They were always meant for the one who got this far.", "win", True),
    (_M, "Carry them well, little one. And do not let the sand run out.", "win", False),
    (_N, "The hourglass turns one last time. The Martyr rests.", "win", True),
]


class CinematicScene(Scene):
    """Full-screen cutscene: animated art above, typed dialogue below. ENTER advances, ESC skips."""

    def __init__(self, app, pages: list, on_done: Callable) -> None:
        super().__init__(app)
        self.pages, self.on_done, self.i, self.age, self.done = pages, on_done, 0, 0.0, False
        self.cache: dict = {}
        self.stars = [(random.Random(i * 7 + 5).uniform(0, 1), random.Random(i * 13 + 1).uniform(0, 1), i * 0.7) for i in range(90)]

    def _finish(self) -> None:
        if not self.done:
            self.done = True
            self.on_done()

    def handle(self, ev: InputEvent) -> None:
        if ev.action is Action.BACK:
            self._finish()
        elif ev.confirm or ev.action in (Action.CONFIRM, Action.FIRE):
            text = self.pages[self.i][1]
            if self.age * 45 < len(text):
                self.age = len(text) / 45 + 0.01
            elif self.i + 1 >= len(self.pages):
                self._finish()
            else:
                self.i, self.age = self.i + 1, 0.0

    def update(self, dt: float) -> None:
        self.age += dt

    def draw(self, screen: Screen) -> None:
        cols, rows = screen.cols, screen.rows
        screen.fill(0, 0, cols, rows, (3, 2, 10))
        who, text, stage, flash = self.pages[min(self.i, len(self.pages) - 1)]
        t, W, H = self.app.time, cols, max(8, (rows - 10) * 2)
        if stage.startswith("map"):
            tank = (DESIGN_BY_KEY.get(self.app.save.loadout(0).tank, DESIGNS[0]), COLOR_BY_NAME[self.app.save.loadout(0).tank_color])
            cv = render_map_preview(self.cache, ANCIENT_MAPS[int(stage[3])], W, H, t, [tank, (DESIGN_BY_KEY["a_martyr"], (190, 150, 255))])
        else:
            cv = PixelCanvas(W, H, (0, 0, 0))
            for py in range(H):
                cv.rows[py] = [mix((4, 2, 14), (60, 24, 96) if stage != "win" else (110, 70, 40), py / max(1, H - 1))] * W
            for sx, sy, ph in self.stars:
                cv.blendf(sx * W, sy * H, (255, 255, 255), 0.3 + 0.5 * (0.5 + 0.5 * math.sin(t * 2 + ph)))
            sc = H / 28
            if stage in ("artifact", "awake"):
                draw_artifact(cv, W / 2, H / 2, sc * 1.5, t)
                if stage == "awake":
                    for k in range(14):
                        a = k * math.tau / 14 + t * 0.3
                        for j in range(int(H * 0.5)):
                            cv.blendf(W / 2 + math.cos(a) * j * 1.6, H / 2 + math.sin(a) * j, (255, 226, 150), 0.25 * min(1, self.age / 2) * (1 - j / (H * 0.5)))
            elif stage == "voice":                                         # an unseen speaker: only the stars and the gradient
                pass
            elif stage == "mage":
                draw_artifact(cv, W * 0.28, H * 0.42, sc * 1.0, t)
                draw_mage(cv, W * 0.62, H * 0.08, max(1.0, H * 0.78 / 32), t, min(1.0, 0.25 + self.age / 3))
            elif stage == "duel":
                for x in range(W):
                    for y in range(max(1, int(H * 0.1))):
                        cv.plot(x, y, (24, 14, 44))
                lo = self.app.save.loadout(0)
                draw_tank(cv, W * 0.25, H * 0.1, 1, 40, COLOR_BY_NAME[lo.tank_color], scale=2, design=DESIGN_BY_KEY.get(lo.tank, DESIGNS[0]), t=t)
                draw_mage(cv, W * 0.7, H * 0.1, max(1.0, H * 0.78 / 32), t, 1.0)
            else:                                                          # win: the Martyr dissolves into sand and light
                draw_artifact(cv, W * 0.3, H * 0.42, sc, t)
                draw_mage(cv, W * 0.62, H * 0.08, max(1.0, H * 0.78 / 32), t, max(0.0, 1 - self.age / 6) * 0.9 if self.i >= 1 else 0.9)
                rng = random.Random(int(t * 5))
                for _ in range(40):
                    cv.blendf(W * 0.62 + rng.uniform(-12, 12), H * 0.12 + ((t * 9 + rng.uniform(0, H)) % (H * 0.8)), (255, 226, 150), 0.7)
        if flash and self.age < 0.6:
            for row in cv.rows:
                for x in range(len(row)):
                    row[x] = mix(row[x], (255, 255, 255), (1 - self.age / 0.6) * 0.9)
        screen.blit(cv, 0, 0)
        bw = min(cols - 4, 92)
        bx, by = (cols - bw) // 2, rows - 9
        screen.box(bx, by, bw, 8, "double", Palette.LINE, Palette.PANEL, "")
        col = GOLD if who == _M else Palette.MUTED
        screen.text(bx + 3, by, f" {who} ", col, Palette.PANEL)
        shown = text[:int(self.age * 45)]
        for k, ln in enumerate(textwrap.wrap(text, bw - 8)[:5]):
            part = shown[:len(ln)] if k == 0 else ""
        shown_left = len(shown)
        for k, ln in enumerate(textwrap.wrap(text, bw - 8)[:5]):
            screen.text(bx + 3, by + 2 + k, ln[:max(0, shown_left)], Palette.WHITE, Palette.PANEL)
            shown_left -= len(ln) + 1
        if self.age * 45 >= len(text) and int(t * 2) % 2 == 0:
            screen.text(bx + bw - 12, by + 6, "▼ ENTER", Palette.PRIMARY, Palette.PANEL)
        screen.text(bx + 3, by + 7, f"{self.i + 1}/{len(self.pages)}   ESC SKIP", Palette.MUTED, Palette.PANEL)


class MartyrRewardsScene(BackdropScene):
    """Everything the Martyr leaves behind."""

    def __init__(self, app) -> None:
        super().__init__(app)
        self.stage = PreviewStage(app.settings, 46, 26, 2)

    def handle(self, ev: InputEvent) -> None:
        if ev.confirm or ev.action in (Action.CONFIRM, Action.BACK):
            self.app.goto(MartyrFinaleScene(self.app))

    def update(self, dt: float) -> None:
        super().update(dt)
        self.stage.update(dt)

    def draw(self, screen: Screen) -> None:
        super().draw(screen)
        t = self.app.time
        W, H = 94, 26
        x, y = (screen.cols - W) // 2, max(0, (screen.rows - H) // 2)
        screen.box(x, y, 44, H, "double", GOLD, Palette.PANEL, "THE MARTYR'S LEGACY")
        rows = ([("ANCIENT ARTIFACT TANKS", GOLD)] + [(f"  ✦ {d.name}", Palette.WHITE) for d in ANCIENT_DESIGNS]
                + [("ANCIENT ARTIFACT AMMO", GOLD)] + [(f"  ✦ {a.name}", Palette.WHITE) for a in ANCIENT_AMMOS]
                + [("BATTLEFIELDS", GOLD)] + [(f"  ✦ {m.name}", m.accent) for m in ANCIENT_MAPS]
                + [("COLOR", GOLD), ("  ✦ ASTRAL PAINT & AMMO COLOR", astral(t))])
        for k, (txt, c) in enumerate(rows[:20]):
            screen.text(x + 3, y + 2 + k, txt, c, Palette.PANEL)
        screen.text(x + 3, y + 2 + len(rows[:20]), "6 HOME SCREENS: ONE FOR EVERY ANCIENT TANK", Palette.AMBER, Palette.PANEL)
        screen.text(x + 3, y + 3 + len(rows[:20]), "★ ULTIMATE AMMO: MARTYR'S BLESSING", astral(t), Palette.PANEL)
        screen.center(y + H - 2, "[ ENTER ]", Palette.PRIMARY if int(t * 2) % 2 == 0 else Palette.MUTED, Palette.PANEL, x, 44)
        px = x + 45
        screen.box(px, y, 49, H, "double", GOLD, Palette.PANEL, "RELICS")
        i = int(t / 2.5)
        d, a = ANCIENT_DESIGNS[i % len(ANCIENT_DESIGNS)], ANCIENT_AMMOS[i % len(ANCIENT_AMMOS)]
        self.stage.set_kit(d, ASTRAL, a, ASTRAL, 0.0)
        screen.blit(self.stage.render(), px + 1, y + 1)
        screen.center(y + 15, d.name, GOLD, Palette.PANEL, px, 49)
        wrap_center(screen, y + 16, d.blurb, 44, Palette.TEXT, Palette.PANEL, px, 49, 2)
        screen.center(y + 19, f"+ {a.name}", Palette.PRIMARY, Palette.PANEL, px, 49)
        wrap_center(screen, y + 20, a.blurb, 44, Palette.MUTED, Palette.PANEL, px, 49, 2)


class MartyrFinaleScene(Scene):
    """The 'you did it' screen: the ETERNAL home screen, shown in full."""

    def __init__(self, app) -> None:
        super().__init__(app)
        self.bd = tt_backdrop = ThemeBackdrop(HOME_THEMES["ETERNAL"])

    def handle(self, ev: InputEvent) -> None:
        if ev.confirm or ev.action in (Action.CONFIRM, Action.BACK):
            self.app.goto(MenuScene(self.app))

    def draw(self, screen: Screen) -> None:
        t, th = self.app.time, HOME_THEMES["ETERNAL"]
        cols, rows = screen.cols, screen.rows
        self.bd.ensure(cols, rows)
        self.bd.draw(screen, t)
        y = draw_theme_logo(screen, 2, t, rows >= 34, th)
        sv = self.app.save
        lines = [("THE LAST SEAL IS OPEN.", GOLD), ("THE HOURGLASS TURNS FOR YOU NOW.", (255, 244, 214)), ("", (255, 255, 255)),
                 ("10 VAULTS  ·  3 PHASES  ·  1 LEGEND", th.accent), ("", (255, 255, 255)),
                 ("A new home screen waits for you every time you return.", Palette.MUTED)]
        bx, bw = (cols - 60) // 2, 60
        screen.box(bx, y + 1, bw, len(lines) + 4, "double", GOLD, Palette.BG, "")
        for k, (txt, c) in enumerate(lines):
            screen.center(y + 3 + k, txt, c, Palette.BG, bx, bw)
        if int(t * 2) % 2 == 0:
            screen.center(y + 4 + len(lines), "[ ENTER ]", Palette.PRIMARY, Palette.BG, bx, bw)


class TicketResultScene(BackdropScene):
    def __init__(self, app, session: "TicketSession") -> None:
        super().__init__(app)
        self.session, self.boss, self.t = session, session.boss, 0.0

    def enter(self) -> None:
        self.app._wallet_flash = 1.6
        if self.session.completed:
            self.app.backdrop.celebrate_with([DESIGN_BY_KEY[self.boss.tank].accent, GOLD, Palette.WHITE])

    def exit(self) -> None:
        self.app.backdrop.celebrate_with(None)

    def handle(self, ev: InputEvent) -> None:
        s = self.session
        if ev.raw in ("r", "R") and not s.completed:
            s.restart(self.app)
        elif ev.confirm:
            go = lambda: self.app.goto(ShopScene(self.app, 4))
            if s.completed and self.app.save.tickets_sold_out():
                whisper(self.app, "hoarders", go)
            else:
                go()

    def update(self, dt: float) -> None:
        super().update(dt)
        self.t += dt

    def draw(self, screen: Screen) -> None:
        super().draw(screen)
        s, b, t = self.session, self.boss, self.t
        ok = s.completed
        col = DESIGN_BY_KEY[b.tank].accent
        x, y = panel(screen, 64, 20, "TERMINAL TICKET", fg=col if ok else Palette.DANGER)
        head = "B O S S   D E F E A T E D" if ok else "T I C K E T   B A T T L E   L O S T"
        if ok:
            prism_text(screen, x + (64 - len(head)) // 2, y + 2, head, t, 0.12, 0.05, 0.55, 1.0, Palette.PANEL)
        else:
            screen.center(y + 2, head, Palette.DANGER, Palette.PANEL, x, 64)
        screen.center(y + 3, b.name, col, Palette.PANEL, x, 64)
        if s.payout:
            screen.center(y + 5, ("VICTORY  " if ok else "CONSOLATION  ") + s.payout.text(), GOLD, Palette.PANEL, x, 64)
        if ok:
            screen.center(y + 7, "CLAIMED", Palette.OK, Palette.PANEL, x, 64)
            for k, uid in enumerate(b.reward_ids()):
                a = clamp((t - 0.6 - k * 0.5) / 0.4, 0, 1)
                if a > 0:
                    lab, name = reward_label(uid)
                    screen.center(y + 9 + k, f"{lab}: {name}", mix(Palette.PANEL, GOLD, a), Palette.PANEL, x, 64)
            left = len(TICKET_BOSS_BY_KEY) - len(self.app.save.tickets_won & set(TICKET_BOSS_BY_KEY))
            screen.center(y + 14, "ALL BOSSES DEFEATED - TICKETS SOLD OUT" if left == 0 else f"{left} BOSS{'ES' if left != 1 else ''} LEFT IN THE VAULT",
                          Palette.MUTED, Palette.PANEL, x, 64)
        else:
            screen.center(y + 8, "YOUR TICKET IS KEPT", Palette.AMBER, Palette.PANEL, x, 64)
            screen.center(y + 9, "RETRY ANY TIME - IT IS NOT WASTED", Palette.MUTED, Palette.PANEL, x, 64)
        if int(t * 2) % 2 == 0 and t > 0.6:
            screen.center(y + 17, "[ ENTER ] SHOP" if ok else "[ ENTER ] SHOP     [ R ] RETRY", Palette.PRIMARY, Palette.PANEL, x, 64)


class TournamentResultScene(BackdropScene):
    """Level complete / failed screen with a staged reveal of every unlock."""

    def __init__(self, app, session: TournamentSession) -> None:
        super().__init__(app)
        self.session, self.level = session, session.level
        self.t = 0.0
        self.stage = PreviewStage(app.settings, 40, 26, 2)

    @property
    def fresh(self) -> bool:
        return self.session.completed and self.session.first_clear

    def enter(self) -> None:
        self.app._wallet_flash = 1.6
        if self.fresh:
            lv = self.level
            self.app.backdrop.celebrate_with([COLOR_BY_NAME[lv.boss_color], COLOR_BY_NAME[lv.boss_shot], Palette.WHITE])

    def exit(self) -> None:
        self.app.backdrop.celebrate_with(None)

    def handle(self, ev: InputEvent) -> None:
        if ev.raw in ("r", "R") and not self.session.completed:      # retry just this level, straight away
            self.session.restart(self.app)
        elif ev.action in (Action.CONFIRM, Action.FIRE):
            n = self.level.number
            nxt = n + 1 if (self.session.completed and n < len(TOURNAMENT)) else n
            go = lambda: self.app.goto(TournamentScene(self.app, nxt))
            if self.session.completed and n == len(TOURNAMENT):
                whisper(self.app, "master", go)
            else:
                go()

    def update(self, dt: float) -> None:
        super().update(dt)
        self.t += dt
        self.stage.update(dt)

    def draw(self, screen: Screen) -> None:
        super().draw(screen)
        s, lv, t = self.session, self.level, self.t
        x, y = (screen.cols - 92) // 2, (screen.rows - 24) // 2
        boss_col = COLOR_BY_NAME[lv.boss_color]
        gold = lv.difficulty is Difficulty.MASTER
        ok = s.completed
        edge = (GOLD if gold else Palette.OK) if ok else Palette.DANGER
        screen.box(x, y, 46, 24, "double", edge, Palette.PANEL, "REWARD KIT" if ok else "BOSS KIT")
        self.stage.set_kit(DESIGN_BY_KEY[lv.tank], boss_col, AMMO_BY_KEY[lv.ammo], COLOR_BY_NAME[lv.boss_shot], 0.0 if ok else 0.5)
        screen.blit(self.stage.render(), x + 3, y + 2)          # 40px canvas centred in the 44-col box
        screen.center(y + 16, f"{DESIGN_BY_KEY[lv.tank].name}  +  {AMMO_BY_KEY[lv.ammo].name}", Palette.TEXT, Palette.PANEL, x, 46)
        n1 = wrap_center(screen, y + 18, DESIGN_BY_KEY[lv.tank].blurb, 42, Palette.MUTED, Palette.PANEL, x, 46, 2)
        wrap_center(screen, y + 18 + n1, AMMO_BY_KEY[lv.ammo].blurb, 42, Palette.MUTED, Palette.PANEL, x, 46, 2)
        rx = x + 46
        screen.box(rx, y, 46, 24, "double", edge, Palette.PANEL, f"LEVEL {lv.number}")
        if ok:
            head = "L E V E L   C L E A R E D"
            prism_text(screen, rx + (46 - len(head)) // 2, y + 2, head, t, 0.12, 0.05, 0.55, 1.0, Palette.PANEL)
            screen.center(y + 4, f"{lv.boss} DEFEATED", boss_col, Palette.PANEL, rx, 46)
            screen.center(y + 5, f"{s.wins[0]} ROUNDS WON  ·  BOSS {s.wins[1]}", Palette.MUTED, Palette.PANEL, rx, 46)
            if s.payout:
                screen.center(y + 6, ("FIRST CLEAR  " if self.fresh else "REPLAY  ") + s.payout.text(), GOLD, Palette.PANEL, rx, 46)
            items = reward_lines(lv) if self.fresh else []
            if not self.fresh:
                screen.center(y + 9, "REWARDS ALREADY UNLOCKED", Palette.TEXT, Palette.PANEL, rx, 46)
            for k, (lab, name) in enumerate(items):
                a = clamp((t - 0.9 - k * 0.7) / 0.4, 0, 1)
                if a <= 0:
                    continue
                col = mix(Palette.PANEL, GOLD if gold else Palette.AMBER, a)
                suffix = "ACTIVATED" if lab == "MASTER HOME SCREEN" else "UNLOCKED"
                screen.text(rx + 4, y + 8 + k * 2, f"✓ {lab} {suffix}", mix(Palette.PANEL, Palette.OK, a), Palette.PANEL)
                screen.text(rx + 6, y + 9 + k * 2, name, col, Palette.PANEL)
            if self.fresh and lv.number == len(TOURNAMENT) and t > 4.8:
                screen.center(y + 20, "Press H on the title or menu to switch home screens", Palette.MUTED, Palette.PANEL, rx, 46)
        else:
            screen.center(y + 3, "L E V E L   F A I L E D", Palette.DANGER, Palette.PANEL, rx, 46)
            screen.center(y + 5, "YOU LOST A ROUND" if lv.fail_at == 1 else f"{lv.boss} WON {lv.fail_at} ROUNDS", boss_col, Palette.PANEL, rx, 46)
            screen.center(y + 7, f"YOU HAD {s.wins[0]}/{lv.wins} ROUND WINS", Palette.TEXT, Palette.PANEL, rx, 46)
            screen.center(y + 10, "ONLY THIS LEVEL RESTARTS", Palette.AMBER, Palette.PANEL, rx, 46)
            screen.center(y + 11, "ALL OTHER PROGRESS IS SAFE", Palette.MUTED, Palette.PANEL, rx, 46)
            if s.payout:
                screen.center(y + 14, "CONSOLATION  " + s.payout.text(), GOLD, Palette.PANEL, rx, 46)
        if int(t * 2) % 2 == 0 and t > 0.6:
            text = "[ ENTER ] CONTINUE" if ok else "[ ENTER ] ROADMAP     [ R ] RETRY LEVEL"
            screen.center(y + 22, text, Palette.PRIMARY, Palette.PANEL, rx, 46)


class CheatScene(BackdropScene):
    """Type a code, press ENTER. Codes live in CHEATS; the status block shows what is unlocked without spoiling the codes."""
    MAX_LEN = 24

    def __init__(self, app) -> None:
        super().__init__(app)
        self.text, self.msg, self.msg_ok, self.msg_t = "", "", True, 0.0

    def handle(self, ev: InputEvent) -> None:
        raw = ev.raw
        if raw == "BACKSPACE":
            self.text = self.text[:-1]
        elif ev.action is Action.BACK:
            self.app.goto(MenuScene(self.app))
        elif ev.action is Action.CONFIRM:
            if self.text:
                self.msg_ok, self.msg = redeem_cheat(self.app, self.text)
                self.msg_t, self.text = 5.0, ""
        elif len(raw) == 1 and raw.isalnum() and len(self.text) < self.MAX_LEN:
            self.text += raw.lower()

    def update(self, dt: float) -> None:
        super().update(dt)
        self.msg_t = max(0.0, self.msg_t - dt)

    def draw(self, screen: Screen) -> None:
        super().draw(screen)
        save, t = self.app.save, self.app.time
        x, y = panel(screen, 64, 24, "CHEAT CODES")
        screen.center(y + 2, "ENTER A CODE", Palette.AMBER, Palette.PANEL, x, 64)
        screen.box(x + 8, y + 3, 48, 3, "single", Palette.LINE, Palette.PANEL_HI)
        cursor = "█" if int(t * 2) % 2 == 0 else " "
        screen.text(x + 10, y + 4, (self.text + cursor)[-44:], Palette.WHITE, Palette.PANEL_HI)
        if self.msg_t > 0:
            screen.center(y + 7, self.msg, Palette.OK if self.msg_ok else Palette.DANGER, Palette.PANEL, x, 64)
        n_levels = sum(save.level_unlocked(l.number) for l in TOURNAMENT)
        lines = [(lab, f"{have}/{total}") for lab, have, total in content_counts(save)] + [
            ("LEVELS", f"{n_levels}/{len(TOURNAMENT)} OPEN  ·  {len(save.completed)}/{len(TOURNAMENT)} CLEARED"),
            ("MISSION", f"{sum(save.mission_unlocked(m.number) for m in MISSIONS)}/{len(MISSIONS)} OPEN  ·  {len(save.mission_completed)}/{len(MISSIONS)} CLEARED"),
            ("TICKETS", f"{len(save.tickets_won & set(TICKET_BOSS_BY_KEY))}/{len(TICKET_BOSS_BY_KEY)} BOSSES"),
            ("MASTER", "DIFFICULTY UNLOCKED" if save.master_unlocked else "DIFFICULTY LOCKED")]
        for k, (lab, val) in enumerate(lines):
            screen.text(x + 12, y + 9 + k, f"{lab:<8}", Palette.MUTED, Palette.PANEL)
            screen.text(x + 21, y + 9 + k, val, Palette.TEXT, Palette.PANEL)
        draw_keycaps(screen, x + 10, y + 21, (("ENTER", "REDEEM"), ("BKSP", "DELETE"), ("ESC", "BACK")), Palette.PANEL)


class MatchResultScene(BackdropScene):
    def __init__(self, app, session: MatchSession) -> None:
        super().__init__(app)
        self.session = session

    def enter(self) -> None:
        self.app._wallet_flash = 1.6
        w = self.session.champion
        pl = (self.session.team_players(w) or self.session.config.players)[0]
        self.app.backdrop.celebrate_with([pl.tank_color, pl.shot_color, Palette.WHITE])

    def exit(self) -> None:
        self.app.backdrop.celebrate_with(None)

    def handle(self, ev: InputEvent) -> None:
        if ev.action in (Action.CONFIRM, Action.FIRE):
            net = self.session.config.net
            if net is not None:
                net.send({"t": "quit"})
                net.close()
            self.app.goto(MenuScene(self.app))

    def draw(self, screen: Screen) -> None:
        super().draw(screen)
        s = self.session
        w = s.champion
        pl = (s.team_players(w) or s.config.players)[0]
        x, y = panel(screen, 46, 17, "MATCH COMPLETE", fg=live_color(pl.tank_color, self.app.time))
        screen.center(y + 2, f"{s.team_name(w)} WINS"[:42], pl.tank_color, Palette.PANEL, x, 46)
        a, b = str(s.wins[0]), str(s.wins[1])
        if len(a) == 1 and len(b) == 1:
            for r in range(5):
                line = _DIGITS[a][r] + ("  ———  " if r == 2 else "       ") + _DIGITS[b][r]
                screen.center(y + 4 + r, line, Palette.WHITE, Palette.PANEL, x, 46)
        else:
            screen.center(y + 6, f"{a}   —   {b}", Palette.WHITE, Palette.PANEL, x, 46)
        cfg, lw = s.config, s.wins[1 - w]
        label = cfg.label or f"FIRST TO {cfg.target}"
        if cfg.net is not None:                               # LAN: each machine hears it from its own player's side
            mine = cfg.my_owner
            won = w == mine
            head, hcol = ("VICTORY!", Palette.OK) if won else ("DEFEAT", Palette.DANGER)
            cond = f"{label}: YOU {'WON' if won else 'LOST'} {s.wins[mine]} TO {s.wins[1 - mine]}"
        elif cfg.single:
            won = w == 0
            head, hcol = ("VICTORY!", Palette.AMBER) if won else ("DEFEAT", Palette.DANGER)
            cond = f"{label}: {'YOU WON' if won else 'THE AI WON'} {s.wins[0]} TO {s.wins[1]}"
        else:
            head, hcol = f"{s.team_name(w)} WINS!", Palette.AMBER
            cond = f"{label}: {s.team_name(w)} WON {s.wins[w]} TO {lw}"
        screen.center(y + 10, head[:40], hcol, Palette.PANEL, x, 46)
        screen.center(y + 11, cond[:44], Palette.TEXT, Palette.PANEL, x, 46)
        if s.payout:
            screen.center(y + 13, s.payout.text(), GOLD, Palette.PANEL, x, 46)
        elif cfg.mode in ("two", "lan"):
            screen.center(y + 13, "TWO-PLAYER AND LAN MATCHES EARN NO COINS", Palette.MUTED, Palette.PANEL, x, 46)
        if int(self.app.time * 2) % 2 == 0:
            screen.center(y + 15, "[ ENTER ]", Palette.PRIMARY, Palette.PANEL, x, 46)


# ============================================================================
# Application
# ============================================================================
@dataclass
class Transition:
    scene: Scene
    phase: str = "out"
    t: float = 0.0
    OUT = 0.22
    IN = 0.30

    @property
    def progress(self) -> float:
        return clamp(self.t / (self.OUT if self.phase == "out" else self.IN), 0, 1)


class Application:
    def __init__(self, term, seed: Optional[int] = None, color_mode: ColorMode = ColorMode.AUTO,
                 save_path: Optional[Path] = None) -> None:
        self.term = term
        self.input = InputManager(term)
        self.profiles = ProfileBook(save_path)
        self.save = SaveData.load(self.profiles.active_path() if save_path is not None else None)
        self.settings = Settings()
        self.save.apply_settings(self.settings)
        if color_mode is not ColorMode.AUTO:
            self.settings.color_mode = color_mode
        self.screen = Screen(term, self.settings.color_mode)
        self.feedback = Feedback(term, self.settings)
        self.backdrop = AmbientBackdrop()
        self.master_backdrop = MasterBackdrop(self.settings)
        self.secret_backdrop = SecretBackdrop(self.settings)
        self.theme_backdrops: dict = {}
        self.rng = random.Random(seed)
        self.stack: list[Scene] = []
        self.transition: Optional[Transition] = None
        self.time = 0.0
        self.running = True

    # -- scene management ----------------------------------------------------
    def _swap(self, scene: Scene) -> None:
        for s in self.stack:
            s.exit()
        self.stack = [scene]
        scene.enter()

    def goto(self, scene: Scene, fade: bool = True) -> None:
        if not self.stack or not fade:
            self._swap(scene)
        elif self.transition is None:
            self.transition = Transition(scene)

    def push(self, scene: Scene) -> None:
        self.stack.append(scene)
        scene.enter()

    def pop(self) -> None:
        if len(self.stack) > 1:
            self.stack.pop().exit()

    def start_match(self, cfg: MatchConfig, mapdef: Optional[MapDefinition], wind: Optional[float] = None, rps: bool = True) -> None:
        session = MatchSession(cfg, mapdef, self.rng.randrange(1 << 30))
        session.wind_override = wind
        if rps and cfg.mode == "two" and cfg.net is None:                  # who shoots first is decided by rock-paper-scissors
            self.goto(RPSScene(self, session))
        else:
            self.goto(BattleScene(self, session))

    def start_tournament(self, level: TournamentLevel, cfg: Optional[MatchConfig] = None) -> None:
        if cfg is None:
            lo = self.save.loadout(0)
            me = PlayerSetup("PLAYER", "YOU", COLOR_BY_NAME[lo.tank_color], COLOR_BY_NAME[lo.shot_color], False, lo.tank, lo.ammo,
                             level.player_hp)
            cfg = MatchConfig([me, boss_setup(level, color_index(lo.tank_color))], True, level.difficulty, "tournament", level.precision, ammo_fx=False)
        go = lambda: self.goto(BattleScene(self, TournamentSession(level, cfg, self.rng.randrange(1 << 30))))
        if level.number == 1:
            whisper(self, "first_battle", go)
        else:
            go()

    def home_theme(self) -> str:
        mode = self.settings.home_mode
        return mode if mode in self.save.home_modes() else "CLASSIC"

    def home_backdrop(self):
        mode = self.home_theme()
        if mode in HOME_THEMES:
            if mode not in self.theme_backdrops:
                self.theme_backdrops[mode] = ThemeBackdrop(HOME_THEMES[mode])
            return self.theme_backdrops[mode]
        return {"MASTER": self.master_backdrop, "SECRET": self.secret_backdrop}.get(mode, self.backdrop)

    def switch_profile(self, pid: str) -> None:
        """Save the current profile, load another into the same running game, return to the main menu."""
        self.save.flush()
        path = self.profiles.path_of(pid)
        if path is None:
            return
        self.profiles.active = pid
        self.profiles.write()
        self.save = SaveData.load(path)
        self.settings = Settings() if False else self.settings
        self.save.apply_settings(self.settings)
        self.screen.set_mode(self.settings.color_mode)
        self.goto(MenuScene(self))

    def profiles_delete(self, pid: str) -> None:
        was_active = self.profiles.active == pid
        if self.profiles.delete(pid) and was_active:
            self.save = SaveData.load(self.profiles.active_path())
            self.save.apply_settings(self.settings)

    def apply_settings(self) -> None:
        self.screen.set_mode(self.settings.color_mode)
        self.save.capture_settings(self.settings)
        self.save.flush()

    # -- frame ---------------------------------------------------------------
    def step(self, dt: float) -> None:
        screen = self.screen
        cols, rows = self.term.size()
        if (cols, rows) != (screen.cols, screen.rows):
            screen.resize(cols, rows)
        if getattr(self.term, "redraw_requested", False):
            screen.force_redraw()
            self.term.redraw_requested = False
        events = self.input.poll()
        self.time += dt
        self.save.playtime += min(dt, 0.5)                       # playtime counter (capped so a stalled frame never inflates it)
        self._pt_flush = getattr(self, "_pt_flush", 0.0) + dt
        if self._pt_flush >= 60.0:
            self._pt_flush = 0.0
            self.save.flush()
        screen.clear()
        if cols < MIN_SIZE[0] or rows < MIN_SIZE[1]:
            self._draw_too_small(cols, rows)
            screen.flush()
            return
        tr = self.transition
        if tr:
            tr.t += dt
            if tr.phase == "out" and tr.progress >= 1:
                self._swap(tr.scene)
                tr.phase, tr.t = "in", 0.0
            elif tr.phase == "in" and tr.progress >= 1:
                self.transition = tr = None
        elif events:
            for ev in events:
                if self.transition is not None:
                    break
                self.stack[-1].handle(ev)
        if self.stack:
            self.stack[-1].update(dt)
            for s in self.stack[:-1]:
                if s.ambient:
                    s.update_ambient(dt)
            base = len(self.stack) - 1
            while base > 0 and self.stack[base].overlay:
                base -= 1
            for s in self.stack[base:]:
                s.draw(screen)
            if self.stack[base].show_wallet and not (tr and tr.phase == "out" and tr.progress > 0.9):
                self._wallet_flash = max(0.0, getattr(self, "_wallet_flash", 0.0) - dt)
                draw_wallet(screen, self.save, self._wallet_flash)
        tr = self.transition
        if tr:
            p = tr.progress
            if tr.phase == "out":
                fac = [clamp(1 - (p * 1.6 - r / rows * 0.6), 0, 1) for r in range(rows)]
            else:
                fac = [clamp(p * 1.6 - r / rows * 0.6, 0, 1) for r in range(rows)]
            screen.fade_rows(fac)
        screen.flush()

    def _draw_too_small(self, cols: int, rows: int) -> None:
        s = self.screen
        lines = ["TerminalTanks requires a larger terminal window.", "",
                 f"Current: {cols}x{rows}", f"Recommended: {RECOMMENDED_SIZE[0]}x{RECOMMENDED_SIZE[1]}"]
        y = max(0, rows // 2 - 2)
        for i, line in enumerate(lines):
            s.center(y + i, line[:cols], Palette.AMBER if i == 0 else Palette.TEXT if i else Palette.TEXT)

    def frames(self):
        """Yields once per rendered frame. run() paces it with sleep(); the web backend lets the browser pace it."""
        self._swap(BootScene(self))
        last = time.perf_counter()
        while self.running:
            start = time.perf_counter()
            dt = min(start - last, 0.1)
            last = start
            self.step(dt)
            yield

    def run(self) -> None:
        frame = 1.0 / FPS
        began = time.perf_counter()
        for _ in self.frames():
            spare = frame - (time.perf_counter() - began)
            if spare > 0:
                time.sleep(spare)
            began = time.perf_counter()


def _parse_cli(argv: Optional[Sequence[str]]):
    parser = argparse.ArgumentParser(prog="terminaltanks", description="Turn-based tank artillery for the terminal.",
                                     epilog=f"Created by {CREDIT_NAME} - https://{CREDIT_URL}")
    parser.add_argument("--color", choices=("auto", "truecolor", "256", "16"), default="auto", help="force a colour mode")
    parser.add_argument("--seed", type=int, default=None, help="seed maps and AI for reproducible matches")
    parser.add_argument("--save-file", default=None, help="progress file (default: per-user config directory)")
    parser.add_argument("--reset-progress", action="store_true", help="delete saved tournament progress and unlocks, then exit")
    parser.add_argument("--version", action="version", version=f"TerminalTanks {__version__} by {CREDIT_NAME} (https://{CREDIT_URL})")
    args = parser.parse_args(argv)
    mode = {"auto": ColorMode.AUTO, "truecolor": ColorMode.TRUE, "256": ColorMode.ANSI256, "16": ColorMode.ANSI16}[args.color]
    save_path = Path(args.save_file).expanduser() if args.save_file else default_save_path()
    return args, mode, save_path


def _reset_progress(save_path: Path) -> int:
    try:
        save_path.unlink()
        print(f"Progress reset ({save_path}).")
    except FileNotFoundError:
        print("No saved progress found.")
    return 0


def main(argv: Optional[Sequence[str]] = None) -> int:
    args, mode, save_path = _parse_cli(argv)
    if args.reset_progress:
        return _reset_progress(save_path)
    try:
        with TerminalIO() as term:
            Application(term, args.seed, mode, save_path).run()
    except KeyboardInterrupt:
        pass
    except TerminalError as exc:
        print(exc, file=sys.stderr)
        return 1
    except Exception:
        traceback.print_exc()
        print("\nTerminalTanks crashed; your terminal has been restored.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
