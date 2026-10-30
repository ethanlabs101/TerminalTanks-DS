/* PreviewStage (the looping tank + ammo demo, l.6289) and the terrain-scan map preview (render_map_preview l.6147). */
#ifndef TT_PREVIEW_H
#define TT_PREVIEW_H
#include "render.h"
#include "sprites.h"

typedef struct {
    int w, h, scale;
    Particles *ps;
    Explosion ex[8];
    int nex;
    int design, ammo;
    RGB body, shot;
    float t, clock, recoil, dim;
    int phase, n;
    float gy, ty;
    float fire_angle, fire_x, fire_y;
    bool fired;
    /* optional battlefield backdrop */
    int map;               /* -1 none */
    World *world;
    Scenery *scn;
    Canvas cv;
} PreviewStage;

void preview_init(PreviewStage *p, int w, int h, int scale, uint32_t seed);
void preview_free(PreviewStage *p);
void preview_set_map(PreviewStage *p, int map);          /* -1: the plain workshop range */
void preview_set_kit(PreviewStage *p, int design, RGB body, int ammo, RGB shot, float dim);
void preview_update(PreviewStage *p, float dt);
Surf *preview_render(PreviewStage *p);

/* terrain scan: cached per map; tanks = two (design, colour) pairs */
typedef struct {
    int map, w, h;
    World *world;
    Scenery *scn;
    double xs[2];
    Canvas cv;
} MapPreview;
void mappreview_init(MapPreview *m);
void mappreview_free(MapPreview *m);
Surf *mappreview_render(MapPreview *m, int map, int w, int h, float t, const int *designs, const RGB *colors);

#endif
