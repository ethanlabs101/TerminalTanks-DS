/* BattleScene's front end (terminaltanks.py l.8671-9530): HUD, labels, floaters, banner, the MARTYR'S BLESSING overlay,
 * input, touch controls; plus PauseScene, ConfirmLeaveScene, RoundResultScene and the practice range (PracticeScene).
 * The simulation itself is core/battle.c. DS layout: top screen = 2 HUD rows, the 128x80 battlefield drawn 2x
 * (rows 2-21), 2 gauge rows; touch screen = the control panel. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "scenes.h"
#include "ui.h"
#include "render.h"
#include "netplay.h"

static const RGB GOLD = {255, 206, 100};
static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static float ease_out(float t) { t = clampf(t, 0, 1); float u = 1 - t; return 1 - u * u * u; }

static float g_banner_alpha;
static int g_banner_y;
#define FY 2            /* first field row */
#define FROWS 20
#define FCOLS T_COLS

typedef struct {
    Scene base;
    Battle b;
    FieldView fv;
    bool result_pushed;
    float hold_t;        /* touch auto-repeat */
    int hold_id;
    /* practice */
    bool coach, done;
    int lesson, adv;
    float trace_cost;    /* EMA of scanlines per AI trace (adaptive planning budget) */
} BattleScn;

enum { H_ANG_L = 10, H_ANG_R, H_POW_U, H_POW_D, H_FIRE, H_CHARGE, H_MENU, H_COACH, H_NEXT, H_SKIP };

static void cell_of(double x, double y, int *cx, int *cy) {
    *cx = (int)(floor(x) * 2) / CELL_W;
    int py = 16 + (FIELD_H - 1 - (int)floor(y)) * 2;
    *cy = py / CELL_H;
}

/* ------------------------------------------------------------------ practice data (l.9590-9617) */
typedef struct { const char *name, *body, *goal, *tip; } Lesson;
static const Lesson LESSONS[4] = {
    {"1  AIMING & POWER", "Rotate the barrel with \xE2\x97\x84 \xE2\x96\xBA (hold L or R for big steps), set the power with \xE2\x96\xB2 \xE2\x96\xBC, fire with A. A higher angle lobs the shell; more power sends it farther.",
     "HIT THE TARGET ONCE", "Too short? add power. Too long? lower it. Change one thing at a time."},
    {"2  WIND", "Wind pushes every shell and changes each turn. The HUD arrows show its direction and strength; the dotted aim guide ignores it.",
     "FIRE 3 SHOTS AND LAND ONE HIT", "Fire the same aim twice and compare. Aim INTO the wind to compensate."},
    {"3  AMMUNITION", "Ammo types change how a shell flies and bursts. PEBBLES makes a small blast, then three bomblets. Marked \xE2\x9C\xA6 in pilot setup = real effects.",
     "LAND A HIT WITH PEBBLES", "Effects are live in Single/Two Player and LAN (SPECIAL AMMO rule); Tournament and Missions keep ammo cosmetic."},
    {"4  TERRAIN", "Every blast digs a crater and the ground falls away. Dig a lane through a hill, or undermine a tank so it drops.",
     "DIG 2 CRATERS, THEN HIT THE TARGET", "Craters change future shots: a shell now rolls into the hole. Some ammo (MUDBALL, BORER) builds or drills instead."},
};
typedef struct { const char *name; int ammo; double wind; int need; } Challenge;
static Challenge CHALLENGES[3] = {{"GALE SNIPER: 3 hits in a row in a strong wind", AMMO_STANDARD, 10.0, 3},
                                  {"WIND-PROOF: SLUG ammo, 3 hits in a row", AMMO_SLUG, 10.0, 3},
                                  {"LOB SHOT: floaty LOB ammo, 2 hits in a row", AMMO_LOB, 6.0, 2}};
const char *practice_challenge_name(int i) { return CHALLENGES[i].name; }
const char *practice_lesson_name(int i) { return LESSONS[i].name; }

static bool goal_met(const BattleScn *s) {
    const Battle *b = &s->b;
    if (s->adv >= 0) return b->pr_streak >= CHALLENGES[s->adv].need;
    switch (s->lesson) {
    case 1: return b->pr_hits >= 1;
    case 2: return b->pr_shots >= 3 && b->pr_hits >= 1;
    case 3: return b->pr_hits >= 1;
    default: return b->pr_craters >= 2 && b->pr_hits >= 1;
    }
}

#define AI_LINES 220.0f

/* ------------------------------------------------------------------ life cycle */
static void bs_enter(Scene *sc) {
    BattleScn *s = (BattleScn *)sc;
#ifdef TT_DEBUG_MAP   /* test builds only: every battle on one map */
    g_sess.current_map = TT_DEBUG_MAP;
#endif
    battle_init(&s->b, &g_sess, &g_app.save, &g_app.settings);
    if (g_sess.net) net_attach_battle(&s->b);
    field_init(&s->fv, &s->b, &g_app.settings);
    s->b.ai_overlap = true;
    s->trace_cost = 40.0f;
}

static void bs_exit(Scene *sc) {
    (void)sc;
    net_attach_battle(NULL);
}

static void drain_fx(BattleScn *s, float dt) {
    for (int i = 0; i < s->b.nfxq; i++)
        if (s->b.fxq[i].type == FX_SOUND) app_feedback(s->b.fxq[i].a == 2 ? 1 : 0);
    field_update(&s->fv, &s->b, dt);
    s->b.nfxq = 0;
}

static int touch_hot_held(void);
static void bs_handle(Scene *sc, const IEv *e);

static void bs_update(Scene *sc, float dt) {
    BattleScn *s = (BattleScn *)sc;
    if (g_sess.net && !net_battle_tick(&s->b, dt)) return;    /* the link went away: net_battle_tick switched scene */
    bool was_wiped = s->b.wiped;
    uint32_t l0 = plat_lines();
    int ph0 = s->b.phase;
    /* AI planning is spread over frames: spend about AI_LINES scanlines (~14 ms)
     * per frame on traces so the battlefield keeps animating while it thinks */
    bool planning = s->b.phase == PH_AI_THINKING && s->b.ai_pending;
    if (planning) {
        int n = (int)(AI_LINES / s->trace_cost);
        s->b.ai_budget = n < 1 ? 1 : n > 48 ? 48 : n;
    }
    battle_update(&s->b, dt);
    uint32_t l1 = plat_lines();
    if (planning && s->b.ai_pending) {    /* a full slice of grid traces: refine the cost estimate */
        float c = (float)(l1 - l0) / (float)s->b.ai_budget;
        s->trace_cost = s->trace_cost * 0.7f + c * 0.3f;
        if (s->trace_cost < 4.0f) s->trace_cost = 4.0f;
    }
    drain_fx(s, dt);
    uint32_t l2 = plat_lines();
    if (g_sess.net && ph0 != s->b.phase) {   /* lockstep check: both consoles must log the same sequence (tests/harness/run_lan_versus.py) */
        uint32_t h = 2166136261u;
        const World *w = &s->b.w;
        for (int x = 0; x < w->terrain.width; x++) h = (h ^ (uint32_t)w->terrain.heights[x]) * 16777619u;
        for (int i = 0; i < w->ntanks; i++) h = (h ^ (uint32_t)w->tanks[i].hp) * 16777619u;
        plat_log("NET phase %d->%d turn %d state %08lx t=%.2f", ph0, s->b.phase, s->b.turn, (unsigned long)h, s->b.time);
    }
    if (l2 - l0 > 400) plat_log("slow update: sim %lu fx %lu lines (phase %d->%d)", (unsigned long)(l1 - l0), (unsigned long)(l2 - l1), ph0, s->b.phase);
    if (s->b.wiped && !was_wiped) {          /* the session committed: it may have switched the home screen (Master / Eternal) */
        g_app.settings.home_mode = g_app.save.settings.home_mode;
        app_flush();
    }
    if (s->b.practice && !s->done && goal_met(s)) {
        s->done = true;
        save_hear(&g_app.save, STORY_TUT1 + (s->adv >= 0 ? 5 : s->lesson - 1));
        app_flush();
    }
    if (s->b.round_over && !s->result_pushed) {
        s->result_pushed = true;
        app_push(scene_round_result(sc));
    }
    /* touch auto-repeat on the aim / power buttons */
    int id = touch_hot_held();
    if (id >= H_ANG_L && id <= H_POW_D) {
        if (id != s->hold_id) { s->hold_id = id; s->hold_t = 0; }
        s->hold_t += dt;
        if (s->hold_t > 0.3f) {
            int steps = (int)((s->hold_t - 0.3f) * 20) - (int)((s->hold_t - dt - 0.3f) * 20);
            for (int k = 0; k < steps; k++) {
                IEv ev = {A_TOUCH, (g_input.held & (BTN_L | BTN_R)) != 0, id, 0, 0};
                bs_handle(sc, &ev);
            }
        }
    } else {
        s->hold_id = -1;
    }
}

