#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ai.h"
#include "xmath.h"

enum { ST_IDLE = 0, ST_GRID, ST_PICK, ST_REFINE, ST_ENS_LOCAL, ST_ENS_ROLL, ST_FINISH, ST_DONE };

void ai_init(AIController *ai, int difficulty, uint32_t seed, double precision) {
    memset(ai, 0, sizeof(*ai));
    ai->p = AI_PROFILES[difficulty];
    if (precision != 1.0) {
        ai->p.angle_sigma = ai->p.angle_sigma * precision;
        ai->p.power_sigma = ai->p.power_sigma * precision;
    }
    rng_seed(&ai->rng, seed);
    ai->gravity_bias = 1 + rng_uniform(&ai->rng, -ai->p.gravity_error, ai->p.gravity_error);
    ai->correction = 0.0;
}

/* ModelProbe (l.3973): the AI's imperfect world model */
static int model_probe(void *c, double x, double y, int *tank) {
    AIController *ai = (AIController *)c;
    const World *w = ai->w;
    *tank = -1;
    if (x < 0 || x >= w->terrain.width) return IMP_OUT;
    const double *r = ai->foe_rect;
    if (r[0] - 1 <= x && x <= r[2] + 1 && r[1] - 1 <= y && y <= r[3] + 1) return IMP_TANK;
    double ground = ai->p.terrain_aware ? (double)w->terrain.heights[(int)x] : ai->foe_flat;
    if (y < ground) return IMP_TERRAIN;
    return IMP_NONE;
}

/* Specialised phys_trace(model_probe) for the AI's planning grid. This is the
 * hot loop of AI thinking and the DS has no FPU, so loop invariants are hoisted.
 * Every hoisted value is computed exactly as the generic path computes it
 * (0.5*ax*t*t parses as ((0.5*ax)*t)*t; r[0]-1 is the same double each time),
 * so results stay bit-identical to phys_trace + model_probe. */
static Impact fast_trace(AIController *ai, const Launch *L) {
    const World *w = ai->w;
    const double *r = ai->foe_rect;
    const double rx0 = r[0] - 1, rx1 = r[2] + 1, ry0 = r[1] - 1, ry1 = r[3] + 1;
    const double hx = 0.5 * L->ax, hg = 0.5 * L->g, dt = ai->p.sim_dt;
    const double x0 = L->x0, y0 = L->y0, vx = L->vx, vy = L->vy;
    const double width = w->terrain.width;
    const int aware = ai->p.terrain_aware;
    const double flat = ai->foe_flat;
    double t = 0.0, px = x0, py = y0;
    Impact im;
    while (t < 10.0) {
        t += dt;
        double x = x0 + vx * t + hx * t * t;
        double y = y0 + vy * t - hg * t * t;
        int kind = IMP_NONE;
        if (x < 0 || x >= width) kind = IMP_OUT;
        else if (rx0 <= x && x <= rx1 && ry0 <= y && y <= ry1) kind = IMP_TANK;
        else if (y < (aware ? (double)w->terrain.heights[(int)x] : flat)) kind = IMP_TERRAIN;
        if (kind) {
            im.x = x; im.y = y; im.kind = kind; im.tank = -1;
            return im;
        }
        px = x;
        py = y;
    }
    im.x = px; im.y = py; im.kind = IMP_OUT; im.tank = -1;
    return im;
}

static Impact shoot(AIController *ai, double rel, double power, double g_scale) {
    const Tank *me = &ai->w->tanks[ai->me];
    double ang = me->facing > 0 ? rel : 180 - rel;
    double ox, oy;
    tank_muzzle(me, ang, &ox, &oy);
    /* 2.0: the AI models its own ammo's gravity, wind and speed (only while special-ammo effects are on) */
    const AmmoType *M = ai->w->ammo_fx ? &AMMOS[me->ammo] : NULL;
    double mg = M && (M->mf & MF_G) ? M->g : 1.0, mw = M && (M->mf & MF_WIND) ? M->wind : 1.0;
    double ms = M && (M->mf & MF_SPEED) ? M->speed : 1.0;
    Launch L = phys_make_launch(ai->w, ox, oy, ang, power, PHYS_GRAVITY * g_scale * mg, ai->ctx_wind * mw);
    if (ms != 1.0) {
        L.vx = L.vx * ms;
        L.vy = L.vy * ms;
    }
    if (L.wa != 0.0) return phys_trace(&L, model_probe, ai, ai->p.sim_dt, 10.0).impact;
    return fast_trace(ai, &L);
}

