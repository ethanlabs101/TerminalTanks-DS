/* Tournament roadmap + result, story whispers + cutscenes, Terminal Ticket result, the Martyr's legacy and finale
 * (terminaltanks.py l.11202-11405, 12599-12953, 12955-13100). */
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
static void *scene_new(size_t size, const SceneVT *vt) {
    Scene *s = (Scene *)calloc(1, size);
    scene_base(s, vt);
    return s;
}
#define NEW(T, vt) ((T *)scene_new(sizeof(T), vt))

/* ------------------------------------------------------------------ reward labels (l.11766) */
void reward_label(int uid, char *lab, int nl, char *name, int nn) {
    const char *u = UNLOCK_UIDS[uid];
    const char *c = strchr(u, ':');
    char kind[24], key[48];
    if (!c) { snprintf(lab, (size_t)nl, "%s", u); name[0] = 0; return; }
    snprintf(kind, sizeof kind, "%.*s", (int)(c - u), u);
    snprintf(key, sizeof key, "%s", c + 1);
    int i;
    if (!strcmp(kind, "tank") && (i = design_by_key(key)) >= 0) { snprintf(lab, (size_t)nl, "TANK"); snprintf(name, (size_t)nn, "%s", DESIGNS[i].name); return; }
    if (!strcmp(kind, "ammo") && (i = ammo_by_key(key)) >= 0) { snprintf(lab, (size_t)nl, "AMMO"); snprintf(name, (size_t)nn, "%s", AMMOS[i].name); return; }
    if (!strcmp(kind, "map") && (i = map_by_key(key)) >= 0) { snprintf(lab, (size_t)nl, "MAP"); snprintf(name, (size_t)nn, "%s", MAPDEFS[i].name); return; }
    if (!strcmp(kind, "theme")) {
        snprintf(lab, (size_t)nl, "SCREEN");
        for (int h = 0; h < N_HOME; h++)
            if (!strcasecmp(HOME_ORDER[h], key)) { snprintf(name, (size_t)nn, "%s", h > 0 && h < HOME_MASTER ? HOME_THEMES[h].name : HOME_ORDER[h]); return; }
        snprintf(name, (size_t)nn, "%s", key);
        for (char *p = name; *p; p++) if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
        return;
    }
    snprintf(lab, (size_t)nl, "%s", kind);
    snprintf(name, (size_t)nn, "%s", key);
    for (char *p = lab; *p; p++) if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
    for (char *p = name; *p; p++) if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
}

/* reward_lines (l.11202) */
typedef struct { char lab[24], name[40]; } RewardLine;
static int reward_lines(int level, bool compact, RewardLine *out) {
    const TournLevel *lv = &TOURNAMENT[level - 1];
    int n = 0;
    if (level == N_LEVELS) {
        const char *mn = MAPDEFS[lv->map].name;
        if (compact) {
            snprintf(out[n].lab, 24, "TANK %d", lv->tank + 1); snprintf(out[n++].name, 40, "%s", DESIGNS[lv->tank].name);
            snprintf(out[n].lab, 24, "AMMO %d", lv->ammo + 1); snprintf(out[n++].name, 40, "%s", AMMOS[lv->ammo].name);
            snprintf(out[n].lab, 24, "MAP"); snprintf(out[n++].name, 40, "%s", !strncmp(mn, "THE ", 4) ? mn + 4 : mn);
            snprintf(out[n].lab, 24, "MASTER"); snprintf(out[n++].name, 40, "DIFFICULTY");
            snprintf(out[n].lab, 24, "MASTER"); snprintf(out[n++].name, 40, "HOME SCREEN");
        } else {
            snprintf(out[n].lab, 24, "MASTER TANK"); snprintf(out[n++].name, 40, "%s", DESIGNS[lv->tank].name);
            snprintf(out[n].lab, 24, "MASTER AMMO"); snprintf(out[n++].name, 40, "%s", AMMOS[lv->ammo].name);
            snprintf(out[n].lab, 24, "MASTER MAP"); snprintf(out[n++].name, 40, "%s", mn);
            snprintf(out[n].lab, 24, "MASTER DIFFICULTY"); snprintf(out[n++].name, 40, "SINGLE PLAYER");
            snprintf(out[n].lab, 24, "MASTER HOME SCREEN"); snprintf(out[n++].name, 40, "TITLE + MENU");
        }
        return n;
    }
    snprintf(out[n].lab, 24, "TANK %d", lv->tank + 1); snprintf(out[n++].name, 40, "%s", DESIGNS[lv->tank].name);
    snprintf(out[n].lab, 24, "AMMO %d", lv->ammo + 1); snprintf(out[n++].name, 40, "%s", AMMOS[lv->ammo].name);
    if (lv->map >= 0) { snprintf(out[n].lab, 24, "MAP"); snprintf(out[n++].name, 40, "%s", MAPDEFS[lv->map].name); }
    return n;
}

static const struct { int first; const char *act, *name; RGB col; } ACTS[4] = {
    {1, "ACT I", "THE PROVING GROUNDS", {120, 225, 150}}, {6, "ACT II", "THE IRON ROAD", {255, 190, 90}},
    {11, "ACT III", "THE ELITE", {130, 190, 255}}, {18, "FINALE", "THE MASTER'S ROAD", {255, 206, 100}}};
static int act_of(int n) {
    int a = 0;
    for (int i = 0; i < 4; i++) if (ACTS[i].first <= n) a = i;
    return a;
}
static const char *const DIFF_NAMES[4] = {"EASY", "NORMAL", "HARD", "MASTER"};
static RGB diff_color(int d) { return d == DIFF_MASTER ? GOLD : d == DIFF_EASY ? P_OK : d == DIFF_NORMAL ? P_AMBER : P_DANGER; }

/* ================================================================== start helpers */
static Scene *go_battle(void) { return scene_battle(); }

void start_tournament(int level, const PlayerSetup *me) {
    session_init_tournament(&g_sess, level, me, (uint32_t)rng_below(&g_app.rng, 1u << 30));
    if (level == 1) whisper(STORY_FIRST_BATTLE, go_battle);
    else app_goto(scene_battle());
}

void start_ticket(int ticket) {
    if (ticket < 0 || !save_tickets_open(&g_app.save)) { app_goto(scene_shop(4)); return; }
    if (ticket >= N_TICKETS) {
        g_app.save.ticket_active = -1;
        app_flush();
        app_goto(scene_shop(4));
        return;
    }
    session_init_ticket(&g_sess, ticket, save_loadout(&g_app.save, 0), (uint32_t)rng_below(&g_app.rng, 1u << 30));
    app_goto(scene_battle());
}

