#include <math.h>
#include <stdio.h>
#include <string.h>
#include "ui.h"
#include "render.h"

static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }

/* ------------------------------------------------------------------ MenuList */
void menu_init(MenuList *m, const char *const *items, int n) {
    m->n = n > 24 ? 24 : n;
    for (int i = 0; i < m->n; i++) m->items[i] = items[i];
    m->index = 0;
}

int menu_handle(MenuList *m, const IEv *e, int hot_base) {
    if (e->action == A_UP) m->index = (m->index - 1 + m->n) % m->n;
    else if (e->action == A_DOWN) m->index = (m->index + 1) % m->n;
    else if (ev_confirm(e)) return m->index;
    else if (e->action == A_TOUCH && e->raw >= hot_base && e->raw < hot_base + m->n) {
        int i = e->raw - hot_base;
        if (i == m->index) return i;    /* tap the highlighted row again to choose it */
        m->index = i;
    }
    return -1;
}

void menu_draw(Term *t, const MenuList *m, int cx, int y, float time, int gap, int width, int hot_base) {
    for (int i = 0; i < m->n; i++) {
        int row = y + i * gap;
        int x0 = cx - width / 2;
        if (i == m->index) {
            float pulse = 0.5f + 0.5f * fsin(time * 6);
            term_fill(t, x0, row, width, 1, P_PANEL_HI);
            term_center(t, row, m->items[i], P_WHITE, &P_PANEL_HI, x0, width);
            RGB arrow = cmix(P_PRIMARY_DIM, P_PRIMARY, pulse);
            term_put(t, x0 + 1, row, 0x25B6, arrow, &P_PANEL_HI);
            term_put(t, x0 + width - 2, row, 0x25C0, arrow, &P_PANEL_HI);
        } else {
            term_center(t, row, m->items[i], P_MUTED, NULL, x0, width);
        }
        if (hot_base >= 0) hot_add(x0, row, width, gap > 1 ? gap : 1, hot_base + i);
    }
}

void ui_keycaps(Term *t, int x, int y, const Keycap *k, int n, const RGB *bg) {
    for (int i = 0; i < n; i++) {
        term_text(t, x, y, k[i].key, P_AMBER, bg);
        x += text_len(k[i].key) + 1;
        term_text(t, x, y, k[i].desc, P_MUTED, bg);
        x += text_len(k[i].desc) + 2;
    }
}

void ui_keycaps_center(Term *t, int y, const Keycap *k, int n, const RGB *bg, int x0, int w) {
    int len = 0;
    for (int i = 0; i < n; i++) len += text_len(k[i].key) + 1 + text_len(k[i].desc) + (i < n - 1 ? 2 : 0);
    ui_keycaps(t, x0 + (w - len) / 2, y, k, n, bg);
}

void ui_panel(Term *t, int w, int h, const char *title, int y, int area_y, int area_h, RGB fg, int *ox, int *oy) {
    int x = (t->cols - w) / 2;
    if (y < 0) y = area_y + (area_h - h) / 2;
    term_box(t, x, y, w, h, TB_DOUBLE, fg, P_PANEL, title, P_PRIMARY);
    if (ox) *ox = x;
    if (oy) *oy = y;
}

int ui_draw_hp(Term *t, int x, int y, const Tank *tk, RGB bg, bool right) {
    if (tk->max_hp <= 10) {
        int w = tk->max_hp * 2 - 1;
        int x0 = right ? x - w + 1 : x;
        for (int k = 0; k < tk->max_hp; k++)
            term_put(t, x0 + k * 2, y, k < tk->hp ? 0x2665 : 0x2661, k < tk->hp ? P_HEART : P_HEART_OFF, &bg);
        return w;
    }
    int n = tk->hp <= 0 ? 0 : (int)ceil(10.0 * tk->hp / tk->max_hp);
    if (tk->hp > 0) n = n < 1 ? 1 : (n > 10 ? 10 : n);
    char label[24];
    snprintf(label, sizeof label, " %d/%d", tk->hp > 0 ? tk->hp : 0, tk->max_hp);
    int w = 10 + (int)strlen(label);
    int x0 = right ? x - w + 1 : x;
    for (int k = 0; k < 10; k++) term_put(t, x0 + k, y, k < n ? 0x2588 : 0x2591, k < n ? P_HEART : P_HEART_OFF, &bg);
    term_text(t, x0 + 10, y, label, P_TEXT, &bg);
    return w;
}

