#include <math.h>
#include <stdio.h>
#include <string.h>
#include "battle.h"
#include "xmath.h"

#define INTRO_TIME 1.8
static const RGB C_PRIMARY = {96, 232, 212}, C_DANGER = {255, 92, 92}, C_AMBER = {255, 184, 64}, C_MUTED = {104, 124, 140};
static const RGB C_GOLD = {255, 206, 100}, C_BGOLD = {255, 214, 90}, C_VIOLET = {176, 120, 255}, C_WHITE = {255, 255, 255};
enum { F_BOOM = 1, F_BURST = 2, F_CARVE = 4, F_DAMAGE = 8, F_START = 16, F_BANNER = 32 };

static double clampd(double v, double lo, double hi) { return py_clamp(v, lo, hi); }
static double ease_in_out(double t) { t = clampd(t, 0.0, 1.0); return t * t * (3 - 2 * t); }
static double lerp(double a, double b, double t) { return a + (b - a) * t; }

Tank *battle_current(Battle *b) { return &b->w.tanks[b->turn]; }

/* ---------------------------------------------------------------- visual helpers */
static void floater(Battle *b, double x, double y, const char *text, RGB c) {
    if (b->nfloaters >= MAX_FLOATERS) {
        memmove(b->floaters, b->floaters + 1, sizeof(FloatText) * (MAX_FLOATERS - 1));
        b->nfloaters--;
    }
    FloatText *f = &b->floaters[b->nfloaters++];
    f->x = x; f->y = y; f->color = c; f->age = 0; f->life = 1.4;
    snprintf(f->text, sizeof f->text, "%s", text);
}
static void banner(Battle *b, const char *text, const char *sub, RGB c, double life) {
    snprintf(b->banner.text, sizeof b->banner.text, "%s", text);
    snprintf(b->banner.sub, sizeof b->banner.sub, "%s", sub);
    b->banner.color = c;
    b->banner.life = life;
    b->banner.age = 0;
    b->banner.on = true;
}
static void explosion(Battle *b, double x, double y, double r, RGB c, double dur, int style) {
    if (b->nexplosions >= MAX_EXPLOSIONS) return;
    Explosion *e = &b->explosions[b->nexplosions++];
    e->x = x; e->y = y; e->radius = r; e->accent = c; e->duration = dur; e->age = 0; e->style = (uint8_t)style;
}
static void kick(Battle *b, double m) { if (b->settings->effects && m > b->shake) b->shake = m; }
static FxReq *fx(Battle *b, int type) {
    if (b->nfxq >= MAX_FXQ) return NULL;
    FxReq *r = &b->fxq[b->nfxq++];
    memset(r, 0, sizeof(*r));
    r->type = (uint8_t)type;
    return r;
}
static void fx_impact(Battle *b, double x, double y, RGB accent, double scale, int ammo) {
    FxReq *r = fx(b, FX_IMPACT);
    if (!r) return;
    r->x = x; r->y = y; r->c0 = accent; r->c1 = THEMES[MAPDEFS[b->w.map].theme].soil; r->p = scale; r->ammo_boom = AMMOS[ammo].boom;
}
static void fx_sound(Battle *b, int kind) { FxReq *r = fx(b, FX_SOUND); if (r) r->a = (uint8_t)kind; }

/* ---------------------------------------------------------------- helpers */
int battle_control(const Battle *b, const Tank *t) {
    const Session *s = b->s;
    if (!s->net) return t->is_ai ? CTL_AI : CTL_LOCAL;
    if (t->is_ai) return s->my_owner == 0 ? CTL_AI : CTL_REMOTE;
    return t->owner == s->my_owner ? CTL_LOCAL : CTL_REMOTE;
}

static int target_for(const Battle *b, const Tank *me) {
    int best = -1;
    double bd = 0, bh = 0;
    for (int i = 0; i < b->w.ntanks; i++) {
        const Tank *t = &b->w.tanks[i];
        if (t->team == me->team || !tank_alive(t)) continue;
        double d = fabs(t->x - me->x);
        if (best < 0 || d < bd || (d == bd && t->hp < bh)) { best = i; bd = d; bh = t->hp; }
    }
    return best >= 0 ? best : 0;
}

void battle_turn_text(const Battle *b, int i, char *out, int n) {
    const Tank *t = &b->w.tanks[i];
    if (t->kind == KIND_UFO) { snprintf(out, (size_t)n, "UFO TURN"); return; }
    if (t->is_ai) {
        int na = 0, k = 0;
        for (int j = 0; j < b->w.ntanks; j++) if (b->w.tanks[j].is_ai) na++;
        for (int j = 0; j <= i; j++) if (b->w.tanks[j].is_ai) k++;
        if (na == 1) snprintf(out, (size_t)n, "AI TURN");
        else snprintf(out, (size_t)n, "AI %d TURN", k);
        return;
    }
    int nh = 0, k = 0;
    for (int j = 0; j < b->w.ntanks; j++) if (!b->w.tanks[j].is_ai) { if (j == i) k = nh; nh++; }
    if (nh == 1) snprintf(out, (size_t)n, "PLAYER TURN");
    else snprintf(out, (size_t)n, "PLAYER %d TURN", k + 1);
}

void battle_wind_text(const Battle *b, char *out, int n) {
    double w = b->w.wind, m = b->wind_setting;
    out[0] = 0;
    if (m <= 0) return;
    if (fabs(w) < 0.05) { snprintf(out, (size_t)n, "WIND CALM"); return; }
    double f = fabs(w) / m;
    int k = 1 + (int)((f < 0.999 ? f : 0.999) * 4);
    char arrows[32] = {0};
    for (int i = 0; i < k; i++) strcat(arrows, w < 0 ? "\xe2\x97\x84" : "\xe2\x96\xba");
    snprintf(out, (size_t)n, "WIND %s %.1f", arrows, fabs(w));
}

void battle_state_text(const Battle *b, char *out, int n) {
    static const char *const T[] = {"DEPLOYING", "AIMING", "AI THINKING", "PROJECTILE AWAY", "IMPACT", "TERRAIN SETTLING", "TARGET DESTROYED",
                                    "THE MARTYR'S BLESSING"};
    if (b->phase == PH_AI_THINKING) {
        int d = (int)(b->time * 3) % 4;
        snprintf(out, (size_t)n, "AI THINKING%.*s", d, "...");
        return;
    }
    snprintf(out, (size_t)n, "%s", T[b->phase]);
}