static void bs_update_amb(Scene *sc, float dt) {
    BattleScn *s = (BattleScn *)sc;
    if (g_sess.net) net_pump();
    battle_update_ambient(&s->b, dt);
    drain_fx(s, dt);
}

static void bs_handle(Scene *sc, const IEv *e) {
    BattleScn *s = (BattleScn *)sc;
    Battle *b = &s->b;
    if (e->action == A_BACK || e->action == A_PAUSE || (e->action == A_TOUCH && e->raw == H_MENU)) {
        app_push(scene_pause(sc));
        return;
    }
    if (b->phase == PH_BLESSING) {
        if (ev_confirm(e) || (e->action == A_TOUCH && e->raw == H_SKIP)) battle_skip_blessing(b);
        return;
    }
    if (b->practice && b->phase == PH_AIMING) {
        if ((e->action == A_OTHER && e->raw == K_Y) || (e->action == A_TOUCH && e->raw == H_COACH)) { s->coach = !s->coach; return; }
        if (s->done && ((e->action == A_OTHER && e->raw == K_SELECT) || (e->action == A_TOUCH && e->raw == H_NEXT))) {
            if (s->adv >= 0 || s->lesson >= 4) app_goto(scene_tutorial(s->adv >= 0));
            else app_goto(scene_practice(s->lesson + 1, -1));
            return;
        }
    }
    if (b->phase != PH_AIMING || battle_control(b, battle_current(b)) != CTL_LOCAL) return;
    int act = e->action;
    if (act == A_TOUCH) {
        switch (e->raw) {
        case H_ANG_L: act = A_LEFT; break;
        case H_ANG_R: act = A_RIGHT; break;
        case H_POW_U: act = A_UP; break;
        case H_POW_D: act = A_DOWN; break;
        case H_FIRE: act = A_FIRE; break;
        case H_CHARGE: battle_forfeit_input(b); return;
        default: return;
        }
    }
    if (act == A_OTHER && e->raw == K_X) battle_forfeit_input(b);
    else if (act == A_LEFT) battle_aim(b, +1, 0, e->coarse);
    else if (act == A_RIGHT) battle_aim(b, -1, 0, e->coarse);
    else if (act == A_UP) battle_aim(b, 0, +1, e->coarse);
    else if (act == A_DOWN) battle_aim(b, 0, -1, e->coarse);
    else if (act == A_CONFIRM || act == A_FIRE) battle_fire(b);
}

/* ------------------------------------------------------------------ pixels */
static void bs_px(Scene *sc, Surf *top, Surf *bot) {
    (void)bot;
    BattleScn *s = (BattleScn *)sc;
    uint32_t l0 = plat_lines();
    field_draw(&s->fv, &s->b, (float)g_app.time);
    uint32_t l1 = plat_lines();
    field_blit(&s->fv, &s->b, top, 0, 16);
    uint32_t l2 = plat_lines();
    if (l2 - l0 > 400) plat_log("slow draw: field %lu blit %lu lines (phase %d, parts %d, expl %d)", (unsigned long)(l1 - l0), (unsigned long)(l2 - l1),
                                s->b.phase, s->fv.ps.n, s->b.nexplosions);
    if (s->b.phase == PH_BLESSING && s->b.bless.on) {
        const Blessing *bl = &s->b.bless;
        float conv = bl->full ? clampf((float)((bl->t - bl->T_CONV) / fmax(0.01, bl->T_END - bl->T_CONV)), 0, 1) : 0;
        surf_rect_blend(top, 0, 16, 256, 160, (RGB){46, 12, 104}, 0.40f * clampf((float)bl->t / 0.8f, 0, 1) * (1 - 0.6f * conv));
        if (bl->t > bl->T_END - 0.3) surf_rect_blend(top, 0, 16, 256, 160, P_WHITE, clampf((float)((bl->t - (bl->T_END - 0.3)) / 0.3), 0, 1) * 0.85f);
    }
}

/* ------------------------------------------------------------------ MARTYR'S BLESSING cells (BlessingFX.draw l.8496) */
static const char *const ROMAN[12] = {"XII", "I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX", "X", "XI"};
static const RGB BG_ = {255, 214, 90}, BV = {176, 120, 255}, BP = {104, 46, 180}, BW = {255, 255, 255};
static Term *g_bt;
static void bput(int x, int y, uint32_t ch, RGB col) {
    if (x >= 0 && x < FCOLS && y >= FY && y < FY + FROWS) term_put(g_bt, x, y, ch, col, NULL);
}
static void btext(int x, int y, const char *s, RGB col) {
    while (*s) bput(x++, y, utf8_next(&s), col);
}
static const uint32_t SHADES[4] = {0x2588, 0x2593, 0x2592, 0x2591};
static void bellipse(int px, int py, float ax, float ay, float fade, RGB hot) {
    static const float GT[4] = {0, 0.35f, 0.75f, 1.0f};
    RGB GC[4] = {BW, hot, BV, BP};
    for (int dy = -(int)ay - 1; dy < (int)ay + 2; dy++)
        for (int dx = -(int)ax - 1; dx < (int)ax + 2; dx++) {
            float d = fhypot(dx / fmaxf(0.5f, ax), dy / fmaxf(0.5f, ay));
            if (d <= 1.0f) bput(px + dx, py + dy, SHADES[(int)(d * 4) < 3 ? (int)(d * 4) : 3], cmix(cgradient(GT, GC, 4, d), BP, 0.8f * (1 - fade)));
        }
}
static void bring(int px, int py, float ax, float ay, int n, RGB col, float fade, uint32_t g0, uint32_t g1) {
    for (int j = 0; j < n; j++) {
        float a = j * 6.2831853f / n;
        bput(px + (int)rintf(fcos(a) * ax), py + (int)rintf(fsin(a) * ay), j % 2 ? g1 : g0, cshade(col, fade));
    }
}
static int iclamp(int v, int lo, int hi) { return lo > hi ? (lo + hi) / 2 : (v < lo ? lo : (v > hi ? hi : v)); }