void ui_hearts(int hp, bool spaced, int total, char *out, int n) {
    if (total > 10) { snprintf(out, (size_t)n, "\xE2\x99\xA5%d/%d", hp > 0 ? hp : 0, total); return; }
    out[0] = 0;
    int l = 0;
    for (int i = 0; i < total && l < n - 5; i++) {
        if (i && spaced) out[l++] = ' ';
        const char *h = i < hp ? "\xE2\x99\xA5" : "\xE2\x99\xA1";
        memcpy(out + l, h, 3);
        l += 3;
    }
    out[l] = 0;
}

void ui_draw_bar(Term *t, int x, int y, int w, float frac) {
    frac = clampf(frac, 0, 1);
    float full = frac * w;
    static const float GT[3] = {0, 0.6f, 1};
    RGB GC[3] = {P_OK, P_AMBER, P_DANGER};
    for (int i = 0; i < w; i++) {
        RGB col = cgradient(GT, GC, 3, i / (float)(w - 1 > 1 ? w - 1 : 1));
        if (i + 1 <= full) term_put(t, x + i, y, 0x2588, col, &P_PANEL);
        else if (i < full) term_put(t, x + i, y, 0x258C, col, &P_PANEL);
        else term_put(t, x + i, y, 0x2591, (RGB){52, 66, 78}, &P_PANEL);
    }
}

static void sw_cell(Term *t, int sx, int y, const char *label, RGB col, bool sel) {
    if (sel) {
        term_put(t, sx, y + 2, 0x2590, P_WHITE, &P_PANEL);
        term_put(t, sx + 1, y + 2, 0x2588, col, &P_PANEL);
        term_put(t, sx + 2, y + 2, 0x258C, P_WHITE, &P_PANEL);
        term_text(t, sx + 4, y + 2, label, col, &P_PANEL);
    } else {
        term_text(t, sx, y + 2, "\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88", col, &P_PANEL);
        term_text(t, sx + 4, y + 2, label, cshade(col, 0.6f), &P_PANEL);
    }
}

/* 24 colours as a 12x2 grid (+ animated RGB / ASTRAL / PRISM / VOID cells once unlocked) (l.7870) */
void ui_swatches(Term *t, int x, int y, int idx, int taken, bool rgb, bool rgb_sel, float time, bool astral_on, bool astral_sel,
                 int cur, bool prism_on, bool void_on) {
    if (rgb) sw_cell(t, x, y, "RGB", rainbow(time), rgb_sel);
    if (astral_on) sw_cell(t, x + 8, y, "ASTRAL", astral(time), astral_sel);
    if (prism_on) sw_cell(t, x + 19, y, "PRISM", prismatic(time), cur == PAINT_PRISMATIC);
    if (void_on) sw_cell(t, x + 29, y, "VOID", void_tint(time), cur == PAINT_VOID);
    for (int k = 0; k < N_COLORS; k++) {
        int sx = x + (k % 12) * 3, sy = y + k / 12;
        RGB col = COLOR_CHOICES[k].c;
        if (k == idx) {
            term_put(t, sx, sy, 0x2590, P_WHITE, &P_PANEL);
            term_put(t, sx + 1, sy, 0x2588, col, &P_PANEL);
            term_put(t, sx + 2, sy, 0x258C, P_WHITE, &P_PANEL);
        } else if (k == taken) {
            term_text(t, sx, sy, "\xE2\x95\xB3\xE2\x95\xB3\xE2\x95\xB3", cshade(col, 0.35f), &P_PANEL);
        } else {
            term_text(t, sx, sy, "\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88", col, &P_PANEL);
        }
        hot_add(sx, sy, 3, 1, 900 + k);
    }
}

void fmt_commas(long v, char *out, int n) {
    char tmp[32];
    snprintf(tmp, sizeof tmp, "%ld", v < 0 ? -v : v);
    int len = (int)strlen(tmp), o = 0;
    if (v < 0 && o < n - 1) out[o++] = '-';
    for (int i = 0; i < len && o < n - 1; i++) {
        out[o++] = tmp[i];
        int rem = len - 1 - i;
        if (rem > 0 && rem % 3 == 0 && o < n - 1) out[o++] = ',';
    }
    out[o] = 0;
}

void ui_wallet_text(const SaveData *s, char *out, int n) {
    char a[24], b[24];
    fmt_commas(s->coins, a, sizeof a);
    fmt_commas(s->credits, b, sizeof b);
    snprintf(out, (size_t)n, "\xE2\x97\x89 %s   \xE2\x97\x88 %s", a, b);
}