static void make_order(Battle *b) {
    int teams[MAX_TANKS], nt = 0;
    for (int i = 0; i < b->w.ntanks; i++) {
        int tm = b->w.tanks[i].team;
        bool seen = false;
        for (int k = 0; k < nt; k++) if (teams[k] == tm) seen = true;
        if (!seen) teams[nt++] = tm;
    }
    for (int i = 1; i < nt; i++) for (int j = i; j > 0 && teams[j - 1] > teams[j]; j--) { int t = teams[j]; teams[j] = teams[j - 1]; teams[j - 1] = t; }
    int lists[MAX_TANKS][MAX_TANKS], len[MAX_TANKS] = {0};
    for (int k = 0; k < nt; k++)
        for (int i = 0; i < b->w.ntanks; i++) if (b->w.tanks[i].team == teams[k]) lists[k][len[k]++] = i;
    int start = session_starting_player(b->s) % nt;
    int maxl = 0;
    for (int k = 0; k < nt; k++) if (len[k] > maxl) maxl = len[k];
    b->norder = 0;
    for (int kk = 0; kk < maxl; kk++)
        for (int q = 0; q < nt; q++) {
            int l = (start + q) % nt;
            if (kk < len[l]) b->order[b->norder++] = lists[l][kk];
        }
    b->turn = b->order[0];
}

/* ---------------------------------------------------------------- setup */
void battle_init(Battle *b, Session *s, SaveData *save, const Settings *settings) {
    memset(b, 0, sizeof(*b));
    b->practice = s->type == SESS_PRACTICE;
    b->pr_ver = -1;
    b->s = s;
    b->save = save;
    b->settings = settings;
    b->doomed = -1;
    b->wind_setting = s->has_wind_override ? s->wind_override : settings->wind;
    uint32_t seed = session_round_seed(s);
    world_init(&b->w, FIELD_W, FIELD_H, s->current_map, seed, s->players, s->nplayers, b->wind_setting);
    b->w.ammo_fx = s->ammo_fx;
    if (!s->blessing) {
        for (int i = 0; i < s->nplayers; i++)
            if (s->players[i].ammo == AMMO_BLESSING) {
                s->players[i].ammo = AMMO_STANDARD;
                b->w.tanks[i].ammo = AMMO_STANDARD;
                b->bless_blocked = true;
            }
    }
    rng_seed(&b->srng, seed ^ 0xA11Eu);
    make_order(b);
    for (int i = 0; i < b->w.ntanks; i++) {
        if (!b->w.tanks[i].is_ai) continue;
        const PlayerSetup *pl = &s->players[i];
        uint32_t aseed = rng_below(&s->rng, 1u << 30);
        ai_init(&b->ai[i], pl->diff >= 0 ? pl->diff : s->difficulty, aseed, s->ai_precision * pl->prec);
        b->has_ai[i] = true;
    }
    /* intro banner (BattleScene.enter) */
    char wt[48], sub[128], note[96], title[48], full[128];
    battle_wind_text(b, wt, sizeof wt);
    snprintf(sub, sizeof sub, "%s%s%s", MAPDEFS[s->current_map].name, wt[0] ? "   \xc2\xb7   " : "", wt);
    bool human_mech = false;
    for (int i = 0; i < b->w.ntanks; i++) if (!b->w.tanks[i].is_ai && AMMOS[b->w.tanks[i].ammo].mf) human_mech = true;
    if (!s->ammo_fx && human_mech) strncat(sub, "   \xc2\xb7   SPECIAL AMMO OFF", sizeof sub - strlen(sub) - 1);
    if (b->bless_blocked) strncat(sub, "   \xc2\xb7   MARTYR'S BLESSING OFF HERE", sizeof sub - strlen(sub) - 1);
    session_banner_note(s, note, sizeof note);
    session_banner_title(s, title, sizeof title);
    bool warn = !strncmp(note, "FINAL", 5);
    if (note[0]) snprintf(full, sizeof full, "%s   \xc2\xb7   %s", note, sub);
    else snprintf(full, sizeof full, "%s", sub);
    banner(b, title, full, warn ? C_DANGER : C_PRIMARY, INTRO_TIME * (warn ? 1.4 : 1.0));
    b->phase = PH_INTRO;
    b->t = 0;
}

/* ---------------------------------------------------------------- turn flow */
static void begin_turn(Battle *b, bool volley);
static void fire(Battle *b, bool announce);

static void practice_end_turn(Battle *b) {
    World *w = &b->w;
    Tank *me = &w->tanks[0], *tgt = &w->tanks[1];
    bool hit = tgt->hp < tgt->max_hp;
    b->pr_shots++;
    b->pr_hits += hit;
    b->pr_streak = hit ? b->pr_streak + 1 : 0;
    int ver = w->terrain.version;
    if (b->pr_ver != -1 && ver != b->pr_ver) b->pr_craters++;
    b->pr_ver = ver;
    if (hit) snprintf(b->pr_note, sizeof b->pr_note, "DIRECT HIT!");
    else if (!b->pr_has_first) snprintf(b->pr_note, sizeof b->pr_note, "OUT OF BOUNDS - try less power");
    else {
        double err = (b->pr_first_x - tgt->x) * (tgt->x >= me->x ? 1 : -1);
        snprintf(b->pr_note, sizeof b->pr_note, "%s by %.0f - %s", err > 0 ? "LONG" : "SHORT", fabs(err), err > 0 ? "lower the power or angle" : "add power");
    }
    b->pr_has_first = false;
    me->hp = me->max_hp;
    tgt->hp = tgt->max_hp;
    tgt->charge = me->charge = 0;
    b->doomed = -1;
    b->ndoomed_more = 0;
    world_drift_wind(w);
    begin_turn(b, false);
}

static void end_turn(Battle *b) {
    if (b->practice) { practice_end_turn(b); return; }
    int pos = 0;
    for (int i = 0; i < b->norder; i++) if (b->order[i] == b->turn) pos = i;
    for (int k = 1; k <= b->norder; k++) {
        int i = b->order[(pos + k) % b->norder];
        if (tank_alive(&b->w.tanks[i])) { b->turn = i; break; }
    }
    world_drift_wind(&b->w);
    begin_turn(b, false);
}

