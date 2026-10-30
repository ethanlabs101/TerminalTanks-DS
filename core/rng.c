#include <math.h>
#include "rng.h"
#include "xmath.h"

#define N 624
#define M 397

static void init_genrand(PyRandom *r, uint32_t s) {
    r->mt[0] = s;
    for (int i = 1; i < N; i++)
        r->mt[i] = 1812433253u * (r->mt[i - 1] ^ (r->mt[i - 1] >> 30)) + (uint32_t)i;
    r->mti = N;
}

static void init_by_array(PyRandom *r, const uint32_t *key, int len) {
    init_genrand(r, 19650218u);
    int i = 1, j = 0;
    int k = N > len ? N : len;
    for (; k; k--) {
        r->mt[i] = (r->mt[i] ^ ((r->mt[i - 1] ^ (r->mt[i - 1] >> 30)) * 1664525u)) + key[j] + (uint32_t)j;
        i++; j++;
        if (i >= N) { r->mt[0] = r->mt[N - 1]; i = 1; }
        if (j >= len) j = 0;
    }
    for (k = N - 1; k; k--) {
        r->mt[i] = (r->mt[i] ^ ((r->mt[i - 1] ^ (r->mt[i - 1] >> 30)) * 1566083941u)) - (uint32_t)i;
        i++;
        if (i >= N) { r->mt[0] = r->mt[N - 1]; i = 1; }
    }
    r->mt[0] = 0x80000000u;
}

void rng_seed(PyRandom *r, uint64_t seed) {
    uint32_t key[2];
    int len = 0;
    if (seed == 0) key[len++] = 0;
    while (seed) { key[len++] = (uint32_t)(seed & 0xffffffffu); seed >>= 32; }
    init_by_array(r, key, len);
    r->has_gauss = 0;
    r->gauss_next = 0.0;
}

uint32_t rng_u32(PyRandom *r) {
    static const uint32_t mag01[2] = {0x0u, 0x9908b0dfu};
    uint32_t y;
    if (r->mti >= N) {
        int kk;
        for (kk = 0; kk < N - M; kk++) {
            y = (r->mt[kk] & 0x80000000u) | (r->mt[kk + 1] & 0x7fffffffu);
            r->mt[kk] = r->mt[kk + M] ^ (y >> 1) ^ mag01[y & 1u];
        }
        for (; kk < N - 1; kk++) {
            y = (r->mt[kk] & 0x80000000u) | (r->mt[kk + 1] & 0x7fffffffu);
            r->mt[kk] = r->mt[kk + (M - N)] ^ (y >> 1) ^ mag01[y & 1u];
        }
        y = (r->mt[N - 1] & 0x80000000u) | (r->mt[0] & 0x7fffffffu);
        r->mt[N - 1] = r->mt[M - 1] ^ (y >> 1) ^ mag01[y & 1u];
        r->mti = 0;
    }
    y = r->mt[r->mti++];
    y ^= (y >> 11);
    y ^= (y << 7) & 0x9d2c5680u;
    y ^= (y << 15) & 0xefc60000u;
    y ^= (y >> 18);
    return y;
}

double rng_random(PyRandom *r) {
    uint32_t a = rng_u32(r) >> 5, b = rng_u32(r) >> 6;
    return (a * 67108864.0 + b) * (1.0 / 9007199254740992.0);
}

double rng_uniform(PyRandom *r, double a, double b) { return a + (b - a) * rng_random(r); }

uint32_t rng_below(PyRandom *r, uint32_t n) {
    int k = 0;
    for (uint32_t v = n; v; v >>= 1) k++;          /* n.bit_length() */
    uint32_t x = rng_u32(r) >> (32 - k);           /* getrandbits(k), k <= 32 */
    while (x >= n) x = rng_u32(r) >> (32 - k);
    return x;
}

int rng_randint(PyRandom *r, int a, int b) { return a + (int)rng_below(r, (uint32_t)(b - a + 1)); }

double rng_gauss(PyRandom *r, double mu, double sigma) {
    double z;
    if (r->has_gauss) {
        z = r->gauss_next;
        r->has_gauss = 0;
    } else {
        double x2pi = rng_random(r) * (2.0 * 3.141592653589793);
        double g2rad = sqrt(-2.0 * py_log(1.0 - rng_random(r)));
        z = py_cos(x2pi) * g2rad;
        r->gauss_next = py_sin(x2pi) * g2rad;
        r->has_gauss = 1;
    }
    return mu + z * sigma;
}

double py_round_int(double v) { return rint(v); }   /* default rounding mode: half-even */

double py_round1(double v) {
    if (!isfinite(v)) return v;
    double p = v * 10.0;
    double e = fma(v, 10.0, -p);                    /* exact error of the product: v*10 == p + e */
    double k = rint(p);
    double d = p - k;                               /* exact; |d| <= 0.5 */
    /* true residual r = d + e. Decide r against +-0.5 exactly. */
    int cmp_hi = (d > 0.5) ? 1 : (d < 0.5) ? -1 : (e > 0) - (e < 0);     /* sign(r - 0.5) */
    int cmp_lo = (d < -0.5) ? -1 : (d > -0.5) ? 1 : (e > 0) - (e < 0);    /* sign(r + 0.5) */
    if (cmp_hi > 0) k += 1.0;
    else if (cmp_lo < 0) k -= 1.0;
    else if (cmp_hi == 0) k = fmod(k, 2.0) == 0.0 ? k : k + 1.0;        /* tie between k and k+1 */
    else if (cmp_lo == 0) k = fmod(k, 2.0) == 0.0 ? k : k - 1.0;        /* tie between k-1 and k */
    return k / 10.0;
}
