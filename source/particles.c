#include <math.h>
#include <string.h>
#include "particles.h"

#define PI_F 3.14159265f
#define TAU_F 6.2831853f

void parts_init(Particles *ps, uint32_t seed, const Settings *st) {
    ps->n = 0;
    ps->wind = 0;
    ps->st = st;
    ps->smoke_timer = 0;
    rng_seed(&ps->rng, seed ^ 0x5EEDu);
}

static float U(Particles *ps, float a, float b) { return (float)rng_uniform(&ps->rng, a, b); }
static float R(Particles *ps) { return (float)rng_random(&ps->rng); }

void parts_emit(Particles *ps, float x, float y, float vx, float vy, float life, float max_life, RGB c0, RGB c1, float gravity,
                 float drag, float alpha, bool solid, int size, float windk) {
    if (ps->n >= PART_CAP) return;
    Particle *p = &ps->p[ps->n++];
    p->x = x; p->y = y; p->vx = vx; p->vy = vy; p->life = life; p->max_life = max_life; p->c0 = c0; p->c1 = c1;
    p->gravity = gravity; p->drag = drag; p->alpha = alpha; p->solid = solid; p->size = (uint8_t)size; p->windk = windk;
}

static int nn(Particles *ps, float base) { int v = (int)(base * ps->st->particle_density); return v > 1 ? v : 1; }

void parts_update(Particles *ps, float dt, const Terrain *terrain) {
    int out = 0;
    for (int i = 0; i < ps->n; i++) {
        Particle p = ps->p[i];
        p.life -= dt;
        if (p.life <= 0) continue;
        if (p.drag != 0) {
            float k = 1.0f - p.drag * dt;
            if (k < 0) k = 0;
            p.vx *= k;
            p.vy *= k;
        }
        p.vy -= p.gravity * dt;
        if (p.windk != 0) p.vx += ps->wind * p.windk * dt;
        p.x += p.vx * dt;
        p.y += p.vy * dt;
        if (p.solid && terrain) {
            int xi = (int)p.x;
            if (p.y < 0 || (xi >= 0 && xi < terrain->width && p.y < terrain->heights[xi])) continue;
        }
        ps->p[out++] = p;
    }
    ps->n = out;
}


void parts_draw(const Particles *ps, Surf *cv) {
    for (int i = 0; i < ps->n; i++) {
        const Particle *p = &ps->p[i];
        float t = 1 - p->life / p->max_life;
        RGB col = cmix(p->c0, p->c1, t);
        float a = p->alpha * (1 - t);
        int x = (int)floorf(p->x), y = (int)floorf(p->y);
        cv_blend(cv, x, y, col, a);
        if (p->size > 1) {
            cv_blend(cv, x + 1, y, col, a * 0.5f);
            cv_blend(cv, x - 1, y, col, a * 0.5f);
            cv_blend(cv, x, y + 1, col, a * 0.5f);
            cv_blend(cv, x, y - 1, col, a * 0.5f);
        }
    }
}

static const RGB WHITE = {255, 255, 255};

static void burst(Particles *ps, float x, float y, int n, float s0, float s1, float l0, float l1, RGB c0, RGB c1, float gravity, float drag,
                  int size, float alpha, float sp0, float sp1, bool solid, float windk, bool rand_hsv) {
    for (int i = 0; i < n; i++) {
        float a = U(ps, sp0, sp1), s = U(ps, s0, s1), life = U(ps, l0, l1);
        RGB ca = c0, cb = c1;
        if (rand_hsv) { ca = chsv(R(ps), 0.6f, 1.0f); cb = (RGB){40, 30, 70}; }
        parts_emit(ps, x, y, fcos(a) * s, fsin(a) * s, life, life, ca, cb, gravity, drag, alpha, solid, size, windk);
    }
}
#define BURST(...) burst(__VA_ARGS__, 0.1f, PI_F - 0.1f, false, 0, false)