static void draw_blessing(Term *t, const Battle *b) {
    g_bt = t;
    const Blessing *bl = &b->bless;
    float tt = (float)bl->t;
    bool full = bl->full;
    int ry = FROWS / 2 - 1 < 12 ? FROWS / 2 - 1 : 12;
    if (ry < 3) ry = 3;
    int rx = ry * 2 + 2 < FCOLS / 2 - 3 ? ry * 2 + 2 : FCOLS / 2 - 3;
    if (rx < 6) rx = 6;
    int ix, iy;
    cell_of(bl->wx, bl->wy, &ix, &iy);
    int cx = iclamp(ix, rx + 2, FCOLS - rx - 3), cy = iclamp(iy, FY + ry + 1, FY + FROWS - ry - 2);
    bool compact = FCOLS < 46 || FROWS < 16;
    int x0 = 0, y0 = FY, x1 = FCOLS, y1 = FY + FROWS;
    float conv = full ? clampf((float)((tt - bl->T_CONV) / fmax(0.01, bl->T_END - bl->T_CONV)), 0, 1) : 0.0f;
    float k = ease_out(clampf(tt / 1.0f, 0, 1)) * (1 - conv * conv);
    /* I - the clock */
    static const struct { float f; int col; uint32_t ch; int dr; } RINGS[4] = {{1.0f, 0, 0xB7, 1}, {0.86f, 1, 0x2022, -1}, {0.62f, 0, 0xB7, 1}, {0.3f, 2, 0x25E6, -1}};
    RGB rc[3] = {BG_, BV, BP};
    for (int ri = 0; ri < 4; ri++) {
        int n = (int)clampf(6.2831853f * rx * RINGS[ri].f * 0.9f, 24, 140);
        for (int j = 0; j < n; j++) {
            float a = j * 6.2831853f / n + RINGS[ri].dr * tt * (0.35f + 0.2f * ri);
            uint32_t mark = j % 10 == 0 ? 0x2727 : RINGS[ri].ch;
            float sv = fsin(tt * 5 + j * 0.7f);
            bput(cx + (int)rintf(fsin(a) * rx * RINGS[ri].f * k), cy - (int)rintf(fcos(a) * ry * RINGS[ri].f * k), mark,
                 mark == RINGS[ri].ch ? cshade(rc[RINGS[ri].col], 0.55f + 0.45f * sv * sv) : BW);
        }
    }
    for (int i = 0; i < 12; i++) {
        if ((compact && i % 3) || tt < 0.25f + i * 0.06f) continue;
        float a = i * 6.2831853f / 12 + tt * 0.18f;
        bool lit = ((int)(tt * 3) % 12) == i && conv == 0;
        btext(cx + (int)rintf(fsin(a) * rx * 0.93f * k) - (int)strlen(ROMAN[i]) / 2, cy - (int)rintf(fcos(a) * ry * 0.93f * k), ROMAN[i], lit ? BW : BG_);
    }
    static const float HANDS[2][2] = {{0.55f, 2.4f}, {0.82f, 0.8f}};
    for (int h = 0; h < 2; h++) {
        float ha = tt * HANDS[h][1] * (1 - conv);
        for (int sgm = 1; sgm < 13; sgm++) {
            float f = sgm / 12.0f * HANDS[h][0] * k;
            bput(cx + (int)rintf(fsin(ha) * rx * f), cy - (int)rintf(fcos(ha) * ry * f), sgm % 3 ? 0x2022 : 0x25E6, h ? BW : BG_);
        }
    }
    if (rx >= 8 && FROWS >= 10) {
        const char *title = "MARTYR'S BLESSING";
        const char *spaced = "M A R T Y R ' S   B L E S S I N G";
        const char *tl = (int)strlen(spaced) > FCOLS - 2 ? title : spaced;
        btext(x0 + (FCOLS - (int)strlen(tl)) / 2, y0 + 1, tl, cmix(BP, BG_, 0.5f + 0.5f * fsin(tt * 6)));
        if (tt >= bl->T_BANG && conv < 1 && FROWS >= 14) {
            const char *sub = "TIME RECEIVES ITS FINAL JUDGMENT";
            btext(x0 + (FCOLS - (int)strlen(sub)) / 2, y0 + 2, sub, BW);
        }
    }
    /* II - the sun */
    if (full && bl->T_SUN <= tt && tt < bl->T_BANG) {
        float u_all = clampf((float)((tt - bl->T_SUN) / (bl->T_BANG - bl->T_SUN)), 0, 1);
        int twin = tt >= bl->T_SUN + 0.6 ? 2 : 1;
        for (int j = 0; j < 12 * twin; j++) {
            float u = clampf(u_all * 1.15f - (j % 12) * 0.02f, 0, 1);
            float ang = (j % 12) * 6.2831853f / 12 + (j >= 12 ? 3.14159265f / 12 : 0) + u * 5.5f;
            float r = powf(1 - u, 1.4f);
            bput(cx + (int)rintf(fcos(ang) * rx * 0.95f * r), cy + (int)rintf(fsin(ang) * ry * 0.95f * r), j < 12 ? 0x2726 : 0x2727, j < 12 ? BG_ : BW);
        }
        float sr = ease_out(u_all);
        bellipse(cx, cy, rx * 0.34f * sr, ry * 0.34f * sr, 1.0f, BG_);
    }
    /* III - the judgment */
    if (full && tt >= bl->T_BANG) {
        float bb = tt - (float)bl->T_BANG;
        if (bb < 0.9f) {
            bellipse(cx, cy, rx * 0.34f * (1 - bb / 0.9f) + 1, ry * 0.34f * (1 - bb / 0.9f) + 1, 1 - bb / 0.9f, BW);
            float rs = ease_out(clampf(bb / 0.7f, 0, 1));
            bring(cx, cy, rx * 1.05f * rs, ry * 1.05f * rs, 64, cmix(BW, BG_, bb / 0.9f), 1 - bb / 0.9f, 0x25CB, 0x25E6);
            for (int j = 0; j < 8; j++) {
                float a = j * 3.14159265f / 4;
                for (int sgm = 2; sgm < (int)(2 + 8 * rs); sgm++)
                    bput(cx + (int)rintf(fcos(a) * sgm * 2), cy - (int)rintf(fsin(a) * sgm), sgm == (int)(7 * rs) ? 0x2726 : 0xB7, j % 2 ? BG_ : BW);
            }
        }
        for (int j = 0; j < 40; j++) {
            float age = tt - ((float)bl->T_BANG + 0.12f * j);
            if (age >= 0 && age < 1.0f) {
                float a = j * 2.399f + 0.4f;
                float spd = 34 + (j % 5) * 9;
                for (int tr = 0; tr < 7; tr++) {
                    float a2 = fmaxf(0.0f, age - tr * 0.025f);
                    RGB col = j % 3 ? BG_ : (j % 3 == 0 ? BW : BV);
                    bput((int)rintf(cx + fcos(a) * spd * a2 * 1.7f), (int)rintf(cy + fsin(a) * spd * a2 * 0.55f - a2 * a2 * 10), tr == 0 ? 0x2726 : 0xB7,
                         cshade(col, 1 - tr * 0.13f));
                }
            }
        }
        int topy = y0 + 3;
        for (int i = 0; i < bl->ntargets; i++) {
            int ex, ey;
            cell_of(bl->targets[i].x, bl->targets[i].y, &ex, &ey);
            ex = (int)clampf((float)ex, (float)(x0 + 2), (float)(x1 - 3));
            ey = (int)clampf((float)ey, (float)(y0 + 4), (float)(y1 - 2));
            bool vic = bl->targets[i].tank >= 0;
            float age = tt - (float)bl->arrive[i];
            float tel = (float)bl->tel;
            if (age >= -tel && age < -0.12f) {
                float p = 1 - (age + tel) / (tel - 0.12f);
                float rr = 2 + (1 - p) * 7;
                bring(ex, ey, rr * 2, rr * 0.7f, 16, (int)(tt * 12) % 2 ? BG_ : BW, 0.5f + 0.5f * p, 0x25CB, 0x25E6);
                bput(ex, ey, 0x271B, BW);
            }
            if (age >= -0.12f && age < 0.45f) {
                float drop = clampf((age + 0.12f) / 0.12f, 0, 1);
                int head = (int)(topy + (ey - topy) * drop);
                int wide = vic ? 2 : 1;
                float fade = age < 0.12f ? 1.0f : clampf(1 - (age - 0.12f) / 0.33f, 0, 1);
                for (int yy = topy; yy <= head; yy++) {
                    bput(ex, yy, 0x2503, cshade(BW, 0.5f + 0.5f * fade));
                    for (int sdx = 1; sdx <= wide; sdx++) {
                        RGB c = cmix(BG_, BV, (sdx - 1) / (float)(wide > 1 ? wide : 1));
                        bput(ex - sdx, yy, sdx == 1 ? 0x2502 : 0x254E, cshade(c, 0.35f + 0.5f * fade));
                        bput(ex + sdx, yy, sdx == 1 ? 0x2502 : 0x254E, cshade(c, 0.35f + 0.5f * fade));
                    }
                }
                bput(ex, head, 0x25BC, BW);
            }
            if (age >= 0 && age < 0.9f) {
                float sw = (2 + age * (vic ? 22 : 15)) * ease_out(clampf(age / 0.5f, 0, 1) + 0.2f);
                bring(ex, ey, sw, sw * 0.4f, 24, BG_, 1 - age / 0.9f, 0x25CB, 0x25E6);
                if (vic && age > 0.08f) bring(ex, ey, sw * 0.62f, sw * 0.25f, 16, BV, 1 - age / 0.9f, 0x25E6, 0x2022);
                for (int j = 0; j < (vic ? 6 : 4); j++) {
                    float a = j * 3.14159265f / (vic ? 3 : 2) + 0.3f;
                    bput(ex + (int)rintf(fcos(a) * age * 11), ey - (int)rintf(fabsf(fsin(a)) * age * 9 - age * age * 12), 0x2726, j % 2 ? BW : BG_);
                }
            }
        }
    }
    /* IV - the finale */
    if (full && tt >= bl->T_CONV) {
        float f = tt - (float)bl->T_CONV;
        if (conv > 0)
            for (int j = 0; j < 24; j++) {
                float a = j * 6.2831853f / 24;
                bput(cx + (int)rintf(fsin(a) * rx * (1 - conv)), cy - (int)rintf(fcos(a) * ry * (1 - conv)), 0x2726, BW);
            }
        int base = y1 - 2 < cy + ry + 2 ? y1 - 2 : cy + ry + 2;
        if (f < 0.5f) bellipse(cx, base, rx * (0.6f + 2.2f * f), ry * (0.5f + 1.6f * f), 1 - f / 0.5f * 0.4f, BW);
        float p = ease_out(clampf(f / 2.2f, 0, 1));
        float fade = 1 - clampf((f - 2.0f) / 1.2f, 0, 1);
        int top_y = (int)(base - (base - (y0 + 4)) * 0.8f * p);
        static const float ST[3] = {0, 0.5f, 1};
        static const RGB SC[3] = {{255, 226, 150}, {255, 110, 30}, {120, 28, 18}};
        static const float CT[4] = {0, 0.3f, 0.7f, 1};
        static const RGB CC[4] = {{255, 240, 190}, {255, 130, 36}, {210, 48, 20}, {60, 18, 30}};
        if (f > 0.15f && fade > 0) {
            for (int yy = top_y; yy <= base; yy++) {
                float span = (float)(base - top_y > 1 ? base - top_y : 1);
                int sw = (int)(2 + (1 - (yy - top_y) / span) * 2 + (yy - top_y) / span * 5);
                if (sw < 2) sw = 2;
                for (int dx = -sw; dx <= sw; dx++) {
                    float u = abs(dx) / (float)(sw > 1 ? sw : 1);
                    bput(cx + dx, yy, SHADES[(int)(u * 4) < 3 ? (int)(u * 4) : 3], cshade(cgradient(ST, SC, 3, u), 0.5f + 0.5f * fade));
                }
            }
            float crx = rx * 1.25f * p + 4, cry = ry * 0.95f * p + 2;
            for (int dy = -(int)cry - 1; dy < (int)cry + 2; dy++)
                for (int dx = -(int)crx - 1; dx < (int)crx + 2; dx++) {
                    float d = fhypot(dx / crx, dy / cry + 0.25f * fsin(dx * 0.4f + tt * 6) * 0.3f);
                    if (d <= 1.0f) bput(cx + dx, top_y + dy, SHADES[(int)(d * 4) < 3 ? (int)(d * 4) : 3], cshade(cgradient(CT, CC, 4, d), 0.4f + 0.6f * fade));
                }
        }
        static const uint32_t WALL[7] = {0x2588, 0x2593, 0x2593, 0x2592, 0x2592, 0x2591, 0x2591};
        for (int sgn = -1; sgn <= 1; sgn += 2) {
            int wx = cx + sgn * (int)(f * 55);
            if (wx >= x0 && wx < x1 && f < 2.5f)
                for (int yy = y0 + 2; yy < y1 - 1; yy++) {
                    int k_ = (yy * 7 + (int)(tt * 20)) % 5;
                    for (int th = 0; th < 7; th++)
                        bput(wx - sgn * th, yy - (th + k_) % 2, WALL[th], cshade(cmix((RGB){255, 245, 200}, (RGB){190, 50, 20}, th / 6.0f), 0.55f + 0.45f * ((k_ + th) % 3) / 2.0f));
                }
        }
    }
}

