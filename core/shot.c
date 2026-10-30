#include <math.h>
#include <string.h>
#include "shot.h"
#include "xmath.h"

const AmmoType *shot_mechanics(const World *w, const Tank *t, bool special) {
    if (special || !w->ammo_fx) return NULL;
    const AmmoType *a = &AMMOS[t->ammo];
    return a->mf ? a : NULL;
}

/* M.get(key, default) helpers */
#define MGET(M, flag, field, dflt) (((M) && ((M)->mf & (flag))) ? (M)->field : (dflt))

Launch make_ammo_launch(const World *w, const Tank *t, double angle, double power, const double *wind, bool special) {
    const AmmoType *M = shot_mechanics(w, t, special);
    double ox, oy;
    tank_muzzle(t, angle, &ox, &oy);
    double base_wind = wind ? *wind : w->wind;
    Launch L = phys_make_launch(w, ox, oy, angle, power, PHYS_GRAVITY * MGET(M, MF_G, g, 1.0),
                                base_wind * MGET(M, MF_WIND, wind, 1.0));
    double sp = MGET(M, MF_SPEED, speed, 1.0);
    if (sp != 1.0) {
        L.vx = L.vx * sp;
        L.vy = L.vy * sp;
    }
    if (M && (M->mf & MF_WOB)) {
        L.wa = M->wob_a;
        L.wf = M->wob_f;
    }
    return L;
}

typedef struct {
    const World *w;
    const Tank *tank;
    const AmmoType *M;
    bool special;
} Ctx;

static int probe_cb(void *c, double x, double y, int *tank) { return owner_probe((OwnerProbe *)c, x, y, tank); }

static TraceResult trace_max(const Ctx *c, const Launch *L, double max_time) {
    OwnerProbe p = {c->w, c->tank->index, false};
    return phys_trace(L, probe_cb, &p, 0.02, max_time);
}

static TraceResult trace_new(const Ctx *c, const Launch *L) { return trace_max(c, L, 12.0); }

static Ev mk_ev(double x, double y, int kind, int tank, double radius, int dmg, int mode, double delay, bool first) {
    Ev e;
    memset(&e, 0, sizeof(e));
    e.x = x; e.y = y; e.kind = kind; e.tank = tank; e.radius = radius; e.dmg = dmg; e.mode = mode; e.delay = delay; e.first = first;
    return e;
}

static Ev main_ev(const Ctx *c, const Impact *imp, bool first, double radius, int mode /* -1: ammo terrain */) {
    if (c->special) {
        Ev e = mk_ev(imp->x, imp->y, imp->kind, imp->tank, SPECIAL_RADIUS, SPECIAL_DMG, TERR_BLAST, 0.0, first);
        e.big = true;
        return e;
    }
    const AmmoType *M = c->M;
    int m = mode >= 0 ? mode : (int)MGET(M, MF_TERRAIN, terrain, TERR_BLAST);
    Ev e = mk_ev(imp->x, imp->y, imp->kind, imp->tank, radius, (int)MGET(M, MF_DMG, dmg, 1), m, 0.0, first);
    e.pull = MGET(M, MF_PULL, pull, 0.0) - MGET(M, MF_PUSH, push, 0.0);
    e.multi = MGET(M, MF_MULTI, multi, 0) != 0;
    return e;
}

static void push(FlightSpec *f, Ev e) {
    if (f->nevents < MAX_EVENTS_PER_FLIGHT) f->events[f->nevents++] = e;
}