static void forfeit(Battle *b, Tank *tank, bool announce) {
    int need = b->s->special_wait;
    if (tank->charge >= need) {
        floater(b, tank->x, tank->y + tank_h(tank) + 8, "SPECIAL ALREADY READY - FIRE!", C_AMBER);
        return;
    }
    tank->charge++;
    char txt[40];
    if (tank->charge >= need) snprintf(txt, sizeof txt, "SPECIAL READY!");
    else snprintf(txt, sizeof txt, "CHARGING %d/%d", tank->charge, need);
    floater(b, tank->x, tank->y + tank_h(tank) + 8, txt, C_GOLD);
    if (announce && b->send) b->send(b->send_ctx, 's', tank->index, 0, 0);
    end_turn(b);
}

/* UFO behaviour (l.8844) */
static bool turn_start(Battle *b, Tank *tank) {
    if (tank->kind != KIND_UFO) return false;
    World *w = &b->w;
    tank->turns++;
    double lo = tank->team == 1 ? 0.52 : 0.07, hi = tank->team == 1 ? 0.93 : 0.48;
    double old = tank->x;
    int half = (int)(tank_w(tank) / 2);
    double cand = tank->x;
    for (int tries = 0; tries < 8; tries++) {
        cand = (int)clampd(rng_uniform(&b->srng, lo, hi) * w->width, 12 + half, w->width - 13 - half) + 0.5;
        bool clear = true;
        for (int i = 0; i < w->ntanks; i++) {
            Tank *o = &w->tanks[i];
            if (o == tank || !tank_alive(o) || o->kind != KIND_UFO) continue;
            if (!(fabs(cand - o->x) >= (tank_w(tank) + tank_w(o)) / 2 + 3)) clear = false;
        }
        if (clear) break;
    }
    tank->x = cand;
    tank->vis_dx = old - tank->x;
    terrain_flatten(&w->terrain, tank->x, 10);
    tank->y = (double)terrain_support(&w->terrain, tank->x) + tank->hover;
    double frac = (double)tank->hp / tank->max_hp;
    static const double TH[2] = {0.66, 0.33};
    bool drone = !strncmp(tank->name, "DRONE", 5);
    while (tank->phase < 2 && frac <= TH[tank->phase]) {
        tank->phase++;
        if (drone) continue;
        tank->shield = 2;
        floater(b, tank->x, tank->y + tank_h(tank) + 8, "SHIELDS UP", (RGB){140, 230, 255});
    }
    b->guide_version++;
    if (tank->turns % 2 == 1) {
        floater(b, tank->x, tank->y + tank_h(tank) + 14, "CHARGING...", (RGB){150, 255, 200});
        return true;
    }
    int period = frac > 0.5 ? 3 : 2;
    if ((tank->turns / 2) % period == 0) {
        tank->volley = 2;
        floater(b, tank->x, tank->y + tank_h(tank) + 14, "VOLLEY!", C_DANGER);
    }
    return false;
}

/* MartyrSession.on_turn_start (l.12671) */
static void on_turn_start(Battle *b, Tank *tank) {
    if (b->s->type != SESS_MARTYR || tank->index != 1) return;
    int ph = b->s->phase_i < 2 ? b->s->phase_i : 2;
    int ne = 0;
    while (ne < 7 && PHASE_ELEMS[ph][ne] != 255) ne++;
    int el = PHASE_ELEMS[ph][rng_below(&b->srng, (uint32_t)ne)];
    int a = MAGE_AMMO_IDX[el][ph];
    const char *name = ELEMENTS[el].name;
    if (rng_random(&b->srng) < 0.7) {
        int pool[64], n = 0;
        for (int i = AMMOS_BASE_FIRST + 5; i < AMMOS_BASE_FIRST + AMMOS_BASE_COUNT; i++) pool[n++] = i;
        for (int i = AMMOS_TICKET_FIRST; i < AMMOS_TICKET_FIRST + AMMOS_TICKET_COUNT; i++) pool[n++] = i;
        for (int i = AMMOS_ANCIENT_FIRST; i < AMMOS_ANCIENT_FIRST + AMMOS_ANCIENT_COUNT; i++) if (i != AMMO_BLESSING) pool[n++] = i;
        a = pool[rng_below(&b->srng, (uint32_t)n)];
        name = AMMOS[a].name;
    }
    tank->ammo = (int16_t)a;
    tank->shot_color = ELEMENTS[el].color;
    char txt[40];
    snprintf(txt, sizeof txt, "%s!", name);
    floater(b, tank->x, tank->y + tank_h(tank) + 14, txt, ELEMENTS[el].color);
}

static void begin_turn(Battle *b, bool volley) {
    if (b->practice) b->turn = 0;
    Tank *tank = battle_current(b);
    b->guide_version++;
    if (!volley && turn_start(b, tank)) {
        banner(b, "UFO CHARGING", "", tank->color, 0.8);
        end_turn(b);
        return;
    }
    if (!volley) on_turn_start(b, tank);
    Session *s = b->s;
    bool ready = s->special && tank->charge >= s->special_wait;
    char tt[32], wt[48];
    battle_turn_text(b, b->turn, tt, sizeof tt);
    battle_wind_text(b, wt, sizeof wt);
    banner(b, tt, ready ? "SPECIAL READY - FIRE FOR 3 DAMAGE!" : wt, tank->color, volley ? 0.9 : 1.3);
    int ctl = battle_control(b, tank);
    b->aim_sent_valid = false;
    if (ctl == CTL_AI && s->mode == MODE_SINGLE && s->special && !s->net && !volley && b->has_ai[tank->index] && tank->kind != KIND_UFO &&
        tank->charge < s->special_wait && rng_random(&b->srng) < 0.2) {
        forfeit(b, tank, false);
        return;
    }
    if (ctl == CTL_AI && b->has_ai[tank->index]) {
        ai_plan_begin(&b->ai[tank->index], &b->w, tank->index, target_for(b, tank));
        b->ai_pending = true;
        b->ai_wait = 0;
        b->has_plan = false;
        b->plan_from[0] = tank->angle;
        b->plan_from[1] = tank->power;
        b->phase = PH_AI_THINKING;
        b->t = 0;
    } else {
        b->phase = PH_AIMING;
        b->t = 0;
    }
}

