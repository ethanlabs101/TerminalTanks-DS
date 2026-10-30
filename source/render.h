/* Battlefield renderer: draws the 128x80 world canvas (sky, buildings, terrain, animated sky layers, tanks, UFOs,
 * projectiles, particles, explosions) in the original's style, then scales it 2x onto the top screen.
 * Port of Scenery / draw_tank / draw_ufo / draw_projectile / Explosion (terminaltanks.py l.1287-3351). */
#ifndef TT_RENDER_H
#define TT_RENDER_H
#include "../core/battle.h"
#include "gfx.h"
#include "particles.h"

typedef struct {
    Px sky[FIELD_H][FIELD_W];
    Px rows[FIELD_H][FIELD_W];
    int width, height;
    int theme;
    int terrain_version;
    int col_h[FIELD_W];
    float col_scorch[FIELD_W];
    struct { int16_t spq, phq; uint8_t b8; } starq[MAX_STARS];   /* stars in integer form (built once) */
    int aur_key[3];       /* each aurora band is rebuilt on its own frame (spreads the cost) */
    int naur[3];
    struct { uint8_t py, x; Px c; } aur[3][FIELD_W * 12];
    int bolt_key;
    float bolt[64][4];
    int nbolt;
} Scenery;

void scenery_build(Scenery *sc, const World *w);
void scenery_terrain(Scenery *sc, const World *w);       /* (re)shade the terrain layer */
void scenery_draw(Scenery *sc, const World *w, Surf *cv, float t, bool flashes);

/* standalone pieces reused by menus / previews */
RGB live_color(RGB c, float t);
RGB rainbow(float t);
RGB astral(float t);
RGB prismatic(float t);
RGB void_tint(float t);
float visual_lift(const TankDesign *d, float t, int scale);
void draw_tank(Surf *cv, float x, float y, int facing, float angle, RGB body, int scale, float flash, bool wreck, float recoil,
               const TankDesign *d, float t, float lift, float hover);
void draw_projectile(Surf *cv, float x, float y, float vx, float vy, RGB accent, const AmmoType *ammo, float t);
void draw_explosion(Surf *cv, const Explosion *e);
void draw_ufo(Surf *cv, const Tank *tk, float now, float flash);

/* whole battle field (canvas) */
typedef struct {
    Scenery sc;
    Particles ps;
    Surf canvas;
    Px canvas_px[FIELD_H * FIELD_W];
    float guide[200];
    int nguide, guide_version;
    PyRandom vis;           /* screen shake etc. (never the simulation rng) */
} FieldView;

void field_init(FieldView *fv, Battle *b, const Settings *st);
void field_update(FieldView *fv, Battle *b, float dt);   /* drains the battle's effect queue, ticks particles */
void field_draw(FieldView *fv, Battle *b, float t);
/* scale the field 2x into dst at (x, y) with the screen-shake offset; returns the offset used */
void field_blit(FieldView *fv, Battle *b, Surf *dst, int x, int y);
/* world (x, y) -> top-screen pixel, given the blit origin */
static inline int field_sx(int ox, float wx) { return ox + (int)(wx * 2); }
static inline int field_sy(int oy, float wy) { return oy + (FIELD_H - 1 - (int)wy) * 2; }

#endif
