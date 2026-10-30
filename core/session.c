#include <stdio.h>
#include <string.h>
#include "session.h"

/* ---------------------------------------------------------------- tournament level rules (l.4159-4216) */
int level_fail_at(int level) {          /* -1 = never fails */
    int r = TOURNAMENT[level - 1].rule;
    return r == RULE_STREAK ? -1 : r == RULE_FLAWLESS ? 1 : r == RULE_ONESTRIKE ? 2 : 3;
}
static int lv_strikes(int level) { int f = level_fail_at(level); return f < 0 ? -1 : f - 1; }
static bool lv_consecutive(int level) { return TOURNAMENT[level - 1].rule == RULE_STREAK; }
static bool lv_final(int level) { return level > N_LEVELS - 3; }
static const char *const RULE_TAG[6] = {"STANDARD", "BACK TO BACK", "FLAWLESS", "ONE STRIKE", "GLASS CANNON", "GALE FORCE"};
static const char *const RULE_TEXT[6] = {"2 STRIKES - BOSS WIN #3 FAILS IT", "NO STRIKES - A LOSS RESETS STREAK", "NO STRIKES - 1 LOST ROUND FAILS",
                                         "1 STRIKE - BOSS WIN #2 FAILS IT", "2 HEARTS - BOSS WIN #3 FAILS IT", "WIND LOCKED AT MAX - 2 STRIKES"};
const char *level_rule_tag(int level) { return RULE_TAG[TOURNAMENT[level - 1].rule]; }
const char *level_rule_text(int level) { return RULE_TEXT[TOURNAMENT[level - 1].rule]; }
void level_objective(int level, char *out, int n) {
    const TournLevel *lv = &TOURNAMENT[level - 1];
    if (lv->rule == RULE_STREAK) snprintf(out, (size_t)n, "WIN %d ROUNDS IN A ROW", lv->wins);
    else if (lv->rule == RULE_FLAWLESS) snprintf(out, (size_t)n, "WIN %d ROUNDS, LOSE NONE", lv->wins);
    else snprintf(out, (size_t)n, "WIN %d ROUNDS", lv->wins);
}

/* ---------------------------------------------------------------- payouts (l.4320-4343, 4669) */
void tournament_payout(int level, bool won, bool first, int *coins, int *credits) {
    if (!won) { *coins = 10; *credits = 0; return; }
    if (lv_final(level)) { *coins = first ? 400 : 50; *credits = first ? 15 : 4; return; }
    *coins = first ? 120 : 50;
    *credits = first ? 4 : 0;
}
int mission_first_coins(int m) {
    const TTMission *ms = &MISSIONS[m - 1];
    int base = 40 + 3 * ms->number;
    if (ms->kind == MK_RUSH) base = (int)(base * 1.5);
    if (ms->kind == MK_MOTHER) base += 400;
    return base;
}
int mission_repeat_coins(int m) { return (int)(mission_first_coins(m) * 0.4); }
static const double SP_WIN_MULT[4] = {1.0, 1.25, 1.5, 2.0};

/* ---------------------------------------------------------------- players */
void player_setup_init(PlayerSetup *p, const char *name, const char *label, RGB tank, RGB shot, bool ai, int design, int ammo, int hp) {
    memset(p, 0, sizeof(*p));
    snprintf(p->name, sizeof p->name, "%s", name);
    snprintf(p->label, sizeof p->label, "%s", label);
    p->tank_color = tank;
    p->shot_color = shot;
    p->is_ai = ai;
    p->design = (int16_t)design;
    p->ammo = (int16_t)ammo;
    p->hp = hp;
    p->team = -1;
    p->diff = -1;
    p->prec = 1.0;
    p->scale = 1.0;
}

PlayerSetup boss_setup(int level, int player_color) {
    const TournLevel *lv = &TOURNAMENT[level - 1];
    int bi = lv->boss_color;
    if (bi == player_color) bi = (bi + 5) % N_COLORS;
    PlayerSetup p;
    player_setup_init(&p, lv->boss, "BOSS", COLOR_CHOICES[bi].c, COLOR_CHOICES[lv->boss_shot].c, true, lv->tank, lv->ammo, MAX_HP);
    return p;
}

PlayerSetup pick_ai_kit(const SaveData *save, PyRandom *rng, int difficulty, int player_color, const int16_t *ammo_pool, int npool) {
    (void)save;
    int design, ammo, body, shot;
    if (difficulty == DIFF_MASTER) {
        const TournLevel *lv = &TOURNAMENT[N_LEVELS - 1];
        body = lv->boss_color; shot = lv->boss_shot; design = lv->tank; ammo = lv->ammo;
    } else {
        design = (int)rng_below(rng, 5);
        ammo = npool ? ammo_pool[rng_below(rng, (uint32_t)npool)] : AMMO_STANDARD;
        body = (int)rng_below(rng, N_COLORS);
        shot = (int)rng_below(rng, N_COLORS);
    }
    int bi = body;
    if (bi == player_color) bi = (bi + 5) % N_COLORS;
    PlayerSetup p;
    player_setup_init(&p, "AI", "CPU", COLOR_CHOICES[bi].c, COLOR_CHOICES[shot].c, true, design, ammo, MAX_HP);
    return p;
}

static PlayerSetup mission_foe(PyRandom *rng, int diff, double prec, const bool *avoid, const char *name, int hp) {
    int colors[N_COLORS], n = 0;
    for (int i = 0; i < N_COLORS; i++) if (!avoid[i]) colors[n++] = i;
    int body = colors[rng_below(rng, (uint32_t)n)];
    int shot = colors[rng_below(rng, (uint32_t)n)];
    int design = (int)rng_below(rng, 5);
    int ammo = (int)rng_below(rng, 5);
    PlayerSetup p;
    player_setup_init(&p, name, "AI", COLOR_CHOICES[body].c, COLOR_CHOICES[shot].c, true, design, ammo, hp);
    p.team = 1;
    p.diff = (int8_t)diff;
    p.prec = prec;
    return p;
}

