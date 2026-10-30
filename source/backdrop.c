/* Menu backdrops (terminaltanks.py l.6175-6290, 6414-6635, 6700-6796, 6797-7396):
 *   AmbientBackdrop  - the classic dark range with lobbed shells
 *   MasterBackdrop   - aurora sky, citadel and the gold Sovereign
 *   SecretBackdrop   - green code rain and an ASCIIBOT
 *   ThemeBackdrop    - every shop / mission / ticket / ancient home screen, driven by a HomeTheme and its layers
 * DS layout: the two screens form one 51x48 cell terminal. The sky spans both screens; theme layers are drawn in the
 * bottom half (where the original's horizon sits relative to its menu); heroes are drawn at scale 1 (the DS terminal
 * is 51 columns, not 96+) and their shell speeds are scaled to keep the arcs on screen. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "render.h"
#include "sprites.h"
#include "particles.h"

#define TAU_F 6.2831853f
#define PI_F 3.14159265f
static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static const RGB WHITE = {255, 255, 255};
static const RGB GOLD = {255, 206, 100};
static float U(PyRandom *r, float a, float b) { return (float)rng_uniform(r, a, b); }

enum { BD_AMBIENT = 0, BD_MASTER, BD_SECRET, BD_THEME };
#define MAX_SHELLS 6
#define MAX_EXPL 12

typedef struct { Launch l; float t; RGB c; } Shell;

struct Backdrop {
    int kind;
    PyRandom rng;
    bool built;
    int w, h;                 /* canvas pixels (cols, rows*2) */
    Canvas layer;             /* baked background (ambient / master) */
    Canvas cv;                /* working canvas */
    int ridge[T_COLS];
    float stars[T_COLS][4];
    int nstars;
    Particles *ps;
    Shell shells[MAX_SHELLS];
    int nshells;
    Explosion ex[MAX_EXPL];
    int nex;
    float spawn_timer, timer, fire_timer, clock, recoil, fw_timer;
    RGB celebrate[8];
    int ncelebrate;
    /* master */
    float hero_x, hero_angle, hero_target;
    bool pending;
    float pend_tx, pend_th;
    int beacons[8][2];
    int nbeacons;
    float shooting[8][5];
    int nshooting;
    int n;
    /* secret */
    float drops[T_COLS][3];
    /* theme */
    int home;
    RGB rowcol[T_ROWS];
    float tstars[64][3];
    int ntstars;
    float motes[160][6];
    int nmotes;
};

static void add_expl(Backdrop *b, float x, float y, float r, RGB c, float dur, int style) {
    if (b->nex >= MAX_EXPL) return;
    b->ex[b->nex++] = (Explosion){x, y, r, c, dur, 0, (uint8_t)style};
}
static void update_expl(Backdrop *b, float dt) {
    int o = 0;
    for (int i = 0; i < b->nex; i++) {
        b->ex[i].age += dt;
        if (b->ex[i].age < b->ex[i].duration) b->ex[o++] = b->ex[i];
    }
    b->nex = o;
}
static Launch mk_launch(float x0, float y0, float vx, float vy, float g) {
    Launch L;
    memset(&L, 0, sizeof L);
    L.x0 = x0; L.y0 = y0; L.vx = vx; L.vy = vy; L.g = g;
    return L;
}
static void shell_at(const Shell *s, float *x, float *y) {
    double dx, dy;
    launch_at(&s->l, s->t, &dx, &dy);
    *x = (float)dx;
    *y = (float)dy;
}

static Backdrop *bd_alloc(int kind, uint32_t seed, int pcap) {
    Backdrop *b = (Backdrop *)calloc(1, sizeof(Backdrop));
    b->kind = kind;
    rng_seed(&b->rng, seed);
    b->w = T_COLS;
    b->h = T_ROWS * 2;
    if (pcap) {
        b->ps = (Particles *)calloc(1, sizeof(Particles));
        parts_init(b->ps, seed, &g_app.settings);
    }
    return b;
}

/* ================================================================== Ambient (l.6175) */
static void amb_ensure(Backdrop *b) {
    if (b->built) return;
    b->built = true;
    int w = b->w, h = b->h;
    PyRandom *r = &b->rng;
    float ph[6];
    for (int i = 0; i < 6; i++) ph[i] = U(r, 0, TAU_F);
    int far[T_COLS];
    for (int x = 0; x < w; x++) {
        float u = x / (float)w * TAU_F;
        b->ridge[x] = (int)(h * (0.22f + 0.06f * fsin(u * 2 + ph[0]) + 0.04f * fsin(u * 6 + ph[1]) + 0.015f * fsin(u * 19 + ph[2])));
        far[x] = (int)(h * (0.34f + 0.08f * fsin(u * 3 + ph[3]) + 0.04f * fsin(u * 9 + ph[4])));
    }
    canvas_alloc(&b->layer, w, h);
    canvas_alloc(&b->cv, w, h);
    static const float GT[4] = {0, 0.25f, 0.6f, 1};
    static const RGB GC[4] = {{30, 110, 120}, {16, 62, 78}, {8, 24, 42}, {4, 8, 20}};
    for (int py = 0; py < h; py++) {
        int y = h - 1 - py;
        RGB base = cgradient(GT, GC, 4, y / (float)h);
        for (int x = 0; x < w; x++) {
            RGB c = base;
            if (y < b->ridge[x]) c = cmix((RGB){10, 26, 34}, (RGB){4, 12, 18}, 1 - y / (float)(b->ridge[x] > 1 ? b->ridge[x] : 1));
            else if (y < far[x]) c = cmix(base, (RGB){20, 48, 62}, 0.75f);
            float vx = (x / (float)w - 0.5f) * 2, vy = (py / (float)h - 0.5f) * 2;
            c = cshade(c, fmaxf(0.25f, 1 - 0.42f * (vx * vx + vy * vy) * 0.8f));
            if ((py / 2) % 2) c = cshade(c, 0.9f);
            b->layer.s.px[py * w + x] = rgb15(c);
        }
    }
    b->nstars = w / 3;
    for (int i = 0; i < b->nstars; i++) {
        b->stars[i][0] = (float)rng_below(r, (uint32_t)w);
        b->stars[i][1] = (float)rng_below(r, (uint32_t)(int)(h * 0.6f));
        b->stars[i][2] = U(r, 0, 6);
        b->stars[i][3] = U(r, 1, 3);
    }
}

static void amb_spawn(Backdrop *b) {
    PyRandom *r = &b->rng;
    int w = b->w;
    bool left = rng_random(r) < 0.5;
    float x0 = left ? U(r, 0.05f, 0.3f) * w : U(r, 0.7f, 0.95f) * w;
    float tx = U(r, 0.3f, 0.7f) * w;
    float th = U(r, 48, 68) * PI_F / 180;
    float y0 = b->ridge[(int)x0] + 2;
    float tmax = 4 * (b->h - 9 - y0) / fmaxf(4.0f, fabsf(tx - x0));
    if (tanf(th) > tmax) th = atanf(fmaxf(0.45f, tmax));
    float v = sqrtf(32 * fabsf(tx - x0) / fsin(2 * th));
    if (b->nshells >= MAX_SHELLS) return;
    Shell *s = &b->shells[b->nshells++];
    s->l = mk_launch(x0, y0, copysignf(v * fcos(th), tx - x0), v * fsin(th), 32);
    s->t = 0;
    s->c = COLOR_CHOICES[rng_below(r, N_COLORS)].c;
}

static void celebrate_tick(Backdrop *b, float dt, float lo, float hi, float wy0, float wy1) {
    if (!b->ncelebrate) return;
    b->fw_timer -= dt;
    if (b->fw_timer > 0) return;
    PyRandom *r = &b->rng;
    b->fw_timer = U(r, lo, hi);
    float bx = U(r, 0.15f, 0.85f) * b->w, by = U(r, wy0, wy1) * b->h;
    RGB col = b->celebrate[rng_below(r, (uint32_t)b->ncelebrate)];
    for (int i = 0; i < 60; i++) {
        float a = U(r, 0, TAU_F), sp = U(r, 8, 34), life = U(r, 0.9f, 1.6f);
        parts_emit(b->ps, bx, by, fcos(a) * sp, fsin(a) * sp, life, life, cmix(WHITE, col, 0.5f), cshade(col, 0.2f), 26, 1.1f, 1, false, 1, 0);
    }
}

static void amb_update(Backdrop *b, float dt) {
    if (!b->built) return;
    parts_update(b->ps, dt, NULL);
    update_expl(b, dt);
    b->spawn_timer -= dt;
    if (b->spawn_timer <= 0 && b->nshells < 3 && !b->ncelebrate) {
        b->spawn_timer = U(&b->rng, 1.0f, 2.8f);
        amb_spawn(b);
    }
    for (int i = 0; i < b->nshells;) {
        Shell *s = &b->shells[i];
        s->t += dt;
        float x, y;
        shell_at(s, &x, &y);
        parts_emit(b->ps, x, y, 0, 0, 0.6f, 0.6f, cmix(WHITE, s->c, 0.5f), cshade(s->c, 0.15f), 0, 0, 0.9f, false, 1, 0);
        if (x < 0 || x >= b->w || y < b->ridge[(int)x]) {
            RGB c = s->c;
            b->shells[i] = b->shells[--b->nshells];
            if (x >= 0 && x < b->w) {
                add_expl(b, x, y, 3.5f, c, 0.7f, BOOM_CLASSIC);
                for (int k = 0; k < 18; k++) {
                    float a = U(&b->rng, 0.2f, 2.9f), sp = U(&b->rng, 10, 34);
                    parts_emit(b->ps, x, y, fcos(a) * sp, fsin(a) * sp, 0.8f, 0.8f, WHITE, cshade(c, 0.2f), 60, 0, 1, false, 1, 0);
                }
            }
            continue;
        }
        i++;
    }
    celebrate_tick(b, dt, 0.25f, 0.6f, 0.45f, 0.85f);
}