/* ------------------------------------------------------------------ HUD and panel */
static void team_list(const Battle *b, int *teams, int *nt) {
    *nt = 0;
    for (int i = 0; i < b->w.ntanks; i++) {
        int tm = b->w.tanks[i].team;
        bool seen = false;
        for (int k = 0; k < *nt; k++) if (teams[k] == tm) seen = true;
        if (!seen) teams[(*nt)++] = tm;
    }
    for (int i = 1; i < *nt; i++)
        for (int j = i; j > 0 && teams[j - 1] > teams[j]; j--) { int x = teams[j]; teams[j] = teams[j - 1]; teams[j - 1] = x; }
    if (*nt > 2) *nt = 2;
}

static void draw_charge(Term *t, int x, int y, const Tank *tk, bool right) {
    char buf[32];
    int o = 0, need = g_sess.special_wait;
    for (int k = 0; k < need && o < 28; k++) o += snprintf(buf + o, sizeof buf - (size_t)o, "%s", k < tk->charge ? "\xE2\x9C\xA6" : "\xC2\xB7");
    int len = need;
    term_text(t, right ? x - len + 1 : x, y, buf, tk->charge >= need ? GOLD : P_MUTED, &P_PANEL);
}

static void draw_hud(BattleScn *s, Term *t) {
    Battle *b = &s->b;
    Session *ss = &g_sess;
    float tm = (float)g_app.time;
    term_fill(t, 0, 0, T_COLS, 2, P_PANEL);
    int teams[2], nt;
    team_list(b, teams, &nt);
    Tank *cur = battle_current(b);
    for (int side = 0; side < nt; side++) {
        const Tank *mem[MAX_TANKS];
        int nm = 0, all = 0;
        for (int i = 0; i < b->w.ntanks; i++)
            if (b->w.tanks[i].team == teams[side]) { all++; if (nm < 2) mem[nm++] = &b->w.tanks[i]; }
        for (int r = 0; r < nm; r++) {
            const Tank *tk = mem[r];
            RGB col = live_color(tk->color, tm);
            int row = nm > 1 ? r : 0;
            char nmv[16];
            snprintf(nmv, sizeof nmv, "%.10s", tk->name);
            int ln = text_len(nmv);
            if (side == 0) {
                term_text(t, 0, row, "\xE2\x96\x8C", col, &P_PANEL);
                term_text(t, 1, row, nmv, col, &P_PANEL);
                int wd = ui_draw_hp(t, 2 + ln, row, tk, P_PANEL, false);
                if (ss->special) draw_charge(t, 3 + ln + wd, row, tk, false);
            } else {
                int x = T_COLS - 1;
                term_text(t, x, row, "\xE2\x96\x90", col, &P_PANEL);
                term_text(t, x - ln, row, nmv, col, &P_PANEL);
                int wd = ui_draw_hp(t, x - 2 - ln, row, tk, P_PANEL, true);
                if (ss->special) draw_charge(t, x - 4 - ln - wd, row, tk, true);
            }
        }
        if (nm == 1) {
            char wt[64];
            session_hud_wins(ss, teams[side], wt, sizeof wt);
            if (side == 0) term_text(t, 1, 1, wt, P_MUTED, &P_PANEL);
            else term_text(t, T_COLS - 1 - text_len(wt), 1, wt, P_MUTED, &P_PANEL);
        }
        (void)all;
    }
    /* turn badge + state on the bottom HUD row of the top screen */
    char badge[40], state[40];
    char tt[32];
    battle_turn_text(b, b->turn, tt, sizeof tt);
    snprintf(badge, sizeof badge, " %s ", tt);
    battle_state_text(b, state, sizeof state);
    RGB ccol = live_color(cur->color, tm);
    term_fill(t, 0, 22, T_COLS, 2, P_PANEL);
    if (b->phase == PH_DESTRUCTION) term_center(t, 22, state, P_DANGER, &P_PANEL, 0, -1);
    else {
        int total = text_len(badge) + 2 + text_len(state);
        int bx = (T_COLS - total) / 2;
        term_text(t, bx, 22, badge, P_INK, &ccol);
        term_text(t, bx + text_len(badge) + 2, 22, state, P_AMBER, &P_PANEL);
    }
    /* angle gauge + power bar */
    char v[16];
    term_text(t, 0, 23, "ANG", P_MUTED, &P_PANEL);
    snprintf(v, sizeof v, "%3.0f\xC2\xB0", tank_rel_angle(cur));
    term_text(t, 4, 23, v, ccol, &P_PANEL);
    int gx = 9, gw = 13;
    term_put(t, gx, 23, 0x251C, P_LINE, &P_PANEL);
    for (int i = 0; i < gw; i++) term_put(t, gx + 1 + i, 23, 0x2500, (RGB){52, 70, 82}, &P_PANEL);
    int marker = (int)((180 - cur->angle) / 180 * (gw - 1));
    term_put(t, gx + 1 + marker, 23, 0x25C6, ccol, &P_PANEL);
    term_put(t, gx + gw + 1, 23, 0x2524, P_LINE, &P_PANEL);
    int px = gx + gw + 3;
    term_text(t, px, 23, "PWR", P_MUTED, &P_PANEL);
    ui_draw_bar(t, px + 4, 23, 10, (float)((cur->power - PHYS_MIN_POWER) / (PHYS_MAX_POWER - PHYS_MIN_POWER)));
    snprintf(v, sizeof v, "%3.0f%%", cur->power);
    term_text(t, px + 15, 23, v, ccol, &P_PANEL);
    term_text(t, px + 20, 23, "\xE2\x97\x8F\xE2\x97\x8F", live_color(cur->shot_color, tm), &P_PANEL);
}

