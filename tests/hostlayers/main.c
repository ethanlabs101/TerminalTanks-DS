/* Runs the 2.0 home-screen layers (source/home_layers2.inc) on the PC with a stubbed screen: checks every layer
 * terminates and counts pixel blends per frame (a cost proxy for the ARM9). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../../core/rng.h"
#include "../../core/content.h"
#include "../../core/world.h"
#define PI_F 3.14159265f
#define TAU_F 6.2831853f
static float fsin(float x) { return sinf(x); }
static float fcos(float x) { return cosf(x); }
static float fhypot(float a, float b) { return hypotf(a, b); }
RGB cmix(RGB a, RGB b, float t) { RGB r = {(uint8_t)(a.r + (b.r - a.r) * t), (uint8_t)(a.g + (b.g - a.g) * t), (uint8_t)(a.b + (b.b - a.b) * t)}; return r; }
RGB cshade(RGB c, float k) { RGB r = {(uint8_t)fminf(255, c.r * k), (uint8_t)fminf(255, c.g * k), (uint8_t)fminf(255, c.b * k)}; return r; }
RGB chsv(float h, float s, float v) { (void)h; (void)s; RGB r = {(uint8_t)(v * 255), 0, 0}; return r; }
typedef struct { int cols, rows; float time; const void *th; } LCtx;
static long g_puts;
static RGB L_bg(LCtx *c, int x, int y) { (void)c; (void)x; RGB r = {10, 10, (uint8_t)(20 + y)}; return r; }
static void L_put(LCtx *c, int x, int y, uint32_t ch, RGB fg, const RGB *bg) { (void)c; (void)x; (void)y; (void)ch; (void)fg; (void)bg; g_puts++; }
static long g_blends;

#include "../../source/home_layers2.inc"

typedef void (*Fn)(LCtx *);
int main(void) {
    const char *names[10] = {"abyss", "shrine", "holocity", "hyper", "kaiju", "shrooms", "wormsea", "jadetemple", "orrery", "armada"};
    Fn fns[10] = {l_abyss, l_shrine, l_holocity, l_hyper, l_kaiju, l_shrooms, l_wormsea, l_jadetemple, l_orrery, l_armada};
    for (int i = 0; i < 10; i++) {
        LCtx c = {51, 24, 0, fns[i]};
        clock_t t0 = clock();
        for (int f = 0; f < 300; f++) { c.time = f / 30.0f; fns[i](&c); }
        printf("%-11s %6.2f ms/frame on this PC, %ld blends/frame\n", names[i], (clock() - t0) * 1000.0 / CLOCKS_PER_SEC / 300, g_blends / 300); g_blends = 0;
        fflush(stdout);
    }
    return 0;
}
