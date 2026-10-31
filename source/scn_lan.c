/* LAN PLAY over DS local wireless: LanMenuScene, LanHostScene, LanJoinScene (terminaltanks.py l.10968-11199) and the
 * player/loadout encoding (player_to_json / loadout_from_json l.10889-10946), in binary over netplay.c. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "scenes.h"
#include "ui.h"
#include "netplay.h"

static const RGB GOLD = {255, 206, 100};
#define VERSION_STR "1.5.0-ds"

/* ------------------------------------------------------------------ encoding */
static CustomTank g_remote_ct[CUSTOM_REMOTE_SLOTS];   /* the peer's custom tanks, kept to echo them back in "start" */
static void wr_custom(Wr *w, int design) {
    if (design >= DESIGN_CUSTOM_FIRST && design < DESIGN_CUSTOM_FIRST + CUSTOM_TANK_MAX && g_app.save.custom[design - DESIGN_CUSTOM_FIRST].used) {
        wr_u8(w, 1);
        wr_bytes(w, &g_app.save.custom[design - DESIGN_CUSTOM_FIRST], (int)sizeof(CustomTank));
    } else if (design >= DESIGN_REMOTE_FIRST && design < N_DESIGNS_RT) {
        /* the peer's own custom tank (received in its hello): send the record back so both DSes draw it */
        wr_u8(w, 1);
        wr_bytes(w, &g_remote_ct[design - DESIGN_REMOTE_FIRST], (int)sizeof(CustomTank));
    } else {
        wr_u8(w, 0);
    }
}
/* returns the design index to use locally */
static int rd_custom(Rd *r, int design, int remote_slot) {
    int kind = rd_u8(r);
    if (kind == 1) {
        CustomTank ct;
        rd_bytes(r, &ct, (int)sizeof ct);
        ct.name[sizeof ct.name - 1] = 0;
        ct.lore[sizeof ct.lore - 1] = 0;
        ct.id[sizeof ct.id - 1] = 0;
        custom_register(CUSTOM_TANK_MAX + remote_slot, &ct);
        g_remote_ct[remote_slot] = ct;
        return DESIGN_REMOTE_FIRST + remote_slot;
    }
    if (kind == 2) {
        int k = rd_u8(r);
        (void)k;
        return DESIGN_RANGER;   /* the peer echoes our own custom tank: it is never sent this way by this build */
    }
    return design >= 0 && design < N_DESIGNS ? design : DESIGN_RANGER;
}

static void wr_loadout(Wr *w, const Loadout *lo) {
    wr_u16(w, (uint16_t)lo->tank);
    wr_u8(w, lo->tank_color);
    wr_u16(w, (uint16_t)lo->ammo);
    wr_u8(w, lo->shot_color);
    wr_custom(w, lo->tank);
}
/* loadout_from_json: never trust the peer blindly (unknown kits fall back to the starters) */
static Loadout rd_loadout(Rd *r, int remote_slot) {
    Loadout lo = {0, 0, AMMO_STANDARD, 6};
    int tank = (int16_t)rd_u16(r);
    int tc = rd_u8(r);
    int ammo = (int16_t)rd_u16(r);
    int sc = rd_u8(r);
    lo.tank = (int16_t)rd_custom(r, tank, remote_slot);
    if (ammo >= 0 && ammo < N_AMMOS) lo.ammo = (int16_t)ammo;
    if (tc < N_PAINTS) lo.tank_color = (uint8_t)tc;
    if (sc < N_PAINTS) lo.shot_color = (uint8_t)sc;
    return lo;
}

