/* Tank Workshop and Tank Studio (terminaltanks.py l.12174-12475). DS layout: preview + the saved-tank list on the top
 * screen, the editing rows on the touch screen; NAME and LORE are typed on the on-screen keyboard. */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "scenes.h"
#include "ui.h"
#include "preview.h"

static const RGB GOLD = {255, 206, 100};
#define PREVIEW_SLOT (CUSTOM_TANK_MAX + CUSTOM_REMOTE_SLOTS - 1)
#define PREVIEW_DESIGN (DESIGN_CUSTOM_FIRST + PREVIEW_SLOT)

enum { W_LOAD, W_TURRET, W_HULL, W_TRACK, W_BARREL, W_GLOW, W_AMBIENT, W_ACCENT, W_COLOR, W_HOVER, W_NAME, W_LORE, W_BUILD, W_EQUIP, W_DELETE, W_N };
static const char *const W_LABEL[W_N] = {"TANK", "TURRET", "HULL", "TRACKS", "BARREL", "GLOW", "AMBIENT", "ACCENT", "COLOR", "HOVER", "NAME", "LORE",
                                         "BUILD", "EQUIP", "DELETE"};
static const int SLOT_OF[W_N] = {-1, SLOT_TURRET, SLOT_HULL, SLOT_TRACK, SLOT_BARREL, SLOT_GLOW, SLOT_AMBIENT, SLOT_ACCENT, -1, -1, -1, -1, -1, -1, -1};

typedef struct {
    Scene base;
    int row, load, pcolor;
    bool confirm_delete;
    char msg[96];
    bool msg_ok;
    float msg_t;
    CustomTank draft;
    int amb;                 /* the single ambient kind chosen (index) */
    WorkshopParts wp;
    PreviewStage stage;
    int editing;             /* 0 none, W_NAME or W_LORE */
    Osk osk;
} Workshop;

static int nsaved(void) { return custom_count(&g_app.save); }

static void draft_new(Workshop *s) {
    memset(&s->draft, 0, sizeof s->draft);
    snprintf(s->draft.name, sizeof s->draft.name, "MY TANK");
    snprintf(s->draft.lore, sizeof s->draft.lore, "Built in the workshop.");
    s->draft.accent = (uint8_t)color_index_by_name("WHITE");
    s->amb = AMB_NONE;
    s->draft.ambient = 1ull << AMB_NONE;
}
static void sync_draft(Workshop *s) {
    if (s->load > 0 && s->load <= nsaved()) {
        s->draft = g_app.save.custom[s->load - 1];
        s->amb = 0;
        for (int k = 0; k < AMB_COUNT; k++) if (s->draft.ambient & (1ull << k)) { s->amb = k; break; }
    }
}
static int slot_value(const Workshop *s, int slot) {
    switch (slot) {
    case SLOT_TURRET: return s->draft.turret;
    case SLOT_HULL: return s->draft.hull;
    case SLOT_TRACK: return s->draft.track;
    case SLOT_BARREL: return s->draft.barrel;
    case SLOT_GLOW: return s->draft.glow;
    case SLOT_AMBIENT: return s->amb;
    default: return s->draft.accent;
    }
}
static void slot_set(Workshop *s, int slot, int v) {
    switch (slot) {
    case SLOT_TURRET: s->draft.turret = (uint8_t)v; break;
    case SLOT_HULL: s->draft.hull = (uint8_t)v; break;
    case SLOT_TRACK: s->draft.track = (uint8_t)v; break;
    case SLOT_BARREL: s->draft.barrel = (uint8_t)v; break;
    case SLOT_GLOW: s->draft.glow = (uint8_t)v; break;
    case SLOT_AMBIENT: s->amb = v; s->draft.ambient = 1ull << v; break;
    default: s->draft.accent = (uint8_t)v; break;
    }
}
static const char *slot_name(int slot, int v) {
    switch (slot) {
    case SLOT_TURRET: return TURRET_NAMES[v];
    case SLOT_HULL: return HULL_NAMES[v];
    case SLOT_TRACK: return TRACK_NAMES[v];
    case SLOT_BARREL: return BARREL_NAMES[v];
    case SLOT_GLOW: return GLOW_NAMES[v];
    case SLOT_AMBIENT: return AMBIENT_NAMES[v];
    default: return COLOR_CHOICES[v].name;
    }
}
static void cycle(Workshop *s, int slot, int d) {
    const WorkshopParts *wp = &s->wp;
    int n = wp->count[slot], cur = slot_value(s, slot), i = -1;
    for (int k = 0; k < n; k++) if (wp->have[slot][k] == cur) i = k;
    if (i < 0) { slot_set(s, slot, wp->have[slot][0]); return; }
    slot_set(s, slot, wp->have[slot][((i + d) % n + n) % n]);
}
static void say(Workshop *s, const char *m, bool ok, float t) { snprintf(s->msg, sizeof s->msg, "%s", m); s->msg_ok = ok; s->msg_t = t; }