/* cutscene page lists */
typedef struct { const CinePage *p; int n; } PageList;
static PageList g_pages[3];
static int g_npl;
static Scene *martyr_go(void) {
    g_app.save.martyr_seen = true;
    app_flush();
    return scene_battle();
}
static Scene *martyr_cine(void);
void start_martyr(bool intro, bool replay) {
    (void)intro;
    SaveData *sv = &g_app.save;
    int ph = replay ? 0 : (sv->martyr_phase < 2 ? sv->martyr_phase : 2);
    session_init_martyr(&g_sess, save_loadout(sv, 0), ph, replay, (uint32_t)rng_below(&g_app.rng, 1u << 30));
    g_npl = 0;
    if (!sv->martyr_seen) g_pages[g_npl++] = (PageList){MARTYR_INTRO, MARTYR_INTRO_N};
    if (ph == 0) g_pages[g_npl++] = (PageList){MARTYR_PHASE0, MARTYR_PHASE0_N};
    else if (ph == 1) g_pages[g_npl++] = (PageList){MARTYR_BETWEEN1, MARTYR_BETWEEN1_N};
    else g_pages[g_npl++] = (PageList){MARTYR_BETWEEN2, MARTYR_BETWEEN2_N};
    if (!replay && !sv->martyr_seen) whisper(STORY_HOARDERS, martyr_cine);
    else app_goto(martyr_cine());
}

/* ================================================================== CinematicScene (l.12793) */
#define MAX_PAGES 24
typedef struct {
    Scene base;
    CinePage pages[MAX_PAGES];
    int n, i;
    float age;
    bool done;
    NextFn next;
    float stars[90][3];
    MapPreview mp;
    Canvas cv;
} Cine;

static void cine_finish(Cine *c) {
    if (c->done) return;
    c->done = true;
    app_goto(c->next());
}
static void cine_handle(Scene *sc, const IEv *e) {
    Cine *c = (Cine *)sc;
    if (e->action == A_BACK) { cine_finish(c); return; }
    if (ev_confirm(e) || e->action == A_TOUCH) {
        int len = (int)strlen(c->pages[c->i].text);
        if (c->age * 45 < len) c->age = len / 45.0f + 0.01f;
        else if (c->i + 1 >= c->n) cine_finish(c);
        else { c->i++; c->age = 0; }
    }
}
static void cine_update(Scene *sc, float dt) { ((Cine *)sc)->age += dt; }
static void cine_draw(Scene *sc, Term *t) {
    Cine *c = (Cine *)sc;
    term_fill(t, 0, 0, T_COLS, T_ROWS, (RGB){3, 2, 10});
    const CinePage *pg = &c->pages[c->i < c->n ? c->i : c->n - 1];
    float tm = (float)g_app.time;
    int W = T_COLS, H = 48;          /* the art fills the top screen */
    Surf *cv = &c->cv.s;
    int st = pg->stage;
    if (st >= CS_MAP0) {
        Loadout *lo = save_loadout(&g_app.save, 0);
        int designs[2] = {lo->tank, DESIGN_A_MARTYR};
        RGB cols[2] = {paint_rgb(lo->tank_color), (RGB){190, 150, 255}};
        Surf *m = mappreview_render(&c->mp, MAPS_ANCIENT_FIRST + (st - CS_MAP0), W, H, tm, designs, cols);
        memcpy(cv->px, m->px, sizeof(Px) * (size_t)(W * H));
    } else {
        for (int py = 0; py < H; py++) {
            Px row = rgb15(cmix((RGB){4, 2, 14}, st != CS_WIN ? (RGB){60, 24, 96} : (RGB){110, 70, 40}, py / (float)(H - 1)));
            for (int x = 0; x < W; x++) cv->px[py * W + x] = row;
        }
        for (int i = 0; i < 90; i++)
            cv_blendf(cv, c->stars[i][0] * W, c->stars[i][1] * H, P_WHITE, 0.3f + 0.5f * (0.5f + 0.5f * fsin(tm * 2 + c->stars[i][2])));
        float sc = H / 28.0f;
        if (st == CS_ARTIFACT || st == CS_AWAKE) {
            draw_artifact(cv, W / 2.0f, H / 2.0f, sc * 1.5f, tm);
            if (st == CS_AWAKE)
                for (int k = 0; k < 14; k++) {
                    float a = k * 6.2831853f / 14 + tm * 0.3f;
                    for (int j = 0; j < (int)(H * 0.5f); j++)
                        cv_blendf(cv, W / 2.0f + fcos(a) * j * 1.6f, H / 2.0f + fsin(a) * j, (RGB){255, 226, 150}, 0.25f * fminf(1, c->age / 2) * (1 - j / (H * 0.5f)));
                }
        } else if (st == CS_MAGE) {
            draw_artifact(cv, W * 0.28f, H * 0.42f, sc, tm);
            draw_mage(cv, W * 0.62f, H * 0.08f, fmaxf(1.0f, H * 0.78f / 32), tm, fminf(1.0f, 0.25f + c->age / 3), true);
        } else if (st == CS_DUEL) {
            for (int x = 0; x < W; x++)
                for (int y = 0; y < (int)fmaxf(1, H * 0.1f); y++) cv_plot(cv, x, y, rgb15i(24, 14, 44));
            Loadout *lo = save_loadout(&g_app.save, 0);
            draw_tank(cv, W * 0.25f, H * 0.1f, 1, 40, paint_rgb(lo->tank_color), 2, 0, false, 0, design_ptr(lo->tank), tm, -1, -1);
            draw_mage(cv, W * 0.7f, H * 0.1f, fmaxf(1.0f, H * 0.78f / 32), tm, 1.0f, true);
        } else if (st == CS_WIN) {
            draw_artifact(cv, W * 0.3f, H * 0.42f, sc, tm);
            draw_mage(cv, W * 0.62f, H * 0.08f, fmaxf(1.0f, H * 0.78f / 32), tm, c->i >= 1 ? fmaxf(0.0f, 1 - c->age / 6) * 0.9f : 0.9f, true);
            PyRandom r;
            rng_seed(&r, (uint32_t)(int)(tm * 5));
            for (int k = 0; k < 40; k++) {
                float dx = (float)rng_uniform(&r, -12, 12), off = (float)rng_uniform(&r, 0, H);
                cv_blendf(cv, W * 0.62f + dx, H * 0.12f + fmodf(tm * 9 + off, H * 0.8f), (RGB){255, 226, 150}, 0.7f);
            }
        }
    }
    if (pg->flash && c->age < 0.6f) {
        float k = (1 - c->age / 0.6f) * 0.9f;
        Px w = rgb15(P_WHITE);
        for (int i = 0; i < W * H; i++) cv->px[i] = pxmix(cv->px[i], w, k);
    }
    term_blit(t, cv, 0, 0, 0, 0);
    /* the dialogue box on the touch screen */
    int bw = 49, bx = 1, by = T_SPLIT + 2;
    term_box(t, bx, by, bw, 16, TB_DOUBLE, P_LINE, P_PANEL, "", P_PRIMARY);
    char who[32];
    snprintf(who, sizeof who, " %s ", pg->who);
    term_text(t, bx + 3, by, who, !strcmp(pg->who, "THE MARTYR") ? GOLD : P_MUTED, &P_PANEL);
    int shown = (int)(c->age * 45);
    char lines[10][64];
    int nl = wrap_text(pg->text, bw - 6, lines, 10);
    for (int k = 0; k < nl; k++) {
        int len = (int)strlen(lines[k]);
        int m = shown < 0 ? 0 : (shown < len ? shown : len);
        char part[64];
        memcpy(part, lines[k], (size_t)m);
        part[m] = 0;
        term_text(t, bx + 3, by + 2 + k, part, P_WHITE, &P_PANEL);
        shown -= len + 1;
    }
    if (c->age * 45 >= (float)strlen(pg->text) && (int)(tm * 2) % 2 == 0) term_text(t, bx + bw - 10, by + 14, "\xE2\x96\xBC A", P_PRIMARY, &P_PANEL);
    char foot[48];
    snprintf(foot, sizeof foot, "%d/%d   B SKIP", c->i + 1, c->n);
    term_text(t, bx + 3, by + 15, foot, P_MUTED, &P_PANEL);
    hot_add(0, T_SPLIT, T_COLS, T_SPLIT, 1);
}
static void cine_destroy(Scene *sc) {
    Cine *c = (Cine *)sc;
    mappreview_free(&c->mp);
    canvas_free(&c->cv);
    free(c);
}
static const SceneVT CINE_VT = {.handle = cine_handle, .update = cine_update, .draw = cine_draw, .destroy = cine_destroy};