static void wr_player(Wr *w, const PlayerSetup *p) {
    wr_bytes(w, p->name, 16);
    wr_bytes(w, p->label, 8);
    wr_bytes(w, &p->tank_color, 3);
    wr_bytes(w, &p->shot_color, 3);
    wr_u8(w, p->is_ai);
    wr_u16(w, (uint16_t)p->design);
    wr_u16(w, (uint16_t)p->ammo);
    wr_u16(w, (uint16_t)p->hp);
    wr_u8(w, (uint8_t)(int8_t)p->team);
    wr_u8(w, (uint8_t)p->owner);
    wr_u8(w, (uint8_t)p->diff);
    wr_f64(w, p->prec);
    wr_u8(w, p->kind);
    wr_f64(w, p->scale);
    wr_f64(w, p->hover);
    wr_u8(w, (uint8_t)p->shield);
    wr_u8(w, p->guard);
    wr_custom(w, p->design);
}
static PlayerSetup rd_player(Rd *r, int remote_slot) {
    PlayerSetup p;
    memset(&p, 0, sizeof p);
    rd_bytes(r, p.name, 16);
    p.name[15] = 0;
    rd_bytes(r, p.label, 8);
    p.label[7] = 0;
    rd_bytes(r, &p.tank_color, 3);
    rd_bytes(r, &p.shot_color, 3);
    p.is_ai = rd_u8(r) != 0;
    int design = (int16_t)rd_u16(r);
    int ammo = (int16_t)rd_u16(r);
    p.ammo = (int16_t)(ammo >= 0 && ammo < N_AMMOS ? ammo : AMMO_STANDARD);
    p.hp = rd_u16(r);
    p.team = (int8_t)rd_u8(r);
    p.owner = rd_u8(r);
    p.diff = (int8_t)rd_u8(r);
    p.prec = rd_f64(r);
    p.kind = rd_u8(r);
    p.scale = rd_f64(r);
    p.hover = rd_f64(r);
    p.shield = rd_u8(r);
    p.guard = rd_u8(r) != 0;
    p.design = (int16_t)rd_custom(r, design, remote_slot);
    return p;
}

/* lan_players (l.10938) */
static void lan_players(const Loadout *h, const Loadout *g, int hp, PlayerSetup *out) {
    int gcol = g->tank_color;
    if (gcol == h->tank_color) gcol = ((gcol < N_COLORS ? gcol : 0) + 12) % N_COLORS;
    player_setup_init(&out[0], "PLAYER 1", "P1", paint_rgb(h->tank_color), paint_rgb(h->shot_color), false, h->tank, h->ammo, hp);
    out[0].owner = 0;
    player_setup_init(&out[1], "PLAYER 2", "P2", paint_rgb(gcol), paint_rgb(g->shot_color), false, g->tank, g->ammo, hp);
    out[1].owner = 1;
}

/* ================================================================== LAN menu */
typedef struct { Scene base; MenuList menu; } LanMenu;
static void lanmenu_done(const Loadout *los, int n) { (void)los; (void)n; app_goto(scene_lan_menu()); }
static void lanmenu_handle(Scene *sc, const IEv *e) {
    LanMenu *s = (LanMenu *)sc;
    if (e->action == A_BACK) { app_goto(scene_menu()); return; }
    int c = menu_handle(&s->menu, e, 1800);
    if (c == 0) app_goto(scene_rules(RK_LAN));
    else if (c == 1) app_goto(scene_lan_join());
    else if (c == 2) app_goto(scene_setup(SM_LAN, 0, NULL, lanmenu_done, 1));
    else if (c == 3) app_goto(scene_menu());
}
static void lanmenu_draw(Scene *sc, Term *t) {
    LanMenu *s = (LanMenu *)sc;
    backdrop_scene_draw(sc, t);
    int x, y;
    ui_panel(t, 51, 16, "LAN PLAY", -1, T_SPLIT, T_SPLIT, P_LINE, &x, &y);
    term_center(t, y + 2, "Play a friend on another DS nearby.", P_TEXT, &P_PANEL, x, 51);
    term_center(t, y + 3, "One hosts, one joins - same rules as Two Player.", P_MUTED, &P_PANEL, x, 51);
    term_center(t, y + 4, "DS-to-DS wireless: no router needed.", P_MUTED, &P_PANEL, x, 51);
    menu_draw(t, &s->menu, T_COLS / 2, y + 6, (float)g_app.time, 1, 26, 1800);
    term_center(t, y + 12, "Your PLAYER 1 loadout is the one you bring.", P_MUTED, &P_PANEL, x, 51);
}
static const SceneVT LANMENU_VT = {.handle = lanmenu_handle, .update = backdrop_scene_update, .draw = lanmenu_draw};
Scene *scene_lan_menu(void) {
    LanMenu *s = (LanMenu *)calloc(1, sizeof(LanMenu));
    scene_base(&s->base, &LANMENU_VT);
    static const char *const ITEMS[4] = {"HOST A GAME", "JOIN A GAME", "PILOT SETUP", "BACK"};
    menu_init(&s->menu, ITEMS, 4);
    return &s->base;
}

