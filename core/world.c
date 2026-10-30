#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "world.h"
#include "xmath.h"

#define TAU 6.283185307179586

/* ---------------------------------------------------------------- terrain (l.869, 1198-1285) */
double sample_profile(const double *pts, int n, double u, int smooth) {
    for (int i = 0; i < n - 1; i++) {
        double x0 = pts[2 * i], h0 = pts[2 * i + 1], x1 = pts[2 * i + 2], h1 = pts[2 * i + 3];
        if (u <= x1) {
            double t = x1 > x0 ? (u - x0) / (x1 - x0) : 1.0;
            if (!smooth) return h0 + (h1 - h0) * t;
            double p0 = pts[2 * (i - 1 < 0 ? 0 : i - 1) + 1];
            double p3 = pts[2 * (i + 2 > n - 1 ? n - 1 : i + 2) + 1];
            double v = 0.5 * (2 * h0 + (-p0 + h1) * t + (2 * p0 - 5 * h0 + 4 * h1 - p3) * t * t
                              + (-p0 + 3 * h0 - 3 * h1 + p3) * py_cube(t));
            return v > 0.02 ? v : 0.02;   /* max(0.02, v) */
        }
    }
    return pts[2 * (n - 1) + 1];
}

void terrain_init(Terrain *t, int w, int h, const MapDef *m, PyRandom *rng) {
    t->width = w;
    t->height = h;
    t->version = 0;
    double ph[3];
    for (int i = 0; i < 3; i++) ph[i] = rng_uniform(rng, 0, TAU);
    double a = (double)h, b = w * 0.55;
    double scale = (a < b ? a : b) * 0.74;          /* min(height, width*0.55) * 0.74 */
    for (int x = 0; x < w; x++) {
        double u = (double)x / (double)(w - 1 > 1 ? w - 1 : 1);
        double hh = sample_profile(m->pts, m->npts, u, m->smooth);
        double r = m->roughness;
        if (r != 0.0)
            hh += r * (py_sin(u * TAU * 7 + ph[0]) + 0.5 * py_sin(u * TAU * 17 + ph[1]) + 0.25 * py_sin(u * TAU * 41 + ph[2]));
        t->heights[x] = (int)py_clamp(hh * scale + 2, 2, h - 10);
        t->scorch[x] = 0.0;
    }
}

int terrain_height_at(const Terrain *t, double x) {
    return t->heights[(int)py_clamp(x, 0, t->width - 1)];
}

int terrain_support(const Terrain *t, double x) {
    int xi = (int)x;
    int lo = xi - 5 < 0 ? 0 : xi - 5, hi = xi + 6 < t->width ? xi + 6 : t->width;
    int m = 1 << 30;
    for (int i = lo; i < hi; i++) if (t->heights[i] < m) m = t->heights[i];
    return m;
}

void terrain_flatten(Terrain *t, double cx, int half) {
    int xc = (int)cx;
    int target = t->heights[xc];
    int lo = xc - half < 0 ? 0 : xc - half, hi = xc + half + 1 < t->width ? xc + half + 1 : t->width;
    for (int x = lo; x < hi; x++) {
        int edge = half - abs(x - xc);
        double weight = edge >= 3 ? 1.0 : edge / 3.0;
        double v = t->heights[x] + (target - t->heights[x]) * weight;   /* lerp */
        t->heights[x] = (int)rint(v);
    }
}

static void span(const Terrain *t, double cx, double r, int *x0, int *x1) {
    int a = (int)(cx - r) - 1, b = (int)(cx + r) + 1;
    *x0 = a < 0 ? 0 : a;
    *x1 = b > t->width - 1 ? t->width - 1 : b;
}