static void big_button(Term *t, int x, int y, int w, int h, const char *label, RGB fg, RGB bg, int id, bool on) {
    RGB b2 = on ? bg : cshade(bg, 0.5f);
    term_box(t, x, y, w, h, TB_ROUND, on ? fg : P_MUTED, b2, "", fg);
    term_center(t, y + h / 2, label, on ? fg : P_MUTED, &b2, x, w);
    if (on) hot_add(x, y, w, h, id);
}

static void draw_panel(BattleScn *s, Term *t) {
    Battle *b = &s->b;
    Session *ss = &g_sess;
    float tm = (float)g_app.time;
    int y0 = T_SPLIT;
    term_fill(t, 0, y0, T_COLS, T_SPLIT, P_PANEL);
    Tank *cur = battle_current(b);
    RGB ccol = live_color(cur->color, tm);
    char buf[96];
    session_hud_title(ss, buf, sizeof buf);
    term_center(t, y0, buf, P_TEXT, &P_PANEL, 0, -1);
    term_fill_ch(t, 0, y0 + 1, T_COLS, 1, P_BG, 0x2500, cmix(P_LINE, ccol, 0.5f));
    ui_wallet_text(&g_app.save, buf, sizeof buf);
    char wb[96];
    snprintf(wb, sizeof wb, " %s ", buf);
    term_text(t, T_COLS - text_len(wb) - 1, y0 + 1, wb, P_TEXT, &P_BG);
    int teams[2], nt;
    team_list(b, teams, &nt);
    for (int side = 0; side < nt; side++) {
        int cnt = 0;
        for (int i = 0; i < b->w.ntanks; i++) cnt += b->w.tanks[i].team == teams[side];
        if (cnt > 1) {
            char wt[64];
            session_hud_wins(ss, teams[side], wt, sizeof wt);
            if (cnt > 2) { int l = (int)strlen(wt); snprintf(wt + l, sizeof wt - (size_t)l, "  +%d MORE", cnt - 2); }
            term_text(t, side == 0 ? 1 : T_COLS - 1 - text_len(wt), y0 + 2, wt, P_MUTED, &P_PANEL);
        }
    }
    bool local = b->phase == PH_AIMING && battle_control(b, cur) == CTL_LOCAL;
    if (b->phase == PH_BLESSING) {
        big_button(t, 8, y0 + 8, 35, 5, "A  SKIP THE SPECTACLE", GOLD, P_PANEL_HI, H_SKIP, true);
    } else {
        /* angle */
        term_text(t, 2, y0 + 4, "ANGLE", P_MUTED, &P_PANEL);
        snprintf(buf, sizeof buf, "%3.0f\xC2\xB0", tank_rel_angle(cur));
        term_text(t, 2, y0 + 5, buf, ccol, &P_PANEL);
        big_button(t, 9, y0 + 3, 9, 4, "\xE2\x97\x84", P_WHITE, P_PANEL_HI, H_ANG_L, local);
        big_button(t, 19, y0 + 3, 9, 4, "\xE2\x96\xBA", P_WHITE, P_PANEL_HI, H_ANG_R, local);
        /* power */
        term_text(t, 2, y0 + 9, "POWER", P_MUTED, &P_PANEL);
        snprintf(buf, sizeof buf, "%3.0f%%", cur->power);
        term_text(t, 2, y0 + 10, buf, ccol, &P_PANEL);
        big_button(t, 9, y0 + 8, 9, 4, "\xE2\x96\xB2", P_WHITE, P_PANEL_HI, H_POW_U, local);
        big_button(t, 19, y0 + 8, 9, 4, "\xE2\x96\xBC", P_WHITE, P_PANEL_HI, H_POW_D, local);
        /* fire */
        big_button(t, 30, y0 + 3, 19, 9, "FIRE", P_DANGER, (RGB){60, 20, 24}, H_FIRE, local);
        if (ss->special) {
            snprintf(buf, sizeof buf, "X CHARGE %d/%d", cur->charge, ss->special_wait);
            big_button(t, 2, y0 + 13, 26, 3, buf, GOLD, P_PANEL_HI, H_CHARGE, local);
        }
    }
    big_button(t, 30, y0 + 13, 19, 3, "START  MENU", P_PRIMARY, P_PANEL_HI, H_MENU, true);
    if (b->phase == PH_AI_THINKING) {
        snprintf(buf, sizeof buf, "AI FIRE CONTROL: %s", b->has_plan || !b->ai_pending ? b->plan.status : "SOLVING...");
        term_wrap_center(t, y0 + 17, buf, 47, P_AMBER, &P_PANEL, 0, T_COLS, 2);
    } else if (b->practice) {
        /* the practice range has its coach panel; its keys are listed there */
    } else {
        Keycap K[6] = {{"\xE2\x97\x84\xE2\x96\xBA", "AIM"}, {"\xE2\x96\xB2\xE2\x96\xBC", "POWER"}, {"L/R", "COARSE"}, {"A", "FIRE"}};
        int n = 4;
        if (ss->special) K[n++] = (Keycap){"X", "CHARGE"};
        ui_keycaps_center(t, y0 + 18, K, n, &P_PANEL, 0, T_COLS);
    }
    /* wind */
    double wmax = b->wind_setting, wind = b->w.wind;
    RGB wc;
    if (wmax <= 0) { snprintf(buf, sizeof buf, "WIND OFF"); wc = P_MUTED; }
    else if (fabs(wind) < 0.05) { snprintf(buf, sizeof buf, "WIND CALM"); wc = P_TEXT; }
    else {
        battle_wind_text(b, buf, sizeof buf);
        static const float GT[3] = {0, 0.6f, 1};
        RGB GC[3] = {P_OK, P_AMBER, P_DANGER};
        wc = cgradient(GT, GC, 3, (float)(fabs(wind) / wmax));
    }
    term_center(t, y0 + 21, buf, wc, &P_PANEL, 0, -1);
}

