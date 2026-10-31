/* Missions roadmap, co-op choice and mission result (terminaltanks.py l.5733-5765, 11758-11990, 11993-12111). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "director.h"
#include "scenes.h"
#include "ui.h"
#include "render.h"
#include "sprites.h"
#include "netplay.h"

static const RGB GOLD = {255, 206, 100};
static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static void *scene_new(size_t size, const SceneVT *vt) {
    Scene *s = (Scene *)calloc(1, size);
    scene_base(s, vt);
    return s;
}
#define NEW(T, vt) ((T *)scene_new(sizeof(T), vt))

static const struct { int first; const char *act, *name; RGB col; } M_ACTS[5] = {
    {1, "ACT I", "BASIC TRAINING", {120, 225, 150}}, {11, "ACT II", "THE FIELD", {255, 190, 90}}, {21, "ACT III", "HOT ZONES", {255, 120, 100}},
    {31, "ACT IV", "THE ELITE", {130, 190, 255}}, {41, "ACT V", "THE ALIEN THREAT", {120, 255, 200}}};
static const RGB M_TYPE_COLORS[6] = {{130, 225, 140}, {255, 180, 90}, {120, 255, 200}, {130, 190, 255}, {255, 110, 100}, {255, 206, 100}};
static const char *const M_TYPE_SHORT[6] = {"RUN", "2v1", "UFO", "CO-OP", "RUSH", "BOSS"};
static const char *const DIFF_NAMES[4] = {"EASY", "NORMAL", "HARD", "MASTER"};

static int m_act(int n) {
    int a = 0;
    for (int i = 0; i < 5; i++) if (M_ACTS[i].first <= n) a = i;
    return a;
}
static int m_state(int n) {
    SaveData *sv = &g_app.save;
    if ((sv->mission_completed >> (n - 1)) & 1) return 2;
    return save_mission_unlocked(sv, n) ? 1 : 0;
}

/* ------------------------------------------------------------------ building the session (build_mission_session l.5754) */
static struct { int mission, nlo; Loadout los[2]; bool wingman; } g_mrestart;
void mission_restart(void) {
    build_mission(g_mrestart.mission, g_mrestart.los, g_mrestart.nlo, g_mrestart.wingman, NULL, false);
}
void build_mission(int mission, const Loadout *los, int nlo, bool wingman, const Loadout *guest, bool net) {
    g_mrestart.mission = mission;
    g_mrestart.nlo = nlo > 2 ? 2 : nlo;
    for (int i = 0; i < g_mrestart.nlo; i++) g_mrestart.los[i] = los[i];
    g_mrestart.wingman = wingman;
    PlayerSetup humans[2];
    int nh = humans_for(mission, los, nlo, wingman, guest, humans);
    session_init_mission(&g_sess, mission, humans, nh, (uint32_t)rng_below(&g_app.rng, 1u << 30));
    (void)net;
    app_goto(scene_battle());
}