static void refresh_guards(Battle *b) {
    World *w = &b->w;
    for (int i = 0; i < w->ntanks; i++) {
        Tank *t = &w->tanks[i];
        if (!t->guard || !tank_alive(t)) continue;
        bool mates = false;
        for (int j = 0; j < w->ntanks; j++) if (j != i && tank_alive(&w->tanks[j]) && w->tanks[j].team == t->team) mates = true;
        if (mates) { if (t->shield < 2) t->shield = 2; }
        else {
            t->guard = false;
            t->shield = 0;
            floater(b, t->x, t->y + tank_h(t) + 8, "SHIELD DOWN!", (RGB){140, 230, 255});
        }
    }
}

/* ---------------------------------------------------------------- firing */
static void fire(Battle *b, bool announce) {
    World *w = &b->w;
    Tank *tank = battle_current(b);
    Session *s = b->s;
    double ox, oy;
    tank_muzzle(tank, tank->angle, &ox, &oy);
    if (tank->ammo == AMMO_BLESSING && !s->blessing) tank->ammo = AMMO_STANDARD;
    b->blessing_shot = tank->ammo == AMMO_BLESSING;
    b->special_shot = !b->blessing_shot && s->special && tank->charge >= s->special_wait;
    if (b->blessing_shot) floater(b, tank->x, tank->y + tank_h(tank) + 8, "MARTYR'S BLESSING!", C_GOLD);
    if (b->special_shot) {
        tank->charge = 0;
        floater(b, tank->x, tank->y + tank_h(tank) + 8, "SPECIAL ATTACK!", C_GOLD);
        if (AMMOS[tank->ammo].mf) floater(b, tank->x, tank->y + tank_h(tank) + 14, "AMMO EFFECT SKIPPED", C_MUTED);
    } else if (AMMOS[tank->ammo].mf && !w->ammo_fx && !tank->is_ai && !(b->fx_warned & (1u << tank->index))) {
        b->fx_warned |= 1u << tank->index;
        floater(b, tank->x, tank->y + tank_h(tank) + 8, "SPECIAL AMMO IS OFF HERE", C_MUTED);
    }
    plan_shot(w, tank, b->special_shot, &b->shot);
    b->nflights = 0;
    for (int i = 0; i < b->shot.n; i++) {
        LiveFlight *f = &b->flights[b->nflights++];
        f->spec = b->shot.f[i];
        f->delay = f->spec.delay;
        f->t = 0;
        f->x = f->lastx = f->spec.launch.x0;
        f->y = f->lasty = f->spec.launch.y0;
        f->n = 0;
        f->active = true;
    }
    b->npending = 0;
    if (announce && b->send) b->send(b->send_ctx, 'f', tank->index, tank->angle, tank->power);
    tank->recoil = 0.2;
    FxReq *r = fx(b, FX_MUZZLE);
    if (r) { r->x = ox; r->y = oy; r->p = tank->angle; r->c0 = tank->shot_color; }
    kick(b, 0.9);
    b->phase = PH_FLIGHT;
    b->t = 0;
}

static void flight_step(Battle *b, LiveFlight *f, double x, double y) {
    if (py_hypot(x - f->lastx, y - f->lasty) >= 1.0) {
        f->lastx = x;
        f->lasty = y;
        f->n++;
        Tank *cur = battle_current(b);
        int k_n = b->special_shot ? 3 : 1;
        for (int k = 0; k < k_n; k++) {
            FxReq *r = fx(b, FX_TRAIL);
            if (!r) break;
            r->x = x + (k - 1) * 1.5;
            r->y = y + (k - 1);
            r->c0 = cur->shot_color;
            r->ammo_trail = AMMOS[cur->ammo].trail;
            r->n = f->n + k;
        }
    }
}

static void push_pending(Battle *b, const Ev *e) {
    if (b->npending < MAX_PENDING) b->pending[b->npending++] = *e;
}
static Ev pop_pending(Battle *b) {
    Ev e = b->pending[0];
    memmove(b->pending, b->pending + 1, sizeof(Ev) * (size_t)(b->npending - 1));
    b->npending--;
    return e;
}

static void advance_flights(Battle *b, double sdt) {
    for (int i = 0; i < b->nflights; i++) {
        LiveFlight *f = &b->flights[i];
        if (f->delay > 0) { f->delay -= sdt; continue; }
        /* advance_projectile with a no-op probe: sub-steps only drive the trail */
        double t0 = f->t, t1 = f->t + sdt, x1, y1;
        launch_at(&f->spec.launch, t1, &x1, &y1);
        int n = (int)(py_hypot(x1 - f->x, y1 - f->y) / PHYS_SUBSTEP) + 1;
        if (n < 1) n = 1;
        for (int k = 1; k <= n; k++) {
            double x, y;
            launch_at(&f->spec.launch, t0 + sdt * k / n, &x, &y);
            flight_step(b, f, x, y);
        }
        f->t = t1;
        f->x = x1;
        f->y = y1;
        if (f->t >= f->spec.trace.time) {
            for (int e = 0; e < f->spec.nevents; e++) push_pending(b, &f->spec.events[e]);
            memmove(&b->flights[i], &b->flights[i + 1], sizeof(LiveFlight) * (size_t)(b->nflights - i - 1));
            b->nflights--;
            i--;
        }
    }
}

/* ---------------------------------------------------------------- blessing (BlessingFX timeline, l.8447) */
static void bless_kill(Battle *b, Tank *v) {
    if (v->hp <= 0) {
        if (b->doomed == v->index) return;
        for (int i = 0; i < b->ndoomed_more; i++) if (b->doomed_more[i] == v->index) return;
    }
    v->shield = 0;
    v->hp = 0;
    v->hurt = 0.6;
    floater(b, v->x, v->y + tank_h(v) + 6, "JUDGED", C_BGOLD);
    if (b->doomed < 0) b->doomed = v->index;
    else if (v->index != b->doomed) {
        bool have = false;
        for (int i = 0; i < b->ndoomed_more; i++) if (b->doomed_more[i] == v->index) have = true;
        if (!have) b->doomed_more[b->ndoomed_more++] = v->index;
    }
}

