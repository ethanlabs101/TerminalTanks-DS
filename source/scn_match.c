/* Match set-up and flow: RulesScene, SetupScene (pilot loadouts), MapSelectScene, RPSScene, RoundMapScene,
 * RoundMapWaitScene, MatchResultScene and the session routing helpers (terminaltanks.py l.7926-8268, 9820-10076,
 * 10280-10374, 10457-10660, 13149-13211, 5441-5548). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "scenes.h"
#include "ui.h"
#include "preview.h"
#include "netplay.h"

Session g_sess;
Rules g_rules_last;
static const RGB GOLD = {255, 206, 100};
static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }

static void *scene_new(size_t size, const SceneVT *vt) {
    Scene *s = (Scene *)calloc(1, size);
    scene_base(s, vt);
    return s;
}
#define NEW(T, vt) ((T *)scene_new(sizeof(T), vt))

/* ------------------------------------------------------------------ shared helpers */
int unlocked_map_list(int16_t *out) {
    int n = 0;
    for (int i = 0; i < N_MAPS_LIB; i++) out[n++] = (int16_t)i;
    for (int i = N_MAPS_LIB; i < N_MAPS_ALL; i++)
        if (save_map_unlocked(&g_app.save, i)) out[n++] = (int16_t)i;
    return n;
}

const char *unlock_hint(bool tank, int idx, char *buf, int n) {
    for (int l = 0; l < N_LEVELS; l++)
        if ((tank && TOURNAMENT[l].tank == idx) || (!tank && TOURNAMENT[l].ammo == idx)) {
            snprintf(buf, (size_t)n, "LOCKED - CLEAR TOURNAMENT LEVEL %d", l + 1);
            return buf;
        }
    char uid[48];
    snprintf(uid, sizeof uid, "%s:%s", tank ? "tank" : "ammo", tank ? DESIGNS[idx].key : AMMOS[idx].key);
    int u = uid_index(uid);
    for (int m = 0; m < N_MISSIONS && u >= 0; m++)
        for (int k = 0; k < MISSIONS[m].nrewards; k++)
            if (MISSIONS[m].rewards[k] == u) { snprintf(buf, (size_t)n, "LOCKED - CLEAR MISSION %d", m + 1); return buf; }
    for (int t = 0; t < N_TICKETS && u >= 0; t++)
        for (int k = 0; k < 4; k++)
            if (TICKET_REWARDS[t][k] == u) { snprintf(buf, (size_t)n, "LOCKED - WIN IT FROM A TERMINAL TICKET"); return buf; }
    for (int i = 0; i < N_SHOP && u >= 0; i++)
        if (SHOP[i].uid == u) {
            if (SHOP[i].credits) snprintf(buf, (size_t)n, "LOCKED - SHOP: %d COINS + %d CREDITS", (int)SHOP[i].coins, (int)SHOP[i].credits);
            else snprintf(buf, (size_t)n, "LOCKED - SHOP: %d COINS", (int)SHOP[i].coins);
            return buf;
        }
    snprintf(buf, (size_t)n, "LOCKED");
    return buf;
}

static void dots(int k, char *out, int n) {
    int o = 0;
    for (int i = 0; i < 3 && o < n - 4; i++) o += snprintf(out + o, (size_t)(n - o), "%s", i < k ? "\xE2\x97\x8F" : "\xE2\x97\x8B");
}

/* ------------------------------------------------------------------ session routing */
Scene *session_finish_scene(void) {
    switch (g_sess.type) {
    case SESS_TOURNAMENT: return scene_tournament_result();
    case SESS_MISSION: return scene_mission_result();
    case SESS_TICKET: return scene_ticket_result();
    case SESS_MARTYR: return g_sess.replay ? scene_shop(4) : scene_martyr_outro();
    case SESS_PRACTICE: return scene_tutorial(g_sess.adv >= 0);
    default: return scene_match_result();
    }
}
Scene *session_exit_scene(void) {
    switch (g_sess.type) {
    case SESS_TOURNAMENT: return scene_tournament(g_sess.level);
    case SESS_MISSION: return scene_missions(g_sess.mission);
    case SESS_TICKET: return scene_shop(4);
    case SESS_MARTYR: return scene_shop(4);
    case SESS_PRACTICE: return scene_tutorial(g_sess.adv >= 0);
    default: return scene_menu();
    }
}
const char *session_exit_label(void) {
    switch (g_sess.type) {
    case SESS_TOURNAMENT: return "TOURNAMENT";
    case SESS_MISSION: return "MISSIONS";
    case SESS_TICKET: return "SHOP";
    case SESS_MARTYR: return "SHOP";
    case SESS_PRACTICE: return "THE TUTORIAL";
    default: return "MAIN MENU";
    }
}

/* the config of the running match, kept for RESTART (MatchSession.restart: start_match(config, played[0], wind, rps=False)) */
static PlayerSetup g_cfg_players[MAX_TANKS];
static int g_cfg_n, g_cfg_mode, g_cfg_diff, g_cfg_target;
static char g_cfg_label[24];
static bool g_cfg_fx, g_cfg_special, g_cfg_pick, g_cfg_bless;
static double g_cfg_wind;

static void begin_match(const PlayerSetup *pl, int n, int mode, int diff, int target, const char *label, bool fx, bool special, bool pick,
                        bool bless, int first_map, double wind, bool rps) {
    memcpy(g_cfg_players, pl, sizeof(PlayerSetup) * (size_t)n);
    g_cfg_n = n; g_cfg_mode = mode; g_cfg_diff = diff; g_cfg_target = target;
    snprintf(g_cfg_label, sizeof g_cfg_label, "%s", label);
    g_cfg_fx = fx; g_cfg_special = special; g_cfg_pick = pick; g_cfg_bless = bless; g_cfg_wind = wind;
    session_init_match(&g_sess, pl, n, mode, diff, target, label, fx, special, pick, bless, first_map, (uint32_t)rng_below(&g_app.rng, 1u << 30));
    g_sess.has_wind_override = true;
    g_sess.wind_override = wind;
    start_match_session(rps);
}

void start_match_session(bool rps) {
    if (rps && g_sess.mode == MODE_TWO && !g_sess.net) app_goto(scene_rps());
    else app_goto(scene_battle());
}

void session_restart(void) {
    switch (g_sess.type) {
    case SESS_TOURNAMENT: start_tournament(g_sess.level, &g_sess.players[0]); return;
    case SESS_TICKET: start_ticket(g_sess.ticket); return;
    case SESS_MARTYR: start_martyr(false, g_sess.replay); return;
    case SESS_PRACTICE: app_goto(scene_practice(g_sess.lesson, g_sess.adv)); return;
    case SESS_MISSION: { extern void mission_restart(void); mission_restart(); return; }
    default: {
        int first = g_app.settings.random_maps ? -1 : (g_sess.nplayed ? g_sess.played[0] : -1);
        PlayerSetup pl[MAX_TANKS];
        memcpy(pl, g_cfg_players, sizeof pl);
        begin_match(pl, g_cfg_n, g_cfg_mode, g_cfg_diff, g_cfg_target, g_cfg_label, g_cfg_fx, g_cfg_special, g_cfg_pick, g_cfg_bless, first, g_cfg_wind, false);
    }
    }
}

/* ================================================================== RulesScene (l.10531) */
enum { RK_FMT, RK_N, RK_NCUST, RK_HP, RK_HPCUST, RK_MAP, RK_NEXT, RK_WIND, RK_AMMO, RK_BLESS, RK_SPECIAL, RK_GO };
static const char *const RK_NAMES[12] = {"FORMAT", "ROUNDS", "  CUSTOM ROUNDS", "HEALTH", "  CUSTOM HEALTH", "BATTLEFIELD", "LATER ROUNDS",
                                         "WIND", "SPECIAL AMMO", "MARTYR'S BLESSING", "SPECIAL ATTACK", "CONTINUE"};
static const int ROUND_PRESETS_[5] = {3, 5, 7, 10, 0}, HP_PRESETS_[6] = {1, 3, 5, 7, 10, 0};

typedef struct {
    Scene base;
    int kind, row;
    Rules r;
    int16_t maps[N_MAPS_ALL + 1];
    int nmaps;
    MapPreview mp;
} RulesScn;

static int rules_keys(const RulesScn *s, int *k) {
    int n = 0;
    k[n++] = RK_FMT; k[n++] = RK_N;
    if (ROUND_PRESETS_[s->r.n_idx] == 0) k[n++] = RK_NCUST;
    k[n++] = RK_HP;
    if (HP_PRESETS_[s->r.hp_idx] == 0) k[n++] = RK_HPCUST;
    k[n++] = RK_MAP; k[n++] = RK_NEXT; k[n++] = RK_WIND; k[n++] = RK_AMMO; k[n++] = RK_BLESS; k[n++] = RK_SPECIAL; k[n++] = RK_GO;
    return n;
}
static int rules_map_index(const RulesScn *s) {
    for (int i = 0; i < s->nmaps; i++) if (s->maps[i] == s->r.map) return i + 1;
    return 0;
}

static void lan_host_with_rules(const Rules *r);
static void rules_done(RulesScn *s) {
    g_rules_last = s->r;
    if (s->kind == RK_SINGLE) app_goto(scene_setup(SM_SINGLE, 0, &s->r, NULL, 1));
    else if (s->kind == RK_TWO) app_goto(scene_setup(SM_TWO, 0, &s->r, NULL, 2));
    else lan_host_with_rules(&s->r);
}

