#ifndef TT_SPRITES_H
#define TT_SPRITES_H
#include "gfx.h"

void draw_artifact(Surf *cv, float cx, float cy, float s, float t);
void draw_mage(Surf *cv, float cx, float by, float s, float t, float alpha, bool aura);

/* a canvas owning its pixels */
typedef struct { Surf s; Px *buf; } Canvas;
void canvas_alloc(Canvas *c, int w, int h);
void canvas_free(Canvas *c);

#endif