static PlayerSetup ufo_setup(const char *name, int hp, int diff, double prec, double scale, RGB color, bool guard) {
    PlayerSetup p;
    player_setup_init(&p, name, "UFO", color, (RGB){170, 255, 200}, true, DESIGN_RANGER, AMMO_XENON, hp);
    p.team = 1;
    p.diff = (int8_t)diff;
    p.prec = prec;
    p.kind = KIND_UFO;
    p.scale = scale;
    p.hover = 4.0 * scale + 2.0;
    p.guard = guard;
    return p;
}

int humans_for(int mission, const Loadout *lo, int nlo, bool wingman, const Loadout *guest, PlayerSetup *out) {
    (void)wingman;
    const TTMission *m = &MISSIONS[mission - 1];
    bool coop = m->kind == MK_COOP;
    int hp = m->hp;
    player_setup_init(&out[0], coop ? "PLAYER 1" : "PLAYER", coop ? "P1" : "YOU", paint_rgb(lo[0].tank_color), paint_rgb(lo[0].shot_color),
                      false, lo[0].tank, lo[0].ammo, hp);
    out[0].team = 0;
    if (!coop) return 1;
    if (guest || nlo > 1) {
        const Loadout *l2 = guest ? guest : &lo[1];
        int col = l2->tank_color;
        if (col == lo[0].tank_color) col = (col < N_COLORS ? col : 0) + 12, col %= N_COLORS;
        player_setup_init(&out[1], "PLAYER 2", "P2", paint_rgb(col), paint_rgb(l2->shot_color), false, l2->tank, l2->ammo, hp);
        out[1].team = 0;
        out[1].owner = guest ? 1 : 0;
        return 2;
    }
    int ci = lo[0].tank_color < N_COLORS ? lo[0].tank_color : 0;
    int col = (ci + 12) % N_COLORS;
    player_setup_init(&out[1], "WINGMAN", "AI", COLOR_CHOICES[col].c, (RGB){200, 255, 200}, true, design_by_key("gemini"), AMMO_STANDARD, hp);
    out[1].team = 0;
    out[1].diff = DIFF_NORMAL;
    out[1].prec = 1.0;
    return 2;
}

/* ---------------------------------------------------------------- map rotation */
static bool played_has(const Session *s, int m) {
    for (int i = 0; i < s->nplayed; i++) if (s->played[i] == m) return true;
    return false;
}
static int base_pick_map(Session *s) {
    int pool[N_MAPS_LIB], n = 0;
    for (int m = 0; m < N_MAPS_LIB; m++) if (!played_has(s, m)) pool[n++] = m;
    if (!n) for (int m = 0; m < N_MAPS_LIB; m++) pool[n++] = m;
    return pool[rng_below(&s->rng, (uint32_t)n)];
}
static int pick_map(Session *s) {
    switch (s->type) {
    case SESS_TOURNAMENT: {
        int mk = TOURNAMENT[s->level - 1].map;
        return mk >= 0 ? mk : base_pick_map(s);
    }
    case SESS_MISSION: {
        const TTMission *m = &MISSIONS[s->mission - 1];
        int key = m->map;
        if (m->kind == MK_RUSH) {
            int st = s->stage < m->nlevels - 1 ? s->stage : m->nlevels - 1;
            key = TOURNAMENT[m->levels[st] - 1].map;
        }
        return key >= 0 ? key : base_pick_map(s);
    }
    case SESS_TICKET: return TICKETS[s->ticket].map;
    case SESS_MARTYR: return MAPS_ANCIENT_FIRST + (s->phase_i < 2 ? s->phase_i : 2);
    default: return base_pick_map(s);
    }
}
static void push_played(Session *s, int m) {
    if (s->nplayed < MAX_PLAYED) s->played[s->nplayed++] = (int16_t)m;
}

static void base_init(Session *s, int first_map, uint32_t seed) {
    rng_seed(&s->rng, seed);
    s->wins[0] = s->wins[1] = 0;
    s->round_no = 1;
    s->nplayed = 0;
    s->pending_map = -1;
    s->current_map = first_map >= 0 ? first_map : pick_map(s);
    push_played(s, s->current_map);
}

static void config_defaults(Session *s) {
    memset(s, 0, sizeof(*s));
    s->ai_precision = 1.0;
    s->target = 2;
    s->special_wait = 2;
    s->ammo_fx = true;
    s->level = 1;
    s->mission = 1;
    s->ticket = -1;
}

void session_init_match(Session *s, const PlayerSetup *players, int n, int mode, int difficulty, int target, const char *label,
                        bool ammo_fx, bool special, bool pick_maps, bool blessing, int first_map, uint32_t seed) {
    config_defaults(s);
    s->type = SESS_MATCH;
    memcpy(s->players, players, sizeof(PlayerSetup) * (size_t)n);
    s->nplayers = n;
    s->mode = mode;
    s->single = mode == MODE_SINGLE;
    s->difficulty = difficulty;
    s->target = target;
    snprintf(s->label, sizeof s->label, "%s", label ? label : "");
    s->ammo_fx = ammo_fx;
    s->special = special;
    s->pick_maps = pick_maps;
    s->blessing = blessing;
    s->net = mode == MODE_LAN;
    base_init(s, first_map, seed);
}