static Scene *back_to_workshop(void) { return scene_workshop(); }
Scene *scene_studio_for(int design, int paint, NextFn back);

#define HOT_WS 1400
static void ws_handle(Scene *sc, const IEv *e) {
    Workshop *s = (Workshop *)sc;
    SaveData *sv = &g_app.save;
    if (s->editing) {
        char *buf = s->editing == W_NAME ? s->draft.name : s->draft.lore;
        int cap = s->editing == W_NAME ? 14 : 60, len = (int)strlen(buf);
        if (e->action == A_BACK) { s->editing = 0; return; }
        int k = osk_handle(&s->osk, e);
        if (k == K_ENTER) s->editing = 0;
        else if (k == K_BKSP) { if (len) buf[len - 1] = 0; }
        else if (k == K_SPACE) { if (len < cap) { buf[len] = ' '; buf[len + 1] = 0; } }
        else if (k > 0 && k < 128 && len < cap && (isalnum(k) || strchr("-.,!'", k))) {
            buf[len] = (char)(s->editing == W_NAME ? toupper(k) : tolower(k));
            if (s->editing == W_LORE && len == 0) buf[len] = (char)toupper(k);
            buf[len + 1] = 0;
        }
        return;
    }
    if (e->action == A_OTHER && e->raw == K_Y) {      /* the original's G key: TANK STUDIO */
        custom_register(PREVIEW_SLOT, &s->draft);
        app_goto(scene_studio_for(PREVIEW_DESIGN, s->pcolor, back_to_workshop));
        return;
    }
    if (e->action == A_BACK) {
        if (s->confirm_delete) s->confirm_delete = false;
        else app_goto(scene_menu());
        return;
    }
    int key = s->row, d = 0;
    bool confirm = ev_confirm(e);
    if (e->action == A_UP) { s->row = (s->row + W_N - 1) % W_N; s->confirm_delete = false; return; }
    if (e->action == A_DOWN) { s->row = (s->row + 1) % W_N; s->confirm_delete = false; return; }
    if (e->action == A_LEFT) d = -1;
    if (e->action == A_RIGHT) d = 1;
    if (e->action == A_TOUCH && e->raw >= HOT_WS && e->raw < HOT_WS + W_N) {
        int r = e->raw - HOT_WS;
        if (r != s->row) { s->row = r; s->confirm_delete = false; return; }
        key = r;
        if (r == W_NAME || r == W_LORE || r >= W_BUILD || r == W_HOVER) confirm = true;
        else d = 1;
    }
    if (e->action == A_TOUCH && (e->raw == HOT_WS + 90 || e->raw == HOT_WS + 91)) d = e->raw == HOT_WS + 90 ? -1 : 1;
    if (e->action == A_TOUCH && e->raw == HOT_WS + 92) { IEv y = {A_OTHER, false, K_Y, 0, 0}; ws_handle(sc, &y); return; }
    if (d) {
        if (key == W_LOAD) { s->load = ((s->load + d) % (nsaved() + 1) + nsaved() + 1) % (nsaved() + 1); if (s->load) sync_draft(s); else draft_new(s); }
        else if (key == W_COLOR) s->pcolor = ((s->pcolor + d * (e->coarse ? 4 : 1)) % N_PAINTS + N_PAINTS) % N_PAINTS;
        else if (SLOT_OF[key] >= 0) cycle(s, SLOT_OF[key], d * (e->coarse ? 5 : 1));
        else if (key == W_HOVER) s->draft.hover = !s->draft.hover;
        return;
    }
    if (!confirm) return;
    char buf[96];
    if (key == W_NAME || key == W_LORE) { s->editing = key; return; }
    if (key == W_HOVER) s->draft.hover = !s->draft.hover;
    else if (key == W_BUILD) {
        bool hv = s->draft.hover;
        CustomTank d2 = s->draft;
        char *nm = d2.name;
        while (*nm == ' ') nm++;
        if (!*nm) snprintf(d2.name, sizeof d2.name, "CUSTOM");
        const char *why = save_create_custom(sv, &d2, &g_app.rng, buf, sizeof buf);
        if (why) say(s, why, false, 3.0f);
        else {
            char a[24], m[96];
            fmt_commas(CUSTOM_TANK_FEE + (hv ? HOVER_COINS : 0), a, sizeof a);
            snprintf(m, sizeof m, "BUILT: %s  (-%s COINS%s)", d2.name, a, hv ? ", -50 CREDITS" : "");
            say(s, m, true, 3.0f);
            s->load = nsaved();
            g_app.wallet_flash = 1.4f;
            app_flush();
        }
    } else if (key == W_EQUIP) {
        if (s->load > 0) {
            sv->loadouts[0].tank = (int16_t)(DESIGN_CUSTOM_FIRST + s->load - 1);
            if (save_paint_ok(sv, s->pcolor, true)) sv->loadouts[0].tank_color = (uint8_t)s->pcolor;
            app_flush();
            say(s, "EQUIPPED AS YOUR PLAYER 1 TANK", true, 2.5f);
        } else say(s, "BUILD IT FIRST - THEN EQUIP", false, 2.5f);
    } else if (key == W_DELETE) {
        if (s->load > 0) {
            if (!s->confirm_delete) { s->confirm_delete = true; say(s, "A AGAIN TO DELETE FOR GOOD", false, 3.0f); }
            else {
                save_delete_custom(sv, s->load - 1);
                custom_register_all(sv);
                s->load = 0;
                s->confirm_delete = false;
                draft_new(s);
                say(s, "TANK DELETED", true, 2.0f);
                app_flush();
            }
        } else say(s, "NOTHING SAVED TO DELETE", false, 2.0f);
    }
}