/* mission_art (l.11783) */
static Tank fake_tank(int i, RGB color, float x, float y, int facing, uint8_t kind, double scale, double hover, int hp, int shield) {
    Tank t;
    memset(&t, 0, sizeof t);
    t.index = i;
    t.color = color;
    t.shot_color = (RGB){255, 255, 255};
    t.x = x;
    t.y = y;
    t.facing = facing;
    t.angle = facing > 0 ? 50.0 : 130.0;
    t.kind = kind;
    t.scale = scale;
    t.hover = hover;
    t.hp = t.max_hp = hp;
    t.shield = shield;
    t.design = DESIGN_RANGER;
    return t;
}
static void mission_art(Surf *cv, const TTMission *m, float t, bool locked) {
    for (int py = 0; py < 16; py++) {
        Px c = rgb15(cmix((RGB){6, 10, 24}, (RGB){30, 40, 70}, py / 15.0f));
        for (int x = 0; x < 34; x++) cv->px[py * 34 + x] = c;
    }
    for (int x = 0; x < 34; x++)
        for (int y = 0; y < 3; y++) cv_plot(cv, x, y, locked ? rgb15i(40, 46, 52) : rgb15i(40, 60, 50));
    RGB dim = {70, 80, 92};
#define PC(...) (locked ? dim : (RGB)(__VA_ARGS__))
    RGB ally = {110, 200, 235}, foe = {235, 120, 100};
    const TankDesign *D0 = &DESIGNS[DESIGN_RANGER];
    bool coop = m->kind == MK_COOP;
    if (m->kind == MK_UFO || m->kind == MK_MOTHER || (coop && m->coop_ufo)) {
        double big = m->kind == MK_MOTHER ? 2.2 : 2.0;
        Tank u = fake_tank(0, PC(((RGB){90, 230, 160})), 25.0f, 7.0f, -1, KIND_UFO, big, 4.0, 5, m->kind != MK_UFO ? 1 : 0);
        draw_ufo(cv, &u, t, 0);
        if (m->kind == MK_MOTHER)
            for (int k = 0; k < 2; k++) {
                Tank d = fake_tank(k + 1, PC(((RGB){190, 140, 255})), 8.0f + k * 9, 9.0f, -1, KIND_UFO, 1.3, 0, 1, 0);
                draw_ufo(cv, &d, t + k, 0);
            }
        if (coop) {
            draw_tank(cv, 4.0f, 3, 1, 55, PC(ally), 1, 0, false, 0, D0, t, -1, -1);
            draw_tank(cv, 11.0f, 3, 1, 55, PC(((RGB){235, 200, 110})), 1, 0, false, 0, D0, t, -1, -1);
        } else if (m->kind == MK_UFO) {
            draw_tank(cv, 6.0f, 3, 1, 60, PC(ally), 1, 0, false, 0, D0, t, -1, -1);
        }
    } else if (m->kind == MK_MARATHON) {
        for (int k = 0; k < 3; k++) draw_tank(cv, 5.0f + k * 8, 3, 1, 50, PC(cmix(ally, (RGB){255, 255, 255}, 0.15f * k)), 1, 0, false, 0, D0, t, -1, -1);
        draw_tank(cv, 29.0f, 3, -1, 130, PC(foe), 1, 0, false, 0, D0, t, -1, -1);
        cv_plotf(cv, 18, 12, PC(GOLD));
    } else if (m->kind == MK_RUSH) {
        int n = m->nlevels > 4 ? 4 : m->nlevels;
        for (int k = 0; k < n; k++) {
            const TournLevel *lv = &TOURNAMENT[m->levels[k] - 1];
            draw_tank(cv, 5.0f + k * 8.5f, 3, k ? -1 : 1, 130, PC(COLOR_CHOICES[lv->boss_color].c), 1, 0, false, 0, &DESIGNS[lv->tank], t, -1, -1);
        }
    } else {
        draw_tank(cv, 5.0f, 3, 1, 50, PC(ally), 1, 0, false, 0, D0, t, -1, -1);
        if (coop) draw_tank(cv, 13.0f, 3, 1, 50, PC(((RGB){235, 200, 110})), 1, 0, false, 0, D0, t, -1, -1);
        for (int k = 0; k < m->enemies; k++)
            draw_tank(cv, 29.0f - k * 9, 3, -1, 130, PC(k == 0 ? foe : (RGB){235, 170, 90}), 1, 0, false, 0, D0, t, -1, -1);
    }
#undef PC
}

static int first_coins(int n) { return mission_first_coins(n); }
static int repeat_coins(int n) { return mission_repeat_coins(n); }

/* ================================================================== MissionsScene (l.11826) */
typedef struct { Scene base; int sel; char msg[64]; float msg_t; Canvas art; } Missions;
#define HOT_MIS 1000
static Missions *g_mis_cur;
static void mission_setup_done(const Loadout *los, int n) {
    (void)n;
    build_mission(g_sess.mission > 0 ? g_sess.mission : 1, los, 1, false, NULL, false);
}
static int g_pending_mission;
static void mission_setup_done1(const Loadout *los, int n) { (void)n; build_mission(g_pending_mission, los, 1, false, NULL, false); }

