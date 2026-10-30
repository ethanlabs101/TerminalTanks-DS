#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "render.h"
#include "platform.h"

#define TAU_F 6.2831853f
#define PI_F 3.14159265f

/* ------------------------------------------------------------------ canvas helpers (y grows upward) */
static inline void cplot(Surf *cv, int x, int y, Px c) {
    int py = cv->h - 1 - y;
    if ((unsigned)x < (unsigned)cv->w && (unsigned)py < (unsigned)cv->h) cv->px[py * cv->stride + x] = c;
}
static inline void cblend(Surf *cv, int x, int y, RGB c, float a) {
    int py = cv->h - 1 - y;
    if ((unsigned)x < (unsigned)cv->w && (unsigned)py < (unsigned)cv->h && a > 0.02f) {
        Px *p = &cv->px[py * cv->stride + x];
        *p = pxmix(*p, rgb15(c), a);
    }
}
static inline void cplotf(Surf *cv, float x, float y, RGB c) { cplot(cv, (int)floorf(x), (int)floorf(y), rgb15(c)); }
static inline void cblendf(Surf *cv, float x, float y, RGB c, float a) { cblend(cv, (int)floorf(x), (int)floorf(y), c, a); }
static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline float ease_out(float t) { t = clampf(t, 0, 1); float u = 1 - t; return 1 - u * u * u; }
static const RGB WHITE = {255, 255, 255}, BLACK = {0, 0, 0};
static inline Px pxmix_k(Px a, Px b, int32_t k) {          /* k in 0..256; skips near-invisible blends like cblend */
    if (k <= 5) return a;
    if (k >= 256) return b;
    int ar = a & 31, ag = (a >> 5) & 31, ab = (a >> 10) & 31;
    int br = b & 31, bg = (b >> 5) & 31, bb = (b >> 10) & 31;
    return (Px)((ar + (((br - ar) * k) >> 8)) | ((ag + (((bg - ag) * k) >> 8)) << 5) | ((ab + (((bb - ab) * k) >> 8)) << 10) | 0x8000);
}
static inline uint32_t isqrt64(uint64_t v) {
    uint64_t r = 0, bit = 1ull << 62;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return (uint32_t)r;
}

static float flick(float t, int a, int b) {
    uint32_t v = ((uint32_t)(int)(t * 30) * 73856093u) ^ ((uint32_t)a * 19349663u) ^ ((uint32_t)b * 83492791u);
    return (v & 255) / 255.0f;
}

/* ------------------------------------------------------------------ live colours (l.2251-2303) */
static const float GAL_T[6] = {0, 0.2f, 0.4f, 0.6f, 0.8f, 1.0f};
static const RGB GAL_C[6] = {{24, 14, 96}, {96, 44, 190}, {196, 64, 196}, {56, 160, 220}, {232, 232, 255}, {24, 14, 96}};
static float frac1(float v) { return v - floorf(v); }
RGB rainbow(float t) { return chsv(roundf(frac1(t * 0.35f) * 36) / 36, 0.85f, 1.0f); }
RGB astral(float t) { return cgradient(GAL_T, GAL_C, 6, roundf(frac1(t * 0.18f) * 48) / 48); }
RGB prismatic(float t) { return chsv(roundf(frac1(t * 0.3f) * 36) / 36, 0.35f, 1.0f); }
RGB void_tint(float t) { return cmix((RGB){90, 54, 190}, (RGB){176, 120, 255}, 0.5f + 0.5f * fsin(t * 2.4f)); }
RGB live_color(RGB c, float t) {
    if (rgb_eq(c, COL_RAINBOW)) return rainbow(t);
    if (rgb_eq(c, COL_ASTRAL)) return astral(t);
    if (rgb_eq(c, COL_PRISMATIC)) return prismatic(t);
    if (rgb_eq(c, COL_VOID)) return void_tint(t);
    return c;
}
static RGB galaxy_px(float t, float c, float r) {
    float v = (fsin(c * 0.55f + t * 1.1f) + fsin(r * 0.9f - t * 0.8f) + fsin((c + r) * 0.45f + t * 0.6f) + 3) / 6;
    RGB col = cgradient(GAL_T, GAL_C, 6, roundf(v * 24) / 24);
    if (flick(t * 1.4f, (int)(c * 7 + r * 3), 23) > 0.93f) return WHITE;
    return col;
}
static RGB prismatic_px(float t, float c, float r) {
    if (flick(t * 1.6f, (int)(c * 5 + r * 11), 29) > 0.94f) return WHITE;
    float hue = frac1(c * 0.05f + r * 0.035f + t * 0.3f + 0.12f * fsin(c * 0.9f - r * 0.6f + t * 1.7f));
    return chsv(roundf(hue * 48) / 48, 0.40f + 0.2f * fsin(r * 0.5f + t), 1.0f);
}
static RGB void_px(float t, float c, float r) {
    if (flick(t * 1.2f, (int)(c * 3 + r * 7), 31) > 0.95f) return (RGB){176, 126, 255};
    float v = (fsin(c * 0.7f + t * 1.3f) + fsin(r * 1.1f - t * 0.9f) + 2) / 4;
    return cmix((RGB){6, 3, 18}, (RGB){54, 30, 118}, v);
}
static RGB paint_shade(RGB paint, RGB gb, char ch) {
    if (rgb_eq(paint, COL_VOID)) {
        switch (ch) {
        case 't': return cmix(gb, (RGB){120, 80, 210}, 0.22f);
        case 'g': return cmix(gb, (RGB){180, 130, 255}, 0.55f);
        case 'k': return cshade(gb, 0.6f);
        case 'w': return cmix(gb, (RGB){90, 70, 150}, 0.3f);
        case 'd': return cshade(gb, 0.7f);
        default: return gb;
        }
    }
    switch (ch) {
    case 't': return cmix(gb, WHITE, 0.15f);
    case 'g': return cmix(gb, WHITE, 0.4f);
    case 'k': return cshade(gb, 0.45f);
    case 'w': return cmix(gb, (RGB){200, 210, 230}, 0.2f);
    case 'd': return cshade(gb, 0.5f);
    default: return gb;
    }
}

/* ------------------------------------------------------------------ barrels & materials (l.2347-2496) */
static RGB grad3(RGB a, RGB b, RGB c, float m, float k) {
    float ts[3] = {0, m, 1};
    RGB cs[3] = {a, b, c};
    return cgradient(ts, cs, 3, k);
}
static RGB barrel_color(int style, RGB base, RGB accent, float t, float d, int lane) {
    switch (style) {
    case 9: if ((int)(d * 3 - t * 10) % 7 == 0) return (RGB){255, 252, 230};
            return cmix(cshade(base, 0.7f), accent, 0.5f + 0.5f * fsin(d * 1.4f - t * 7 + lane));
    case 1: return (int)(d * 2 - t * 14) % 6 == 0 ? accent : base;
    case 2: return (int)(d * 2.2f - t * 9) % 3 == 0 ? accent : cshade(base, 0.7f);
    case 3: return chsv(t * 0.5f + d * 0.04f, 0.55f, 1.0f);
    case 4: { float f = 0.5f + 0.5f * fsin(d * 1.7f - t * 5 + lane * 2.0f); return cmix(chsv(t * 0.45f + d * 0.07f + lane * 0.2f, 0.5f, 1.0f), WHITE, 0.6f * f); }
    case 5: { float k = clampf(d / 8.0f + 0.2f * fsin(t * 20 + d * 3), 0, 1); return grad3(base, (RGB){255, 120, 30}, (RGB){255, 230, 120}, 0.55f, k); }
    case 6: return ((int)(t * 18) + lane) % 3 == 0 ? accent : base;
    case 7: return cmix((RGB){20, 10, 40}, (RGB){170, 90, 255}, 0.5f + 0.5f * fsin(d * 1.3f - t * 6));
    case 8: return flick(t, (int)(d * 2), lane) > 0.8f ? (RGB){235, 245, 255} : cmix(base, accent, 0.5f);
    default: return base;
    }
}

static RGB material(char ch, RGB body, RGB a, float t, float c, float r) {
    switch (ch) {
    case 'p': return chsv(t * 0.35f + c * 0.085f + r * 0.14f, 0.62f, 1.0f);
    case 'q': { float k = fmaxf(0.0f, 1 - fabsf((c + r) - (fmodf(t * 7, 18) - 3)) / 2.2f); return cmix(chsv(t * 0.35f + c * 0.085f + r * 0.14f, 0.25f, 1.0f), WHITE, k); }
    case 'f': { float k = clampf(0.55f + 0.45f * fsin(t * 21 + c * 2.3f + r * 1.7f) * fsin(t * 9 + c), 0, 1); return grad3((RGB){150, 20, 8}, (RGB){255, 110, 20}, (RGB){255, 236, 120}, 0.5f, k); }
    case 'e': { float ts[2] = {0, 1}; RGB cs[2] = {cshade(body, 0.4f), {255, 90, 30}}; return cgradient(ts, cs, 2, 0.5f + 0.5f * fsin(t * 3 + c * 0.9f + r)); }
    case 'v': return cmix((RGB){8, 4, 20}, (RGB){110, 50, 200}, 0.5f + 0.5f * fsin(t * 4 + c * 0.8f - r));
    case 'b': return flick(t, (int)(c * 7 + r), 11) > 0.84f ? (RGB){240, 248, 255} : cmix(cshade(body, 0.35f), a, 0.45f);
    case 'y': { float k = fmaxf(0.0f, 1 - fabsf((c * 1.3f + r) - (fmodf(t * 6, 20) - 4)) / 2.5f); return cmix((RGB){196, 150, 50}, (RGB){255, 244, 180}, k); }
    case 'o': return grad3((RGB){255, 130, 30}, (RGB){255, 222, 96}, (RGB){255, 255, 236}, 0.5f, 0.5f + 0.5f * fsin(t * 5 - r * 1.6f + c * 0.45f));
    case 'r': if (flick(t * 0.7f, (int)(c * 3 + r), 13) > 0.78f) return (RGB){255, 244, 190};
              return cmix((RGB){140, 100, 28}, (RGB){226, 176, 70}, 0.5f + 0.5f * fsin(t * 2.4f + c * 0.9f - r));
    case 's': return cmix((RGB){196, 212, 255}, WHITE, 0.5f + 0.5f * fsin(t * 9 + c * 2.1f + r));
    case 'n': if (flick(t * 0.8f, (int)(c * 5 + r), 17) > 0.93f) return (RGB){226, 206, 255};
              return cmix((RGB){10, 6, 26}, (RGB){74, 32, 146}, 0.5f + 0.5f * fsin(t * 2 + c * 0.7f + r));
    case 'm': return cmix((RGB){20, 110, 150}, (RGB){150, 255, 240}, 0.5f + 0.5f * fsin(t * 3.5f - c * 0.9f + r * 0.6f));
    case 'z': { float k = fmaxf(0.0f, 1 - fabsf((c + r * 2) - (fmodf(t * 8, 24) - 6)) / 2.2f);
                return cmix(cmix((RGB){120, 70, 30}, (RGB){232, 172, 72}, 0.5f + 0.5f * fsin(t * 1.5f + c)), (RGB){255, 250, 222}, k); }
    }
    return body;
}
static bool is_material(char ch) { return ch && strchr("pqfevbyorsnmz", ch) != NULL; }

static RGB glow_color(const TankDesign *d, RGB body, float t, int idx) {
    RGB a = d->accent;
    switch (d->glow) {
    case GLOW_AEON: return grad3((RGB){90, 40, 190}, (RGB){255, 214, 120}, (RGB){255, 255, 244}, 0.5f, 0.5f + 0.5f * fsin(t * 3.2f + idx * 0.8f));
    case GLOW_PULSE: return cmix(cshade(a, 0.3f), a, 0.5f + 0.5f * fsin(t * 4 + idx));
    case GLOW_BLINK: return (int)(t * 3 + idx) % 2 == 0 ? a : cshade(a, 0.25f);
    case GLOW_FLAME: { float k = 0.5f + 0.5f * fsin(t * 23 + idx * 2.1f) * fsin(t * 11 + idx); return grad3((RGB){120, 30, 10}, (RGB){255, 120, 20}, (RGB){255, 232, 130}, 0.5f, k); }
    case GLOW_PRISM: return chsv(t * 0.4f + idx * 0.13f, 0.7f, 1.0f);
    case GLOW_SCAN: { float k = fmaxf(0.0f, 1 - fabsf(idx - (fmodf(t * 9, 15) - 2)) / 2.5f); return cmix(cmix(cshade(a, 0.28f), a, 0.35f), WHITE, k * 0.85f); }
    case GLOW_PLASMA: return grad3((RGB){255, 90, 20}, a, (RGB){255, 250, 205}, 0.5f, 0.5f + 0.5f * fsin(t * 4.5f + idx * 0.7f));
    case GLOW_LAVA: { float k = clampf(0.5f + 0.4f * fsin(t * 2.2f + idx * 1.7f) + 0.2f * (flick(t, idx, 3) - 0.5f), 0, 1);
                      return grad3((RGB){90, 10, 6}, (RGB){230, 70, 18}, (RGB){255, 196, 80}, 0.6f, k); }
    case GLOW_STORM: { float f = flick(t, idx, 5); return f > 0.86f ? cmix(cshade(a, 0.22f), (RGB){235, 245, 255}, 0.85f) : cmix(cshade(a, 0.22f), a, f * 0.6f); }
    case GLOW_MATRIX: return flick(t, idx, 9) > 0.5f ? (RGB){96, 255, 130} : (RGB){18, 96, 44};
    case GLOW_REFRACT: { float k = fmaxf(0.0f, 1 - fabsf(idx - (fmodf(t * 8, 16) - 2)) / 2.0f); return cmix(chsv(t * 0.4f + idx * 0.09f, 0.6f, 1.0f), WHITE, k * 0.8f); }
    case GLOW_EMBER: return grad3((RGB){110, 14, 8}, (RGB){230, 60, 20}, (RGB){255, 190, 90}, 0.6f,
                                 clampf(0.5f + 0.35f * fsin(t * 2.6f + idx * 1.3f) + 0.3f * (flick(t, idx, 7) - 0.5f), 0, 1));
    case GLOW_VOID: return cmix((RGB){10, 4, 24}, a, 0.5f + 0.5f * fsin(t * 3 + idx * 0.9f));
    case GLOW_ICE: return cmix(cshade(a, 0.5f), (RGB){240, 252, 255}, 0.5f + 0.5f * fsin(t * 2.4f + idx * 0.8f));
    case GLOW_TOXIC: return cmix((RGB){20, 70, 20}, (RGB){170, 255, 60}, 0.5f + 0.5f * fsin(t * 5 + idx * 1.1f));
    case GLOW_NEON: return ((int)(t * 4) + idx) % 2 == 0 ? (RGB){255, 70, 220} : (RGB){60, 240, 255};
    case GLOW_GOLD: { float k = fmaxf(0.0f, 1 - fabsf(idx - (fmodf(t * 6, 15) - 2)) / 2.5f); return cmix((RGB){190, 140, 40}, (RGB){255, 244, 170}, k); }
    }
    return cmix(body, WHITE, 0.6f);
}

