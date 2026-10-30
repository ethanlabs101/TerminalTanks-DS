/* Correctly rounded (in practice) sin, cos, log, hypot and cube for the DS build.
 *
 * Python's math functions come from the platform libm (glibc on Linux), whose sin/cos/log/hypot/pow return the
 * correctly rounded double for practically every input. The DS's picolibc versions are accurate to about 1 ulp
 * and differ from glibc in the last bit on a few percent of inputs (measured with tests/ds_selftest). Since the
 * simulation has to follow the original's numbers exactly, the DS build evaluates these functions in double-double
 * arithmetic (~100 bits) and rounds once, which yields the correctly rounded result, i.e. glibc's result.
 * Only plain IEEE double + - * / are used (no FMA): exact products come from Dekker's split.
 * Domain: finite arguments of moderate size (|x| < 2^20 for sin/cos), which covers every call in core/. */
#include <math.h>
#include "xmath.h"

typedef struct { double hi, lo; } dd;

static inline dd two_sum(double a, double b) {
    double s = a + b, bb = s - a;
    dd r = {s, (a - (s - bb)) + (b - bb)};
    return r;
}
static inline dd quick_two_sum(double a, double b) {
    double s = a + b;
    dd r = {s, b - (s - a)};
    return r;
}
static inline void split(double a, double *h, double *l) {
    double t = 134217729.0 * a;   /* 2^27 + 1 */
    *h = t - (t - a);
    *l = a - *h;
}
static inline dd two_prod(double a, double b) {
    double p = a * b, ah, al, bh, bl;
    split(a, &ah, &al);
    split(b, &bh, &bl);
    dd r = {p, ((ah * bh - p) + ah * bl + al * bh) + al * bl};
    return r;
}
static inline dd dd_add(dd a, dd b) {
    dd s = two_sum(a.hi, b.hi), t = two_sum(a.lo, b.lo);
    s.lo += t.hi;
    s = quick_two_sum(s.hi, s.lo);
    s.lo += t.lo;
    return quick_two_sum(s.hi, s.lo);
}
static inline dd dd_neg(dd a) { dd r = {-a.hi, -a.lo}; return r; }
static inline dd dd_mul(dd a, dd b) {
    dd p = two_prod(a.hi, b.hi);
    p.lo += a.hi * b.lo + a.lo * b.hi;
    return quick_two_sum(p.hi, p.lo);
}
static inline dd dd_mul_d(dd a, double b) {
    dd p = two_prod(a.hi, b);
    p.lo += a.lo * b;
    return quick_two_sum(p.hi, p.lo);
}
static inline dd dd_div(dd a, dd b) {
    double q1 = a.hi / b.hi;
    dd r = dd_add(a, dd_neg(dd_mul_d(b, q1)));
    double q2 = r.hi / b.hi;
    r = dd_add(r, dd_neg(dd_mul_d(b, q2)));
    double q3 = r.hi / b.hi;
    dd q = quick_two_sum(q1, q2);
    return dd_add(q, (dd){q3, 0.0});
}
static inline dd dd_from(double a) { dd r = {a, 0.0}; return r; }

/* pi/2 as a sum of three doubles (159 bits) */
static const double PIO2_1 = 1.5707963267948966, PIO2_2 = 6.123233995736766e-17, PIO2_3 = -1.4973849048591698e-33;
static const double PIO2_3B = 5.562271104316826e-50;
static const dd TWO_OVER_PI = {0.6366197723675814, -3.935735335036497e-17};

/* 1/k! for k = 0..31 as double-doubles (computed once) */
static dd INV_FACT[32];
static int g_init;
static void init_tables(void) {
    INV_FACT[0] = dd_from(1.0);
    for (int k = 1; k < 32; k++) INV_FACT[k] = dd_div(INV_FACT[k - 1], dd_from((double)k));
    g_init = 1;
}

/* r = x - k*pi/2 in double-double, |r| <= ~pi/4 */
static dd reduce(double x, int *quad) {
    dd q = dd_mul_d(TWO_OVER_PI, x);
    double k = nearbyint(q.hi);
    /* nearbyint of the hi part can be off by one when q.hi is within lo of a .5 boundary: fix below */
    dd r = dd_add(dd_from(x), dd_neg(two_prod(k, PIO2_1)));
    r = dd_add(r, dd_neg(two_prod(k, PIO2_2)));
    r = dd_add(r, dd_neg(two_prod(k, PIO2_3)));
    r = dd_add(r, dd_neg(two_prod(k, PIO2_3B)));
    long ki = (long)k;
    if (r.hi > 0.7853981633974483) { /* > pi/4: move one quadrant */
        r = dd_add(r, dd_neg(dd_from(PIO2_1)));
        r = dd_add(r, dd_neg(dd_from(PIO2_2)));
        r = dd_add(r, dd_neg(dd_from(PIO2_3)));
        ki++;
    } else if (r.hi < -0.7853981633974483) {
        r = dd_add(r, dd_from(PIO2_1));
        r = dd_add(r, dd_from(PIO2_2));
        r = dd_add(r, dd_from(PIO2_3));
        ki--;
    }
    *quad = (int)(ki & 3);
    return r;
}