static void begin_blessing(Battle *b, const Ev *ev) {
    World *w = &b->w;
    Tank *shooter = battle_current(b);
    b->ev = *ev;
    b->flags = 0;
    b->impact = (Impact){ev->x, ev->y, ev->kind, ev->tank};
    b->nvictims = 0;
    for (int i = 0; i < w->ntanks; i++) if (tank_alive(&w->tanks[i]) && w->tanks[i].team != shooter->team) b->victims[b->nvictims++] = i;
    b->blessing_shot = false;
    b->npending = 0;
    b->nflights = 0;
    Blessing *bl = &b->bless;
    memset(bl, 0, sizeof(*bl));
    double W = w->width, span = W * 0.55;
    int n = 10;
    double c0 = clampd(ev->x - span / 2, 4, (4 > W - 4 - span ? 4 : W - 4 - span));
    double ground[10];
    for (int k = 0; k < n; k++) ground[k] = c0 + span * (k + 0.5) / n;
    for (int i = 1; i < n; i++)                 /* stable sort by distance to the impact */
        for (int j = i; j > 0 && fabs(ground[j - 1] - ev->x) > fabs(ground[j] - ev->x); j--) { double t = ground[j]; ground[j] = ground[j - 1]; ground[j - 1] = t; }
    for (int i = 0; i < b->nvictims && bl->ntargets < 14; i++) {
        Tank *v = &w->tanks[b->victims[i]];
        bl->targets[bl->ntargets++] = (BlessTarget){v->x, v->y + tank_h(v) * 0.5, v->index};
    }
    for (int k = 0; k < n && bl->ntargets < 14; k++)
        if (fabs(ground[k] - shooter->x) >= 14)
            bl->targets[bl->ntargets++] = (BlessTarget){ground[k], (double)w->terrain.heights[(int)clampd(ground[k], 0, W - 1)], -1};
    for (int k = 0; k < 12 && bl->nwipes < 16; k++) {
        double x = 4 + (W - 9) * k / 11;
        if (fabs(x - shooter->x) >= 14) {
            bl->wipes[bl->nwipes][0] = x;
            bl->wipes[bl->nwipes][1] = (double)w->terrain.heights[(int)x];
            bl->nwipes++;
        }
    }
    bl->full = b->settings->effects;
    double tail;
    if (bl->full) { bl->T_SUN = 1.6; bl->T_BANG = 3.0; bl->first = 0.7; bl->gap = 0.1; bl->tel = 0.6; tail = 2.6; }
    else { bl->T_SUN = 99.0; bl->T_BANG = 0.4; bl->first = 0.2; bl->gap = 0.04; bl->tel = 0.2; tail = 0.5; }
    bl->wx = ev->x;
    bl->wy = ev->y;
    double mx = bl->T_BANG;
    for (int i = 0; i < bl->ntargets; i++) {
        bl->arrive[i] = bl->T_BANG + bl->first + bl->gap * i;
        if (i == 0 || bl->arrive[i] > mx) mx = bl->arrive[i];
    }
    bl->T_CONV = mx + 0.45;
    bl->T_END = bl->T_CONV + tail;
    for (int i = 0; i < bl->nwipes; i++) {
        bl->wipe_t[i] = bl->T_CONV + 0.05 + fabs(bl->wipes[i][0] - ev->x) / (bl->full ? 55.0 : 400.0);
        if (bl->wipe_t[i] + 0.7 > bl->T_END) bl->T_END = bl->wipe_t[i] + 0.7;
    }
    bl->on = true;
    b->phase = PH_BLESSING;
    b->t = 0;
}

void battle_skip_blessing(Battle *b) {
    if (b->phase != PH_BLESSING || !b->bless.on) return;
    b->bless.skipped = true;
    if (b->bless.t < b->bless.T_END) b->bless.t = b->bless.T_END;
}

static void blessing_update(Battle *b, double sdt) {
    World *w = &b->w;
    Blessing *bl = &b->bless;
    Tank *cur = battle_current(b);
    bl->t += sdt;
    double R = PHYS_BLAST_RADIUS;
    /* events in key order: bang, hit0.., finale, wipe0.. */
    for (int key = 0; key < 1 + bl->ntargets + 1 + bl->nwipes; key++) {
        double when;
        uint32_t bitn;
        int kind, idx = 0;
        if (key == 0) { when = bl->T_BANG; bitn = 0; kind = 0; }
        else if (key <= bl->ntargets) { idx = key - 1; when = bl->arrive[idx]; bitn = 1 + (uint32_t)idx; kind = 1; }
        else if (key == bl->ntargets + 1) { when = bl->T_CONV; bitn = 15; kind = 2; }
        else { idx = key - bl->ntargets - 2; when = bl->wipe_t[idx]; bitn = 16 + (uint32_t)idx; kind = 3; }
        if ((bl->fired & (1u << bitn)) || bl->t < when) continue;
        bl->fired |= 1u << bitn;
        if (bl->skipped && kind == 0) continue;
        if (kind == 0) {
            explosion(b, b->impact.x, b->impact.y, R * 3.0, C_BGOLD, 1.2, BOOM_BLESSING);
            b->flash = 1.0;
            kick(b, 5.0);
            fx_sound(b, 1);
        } else if (kind == 1) {
            BlessTarget *tg = &bl->targets[idx];
            world_terrain_op(w, TERR_BLAST, tg->x, tg->y, R * (tg->tank >= 0 ? 1.7 : 2.6));
            if (!bl->skipped) {
                explosion(b, tg->x, tg->y, R * 3.2, idx % 2 == 0 ? C_BGOLD : C_VIOLET, 1.1, BOOM_BLESSING);
                fx_impact(b, tg->x, tg->y, C_BGOLD, 1.8, cur->ammo);
                double fl = tg->tank < 0 ? 0.25 : 0.5;
                if (fl > b->flash) b->flash = fl;
                kick(b, tg->tank < 0 ? 3.0 : 5.0);
            }
            if (tg->tank >= 0) bless_kill(b, &w->tanks[tg->tank]);
        } else if (kind == 2) {
            world_terrain_op(w, TERR_BLAST, b->impact.x, b->impact.y, R * 4.2);
            if (!bl->skipped) {
                explosion(b, b->impact.x, b->impact.y, R * 7.5, C_WHITE, 2.0, BOOM_BLESSING);
                b->flash = 1.0;
                kick(b, 11.0);
                fx_sound(b, 1);
            }
        } else {
            world_terrain_op(w, TERR_BLAST, bl->wipes[idx][0], bl->wipes[idx][1], R * 3.4);
            if (!bl->skipped) {
                explosion(b, bl->wipes[idx][0], bl->wipes[idx][1], R * 3.4, (RGB){255, 150, 60}, 1.2, BOOM_BLESSING);
                kick(b, 2.0);
            }
        }
        b->guide_version++;
    }
    if (bl->t >= bl->T_END) {
        for (int i = 0; i < b->nvictims; i++) bless_kill(b, &w->tanks[b->victims[i]]);
        bl->on = false;
        b->phase = PH_SETTLE;
        b->t = 0;
    }
}