void fx_trail(Particles *ps, float x, float y, RGB accent, int style, int n, float t) {
    float life = (float)ps->st->trail_life;
    if (life <= 0) return;
    float l = life * U(ps, 0.7f, 1.0f);
    RGB plain_c0 = cmix(WHITE, accent, 0.5f), plain_c1 = cshade(accent, 0.15f);
#define PLAIN() parts_emit(ps, x, y, 0, 0, l, l, plain_c0, plain_c1, 0, 0, 0.9f, false, 1, 0)
    switch (style) {
    case TRAIL_PLAIN: PLAIN(); break;
    /* 2.0 trails (effects.py trail) */
    case TRAIL_BIOLUME:                                   /* slow glowing droplets that sink and fade like plankton */
        parts_emit(ps, x, y, 0, 0, l * 1.2f, l * 1.2f, (RGB){150, 255, 236}, (RGB){10, 50, 90}, 0, 0, 0.85f, false, 1, 0);
        if (R(ps) < 0.45f) { float a = U(ps, -1, 1), b = U(ps, -2, 2), c = U(ps, -4, -1);
            parts_emit(ps, x + a, y, b, c, l * 1.8f, l * 1.8f, (RGB){90, 220, 230}, (RGB){10, 40, 70}, 0, 0, 0.7f, false, R(ps) < 0.2f ? 2 : 1, 0); }
        break;
    case TRAIL_FOXFIRE: { float a = U(ps, -0.8f, 0.8f), b = U(ps, -2, 2), c = U(ps, 3, 10);
        parts_emit(ps, x + a, y, b, c, l * 0.9f, l * 0.9f, (RGB){180, 220, 255}, (RGB){40, 50, 200}, -14, 0, 0.95f, false, R(ps) < 0.3f ? 2 : 1, 0); break; }
    case TRAIL_HARDLIGHT:                                 /* a hard, bright line that lingers, edged in cyan */
        parts_emit(ps, x, y, 0, 0, l * 1.5f, l * 1.5f, WHITE, (RGB){30, 120, 255}, 0, 0, 0.95f, false, 1, 0);
        parts_emit(ps, x, y + 1, 0, 0, l * 0.9f, l * 0.9f, (RGB){90, 230, 255}, (RGB){10, 40, 120}, 0, 0, 0.5f, false, 1, 0);
        break;
    case TRAIL_PARADOX: {                                 /* afterimages that alternate between now and then */
        RGB c = n % 2 ? WHITE : (RGB){190, 140, 255};
        parts_emit(ps, x, y, 0, 0, l, l, c, (RGB){40, 20, 90}, 0, 0, 0.85f, false, 1, 0);
        if (n % 5 == 0) parts_emit(ps, x, y, 0, 0, l * 1.6f, l * 1.6f, (RGB){150, 110, 255}, (RGB){20, 10, 50}, 0, 0, 0.4f, false, 2, 0);
        break;
    }
    case TRAIL_ATOMIC:
        parts_emit(ps, x, y, 0, 0, l, l, (RGB){210, 240, 255}, (RGB){30, 60, 160}, 0, 0, 0.9f, false, n % 3 == 0 ? 2 : 1, 0);
        if (R(ps) < 0.5f) { float a = U(ps, 0, TAU_F);
            parts_emit(ps, x, y, fcos(a) * 12, fsin(a) * 12, 0.25f, 0.25f, WHITE, (RGB){80, 140, 255}, 0, 2.0f, 1, false, 1, 0); }
        break;
    case TRAIL_SPORES: { float a = U(ps, -1, 1), b = U(ps, -1, 1), c = U(ps, -3, 3), d = U(ps, -1, 4);
        parts_emit(ps, x + a, y + b, c, d, l * 1.6f, l * 1.6f, (RGB){180, 255, 214}, (RGB){40, 110, 90}, 0, 0, 0.75f, false, 1, 0.6f); break; }
    case TRAIL_SAND: { float a = U(ps, -3, 3), b = U(ps, -2, 3);
        parts_emit(ps, x, y, a, b, l, l, (RGB){230, 196, 130}, (RGB){120, 84, 46}, 50, 0, 0.9f, false, 1, 0);
        if (R(ps) < 0.3f) { float c = U(ps, -4, 4), d = U(ps, 2, 6);
            parts_emit(ps, x, y, c, d, l * 1.4f, l * 1.4f, (RGB){200, 170, 120}, (RGB){90, 70, 50}, 0, 0, 0.4f, false, 2, 0.8f); }
        break; }
    case TRAIL_FEATHERS: { RGB c = n % 3 ? (RGB){60, 220, 150} : (RGB){230, 70, 60};
        float a = U(ps, -1, 1), b = U(ps, -3, 3), d = U(ps, -4, 1);
        parts_emit(ps, x + a, y, b, d, l * 1.4f, l * 1.4f, c, cshade(c, 0.3f), 0, 0, 0.9f, false, 1, 0.7f); break; }
    case TRAIL_CONSTELLATION:                             /* a dotted line of stars: every fourth step a bright one */
        if (n % 4 == 0) parts_emit(ps, x, y, 0, 0, l * 2.0f, l * 2.0f, (RGB){255, 250, 220}, (RGB){90, 110, 230}, 0, 0, 0.95f, false, 2, 0);
        else if (n % 2 == 0) parts_emit(ps, x, y, 0, 0, l * 1.3f, l * 1.3f, (RGB){220, 190, 120}, (RGB){40, 40, 110}, 0, 0, 0.6f, false, 1, 0);
        break;
    case TRAIL_GHOSTFIRE: { float a = U(ps, -0.6f, 0.6f), b = U(ps, -2, 2), c = U(ps, 3, 10);
        parts_emit(ps, x + a, y, b, c, l * 0.9f, l * 0.9f, (RGB){200, 255, 220}, (RGB){20, 80, 50}, -12, 0, 0.9f, false, R(ps) < 0.35f ? 2 : 1, 0);
        if (R(ps) < 0.3f) { float d = U(ps, -3, 3), e = U(ps, -1, 3);
            parts_emit(ps, x, y, d, e, l * 2, l * 2, (RGB){120, 150, 140}, (RGB){20, 30, 30}, 0, 0, 0.35f, false, 2, 0.8f); }
        break; }
    case TRAIL_DASH: if (n % 4 < 2) parts_emit(ps, x, y, 0, 0, l * 0.8f, l * 0.8f, WHITE, cshade(accent, 0.3f), 0, 0, 1, false, 1, 0); break;
    case TRAIL_GLOW: parts_emit(ps, x, y, 0, 0, l * 1.1f, l * 1.1f, cmix(accent, WHITE, 0.35f), cshade(accent, 0.1f), 0, 0, 0.55f, false, 2, 0); break;
    case TRAIL_SPARKS:
        PLAIN();
        if (R(ps) < 0.5f) parts_emit(ps, x, y, U(ps, -12, 12), U(ps, -6, 14), 0.5f, 0.5f, WHITE, cshade(accent, 0.3f), 60, 0, 1, false, 1, 0);
        break;
    case TRAIL_EMBERS:
        parts_emit(ps, x, y, 0, 0, l * 1.4f, l * 1.4f, cmix((RGB){255, 200, 120}, accent, 0.4f), (RGB){120, 30, 10}, 0, 0, 0.9f, false, 1, 0);
        if (R(ps) < 0.4f) parts_emit(ps, x, y, U(ps, -4, 4), U(ps, 3, 9), l, l, (RGB){255, 190, 100}, (RGB){90, 20, 8}, -6, 0, 1, false, 1, 0);
        break;
    case TRAIL_DRIP:
        parts_emit(ps, x, y, 0, 0, l, l, cmix(accent, WHITE, 0.15f), cshade(accent, 0.2f), 0, 0, 0.9f, false, 1, 0);
        if (R(ps) < 0.35f) parts_emit(ps, x, y, U(ps, -2, 2), -8, 0.7f, 0.7f, accent, cshade(accent, 0.2f), 40, 0, 1, false, 1, 0);
        break;
    case TRAIL_TWINKLE: {
        RGB c = R(ps) < 0.5f ? WHITE : accent;
        float ox = U(ps, -0.7f, 0.7f), oy = U(ps, -0.7f, 0.7f);
        parts_emit(ps, x + ox, y + oy, 0, 0, l * 0.9f, l * 0.9f, c, cshade(accent, 0.2f), 0, 0, 1, false, 1, 0);
        break;
    }
    case TRAIL_RUNES: {
        float ox = U(ps, -1.2f, 1.2f), oy = U(ps, -1.2f, 1.2f), vy = U(ps, 0, 3);
        RGB c = R(ps) < 0.5f ? (RGB){255, 240, 170} : accent;
        parts_emit(ps, x + ox, y + oy, 0, vy, l, l, c, (RGB){150, 100, 30}, 0, 0, 0.9f, false, 1, 0);
        if (R(ps) < 0.25f) parts_emit(ps, x, y, 0, 0, l * 0.5f, l * 0.5f, WHITE, accent, 0, 0, 0.7f, false, 2, 0);
        break;
    }
    case TRAIL_AEON:
        for (int sgn = -1; sgn <= 1; sgn += 2)
            parts_emit(ps, x + sgn * fsin(t * 14) * 1.6f, y, 0, 0, l, l, sgn > 0 ? (RGB){255, 250, 225} : (RGB){170, 110, 255}, (RGB){30, 10, 60}, 0, 0, 0.8f, false, 1, 0);
        break;
    case TRAIL_TIDES: {
        float vx = U(ps, -4, 4), vy = U(ps, 2, 8);
        parts_emit(ps, x, y, vx, vy, l, l, cmix((RGB){190, 240, 255}, accent, 0.3f), (RGB){10, 60, 110}, 40, 0, 0.9f, false, 1, 0);
        if (R(ps) < 0.3f) parts_emit(ps, x, y, 0, 0, l * 0.8f, l * 0.8f, (RGB){230, 250, 255}, accent, 0, 0, 0.5f, false, 2, 0);
        break;
    }
    case TRAIL_CINDERS: {
        float ox = U(ps, -0.8f, 0.8f), vx = U(ps, -4, 4), vy = U(ps, 2, 12);
        int size = R(ps) < 0.3f ? 2 : 1;
        parts_emit(ps, x + ox, y, vx, vy, l * 0.9f, l * 0.9f, (RGB){255, 200, 100}, (RGB){60, 20, 10}, -12, 0, 0.9f, false, size, 0);
        if (R(ps) < 0.4f) { float a = U(ps, -5, 5), b2 = U(ps, -5, 5); parts_emit(ps, x, y, a, b2, 0.35f, 0.35f, (RGB){44, 22, 22}, (RGB){10, 5, 5}, 0, 0, 0.8f, false, 1, 0); }
        break;
    }
    case TRAIL_CHRONO:
        if (n % 3 == 0) parts_emit(ps, x, y, 0, 0, l * 1.4f, l * 1.4f, (RGB){255, 236, 170}, (RGB){120, 80, 30}, 0, 0, 0.55f, false, 2, 0);
        parts_emit(ps, x, y, 0, 0, l * 0.6f, l * 0.6f, WHITE, accent, 0, 0, 0.9f, false, 1, 0);
        break;
    case TRAIL_MIST: {
        float vx = U(ps, -3, 3), vy = U(ps, -3, 3);
        parts_emit(ps, x, y, vx, vy, l * 1.2f, l * 1.2f, cshade(accent, 0.25f), (RGB){8, 4, 16}, 0, 0, 0.75f, false, 2, 0);
        if (R(ps) < 0.3f) parts_emit(ps, x, y, 0, 0, l * 0.6f, l * 0.6f, cmix(accent, WHITE, 0.3f), cshade(accent, 0.2f), 0, 0, 1, false, 1, 0);
        break;
    }
    case TRAIL_FIRE: {
        float ox = U(ps, -0.6f, 0.6f), vx = U(ps, -3, 3), vy = U(ps, 4, 14);
        int size = R(ps) < 0.4f ? 2 : 1;
        parts_emit(ps, x + ox, y, vx, vy, l * 0.8f, l * 0.8f, cmix((RGB){255, 236, 150}, accent, 0.3f), (RGB){120, 26, 8}, -20, 0, 0.9f, false, size, 0);
        break;
    }
    case TRAIL_RIPPLE:
        for (int sgn = -1; sgn <= 1; sgn += 2)
            parts_emit(ps, x + sgn * 0.8f, y, sgn * 3.5f, 0, l, l, cmix(accent, WHITE, 0.4f), cshade(accent, 0.15f), 0, 1.2f, 0.6f, false, 1, 0);
        break;
    case TRAIL_FROST: {
        float ox = U(ps, -1, 1), oy = U(ps, -1, 1), vx = U(ps, -2, 2);
        parts_emit(ps, x + ox, y + oy, vx, -4, l * 1.1f, l * 1.1f, cmix((RGB){235, 250, 255}, accent, 0.3f), cshade(accent, 0.2f), 10, 0, 0.9f, false, 1, 0);
        if (R(ps) < 0.35f) parts_emit(ps, x, y, 0, 0, l * 0.7f, l * 0.7f, WHITE, accent, 0, 0, 0.6f, false, 2, 0);
        break;
    }
    case TRAIL_SOLAR: {
        parts_emit(ps, x, y, 0, 0, l * 0.9f, l * 0.9f, (RGB){255, 250, 210}, (RGB){255, 130, 30}, 0, 0, 0.95f, false, 2, 0);
        float a = U(ps, 0, TAU_F);
        parts_emit(ps, x, y, fcos(a) * 9, fsin(a) * 9, 0.4f, 0.4f, WHITE, (RGB){255, 150, 40}, 0, 2.0f, 1, false, 1, 0);
        break;
    }
    case TRAIL_LAVA: {
        float vx = U(ps, -3, 3);
        int size = R(ps) < 0.3f ? 2 : 1;
        parts_emit(ps, x, y, vx, 0, l * 0.9f, l * 0.9f, (RGB){255, 170, 60}, (RGB){70, 16, 8}, 70, 0, 0.95f, false, size, 0);
        break;
    }
    case TRAIL_ARC:
        for (int k = 0; k < 2; k++) {
            float ox = U(ps, -2.6f, 2.6f), oy = U(ps, -2.6f, 2.6f), lf = U(ps, 0.1f, 0.22f);
            parts_emit(ps, x + ox, y + oy, 0, 0, lf, 0.22f, (RGB){235, 245, 255}, cshade(accent, 0.3f), 0, 0, 1, false, 1, 0);
        }
        PLAIN();
        break;
    case TRAIL_BLESSING:
        for (int k = 0; k < 2; k++) {
            float a = U(ps, 0, TAU_F);
            parts_emit(ps, x, y, fcos(a) * 10, fsin(a) * 10, l * 1.4f, l * 1.4f, (RGB){255, 220, 110}, (RGB){120, 60, 200}, 0, 1.6f, 0.95f, false, 1, 0);
        }
        parts_emit(ps, x, y, 0, 0, l * 1.1f, l * 1.1f, WHITE, (RGB){255, 200, 80}, 0, 0, 0.8f, false, 2, 0);
        break;
    case TRAIL_XENON:
        for (int k = 0; k < 2; k++) {
            float a = U(ps, 0, TAU_F);
            parts_emit(ps, x, y, fcos(a) * 9, fsin(a) * 9, l * 1.3f, l * 1.3f, (RGB){160, 255, 215}, (RGB){110, 50, 210}, 0, 1.8f, 0.9f, false, 1, 0);
        }
        parts_emit(ps, x, y, 0, 0, l, l, WHITE, (RGB){90, 60, 200}, 0, 0, 0.7f, false, 2, 0);
        if (R(ps) < 0.25f) { float a = U(ps, -14, 14), b2 = U(ps, -14, 14); parts_emit(ps, x, y, a, b2, l * 0.7f, l * 0.7f, (RGB){230, 170, 255}, (RGB){60, 20, 120}, 0, 2.0f, 0.9f, false, 1, 0); }
        break;
    case TRAIL_ALIEN: {
        float a = U(ps, 0, TAU_F);
        parts_emit(ps, x, y, fcos(a) * 6, fsin(a) * 6 - 2, l * 1.2f, l * 1.2f, (RGB){150, 255, 190}, (RGB){120, 60, 200}, 0, 1.5f, 0.85f, false, 1, 0);
        if (R(ps) < 0.3f) parts_emit(ps, x, y, 0, 0, l * 0.8f, l * 0.8f, (RGB){210, 150, 255}, (RGB){60, 20, 120}, 0, 0, 0.6f, false, 2, 0);
        break;
    }
    case TRAIL_CODE:
        parts_emit(ps, x, y, 0, -10, l * 1.1f, l * 1.1f, (RGB){150, 255, 175}, (RGB){8, 56, 22}, 0, 0, 0.9f, false, 1, 0);
        if (R(ps) < 0.4f) { float ox = U(ps, -1, 1); parts_emit(ps, x + ox, y + 1, 0, -14, l, l, (RGB){80, 220, 110}, (RGB){6, 40, 16}, 0, 0, 0.7f, false, 1, 0); }
        break;
    case TRAIL_PRISM: {
        RGB c = chsv(t * 0.8f + n * 0.05f, 0.65f, 1.0f);
        parts_emit(ps, x, y, 0, 0, l * 1.1f, l * 1.1f, c, cshade(c, 0.15f), 0, 0, 0.95f, false, 1, 0);
        if (R(ps) < 0.4f) { float ox = U(ps, -1.5f, 1.5f), oy = U(ps, -1.5f, 1.5f); parts_emit(ps, x + ox, y + oy, 0, 0, l * 0.6f, l * 0.6f, WHITE, c, 0, 0, 1, false, 1, 0); }
        break;
    }
    default: PLAIN();
    }
#undef PLAIN
}