void session_init_tournament(Session *s, int level, const PlayerSetup *me, uint32_t seed) {
    config_defaults(s);
    const TournLevel *lv = &TOURNAMENT[level - 1];
    s->type = SESS_TOURNAMENT;
    s->level = level;
    s->players[0] = *me;
    s->players[0].hp = lv->rule == RULE_GLASS ? 2 : MAX_HP;
    int pcol = -1;
    for (int i = 0; i < N_COLORS; i++) if (rgb_eq(COLOR_CHOICES[i].c, me->tank_color)) { pcol = i; break; }
    s->players[1] = boss_setup(level, pcol);
    s->nplayers = 2;
    s->single = true;
    s->difficulty = lv->difficulty;
    s->mode = MODE_TOURNAMENT;
    s->ai_precision = lv->precision;
    s->ammo_fx = false;
    if (lv->rule == RULE_GALE) { s->has_wind_override = true; s->wind_override = 10.0; }
    base_init(s, -1, seed);
}

static void stage_players(Session *s, int stage) {
    const TTMission *m = &MISSIONS[s->mission - 1];
    PyRandom rng;
    rng_seed(&rng, rng_below(&s->rng, 1u << 30));
    bool avoid[N_COLORS] = {0};
    for (int c = 0; c < N_COLORS; c++)
        for (int h = 0; h < s->nhumans; h++) if (rgb_eq(COLOR_CHOICES[c].c, s->humans[h].tank_color)) avoid[c] = true;
    PlayerSetup foes[4];
    int nf = 0;
    char name[16];
    if (m->kind == MK_MARATHON) {
        snprintf(name, sizeof name, "RIVAL %d", stage + 1);
        foes[nf++] = mission_foe(&rng, session_stage_diff(s), m->prec, avoid, name, 3);
    } else if (m->kind == MK_RUSH) {
        int lvn = m->levels[stage];
        int pcol = 0;
        for (int i = 0; i < N_COLORS; i++) if (rgb_eq(COLOR_CHOICES[i].c, s->humans[0].tank_color)) { pcol = i; break; }
        PlayerSetup b = boss_setup(lvn, pcol);
        b.team = 1;
        b.hp = 3;
        b.diff = (int8_t)TOURNAMENT[lvn - 1].difficulty;
        b.prec = TOURNAMENT[lvn - 1].precision;
        foes[nf++] = b;
    } else if (m->kind == MK_MOTHER) {
        foes[nf++] = ufo_setup("MOTHERSHIP", m->ufo_hp, m->diff, m->prec * 1.2, 4.0, (RGB){120, 255, 190}, true);
        foes[nf++] = ufo_setup("DRONE A", m->enemy_hp, m->diff, m->prec * 1.5, 2.0, (RGB){200, 140, 255}, false);
        foes[nf++] = ufo_setup("DRONE B", m->enemy_hp, m->diff, m->prec * 1.5, 2.0, (RGB){140, 200, 255}, false);
    } else if (m->kind == MK_UFO || (m->kind == MK_COOP && m->coop_ufo)) {
        const char *nm = m->name;
        if (!strncmp(nm, "UFO: ", 5)) nm += 5;
        foes[nf++] = ufo_setup(nm, m->ufo_hp, m->diff, m->prec * 1.4, 3.0, (RGB){90, 230, 160}, false);
    } else {
        for (int k = 0; k < m->enemies; k++) {
            snprintf(name, sizeof name, "ENEMY %d", k + 1);
            foes[nf++] = mission_foe(&rng, m->diff, m->prec * 1.3, avoid, name, m->enemy_hp);
        }
    }
    s->nplayers = 0;
    for (int h = 0; h < s->nhumans; h++) s->players[s->nplayers++] = s->humans[h];
    for (int f = 0; f < nf; f++) s->players[s->nplayers++] = foes[f];
}

void session_init_mission(Session *s, int mission, const PlayerSetup *humans, int nhumans, uint32_t seed) {
    config_defaults(s);
    const TTMission *m = &MISSIONS[mission - 1];
    s->type = SESS_MISSION;
    s->mission = mission;
    s->nhumans = nhumans;
    memcpy(s->humans, humans, sizeof(PlayerSetup) * (size_t)nhumans);
    memcpy(s->players, humans, sizeof(PlayerSetup) * (size_t)nhumans);
    s->nplayers = nhumans;
    s->single = false;
    s->difficulty = m->diff;
    s->mode = MODE_MISSION;
    s->target = m->target;
    s->ammo_fx = false;
    base_init(s, -1, seed);
    stage_players(s, 0);
}

void session_init_ticket(Session *s, int ticket, const Loadout *lo, uint32_t seed) {
    config_defaults(s);
    const TicketBoss *b = &TICKETS[ticket];
    s->type = SESS_TICKET;
    s->ticket = ticket;
    player_setup_init(&s->players[0], "PLAYER", "YOU", paint_rgb(lo->tank_color), paint_rgb(lo->shot_color), false, lo->tank, lo->ammo, 6);
    s->players[0].team = 0;
    player_setup_init(&s->players[1], b->name, "BOSS", DESIGNS[b->tank].accent, (RGB){255, 255, 255}, true, b->tank, b->ammo, b->hp);
    s->players[1].team = 1;
    s->players[1].diff = DIFF_MASTER;
    s->players[1].prec = b->prec;
    s->nplayers = 2;
    s->difficulty = DIFF_MASTER;
    s->mode = MODE_TICKET;
    s->target = 2;
    base_init(s, -1, seed);
}

static const int MARTYR_MAGE_HP[3] = {250, 200, 260};
static const int MARTYR_MAGE_MULT[3] = {4, 5, 6};
static const double MARTYR_PREC[3] = {0.8, 0.68, 0.55};