static void mis_handle(Scene *sc, const IEv *e) {
    Missions *s = (Missions *)sc;
    int step = e->coarse ? 5 : 1;
    if (e->action == A_BACK) { app_goto(scene_menu()); return; }
    if (e->action == A_LEFT || e->action == A_UP || (e->action == A_TOUCH && e->raw == HOT_MIS)) s->sel = s->sel - step > 0 ? s->sel - step : 0;
    else if (e->action == A_RIGHT || e->action == A_DOWN || (e->action == A_TOUCH && e->raw == HOT_MIS + 1)) s->sel = s->sel + step < N_MISSIONS - 1 ? s->sel + step : N_MISSIONS - 1;
    else if (e->action == A_TOUCH && e->raw >= HOT_MIS + 10 && e->raw < HOT_MIS + 10 + N_MISSIONS) s->sel = e->raw - HOT_MIS - 10;
    else if (ev_confirm(e) || (e->action == A_TOUCH && e->raw == HOT_MIS + 2)) {
        const TTMission *m = &MISSIONS[s->sel];
        if (!save_mission_unlocked(&g_app.save, m->number)) {
            snprintf(s->msg, sizeof s->msg, "MISSION %d IS LOCKED - CLEAR MISSION %d FIRST", m->number, m->number - 1);
            s->msg_t = 2.5f;
        } else if (m->kind == MK_COOP) {
            app_goto(scene_coop_choice(m->number));
        } else {
            g_pending_mission = m->number;
            app_goto(scene_setup(SM_MISSION, 0, NULL, mission_setup_done1, 1));
        }
    }
    (void)mission_setup_done;
}
static void mis_update(Scene *sc, float dt) {
    Missions *s = (Missions *)sc;
    backdrop_scene_update(sc, dt);
    s->msg_t = fmaxf(0, s->msg_t - dt);
}
static void mis_draw(Scene *sc, Term *t) {
    Missions *s = (Missions *)sc;
    g_mis_cur = s;
    backdrop_scene_draw(sc, t);
    SaveData *sv = &g_app.save;
    float tm = (float)g_app.time;
    const TTMission *m = &MISSIONS[s->sel];
    int st = m_state(m->number), ai = m_act(m->number);
    RGB col = M_TYPE_COLORS[m->kind];
    char buf[128];
    term_fill(t, 0, 0, T_COLS, 24, P_BG);
    int ncl = 0;
    for (int i = 0; i < N_MISSIONS; i++) ncl += (int)((sv->mission_completed >> i) & 1);
    term_text(t, 1, 0, "M I S S I O N S", GOLD, NULL);
    snprintf(buf, sizeof buf, "%d/%d CLEARED", ncl, N_MISSIONS);
    term_text(t, T_COLS - 1 - text_len(buf), 0, buf, ncl == N_MISSIONS ? P_OK : P_TEXT, NULL);
    snprintf(buf, sizeof buf, "%s \xC2\xB7 %s", M_ACTS[ai].act, M_ACTS[ai].name);
    term_center(t, 1, buf, M_ACTS[ai].col, NULL, 0, -1);
    /* ribbon: 50 pips in two rows of 25, two columns apart */
    int per = 25, stride = 2, start = (T_COLS - per * stride) / 2 + 1;
    for (int i = 0; i < N_MISSIONS; i++) {
        const TTMission *mm = &MISSIONS[i];
        int sst = m_state(i + 1);
        RGB c, mc = M_TYPE_COLORS[mm->kind];
        uint32_t ch;
        if (sst == 2) { ch = 0x25C6; c = mc; }
        else if (sst == 1) { ch = 0x25C7; c = cmix(mc, P_WHITE, 0.4f + 0.3f * fsin(tm * 4 + i)); }
        else { ch = 0xB7; c = (RGB){70, 84, 98}; }
        if (mm->nrewards && sst) ch = sst == 2 ? 0x2605 : 0x2606;
        int px = start + (i % per) * stride, py = 3 + i / per;
        if (i == s->sel) {
            RGB hl = cmix(mc, P_WHITE, 0.5f + 0.4f * fsin(tm * 6));
            term_put(t, px - 1, py, '[', hl, NULL);
            term_put(t, px + 1, py, ']', hl, NULL);
            term_put(t, px, py, ch, P_WHITE, NULL);
        } else term_put(t, px, py, ch, c, NULL);
        hot_add(px - 1, py, 2, 1, HOT_MIS + 10 + i);
    }
    /* the selected card */
    int cx = 7, cy = 6, w = 37, h = 10;
    bool locked = st == 0;
    RGB edge = st == 2 ? col : st == 1 ? cmix(P_PRIMARY, P_WHITE, 0.25f) : (RGB){62, 76, 90};
    if (st) edge = cmix(edge, P_WHITE, 0.35f + 0.25f * fsin(tm * 5));
    snprintf(buf, sizeof buf, "MISSION %d", m->number);
    term_box(t, cx, cy, w, h, TB_DOUBLE, edge, P_PANEL, buf, locked ? P_MUTED : P_WHITE);
    mission_art(&s->art.s, m, tm, locked);
    term_blit(t, &s->art.s, cx + 1, cy + 1, 0, 0);
    const char *badge = st == 2 ? " \xE2\x9C\x93 CLEARED " : st == 1 ? " \xE2\x96\xB6 OPEN " : " \xE2\x96\x91 LOCKED ";
    term_text(t, cx + w - text_len(badge) - 2, cy + h - 1, badge, edge, &P_PANEL);
    snprintf(buf, sizeof buf, " %s ", mission_type_name(m->number));
    term_text(t, cx + 2, cy + h - 1, buf, locked ? P_MUTED : col, &P_PANEL);
    for (int d = -1; d <= 1; d += 2) {
        int i = s->sel + d;
        if (i < 0 || i >= N_MISSIONS) continue;
        const TTMission *mm = &MISSIONS[i];
        int sst = m_state(i + 1);
        RGB mc = M_TYPE_COLORS[mm->kind];
        RGB e = sst == 2 ? mc : sst == 1 ? cmix(P_PRIMARY, P_WHITE, 0.25f) : (RGB){62, 76, 90};
        int x = d < 0 ? 0 : cx + w + 1;
        term_box(t, x, cy, 6, 10, TB_SINGLE, e, P_PANEL, "", P_TEXT);
        snprintf(buf, sizeof buf, "M%d", i + 1);
        term_center(t, cy + 1, buf, sst ? P_TEXT : P_MUTED, &P_PANEL, x, 6);
        term_center(t, cy + 3, M_TYPE_SHORT[mm->kind], sst ? mc : cshade(mc, 0.45f), &P_PANEL, x, 6);
        term_center(t, cy + 5, sst == 2 ? "\xE2\x9C\x93" : sst == 1 ? "\xE2\x96\xB6" : "\xE2\x96\x91", e, &P_PANEL, x, 6);
        if (mm->nrewards) term_center(t, cy + 7, "\xE2\x98\x85", sst ? GOLD : (RGB){96, 86, 50}, &P_PANEL, x, 6);
        hot_add(x, cy, 6, 10, HOT_MIS + (d < 0 ? 0 : 1));
    }
    if (s->sel > 1) { snprintf(buf, sizeof buf, "\xE2\x97\x82%d", s->sel - 1); term_text(t, 0, cy + 11, buf, P_MUTED, NULL); }
    if (s->sel < N_MISSIONS - 2) { snprintf(buf, sizeof buf, "%d\xE2\x96\xB8", N_MISSIONS - 2 - s->sel); term_text(t, T_COLS - text_len(buf), cy + 11, buf, P_MUTED, NULL); }
    /* blurb on the top screen under the card */
    term_wrap_center(t, 18, m->blurb, 47, P_TEXT, NULL, 0, T_COLS, 5);
    /* detail panel (touch screen) */
    int py = T_SPLIT;
    snprintf(buf, sizeof buf, "MISSION %d - %s", m->number, m->name);
    term_box(t, 0, py, T_COLS, T_SPLIT, TB_DOUBLE, cmix(col, (RGB){0, 0, 0}, 0.3f), P_PANEL, buf, col);
    int ax = 2;
    char foe[48];
    switch (m->kind) {
    case MK_MARATHON: snprintf(foe, sizeof foe, "%d AI RIVALS", m->stages); break;
    case MK_2V1: snprintf(foe, sizeof foe, "2 AI TANKS"); break;
    case MK_UFO: snprintf(foe, sizeof foe, "1 ALIEN SAUCER"); break;
    case MK_COOP: snprintf(foe, sizeof foe, "%s", m->coop_ufo ? "1 ALIEN SAUCER" : "2 AI TANKS"); break;
    case MK_RUSH: snprintf(foe, sizeof foe, "%d TOURNAMENT BOSSES", m->stages); break;
    default: snprintf(foe, sizeof foe, "MOTHERSHIP + 2 DRONES"); break;
    }
    term_text(t, ax, py + 1, "TYPE", P_MUTED, &P_PANEL);
    term_text(t, ax + 6, py + 1, mission_type_name(m->number), col, &P_PANEL);
    term_text(t, ax, py + 2, "FOES", P_MUTED, &P_PANEL);
    term_text(t, ax + 6, py + 2, foe, P_TEXT, &P_PANEL);
    char goal[96];
    mission_objective(m->number, goal, sizeof goal);
    term_text(t, ax, py + 3, "GOAL", P_MUTED, &P_PANEL);
    term_text(t, ax + 6, py + 3, goal, P_TEXT, &P_PANEL);
    if (m->kind != MK_MARATHON || m->diff == m->diff_end) snprintf(buf, sizeof buf, "%s", DIFF_NAMES[m->diff]);
    else snprintf(buf, sizeof buf, "%s > %s", DIFF_NAMES[m->diff], DIFF_NAMES[m->diff_end]);
    term_text(t, ax, py + 4, "AI", P_MUTED, &P_PANEL);
    term_text(t, ax + 6, py + 4, buf, P_AMBER, &P_PANEL);
    char rule[96];
    mission_rule_text(m->number, rule, sizeof rule);
    term_text(t, ax, py + 5, rule, GOLD, &P_PANEL);
    bool first_left = !((sv->mission_paid >> (m->number - 1)) & 1);
    snprintf(buf, sizeof buf, "%s  %d \xE2\x97\x89", first_left ? "FIRST CLEAR" : "REPLAY", first_left ? first_coins(m->number) : repeat_coins(m->number));
    term_text(t, ax, py + 7, buf, first_left ? GOLD : P_MUTED, &P_PANEL);
    if (m->kind == MK_COOP) term_text(t, ax + 24, py + 7, "LOCAL \xC2\xB7 LAN \xC2\xB7 AI WINGMAN", (RGB){130, 190, 255}, &P_PANEL);
    term_text(t, ax, py + 9, m->number == N_MISSIONS ? "GRAND PRIZE" : "REWARDS", GOLD, &P_PANEL);
    bool earned = st == 2;
    for (int k = 0; k < m->nrewards && k < 4; k++) {
        char lab[24], nm[48];
        reward_label(m->rewards[k], lab, sizeof lab, nm, sizeof nm);
        snprintf(buf, sizeof buf, "%s%-7s%s", earned ? "\xE2\x9C\x93 " : "\xC2\xB7 ", lab, nm);
        term_text(t, ax, py + 10 + k, buf, earned ? P_OK : P_TEXT, &P_PANEL);
    }
    if (!m->nrewards) term_text(t, ax, py + 10, "\xC2\xB7 COINS ONLY", P_MUTED, &P_PANEL);
    const char *status;
    RGB scol;
    char sb[64];
    if (st == 2) { status = "\xE2\x9C\x93 CLEARED - REPLAY ANY TIME"; scol = P_OK; }
    else if (st == 1) { status = "\xE2\x96\xB6 READY"; scol = P_PRIMARY; }
    else { snprintf(sb, sizeof sb, "\xE2\x96\x91 LOCKED - CLEAR MISSION %d FIRST", m->number - 1); status = sb; scol = P_MUTED; }
    term_text(t, ax, py + 15, status, scol, &P_PANEL);
    ui_button(t, 2, T_ROWS - 4, "\xE2\x97\x84", P_PRIMARY, P_PANEL_HI, HOT_MIS);
    ui_button(t, 19, T_ROWS - 4, st ? "A  START" : "LOCKED", st ? P_OK : P_MUTED, P_PANEL_HI, HOT_MIS + 2);
    ui_button(t, T_COLS - 7, T_ROWS - 4, "\xE2\x96\xBA", P_PRIMARY, P_PANEL_HI, HOT_MIS + 1);
    if (s->msg_t > 0) term_center(t, T_ROWS - 2, s->msg, P_DANGER, &P_PANEL, 0, -1);
    else {
        static const Keycap K[3] = {{"\xE2\x97\x84\xE2\x96\xBA", "MISSION"}, {"L/R", "JUMP 5"}, {"B", "BACK"}};
        ui_keycaps_center(t, T_ROWS - 2, K, 3, &P_PANEL, 0, T_COLS);
    }
}
static void mis_destroy(Scene *sc) { Missions *s = (Missions *)sc; canvas_free(&s->art); free(s); }
static const SceneVT MIS_VT = {.handle = mis_handle, .update = mis_update, .draw = mis_draw, .destroy = mis_destroy};
Scene *scene_missions(int number) {
    Missions *s = NEW(Missions, &MIS_VT);
    SaveData *sv = &g_app.save;
    if (number <= 0) {
        number = N_MISSIONS;
        for (int n = 1; n <= N_MISSIONS; n++)
            if (save_mission_unlocked(sv, n) && !((sv->mission_completed >> (n - 1)) & 1)) { number = n; break; }
    }
    s->sel = number - 1;
    canvas_alloc(&s->art, 34, 16);
    return &s->base;
}