static void amb_draw(Backdrop *b, Term *t, float time) {
    amb_ensure(b);
    Surf *cv = &b->cv.s;
    memcpy(cv->px, b->layer.s.px, sizeof(Px) * (size_t)(b->w * b->h));
    for (int i = 0; i < b->nstars; i++) {
        int x = (int)b->stars[i][0], py = (int)b->stars[i][1];
        float s = fsin(time * b->stars[i][3] + b->stars[i][2]);
        Px *p = &cv->px[py * b->w + x];
        *p = rgb15(cmix(px_rgb(*p), (RGB){220, 240, 255}, 0.25f + 0.5f * s * s));
    }
    parts_draw(b->ps, cv);
    for (int i = 0; i < b->nex; i++) draw_explosion(cv, &b->ex[i]);
    for (int i = 0; i < b->nshells; i++) {
        float x, y;
        shell_at(&b->shells[i], &x, &y);
        cv_plotf(cv, x, y, WHITE);
    }
    term_blit(t, cv, 0, 0, 0, 0);
}

/* ================================================================== Master (l.6414) */
static void mst_ensure(Backdrop *b) {
    if (b->built) return;
    b->built = true;
    int w = b->w, h = b->h;
    PyRandom *r = &b->rng;
    static const float ST[4] = {0, 0.16f, 0.42f, 1};
    static const RGB SC[4] = {{255, 166, 96}, {214, 78, 124}, {74, 26, 116}, {5, 3, 24}};
    float ph[6];
    for (int i = 0; i < 6; i++) ph[i] = U(r, 0, TAU_F);
    int far[T_COLS], ground[T_COLS], cit[T_COLS];
    for (int x = 0; x < w; x++) {
        float u = x / (float)w * TAU_F;
        far[x] = (int)(h * (0.30f + 0.07f * fsin(u * 3 + ph[0]) + 0.04f * fsin(u * 9 + ph[1])));
        ground[x] = (int)(h * (0.15f + 0.03f * fsin(u * 2.4f + ph[2]) + 0.015f * fsin(u * 13 + ph[3])));
        cit[x] = 0;
    }
    b->hero_x = w * 0.13f;
    int hx = (int)b->hero_x;
    int plat = ground[hx];
    for (int x = hx - 10 < 0 ? 0 : hx - 10; x < (hx + 11 < w ? hx + 11 : w); x++) {
        int ax = x - hx < 0 ? hx - x : x - hx;
        ground[x] = (int)(ground[x] + ((plat + 2) - ground[x]) * (ax <= 7 ? 1.0f : 0.5f));
    }
    int cx = (int)(w * 0.80f);
    b->nbeacons = 0;
    int wall = (int)(h * 0.26f);
    static const float TOW[5][3] = {{-0.17f, 0.42f, 5}, {-0.09f, 0.34f, 4}, {-0.02f, 0.62f, 6}, {0.07f, 0.38f, 4}, {0.15f, 0.48f, 5}};
    int windows[25][2], nw = 0;
    for (int k = 0; k < 5; k++) {
        int tw = (int)TOW[k][2];
        int tx = cx + (int)(TOW[k][0] * w);
        int top = (int)(h * TOW[k][1]);
        for (int x = tx - tw; x <= tx + tw; x++) if (x >= 0 && x < w && top > cit[x]) cit[x] = top;
        for (int kk = 1; kk < 5; kk++)
            for (int x = tx - tw + kk; x <= tx + tw - kk; x++)
                if (x >= 0 && x < w && (kk * 2 + top) > cit[x] && kk < tw) cit[x] = top + kk * 2 > cit[x] ? top + kk * 2 : cit[x];
        if (tx >= 0 && tx < w && b->nbeacons < 8) { b->beacons[b->nbeacons][0] = tx; b->beacons[b->nbeacons][1] = top + tw * 2; b->nbeacons++; }
        for (int q = 0; q < 5; q++) {
            int lo = (int)(h * 0.2f), hi = top - 2 > (int)(h * 0.21f) ? top - 2 : (int)(h * 0.21f);
            windows[nw][0] = tx + rng_randint(r, -tw + 1, tw - 1);
            windows[nw][1] = rng_randint(r, lo, hi);
            nw++;
        }
    }
    for (int x = (int)(cx - 0.2f * w); x < (int)(cx + 0.2f * w); x++)
        if (x >= 0 && x < w && wall > cit[x]) cit[x] = wall;
    for (int x = 0; x < w; x++) b->ridge[x] = ground[x] > cit[x] - 1 ? ground[x] : cit[x] - 1;
    canvas_alloc(&b->layer, w, h);
    canvas_alloc(&b->cv, w, h);
    for (int py = 0; py < h; py++) {
        int y = h - 1 - py;
        RGB base = cgradient(ST, SC, 4, y / (float)h);
        for (int x = 0; x < w; x++) {
            RGB c = base;
            if (y < far[x]) c = cmix(base, (RGB){46, 20, 78}, 0.7f);
            if (y < cit[x]) c = cmix(c, (RGB){10, 5, 22}, 0.94f);
            if (y < ground[x]) {
                c = cmix((RGB){20, 10, 34}, (RGB){8, 4, 16}, 1 - y / (float)(ground[x] > 1 ? ground[x] : 1));
                if (y == ground[x] - 1) c = (RGB){150, 104, 56};
            }
            float vx = (x / (float)w - 0.5f) * 2, vy = (py / (float)h - 0.5f) * 2;
            c = cshade(c, fmaxf(0.3f, 1 - 0.38f * (vx * vx + vy * vy) * 0.8f));
            if ((py / 2) % 2) c = cshade(c, 0.92f);
            b->layer.s.px[py * w + x] = rgb15(c);
        }
    }
    for (int i = 0; i < nw; i++) {
        int wx = windows[i][0], wy = windows[i][1], py = h - 1 - wy;
        if (wx >= 0 && wx < w && py >= 0 && py < h && wy < cit[wx] - 2) b->layer.s.px[py * w + wx] = rgb15i(255, 196, 96);
    }
    b->nstars = w / 2;
    for (int i = 0; i < b->nstars; i++) {
        b->stars[i][0] = (float)rng_below(r, (uint32_t)w);
        b->stars[i][1] = (float)rng_below(r, (uint32_t)(int)(h * 0.62f));
        b->stars[i][2] = U(r, 0, 6);
        b->stars[i][3] = U(r, 1, 4);
    }
    b->timer = 1.5f;
    b->fire_timer = 2.0f;
    b->hero_angle = b->hero_target = 40;
}

static bool mst_sky_ok(Backdrop *b, int x, int y) { return x >= 0 && x < b->w && y > b->ridge[x] + 2; }

static void mst_update(Backdrop *b, float dt) {
    if (!b->built) return;
    PyRandom *r = &b->rng;
    b->clock += dt;
    b->recoil = fmaxf(0, b->recoil - dt);
    int w = b->w, h = b->h;
    b->ps->wind = 0;
    parts_update(b->ps, dt, NULL);
    update_expl(b, dt);
    if (rng_random(r) < dt * 5 * g_app.settings.particle_density)
        parts_emit(b->ps, U(r, 0, w), U(r, 0, h * 0.2f), U(r, -1.5f, 1.5f), U(r, 3, 8), U(r, 3, 6), 5, (RGB){255, 214, 120}, (RGB){140, 60, 120}, 0, 0,
                   0.85f, false, 1, 0);
    b->timer -= dt;
    if (b->timer <= 0) {
        b->timer = U(r, 2.5f, 6);
        if (b->nshooting < 8) {
            float *s = b->shooting[b->nshooting++];
            s[0] = U(r, 0.1f, 0.9f) * w; s[1] = U(r, 0.6f, 0.95f) * h; s[2] = U(r, 40, 70); s[3] = -U(r, 10, 24); s[4] = 0;
        }
    }
    int o = 0;
    for (int i = 0; i < b->nshooting; i++) {
        float *s = b->shooting[i];
        s[0] += s[2] * dt; s[1] += s[3] * dt; s[4] += dt;
        if (s[4] < 1.1f) memcpy(b->shooting[o++], s, sizeof(float) * 5);
    }
    b->nshooting = o;
    b->hero_angle += (b->hero_target - b->hero_angle) * fminf(1.0f, dt * 4);
    b->fire_timer -= dt;
    if (b->fire_timer <= 0 && !b->pending && !b->ncelebrate) {
        b->fire_timer = U(r, 2.2f, 3.6f);
        b->pend_tx = U(r, 0.66f, 0.92f) * w;
        b->pend_th = U(r, 42, 62);
        b->hero_target = b->pend_th;
        b->pending = true;
    }
    if (b->pending && fabsf(b->hero_angle - b->hero_target) < 1.5f) {
        b->pending = false;
        float hy = (float)b->ridge[(int)b->hero_x];
        float th = b->hero_angle, rad = th * PI_F / 180;
        double ox, oy;
        muzzle_point((int)b->hero_x + 0.5, hy, th, DESIGN_SOVEREIGN, 1, 0, &ox, &oy);
        float reach = fmaxf(10.0f, b->pend_tx - (float)ox);
        reach = fminf(reach, 4 * fmaxf(10.0f, h - 10 - (float)oy) / fmaxf(0.3f, tanf(rad)));
        float v = sqrtf(32 * reach / fmaxf(0.2f, fsin(2 * rad)));
        if (b->nshells < MAX_SHELLS) {
            Shell *s = &b->shells[b->nshells++];
            s->l = mk_launch((float)ox, (float)oy, v * fcos(rad), v * fsin(rad), 32);
            s->t = 0;
            s->c = GOLD;
        }
        b->recoil = 0.25f;
        fx_muzzle(b->ps, (float)ox, (float)oy, th, (RGB){255, 214, 120});
    }
    int aur = ammo_by_key("aurora");
    for (int i = 0; i < b->nshells;) {
        Shell *s = &b->shells[i];
        s->t += dt;
        float x, y;
        shell_at(s, &x, &y);
        b->n++;
        fx_trail(b->ps, x, y, GOLD, AMMOS[aur].trail, b->n, b->clock);
        if (x < 0 || x >= w || (y < b->ridge[(int)x] && s->t > 0.2f)) {
            b->shells[i] = b->shells[--b->nshells];
            if (x >= 0 && x < w) {
                add_expl(b, x, y, 6.0f, GOLD, 0.95f, BOOM_PRISM);
                fx_impact(b->ps, x, y, GOLD, (RGB){90, 60, 80}, 0.9f, AMMOS[aur].boom);
            }
            continue;
        }
        i++;
    }
    celebrate_tick(b, dt, 0.2f, 0.5f, 0.45f, 0.85f);
}