static Scene *cine_new(const PageList *lists, int nl, NextFn next) {
    Cine *c = NEW(Cine, &CINE_VT);
    c->base.show_wallet = false;
    for (int l = 0; l < nl; l++)
        for (int i = 0; i < lists[l].n && c->n < MAX_PAGES; i++) c->pages[c->n++] = lists[l].p[i];
    c->next = next;
    for (int i = 0; i < 90; i++) {
        PyRandom a, b;
        rng_seed(&a, (uint32_t)(i * 7 + 5));
        rng_seed(&b, (uint32_t)(i * 13 + 1));
        c->stars[i][0] = (float)rng_uniform(&a, 0, 1);
        c->stars[i][1] = (float)rng_uniform(&b, 0, 1);
        c->stars[i][2] = i * 0.7f;
    }
    mappreview_init(&c->mp);
    canvas_alloc(&c->cv, T_COLS, 48);
    return &c->base;
}

static Scene *martyr_cine(void) { return cine_new(g_pages, g_npl, martyr_go); }

/* Between Martyr phases (MartyrSession.interlude): which = -1 picks the page list from the session */
Scene *scene_cinematic(int which, NextFn next) {
    (void)which;
    PageList pl;
    if (g_sess.type == SESS_MARTYR && g_sess.won_phase) {
        if (g_sess.phase_i == 1) pl = (PageList){MARTYR_BETWEEN1, MARTYR_BETWEEN1_N};
        else pl = (PageList){MARTYR_BETWEEN2, MARTYR_BETWEEN2_N};
        return cine_new(&pl, 1, next);
    }
    return next();
}

static Scene *martyr_rewards_next(void) { return scene_martyr_rewards(); }
Scene *scene_martyr_outro(void) {
    PageList pl = {MARTYR_OUTRO, MARTYR_OUTRO_N};
    return cine_new(&pl, 1, martyr_rewards_next);
}

/* whisper (l.12745): plays a story line once, then continues */
static CinePage g_wpages[2];
void whisper(int story, NextFn next) {
    if (story >= 0 && story <= STORY_HOARDERS && save_hear(&g_app.save, story)) {
        app_flush();
        int n = 0;
        for (int k = 0; k < 2; k++)
            if (STORY_LINES[story][k]) g_wpages[n++] = (CinePage){"A VOICE", STORY_LINES[story][k], CS_VOICE, 0};
        PageList pl = {g_wpages, n};
        app_goto(cine_new(&pl, 1, next));
    } else {
        app_goto(next());
    }
}