static void finale(const Ctx *c, FlightSpec *f, const Impact *imp, bool first, double vx, double vy) {
    const AmmoType *M = c->M;
    const World *w = c->w;
    push(f, main_ev(c, imp, first, MGET(M, MF_RADIUS, radius, 1.0), -1));
    if (imp->kind == IMP_OUT) return;
    if (M && (M->mf & MF_CLUSTER) && M->cluster) {
        int n = M->cluster;
        double sp = MGET(M, MF_CSPACE, cspace, 5.5), cd = MGET(M, MF_CDELAY, cdelay, 0.09);
        for (int k = 0; k < n; k++) {
            double bx = py_clamp(imp->x + (k - (n - 1) / 2.0) * sp, 2, w->width - 3);
            push(f, mk_ev(bx, terrain_height_at(&w->terrain, bx) + 1.0, IMP_TERRAIN, -1, MGET(M, MF_CR, cr, 0.55), 1, TERR_BLAST,
                          (k == 0 ? cd : 0.09) + 0.07 * k, false));
        }
    }
    if (M && (M->mf & MF_CHAIN) && M->chain) {
        int n = M->chain;
        double st = MGET(M, MF_CSTEP, cstep, 7.0);
        double d = vx >= 0 ? 1.0 : -1.0;
        for (int k = 1; k <= n; k++) {
            double bx = py_clamp(imp->x + d * k * st, 2, w->width - 3);
            push(f, mk_ev(bx, terrain_height_at(&w->terrain, bx) + 1.0, IMP_TERRAIN, -1, MGET(M, MF_CR, cr, 0.6), 1, TERR_BLAST, 0.1, false));
        }
    }
    if (M && (M->mf & MF_PIERCE) && M->pierce) {               /* HARDLIGHT: the lance keeps going, carving a shaft */
        double sp = py_hypot(vx, vy);
        if (sp == 0.0) sp = 1.0;
        double ux = vx / sp, uy = vy / sp, st = MGET(M, MF_PSTEP, pstep, 4.5);
        int n = M->pierce;
        for (int k = 1; k <= n; k++) {
            double px = py_clamp(imp->x + ux * st * k, 2, w->width - 3), py = imp->y + uy * st * k;
            if (py < 1.0) py = 1.0;
            push(f, mk_ev(px, py, IMP_TERRAIN, -1, 0.55, k == n ? 1 : 0, TERR_BLAST, 0.05, false));
        }
    }
    if (M && (M->mf & MF_FUSE) && M->fuse != 0.0)
        push(f, mk_ev(imp->x, imp->y, IMP_TERRAIN, -1, MGET(M, MF_FR, fr, 1.5), (int)MGET(M, MF_DMG, dmg, 1), TERR_BLAST, M->fuse, false));
}

static FlightSpec *new_flight(ShotPlan *out, const Launch *L, TraceResult tr, double delay) {
    if (out->n >= MAX_FLIGHTS) return NULL;
    FlightSpec *f = &out->f[out->n++];
    f->launch = *L;
    f->trace = tr;
    f->delay = delay;
    f->nevents = 0;
    return f;
}

/* the closest living enemy to world-x `x` (with `ahead` != 0: only those up to `reach` px that way) */
static const Tank *nearest_foe(const World *w, const Tank *tank, double x, double ahead, double reach) {
    const Tank *best = NULL;
    for (int i = 0; i < w->ntanks; i++) {
        const Tank *t = &w->tanks[i];
        if (!tank_alive(t) || t->team == tank->team) continue;
        double dx = t->x - x;
        if (ahead != 0.0 && !(0 < dx * ahead && dx * ahead <= reach)) continue;
        if (!best || fabs(dx) < fabs(best->x - x)) best = t;
    }
    return best;
}

/* SEEK: from (x, y) with velocity (vx, vy), bend the horizontal speed `k` of the way toward the nearest enemy */
static Launch reaim(const Ctx *c, const Launch *L, double x, double y, double vx, double vy, double k) {
    const Tank *foe = nearest_foe(c->w, c->tank, x, 0.0, 1e9);
    Launch sub = *L;
    sub.x0 = x; sub.y0 = y; sub.vx = vx; sub.vy = vy; sub.wa = 0.0;
    if (!foe || k <= 0) return sub;
    double ty = foe->y + tank_h(foe) * 0.5;
    double g = L->g > 1e-3 ? L->g : 1e-3;
    double disc = vy * vy + 2 * g * (y - ty);
    if (disc <= 0) return sub;
    double tf = (vy + sqrt(disc)) / g;
    double want = (foe->x - x - 0.5 * L->ax * tf * tf) / (tf > 0.05 ? tf : 0.05);
    double tf_land = tf;
    double shift = py_clamp(k, 0, 1) * (want - vx) * tf_land;
    shift = py_clamp(shift, -18.0, 18.0);
    sub.vx = vx + shift / (tf_land > 0.05 ? tf_land : 0.05);
    return sub;
}