void terrain_carve(Terrain *t, double cx, double cy, double r, int *ox0, int *ox1) {
    int x0, x1;
    span(t, cx, r, &x0, &x1);
    for (int x = x0; x <= x1; x++) {
        double dx = x + 0.5 - cx;
        if (fabs(dx) > r) continue;
        double dy = sqrt(r * r - dx * dx);
        double low = cy - dy;
        int h = t->heights[x];
        if (low < h && cy + dy >= h - 1) {
            int v = (int)low;
            t->heights[x] = v > 0 ? v : 0;
            double s = 1 - fabs(dx) / r * 0.6;
            if (s > t->scorch[x]) t->scorch[x] = s;
        }
    }
    t->version++;
    if (ox0) *ox0 = x0;
    if (ox1) *ox1 = x1;
}

void terrain_mound(Terrain *t, double cx, double r, double height, int *ox0, int *ox1) {
    int x0, x1;
    span(t, cx, r, &x0, &x1);
    for (int x = x0; x <= x1; x++) {
        double u = (x + 0.5 - cx) / r;
        if (fabs(u) <= 1) {
            double v = t->heights[x] + height * (1 - u * u);
            double cap = (double)(t->height - 12);
            t->heights[x] = (int)(cap < v ? cap : v);
        }
    }
    t->version++;
    if (ox0) *ox0 = x0;
    if (ox1) *ox1 = x1;
}

void terrain_trench(Terrain *t, double cx, double half, double depth, int *ox0, int *ox1) {
    int x0, x1;
    span(t, cx, half, &x0, &x1);
    for (int x = x0; x <= x1; x++) {
        double u = fabs(x + 0.5 - cx) / half;
        if (u <= 1) {
            int v = (int)(t->heights[x] - depth * (1 - 0.5 * u * u));
            t->heights[x] = v > 1 ? v : 1;
            if (0.8 > t->scorch[x]) t->scorch[x] = 0.8;
        }
    }
    t->version++;
    if (ox0) *ox0 = x0;
    if (ox1) *ox1 = x1;
}

void terrain_smooth(Terrain *t, double cx, double r, int *ox0, int *ox1) {
    int x0, x1;
    span(t, cx, r, &x0, &x1);
    long sum = 0;
    int n = x1 - x0 + 1;
    for (int x = x0; x <= x1; x++) sum += t->heights[x];
    int level = (int)(sum / (n > 1 ? n : 1));
    for (int x = x0; x <= x1; x++) {
        double u = fabs(x + 0.5 - cx) / r;
        if (u <= 1) {
            double k = u < 0.7 ? 1.0 : (1 - u) / 0.3;
            t->heights[x] = (int)(t->heights[x] + (level - t->heights[x]) * k);
            if (0.4 > t->scorch[x]) t->scorch[x] = 0.4;
        }
    }
    t->version++;
    if (ox0) *ox0 = x0;
    if (ox1) *ox1 = x1;
}

/* ---------------------------------------------------------------- world (l.3743) */
static void copy_str(char *dst, const char *src, size_t n) {
    strncpy(dst, src, n - 1);
    dst[n - 1] = 0;
}

void world_roll_wind(World *w) {
    double m = w->wind_max;
    if (m > 0) {
        double sgn = rng_below(&w->rng, 2) == 0 ? -1.0 : 1.0;   /* choice((-1, 1)) */
        w->wind = py_round1(sgn * rng_uniform(&w->rng, 0.2, 1.0) * m);
    } else {
        w->wind = 0.0;
    }
}

void world_drift_wind(World *w) {
    double m = w->wind_max;
    if (m > 0) w->wind = py_round1(py_clamp(w->wind + rng_uniform(&w->rng, -0.45, 0.45) * m, -m, m));
}