/* ================================================================== TournamentScene (l.11225) */
typedef struct { Scene base; int sel; PreviewStage stage; Canvas mini; char msg[64]; float msg_t, moved; } Tourney;
static int lv_state(int n) {
    SaveData *sv = &g_app.save;
    if ((sv->completed >> (n - 1)) & 1) return 2;
    return save_level_unlocked(sv, n) ? 1 : 0;
}
#define HOT_TOUR 800
static void tour_handle(Scene *sc, const IEv *e) {
    Tourney *s = (Tourney *)sc;
    int step = e->coarse ? 5 : 1;
    if (e->action == A_BACK) { app_goto(scene_menu()); return; }
    if (e->action == A_LEFT || e->action == A_UP || (e->action == A_TOUCH && e->raw == HOT_TOUR)) { s->sel = s->sel - step > 0 ? s->sel - step : 0; s->moved = 1; }
    else if (e->action == A_RIGHT || e->action == A_DOWN || (e->action == A_TOUCH && e->raw == HOT_TOUR + 1)) { s->sel = s->sel + step < N_LEVELS - 1 ? s->sel + step : N_LEVELS - 1; s->moved = 1; }
    else if (e->action == A_TOUCH && e->raw >= HOT_TOUR + 10 && e->raw < HOT_TOUR + 10 + N_LEVELS) s->sel = e->raw - HOT_TOUR - 10;
    else if (ev_confirm(e) || (e->action == A_TOUCH && e->raw == HOT_TOUR + 2)) {
        int n = s->sel + 1;
        if (save_level_unlocked(&g_app.save, n)) app_goto(scene_setup(SM_TOURNAMENT, n, NULL, NULL, 1));
        else { snprintf(s->msg, sizeof s->msg, "LEVEL %d IS LOCKED - CLEAR LEVEL %d FIRST", n, n - 1); s->msg_t = 2.5f; }
    }
}
static void tour_update(Scene *sc, float dt) {
    Tourney *s = (Tourney *)sc;
    backdrop_scene_update(sc, dt);
    preview_update(&s->stage, dt);
    s->msg_t = fmaxf(0, s->msg_t - dt);
    s->moved = fmaxf(0, s->moved - dt * 4);
}
static RGB edge_col(int st, RGB act, bool selected) {
    RGB e = st == 2 ? act : st == 1 ? cmix(P_PRIMARY, P_WHITE, 0.25f) : (RGB){62, 76, 90};
    return selected && st ? cmix(e, P_WHITE, 0.35f + 0.25f * fsin((float)g_app.time * 5)) : e;
}
static void tour_draw(Scene *sc, Term *t) {
    Tourney *s = (Tourney *)sc;
    backdrop_scene_draw(sc, t);
    SaveData *sv = &g_app.save;
    float tm = (float)g_app.time;
    const TournLevel *lv = &TOURNAMENT[s->sel];
    int st = lv_state(s->sel + 1), ai = act_of(s->sel + 1);
    RGB actc = ACTS[ai].col;
    int ncl = 0;
    for (int i = 0; i < N_LEVELS; i++) ncl += (sv->completed >> i) & 1;
    char buf[96];
    term_fill(t, 0, 0, T_COLS, 24, P_BG);
    term_text(t, 1, 0, "T O U R N A M E N T", GOLD, NULL);
    snprintf(buf, sizeof buf, "%d/%d CLEARED", ncl, N_LEVELS);
    term_text(t, T_COLS - 1 - text_len(buf), 1, buf, ncl == N_LEVELS ? P_OK : P_TEXT, NULL);
    snprintf(buf, sizeof buf, "%s \xC2\xB7 %s", ACTS[ai].act, ACTS[ai].name);
    term_text(t, 1, 1, buf, actc, NULL);
    /* ribbon */
    int y = 3, px[N_LEVELS];
    for (int i = 0; i < N_LEVELS; i++) px[i] = 2 + (int)lroundf(i * (T_COLS - 5) / (float)(N_LEVELS - 1));
    for (int i = 0; i < N_LEVELS - 1; i++) {
        bool done = (sv->completed >> i) & 1;
        RGB col = done ? ACTS[act_of(i + 1)].col : (RGB){50, 62, 74};
        for (int x = px[i] + 1; x < px[i + 1]; x++) term_put(t, x, y + 1, done ? 0x2500 : 0x2504, col, NULL);
    }
    for (int i = 0; i < N_LEVELS; i++) {
        int sst = lv_state(i + 1);
        RGB ac = ACTS[act_of(i + 1)].col, col;
        bool fin = i + 1 == N_LEVELS;
        uint32_t ch;
        if (sst == 2) { ch = fin ? 0x2605 : 0x25C6; col = ac; }
        else if (sst == 1) { ch = fin ? 0x2606 : 0x25C7; col = cmix(ac, P_WHITE, 0.4f + 0.3f * fsin(tm * 4 + i)); }
        else { ch = 0xB7; col = (RGB){70, 84, 98}; }
        bool sel = i == s->sel;
        term_put(t, px[i], y + 1, ch, sel ? P_WHITE : col, NULL);
        if (sel) term_put(t, px[i], y, 0x25BC, cmix(ac, P_WHITE, 0.5f + 0.4f * fsin(tm * 6)), NULL);
        snprintf(buf, sizeof buf, "%d", i + 1);
        term_text(t, px[i] - (int)strlen(buf) / 2, y + 2 + (i % 2), buf, sel ? P_WHITE : sst == 0 ? (RGB){110, 124, 138} : col, NULL);
        if (TOURNAMENT[i].rule) term_put(t, px[i], y + 4, '!', sst ? GOLD : (RGB){96, 86, 50}, NULL);
        hot_add(px[i] - 1, y, 3, 5, HOT_TOUR + 10 + i);
    }
    /* cards: the selected one big, its neighbours as slim tabs */
    int cy = 9, cx = 7, w = 37, h = 11;
    bool locked = st == 0;
    RGB edge = edge_col(st, actc, true);
    snprintf(buf, sizeof buf, "LEVEL %d", lv->number);
    term_box(t, cx, cy, w, h, TB_DOUBLE, edge, P_PANEL, buf, locked ? P_MUTED : P_WHITE);
    preview_set_kit(&s->stage, lv->tank, COLOR_CHOICES[lv->boss_color].c, lv->ammo, COLOR_CHOICES[lv->boss_shot].c, locked ? 0.5f : 0.0f);
    term_blit(t, preview_render(&s->stage), cx + 1, cy + 1, 0, 0);
    const char *badge = st == 2 ? " \xE2\x9C\x93 CLEARED " : st == 1 ? " \xE2\x96\xB6 OPEN " : " \xE2\x96\x91 LOCKED ";
    term_text(t, cx + w - text_len(badge) - 2, cy + h - 1, badge, edge, &P_PANEL);
    snprintf(buf, sizeof buf, " %.16s ", lv->boss);
    term_text(t, cx + 2, cy + h - 1, buf, locked ? P_MUTED : P_WHITE, &P_PANEL);
    for (int d = -1; d <= 1; d += 2) {
        int i = s->sel + d;
        if (i < 0 || i >= N_LEVELS) continue;
        int sst = lv_state(i + 1);
        int x = d < 0 ? 0 : cx + w + 1;
        RGB e = edge_col(sst, ACTS[act_of(i + 1)].col, false);
        term_box(t, x, cy + 1, 6, 9, TB_SINGLE, e, P_PANEL, "", P_TEXT);
        snprintf(buf, sizeof buf, "%d", i + 1);
        term_center(t, cy + 2, "LV", sst ? P_TEXT : P_MUTED, &P_PANEL, x, 6);
        term_center(t, cy + 3, buf, sst ? P_TEXT : P_MUTED, &P_PANEL, x, 6);
        RGB dc = diff_color(TOURNAMENT[i].difficulty);
        snprintf(buf, sizeof buf, "%.4s", DIFF_NAMES[TOURNAMENT[i].difficulty]);
        term_center(t, cy + 5, buf, sst ? dc : cshade(dc, 0.45f), &P_PANEL, x, 6);
        term_center(t, cy + 7, sst == 2 ? "\xE2\x9C\x93" : sst == 1 ? "\xE2\x96\xB6" : "\xE2\x96\x91", e, &P_PANEL, x, 6);
        hot_add(x, cy + 1, 6, 9, HOT_TOUR + (d < 0 ? 0 : 1));
    }
    if (s->sel > 1) { snprintf(buf, sizeof buf, "\xE2\x97\x82%d", s->sel - 1); term_text(t, 0, cy + 11, buf, P_MUTED, NULL); }
    if (s->sel < N_LEVELS - 2) { snprintf(buf, sizeof buf, "%d\xE2\x96\xB8", N_LEVELS - 2 - s->sel); term_text(t, T_COLS - text_len(buf), cy + 11, buf, P_MUTED, NULL); }
    /* detail panel (touch screen) */
    bool gold = lv->difficulty == DIFF_MASTER;
    int py = T_SPLIT;
    snprintf(buf, sizeof buf, "LEVEL %d - %s", lv->number, lv->name);
    term_box(t, 0, py, T_COLS, T_SPLIT, TB_DOUBLE, gold ? GOLD : cmix(actc, (RGB){0, 0, 0}, 0.25f), P_PANEL, buf, actc);
    int ax = 2;
    char goal[96];
    level_objective(lv->number, goal, sizeof goal);
    const char *labs[4] = {"BOSS", "AI", "GOAL", "RULE"};
    const char *vals[4] = {lv->boss, DIFF_NAMES[lv->difficulty], goal, level_rule_tag(lv->number)};
    RGB vcol[4] = {COLOR_CHOICES[lv->boss_color].c, diff_color(lv->difficulty), P_TEXT, lv->rule ? GOLD : P_MUTED};
    for (int k = 0; k < 4; k++) {
        term_text(t, ax, py + 1 + k, labs[k], P_MUTED, &P_PANEL);
        term_text(t, ax + 6, py + 1 + k, vals[k], vcol[k], &P_PANEL);
    }
    term_text(t, ax, py + 5, level_rule_text(lv->number), lv->rule ? GOLD : P_MUTED, &P_PANEL);
    char lines[2][64];
    int nl = wrap_text(lv->blurb, 47, lines, 2);
    for (int k = 0; k < nl; k++) term_text(t, ax, py + 7 + k, lines[k], P_TEXT, &P_PANEL);
    snprintf(buf, sizeof buf, "TANK  %s", DESIGNS[lv->tank].name);
    term_text(t, ax, py + 10, buf, P_TEXT, &P_PANEL);
    snprintf(buf, sizeof buf, "AMMO  %s", AMMOS[lv->ammo].name);
    term_text(t, ax, py + 11, buf, P_AMBER, &P_PANEL);
    if (lv->map >= 0) {
        snprintf(buf, sizeof buf, "MAP   %s", MAPDEFS[lv->map].name);
        term_text(t, ax, py + 12, buf, MAPDEFS[lv->map].accent, &P_PANEL);
    }
    int coins, credits;
    bool cleared = st == 2;
    tournament_payout(lv->number, true, !cleared, &coins, &credits);
    if (credits) snprintf(buf, sizeof buf, "%s %d \xE2\x97\x89 %d \xE2\x97\x88", cleared ? "REPLAY" : "FIRST CLEAR", coins, credits);
    else snprintf(buf, sizeof buf, "%s %d \xE2\x97\x89", cleared ? "REPLAY" : "FIRST CLEAR", coins);
    term_text(t, ax, py + 13, buf, cleared ? P_MUTED : GOLD, &P_PANEL);
    int rx = 29;
    term_text(t, rx, py + 9, "REWARDS", GOLD, &P_PANEL);
    RewardLine rl[5];
    int nr = reward_lines(lv->number, true, rl);
    for (int k = 0; k < nr; k++) {
        snprintf(buf, sizeof buf, "%s%-7.7s %.12s", cleared ? "\xE2\x9C\x93 " : "\xC2\xB7 ", rl[k].lab, rl[k].name);
        term_text(t, rx, py + 10 + k, buf, cleared ? P_OK : (st ? P_TEXT : P_MUTED), &P_PANEL);
    }
    const char *status;
    RGB scol;
    if (st == 2) { status = "\xE2\x9C\x93 CLEARED - REPLAY ANY TIME"; scol = P_OK; }
    else if (st == 1) { status = "\xE2\x96\xB6 READY TO FIGHT"; scol = P_PRIMARY; }
    else { snprintf(buf, sizeof buf, "\xE2\x96\x91 LOCKED - CLEAR LEVEL %d FIRST", lv->number - 1); status = buf; scol = P_MUTED; }
    char stb[96];
    snprintf(stb, sizeof stb, "%s", status);
    term_text(t, ax, py + 16, stb, scol, &P_PANEL);
    ui_button(t, 2, T_ROWS - 4, "\xE2\x97\x84", P_PRIMARY, P_PANEL_HI, HOT_TOUR);
    ui_button(t, 17, T_ROWS - 4, st ? "A  FIGHT / REPLAY" : "LOCKED", st ? P_OK : P_MUTED, P_PANEL_HI, HOT_TOUR + 2);
    ui_button(t, T_COLS - 7, T_ROWS - 4, "\xE2\x96\xBA", P_PRIMARY, P_PANEL_HI, HOT_TOUR + 1);
    if (s->msg_t > 0) term_center(t, T_ROWS - 2, s->msg, P_DANGER, &P_PANEL, 0, -1);
    else {
        static const Keycap K[3] = {{"\xE2\x97\x84\xE2\x96\xBA", "LEVEL"}, {"L/R", "JUMP 5"}, {"B", "BACK"}};
        ui_keycaps_center(t, T_ROWS - 2, K, 3, &P_PANEL, 0, T_COLS);
    }
}
static void tour_destroy(Scene *sc) { Tourney *s = (Tourney *)sc; preview_free(&s->stage); free(s); }
static const SceneVT TOUR_VT = {.handle = tour_handle, .update = tour_update, .draw = tour_draw, .destroy = tour_destroy};
Scene *scene_tournament(int level) {
    Tourney *s = NEW(Tourney, &TOUR_VT);
    SaveData *sv = &g_app.save;
    if (level <= 0) {
        level = N_LEVELS;
        for (int n = 1; n <= N_LEVELS; n++)
            if (save_level_unlocked(sv, n) && !((sv->completed >> (n - 1)) & 1)) { level = n; break; }
    }
    s->sel = level - 1;
    preview_init(&s->stage, 34, 16, 1, 3);
    return &s->base;
}

