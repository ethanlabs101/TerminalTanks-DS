/* ShopScene (terminaltanks.py l.11407-11757) with theme_preview, ticket_art (l.5855) and martyr_art (l.12586).
 * DS layout: the item detail + live preview on the top screen, page tabs and the item grid on the touch screen
 * (five items per row as in the original; the cells are 9 columns wide instead of 10). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "scenes.h"
#include "ui.h"
#include "preview.h"
#include "sprites.h"

static const RGB GOLD = {255, 206, 100};
static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static const char *const SHOP_PAGES[5] = {"TANKS", "AMMO", "MAIN SCREEN", "MAPS", "EXTRAS"};
static const RGB TIER_COLORS[5] = {{170, 205, 175}, {120, 190, 255}, {255, 165, 90}, {255, 206, 100}, {255, 70, 140}};

#define CW 9
#define GRID_TOP (T_SPLIT + 3)
#define VIS 6
#define MAXROWS 32

typedef struct {
    Scene base;
    int page;
    int pos[5][2];
    int scroll[5];
    bool pending;
    char msg[80];
    bool msg_ok;
    float msg_t;
    PreviewStage stage;
    MapPreview mp;
    Canvas cv;
    int16_t grid[MAXROWS][5];
    int glen[MAXROWS], nrows;
} Shop;

static void uid_kind(int uid, char *kind, int nk, char *key, int nkey) {
    const char *u = UNLOCK_UIDS[uid];
    const char *c = strchr(u, ':');
    if (!c) { snprintf(kind, (size_t)nk, "%s", u); key[0] = 0; return; }
    snprintf(kind, (size_t)nk, "%.*s", (int)(c - u), u);
    snprintf(key, (size_t)nkey, "%s", c + 1);
}

static void build_grid(Shop *s) {
    s->nrows = 0;
    for (int tier = 0; tier < 5; tier++) {
        int n = 0;
        for (int i = 0; i < N_SHOP; i++) {
            if (SHOP[i].page != s->page || SHOP[i].tier != tier) continue;
            if (n == 0 && s->nrows < MAXROWS) s->glen[s->nrows++] = 0;
            int r = s->nrows - 1;
            s->grid[r][s->glen[r]++] = (int16_t)i;
            n++;
            if (s->glen[r] == 5) n = 0;
        }
    }
}

static const ShopItem *cur_item(Shop *s) {
    int r = s->pos[s->page][0], c = s->pos[s->page][1];
    if (r >= s->nrows) r = s->nrows - 1;
    if (c >= s->glen[r]) c = s->glen[r] - 1;
    return &SHOP[s->grid[r][c]];
}

static void fit(Shop *s) {
    int r = s->pos[s->page][0];
    int top = s->scroll[s->page];
    if (r < top) top = r;
    else if (r >= top + VIS) top = r - VIS + 1;
    int mx = s->nrows - VIS > 0 ? s->nrows - VIS : 0;
    s->scroll[s->page] = top < 0 ? 0 : (top > mx ? mx : top);
}

static void go_page(Shop *s, int p) {
    s->page = (p % 5 + 5) % 5;
    s->pending = false;
    build_grid(s);
}

static void say(Shop *s, const char *m, bool ok, float t) {
    snprintf(s->msg, sizeof s->msg, "%s", m);
    s->msg_ok = ok;
    s->msg_t = t;
}

static void do_martyr(Shop *s) {
    SaveData *sv = &g_app.save;
    const ShopItem *it = cur_item(s);
    char buf[64];
    if (sv->martyr_won) {
        say(s, "THE MARTYR RESTS. REPLAY THE DUEL? PRESS A AGAIN", true, 2.5f);
        if (s->pending) { s->pending = false; start_martyr(false, true); }
        else s->pending = true;
    } else if (!save_martyr_available(sv)) {
        say(s, "DEFEAT ALL 10 VAULT BOSSES FIRST", false, 2.5f);
    } else if (sv->martyr_paid) {
        start_martyr(false, false);
    } else if (save_shortfall(sv, it, buf, sizeof buf)) {
        s->pending = false;
        say(s, buf, false, 2.5f);
    } else if (!s->pending) {
        s->pending = true;
    } else {
        s->pending = false;
        g_app.wallet_flash = 1.4f;
        if (save_buy_martyr(sv, it)) { app_flush(); start_martyr(true, false); }
    }
}

static void do_ticket(Shop *s) {
    SaveData *sv = &g_app.save;
    const ShopItem *it = cur_item(s);
    char buf[64];
    if (!save_tickets_open(sv)) { s->pending = false; say(s, "SEALED - BEAT THE MASTER AND THE FINAL MISSION", false, 3.0f); }
    else if (save_tickets_sold_out(sv)) say(s, "SOLD OUT - EVERY BOSS IS DEFEATED", false, 2.5f);
    else if (sv->ticket_active >= 0) start_ticket(sv->ticket_active);
    else if (save_shortfall(sv, it, buf, sizeof buf)) { s->pending = false; say(s, buf, false, 2.5f); }
    else if (!s->pending) s->pending = true;
    else {
        s->pending = false;
        g_app.wallet_flash = 1.4f;
        int t = save_buy_ticket(sv, it, &g_app.rng);
        app_flush();
        start_ticket(t);
    }
}

#define HOT_SHOP 1200
static void shop_handle(Scene *sc, const IEv *e) {
    Shop *s = (Shop *)sc;
    int r = s->pos[s->page][0], c = s->pos[s->page][1];
    if (e->action == A_BACK) {
        if (s->pending) s->pending = false;
        else app_goto(scene_menu());
        return;
    }
    if (e->action == A_OTHER && (e->raw == K_R || e->raw == K_SELECT)) { go_page(s, s->page + 1); return; }
    if (e->action == A_OTHER && e->raw == K_L) { go_page(s, s->page - 1); return; }
    if (e->action == A_TOUCH && e->raw >= HOT_SHOP && e->raw < HOT_SHOP + 5) { go_page(s, e->raw - HOT_SHOP); return; }
    bool confirm = ev_confirm(e);
    if (e->action == A_TOUCH && e->raw >= HOT_SHOP + 100 && e->raw < HOT_SHOP + 100 + MAXROWS * 5) {
        int k = e->raw - HOT_SHOP - 100, rr = k / 5, cc = k % 5;
        if (rr == r && cc == c) confirm = true;
        else {
            s->pos[s->page][0] = rr;
            s->pos[s->page][1] = cc;
            s->pending = false;
            fit(s);
            return;
        }
    }
    if (e->action == A_TOUCH && e->raw == HOT_SHOP + 50) confirm = true;
    if (e->action == A_LEFT || e->action == A_RIGHT || e->action == A_UP || e->action == A_DOWN) {
        s->pending = false;
        if (e->action == A_LEFT) c = c > 0 ? c - 1 : 0;
        else if (e->action == A_RIGHT) c = c + 1 < s->glen[r] ? c + 1 : s->glen[r] - 1;
        else {
            int jump = e->coarse ? VIS : 1;
            r = e->action == A_UP ? (r - jump > 0 ? r - jump : 0) : (r + jump < s->nrows - 1 ? r + jump : s->nrows - 1);
        }
        s->pos[s->page][0] = r;
        s->pos[s->page][1] = c < s->glen[r] - 1 ? c : s->glen[r] - 1;
        fit(s);
        return;
    }
    if (!confirm) return;
    const ShopItem *it = cur_item(s);
    if (it->uid == UID_TICKET) { do_ticket(s); return; }
    if (it->uid == UID_TIMEMACHINE) { do_martyr(s); return; }
    SaveData *sv = &g_app.save;
    char buf[64];
    if (save_is_unlocked(sv, it->uid)) say(s, "YOU ALREADY OWN THIS", false, 2.0f);
    else if (save_shortfall(sv, it, buf, sizeof buf)) { s->pending = false; say(s, buf, false, 2.5f); }
    else if (!s->pending) s->pending = true;
    else {
        const char *why = save_buy(sv, it, buf, sizeof buf);
        s->pending = false;
        if (why) say(s, why, false, 2.5f);
        else {
            char m[80];
            snprintf(m, sizeof m, "PURCHASED: %s", it->name);
            say(s, m, true, 3.0f);
            g_app.wallet_flash = 1.4f;
            save_refresh(sv);
            app_flush();
        }
    }
}

static void shop_update(Scene *sc, float dt) {
    Shop *s = (Shop *)sc;
    backdrop_scene_update(sc, dt);
    preview_update(&s->stage, dt);
    s->msg_t = fmaxf(0, s->msg_t - dt);
}

/* ------------------------------------------------------------------ previews */
static void theme_preview(Surf *cv, const HomeTheme *th, float t, RGB hull) {
    int w = 34, h = 20;
    float st[4];
    RGB sc[4];
    for (int i = 0; i < th->nsky; i++) { st[i] = (float)th->sky[i].t; sc[i] = th->sky[i].c; }
    for (int py = 0; py < h; py++) {
        Px c = rgb15(cgradient(st, sc, th->nsky, clampf(1 - py / (float)(h - 1), 0, 1)));
        for (int x = 0; x < w; x++) cv->px[py * w + x] = c;
    }
    if (th->stars)
        for (int k = 0; k < 26; k++) cv_blendf(cv, (float)((k * 37) % w), (float)(h - 1 - (k * 11) % (int)(h * 0.55f)), P_WHITE, 0.35f + 0.45f * fsin(t * 2 + k));
    if (th->corona)
        for (int dy = -8; dy < 9; dy++)
            for (int dx = -8; dx < 9; dx++) {
                float d = fhypot((float)dx, (float)dy);
                if (d <= 4.5f) cv_plot(cv, 24 + dx, 13 + dy, rgb15i(2, 1, 8));
                else if (d <= 7) {
                    float hh = fmodf(atan2f((float)dy, (float)dx) / 6.2831853f + t * 0.1f, 1.0f);
                    if (hh < 0) hh += 1;
                    cv_blendf(cv, 24.0f + dx, 13.0f + dy, chsv(hh, 0.55f, 1.0f), 0.8f * (1 - (d - 4.5f) / 2.5f));
                }
            }
    const MoteStyle *ms = &MOTE_STYLES[th->fx];
    if (ms->on)
        for (int k = 0; k < 14; k++) {
            float mx = fmodf(k * 53 + fsin(t + k) * 2, (float)w);
            float my = fmodf(k * 29 + t * fabsf((float)ms->speed) * 0.6f * (ms->speed < 0 ? 1 : -1), (float)h);
            if (my < 0) my += h;
            cv_blendf(cv, mx, my, ms->pal[k % 3], 0.85f);
        }
    for (int x = 0; x < w; x++) {
        int rh = (int)(3 + 2 * fsin(x * 0.3f) + fsin(x * 0.7f));
        for (int y = 0; y < rh + 3; y++) cv_plot(cv, x, y, rgb15(th->far));
        for (int y = 0; y < 3; y++) cv_plot(cv, x, y, rgb15(y < 2 ? th->ground : th->ground_top));
    }
    draw_tank(cv, 8.0f, 3, 1, 40, hull, 1, 0, false, 0, &DESIGNS[th->hero], t, -1, -1);
}