/* Coins (◉) and Tournament Credits (◈) in the top-right corner of every screen (l.7498) */
void ui_wallet(Term *t, const SaveData *s, float flash) {
    char a[24], b[24], coin[32], cred[32];
    fmt_commas(s->coins, a, sizeof a);
    fmt_commas(s->credits, b, sizeof b);
    snprintf(coin, sizeof coin, "\xE2\x97\x89 %s", a);
    snprintf(cred, sizeof cred, "\xE2\x97\x88 %s", b);
    int len = text_len(coin) + 3 + text_len(cred);
    int x = t->cols - len - 1;
    RGB bg = cmix((RGB){16, 24, 34}, (RGB){60, 52, 20}, clampf(flash, 0, 1));
    term_fill(t, x - 1, 0, len + 2, 1, bg);
    term_text(t, x, 0, coin, P_GOLD, &bg);
    term_text(t, x + text_len(coin) + 3, 0, cred, (RGB){140, 225, 255}, &bg);
}

void ui_prism_text(Term *t, int x, int y, const char *s, float time, float base, float spread, float sat, float val, const RGB *bg) {
    int i = 0;
    while (*s) {
        uint32_t cp = utf8_next(&s);
        if (cp != ' ') term_put(t, x + i, y, cp, chsv(base + 0.09f * fsin(time * 1.5f + i * spread * 3) + i * spread * 0.2f, sat, val), bg);
        i++;
    }
}

void fmt_playtime(double sec, char *out, int n) {
    long m = (long)(sec > 0 ? sec : 0) / 60;
    long y = m / 525600; m %= 525600;
    long mo = m / 43200; m %= 43200;
    long d = m / 1440; m %= 1440;
    long h = m / 60, mi = m % 60;
    long vals[4] = {y, mo, d, h};
    static const char *labs[4] = {"Y", "MO", "D", "H"};
    int o = 0;
    bool any = false;
    out[0] = 0;
    for (int i = 0; i < 4; i++)
        if (any || vals[i]) {
            any = true;
            o += snprintf(out + o, (size_t)(n - o), "%ld%s ", vals[i], labs[i]);
        }
    if (any) snprintf(out + o, (size_t)(n - o), "%02ldM", mi);
    else snprintf(out, (size_t)n, "%ldM", mi);
}

void ui_button(Term *t, int x, int y, const char *label, RGB fg, RGB bg, int hot_id) {
    char b[64];
    snprintf(b, sizeof b, "[ %s ]", label);
    term_text(t, x, y, b, fg, &bg);
    hot_add(x, y, text_len(b), 1, hot_id);
}

/* ------------------------------------------------------------------ big logos */
static const char *const W_TERMINAL[6] = {
    "████████╗███████╗██████╗ ███╗   ███╗██╗███╗   ██╗ █████╗ ██╗     ",
    "╚══██╔══╝██╔════╝██╔══██╗████╗ ████║██║████╗  ██║██╔══██╗██║     ",
    "   ██║   █████╗  ██████╔╝██╔████╔██║██║██╔██╗ ██║███████║██║     ",
    "   ██║   ██╔══╝  ██╔══██╗██║╚██╔╝██║██║██║╚██╗██║██╔══██║██║     ",
    "   ██║   ███████╗██║  ██║██║ ╚═╝ ██║██║██║ ╚████║██║  ██║███████╗",
    "   ╚═╝   ╚══════╝╚═╝  ╚═╝╚═╝     ╚═╝╚═╝╚═╝  ╚═══╝╚═╝  ╚═╝╚══════╝"};
static const char *const W_TANKS[6] = {
    "████████╗ █████╗ ███╗   ██╗██╗  ██╗███████╗",
    "╚══██╔══╝██╔══██╗████╗  ██║██║ ██╔╝██╔════╝",
    "   ██║   ███████║██╔██╗ ██║█████╔╝ ███████╗",
    "   ██║   ██╔══██║██║╚██╗██║██╔═██╗ ╚════██║",
    "   ██║   ██║  ██║██║ ╚████║██║  ██╗███████║",
    "   ╚═╝   ╚═╝  ╚═╝╚═╝  ╚═══╝╚═╝  ╚═╝╚══════╝"};
#define LW 3
#define LH 6