/* ================================================================== TournamentResultScene (l.13014) */
typedef struct { Scene base; float t; PreviewStage stage; } TourRes;
static bool tres_fresh(void) { return session_completed(&g_sess) && g_sess.first_clear; }
static void tres_enter(Scene *sc) {
    (void)sc;
    g_app.wallet_flash = 1.6f;
    if (tres_fresh()) {
        const TournLevel *lv = &TOURNAMENT[g_sess.level - 1];
        RGB c[3] = {COLOR_CHOICES[lv->boss_color].c, COLOR_CHOICES[lv->boss_shot].c, P_WHITE};
        backdrop_celebrate(g_app.backdrop, c, 3);
    }
}
static void tres_exit(Scene *sc) { (void)sc; backdrop_celebrate(g_app.backdrop, NULL, 0); }
static int g_next_level;
static Scene *tres_go(void) { return scene_tournament(g_next_level); }
static void tres_handle(Scene *sc, const IEv *e) {
    (void)sc;
    bool ok = session_completed(&g_sess);
    if (((e->action == A_OTHER && e->raw == K_Y) || (e->action == A_TOUCH && e->raw == 2)) && !ok) { session_restart(); return; }
    if (e->action == A_CONFIRM || e->action == A_FIRE || (e->action == A_TOUCH && e->raw == 1)) {
        int n = g_sess.level;
        g_next_level = ok && n < N_LEVELS ? n + 1 : n;
        if (ok && n == N_LEVELS) whisper(STORY_MASTER, tres_go);
        else app_goto(tres_go());
    }
}
static void tres_update(Scene *sc, float dt) {
    TourRes *s = (TourRes *)sc;
    backdrop_scene_update(sc, dt);
    s->t += dt;
    preview_update(&s->stage, dt);
}
static void tres_draw(Scene *sc, Term *t) {
    TourRes *s = (TourRes *)sc;
    backdrop_scene_draw(sc, t);
    Session *ss = &g_sess;
    const TournLevel *lv = &TOURNAMENT[ss->level - 1];
    RGB boss_col = COLOR_CHOICES[lv->boss_color].c;
    bool gold = lv->difficulty == DIFF_MASTER, ok = session_completed(ss), fresh = tres_fresh();
    RGB edge = ok ? (gold ? GOLD : P_OK) : P_DANGER;
    char buf[96];
    /* top: the kit */
    term_box(t, 2, 0, 47, 24, TB_DOUBLE, edge, P_PANEL, ok ? "REWARD KIT" : "BOSS KIT", P_PRIMARY);
    preview_set_kit(&s->stage, lv->tank, boss_col, lv->ammo, COLOR_CHOICES[lv->boss_shot].c, ok ? 0.0f : 0.5f);
    term_blit(t, preview_render(&s->stage), 5, 2, 0, 0);
    snprintf(buf, sizeof buf, "%s  +  %s", DESIGNS[lv->tank].name, AMMOS[lv->ammo].name);
    term_center(t, 16, buf, P_TEXT, &P_PANEL, 2, 47);
    int n1 = term_wrap_center(t, 18, DESIGNS[lv->tank].blurb, 43, P_MUTED, &P_PANEL, 2, 47, 2);
    term_wrap_center(t, 18 + n1, AMMOS[lv->ammo].blurb, 43, P_MUTED, &P_PANEL, 2, 47, 2);
    /* bottom: the result */
    int rx = 2, y = T_SPLIT;
    snprintf(buf, sizeof buf, "LEVEL %d", lv->number);
    term_box(t, rx, y, 47, 24, TB_DOUBLE, edge, P_PANEL, buf, P_PRIMARY);
    char pay[48] = "";
    if (ss->has_payout) {
        if (ss->payout_credits) snprintf(pay, sizeof pay, "+%d \xE2\x97\x89   +%d \xE2\x97\x88", ss->payout_coins, ss->payout_credits);
        else snprintf(pay, sizeof pay, "+%d \xE2\x97\x89", ss->payout_coins);
    }
    if (ok) {
        const char *head = "L E V E L   C L E A R E D";
        ui_prism_text(t, rx + (47 - text_len(head)) / 2, y + 2, head, s->t, 0.12f, 0.05f, 0.55f, 1.0f, &P_PANEL);
        snprintf(buf, sizeof buf, "%s DEFEATED", lv->boss);
        term_center(t, y + 4, buf, boss_col, &P_PANEL, rx, 47);
        snprintf(buf, sizeof buf, "%d ROUNDS WON \xC2\xB7 BOSS %d", ss->wins[0], ss->wins[1]);
        term_center(t, y + 5, buf, P_MUTED, &P_PANEL, rx, 47);
        if (ss->has_payout) {
            snprintf(buf, sizeof buf, "%s%s", fresh ? "FIRST CLEAR  " : "REPLAY  ", pay);
            term_center(t, y + 6, buf, GOLD, &P_PANEL, rx, 47);
        }
        if (!fresh) term_center(t, y + 9, "REWARDS ALREADY UNLOCKED", P_TEXT, &P_PANEL, rx, 47);
        else {
            RewardLine rl[5];
            int nr = reward_lines(lv->number, false, rl);
            for (int k = 0; k < nr; k++) {
                float a = clampf((s->t - 0.9f - k * 0.7f) / 0.4f, 0, 1);
                if (a <= 0) continue;
                snprintf(buf, sizeof buf, "\xE2\x9C\x93 %s %s", rl[k].lab, !strcmp(rl[k].lab, "MASTER HOME SCREEN") ? "ACTIVATED" : "UNLOCKED");
                term_text(t, rx + 3, y + 8 + k * 2, buf, cmix(P_PANEL, P_OK, a), &P_PANEL);
                term_text(t, rx + 5, y + 9 + k * 2, rl[k].name, cmix(P_PANEL, gold ? GOLD : P_AMBER, a), &P_PANEL);
            }
            if (lv->number == N_LEVELS && s->t > 4.8f) term_center(t, y + 19, "Press SELECT on the title or menu to switch home screens", P_MUTED, &P_PANEL, rx, 47);
        }
    } else {
        term_center(t, y + 3, "L E V E L   F A I L E D", P_DANGER, &P_PANEL, rx, 47);
        if (level_fail_at(lv->number) == 1) snprintf(buf, sizeof buf, "YOU LOST A ROUND");
        else snprintf(buf, sizeof buf, "%s WON %d ROUNDS", lv->boss, level_fail_at(lv->number));
        term_center(t, y + 5, buf, boss_col, &P_PANEL, rx, 47);
        snprintf(buf, sizeof buf, "YOU HAD %d/%d ROUND WINS", ss->wins[0], lv->wins);
        term_center(t, y + 7, buf, P_TEXT, &P_PANEL, rx, 47);
        term_center(t, y + 10, "ONLY THIS LEVEL RESTARTS", P_AMBER, &P_PANEL, rx, 47);
        term_center(t, y + 11, "ALL OTHER PROGRESS IS SAFE", P_MUTED, &P_PANEL, rx, 47);
        if (ss->has_payout) {
            snprintf(buf, sizeof buf, "CONSOLATION  %s", pay);
            term_center(t, y + 14, buf, GOLD, &P_PANEL, rx, 47);
        }
    }
    if ((int)(s->t * 2) % 2 == 0 && s->t > 0.6f) {
        if (ok) ui_button(t, rx + 13, y + 21, "A CONTINUE", P_PRIMARY, P_PANEL, 1);
        else {
            ui_button(t, rx + 3, y + 21, "A ROADMAP", P_PRIMARY, P_PANEL, 1);
            ui_button(t, rx + 24, y + 21, "Y RETRY LEVEL", P_PRIMARY, P_PANEL, 2);
        }
    }
}
static void tres_destroy(Scene *sc) { TourRes *s = (TourRes *)sc; preview_free(&s->stage); free(s); }
static const SceneVT TRES_VT = {.enter = tres_enter, .exit = tres_exit, .handle = tres_handle, .update = tres_update, .draw = tres_draw, .destroy = tres_destroy};
Scene *scene_tournament_result(void) {
    TourRes *s = NEW(TourRes, &TRES_VT);
    preview_init(&s->stage, 40, 26, 2, 3);
    return &s->base;
}

