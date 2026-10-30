#include <stdio.h>
#include <string.h>
#include "term.h"

int G_SPACE, G_UPPER;

void term_init(void) {
    G_SPACE = font_index(' ');
    G_UPPER = font_index(0x2580);
}

int utf8_len(const char *s) { return text_len(s); }

void term_clear(Term *t, RGB bg) {
    t->cols = T_COLS;
    t->rows = T_ROWS;
    Px b = rgb15(bg);
    for (int y = 0; y < T_ROWS; y++)
        for (int x = 0; x < T_COLS; x++) t->c[y][x] = (Cell){(uint16_t)G_SPACE, b, b};
}

void term_clear_transparent(Term *t) {
    t->cols = T_COLS;
    t->rows = T_ROWS;
    for (int y = 0; y < T_ROWS; y++)
        for (int x = 0; x < T_COLS; x++) t->c[y][x] = (Cell){G_NONE, 0, 0};
}

static inline Px old_bg(const Cell *c) {
    if (c->g == G_UPPER) return pxmix(c->fg, c->bg, 0.5f);
    return c->bg;
}

void term_putg(Term *t, int x, int y, int g, Px fg, Px bg) {
    if ((unsigned)x < (unsigned)t->cols && (unsigned)y < (unsigned)t->rows) t->c[y][x] = (Cell){(uint16_t)g, fg, bg};
}

void term_put(Term *t, int x, int y, uint32_t cp, RGB fg, const RGB *bg) {
    if ((unsigned)x >= (unsigned)t->cols || (unsigned)y >= (unsigned)t->rows) return;
    Cell *c = &t->c[y][x];
    Px b = bg ? rgb15(*bg) : (c->g == G_NONE ? 0 : old_bg(c));
    c->g = (uint16_t)(cp == ' ' ? G_SPACE : font_index(cp));
    c->fg = rgb15(fg);
    c->bg = b;
    if (!bg && b == 0) c->bg = 0;   /* over a transparent cell: glyph only (see term_render) */
}

void term_cell(Term *t, int x, int y, RGB col) {
    if ((unsigned)x >= (unsigned)t->cols || (unsigned)y >= (unsigned)t->rows) return;
    Px p = rgb15(col);
    t->c[y][x] = (Cell){(uint16_t)G_SPACE, p, p};
}

void term_text(Term *t, int x, int y, const char *s, RGB fg, const RGB *bg) {
    if ((unsigned)y >= (unsigned)t->rows) return;
    while (*s) {
        uint32_t cp = utf8_next(&s);
        if (cp != ' ' || bg) term_put(t, x, y, cp, fg, bg);
        x++;
    }
}

void term_center(Term *t, int y, const char *s, RGB fg, const RGB *bg, int x0, int width) {
    if (width < 0) width = t->cols;
    int n = text_len(s);
    int d = width - n;
    term_text(t, x0 + (d >= 0 ? d / 2 : -((-d + 1) / 2)), y, s, fg, bg);   /* python floor division */
}

void term_fill(Term *t, int x, int y, int w, int h, RGB bg) { term_fill_ch(t, x, y, w, h, bg, ' ', P_TEXT); }

void term_fill_ch(Term *t, int x, int y, int w, int h, RGB bg, uint32_t cp, RGB fg) {
    Px b = rgb15(bg), f = rgb15(fg);
    int g = cp == ' ' ? G_SPACE : font_index(cp);
    int y0 = y < 0 ? 0 : y, y1 = y + h > t->rows ? t->rows : y + h;
    int x0 = x < 0 ? 0 : x, x1 = x + w > t->cols ? t->cols : x + w;
    for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++) t->c[yy][xx] = (Cell){(uint16_t)g, f, b};
}

