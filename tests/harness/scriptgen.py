"""Builds melonDS harness scripts (see tools/harness/harness.cpp) from a tiny DSL so tests read like play sessions."""
OSK = ["1234567890", "QWERTYUIOP", "ASDFGHJKL-", "ZXCVBNM_. "]


class Script:
    def __init__(self, start=380):
        self.f, self.lines = start, []

    def wait(self, n):
        self.f += n
        return self

    def tap(self, key, gap=12):
        self.lines.append(f"{self.f} tap {key}")
        self.f += gap
        return self

    def taps(self, key, n, gap=10):
        for _ in range(n):
            self.tap(key, gap)
        return self

    def touch(self, x, y, gap=12):
        self.lines.append(f"{self.f} touch {x} {y}")
        self.lines.append(f"{self.f + 4} untouch")
        self.f += gap
        return self

    def osk_type(self, text, osk_row=3):
        """Types on the on-screen keyboard (ui.c osk_draw: 10 keys of 4 cells, x0 = 5, rows every 2 cells)."""
        for ch in text.upper():
            for r, row in enumerate(OSK):
                if ch in row:
                    c = row.index(ch)
                    self.touch((5 + c * 4 + 1) * 5 + 2, (osk_row + r * 2) * 8 + 4)
                    break
        return self

    def osk_ok(self, osk_row=3):
        return self.touch((5 + 7 * 4 + 2) * 5, (osk_row + 8) * 8 + 4)

    def shot(self, name, settle=40):
        self.f += settle
        self.lines.append(f"{self.f} shot {name}")
        self.f += 2
        return self

    def save(self, path):
        self.lines.append(f"{self.f + 2} end")
        open(path, "w").write("\n".join(self.lines) + "\n")
        return self.f