static void mst_draw(Backdrop *b, Term *t, float time) {
    (void)time;
    mst_ensure(b);
    Surf *cv = &b->cv.s;
    int w = b->w, h = b->h;
    float c = b->clock;
    memcpy(cv->px, b->layer.s.px, sizeof(Px) * (size_t)(w * h));
    /* DS: one aurora band (the original has three) and no god rays - the small screens were too busy */
    for (int x = 0; x < w; x++) {
        float mid = h * 0.70f + h * 0.06f * fsin(x * 0.045f + c * 0.6f) + h * 0.035f * fsin(x * 0.11f - c * 0.9f);
        int thick = 4 + (int)(2 * fsin(x * 0.07f + c));
        Px col = rgb15(chsv(0.46f + 0.22f * fsin(x * 0.02f + c * 0.25f), 0.65f, 1.0f));
        for (int dy = -thick; dy <= thick; dy++) {
            int y = (int)mid + dy;
            if (!mst_sky_ok(b, x, y) || y < 0 || y >= h) continue;
            int ady = dy < 0 ? -dy : dy;
            float a = 0.28f * (1 - ady / (float)(thick + 1));
            Px *p = &cv->px[(h - 1 - y) * cv->stride + x];
            *p = pxmix(*p, col, a);
        }
    }
    for (int i = 0; i < b->nstars; i++) {
        int x = (int)b->stars[i][0], py = (int)b->stars[i][1];
        if (mst_sky_ok(b, x, h - 1 - py)) {
            float s = fsin(c * b->stars[i][3] + b->stars[i][2]);
            Px *p = &cv->px[py * w + x];
            *p = rgb15(cmix(px_rgb(*p), (RGB){240, 235, 255}, 0.25f + 0.55f * s * s));
        }
    }
    for (int i = 0; i < b->nshooting; i++) {
        float *s = b->shooting[i];
        for (int k = 0; k < 10; k++) cv_blendf(cv, s[0] - s[2] * 0.03f * k, s[1] - s[3] * 0.03f * k, (RGB){255, 240, 220}, (1 - k / 10.0f) * (1 - s[4] / 1.1f));
    }
    for (int i = 0; i < b->nbeacons; i++) {
        int bx = b->beacons[i][0], by = b->beacons[i][1];
        float pulse = 0.5f + 0.5f * fsin(c * 3 + bx);
        for (int dx = -2; dx <= 2; dx++)
            for (int dy = -2; dy <= 2; dy++) cv_blend(cv, bx + dx, by + dy, (RGB){255, 150, 90}, 0.5f * pulse / (1 + abs(dx) + abs(dy)));
    }
    parts_draw(b->ps, cv);
    float hy = (float)b->ridge[(int)b->hero_x];
    draw_tank(cv, (int)b->hero_x + 0.5f, hy, 1, b->hero_angle, GOLD, 1, 0, false, b->recoil, &DESIGNS[DESIGN_SOVEREIGN], c, -1, -1);
    int aur = ammo_by_key("aurora");
    for (int i = 0; i < b->nshells; i++) {
        float x, y;
        shell_at(&b->shells[i], &x, &y);
        draw_projectile(cv, x, y, (float)b->shells[i].l.vx, (float)b->shells[i].l.vy - 32 * b->shells[i].t, GOLD, &AMMOS[aur], c);
    }
    for (int i = 0; i < b->nex; i++) draw_explosion(cv, &b->ex[i]);
    term_blit(t, cv, 0, 0, 0, 0);
}

/* ================================================================== Secret (l.6700) */
static const char SECRET_GLYPHS[] = "01ABCDEF{}[]<>/\\|#$%&*+=~;:?";
#define HERO_V 0.72f    /* shell speed scale for the narrower DS terminal (range ~ v^2) */

static void sec_ensure(Backdrop *b) {
    if (b->built) return;
    b->built = true;
    PyRandom *r = &b->rng;
    int rows = T_ROWS;
    for (int x = 0; x < T_COLS; x++) {
        b->drops[x][0] = U(r, -rows, rows);
        b->drops[x][1] = U(r, 8, 26);
        b->drops[x][2] = (float)rng_randint(r, 6, 16);
    }
    canvas_alloc(&b->cv, T_COLS, 72);
    b->timer = 1.2f;
}

static void hero_fire(Backdrop *b, int design, float bx, float by, float g, float hmax) {
    PyRandom *r = &b->rng;
    b->recoil = 0.25f;
    float v = U(r, 34, 44) * HERO_V;
    float ang = 38 + 9 * fsin(b->clock * 0.7f);
    float lift = DESIGNS[design].hover > 0 ? (float)DESIGNS[design].hover + 0.6f * fsin(b->clock * 2.4f) : 0;
    double ox, oy;
    muzzle_point(bx, by + lift, ang, design, 1, 0, &ox, &oy);
    float a = ang * PI_F / 180;
    float cap = hmax - 10;
    float apex = (float)oy + powf(v * fsin(a), 2) / (2 * g);
    if (apex > cap && cap > oy) v *= sqrtf((cap - (float)oy) / (apex - (float)oy));
    if (b->nshells < MAX_SHELLS) {
        Shell *s = &b->shells[b->nshells++];
        s->l = mk_launch((float)ox, (float)oy, v * fcos(a), v * fsin(a), g);
        s->t = 0;
    }
}

static void sec_update(Backdrop *b, float dt) {
    if (!b->built) return;
    b->clock += dt;
    int rows = T_ROWS;
    for (int x = 0; x < T_COLS; x++) {
        float *d = b->drops[x];
        d[0] += d[1] * dt;
        if (d[0] - d[2] > rows) {
            d[0] = -U(&b->rng, 0, rows * 0.5f);
            d[1] = U(&b->rng, 8, 26);
            d[2] = (float)rng_randint(&b->rng, 6, 16);
        }
    }
    b->recoil = fmaxf(0, b->recoil - dt);
    b->timer -= dt;
    if (b->timer <= 0) {
        b->timer = U(&b->rng, 2.2f, 3.4f);
        hero_fire(b, DESIGN_ASCIIBOT, 6.5f, 2, 40, 72);
    }
    for (int i = 0; i < b->nshells;) {
        Shell *s = &b->shells[i];
        s->t += dt;
        float x, y;
        shell_at(s, &x, &y);
        if (y < 3.5f && s->t > 0.2f) {
            b->shells[i] = b->shells[--b->nshells];
            add_expl(b, x, 3.5f, 5.5f, (RGB){110, 255, 150}, 0.9f, BOOM_CODE);
            continue;
        }
        i++;
    }
    update_expl(b, dt);
}

static void sec_draw(Backdrop *b, Term *t, float time) {
    sec_ensure(b);
    int cols = T_COLS, rows = T_ROWS;
    for (int y = 0; y < rows; y++) {
        RGB bg = cmix((RGB){2, 14, 8}, (RGB){0, 32, 16}, y / (float)(rows - 1));
        term_fill(t, 0, y, cols, 1, y % 2 ? cshade(bg, 0.8f) : bg);
    }
    int tick = (int)(time * 8);
    int ng = (int)strlen(SECRET_GLYPHS);
    for (int x = 0; x < cols; x++) {
        int head = (int)b->drops[x][0], trail = (int)b->drops[x][2];
        for (int i = 0; i < trail; i++) {
            int y = head - i;
            if (y >= 0 && y < rows) {
                char ch = SECRET_GLYPHS[(unsigned)(x * 7919 + y * 104729 + (i < 2 ? tick : tick / 4) * 31) % (unsigned)ng];
                term_put(t, x, y, (uint32_t)ch, i == 0 ? (RGB){205, 255, 215} : cmix((RGB){70, 255, 120}, (RGB){4, 40, 18}, i / (float)trail), NULL);
            }
        }
    }
    Surf *cv = &b->cv.s;
    memset(cv->px, 0, sizeof(Px) * (size_t)(cv->w * cv->h));
    for (int x = 0; x < cols; x++) {
        cv_plot(cv, x, 0, rgb15i(22, 96, 48));
        cv_plot(cv, x, 1, rgb15i(40, 150, 76));
    }
    draw_tank(cv, 6.5f, 2, 1, 38 + 9 * fsin(b->clock * 0.7f), (RGB){70, 220, 120}, 1, 0, false, b->recoil, &DESIGNS[DESIGN_ASCIIBOT], b->clock, -1, -1);
    for (int i = 0; i < b->nshells; i++) {
        float x, y;
        shell_at(&b->shells[i], &x, &y);
        draw_projectile(cv, x, y, (float)b->shells[i].l.vx, (float)b->shells[i].l.vy - 40 * b->shells[i].t, (RGB){110, 255, 150}, &AMMOS[AMMO_GLYPH], b->clock);
    }
    for (int i = 0; i < b->nex; i++) draw_explosion(cv, &b->ex[i]);
    term_blit_masked(t, cv, 0, rows - cv->h / 2, 0, cols);
}

/* ================================================================== Theme (l.7259) + layers (l.6797-7257) */
#define LROWS ROWS            /* theme layers live in the bottom screen: a 51x24 window */
#define LOFF T_SPLIT

typedef struct { Backdrop *b; Term *t; int cols, rows; float time; const HomeTheme *th; } LCtx;