/* ================================================================== host */
typedef struct {
    Scene base;
    bool has_rules;
    Rules rules;
    int coop_mission;       /* >0: a co-op mission launch instead of a versus match */
    Loadout coop_lo;
    bool guest_ok;
    Loadout guest;
    char guest_ver[24];
    char title[48];
    bool started;
} LanHost;

static void host_start(LanHost *s) {
    s->started = true;
    uint8_t buf[NET_MAX_MSG];
    Wr w = {buf, 0, sizeof buf};
    uint32_t seed = (uint32_t)rng_below(&g_app.rng, 1u << 30);
    if (s->coop_mission > 0) {
        PlayerSetup humans[2];
        int nh = humans_for(s->coop_mission, &s->coop_lo, 1, false, &s->guest, humans);
        double wind = g_app.settings.wind;
        wr_u8(&w, 1);
        wr_u8(&w, (uint8_t)s->coop_mission);
        wr_u32(&w, seed);
        wr_f64(&w, wind);
        wr_u8(&w, (uint8_t)nh);
        for (int i = 0; i < nh; i++) wr_player(&w, &humans[i]);
        net_send(M_START, buf, w.n);
        net_set_match(true);
        session_init_mission(&g_sess, s->coop_mission, humans, nh, seed);
        g_sess.net = true;
        g_sess.my_owner = 0;
        g_sess.has_wind_override = true;
        g_sess.wind_override = wind;
        app_goto(scene_battle());
        return;
    }
    Rules *r = &s->rules;
    SaveData *sv = &g_app.save;
    PlayerSetup players[2];
    lan_players(save_loadout(sv, 0), &s->guest, rules_hp(r), players);
    bool bless = r->blessing && save_blessing_unlocked(sv);      /* host-authoritative: the host's save and rules decide */
    if (!bless)
        for (int i = 0; i < 2; i++) if (players[i].ammo == AMMO_BLESSING) players[i].ammo = AMMO_STANDARD;
    double wind = rules_wind_value(r, &g_app.settings);
    char label[32];
    rules_label(r, label, sizeof label);
    wr_u8(&w, 0);
    wr_u32(&w, seed);
    wr_f64(&w, wind);
    wr_u16(&w, (uint16_t)r->map);
    wr_u8(&w, (uint8_t)rules_target(r));
    wr_str(&w, label);
    wr_u8(&w, r->special);
    wr_u8(&w, r->ammo_fx);
    wr_u8(&w, r->pick_maps);
    wr_u8(&w, bless);
    wr_u8(&w, 2);
    for (int i = 0; i < 2; i++) wr_player(&w, &players[i]);
    net_send(M_START, buf, w.n);
    net_set_match(true);
    session_init_match(&g_sess, players, 2, MODE_LAN, g_app.settings.difficulty, rules_target(r), label, r->ammo_fx, r->special, r->pick_maps,
                       bless, r->map, seed);
    g_sess.my_owner = 0;
    g_sess.has_wind_override = true;
    g_sess.wind_override = wind;
    extern Scene *scene_rps_seat(int seat);
    app_goto(scene_rps_seat(0));
}