#define HOT_RULES 400
static void rules_handle(Scene *sc, const IEv *e) {
    RulesScn *s = (RulesScn *)sc;
    int keys[12], nk = rules_keys(s, keys);
    if (s->row >= nk) s->row = nk - 1;
    Rules *r = &s->r;
    if (e->action == A_BACK) { app_goto(s->kind == RK_LAN ? scene_lan_menu() : scene_menu()); return; }
    int d = 0;
    if (e->action == A_UP) s->row = (s->row + nk - 1) % nk;
    else if (e->action == A_DOWN) s->row = (s->row + 1) % nk;
    else if (e->action == A_LEFT) d = -1;
    else if (e->action == A_RIGHT) d = 1;
    else if (e->action == A_TOUCH && e->raw >= HOT_RULES && e->raw < HOT_RULES + nk) {
        int row = e->raw - HOT_RULES;
        if (row == s->row) { if (keys[row] == RK_GO) goto confirm; d = 1; }
        s->row = row;
    } else if (ev_confirm(e)) {
    confirm:
        r->blessing = r->blessing && save_blessing_unlocked(&g_app.save);
        g_app.save.rules[s->kind] = *r;
        g_app.save.has_rules[s->kind] = true;
        app_flush();
        rules_done(s);
        return;
    }
    if (!d) return;
    int step = d * (e->coarse ? 5 : 1);
    switch (keys[s->row]) {
    case RK_FMT: r->best = !r->best; break;
    case RK_N: r->n_idx = (uint8_t)((r->n_idx + d + 5) % 5); break;
    case RK_HP: r->hp_idx = (uint8_t)((r->hp_idx + d + 6) % 6); break;
    case RK_NCUST: r->n_custom = (uint8_t)clampf((float)(r->n_custom + step), 1, 25); break;
    case RK_HPCUST: r->hp_custom = (uint8_t)clampf((float)(r->hp_custom + step), 1, 50); break;
    case RK_MAP: {
        int n = s->nmaps + 1;
        int i = ((rules_map_index(s) + step) % n + n) % n;
        r->map = i == 0 ? -1 : s->maps[i - 1];
        break;
    }
    case RK_NEXT: r->pick_maps = !r->pick_maps; break;
    case RK_WIND: r->wind = !r->wind; break;
    case RK_AMMO: r->ammo_fx = !r->ammo_fx; break;
    case RK_BLESS: r->blessing = !r->blessing && save_blessing_unlocked(&g_app.save); break;
    case RK_SPECIAL: r->special = !r->special; break;
    }
}

static void rules_update(Scene *sc, float dt) { backdrop_scene_update(sc, dt); }

static void rules_draw(Scene *sc, Term *t) {
    RulesScn *s = (RulesScn *)sc;
    backdrop_scene_draw(sc, t);
    Rules *r = &s->r;
    float tm = (float)g_app.time;
    int keys[12], nk = rules_keys(s, keys);
    if (s->row >= nk) s->row = nk - 1;
    /* --- top: live terrain scan */
    int VW = 47, vx = 2;
    term_box(t, vx, 0, VW, 23, TB_DOUBLE, P_LINE, P_PANEL, "TERRAIN SCAN", P_PRIMARY);
    int mi = rules_map_index(s);
    int m = mi ? s->maps[mi - 1] : (int)(tm / 1.2f) % N_MAPS_LIB;
    int designs[2];
    RGB cols[2];
    for (int i = 0; i < 2; i++) {
        Loadout *lo = save_loadout(&g_app.save, i);
        designs[i] = lo->tank;
        cols[i] = paint_rgb(lo->tank_color);
    }
    term_blit(t, mappreview_render(&s->mp, m, VW - 4, 24, tm, designs, cols), vx + 2, 2, 0, 0);
    char buf[96];
    if (mi) snprintf(buf, sizeof buf, "%03d/%d  %s", mi, s->nmaps, MAPDEFS[m].name);
    else snprintf(buf, sizeof buf, "RANDOM");
    term_center(t, 14, buf, (mi && MAPDEFS[m].special) ? MAPDEFS[m].accent : P_AMBER, &P_PANEL, vx, VW);
    int n = term_wrap_center(t, 15, mi ? MAPDEFS[m].desc : "A fresh battlefield every round.", VW - 4, P_TEXT, &P_PANEL, vx, VW, 3);
    if (mi) {
        char a[24], b[24];
        dots(MAPDEFS[m].cover, a, sizeof a);
        dots(MAPDEFS[m].relief, b, sizeof b);
        snprintf(buf, sizeof buf, "COVER %s     RELIEF %s", a, b);
        term_center(t, 15 + n + 1, buf, P_PRIMARY, &P_PANEL, vx, VW);
    }
    term_center(t, 21, "Opening map only - later rounds rotate", P_MUTED, &P_PANEL, vx, VW);
    /* --- bottom: the rules */
    const char *title = s->kind == RK_SINGLE ? "SINGLE PLAYER RULES" : s->kind == RK_TWO ? "TWO PLAYER RULES" : "LAN MATCH RULES";
    int PW = 51, x = 0, y = T_SPLIT;
    term_box(t, x, y, PW, T_SPLIT, TB_DOUBLE, P_LINE, P_PANEL, title, P_PRIMARY);
    for (int i = 0; i < nk; i++) {
        int ry = y + 1 + i;
        bool sel = i == s->row;
        RGB bg = sel ? P_PANEL_HI : P_PANEL;
        term_fill(t, x + 1, ry, PW - 2, 1, bg);
        term_text(t, x + 2, ry, RK_NAMES[keys[i]], sel ? P_WHITE : P_MUTED, &bg);
        char val[64];
        switch (keys[i]) {
        case RK_FMT: snprintf(val, sizeof val, "%s", r->best ? "BEST OF" : "FIRST TO"); break;
        case RK_N: if (ROUND_PRESETS_[r->n_idx]) snprintf(val, sizeof val, "%d", rules_n(r)); else snprintf(val, sizeof val, "CUSTOM"); break;
        case RK_NCUST: snprintf(val, sizeof val, "%d", r->n_custom); break;
        case RK_HP: if (HP_PRESETS_[r->hp_idx]) snprintf(val, sizeof val, "%d", rules_hp(r)); else snprintf(val, sizeof val, "CUSTOM"); break;
        case RK_HPCUST: snprintf(val, sizeof val, "%d", r->hp_custom); break;
        case RK_MAP: if (mi) snprintf(val, sizeof val, "%03d/%d %.14s", mi, s->nmaps, MAPDEFS[m].name); else snprintf(val, sizeof val, "RANDOM"); break;
        case RK_NEXT: snprintf(val, sizeof val, "%s", r->pick_maps ? "PICK EACH ROUND" : "RANDOM"); break;
        case RK_WIND: snprintf(val, sizeof val, "%s", r->wind ? "ON" : "OFF"); break;
        case RK_AMMO: snprintf(val, sizeof val, "%s", r->ammo_fx ? "ON" : "OFF"); break;
        case RK_BLESS: snprintf(val, sizeof val, "%s", !save_blessing_unlocked(&g_app.save) ? "LOCKED" : r->blessing ? "ON" : "OFF"); break;
        case RK_SPECIAL: snprintf(val, sizeof val, "%s", r->special ? "ON" : "OFF"); break;
        default: snprintf(val, sizeof val, "\xE2\x96\xB6"); break;
        }
        char shown[80];
        if (sel && keys[i] != RK_GO) snprintf(shown, sizeof shown, "\xE2\x97\x84 %s \xE2\x96\xBA", val);
        else snprintf(shown, sizeof shown, "%s", val);
        term_text(t, x + PW - 2 - text_len(shown), ry, shown, sel ? GOLD : P_TEXT, &bg);
        hot_add(x + 1, ry, PW - 2, 1, HOT_RULES + i);
    }
    int by = y + 1 + nk + 1;
    char lab[32];
    rules_label(r, lab, sizeof lab);
    int hp = rules_hp(r);
    snprintf(buf, sizeof buf, "%s \xC2\xB7 %d %s EACH \xC2\xB7 WIND %s", lab, hp, hp == 1 ? "HEART" : "HEARTS", r->wind ? "ON" : "OFF");
    term_center(t, by, buf, P_AMBER, &P_PANEL, x, PW);
    if (r->special) snprintf(buf, sizeof buf, "SPECIAL: X forfeits a turn; after two, next shot deals 3");
    else snprintf(buf, sizeof buf, "A side wins the match with %d round wins", rules_target(r));
    term_center(t, by + 1, buf, P_MUTED, &P_PANEL, x, PW);
    const char *tip;
    RGB tc;
    if (keys[s->row] == RK_AMMO) {
        tip = r->ammo_fx ? "ON: \xE2\x9C\xA6 ammo really works (bounce, split, cluster, gravity, wind)." : "OFF: every ammo only changes looks - no special-ammo edge.";
        tc = P_PRIMARY;
    } else if (keys[s->row] == RK_BLESS) {
        tip = !save_blessing_unlocked(&g_app.save) ? "Defeat the Martyr to inherit this ultimate ammo."
              : r->blessing ? "ON: MARTYR'S BLESSING may be loaded - one direct hit erases every foe. Separate from SPECIAL AMMO."
                            : "OFF: MARTYR'S BLESSING cannot be selected or fired in this mode.";
        tc = GOLD;
    } else {
        tip = r->ammo_fx ? "SPECIAL AMMO ON: effects active" : "SPECIAL AMMO OFF: all ammo is cosmetic";
        tc = r->ammo_fx ? P_PRIMARY : P_MUTED;
    }
    term_wrap_center(t, by + 2, tip, PW - 4, tc, &P_PANEL, x, PW, 2);
    static const Keycap K[4] = {{"\xE2\x96\xB2\xE2\x96\xBC", "ROW"}, {"\xE2\x97\x84\xE2\x96\xBA", "CHANGE"}, {"L/R", "x5"}, {"A", "OK"}};
    ui_keycaps_center(t, T_ROWS - 2, K, 4, &P_PANEL, x, PW);
}
static void rules_destroy(Scene *sc) {
    RulesScn *s = (RulesScn *)sc;
    mappreview_free(&s->mp);
    free(s);
}
static const SceneVT RULES_VT = {.handle = rules_handle, .update = rules_update, .draw = rules_draw, .destroy = rules_destroy};
Scene *scene_rules(int kind) {
    RulesScn *s = NEW(RulesScn, &RULES_VT);
    s->kind = kind;
    SaveData *sv = &g_app.save;
    if (sv->has_rules[kind]) s->r = sv->rules[kind];
    else if (kind == RK_LAN && sv->has_rules[RK_TWO]) { s->r = sv->rules[RK_TWO]; s->r.blessing = false; }
    else rules_default(&s->r);
    if (!save_blessing_unlocked(sv)) s->r.blessing = false;
    s->nmaps = unlocked_map_list(s->maps);
    if (s->r.map >= 0 && rules_map_index(s) == 0) s->r.map = -1;
    mappreview_init(&s->mp);
    return &s->base;
}