void term_box(Term *t, int x, int y, int w, int h, int style, RGB fg, RGB bg, const char *title, RGB title_fg) {
    static const uint16_t ST[4][6] = {{0x2554, 0x2557, 0x255A, 0x255D, 0x2550, 0x2551}, {0x250C, 0x2510, 0x2514, 0x2518, 0x2500, 0x2502},
                                      {0x256D, 0x256E, 0x2570, 0x256F, 0x2500, 0x2502}, {0x250F, 0x2513, 0x2517, 0x251B, 0x2501, 0x2503}};
    const uint16_t *g = ST[style & 3];
    term_fill(t, x, y, w, h, bg);
    for (int i = 1; i < w - 1; i++) {
        term_put(t, x + i, y, g[4], fg, &bg);
        term_put(t, x + i, y + h - 1, g[4], fg, &bg);
    }
    for (int j = 1; j < h - 1; j++) {
        term_put(t, x, y + j, g[5], fg, &bg);
        term_put(t, x + w - 1, y + j, g[5], fg, &bg);
    }
    term_put(t, x, y, g[0], fg, &bg);
    term_put(t, x + w - 1, y, g[1], fg, &bg);
    term_put(t, x, y + h - 1, g[2], fg, &bg);
    term_put(t, x + w - 1, y + h - 1, g[3], fg, &bg);
    if (title && title[0]) {
        char lab[96];
        snprintf(lab, sizeof lab, " %s ", title);
        int n = text_len(lab);
        int d = w - n;
        term_text(t, x + (d >= 0 ? d / 2 : -((-d + 1) / 2)), y, lab, title_fg, &bg);
    }
}

void term_blit(Term *t, const Surf *cv, int x, int y, int dx, int dy) {
    int h = cv->h;
    for (int r = 0; r < h / 2; r++) {
        int ty = 2 * r - dy, by = 2 * r + 1 - dy;
        ty = ty < 0 ? 0 : (ty > h - 1 ? h - 1 : ty);
        by = by < 0 ? 0 : (by > h - 1 ? h - 1 : by);
        if ((unsigned)(y + r) >= (unsigned)t->rows) continue;
        const Px *top = cv->px + ty * cv->stride, *bot = cv->px + by * cv->stride;
        for (int c = 0; c < cv->w; c++) {
            int xx = x + c;
            if ((unsigned)xx >= (unsigned)t->cols) continue;
            int sc = c - dx;
            sc = sc < 0 ? 0 : (sc >= cv->w ? cv->w - 1 : sc);
            t->c[y + r][xx] = (Cell){(uint16_t)G_UPPER, top[sc], bot[sc]};
        }
    }
}

void term_blit_masked(Term *t, const Surf *cv, int x, int y, int x0, int x1) {
    for (int r = 0; r < cv->h / 2; r++) {
        if ((unsigned)(y + r) >= (unsigned)t->rows) continue;
        const Px *top = cv->px + (2 * r) * cv->stride, *bot = cv->px + (2 * r + 1) * cv->stride;
        for (int c = x0; c < x1 && c < cv->w; c++) {
            if (c < 0) continue;
            Px a = top[c] & 0x7FFF, b = bot[c] & 0x7FFF;
            if (!a && !b) continue;
            int xx = x + c;
            if ((unsigned)xx >= (unsigned)t->cols) continue;
            Cell *cell = &t->c[y + r][xx];
            Px bgc = cell->bg;
            *cell = (Cell){(uint16_t)G_UPPER, a ? top[c] : bgc, b ? bot[c] : bgc};
        }
    }
}

void term_tint(Term *t, int x, int y, int w, int h, RGB c, float a) {
    Px p = rgb15(c);
    int y0 = y < 0 ? 0 : y, y1 = y + h > t->rows ? t->rows : y + h;
    int x0 = x < 0 ? 0 : x, x1 = x + w > t->cols ? t->cols : x + w;
    for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++) {
            Cell *cl = &t->c[yy][xx];
            if (cl->g == G_NONE) continue;
            cl->fg = pxmix(cl->fg, p, a);
            cl->bg = pxmix(cl->bg, p, a);
        }
}