static Launch mk_launch(double x0, double y0, double vx, double vy, double g) {
    Launch L;
    memset(&L, 0, sizeof(L));
    L.x0 = x0; L.y0 = y0; L.vx = vx; L.vy = vy; L.g = g;
    return L;
}

/* follow-up flights that start where the shell came down (BURROWER, SERPENT, CONSTELLATION) */
static void after_landing(const Ctx *c, ShotPlan *out, const Launch *L, const Impact *imp, double t_land, double vx) {
    const AmmoType *M = c->M;
    const World *w = c->w;
    int W = w->width;
    if (c->special || !M || imp->kind == IMP_OUT) return;
    if ((M->mf & MF_BURROW) && M->burrow != 0.0 && imp->kind == IMP_TERRAIN) {
        double d = vx >= 0 ? 1.0 : -1.0, reach = M->burrow;
        const Tank *foe = nearest_foe(w, c->tank, imp->x, d, reach);
        double ex = foe ? foe->x : py_clamp(imp->x + d * reach, 3, W - 4);
        double dist = fabs(ex - imp->x);
        if (dist >= 2) {
            double y0 = terrain_height_at(&w->terrain, imp->x) - 2.5, speed = 34.0;
            Launch sub = mk_launch(imp->x, y0, d * speed, 0.0, 0.0);
            double ty = terrain_height_at(&w->terrain, ex) + 1.0;
            TraceResult tr = {{ex, y0, IMP_OUT, -1}, dist / speed};
            FlightSpec *f = new_flight(out, &sub, tr, t_land + 0.25);
            if (f) push(f, mk_ev(ex, ty, foe ? IMP_TANK : IMP_TERRAIN, foe ? foe->index : -1, MGET(M, MF_RADIUS, radius, 1.3),
                                 (int)MGET(M, MF_DMG, dmg, 1), TERR_BLAST, 0.0, false));
        }
    }
    if ((M->mf & MF_SLITHER) && M->slither && imp->kind == IMP_TERRAIN) {     /* hops that steer toward the nearest enemy */
        double x = imp->x, y = imp->y, t0 = t_land;
        for (int k = 0; k < M->slither; k++) {
            const Tank *foe = nearest_foe(w, c->tank, x, 0.0, 1e9);
            if (!foe) break;
            double dx = foe->x - x;
            double hop = py_clamp(fabs(dx), 4.0, 16.0), vy0 = 26.0;
            double tf = 2 * vy0 / (L->g > 1e-3 ? L->g : 1e-3);
            Launch sub = mk_launch(x, y + 1.5, copysign(hop / tf, dx), vy0, L->g);
            TraceResult st = trace_max(c, &sub, 6.0);
            bool last = k == M->slither - 1;
            FlightSpec *f = new_flight(out, &sub, st, t0 + 0.12);
            if (f) push(f, main_ev(c, &st.impact, false, last ? MGET(M, MF_RADIUS, radius, 0.9) : 0.55, -1));
            t0 += 0.12 + st.time;
            x = st.impact.x; y = st.impact.y;
            if (st.impact.kind != IMP_TERRAIN) break;
        }
    }
    if ((M->mf & MF_STARS) && M->stars) {                       /* a pentagon of stars falls from the top of the sky */
        int n = M->stars;
        const double TAU = 6.283185307179586, PI = 3.141592653589793;
        for (int k = 0; k < n; k++) {
            double ang = PI / 2 + k * TAU / n;
            double sx = py_clamp(imp->x + py_cos(ang) * 15.0, 3, W - 4);
            double top = w->height + 4.0;
            Launch sub = mk_launch(sx + 4.0, top, -4.0, -48.0, L->g);
            TraceResult st = trace_max(c, &sub, 8.0);
            FlightSpec *f = new_flight(out, &sub, st, t_land + 0.3 + 0.14 * k);
            if (f) push(f, main_ev(c, &st.impact, false, 0.65, TERR_BLAST));
        }
    }
}