/* ================================================================== SetupScene (l.7941) */
enum { SR_TANK, SR_HULL, SR_AMMO, SR_SHELL, SR_DIFF, SR_CONFIRM };
typedef struct {
    Scene base;
    int mode, level, pages, page, row;
    bool has_rules;
    Rules rules;
    SetupDone done;
    Loadout sel[2];
    char msg[64];
    float msg_t;
    PreviewStage stage;
} Setup;

static int setup_rows(const Setup *s, int *r) {
    int n = 0;
    r[n++] = SR_TANK; r[n++] = SR_HULL; r[n++] = SR_AMMO; r[n++] = SR_SHELL;
    if (s->mode == SM_SINGLE && s->page == 0) r[n++] = SR_DIFF;
    r[n++] = SR_CONFIRM;
    return n;
}

static int kit_list(bool tank, int16_t *out) {
    int n = 0;
    SaveData *sv = &g_app.save;
    if (tank) {
        for (int i = 0; i < DESIGNS_SECRET_FIRST; i++) out[n++] = (int16_t)i;
        for (int i = DESIGNS_ANCIENT_FIRST; i < DESIGNS_ANCIENT_FIRST + DESIGNS_ANCIENT_COUNT; i++) out[n++] = (int16_t)i;
        for (int k = 0; k < CUSTOM_TANK_MAX; k++) if (sv->custom[k].used) out[n++] = (int16_t)(DESIGN_CUSTOM_FIRST + k);
        for (int i = DESIGNS_SECRET_FIRST; i < DESIGNS_SECRET_FIRST + DESIGNS_SECRET_COUNT; i++) if (save_tank_unlocked(sv, i)) out[n++] = (int16_t)i;
    } else {
        for (int i = 0; i < AMMOS_SECRET_FIRST; i++) out[n++] = (int16_t)i;
        for (int i = AMMOS_ANCIENT_FIRST; i < AMMOS_ANCIENT_FIRST + AMMOS_ANCIENT_COUNT; i++) out[n++] = (int16_t)i;
        for (int i = AMMOS_SECRET_FIRST; i < AMMOS_SECRET_FIRST + AMMOS_SECRET_COUNT; i++) if (save_ammo_unlocked(sv, i)) out[n++] = (int16_t)i;
    }
    return n;
}

static void cycle_kit(Setup *s, bool tank, int d) {
    int16_t items[N_DESIGNS_RT + N_AMMOS];
    int n = kit_list(tank, items);
    Loadout *lo = &s->sel[s->page];
    int cur = tank ? lo->tank : lo->ammo, i = 0;
    for (int k = 0; k < n; k++) if (items[k] == cur) i = k;
    int v = items[((i + d) % n + n) % n];
    if (tank) lo->tank = (int16_t)v;
    else lo->ammo = (int16_t)v;
}

static int color_names(bool hull, int *out) {
    int n = 0;
    for (int i = 0; i < N_COLORS; i++) out[n++] = i;
    for (int p = PAINT_RGB; p <= PAINT_VOID; p++) if (save_paint_ok(&g_app.save, p, hull)) out[n++] = p;
    return n;
}

static void cycle_color(Setup *s, bool hull, int d) {
    int names[N_PAINTS];
    int n = color_names(hull, names);
    Loadout *lo = &s->sel[s->page];
    int cur = hull ? lo->tank_color : lo->shot_color, i = 0;
    for (int k = 0; k < n; k++) if (names[k] == cur) i = k;
    int other = (hull && s->page == 1) ? s->sel[0].tank_color : -1;
    for (int k = 0; k < n; k++) {
        i = ((i + d) % n + n) % n;
        if (names[i] != other) break;
    }
    if (hull) lo->tank_color = (uint8_t)names[i];
    else lo->shot_color = (uint8_t)names[i];
}

static const char *setup_locked(const Loadout *lo, char *buf, int n) {
    if (!save_tank_unlocked(&g_app.save, lo->tank)) return unlock_hint(true, lo->tank, buf, n);
    if (!save_ammo_unlocked(&g_app.save, lo->ammo)) return unlock_hint(false, lo->ammo, buf, n);
    return NULL;
}

static void setup_finish(Setup *s) {
    for (int i = 0; i < s->pages; i++) g_app.save.loadouts[i] = s->sel[i];
    app_flush();
    if (s->mode == SM_LAN || s->mode == SM_MISSION) {
        if (s->done) s->done(s->sel, s->pages);
        return;
    }
    Rules r = s->rules;
    if (!s->has_rules) rules_default(&r);
    PlayerSetup mk[2];
    for (int i = 0; i < 2; i++) {
        const char *name = s->mode == SM_TWO ? (i ? "PLAYER 2" : "PLAYER 1") : "PLAYER";
        const char *label = s->mode == SM_TWO ? (i ? "P2" : "P1") : "YOU";
        int hp = (s->mode == SM_TOURNAMENT && i == 0) ? (TOURNAMENT[s->level - 1].rule == RULE_GLASS ? 2 : MAX_HP) : rules_hp(&r);
        player_setup_init(&mk[i], name, label, paint_rgb(s->sel[i].tank_color), paint_rgb(s->sel[i].shot_color), false, s->sel[i].tank, s->sel[i].ammo, hp);
    }
    int pcol = s->sel[0].tank_color < N_COLORS ? s->sel[0].tank_color : -1;
    if (s->mode == SM_TOURNAMENT) {
        start_tournament(s->level, &mk[0]);
        return;
    }
    char label[32];
    rules_label(&r, label, sizeof label);
    bool bless = r.blessing && save_blessing_unlocked(&g_app.save);
    double wind = rules_wind_value(&r, &g_app.settings);
    if (s->mode == SM_SINGLE) {
        int diff = g_app.settings.difficulty;
        int16_t pool[N_AMMOS];
        int np = 0;
        if (r.ammo_fx && mk[0].ammo != AMMO_STANDARD) {       /* you chose a special ammo: the CPU fights with special ammo too */
            for (int a = 0; a < N_AMMOS; a++) {
                if (a == AMMO_BLESSING || a == AMMO_STANDARD) continue;
                if (a >= AMMOS_ANCIENT_FIRST && a < AMMOS_ANCIENT_FIRST + AMMOS_ANCIENT_COUNT) continue;
                if (a >= AMMOS_TICKET_FIRST && a < AMMOS_TICKET_FIRST + AMMOS_TICKET_COUNT) continue;
                if (a >= AMMOS_SECRET_FIRST && a < AMMOS_SECRET_FIRST + AMMOS_SECRET_COUNT) continue;
                if (a >= AMMOS_MAGE_FIRST) continue;
                if (save_ammo_unlocked(&g_app.save, a)) pool[np++] = (int16_t)a;
            }
            /* sorted by key, as the original sorts the keys */
            for (int i = 1; i < np; i++)
                for (int j = i; j > 0 && strcmp(AMMOS[pool[j - 1]].key, AMMOS[pool[j]].key) > 0; j--) { int16_t x = pool[j]; pool[j] = pool[j - 1]; pool[j - 1] = x; }
        }
        PlayerSetup players[2] = {mk[0], pick_ai_kit(&g_app.save, &g_app.rng, diff, pcol, pool, np)};
        players[1].hp = rules_hp(&r);
        begin_match(players, 2, MODE_SINGLE, diff, rules_target(&r), label, r.ammo_fx, r.special, r.pick_maps, bless, r.map, wind, true);
        return;
    }
    begin_match(mk, 2, MODE_TWO, g_app.settings.difficulty, rules_target(&r), label, r.ammo_fx, r.special, r.pick_maps, bless, r.map, wind, true);
}

#define HOT_SETUP 500
static void setup_handle(Scene *sc, const IEv *e) {
    Setup *s = (Setup *)sc;
    int rows[6], nr = setup_rows(s, rows);
    if (s->row >= nr) s->row = nr - 1;
    if (e->action == A_BACK) {
        if (s->page > 0) { s->page--; s->row = 0; }
        else if (s->mode == SM_TOURNAMENT) app_goto(scene_tournament(s->level));
        else if (s->mode == SM_SINGLE) app_goto(scene_rules(RK_SINGLE));
        else if (s->mode == SM_TWO) app_goto(scene_rules(RK_TWO));
        else if (s->mode == SM_LAN) app_goto(scene_lan_menu());
        else app_goto(scene_missions(0));
        return;
    }
    int d = 0;
    bool confirm = false;
    if (e->action == A_UP) s->row = (s->row + nr - 1) % nr;
    else if (e->action == A_DOWN) s->row = (s->row + 1) % nr;
    else if (e->action == A_LEFT) d = -1;
    else if (e->action == A_RIGHT) d = 1;
    else if (e->action == A_TOUCH && e->raw >= HOT_SETUP && e->raw < HOT_SETUP + nr) {
        int row = e->raw - HOT_SETUP;
        if (row == s->row) { if (rows[row] == SR_CONFIRM) confirm = true; else d = 1; }
        s->row = row;
    } else if (e->action == A_TOUCH && (e->raw == 590 || e->raw == 591)) {
        d = e->raw == 590 ? -1 : 1;
    } else if (e->action == A_TOUCH && e->raw >= 900 && e->raw < 900 + N_COLORS) {
        int k = e->raw - 900;
        if (rows[s->row] == SR_HULL && !(s->page == 1 && k == s->sel[0].tank_color)) s->sel[s->page].tank_color = (uint8_t)k;
        if (rows[s->row] == SR_SHELL) s->sel[s->page].shot_color = (uint8_t)k;
    } else if (ev_confirm(e)) {
        if (rows[s->row] == SR_CONFIRM || e->action == A_CONFIRM) confirm = true;
        else s->row = (s->row + 1) % nr;
    }
    if (d) {
        int name = rows[s->row];
        if ((name == SR_TANK || name == SR_AMMO) && e->coarse) d *= 5;
        if (name == SR_TANK) cycle_kit(s, true, d);
        else if (name == SR_AMMO) cycle_kit(s, false, d);
        else if (name == SR_HULL) cycle_color(s, true, d);
        else if (name == SR_SHELL) cycle_color(s, false, d);
        else if (name == SR_DIFF) {
            int n = save_master_unlocked(&g_app.save) ? 4 : 3;
            g_app.settings.difficulty = ((g_app.settings.difficulty + d) % n + n) % n;
            app_apply_settings();
        }
    }
    if (confirm) {
        char buf[64];
        const char *lock = setup_locked(&s->sel[s->page], buf, sizeof buf);
        if (lock) {
            snprintf(s->msg, sizeof s->msg, "%s", lock);
            s->msg_t = 2.5f;
            s->row = 0;
        } else if (s->page + 1 < s->pages) {
            s->page++;
            s->row = 0;
        } else {
            setup_finish(s);
        }
    }
}