static double miss(AIController *ai, double rel, double power) {
    Impact imp = shoot(ai, rel, power, ai->gravity_bias);
    double d = imp.kind == IMP_TANK ? fabs(imp.x - ai->foe_x) * 0.15 : py_hypot(imp.x - ai->aim_x, imp.y - ai->aim_y);
    for (int i = 0; i < ai->nhist; i++) {
        const ShotRecord *rec = &ai->history[i];
        if (!rec->hit && fabs(rec->rel - rel) < 1.5 && fabs(rec->power - power) < 3) d += 12;
    }
    return d + 0.02 * fabs(rel - 52);
}

static bool is_hit(const AIController *ai, const Impact *imp) {
    const Tank *foe = &ai->w->tanks[ai->foe];
    return imp->kind == IMP_TANK || (imp->kind == IMP_TERRAIN && world_distance_to_tank(foe, imp->x, imp->y) <= PHYS_SPLASH);
}

static int cmp_scored(const void *a, const void *b) {
    const Scored *x = a, *y = b;
    if (x->d < y->d) return -1;
    if (x->d > y->d) return 1;
    if (x->rel != y->rel) return x->rel < y->rel ? -1 : 1;
    if (x->power != y->power) return x->power < y->power ? -1 : 1;
    return 0;
}

static bool lt(const Scored *a, const Scored *b) { return cmp_scored(a, b) < 0; }

void ai_plan_begin(AIController *ai, const World *w, int me, int foe) {
    const AIProfile *p = &ai->p;
    const Tank *f = &w->tanks[foe];
    ai->w = w;
    ai->me = me;
    ai->foe = foe;
    ai->aim_x = py_clamp(f->x + ai->correction, 3, w->width - 4);
    ai->aim_y = f->y + tank_h(f) / 2;
    ai->ctx_wind = w->wind * (1 + rng_uniform(&ai->rng, -p->wind_error, p->wind_error));
    if (ai->has_last_wind && fabs(w->wind - ai->last_wind) > 0.5) ai->correction *= 0.5;
    ai->has_last_wind = true;
    ai->last_wind = w->wind;
    double hw = tank_w(f) / 2;
    ai->foe_rect[0] = f->x - hw; ai->foe_rect[1] = f->y; ai->foe_rect[2] = f->x + hw; ai->foe_rect[3] = f->y + tank_h(f);
    ai->foe_flat = f->y;
    ai->foe_x = f->x;
    ai->has_ctx = true;
    ai->ngr = 0;
    for (int rel = 18; rel < 83; rel += p->angle_step) ai->ngr++;
    ai->ngp = 0;
    for (int pw = 22; pw < 101; pw += 3) ai->ngp++;
    ai->gi = ai->gj = 0;
    ai->nscored = 0;
    ai->stage = ST_GRID;
}

static void finish(AIController *ai, int rel, int power, double prob) {
    const AIProfile *p = &ai->p;
    const Tank *me = &ai->w->tanks[ai->me];
    double exec_rel = py_clamp(rel + rng_gauss(&ai->rng, 0, p->angle_sigma), 4, 176);
    double exec_power = py_clamp(power * (1 + rng_gauss(&ai->rng, 0, p->power_sigma)), 12, 100);
    AimPlan *o = &ai->out;
    o->angle = me->facing > 0 ? exec_rel : 180 - exec_rel;
    o->power = exec_power;
    o->intended_rel = rel;
    o->intended_power = power;
    o->prob = prob;
    if (p->adaptive) {
        int n = snprintf(o->status, sizeof o->status, "ENSEMBLE SOLVE - P(HIT) %.0f%%", prob * 100.0);
        if (ai->nhist && n > 0 && n < (int)sizeof o->status)
            snprintf(o->status + n, sizeof o->status - (size_t)n, " - MODEL RECALIBRATED x%d", ai->nhist);
    } else if (ai->nhist) {
        const ShotRecord *last = &ai->history[ai->nhist - 1];
        bool long_shot = last->error_x * me->facing > 0;
        snprintf(o->status, sizeof o->status, "LAST SHOT %s %.0fPX - RECALIBRATING", long_shot ? "LONG" : "SHORT", fabs(last->error_x));
        if (last->hit) snprintf(o->status, sizeof o->status, "TARGET LOCKED - REPEATING SOLUTION");
    } else {
        snprintf(o->status, sizeof o->status, "SOLVING FIRING SOLUTION");
    }
    o->think_time = p->think_time * rng_uniform(&ai->rng, 0.85, 1.2);
    ai->stage = ST_DONE;
}