static void set_mage(Session *s) {
    int p = s->phase_i < 2 ? s->phase_i : 2;
    s->players[1].hp = MARTYR_MAGE_HP[p];
    s->players[1].prec = MARTYR_PREC[p];
}

void session_init_martyr(Session *s, const Loadout *lo, int phase, bool replay, uint32_t seed) {
    config_defaults(s);
    s->type = SESS_MARTYR;
    int ph = replay ? 0 : (phase < 2 ? phase : 2);
    player_setup_init(&s->players[0], "PLAYER", "YOU", paint_rgb(lo->tank_color), paint_rgb(lo->shot_color), false, lo->tank, lo->ammo, 175);
    s->players[0].team = 0;
    player_setup_init(&s->players[1], "THE MARTYR", "MAGE", (RGB){190, 150, 255}, (RGB){255, 240, 170}, true, DESIGN_A_MARTYR, AMMO_TIMERIFT,
                      MARTYR_MAGE_HP[ph]);
    s->players[1].team = 1;
    s->players[1].diff = DIFF_MASTER;
    s->players[1].prec = MARTYR_PREC[ph];
    s->nplayers = 2;
    s->difficulty = DIFF_MASTER;
    s->mode = MODE_MARTYR;
    s->target = 3;
    s->ammo_fx = true;
    s->special = true;
    s->special_wait = 1;
    s->phase_i = ph;
    s->replay = replay;
    base_init(s, -1, seed);
    s->wins[0] = ph;
    set_mage(s);
}

void session_init_practice(Session *s, const PlayerSetup *me, const PlayerSetup *target, uint32_t seed, int lesson, int adv, double wind) {
    config_defaults(s);
    s->type = SESS_PRACTICE;
    s->players[0] = *me;
    s->players[1] = *target;
    s->nplayers = 2;
    s->single = true;
    s->difficulty = DIFF_EASY;
    s->mode = MODE_SINGLE;
    s->target = 99;
    s->lesson = lesson;
    s->adv = adv;
    s->has_wind_override = true;
    s->wind_override = wind;
    base_init(s, 0 /* MAPS[0]: VALLEY */, seed);
}

/* ---------------------------------------------------------------- rules */
int session_starting_player(const Session *s) { return (s->round_no - 1 + s->first_offset) % 2; }
uint32_t session_round_seed(Session *s) { return rng_below(&s->rng, 1u << 30); }

int session_stage_diff(const Session *s) {
    const TTMission *m = &MISSIONS[s->mission - 1];
    if (m->kind == MK_MARATHON && m->stages > 1) {
        int a = m->diff, b = m->diff_end;
        return a != b ? a + (b - a) * s->stage / (m->stages - 1) : m->diff;
    }
    return m->diff;
}
static int stage_target(const Session *s) { const TTMission *m = &MISSIONS[s->mission - 1]; return m->kind == MK_MARATHON ? 2 : m->target; }
static int fail_target(const Session *s) { const TTMission *m = &MISSIONS[s->mission - 1]; return m->kind == MK_MARATHON ? 2 : m->fail_at; }

void session_record_round(Session *s, int winner) {
    switch (s->type) {
    case SESS_TOURNAMENT:
        s->wins[winner]++;
        if (winner == 1 && lv_consecutive(s->level)) s->wins[0] = 0;
        break;
    case SESS_MISSION:
        s->wins[winner]++;
        if (winner == 0 && s->wins[0] >= stage_target(s) && s->stage < MISSIONS[s->mission - 1].stages - 1) s->stage_cleared = true;
        break;
    case SESS_MARTYR:
        s->won_phase = winner == 0;
        if (winner == 0) s->wins[0]++;
        break;
    default:
        s->wins[winner]++;
    }
}

bool session_completed(const Session *s) {
    switch (s->type) {
    case SESS_TOURNAMENT: return s->wins[0] >= TOURNAMENT[s->level - 1].wins;
    case SESS_MISSION: return s->stage == MISSIONS[s->mission - 1].stages - 1 && s->wins[0] >= stage_target(s);
    case SESS_TICKET: return s->wins[0] >= 2;
    case SESS_MARTYR: return s->wins[0] >= 3;
    default: return s->wins[0] >= s->target && s->wins[0] > s->wins[1];
    }
}

static bool session_failed(const Session *s) {
    switch (s->type) {
    case SESS_TOURNAMENT: { int f = level_fail_at(s->level); return f >= 0 && s->wins[1] >= f && !session_completed(s); }
    case SESS_MISSION: return s->wins[1] >= fail_target(s) && !session_completed(s);
    case SESS_TICKET: return s->wins[1] >= 2;
    default: return false;
    }
}

bool session_over(const Session *s) {
    if (s->type == SESS_MATCH || s->type == SESS_PRACTICE) return (s->wins[0] > s->wins[1] ? s->wins[0] : s->wins[1]) >= s->target;
    if (s->type == SESS_MARTYR) return session_completed(s);
    return session_completed(s) || session_failed(s);
}

int session_champion(const Session *s) {
    if (s->type == SESS_MATCH || s->type == SESS_PRACTICE) return s->wins[0] > s->wins[1] ? 0 : 1;
    if (s->type == SESS_MARTYR) return 0;
    return session_completed(s) ? 0 : 1;
}