static void setup_update(Scene *sc, float dt) {
    Setup *s = (Setup *)sc;
    backdrop_scene_update(sc, dt);
    preview_update(&s->stage, dt);
    s->msg_t = fmaxf(0, s->msg_t - dt);
}

static void setup_draw(Scene *sc, Term *t) {
    Setup *s = (Setup *)sc;
    backdrop_scene_draw(sc, t);
    SaveData *sv = &g_app.save;
    float tm = (float)g_app.time;
    Loadout *lo = &s->sel[s->page];
    const TankDesign *design = design_ptr(lo->tank);
    const AmmoType *ammo = &AMMOS[lo->ammo];
    char buf[96], lockb[64];
    /* ---- top screen: LIVE PREVIEW */
    int px = 1, PW = 49;
    term_box(t, px, 0, PW, 23, TB_DOUBLE, P_LINE, P_PANEL, "LIVE PREVIEW", P_PRIMARY);
    const char *lock = setup_locked(lo, lockb, sizeof lockb);
    preview_set_kit(&s->stage, lo->tank, paint_rgb(lo->tank_color), lo->ammo, paint_rgb(lo->shot_color), lock ? 0.45f : 0.0f);
    term_blit(t, preview_render(&s->stage), px + 1, 1, 0, 0);
    snprintf(buf, sizeof buf, "%s  +  %s", design->name, ammo->name);
    term_center(t, 15, buf, P_TEXT, &P_PANEL, px, PW);
    snprintf(buf, sizeof buf, "%s HULL  /  %s SHELL", paint_name(lo->tank_color), paint_name(lo->shot_color));
    term_center(t, 16, buf, P_MUTED, &P_PANEL, px, PW);
    if (s->mode == SM_TOURNAMENT) {
        const TournLevel *lv = &TOURNAMENT[s->level - 1];
        static const char *const DN[4] = {"EASY", "NORMAL", "HARD", "MASTER"};
        term_center(t, 17, "SPECIAL AMMO IS OFF IN TOURNAMENT", P_MUTED, &P_PANEL, px, PW);
        snprintf(buf, sizeof buf, "VS %s \xC2\xB7 %s AI", lv->boss, DN[lv->difficulty]);
        term_center(t, 18, buf, lv->difficulty == DIFF_MASTER ? GOLD : P_AMBER, &P_PANEL, px, PW);
        level_objective(s->level, buf, sizeof buf);
        term_center(t, 19, buf, P_TEXT, &P_PANEL, px, PW);
        term_center(t, 20, level_rule_text(s->level), lv->rule ? GOLD : P_MUTED, &P_PANEL, px, PW);
    } else {
        const char *n1, *n2;
        RGB nc;
        if (s->mode == SM_MISSION) { n1 = "SPECIAL AMMO IS OFF IN MISSIONS"; n2 = "Every kit is cosmetic: all ammo plays alike."; nc = P_MUTED; }
        else if (s->mode == SM_LAN && !s->has_rules) { n1 = "\xE2\x9C\xA6 = ammo with a special effect"; n2 = "LAN: the host's SPECIAL AMMO rule decides."; nc = P_AMBER; }
        else if (s->has_rules && !s->rules.ammo_fx) { n1 = "SPECIAL AMMO IS OFF (match rule)"; n2 = "\xE2\x9C\xA6 ammo plays like plain ammo this match."; nc = P_MUTED; }
        else { n1 = "SPECIAL AMMO IS ON  \xE2\x9C\xA6 = has an effect"; n2 = "Bounce, split, cluster... see HOW TO PLAY."; nc = P_AMBER; }
        term_center(t, 18, n1, nc, &P_PANEL, px, PW);
        term_center(t, 19, n2, nc, &P_PANEL, px, PW);
        if (s->has_rules && s->rules.special) {
            term_center(t, 20, "SPECIAL ATTACK: X skips a turn; after two,", GOLD, &P_PANEL, px, PW);
            term_center(t, 21, "your next shot deals 3 damage.", GOLD, &P_PANEL, px, PW);
        }
    }
    /* ---- bottom screen: the rows */
    const char *title = (s->mode == SM_SINGLE || s->mode == SM_LAN || (s->mode == SM_MISSION && s->pages == 1)) ? "PILOT SETUP" : NULL;
    char tb[48];
    if (!title) {
        if (s->mode == SM_TWO || s->mode == SM_MISSION) snprintf(tb, sizeof tb, "PLAYER %d SETUP", s->page + 1);
        else snprintf(tb, sizeof tb, "LEVEL %d LOADOUT", s->level);
        title = tb;
    }
    int x = 0, y = T_SPLIT, W = 51;
    term_box(t, x, y, W, T_SPLIT, TB_DOUBLE, P_LINE, P_PANEL, title, P_PRIMARY);
    int rows[6], nr = setup_rows(s, rows);
    if (s->row >= nr) s->row = nr - 1;
    int other = s->page == 1 ? s->sel[0].tank_color : -1;
    int ry = y + 1;
    for (int i = 0; i < nr; i++) {
        bool sel = i == s->row;
        RGB bg = sel ? P_PANEL_HI : P_PANEL;
        int name = rows[i];
        static const char *const NM[6] = {"TANK", "HULL COLOR", "AMMO", "SHELL COLOR", "AI DIFFICULTY", "CONFIRM"};
        term_fill(t, x + 1, ry, W - 2, 1, bg);
        if (name != SR_CONFIRM) term_text(t, x + 2, ry, NM[name], sel ? P_WHITE : P_MUTED, &bg);
        hot_add(x + 1, ry, W - 2, 1, HOT_SETUP + i);
        if (name == SR_TANK || name == SR_AMMO) {
            bool tank = name == SR_TANK;
            int16_t items[N_DESIGNS_RT + N_AMMOS];
            int n = kit_list(tank, items), pos = 1;
            int cur = tank ? lo->tank : lo->ammo;
            for (int k = 0; k < n; k++) if (items[k] == cur) pos = k + 1;
            bool bad = tank ? !save_tank_unlocked(sv, cur) : !save_ammo_unlocked(sv, cur);
            bool mech = !tank && AMMOS[cur].mf != 0;
            const char *kname = tank ? design->name : ammo->name;
            char lab[64], shown[80];
            snprintf(lab, sizeof lab, "%d/%d %s%s%s", pos, n, kname, mech ? " \xE2\x9C\xA6" : "", bad ? " [LOCKED]" : "");
            if (sel) snprintf(shown, sizeof shown, "\xE2\x97\x84 %s \xE2\x96\xBA", lab);
            else snprintf(shown, sizeof shown, "%s", lab);
            term_text(t, x + W - 2 - text_len(shown), ry, shown, bad ? P_DANGER : P_AMBER, &bg);
            const char *hint = bad ? unlock_hint(tank, cur, lockb, sizeof lockb) : (tank ? design->blurb : ammo->blurb);
            char lines[2][64];
            int k2 = wrap_text(hint, 47, lines, 2);
            for (int k = 0; k < k2; k++) term_text(t, x + 2, ry + 1 + k, lines[k], bad ? P_DANGER : P_MUTED, &P_PANEL);
            ry += 3;
        } else if (name == SR_HULL || name == SR_SHELL) {
            bool hull = name == SR_HULL;
            int cur = hull ? lo->tank_color : lo->shot_color;
            char shown[48];
            if (sel) snprintf(shown, sizeof shown, "\xE2\x97\x84 %s \xE2\x96\xBA", paint_name(cur));
            else snprintf(shown, sizeof shown, "%s", paint_name(cur));
            term_text(t, x + W - 2 - text_len(shown), ry, shown, live_color(paint_rgb(cur), tm), &bg);
            if (sel)
                ui_swatches(t, x + 2, ry + 1, cur < N_COLORS ? cur : -1, hull ? other : -1, save_paint_ok(sv, PAINT_RGB, hull), cur == PAINT_RGB, tm,
                            save_paint_ok(sv, PAINT_ASTRAL, hull), cur == PAINT_ASTRAL, cur, save_paint_ok(sv, PAINT_PRISMATIC, hull), save_paint_ok(sv, PAINT_VOID, hull));
            ry += sel ? 4 : 1;
        } else if (name == SR_DIFF) {
            static const char *const DN[4] = {"EASY", "NORMAL", "HARD", "MASTER"};
            const char *dn = DN[g_app.settings.difficulty];
            char shown[32];
            if (sel) snprintf(shown, sizeof shown, "\xE2\x97\x84 %s \xE2\x96\xBA", dn);
            else snprintf(shown, sizeof shown, "%s", dn);
            term_text(t, x + W - 2 - text_len(shown), ry, shown, g_app.settings.difficulty == DIFF_MASTER ? GOLD : P_AMBER, &bg);
            ry += 2;
        } else {
            bool last = s->page + 1 == s->pages;
            const char *text = last ? (s->mode == SM_TOURNAMENT ? "[ START RUN ]" : "[ READY ]") : "[ NEXT PLAYER ]";
            term_center(t, ry, text, sel ? P_PRIMARY : P_MUTED, &bg, x + 1, W - 2);
            ry += 1;
        }
    }
    if (s->msg_t > 0) term_wrap_center(t, T_ROWS - 4, s->msg, W - 4, P_DANGER, &P_PANEL, x, W, 2);
    ui_button(t, x + 2, T_ROWS - 2, "\xE2\x97\x84", P_PRIMARY, P_PANEL_HI, 590);
    ui_button(t, x + W - 7, T_ROWS - 2, "\xE2\x96\xBA", P_PRIMARY, P_PANEL_HI, 591);
    static const Keycap K[4] = {{"\xE2\x96\xB2\xE2\x96\xBC", "ROW"}, {"\xE2\x97\x84\xE2\x96\xBA", "PICK"}, {"L/R", "x5"}, {"A", "OK"}};
    ui_keycaps_center(t, T_ROWS - 2, K, 4, &P_PANEL, x + 7, W - 14);
}