/* ---------------------------------------------------------------- impacts */
static int dist_cmp(double da, int ia, double db, int ib) { return da < db ? -1 : da > db ? 1 : (ia < ib ? -1 : ia > ib ? 1 : 0); }

static void start_event(Battle *b, Ev ev) {
    World *w = &b->w;
    Tank *shooter = battle_current(b);
    if (b->practice && ev.first && ev.kind != IMP_OUT) {
        b->pr_has_first = true;
        b->pr_first_x = ev.x;
    }
    if (b->blessing_shot && ev.first) {
        if (ev.kind == IMP_TANK && ev.tank >= 0 && w->tanks[ev.tank].team != shooter->team) {
            begin_blessing(b, &ev);
            return;
        }
        b->blessing_shot = false;
        if (ev.kind != IMP_OUT) floater(b, ev.x, ev.y + 8, "THE BLESSING NEEDS A DIRECT HIT", C_BGOLD);
    }
    b->ev = ev;
    b->flags = 0;
    b->impact = (Impact){ev.x, ev.y, ev.kind, ev.tank};
    b->nvictims = 0;
    double reach = PHYS_SPLASH * ev.radius;
    if (ev.dmg > 0 && ev.kind == IMP_TANK && ev.tank >= 0) {
        b->victims[b->nvictims++] = ev.tank;
    } else if (ev.dmg > 0 && ev.kind == IMP_TERRAIN) {
        double d[MAX_TANKS];
        int idx[MAX_TANKS], n = 0;
        for (int i = 0; i < w->ntanks; i++)
            if (tank_alive(&w->tanks[i])) { d[n] = world_distance_to_tank(&w->tanks[i], ev.x, ev.y); idx[n] = i; n++; }
        for (int i = 1; i < n; i++)
            for (int j = i; j > 0 && dist_cmp(d[j - 1], idx[j - 1], d[j], idx[j]) > 0; j--) {
                double td = d[j]; d[j] = d[j - 1]; d[j - 1] = td;
                int ti = idx[j]; idx[j] = idx[j - 1]; idx[j - 1] = ti;
            }
        int limit = ev.multi ? n : 1;
        int got = 0;
        for (int i = 0; i < n; i++) if (d[i] <= reach) { if (got < limit) b->victims[b->nvictims++] = idx[i]; got++; }
    }
    if (ev.first && b->has_ai[shooter->index] && shooter->is_ai && b->has_plan && battle_control(b, shooter) == CTL_AI) {
        int foe = target_for(b, shooter);
        bool hit = false;
        for (int i = 0; i < b->nvictims; i++) if (b->victims[i] == foe) hit = true;
        ai_observe(&b->ai[shooter->index], &b->plan, &b->impact, &w->tanks[foe], hit);
    }
    if (ev.kind == IMP_OUT && ev.mode != TERR_FX) {
        floater(b, clampd(ev.x, 6, w->width - 6), w->height - 6, "OUT OF BOUNDS", C_MUTED);
        b->flags |= F_BOOM | F_BURST | F_CARVE | F_DAMAGE;
        b->t = 0.6;
    } else {
        b->t = -ev.delay;
    }
    b->phase = PH_IMPACT;
}

static void impact_update(Battle *b, double sdt) {
    World *w = &b->w;
    Ev *ev = &b->ev;
    Impact *imp = &b->impact;
    advance_flights(b, sdt);
    b->t += sdt;
    if (b->t < 0) return;
    Tank *shooter = battle_current(b);
    double R = PHYS_BLAST_RADIUS * ev->radius;
    if (!(b->flags & F_BOOM)) {
        b->flags |= F_BOOM;
        explosion(b, imp->x, imp->y, R > 2.0 ? R : 2.0, shooter->shot_color, 0.95, AMMOS[shooter->ammo].boom);
        if (ev->radius >= 0.5) kick(b, ev->big ? 5.0 : 1.6 * (1.4 < ev->radius ? 1.4 : ev->radius));
        fx_sound(b, 1);
    }
    if (b->t >= 0.06 && !(b->flags & F_BURST)) {
        b->flags |= F_BURST;
        double sc = 0.4 + 0.6 * ev->radius;
        fx_impact(b, imp->x, imp->y, shooter->shot_color, ev->big ? 2.6 : (1.4 < sc ? 1.4 : sc), shooter->ammo);
        if (ev->big) {
            static const double SC[2] = {1.5, 2.1};
            for (int k = 0; k < 2; k++) {
                RGB c = shooter->shot_color;
                double a = 0.3 * k;
                RGB m = {(uint8_t)(c.r + (255 - c.r) * a), (uint8_t)(c.g + (255 - c.g) * a), (uint8_t)(c.b + (255 - c.b) * a)};
                if (a <= 0) m = c;
                explosion(b, imp->x, imp->y, R * SC[k], m, 0.95, AMMOS[shooter->ammo].boom);
            }
        }
    }
    if (b->t >= 0.28 && !(b->flags & F_CARVE)) {
        b->flags |= F_CARVE;
        if (ev->mode != TERR_FX) world_terrain_op(w, ev->mode, imp->x, imp->y, R * (imp->kind == IMP_TANK ? 0.85 : 1.0));
        b->guide_version++;
    }
    if (b->t >= 0.42 && !(b->flags & F_DAMAGE)) {
        b->flags |= F_DAMAGE;
        bool direct = imp->kind == IMP_TANK;
        for (int i = 0; i < b->nvictims; i++) {
            Tank *v = &w->tanks[b->victims[i]];
            int dmg = ev->dmg * ((direct && v->scale > 1.0) ? 2 : 1) * session_dmg_mult(b->s, shooter);
            if (v->shield > 0) { v->shield--; dmg = 0; }
            v->hp -= dmg;
            v->hurt = 0.6;
            char txt[40];
            if (dmg == 0) snprintf(txt, sizeof txt, "%s", v->guard ? "SHIELDED - KILL THE DRONES" : "SHIELD ABSORBED");
            else snprintf(txt, sizeof txt, "%s -%d", direct ? "DIRECT HIT" : "HIT", dmg);
            floater(b, v->x, v->y + tank_h(v) + 6, txt, dmg == 0 ? C_AMBER : C_DANGER);
            kick(b, dmg ? 2.6 : 1.0);
            if (v->hp <= 0) {
                if (b->doomed < 0) b->doomed = v->index;
                else if (v->index != b->doomed) {
                    bool have = false;
                    for (int k = 0; k < b->ndoomed_more; k++) if (b->doomed_more[k] == v->index) have = true;
                    if (!have) b->doomed_more[b->ndoomed_more++] = v->index;
                }
            }
        }
        if (ev->pull > 0) {
            for (int i = 0; i < w->ntanks; i++) {
                Tank *t = &w->tanks[i];
                double dx = imp->x - t->x;
                if (tank_alive(t) && t->kind != KIND_UFO && fabs(dx) <= ev->pull * 2.2) {
                    t->x = clampd(t->x + clampd(dx * 0.6, -ev->pull, ev->pull), 9, w->width - 10) + 0.0;
                    double sup = (double)terrain_support(&w->terrain, t->x) + t->hover;
                    if (sup > t->y) t->y = sup;
                }
            }
            b->guide_version++;
        } else if (ev->pull < 0) {                     /* ATOMIC BREATH: the roar hurls tanks away */
            double P = -ev->pull;
            for (int i = 0; i < w->ntanks; i++) {
                Tank *t = &w->tanks[i];
                double dx = t->x - imp->x;
                if (tank_alive(t) && t->kind != KIND_UFO && fabs(dx) <= P * 2.2) {
                    double shove = (P * 2.2 - fabs(dx)) * 0.65;
                    t->x = clampd(t->x + copysign(P < shove ? P : shove, dx != 0.0 ? dx : 1.0), 9, w->width - 10) + 0.0;
                    double sup = (double)terrain_support(&w->terrain, t->x) + t->hover;
                    if (sup > t->y) t->y = sup;
                    if (t->hurt < 0.3) t->hurt = 0.3;
                }
            }
            b->guide_version++;
        }
    }
    double end_t = (b->nflights || b->npending) ? 0.5 : 1.0;
    if (b->t >= end_t) {
        if (b->npending) start_event(b, pop_pending(b));
        else if (b->nflights) { b->phase = PH_FLIGHT; b->t = 0; }
        else { b->phase = PH_SETTLE; b->t = 0; }
    }
}