void fx_muzzle(Particles *ps, float x, float y, float angle, RGB accent) {
    for (int i = 0, n = nn(ps, 12); i < n; i++) {
        float a = (angle + U(ps, -18, 18)) * (PI_F / 180.0f);
        float s = U(ps, 20, 55), life = U(ps, 0.15f, 0.4f);
        parts_emit(ps, x, y, fcos(a) * s, fsin(a) * s, life, 0.4f, (RGB){255, 250, 220}, cshade(accent, 0.3f), 0, 3.0f, 1, false, 1, 0);
    }
}

void fx_smoke(Particles *ps, float x, float y, int n, float spread, const RGB *tint) {
    RGB c0 = tint ? cmix((RGB){118, 118, 126}, *tint, 0.6f) : (RGB){118, 118, 126};
    for (int i = 0; i < n; i++) {
        float life = U(ps, 1.2f, 2.4f);
        float ox = U(ps, -spread, spread), vx = U(ps, -4, 4), vy = U(ps, 5, 14);
        parts_emit(ps, x + ox, y, vx, vy, life, life, c0, (RGB){34, 36, 44}, -4, 0.8f, 0.55f, false, 2, 0.8f);
    }
}

void fx_impact(Particles *ps, float x, float y, RGB accent, RGB soil, float scale, int style) {
    RGB spark = cmix(WHITE, accent, 0.4f);
    BURST(ps, x, y, nn(ps, 46 * scale), 14 * scale, 55 * scale, 0.5f, 1.1f, spark, cshade(accent, 0.2f), 70, 0.4f, 1, 1);
    for (int i = 0, n = nn(ps, 22 * scale); i < n; i++) {
        float a = U(ps, 0.3f, PI_F - 0.3f), s = U(ps, 10, 38) * scale, life = U(ps, 0.8f, 1.5f);
        RGB c = cshade(soil, U(ps, 0.7f, 1.2f));
        parts_emit(ps, x, y, fcos(a) * s, fsin(a) * s, life, life, c, cshade(c, 0.5f), 90, 0, 1, true, 1, 0);
    }
    bool tinted = style == BOOM_CLOUD || style == BOOM_IMPLODE;
    fx_smoke(ps, x, y + 1, nn(ps, 16 * scale), 3.0f * scale, tinted ? &accent : NULL);
    switch (style) {
    case BOOM_CLOUD:
        burst(ps, x, y + 1, nn(ps, 26 * scale), 2, 9, 2.0f, 3.5f, cmix(accent, (RGB){60, 90, 40}, 0.3f), (RGB){20, 30, 18}, -3, 0.8f, 2, 0.5f,
              0.1f, PI_F - 0.1f, false, 0.8f, false);
        break;
    case BOOM_EMBERS: BURST(ps, x, y, nn(ps, 30 * scale), 10, 40, 1.2f, 2.2f, (RGB){255, 190, 110}, (RGB){120, 30, 10}, 30, 0.4f, 1, 1); break;
    case BOOM_FIRE:
        burst(ps, x, y, nn(ps, 36 * scale), 15, 45, 0.5f, 1.1f, cmix((RGB){255, 230, 140}, accent, 0.25f), (RGB){120, 26, 8}, -10, 0.6f, 2, 1,
              1.0f, 2.1f, false, 0, false);
        break;
    case BOOM_PRISM:
    case BOOM_BLOOM: burst(ps, x, y, nn(ps, 50 * scale), 14, 60, 0.7f, 1.4f, WHITE, WHITE, 40, 0.5f, 1, 1, 0.1f, PI_F - 0.1f, false, 0, true); break;
    case BOOM_CLUSTER: BURST(ps, x, y, nn(ps, 24 * scale), 30, 80, 0.3f, 0.45f, WHITE, cshade(accent, 0.3f), 20, 0.4f, 1, 1); break;
    case BOOM_IMPLODE: BURST(ps, x, y, nn(ps, 24 * scale), 8, 30, 1.0f, 1.8f, cshade(accent, 0.3f), (RGB){10, 4, 20}, 20, 0.4f, 2, 0.8f); break;
    case BOOM_RAYS:
    case BOOM_STARRAYS: BURST(ps, x, y, nn(ps, 30 * scale), 30, 70, 0.5f, 0.9f, WHITE, cshade(accent, 0.3f), 40, 0.4f, 1, 1); break;
    case BOOM_SHOCK: BURST(ps, x, y, nn(ps, 26 * scale), 25, 60, 0.4f, 0.8f, WHITE, cshade(accent, 0.3f), 30, 1.0f, 1, 1); break;
    case BOOM_SHATTER: BURST(ps, x, y, nn(ps, 40 * scale), 20, 75, 0.6f, 1.2f, (RGB){240, 252, 255}, cshade(accent, 0.4f), 55, 0.3f, 1, 1); break;
    case BOOM_NOVA:
        burst(ps, x, y, nn(ps, 44 * scale), 20, 80, 0.6f, 1.3f, (RGB){255, 250, 220}, (RGB){255, 120, 30}, -6, 0.9f, 1, 1, 0.0f, PI_F, false, 0, false);
        break;
    case BOOM_SIGIL: BURST(ps, x, y, nn(ps, 40 * scale), 18, 64, 0.7f, 1.4f, (RGB){255, 240, 180}, (RGB){150, 90, 20}, 10, 0.7f, 2, 1); break;
    case BOOM_ECLIPSE: BURST(ps, x, y, nn(ps, 34 * scale), 10, 44, 0.8f, 1.6f, (RGB){255, 220, 120}, (RGB){40, 12, 90}, -8, 0.6f, 2, 1); break;
    case BOOM_TIDAL: BURST(ps, x, y, nn(ps, 46 * scale), 16, 58, 0.7f, 1.3f, (RGB){200, 245, 255}, (RGB){10, 50, 120}, 60, 0.4f, 1, 1); break;
    case BOOM_CINDERSTORM: BURST(ps, x, y, nn(ps, 44 * scale), 14, 56, 1.0f, 1.8f, (RGB){255, 190, 90}, (RGB){70, 16, 8}, 45, 0.4f, 2, 1); break;
    case BOOM_REWIND: BURST(ps, x, y, nn(ps, 36 * scale), 8, 36, 0.9f, 1.6f, (RGB){255, 240, 200}, (RGB){60, 30, 140}, -14, 0.8f, 2, 1); break;
    case BOOM_MAGMA:
        burst(ps, x, y, nn(ps, 36 * scale), 14, 52, 1.0f, 1.9f, (RGB){255, 190, 80}, (RGB){120, 24, 8}, 85, 0.4f, 2, 1, 0.5f, PI_F - 0.5f, true, 0, false);
        BURST(ps, x, y, nn(ps, 16 * scale), 6, 22, 1.4f, 2.4f, (RGB){255, 140, 40}, (RGB){60, 16, 8}, -8, 0.7f, 1, 1);
        break;
    case BOOM_THUNDER: BURST(ps, x, y, nn(ps, 34 * scale), 40, 95, 0.2f, 0.45f, (RGB){235, 245, 255}, cshade(accent, 0.3f), 10, 1.5f, 1, 1); break;
    case BOOM_CODE: BURST(ps, x, y, nn(ps, 34 * scale), 10, 48, 0.7f, 1.4f, (RGB){150, 255, 175}, (RGB){8, 56, 22}, 45, 0.5f, 2, 1); break;
    case BOOM_BLESSING:
        BURST(ps, x, y, nn(ps, 60 * scale), 14, 70, 0.9f, 1.9f, (RGB){255, 226, 120}, (RGB){150, 90, 230}, -6, 0.9f, 2, 1);
        BURST(ps, x, y, nn(ps, 30 * scale), 30, 90, 0.5f, 1.2f, WHITE, (RGB){255, 200, 80}, 0, 1.4f, 1, 1);
        for (int k = 0; k < 20; k++) {
            float a = k * TAU_F / 20;
            parts_emit(ps, x, y, fcos(a) * 52, fsin(a) * 52, 1.0f, 1.0f, (RGB){255, 236, 160}, (RGB){176, 120, 255}, 0, 1.5f, 0.95f, false, 1, 0);
        }
        break;
    case BOOM_XENON:
        BURST(ps, x, y, nn(ps, 46 * scale), 12, 58, 0.9f, 1.8f, (RGB){170, 255, 215}, (RGB){120, 60, 220}, -10, 0.8f, 2, 1);
        BURST(ps, x, y, nn(ps, 26 * scale), 30, 80, 0.5f, 1.1f, WHITE, (RGB){170, 110, 255}, 0, 1.4f, 1, 1);
        for (int k = 0; k < 16; k++) {
            float a = k * TAU_F / 16;
            parts_emit(ps, x, y, fcos(a) * 44, fsin(a) * 44, 0.9f, 0.9f, (RGB){190, 255, 230}, (RGB){110, 60, 220}, 0, 1.6f, 0.9f, false, 1, 0);
        }
        break;
    case BOOM_ALIEN: BURST(ps, x, y, nn(ps, 40 * scale), 12, 52, 0.8f, 1.6f, (RGB){150, 255, 200}, (RGB){110, 50, 200}, -10, 0.8f, 2, 1); break;
    /* 2.0 impacts (effects.py impact) */
    case BOOM_BIOLUME: BURST(ps, x, y, nn(ps, 40 * scale), 6, 30, 1.6f, 2.8f, (RGB){150, 255, 236}, (RGB){10, 50, 90}, -4, 0.9f, 1, 0.85f); break;
    case BOOM_FOXFIRE: BURST(ps, x, y, nn(ps, 40 * scale), 10, 40, 0.7f, 1.4f, (RGB){200, 230, 255}, (RGB){40, 50, 200}, -22, 0.6f, 2, 1); break;
    case BOOM_HARDLIGHT: BURST(ps, x, y, nn(ps, 34 * scale), 40, 110, 0.25f, 0.5f, WHITE, (RGB){40, 160, 255}, 0, 1.2f, 1, 1); break;
    case BOOM_PARADOX:
        BURST(ps, x, y, nn(ps, 36 * scale), 20, 60, 0.6f, 1.2f, WHITE, (RGB){140, 90, 240}, 0, 1.4f, 1, 1);
        BURST(ps, x, y, nn(ps, 20 * scale), 2, 10, 1.0f, 1.8f, (RGB){200, 170, 255}, (RGB){30, 10, 70}, -6, 0.4f, 2, 0.6f);
        break;
    case BOOM_ATOMIC:
        BURST(ps, x, y, nn(ps, 50 * scale), 30, 90, 0.5f, 1.0f, (RGB){230, 245, 255}, (RGB){40, 80, 200}, 10, 1.0f, 1, 1);
        BURST(ps, x, y, nn(ps, 26 * scale), 6, 22, 1.4f, 2.4f, (RGB){170, 200, 230}, (RGB){30, 40, 70}, -12, 0.6f, 2, 0.6f);
        break;
    case BOOM_SPORE: {
        int n = nn(ps, 44 * scale);
        for (int i = 0; i < n; i++) {
            float a = U(ps, 0.1f, PI_F - 0.1f), sp = U(ps, 6, 28), life = U(ps, 1.8f, 3.0f);
            RGB c = R(ps) < 0.5f ? (RGB){190, 255, 220} : (RGB){255, 190, 220};
            parts_emit(ps, x, y, fcos(a) * sp, fsin(a) * sp, life, life, c, (RGB){40, 90, 70}, -3, 0.9f, 0.85f, false, 1, 0.7f);
        }
        break;
    }
    case BOOM_ERUPTION:
        burst(ps, x, y, nn(ps, 56 * scale), 20, 70, 0.8f, 1.6f, (RGB){240, 200, 130}, (RGB){110, 80, 50}, 70, 0.4f, 2, 1, 1.2f, 1.95f, true, 0, false);
        break;
    case BOOM_JADE: {
        int n = nn(ps, 40 * scale);
        for (int i = 0; i < n; i++) {
            float a = U(ps, 0.1f, PI_F - 0.1f), sp = U(ps, 14, 50), life = U(ps, 0.7f, 1.4f), pick = R(ps);
            RGB c = pick < 1 / 3.0f ? (RGB){60, 220, 150} : pick < 2 / 3.0f ? (RGB){250, 214, 120} : (RGB){230, 70, 60};
            parts_emit(ps, x, y, fcos(a) * sp, fsin(a) * sp, life, life, c, (RGB){14, 60, 40}, 30, 0.6f, 1, false, 1, 0);
        }
        break;
    }
    case BOOM_ZODIAC: BURST(ps, x, y, nn(ps, 36 * scale), 16, 60, 0.7f, 1.5f, (RGB){255, 240, 190}, (RGB){60, 70, 200}, -4, 0.9f, 1, 1); break;
    case BOOM_GHOSTFIRE: BURST(ps, x, y, nn(ps, 44 * scale), 10, 40, 0.6f, 1.3f, (RGB){200, 255, 220}, (RGB){20, 90, 50}, -18, 0.6f, 2, 1); break;
    default: break;
    }
}