static void setup_destroy(Scene *sc) {
    Setup *s = (Setup *)sc;
    preview_free(&s->stage);
    free(s);
}
static const SceneVT SETUP_VT = {.handle = setup_handle, .update = setup_update, .draw = setup_draw, .destroy = setup_destroy};
Scene *scene_setup(int mode, int level, const Rules *rules, SetupDone done, int pages) {
    Setup *s = NEW(Setup, &SETUP_VT);
    s->mode = mode;
    s->level = level;
    s->has_rules = rules != NULL;
    if (rules) s->rules = *rules;
    s->done = done;
    s->pages = pages > 0 ? pages : (mode == SM_TWO ? 2 : 1);
    for (int i = 0; i < 2; i++) s->sel[i] = *save_loadout(&g_app.save, i);
    preview_init(&s->stage, 46, 26, 2, 3);
    return &s->base;
}

/* ================================================================== RoundMapScene (l.10280) + wait (l.10344) */
typedef struct { Scene base; int picker, index; int16_t maps[N_MAPS_ALL]; int nmaps; MapPreview mp; } RoundMap;
#define VISIBLE 17
#define HOT_RMAP 600
static void rmap_pick(RoundMap *s) {
    int m = s->index ? s->maps[s->index - 1] : -1;
    if (g_sess.net) {
        uint8_t b[2] = {(uint8_t)(m & 255), (uint8_t)((m >> 8) & 255)};
        net_send(M_MAP, b, 2);
    }
    g_sess.pending_map = m;
    session_advance(&g_sess);
    app_goto(scene_battle());
}
static void rmap_handle(Scene *sc, const IEv *e) {
    RoundMap *s = (RoundMap *)sc;
    int n = s->nmaps + 1;
    if (e->action == A_UP) s->index = (s->index + n - 1) % n;
    else if (e->action == A_DOWN) s->index = (s->index + 1) % n;
    else if (e->action == A_LEFT) s->index = s->index - 8 > 0 ? s->index - 8 : 0;
    else if (e->action == A_RIGHT) s->index = s->index + 8 < n - 1 ? s->index + 8 : n - 1;
    else if (e->action == A_TOUCH && e->raw >= HOT_RMAP && e->raw < HOT_RMAP + n) {
        int i = e->raw - HOT_RMAP;
        if (i == s->index) rmap_pick(s);
        else s->index = i;
    } else if (ev_confirm(e)) rmap_pick(s);
}
static void rmap_update(Scene *sc, float dt) {
    backdrop_scene_update(sc, dt);
    if (g_sess.net) net_alive_or_leave();
}
static void draw_map_list(Term *t, int x, int y, int w, int index, const int16_t *maps, int n, int hot, bool three_digit) {
    int top = (int)clampf((float)(index - VISIBLE / 2), 0, (float)(n - VISIBLE > 0 ? n - VISIBLE : 0));
    for (int row = 0; row < VISIBLE && row < n; row++) {
        int i = top + row;
        bool sel = i == index;
        RGB bg = sel ? P_PANEL_HI : P_PANEL;
        char name[64];
        int m = i ? maps[i - 1] : -1;
        if (!i) snprintf(name, sizeof name, "RANDOM");
        else if (!MAPDEFS[m].special) snprintf(name, sizeof name, three_digit ? "%03d %s" : "%02d %s", i, MAPDEFS[m].name);
        else if (three_digit) snprintf(name, sizeof name, "%03d \xE2\x98\x85 %s", i, MAPDEFS[m].name);
        else snprintf(name, sizeof name, "\xE2\x98\x85  %s", MAPDEFS[m].name);
        RGB col = sel ? P_WHITE : (m >= 0 && MAPDEFS[m].special ? MAPDEFS[m].accent : (!i ? P_AMBER : P_MUTED));
        char line[80];
        snprintf(line, sizeof line, "%s%.*s", sel ? "\xE2\x96\xB6 " : "  ", w - 5, name);
        term_fill(t, x, y + row, w, 1, bg);
        term_text(t, x + 1, y + row, line, col, &bg);
        hot_add(x, y + row, w, 1, hot + i);
    }
    if (top > 0) term_put(t, x + w - 1, y, 0x25B2, P_PRIMARY_DIM, &P_PANEL);
    if (top + VISIBLE < n) term_put(t, x + w - 1, y + VISIBLE - 1, 0x25BC, P_PRIMARY_DIM, &P_PANEL);
}
static void map_scan_panel(Term *t, MapPreview *mp, int idx, int m, int count, const int *designs, const RGB *cols) {
    float tm = (float)g_app.time;
    int VW = 49, vx = 1;
    term_box(t, vx, 0, VW, 23, TB_DOUBLE, P_LINE, P_PANEL, "TERRAIN SCAN", P_PRIMARY);
    int show = idx ? m : (int)(tm / 1.2f) % N_MAPS_LIB;
    term_blit(t, mappreview_render(mp, show, 46, 24, tm, designs, cols), vx + 1, 2, 0, 0);
    char buf[96];
    if (idx) snprintf(buf, sizeof buf, "%03d/%d  %s", idx, count, MAPDEFS[show].name);
    else snprintf(buf, sizeof buf, "RANDOM");
    term_center(t, 15, buf, (idx && MAPDEFS[show].special) ? MAPDEFS[show].accent : P_AMBER, &P_PANEL, vx, VW);
    term_wrap_center(t, 16, idx ? MAPDEFS[show].desc : "A fresh battlefield.", VW - 4, P_TEXT, &P_PANEL, vx, VW, 3);
    if (idx) {
        char a[24], b[24];
        dots(MAPDEFS[show].cover, a, sizeof a);
        dots(MAPDEFS[show].relief, b, sizeof b);
        snprintf(buf, sizeof buf, "COVER %s     RELIEF %s", a, b);
        term_center(t, 20, buf, P_PRIMARY, &P_PANEL, vx, VW);
    }
}
static void rmap_draw(Scene *sc, Term *t) {
    RoundMap *s = (RoundMap *)sc;
    backdrop_scene_draw(sc, t);
    int designs[2];
    RGB cols[2];
    for (int i = 0; i < 2; i++) { designs[i] = g_sess.players[i].design; cols[i] = g_sess.players[i].tank_color; }
    map_scan_panel(t, &s->mp, s->index, s->index ? s->maps[s->index - 1] : -1, s->nmaps, designs, cols);
    char who[64], buf[96];
    if (g_sess.mode != MODE_SINGLE) session_team_name(&g_sess, s->picker, who, sizeof who);
    else snprintf(who, sizeof who, "YOU");
    int x = 0, y = T_SPLIT, W = 51;
    term_box(t, x, y, W, T_SPLIT, TB_DOUBLE, P_LINE, P_PANEL, "NEXT BATTLEFIELD", P_PRIMARY);
    snprintf(buf, sizeof buf, "%.20s PICK%s", who, strcmp(who, "YOU") ? "S" : "");
    term_center(t, y + 1, buf, P_AMBER, &P_PANEL, x, W);
    draw_map_list(t, x + 2, y + 3, W - 4, s->index, s->maps, s->nmaps + 1, HOT_RMAP, true);
    snprintf(buf, sizeof buf, "%03d/%d \xC2\xB7 \xE2\x97\x84\xE2\x96\xBA PAGE \xC2\xB7 A PICK", s->index, s->nmaps);
    term_center(t, T_ROWS - 2, buf, P_MUTED, &P_PANEL, x, W);
}
static void rmap_destroy(Scene *sc) { RoundMap *s = (RoundMap *)sc; mappreview_free(&s->mp); free(s); }
static const SceneVT RMAP_VT = {.handle = rmap_handle, .update = rmap_update, .draw = rmap_draw, .destroy = rmap_destroy};
Scene *scene_round_map(int picker) {
    RoundMap *s = NEW(RoundMap, &RMAP_VT);
    s->picker = picker;
    s->nmaps = unlocked_map_list(s->maps);
    mappreview_init(&s->mp);
    return &s->base;
}