static void ambient_overlay(Surf *cv, float x, float y, int scale, const TankDesign *d, float t) {
    uint32_t k = d->ambient;
    RGB acc = d->accent;
    if (k & (1u << AMB_HALO))
        for (int i = 0; i < 14; i++) {
            float ang = t * 1.2f + i * TAU_F / 14;
            cblendf(cv, x + fcos(ang) * 6.5f * scale, y + 9 * scale + fsin(ang) * 1.6f * scale, (RGB){255, 232, 150}, 0.35f + 0.5f * (0.5f + 0.5f * fsin(ang * 2 - t * 4)));
        }
    if (k & (1u << AMB_GLYPHS))
        for (int i = 0; i < 4; i++) {
            float ang = -t * 0.9f + i * PI_F / 2;
            float gx = x + fcos(ang) * 8.5f * scale, gy = y + 4 * scale + fsin(ang) * 3.2f * scale, a = 0.45f + 0.45f * fsin(t * 5 + i * 1.9f);
            cblendf(cv, gx, gy, acc, a);
            cblendf(cv, gx + 1, gy, acc, a * 0.4f); cblendf(cv, gx - 1, gy, acc, a * 0.4f);
            cblendf(cv, gx, gy + 1, acc, a * 0.4f); cblendf(cv, gx, gy - 1, acc, a * 0.4f);
        }
    if (k & (1u << AMB_RIFTMOTES))
        for (int i = 0; i < 8; i++) {
            float ang = t * 1.5f + i * TAU_F / 8;
            cblendf(cv, x + fcos(ang) * (6 + i % 3) * scale, y + 4 * scale + fsin(ang) * 3 * scale, i % 2 == 0 ? (RGB){255, 244, 205} : (RGB){120, 60, 220}, 0.85f);
        }
    if (k & (1u << AMB_SPARKLE))
        for (int i = 0; i < 4; i++) {
            float ang = t * 1.3f + i * PI_F / 2;
            float sx = x + fcos(ang) * 8 * scale, sy = y + 3 * scale + fsin(ang) * 5 * scale, tw = 0.5f + 0.5f * fsin(t * 9 + i * 1.7f);
            cblendf(cv, sx, sy, (RGB){255, 230, 150}, 0.85f * tw);
            cblendf(cv, sx + 1, sy, (RGB){255, 200, 110}, 0.3f * tw); cblendf(cv, sx - 1, sy, (RGB){255, 200, 110}, 0.3f * tw);
            cblendf(cv, sx, sy + 1, (RGB){255, 200, 110}, 0.3f * tw); cblendf(cv, sx, sy - 1, (RGB){255, 200, 110}, 0.3f * tw);
        }
    if (k & (1u << AMB_PRISM))
        for (int i = 0; i < 6; i++) {
            float ph = frac1(t * 0.9f + i * 0.37f);
            float sx = x + fcos(i * 2.4f + t * 0.6f) * (5 + i % 3) * scale;
            float sy = y + (1.5f + 3.2f * ((i * 5) % 4) / 3.0f) * scale + 2 * fsin(t * 2 + i);
            RGB col = chsv(frac1(i / 6.0f + t * 0.4f), 0.5f, 1.0f);
            float a = fsin(ph * PI_F);
            cblendf(cv, sx, sy, WHITE, 0.95f * a);
            for (int s = 1; s <= 2; s++) {
                float al = s == 1 ? 0.55f * a : 0.25f * a;
                cblendf(cv, sx + s, sy, col, al); cblendf(cv, sx - s, sy, col, al); cblendf(cv, sx, sy + s, col, al); cblendf(cv, sx, sy - s, col, al);
            }
        }
    if (k & (1u << AMB_ORBIT))
        for (int i = 0; i < 5; i++) {
            float ang = t * (1.6f + 0.2f * i) + i * 1.26f;
            float sx = x + fcos(ang) * (7 + i % 2) * scale, sy = y + 3.5f * scale + fsin(ang) * 2.4f * scale;
            cblendf(cv, sx, sy, acc, 0.9f);
            cblendf(cv, sx - fcos(ang + 1.5f) * 0.9f, sy - fsin(ang + 1.5f) * 0.9f, acc, 0.35f);
        }
    if (k & (1u << AMB_LIGHTNING)) {
        int step = (int)(t * 14);
        for (int i = 0; i < 2; i++) {
            if (flick(step / 30.0f, i, 33) > 0.55f) {
                float px = x + (flick(step / 30.0f, i, 1) - 0.5f) * 9 * scale, py = y + 6 * scale;
                for (int s = 0; s < 5; s++) {
                    px += (flick(step / 30.0f, i, 10 + s) - 0.5f) * 3;
                    py += 1.6f;
                    cblendf(cv, px, py, (RGB){230, 245, 255}, 0.9f - s * 0.14f);
                }
            }
        }
    }
    if (k & (1u << AMB_VOID))
        for (int i = 0; i < 6; i++) {
            float kk = frac1(t * 0.8f + i / 6.0f), ang = i * 1.05f + t * 0.7f;
            cblendf(cv, x + fcos(ang) * (1 - kk) * 9 * scale, y + 3 * scale + fsin(ang) * (1 - kk) * 5 * scale, (RGB){170, 110, 255}, 0.7f * kk);
        }
}

void draw_tank(Surf *cv, float x, float y, int facing, float angle, RGB body, int scale, float flash, bool wreck, float recoil,
               const TankDesign *d, float t, float lift_in, float hover_in) {
    bool galaxy_on = rgb_is_pixel_paint(body) && !wreck;
    RGB paint = body;
    if (galaxy_on) body = live_color(paint, t);
    else if (d->has_paint) body = d->paint;
    else if (rgb_eq(body, COL_RAINBOW)) body = live_color(body, t);
    float lift = lift_in < 0 ? (float)d->hover : lift_in;
    bool hover_given = hover_in >= 0;
    float air = hover_given ? hover_in : lift;
    float ground_y = y;
    if (air > 0 && !wreck) {
        float bob = 0.6f * fsin(t * 2.4f) * scale;
        y = y + lift + bob;
        ground_y = hover_given ? y - air * scale - bob : y - lift - bob;
    } else if (lift != 0) {
        y = y + lift;
    }
    if (air > 0 && !wreck) {
        float sh = clampf(1.0f - air * scale / 16.0f, 0.35f, 0.9f);
        for (int dx = (int)(-5.5f * scale); dx <= (int)(5.5f * scale); dx++) {
            float k = 1 - fabsf((float)dx) / (5.5f * scale);
            cblendf(cv, x + dx, ground_y, BLACK, 0.55f * sh * k);
        }
        for (int k = 0; k < 3; k++) cblendf(cv, x + (k - 1) * 2 * scale, y - 1, d->accent, 0.35f + 0.25f * fsin(t * 17 + k * 2));
    }
    RGB pal_t, pal_h, pal_g, pal_k, pal_w, pal_d, pal_a, pal_l;
    if (wreck) {
        pal_t = (RGB){34, 34, 38}; pal_h = (RGB){28, 28, 32}; pal_g = (RGB){44, 40, 40}; pal_k = (RGB){18, 18, 20};
        pal_w = (RGB){30, 30, 34}; pal_d = (RGB){20, 20, 22}; pal_a = (RGB){44, 40, 38}; pal_l = (RGB){62, 34, 22};
    } else {
        pal_t = cmix(body, WHITE, 0.28f); pal_h = body; pal_g = cmix(body, WHITE, 0.5f);
        pal_k = cmix(cshade(body, 0.3f), (RGB){36, 40, 48}, 0.5f); pal_w = cmix(body, (RGB){200, 210, 220}, 0.3f);
        pal_d = cshade(body, 0.38f); pal_a = d->accent; pal_l = body;
    }
    if (flash > 0) {
        pal_t = cmix(pal_t, WHITE, flash); pal_h = cmix(pal_h, WHITE, flash); pal_g = cmix(pal_g, WHITE, flash);
        pal_k = cmix(pal_k, WHITE, flash); pal_w = cmix(pal_w, WHITE, flash); pal_d = cmix(pal_d, WHITE, flash);
        pal_a = cmix(pal_a, WHITE, flash); pal_l = cmix(pal_l, WHITE, flash);
    }
    if (!wreck) {
        float a = angle * (PI_F / 180.0f), ca = fcos(a), sa = fsin(a);
        const BarrelSpec *spec = &BARRELS[d->barrel];
        float px = x, py = y + (float)PIVOT_H * scale;
        float length = ((float)spec->length - recoil * 2.5f) * scale;
        RGB base = cmix(body, (RGB){225, 232, 240}, 0.4f);
        if (flash > 0) base = cmix(base, WHITE, flash);
        for (int lane = 0; lane < spec->n; lane++) {
            float off = (float)spec->offsets[lane];
            for (int k = 0; k <= (int)(length * 2); k++) {
                float dd = k * 0.5f;
                float bx = px + dd * ca - off * scale * sa, by = py + dd * sa + off * scale * ca;
                RGB col = barrel_color(spec->style, base, d->accent, t, dd, lane);
                for (int ox = 0; ox < scale; ox++)
                    for (int oy = 0; oy < scale; oy++) cplotf(cv, bx + ox * 0.9f - (scale - 1) * 0.45f, by + oy * 0.9f - (scale - 1) * 0.45f, col);
            }
        }
        cplotf(cv, px + length * ca, py + length * sa, cmix(base, WHITE, 0.6f));
    }
    int base_x = (int)floorf(x) - 5 * scale, base_y = (int)rintf(y);
    for (int r = 0; r < TANK_H; r++) {
        if (wreck && r < 2 && (r == 0 || facing > 0)) continue;
        for (int c = 0; c < TANK_W; c++) {
            char ch = d->mask[r * TANK_W + c];
            if (ch == '.') continue;
            int cc = facing > 0 ? c : TANK_W - 1 - c;
            RGB col;
            if (ch == 'l' && !wreck) col = glow_color(d, body, t, cc);
            else if (is_material(ch) && !wreck) col = material(ch, body, d->accent, t, (float)cc, (float)r);
            else if (galaxy_on && strchr("thgkwd", ch)) {
                RGB gb = rgb_eq(paint, COL_ASTRAL) ? galaxy_px(t, cc, r) : rgb_eq(paint, COL_PRISMATIC) ? prismatic_px(t, cc, r) : void_px(t, cc, r);
                col = paint_shade(paint, gb, ch);
            } else {
                switch (ch) {
                case 't': col = pal_t; break;
                case 'g': col = pal_g; break;
                case 'k': col = pal_k; break;
                case 'w': col = pal_w; break;
                case 'd': col = pal_d; break;
                case 'a': col = pal_a; break;
                case 'l': col = pal_l; break;
                default: col = pal_h;
                }
            }
            if (flash > 0 && (ch == 'l' || is_material(ch)) && !wreck) col = cmix(col, WHITE, flash);
            int wx = base_x + cc * scale, wy = base_y + (TANK_H - 1 - r) * scale;
            Px p = rgb15(col);
            for (int ox = 0; ox < scale; ox++)
                for (int oy = 0; oy < scale; oy++) cplot(cv, wx + ox, wy + oy, p);
        }
    }
    if (!wreck) ambient_overlay(cv, x, y, scale, d, t);
}

/* ------------------------------------------------------------------ UFO (l.2662) */
void draw_ufo(Surf *cv, const Tank *tk, float now, float flash) {
    if (tk->destroyed) return;
    float w = (float)tank_w(tk), h = (float)tank_h(tk);
    float cx = (float)(tk->x + tk->vis_dx), cy = (float)tk->y + 0.9f * fsin(now * 2.0f + tk->index);
    RGB body = tk->color;
    bool firing = tk->recoil > 0;
    float gy = (float)(tk->y - tk->hover);
    for (int dx = (int)(-w * 0.42f); dx <= (int)(w * 0.42f); dx++) {
        float k = 1 - fabsf((float)dx) / (w * 0.42f);
        cblendf(cv, (float)tk->x + dx, gy, BLACK, 0.5f * k * clampf(1.2f - (float)tk->hover / 40.0f, 0.35f, 0.9f));
        if (k > 0.5f) cblendf(cv, (float)tk->x + dx, gy + 1, BLACK, 0.25f * k);
    }
    RGB hull_a = cmix(body, WHITE, 0.35f), hull_b = cshade(body, 0.45f);
    for (int ix = (int)(-w / 2) - 1; ix < (int)(w / 2) + 2; ix++)
        for (int iy = -1; iy < (int)h + 2; iy++) {
            float u = ix / (w / 2), v = (iy - h * 0.40f) / (h * 0.5f);
            bool have = false;
            RGB col = {0, 0, 0};
            if ((u * u) + (v / 0.42f) * (v / 0.42f) <= 1.0f) {
                float sk = 0.5f + 0.5f * (-v / 0.42f);
                col = cmix(hull_b, hull_a, clampf(sk, 0, 1));
                if (fabsf(v) < 0.06f) col = cmix(col, WHITE, 0.5f);
                have = true;
            }
            float dv = (iy - h * 0.52f) / (h * 0.42f);
            if (u * u / 0.25f + dv * dv <= 1.0f && dv >= -0.05f) {
                col = cmix((RGB){60, 220, 200}, (RGB){190, 255, 255}, clampf(0.3f + 0.7f * dv, 0, 1));
                if (dv > 0.45f && u < -0.05f) col = cmix(col, WHITE, 0.55f);
                have = true;
            }
            float pv = (iy - h * 0.16f) / (h * 0.16f);
            if ((u / 0.28f) * (u / 0.28f) + pv * pv <= 1.0f) {
                col = cmix(cshade(body, 0.3f), firing ? (RGB){255, 220, 120} : (RGB){90, 100, 120}, firing ? 0.7f : 0.3f);
                have = true;
            }
            if (have) {
                if (flash > 0) col = cmix(col, WHITE, fminf(1.0f, flash));
                cplotf(cv, cx + ix, cy + iy, col);
            }
        }
    for (int k = 0; k < 9; k++) {
        float ang = k / 9.0f * TAU_F;
        bool on = (int)(now * 6 + k) % 3 == 0;
        cplotf(cv, cx + fcos(ang) * w * 0.44f, cy + h * 0.40f + fsin(ang) * h * 0.14f, on ? chsv(frac1(k / 9.0f + now * 0.2f), 0.5f, 1.0f) : cshade(body, 0.4f));
    }
    for (int dy = 1; dy <= (int)fminf(8, (float)tk->hover); dy++)
        cblendf(cv, cx, cy + h * 0.1f - dy, (RGB){150, 255, 200}, 0.38f - dy * 0.04f + 0.1f * fsin(now * 14 + dy));
    if (firing || (int)(now * 3) % 5 == 0)
        for (int dy = 1; dy < 6; dy++) cblendf(cv, cx, cy + h * 0.08f - dy, (RGB){170, 255, 200}, 0.5f - dy * 0.08f);
    if (tk->shield > 0)
        for (int k = 0; k < (int)(w * 4); k++) {
            float ang = k / (w * 4) * TAU_F, fl = 0.35f + 0.3f * fsin(now * 9 + k * 0.7f);
            cblendf(cv, cx + fcos(ang) * w * 0.62f, cy + h * 0.5f + fsin(ang) * h * 0.78f, (RGB){140, 230, 255}, fl);
        }
}