/* Scenery(width, height, theme, rng) - l.1437. These draws are visual but come from the simulation rng. */
void world_scenery_init(World *w) {
    int width = w->width, height = w->height;
    const MapDef *m = &MAPDEFS[w->map];
    const MapTheme *th = &THEMES[m->theme];
    w->orb_x = (int)(width * rng_uniform(&w->rng, 0.35, 0.65));
    for (int i = 0; i < 6; i++) w->ridge_ph[i] = rng_uniform(&w->rng, 0, TAU);
    int nst = (int)(width * height * th->star_density * 3) + 8;
    int sy = (int)(height * 0.38);
    w->nstars = 0;
    for (int i = 0; i < nst; i++) {
        Star s;
        s.x = (uint8_t)rng_below(&w->rng, (uint32_t)width);
        s.py = (uint8_t)rng_below(&w->rng, (uint32_t)(sy > 1 ? sy : 1));
        s.ph = (float)rng_uniform(&w->rng, 0, TAU);
        s.sp = (float)rng_uniform(&w->rng, 1.0, 3.0);
        s.b = (float)rng_uniform(&w->rng, 0.4, 1.0);
        if (w->nstars < MAX_STARS) w->stars[w->nstars++] = s;
    }
    w->nmotes = 0;
    if (MOTE_STYLES[th->fx].on) {
        int nm = (int)(width * height * MOTE_STYLES[th->fx].density);
        if (nm < 16) nm = 16;
        for (int i = 0; i < nm; i++) {
            Mote mo;
            mo.x0 = (float)rng_uniform(&w->rng, 0, width);
            mo.y0 = (float)rng_uniform(&w->rng, 0, height);
            mo.sp = (float)rng_uniform(&w->rng, 0.6, 1.4);
            mo.ph = (float)rng_uniform(&w->rng, 0, TAU);
            mo.amp = (float)rng_uniform(&w->rng, 0.5, 2.5);
            mo.ci = (uint8_t)rng_below(&w->rng, 3);
            if (w->nmotes < MAX_MOTES) w->motes[w->nmotes++] = mo;
        }
    }
}

void world_init(World *w, int width, int height, int map, uint32_t seed, const PlayerSetup *players, int n, double wind_setting) {
    memset(w, 0, sizeof(*w));
    w->width = width;
    w->height = height;
    w->map = map;
    w->wind_max = wind_setting;
    w->ammo_fx = true;
    rng_seed(&w->rng, seed);
    const MapDef *m = &MAPDEFS[map];
    terrain_init(&w->terrain, width, height, m, &w->rng);
    w->max_speed = sqrt(PHYS_GRAVITY * width * PHYS_RANGE);
    int slots[MAX_TANKS + 2];
    for (int i = 0; i < MAX_TANKS + 2; i++) slots[i] = -1;
    bool all_unit = true;
    for (int i = 0; i < n; i++) if (players[i].scale != 1.0) all_unit = false;
    w->ntanks = n;
    for (int i = 0; i < n; i++) {
        const PlayerSetup *pl = &players[i];
        double jitter = rng_uniform(&w->rng, -0.02, 0.02);
        int team = pl->team >= 0 ? pl->team : i;
        int k = slots[team] = slots[team] + 1;
        double gap = all_unit ? 0.13 : 0.18;
        double frac = team == 0 ? m->spawns[0] + k * gap : m->spawns[1] - k * gap;
        int half = (int)(TANK_W * pl->scale / 2);
        int x = (int)py_clamp((frac + jitter) * width, 9 + half, width - 10 - half);
        terrain_flatten(&w->terrain, x + 0.5, 10);
        double hover = pl->hover != 0.0 ? pl->hover : DESIGNS[pl->design].hover;
        Tank *t = &w->tanks[i];
        memset(t, 0, sizeof(*t));
        t->index = i;
        copy_str(t->name, pl->name, sizeof(t->name));
        copy_str(t->label, pl->label, sizeof(t->label));
        t->color = pl->tank_color;
        t->shot_color = pl->shot_color;
        t->is_ai = pl->is_ai;
        t->x = x + 0.5;
        t->y = (double)terrain_support(&w->terrain, x) + hover;
        t->facing = team == 0 ? 1 : -1;
        t->angle = team == 0 ? 50.0 : 130.0;
        t->power = 60.0;
        t->hp = t->max_hp = pl->hp;
        t->hull_sentinel = pl->tank_color;
        t->shot_sentinel = pl->shot_color;
        t->rgb_hull = rgb_is_live(pl->tank_color);
        t->rgb_shot = rgb_is_live(pl->shot_color);
        t->design = pl->design;
        t->ammo = pl->ammo;
        t->team = team;
        t->owner = pl->owner;
        t->kind = pl->kind;
        t->scale = pl->scale;
        t->hover = hover;
        t->shield = pl->shield;
        t->guard = pl->guard;
    }
    world_scenery_init(w);
    world_roll_wind(w);
    int ns = width * height / 450;
    if (ns < 8) ns = 8;
    w->nstreaks = 0;
    for (int i = 0; i < ns; i++) {
        double sx = rng_uniform(&w->rng, 0, width);
        double syy = rng_uniform(&w->rng, height * 0.25, height - 2);
        if (w->nstreaks < MAX_STREAKS) {
            w->streaks[w->nstreaks][0] = sx;
            w->streaks[w->nstreaks][1] = syy;
            w->nstreaks++;
        }
    }
}