static inline Px pshade(Px p, int kk) {
    int r = ((p & 31) * kk) >> 8, g = (((p >> 5) & 31) * kk) >> 8, b = (((p >> 10) & 31) * kk) >> 8;
    return (Px)(r | (g << 5) | (b << 10) | 0x8000);
}

void term_dim(Term *t, float k) {
    int kk = (int)(k * 256);
    for (int y = 0; y < t->rows; y++)
        for (int x = 0; x < t->cols; x++) {
            Cell *cl = &t->c[y][x];
            cl->fg = pshade(cl->fg, kk);
            cl->bg = pshade(cl->bg, kk);
        }
}

void term_fade_rows(Term *t, const float *k) {
    for (int y = 0; y < t->rows; y++) {
        if (k[y] >= 0.999f) continue;
        int kk = (int)(k[y] * 256);
        for (int x = 0; x < t->cols; x++) {
            Cell *cl = &t->c[y][x];
            if (cl->g == G_NONE) cl->g = (uint16_t)G_SPACE, cl->fg = cl->bg = 0x8000;
            cl->fg = pshade(cl->fg, kk);
            cl->bg = pshade(cl->bg, kk);
        }
    }
}

RGB term_bg(const Term *t, int x, int y) {
    if ((unsigned)x >= (unsigned)t->cols || (unsigned)y >= (unsigned)t->rows) return P_BG;
    return px_rgb(t->c[y][x].bg);
}

int term_wrap_center(Term *t, int y, const char *text, int width, RGB fg, const RGB *bg, int x, int w, int maxl) {
    char lines[8][64];
    int n = wrap_text(text, width, lines, maxl > 8 ? 8 : maxl);
    if (n == 0) n = 1, lines[0][0] = 0;
    for (int i = 0; i < n; i++) term_center(t, y + i, lines[i], fg, bg, x, w);
    return n;
}

/* Cells with bg == 0 (no 0x8000 bit) over a transparent cell draw only the glyph's foreground pixels. */
void term_render(const Term *t, Surf *s, int row0) {
    for (int yy = 0; yy < ROWS; yy++) {
        int y = row0 + yy;
        for (int x = 0; x < t->cols; x++) {
            const Cell *c = &t->c[y][x];
            if (c->g == G_NONE) continue;
            Px *dst = s->px + (yy * CELL_H) * s->stride + x * CELL_W;
            Px f = c->fg, b = c->bg;
            bool opaque = (b & 0x8000) != 0;
            if (c->g == G_UPPER) {
                for (int r = 0; r < 4; r++, dst += s->stride) dst[0] = dst[1] = dst[2] = dst[3] = dst[4] = f;
                for (int r = 0; r < 4; r++, dst += s->stride) dst[0] = dst[1] = dst[2] = dst[3] = dst[4] = b;
                continue;
            }
            if (c->g == G_SPACE) {
                if (!opaque) continue;
                for (int r = 0; r < 8; r++, dst += s->stride) dst[0] = dst[1] = dst[2] = dst[3] = dst[4] = b;
                continue;
            }
            const uint8_t *bits = font_bits(c->g);
            for (int r = 0; r < 8; r++, dst += s->stride) {
                uint8_t m = bits[r];
                if (opaque) {
                    dst[0] = (m & 16) ? f : b;
                    dst[1] = (m & 8) ? f : b;
                    dst[2] = (m & 4) ? f : b;
                    dst[3] = (m & 2) ? f : b;
                    dst[4] = (m & 1) ? f : b;
                } else {
                    if (m & 16) dst[0] = f;
                    if (m & 8) dst[1] = f;
                    if (m & 4) dst[2] = f;
                    if (m & 2) dst[3] = f;
                    if (m & 1) dst[4] = f;
                }
            }
        }
    }
}
