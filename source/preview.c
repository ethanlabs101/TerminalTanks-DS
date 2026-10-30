#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "preview.h"
#include "app.h"

#define PI_F 3.14159265f
#define PERIOD 3.4f

static float tank_x(const PreviewStage *p) { return 5.5f * p->scale + 6; }
static int target_x(const PreviewStage *p) { return p->w - 6; }

void preview_init(PreviewStage *p, int w, int h, int scale, uint32_t seed) {
    memset(p, 0, sizeof *p);
    p->w = w;
    p->h = h;
    p->scale = scale;
    p->ps = (Particles *)calloc(1, sizeof(Particles));
    parts_init(p->ps, seed, &g_app.settings);
    p->design = 0;
    p->ammo = 0;
    p->body = COLOR_CHOICES[0].c;
    p->shot = COLOR_CHOICES[6].c;
    p->map = -1;
    p->gy = p->ty = 4.0f;
    canvas_alloc(&p->cv, w, h);
}

void preview_free(PreviewStage *p) {
    free(p->ps);
    free(p->world);
    free(p->scn);
    canvas_free(&p->cv);
    p->ps = NULL;
    p->world = NULL;
    p->scn = NULL;
}

/* Builds a World holding just a terrain + the Scenery draws (Terrain(w, h, map, rng) then Scenery(w, h, theme, rng)). */
static void build_scan_world(World *w, int map, int width, int height, uint32_t seed, const float *flat_x, int nflat) {
    memset(w, 0, sizeof *w);
    w->width = width;
    w->height = height;
    w->map = map;
    rng_seed(&w->rng, seed);
    terrain_init(&w->terrain, width, height, &MAPDEFS[map], &w->rng);
    for (int i = 0; i < nflat; i++) terrain_flatten(&w->terrain, flat_x[i], 10);
    world_scenery_init(w);
}

void preview_set_map(PreviewStage *p, int map) {
    p->t = 0;
    p->phase = 0;
    p->nex = 0;
    p->map = map;
    if (map < 0) {
        p->gy = p->ty = 4.0f;
        return;
    }
    if (!p->world) p->world = (World *)malloc(sizeof(World));
    if (!p->scn) p->scn = (Scenery *)malloc(sizeof(Scenery));
    float fx[2] = {tank_x(p), (float)target_x(p)};
    build_scan_world(p->world, map, p->w, p->h, 5, fx, 2);
    scenery_build(p->scn, p->world);
    p->gy = (float)terrain_support(&p->world->terrain, (int)tank_x(p));
    p->ty = (float)terrain_support(&p->world->terrain, target_x(p));
}

void preview_set_kit(PreviewStage *p, int design, RGB body, int ammo, RGB shot, float dim) {
    if (design != p->design || ammo != p->ammo || !rgb_eq(body, p->body) || !rgb_eq(shot, p->shot)) {
        p->t = 0;
        p->phase = 0;
        p->ps->n = 0;
        p->nex = 0;
    }
    p->design = design;
    p->ammo = ammo;
    p->body = body;
    p->shot = shot;
    p->dim = dim;
}

static float aim_angle(const PreviewStage *p) { return 26 + 9 * fsin(p->clock * 2.2f); }

static void shell_pos(const PreviewStage *p, float u, float *x, float *y) {
    float x0 = p->fired ? p->fire_x : tank_x(p), y0 = p->fired ? p->fire_y : 8.0f;
    float x1 = (float)target_x(p), y1 = p->ty;
    float k = tanf((p->fired ? p->fire_angle : 26.0f) * PI_F / 180) * (x1 - x0) - (y1 - y0);
    *x = x0 + (x1 - x0) * u;
    *y = y0 + (y1 - y0) * u + k * u * (1 - u);
}