static void draw_labels(BattleScn *s, Term *t) {
    Battle *b = &s->b;
    float tm = (float)g_app.time;
    for (int i = 0; i < b->w.ntanks; i++) {
        const Tank *tk = &b->w.tanks[i];
        if (tk->destroyed) continue;
        int cx, cy;
        cell_of(tk->x, tk->y + tank_h(tk) + 8, &cx, &cy);
        if (cy < FY) cy = FY;
        char hearts[64], text[96];
        ui_hearts(tk->hp, false, tk->max_hp, hearts, sizeof hearts);
        snprintf(text, sizeof text, "%s %s", tk->label, hearts);
        term_text(t, cx - text_len(text) / 2, cy, text, tk->hp > 0 ? live_color(tk->color, tm) : P_MUTED, NULL);
        if (i == b->turn && (b->phase == PH_AIMING || b->phase == PH_AI_THINKING) && cy > FY) {
            int bob = (int)(tm * 3) % 2;
            int yy = cy - 1 - bob;
            if (yy < FY) yy++;
            term_put(t, cx, yy, 0x25BC, P_WHITE, NULL);
        }
    }
}

static void draw_overlays(BattleScn *s, Term *t) {
    Battle *b = &s->b;
    for (int i = 0; i < b->nfloaters; i++) {
        const FloatText *f = &b->floaters[i];
        int x, y;
        cell_of(f->x, f->y + f->age * 6, &x, &y);
        float a = 1 - (float)(f->age / f->life);
        term_text(t, x - text_len(f->text) / 2, y < FY ? FY : y, f->text, cmix(P_BG, f->color, clampf(a * 2, 0, 1)), NULL);
    }
    const Banner *bn = &b->banner;
    if (bn->on) {
        float a = clampf(fminf((float)bn->age * 6, (float)(bn->life - bn->age) * 4), 0, 1);
        int y = FY + FROWS / 3;
        char spaced[100];
        int o = 0;
        for (const char *p = bn->text; *p && o < 96; p++) {
            if (o) spaced[o++] = ' ';
            spaced[o++] = *p;
        }
        spaced[o] = 0;
        if (text_len(spaced) > T_COLS) snprintf(spaced, sizeof spaced, "%s", bn->text);
        /* the original tints these rows dark: done with a solid-ish strip behind the text */
        for (int yy = y - 1; yy < y + 3; yy++)
            for (int x = 0; x < T_COLS; x++) {
                Cell *c = &t->c[yy][x];
                if (c->g == G_NONE) { c->g = (uint16_t)G_SPACE; c->fg = 0; c->bg = 0; }
            }
        g_banner_alpha = a;
        g_banner_y = y;
        term_center(t, y, spaced, cmix(P_BG, bn->color, a), NULL, 0, -1);
        if (bn->sub[0]) {
            char lines[3][64];
            int n = wrap_text(bn->sub, T_COLS - 1, lines, 2);
            for (int k = 0; k < n; k++) term_center(t, y + 2 + k, lines[k], cmix(P_BG, P_TEXT, a), NULL, 0, -1);
        }
    }
    /* off-screen marker */
    for (int i = 0; i < b->nflights; i++) {
        const LiveFlight *f = &b->flights[i];
        if (!f->active || f->delay > 0) continue;
        if (f->y >= FIELD_H) {
            int cx = (int)(f->x * 2) / CELL_W;
            cx = cx < 3 ? 3 : (cx > T_COLS - 8 ? T_COLS - 8 : cx);
            char h[12];
            snprintf(h, sizeof h, "%d", (int)(f->y - FIELD_H));
            term_put(t, cx, FY, 0x25B2, P_WHITE, NULL);
            term_text(t, cx + 1, FY, h, P_WHITE, NULL);
        }
    }
}