typedef struct { Scene base; int picker; } RoundWait;
static void rwait_update(Scene *sc, float dt) {
    backdrop_scene_update(sc, dt);
    if (!net_alive_or_leave()) return;
    NetMsg m;
    bool got = false;
    while (net_take(M_MAP, &m)) got = true;   /* the latest pick wins */
    if (got) {
        int v = (int16_t)(m.data[0] | (m.data[1] << 8));
        g_sess.pending_map = v >= 0 && v < N_MAPS_ALL ? v : -1;
        session_advance(&g_sess);
        app_goto(scene_battle());
    }
}
static void rwait_handle(Scene *sc, const IEv *e) { (void)sc; if (e->action == A_BACK) app_push(scene_confirm_leave()); }
static void rwait_draw(Scene *sc, Term *t) {
    RoundWait *s = (RoundWait *)sc;
    backdrop_scene_draw(sc, t);
    int x, y;
    ui_panel(t, 49, 9, "NEXT BATTLEFIELD", -1, T_SPLIT, T_SPLIT, P_LINE, &x, &y);
    char nm[64], buf[96];
    session_team_name(&g_sess, s->picker, nm, sizeof nm);
    snprintf(buf, sizeof buf, "%s IS CHOOSING THE NEXT MAP", nm);
    term_center(t, y + 3, buf, P_AMBER, &P_PANEL, x, 49);
    snprintf(buf, sizeof buf, "WAITING%.*s", 1 + (int)(g_app.time * 2) % 3, "...");
    term_center(t, y + 5, buf, P_MUTED, &P_PANEL, x, 49);
}
static const SceneVT RWAIT_VT = {.handle = rwait_handle, .update = rwait_update, .draw = rwait_draw};
Scene *scene_round_map_wait(int picker) {
    RoundWait *s = NEW(RoundWait, &RWAIT_VT);
    s->picker = picker;
    return &s->base;
}

/* MapSelectScene (l.8181) is only reached when Settings MAP SELECTION = CHOOSE outside the rules screens; in v1.5.0 the
 * rules screen already picks the map, so it is kept for parity with the same list UI. */
typedef struct { Scene base; int index; int16_t maps[N_MAPS_ALL]; int nmaps; MapPreview mp; } MapSel;
static void msel_handle(Scene *sc, const IEv *e) {
    MapSel *s = (MapSel *)sc;
    int n = s->nmaps + 1;
    if (e->action == A_BACK) { app_goto(scene_menu()); return; }
    if (e->action == A_UP) s->index = (s->index + n - 1) % n;
    else if (e->action == A_DOWN) s->index = (s->index + 1) % n;
    else if (e->action == A_LEFT) s->index = s->index - 8 > 0 ? s->index - 8 : 0;
    else if (e->action == A_RIGHT) s->index = s->index + 8 < n - 1 ? s->index + 8 : n - 1;
    else if (e->action == A_TOUCH && e->raw >= HOT_RMAP && e->raw < HOT_RMAP + n) s->index = e->raw - HOT_RMAP;
    else if (ev_confirm(e)) {
        g_sess.current_map = s->index ? s->maps[s->index - 1] : g_sess.current_map;
        start_match_session(true);
    }
}
static void msel_draw(Scene *sc, Term *t) {
    MapSel *s = (MapSel *)sc;
    backdrop_scene_draw(sc, t);
    int designs[2];
    RGB cols[2];
    for (int i = 0; i < 2; i++) { designs[i] = g_sess.players[i].design; cols[i] = g_sess.players[i].tank_color; }
    map_scan_panel(t, &s->mp, s->index, s->index ? s->maps[s->index - 1] : -1, s->nmaps, designs, cols);
    term_box(t, 0, T_SPLIT, 51, T_SPLIT, TB_DOUBLE, P_LINE, P_PANEL, "BATTLEFIELD", P_PRIMARY);
    draw_map_list(t, 2, T_SPLIT + 2, 47, s->index, s->maps, s->nmaps + 1, HOT_RMAP, false);
    char buf[64];
    snprintf(buf, sizeof buf, "%d/%d \xC2\xB7 \xE2\x97\x84\xE2\x96\xBA PAGE", s->index, s->nmaps);
    term_center(t, T_ROWS - 3, buf, P_MUTED, &P_PANEL, 0, 51);
    term_center(t, T_ROWS - 2, "Opening map only - later rounds rotate", P_MUTED, &P_PANEL, 0, 51);
}
static void msel_destroy(Scene *sc) { MapSel *s = (MapSel *)sc; mappreview_free(&s->mp); free(s); }
static const SceneVT MSEL_VT = {.handle = msel_handle, .update = backdrop_scene_update, .draw = msel_draw, .destroy = msel_destroy};
Scene *scene_mapselect(void) {
    MapSel *s = NEW(MapSel, &MSEL_VT);
    s->nmaps = unlocked_map_list(s->maps);
    mappreview_init(&s->mp);
    return &s->base;
}

/* ================================================================== MatchResultScene (l.13149) */
static const char *const DIGITS[4][5] = {{"███", "█ █", "█ █", "█ █", "███"}, {" █ ", "██ ", " █ ", " █ ", "███"},
                                         {"███", "  █", "███", "█  ", "███"}, {"███", "  █", "███", "  █", "███"}};
typedef struct { Scene base; } MatchRes;
static void mres_enter(Scene *sc) {
    (void)sc;
    g_app.wallet_flash = 1.6f;
    int w = session_champion(&g_sess);
    int p = session_team_player(&g_sess, w);
    const PlayerSetup *pl = &g_sess.players[p < 0 ? 0 : p];
    RGB c[3] = {pl->tank_color, pl->shot_color, P_WHITE};
    backdrop_celebrate(g_app.backdrop, c, 3);
}
static void mres_exit(Scene *sc) { (void)sc; backdrop_celebrate(g_app.backdrop, NULL, 0); }
static void mres_handle(Scene *sc, const IEv *e) {
    (void)sc;
    if (ev_confirm(e) || e->action == A_TOUCH) {
        if (g_sess.net) net_leave();
        app_goto(scene_menu());
    }
}
static void mres_draw(Scene *sc, Term *t) {
    backdrop_scene_draw(sc, t);
    Session *s = &g_sess;
    int w = session_champion(s);
    int p = session_team_player(s, w);
    const PlayerSetup *pl = &s->players[p < 0 ? 0 : p];
    int x, y;
    ui_panel(t, 46, 17, "MATCH COMPLETE", -1, T_SPLIT, T_SPLIT, live_color(pl->tank_color, (float)g_app.time), &x, &y);
    char nm[64], buf[128];
    session_team_name(s, w, nm, sizeof nm);
    snprintf(buf, sizeof buf, "%.36s WINS", nm);
    term_center(t, y + 2, buf, pl->tank_color, &P_PANEL, x, 46);
    if (s->wins[0] <= 3 && s->wins[1] <= 3) {   /* (the original only has big digits 0-3 and would crash past them) */
        for (int r = 0; r < 5; r++) {
            snprintf(buf, sizeof buf, "%s%s%s", DIGITS[s->wins[0]][r], r == 2 ? "  \xE2\x80\x94\xE2\x80\x94\xE2\x80\x94  " : "       ", DIGITS[s->wins[1]][r]);
            term_center(t, y + 4 + r, buf, P_WHITE, &P_PANEL, x, 46);
        }
    } else {
        snprintf(buf, sizeof buf, "%d   \xE2\x80\x94   %d", s->wins[0], s->wins[1]);
        term_center(t, y + 6, buf, P_WHITE, &P_PANEL, x, 46);
    }
    char label[32], head[64], cond[96];
    if (s->label[0]) snprintf(label, sizeof label, "%s", s->label);
    else snprintf(label, sizeof label, "FIRST TO %d", s->target);
    RGB hcol;
    int lw = s->wins[1 - w];
    if (s->net) {
        int mine = s->my_owner;
        bool won = w == mine;
        snprintf(head, sizeof head, "%s", won ? "VICTORY!" : "DEFEAT");
        hcol = won ? P_OK : P_DANGER;
        snprintf(cond, sizeof cond, "%s: YOU %s %d TO %d", label, won ? "WON" : "LOST", s->wins[mine], s->wins[1 - mine]);
    } else if (s->single) {
        bool won = w == 0;
        snprintf(head, sizeof head, "%s", won ? "VICTORY!" : "DEFEAT");
        hcol = won ? P_AMBER : P_DANGER;
        snprintf(cond, sizeof cond, "%s: %s %d TO %d", label, won ? "YOU WON" : "THE AI WON", s->wins[0], s->wins[1]);
    } else {
        snprintf(head, sizeof head, "%.30s WINS!", nm);
        hcol = P_AMBER;
        snprintf(cond, sizeof cond, "%s: %s WON %d TO %d", label, nm, s->wins[w], lw);
    }
    term_center(t, y + 10, head, hcol, &P_PANEL, x, 46);
    snprintf(buf, sizeof buf, "%.44s", cond);
    term_center(t, y + 11, buf, P_TEXT, &P_PANEL, x, 46);
    if (s->has_payout) {
        if (s->payout_credits) snprintf(buf, sizeof buf, "+%d \xE2\x97\x89   +%d \xE2\x97\x88", s->payout_coins, s->payout_credits);
        else snprintf(buf, sizeof buf, "+%d \xE2\x97\x89", s->payout_coins);
        term_center(t, y + 13, buf, GOLD, &P_PANEL, x, 46);
    } else if (s->mode == MODE_TWO || s->mode == MODE_LAN) {
        term_center(t, y + 13, "TWO-PLAYER AND LAN MATCHES EARN NO COINS", P_MUTED, &P_PANEL, x, 46);
    }
    if ((int)(g_app.time * 2) % 2 == 0) term_center(t, y + 15, "[ A ]", P_PRIMARY, &P_PANEL, x, 46);
    hot_add(x, y, 46, 17, 1);
}
static const SceneVT MRES_VT = {.enter = mres_enter, .exit = mres_exit, .handle = mres_handle, .update = backdrop_scene_update, .draw = mres_draw};
Scene *scene_match_result(void) { return &NEW(MatchRes, &MRES_VT)->base; }

