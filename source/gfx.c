#include <math.h>
#include <stdio.h>
#include <string.h>
#include "gfx.h"

const RGB P_BG = {5, 9, 15}, P_PANEL = {9, 16, 26}, P_PANEL_HI = {16, 38, 52}, P_LINE = {38, 118, 128}, P_PRIMARY = {96, 232, 212},
          P_PRIMARY_DIM = {44, 116, 112}, P_AMBER = {255, 184, 64}, P_TEXT = {208, 222, 232}, P_MUTED = {104, 124, 140},
          P_DANGER = {255, 92, 92}, P_HEART = {255, 82, 108}, P_HEART_OFF = {76, 52, 66}, P_OK = {124, 240, 152},
          P_WHITE = {255, 255, 255}, P_INK = {8, 12, 18}, P_GOLD = {255, 206, 100};

/* ---------------------------------------------------------------- maths */
#define LUT 1024
static float g_sin[LUT + 1];
static bool g_lut;
static void lut_init(void) {
    for (int i = 0; i <= LUT; i++) g_sin[i] = sinf(i * (6.2831853f / LUT));
    g_lut = true;
}
float fsin(float a) {
    if (!g_lut) lut_init();
    /* 16.16 fixed point phase: one float multiply, no floorf (large angles are wrapped first) */
    if (a > 150.0f || a < -150.0f) a -= 6.2831853f * (float)(int32_t)(a * 0.15915494f);
    int32_t ph = (int32_t)(a * (LUT * 65536.0f / 6.2831853f));
    int i = (ph >> 16) & (LUT - 1);
    float fr = (float)(ph & 0xFFFF) * (1.0f / 65536.0f);
    return g_sin[i] + (g_sin[i + 1] - g_sin[i]) * fr;
}
float fcos(float a) { return fsin(a + 1.5707963f); }
float fsqrt(float v) {
    if (v <= 0) return 0;
    union { float f; uint32_t i; } u = {v};
    u.i = 0x1fbd1df5 + (u.i >> 1);
    float y = u.f;
    y = 0.5f * (y + v / y);
    return 0.5f * (y + v / y);
}
float fhypot(float x, float y) { return fsqrt(x * x + y * y); }

/* ---------------------------------------------------------------- colours */
/* integer mixes: one float multiply per call instead of six (the ARM9 has no FPU) */
RGB cmix(RGB a, RGB b, float t) {
    if (t <= 0) return a;
    if (t >= 1) return b;
    int k = (int)(t * 256.0f);
    RGB r = {(uint8_t)(a.r + (((b.r - a.r) * k) >> 8)), (uint8_t)(a.g + (((b.g - a.g) * k) >> 8)), (uint8_t)(a.b + (((b.b - a.b) * k) >> 8))};
    return r;
}
RGB cshade(RGB c, float k) {
    if (k <= 0) return (RGB){0, 0, 0};
    int q = (int)(k * 256.0f);
    int r = (c.r * q) >> 8, g = (c.g * q) >> 8, b = (c.b * q) >> 8;
    RGB o = {(uint8_t)(r > 255 ? 255 : r), (uint8_t)(g > 255 ? 255 : g), (uint8_t)(b > 255 ? 255 : b)};
    return o;
}
RGB cgradient(const float *ts, const RGB *cs, int n, float t) {
    if (t <= ts[0]) return cs[0];
    for (int i = 0; i < n - 1; i++)
        if (t <= ts[i + 1]) return cmix(cs[i], cs[i + 1], ts[i + 1] > ts[i] ? (t - ts[i]) / (ts[i + 1] - ts[i]) : 1.0f);
    return cs[n - 1];
}
RGB chsv(float h, float s, float v) {
    /* fixed point: h6 = hue * 6 in 1/256 steps, s and v in 0..256 */
    int h6 = (int)(h * 1536.0f) % 1536;
    if (h6 < 0) h6 += 1536;
    int si = (int)(s * 256.0f), vi = (int)(v * 255.0f);
    int i = h6 >> 8, f = h6 & 255;
    int p = (vi * (256 - si)) >> 8, q = (vi * (65536 - si * f)) >> 16, t = (vi * (65536 - si * (256 - f))) >> 16;
    int r, g, b;
    switch (i) {
    case 0: r = vi; g = t; b = p; break;
    case 1: r = q; g = vi; b = p; break;
    case 2: r = p; g = vi; b = t; break;
    case 3: r = p; g = q; b = vi; break;
    case 4: r = t; g = p; b = vi; break;
    default: r = vi; g = p; b = q; break;
    }
    RGB o = {(uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r), (uint8_t)(g < 0 ? 0 : g > 255 ? 255 : g), (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b)};
    return o;
}

Px pxmix(Px a, Px b, float t) {
    if (t <= 0) return a;
    if (t >= 1) return b;
    int k = (int)(t * 256);
    int ar = a & 31, ag = (a >> 5) & 31, ab = (a >> 10) & 31;
    int br = b & 31, bg = (b >> 5) & 31, bb = (b >> 10) & 31;
    int r = ar + (((br - ar) * k) >> 8), g = ag + (((bg - ag) * k) >> 8), bl = ab + (((bb - ab) * k) >> 8);
    return (Px)(r | (g << 5) | (bl << 10) | 0x8000);
}

