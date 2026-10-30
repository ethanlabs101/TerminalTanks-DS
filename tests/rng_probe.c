#include <stdio.h>
#include <stdlib.h>
#include "../core/rng.h"
int main(int argc, char **argv) {
    unsigned long long seed = strtoull(argv[1], 0, 10);
    PyRandom r; rng_seed(&r, seed);
    for (int i = 0; i < 5; i++) printf("%.17g\n", rng_random(&r));
    for (int i = 0; i < 5; i++) printf("%u\n", rng_below(&r, 1u << 30));
    for (int i = 0; i < 5; i++) printf("%.17g\n", rng_uniform(&r, -0.45, 0.45));
    for (int i = 0; i < 5; i++) printf("%.17g\n", rng_gauss(&r, 0, 5.5));
    for (int i = 0; i < 5; i++) printf("%u\n", rng_below(&r, 50));
    double vals[] = {0.25, 0.35, 2.45, -3.85, 5.55, 1.05, 9.95, -0.05, 4.4499999, 7.15};
    for (int i = 0; i < 10; i++) printf("%.17g\n", py_round1(vals[i] * 1.0));
    return 0;
}