void session_advance(Session *s) {
    if (s->type == SESS_MARTYR) {
        s->round_no++;
        if (s->won_phase) s->phase_i = s->phase_i + 1 < 2 ? s->phase_i + 1 : 2;
        set_mage(s);
        s->current_map = pick_map(s);
        push_played(s, s->current_map);
        return;
    }
    if (s->type == SESS_MISSION && s->stage_cleared) {
        s->stage++;
        s->wins[0] = s->wins[1] = 0;
        s->stage_cleared = false;
        stage_players(s, s->stage);
    }
    s->round_no++;
    s->current_map = pick_map(s);                 /* always drawn, so both linked consoles keep their rngs in step */
    if (s->pending_map >= 0) {
        s->current_map = s->pending_map;
        s->pending_map = -1;
    }
    push_played(s, s->current_map);
}

bool session_wants_interlude(const Session *s) { return s->type == SESS_MARTYR && s->won_phase; }

int session_dmg_mult(const Session *s, const Tank *shooter) {
    if (s->type != SESS_MARTYR) return 1;
    return shooter->index == 0 ? 10 : MARTYR_MAGE_MULT[s->phase_i < 2 ? s->phase_i : 2];
}

void session_commit(Session *s, SaveData *save) {
    switch (s->type) {
    case SESS_MATCH:
    case SESS_PRACTICE:
        if (session_over(s) && !s->paid && s->mode == MODE_SINGLE) {
            s->paid = true;
            s->has_payout = true;
            s->payout_coins = session_champion(s) == 0 ? (int)(20 * SP_WIN_MULT[s->difficulty]) : 10;
            s->payout_credits = 0;
            save_earn(save, s->payout_coins, 0);
        }
        break;
    case SESS_TOURNAMENT:
        if (session_over(s) && !s->committed) {
            s->committed = true;
            bool done = session_completed(s);
            if (done) {
                s->first_clear = save_complete_level(save, s->level);
                if (s->first_clear && s->level == N_LEVELS) save->settings.home_mode = HOME_MASTER;
            }
            tournament_payout(s->level, done, s->first_clear, &s->payout_coins, &s->payout_credits);
            s->has_payout = true;
            save_earn(save, s->payout_coins, s->payout_credits);
        }
        break;
    case SESS_MISSION:
        if (session_over(s) && !s->committed) {
            s->committed = true;
            int n = s->mission;
            bool host = !s->net || s->my_owner == 0;
            bool done = session_completed(s);
            if (done) {
                s->first_clear = !(save->mission_paid & (1ull << (n - 1)));
                if (host) save_complete_mission(save, n);
                else {
                    save->mission_paid |= 1ull << (n - 1);
                    if (s->first_clear) for (int k = 0; k < MISSIONS[n - 1].nrewards; k++) save_grant(save, MISSIONS[n - 1].rewards[k]);
                }
            }
            s->payout_coins = !done ? 10 : (s->first_clear ? mission_first_coins(n) : mission_repeat_coins(n));
            s->payout_credits = 0;
            s->has_payout = true;
            save_earn(save, s->payout_coins, 0);
        }
        break;
    case SESS_TICKET:
        if (session_over(s) && !s->committed) {
            s->committed = true;
            if (session_completed(s)) {
                save->tickets_won |= (uint16_t)(1u << s->ticket);
                save->ticket_active = -1;
                for (int k = 0; k < 4; k++) save_grant(save, TICKET_REWARDS[s->ticket][k]);
                s->payout_coins = 600;
            } else {
                s->payout_coins = 10;
            }
            s->payout_credits = 0;
            s->has_payout = true;
            save_earn(save, s->payout_coins, 0);
        }
        break;
    case SESS_MARTYR:
        if (s->won_phase && !session_completed(s) && !s->replay && save->martyr_phase < s->wins[0]) save->martyr_phase = (uint8_t)s->wins[0];
        if (session_completed(s) && !s->committed) {
            s->committed = true;
            if (!s->replay) {
                save->martyr_won = true;
                save->martyr_phase = 3;
                for (int i = 0; i < N_MARTYR_REWARDS; i++) save_grant(save, MARTYR_REWARDS[i]);
                s->payout_coins = 25000;
                s->payout_credits = 0;
                s->has_payout = true;
                save_earn(save, 25000, 0);
                save->settings.home_mode = HOME_ETERNAL;
            }
        }
        break;
    }
}

/* ---------------------------------------------------------------- presentation */
void session_team_name(const Session *s, int team, char *out, int n) {
    out[0] = 0;
    int len = 0;
    for (int i = 0; i < s->nplayers; i++) {
        int tm = s->players[i].team >= 0 ? s->players[i].team : i;
        if (tm != team) continue;
        len += snprintf(out + len, (size_t)(n - len > 0 ? n - len : 0), "%s%s", len ? " + " : "", s->players[i].name);
        if (len >= n) break;
    }
    if (!out[0]) snprintf(out, (size_t)n, "TEAM %d", team + 1);
}

int session_team_player(const Session *s, int team) {
    for (int i = 0; i < s->nplayers; i++) if ((s->players[i].team >= 0 ? s->players[i].team : i) == team) return i;
    return 0;
}

static void pips(char *out, int n, int have, int total, const char *on, const char *off, const char *sep) {
    int len = 0;
    out[0] = 0;
    for (int k = 0; k < total && len < n - 8; k++) len += snprintf(out + len, (size_t)(n - len), "%s%s", k ? sep : "", k < have ? on : off);
}

static const char *const MARTYR_PHASES[3] = {"PHASE I  -  FIRE & EARTH", "PHASE II  -  WIND & WATER", "PHASE III  -  LIGHT & DARK"};
static const char *const MARTYR_TAUNT[3] = {"LET US BEGIN WHERE ALL POWER BEGINS.", "THE SKY ITSELF WILL FIGHT YOU.",
                                            "THIS IS THE END OF TIME. HOLD NOTHING BACK."};