static void L_cell(LCtx *c, int x, int y, RGB col) { if (y >= 0 && y < c->rows) term_cell(c->t, x, y + LOFF, col); }
static void L_cellch(LCtx *c, int x, int y, RGB col, uint32_t ch, RGB fg) {
    if (x >= 0 && x < c->cols && y >= 0 && y < c->rows) term_put(c->t, x, y + LOFF, ch, fg, &col);
}
static void L_put(LCtx *c, int x, int y, uint32_t ch, RGB fg, const RGB *bg) { if (y >= 0 && y < c->rows) term_put(c->t, x, y + LOFF, ch, fg, bg); }
static Cell *L_back(LCtx *c, int x, int y) { return &c->t->c[y + LOFF][x]; }
static RGB L_rc(LCtx *c, int y) {
    y = y < 0 ? 0 : (y >= c->rows ? c->rows - 1 : y);
    return c->b->rowcol[y + LOFF];
}
static RGB L_bg(LCtx *c, int x, int y) { return px_rgb(L_back(c, x, y)->bg); }

static void L_disc(LCtx *c, float cx, float cy, float r, RGB col, bool glow) {
    for (int y = (int)(cy - r * 1.8f); y <= (int)(cy + r * 1.8f); y++)
        for (int x = (int)(cx - r * 3.4f); x <= (int)(cx + r * 3.4f); x++) {
            float d = fhypot((x - cx) / 2.0f, y - cy);
            if (d <= r) L_cell(c, x, y, col);
            else if (glow && d <= r * 1.7f && y >= 0 && y < c->rows && x >= 0 && x < c->cols) {
                Cell *cl = L_back(c, x, y);
                cl->bg = rgb15(cmix(px_rgb(cl->bg), col, 0.32f * (1 - (d - r) / (r * 0.7f))));
            }
        }
}