/* ================================================================== RPSScene (l.9901) */
static const char *const RPS_NAMES[3] = {"ROCK", "PAPER", "SCISSORS"};
enum { RPS_PICK, RPS_COUNT, RPS_REVEAL, RPS_CHOOSE, RPS_WAIT };
typedef struct {
    Scene base;
    int seat;               /* -1 local */
    int picks[2];
    int stage, turn, sel, winner, choice;
    float t;
    char note[32];
    char names[2][16];
    Canvas cv;
} RPS;

static void rps_finish(RPS *s, int first_seat) {
    g_sess.first_offset = first_seat % 2;
    app_goto(scene_battle());
}
static bool beats(int a, int b) { return ((a - b) % 3 + 3) % 3 == 1; }
static void rps_both(RPS *s) {
    int a = s->picks[0], b = s->picks[1];
    s->stage = RPS_COUNT;
    s->t = 0;
    s->winner = a == b ? -1 : (beats(a, b) ? 0 : 1);
}
static void rps_update(Scene *sc, float dt) {
    RPS *s = (RPS *)sc;
    backdrop_scene_update(sc, dt);
    s->t += dt;
    if (s->seat >= 0) {
        if (!net_alive_or_leave()) return;
        int peer = 1 - s->seat;
        NetMsg *m = net_peek(NB(M_RPS));
        if (m && s->picks[peer] < 0 && m->len >= 1) {
            s->picks[peer] = m->data[0] % 3;
            net_drop(m);
            if (s->picks[s->seat] >= 0) rps_both(s);
        }
        NetMsg *f = net_peek(NB(M_RPSFIRST));
        if (f && s->stage == RPS_WAIT) {
            int p = f->len ? f->data[0] : 0;
            net_drop(f);
            rps_finish(s, p);
            return;
        }
    }
    if (s->stage == RPS_COUNT && s->t > 2.3f) { s->stage = RPS_REVEAL; s->t = 0; }
    else if (s->stage == RPS_REVEAL && s->t > 2.0f) {
        if (s->winner < 0) {
            s->picks[0] = s->picks[1] = -1;
            s->stage = RPS_PICK;
            s->turn = 0;
            snprintf(s->note, sizeof s->note, "TIE! PICK AGAIN");
        } else if (s->seat < 0) s->stage = RPS_CHOOSE;
        else s->stage = s->winner == s->seat ? RPS_CHOOSE : RPS_WAIT;
    }
}
static void rps_handle(Scene *sc, const IEv *e) {
    RPS *s = (RPS *)sc;
    if (e->action == A_BACK && s->seat < 0) { app_goto(scene_menu()); return; }
    int a = e->action;
    if (a == A_TOUCH && e->raw >= 700 && e->raw < 703 && s->stage == RPS_PICK) {
        if (s->sel == e->raw - 700) a = A_CONFIRM;
        else { s->sel = e->raw - 700; return; }
    }
    if (a == A_TOUCH && (e->raw == 710 || e->raw == 711) && s->stage == RPS_CHOOSE) {
        if (s->choice == e->raw - 710) a = A_CONFIRM;
        else { s->choice = e->raw - 710; return; }
    }
    if (a == A_LEFT || a == A_RIGHT || a == A_UP || a == A_DOWN) {
        int d = (a == A_LEFT || a == A_UP) ? -1 : 1;
        if (s->stage == RPS_PICK) s->sel = (s->sel + d + 3) % 3;
        else if (s->stage == RPS_CHOOSE) s->choice = 1 - s->choice;
    } else if (a == A_CONFIRM || a == A_FIRE) {
        if (s->stage == RPS_PICK) {
            if (s->seat >= 0) {
                if (s->picks[s->seat] < 0) {
                    s->picks[s->seat] = s->sel;
                    uint8_t c = (uint8_t)s->sel;
                    net_send(M_RPS, &c, 1);
                    if (s->picks[1 - s->seat] >= 0) rps_both(s);
                }
            } else {
                s->picks[s->turn] = s->sel;
                if (s->turn == 0) { s->turn = 1; s->sel = 0; }
                else rps_both(s);
            }
            s->note[0] = 0;
        } else if (s->stage == RPS_CHOOSE) {
            int w = s->winner;
            int first = s->choice == 0 ? w : 1 - w;
            if (s->seat >= 0) { uint8_t p = (uint8_t)first; net_send(M_RPSFIRST, &p, 1); }
            rps_finish(s, first);
        }
    }
}

/* hand sprites (l.9824-9898) */
static void hp_ell(Surf *cv, float cx, float cy, float rx, float ry, RGB col, float ysh) {
    for (int dx = -(int)rx - 1; dx < (int)rx + 2; dx++)
        for (int dy = -(int)ry - 1; dy < (int)ry + 2; dy++) {
            float a = dx / fmaxf(0.5f, rx), b = dy / fmaxf(0.5f, ry);
            if (a * a + b * b <= 1.0f) cv_plot(cv, (int)(cx + dx), (int)(cy + dy), rgb15(cshade(col, 1.0f - ysh * (0.5f - dy / (2 * fmaxf(0.5f, ry))))));
        }
}
static void hp_cap(Surf *cv, float x0, float y0, float x1, float y1, float w, RGB col, float ysh) {
    int n = (int)(fmaxf(fabsf(x1 - x0), fabsf(y1 - y0)) * 2) + 1;
    for (int i = 0; i <= n; i++) {
        float u = i / (float)n;
        hp_ell(cv, x0 + (x1 - x0) * u, y0 + (y1 - y0) * u, w / 2, w / 2, col, ysh);
    }
}
static void hand_parts(Surf *cv, int kind, float g, RGB col, float sh) {
    if (kind == 0) {
        hp_ell(cv, 16, 13, 9 + g, 7.5f + g, col, sh);
        for (int i = 0; i < 4; i++) hp_ell(cv, 9.6f + i * 4.3f, 19.2f, 2.7f + g, 3.4f + g, col, sh);
        hp_cap(cv, 7, 11, 15, 14.5f, 4.6f + 2 * g, col, sh);
    } else if (kind == 1) {
        hp_ell(cv, 16, 12, 8.2f + g, 6.6f + g, col, sh);
        static const float ANG[4] = {-13, -4, 5, 14}, LN[4] = {14, 16, 15.5f, 12};
        for (int i = 0; i < 4; i++) {
            float a = ANG[i] * 3.14159265f / 180, bx = 11 + i * 3.3f;
            hp_cap(cv, bx, 16, bx + fsin(a) * LN[i], 16 + fcos(a) * LN[i], 3.3f + 2 * g, col, sh);
        }
        hp_cap(cv, 8, 10, 2.5f, 19, 3.8f + 2 * g, col, sh);
    } else {
        hp_ell(cv, 16, 11, 8.2f + g, 6.2f + g, col, sh);
        hp_cap(cv, 12, 15, 6.5f, 31, 3.6f + 2 * g, col, sh);
        hp_cap(cv, 19, 15, 25, 31, 3.6f + 2 * g, col, sh);
        hp_ell(cv, 11.5f, 14.5f, 2.7f + g, 2.5f + g, col, sh);
        hp_ell(cv, 16.5f, 14.5f, 2.7f + g, 2.5f + g, col, sh);
        hp_cap(cv, 8, 10, 15, 13, 4.0f + 2 * g, col, sh);
    }
}
/* A hand is rendered once per (kind, skin) and cached: the soft-float ellipse rasteriser costs ~110 ms a hand on
 * the ARM9. Sleeve pixels are stored as markers 1..6 (sleeve row) and coloured at blit time, so animated paint
 * colours and mirroring stay free. */
static void hand_sprite(Surf *cv, int kind, RGB skin) {
    memset(cv->px, 0, sizeof(Px) * 32 * 36);
    RGB ol = {46, 26, 24};
    hand_parts(cv, kind, 1.0f, ol, 0.0f);
    for (int x = 8; x < 25; x++) for (int y = 0; y < 7; y++) cv_plot(cv, x, y, rgb15(ol));
    for (int x = 9; x < 24; x++) for (int y = 1; y < 6; y++) cv_plot(cv, x, y, (Px)y);
    for (int x = 9; x < 24; x++) cv_plot(cv, x, 6, (Px)6);
    hand_parts(cv, kind, 0.0f, skin, 0.45f);
    if (kind == 0) for (int i = 0; i < 3; i++) for (int y = 16; y < 19; y++) cv_plot(cv, (int)(11.9f + i * 4.3f), y, rgb15(cshade(skin, 0.62f)));
    else if (kind == 1) for (int i = 0; i < 3; i++) cv_plot(cv, (int)(13.6f + i * 3.3f), 18, rgb15(cshade(skin, 0.6f)));
}
static Px g_hand_px[2][3][32 * 36];
static bool g_hand_ok[2][3];
static const RGB SKINS[2] = {{244, 204, 170}, {176, 118, 84}};
static const Px *hand_get(Surf *scratch, int kind, int skin) {
    if (!g_hand_ok[skin][kind]) {
        hand_sprite(scratch, kind, SKINS[skin]);
        for (int y = 0; y < 36; y++) memcpy(g_hand_px[skin][kind] + y * 32, scratch->px + y * scratch->stride, sizeof(Px) * 32);
        g_hand_ok[skin][kind] = true;
    }
    return g_hand_px[skin][kind];
}
/* hands are drawn as real pixels (2x2 per sprite pixel) on the top screen: 84 cells would not fit 51 columns */
static void blit_hand(Surf *dst, const Px *src, RGB sleeve, bool mirror, int x, int y, int scale, float dim) {
    Px pal[7];
    pal[0] = 0;
    for (int r = 1; r < 6; r++) pal[r] = rgb15(cshade(sleeve, 0.7f + 0.3f * (r / 5.0f)));
    pal[6] = rgb15(cmix(sleeve, P_WHITE, 0.45f));
    for (int sy = 0; sy < 36; sy++)
        for (int sx = 0; sx < 32; sx++) {
            Px c = src[sy * 32 + (mirror ? 31 - sx : sx)];
            if (!(c & 0x7FFF)) continue;
            if (!(c & 0x8000)) c = pal[c < 7 ? c : 0];
            if (dim < 1) c = pxmix(rgb15i(0, 0, 0), c, dim);
            for (int dy = 0; dy < scale; dy++)
                for (int dx = 0; dx < scale; dx++) {
                    int px = x + sx * scale + dx, py = y + sy * scale + dy;
                    if ((unsigned)px < (unsigned)dst->w && (unsigned)py < (unsigned)dst->h) dst->px[py * dst->stride + px] = c;
                }
        }
}
static float ease_out(float t) { t = clampf(t, 0, 1); float u = 1 - t; return 1 - u * u * u; }