void plan_shot(const World *w, const Tank *tank, bool special, ShotPlan *out) {
    Ctx c = {w, tank, shot_mechanics(w, tank, special), special};
    const AmmoType *M = c.M;
    out->n = 0;
    Launch L = make_ammo_launch(w, tank, tank->angle, tank->power, NULL, special);
    TraceResult tr = trace_new(&c, &L);
    double t_ap = L.g > 0 ? L.vy / L.g : 0.0;
    if (M && (M->mf & MF_APEX) && 0.35 < t_ap && t_ap < tr.time - 0.2) {
        double ax, ay;
        launch_at(&L, t_ap, &ax, &ay);
        double vxa = L.vx + L.ax * t_ap, vya = L.vy - L.g * t_ap;
        if (M->apex_kind == APEX_MIRROR) {                     /* PARADOX: the shell flies on; its twin falls across the centre line */
            double vxi = L.vx + L.ax * tr.time, vyi = L.vy - L.g * tr.time;
            FlightSpec *fm = new_flight(out, &L, tr, 0.0);
            if (fm) finale(&c, fm, &tr.impact, true, vxi, vyi);
            double mx = py_clamp(w->width - ax, 4, w->width - 5);
            Launch twin = L;
            twin.x0 = mx; twin.y0 = ay; twin.vx = -vxa; twin.vy = vya; twin.ax = -L.ax; twin.wa = 0.0;
            TraceResult st = trace_new(&c, &twin);
            TraceResult portal = {{mx, ay, IMP_OUT, -1}, 0.0};
            FlightSpec *fp = new_flight(out, &twin, portal, t_ap);
            if (fp) push(fp, mk_ev(mx, ay, IMP_OUT, -1, 0.5, 0, TERR_FX, 0.0, false));
            FlightSpec *ft = new_flight(out, &twin, st, t_ap + 0.02);
            if (ft) push(ft, main_ev(&c, &st.impact, false, MGET(M, MF_RADIUS, radius, 1.0), -1));
            return;
        }
        TraceResult tap = {{ax, ay, IMP_OUT, -1}, t_ap};
        FlightSpec *f0 = new_flight(out, &L, tap, 0.0);
        if (M->apex_kind == APEX_JUMP && f0) push(f0, mk_ev(ax, ay, IMP_OUT, -1, 0.5, 0, TERR_FX, 0.0, false));
        if (M->apex_kind == APEX_SPLIT) {
            int n = M->apex_n;
            double spread = M->apex_v;
            for (int k = 0; k < n; k++) {
                double half = (n - 1) / 2.0;
                double f = (k - half) / (1.0 > half ? 1.0 : half);
                Launch sub = L;
                sub.x0 = ax + f * 1.5;
                sub.y0 = ay;
                sub.vx = vxa * (1 + spread * f) + f * spread * 6;
                sub.vy = vya + 1.0 - fabs(f) * 3.0;
                sub.wa = 0.0;
                if (M->mf & MF_SEEK) sub = reaim(&c, &sub, sub.x0, sub.y0, sub.vx, sub.vy, M->seek);
                TraceResult st = trace_new(&c, &sub);
                FlightSpec *fs = new_flight(out, &sub, st, t_ap + 0.04 * k);
                if (fs) push(fs, main_ev(&c, &st.impact, k == 0, MGET(M, MF_SUBR, subr, 0.6), TERR_BLAST));
            }
            return;
        }
        double dirx = vxa >= 0 ? 1.0 : -1.0;
        double nx = py_clamp(ax + dirx * M->apex_v, 4, w->width - 5);
        Launch sub = L;
        sub.x0 = nx;
        sub.y0 = ay;
        sub.vx = vxa;
        sub.vy = vya;
        sub.wa = 0.0;
        TraceResult st = trace_new(&c, &sub);
        TraceResult portal = {{nx, ay, IMP_OUT, -1}, 0.0};
        FlightSpec *fp = new_flight(out, &sub, portal, t_ap + 0.1);
        if (fp) push(fp, mk_ev(nx, ay, IMP_OUT, -1, 0.5, 0, TERR_FX, 0.0, false));
        FlightSpec *ff = new_flight(out, &sub, st, t_ap + 0.12);
        if (ff) finale(&c, ff, &st.impact, true, vxa, -1.0);
        return;
    }
    if (M && (M->mf & MF_SEEK) && M->seek != 0.0 && 0.35 < t_ap && t_ap < tr.time - 0.2) {   /* ANGLER'S LURE */
        double ax, ay;
        launch_at(&L, t_ap, &ax, &ay);
        double vxa = L.vx + L.ax * t_ap, vya = L.vy - L.g * t_ap;
        TraceResult tap = {{ax, ay, IMP_OUT, -1}, t_ap};
        FlightSpec *f0 = new_flight(out, &L, tap, 0.0);
        if (f0) push(f0, mk_ev(ax, ay, IMP_OUT, -1, 0.35, 0, TERR_FX, 0.0, false));
        Launch sub = reaim(&c, &L, ax, ay, vxa, vya, M->seek);
        TraceResult st = trace_new(&c, &sub);
        FlightSpec *f = new_flight(out, &sub, st, t_ap);
        if (f) finale(&c, f, &st.impact, true, sub.vx, -1.0);
        after_landing(&c, out, &L, &st.impact, t_ap + st.time, sub.vx);
        return;
    }
    if (M && (M->mf & MF_VOLLEY) && M->volley) {              /* BROADSIDE: short, true and long, fired together */
        int n = M->volley;
        for (int k = 0; k < n; k++) {
            double f = (k - (n - 1) / 2.0) * 0.085;
            Launch sub = L;
            sub.vx = L.vx * (1 + f);
            sub.vy = L.vy * (1 + f);
            TraceResult st = trace_new(&c, &sub);
            FlightSpec *fv = new_flight(out, &sub, st, 0.11 * k);
            if (fv) push(fv, main_ev(&c, &st.impact, k == (n - 1) / 2, MGET(M, MF_RADIUS, radius, 0.8), -1));
        }
        return;
    }
    int bounces = M && (M->mf & MF_BOUNCE) ? M->bounce : 0;
    Launch cur = L;
    TraceResult st = tr;
    double tcum = 0.0;
    for (int b = 0; b <= bounces; b++) {
        Impact imp = st.impact;
        double vxi = cur.vx + cur.ax * st.time, vyi = cur.vy - cur.g * st.time;
        bool last = b == bounces || imp.kind != IMP_TERRAIN || vyi > -4;
        FlightSpec *f = new_flight(out, &cur, st, tcum);
        if (f) {
            if (last && M && (M->mf & MF_BURROW) && M->burrow != 0.0 && imp.kind == IMP_TERRAIN && !special)
                push(f, mk_ev(imp.x, imp.y, imp.kind, imp.tank, 0.5, (int)MGET(M, MF_DMG, dmg, 1), TERR_TRENCH, 0.0, b == 0));   /* the worm's entry hole */
            else if (last) finale(&c, f, &imp, b == 0, vxi, vyi);
            else push(f, mk_ev(imp.x, imp.y, IMP_TERRAIN, -1, MGET(M, MF_BR, br, 0.4), 1, TERR_BLAST, 0.0, b == 0));
        }
        tcum += st.time;
        if (last) {
            after_landing(&c, out, &L, &imp, tcum, vxi);
            break;
        }
        cur.x0 = imp.x;
        cur.y0 = imp.y + 1.5;
        cur.vx = vxi * 0.82;
        cur.vy = -vyi * 0.55;
        st = trace_new(&c, &cur);
    }
}