static const char *const MTYPE[6] = {"MARATHON", "2 vs 1", "UFO ASSAULT", "CO-OP", "BOSS RUSH", "MOTHERSHIP"};
const char *mission_type_name(int m) { return MTYPE[MISSIONS[m - 1].kind]; }
static const char *const DIFF_NAMES[4] = {"EASY", "NORMAL", "HARD", "MASTER"};

void mission_objective(int mi, char *out, int n) {
    const TTMission *m = &MISSIONS[mi - 1];
    switch (m->kind) {
    case MK_MARATHON: snprintf(out, (size_t)n, "WIN %d MATCHES IN A ROW", m->stages); break;
    case MK_RUSH: snprintf(out, (size_t)n, "BEAT %d BOSSES, NO LOSSES", m->stages); break;
    case MK_MOTHER: snprintf(out, (size_t)n, "DESTROY THE MOTHERSHIP FLEET"); break;
    case MK_COOP: snprintf(out, (size_t)n, "TEAM UP: %s (%d RDS)", m->coop_ufo ? "DOWN THE SAUCER" : "BEAT BOTH TANKS", m->target); break;
    case MK_UFO: snprintf(out, (size_t)n, "DOWN THE SAUCER (%d %s)", m->target, m->target == 1 ? "RD" : "RDS"); break;
    default: snprintf(out, (size_t)n, "BEAT BOTH TANKS - WIN %d ROUNDS", m->target);
    }
}
void mission_rule_text(int mi, char *out, int n) {
    const TTMission *m = &MISSIONS[mi - 1];
    if (m->kind == MK_MARATHON) snprintf(out, (size_t)n, "LOSE ONE MATCH AND IT'S OVER");
    else if (m->kind == MK_RUSH) snprintf(out, (size_t)n, "LOSE ONE ROUND AND IT'S OVER");
    else snprintf(out, (size_t)n, "%d LOST ROUNDS FAIL IT  \xc2\xb7  %d HEARTS", m->fail_at, m->hp);
}

void session_hud_wins(const Session *s, int i, char *out, int n) {
    char p[96];
    switch (s->type) {
    case SESS_TOURNAMENT: {
        const TournLevel *lv = &TOURNAMENT[s->level - 1];
        int need = lv->wins;
        if (i == 1) {
            int f = level_fail_at(s->level);
            if (f < 0) { snprintf(out, (size_t)n, "BOSS WINS %d", s->wins[1]); return; }
            if (f == 1) { snprintf(out, (size_t)n, "NO LOSSES ALLOWED"); return; }
            int sa = lv_strikes(s->level);
            int st = s->wins[1] < sa ? s->wins[1] : sa;
            pips(p, sizeof p, st, sa, "\xe2\x96\xa0", "\xe2\x96\xa1", "");
            snprintf(out, (size_t)n, "BOSS %d/%d  STRIKES %s", s->wins[1], f, p);
            return;
        }
        if (need <= 5) pips(p, sizeof p, s->wins[0], need, "\xe2\x97\x8f", "\xe2\x97\x8b", "");
        else snprintf(p, sizeof p, "%d", s->wins[0]);
        snprintf(out, (size_t)n, "%s %s %d/%d", lv_consecutive(s->level) ? "STREAK" : "WINS", p, s->wins[0], need);
        return;
    }
    case SESS_MISSION: {
        const TTMission *m = &MISSIONS[s->mission - 1];
        if (i == 1) { snprintf(out, (size_t)n, "LOST %d/%d", s->wins[1], fail_target(s)); return; }
        if (m->kind == MK_RUSH) { snprintf(out, (size_t)n, "BOSS %d/%d  \xc2\xb7  %d AFTER THIS", s->stage + 1, m->stages, m->stages - s->stage - 1); return; }
        if (m->stages > 1) snprintf(out, (size_t)n, "MATCH %d/%d  WINS %d/%d", s->stage + 1, m->stages, s->wins[0], stage_target(s));
        else snprintf(out, (size_t)n, "WINS %d/%d", s->wins[0], stage_target(s));
        return;
    }
    case SESS_TICKET:
        if (i == 0) snprintf(out, (size_t)n, "WINS %d/2", s->wins[0]);
        else snprintf(out, (size_t)n, "LOST %d/2", s->wins[1]);
        return;
    case SESS_MARTYR:
        if (i == 0) snprintf(out, (size_t)n, "PHASE %d/3", s->phase_i + 1 < 3 ? s->phase_i + 1 : 3);
        else snprintf(out, (size_t)n, "THE MARTYR");
        return;
    default:
        if (s->target > 5) { snprintf(out, (size_t)n, "WINS %d/%d", s->wins[i], s->target); return; }
        pips(p, sizeof p, s->wins[i], s->target, "\xe2\x97\x8f", "\xe2\x97\x8b", " ");
        snprintf(out, (size_t)n, "WINS %s", p);
    }
}

void session_hud_title(const Session *s, char *out, int n) {
    const char *mn = MAPDEFS[s->current_map].name;
    switch (s->type) {
    case SESS_TOURNAMENT:
        if (TOURNAMENT[s->level - 1].rule) snprintf(out, (size_t)n, "LEVEL %d \xc2\xb7 ROUND %d \xc2\xb7 %s \xc2\xb7 %s", s->level, s->round_no, mn, level_rule_tag(s->level));
        else snprintf(out, (size_t)n, "LEVEL %d \xc2\xb7 ROUND %d \xc2\xb7 %s", s->level, s->round_no, mn);
        return;
    case SESS_MISSION: snprintf(out, (size_t)n, "MISSION %d \xc2\xb7 %s \xc2\xb7 %s", s->mission, mission_type_name(s->mission), mn); return;
    case SESS_TICKET: snprintf(out, (size_t)n, "TERMINAL TICKET \xc2\xb7 %s \xc2\xb7 %s", TICKETS[s->ticket].name, mn); return;
    case SESS_MARTYR: snprintf(out, (size_t)n, "MARTYR'S TIME MACHINE \xc2\xb7 %s \xc2\xb7 %s", MARTYR_PHASES[s->phase_i < 2 ? s->phase_i : 2], mn); return;
    default: snprintf(out, (size_t)n, "ROUND %d \xc2\xb7 %s", s->round_no, mn);
    }
}