/* Taylor series in double-double: sin(r) = sum (-1)^n r^(2n+1)/(2n+1)!, cos(r) = sum (-1)^n r^(2n)/(2n)! */
static dd dd_sin_core(dd r) {
    dd r2 = dd_mul(r, r), term = r, sum = r;
    for (int k = 3; k < 31; k += 2) {
        term = dd_mul(term, r2);
        dd t = dd_mul(term, INV_FACT[k]);
        sum = dd_add(sum, ((k >> 1) & 1) ? dd_neg(t) : t);
        if (fabs(t.hi) < 1e-36) break;
    }
    return sum;
}
static dd dd_cos_core(dd r) {
    dd r2 = dd_mul(r, r), term = dd_from(1.0), sum = dd_from(1.0);
    for (int k = 2; k < 31; k += 2) {
        term = dd_mul(term, r2);
        dd t = dd_mul(term, INV_FACT[k]);
        sum = dd_add(sum, ((k >> 1) & 1) ? dd_neg(t) : t);
        if (fabs(t.hi) < 1e-36) break;
    }
    return sum;
}

/* the game asks for the same angles over and over (integer degrees in the AI grid): remember recent results */
typedef struct { unsigned long long key; double v; int ok; } Memo;
static Memo g_msin[256], g_mcos[256];
static inline unsigned long long bits(double x) { union { double d; unsigned long long u; } c = {x}; return c.u; }
static inline unsigned memo_slot(unsigned long long u) { return (unsigned)((u * 0x9E3779B97F4A7C15ull) >> 56); }
static double x_sin_raw(double x);
static double x_cos_raw(double x);
double x_sin(double x) {
    unsigned long long k = bits(x);
    Memo *m = &g_msin[memo_slot(k)];
    if (m->ok && m->key == k) return m->v;
    m->v = x_sin_raw(x);
    m->key = k;
    m->ok = 1;
    return m->v;
}
double x_cos(double x) {
    unsigned long long k = bits(x);
    Memo *m = &g_mcos[memo_slot(k)];
    if (m->ok && m->key == k) return m->v;
    m->v = x_cos_raw(x);
    m->key = k;
    m->ok = 1;
    return m->v;
}

static double x_sin_raw(double x) {
    if (!g_init) init_tables();
    if (x == 0.0 || !isfinite(x)) return x == 0.0 ? x : sin(x);
    if (fabs(x) < 1.4901161193847656e-08) return x;    /* sin x = x correctly rounded below 2^-26 */
    int q;
    dd r = reduce(x, &q);
    dd v;
    switch (q) {
    case 0: v = dd_sin_core(r); break;
    case 1: v = dd_cos_core(r); break;
    case 2: v = dd_neg(dd_sin_core(r)); break;
    default: v = dd_neg(dd_cos_core(r)); break;
    }
    return v.hi + v.lo;
}

static double x_cos_raw(double x) {
    if (!g_init) init_tables();
    if (!isfinite(x)) return cos(x);
    if (fabs(x) < 7.450580596923828e-09) return 1.0;
    int q;
    dd r = reduce(x, &q);
    dd v;
    switch (q) {
    case 0: v = dd_cos_core(r); break;
    case 1: v = dd_neg(dd_sin_core(r)); break;
    case 2: v = dd_neg(dd_cos_core(r)); break;
    default: v = dd_sin_core(r); break;
    }
    return v.hi + v.lo;
}

/* log(x) = e*ln2 + 2*atanh((m-1)/(m+1)),  m in [sqrt(1/2), sqrt(2)) */
static const dd LN2 = {0.6931471805599453, 2.3190468138462996e-17};
double x_log(double x) {
    if (!(x > 0.0) || !isfinite(x)) return log(x);
    if (x == 1.0) return 0.0;
    int e;
    double m = frexp(x, &e);            /* x = m * 2^e, m in [0.5, 1) */
    if (m < 0.70710678118654752440) { m *= 2.0; e--; }
    dd num = dd_add(dd_from(m), dd_from(-1.0)), den = dd_add(dd_from(m), dd_from(1.0));
    dd s = dd_div(num, den), s2 = dd_mul(s, s), term = s, sum = s;
    for (int k = 3; k < 80; k += 2) {
        term = dd_mul(term, s2);
        dd t = dd_div(term, dd_from((double)k));
        sum = dd_add(sum, t);
        if (fabs(t.hi) < 1e-36 * (fabs(sum.hi) + 1e-300)) break;
    }
    dd v = dd_add(dd_mul_d(LN2, (double)e), dd_mul_d(sum, 2.0));
    return v.hi + v.lo;
}

/* hypot via an exact sum of squares and a double-double square root */
double x_hypot(double x, double y) {
    x = fabs(x);
    y = fabs(y);
    if (!isfinite(x) || !isfinite(y)) return hypot(x, y);
    if (x < y) { double t = x; x = y; y = t; }
    if (y == 0.0) return x;
    /* scale into a safe range (the game's values are small; this only guards extremes) */
    int ex;
    frexp(x, &ex);
    double sc = ldexp(1.0, -ex), inv = ldexp(1.0, ex);
    double xs = x * sc, ys = y * sc;
    dd a = dd_add(two_prod(xs, xs), two_prod(ys, ys));
    double s = sqrt(a.hi);
    /* one Newton step in double-double: s + (a - s^2) / (2s) */
    dd s2 = two_prod(s, s);
    dd diff = dd_add(a, dd_neg(s2));
    double corr = diff.hi / (2.0 * s);
    dd v = quick_two_sum(s, corr);
    return (v.hi + v.lo) * inv;
}

/* t**3 (Python float ** 3 -> pow(t, 3.0)): exact product rounded once */
double x_cube(double t) {
    dd v = dd_mul_d(two_prod(t, t), t);
    return v.hi + v.lo;
}