int battle_winner_team(const Battle *b) { return b->doomed >= 0 ? 1 - b->w.tanks[b->doomed].team : 0; }

static void destruction_update(Battle *b, double sdt) {
    World *w = &b->w;
    Tank *d = &w->tanks[b->doomed];
    b->t += sdt;
    if (!(b->flags & F_START)) {
        b->flags |= F_START;
        bool any = false;
        for (int i = 0; i < w->ntanks; i++) if (w->tanks[i].team == d->team && tank_alive(&w->tanks[i])) any = true;
        b->wiped = !any;
        if (b->wiped) {
            session_record_round(b->s, 1 - d->team);
            session_commit(b->s, b->save);
        }
    }
    if (b->t < 0.8) d->hurt = 0.6;
    if (b->t >= 0.8 && !(b->flags & F_BOOM)) {
        b->flags |= F_BOOM;
        d->destroyed = true;
        for (int i = 0; i < b->ndoomed_more; i++) w->tanks[b->doomed_more[i]].destroyed = true;
        d->hurt = 0.0;
        Tank *cur = battle_current(b);
        explosion(b, d->x, d->y + 3, 12, d->shot_color, 1.3, AMMOS[cur->ammo].boom);
        FxReq *r = fx(b, FX_DEBRIS);
        if (r) { r->x = d->x; r->y = d->y + 3; r->c0 = d->color; }
        fx_impact(b, d->x, d->y + 3, d->shot_color, 1.8, cur->ammo);
        b->flash = 1.0;
        kick(b, 4.5);
        fx_sound(b, 2);
        b->guide_version++;
    }
    if (b->t >= 1.6 && !(b->flags & F_BANNER)) {
        b->flags |= F_BANNER;
        char sub[48];
        snprintf(sub, sizeof sub, "%s IS OUT", d->name);
        banner(b, "TARGET DESTROYED", sub, d->color, 2.0);
    }
    if (b->wiped && b->t >= 3.4 && !b->finished) {
        b->finished = true;
        b->round_over = true;
    } else if (!b->wiped && b->t >= 2.4) {
        if (b->ndoomed_more) {
            b->doomed = b->doomed_more[0];
            memmove(b->doomed_more, b->doomed_more + 1, sizeof(int) * (size_t)(b->ndoomed_more - 1));
            b->ndoomed_more--;
            b->flags = 0;
            b->t = 0;
        } else {
            b->doomed = -1;
            b->flags = 0;
            end_turn(b);
        }
    }
}

/* ---------------------------------------------------------------- input */
void battle_aim(Battle *b, int da, int dp, bool coarse) {
    if (b->phase != PH_AIMING) return;
    Tank *t = battle_current(b);
    if (battle_control(b, t) != CTL_LOCAL) return;
    int step = coarse ? 5 : 1;
    if (da) t->angle = clampd(t->angle + da * step, 2, 178);
    if (dp) t->power = clampd(t->power + dp * step, PHYS_MIN_POWER, PHYS_MAX_POWER);
    b->guide_version++;
}

void battle_fire(Battle *b) {
    if (b->phase != PH_AIMING || battle_control(b, battle_current(b)) != CTL_LOCAL) return;
    fire(b, true);
}

void battle_forfeit_input(Battle *b) {
    if (b->phase != PH_AIMING || battle_control(b, battle_current(b)) != CTL_LOCAL || !b->s->special) return;
    forfeit(b, battle_current(b), true);
}

void battle_remote_aim(Battle *b, int tank, double angle, double power) {
    Tank *t = &b->w.tanks[tank];
    t->angle = clampd(angle, 2, 178);
    t->power = clampd(power, PHYS_MIN_POWER, PHYS_MAX_POWER);
    b->guide_version++;
}
void battle_remote_fire(Battle *b, int tank, double angle, double power) {
    battle_remote_aim(b, tank, angle, power);
    fire(b, false);
}
void battle_remote_skip(Battle *b, int tank) { forfeit(b, &b->w.tanks[tank], false); }