static void ambient_one(Particles *ps, int kind, float x, float y, float dt) {
    float rate = (kind == AMB_FIRE || kind == AMB_BURN) ? 15.0f : kind == AMB_SMOKE ? 7.0f : (kind == AMB_PRISM || kind == AMB_RADIATION) ? 9.0f
                 : kind == AMB_GHOSTFIRE ? 12.0f : kind == AMB_SAND ? 10.0f : 6.0f;
    if (R(ps) > dt * rate * ps->st->particle_density) return;
    if (kind == AMB_BURN) {
        ambient_one(ps, AMB_FIRE, x, y, 1e9f);
        ambient_one(ps, AMB_SMOKE, x, y, 1e9f);
        ambient_one(ps, AMB_EMBERS, x, y, 1e9f);
        return;
    }
    float a, b, c, d;
    switch (kind) {
    case AMB_SPARKS: a = U(ps, -4, 4); b = U(ps, -8, 8); c = U(ps, 10, 24); parts_emit(ps, x + a, y + 6, b, c, 0.5f, 0.5f, (RGB){255, 230, 120}, (RGB){255, 120, 20}, 40, 0, 1, false, 1, 0); break;
    case AMB_EMBERS: a = U(ps, -4, 4); b = U(ps, -3, 3); c = U(ps, 6, 14); parts_emit(ps, x + a, y + 5, b, c, 1.2f, 1.2f, (RGB){255, 150, 40}, (RGB){80, 20, 10}, -8, 0, 0.9f, false, 1, 0); break;
    case AMB_MIST: a = U(ps, -5, 5); b = U(ps, -3, 3); c = U(ps, 2, 6); parts_emit(ps, x + a, y + 2, b, c, 1.8f, 1.8f, (RGB){150, 110, 230}, (RGB){20, 10, 40}, 0, 0, 0.45f, false, 2, 0.5f); break;
    case AMB_STATIC: d = U(ps, 0, TAU_F); a = U(ps, -5, 5); b = U(ps, 2, 8); parts_emit(ps, x + a, y + b, fcos(d) * 14, fsin(d) * 14, 0.16f, 0.16f, (RGB){235, 245, 255}, (RGB){90, 150, 255}, 0, 2.0f, 1, false, 1, 0); break;
    case AMB_FROST: a = U(ps, -5, 5); b = U(ps, -3, 3); c = U(ps, -6, -2); parts_emit(ps, x + a, y + 8, b, c, 1.6f, 1.6f, (RGB){235, 250, 255}, (RGB){120, 170, 220}, 0, 0, 0.8f, false, 1, 0.6f); break;
    case AMB_FLARE: a = U(ps, -5, 5); b = U(ps, -4, 4); c = U(ps, 8, 18); parts_emit(ps, x + a, y + 6, b, c, 1.0f, 1.0f, (RGB){255, 244, 190}, (RGB){255, 130, 30}, -6, 0, 0.9f, false, 1, 0); break;
    case AMB_BITS: a = U(ps, -5, 5); c = U(ps, 6, 14); parts_emit(ps, x + a, y + 7, 0, c, 0.9f, 0.9f, (RGB){150, 255, 175}, (RGB){8, 56, 22}, 0, 0, 0.9f, false, 1, 0); break;
    case AMB_FIRE: {
        a = U(ps, -4.5f, 4.5f); b = U(ps, 3, 6); c = U(ps, -3, 3); d = U(ps, 14, 26);
        int size = R(ps) < 0.35f ? 2 : 1;
        parts_emit(ps, x + a, y + b, c, d, 0.55f, 0.55f, (RGB){255, 214, 90}, (RGB){200, 30, 8}, -24, 0, 0.95f, false, size, 0);
        break;
    }
    case AMB_SMOKE: a = U(ps, -4, 4); b = U(ps, -3, 3); c = U(ps, 5, 11); parts_emit(ps, x + a, y + 7, b, c, 2.2f, 2.2f, (RGB){88, 86, 92}, (RGB){18, 18, 22}, -4, 0, 0.55f, false, 2, 0.9f); break;
    case AMB_PRISM: {
        RGB col = chsv(R(ps), 0.5f, 1.0f);
        a = U(ps, -5, 5); b = U(ps, 1, 7); c = U(ps, -4, 4); d = U(ps, 3, 10);
        parts_emit(ps, x + a, y + b, c, d, 1.1f, 1.1f, col, cshade(col, 0.2f), -3, 0, 0.9f, false, 1, 0);
        break;
    }
    case AMB_VOID: a = U(ps, -6, 6); b = U(ps, 1, 8); c = U(ps, -3, 3); d = U(ps, -2, 4); parts_emit(ps, x + a, y + b, c, d, 1.4f, 1.4f, (RGB){150, 90, 240}, (RGB){10, 4, 24}, 0, 0, 0.7f, false, 2, 0.2f); break;
    case AMB_BUBBLES: a = U(ps, -5, 5); b = U(ps, -2, 2); c = U(ps, 5, 12); parts_emit(ps, x + a, y + 2, b, c, 1.5f, 1.5f, (RGB){190, 235, 255}, (RGB){90, 150, 220}, 0, 0, 0.7f, false, 1, 0.4f); break;
    /* 2.0 (effects.py _ambient_one) */
    case AMB_RADIATION: a = U(ps, -5, 5); b = U(ps, 5, 8); c = U(ps, -2, 2); d = U(ps, 8, 18);
        parts_emit(ps, x + a, y + b, c, d, 0.9f, 0.9f, (RGB){210, 240, 255}, (RGB){40, 80, 170}, -10, 0, 0.9f, false, 1, 0); break;
    case AMB_SPORES: a = U(ps, -6, 6); b = U(ps, 4, 9); c = U(ps, -3, 3); d = U(ps, 2, 6);
        parts_emit(ps, x + a, y + b, c, d, 2.6f, 2.6f, (RGB){200, 255, 220}, (RGB){40, 140, 110}, -1.5f, 0, 0.85f, false, 1, 0.7f); break;
    case AMB_SAND: { float side = R(ps) < 0.5f ? -5.0f : 5.0f; a = U(ps, -1, 1); b = U(ps, 1, 5); c = U(ps, -4, 4); d = U(ps, -2, 4);
        parts_emit(ps, x + side + a, y + b, c, d, 0.9f, 0.9f, (RGB){226, 190, 120}, (RGB){130, 90, 50}, 40, 0, 0.8f, false, 1, 0); break; }
    case AMB_FEATHERS: { float pick = R(ps); RGB col = pick < 1 / 3.0f ? (RGB){60, 220, 150} : pick < 2 / 3.0f ? (RGB){40, 170, 210} : (RGB){230, 60, 60};
        a = U(ps, -5, 5); b = U(ps, -6, 6); c = U(ps, -5, -1);
        parts_emit(ps, x + a, y + 9, b, c, 2.2f, 2.2f, col, cshade(col, 0.4f), 0, 0, 0.85f, false, 1, 0.9f); break; }
    case AMB_GHOSTFIRE: a = U(ps, -5, 5); b = U(ps, 2, 5); c = U(ps, -2, 2); d = U(ps, 10, 18);
        parts_emit(ps, x + a, y + b, c, d, 0.6f, 0.6f, (RGB){200, 255, 220}, (RGB){20, 90, 50}, -16, 0, 0.9f, false, R(ps) < 0.3f ? 2 : 1, 0); break;
    case AMB_PETALS: a = U(ps, -5, 5); b = U(ps, -5, 5); c = U(ps, -6, -1); parts_emit(ps, x + a, y + 8, b, c, 1.8f, 1.8f, (RGB){255, 170, 200}, (RGB){200, 80, 130}, 0, 0, 0.85f, false, 1, 0.8f); break;
    default: break;
    }
}