static void ticket_art(Surf *cv, float t) {
    for (int py = 0; py < 20; py++) {
        Px c = rgb15(cmix((RGB){10, 6, 28}, (RGB){60, 20, 80}, py / 19.0f));
        for (int x = 0; x < 34; x++) cv->px[py * 34 + x] = c;
    }
    int b = g_app.save.ticket_active;
    if (b >= 0 && b < N_TICKETS) {
        for (int x = 0; x < 34; x++) for (int y = 0; y < 3; y++) cv_plot(cv, x, y, rgb15i(40, 36, 60));
        draw_tank(cv, 17.0f, 3, -1, 130, DESIGNS[TICKETS[b].tank].accent, 2, 0, false, 0, &DESIGNS[TICKETS[b].tank], t, -1, -1);
        return;
    }
    for (int x = 5; x < 29; x++)
        for (int y = 5; y < 15; y++)
            if (!((x == 5 || x == 28) && (y == 9 || y == 10))) cv_plot(cv, x, y, rgb15(cmix((RGB){230, 190, 90}, (RGB){255, 240, 170}, 0.5f + 0.5f * fsin(t * 2 + x * 0.3f))));
    for (int y = 6; y < 14; y++) cv_plot(cv, 8, y, rgb15i(150, 110, 40));
    static const int Q[8][2] = {{15, 11}, {16, 12}, {17, 12}, {18, 11}, {18, 10}, {17, 9}, {17, 8}, {17, 6}};
    for (int i = 0; i < 8; i++) cv_plot(cv, Q[i][0] + 4, Q[i][1], rgb15i(90, 40, 120));
    cv_plot(cv, 21, 6, rgb15i(90, 40, 120));
}