static void l_moon(LCtx *c) {
    float r = (float)(c->rows / 8 > 3 ? c->rows / 8 : 3);
    float cx = c->cols * 0.2f, cy = c->rows * 0.22f;
    L_disc(c, cx, cy, r, cmix(c->th->accent, WHITE, 0.7f), true);
    static const float K[3][3] = {{-0.3f, -0.2f, 0.28f}, {0.25f, 0.25f, 0.22f}, {0.1f, -0.4f, 0.15f}};
    for (int i = 0; i < 3; i++) L_disc(c, cx + K[i][0] * r * 2, cy + K[i][1] * r, r * K[i][2], cmix(c->th->accent, (RGB){60, 60, 80}, 0.4f), false);
}
static void l_sun(LCtx *c) {
    float r = (float)(c->rows / 6 > 4 ? c->rows / 6 : 4);
    float cx = c->cols * 0.66f, cy = c->rows * 0.58f + fsin(c->time * 0.2f) * 0.4f;
    L_disc(c, cx, cy, r, cmix(c->th->accent, (RGB){255, 255, 235}, 0.5f), true);
    for (int k = 0; k < (int)(r * 2); k += 3)
        for (int x = (int)(cx - r * 2); x <= (int)(cx + r * 2); x++) {
            int y = (int)(cy + r * 0.2f + k * 0.6f);
            if (y >= 0 && y < c->rows && x >= 0 && x < c->cols && fhypot((x - cx) / 2.0f, y - cy) <= r) {
                Cell *cl = L_back(c, x, y);
                cl->g = (uint16_t)G_SPACE;
                cl->fg = rgb15((RGB){0, 0, 0});
                cl->bg = rgb15(L_rc(c, y));
            }
        }
}
static uint32_t str_seed_hash(const char *s);
static void l_city(LCtx *c) {
    PyRandom r;
    rng_seed(&r, str_seed_hash(c->th->key));
    int x = 0, hz = (int)(c->rows * 0.76f);
    while (x < c->cols) {
        int bw = rng_randint(&r, 4, 9), bh = rng_randint(&r, c->rows / 8, c->rows / 3);
        for (int yy = hz - bh; yy < hz; yy++)
            for (int xx = x; xx < (x + bw < c->cols ? x + bw : c->cols); xx++) {
                bool lit = (xx * 7 + yy * 13 + x) % 5 < 2 && (xx - x) % 2 == 1 && yy % 2 == 0 && ((int)(c->time * 0.4f) + xx + yy) % 11 != 0;
                L_cell(c, xx, yy, lit ? cmix(c->th->accent, (RGB){255, 230, 160}, 0.5f) : cmix(c->th->far, (RGB){0, 0, 0}, 0.45f));
            }
        x += bw + rng_randint(&r, 0, 2);
    }
}
static void l_mountains(LCtx *c) {
    int rows = c->rows, hz = (int)(rows * 0.76f);
    for (int x = 0; x < c->cols; x++) {
        int h = (int)(rows * 0.2f + rows * 0.12f * fsin(x * 0.045f + 1) + rows * 0.07f * fsin(x * 0.11f) + 3 * fabsf(fsin(x * 0.02f)));
        for (int k = 0; k < h; k++) {
            bool cap = k >= h - 2 && h > rows * 0.24f;
            L_cell(c, x, hz - k, cap ? cmix(c->th->far, WHITE, 0.65f) : cmix(c->th->far, (RGB){0, 0, 0}, 0.25f + 0.2f * k / (float)(h > 1 ? h : 1)));
        }
    }
}
static void l_grid(LCtx *c) {
    int rows = c->rows, hz = (int)(rows * 0.72f);
    for (int y = hz; y < rows; y++) {
        float k = (y - hz) / (float)(rows - hz > 1 ? rows - hz : 1);
        for (int x = 0; x < c->cols; x++) {
            bool on_h = (int)((k * 8 + c->time * 0.9f) * 2) % 4 == 0;
            float vx = (x - c->cols / 2.0f) / fmaxf(0.3f, 0.25f + k * 1.6f);
            bool on_v = fabsf(vx - roundf(vx / 6) * 6) < 0.5f;
            RGB bg = L_rc(c, y);
            L_cell(c, x, y, (on_h || on_v) ? cmix(bg, c->th->accent, 0.45f + 0.4f * k) : cmix(bg, (RGB){0, 0, 0}, 0.55f));
        }
    }
}
static void l_ocean(LCtx *c) {
    int rows = c->rows, hz = (int)(rows * 0.7f);
    for (int y = hz; y < rows; y++) {
        float k = (y - hz) / (float)(rows - hz > 1 ? rows - hz : 1);
        for (int x = 0; x < c->cols; x++) {
            float w = fsin(x * 0.35f + c->time * 1.6f + y * 0.9f) + fsin(x * 0.13f - c->time * 0.7f);
            RGB base = cmix(c->th->far, (RGB){0, 0, 0}, 0.3f + 0.3f * k);
            if (w > 1.25f) L_cellch(c, x, y, cmix(base, c->th->accent, 0.35f), '~', cmix(base, c->th->accent, 0.8f));
            else L_cell(c, x, y, base);
        }
    }
}
static void l_planet(LCtx *c) {
    float r = (float)(c->rows / 5 > 5 ? c->rows / 5 : 5);
    float cx = c->cols * 0.74f, cy = c->rows * 0.28f;
    L_disc(c, cx, cy, r, cmix(c->th->far, c->th->accent, 0.5f), false);
    for (int x = (int)(cx - r * 3.3f); x <= (int)(cx + r * 3.3f); x++) {
        int y = (int)(cy + (x - cx) * 0.12f);
        if (fabsf((x - cx) / 2.0f) > r * 0.4f && y >= 0 && y < c->rows && x >= 0 && x < c->cols) {
            L_cellch(c, x, y, cmix(c->th->accent, WHITE, 0.4f), 0x2500, cmix(c->th->accent, WHITE, 0.4f));
            L_cellch(c, x, y + 1, cmix(c->th->accent, WHITE, 0.2f), 0x2500, cmix(c->th->accent, WHITE, 0.2f));
        }
    }
}
static void l_pyramids(LCtx *c) {
    int hz = (int)(c->rows * 0.76f);
    float P[3][2] = {{c->cols * 0.25f, c->rows * 0.28f}, {c->cols * 0.55f, c->rows * 0.4f}, {c->cols * 0.82f, c->rows * 0.22f}};
    for (int i = 0; i < 3; i++) {
        float cx = P[i][0], h = P[i][1];
        for (int k = 0; k < (int)h; k++) {
            int hw = (int)((h - k) * 1.9f);
            for (int x = (int)(cx - hw); x <= (int)(cx + hw); x++)
                L_cell(c, x, hz - k, x >= cx ? cmix(c->th->far, WHITE, 0.18f) : cmix(c->th->far, (RGB){0, 0, 0}, 0.3f));
        }
    }
}
static void l_trees(LCtx *c) {
    char key[40];
    snprintf(key, sizeof key, "%st", c->th->key);
    PyRandom r;
    rng_seed(&r, str_seed_hash(key));
    int hz = (int)(c->rows * 0.77f), x = 1;
    while (x < c->cols - 2) {
        int h = rng_randint(&r, c->rows / 8, c->rows / 4);
        for (int k = 0; k < h; k++) {
            int hw = (int)((h - k) * 0.55f) + (k % 3 == 0 ? 1 : 0);
            for (int xx = x - hw; xx <= x + hw; xx++) L_cell(c, xx, hz - k, cmix(c->th->far, (RGB){0, 0, 0}, 0.5f));
        }
        x += rng_randint(&r, 3, 7);
    }
}
static void l_rain(LCtx *c) {
    for (int i = 0; i < c->cols / 2; i++) {
        int x0 = (i * 37) % c->cols, sp = 14 + (i * 5) % 9;
        int y = (int)fmodf(c->time * sp + i * 11, (float)c->rows);
        int x = ((int)(x0 - y * 0.3f) % c->cols + c->cols) % c->cols;
        if (y >= 0 && y < c->rows) {
            RGB bg = L_bg(c, x, y);
            L_put(c, x, y, 0x2571, cmix(bg, (RGB){200, 220, 255}, 0.55f), &bg);
        }
    }
}
static void l_lightning(LCtx *c) {
    float period = 3.2f;
    int k = (int)(c->time / period);
    float local = c->time - k * period;
    if (local > 0.3f) return;
    PyRandom r;
    rng_seed(&r, (uint32_t)k);
    int x = rng_randint(&r, c->cols / 6, c->cols - c->cols / 6), y = 0;
    while (y < (int)(c->rows * 0.76f)) {
        L_cellch(c, x, y, (RGB){240, 245, 255}, local < 0.15f ? 0x2588 : 0x2593, (RGB){240, 245, 255});
        x += ((int)rng_below(&r, 3) - 1) * 2;
        y++;
    }
}
static void l_tower(LCtx *c) {
    int hz = (int)(c->rows * 0.76f), cx = (int)(c->cols * 0.3f);
    for (int k = 0; k < (int)(c->rows * 0.7f); k++) {
        int hw = (int)(3 - k * 3.0f / (c->rows * 0.7f)) + (k % 6 == 0 ? 1 : 0);
        if (hw < 0) hw = 0;
        for (int x = cx - hw; x <= cx + hw; x++) L_cell(c, x, hz - k, cmix(c->th->far, (RGB){0, 0, 0}, 0.35f));
        if (k % 5 == 2) L_cell(c, cx, hz - k, cmix(c->th->accent, WHITE, 0.4f));
    }
    if ((int)(c->time * 2) % 2 == 0) L_cell(c, cx, hz - (int)(c->rows * 0.7f), (RGB){255, 80, 80});
}
static void l_comet(LCtx *c) {
    for (int i = 0; i < 3; i++) {
        float k = fmodf(c->time * 0.18f + i * 0.37f, 1.0f);
        int x = (int)(c->cols * (1.1f - k * 1.3f)), y = (int)(c->rows * (0.05f + k * 0.5f + i * 0.07f));
        for (int s = 0; s < 10; s++) {
            RGB col = cmix(L_rc(c, y), WHITE, 0.9f * (1 - s / 10.0f));
            L_cellch(c, x + s * 2, y - s / 2, col, 0x2500, col);
        }
    }
}
static void l_glitch(LCtx *c) {
    PyRandom r;
    rng_seed(&r, (uint32_t)(int)(c->time * 6));
    for (int k = 0; k < 3; k++) {
        int y = (int)rng_below(&r, (uint32_t)c->rows), w = rng_randint(&r, c->cols / 8, c->cols / 3), x0 = (int)rng_below(&r, (uint32_t)c->cols);
        for (int x = x0; x < (x0 + w < c->cols ? x0 + w : c->cols); x++) {
            Cell *cl = L_back(c, x, y);
            Px fg = cl->fg, bg = cl->bg;
            cl->fg = bg;
            cl->bg = rgb15(cmix(px_rgb(fg), c->th->accent, 0.5f));
        }
    }
}
static void l_volcano(LCtx *c) {
    int hz = (int)(c->rows * 0.78f);
    float pulse = 0.5f + 0.5f * fsin(c->time * 1.6f);
    float V[2][3] = {{c->cols * 0.28f, c->rows * 0.46f, 2.4f}, {c->cols * 0.74f, c->rows * 0.36f, 2.0f}};
    for (int i = 0; i < 2; i++) {
        float cx = V[i][0], h = V[i][1], wd = V[i][2];
        for (int k = 0; k < (int)h; k++) {
            int hw = (int)((h - k) * wd) + 1;
            for (int x = (int)(cx - hw); x <= (int)(cx + hw); x++) {
                RGB base = cmix(c->th->far, (RGB){0, 0, 0}, 0.35f);
                if (k > h - 4 && fabsf(x - cx) < 4) base = cmix((RGB){255, 90, 20}, (RGB){255, 210, 90}, pulse);
                L_cell(c, x, hz - k, base);
            }
        }
        for (int k = 0; k < (int)(h * 0.7f); k++) {
            float f = 0.5f + 0.5f * fsin(c->time * 3 + k * 0.6f);
            L_cell(c, (int)(cx + fsin(k * 0.5f) * 1.3f), hz - (int)h + 3 + k, cmix((RGB){200, 50, 14}, (RGB){255, 190, 80}, f));
        }
    }
}
static void l_isles(LCtx *c) {
    static const float I[3][3] = {{0.15f, 0.30f, 9}, {0.5f, 0.22f, 13}, {0.82f, 0.34f, 10}};
    for (int i = 0; i < 3; i++) {
        int rw = (int)I[i][2];
        int cx = (int)(c->cols * I[i][0]);
        int cy = (int)(c->rows * I[i][1] + fsin(c->time * 0.7f + I[i][0] * 9) * 1.0f);
        for (int dx = -rw; dx <= rw; dx++) L_cell(c, cx + dx, cy, cmix(c->th->accent, WHITE, 0.2f));
        for (int k = 1; k <= rw / 2; k++) {
            int half = rw - k * 2;
            for (int dx = -half; dx <= half; dx++) L_cell(c, cx + dx, cy + k, cmix(c->th->far, c->th->accent, 0.25f));
        }
        for (int k = 1; k < 7; k++) {
            float f = 0.5f + 0.5f * fsin(c->time * 8 + k);
            L_cellch(c, cx, cy + rw / 2 + k, cmix((RGB){120, 200, 240}, (RGB){230, 250, 255}, f), k % 2 ? 0x2502 : 0x254E, (RGB){200, 240, 255});
        }
    }
}
static void l_riftring(LCtx *c) {
    float cx = c->cols * 0.5f, cy = c->rows * 0.42f, R = c->rows * 0.34f;
    float RR[2][2] = {{R, 0.35f}, {R * 0.72f, -0.55f}};
    for (int q = 0; q < 2; q++) {
        float rad = RR[q][0], spd = RR[q][1];
        int n = (int)(rad * 14);
        for (int i = 0; i < n; i++) {
            float ang = i / (float)n * TAU_F + c->time * spd;
            L_cell(c, (int)(cx + fcos(ang) * rad * 2.0f), (int)(cy + fsin(ang) * rad), fsin(ang) > 0 ? (RGB){255, 240, 190} : (RGB){170, 110, 255});
        }
    }
    for (int k = 0; k < 12; k++) {
        float ang = k * TAU_F / 12 + c->time * 0.15f;
        for (int j = (int)(R * 0.4f); j < (int)(R * 1.5f); j += 2) {
            int x = (int)(cx + fcos(ang) * j * 2.0f), y = (int)(cy + fsin(ang) * j);
            if (x >= 0 && x < c->cols && y >= 0 && y < c->rows) L_put(c, x, y, 0xB7, k % 2 ? (RGB){255, 236, 190} : (RGB){150, 100, 240}, NULL);
        }
    }
    float r0 = R * 0.28f;
    for (int yy = (int)(cy - r0); yy <= (int)(cy + r0); yy++)
        for (int xx = (int)(cx - r0 * 2); xx <= (int)(cx + r0 * 2); xx++) {
            float d = fhypot((xx - cx) / 2.0f, yy - cy);
            if (d <= r0) L_cell(c, xx, yy, (RGB){6, 2, 16});
            else if (d <= r0 + 0.9f) L_cell(c, xx, yy, cmix((RGB){255, 200, 100}, (RGB){255, 250, 220}, 0.5f + 0.5f * fsin(c->time * 5)));
        }
}
static void l_hourglass(LCtx *c) {
    int cx = (int)(c->cols * 0.5f), top = (int)(c->rows * 0.1f), H = (int)(c->rows * 0.6f);
    int Wd = (int)(c->rows * 0.3f) > 6 ? (int)(c->rows * 0.3f) : 6;
    float level = fmodf(c->time * 0.06f, 1.0f);
    for (int k = 0; k < H; k++) {
        float f = fabsf(H / 2.0f - k) / (H / 2.0f);
        int hw = (int)(3 + f * Wd);
        int y = top + k;
        bool sand = (k < H / 2.0f && k > (H / 2.0f) * level) || (k >= H / 2.0f && (k - H / 2.0f) / (H / 2.0f) > 1 - level);
        for (int dx = -hw; dx <= hw; dx++) {
            bool edge = abs(dx) >= hw - 1 || k == 0 || k == H - 1;
            if (edge) L_cell(c, cx + dx * 2, y, (RGB){255, 226, 150});
            else if (sand) L_cell(c, cx + dx * 2, y, cmix((RGB){200, 140, 50}, (RGB){255, 230, 150}, 0.5f + 0.5f * fsin(c->time * 2 + dx + k)));
        }
        if (fabsf(H / 2.0f - k) < H / 2.0f * 0.5f && level > 0 && level < 1)
            L_put(c, cx, y, (int)(c->time * 12 + k) % 2 ? 0x2502 : 0x254E, (RGB){255, 236, 180}, NULL);
    }
    for (int i = 0; i < 80; i++) {
        float ang = i / 80.0f * TAU_F + c->time * 0.4f;
        L_cell(c, (int)(cx + fcos(ang) * Wd * 3.2f), (int)(top + H / 2.0f + fsin(ang) * H * 0.6f),
               cmix((RGB){255, 226, 150}, WHITE, 0.5f + 0.5f * fsin(ang * 5 + c->time * 3)));
    }
}
static void l_sunburst(LCtx *c) {
    float cx = c->cols * 0.5f, cy = c->rows * 0.45f, R = c->rows * 0.28f;
    for (int k = 0; k < 18; k++) {
        float ang = k * TAU_F / 18 + c->time * 0.12f;
        for (int j = (int)(R * 1.1f); j < (int)(R * 3.2f); j += 2) {
            int x = (int)(cx + fcos(ang) * j * 2.0f), y = (int)(cy + fsin(ang) * j);
            if (x >= 0 && x < c->cols && y >= 0 && y < c->rows) L_put(c, x, y, k % 2 ? 0xB7 : 0x2219, k % 3 ? (RGB){255, 226, 130} : (RGB){255, 255, 235}, NULL);
        }
    }
    for (int yy = (int)(cy - R); yy <= (int)(cy + R); yy++)
        for (int xx = (int)(cx - R * 2); xx <= (int)(cx + R * 2); xx++) {
            float d = fhypot((xx - cx) / 2.0f, yy - cy);
            if (d <= R) L_cell(c, xx, yy, cmix((RGB){255, 250, 220}, (RGB){255, 190, 70}, d / R * 0.8f + 0.1f * fsin(c->time * 2 + d)));
            else if (d <= R + 1.0f) L_cell(c, xx, yy, cmix((RGB){255, 210, 90}, WHITE, 0.5f + 0.5f * fsin(c->time * 4)));
        }
    int n = (int)(R * 12);
    for (int i = 0; i < n; i++) {
        float ang = i / (float)n * TAU_F - c->time * 0.5f;
        L_cell(c, (int)(cx + fcos(ang) * R * 1.45f * 2.0f), (int)(cy + fsin(ang) * R * 1.45f), (RGB){255, 226, 130});
    }
}
static void l_arcane(LCtx *c) {
    float cx = c->cols * 0.5f, cy = c->rows * 0.42f, R = c->rows * 0.36f;
#define P_(a, rad, X, Y) do { X = (int)(cx + fcos(a) * (rad) * 2.0f); Y = (int)(cy + fsin(a) * (rad)); } while (0)
    float RR[2][2] = {{R * 1.08f, 0.15f}, {R * 0.92f, -0.2f}};
    for (int q = 0; q < 2; q++) {
        float rad = RR[q][0], spd = RR[q][1];
        int n = (int)(rad * 16);
        for (int i = 0; i < n; i++) {
            float ang = i / (float)n * TAU_F + c->time * spd;
            int x, y;
            P_(ang, rad, x, y);
            if (x >= 0 && x < c->cols && y >= 0 && y < c->rows && i % 3 != 0) L_put(c, x, y, i % 2 ? 0x25E6 : 0xB7, (RGB){255, 226, 150}, NULL);
        }
    }
    for (int tri = 0; tri < 2; tri++) {
        int px[3], py[3];
        for (int k = 0; k < 3; k++) P_(c->time * 0.25f + tri * PI_F / 3 + k * TAU_F / 3, R * 0.9f, px[k], py[k]);
        for (int k = 0; k < 3; k++) {
            int x0 = px[k], y0 = py[k], x1 = px[(k + 1) % 3], y1 = py[(k + 1) % 3];
            for (int j = 0; j < 40; j++) {
                float f = j / 40.0f;
                int x = (int)(x0 + (x1 - x0) * f), y = (int)(y0 + (y1 - y0) * f);
                if (x >= 0 && x < c->cols && y >= 0 && y < c->rows) L_put(c, x, y, 0xB7, tri ? (RGB){255, 240, 190} : (RGB){190, 140, 255}, NULL);
            }
        }
    }
    for (int i = 0; i < 6; i++) {
        float ang = c->time * 0.7f + i * TAU_F / 6;
        int x, y;
        P_(ang, R * 1.08f, x, y);
        for (int dx = -1; dx <= 1; dx++) L_cell(c, x + dx, y, ELEMENTS[i].color);
    }
#undef P_
}
/* DS: the original's sanctum (mage, artifact, orbiting orbs, a rune column and a rune ring) is far too busy for a
 * 51-column screen; the column and the ring are left out and the mage is drawn smaller, into a cached canvas. */