/* ================================================================== TicketResultScene (l.12955) */
typedef struct { Scene base; float t; } TicketRes;
static Scene *shop4(void) { return scene_shop(4); }
static void tkres_enter(Scene *sc) {
    (void)sc;
    g_app.wallet_flash = 1.6f;
    if (session_completed(&g_sess)) {
        RGB c[3] = {DESIGNS[TICKETS[g_sess.ticket].tank].accent, GOLD, P_WHITE};
        backdrop_celebrate(g_app.backdrop, c, 3);
    }
}
static void tkres_handle(Scene *sc, const IEv *e) {
    (void)sc;
    bool ok = session_completed(&g_sess);
    if (((e->action == A_OTHER && e->raw == K_Y) || (e->action == A_TOUCH && e->raw == 2)) && !ok) { session_restart(); return; }
    if (ev_confirm(e) || (e->action == A_TOUCH && e->raw == 1)) {
        if (ok && save_tickets_sold_out(&g_app.save)) whisper(STORY_HOARDERS, shop4);
        else app_goto(shop4());
    }
}
static void tkres_update(Scene *sc, float dt) { backdrop_scene_update(sc, dt); ((TicketRes *)sc)->t += dt; }
static void tkres_draw(Scene *sc, Term *t) {
    TicketRes *s = (TicketRes *)sc;
    backdrop_scene_draw(sc, t);
    const TicketBoss *b = &TICKETS[g_sess.ticket];
    bool ok = session_completed(&g_sess);
    RGB col = DESIGNS[b->tank].accent;
    int x, y;
    ui_panel(t, 51, 22, "TERMINAL TICKET", -1, T_SPLIT - 2, T_SPLIT + 4, ok ? col : P_DANGER, &x, &y);
    const char *head = ok ? "B O S S   D E F E A T E D" : "T I C K E T   B A T T L E   L O S T";
    if (ok) ui_prism_text(t, x + (51 - text_len(head)) / 2, y + 2, head, s->t, 0.12f, 0.05f, 0.55f, 1.0f, &P_PANEL);
    else term_center(t, y + 2, head, P_DANGER, &P_PANEL, x, 51);
    term_center(t, y + 3, b->name, col, &P_PANEL, x, 51);
    char buf[96];
    if (g_sess.has_payout) {
        snprintf(buf, sizeof buf, "%s+%d \xE2\x97\x89", ok ? "VICTORY  " : "CONSOLATION  ", g_sess.payout_coins);
        term_center(t, y + 5, buf, GOLD, &P_PANEL, x, 51);
    }
    if (ok) {
        term_center(t, y + 7, "CLAIMED", P_OK, &P_PANEL, x, 51);
        for (int k = 0; k < 4; k++) {
            float a = clampf((s->t - 0.6f - k * 0.5f) / 0.4f, 0, 1);
            if (a <= 0 || TICKET_REWARDS[g_sess.ticket][k] < 0) continue;
            char lab[24], nm[48];
            reward_label(TICKET_REWARDS[g_sess.ticket][k], lab, sizeof lab, nm, sizeof nm);
            snprintf(buf, sizeof buf, "%s: %s", lab, nm);
            term_center(t, y + 9 + k, buf, cmix(P_PANEL, GOLD, a), &P_PANEL, x, 51);
        }
        int left = 0;
        for (int i = 0; i < N_TICKETS; i++) left += !((g_app.save.tickets_won >> i) & 1);
        if (left == 0) snprintf(buf, sizeof buf, "ALL BOSSES DEFEATED - TICKETS SOLD OUT");
        else snprintf(buf, sizeof buf, "%d BOSS%s LEFT IN THE VAULT", left, left != 1 ? "ES" : "");
        term_center(t, y + 14, buf, P_MUTED, &P_PANEL, x, 51);
    } else {
        term_center(t, y + 8, "YOUR TICKET IS KEPT", P_AMBER, &P_PANEL, x, 51);
        term_center(t, y + 9, "RETRY ANY TIME - IT IS NOT WASTED", P_MUTED, &P_PANEL, x, 51);
    }
    if ((int)(s->t * 2) % 2 == 0 && s->t > 0.6f) {
        ui_button(t, x + (ok ? 18 : 6), y + 18, "A SHOP", P_PRIMARY, P_PANEL, 1);
        if (!ok) ui_button(t, x + 28, y + 18, "Y RETRY", P_PRIMARY, P_PANEL, 2);
    }
}
static const SceneVT TKRES_VT = {.enter = tkres_enter, .exit = tres_exit, .handle = tkres_handle, .update = tkres_update, .draw = tkres_draw};
Scene *scene_ticket_result(void) { return &NEW(TicketRes, &TKRES_VT)->base; }