int world_collision(const World *w, double x, double y, int skip, int *tank) {
    *tank = -1;
    if (x < 0 || x >= w->width) return IMP_OUT;
    if (y < w->terrain.heights[(int)x]) return IMP_TERRAIN;
    double r = PHYS_PROJ_RADIUS;
    for (int i = 0; i < w->ntanks; i++) {
        const Tank *t = &w->tanks[i];
        if (t->index == skip || t->destroyed) continue;
        double hw = tank_w(t) / 2;
        double x0 = t->x - hw, y0 = t->y, x1 = t->x + hw, y1 = t->y + tank_h(t);
        if (x0 - r <= x && x <= x1 + r && y0 - r <= y && y <= y1 + r) {
            *tank = t->index;
            return IMP_TANK;
        }
    }
    return IMP_NONE;
}

bool world_inside_tank(const World *w, int idx, double x, double y) {
    const Tank *t = &w->tanks[idx];
    double hw = tank_w(t) / 2;
    double x0 = t->x - hw, y0 = t->y, x1 = t->x + hw, y1 = t->y + tank_h(t);
    return x0 - 1.5 <= x && x <= x1 + 1.5 && y0 - 1.5 <= y && y <= y1 + 1.5;
}

int owner_probe(OwnerProbe *p, double x, double y, int *tank) {
    if (!p->left && !world_inside_tank(p->w, p->owner, x, y)) p->left = true;
    return world_collision(p->w, x, y, p->left ? -1 : p->owner, tank);
}

double world_distance_to_tank(const Tank *t, double x, double y) {
    double hw = tank_w(t) / 2;
    double x0 = t->x - hw, y0 = t->y, x1 = t->x + hw, y1 = t->y + tank_h(t);
    return py_hypot(x - py_clamp(x, x0, x1), y - py_clamp(y, y0, y1));
}

bool world_settle(World *w, double dt) {
    bool moving = false;
    for (int i = 0; i < w->ntanks; i++) {
        Tank *t = &w->tanks[i];
        if (t->destroyed) continue;
        double target = (double)terrain_support(&w->terrain, t->x) + t->hover;
        if (t->y > target) {
            double ny = t->y - PHYS_FALL * dt;
            t->y = target > ny ? target : ny;
            moving = true;
        } else if (t->y < target) {
            t->y = target;
        }
    }
    return moving;
}

