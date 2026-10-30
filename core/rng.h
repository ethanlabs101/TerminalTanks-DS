/* Bit-exact port of CPython's random.Random (MT19937) for the calls the game uses. */
#ifndef TT_RNG_H
#define TT_RNG_H
#include <stdint.h>

typedef struct {
    uint32_t mt[624];
    int mti;
    int has_gauss;
    double gauss_next;
} PyRandom;

void   rng_seed(PyRandom *r, uint64_t seed);          /* random.Random(int seed), seed >= 0 */
uint32_t rng_u32(PyRandom *r);                         /* genrand_uint32 */
double rng_random(PyRandom *r);                        /* random() */
double rng_uniform(PyRandom *r, double a, double b);   /* uniform(a, b) */
uint32_t rng_below(PyRandom *r, uint32_t n);           /* _randbelow(n), n >= 1 */
int    rng_randint(PyRandom *r, int a, int b);         /* randint(a, b) */
double rng_gauss(PyRandom *r, double mu, double sigma);/* gauss(mu, sigma) */

/* Python float helpers that must match exactly */
double py_round1(double v);    /* round(v, 1): correctly rounded, half-even on the exact binary value */
double py_round_int(double v); /* round(v): half-even */
static inline double py_clamp(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
#define PY_RADIANS(a) ((a) * (3.141592653589793 / 180.0))

#endif