/* ================================================================== MartyrRewardsScene (l.12884) + finale (l.12926) */
typedef struct { Scene base; PreviewStage stage; } MRew;
static void mrew_handle(Scene *sc, const IEv *e) {
    (void)sc;
    if (ev_confirm(e) || e->action == A_BACK || e->action == A_TOUCH) app_goto(scene_martyr_finale());
}
static void mrew_update(Scene *sc, float dt) { backdrop_scene_update(sc, dt); preview_update(&((MRew *)sc)->stage, dt); }
static void mrew_draw(Scene *sc, Term *t) {
    MRew *s = (MRew *)sc;
    backdrop_scene_draw(sc, t);
    float tm = (float)g_app.time;
    char buf[96];
    /* top: relics */
    term_box(t, 1, 0, 49, 24, TB_DOUBLE, GOLD, P_PANEL, "RELICS", P_PRIMARY);
    int i = (int)(tm / 2.5f);
    int d = DESIGNS_ANCIENT_FIRST + i % DESIGNS_ANCIENT_COUNT, a = AMMOS_ANCIENT_FIRST + i % AMMOS_ANCIENT_COUNT;
    preview_set_kit(&s->stage, d, COL_ASTRAL, a, COL_ASTRAL, 0);
    term_blit(t, preview_render(&s->stage), 2, 1, 0, 0);
    term_center(t, 15, DESIGNS[d].name, GOLD, &P_PANEL, 1, 49);
    term_wrap_center(t, 16, DESIGNS[d].blurb, 44, P_TEXT, &P_PANEL, 1, 49, 2);
    snprintf(buf, sizeof buf, "+ %s", AMMOS[a].name);
    term_center(t, 19, buf, P_PRIMARY, &P_PANEL, 1, 49);
    term_wrap_center(t, 20, AMMOS[a].blurb, 44, P_MUTED, &P_PANEL, 1, 49, 2);
    /* bottom: the list */
    int x = 1, y = T_SPLIT;
    term_box(t, x, y, 49, 24, TB_DOUBLE, GOLD, P_PANEL, "THE MARTYR'S LEGACY", P_PRIMARY);
    int row = y + 1;
    term_text(t, x + 2, row++, "ANCIENT ARTIFACT TANKS", GOLD, &P_PANEL);
    for (int k = 0; k < DESIGNS_ANCIENT_COUNT; k += 2) {
        snprintf(buf, sizeof buf, "\xE2\x9C\xA6 %-20.20s \xE2\x9C\xA6 %.20s", DESIGNS[DESIGNS_ANCIENT_FIRST + k].name,
                 k + 1 < DESIGNS_ANCIENT_COUNT ? DESIGNS[DESIGNS_ANCIENT_FIRST + k + 1].name : "");
        term_text(t, x + 2, row++, buf, P_WHITE, &P_PANEL);
    }
    term_text(t, x + 2, row++, "ANCIENT ARTIFACT AMMO", GOLD, &P_PANEL);
    for (int k = 0; k < AMMOS_ANCIENT_COUNT; k += 2) {
        snprintf(buf, sizeof buf, "\xE2\x9C\xA6 %-20.20s %s%.20s", AMMOS[AMMOS_ANCIENT_FIRST + k].name, k + 1 < AMMOS_ANCIENT_COUNT ? "\xE2\x9C\xA6 " : "",
                 k + 1 < AMMOS_ANCIENT_COUNT ? AMMOS[AMMOS_ANCIENT_FIRST + k + 1].name : "");
        term_text(t, x + 2, row++, buf, P_WHITE, &P_PANEL);
    }
    term_text(t, x + 2, row++, "BATTLEFIELDS", GOLD, &P_PANEL);
    for (int k = 0; k < MAPS_ANCIENT_COUNT; k++) {
        snprintf(buf, sizeof buf, "  \xE2\x9C\xA6 %s", MAPDEFS[MAPS_ANCIENT_FIRST + k].name);
        term_text(t, x + 2, row++, buf, MAPDEFS[MAPS_ANCIENT_FIRST + k].accent, &P_PANEL);
    }
    term_text(t, x + 2, row++, "COLOR", GOLD, &P_PANEL);
    term_text(t, x + 2, row++, "  \xE2\x9C\xA6 ASTRAL PAINT & AMMO COLOR", astral(tm), &P_PANEL);
    term_text(t, x + 2, row++, "6 HOME SCREENS: ONE FOR EVERY ANCIENT TANK", P_AMBER, &P_PANEL);
    term_text(t, x + 2, row++, "\xE2\x98\x85 ULTIMATE AMMO: MARTYR'S BLESSING", astral(tm), &P_PANEL);
    term_center(t, y + 22, "[ A ]", (int)(tm * 2) % 2 == 0 ? P_PRIMARY : P_MUTED, &P_PANEL, x, 49);
}
static void mrew_destroy(Scene *sc) { MRew *s = (MRew *)sc; preview_free(&s->stage); free(s); }
static const SceneVT MREW_VT = {.handle = mrew_handle, .update = mrew_update, .draw = mrew_draw, .destroy = mrew_destroy};
Scene *scene_martyr_rewards(void) {
    MRew *s = NEW(MRew, &MREW_VT);
    preview_init(&s->stage, 46, 26, 2, 3);
    return &s->base;
}

