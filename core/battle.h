/* The battle: turn flow, flights, impact resolution, destruction, UFO behaviour, special attack and MARTYR'S
 * BLESSING. Port of BattleScene's simulation half (terminaltanks.py l.8671-9310). Rendering reads this state;
 * networking feeds remote aim/fire/skip commands in and receives local ones through the `send` hook. */
#ifndef TT_BATTLE_H
#define TT_BATTLE_H
#include "world.h"
#include "shot.h"
#include "ai.h"
#include "session.h"

enum { PH_INTRO = 0, PH_AIMING, PH_AI_THINKING, PH_FLIGHT, PH_IMPACT, PH_SETTLE, PH_DESTRUCTION, PH_BLESSING };
enum { CTL_LOCAL = 0, CTL_AI, CTL_REMOTE };

#define MAX_FLOATERS 16
#define MAX_EXPLOSIONS 32
#define MAX_PENDING 64
#define MAX_FXQ 96

typedef struct { double x, y; char text[40]; RGB color; double age, life; } FloatText;
typedef struct { char text[48]; char sub[128]; RGB color; double life, age; bool on; } Banner;
typedef struct { double x, y, radius; RGB accent; double duration, age; uint8_t style; } Explosion;

/* visual requests for the renderer's particle system (all effects use the renderer's own rng) */
enum { FX_MUZZLE = 1, FX_TRAIL, FX_IMPACT, FX_DEBRIS, FX_SOUND };
typedef struct { uint8_t type; uint8_t ammo_boom, ammo_trail; uint8_t a; double x, y, p, q; RGB c0, c1; int n; } FxReq;

typedef struct {
    FlightSpec spec;
    double delay, t, x, y, lastx, lasty;
    int n;
    bool active;
} LiveFlight;

typedef struct {
    double x, y;
    int tank;             /* -1 ground pillar */
} BlessTarget;

typedef struct {
    bool on, full, skipped;
    double wx, wy, t;
    BlessTarget targets[14];
    int ntargets;
    double wipes[16][2];
    int nwipes;
    double T_SUN, T_BANG, first, gap, tel, T_CONV, T_END;
    double arrive[14], wipe_t[16];
    uint32_t fired;       /* bits: 0 bang, 1..14 hits, 15 finale, 16..31 wipes */
} Blessing;

typedef struct Battle Battle;
typedef void (*SendFn)(void *ctx, int type, int tank, double angle, double power);   /* type: 'a' aim, 'f' fire, 's' skip */

struct Battle {
    Session *s;
    SaveData *save;
    const Settings *settings;
    double wind_setting;  /* the private settings copy's wind (GALE pins it) */
    World w;
    int phase;
    double t;
    int turn;
    int order[MAX_TANKS], norder;
    AIController ai[MAX_TANKS];
    bool has_ai[MAX_TANKS];
    bool ai_pending;      /* plan still being computed */
    int ai_budget;        /* AI traces per frame (host default 48) */
    bool ai_overlap;      /* count planning time toward the idle first 30% of think_time */
    double ai_wait;       /* time spent planning this turn */
    AimPlan plan;
    bool has_plan;
    double plan_from[2];
    PyRandom srng;
    ShotPlan shot;
    LiveFlight flights[MAX_FLIGHTS];
    int nflights;
    Ev pending[MAX_PENDING];
    int npending;
    bool special_shot, blessing_shot, bless_blocked;
    uint32_t fx_warned;
    Ev ev;
    Impact impact;
    int victims[MAX_TANKS], nvictims;
    int doomed;           /* -1 */
    int doomed_more[MAX_TANKS], ndoomed_more;
    uint32_t flags;
    bool wiped, finished, round_over;
    Blessing bless;
    /* visuals produced by the simulation */
    FloatText floaters[MAX_FLOATERS];
    int nfloaters;
    Banner banner;
    Explosion explosions[MAX_EXPLOSIONS];
    int nexplosions;
    double flash, shake;
    FxReq fxq[MAX_FXQ];
    int nfxq;
    int guide_version;    /* bumps whenever the aim guide must be recomputed */
    /* networking */
    SendFn send;
    void *send_ctx;
    double aim_timer;
    double aim_sent[2];
    bool aim_sent_valid;
    double time;          /* visual clock */
    /* practice range (PracticeScene l.9632): the player always shoots again, the target is restored */
    bool practice;
    int pr_shots, pr_hits, pr_streak, pr_craters, pr_ver;
    bool pr_has_first;
    double pr_first_x;
    char pr_note[64];
};

void battle_init(Battle *b, Session *s, SaveData *save, const Settings *settings);
void battle_update(Battle *b, double dt);
void battle_update_ambient(Battle *b, double dt);
int  battle_control(const Battle *b, const Tank *t);
Tank *battle_current(Battle *b);

/* local input (only acts while the current tank is locally controlled and aiming) */
void battle_aim(Battle *b, int d_angle, int d_power, bool coarse);
void battle_fire(Battle *b);
void battle_forfeit_input(Battle *b);
void battle_skip_blessing(Battle *b);

/* remote commands (wireless peer) */
void battle_remote_aim(Battle *b, int tank, double angle, double power);
void battle_remote_fire(Battle *b, int tank, double angle, double power);
void battle_remote_skip(Battle *b, int tank);

/* helpers for the HUD */
void battle_turn_text(const Battle *b, int i, char *out, int n);
void battle_wind_text(const Battle *b, char *out, int n);
void battle_state_text(const Battle *b, char *out, int n);
int  battle_winner_team(const Battle *b);   /* valid once round_over */

#endif