/* ================================================================== CoopChoiceScene (l.11993) */
typedef struct { Scene base; int m; MenuList menu; } Coop;
static int g_coop_m;
static void coop_local_done(const Loadout *los, int n) { build_mission(g_coop_m, los, n, false, NULL, false); }
static void coop_wing_done(const Loadout *los, int n) { (void)n; build_mission(g_coop_m, los, 1, true, NULL, false); }
extern void lan_host_coop(int mission, const Loadout *lo);
static void coop_lan_done(const Loadout *los, int n) { (void)n; lan_host_coop(g_coop_m, &los[0]); }
static void coop_handle(Scene *sc, const IEv *e) {
    Coop *s = (Coop *)sc;
    if (e->action == A_BACK) { app_goto(scene_missions(s->m)); return; }
    int c = menu_handle(&s->menu, e, 1100);
    g_coop_m = s->m;
    if (c == 0) app_goto(scene_setup(SM_MISSION, 0, NULL, coop_local_done, 2));
    else if (c == 1) app_goto(scene_setup(SM_MISSION, 0, NULL, coop_lan_done, 1));
    else if (c == 2) app_goto(scene_setup(SM_MISSION, 0, NULL, coop_wing_done, 1));
    else if (c == 3) app_goto(scene_missions(s->m));
}
static void coop_draw(Scene *sc, Term *t) {
    Coop *s = (Coop *)sc;
    backdrop_scene_draw(sc, t);
    const TTMission *m = &MISSIONS[s->m - 1];
    char buf[96], goal[96];
    snprintf(buf, sizeof buf, "CO-OP MISSION %d: %s", m->number, m->name);
    int x, y;
    ui_panel(t, 51, 18, buf, -1, T_SPLIT, T_SPLIT, P_LINE, &x, &y);
    mission_objective(m->number, goal, sizeof goal);
    term_center(t, y + 2, goal, P_TEXT, &P_PANEL, x, 51);
    menu_draw(t, &s->menu, T_COLS / 2, y + 5, (float)g_app.time, 1, 30, 1100);
    term_center(t, y + 11, "LAN: both players earn coins and the kit;", P_MUTED, &P_PANEL, x, 51);
    term_center(t, y + 12, "only the host's mission progress advances.", P_MUTED, &P_PANEL, x, 51);
    term_center(t, y + 13, "LOCAL: the one save on this DS earns it all.", P_MUTED, &P_PANEL, x, 51);
}
static const SceneVT COOP_VT = {.handle = coop_handle, .update = backdrop_scene_update, .draw = coop_draw};
Scene *scene_coop_choice(int mission) {
    Coop *s = NEW(Coop, &COOP_VT);
    s->m = mission;
    static const char *const ITEMS[4] = {"LOCAL TWO PLAYER", "HOST OVER LAN", "SOLO + AI WINGMAN", "BACK"};
    menu_init(&s->menu, ITEMS, 4);
    return &s->base;
}

