#include <math.h>
#include <string.h>
#include "shot.h"

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

static TraceResult trace_new(const Ctx *c, const Launch *L) {
    OwnerProbe p = {c->w, c->tank->index, false};
    return phys_trace(L, probe_cb, &p, 0.02, 12.0);
}

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
    e.pull = MGET(M, MF_PULL, pull, 0.0);
    e.multi = MGET(M, MF_MULTI, multi, 0) != 0;
    return e;
}

static void push(FlightSpec *f, Ev e) {
    if (f->nevents < MAX_EVENTS_PER_FLIGHT) f->events[f->nevents++] = e;
}

static void finale(const Ctx *c, FlightSpec *f, const Impact *imp, bool first, double vx) {
    const AmmoType *M = c->M;
    const World *w = c->w;
    push(f, main_ev(c, imp, first, MGET(M, MF_RADIUS, radius, 1.0), -1));
    if (imp->kind == IMP_OUT) return;
    if (M && (M->mf & MF_CLUSTER) && M->cluster) {
        int n = M->cluster;
        double sp = MGET(M, MF_CSPACE, cspace, 5.5);
        for (int k = 0; k < n; k++) {
            double bx = py_clamp(imp->x + (k - (n - 1) / 2.0) * sp, 2, w->width - 3);
            push(f, mk_ev(bx, terrain_height_at(&w->terrain, bx) + 1.0, IMP_TERRAIN, -1, MGET(M, MF_CR, cr, 0.55), 1, TERR_BLAST,
                          0.09 + 0.07 * k, false));
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
        if (ff) finale(&c, ff, &st.impact, true, vxa);
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
            if (last) finale(&c, f, &imp, b == 0, vxi);
            else push(f, mk_ev(imp.x, imp.y, IMP_TERRAIN, -1, MGET(M, MF_BR, br, 0.4), 1, TERR_BLAST, 0.0, b == 0));
        }
        tcum += st.time;
        if (last) break;
        cur.x0 = imp.x;
        cur.y0 = imp.y + 1.5;
        cur.vx = vxi * 0.82;
        cur.vy = -vyi * 0.55;
        st = trace_new(&c, &cur);
    }
}