bool ai_plan_step(AIController *ai, int budget) {
    const AIProfile *p = &ai->p;
    while (budget > 0 && ai->stage != ST_DONE) {
        switch (ai->stage) {
        case ST_GRID: {
            int rel = 18 + ai->gi * p->angle_step, power = 22 + ai->gj * 3;
            double d = miss(ai, rel, power);
            d = d + rng_random(&ai->rng) * 0.4;
            if (ai->nscored < AI_MAX_SCORED) {
                Scored s = {d, rel, power};
                ai->scored[ai->nscored++] = s;
            }
            budget--;
            if (++ai->gj >= ai->ngp) { ai->gj = 0; ai->gi++; }
            if (ai->gi >= ai->ngr) {
                qsort(ai->scored, (size_t)ai->nscored, sizeof(Scored), cmp_scored);
                ai->stage = ST_PICK;
            }
            break;
        }
        case ST_PICK:
            if (p->adaptive) {
                /* _ensemble_pick: distinct candidates */
                ai->npicks = 0;
                int seen[AI_MAX_SCORED][2];
                int nseen = 0;
                for (int i = 0; i < ai->nscored; i++) {
                    int k0 = (int)py_round_int(ai->scored[i].rel / 2.0), k1 = (int)py_round_int(ai->scored[i].power / 3.0);
                    bool dup = false;
                    for (int j = 0; j < nseen; j++) if (seen[j][0] == k0 && seen[j][1] == k1) { dup = true; break; }
                    if (!dup) {
                        seen[nseen][0] = k0; seen[nseen][1] = k1; nseen++;
                        ai->picks[ai->npicks][0] = ai->scored[i].rel;
                        ai->picks[ai->npicks][1] = ai->scored[i].power;
                        ai->npicks++;
                    }
                    if (ai->npicks == 8) break;
                }
                ai->pick_i = 0;
                ai->sub_i = -1;
                ai->have_best = false;
                ai->stage = ST_ENS_LOCAL;
            } else {
                int n = ai->nscored < p->pick_from ? ai->nscored : p->pick_from;
                const Scored *c = &ai->scored[rng_below(&ai->rng, (uint32_t)n)];
                ai->sel_rel = c->rel;
                ai->sel_power = c->power;
                if (p->refine) {
                    ai->sub_i = -1;
                    ai->stage = ST_REFINE;
                } else {
                    finish(ai, ai->sel_rel, ai->sel_power, 0.0);
                }
            }
            break;
        case ST_REFINE: {
            if (ai->sub_i < 0) {
                Scored b = {miss(ai, ai->sel_rel, ai->sel_power), ai->sel_rel, ai->sel_power};
                ai->best = b;
                ai->sub_i = 0;
            } else {
                int da = -2 + ai->sub_i / 7, dp = -3 + ai->sub_i % 7;
                int pw = (int)py_clamp(ai->sel_power + dp, 15, 100);
                Scored cand = {miss(ai, ai->sel_rel + da, pw), ai->sel_rel + da, pw};
                if (lt(&cand, &ai->best)) ai->best = cand;
                ai->sub_i++;
            }
            budget--;
            if (ai->sub_i >= 35) finish(ai, ai->best.rel, ai->best.power, 0.0);
            break;
        }
        case ST_ENS_LOCAL: {
            int rel0 = ai->picks[ai->pick_i][0], pow0 = ai->picks[ai->pick_i][1];
            if (ai->sub_i < 0) {
                Scored l = {miss(ai, rel0, pow0), rel0, pow0};
                ai->local = l;
                ai->sub_i = 0;
            } else {
                static const int DA[3] = {-1, 0, 1}, DP[3] = {-2, 0, 2};
                int da = DA[ai->sub_i / 3], dp = DP[ai->sub_i % 3];
                int pw = (int)py_clamp(pow0 + dp, 15, 100);
                Scored cand = {miss(ai, rel0 + da, pw), rel0 + da, pw};
                if (lt(&cand, &ai->local)) ai->local = cand;
                ai->sub_i++;
            }
            budget--;
            if (ai->sub_i >= 9) {
                ai->roll_i = 0;
                ai->hits = 0;
                ai->stage = ST_ENS_ROLL;
            }
            break;
        }
        case ST_ENS_ROLL: {
            double r = ai->local.rel + rng_gauss(&ai->rng, 0, p->angle_sigma);
            double pw = py_clamp(ai->local.power * (1 + rng_gauss(&ai->rng, 0, p->power_sigma)), 12, 100);
            Impact imp = shoot(ai, r, pw, ai->gravity_bias);
            ai->hits += is_hit(ai, &imp);
            budget--;
            if (++ai->roll_i >= 14) {
                double score = ai->hits / 14.0 - 0.003 * ai->local.d;
                if (!ai->have_best || score > ai->best_score) {
                    ai->have_best = true;
                    ai->best_score = score;
                    ai->best_rel = ai->local.rel;
                    ai->best_pow = ai->local.power;
                    ai->best_prob = ai->hits / 14.0;
                }
                ai->pick_i++;
                ai->sub_i = -1;
                if (ai->pick_i >= ai->npicks) finish(ai, ai->best_rel, ai->best_pow, ai->best_prob);
                else ai->stage = ST_ENS_LOCAL;
            }
            break;
        }
        default:
            return true;
        }
    }
    return ai->stage == ST_DONE;
}

