/* AI fire control: port of AIController (terminaltanks.py l.3928-4129). The planner is resumable so the DS can
 * spread the search over several frames while the "AI THINKING" animation plays; the sequence of rng draws and
 * the result are identical to running it in one go (verified against the Python reference). */
#ifndef TT_AI_H
#define TT_AI_H
#include "world.h"

#define AI_MAX_HISTORY 256
#define AI_MAX_SCORED 600

typedef struct { double rel, power, error_x; bool hit; } ShotRecord;

typedef struct {
    double angle, power, intended_rel, intended_power, think_time, prob;
    char status[64];
} AimPlan;

typedef struct { double d; int rel; int power; } Scored;

typedef struct {
    AIProfile p;
    PyRandom rng;
    double gravity_bias, correction;
    bool has_last_wind;
    double last_wind;
    ShotRecord history[AI_MAX_HISTORY];
    int nhist;
    /* model context (ModelProbe + shooter) captured by plan(); the probe reads the live terrain */
    bool has_ctx;
    const World *w;
    int me;
    double ctx_wind;
    double foe_rect[4], foe_flat, foe_x;
    double aim_x, aim_y;
    /* resumable job */
    int stage;
    int gi, gj, ngr, ngp;
    Scored scored[AI_MAX_SCORED];
    int nscored;
    int sel_rel, sel_power;
    /* refine / ensemble state */
    Scored best;
    int picks[8][2], npicks, pick_i, sub_i, roll_i, hits;
    Scored local;
    bool have_best;
    double best_score, best_prob;
    int best_rel, best_pow;
    int foe;
    AimPlan out;
} AIController;

void ai_init(AIController *ai, int difficulty, uint32_t seed, double precision);
/* start planning for tank `me` against `foe` */
void ai_plan_begin(AIController *ai, const World *w, int me, int foe);
/* run up to `budget` model shots; returns true when the plan is ready (in ai->out) */
bool ai_plan_step(AIController *ai, int budget);
void ai_plan(AIController *ai, const World *w, int me, int foe, AimPlan *out);   /* synchronous */
void ai_observe(AIController *ai, const AimPlan *plan, const Impact *impact, const Tank *foe, bool hit);

#endif