/* ------------------------------------------------------------------ projectiles (l.2721) */
static const char *const GLYPH_BITS[8][5] = {
    {"#.#", "###", "#.#", "###", "#.#"}, {"###", "#.#", "#.#", "#.#", "###"}, {".#.", "##.", ".#.", ".#.", "###"}, {"...", ".#.", "###", ".#.", "..."},
    {"#.#", ".#.", "###", ".#.", "#.#"}, {"#.#", "..#", ".#.", "#..", "#.#"}, {".#.", "#.#", ".#.", "#.#", ".##"}, {"..#", "..#", ".#.", "#..", "#.."}};
static void draw_glyph(Surf *cv, float x, float y, int k, RGB col, float a) {
    const char *const *g = GLYPH_BITS[((k % 8) + 8) % 8];
    for (int r = 0; r < 5; r++)
        for (int c = 0; c < 3; c++)
            if (g[r][c] == '#') cblendf(cv, x + c - 1, y + 2 - r, col, a);
}

void draw_projectile(Surf *cv, float x, float y, float vx, float vy, RGB accent, const AmmoType *ammo, float t) {
    accent = live_color(accent, t);
    float sp = fhypot(vx, vy);
    if (sp == 0) sp = 1;
    float ux = vx / sp, uy = vy / sp;
    static const int NB[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    switch (ammo->proj) {
    case PROJ_STREAK:
        for (int i = 0; i < 6; i++) cblendf(cv, x - ux * i, y - uy * i, cmix(WHITE, accent, i / 6.0f), 1.0f - i / 7.0f);
        break;
    case PROJ_ORB: {
        float r = 1.5f + 0.4f * fsin(t * 14);
        for (int dx = -3; dx <= 3; dx++)
            for (int dy = -3; dy <= 3; dy++) {
                float d = fsqrt((float)(dx * dx + dy * dy));
                if (d <= r + 0.5f) cblendf(cv, x + dx, y + dy, cmix(accent, WHITE, 0.4f), 0.9f);
                else if (d <= 3.2f) cblendf(cv, x + dx, y + dy, accent, 0.22f);
            }
        cplotf(cv, x, y, WHITE);
        break;
    }
    case PROJ_SPARK:
        cplotf(cv, x, y, WHITE);
        for (int k = 0; k < 3; k++) cblendf(cv, x + (flick(t, k, 1) - 0.5f) * 3.5f, y + (flick(t, k, 2) - 0.5f) * 3.5f, accent, 0.9f);
        break;
    case PROJ_COMET:
        for (int dx = -1; dx <= 1; dx++)
            for (int dy = -1; dy <= 1; dy++) cblendf(cv, x + dx, y + dy, cmix(accent, WHITE, 0.3f), 0.7f);
        cplotf(cv, x, y, WHITE);
        break;
    case PROJ_DRIP:
        cplotf(cv, x, y, cmix(accent, WHITE, 0.35f));
        for (int i = 0; i < 4; i++) cblendf(cv, x + NB[i][0], y + NB[i][1], accent, 0.6f);
        cblendf(cv, x, y - 2, accent, 0.55f);
        cblendf(cv, x, y - 3, accent, 0.25f);
        break;
    case PROJ_STAR:
    case PROJ_PRISM: {
        float tw = 0.5f + 0.5f * fsin(t * 12);
        cplotf(cv, x, y, WHITE);
        for (int i = 0; i < 4; i++) {
            RGB c = ammo->proj == PROJ_STAR ? accent : chsv(t * 1.2f + i * 0.25f, 0.6f, 1.0f);
            cblendf(cv, x + NB[i][0], y + NB[i][1], c, 0.95f);
            cblendf(cv, x + NB[i][0] * 2, y + NB[i][1] * 2, c, 0.35f + 0.5f * tw);
        }
        if (tw > 0.55f) { cblendf(cv, x + 1, y + 1, WHITE, 0.5f); cblendf(cv, x - 1, y + 1, WHITE, 0.5f); cblendf(cv, x + 1, y - 1, WHITE, 0.5f); cblendf(cv, x - 1, y - 1, WHITE, 0.5f); }
        break;
    }
    case PROJ_RELIC: {
        float a0 = t * 6;
        for (int k = 0; k < 12; k++) { float a = k * TAU_F / 12 - a0 * 0.5f; cblendf(cv, x + fcos(a) * 3.4f, y + fsin(a) * 3.4f, (RGB){255, 226, 140}, 0.35f); }
        for (int k = 0; k < 4; k++) { float a = a0 + k * PI_F / 2; cblendf(cv, x + fcos(a) * 2.2f, y + fsin(a) * 2.2f, cmix(accent, WHITE, 0.5f), 0.9f); }
        for (int i = 0; i < 4; i++) cblendf(cv, x + NB[i][0], y + NB[i][1], accent, 0.95f);
        cplotf(cv, x, y, WHITE);
        break;
    }
    case PROJ_EYE:
        for (int dx = -3; dx <= 3; dx++)
            for (int dy = -3; dy <= 3; dy++) {
                float d = fhypot(dx * 0.8f, dy * 1.25f);
                if (d <= 2.6f) cblendf(cv, x + dx, y + dy, d > 1.1f ? cmix(accent, WHITE, 0.55f) : cshade(accent, 0.1f), 0.95f);
            }
        for (int dy = -1; dy <= 1; dy++) cblendf(cv, x, y + dy, (RGB){10, 4, 20}, 0.9f);
        for (int k = 0; k < 10; k++) { float a = k * TAU_F / 10 + t * 3; cblendf(cv, x + fcos(a) * 3.8f, y + fsin(a) * 3.8f, accent, 0.4f); }
        break;
    case PROJ_RUNES: {
        float a0 = t * 9;
        for (int k = 0; k < 4; k++) {
            float a = a0 + k * PI_F / 2;
            for (int s = 1; s <= 3; s++) cblendf(cv, x + fcos(a) * s, y + fsin(a) * s, s < 3 ? accent : WHITE, 0.95f - 0.2f * s);
        }
        cplotf(cv, x, y, WHITE);
        break;
    }
    case PROJ_SIGIL: {
        float a0 = -t * 5;
        for (int k = 0; k < 6; k++) {
            float a = a0 + k * TAU_F / 6;
            cblendf(cv, x + fcos(a) * 2.6f, y + fsin(a) * 2.6f, k % 2 ? (RGB){255, 150, 40} : WHITE, 0.95f);
            cblendf(cv, x + fcos(a) * 1.4f, y + fsin(a) * 1.4f, (RGB){255, 214, 120}, 0.7f);
        }
        cplotf(cv, x, y, WHITE);
        break;
    }
    case PROJ_SANDGLASS:
        for (int dy = -3; dy <= 3; dy++) {
            float w = fabsf((float)dy) * 0.8f;
            for (int dx = -3; dx <= 3; dx++)
                if (fabsf((float)dx) <= w && abs(dy) >= 1) cblendf(cv, x + dx, y + dy, cmix(accent, WHITE, 0.4f), 0.85f);
        }
        cplotf(cv, x, y + 1 - fmodf(t * 14, 3), WHITE);
        break;
    case PROJ_VOID:
        for (int dx = -3; dx <= 3; dx++)
            for (int dy = -3; dy <= 3; dy++) {
                float d = fsqrt((float)(dx * dx + dy * dy));
                if (d <= 1.2f) cblendf(cv, x + dx, y + dy, cshade(accent, 0.08f), 1.0f);
                else if (d <= 2.2f) cblendf(cv, x + dx, y + dy, cmix(accent, WHITE, 0.25f), 0.95f);
                else if (d <= 3.2f) cblendf(cv, x + dx, y + dy, cshade(accent, 0.2f), 0.3f);
            }
        break;
    case PROJ_RING: {
        float r = 2.4f + 1.1f * fabsf(fsin(t * 9));
        for (int dx = -4; dx <= 4; dx++)
            for (int dy = -4; dy <= 4; dy++) {
                float d = fsqrt((float)(dx * dx + dy * dy));
                if (fabsf(d - r) < 0.75f) cblendf(cv, x + dx, y + dy, cmix(accent, WHITE, 0.3f), 0.9f);
                else if (d < r - 0.75f) cblendf(cv, x + dx, y + dy, accent, 0.18f);
            }
        cplotf(cv, x, y, WHITE);
        break;
    }
    case PROJ_SHARD: {
        float px = -uy, py = ux;
        for (int i = -3; i <= 3; i++) {
            float k = 1 - abs(i) / 4.0f;
            cblendf(cv, x + ux * i, y + uy * i, cmix(accent, (RGB){225, 248, 255}, k), 0.95f);
            if (abs(i) < 2) {
                cblendf(cv, x + ux * i + px * 0.9f, y + uy * i + py * 0.9f, accent, 0.7f);
                cblendf(cv, x + ux * i - px * 0.9f, y + uy * i - py * 0.9f, accent, 0.7f);
            }
        }
        if ((int)(t * 14) % 3 == 0) cblendf(cv, x, y + 2, WHITE, 0.6f);
        break;
    }
    case PROJ_SUN:
        for (int dx = -3; dx <= 3; dx++)
            for (int dy = -3; dy <= 3; dy++) {
                float d = fsqrt((float)(dx * dx + dy * dy));
                if (d <= 1.6f) cblendf(cv, x + dx, y + dy, cmix((RGB){255, 250, 210}, WHITE, 0.5f), 1.0f);
                else if (d <= 3.2f) cblendf(cv, x + dx, y + dy, accent, 0.28f);
            }
        for (int k = 0; k < 8; k++) {
            float a = k * PI_F / 4 + t * 7;
            float ss[3] = {2.4f, 3.4f, k % 2 == 0 ? 4.4f : 3.4f};
            for (int j = 0; j < 3; j++) cblendf(cv, x + fcos(a) * ss[j], y + fsin(a) * ss[j], cmix(accent, WHITE, 0.5f), 0.85f - (ss[j] - 2.4f) * 0.15f);
        }
        break;
    case PROJ_LAVA:
        for (int dx = -1; dx <= 1; dx++)
            for (int dy = -1; dy <= 1; dy++) {
                bool hot = flick(t, dx + 3, dy + 7) > 0.45f;
                cblendf(cv, x + dx, y + dy, hot ? (RGB){255, 190, 70} : (RGB){200, 60, 18}, 0.95f);
            }
        cplotf(cv, x, y, (RGB){255, 240, 170});
        cblendf(cv, x - ux * 2.2f, y - uy * 2.2f - 1, (RGB){255, 120, 30}, 0.8f);
        cblendf(cv, x - ux * 3.4f, y - uy * 3.4f - 2, (RGB){170, 40, 12}, 0.6f);
        cblendf(cv, x + 1.5f, y - 2.5f - fmodf(t * 14, 2), (RGB){255, 150, 40}, 0.7f);
        break;
    case PROJ_BOLT:
        cplotf(cv, x, y, WHITE);
        for (int i = 0; i < 4; i++) cblendf(cv, x + NB[i][0], y + NB[i][1], cmix(accent, WHITE, 0.5f), 0.9f);
        for (int k = 0; k < 4; k++) {
            float ang = flick(t, k, 4) * TAU_F;
            for (int s = 1; s < 4; s++) {
                float nx = x + fcos(ang) * s * 1.5f + (flick(t, k, s + 10) - 0.5f) * 2;
                float ny = y + fsin(ang) * s * 1.5f + (flick(t, k, s + 20) - 0.5f) * 2;
                cblendf(cv, nx, ny, cmix(accent, WHITE, 0.7f), 0.85f - s * 0.2f);
            }
        }
        break;
    case PROJ_BLESSING:
        for (int k = 0; k < 8; k++) {
            float a = t * 5 + k * PI_F / 4;
            int ln = k % 2 == 0 ? 5 : 3;
            for (int s = 1; s <= ln; s++) cblendf(cv, x + fcos(a) * s, y + fsin(a) * s, k % 2 == 0 ? (RGB){255, 226, 120} : WHITE, 0.95f - s * 0.12f);
        }
        for (int dx = -2; dx <= 2; dx++)
            for (int dy = -2; dy <= 2; dy++)
                if (dx * dx + dy * dy <= 4.84f) cblendf(cv, x + dx, y + dy, (RGB){255, 250, 220}, 0.9f);
        for (int s = 2; s < 8; s++) cblendf(cv, x + fcos(t * 3) * s, y + fsin(t * 3) * s, (RGB){255, 200, 80}, 0.9f - s * 0.08f);
        for (int k = 0; k < 3; k++) { float a = -t * 6 + k * TAU_F / 3; cblendf(cv, x + fcos(a) * 6.5f, y + fsin(a) * 6.5f, (RGB){176, 120, 255}, 0.95f); }
        break;
    case PROJ_PHASE: {
        int fl = (int)(t * 18) % 2;
        cblendf(cv, x, y, (RGB){170, 255, 200}, fl ? 0.95f : 0.5f);
        cblendf(cv, x - ux * 3, y - uy * 3, (RGB){190, 130, 255}, fl ? 0.45f : 0.9f);
        cplotf(cv, x, y, WHITE);
        break;
    }
    case PROJ_XENON: {
        float pulse = 0.5f + 0.5f * fsin(t * 9);
        for (int dx = -4; dx <= 4; dx++)
            for (int dy = -4; dy <= 4; dy++) {
                float d = fsqrt((float)(dx * dx + dy * dy));
                if (d <= 1.6f) cblendf(cv, x + dx, y + dy, cmix((RGB){235, 255, 245}, WHITE, 0.6f * pulse), 1.0f);
                else if (d <= 3.6f) cblendf(cv, x + dx, y + dy, cmix((RGB){60, 235, 160}, (RGB){120, 80, 230}, (d - 1.6f) / 2.0f), 0.3f + 0.25f * pulse);
            }
        for (int k = 0; k < 6; k++) {
            float a = t * 8 + k * TAU_F / 6;
            cblendf(cv, x + fcos(a) * 4.6f, y + fsin(a) * 4.6f, k % 2 ? (RGB){210, 140, 255} : (RGB){150, 255, 210}, 0.95f);
            cblendf(cv, x + fcos(a - 0.35f) * 4.6f, y + fsin(a - 0.35f) * 4.6f, (RGB){120, 70, 220}, 0.5f);
        }
        for (int k = 0; k < 4; k++) { float a = -t * 12 + k * PI_F / 2; cblendf(cv, x + fcos(a) * 2.6f, y + fsin(a) * 2.6f, WHITE, 0.9f); }
        for (int k = 0; k < 4; k++) {
            float a = PI_F / 4 + k * PI_F / 2 + t * 1.5f;
            for (int s = 3; s < 3 + (int)(3 + 3 * pulse); s++) cblendf(cv, x + fcos(a) * s, y + fsin(a) * s, (RGB){180, 255, 220}, 0.55f - s * 0.05f);
        }
        break;
    }
    case PROJ_HELIX: {
        float px = -uy, py = ux;
        for (int k = 0; k < 8; k++) {
            float ph = t * 14 - k * 0.9f, s = k * 1.3f, off = fsin(ph) * 2.2f;
            RGB col = chsv(t * 0.8f + k * 0.1f, 0.6f, 1.0f);
            float a = 0.95f - k * 0.1f;
            cblendf(cv, x - ux * s + px * off, y - uy * s + py * off, col, a);
            cblendf(cv, x - ux * s - px * off, y - uy * s - py * off, cmix(col, WHITE, 0.4f), a);
        }
        cplotf(cv, x, y, WHITE);
        break;
    }
    case PROJ_GLYPH: {
        int k = (int)(t * 12);
        draw_glyph(cv, x, y, k, cmix((RGB){90, 255, 130}, WHITE, 0.35f), 1.0f);
        draw_glyph(cv, x - ux * 5, y - uy * 5 + 1, k + 3, (RGB){40, 190, 80}, 0.45f);
        break;
    }
    case PROJ_FLAME:
        for (int dx = -1; dx <= 1; dx++)
            for (int dy = -1; dy <= 1; dy++)
                if (flick(t, dx + 5, dy + 5) > 0.25f) {
                    float ts[2] = {0, 1};
                    RGB cs[2] = {{255, 236, 150}, cmix((RGB){255, 100, 20}, accent, 0.3f)};
                    cblendf(cv, x + dx, y + dy, cgradient(ts, cs, 2, flick(t, dx, dy)), 0.95f);
                }
        for (int k = 1; k < 3; k++) cblendf(cv, x + (flick(t, k, 9) - 0.5f), y + k + 1, (RGB){255, 150, 40}, 0.7f / k);
        cplotf(cv, x, y, (RGB){255, 250, 220});
        break;
    default:
        cplotf(cv, x, y, WHITE);
        for (int i = 0; i < 4; i++) cblendf(cv, x + NB[i][0], y + NB[i][1], accent, 0.75f);
    }
}

/* ------------------------------------------------------------------ explosions (l.3020) */
static void ringline(Surf *cv, float ex, float ey, float r, RGB col, float a) {
    int n = (int)(TAU_F * r * 1.5f) + 1;
    for (int k = 0; k < n; k++) {
        float ang = k / fmaxf(1.0f, r * 1.5f);
        cblendf(cv, ex + fcos(ang) * r, ey + fsin(ang) * r, col, a);
    }
}

static void exp_palette(const Explosion *e, RGB *pal) {
    RGB a = e->accent;
    float ts[5];
    RGB cs[5];
    int n = 5;
#define S5(t0, c0, t1, c1, t2, c2, t3, c3, t4, c4) do { ts[0]=t0;cs[0]=c0;ts[1]=t1;cs[1]=c1;ts[2]=t2;cs[2]=c2;ts[3]=t3;cs[3]=c3;ts[4]=t4;cs[4]=c4; n=5; } while (0)
#define S4(t0, c0, t1, c1, t2, c2, t3, c3) do { ts[0]=t0;cs[0]=c0;ts[1]=t1;cs[1]=c1;ts[2]=t2;cs[2]=c2;ts[3]=t3;cs[3]=c3; n=4; } while (0)
    switch (e->style) {
    case BOOM_IMPLODE: S4(0, cshade(a, 0.12f), 0.45f, cshade(a, 0.35f), 0.8f, a, 1.0f, cmix(a, WHITE, 0.6f)); break;
    case BOOM_FIRE: S5(0, ((RGB){255, 250, 220}), 0.25f, ((RGB){255, 214, 110}), 0.55f, cmix((RGB){255, 110, 24}, a, 0.25f), 0.85f, ((RGB){150, 32, 10}), 1.0f, ((RGB){50, 14, 8})); break;
    case BOOM_MAGMA: S5(0, ((RGB){255, 244, 190}), 0.25f, ((RGB){255, 176, 60}), 0.55f, ((RGB){226, 70, 18}), 0.85f, ((RGB){110, 22, 10}), 1.0f, ((RGB){34, 10, 8})); break;
    case BOOM_NOVA: S5(0, ((RGB){255, 255, 255}), 0.3f, ((RGB){255, 252, 214}), 0.55f, cmix((RGB){255, 200, 90}, a, 0.3f), 0.85f, ((RGB){255, 110, 34}), 1.0f, ((RGB){80, 22, 12})); break;
    case BOOM_SIGIL: S5(0, ((RGB){255, 252, 230}), 0.3f, ((RGB){255, 226, 130}), 0.6f, ((RGB){230, 150, 40}), 0.85f, ((RGB){120, 60, 20}), 1.0f, ((RGB){30, 16, 8})); break;
    case BOOM_ECLIPSE: S4(0, ((RGB){8, 3, 18}), 0.45f, ((RGB){56, 20, 110}), 0.75f, ((RGB){255, 200, 90}), 1.0f, ((RGB){255, 250, 222})); break;
    case BOOM_TIDAL: S5(0, ((RGB){240, 255, 255}), 0.3f, ((RGB){140, 230, 255}), 0.6f, ((RGB){30, 120, 200}), 0.85f, ((RGB){10, 40, 110}), 1.0f, ((RGB){4, 14, 40})); break;
    case BOOM_CINDERSTORM: S5(0, ((RGB){255, 240, 200}), 0.3f, ((RGB){255, 150, 50}), 0.6f, ((RGB){180, 40, 16}), 0.85f, ((RGB){60, 14, 10}), 1.0f, ((RGB){16, 6, 6})); break;
    case BOOM_REWIND: S5(0, ((RGB){255, 255, 250}), 0.35f, ((RGB){255, 226, 150}), 0.65f, ((RGB){150, 100, 240}), 0.9f, ((RGB){50, 20, 110}), 1.0f, ((RGB){14, 6, 40})); break;
    case BOOM_CODE: S5(0, ((RGB){230, 255, 235}), 0.3f, ((RGB){120, 255, 150}), 0.6f, ((RGB){30, 170, 70}), 0.85f, ((RGB){10, 70, 30}), 1.0f, ((RGB){4, 24, 12})); break;
    case BOOM_BLESSING: S4(0, ((RGB){255, 255, 255}), 0.25f, ((RGB){255, 226, 120}), 0.6f, ((RGB){176, 120, 255}), 1.0f, ((RGB){30, 10, 70})); break;
    case BOOM_XENON: S5(0, ((RGB){255, 255, 255}), 0.2f, ((RGB){150, 255, 215}), 0.5f, ((RGB){150, 100, 255}), 0.8f, ((RGB){70, 30, 150}), 1.0f, ((RGB){14, 6, 40})); break;
    case BOOM_ALIEN: S5(0, ((RGB){235, 255, 240}), 0.25f, ((RGB){130, 255, 190}), 0.55f, ((RGB){80, 90, 220}), 0.85f, ((RGB){60, 20, 120}), 1.0f, ((RGB){16, 6, 36})); break;
    default: S5(0, ((RGB){255, 252, 238}), 0.28f, cmix(WHITE, a, 0.35f), 0.55f, a, 0.8f, cshade(a, 0.5f), 1.0f, cshade(a, 0.2f));
    }
    for (int i = 0; i < 16; i++) pal[i] = cgradient(ts, cs, n, i / 15.0f);
}

void draw_explosion(Surf *cv, const Explosion *e_in) {
    Explosion ee = *e_in;
    const Explosion *e = &ee;
    float p = (float)(e->age / e->duration);
    if (p >= 1) return;
    RGB pal[16];
    exp_palette(e, pal);
    float R = (float)e->radius, ex = (float)e->x, ey = (float)e->y;
    int style = e->style;
    float grow = style == BOOM_IMPLODE ? fmaxf(0.0f, (p - 0.1f) / 0.9f) : p;
    float rr = grow < 0.25f ? R * ease_out(grow / 0.25f) : R * (1 - 0.45f * (grow - 0.25f) / 0.75f);
    float alpha = p < 0.5f ? 1.0f : 1 - (p - 0.5f) / 0.5f;
    int span = (int)fmaxf(rr, R * 2.2f) + 2;
    int cx = (int)ex, cy = (int)ey;
    float ring = R * (0.4f + 1.6f * ease_out(p));
    float flash = (p < 0.12f && style != BOOM_IMPLODE) ? R * 1.9f * (1 - p / 0.12f) : 0.0f;
    float pull = (style == BOOM_IMPLODE && p < 0.2f) ? R * (2.4f - 12 * p) : 0.0f;
    bool prismy = style == BOOM_PRISM || style == BOOM_BLOOM;
    float maxr = fmaxf(fmaxf(rr, flash), fmaxf(ring + 0.75f, pull + 0.8f));
    float maxr2 = (maxr + 1) * (maxr + 1);
    /* fixed-point core (1/256 px): the ARM9 has no FPU, so the per-pixel work avoids soft-float */
    int32_t ex8 = (int32_t)(ex * 256), ey8 = (int32_t)(ey * 256);
    int32_t rr8 = (int32_t)(rr * 256), fl8 = (int32_t)(flash * 256), ring8 = (int32_t)(ring * 256), pull8 = (int32_t)(pull * 256);
    int32_t maxr8 = (int32_t)((maxr + 1) * 256);
    int64_t maxr2_8 = (int64_t)maxr8 * maxr8;
    int32_t rrd8 = rr8 > 25 ? rr8 : 25;                      /* max(rr, 0.1) */
    int32_t a8 = (int32_t)(alpha * 256), fl_a8 = (int32_t)(0.75f * 256), ring_a8 = (int32_t)((1 - p) * 0.55f * 256);
    int32_t p55 = (int32_t)(p * 0.55f * 256);
    bool ring_on = p < 0.9f && style != BOOM_IMPLODE;
    Px palx[16];
    for (int i = 0; i < 16; i++) palx[i] = rgb15(pal[i]);
    Px fl_c = rgb15i(255, 255, 250), pull_c = rgb15(cmix(e->accent, WHITE, 0.4f));
    (void)maxr2;
    for (int yy = cy - span; yy <= cy + span; yy++) {
        int py = cv->h - 1 - yy;
        if ((unsigned)py >= (unsigned)cv->h) continue;
        Px *row = cv->px + py * cv->stride;
        int32_t dy = yy * 256 + 128 - ey8;
        for (int xx = cx - span; xx <= cx + span; xx++) {
            if ((unsigned)xx >= (unsigned)cv->w) continue;
            int32_t dx = xx * 256 + 128 - ex8;
            int64_t d2 = (int64_t)dx * dx + (int64_t)dy * dy;
            if (d2 > maxr2_8) continue;
            int32_t d = (int32_t)isqrt64((uint64_t)d2);
            Px *px = &row[xx];
            if (fl8 && d <= fl8) *px = pxmix_k(*px, fl_c, (fl_a8 * (256 - (int32_t)(((int64_t)d * 128) / fl8))) >> 8);
            if (pull8 && abs(d - pull8) < 205) *px = pxmix_k(*px, pull_c, 205);
            if (d <= rr8) {
                int32_t f = (int32_t)(((int64_t)d * 256) / rrd8) + p55;
                if (f > 256) f = 256;
                Px col = palx[(f * 15) >> 8];
                if (prismy) {
                    float fdx = dx / 256.0f, fdy = dy / 256.0f;
                    col = pxmix(col, rgb15(chsv(atan2f(fdy, fdx) / TAU_F + p * 0.7f + d / 256.0f * 0.03f, 0.6f, 1.0f)), 0.55f);
                }
                *px = pxmix_k(*px, col, (a8 * (256 - (int32_t)(((int64_t)d * 64) / rrd8))) >> 8);
            }
            if (ring_on && abs(d - ring8) < 192) *px = pxmix_k(*px, palx[3], ring_a8);
        }
    }
    switch (style) {
    case BOOM_RAYS: case BOOM_STARRAYS: case BOOM_PRISM: {
        int n = style == BOOM_RAYS ? 8 : style == BOOM_STARRAYS ? 12 : 16;
        float length = R * (1.1f + (style == BOOM_RAYS ? 1.0f : 1.7f) * ease_out(p));
        for (int k = 0; k < n; k++) {
            float a = k * TAU_F / n + (style == BOOM_RAYS ? 0.3f : style == BOOM_PRISM ? p * 0.8f : 0.0f);
            float ca = fcos(a), sa = fsin(a);
            RGB col = style == BOOM_PRISM ? chsv((float)k / n + p, 0.6f, 1.0f) : cmix(WHITE, e->accent, 0.4f);
            for (int s = 1; s <= (int)length; s++) cblendf(cv, ex + ca * s, ey + sa * s, col, (1 - p) * (1 - s / length) * 0.85f);
        }
        break;
    }
    case BOOM_CLUSTER:
        for (int k = 0; k < 6; k++) {
            float a = k * TAU_F / 6 + 0.4f;
            float local = clampf((p - 0.12f - k * 0.05f) / 0.5f, 0, 1);
            if (local > 0 && local < 1) {
                float r = R * 0.45f * fsin(local * PI_F);
                float bx = ex + fcos(a) * R * 1.45f, by = ey + fsin(a) * R * 1.45f;
                for (int yy = (int)(by - r) - 1; yy < (int)(by + r) + 2; yy++)
                    for (int xx = (int)(bx - r) - 1; xx < (int)(bx + r) + 2; xx++) {
                        float d = fhypot(xx + 0.5f - bx, yy + 0.5f - by);
                        if (d <= r) cblend(cv, xx, yy, cmix(WHITE, pal[2], d / fmaxf(r, 0.1f)), (1 - local) * 0.9f);
                    }
            }
        }
        break;
    case BOOM_FIRE:
        if (p < 0.8f) {
            int cols = (int)(R * 1.2f);
            for (int dx = -cols; dx <= cols; dx++) {
                float height = R * (1.0f + 1.4f * flick((float)e->age, dx, 7)) * (1 - p / 0.8f) * (1 - abs(dx) / (float)(cols + 1));
                for (int dy = 0; dy < (int)height; dy++)
                    cblendf(cv, ex + dx, ey + dy + 1, grad3((RGB){255, 230, 140}, (RGB){255, 120, 24}, (RGB){120, 24, 8}, 0.5f, dy / fmaxf(1.0f, height)), 0.8f * (1 - p));
            }
        }
        break;
    case BOOM_RING: ringline(cv, ex, ey, R * (0.8f + 2.4f * ease_out(p)), pal[2], (1 - p) * 0.7f); break;
    case BOOM_CLOUD: {
        float r = R * (1.0f + 1.2f * ease_out(p));
        RGB col = cmix(e->accent, (RGB){60, 90, 40}, 0.4f);
        for (int yy = (int)(ey - r) - 1; yy < (int)(ey + r) + 2; yy++)
            for (int xx = (int)(ex - r) - 1; xx < (int)(ex + r) + 2; xx++)
                if (fhypot(xx + 0.5f - ex, yy + 0.5f - ey) <= r) cblend(cv, xx, yy, col, 0.22f * (1 - p));
        break;
    }
    case BOOM_SHOCK:
        for (int k = 0; k < 3; k++) {
            float q = clampf((p - k * 0.12f) / 0.7f, 0, 1);
            if (q > 0 && q < 1) ringline(cv, ex, ey, R * (0.6f + 2.8f * ease_out(q)), cmix(e->accent, WHITE, 0.5f), (1 - q) * 0.8f);
        }
        break;
    case BOOM_SHATTER:
        for (int k = 0; k < 14; k++) {
            float a = k * TAU_F / 14 + 0.2f;
            float d0 = R * (0.5f + 2.2f * ease_out(p)) * (0.7f + 0.5f * ((k * 7) % 5) / 4.0f);
            for (int s = 0; s < 3; s++) cblendf(cv, ex + fcos(a) * (d0 - s), ey + fsin(a) * (d0 - s), cmix((RGB){235, 250, 255}, e->accent, s / 3.0f), (1 - p) * (0.95f - s * 0.25f));
        }
        break;
    case BOOM_NOVA:
        if (p < 0.25f) {
            float fr = R * 2.4f * (1 - p / 0.25f);
            for (int yy = (int)(ey - fr) - 1; yy < (int)(ey + fr) + 2; yy++)
                for (int xx = (int)(ex - fr) - 1; xx < (int)(ex + fr) + 2; xx++) {
                    float d = fhypot(xx + 0.5f - ex, yy + 0.5f - ey);
                    if (d <= fr) cblend(cv, xx, yy, (RGB){255, 255, 245}, 0.5f * (1 - d / fr));
                }
        }
        ringline(cv, ex, ey, R * (0.7f + 2.2f * ease_out(p)), (RGB){255, 244, 200}, (1 - p) * 0.8f);
        ringline(cv, ex, ey, R * (0.4f + 3.4f * ease_out(p)), e->accent, (1 - p) * 0.5f);
        for (int k = 0; k < 12; k++) {
            float a = k * TAU_F / 12 + p * 0.5f;
            for (int s = 1; s <= (int)(R * (1.4f + 2.2f * ease_out(p))); s++)
                cblendf(cv, ex + fcos(a) * s, ey + fsin(a) * s, (RGB){255, 240, 190}, (1 - p) * 0.7f * (1 - s / (R * 3.6f)));
        }
        break;
    case BOOM_MAGMA:
        for (int k = 0; k < 9; k++) {
            float a = 0.35f + k * (PI_F - 0.7f) / 8;
            float v = R * (2.2f + 0.5f * ((k * 5) % 3)), tau = p * 1.6f;
            float gx = ex + fcos(a) * v * tau * 0.9f, gy = ey + fsin(a) * v * tau - 4.5f * R * tau * tau;
            if (p < 0.85f) {
                RGB c = {255, (uint8_t)(170 - 60 * p), 50};
                cblendf(cv, gx, gy, c, 0.9f * (1 - p)); cblendf(cv, gx + 1, gy, c, 0.9f * (1 - p));
                cblendf(cv, gx, gy + 1, c, 0.9f * (1 - p)); cblendf(cv, gx + 1, gy + 1, c, 0.9f * (1 - p));
            }
        }
        if (p < 0.9f)
            for (int dx = -(int)(R * 1.3f); dx <= (int)(R * 1.3f); dx++)
                cblendf(cv, ex + dx, ey - 1, (RGB){255, 120, 30}, 0.45f * (1 - p) * (1 - abs(dx) / (R * 1.4f)));
        break;
    case BOOM_THUNDER:
        if (p <= 0.75f) {
            int step = (int)(e->age * 22);
            for (int k = 0; k < 7; k++) {
                float a = k * TAU_F / 7 + 0.3f, x = ex, y = ey;
                for (int s = 0; s < (int)(R * 1.8f); s++) {
                    float a2 = a + (flick(step * 0.033f, k, s) - 0.5f) * 1.1f;
                    x += fcos(a2) * 1.6f;
                    y += fsin(a2) * 1.6f;
                    cblendf(cv, x, y, (RGB){235, 245, 255}, (1 - p / 0.75f) * 0.95f);
                    if (s % 3 == 0) cblendf(cv, x + 1, y, e->accent, (1 - p / 0.75f) * 0.4f);
                }
            }
        }
        break;
    case BOOM_CODE:
        for (int k = 0; k < 10; k++) {
            float a = k * TAU_F / 10 + 0.4f, d = R * (0.4f + 2.4f * ease_out(p));
            draw_glyph(cv, ex + fcos(a) * d, ey + fsin(a) * d * 0.9f, (int)(e->age * 10) + k, (RGB){110, 255, 150}, (1 - p) * 0.9f);
        }
        break;
    case BOOM_BLOOM: {
        float reach = R * (1.0f + 2.6f * ease_out(p));
        for (int k = 0; k < 8; k++) {
            float a = k * TAU_F / 8 + p * 0.7f, ca = fcos(a), sa = fsin(a);
            RGB col = chsv(k / 8.0f + p * 0.6f, 0.55f, 1.0f);
            for (int s = 1; s <= (int)reach; s++) {
                float w = fsin(s / reach * PI_F) * (1.2f + 2.2f * p);
                cblendf(cv, ex + ca * s - sa * w, ey + sa * s + ca * w, col, (1 - p) * 0.85f);
                cblendf(cv, ex + ca * s + sa * w, ey + sa * s - ca * w, col, (1 - p) * 0.85f);
            }
        }
        ringline(cv, ex, ey, R * (0.5f + 2.0f * ease_out(p)), WHITE, (1 - p) * 0.5f);
        break;
    }
    case BOOM_SIGIL: {
        RGB gold = {255, 226, 130};
        float r0 = R * (0.9f + 1.7f * ease_out(p));
        ringline(cv, ex, ey, r0, gold, (1 - p) * 0.9f);
        ringline(cv, ex, ey, r0 * 0.72f, WHITE, (1 - p) * 0.5f);
        for (int k = 0; k < 6; k++) {
            float a = k * TAU_F / 6 + p * 2.4f;
            for (int j = 0; j < (int)r0; j++) cblendf(cv, ex + fcos(a) * j, ey + fsin(a) * j, gold, (1 - p) * 0.7f);
        }
        break;
    }
    case BOOM_ECLIPSE:
        for (int k = 0; k < 16; k++) {
            float a = k * TAU_F / 16 + p * 0.8f;
            float ln = R * (1.0f + 2.6f * ease_out(p)) * (0.6f + 0.4f * fsin(k * 3.1f));
            for (int j = 0; j < (int)ln; j++)
                cblendf(cv, ex + fcos(a) * (R * 0.6f + j), ey + fsin(a) * (R * 0.6f + j), (RGB){255, 226, 130}, (1 - p) * 0.8f * (1 - j / fmaxf(1.0f, ln)));
        }
        ringline(cv, ex, ey, R * (0.7f + 2.0f * ease_out(p)), (RGB){170, 110, 255}, (1 - p) * 0.7f);
        break;
    case BOOM_TIDAL:
        for (int k = 0; k < 4; k++) {
            float q = clampf((p - k * 0.1f) / 0.7f, 0, 1);
            if (q > 0 && q < 1) ringline(cv, ex, ey, R * (0.5f + 3.2f * ease_out(q)), cmix((RGB){120, 220, 255}, WHITE, 0.3f * k / 3), (1 - q) * 0.85f);
        }
        break;
    case BOOM_CINDERSTORM:
        for (int k = 0; k < 18; k++) {
            float a = k * TAU_F / 18 + 0.3f;
            float s = R * (0.6f + 2.8f * ease_out(p)) * (0.7f + 0.3f * fsin(k * 2.3f));
            cblendf(cv, ex + fcos(a) * s, ey + fsin(a) * s - 5 * p * p * R * 0.4f, (RGB){255, 190, 90}, (1 - p) * 0.9f);
        }
        ringline(cv, ex, ey, R * (0.5f + 1.8f * ease_out(p)), (RGB){255, 120, 40}, (1 - p) * 0.6f);
        break;
    case BOOM_REWIND:
        for (int k = 0; k < 3; k++) {
            float q = clampf(1 - (p - k * 0.1f) / 0.8f, 0, 1);
            if (q > 0 && q < 1) ringline(cv, ex, ey, R * (0.4f + 2.6f * q), cmix((RGB){255, 226, 150}, (RGB){170, 110, 255}, k / 2.0f), fminf(1.0f, 1.6f * q) * 0.8f);
        }
        for (int k = 0; k < 8; k++) {
            float a = k * TAU_F / 8 - p * 3, ln = R * 2.4f * (1 - p);
            for (int j = 0; j < (int)ln; j++) cblendf(cv, ex + fcos(a) * (ln - j), ey + fsin(a) * (ln - j), WHITE, (1 - p) * 0.5f);
        }
        break;
    case BOOM_BLESSING: {
        float q = ease_out(clampf(p / 0.6f, 0, 1));
        if (p < 0.3f)
            for (int dx = -(int)R; dx <= (int)R; dx++)
                for (int dy = -(int)R; dy <= (int)R; dy++)
                    if (fhypot((float)dx, (float)dy) <= R * (0.4f + 1.6f * p)) cblendf(cv, ex + dx, ey + dy, (RGB){255, 252, 235}, (1 - p / 0.3f) * 0.95f);
        for (int k = 0; k < 8; k++) {
            float a = k * PI_F / 4 + p * 1.2f, ln = R * (k % 2 == 0 ? 3.4f : 1.9f) * q;
            for (int s = 1; s <= (int)ln; s++)
                cblendf(cv, ex + fcos(a) * s, ey + fsin(a) * s, k % 2 == 0 ? (RGB){255, 226, 120} : WHITE, (1 - p) * (0.95f - 0.4f * s / fmaxf(1, ln)));
        }
        static const RGB RC[3] = {{255, 214, 90}, {255, 255, 255}, {176, 120, 255}};
        for (int k = 0; k < 3; k++) {
            float qq = clampf((p - k * 0.1f) / 0.7f, 0, 1);
            if (qq > 0 && qq < 1) ringline(cv, ex, ey, R * (0.5f + 3.0f * ease_out(qq)), RC[k], (1 - qq) * 0.9f);
        }
        for (int k = 0; k < 10; k++) {
            float a = -p * 5 + k * TAU_F / 10, r2 = R * (1.2f + 2.2f * q);
            cblendf(cv, ex + fcos(a) * r2, ey + fsin(a) * r2, k % 2 ? (RGB){255, 240, 170} : (RGB){176, 120, 255}, (1 - p) * 0.9f);
        }
        break;
    }
    case BOOM_XENON:
        if (p < 0.25f)
            for (int dx = -(int)R; dx <= (int)R; dx++)
                for (int dy = -(int)R; dy <= (int)R; dy++) {
                    float d = fhypot((float)dx, (float)dy);
                    if (d <= R * (0.5f + 2 * p)) cblendf(cv, ex + dx, ey + dy, cmix(WHITE, (RGB){150, 255, 215}, d / (R * 1.5f)), (1 - p / 0.25f) * 0.9f);
                }
        for (int k = 0; k < 5; k++) {
            float q = clampf((p - k * 0.1f) / 0.65f, 0, 1);
            if (q > 0 && q < 1) ringline(cv, ex, ey, R * (0.4f + 3.4f * ease_out(q)), cmix((RGB){120, 255, 190}, (RGB){190, 120, 255}, k / 4.0f), (1 - q) * 0.9f);
        }
        if (p < 0.8f) {
            float q = p / 0.8f;
            for (int k = 0; k < 12; k++) {
                float a = k * TAU_F / 12 + p * 3, ln = R * (0.5f + 3.2f * ease_out(q)) * (k % 2 ? 1.0f : 0.6f);
                for (int s = 2; s <= (int)ln; s += 2)
                    cblendf(cv, ex + fcos(a) * s, ey + fsin(a) * s, s > ln - 3 ? (RGB){230, 255, 240} : (RGB){130, 255, 200}, (1 - q) * 0.85f);
            }
            for (int k = 0; k < 6; k++) {
                float a = k * PI_F / 3 - p * 2;
                for (int s = 1; s < (int)(R * 0.9f); s++) cblendf(cv, ex + fcos(a) * s, ey + fsin(a) * s, WHITE, (1 - q) * 0.9f);
            }
        }
        break;
    case BOOM_ALIEN:
        for (int k = 0; k < 3; k++) {
            float q = clampf((p - k * 0.14f) / 0.7f, 0, 1);
            if (q > 0 && q < 1) ringline(cv, ex, ey, R * (0.5f + 3.0f * ease_out(q)), cmix((RGB){120, 255, 190}, (RGB){190, 120, 255}, k / 2.0f), (1 - q) * 0.85f);
        }
        break;
    default: break;
    }
}

/* ------------------------------------------------------------------ scenery (l.1287-1672) */
static void bput(Scenery *sc, int x, int y, Px c) {
    if (x >= 0 && x < sc->width && y >= 0 && y < sc->height) sc->sky[sc->height - 1 - y][x] = c;
}
static void b_rect(Scenery *sc, int x0, int y0, int w, int h, Px c) {
    for (int x = x0; x < x0 + w; x++) for (int y = y0; y < y0 + h; y++) bput(sc, x, y, c);
}
static void b_tri(Scenery *sc, int cx, int y0, int half, int h, Px c) {
    for (int k = 0; k < h; k++) {
        int hw = (int)(half * (1 - (double)k / h));
        for (int x = cx - hw; x <= cx + hw; x++) bput(sc, x, y0 + k, c);
    }
}
static void b_dome(Scenery *sc, int cx, int y0, int rx, int ry, Px c) {
    for (int x = cx - rx; x <= cx + rx; x++)
        for (int y = y0; y < y0 + ry; y++) {
            double a = (double)(x - cx) / rx, b = (double)(y - y0) / ry;
            if (a * a + b * b <= 1) bput(sc, x, y, c);
        }
}
static void b_windows(Scenery *sc, int x0, int y0, int w, int h, Px lit, int seed) {
    for (int x = x0 + 1; x < x0 + w - 1; x += 2)
        for (int y = y0 + 2; y < y0 + h - 1; y += 3)
            if (((x * 7 + y * 13 + seed) % 5 + 5) % 5 < 3) bput(sc, x, y, lit);
}

static void bake_buildings(Scenery *sc, int kind, Px col, Px lit) {
    int w = sc->width, h = sc->height, base_y = (int)(h * 0.2);
    const char *name = BUILDING_NAMES[kind];
    uint32_t sum = 0;
    for (const char *p = name; *p; p++) sum += (unsigned char)*p;
    PyRandom r;
    rng_seed(&r, (uint64_t)(w * 31 + h * 7 + sum));
    switch (kind) {
    case BLD_SKYLINE: {
        int x = 2;
        while (x < w - 4) {
            int bw = rng_randint(&r, 5, 9), bh = (int)(h * rng_uniform(&r, 0.22, 0.62));
            b_rect(sc, x, base_y, bw, bh, col);
            b_windows(sc, x, base_y, bw, bh, lit, x);
            if (rng_random(&r) < 0.4) b_rect(sc, x + bw / 2, base_y + bh, 1, rng_randint(&r, 3, 7), col);
            x += bw + rng_randint(&r, 0, 2);
        }
        break;
    }
    case BLD_CLOCK: {
        int cx = w / 2;
        b_rect(sc, cx - 6, base_y, 12, (int)(h * 0.55), col);
        b_tri(sc, cx, base_y + (int)(h * 0.55), 8, (int)(h * 0.22), col);
        RGB l = px_rgb(lit);
        Px half = rgb15(cshade(l, 0.5f));
        for (int dx = -4; dx <= 4; dx++)
            for (int dy = -4; dy <= 4; dy++)
                if (dx * dx + dy * dy <= 16) bput(sc, cx + dx, base_y + (int)(h * 0.42) + dy, dx * dx + dy * dy > 6 ? lit : half);
        int sxs[2] = {w / 6, w - w / 6};
        for (int i = 0; i < 2; i++) { b_rect(sc, sxs[i] - 5, base_y, 10, (int)(h * 0.3), col); b_windows(sc, sxs[i] - 5, base_y, 10, (int)(h * 0.3), lit, sxs[i]); }
        break;
    }
    case BLD_CATHEDRAL: {
        int cx = w / 2;
        b_rect(sc, cx - 14, base_y, 28, (int)(h * 0.3), col);
        b_tri(sc, cx, base_y + (int)(h * 0.3), 14, (int)(h * 0.14), col);
        int sxs[2] = {cx - 18, cx + 18};
        for (int i = 0; i < 2; i++) { b_rect(sc, sxs[i] - 3, base_y, 6, (int)(h * 0.55), col); b_tri(sc, sxs[i], base_y + (int)(h * 0.55), 4, (int)(h * 0.2), col); }
        b_dome(sc, cx, base_y + 3, 6, (int)(h * 0.2), lit);
        break;
    }
    case BLD_TEMPLE: {
        int cx = w / 2;
        for (int i = 0; i < 6; i++) b_rect(sc, cx - 24 + i * 4, base_y + i * 4, 48 - i * 8, 4, col);
        b_rect(sc, cx - 3, base_y + 24, 6, 5, lit);
        int sxs[2] = {w / 7, w - w / 7};
        for (int i = 0; i < 2; i++) for (int k = 0; k < 4; k++) b_rect(sc, sxs[i] + k * 4 - 8, base_y, 2, (int)(h * 0.3), col);
        break;
    }
    case BLD_RUINS: {
        int x = 4;
        while (x < w - 6) {
            int bh = (int)(h * rng_uniform(&r, 0.1, 0.45));
            b_rect(sc, x, base_y, 3, bh, col);
            if (rng_random(&r) < 0.5) b_rect(sc, x, base_y + bh, 9, 2, col);
            x += rng_randint(&r, 8, 14);
        }
        break;
    }
    case BLD_PYRAMIDS: {
        int cxs[3] = {w / 5, w / 2, w - w / 4};
        double phs[3] = {0.45, 0.6, 0.35};
        for (int i = 0; i < 3; i++) { b_tri(sc, cxs[i], base_y, (int)(h * phs[i] * 0.9), (int)(h * phs[i]), col); bput(sc, cxs[i], base_y + (int)(h * phs[i]), lit); }
        break;
    }
    case BLD_FACTORY: {
        int x = 3;
        while (x < w - 8) {
            int bw = rng_randint(&r, 10, 16);
            b_rect(sc, x, base_y, bw, (int)(h * 0.2), col);
            for (int sx = x; sx < x + bw - 3; sx += 5) b_tri(sc, sx + 2, base_y + (int)(h * 0.2), 2, 4, col);
            if (rng_random(&r) < 0.7) b_rect(sc, x + bw - 3, base_y, 3, (int)(h * rng_uniform(&r, 0.35, 0.6)), col);
            b_windows(sc, x, base_y, bw, (int)(h * 0.2), lit, x);
            x += bw + 2;
        }
        break;
    }
    case BLD_CASTLE: {
        b_rect(sc, 2, base_y, w - 4, (int)(h * 0.22), col);
        for (int x = 2; x < w - 4; x += 4) b_rect(sc, x, base_y + (int)(h * 0.22), 2, 3, col);
        int sxs[4] = {w / 8, w / 3, w - w / 3, w - w / 8};
        for (int i = 0; i < 4; i++) {
            b_rect(sc, sxs[i] - 4, base_y, 8, (int)(h * 0.5), col);
            b_tri(sc, sxs[i], base_y + (int)(h * 0.5), 5, (int)(h * 0.16), col);
            b_windows(sc, sxs[i] - 4, base_y, 8, (int)(h * 0.5), lit, sxs[i]);
        }
        break;
    }
    case BLD_STATION: {
        int cx = w / 2;
        b_dome(sc, cx, base_y, 16, (int)(h * 0.3), col);
        b_rect(sc, cx - 30, base_y + (int)(h * 0.12), 60, 3, col);
        int sxs[2] = {cx - 30, cx + 30};
        for (int i = 0; i < 2; i++) { b_rect(sc, sxs[i] - 1, base_y, 3, (int)(h * 0.4), col); bput(sc, sxs[i], base_y + (int)(h * 0.4), lit); }
        b_windows(sc, cx - 14, base_y, 28, (int)(h * 0.2), lit, 3);
        break;
    }
    case BLD_VOLCANO: {
        int cxs[2] = {w / 4, w - w / 3};
        double phs[2] = {0.58, 0.44}, hws[2] = {0.30, 0.24};
        for (int i = 0; i < 2; i++) {
            b_tri(sc, cxs[i], base_y, (int)(h * hws[i] * 2.6), (int)(h * phs[i]), col);
            int top = base_y + (int)(h * phs[i]);
            for (int dx = -3; dx <= 3; dx++) bput(sc, cxs[i] + dx, top, lit);
            for (int k = 0; k < 7; k++) bput(sc, cxs[i] + (k % 2) * 2 - 1, top - 2 - k * 3, lit);
        }
        break;
    }
    case BLD_ISLES: {
        int cxs[5] = {w / 6, w / 2, w - w / 5, w / 3, w - w / 3};
        double cys[5] = {0.55, 0.7, 0.5, 0.38, 0.36};
        int rws[5] = {12, 18, 13, 8, 9};
        for (int i = 0; i < 5; i++) {
            int y0 = base_y + (int)(h * cys[i]), rw = rws[i], cx = cxs[i];
            b_rect(sc, cx - rw, y0, 2 * rw, 2, col);
            for (int k = 1; k < rw; k++) b_rect(sc, cx - rw + k, y0 - k / 2 - 1, 2 * (rw - k), 1, col);
            b_rect(sc, cx - 1, y0 + 2, 3, 6 + rw / 3, col);
            bput(sc, cx, y0 + 7 + rw / 3, lit);
        }
        break;
    }
    case BLD_RIFT: {
        int cx = w / 2, cy = base_y + (int)(h * 0.5), R = (int)(h * 0.34) > 6 ? (int)(h * 0.34) : 6;
        for (int a = 0; a < 720; a++) {
            double ang = a * 3.141592653589793 / 360;
            for (int rr = R - 1; rr <= R + 1; rr++) bput(sc, (int)(cx + cos(ang) * rr), (int)(cy + sin(ang) * rr), rr == R ? lit : col);
        }
        int sxs[2] = {w / 7, w - w / 7};
        for (int i = 0; i < 2; i++) {
            b_rect(sc, sxs[i] - 3, base_y, 3, (int)(h * 0.5), col);
            b_rect(sc, sxs[i] + 4, base_y, 3, (int)(h * 0.35), col);
            b_rect(sc, sxs[i] - 3, base_y + (int)(h * 0.5), 10, 2, col);
        }
        for (int k = 0; k < 9; k++) {
            int px = rng_randint(&r, 4, w - 5 > 5 ? w - 5 : 5);
            int py = base_y + (int)(h * rng_uniform(&r, 0.3, 0.8));
            b_tri(sc, px, py, 2, rng_randint(&r, 3, 6), col);
        }
        break;
    }
    case BLD_PAGODA: {
        int cxs[3] = {w / 4, w / 2, w - w / 4};
        double scs[3] = {0.8, 1.0, 0.8};
        for (int i = 0; i < 3; i++) {
            for (int tier = 0; tier < 4; tier++) {
                int tw = (int)((14 - tier * 3) * scs[i]);
                b_rect(sc, cxs[i] - tw / 2, base_y + tier * (int)(h * 0.1), tw, (int)(h * 0.07), col);
                b_rect(sc, cxs[i] - tw / 2 - 2, base_y + tier * (int)(h * 0.1) + (int)(h * 0.07), tw + 4, 2, col);
            }
            b_rect(sc, cxs[i], base_y + 4 * (int)(h * 0.1) + 2, 1, 5, col);
        }
        break;
    }
    }
}

/* integer colour mixes for the per-pixel scenery bakes (soft-float cmix costs ~10 us a pixel) */
static inline RGB cmix_k(RGB a, RGB b, int k) {   /* k in 0..256 */
    RGB r = {(uint8_t)(a.r + (((b.r - a.r) * k) >> 8)), (uint8_t)(a.g + (((b.g - a.g) * k) >> 8)), (uint8_t)(a.b + (((b.b - a.b) * k) >> 8))};
    return r;
}
static inline RGB cshade_k(RGB c, int k) {        /* k in 1/1024 units */
    int r = (c.r * k) >> 10, g = (c.g * k) >> 10, b = (c.b * k) >> 10;
    RGB o = {(uint8_t)(r > 255 ? 255 : r), (uint8_t)(g > 255 ? 255 : g), (uint8_t)(b > 255 ? 255 : b)};
    return o;
}

void scenery_build(Scenery *sc, const World *w) {
    const MapTheme *th = &THEMES[MAPDEFS[w->map].theme];
    int W = w->width, H = w->height;
    sc->width = W;
    sc->height = H;
    sc->theme = MAPDEFS[w->map].theme;
    for (int b = 0; b < 3; b++) { sc->aur_key[b] = -1; sc->naur[b] = 0; }
    for (int i = 0; i < w->nstars; i++) {
        sc->starq[i].spq = (int16_t)(w->stars[i].sp * 16);
        sc->starq[i].phq = (int16_t)(w->stars[i].ph * 163);
        float b8 = w->stars[i].b * 256;
        sc->starq[i].b8 = (uint8_t)(b8 > 255 ? 255 : b8 < 0 ? 0 : b8);
    }
    sc->bolt_key = -1;
    sc->nbolt = 0;
    static RGB rows[FIELD_H][FIELD_W];    /* static: 30 KB would not fit the DTCM stack */
    for (int py = 0; py < H; py++) {
        RGB c = cmix(th->sky_top, th->sky_bottom, powf((float)py / (H - 1 > 1 ? H - 1 : 1), 1.4f));
        for (int x = 0; x < W; x++) rows[py][x] = c;
    }
    int ox = w->orb_x, oy = (int)(H * 0.7), r = (int)(H * 0.09) > 3 ? (int)(H * 0.09) : 3;
    for (int py = (H - 1 - oy - 3 * r > 0 ? H - 1 - oy - 3 * r : 0); py < (H - 1 - oy + 3 * r < H ? H - 1 - oy + 3 * r : H); py++)
        for (int x = (ox - 3 * r > 0 ? ox - 3 * r : 0); x < (ox + 3 * r < W ? ox + 3 * r : W); x++) {
            int dx = x - ox, dy = (H - 1 - py) - oy, d2 = dx * dx + dy * dy;
            if (d2 <= r * r) rows[py][x] = cmix_k(rows[py][x], th->orb, 243);
            else if (d2 < 9 * r * r) {
                /* 0.35 * k^2 with k = 1 - (d - r) / 2r, d in 1/256 px via an integer sqrt */
                int d = (int)isqrt64((uint64_t)d2 << 16);
                int k = 256 - ((d - (r << 8)) / (2 * r));
                rows[py][x] = cmix_k(rows[py][x], th->orb, (k * k * 90) >> 16);
            }
        }
    static const float LB[2] = {0.36f, 0.24f}, LK[2] = {0.55f, 0.75f}, LD[2] = {1.0f, 0.7f};
    for (int layer = 0; layer < 2; layer++) {
        RGB far = cshade(th->far, LD[layer]);
        int lk = (int)(LK[layer] * 256);
        for (int x = 0; x < W; x++) {
            float u = (float)x / W;
            float ridge = H * (LB[layer] + 0.08f * fsin(u * TAU_F * 3 + (float)w->ridge_ph[layer]) + 0.05f * fsin(u * TAU_F * 8 + (float)w->ridge_ph[layer + 2]) +
                               0.02f * fsin(u * TAU_F * 21 + (float)w->ridge_ph[layer + 4]));
            for (int py = (H - 1 - (int)ridge > 0 ? H - 1 - (int)ridge : 0); py < H; py++) rows[py][x] = cmix_k(rows[py][x], far, lk);
        }
    }
    for (int py = 0; py < H; py++) for (int x = 0; x < W; x++) sc->sky[py][x] = rgb15(rows[py][x]);
    if (th->buildings) {
        Px col = rgb15(cmix(th->sky_bottom, BLACK, 0.62f));
        Px lit = rgb15(cmix((RGB){255, 220, 140}, (RGB){255, 230, 150}, 0.5f));
        bake_buildings(sc, th->buildings, col, lit);
    }
    sc->terrain_version = -1;
    scenery_terrain(sc, w);
}

void scenery_terrain(Scenery *sc, const World *w) {
    const MapTheme *th = &THEMES[sc->theme];
    int H = sc->height;
    bool full = sc->terrain_version < 0;
    RGB dcol[FIELD_H];   /* soil colour by depth below the surface */
    for (int depth = 0; depth < H; depth++) {
        if (depth == 0) dcol[depth] = th->surface;
        else if (depth < 3) dcol[depth] = cmix(th->topsoil, th->soil, depth / 3.0f);
        else dcol[depth] = cmix(th->soil, th->deep, fminf(1.0f, (depth - 3) / (H * 0.5f)));
    }
    for (int x = 0; x < sc->width; x++) {
        int top = w->terrain.heights[x];
        float scorch = (float)w->terrain.scorch[x];
        /* only re-shade the columns a blast actually changed (a full pass costs ~80 ms on the ARM9) */
        if (!full && sc->col_h[x] == top && sc->col_scorch[x] == scorch) continue;
        sc->col_h[x] = top;
        sc->col_scorch[x] = scorch;
        RGB sc4[4];
        if (scorch > 0.02f) for (int d = 0; d < 4; d++) sc4[d] = cshade(dcol[d], 1 - 0.6f * scorch);
        for (int py = 0; py < H; py++) {
            int y = H - 1 - py;
            if (y >= top) { sc->rows[py][x] = sc->sky[py][x]; continue; }
            int depth = top - 1 - y;
            RGB c = depth < 4 && scorch > 0.02f ? sc4[depth] : dcol[depth];
            uint32_t n = (((uint32_t)x * 374761393u + (uint32_t)y * 668265263u) ^ ((uint32_t)x * (uint32_t)y * 2246822519u + 1013904223u)) & 0xFF;
            sc->rows[py][x] = rgb15(cshade_k(c, 963 + (int)(n * 123 / 255)));
        }
    }
    sc->terrain_version = w->terrain.version;
}

static int16_t g_sinq[1024];
static void sinq_init(void);
#define SINQ(a) g_sinq[(a) & 1023]
#define COSQ(a) g_sinq[((a) + 256) & 1023]
/* one aurora band, integer maths (angles in 1/1024 turn, Q14 sines); same curves as the original's floats */
static void build_aurora(Scenery *sc, int b, float c) {
    sinq_init();
    static Px hue_px[24];
    static bool hue_ok;
    if (!hue_ok) { for (int i = 0; i < 24; i++) hue_px[i] = rgb15(chsv(i / 24.0f, 0.65f, 1.0f)); hue_ok = true; }
    int h = sc->height, w = sc->width;
    int cq = (int)(c * 163.0f);                       /* c radians in 1/1024 turn */
    int bq = b * 163;
    int o1 = cq * 6 / 10 + bq * 21 / 10, o2 = -cq * 9 / 10 + bq, o3 = cq + bq, o4 = cq / 4 + bq * 13 / 10;
    int base8 = h * (169 + 20 * b);                    /* h (0.66 + 0.08 b) in 1/256 px */
    sc->naur[b] = 0;
    for (int x = 0; x < w; x++) {
        int s1 = SINQ(((x * 730) >> 6) + o1), s2 = SINQ(((x * 1773) >> 6) + o2);   /* x*0.07, x*0.17 rad */
        int s3 = SINQ(((x * 939) >> 6) + o3), s4 = SINQ(((x * 313) >> 6) + o4);    /* x*0.09, x*0.03 rad */
        int mid = (base8 + ((h * 13 * s1) >> 14) + ((h * 8 * s2) >> 14)) >> 8;
        int th8 = 768 + ((512 * s3) >> 14);            /* 3 + 2 sin */
        int hue = (2826 + ((1352 * s4) >> 14) + 128) >> 8;
        Px col = hue_px[((hue % 24) + 24) % 24];
        int th = th8 >> 8, thp = th8 + 256;
        for (int dy = -th; dy <= th; dy++) {
            int py = h - 1 - (mid + dy);
            if (py < 0 || py >= h) continue;
            int ady = dy < 0 ? -dy : dy;
            int k8 = 256 - (ady * 65536) / thp;                   /* 1 - |dy| / (thick + 1) */
            int a8 = (82 * k8 * (179 + ((77 * k8) >> 8))) >> 16;  /* 0.32 k^1.3 ~ 0.32 k (0.7 + 0.3 k) */
            if (a8 > 5 && sc->naur[b] < FIELD_W * 12) {
                sc->aur[b][sc->naur[b]].py = (uint8_t)py;
                sc->aur[b][sc->naur[b]].x = (uint8_t)x;
                sc->aur[b][sc->naur[b]].c = pxmix_k(sc->sky[py][x], col, a8);
                sc->naur[b]++;
            }
        }
    }
}

static void sky_blend(Scenery *sc, Surf *cv, float x, float y, RGB col, float a) {
    int xi = (int)x, py = sc->height - 1 - (int)y;
    if (xi >= 0 && xi < sc->width && py >= 0 && py < sc->height && cv->px[py * cv->stride + xi] == sc->sky[py][xi])
        cv->px[py * cv->stride + xi] = pxmix(sc->sky[py][xi], rgb15(col), a);
}

static void make_bolt(Scenery *sc, const World *w, int k) {
    PyRandom r;
    rng_seed(&r, (uint64_t)(k * 7919 + sc->width));
    sc->nbolt = 0;
    double x = rng_uniform(&r, 0.12, 0.88) * sc->width, y = sc->height - 1;
    /* iterative version of the recursive run(): one branch level */
    double stack[8][3];
    int sp = 0;
    int stop = w->terrain.heights[(int)py_clamp(x, 0, sc->width - 1)] + 1;
    stack[sp][0] = x; stack[sp][1] = y; stack[sp][2] = stop; sp++;
    bool branch_phase = false;
    while (sp > 0) {
        sp--;
        double px = stack[sp][0], pyy = stack[sp][1], limit = stack[sp][2];
        bool branch = branch_phase;
        while (pyy > limit && sc->nbolt < 64) {
            double nx = px + rng_uniform(&r, -4.5, 4.5), ny = pyy - rng_uniform(&r, 2.0, 5.0);
            sc->bolt[sc->nbolt][0] = (float)px; sc->bolt[sc->nbolt][1] = (float)pyy; sc->bolt[sc->nbolt][2] = (float)nx; sc->bolt[sc->nbolt][3] = (float)ny;
            sc->nbolt++;
            if (!branch && rng_random(&r) < 0.16 && sp < 8) {
                double lim2 = ny - rng_uniform(&r, 8, 18);
                stack[sp][0] = nx; stack[sp][1] = ny; stack[sp][2] = limit > lim2 ? limit : lim2; sp++;
            }
            px = nx;
            pyy = ny;
        }
        branch_phase = true;
    }
}

/* ---- the Astral Rift's ring (FX_RIFT): integer version. The original evaluates ~2000 sin/cos/blends per frame,
 * ~300 ms on the ARM9; here angles are 1/1024 turns, sines come from a Q14 table, blends are 8-bit. Same shapes. */
static void sinq_init(void) {
    if (g_sinq[256]) return;
    for (int i = 0; i < 1024; i++) g_sinq[i] = (int16_t)lrintf(sinf(i * (6.2831853f / 1024)) * 16384);
}
static inline void sky_blend_k(Scenery *sc, Surf *cv, int xi, int yi, Px col, int k) {
    int py = sc->height - 1 - yi;
    if ((unsigned)xi < (unsigned)sc->width && (unsigned)py < (unsigned)sc->height) {
        Px *p = &cv->px[py * cv->stride + xi];
        if (*p == sc->sky[py][xi]) *p = pxmix_k(*p, col, k);
    }
}
static void rift_draw(Scenery *sc, Surf *cv, float t) {
    sinq_init();
    int W = sc->width, H = sc->height;
    /* positions in 1/256 px */
    int cx = W * 128, cy = (int)(H * 0.62f * 256), R = (int)(H * 0.36f * 256);
    int tq = (int)(t * (1024 / 6.2831853f));            /* t in 1/1024-turn units per radian of t */
    Px c_light = rgb15i(255, 240, 190), c_dark = rgb15i(150, 90, 255);
    int rads[3] = {R, R * 78 / 100, R * 12 / 10};
    int spd[3] = {35, -55, 20};                         /* rad/s x100 */
    for (int j = 0; j < 3; j++) {
        int n = (int)(6.2831853f * (rads[j] / 256.0f) * 1.6f);
        int rot = tq * spd[j] / 100;
        for (int i = 0; i < n; i++) {
            int a = i * 1024 / n + rot;
            int x = (cx + ((rads[j] * COSQ(a)) >> 14)) >> 8, y = (cy + ((rads[j] * SINQ(a)) >> 14)) >> 8;
            int k = 141 + ((90 * SINQ(a * 6 + tq * 3)) >> 14);  /* 0.55 + 0.35 sin(6a + 3t) */
            sky_blend_k(sc, cv, x, y, SINQ(a) > 0 ? c_light : c_dark, k);
        }
    }
    Px r_light = rgb15i(255, 236, 190), r_dark = rgb15i(140, 90, 230);
    int j0 = (int)(R * 0.3f / 256), j1 = (int)(R * 1.6f / 256), R17 = (int)(R * 1.7f / 256);
    for (int kk = 0; kk < 12; kk++) {
        int a = kk * 1024 / 12 + tq * 15 / 100;
        int ca = COSQ(a), sa = SINQ(a);
        for (int j = j0; j < j1; j++) {
            int x = (cx + ((j * 256 * ca) >> 14)) >> 8, y = (cy + ((j * 256 * sa) >> 14)) >> 8;
            sky_blend_k(sc, cv, x, y, kk % 2 ? r_light : r_dark, 72 * (R17 - j) / R17);   /* 0.28 (1 - j / 1.7R) */
        }
    }
    Px arm = rgb15i(255, 250, 230);
    for (int am = 0; am < 3; am++)
        for (int k = 0; k < 44; k++) {
            int rr = k * R * 95 / (44 * 100);
            int a = am * 1024 / 3 + (k * 17 * 1024) / 628 + tq * 110 / 100;   /* k * 0.17 rad */
            int x = (cx + ((rr * COSQ(a)) >> 14)) >> 8, y = (cy + ((rr * SINQ(a)) >> 14)) >> 8;
            sky_blend_k(sc, cv, x, y, arm, 205 * (50 - k) / 50);                       /* 0.8 (1 - k/50) */
        }
    /* the dark core and its rim */
    float r0 = R / 256.0f * 0.3f;
    float fcx = cx / 256.0f, fcy = cy / 256.0f;
    int rim_k = 154 + (int)(77 * fsin(t * 5));
    Px core = rgb15i(6, 2, 16), rim = rgb15i(255, 214, 120);
    for (int yy = (int)(fcy - r0); yy <= (int)(fcy + r0); yy++)
        for (int xx = (int)(fcx - r0); xx <= (int)(fcx + r0); xx++) {
            float dx = xx - fcx, dy = yy - fcy, d2 = dx * dx + dy * dy;
            if (d2 <= r0 * r0) sky_blend_k(sc, cv, xx, yy, core, 205);
            else if (d2 <= (r0 + 1.2f) * (r0 + 1.2f)) sky_blend_k(sc, cv, xx, yy, rim, rim_k);
        }
}

void scenery_draw(Scenery *sc, const World *w, Surf *cv, float t, bool flashes) {
    const MapTheme *th = &THEMES[sc->theme];
    if (sc->terrain_version != w->terrain.version) scenery_terrain(sc, w);
    int H = sc->height, W = sc->width;
    for (int py = 0; py < H; py++) memcpy(cv->px + py * cv->stride, sc->rows[py], sizeof(Px) * (size_t)W);
    sinq_init();
    Px starc = rgb15i(235, 240, 255);
    int tq = (int)(t * (1024 / 6.2831853f) * 16);      /* t in 1/16384 turn */
    for (int i = 0; i < w->nstars; i++) {
        const Star *s = &w->stars[i];
        Px *p = &cv->px[s->py * cv->stride + s->x];
        if (*p == sc->sky[s->py][s->x]) {
            /* (0.3 + 0.5 sin^2(t sp + ph)) b, in integers: phase in 1/1024 turns from Q4 speed */
            int a = (int)(((int64_t)tq * sc->starq[i].spq) >> 8) + sc->starq[i].phq;
            int v = SINQ(a);                                  /* Q14 */
            int k = ((77 + ((128 * ((v * v) >> 14)) >> 14)) * sc->starq[i].b8) >> 8;
            *p = pxmix_k(*p, starc, k);
        }
    }
    if (th->aurora) {
        int key = (int)(t * 6);           /* 6 Hz, at most one band rebuilt per frame (the ARM9 has no FPU) */
        for (int b = 0; b < 3; b++)
            if (key != sc->aur_key[b]) { sc->aur_key[b] = key; build_aurora(sc, b, key / 6.0f); break; }
        for (int b = 0; b < 3; b++)
            for (int i = 0; i < sc->naur[b]; i++) {
                Px *p = &cv->px[sc->aur[b][i].py * cv->stride + sc->aur[b][i].x];
                if (*p == sc->sky[sc->aur[b][i].py][sc->aur[b][i].x]) *p = sc->aur[b][i].c;
            }
    }
    const MoteStyle *ms = &MOTE_STYLES[th->fx];
    if (ms->on) {
        float top = H * 0.92f;
        for (int i = 0; i < w->nmotes; i++) {
            const Mote *m = &w->motes[i];
            int xi = (int)fmodf(m->x0 + fsin(t * 0.8f * m->sp + m->ph) * m->amp + W * 4, (float)W);
            float yy = fmodf(m->y0 + t * (float)ms->speed * m->sp, top);
            if (yy < 0) yy += top;
            int py = H - 1 - (int)yy;
            if (py >= 0 && py < H && xi >= 0 && xi < W && cv->px[py * cv->stride + xi] == sc->sky[py][xi]) {
                static const float LV[3] = {0.5f, 0.75f, 1.0f};
                int li = (int)((fsin(t * 5 * m->sp + m->ph) + 1) * 1.49f);
                cv->px[py * cv->stride + xi] = pxmix(sc->sky[py][xi], rgb15(ms->pal[m->ci]), (float)ms->alpha * LV[li < 0 ? 0 : li > 2 ? 2 : li]);
            }
        }
    }
    if (th->fx == FX_STORM || th->fx == FX_TEMPEST) {
        float period = 3.4f;
        int k = (int)(t / period);
        float off = 0.5f + (((uint32_t)k * 2654435761u >> 8) & 255) / 255.0f * 1.6f;
        float local = t - k * period - off;
        if (local >= 0 && local < 0.42f) {
            if (k != sc->bolt_key) { sc->bolt_key = k; make_bolt(sc, w, k); }
            float flk = (local < 0.1f || (int)(local * 25) % 2 == 0) ? 1.0f : 0.5f;
            float inten = roundf((1 - local / 0.42f) * flk * 4) / 4;
            if (inten > 0 && sc->nbolt) {
                float bx = sc->bolt[0][0];
                if (flashes) {
                    int x0 = (int)bx - 22 > 0 ? (int)bx - 22 : 0, x1 = (int)bx + 23 < W ? (int)bx + 23 : W;
                    int ka[FIELD_W];   /* the flash strength only depends on the column */
                    for (int x = x0; x < x1; x++) ka[x] = (int)(roundf(0.2f * inten * (1 - fabsf(x - bx) / 23) * 20) / 20 * 256);
                    Px fc = rgb15i(170, 200, 255);
                    for (int py = 0; py < H; py++)
                        for (int x = x0; x < x1; x++)
                            if (ka[x] > 5 && cv->px[py * cv->stride + x] == sc->sky[py][x]) cv->px[py * cv->stride + x] = pxmix_k(sc->sky[py][x], fc, ka[x]);
                }
                for (int s = 0; s < sc->nbolt; s++) {
                    float x0 = sc->bolt[s][0], y0 = sc->bolt[s][1], x1 = sc->bolt[s][2], y1 = sc->bolt[s][3];
                    int n = (int)fmaxf(fabsf(x1 - x0), fabsf(y1 - y0)) + 1;
                    for (int i = 0; i <= n; i++) {
                        float u = (float)i / n, px = x0 + (x1 - x0) * u, py2 = y0 + (y1 - y0) * u;
                        cblendf(cv, px, py2, (RGB){235, 245, 255}, fminf(1.0f, inten * 1.3f));
                        if (inten > 0.5f) { cblendf(cv, px + 1, py2, (RGB){130, 170, 255}, 0.35f * inten); cblendf(cv, px - 1, py2, (RGB){130, 170, 255}, 0.35f * inten); }
                    }
                }
            }
        }
    } else if (th->fx == FX_INFERNO) {
        float pulse = 0.5f + 0.5f * fsin(t * 1.7f);
        int y0 = (int)(H * 0.62f);
        for (int py = y0; py < H; py++) {
            float q = (float)(py - y0) / (H - y0 > 1 ? H - y0 : 1);
            float a = roundf(0.24f * pulse * powf(q, 1.5f) * 20) / 20;
            if (a < 0.03f) continue;
            Px glow = rgb15i(255, 110, 30);
            int ka = (int)(a * 256);
            for (int x = 0; x < W; x++)
                if (cv->px[py * cv->stride + x] == sc->sky[py][x]) cv->px[py * cv->stride + x] = pxmix_k(sc->sky[py][x], glow, ka);
        }
    } else if (th->fx == FX_RIFT) {
        rift_draw(sc, cv, t);
    }
}

/* ------------------------------------------------------------------ field view */
void field_init(FieldView *fv, Battle *b, const Settings *st) {
    fv->canvas.px = fv->canvas_px;
    fv->canvas.w = FIELD_W;
    fv->canvas.h = FIELD_H;
    fv->canvas.stride = FIELD_W;
    scenery_build(&fv->sc, &b->w);
    uint32_t seed = b->w.rng.mt[0] ^ (uint32_t)b->w.map;
    parts_init(&fv->ps, seed, st);
    rng_seed(&fv->vis, seed ^ 0x51u);
    fv->nguide = 0;
    fv->guide_version = -1;
}

void field_update(FieldView *fv, Battle *b, float dt) {
    float t = (float)b->time;
    for (int i = 0; i < b->nfxq; i++) {
        FxReq *r = &b->fxq[i];
        RGB c0 = live_color(r->c0, (float)b->w.clock);
        switch (r->type) {
        case FX_MUZZLE: fx_muzzle(&fv->ps, (float)r->x, (float)r->y, (float)r->p, c0); break;
        case FX_TRAIL: fx_trail(&fv->ps, (float)r->x, (float)r->y, c0, r->ammo_trail, r->n, t); break;
        case FX_IMPACT: fx_impact(&fv->ps, (float)r->x, (float)r->y, c0, r->c1, (float)r->p, r->ammo_boom); break;
        case FX_DEBRIS: fx_debris(&fv->ps, (float)r->x, (float)r->y, c0); break;
        default: break;
        }
    }
    float sdt = dt * (float)b->settings->anim_speed;
    fx_world_tick(&fv->ps, &b->w, sdt);
    parts_update(&fv->ps, sdt, &b->w.terrain);
}

static int probe_cb(void *c, double x, double y, int *tank) { return owner_probe((OwnerProbe *)c, x, y, tank); }

void field_draw(FieldView *fv, Battle *b, float t) {
    World *w = &b->w;
    Surf *cv = &fv->canvas;
    scenery_draw(&fv->sc, w, cv, t, b->settings->effects);
    /* wind streaks (l.3794) */
    if (fabs(w->wind) >= 0.3) {
        int sgn = w->wind > 0 ? 1 : -1, length = 3 + (int)(fabs(w->wind) * 0.9);
        for (int i = 0; i < w->nstreaks; i++) {
            int py = w->height - 1 - (int)w->streaks[i][1];
            if (py < 0 || py >= w->height) continue;
            for (int k = 0; k < length; k++) {
                int px = (int)w->streaks[i][0] - sgn * k;
                if (px >= 0 && px < w->width && cv->px[py * cv->stride + px] == fv->sc.sky[py][px])
                    cv->px[py * cv->stride + px] = pxmix(fv->sc.sky[py][px], rgb15i(235, 242, 255), 0.22f * (1 - (float)k / length));
            }
        }
    }
    Tank *cur = battle_current(b);
    RGB shot_col = live_color(cur->shot_color, t);
    if (b->phase == PH_AIMING && b->settings->aim_guide) {
        if (fv->guide_version != b->guide_version) {
            fv->guide_version = b->guide_version;
            double zero = 0.0;
            Launch L = make_ammo_launch(w, cur, cur->angle, cur->power, &zero, false);
            OwnerProbe p = {w, cur->index, false};
            fv->nguide = phys_trace_points(&L, probe_cb, &p, 0.02, 2.6, 8.0, fv->guide, 100);
        }
        int n = b->settings->aim_guide == 1 ? (fv->nguide < 10 ? fv->nguide : 10) : fv->nguide;
        for (int i = 0; i < n; i++) cblendf(cv, fv->guide[2 * i], fv->guide[2 * i + 1], shot_col, 0.85f * (1 - (float)i / (n + 4)));
    }
    for (int i = 0; i < w->ntanks; i++) {
        Tank *tk = &w->tanks[i];
        float flash = tk->hurt > 0 ? (float)(tk->hurt / 0.6) * (0.4f + 0.4f * fsin(t * 40)) : 0.0f;
        if (flash < 0) flash = 0;
        RGB body = (tk->rgb_hull && rgb_is_pixel_paint(tk->hull_sentinel)) ? tk->hull_sentinel : live_color(tk->color, (float)w->clock);
        if (tk->kind == KIND_UFO) { draw_ufo(cv, tk, t, flash); continue; }
        draw_tank(cv, (float)tk->x, (float)tk->y, tk->facing, (float)tk->angle, body, 1, flash, tk->destroyed, (float)tk->recoil,
                  design_ptr(tk->design), t, 0.0f, (float)tk->hover);
    }
    parts_draw(&fv->ps, cv);
    for (int i = 0; i < b->nflights; i++) {
        LiveFlight *f = &b->flights[i];
        if (f->delay > 0 || f->spec.trace.time <= 0) continue;
        float x = (float)f->x, y = (float)f->y;
        const Launch *lc = &f->spec.launch;
        float vx = (float)(lc->vx + lc->ax * f->t), vy = (float)(lc->vy - lc->g * f->t);
        if (y >= w->height) {
            draw_projectile(cv, x, (float)w->height - 5, (float)lc->vx, 0.0f, shot_col, &AMMOS[cur->ammo], t);
            continue;
        }
        if (b->blessing_shot) {
            for (int k = 0; k < 30; k++) {
                float a = k / 30.0f * TAU_F - t * 5, rr = 4.5f + 1.5f * fsin(t * 8 + k);
                cblendf(cv, x + fcos(a) * rr, y + fsin(a) * rr, k % 2 ? (RGB){255, 214, 90} : (RGB){176, 120, 255}, 0.7f);
            }
            for (int k = 0; k < 6; k++) { float a = t * 6 + k * TAU_F / 6; cblendf(cv, x + fcos(a) * 2.6f, y + fsin(a) * 2.6f, WHITE, 0.9f); }
        }
        if (b->special_shot) {
            for (int k = 0; k < 26; k++) {
                float a = k / 26.0f * TAU_F + t * 6, rr = 5 + fsin(t * 9);
                cblendf(cv, x + fcos(a) * rr, y + fsin(a) * rr, shot_col, 0.55f);
            }
            for (int k = 0; k < 3; k++) {
                float a = t * 7 + k * TAU_F / 3;
                draw_projectile(cv, x + fcos(a) * 3.2f, y + fsin(a) * 3.2f, vx, vy, shot_col, &AMMOS[cur->ammo], t);
            }
        }
        draw_projectile(cv, x, y, vx, vy, shot_col, &AMMOS[cur->ammo], t);
    }
    for (int i = 0; i < b->nexplosions; i++) {
        Explosion e = b->explosions[i];
        e.accent = live_color(e.accent, (float)w->clock);
        draw_explosion(cv, &e);
    }
}

void field_blit(FieldView *fv, Battle *b, Surf *dst, int x0, int y0) {
    int shx = 0, shy = 0;
    if (b->settings->effects && b->shake >= 0.35) {
        shx = (int)rint(rng_uniform(&fv->vis, -1, 1) * b->shake);
        shy = (int)rint(rng_uniform(&fv->vis, -1, 1) * b->shake * 0.6);
    }
    const Surf *cv = &fv->canvas;
    /* the original shifts the half-block rows (dx in columns, dy in pixel rows): here 1 world px = 2 screen px */
    for (int py = 0; py < cv->h; py++) {
        int sy = py - shy;
        if (sy < 0) sy = 0;
        if (sy >= cv->h) sy = cv->h - 1;
        const Px *src = cv->px + sy * cv->stride;
        Px *d0 = dst->px + (y0 + py * 2) * dst->stride + x0;
        uint32_t *w0 = (uint32_t *)d0;            /* x0 is even: 32-bit writes, two screen pixels per world pixel */
        if (shx == 0) {
            for (int x = 0; x < cv->w; x++) { uint32_t p = src[x]; w0[x] = p | (p << 16); }
        } else {
            for (int x = 0; x < cv->w; x++) {
                int sx = x - shx;
                sx = sx < 0 ? 0 : (sx >= cv->w ? cv->w - 1 : sx);
                uint32_t p = src[sx];
                w0[x] = p | (p << 16);
            }
        }
        memcpy(d0 + dst->stride, d0, (size_t)cv->w * 4);
    }
    if (b->flash > 0 && b->settings->effects) surf_rect_blend(dst, x0, y0, cv->w * 2, cv->h * 2, WHITE, (float)b->flash * 0.45f);
}