static void host_enter(Scene *sc) {
    (void)sc;
    char name[24];
    snprintf(name, sizeof name, "%.10s", g_app.profiles.p[g_app.profiles.active].name);
    net_host_start(name);
}
static void host_exit(Scene *sc) {
    LanHost *s = (LanHost *)sc;
    if (!s->started) net_close();
}
static void host_update(Scene *sc, float dt) {
    LanHost *s = (LanHost *)sc;
    backdrop_scene_update(sc, dt);
    int st = net_state();
    if (st == NS_OFF) return;
    net_pump();
    NetMsg m;
    while (net_take(M_HELLO, &m)) {
        Rd r = {m.data, m.len, 0, false};
        int proto = rd_u8(&r);
        rd_str(&r, s->guest_ver, sizeof s->guest_ver);
        Loadout lo = rd_loadout(&r, 0);
        if (proto != NET_PROTO || strcmp(s->guest_ver, VERSION_STR) || r.bad) {
            char why[64];
            snprintf(why, sizeof why, "VERSION MISMATCH: HOST %s", VERSION_STR);
            net_send(M_REFUSE, why, (int)strlen(why));
            continue;
        }
        s->guest = lo;
        s->guest_ok = true;
        char txt[96];
        if (s->has_rules) {
            char lab[32];
            rules_label(&s->rules, lab, sizeof lab);
            snprintf(txt, sizeof txt, "%s \xC2\xB7 %d HP \xC2\xB7 %s \xC2\xB7 WIND %s%s%s%s", lab, rules_hp(&s->rules),
                     s->rules.map >= 0 ? MAPDEFS[s->rules.map].name : "RANDOM MAP", s->rules.wind ? "ON" : "OFF", s->rules.special ? " \xC2\xB7 SPECIAL" : "",
                     s->rules.ammo_fx ? "" : " \xC2\xB7 AMMO FX OFF", s->rules.blessing ? " \xC2\xB7 BLESSING" : "");
        } else snprintf(txt, sizeof txt, "%s", s->title);
        net_send(M_WELCOME, txt, (int)strlen(txt));
    }
    if (net_take(M_QUIT, NULL) || net_dead()) {
        s->guest_ok = false;
        net_close();
        host_enter(sc);
    }
}
static void host_handle(Scene *sc, const IEv *e) {
    LanHost *s = (LanHost *)sc;
    if (e->action == A_BACK) {
        net_leave();
        app_goto(scene_lan_menu());
    } else if ((ev_confirm(e) || (e->action == A_TOUCH && e->raw == 1)) && s->guest_ok && net_state() == NS_CONNECTED) {
        host_start(s);
    } else if (e->action == A_LEFT || e->action == A_RIGHT) {
        static const int CH[3] = {1, 6, 11};
        int k = 1;
        for (int i = 0; i < 3; i++) if (CH[i] == net_host_channel()) k = i;
        k = (k + (e->action == A_LEFT ? 2 : 1)) % 3;
        net_host_set_channel(CH[k]);
    }
}
static void host_draw(Scene *sc, Term *t) {
    LanHost *s = (LanHost *)sc;
    backdrop_scene_draw(sc, t);
    int x, y;
    ui_panel(t, 51, 19, s->title, -1, T_SPLIT, T_SPLIT, P_LINE, &x, &y);
    char buf[96];
    if (!net_available() || net_state() == NS_OFF) {
        term_center(t, y + 4, "Could not start the wireless:", P_DANGER, &P_PANEL, x, 51);
        term_center(t, y + 5, net_error(), P_MUTED, &P_PANEL, x, 51);
        term_center(t, y + 17, "B  CANCEL", P_MUTED, &P_PANEL, x, 51);
        return;
    }
    term_center(t, y + 2, "TELL YOUR FRIEND TO JOIN:", P_MUTED, &P_PANEL, x, 51);
    snprintf(buf, sizeof buf, "%s  \xC2\xB7  CHANNEL %d", g_app.profiles.p[g_app.profiles.active].name, net_host_channel());
    term_center(t, y + 3, buf, GOLD, &P_PANEL, x, 51);
    term_center(t, y + 5, "(LAN PLAY > JOIN A GAME on the other DS)", P_MUTED, &P_PANEL, x, 51);
    term_center(t, y + 6, "\xE2\x97\x84\xE2\x96\xBA change the wireless channel", P_MUTED, &P_PANEL, x, 51);
    if (s->has_rules) {
        char lab[32];
        rules_label(&s->rules, lab, sizeof lab);
        snprintf(buf, sizeof buf, "%s \xC2\xB7 %d HEARTS EACH", lab, rules_hp(&s->rules));
        term_center(t, y + 8, buf, P_AMBER, &P_PANEL, x, 51);
    }
    if (s->guest_ok && net_state() == NS_CONNECTED) {
        term_center(t, y + 10, "PLAYER 2 CONNECTED", P_OK, &P_PANEL, x, 51);
        snprintf(buf, sizeof buf, "%s + %s", design_ptr(s->guest.tank)->name, AMMOS[s->guest.ammo].name);
        term_center(t, y + 11, buf, P_TEXT, &P_PANEL, x, 51);
        if ((int)(g_app.time * 2) % 2 == 0) term_center(t, y + 13, "[ A ] START", P_PRIMARY, &P_PANEL, x, 51);
        hot_add(x, y + 12, 51, 3, 1);
    } else {
        snprintf(buf, sizeof buf, "WAITING FOR PLAYER 2%.*s", (int)(g_app.time * 2) % 4, "...");
        term_center(t, y + 10, buf, P_AMBER, &P_PANEL, x, 51);
    }
    term_center(t, y + 17, "B  CANCEL", P_MUTED, &P_PANEL, x, 51);
}
static const SceneVT HOST_VT = {.enter = host_enter, .exit = host_exit, .handle = host_handle, .update = host_update, .draw = host_draw};
Scene *scene_lan_host_rules(const Rules *r) {
    LanHost *s = (LanHost *)calloc(1, sizeof(LanHost));
    scene_base(&s->base, &HOST_VT);
    s->has_rules = r != NULL;
    if (r) s->rules = *r;
    snprintf(s->title, sizeof s->title, "HOSTING A LAN GAME");
    return &s->base;
}
Scene *scene_lan_host(void) { return scene_lan_host_rules(NULL); }
void lan_host_coop(int mission, const Loadout *lo) {
    LanHost *s = (LanHost *)scene_lan_host_rules(NULL);
    s->coop_mission = mission;
    s->coop_lo = *lo;
    snprintf(s->title, sizeof s->title, "CO-OP MISSION %d", mission);
    app_goto(&s->base);
}