void ai_plan(AIController *ai, const World *w, int me, int foe, AimPlan *out) {
    ai_plan_begin(ai, w, me, foe);
    while (!ai_plan_step(ai, 1 << 20)) {}
    *out = ai->out;
}

static void fit_gravity(AIController *ai, const AimPlan *plan, const Impact *impact) {
    double lo = 0.75, hi = 1.25;
    double flo = shoot(ai, plan->intended_rel, plan->intended_power, lo).x - impact->x;
    double fhi = shoot(ai, plan->intended_rel, plan->intended_power, hi).x - impact->x;
    if (flo * fhi > 0) return;
    for (int i = 0; i < 10; i++) {
        double mid = (lo + hi) / 2;
        double fm = shoot(ai, plan->intended_rel, plan->intended_power, mid).x - impact->x;
        if (flo * fm <= 0) { hi = mid; fhi = fm; }
        else { lo = mid; flo = fm; }
    }
    ai->gravity_bias = py_clamp(ai->gravity_bias + ((lo + hi) / 2 - ai->gravity_bias) * 0.6, 0.8, 1.2);
}

void ai_observe(AIController *ai, const AimPlan *plan, const Impact *impact, const Tank *foe, bool hit) {
    double err = impact->x - foe->x;
    if (ai->nhist < AI_MAX_HISTORY) {
        ShotRecord r = {plan->intended_rel, plan->intended_power, err, hit};
        ai->history[ai->nhist++] = r;
    } else {
        memmove(ai->history, ai->history + 1, sizeof(ShotRecord) * (AI_MAX_HISTORY - 1));
        ShotRecord r = {plan->intended_rel, plan->intended_power, err, hit};
        ai->history[AI_MAX_HISTORY - 1] = r;
    }
    if (ai->p.adaptive) {
        if (!hit && ai->has_ctx) fit_gravity(ai, plan, impact);
        return;
    }
    if (!hit) ai->correction = py_clamp(ai->correction - err * ai->p.learn_rate, -30, 30);
}