static void martyr_art(Surf *cv, float t) {
    for (int py = 0; py < 20; py++) {
        Px c = rgb15(cmix((RGB){6, 2, 20}, (RGB){70, 30, 110}, py / 19.0f));
        for (int x = 0; x < 34; x++) cv->px[py * 34 + x] = c;
    }
    PyRandom r;
    rng_seed(&r, 7);
    for (int k = 0; k < 16; k++) {
        float x = (float)rng_uniform(&r, 0, 33), y = (float)rng_uniform(&r, 4, 19), ph = (float)rng_uniform(&r, 0, 6);
        cv_blendf(cv, x, y, P_WHITE, 0.4f + 0.4f * fsin(t * 3 + ph));
    }
    draw_artifact(cv, 17, 10, 0.95f, t);
    if (save_martyr_available(&g_app.save) && !g_app.save.martyr_won) draw_mage(cv, 17, 2, 0.5f, t, 0.35f + 0.25f * fsin(t * 2), false);
}

static Surf *item_preview(Shop *s, const ShopItem *it) {
    float t = (float)g_app.time;
    Surf *cv = &s->cv.s;
    if (it->uid == UID_TICKET) { ticket_art(cv, t); return cv; }
    if (it->uid == UID_TIMEMACHINE) { martyr_art(cv, t); return cv; }
    char kind[24], key[48];
    uid_kind(it->uid, kind, sizeof kind, key, sizeof key);
    int design = DESIGN_RANGER, ammo = AMMO_STANDARD;
    RGB hull = COLOR_CHOICES[color_index_by_name("CYAN")].c, shot = COLOR_CHOICES[color_index_by_name("YELLOW")].c;
    if (!strcmp(kind, "map")) {
        int designs[2] = {DESIGN_RANGER, DESIGN_RANGER};
        RGB cols[2] = {{230, 190, 90}, {90, 190, 230}};
        return mappreview_render(&s->mp, map_by_key(key), 34, 20, t, designs, cols);
    }
    if (!strcmp(kind, "theme")) {
        for (int h = 1; h < HOME_MASTER; h++)
            if (!strcasecmp(HOME_ORDER[h], key)) { theme_preview(cv, &HOME_THEMES[h], t, hull); return cv; }
    }
    if (!strcmp(kind, "tank")) design = design_by_key(key);
    else if (!strcmp(kind, "ammo")) ammo = ammo_by_key(key);
    else if (!strcmp(kind, "paint")) hull = shot = !strcmp(key, "astral") ? COL_ASTRAL : !strcmp(key, "prismatic") ? COL_PRISMATIC : COL_VOID;
    else if (!strcmp(key, "hull")) hull = COL_RAINBOW;
    else if (!strcmp(key, "shell")) shot = COL_RAINBOW;
    preview_set_kit(&s->stage, design < 0 ? 0 : design, hull, ammo < 0 ? 0 : ammo, shot, 0);
    return preview_render(&s->stage);
}