/* ---------------------------------------------------------------- frame */

/* world.update_effects + BattleScene._tick_visuals */
static void tick_effects(Battle *b, double dt, double sdt) {
    World *w = &b->w;
    w->clock += sdt;
    for (int i = 0; i < w->ntanks; i++) {
        Tank *t = &w->tanks[i];
        if (t->vis_dx != 0.0) {
            double k = 1 - sdt * 3.2;
            t->vis_dx = fabs(t->vis_dx) < 0.05 ? 0.0 : t->vis_dx * (k > 0 ? k : 0.0);
        }
        t->hurt = t->hurt - sdt > 0 ? t->hurt - sdt : 0.0;
        t->recoil = t->recoil - sdt > 0 ? t->recoil - sdt : 0.0;
    }
    double drift = w->wind * 7 * sdt;
    for (int i = 0; i < w->nstreaks; i++) {
        double v = fmod(w->streaks[i][0] + drift, w->width);
        w->streaks[i][0] = v < 0 ? v + w->width : v;
    }
    for (int i = 0; i < b->nexplosions; i++) {
        b->explosions[i].age += sdt;
        if (b->explosions[i].age >= b->explosions[i].duration) {
            b->explosions[i] = b->explosions[--b->nexplosions];
            i--;
        }
    }
    /* _tick_visuals */
    b->shake = b->shake - dt * 7 > 0 ? b->shake - dt * 7 : 0.0;
    b->flash = b->flash - dt * 2.2 > 0 ? b->flash - dt * 2.2 : 0.0;
    for (int i = 0; i < b->nfloaters; i++) {
        b->floaters[i].age += dt;
        if (b->floaters[i].age >= b->floaters[i].life) {
            memmove(&b->floaters[i], &b->floaters[i + 1], sizeof(FloatText) * (size_t)(b->nfloaters - i - 1));
            b->nfloaters--;
            i--;
        }
    }
    if (b->banner.on) {
        b->banner.age += dt;
        if (b->banner.age >= b->banner.life) b->banner.on = false;
    }
}

/* BattleScene.update_ambient: while an overlay (pause, round result) is on top */
void battle_update_ambient(Battle *b, double dt) {
    b->time += dt;
    tick_effects(b, dt, dt);
}

void battle_update(Battle *b, double dt) {
    World *w = &b->w;
    b->time += dt;
    refresh_guards(b);
    double sdt = dt * b->settings->anim_speed;
    tick_effects(b, dt, sdt);
    /* network aim echo (_net_tick, local half) */
    if (b->s->net && b->send) {
        Tank *cur = battle_current(b);
        int ctl = battle_control(b, cur);
        if ((b->phase == PH_AIMING && ctl == CTL_LOCAL) || (b->phase == PH_AI_THINKING && !b->ai_pending)) {
            b->aim_timer -= dt;
            double a = round(cur->angle * 100) / 100, p = round(cur->power * 100) / 100;
            if (b->aim_timer <= 0 && (!b->aim_sent_valid || a != b->aim_sent[0] || p != b->aim_sent[1])) {
                b->aim_timer = 0.08;
                b->aim_sent[0] = a;
                b->aim_sent[1] = p;
                b->aim_sent_valid = true;
                b->send(b->send_ctx, 'a', cur->index, cur->angle, cur->power);
            }
        }
    }
    switch (b->phase) {
    case PH_INTRO:
        b->t += dt;
        if (b->t >= INTRO_TIME) begin_turn(b, false);
        break;
    case PH_AI_THINKING: {
        Tank *tank = battle_current(b);
        if (b->ai_pending) {
            b->ai_wait += sdt;
            if (ai_plan_step(&b->ai[tank->index], b->ai_budget > 0 ? b->ai_budget : 48)) {
                b->plan = b->ai[tank->index].out;
                b->has_plan = true;
                b->ai_pending = false;
                /* the aim tween is flat for p < 0.3, so time already spent planning
                 * can stand in for that idle stretch (DS only; off for parity) */
                if (b->ai_overlap) {
                    double cap = 0.3 * b->plan.think_time;
                    b->t = b->ai_wait < cap ? b->ai_wait : cap;
                }
            }
            break;
        }
        b->t += sdt;
        double p = b->t / b->plan.think_time;
        double k = ease_in_out((p - 0.3) / 0.6);
        tank->angle = lerp(b->plan_from[0], b->plan.angle, k);
        tank->power = lerp(b->plan_from[1], b->plan.power, k);
        b->guide_version++;
        if (p >= 1) {
            tank->angle = b->plan.angle;
            tank->power = b->plan.power;
            fire(b, true);
        }
        break;
    }
    case PH_FLIGHT:
        advance_flights(b, sdt);
        if (b->npending) start_event(b, pop_pending(b));
        break;
    case PH_IMPACT:
        impact_update(b, sdt);
        break;
    case PH_SETTLE: {
        b->t += sdt;
        bool moving = world_settle(w, sdt);
        if (moving) b->guide_version++;
        if (b->t > 0.25 && !moving) {
            if (b->doomed >= 0) {
                int pool[MAX_TANKS + 1], n = 0;
                pool[n++] = b->doomed;
                for (int i = 0; i < b->ndoomed_more; i++) pool[n++] = b->doomed_more[i];
                int cur_team = battle_current(b)->team, pick = pool[0];
                for (int i = 0; i < n; i++) if (w->tanks[pool[i]].team != cur_team) { pick = pool[i]; break; }
                b->doomed = pick;
                b->ndoomed_more = 0;
                for (int i = 0; i < n; i++) if (pool[i] != pick) b->doomed_more[b->ndoomed_more++] = pool[i];
                b->phase = PH_DESTRUCTION;
                b->t = 0;
                b->flags = 0;
            } else if (tank_alive(battle_current(b)) && battle_current(b)->volley > 0) {
                battle_current(b)->volley--;
                begin_turn(b, true);
            } else {
                end_turn(b);
            }
        }
        break;
    }
    case PH_DESTRUCTION:
        destruction_update(b, sdt);
        break;
    case PH_BLESSING:
        blessing_update(b, sdt);
        break;
    }
}