static void ws_update(Scene *sc, float dt) {
    Workshop *s = (Workshop *)sc;
    backdrop_scene_update(sc, dt);
    preview_update(&s->stage, dt);
    s->msg_t = fmaxf(0, s->msg_t - dt);
}

static void ws_draw(Scene *sc, Term *t) {
    Workshop *s = (Workshop *)sc;
    backdrop_scene_draw(sc, t);
    SaveData *sv = &g_app.save;
    float tm = (float)g_app.time;
    char buf[128], a[24];
    /* ---- top: preview */
    int px = 1, PW = 49;
    term_box(t, px, 0, PW, 24, TB_DOUBLE, P_LINE, P_PANEL, "PREVIEW \xC2\xB7 [Y] STUDIO", P_PRIMARY);
    custom_register(PREVIEW_SLOT, &s->draft);
    const TankDesign *d = design_ptr(PREVIEW_DESIGN);
    Loadout *lo = save_loadout(sv, 0);
    preview_set_kit(&s->stage, PREVIEW_DESIGN, paint_rgb(s->pcolor), lo->ammo, paint_rgb(lo->shot_color), 0);
    term_blit(t, preview_render(&s->stage), px + 1, 1, 0, 0);
    term_center(t, 14, d->name, P_WHITE, &P_PANEL, px, PW);
    term_wrap_center(t, 15, d->blurb, 44, P_TEXT, &P_PANEL, px, PW, 1);
    snprintf(buf, sizeof buf, "%s \xC2\xB7 %s \xC2\xB7 %s", BARREL_NAMES[d->barrel], GLOW_NAMES[d->glow], AMBIENT_NAMES[s->amb]);
    for (char *p = buf; *p; p++) *p = (char)toupper((unsigned char)*p);
    term_center(t, 16, buf, P_MUTED, &P_PANEL, px, PW);
    fmt_commas(sv->coins, a, sizeof a);
    snprintf(buf, sizeof buf, "YOU HAVE %s \xE2\x97\x89  %d CR", a, (int)sv->credits);
    term_center(t, 17, buf, sv->coins >= CUSTOM_TANK_FEE ? GOLD : P_DANGER, &P_PANEL, px, PW);
    int ns = nsaved();
    snprintf(buf, sizeof buf, "MY TANKS  %d/%d", ns, CUSTOM_TANK_MAX);
    term_text(t, px + 2, 18, buf, P_AMBER, &P_PANEL);
    int total = ns + 1, vis = 4;
    int top = s->load - vis / 2;
    if (top > total - vis) top = total - vis;
    if (top < 0) top = 0;
    for (int r = 0; r < vis && top + r < total; r++) {
        int i = top + r;
        bool cur = i == s->load;
        RGB bg = cur ? P_PANEL_HI : P_PANEL;
        term_fill(t, px + 2, 19 + r, PW - 4, 1, bg);
        if (i == 0) snprintf(buf, sizeof buf, "%s+ NEW DESIGN", cur ? "\xE2\x96\xB6 " : "  ");
        else snprintf(buf, sizeof buf, "%s%02d %.20s%s", cur ? "\xE2\x96\xB6 " : "  ", i, sv->custom[i - 1].name,
                      sv->loadouts[0].tank == DESIGN_CUSTOM_FIRST + i - 1 ? "  \xE2\x98\x85 EQUIPPED" : "");
        term_text(t, px + 3, 19 + r, buf, cur ? P_WHITE : P_TEXT, &bg);
    }
    /* ---- bottom: rows or the keyboard */
    int x = 0, y = T_SPLIT, W = 51;
    if (s->editing) {
        term_fill(t, 0, y, T_COLS, T_SPLIT, P_BG);
        term_text(t, 1, y + 1, s->editing == W_NAME ? "NAME (14)" : "LORE (60)", P_AMBER, NULL);
        char *v = s->editing == W_NAME ? s->draft.name : s->draft.lore;
        term_fill(t, 1, y + 2, 49, 2, P_PANEL_HI);
        char lines[2][64];
        snprintf(buf, sizeof buf, "%s%s", v, (int)(tm * 2) % 2 == 0 ? "\xE2\x96\x88" : " ");
        int nl = wrap_text(buf, 47, lines, 2);
        for (int k = 0; k < nl; k++) term_text(t, 2, y + 2 + k, lines[k], P_WHITE, &P_PANEL_HI);
        osk_draw(t, &s->osk, y + 6, tm);
        static const Keycap K[4] = {{"A", "TYPE"}, {"X", "DELETE"}, {"START", "DONE"}, {"B", "CANCEL"}};
        ui_keycaps_center(t, T_ROWS - 2, K, 4, NULL, 0, T_COLS);
        return;
    }
    term_box(t, x, y, W, T_SPLIT, TB_DOUBLE, P_LINE, P_PANEL, "TANK WORKSHOP", P_PRIMARY);
    for (int i = 0; i < W_N; i++) {
        int ry = y + 1 + i;
        bool sel = i == s->row;
        RGB bg = sel ? P_PANEL_HI : P_PANEL;
        term_fill(t, x + 1, ry, W - 2, 1, bg);
        term_text(t, x + 2, ry, W_LABEL[i], sel ? P_WHITE : P_MUTED, &bg);
        hot_add(x + 1, ry, W - 2, 1, HOT_WS + i);
        char val[64];
        bool arrows = sel && (i == W_LOAD || i == W_HOVER || i == W_COLOR || SLOT_OF[i] >= 0);
        if (i == W_LOAD) {
            if (s->load == 0) snprintf(val, sizeof val, "NEW DESIGN");
            else snprintf(val, sizeof val, "%d/%d  %.14s", s->load, ns, sv->custom[s->load - 1].name);
        } else if (SLOT_OF[i] >= 0) {
            snprintf(val, sizeof val, "%s", slot_name(SLOT_OF[i], slot_value(s, SLOT_OF[i])));
            for (char *p = val; *p; p++) *p = *p == '_' ? ' ' : (char)toupper((unsigned char)*p);
        } else if (i == W_HOVER) snprintf(val, sizeof val, "%s", s->draft.hover ? "ON" : "OFF");
        else if (i == W_COLOR) {
            snprintf(val, sizeof val, "%s%s", paint_name(s->pcolor), save_paint_ok(sv, s->pcolor, true) ? "" : " (LOCKED)");
            term_text(t, x + 10, ry, "\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88", live_color(paint_rgb(s->pcolor), tm), &bg);
        } else if (i == W_NAME) snprintf(val, sizeof val, "%s", s->draft.name);
        else if (i == W_LORE) {
            int l = (int)strlen(s->draft.lore);
            snprintf(val, sizeof val, "%s", s->draft.lore + (l > 26 ? l - 26 : 0));
        } else if (i == W_BUILD) {
            fmt_commas(CUSTOM_TANK_FEE + (s->draft.hover ? HOVER_COINS : 0), a, sizeof a);
            snprintf(val, sizeof val, "\xE2\x96\xB6 %s COINS%s", a, s->draft.hover ? " +50CR" : "");
        } else if (i == W_EQUIP) snprintf(val, sizeof val, "\xE2\x96\xB6 USE AS P1");
        else snprintf(val, sizeof val, "%s", s->confirm_delete ? "\xE2\x96\xB6 SURE?" : "\xE2\x96\xB6 REMOVE");
        char shown[80];
        if (arrows) snprintf(shown, sizeof shown, "\xE2\x97\x84 %s \xE2\x96\xBA", val);
        else snprintf(shown, sizeof shown, "%s", val);
        term_text(t, x + W - 2 - text_len(shown), ry, shown, sel ? GOLD : P_TEXT, &bg);
    }
    int my = y + 2 + W_N;
    if (s->msg_t > 0) term_wrap_center(t, my, s->msg, 47, s->msg_ok ? P_OK : P_DANGER, &P_PANEL, x, W, 2);
    else {
        fmt_commas(CUSTOM_TANK_FEE, a, sizeof a);
        snprintf(buf, sizeof buf, "Parts come from tanks you own. BUILD: %s coins. HOVER: +25,000 coins +50 credits.", a);
        term_wrap_center(t, my, buf, 47, P_MUTED, &P_PANEL, x, W, 2);
    }
    ui_button(t, 2, T_ROWS - 3, "\xE2\x97\x84", P_PRIMARY, P_PANEL_HI, HOT_WS + 90);
    ui_button(t, 16, T_ROWS - 3, "Y TANK STUDIO", P_PRIMARY, P_PANEL_HI, HOT_WS + 92);
    ui_button(t, W - 7, T_ROWS - 3, "\xE2\x96\xBA", P_PRIMARY, P_PANEL_HI, HOT_WS + 91);
}