static void price_str(const ShopItem *it, char *out, int n) {
    if (it->credits) snprintf(out, (size_t)n, "%d\xE2\x97\x89 %d\xE2\x97\x88", (int)it->coins, (int)it->credits);
    else snprintf(out, (size_t)n, "%d\xE2\x97\x89", (int)it->coins);
}

static void shop_draw(Scene *sc, Term *t) {
    Shop *s = (Shop *)sc;
    backdrop_scene_draw(sc, t);
    SaveData *sv = &g_app.save;
    float tm = (float)g_app.time;
    char buf[96], cbuf[48];
    const ShopItem *it = cur_item(s);
    /* ---- top screen: the ITEM panel */
    RGB col = TIER_COLORS[it->tier];
    bool owned = save_is_unlocked(sv, it->uid);
    int px = 8, PW = 36;
    term_box(t, px, 0, PW, 23, TB_DOUBLE, it->tier >= 4 ? rainbow(tm) : col, P_PANEL, "ITEM", P_PRIMARY);
    term_blit(t, item_preview(s, it), px + 1, 1, 0, 0);
    term_center(t, 11, it->name, P_WHITE, &P_PANEL, px, PW);
    char kind[24], key[48];
    uid_kind(it->uid, kind, sizeof kind, key, sizeof key);
    const char *kname = !strcmp(kind, "tank") ? "TANK" : !strcmp(kind, "ammo") ? "AMMO" : !strcmp(kind, "map") ? "MAP" : !strcmp(kind, "theme") ? "MAIN SCREEN"
                      : !strcmp(kind, "ticket") ? "TICKET" : !strcmp(kind, "timemachine") ? "ARTIFACT" : "EXTRA";
    snprintf(buf, sizeof buf, "%s%s", kname, it->tier >= 4 ? "  \xC2\xB7  PREMIUM" : "");
    term_center(t, 12, buf, col, &P_PANEL, px, PW);
    term_wrap_center(t, 13, it->blurb, 32, P_TEXT, &P_PANEL, px, PW, 4);
    char a[24], b[24];
    if (it->uid == UID_TICKET) {
        if (owned) snprintf(buf, sizeof buf, "SOLD OUT");
        else if (!save_tickets_open(sv)) snprintf(buf, sizeof buf, "SEALED: BEAT THE MASTER + FINAL MISSION");
        else if (sv->ticket_active >= 0) snprintf(buf, sizeof buf, "ACTIVE BOSS: %s", TICKETS[sv->ticket_active].name);
        else snprintf(buf, sizeof buf, "PRICE  %d \xE2\x97\x89  %d \xE2\x97\x88", (int)it->coins, (int)it->credits);
        term_center(t, 17, buf, owned ? P_OK : GOLD, &P_PANEL, px, PW);
    } else if (it->uid == UID_TIMEMACHINE) {
        fmt_commas(it->coins, a, sizeof a);
        if (owned) snprintf(buf, sizeof buf, "DUEL COMPLETE");
        else if (!save_martyr_available(sv)) snprintf(buf, sizeof buf, "DEFEAT ALL 10 VAULT BOSSES TO UNLOCK");
        else if (sv->martyr_paid) snprintf(buf, sizeof buf, "PHASE %d/3 - RESUME", sv->martyr_phase + 1);
        else snprintf(buf, sizeof buf, "PRICE  %s \xE2\x97\x89  %d \xE2\x97\x88", a, (int)it->credits);
        term_center(t, 17, buf, owned ? P_OK : save_martyr_available(sv) ? GOLD : P_MUTED, &P_PANEL, px, PW);
    } else {
        if (owned) snprintf(buf, sizeof buf, "OWNED");
        else if (it->credits) snprintf(buf, sizeof buf, "PRICE  %d \xE2\x97\x89  %d \xE2\x97\x88", (int)it->coins, (int)it->credits);
        else snprintf(buf, sizeof buf, "PRICE  %d \xE2\x97\x89", (int)it->coins);
        term_center(t, 17, buf, owned ? P_OK : GOLD, &P_PANEL, px, PW);
    }
    fmt_commas(sv->coins, a, sizeof a);
    fmt_commas(sv->credits, b, sizeof b);
    snprintf(buf, sizeof buf, "YOU HAVE  %s \xE2\x97\x89  %s \xE2\x97\x88", a, b);
    term_center(t, 18, buf, P_MUTED, &P_PANEL, px, PW);
    bool blink = (int)(tm * 3) % 2 == 0;
    const char *line = NULL;
    RGB lc = P_PRIMARY;
    if (s->msg_t > 0) { line = s->msg; lc = s->msg_ok ? P_OK : P_DANGER; }
    else if (it->uid == UID_TIMEMACHINE) {
        if (s->pending) { line = blink ? "A AGAIN TO CONTINUE \xC2\xB7 B CANCEL" : NULL; lc = P_AMBER; }
        else line = owned ? "[A] REPLAY" : sv->martyr_paid ? "[A] RESUME" : "[A] BUY";
    } else if (it->uid == UID_TICKET && !owned && !save_tickets_open(sv)) { line = "[A] WHY IS IT SEALED?"; lc = P_MUTED; }
    else if (it->uid == UID_TICKET && !owned) {
        int n = 0;
        for (int i = 0; i < N_TICKETS; i++) n += (sv->tickets_won >> i) & 1;
        if (s->pending) { line = blink ? "A AGAIN TO BUY \xC2\xB7 B CANCEL" : NULL; lc = P_AMBER; }
        else { snprintf(cbuf, sizeof cbuf, "[A] %s   %d/%d BEATEN", sv->ticket_active >= 0 ? "RESUME FIGHT" : "BUY", n, N_TICKETS); line = cbuf; }
    } else if (owned) {
        lc = P_MUTED;
        line = !strcmp(kind, "map") ? "PICK IT IN BATTLEFIELD" : !strcmp(kind, "theme") ? "SET IT IN SETTINGS OR PRESS SELECT"
               : !strcmp(kind, "paint") ? "PICK IT IN PILOT SETUP OR THE WORKSHOP" : !strcmp(kind, "ticket") ? "ALL BOSSES DEFEATED" : "PICK IT IN PILOT SETUP";
    } else if (s->pending) { line = blink ? "A AGAIN TO BUY \xC2\xB7 B CANCEL" : NULL; lc = P_AMBER; }
    else {
        const char *why = save_shortfall(sv, it, cbuf, sizeof cbuf);
        line = why ? why : "[A] BUY";
        lc = why ? (RGB){200, 90, 90} : P_PRIMARY;
    }
    if (line) term_wrap_center(t, 19, line, PW - 2, lc, &P_PANEL, px, PW, 2);
    /* ---- touch screen: tabs + grid */
    int y0 = T_SPLIT;
    term_fill(t, 0, y0, T_COLS, T_SPLIT, P_BG);
    int tx = 0;
    for (int i = 0; i < 5; i++) {
        static const char *const SHORT[5] = {"TANKS", "AMMO", "SCREENS", "MAPS", "EXTRAS"};
        snprintf(buf, sizeof buf, " %s ", SHORT[i]);
        bool sel = i == s->page;
        term_text(t, tx, y0, buf, sel ? P_INK : P_MUTED, sel ? &GOLD : &P_PANEL);
        hot_add(tx, y0, text_len(buf), 1, HOT_SHOP + i);
        tx += text_len(buf) + 1;
    }
    fit(s);
    int top = s->scroll[s->page];
    int sel_r = s->pos[s->page][0] < s->nrows ? s->pos[s->page][0] : s->nrows - 1, sel_c = s->pos[s->page][1];
    snprintf(buf, sizeof buf, "%s%s", SHOP_PAGES[s->page], "");
    if (s->nrows > VIS) snprintf(buf, sizeof buf, "%s \xC2\xB7 ROW %d/%d", SHOP_PAGES[s->page], sel_r + 1, s->nrows);
    term_box(t, 0, y0 + 1, T_COLS, 21, TB_DOUBLE, P_LINE, P_PANEL, buf, P_PRIMARY);
    for (int k = 0; k < VIS; k++) {
        int r = top + k;
        if (r >= s->nrows) break;
        int gy = GRID_TOP + k * 3;
        RGB tc = TIER_COLORS[SHOP[s->grid[r][0]].tier];
        for (int dy = 0; dy < 2; dy++)
            term_put(t, 1, gy + dy, 0x258C, SHOP[s->grid[r][0]].tier >= 4 ? rainbow(tm + r * 0.1f + dy * 0.05f) : tc, &P_PANEL);
        for (int c = 0; c < s->glen[r]; c++) {
            const ShopItem *si = &SHOP[s->grid[r][c]];
            int x = 2 + c * (CW + 1);
            bool sel = r == sel_r && c == sel_c;
            bool own = save_is_unlocked(sv, si->uid);
            RGB bg = sel ? P_PANEL_HI : P_PANEL;
            term_fill(t, x, gy, CW, 2, bg);
            char nm[24];
            if (text_len(si->name) <= CW) snprintf(nm, sizeof nm, "%s", si->name);
            else snprintf(nm, sizeof nm, "%.*s\xE2\x80\xA6", CW - 1, si->name);
            term_text(t, x, gy, nm, sel ? P_WHITE : (own ? P_OK : tc), &bg);
            char price[32];
            RGB pc;
            if (si->uid == UID_TICKET) {
                if (own) { snprintf(price, sizeof price, "SOLD OUT"); pc = P_MUTED; }
                else if (!save_tickets_open(sv)) { snprintf(price, sizeof price, "SEALED"); pc = P_MUTED; }
                else { if (sv->ticket_active >= 0) snprintf(price, sizeof price, "RESUME"); else price_str(si, price, sizeof price); pc = P_AMBER; }
            } else if (si->uid == UID_TIMEMACHINE) {
                if (own) { snprintf(price, sizeof price, "\xE2\x9C\x93 BEATEN"); pc = P_OK; }
                else if (!save_martyr_available(sv)) { snprintf(price, sizeof price, "LOCKED"); pc = P_MUTED; }
                else { if (sv->martyr_paid) snprintf(price, sizeof price, "RESUME"); else price_str(si, price, sizeof price); pc = P_AMBER; }
            } else if (own) { snprintf(price, sizeof price, "\xE2\x9C\x93 OWNED"); pc = P_OK; }
            else {
                price_str(si, price, sizeof price);
                pc = save_shortfall(sv, si, cbuf, sizeof cbuf) ? (RGB){200, 90, 90} : P_AMBER;
            }
            term_text(t, x, gy + 1, price, pc, &bg);
            hot_add(x, gy, CW, 2, HOT_SHOP + 100 + r * 5 + c);
        }
    }
    if (s->nrows > VIS) {
        int track = 19, thumb = track * VIS / s->nrows;
        if (thumb < 1) thumb = 1;
        int pos = (track - thumb) * top / (s->nrows - VIS > 1 ? s->nrows - VIS : 1);
        for (int k = 0; k < track; k++) {
            bool on = k >= pos && k < pos + thumb;
            term_put(t, T_COLS - 1, y0 + 2 + k, on ? 0x2588 : 0x2502, on ? GOLD : P_LINE, &P_PANEL);
        }
    }
    ui_button(t, 15, T_ROWS - 2, s->pending ? "A CONFIRM" : "A BUY / OPEN", s->pending ? P_AMBER : P_PRIMARY, P_PANEL_HI, HOT_SHOP + 50);
    term_text(t, 0, T_ROWS - 1, "L/R PAGE \xC2\xB7 \xE2\x96\xB2\xE2\x96\xBC\xE2\x97\x84\xE2\x96\xBA SELECT \xC2\xB7 B BACK", P_MUTED, NULL);
}

static void shop_destroy(Scene *sc) {
    Shop *s = (Shop *)sc;
    preview_free(&s->stage);
    mappreview_free(&s->mp);
    canvas_free(&s->cv);
    free(s);
}
static const SceneVT SHOP_VT = {.handle = shop_handle, .update = shop_update, .draw = shop_draw, .destroy = shop_destroy};
Scene *scene_shop(int page) {
    Shop *s = (Shop *)calloc(1, sizeof(Shop));
    scene_base(&s->base, &SHOP_VT);
    preview_init(&s->stage, 34, 20, 1, 3);
    mappreview_init(&s->mp);
    canvas_alloc(&s->cv, 34, 20);
    go_page(s, page);
    return &s->base;
}