void world_terrain_op(World *w, int mode, double x, double y, double r) {
    Terrain *t = &w->terrain;
    if (mode == TERR_MOUND) terrain_mound(t, x, r * 1.25, r * 0.9, NULL, NULL);
    else if (mode == TERR_TRENCH) terrain_trench(t, x, (2.0 > r * 0.45 ? 2.0 : r * 0.45), r * 1.5, NULL, NULL);
    else if (mode == TERR_GLASS) terrain_smooth(t, x, r * 1.4, NULL, NULL);
    else terrain_carve(t, x, y, r, NULL, NULL);
}

/* ---------------------------------------------------------------- ballistics (l.3655-3711, 2333) */
double phys_speed_for(const World *w, double power) {
    return w->max_speed * py_clamp(power / PHYS_MAX_POWER, 0, 1);
}

Launch phys_make_launch(const World *w, double ox, double oy, double angle, double power, double gravity, double wind) {
    double a = PY_RADIANS(angle), v = phys_speed_for(w, power);
    Launch L = {ox, oy, v * py_cos(a), v * py_sin(a), gravity, wind, 0.0, 0.0};
    return L;
}

void launch_at(const Launch *L, double t, double *ox, double *oy) {
    double x = L->x0 + L->vx * t + 0.5 * L->ax * t * t;
    double y = L->y0 + L->vy * t - 0.5 * L->g * t * t;
    if (L->wa != 0.0) {
        double sp = py_hypot(L->vx, L->vy);
        if (sp == 0.0) sp = 1.0;
        double k = t * 2.5;
        double s = L->wa * py_sin(L->wf * t) * (1.0 < k ? 1.0 : k);
        double nx = x - L->vy / sp * s, ny = y + L->vx / sp * s;
        x = nx;
        y = ny;
    }
    *ox = x;
    *oy = y;
}

void muzzle_point(double x, double y, double angle, int design, double scale, double recoil, double *mx, double *my) {
    double a = PY_RADIANS(angle);
    double d = (BARRELS[DESIGNS[design].barrel].length - recoil * 2.5 + 1.0) * scale;
    *mx = x + d * py_cos(a);
    *my = y + PIVOT_H * scale + d * py_sin(a);
}

void tank_muzzle(const Tank *t, double angle, double *x, double *y) {
    if (t->scale == 1.0) {
        muzzle_point(t->x, t->y, angle, t->design, 1.0, 0.0, x, y);
        return;
    }
    double a = PY_RADIANS(angle);
    double d = tank_w(t) * 0.45 + 1.0, ph = tank_h(t) * 0.35;
    *x = t->x + d * py_cos(a);
    *y = t->y + ph + d * py_sin(a);
}

TraceResult phys_trace(const Launch *L, ProbeFn probe, void *ctx, double dt, double max_time) {
    TraceResult r;
    double t = 0.0, px = L->x0, py = L->y0;
    while (t < max_time) {
        t += dt;
        double x, y;
        launch_at(L, t, &x, &y);
        int tank;
        int hit = probe(ctx, x, y, &tank);
        if (hit) {
            r.impact.x = x; r.impact.y = y; r.impact.kind = hit; r.impact.tank = tank;
            r.time = t;
            return r;
        }
        px = x;
        py = y;
    }
    r.impact.x = px; r.impact.y = py; r.impact.kind = IMP_OUT; r.impact.tank = -1;
    r.time = t;
    return r;
}

int phys_trace_points(const Launch *L, ProbeFn probe, void *ctx, double dt, double spacing, double max_time,
                      float *pts, int max_pts) {
    int n = 0;
    double acc = 0.0, t = 0.0, px = L->x0, py = L->y0;
    while (t < max_time) {
        t += dt;
        double x, y;
        launch_at(L, t, &x, &y);
        int tank;
        int hit = probe(ctx, x, y, &tank);
        acc += py_hypot(x - px, y - py);
        if (acc >= spacing) {
            if (n < max_pts) { pts[2 * n] = (float)x; pts[2 * n + 1] = (float)y; n++; }
            acc = 0.0;
        }
        if (hit) return n;
        px = x;
        py = y;
    }
    return n;
}