void fx_ambient(Particles *ps, uint64_t mask, float x, float y, RGB body, float dt) {
    (void)body;
    for (int k = 0; k < AMB_COUNT; k++) {
        if (!(mask & (1ull << k))) continue;
        if (k == AMB_NONE || k == AMB_SPARKLE || k == AMB_ORBIT || k == AMB_LIGHTNING) continue;
        ambient_one(ps, k, x, y, dt);
    }
}

void fx_debris(Particles *ps, float x, float y, RGB color) {
    for (int i = 0, n = nn(ps, 40); i < n; i++) {
        float a = U(ps, 0.2f, PI_F - 0.2f), s = U(ps, 12, 50), life = U(ps, 1.0f, 2.0f);
        int k = (int)rng_below(&ps->rng, 3);
        RGB c = k == 0 ? color : k == 1 ? cshade(color, 0.5f) : (RGB){36, 38, 44};
        parts_emit(ps, x, y, fcos(a) * s, fsin(a) * s, life, life, c, cshade(c, 0.4f), 80, 0, 1, true, 2, 0);
    }
}

void fx_world_tick(Particles *ps, const World *w, float dt) {
    ps->wind = (float)w->wind;
    for (int i = 0; i < w->ntanks; i++) {
        const Tank *t = &w->tanks[i];
        if (tank_alive(t) && t->kind != KIND_UFO) fx_ambient(ps, design_ptr(t->design)->ambient, (float)t->x, (float)t->y, t->color, dt);
    }
    ps->smoke_timer -= dt;
    if (ps->smoke_timer <= 0) {
        ps->smoke_timer = 0.09f;
        for (int i = 0; i < w->ntanks; i++) {
            const Tank *t = &w->tanks[i];
            if (t->destroyed) fx_smoke(ps, (float)t->x, (float)t->y + 4, 1, 3.0f, NULL);
            else if (t->hp == 1 && R(ps) < 0.35f) fx_smoke(ps, (float)t->x, (float)t->y + 6, 1, 1.5f, NULL);
        }
    }
}