static void l_sanctum(LCtx *c) {
    int cols = c->cols, rows = c->rows;
    int mx = (int)(cols * 0.88f);
    int rid = c->b->ridge[mx < cols - 1 ? mx : cols - 1];
    float sc = 0.9f, tm_s = 0.9f;
    int tcy = rows * 2 - (int)(rows * 2 * 0.20f);
    static const RGB ORB[3] = {{255, 120, 70}, {110, 190, 255}, {150, 255, 170}};
    for (int k = 0; k < 3; k++) {
        float ang = c->time * 0.8f + k * TAU_F / 3;
        int x = mx + (int)(fcos(ang) * 9 * tm_s);
        int y = rows - 1 - tcy / 2 + (int)(fsin(ang) * 3 * tm_s);
        if (x >= 0 && x < cols - 1 && y >= 0 && y < rows - rid) {
            RGB bg = L_rc(c, y);
            L_put(c, x, y, 0x25CF, ORB[k], &bg);
        }
    }
    static Canvas cv;
    static int age;
    if (!cv.s.px) { canvas_alloc(&cv, cols, rows * 2); age = 0; }
    if (age++ % 3 == 0) {   /* the mage is the costliest sprite in the game: animate it at a third of the backdrop rate */
        memset(cv.s.px, 0, sizeof(Px) * (size_t)(cv.s.w * cv.s.h));
        draw_artifact(&cv.s, (float)mx, (float)tcy, tm_s, c->time);
        draw_mage(&cv.s, (float)mx, (float)(rid * 2 + 2), sc, c->time, 1.0f, true);
    }
    term_blit_masked(c->t, &cv.s, 0, LOFF, mx - 34 > 0 ? mx - 34 : 0, mx + 34 < cols ? mx + 34 : cols);
}
static void l_twinsuns(LCtx *c) {
    int cols = c->cols, rows = c->rows;
    float S[2][3] = {{0.26f, 0.34f, (float)(rows / 6 > 3 ? rows / 6 : 3)}, {0.44f, 0.45f, (float)(rows / 10 > 2 ? rows / 10 : 2)}};
    RGB SC[2] = {{255, 236, 176}, {255, 150, 110}};
    for (int i = 0; i < 2; i++) L_disc(c, (float)(int)(cols * S[i][0]), (float)(int)(rows * S[i][1]), S[i][2], SC[i], true);
    float XF[2] = {0.26f, 0.44f};
    for (int k = 0; k < 2; k++) {
        int x = (int)(cols * XF[k]);
        for (int y = (int)(rows * 0.5f); y < rows - 4; y++)
            if ((y + (int)(c->time * 6)) % 3 == 0 && x >= 0 && x < cols)
                L_put(c, x + (int)(fsin(y * 0.5f + c->time * 2) * (1 + k)), y, 0x2502, cmix(L_rc(c, y), (RGB){255, 230, 170}, 0.5f), NULL);
    }
    for (int i = 0; i < 14; i++) {
        int gx = (i * 53 + 11) % cols, gy = rows - 3 - (i * 7) % 6;
        if (((int)(c->time * 3) + i) % 5 == 0 && gy >= 0 && gy < rows) L_put(c, gx, gy, 0x2726, rainbow(c->time * 0.7f + i * 0.3f), NULL);
    }
}
static void l_lanterns(LCtx *c) {
    int cols = c->cols, rows = c->rows;
    for (int i = 0; i < 30; i++) {
        float ph = fmodf(c->time * (0.04f + (i % 5) * 0.01f) + i * 0.173f, 1.0f);
        int x = (int)((i * 37 + 9) % cols + fsin(c->time * 0.8f + i) * 2);
        int y = (int)(rows * 0.9f - ph * rows * 0.88f);
        if (!(x >= 1 && x < cols - 3 && y >= 2 && y < rows - 4)) continue;
        float glow = 0.7f + 0.3f * fsin(c->time * 3 + i * 1.7f);
        static const RGB B[3] = {{255, 90, 50}, {255, 190, 80}, {255, 236, 200}};
        RGB col = cshade(B[i % 3], 0.7f + 0.3f * glow);
        for (int dy = -2; dy <= 2; dy++)
            for (int dx = -3; dx <= 3; dx++)
                if (abs(dx) + abs(dy) * 2 <= 4 && (dx || dy) && y + dy >= 0 && y + dy < rows && x + dx >= 0 && x + dx < cols)
                    L_cell(c, x + dx, y + dy, cmix(L_rc(c, y + dy), col, 0.14f * glow));
        for (int dx = 0; dx < 2; dx++) {
            L_cellch(c, x + dx, y, col, 0x2580, cmix(col, WHITE, 0.3f));
            L_cellch(c, x + dx, y + 1, cshade(col, 0.8f), 0x2584, cshade(col, 0.55f));
        }
        L_put(c, x, y - 1, 0x2582, (RGB){90, 40, 20}, NULL);
        L_put(c, x, y + 2, 0x2575, cshade(col, 0.6f), NULL);
    }
}
static void l_worldtree(LCtx *c) {
    int cols = c->cols, rows = c->rows;
    int tx = (int)(cols * 0.13f);
    for (int y = 0; y < rows - 3; y++) {
        int w_ = 5 + (int)((y / (float)(rows > 1 ? rows : 1)) * 9) + (int)(fsin(y * 0.5f) * 1.2f);
        for (int dx = -w_; dx <= w_; dx++) {
            float u = abs(dx) / (float)(w_ > 1 ? w_ : 1);
            if (tx + dx >= 0 && tx + dx < cols)
                L_cell(c, tx + dx, y, cmix((RGB){26, 60, 50}, (RGB){6, 20, 22}, fsqrt(u) * 0.9f + (((dx + y / 3) % 4 + 4) % 4 == 0 ? 0.12f : 0)));
        }
        float pulse = fmodf(y * 0.35f - c->time * 3, 9.0f);
        if (pulse < 0) pulse += 9;
        for (int k = -2; k <= 2; k++)
            if (pulse < 1.5f && tx + k * 2 >= 0 && tx + k * 2 < cols)
                L_cell(c, tx + k * 2 + (int)(fsin(y * 0.5f) * 1.5f), y, cmix((RGB){30, 120, 90}, (RGB){150, 255, 210}, 1 - pulse / 1.5f));
    }
    int cy = (int)(rows * 0.3f) > 4 ? (int)(rows * 0.3f) : 4;
    for (int y = 0; y < cy; y++)
        for (int x = 0; x < cols; x++) {
            float v = fsin(x * 0.16f + y * 0.9f + c->time * 0.15f) + fsin(x * 0.06f - y * 0.55f + 1.3f) * 0.9f + fcos(x * 0.11f + c->time * 0.1f) * 0.5f;
            if (v > 0.45f - y * 0.05f) {
                float k = clampf((v - 0.2f) / 2.2f, 0, 1);
                L_cellch(c, x, y, cmix(L_rc(c, y), cmix((RGB){14, 90, 70}, (RGB){80, 230, 160}, k), 0.55f + 0.35f * k), k > 0.5f ? 0x2592 : 0x2591,
                         cmix((RGB){120, 255, 200}, WHITE, k * 0.5f));
            }
        }
    for (int i = 0; i < 22; i++) {
        float ph = fmodf(c->time * 0.06f + i * 0.137f, 1.0f);
        int x = (int)((i * 41 + 7) % cols + fsin(c->time + i) * 3);
        int y = (int)(ph * (rows - 4));
        if (x >= 0 && x < cols && y >= 0 && y < rows - 3) L_put(c, x, y, i % 2 ? 0x273F : 0x2740, cmix((RGB){150, 255, 200}, WHITE, 0.5f + 0.5f * fsin(c->time * 4 + i)), NULL);
    }
}
static void l_whale(LCtx *c) {
    int cols = c->cols, rows = c->rows;
    static const float WH[2][5] = {{1.0f, 0.30f, 0.010f, 0.25f, 0.0f}, {0.4f, 0.62f, 0.016f, 0.55f, 0.5f}};
    for (int q = 0; q < 2; q++) {
        float scale = WH[q][0], yf = WH[q][1], spd = WH[q][2], off = WH[q][3], dim = WH[q][4];
        int L_ = (int)(cols * 0.98f * scale) > 20 ? (int)(cols * 0.98f * scale) : 20;
        int x0 = (int)((cols + L_) * fmodf(off + c->time * spd, 1.0f) - L_);
        int cy = (int)(rows * yf + fsin(c->time * 0.3f) * 1.3f);
        for (int i = 0; i < L_; i++) {
            float u = i / (float)L_;
            float hgt = powf(fmaxf(0.0f, fsin(fminf(1.0f, u * 1.08f) * PI_F)), 0.7f) * (rows * 0.26f * scale) * (1.0f - 0.6f * fmaxf(0.0f, u - 0.7f) / 0.3f);
            if (u > 0.86f) hgt += (u - 0.86f) * 20 * scale * (0.5f + 0.5f * fsin(c->time * 1.6f));
            int x = x0 + i;
            for (int dy = -(int)hgt; dy <= (int)(hgt * 0.75f); dy++)
                if (x >= 0 && x < cols && cy + dy >= 0 && cy + dy < rows - 3) {
                    float f = (dy + hgt) / fmaxf(1.0f, hgt * 1.75f);
                    RGB col = cmix((RGB){18, 34, 84}, (RGB){150, 190, 235}, powf(fmaxf(f, 0), 1.5f));
                    L_cell(c, x, cy + dy, cmix(L_rc(c, cy + dy), col, 1.0f - dim * 0.6f));
                }
            if (i % 6 == 0 && x >= 0 && x < cols && cy - (int)(hgt * 0.35f) >= 0 && cy - (int)(hgt * 0.35f) < rows)
                L_put(c, x, cy - (int)(hgt * 0.35f), 0x2022, cshade((RGB){150, 235, 255}, (0.45f + 0.55f * fsin(c->time * 2 + i)) * (1 - dim * 0.5f)), NULL);
            if ((int)(hgt * 0.7f) > 1 && i % 3 == 0 && x >= 0 && x < cols && cy + (int)(hgt * 0.45f) >= 0 && cy + (int)(hgt * 0.45f) < rows)
                L_put(c, x, cy + (int)(hgt * 0.45f), 0x2500, (RGB){170, 205, 245}, NULL);
        }
        int ex = x0 + (int)(L_ * 0.13f);
        if (ex >= 0 && ex < cols - 1 && cy >= 0 && cy < rows) L_cellch(c, ex, cy - 1, (RGB){240, 250, 255}, 0x25CF, (RGB){20, 40, 90});
    }
}
static void l_shards(LCtx *c) {
    int cols = c->cols, rows = c->rows;
    int mx_ = (int)(cols * 0.5f), my_ = (int)(rows * 0.34f);
    for (int a = 0; a < 9; a++) {
        float ang = a * TAU_F / 9 + 0.4f;
        for (int s = 3; s < (int)(rows * 0.62f); s++) {
            int x = mx_ + (int)(fcos(ang) * s * 2), y = my_ + (int)(fsin(ang) * s);
            if (x >= 0 && x < cols && y >= 0 && y < rows - 3 && (s + a) % 3) L_put(c, x, y, a % 2 ? 0x2571 : 0x2572, (RGB){214, 204, 255}, NULL);
        }
    }
    for (int i = 0; i < 12; i++) {
        int cx_ = (int)((i * 61 + 5) % cols + fsin(c->time * 0.3f + i) * 3);
        int cy_ = (int)(rows * (0.1f + (i * 29 % 50) / 100.0f) + fsin(c->time * 0.5f + i * 2) * 1.5f);
        int w_ = 5 + (i * 7) % 7;
        float tilt = fsin(c->time * 0.6f + i);
        for (int dx = -w_; dx <= w_; dx++)
            for (int dy = -2; dy <= 2; dy++)
                if (abs(dx) + abs(dy) * 3 <= w_ + 1 && cx_ + dx >= 0 && cx_ + dx < cols && cy_ + dy >= 0 && cy_ + dy < rows - 3) {
                    float k = (dx * tilt * 0.25f + dy * 0.4f) + c->time * 0.35f + i * 0.2f;
                    bool edge = abs(dx) + abs(dy) * 3 >= w_;
                    L_cellch(c, cx_ + dx, cy_ + dy, cmix(L_rc(c, cy_ + dy), rainbow(k), edge ? 0.7f : 0.30f), ((dx + dy + i) % 5 + 5) % 5 == 0 ? 0x25C6 : ' ', WHITE);
                }
    }
}