/* ---------------------------------------------------------------- surfaces */
void surf_fill(Surf *s, Px c) {
    for (int y = 0; y < s->h; y++) {
        Px *row = s->px + y * s->stride;
        for (int x = 0; x < s->w; x++) row[x] = c;
    }
}
void surf_rect(Surf *s, int x, int y, int w, int h, Px c) {
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y, x1 = x + w > s->w ? s->w : x + w, y1 = y + h > s->h ? s->h : y + h;
    for (int yy = y0; yy < y1; yy++) {
        Px *row = s->px + yy * s->stride;
        for (int xx = x0; xx < x1; xx++) row[xx] = c;
    }
}
void surf_blend(Surf *s, int x, int y, RGB c, float a) {
    if ((unsigned)x >= (unsigned)s->w || (unsigned)y >= (unsigned)s->h || a <= 0.02f) return;
    Px *p = &s->px[y * s->stride + x];
    *p = pxmix(*p, rgb15(c), a);
}
void surf_rect_blend(Surf *s, int x, int y, int w, int h, RGB c, float a) {
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y, x1 = x + w > s->w ? s->w : x + w, y1 = y + h > s->h ? s->h : y + h;
    if (a <= 0) return;
    Px cc = rgb15(c);
    int k = a >= 1 ? 256 : (int)(a * 256);
    int br = cc & 31, bg = (cc >> 5) & 31, bb = (cc >> 10) & 31;
    for (int yy = y0; yy < y1; yy++) {
        Px *row = s->px + yy * s->stride;
        for (int xx = x0; xx < x1; xx++) {
            Px p = row[xx];
            int ar = p & 31, ag = (p >> 5) & 31, ab = (p >> 10) & 31;
            row[xx] = (Px)((ar + (((br - ar) * k) >> 8)) | ((ag + (((bg - ag) * k) >> 8)) << 5) | ((ab + (((bb - ab) * k) >> 8)) << 10) | 0x8000);
        }
    }
}
void surf_tint(Surf *s, int x, int y, int w, int h, RGB c, float a) { surf_rect_blend(s, x, y, w, h, c, a); }
void surf_dim(Surf *s, float k) {
    int kk = (int)(k * 256);
    for (int y = 0; y < s->h; y++) {
        Px *row = s->px + y * s->stride;
        for (int x = 0; x < s->w; x++) {
            Px p = row[x];
            int r = ((p & 31) * kk) >> 8, g = (((p >> 5) & 31) * kk) >> 8, b = (((p >> 10) & 31) * kk) >> 8;
            row[x] = (Px)(r | (g << 5) | (b << 10) | 0x8000);
        }
    }
}

/* ---------------------------------------------------------------- font */
extern const int FONT_N;
extern const uint16_t FONT_CP[];
extern const uint8_t FONT_BITS[][8];

static int font_search(uint32_t cp);
int font_index(uint32_t cp) {
    /* cached: ASCII directly, everything else through a small direct-mapped cache */
    static int16_t ascii[128];
    static uint32_t ckey[256];
    static int16_t cval[256];
    static bool init;
    if (!init) {
        for (int i = 0; i < 128; i++) ascii[i] = (int16_t)font_search((uint32_t)i);
        for (int i = 0; i < 256; i++) ckey[i] = 0xFFFFFFFFu;
        init = true;
    }
    if (cp < 128) return ascii[cp];
    uint32_t h = (cp * 2654435761u) >> 24;
    if (ckey[h] != cp) { ckey[h] = cp; cval[h] = (int16_t)font_search(cp); }
    return cval[h];
}
static int font_search(uint32_t cp) {
    int lo = 0, hi = FONT_N - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (FONT_CP[mid] == cp) return mid;
        if (FONT_CP[mid] < cp) lo = mid + 1;
        else hi = mid - 1;
    }
    return cp == '?' ? 0 : font_search('?');
}
const uint8_t *font_bits(int idx) { return FONT_BITS[idx]; }
static const uint8_t *glyph(uint32_t cp) { return FONT_BITS[font_index(cp)]; }

uint32_t utf8_next(const char **p) {
    const unsigned char *s = (const unsigned char *)*p;
    uint32_t c = *s++;
    if (c >= 0xF0) { c = ((c & 7) << 18) | ((s[0] & 63) << 12) | ((s[1] & 63) << 6) | (s[2] & 63); s += 3; }
    else if (c >= 0xE0) { c = ((c & 15) << 12) | ((s[0] & 63) << 6) | (s[1] & 63); s += 2; }
    else if (c >= 0xC0) { c = ((c & 31) << 6) | (s[0] & 63); s += 1; }
    *p = (const char *)s;
    return c;
}

int text_len(const char *s) {
    int n = 0;
    while (*s) { utf8_next(&s); n++; }
    return n;
}

