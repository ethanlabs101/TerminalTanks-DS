/* Particles and the effect recipes (muzzle flashes, trails, impacts, ambient tank flair, wreck smoke). Port of
 * Particle / ParticleSystem / EffectsFactory (terminaltanks.py l.2958-3634). Purely visual: it uses its own rng
 * stream (seed ^ 0x5EED, as in the original) and never touches the simulation. */
#ifndef TT_PARTICLES_H
#define TT_PARTICLES_H
#include "../core/world.h"
#include "../core/save.h"
#include "gfx.h"

#define PART_CAP 900

typedef struct {
    float x, y, vx, vy, life, max_life;
    RGB c0, c1;
    float gravity, drag, alpha, windk;
    bool solid;
    uint8_t size;
} Particle;

typedef struct {
    Particle p[PART_CAP];
    int n;
    float wind;
    PyRandom rng;
    const Settings *st;
    float smoke_timer;
} Particles;

void parts_init(Particles *ps, uint32_t seed, const Settings *st);
void parts_update(Particles *ps, float dt, const Terrain *terrain);
void parts_emit(Particles *ps, float x, float y, float vx, float vy, float life, float max_life, RGB c0, RGB c1, float gravity,
                float drag, float alpha, bool solid, int size, float windk);
void parts_draw(const Particles *ps, Surf *cv);   /* canvas with y-up world coords */

void fx_trail(Particles *ps, float x, float y, RGB accent, int trail_style, int n, float t);
void fx_muzzle(Particles *ps, float x, float y, float angle, RGB accent);
void fx_smoke(Particles *ps, float x, float y, int n, float spread, const RGB *tint);
void fx_impact(Particles *ps, float x, float y, RGB accent, RGB soil, float scale, int boom_style);
void fx_ambient(Particles *ps, uint64_t ambient_mask, float x, float y, RGB body, float dt);
void fx_debris(Particles *ps, float x, float y, RGB color);
/* per-frame wreck / low-health smoke (world.update_effects) */
void fx_world_tick(Particles *ps, const World *w, float dt);

#endif