static void draw_coach(BattleScn *s, Term *t) {
    Battle *b = &s->b;
    const char *name, *goal, *tip, *body;
    char prog[64];
    if (s->adv >= 0) {
        name = "CHALLENGE";
        goal = CHALLENGES[s->adv].name;
        tip = "Streaks reset on a miss. Wind shifts every turn.";
        body = "Practice what you learned. No rewards - just bragging rights.";
        snprintf(prog, sizeof prog, "STREAK %d/%d", b->pr_streak, CHALLENGES[s->adv].need);
    } else {
        const Lesson *L = &LESSONS[s->lesson - 1];
        name = L->name; body = L->body; goal = L->goal; tip = L->tip;
        if (s->lesson == 4) snprintf(prog, sizeof prog, "SHOTS %d  HITS %d  CRATERS %d", b->pr_shots, b->pr_hits, b->pr_craters);
        else snprintf(prog, sizeof prog, "SHOTS %d  HITS %d", b->pr_shots, b->pr_hits);
    }
    int y0 = T_SPLIT + 17;
    term_fill(t, 0, y0 - 1, T_COLS, 7, P_PANEL);
    char head[64];
    snprintf(head, sizeof head, " TUTORIAL \xC2\xB7 %s ", name);
    term_text(t, 1, y0 - 1, head, GOLD, &P_PANEL);
    if (!s->coach) {
        ui_button(t, 1, y0 + 1, "Y SHOW COACH", P_MUTED, P_PANEL, H_COACH);
        return;
    }
    /* the coach panel spans the field (top screen) in the original; here it sits on the touch screen */
    char lines[4][64];
    int n = wrap_text(body, 49, lines, 2);
    for (int k = 0; k < n; k++) term_text(t, 1, y0 + k, lines[k], P_WHITE, &P_PANEL);
    char g[96];
    snprintf(g, sizeof g, "GOAL: %s  %s", goal, prog);
    term_text(t, 1, y0 + 2, g, GOLD, &P_PANEL);
    if (s->done) {
        snprintf(g, sizeof g, "DONE!  [SELECT] %s", s->adv >= 0 || s->lesson >= 4 ? "BACK TO LESSONS" : "NEXT LESSON");
        term_text(t, 1, y0 + 3, g, P_OK, &P_PANEL);
        hot_add(1, y0 + 3, text_len(g), 1, H_NEXT);
    } else {
        char lines2[2][64];
        snprintf(g, sizeof g, "%s \xC2\xB7 %s", b->pr_note[0] ? b->pr_note : "FIRE WHEN READY", tip);
        int m = wrap_text(g, 49, lines2, 2);
        for (int k = 0; k < m; k++) term_text(t, 1, y0 + 3 + k, lines2[k], P_AMBER, &P_PANEL);
    }
    ui_button(t, 30, y0 - 1, "Y HIDE", P_MUTED, P_PANEL, H_COACH);
}


static void bs_draw(Scene *sc, Term *t) {
    BattleScn *s = (BattleScn *)sc;
    for (int y = FY; y < FY + FROWS; y++)
        for (int x = 0; x < T_COLS; x++) t->c[y][x] = (Cell){G_NONE, 0, 0};
    g_banner_alpha = 0;
    if (s->b.phase == PH_BLESSING && s->b.bless.on) draw_blessing(t, &s->b);
    draw_labels(s, t);
    draw_overlays(s, t);
    draw_hud(s, t);
    draw_panel(s, t);
    if (s->b.practice) draw_coach(s, t);
}

/* banner rows: the original tints them black at 0.6 * alpha; drawn as a pixel tint under the text */
static void bs_over(Scene *sc, Surf *top, Surf *bot) {
    (void)sc; (void)bot; (void)top;
}

static void bs_px_full(Scene *sc, Surf *top, Surf *bot) {
    bs_px(sc, top, bot);
    if (g_banner_alpha > 0) surf_rect_blend(top, 0, (g_banner_y - 1) * CELL_H, 256, 4 * CELL_H, (RGB){0, 0, 0}, 0.6f * g_banner_alpha);
}

static void bs_destroy(Scene *sc) { free(sc); }

static const SceneVT BATTLE_VT = {.enter = bs_enter, .exit = bs_exit, .handle = bs_handle, .update = bs_update,
                                  .update_ambient = bs_update_amb, .draw = bs_draw, .draw_px = bs_px_full,
                                  .draw_over = bs_over, .destroy = bs_destroy};

static BattleScn *battle_alloc(void) {
    BattleScn *s = (BattleScn *)calloc(1, sizeof(BattleScn));
    scene_base(&s->base, &BATTLE_VT);
    s->base.show_wallet = false;
    s->base.ambient = true;
    s->hold_id = -1;
    s->adv = -1;
    s->coach = true;
    return s;
}

Scene *scene_battle(void) { return &battle_alloc()->base; }

int battle_scene_doomed_team(Scene *sc) {
    BattleScn *s = (BattleScn *)sc;
    return s->b.doomed >= 0 ? s->b.w.tanks[s->b.doomed].team : 1;
}

/* ------------------------------------------------------------------ practice range (l.9632) */
Scene *scene_practice(int lesson, int adv) {
    Loadout *lo = save_loadout(&g_app.save, 0);
    int ammo = lo->ammo != AMMO_BLESSING ? lo->ammo : AMMO_STANDARD;
    double wind = 0.0;
    if (adv >= 0) { ammo = CHALLENGES[adv].ammo; wind = CHALLENGES[adv].wind; }
    else if (lesson == 2) wind = 8.0;
    else if (lesson == 3) ammo = AMMO_PEBBLES;
    else if (lesson != 1 && lesson != 4) ammo = AMMO_STANDARD;
    if (adv < 0 && (lesson == 1 || lesson == 2 || lesson == 4)) ammo = AMMO_STANDARD;
    PlayerSetup me, tgt;
    player_setup_init(&me, "PLAYER", "YOU", paint_rgb(lo->tank_color), paint_rgb(lo->shot_color), false,
                      save_tank_unlocked(&g_app.save, lo->tank) ? lo->tank : DESIGN_RANGER, ammo, 9);
    player_setup_init(&tgt, "TARGET", "TGT", COLOR_CHOICES[color_index_by_name("ORANGE")].c, COLOR_CHOICES[color_index_by_name("PINK")].c, true,
                      DESIGN_RANGER, AMMO_STANDARD, 9);
    session_init_practice(&g_sess, &me, &tgt, (uint32_t)rng_below(&g_app.rng, 1u << 30), lesson, adv, wind);
    BattleScn *s = battle_alloc();
    s->lesson = lesson;
    s->adv = adv;
    return &s->base;
}