/* ================================================================== join */
typedef struct { Scene base; NetGame found[8]; int nfound, index; bool hello_sent; char msg[64]; char rules[96]; } LanJoin;
static void join_enter(Scene *sc) { (void)sc; net_scan_start(); }
static void join_exit(Scene *sc) { (void)sc; }
static void join_begin(LanJoin *s, NetMsg *m) {
    (void)s;
    net_set_match(true);                          /* from here on a dropped link is re-joined, not ended */
    Rd r = {m->data, m->len, 0, false};
    int kind = rd_u8(&r);
    if (kind == 1) {
        int mission = rd_u8(&r);
        uint32_t seed = rd_u32(&r);
        double wind = rd_f64(&r);
        int nh = rd_u8(&r);
        PlayerSetup humans[2];
        for (int i = 0; i < nh && i < 2; i++) humans[i] = rd_player(&r, i);
        if (r.bad || mission < 1 || mission > N_MISSIONS) { net_leave(); app_goto(scene_notice("LAN ERROR", "The host sent a match this DS could not read.")); return; }
        session_init_mission(&g_sess, mission, humans, nh, seed);
        g_sess.net = true;
        g_sess.my_owner = 1;
        g_sess.has_wind_override = true;
        g_sess.wind_override = wind;
        app_goto(scene_battle());
        return;
    }
    uint32_t seed = rd_u32(&r);
    double wind = rd_f64(&r);
    int map = (int16_t)rd_u16(&r);
    int target = rd_u8(&r);
    char label[32];
    rd_str(&r, label, sizeof label);
    bool special = rd_u8(&r), fx = rd_u8(&r), pick = rd_u8(&r), bless = rd_u8(&r);
    int n = rd_u8(&r);
    PlayerSetup pl[2];
    for (int i = 0; i < n && i < 2; i++) pl[i] = rd_player(&r, i);
    if (r.bad || n != 2) { net_leave(); app_goto(scene_notice("LAN ERROR", "The host sent a match this DS could not read.")); return; }
    if (map >= N_MAPS_ALL) map = -1;
    session_init_match(&g_sess, pl, 2, MODE_LAN, g_app.settings.difficulty, target, label, fx, special, pick, bless, map, seed);
    g_sess.my_owner = 1;
    g_sess.has_wind_override = true;
    g_sess.wind_override = wind;
    extern Scene *scene_rps_seat(int seat);
    app_goto(scene_rps_seat(1));
}
static void join_update(Scene *sc, float dt) {
    LanJoin *s = (LanJoin *)sc;
    backdrop_scene_update(sc, dt);
    int st = net_state();
    if (st == NS_SCANNING) {
        s->nfound = net_scan_list(s->found, 8);
        if (s->index >= s->nfound) s->index = s->nfound ? s->nfound - 1 : 0;
        return;
    }
    if (st == NS_DEAD) {
        snprintf(s->msg, sizeof s->msg, "%s", net_error()[0] ? net_error() : "THE HOST CLOSED THE GAME");
        s->hello_sent = false;
        s->rules[0] = 0;
        net_close();
        net_scan_start();
        return;
    }
    if (st != NS_CONNECTING && st != NS_CONNECTED) return;
    net_pump();
    if (net_state() == NS_CONNECTED && !s->hello_sent) {
        uint8_t buf[NET_MAX_MSG];
        Wr w = {buf, 0, sizeof buf};
        wr_u8(&w, NET_PROTO);
        wr_str(&w, VERSION_STR);
        wr_loadout(&w, save_loadout(&g_app.save, 0));
        net_send(M_HELLO, buf, w.n);
        s->hello_sent = true;
    }
    NetMsg m;
    while (net_take(M_WELCOME, &m)) {
        int l = m.len < 95 ? m.len : 95;
        memcpy(s->rules, m.data, (size_t)l);
        s->rules[l] = 0;
    }
    if (net_take(M_REFUSE, &m)) {
        int l = m.len < 63 ? m.len : 63;
        memcpy(s->msg, m.data, (size_t)l);
        s->msg[l] = 0;
        net_close();
        net_scan_start();
        s->hello_sent = false;
        return;
    }
    if (net_take(M_QUIT, NULL)) {
        snprintf(s->msg, sizeof s->msg, "THE HOST CLOSED THE GAME");
        net_close();
        net_scan_start();
        s->hello_sent = false;
        return;
    }
    if (net_take(M_START, &m)) join_begin(s, &m);
}
static void join_handle(Scene *sc, const IEv *e) {
    LanJoin *s = (LanJoin *)sc;
    if (e->action == A_BACK) {
        net_leave();
        app_goto(scene_lan_menu());
        return;
    }
    if (net_state() != NS_SCANNING) return;
    if (e->action == A_UP) s->index = s->index > 0 ? s->index - 1 : 0;
    else if (e->action == A_DOWN) s->index = s->index + 1 < s->nfound ? s->index + 1 : (s->nfound ? s->nfound - 1 : 0);
    else if (e->action == A_TOUCH && e->raw >= 1900 && e->raw < 1900 + s->nfound) {
        if (s->index == e->raw - 1900) goto join;
        s->index = e->raw - 1900;
    } else if (ev_confirm(e) && s->nfound) {
    join:
        if (!s->found[s->index].open) { snprintf(s->msg, sizeof s->msg, "THAT GAME IS FULL OR ALREADY STARTED"); return; }
        s->msg[0] = 0;
        net_join(s->found[s->index].idx);
    }
}
static void join_draw(Scene *sc, Term *t) {
    LanJoin *s = (LanJoin *)sc;
    backdrop_scene_draw(sc, t);
    int x, y;
    ui_panel(t, 51, 20, "JOIN A LAN GAME", -1, T_SPLIT, T_SPLIT, P_LINE, &x, &y);
    char buf[96];
    int st = net_state();
    if (!net_available()) {
        term_center(t, y + 5, "Could not start the wireless:", P_DANGER, &P_PANEL, x, 51);
        term_center(t, y + 6, net_error(), P_MUTED, &P_PANEL, x, 51);
        return;
    }
    if (st == NS_CONNECTING || st == NS_CONNECTED) {
        term_center(t, y + 6, st == NS_CONNECTED ? "CONNECTED" : "CONNECTING...", st == NS_CONNECTED ? P_OK : P_AMBER, &P_PANEL, x, 51);
        if (s->rules[0]) term_wrap_center(t, y + 8, s->rules, 47, P_AMBER, &P_PANEL, x, 51, 2);
        snprintf(buf, sizeof buf, "WAITING FOR THE HOST TO START%.*s", (int)(g_app.time * 2) % 4, "...");
        term_center(t, y + 11, buf, P_TEXT, &P_PANEL, x, 51);
        term_center(t, y + 17, "B  LEAVE", P_MUTED, &P_PANEL, x, 51);
        return;
    }
    term_text(t, x + 3, y + 2, "GAMES NEARBY", P_AMBER, &P_PANEL);
    if (!s->nfound) term_text(t, x + 3, y + 4, (int)(g_app.time * 2) % 2 ? "searching..." : "searching", P_MUTED, &P_PANEL);
    for (int i = 0; i < s->nfound && i < 6; i++) {
        bool sel = i == s->index;
        RGB bg = sel ? P_PANEL_HI : P_PANEL;
        term_fill(t, x + 2, y + 4 + i, 47, 1, bg);
        snprintf(buf, sizeof buf, "%s%-14s CH %-2d  %d/%d%s", sel ? "\xE2\x96\xB8 " : "  ", s->found[i].name, s->found[i].channel, s->found[i].players, s->found[i].max,
                 s->found[i].open ? "" : "  FULL");
        term_text(t, x + 3, y + 4 + i, buf, sel ? P_WHITE : P_TEXT, &bg);
        hot_add(x + 2, y + 4 + i, 47, 1, 1900 + i);
    }
    if (s->msg[0]) term_center(t, y + 15, s->msg, P_DANGER, &P_PANEL, x, 51);
    else term_center(t, y + 15, "\xE2\x96\xB2\xE2\x96\xBC PICK   A JOIN   B BACK", P_MUTED, &P_PANEL, x, 51);
}
static const SceneVT JOIN_VT = {.enter = join_enter, .exit = join_exit, .handle = join_handle, .update = join_update, .draw = join_draw};
Scene *scene_lan_join(void) {
    LanJoin *s = (LanJoin *)calloc(1, sizeof(LanJoin));
    scene_base(&s->base, &JOIN_VT);
    return &s->base;
}