void preview_update(PreviewStage *p, float dt) {
    p->t += dt;
    p->clock += dt;
    p->recoil = fmaxf(0, p->recoil - dt);
    parts_update(p->ps, dt, NULL);
    const TankDesign *d = design_ptr(p->design);
    fx_ambient(p->ps, d->ambient, tank_x(p), p->gy, live_color(p->body, p->clock), dt);
    int o = 0;
    for (int i = 0; i < p->nex; i++) {
        p->ex[i].age += dt;
        if (p->ex[i].age < p->ex[i].duration) p->ex[o++] = p->ex[i];
    }
    p->nex = o;
    if (p->t > PERIOD) {
        p->t = 0;
        p->phase = 0;
    }
    if (p->phase == 0 && p->t > 0.6f) {
        p->phase = 1;
        p->recoil = 0.2f;
        float ang = aim_angle(p);
        p->fire_angle = ang;
        float lift = d->hover > 0 ? (float)d->hover + 0.6f * fsin(p->clock * 2.4f) * p->scale : 0;
        double mx, my;
        muzzle_point(tank_x(p), p->gy + lift, ang, p->design, p->scale, 0, &mx, &my);
        p->fire_x = (float)mx;
        p->fire_y = (float)my;
        p->fired = true;
    }
    if (p->phase == 1) {
        float u = (p->t - 0.6f) / 1.2f;
        float x, y;
        shell_pos(p, u, &x, &y);
        p->n++;
        RGB shot = live_color(p->shot, p->clock);
        fx_trail(p->ps, x, y, shot, AMMOS[p->ammo].trail, p->n, p->clock);
        if (u >= 1) {
            p->phase = 2;
            if (p->nex < 8) p->ex[p->nex++] = (Explosion){(double)target_x(p), p->ty, 4.5, shot, 0.8, 0, AMMOS[p->ammo].boom};
            fx_impact(p->ps, (float)target_x(p), p->ty, shot, (RGB){92, 84, 70}, 0.6f, AMMOS[p->ammo].boom);
        }
    }
}

Surf *preview_render(PreviewStage *p) {
    Surf *cv = &p->cv.s;
    int w = p->w, h = p->h, s = p->scale;
    if (p->map >= 0 && p->scn) {
        scenery_draw(p->scn, p->world, cv, p->clock, true);
    } else {
        for (int py = 0; py < h; py++) {
            Px c = rgb15(cmix((RGB){14, 22, 60}, (RGB){200, 120, 110}, powf(py / (float)h, 1.4f)));
            for (int x = 0; x < w; x++) cv->px[py * w + x] = c;
        }
        for (int x = 0; x < w; x++) {
            for (int y = 0; y < 3; y++) cv_plot(cv, x, y, rgb15(cshade((RGB){92, 84, 70}, 0.75f + 0.1f * ((x + y) % 3))));
            cv_plot(cv, x, 3, rgb15i(110, 210, 120));
        }
    }
    const TankDesign *d = design_ptr(p->design);
    draw_tank(cv, tank_x(p), p->gy, 1, p->phase != 1 ? aim_angle(p) : p->fire_angle, p->body, s, 0, false, p->recoil, d, p->clock, -1, -1);
    if (p->phase == 1) {
        float u = (p->t - 0.6f) / 1.2f, x, y, x2, y2;
        shell_pos(p, u, &x, &y);
        shell_pos(p, fminf(1.0f, u + 0.02f), &x2, &y2);
        draw_projectile(cv, x, y, x2 - x + 1e-3f, y2 - y, p->shot, &AMMOS[p->ammo], p->clock);
    }
    parts_draw(p->ps, cv);
    for (int i = 0; i < p->nex; i++) draw_explosion(cv, &p->ex[i]);
    if (p->dim > 0) {
        Px dk = rgb15i(6, 10, 16);
        for (int i = 0; i < w * h; i++) cv->px[i] = pxmix(cv->px[i], dk, p->dim);
    }
    return cv;
}

/* ------------------------------------------------------------------ terrain scan */
void mappreview_init(MapPreview *m) {
    memset(m, 0, sizeof *m);
    m->map = -1;
}
void mappreview_free(MapPreview *m) {
    free(m->world);
    free(m->scn);
    canvas_free(&m->cv);
    memset(m, 0, sizeof *m);
    m->map = -1;
}

Surf *mappreview_render(MapPreview *m, int map, int w, int h, float t, const int *designs, const RGB *colors) {
    if (m->map != map || m->w != w || m->h != h) {
        if (!m->world) m->world = (World *)malloc(sizeof(World));
        if (!m->scn) m->scn = (Scenery *)malloc(sizeof(Scenery));
        if (m->w != w || m->h != h) {
            canvas_free(&m->cv);
            canvas_alloc(&m->cv, w, h);
        }
        m->map = map;
        m->w = w;
        m->h = h;
        float fx[2];
        for (int i = 0; i < 2; i++) {
            int x = (int)(MAPDEFS[map].spawns[i] * w);
            fx[i] = x + 0.5f;
            m->xs[i] = fx[i];
        }
        build_scan_world(m->world, map, w, h, 1, fx, 2);
        scenery_build(m->scn, m->world);
    }
    Surf *cv = &m->cv.s;
    scenery_draw(m->scn, m->world, cv, t, true);
    for (int i = 0; i < 2; i++) {
        float x = (float)m->xs[i];
        draw_tank(cv, x, (float)terrain_support(&m->world->terrain, x), i == 0 ? 1 : -1, i == 0 ? 50 : 130, colors[i], 1, 0, false, 0,
                  design_ptr(designs[i]), t, -1, -1);
    }
    return cv;
}