typedef struct { bool on; int kind, py, home, nwords; float time; } LogoReq;
static LogoReq g_logo;

static LogoReq g_logo_last;
bool ui_logo_pending(void) { return g_logo.on; }
/* true when the queued logo differs from the one already on screen */
bool ui_logo_changed(void) {
    if (g_logo.on != g_logo_last.on) return true;
    if (!g_logo.on) return false;
    return g_logo.kind != g_logo_last.kind || g_logo.py != g_logo_last.py || g_logo.home != g_logo_last.home ||
           g_logo.nwords != g_logo_last.nwords || g_logo.time != g_logo_last.time;
}
/* the screen was not re-rendered: the previous logo pixels are still there */
void ui_logo_keep(void) { g_logo_last = g_logo; g_logo.on = false; }

/* pixel masks of the logo glyphs in a 3x6 cell (bit x of row y) */
static void mini_glyph(uint32_t cp, uint8_t rows[LH]) {
    memset(rows, 0, LH);
    switch (cp) {
    case 0x2588: for (int y = 0; y < LH; y++) rows[y] = 7; break;                       /* █ */
    case 0x2550: rows[1] = rows[3] = 7; break;                                            /* ═ */
    case 0x2551: for (int y = 0; y < LH; y++) rows[y] = 5; break;                         /* ║ */
    case 0x2557: rows[1] = 7; rows[3] = 1; for (int y = 2; y < LH; y++) rows[y] |= 4; for (int y = 4; y < LH; y++) rows[y] |= 1; break;  /* ╗ */
    case 0x2554: rows[1] = 7; rows[3] = 4; for (int y = 2; y < LH; y++) rows[y] |= 1; for (int y = 4; y < LH; y++) rows[y] |= 4; break;  /* ╔ */
    case 0x255D: rows[3] = 7; rows[1] = 1; for (int y = 0; y < 3; y++) rows[y] |= 4; rows[0] |= 1; break;                                /* ╝ */
    case 0x255A: rows[3] = 7; rows[1] = 4; for (int y = 0; y < 3; y++) rows[y] |= 1; rows[0] |= 4; break;                                /* ╚ */
    default: break;
    }
}

/* Colour of one logo glyph. The gradient part depends only on (word, row): it is computed once per row; the moving
 * shimmer band only touches the few glyphs it covers, so most glyphs cost no float maths at all. */
static RGB logo_base(const LogoReq *L, int wi, int r) {
    float g = (r + wi * 6) / 11.0f;
    switch (L->kind) {
    case LOGO_MASTER: return cmix((RGB){255, 236, 150}, (RGB){214, 120, 60}, g);
    case LOGO_SECRET: return cmix((RGB){190, 255, 205}, (RGB){30, 170, 80}, g);
    case LOGO_THEME: {
        const HomeTheme *th = &HOME_THEMES[L->home];
        return cmix(th->logo_a, th->logo_b, (r + wi * 6) / (L->nwords == 2 ? 11.0f : 5.0f));
    }
    default: return cmix((RGB){150, 255, 235}, (RGB){50, 150, 235}, g);
    }
}
static RGB logo_shadow(const LogoReq *L, RGB base, int i) {
    switch (L->kind) {
    case LOGO_MASTER: return (RGB){70, 32, 90};
    case LOGO_SECRET: return ((int)(L->time * 6) % 9 == 0 && i % 2) ? (RGB){255, 60, 120} : (RGB){20, 90, 54};
    case LOGO_THEME: return cshade(base, 0.4f);
    default: return (RGB){30, 76, 100};
    }
}
static RGB logo_color(const LogoReq *L, RGB base, int wi, int r, int i, int shimmer, int width) {
    int d = i + r * 2 + wi * 14 - shimmer;
    if (d < 0) d = -d;
    if (d >= width) return base;
    float b = 1.0f - d / (float)width;
    switch (L->kind) {
    case LOGO_MASTER: return cmix(base, chsv(0.55f + i * 0.01f + L->time * 0.3f, 0.45f, 1.0f), b * 0.8f);
    case LOGO_SECRET: return cmix(base, (RGB){235, 255, 240}, b * 0.8f);
    default: return cmix(base, P_WHITE, b * 0.75f);
    }
}