static void rps_cols(RGB *cols) {
    int n = 0;
    for (int i = 0; i < g_sess.nplayers && n < 2; i++)
        if (!g_sess.players[i].is_ai) cols[n++] = live_color(g_sess.players[i].tank_color, (float)g_app.time);
    static const RGB D[2] = {{90, 200, 255}, {255, 150, 90}};
    for (; n < 2; n++) cols[n] = D[n];
}

static void rps_over(Scene *sc, Surf *top, Surf *bot) {
    (void)bot;
    RPS *s = (RPS *)sc;
    Surf *cv = &s->cv.s;
    float tm = (float)g_app.time;
    if (s->stage == RPS_PICK) {
        int me = s->seat >= 0 ? s->seat : s->turn;
        bool locked = s->seat >= 0 && s->picks[s->seat] >= 0;
        for (int i = 0; i < 3; i++) {
            bool sel = i == s->sel && !locked;
            const Px *h = hand_get(cv, i, me % 2);
            int bob = sel && (int)(tm * 4) % 2 ? 6 : 0;
            blit_hand(top, h, me == 0 ? (RGB){90, 200, 255} : (RGB){255, 150, 90}, false, 14 + i * 80, 40 + 72 - 72 - bob + 24, 2, sel ? 1.0f : 0.6f);
        }
    } else if (s->stage == RPS_COUNT || s->stage == RPS_REVEAL) {
        RGB cols[2];
        rps_cols(cols);
        for (int k = 0; k < 2; k++) {
            int kind = s->stage == RPS_COUNT ? 0 : s->picks[k];
            float bob;
            if (s->stage == RPS_COUNT) {
                int beat = (int)(s->t / 0.5f);
                bob = beat < 3 ? fmaxf(0, 14 * fabsf(fsin(s->t * 3.14159265f / 0.5f)) - 2) : 0;
            } else bob = 4 * ease_out(s->t / 0.2f);
            const Px *h = hand_get(cv, kind, k);
            /* the original's canvas is y-up: a bigger bob lifts the hand */
            blit_hand(top, h, cols[k], k == 1, k == 0 ? 24 : 256 - 24 - 64, 48 - (int)(bob * 2), 2, 1.0f);
        }
        if (s->stage == RPS_REVEAL && s->t < 0.12f) surf_rect_blend(top, 0, 8, 256, 176, P_WHITE, 0.5f * (1 - s->t / 0.12f));
    }
}

static void rps_draw(Scene *sc, Term *t) {
    RPS *s = (RPS *)sc;
    backdrop_scene_draw(sc, t);
    float tm = (float)g_app.time;
    term_box(t, 0, 0, 51, 24, TB_DOUBLE, P_LINE, P_PANEL, "WHO SHOOTS FIRST?", P_PRIMARY);
    term_box(t, 0, T_SPLIT, 51, T_SPLIT, TB_DOUBLE, P_LINE, P_PANEL, "", P_PRIMARY);
    int y = T_SPLIT;
    char buf[96];
    if (s->stage == RPS_PICK) {
        int me = s->seat;
        if (me >= 0) snprintf(buf, sizeof buf, "%s: CHOOSE YOUR HAND", s->names[me]);
        else snprintf(buf, sizeof buf, "%s: CHOOSE", s->names[s->turn]);
        term_center(t, 2, buf, GOLD, &P_PANEL, 0, 51);
        if (me < 0) term_center(t, 3, "(the other player look away!)", P_MUTED, &P_PANEL, 0, 51);
        bool locked = me >= 0 && s->picks[me] >= 0;
        for (int i = 0; i < 3; i++) {
            bool sel = i == s->sel && !locked;
            snprintf(buf, sizeof buf, "%s%s", sel ? "\xE2\x96\xB6 " : "  ", RPS_NAMES[i]);
            term_center(t, 20, "", P_PANEL, &P_PANEL, 0, 1);
            term_text(t, 3 + i * 16, 20, buf, sel ? P_WHITE : P_MUTED, &P_PANEL);
            term_fill(t, 2 + i * 16, y + 4, 15, 9, sel ? P_PANEL_HI : P_PANEL);
            term_center(t, y + 8, RPS_NAMES[i], sel ? P_WHITE : P_MUTED, sel ? &P_PANEL_HI : &P_PANEL, 2 + i * 16, 15);
            hot_add(2 + i * 16, y + 4, 15, 9, 700 + i);
        }
        if (locked) term_center(t, y + 15, "LOCKED IN - WAITING FOR THE OTHER PLAYER...", P_MUTED, &P_PANEL, 0, 51);
        else if (s->note[0]) term_center(t, y + 15, s->note, P_AMBER, &P_PANEL, 0, 51);
        static const Keycap K[2] = {{"\xE2\x97\x84 \xE2\x96\xBA", "CHOOSE"}, {"A", "LOCK IN"}};
        ui_keycaps_center(t, T_ROWS - 2, K, 2, &P_PANEL, 0, 51);
    } else if (s->stage == RPS_COUNT) {
        int beat = (int)(s->t / 0.5f);
        static const char *const W[5] = {"ROCK...", "PAPER...", "SCISSORS...", "SHOOT!", "SHOOT!"};
        term_center(t, 21, W[beat < 4 ? beat : 4], beat >= 3 ? GOLD : P_WHITE, &P_PANEL, 0, 51);
        snprintf(buf, sizeof buf, "%s     vs     %s", s->names[0], s->names[1]);
        term_center(t, 2, buf, P_AMBER, &P_PANEL, 0, 51);
    } else if (s->stage == RPS_REVEAL) {
        snprintf(buf, sizeof buf, "%s     vs     %s", s->names[0], s->names[1]);
        term_center(t, 2, buf, P_AMBER, &P_PANEL, 0, 51);
        snprintf(buf, sizeof buf, "%s   vs   %s", RPS_NAMES[s->picks[0]], RPS_NAMES[s->picks[1]]);
        term_center(t, 20, buf, P_WHITE, &P_PANEL, 0, 51);
        if (s->winner < 0) term_center(t, 21, "IT'S A TIE!", P_AMBER, &P_PANEL, 0, 51);
        else { snprintf(buf, sizeof buf, "%s WINS!", s->names[s->winner]); term_center(t, 21, buf, P_OK, &P_PANEL, 0, 51); }
    } else if (s->stage == RPS_CHOOSE) {
        snprintf(buf, sizeof buf, "%s WON", s->names[s->winner]);
        term_center(t, 8, buf, GOLD, &P_PANEL, 0, 51);
        term_center(t, 11, "CHOOSE WHO SHOOTS FIRST", P_WHITE, &P_PANEL, 0, 51);
        static const char *const L[2] = {"GO FIRST", "GO SECOND"};
        for (int i = 0; i < 2; i++) {
            bool sel = i == s->choice;
            RGB bg = sel ? P_PANEL_HI : P_PANEL;
            term_fill(t, 10, y + 4 + i * 6, 31, 4, bg);
            snprintf(buf, sizeof buf, "%s%s%s", sel ? "\xE2\x96\xB6 " : "  ", L[i], sel ? " \xE2\x97\x80" : "  ");
            term_center(t, y + 5 + i * 6 + 1, buf, sel ? GOLD : P_TEXT, &bg, 10, 31);
            hot_add(10, y + 4 + i * 6, 31, 4, 710 + i);
        }
        static const Keycap K[2] = {{"\xE2\x97\x84 \xE2\x96\xBA", "CHOOSE"}, {"A", "CONFIRM"}};
        ui_keycaps_center(t, T_ROWS - 2, K, 2, &P_PANEL, 0, 51);
    } else {
        snprintf(buf, sizeof buf, "%s WON - CHOOSING WHO GOES FIRST...", s->names[s->winner]);
        term_wrap_center(t, 10, buf, 47, P_AMBER, &P_PANEL, 0, 51, 2);
    }
    (void)tm;
}
static void rps_destroy(Scene *sc) { RPS *s = (RPS *)sc; canvas_free(&s->cv); free(s); }
static const SceneVT RPS_VT = {.handle = rps_handle, .update = rps_update, .draw = rps_draw, .draw_over = rps_over, .destroy = rps_destroy};
Scene *scene_rps_seat(int seat) {
    RPS *s = NEW(RPS, &RPS_VT);
    s->seat = seat;
    s->picks[0] = s->picks[1] = -1;
    s->winner = -1;
    int n = 0;
    for (int i = 0; i < g_sess.nplayers && n < 2; i++)
        if (!g_sess.players[i].is_ai) {
            snprintf(s->names[n], sizeof s->names[n], "%.14s", g_sess.players[i].name[0] ? g_sess.players[i].name : (n ? "PLAYER 2" : "PLAYER 1"));
            n++;
        }
    for (; n < 2; n++) snprintf(s->names[n], sizeof s->names[n], "PLAYER %d", n + 1);
    canvas_alloc(&s->cv, 32, 36);
    return &s->base;
}
Scene *scene_rps(void) { return scene_rps_seat(-1); }

/* ================================================================== LAN host with rules (LanHostScene._start versus) */
static void lan_host_with_rules(const Rules *r) {
    extern Scene *scene_lan_host_rules(const Rules *r);
    app_goto(scene_lan_host_rules(r));
}