/* ================================================================== pause + confirm leave (l.9533-9615) */
typedef struct { Scene base; Scene *battle; MenuList menu; } Pause;
static void pause_handle(Scene *sc, const IEv *e) {
    Pause *p = (Pause *)sc;
    if (e->action == A_BACK || e->action == A_PAUSE) { app_pop(); return; }
    int c = menu_handle(&p->menu, e, 50);
    if (c < 0) return;
    const char *lab = p->menu.items[c];
    if (!strcmp(lab, "RESUME")) app_pop();
    else if (!strcmp(lab, "RESTART")) session_restart();
    else app_push(scene_confirm_leave());
}
static void pause_draw(Scene *sc, Term *t) {
    Pause *p = (Pause *)sc;
    term_dim(t, 0.45f);
    int x, y;
    ui_panel(t, 34, 9, "MATCH MENU", -1, T_SPLIT, T_SPLIT, P_LINE, &x, &y);
    menu_draw(t, &p->menu, T_COLS / 2, y + 3, (float)g_app.time, 1, 24, 50);
    if (g_sess.net) term_wrap_center(t, y + 6, "The match keeps running for the other player.", 30, P_MUTED, &P_PANEL, x, 34, 2);
}
static const SceneVT PAUSE_VT = {.handle = pause_handle, .draw = pause_draw};
Scene *scene_pause(Scene *battle) {
    Pause *p = (Pause *)calloc(1, sizeof(Pause));
    scene_base(&p->base, &PAUSE_VT);
    p->base.overlay = true;
    p->base.show_wallet = false;
    p->battle = battle;
    static const char *const NET_ITEMS[2] = {"RESUME", "EXIT MATCH"};
    static const char *const ITEMS[3] = {"RESUME", "RESTART", "EXIT MATCH"};
    if (g_sess.net) menu_init(&p->menu, NET_ITEMS, 2);
    else menu_init(&p->menu, ITEMS, 3);
    return &p->base;
}

typedef struct { Scene base; } Confirm;
static void confirm_handle(Scene *sc, const IEv *e) {
    (void)sc;
    if (e->action == A_BACK || (e->action == A_TOUCH && e->raw == 61)) { app_pop(); return; }
    if (ev_confirm(e) || (e->action == A_TOUCH && e->raw == 60)) {
        if (g_sess.net) net_leave();
        app_flush();
        app_goto(session_exit_scene());
    }
}
static void confirm_draw(Scene *sc, Term *t) {
    (void)sc;
    term_dim(t, 0.55f);
    int x, y;
    ui_panel(t, 49, 10, "LEAVE THE MATCH?", -1, T_SPLIT, T_SPLIT, P_DANGER, &x, &y);
    term_center(t, y + 2, g_sess.net ? "The other player will be told you left." : "This match will end and its progress is lost.", P_TEXT, &P_PANEL, x, 49);
    char buf[80];
    const char *lab = session_exit_label();
    snprintf(buf, sizeof buf, "You return to %s.", strcmp(lab, "MENU") ? lab : "the menu");
    term_center(t, y + 3, buf, P_MUTED, &P_PANEL, x, 49);
    ui_button(t, x + 4, y + 6, "A LEAVE", P_AMBER, P_PANEL, 60);
    ui_button(t, x + 22, y + 6, "B KEEP PLAYING", P_AMBER, P_PANEL, 61);
}
static const SceneVT CONFIRM_VT = {.handle = confirm_handle, .draw = confirm_draw};
Scene *scene_confirm_leave(void) {
    Confirm *c = (Confirm *)calloc(1, sizeof(Confirm));
    scene_base(&c->base, &CONFIRM_VT);
    c->base.overlay = true;
    c->base.show_wallet = false;
    return &c->base;
}

/* ================================================================== round result (l.10376) */
typedef struct { Scene base; Scene *battle; bool ready_me, ready_peer, moved; int loser; } RoundRes;

static void rr_proceed(RoundRes *r) {
    if (r->moved) return;
    r->moved = true;
    Session *s = &g_sess;
    if (session_over(s)) { app_goto(session_finish_scene()); return; }
    if (s->pick_maps && s->type == SESS_MATCH && (s->mode == MODE_SINGLE || s->mode == MODE_TWO || s->mode == MODE_LAN)) {
        int loser = r->loser;
        if (s->mode == MODE_LAN) {
            app_goto(loser == s->my_owner ? scene_round_map(loser) : scene_round_map_wait(loser));
            return;
        }
        if (s->mode == MODE_TWO || s->mode == MODE_SINGLE || loser == 0) {   /* (the original's condition, verbatim) */
            app_goto(scene_round_map(loser));
            return;
        }
    }
    session_advance(s);
    if (session_wants_interlude(s)) app_goto(scene_cinematic(-1, scene_battle));
    else app_goto(scene_battle());
}
static void rr_update(Scene *sc, float dt) {
    (void)dt;
    RoundRes *r = (RoundRes *)sc;
    if (g_sess.net) {
        if (!net_alive_or_leave()) return;
        if (net_take_ready()) r->ready_peer = true;
        if (r->ready_me && r->ready_peer) rr_proceed(r);
    }
}
static void rr_handle(Scene *sc, const IEv *e) {
    RoundRes *r = (RoundRes *)sc;
    if (e->action == A_BACK && (g_sess.mode == MODE_TWO || g_sess.mode == MODE_LAN)) { app_push(scene_confirm_leave()); return; }
    if (ev_confirm(e) || e->action == A_TOUCH) {
        if (!g_sess.net) rr_proceed(r);
        else if (!r->ready_me) {
            r->ready_me = true;
            net_send_ready();
        }
    }
}
static void rr_draw(Scene *sc, Term *t) {
    RoundRes *r = (RoundRes *)sc;
    term_dim(t, 0.5f);
    Session *s = &g_sess;
    int winner = 1 - r->loser;
    RoundSummary info;
    session_round_summary(s, winner, &info);
    int p = session_team_player(s, winner);
    RGB col = s->players[p < 0 ? 0 : p].tank_color;
    RGB lc = live_color(col, (float)g_app.time);
    int x, y;
    ui_panel(t, 49, 13, info.title, -1, T_SPLIT, T_SPLIT, lc, &x, &y);
    term_center(t, y + 2, info.headline, lc, &P_PANEL, x, 49);
    term_center(t, y + 4, info.score, P_WHITE, &P_PANEL, x, 49);
    term_center(t, y + 5, info.sub, P_MUTED, &P_PANEL, x, 49);
    term_center(t, y + 8, info.next, session_over(s) ? P_AMBER : P_TEXT, &P_PANEL, x, 49);
    if (g_sess.net && r->ready_me && !r->ready_peer) term_center(t, y + 10, "WAITING FOR THE OTHER PLAYER...", P_AMBER, &P_PANEL, x, 49);
    else if ((int)(g_app.time * 2) % 2 == 0) term_center(t, y + 10, "[ A ]", P_PRIMARY, &P_PANEL, x, 49);
    hot_add(x, y, 49, 13, 1);
}
static const SceneVT RR_VT = {.handle = rr_handle, .update = rr_update, .draw = rr_draw};
Scene *scene_round_result(Scene *battle) {
    RoundRes *r = (RoundRes *)calloc(1, sizeof(RoundRes));
    scene_base(&r->base, &RR_VT);
    r->base.overlay = true;
    r->battle = battle;
    r->loser = battle_scene_doomed_team(battle);
    return &r->base;
}

/* ------------------------------------------------------------------ touch helper */
extern int app_hot_at(int cx, int cy);
static int touch_hot_held(void) {
    if (!g_input.touching) return -1;
    return app_hot_at(g_input.tx / CELL_W, T_SPLIT + g_input.ty / CELL_H);
}