typedef void (*LayerFn)(LCtx *);
#include "home_layers2.inc"

static const LayerFn LAYERS[LAYER_COUNT] = {
    [LAYER_TWINSUNS] = l_twinsuns, [LAYER_LANTERNS] = l_lanterns, [LAYER_WORLDTREE] = l_worldtree, [LAYER_WHALE] = l_whale,
    [LAYER_SHARDS] = l_shards, [LAYER_SANCTUM] = l_sanctum, [LAYER_MOON] = l_moon, [LAYER_SUN] = l_sun, [LAYER_CITY] = l_city,
    [LAYER_MOUNTAINS] = l_mountains, [LAYER_GRID] = l_grid, [LAYER_OCEAN] = l_ocean, [LAYER_PLANET] = l_planet,
    [LAYER_PYRAMIDS] = l_pyramids, [LAYER_TREES] = l_trees, [LAYER_RAIN] = l_rain, [LAYER_LIGHTNING] = l_lightning,
    [LAYER_TOWER] = l_tower, [LAYER_COMET] = l_comet, [LAYER_GLITCH] = l_glitch, [LAYER_VOLCANO] = l_volcano,
    [LAYER_ISLES] = l_isles, [LAYER_RIFTRING] = l_riftring, [LAYER_HOURGLASS] = l_hourglass, [LAYER_SUNBURST] = l_sunburst,
    [LAYER_ARCANE] = l_arcane,
    [LAYER_ABYSS] = l_abyss, [LAYER_SHRINE] = l_shrine, [LAYER_HOLOCITY] = l_holocity, [LAYER_HYPER] = l_hyper, [LAYER_KAIJU] = l_kaiju,
    [LAYER_SHROOMS] = l_shrooms, [LAYER_WORMSEA] = l_wormsea, [LAYER_JADETEMPLE] = l_jadetemple, [LAYER_ORRERY] = l_orrery,
    [LAYER_ARMADA] = l_armada};

/* Python seeds random.Random(str) via sha512 of the string; we use a stable FNV hash instead (only the building and
 * tree layouts of a theme depend on it). */