static void ws_destroy(Scene *sc) { Workshop *s = (Workshop *)sc; preview_free(&s->stage); free(s); }
static const SceneVT WS_VT = {.handle = ws_handle, .update = ws_update, .draw = ws_draw, .destroy = ws_destroy};
Scene *scene_workshop(void) {
    Workshop *s = (Workshop *)calloc(1, sizeof(Workshop));
    scene_base(&s->base, &WS_VT);
    draft_new(s);
    workshop_components(&g_app.save, &s->wp);
    s->pcolor = save_loadout(&g_app.save, 0)->tank_color;
    preview_init(&s->stage, 46, 26, 2, 3);
    return &s->base;
}

/* ================================================================== StudioScene (l.12174) */
enum { ST_MAP, ST_PAINT, ST_AMMO, ST_SHOT, ST_BACK, ST_N };
typedef struct {
    Scene base;
    int design, row, mi, ai, pi, si, applied;
    int16_t ammos[N_AMMOS];
    int nammo;
    NextFn back;
    PreviewStage stage;
} Studio;

static void st_handle(Scene *sc, const IEv *e) {
    Studio *s = (Studio *)sc;
    if (e->action == A_BACK) { app_goto(s->back()); return; }
    if (e->action == A_UP) { s->row = (s->row + ST_N - 1) % ST_N; return; }
    if (e->action == A_DOWN) { s->row = (s->row + 1) % ST_N; return; }
    int d = 0, key = s->row;
    if (e->action == A_LEFT || e->action == A_RIGHT) d = (e->action == A_LEFT ? -1 : 1) * (e->coarse ? 5 : 1);
    if (e->action == A_TOUCH && e->raw >= 1500 && e->raw < 1500 + ST_N) {
        key = e->raw - 1500;
        if (key != s->row) { s->row = key; return; }
        if (key == ST_BACK) { app_goto(s->back()); return; }
        d = 1;
    }
    if (d) {
        if (key == ST_MAP) s->mi = ((s->mi + d) % N_MAPS_ALL + N_MAPS_ALL) % N_MAPS_ALL;
        else if (key == ST_PAINT) s->pi = ((s->pi + d) % N_PAINTS + N_PAINTS) % N_PAINTS;
        else if (key == ST_AMMO) s->ai = ((s->ai + d) % s->nammo + s->nammo) % s->nammo;
        else if (key == ST_SHOT) s->si = ((s->si + d) % N_PAINTS + N_PAINTS) % N_PAINTS;
    } else if (ev_confirm(e) && key == ST_BACK) app_goto(s->back());
}
static void st_update(Scene *sc, float dt) {
    Studio *s = (Studio *)sc;
    backdrop_scene_update(sc, dt);
    preview_update(&s->stage, dt);
}
static void st_draw(Scene *sc, Term *t) {
    Studio *s = (Studio *)sc;
    backdrop_scene_draw(sc, t);
    SaveData *sv = &g_app.save;
    float tm = (float)g_app.time;
    char buf[96];
    /* top: live preview (46x44 pixels = 22 rows) */
    term_box(t, 1, 0, 49, 24, TB_DOUBLE, P_LINE, P_PANEL, "LIVE PREVIEW", P_PRIMARY);
    if (s->applied != s->mi) { preview_set_map(&s->stage, s->mi); s->applied = s->mi; }
    preview_set_kit(&s->stage, s->design, paint_rgb(s->pi), s->ammos[s->ai], paint_rgb(s->si), 0);
    term_blit(t, preview_render(&s->stage), 3, 1, 0, 0);
    term_center(t, 23, design_ptr(s->design)->name, P_WHITE, &P_PANEL, 1, 49);
    /* bottom: rows */
    int x = 1, y = T_SPLIT, W = 49;
    term_box(t, x, y, W, T_SPLIT, TB_DOUBLE, P_LINE, P_PANEL, "TANK STUDIO", P_PRIMARY);
    static const char *const LAB[ST_N] = {"BATTLEFIELD", "TANK PAINT", "AMMO", "SHELL COLOR", ""};
    for (int i = 0; i < ST_N; i++) {
        int ry = y + 2 + i * 3;
        bool sel = i == s->row;
        RGB bg = sel ? P_PANEL_HI : P_PANEL;
        term_fill(t, x + 1, ry, W - 2, 1, bg);
        term_text(t, x + 2, ry, LAB[i], sel ? P_WHITE : P_MUTED, &bg);
        hot_add(x + 1, ry, W - 2, 2, 1500 + i);
        char val[64];
        if (i == ST_MAP) snprintf(val, sizeof val, "%s", MAPDEFS[s->mi].name);
        else if (i == ST_PAINT) snprintf(val, sizeof val, "%s%s", paint_name(s->pi), save_paint_ok(sv, s->pi, true) ? "" : "  (LOCKED)");
        else if (i == ST_AMMO) snprintf(val, sizeof val, "%s", AMMOS[s->ammos[s->ai]].name);
        else if (i == ST_SHOT) snprintf(val, sizeof val, "%s%s", paint_name(s->si), save_paint_ok(sv, s->si, true) ? "" : "  (LOCKED)");
        else snprintf(val, sizeof val, "\xE2\x97\x84  BACK");
        if (sel && i != ST_BACK) snprintf(buf, sizeof buf, "\xE2\x97\x84 %.30s \xE2\x96\xBA", val);
        else snprintf(buf, sizeof buf, "%.32s", val);
        term_text(t, x + W - 2 - text_len(buf), ry, buf, sel ? GOLD : P_TEXT, &bg);
        if (i == ST_PAINT || i == ST_SHOT)
            term_text(t, x + 2, ry + 1, "\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88", live_color(paint_rgb(i == ST_PAINT ? s->pi : s->si), tm), &P_PANEL);
    }
    bool locked = !save_paint_ok(sv, s->pi, true) || !save_paint_ok(sv, s->si, false);
    term_wrap_center(t, y + 18, locked ? "Paints marked LOCKED are preview-only until you own them."
                                       : "Preview your tank on any battlefield with any paint, ammo and shell colour. Nothing is saved.",
                     45, P_MUTED, &P_PANEL, x, W, 3);
}
static void st_destroy(Scene *sc) { Studio *s = (Studio *)sc; preview_free(&s->stage); free(s); }
static const SceneVT ST_VT = {.handle = st_handle, .update = st_update, .draw = st_draw, .destroy = st_destroy};
Scene *scene_studio_for(int design, int paint, NextFn back) {
    Studio *s = (Studio *)calloc(1, sizeof(Studio));
    scene_base(&s->base, &ST_VT);
    s->design = design;
    s->back = back;
    s->pi = paint;
    SaveData *sv = &g_app.save;
    Loadout *lo = save_loadout(sv, 0);
    for (int a = 0; a < AMMOS_SECRET_FIRST + AMMOS_SECRET_COUNT + AMMOS_ANCIENT_COUNT && a < N_AMMOS; a++)
        if (save_ammo_unlocked(sv, a)) s->ammos[s->nammo++] = (int16_t)a;
    if (!s->nammo) s->ammos[s->nammo++] = AMMO_STANDARD;
    for (int i = 0; i < s->nammo; i++) if (s->ammos[i] == lo->ammo) s->ai = i;
    s->si = lo->shot_color;
    s->applied = -1;
    preview_init(&s->stage, 46, 44, 2, 3);
    return &s->base;
}
Scene *scene_studio(void) { return scene_studio_for(save_loadout(&g_app.save, 0)->tank, save_loadout(&g_app.save, 0)->tank_color, scene_workshop); }