/* ================================================================== MissionResultScene (l.12044) */
typedef struct { Scene base; float t; } MisRes;
static int g_next_mission;
static Scene *mres_go(void) { return scene_missions(g_next_mission); }
static void misr_enter(Scene *sc) {
    (void)sc;
    g_app.wallet_flash = 1.6f;
    if (session_completed(&g_sess)) {
        RGB c[3] = {M_TYPE_COLORS[MISSIONS[g_sess.mission - 1].kind], GOLD, P_WHITE};
        backdrop_celebrate(g_app.backdrop, c, 3);
    }
}
static void misr_exit(Scene *sc) { (void)sc; backdrop_celebrate(g_app.backdrop, NULL, 0); }
static void misr_handle(Scene *sc, const IEv *e) {
    (void)sc;
    Session *s = &g_sess;
    bool ok = session_completed(s);
    const TTMission *m = &MISSIONS[s->mission - 1];
    if (((e->action == A_OTHER && e->raw == K_Y) || (e->action == A_TOUCH && e->raw == 2)) && !ok && !s->net) { session_restart(); return; }
    if (ev_confirm(e) || (e->action == A_TOUCH && e->raw == 1)) {
        if (s->net) net_leave();
        g_next_mission = ok && m->number < N_MISSIONS && !s->net ? m->number + 1 : m->number;
        if (s->net) app_goto(mres_go());
        else if (m->kind == MK_MOTHER && ok) whisper(STORY_MOTHERSHIP, mres_go);
        else if (m->kind == MK_UFO || (m->kind == MK_COOP && m->coop_ufo)) whisper(STORY_ALIEN, mres_go);
        else app_goto(mres_go());
    }
}
static void misr_update(Scene *sc, float dt) { backdrop_scene_update(sc, dt); ((MisRes *)sc)->t += dt; }
static void misr_draw(Scene *sc, Term *t) {
    MisRes *r = (MisRes *)sc;
    backdrop_scene_draw(sc, t);
    Session *s = &g_sess;
    const TTMission *m = &MISSIONS[s->mission - 1];
    bool ok = session_completed(s);
    RGB col = M_TYPE_COLORS[m->kind];
    char buf[96];
    snprintf(buf, sizeof buf, "MISSION %d", m->number);
    int x, y;
    ui_panel(t, 51, 20, buf, -1, T_SPLIT - 2, T_SPLIT + 4, ok ? col : P_DANGER, &x, &y);
    const char *head = ok ? "M I S S I O N   C O M P L E T E" : "M I S S I O N   F A I L E D";
    if (ok) ui_prism_text(t, x + (51 - text_len(head)) / 2, y + 2, head, r->t, 0.12f, 0.05f, 0.55f, 1.0f, &P_PANEL);
    else term_center(t, y + 2, head, P_DANGER, &P_PANEL, x, 51);
    term_center(t, y + 3, m->name, col, &P_PANEL, x, 51);
    if (s->has_payout) {
        snprintf(buf, sizeof buf, "%s+%d \xE2\x97\x89", s->first_clear ? "FIRST CLEAR  " : ok ? "REPLAY  " : "CONSOLATION  ", s->payout_coins);
        term_center(t, y + 5, buf, GOLD, &P_PANEL, x, 51);
    }
    if (ok && s->first_clear && m->nrewards) {
        term_center(t, y + 7, m->number == N_MISSIONS ? "GRAND PRIZE UNLOCKED" : "UNLOCKED", m->number == N_MISSIONS ? GOLD : P_OK, &P_PANEL, x, 51);
        for (int k = 0; k < m->nrewards; k++) {
            float a = clampf((r->t - 0.6f - k * 0.5f) / 0.4f, 0, 1);
            if (a <= 0) continue;
            char lab[24], nm[48];
            reward_label(m->rewards[k], lab, sizeof lab, nm, sizeof nm);
            snprintf(buf, sizeof buf, "%s: %s", lab, nm);
            term_center(t, y + 9 + k, buf, cmix(P_PANEL, GOLD, a), &P_PANEL, x, 51);
        }
    } else if (ok) {
        term_center(t, y + 8, "ALREADY CLEARED - NO NEW REWARDS", P_MUTED, &P_PANEL, x, 51);
    } else {
        term_center(t, y + 8, "ONLY THIS MISSION RESTARTS", P_AMBER, &P_PANEL, x, 51);
    }
    if ((int)(r->t * 2) % 2 == 0 && r->t > 0.6f) {
        if (ok) ui_button(t, x + 16, y + 17, "A CONTINUE", P_PRIMARY, P_PANEL, 1);
        else {
            ui_button(t, x + (s->net ? 16 : 5), y + 17, "A MISSIONS", P_PRIMARY, P_PANEL, 1);
            if (!s->net) ui_button(t, x + 28, y + 17, "Y RETRY", P_PRIMARY, P_PANEL, 2);
        }
    }
}
static const SceneVT MISR_VT = {.enter = misr_enter, .exit = misr_exit, .handle = misr_handle, .update = misr_update, .draw = misr_draw, .music = music_result};
Scene *scene_mission_result(void) { return &NEW(MisRes, &MISR_VT)->base; }