/* The finale shows the ETERNAL home screen in full: it uses the themed backdrop by temporarily viewing ETERNAL. */
typedef struct { Scene base; } MFin;
static void mfin_handle(Scene *sc, const IEv *e) { (void)sc; if (ev_confirm(e) || e->action == A_BACK || e->action == A_TOUCH) app_goto(scene_menu()); }
static void mfin_update(Scene *sc, float dt) { (void)sc; backdrop_update(app_home_backdrop(), dt); }
static void mfin_draw(Scene *sc, Term *t) {
    (void)sc;
    float tm = (float)g_app.time;
    const HomeTheme *th = &HOME_THEMES[HOME_ETERNAL];
    backdrop_draw(app_home_backdrop(), t, tm);
    int y = ui_logo(t, LOGO_THEME, 2, tm, true, HOME_ETERNAL, false);
    (void)y;
    static const struct { const char *s; RGB c; } L[6] = {{"THE LAST SEAL IS OPEN.", {255, 206, 100}}, {"THE HOURGLASS TURNS FOR YOU NOW.", {255, 244, 214}},
                                                         {"", {255, 255, 255}}, {"10 VAULTS \xC2\xB7 3 PHASES \xC2\xB7 1 LEGEND", {0, 0, 0}},
                                                         {"", {255, 255, 255}}, {"A new home screen waits for you every time you return.", {104, 124, 140}}};
    int bx = 2, bw = 47, by = T_SPLIT + 3;
    term_box(t, bx, by, bw, 10, TB_DOUBLE, GOLD, P_BG, "", P_PRIMARY);
    for (int k = 0; k < 6; k++) {
        if (k == 5) term_wrap_center(t, by + 2 + k, L[k].s, bw - 4, L[k].c, &P_BG, bx, bw, 1);
        else term_center(t, by + 2 + k, L[k].s, k == 3 ? th->accent : L[k].c, &P_BG, bx, bw);
    }
    if ((int)(tm * 2) % 2 == 0) term_center(t, by + 8, "[ A ]", P_PRIMARY, &P_BG, bx, bw);
}
static const SceneVT MFIN_VT = {.handle = mfin_handle, .update = mfin_update, .draw = mfin_draw};
Scene *scene_martyr_finale(void) {
    MFin *s = NEW(MFin, &MFIN_VT);
    return &s->base;
}