void ui_logo_flush(Surf *s) {
    g_logo_last = g_logo;
    if (!g_logo.on) return;
    g_logo.on = false;
    const LogoReq *L = &g_logo;
    int py = L->py;
    Px bgc = rgb15(P_BG);
    int shimmer, swidth = 9;
    switch (L->kind) {
    case LOGO_MASTER: shimmer = (int)(L->time * 36) % 120 - 25; swidth = 10; break;
    case LOGO_SECRET: shimmer = (int)(L->time * 40) % 130 - 25; break;
    case LOGO_THEME: shimmer = (int)(L->time * 36) % 140 - 25; break;
    default: shimmer = (int)(L->time * 42) % 140 - 30; break;
    }
    for (int wi = 0; wi < L->nwords; wi++) {
        const char *const *rows = wi == 0 ? W_TERMINAL : W_TANKS;
        int width = wi == 0 ? 65 : 43;
        int x0 = (SCR_W - width * LW) / 2;
        for (int r = 0; r < 6; r++) {
            int jitter = 0;
            if (L->kind == LOGO_SECRET) {
                bool glitch = (int)(L->time * 6) % 9 == 0;
                if (glitch && (r * 7 + (int)(L->time * 6)) % 4 == 0) jitter = ((r + wi + (int)(L->time * 6)) % 2) ? LW : -LW;
            }
            const char *p = rows[r];
            RGB base = logo_base(L, wi, r);
            Px shadow_same = rgb15(logo_shadow(L, base, 0));
            for (int i = 0; *p; i++) {
                uint32_t cp = utf8_next(&p);
                if (cp == ' ') continue;
                uint8_t m[LH];
                mini_glyph(cp, m);
                Px pc;
                if (cp == 0x2588) pc = rgb15(logo_color(L, base, wi, r, i, shimmer, swidth));
                else pc = L->kind == LOGO_SECRET ? rgb15(logo_shadow(L, base, i)) : shadow_same;
                for (int yy = 0; yy < LH; yy++) {
                    int sy = py + r * LH + yy;
                    if ((unsigned)sy >= (unsigned)s->h) continue;
                    Px *row = s->px + sy * s->stride;
                    for (int xx = 0; xx < LW; xx++) {
                        int sx = x0 + i * LW + xx + jitter;
                        if ((unsigned)sx < (unsigned)s->w) row[sx] = (m[yy] & (1 << xx)) ? pc : bgc;
                    }
                }
            }
        }
        py += 6 * LH + (wi == 0 ? LH : 0);
    }
}

/* Reserves the rows, queues the pixel logo and draws the text lines of each variant. */
int ui_logo(Term *t, int kind, int y, float time, bool tall, int home, bool compact) {
    int nwords = tall ? 2 : 1;
    if (kind == LOGO_CLASSIC) nwords = compact ? 1 : 2;
    if (kind == LOGO_SECRET) {
        term_center(t, y, "┌─[ root@tanks ]─[ ~/secret ]─┐", (RGB){80, 230, 130}, NULL, 0, -1);
        y += 1;
    } else if (kind != LOGO_CLASSIC) {
        y += 1;
    }
    g_logo = (LogoReq){true, kind, y * CELL_H, home, nwords, time};
    int px = nwords * 6 * LH + (nwords - 1) * LH;
    y += (px + CELL_H - 1) / CELL_H;
    switch (kind) {
    case LOGO_CLASSIC:
        if (nwords == 1) { term_center(t, y, "▀▄▀  T  A  N  K  S  ▀▄▀", P_AMBER, NULL, 0, -1); y += 1; }
        return y;
    case LOGO_MASTER: {
        if (!tall) {
            const char *sub = "◆  T  A  N  K  S  ◆";
            ui_prism_text(t, (t->cols - text_len(sub)) / 2, y, sub, time, 0.1f, 0.05f, 0.6f, 1.0f, NULL);
            y += 1;
        }
        const char *ed = "M A S T E R   E D I T I O N";
        ui_prism_text(t, (t->cols - text_len(ed)) / 2, y + 1, ed, time, 0.1f, 0.06f, 0.45f, 1.0f, NULL);
        return y + 2;
    }
    case LOGO_SECRET: {
        if (!tall) { term_center(t, y, "▌▌ T A N K S ▐▐", (RGB){110, 255, 150}, NULL, 0, -1); y += 1; }
        const char *ed = "S E C R E T   E D I T I O N";
        int len = text_len(ed);
        char shown[64];
        int k = (int)time % 6 < 3 ? (int)(fmodf(time, 6) * 12) : len;
        if (k < 1) k = 1;
        if (k > len) k = len;
        memcpy(shown, ed, (size_t)k);
        shown[k] = 0;
        int x = (t->cols - len) / 2;
        term_text(t, x, y + 1, shown, (RGB){130, 255, 170}, NULL);
        if ((int)(time * 2) % 2 == 0) term_put(t, x + k, y + 1, 0x2588, (RGB){130, 255, 170}, NULL);
        return y + 2;
    }
    default: {
        const HomeTheme *th = &HOME_THEMES[home];
        if (!tall) { term_center(t, y, "▌▌ T A N K S ▐▐", th->logo_b, NULL, 0, -1); y += 1; }
        char ed[96];
        if (home == HOME_ETERNAL) {
            snprintf(ed, sizeof ed, "P R O T E C T O R   O F   T H E   U N I V E R S E");
        } else {
            int o = 0;
            for (const char *p = th->name; *p && o < 80; p++) {
                if (*p == ' ') { o += snprintf(ed + o, sizeof ed - (size_t)o, "   "); continue; }
                if (o && ed[o - 1] != ' ') ed[o++] = ' ';
                ed[o++] = *p;
            }
            ed[o] = 0;
            snprintf(ed + o, sizeof ed - (size_t)o, "   \xC2\xB7   E D I T I O N");
        }
        term_center(t, y + 1, ed, th->accent, NULL, 0, -1);
        return y + 2;
    }
    }
}