void session_banner_title(const Session *s, char *out, int n) {
    switch (s->type) {
    case SESS_TOURNAMENT: snprintf(out, (size_t)n, "LEVEL %d - ROUND %d", s->level, s->round_no); return;
    case SESS_MISSION: {
        const TTMission *m = &MISSIONS[s->mission - 1];
        if (m->stages > 1) snprintf(out, (size_t)n, "%s %d OF %d", m->kind == MK_MARATHON ? "MATCH" : "BOSS", s->stage + 1, m->stages);
        else snprintf(out, (size_t)n, "MISSION %d - ROUND %d", m->number, s->round_no);
        return;
    }
    case SESS_TICKET: snprintf(out, (size_t)n, "%s - ROUND %d", TICKETS[s->ticket].name, s->round_no); return;
    case SESS_MARTYR: snprintf(out, (size_t)n, "%s", MARTYR_PHASES[s->phase_i < 2 ? s->phase_i : 2]); return;
    default: snprintf(out, (size_t)n, "ROUND %d", s->round_no);
    }
}

void session_banner_note(const Session *s, char *out, int n) {
    out[0] = 0;
    switch (s->type) {
    case SESS_TOURNAMENT: {
        int sa = lv_strikes(s->level), bw = s->wins[1];
        const TournLevel *lv = &TOURNAMENT[s->level - 1];
        if (sa > 0 && bw >= sa) snprintf(out, (size_t)n, "FINAL WARNING - ONE MORE LOSS FAILS THE LEVEL");
        else if (sa > 0 && bw) snprintf(out, (size_t)n, "%d OF %d STRIKES USED", bw, sa);
        else if (lv->rule && s->round_no == 1) snprintf(out, (size_t)n, "%s: %s", level_rule_tag(s->level), level_rule_text(s->level));
        else if (lv_consecutive(s->level) && bw) snprintf(out, (size_t)n, "STREAK BROKEN %dX - BACK TO ZERO", bw);
        return;
    }
    case SESS_MISSION: {
        const TTMission *m = &MISSIONS[s->mission - 1];
        if (m->kind == MK_MOTHER && s->round_no == 1) snprintf(out, (size_t)n, "THE MOTHERSHIP IS SHIELDED - DESTROY THE DRONES FIRST");
        else if (m->kind == MK_MARATHON) snprintf(out, (size_t)n, "VS %s  \xc2\xb7  %s", s->players[s->nplayers - 1].name, DIFF_NAMES[session_stage_diff(s)]);
        else if (m->kind == MK_RUSH) snprintf(out, (size_t)n, "VS %s", s->players[s->nplayers - 1].name);
        else if (s->wins[1] && s->wins[1] >= fail_target(s) - 1) snprintf(out, (size_t)n, "FINAL WARNING - ONE MORE LOST ROUND FAILS THE MISSION");
        return;
    }
    case SESS_TICKET: if (s->round_no == 1) snprintf(out, (size_t)n, "%s", TICKETS[s->ticket].taunt); return;
    case SESS_MARTYR: snprintf(out, (size_t)n, "%s", MARTYR_TAUNT[s->phase_i < 2 ? s->phase_i : 2]); return;
    default: return;
    }
}