int text_px(Surf *s, int x, int y, const char *str, RGB fg, const RGB *bg) {
    Px f = rgb15(fg), b = bg ? rgb15(*bg) : 0;
    int x0 = x;
    while (*str) {
        uint32_t cp = utf8_next(&str);
        const uint8_t *g = cp == ' ' ? NULL : glyph(cp);
        for (int r = 0; r < CELL_H; r++) {
            int yy = y + r;
            if ((unsigned)yy >= (unsigned)s->h) continue;
            Px *row = s->px + yy * s->stride;
            uint8_t bits = g ? g[r] : 0;
            for (int c = 0; c < CELL_W; c++) {
                int xx = x + c;
                if ((unsigned)xx >= (unsigned)s->w) continue;
                if (bits & (16 >> c)) row[xx] = f;
                else if (bg) row[xx] = b;
            }
        }
        x += CELL_W;
    }
    return x - x0;
}

void cell_text(Surf *s, int col, int row, const char *t, RGB fg, const RGB *bg) { text_px(s, col * CELL_W, row * CELL_H, t, fg, bg); }
void cell_center(Surf *s, int row, const char *t, RGB fg, const RGB *bg, int col0, int width) {
    int n = text_len(t);
    cell_text(s, col0 + (width - n) / 2, row, t, fg, bg);
}
void cell_fill(Surf *s, int col, int row, int w, int h, RGB bg) { surf_rect(s, col * CELL_W, row * CELL_H, w * CELL_W, h * CELL_H, rgb15(bg)); }

void cell_box(Surf *s, int col, int row, int w, int h, int style, RGB fg, RGB bg, const char *title, RGB title_fg) {
    static const char *const ST[4][6] = {{"╔", "╗", "╚", "╝", "═", "║"}, {"┌", "┐", "└", "┘", "─", "│"},
                                         {"╭", "╮", "╰", "╯", "─", "│"}, {"┏", "┓", "┗", "┛", "━", "┃"}};
    const char *const *g = ST[style & 3];
    cell_fill(s, col, row, w, h, bg);
    for (int i = 1; i < w - 1; i++) { cell_text(s, col + i, row, g[4], fg, &bg); cell_text(s, col + i, row + h - 1, g[4], fg, &bg); }
    for (int j = 1; j < h - 1; j++) { cell_text(s, col, row + j, g[5], fg, &bg); cell_text(s, col + w - 1, row + j, g[5], fg, &bg); }
    cell_text(s, col, row, g[0], fg, &bg);
    cell_text(s, col + w - 1, row, g[1], fg, &bg);
    cell_text(s, col, row + h - 1, g[2], fg, &bg);
    cell_text(s, col + w - 1, row + h - 1, g[3], fg, &bg);
    if (title && title[0]) {
        char lab[64];
        int n = text_len(title);
        if (n > w - 4) n = w - 4;
        snprintf(lab, sizeof lab, " %s ", title);
        cell_text(s, col + (w - n - 2) / 2, row, lab, title_fg, &bg);
    }
}

int wrap_text(const char *text, int width, char lines[][64], int max_lines) {
    int n = 0;
    const char *p = text;
    while (*p && n < max_lines) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *best = NULL, *q = p;
        int cnt = 0;
        while (*q) {
            const char *start = q;
            uint32_t c = utf8_next(&q);
            cnt++;
            if (c == ' ') best = start;
            if (cnt > width) { q = start; break; }
        }
        const char *end = (*q == 0) ? q : (best && best > p ? best : q);
        int len = (int)(end - p);
        if (len > 63) len = 63;
        memcpy(lines[n], p, (size_t)len);
        lines[n][len] = 0;
        while (len > 0 && lines[n][len - 1] == ' ') lines[n][--len] = 0;
        n++;
        p = end;
    }
    if (*p && n == max_lines && n > 0) {         /* truncated: end with an ellipsis */
        int l = (int)strlen(lines[n - 1]);
        if (l > 60) l = 60;
        while (l > 0 && text_len(lines[n - 1]) >= width) lines[n - 1][--l] = 0;
        strcat(lines[n - 1], "…");
    }
    return n;
}

int cell_wrap_center(Surf *s, int row, const char *text, int width, RGB fg, const RGB *bg, int col0, int w, int maxl) {
    char lines[6][64];
    int n = wrap_text(text, width, lines, maxl > 6 ? 6 : maxl);
    for (int i = 0; i < n; i++) cell_center(s, row + i, lines[i], fg, bg, col0, w);
    return n;
}

void cv_blend(Surf *cv, int x, int y, RGB c, float a) {
    int py = cv->h - 1 - y;
    if ((unsigned)x < (unsigned)cv->w && (unsigned)py < (unsigned)cv->h && a > 0.02f) {
        Px *p = &cv->px[py * cv->stride + x];
        *p = pxmix(*p, rgb15(c), a);
    }
}
void cv_plotf(Surf *cv, float x, float y, RGB c) { cv_plot(cv, (int)floorf(x), (int)floorf(y), rgb15(c)); }
void cv_blendf(Surf *cv, float x, float y, RGB c, float a) { cv_blend(cv, (int)floorf(x), (int)floorf(y), c, a); }