static uint32_t str_seed_hash(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

static void thm_ensure(Backdrop *b) {
    if (b->built) return;
    b->built = true;
    const HomeTheme *th = &HOME_THEMES[b->home];
    int cols = T_COLS, rows = T_ROWS;
    PyRandom *r = &b->rng;
    float st[4];
    RGB sc[4];
    for (int i = 0; i < th->nsky; i++) { st[i] = (float)th->sky[i].t; sc[i] = th->sky[i].c; }
    for (int y = 0; y < rows; y++) b->rowcol[y] = cgradient(st, sc, th->nsky, clampf(1 - y / (float)(rows - 1), 0, 1));
    b->ntstars = 0;
    if (th->stars) {
        int n = cols * rows / 55;
        if (n > 64) n = 64;
        for (int i = 0; i < n; i++) {
            b->tstars[i][0] = (float)rng_below(r, (uint32_t)cols);
            b->tstars[i][1] = (float)rng_below(r, (uint32_t)(int)(rows * 0.62f));
            b->tstars[i][2] = U(r, 0, TAU_F);
        }
        b->ntstars = n;
    }
    const MoteStyle *ms = &MOTE_STYLES[th->fx];
    b->nmotes = 0;
    if (ms->on) {
        int n = (int)(cols * rows * ms->density * 3) + 8;
        if (n > 60) n = 60;   /* DS: fewer motes on the small screens (the original caps at 160 on a wide terminal) */
        for (int i = 0; i < n; i++) {
            float *m = b->motes[i];
            m[0] = U(r, 0, cols); m[1] = U(r, 0, rows); m[2] = U(r, 0.6f, 1.4f); m[3] = U(r, 0, TAU_F); m[4] = U(r, 0.5f, 2.5f);
            m[5] = (float)rng_below(r, 3);
        }
        b->nmotes = n;
    }
    for (int x = 0; x < cols; x++) b->ridge[x] = (int)(LROWS * 0.27f + 1.6f * fsin(x * 0.06f + 1) + 1.1f * fsin(x * 0.17f));
    canvas_alloc(&b->cv, cols, 72);
    b->timer = 1.2f;
}

static void thm_update(Backdrop *b, float dt) {
    if (!b->built) return;
    const HomeTheme *th = &HOME_THEMES[b->home];
    b->clock += dt;
    b->recoil = fmaxf(0, b->recoil - dt);
    b->timer -= dt;
    if (b->timer <= 0) {
        b->timer = U(&b->rng, 2.2f, 3.4f);
        hero_fire(b, th->hero, 8.0f, 3, 40, 72);
    }
    for (int i = 0; i < b->nshells;) {
        Shell *s = &b->shells[i];
        s->t += dt;
        float x, y;
        shell_at(s, &x, &y);
        if (y < 3.0f && s->t > 0.2f) {
            b->shells[i] = b->shells[--b->nshells];
            add_expl(b, x, 3.0f, 5.0f, th->accent, 0.9f, AMMOS[th->hero_ammo].boom);
            continue;
        }
        i++;
    }
    update_expl(b, dt);
}

/* the corona's geometry never changes: cells, distances and angles are tabulated once */
typedef struct { int8_t x, y; uint8_t inner, k6, a8; } CoronaCell;   /* a8: angle / TAU in 1/256 */
static CoronaCell g_corona[1400];
static int g_ncorona = -1;
static void corona_build(void) {
    int cols = T_COLS, rows = T_ROWS;
    int cx = cols - 10, cy = (int)(rows * 0.3f), R = rows / 6 > 4 ? rows / 6 : 4;
    g_ncorona = 0;
    for (int dy = -R - 3; dy < R + 4; dy++)
        for (int dx = -2 * (R + 3); dx <= 2 * (R + 3); dx++) {
            int x = cx + dx, y = cy + dy;
            if (!(x >= 0 && x < cols && y >= 0 && y < rows) || g_ncorona >= 1400) continue;
            float d = sqrtf(dx * dx / 4.0f + (float)(dy * dy));
            CoronaCell *c = &g_corona[g_ncorona];
            c->x = (int8_t)x;
            c->y = (int8_t)y;
            if (d <= R) { c->inner = 1; g_ncorona++; continue; }
            if (d > R + 2.6f) continue;
            c->inner = 0;
            c->k6 = (uint8_t)roundf((1 - (d - R) / 2.6f) * 6);
            float a = atan2f((float)dy, dx / 2.0f) / TAU_F;
            if (a < 0) a += 1;
            c->a8 = (uint8_t)((int)(a * 256) & 255);
            g_ncorona++;
        }
}
static void thm_corona(Backdrop *b, Term *t, float time) {
    if (g_ncorona < 0) corona_build();
    static const RGB BLACK_IN = {2, 1, 8};
    int shift = (int)(time * 0.08f * 256) & 255;
    RGB hues[24];
    for (int i = 0; i < 24; i++) hues[i] = chsv(i / 24.0f, 0.55f, 1.0f);
    for (int i = 0; i < g_ncorona; i++) {
        const CoronaCell *c = &g_corona[i];
        if (c->inner) { term_put(t, c->x, c->y, ' ', (RGB){0, 0, 0}, &BLACK_IN); continue; }
        int hue = (((c->a8 + shift) & 255) * 24 + 128) >> 8;
        float k = c->k6 / 6.0f;
        RGB row = b->rowcol[c->y];
        RGB bg = cmix(row, (RGB){255, 244, 210}, 0.4f * k);
        term_put(t, c->x, c->y, k > 0.5f ? 0x2593 : 0x2591, cmix(row, hues[hue % 24], 0.35f + 0.6f * k), &bg);
    }
}

static void thm_draw(Backdrop *b, Term *t, float time) {
    thm_ensure(b);
    const HomeTheme *th = &HOME_THEMES[b->home];
    int cols = T_COLS, rows = T_ROWS;
    for (int y = 0; y < rows; y++) term_fill(t, 0, y, cols, 1, b->rowcol[y]);
    static const float LV[3] = {0.25f, 0.5f, 0.85f};
    for (int i = 0; i < b->ntstars; i++) {
        int x = (int)b->tstars[i][0], y = (int)b->tstars[i][1];
        int lvl = (int)((fsin(time * 1.6f + b->tstars[i][2]) + 1) * 1.49f);
        term_put(t, x, y, lvl < 2 ? 0xB7 : '+', cmix(b->rowcol[y], WHITE, LV[lvl]), &b->rowcol[y]);
    }
    if (th->corona) thm_corona(b, t, time);
    if (th->aurora)
        for (int bb = 0; bb < 2; bb++)
            for (int x = 0; x < cols; x++) {
                int y = (int)(rows * (0.2f + 0.11f * bb) + fsin(x * 0.07f + time * 0.5f + bb * 2.1f) * rows * 0.05f);
                if (y >= 0 && y < rows) {
                    float hue = roundf((0.45f + 0.2f * fsin(x * 0.03f + time * 0.2f + bb)) * 24) / 24;
                    term_put(t, x, y, 0x2592, cmix(b->rowcol[y], chsv(hue, 0.6f, 1.0f), 0.45f), &b->rowcol[y]);
                }
            }
    const MoteStyle *ms = &MOTE_STYLES[th->fx];
    if (ms->on)
        for (int i = 0; i < b->nmotes; i++) {
            float *m = b->motes[i];
            int x = (int)(m[0] + fsin(time * 0.8f * m[2] + m[3]) * m[4]) % cols;
            if (x < 0) x += cols;
            int yy = (int)(m[1] + time * (float)ms->speed * m[2] * 0.45f) % rows;
            if (yy < 0) yy += rows;
            int y = rows - 1 - yy;
            if (y >= 0 && y < rows) {
                static const float L3[3] = {0.5f, 0.75f, 1.0f};
                float lvl = L3[(int)((fsin(time * 5 * m[2] + m[3]) + 1) * 1.49f)];
                term_put(t, x, y, m[2] > 1.0f ? 0x2022 : 0x2219, cmix(b->rowcol[y], ms->pal[(int)m[5]], (float)ms->alpha * lvl), &b->rowcol[y]);
            }
        }
    LCtx c = {b, t, cols, LROWS, time, th};
#ifdef TT_DEBUG_PERF
    uint32_t l0 = plat_lines();
#endif
    for (int i = 0; i < th->nlayers; i++)
        if (th->layers[i] < LAYER_COUNT && LAYERS[th->layers[i]]) LAYERS[th->layers[i]](&c);
#ifdef TT_DEBUG_PERF
    { char d[32]; snprintf(d, sizeof d, " LAYER %lu MS ", (unsigned long)((plat_lines() - l0) * 6356 / 100000)); term_text(t, 0, T_SPLIT, d, (RGB){255, 255, 0}, &(RGB){0, 0, 0}); }
#endif
    for (int x = 0; x < cols; x++) {
        int h = b->ridge[x];
        term_fill(t, x, rows - h, 1, h, th->far);
    }
    /* the hero */
    Surf *cv = &b->cv.s;
    memset(cv->px, 0, sizeof(Px) * (size_t)(cv->w * cv->h));
    for (int x = 0; x < cols; x++) {
        cv_plot(cv, x, 0, rgb15(th->ground));
        cv_plot(cv, x, 1, rgb15(th->ground));
        cv_plot(cv, x, 2, rgb15(th->ground_top));
    }
    draw_tank(cv, 8.0f, 3, 1, 38 + 9 * fsin(b->clock * 0.7f), th->accent, 1, 0, false, b->recoil, &DESIGNS[th->hero], b->clock, -1, -1);
    for (int i = 0; i < b->nshells; i++) {
        float x, y;
        shell_at(&b->shells[i], &x, &y);
        draw_projectile(cv, x, y, (float)b->shells[i].l.vx, (float)b->shells[i].l.vy - 40 * b->shells[i].t, th->accent, &AMMOS[th->hero_ammo], b->clock);
    }
    for (int i = 0; i < b->nex; i++) draw_explosion(cv, &b->ex[i]);
    term_blit_masked(t, cv, 0, rows - cv->h / 2, 0, cols);
}

/* ================================================================== dispatch */
Backdrop *backdrop_new_ambient(void) { return bd_alloc(BD_AMBIENT, 11, 1); }

static Backdrop *g_master, *g_secret, *g_theme;

Backdrop *app_home_backdrop(void) {
    int mode = app_home_theme();
    if (mode == HOME_MASTER) {
        if (!g_master) g_master = bd_alloc(BD_MASTER, 21, 1);
        return g_master;
    }
    if (mode == HOME_SECRET) {
        if (!g_secret) g_secret = bd_alloc(BD_SECRET, 33, 0);
        return g_secret;
    }
    if (mode == HOME_CLASSIC) return g_app.backdrop;
    if (!g_theme || g_theme->home != mode) {   /* one themed backdrop cached at a time (RAM) */
        if (g_theme) {
            canvas_free(&g_theme->cv);
            free(g_theme);
        }
        uint32_t seed = 0;
        for (const char *p = HOME_THEMES[mode].key; *p; p++) seed += (uint8_t)*p;
        g_theme = bd_alloc(BD_THEME, seed, 0);
        g_theme->home = mode;
    }
    return g_theme;
}

void backdrop_update(Backdrop *b, float dt) {
    switch (b->kind) {
    case BD_AMBIENT: amb_update(b, dt); break;
    case BD_MASTER: mst_update(b, dt); break;
    case BD_SECRET: sec_update(b, dt); break;
    default: thm_update(b, dt); break;
    }
}

void backdrop_draw(Backdrop *b, Term *t, float time) {
    switch (b->kind) {
    case BD_AMBIENT: amb_draw(b, t, time); break;
    case BD_MASTER: mst_draw(b, t, time); break;
    case BD_SECRET: sec_draw(b, t, time); break;
    default: thm_draw(b, t, time); break;
    }
}

void backdrop_celebrate(Backdrop *b, const RGB *colors, int n) {
    if (b->kind == BD_THEME || b->kind == BD_SECRET) return;   /* those backdrops do not celebrate (as in the original) */
    b->ncelebrate = n > 8 ? 8 : n;
    for (int i = 0; i < b->ncelebrate; i++) b->celebrate[i] = colors[i];
}