/* ------------------------------------------------------------------ on-screen keyboard */
static const char *const OSK_ROWS[4] = {"1234567890", "QWERTYUIOP", "ASDFGHJKL-", "ZXCVBNM_. "};
#define OSK_KW 4

static int osk_key(int r, int c) {
    if (r < 4) {
        char ch = OSK_ROWS[r][c];
        return ch == ' ' ? K_SPACE : ch;
    }
    return c < 4 ? K_SPACE : (c < 7 ? K_BKSP : K_ENTER);
}

int osk_handle(Osk *k, const IEv *e) {
    switch (e->action) {
    case A_UP: k->r = (k->r + 4) % 5; return 0;
    case A_DOWN: k->r = (k->r + 1) % 5; return 0;
    case A_LEFT: k->c = (k->c + 9) % 10; return 0;
    case A_RIGHT: k->c = (k->c + 1) % 10; return 0;
    case A_CONFIRM: case A_FIRE: return osk_key(k->r, k->c);
    case A_PAUSE: return K_ENTER;
    case A_OTHER: return e->raw == K_X ? K_BKSP : (e->raw == K_Y ? K_SPACE : 0);
    case A_TOUCH:
        if (e->raw >= HOT_OSK && e->raw < HOT_OSK + 50) {
            int i = e->raw - HOT_OSK;
            k->r = i / 10;
            k->c = i % 10;
            return osk_key(k->r, k->c);
        }
        return 0;
    default: return 0;
    }
}

void osk_draw(Term *t, const Osk *k, int y, float time) {
    (void)time;
    int x0 = (t->cols - 10 * OSK_KW) / 2;
    for (int r = 0; r < 5; r++)
        for (int c = 0; c < 10; c++) {
            bool sel = r == k->r && (r < 4 ? c == k->c : (k->c < 4 ? c < 4 : (k->c < 7 ? (c >= 4 && c < 7) : c >= 7)));
            RGB bg = sel ? P_PANEL_HI : P_PANEL;
            RGB fg = sel ? P_WHITE : P_TEXT;
            int x = x0 + c * OSK_KW, yy = y + r * 2;
            if (r < 4) {
                char lab[4] = {OSK_ROWS[r][c] == ' ' ? 0 : OSK_ROWS[r][c], 0};
                term_fill(t, x, yy, OSK_KW - 1, 1, bg);
                if (lab[0]) term_text(t, x + 1, yy, lab, fg, &bg);
                else term_text(t, x, yy, "SPC", fg, &bg);
                hot_add(x, yy, OSK_KW, 2, HOT_OSK + r * 10 + c);
            } else if (c == 0 || c == 4 || c == 7) {
                int w = c == 0 ? 4 * OSK_KW - 1 : 3 * OSK_KW - 1;
                const char *lab = c == 0 ? "SPACE" : (c == 4 ? "DEL" : "OK");
                term_fill(t, x, yy, w, 1, bg);
                term_center(t, yy, lab, c == 7 ? P_OK : fg, &bg, x, w);
                hot_add(x, yy, w + 1, 2, HOT_OSK + 40 + c);
            }
        }
}