void session_round_summary(const Session *s, int winner, RoundSummary *rs) {
    memset(rs, 0, sizeof(*rs));
    char n0[48], n1[48];
    switch (s->type) {
    case SESS_TOURNAMENT: {
        const TournLevel *lv = &TOURNAMENT[s->level - 1];
        int need = lv->wins, bw = s->wins[1], f = level_fail_at(s->level), sa = lv_strikes(s->level);
        if (session_completed(s)) snprintf(rs->next, sizeof rs->next, "LEVEL CLEARED!");
        else if (session_failed(s)) {
            if (f == 1) snprintf(rs->next, sizeof rs->next, "LEVEL FAILED - YOU LOST A ROUND");
            else snprintf(rs->next, sizeof rs->next, "LEVEL FAILED - %s WON %d ROUNDS", lv->boss, f);
        } else if (winner == 0) snprintf(rs->next, sizeof rs->next, "%d MORE TO GO", need - s->wins[0]);
        else if (lv_consecutive(s->level)) snprintf(rs->next, sizeof rs->next, "STREAK BROKEN - BACK TO ZERO");
        else if (bw >= sa) snprintf(rs->next, sizeof rs->next, "STRIKE %d OF %d - ONE MORE LOSS FAILS THE LEVEL", sa, sa);
        else snprintf(rs->next, sizeof rs->next, "STRIKE %d OF %d - WARNING", bw, sa);
        snprintf(rs->title, sizeof rs->title, "LEVEL %d - ROUND %d", lv->number, s->round_no);
        if (winner == 0) snprintf(rs->headline, sizeof rs->headline, "YOU WIN THE ROUND");
        else snprintf(rs->headline, sizeof rs->headline, "%s WINS THE ROUND", lv->boss);
        if (f > 0) snprintf(rs->score, sizeof rs->score, "%d/%d  \xc2\xb7  BOSS %d/%d", s->wins[0], need, bw, f);
        else snprintf(rs->score, sizeof rs->score, "%d/%d  \xc2\xb7  BOSS %d", s->wins[0], need, bw);
        level_objective(s->level, rs->sub, sizeof rs->sub);
        return;
    }
    case SESS_MISSION: {
        const TTMission *m = &MISSIONS[s->mission - 1];
        if (session_completed(s)) snprintf(rs->next, sizeof rs->next, "MISSION COMPLETE!");
        else if (session_failed(s)) snprintf(rs->next, sizeof rs->next, "MISSION FAILED");
        else if (s->stage_cleared) {
            int left = m->stages - s->stage - 1;
            if (m->kind == MK_MARATHON) snprintf(rs->next, sizeof rs->next, "MATCH WON - %d MORE TO GO", left);
            else snprintf(rs->next, sizeof rs->next, "BOSS WON - %d BOSS%s LEFT IN THE GAUNTLET", left, left != 1 ? "ES" : "");
        } else if (winner == 0) snprintf(rs->next, sizeof rs->next, "%d MORE TO GO", stage_target(s) - s->wins[0]);
        else snprintf(rs->next, sizeof rs->next, "ROUND LOST - %d MORE FAILS THE MISSION", fail_target(s) - s->wins[1]);
        if (m->kind == MK_RUSH) {
            int beaten = s->stage + ((s->stage_cleared || session_completed(s)) ? 1 : 0);
            snprintf(rs->score, sizeof rs->score, "BOSSES BEATEN %d/%d  \xc2\xb7  %d LEFT", beaten, m->stages, m->stages - beaten);
        } else if (m->kind == MK_MARATHON) {
            snprintf(rs->score, sizeof rs->score, "MATCH %d/%d  \xc2\xb7  ROUNDS %d-%d", s->stage + 1, m->stages, s->wins[0], s->wins[1]);
        } else {
            snprintf(rs->score, sizeof rs->score, "%d/%d  \xc2\xb7  LOST %d/%d", s->wins[0], stage_target(s), s->wins[1], fail_target(s));
        }
        snprintf(rs->title, sizeof rs->title, "MISSION %d - ROUND %d", m->number, s->round_no);
        snprintf(rs->headline, sizeof rs->headline, winner == 0 ? "YOUR TEAM WINS THE ROUND" : "THE ENEMY WINS THE ROUND");
        mission_objective(s->mission, rs->sub, sizeof rs->sub);
        return;
    }
    case SESS_TICKET: {
        const TicketBoss *b = &TICKETS[s->ticket];
        if (session_completed(s)) snprintf(rs->next, sizeof rs->next, "BOSS DEFEATED!");
        else if (session_failed(s)) snprintf(rs->next, sizeof rs->next, "TICKET BATTLE LOST - TRY AGAIN, THE TICKET IS KEPT");
        else if (winner == 0) snprintf(rs->next, sizeof rs->next, "%d MORE TO GO", 2 - s->wins[0]);
        else snprintf(rs->next, sizeof rs->next, "ROUND LOST - ONE MORE FAILS THE BATTLE");
        snprintf(rs->title, sizeof rs->title, "%s - ROUND %d", b->name, s->round_no);
        if (winner == 0) snprintf(rs->headline, sizeof rs->headline, "YOU WIN THE ROUND");
        else snprintf(rs->headline, sizeof rs->headline, "%s WINS THE ROUND", b->name);
        snprintf(rs->score, sizeof rs->score, "%d/2  \xc2\xb7  LOST %d/2", s->wins[0], s->wins[1]);
        snprintf(rs->sub, sizeof rs->sub, "WIN TWO ROUNDS");
        return;
    }
    case SESS_MARTYR: {
        int p = s->phase_i < 2 ? s->phase_i : 2;
        if (session_completed(s)) snprintf(rs->next, sizeof rs->next, "THE MARTYR FALLS.");
        else if (winner == 0) snprintf(rs->next, sizeof rs->next, "NEXT: %s", MARTYR_PHASES[p + 1 < 2 ? p + 1 : 2]);
        else snprintf(rs->next, sizeof rs->next, "YOU FALL - THE PHASE IS REPEATED. YOUR PROGRESS IS SAVED.");
        snprintf(rs->title, sizeof rs->title, "%s", MARTYR_PHASES[p]);
        snprintf(rs->headline, sizeof rs->headline, winner == 0 ? "PHASE CLEARED" : "THE MARTYR PREVAILS");
        snprintf(rs->score, sizeof rs->score, "PHASES CLEARED %d/3", s->wins[0]);
        snprintf(rs->sub, sizeof rs->sub, "ESCALATING: HIS POWER GROWS EACH PHASE");
        return;
    }
    default:
        session_team_name(s, 0, n0, sizeof n0);
        session_team_name(s, 1, n1, sizeof n1);
        snprintf(rs->title, sizeof rs->title, "ROUND %d COMPLETE", s->round_no);
        snprintf(rs->headline, sizeof rs->headline, "%s WINS THE ROUND", winner == 0 ? n0 : n1);
        snprintf(rs->score, sizeof rs->score, "%d   \xe2\x80\x94   %d", s->wins[0], s->wins[1]);
        snprintf(rs->sub, sizeof rs->sub, "%s  vs  %s", n0, n1);
        if (session_over(s)) snprintf(rs->next, sizeof rs->next, "MATCH DECIDED");
        else snprintf(rs->next, sizeof rs->next, "NEXT: ROUND %d - NEW BATTLEFIELD", s->round_no + 1);
    }
}
