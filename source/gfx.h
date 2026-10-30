/* Software 2D on 15-bit DS framebuffers: pixels, alpha blending, rectangles and the 5x8 terminal font.
 * A Surf is any RGB555 pixel buffer (the 256x192 screen back buffers or the 128x80 battlefield canvas). */
#ifndef TT_GFX_H
#define TT_GFX_H
#include <stdbool.h>
#include <stdint.h>
#include "../core/content.h"

#define SCR_W 256
#define SCR_H 192
#define CELL_W 5
#define CELL_H 8
#define COLS (SCR_W / CELL_W)     /* 51 */
#define ROWS (SCR_H / CELL_H)     /* 24 */

typedef uint16_t Px;
typedef struct { Px *px; int w, h, stride; } Surf;

static inline Px rgb15(RGB c) { return (Px)((c.r >> 3) | ((c.g >> 3) << 5) | ((c.b >> 3) << 10) | 0x8000); }
static inline Px rgb15i(int r, int g, int b) { return (Px)(((r & 255) >> 3) | (((g & 255) >> 3) << 5) | (((b & 255) >> 3) << 10) | 0x8000); }
static inline RGB px_rgb(Px p) { RGB c = {(uint8_t)((p & 31) << 3), (uint8_t)(((p >> 5) & 31) << 3), (uint8_t)(((p >> 10) & 31) << 3)}; return c; }

/* colour maths on 8-bit RGB (matches the original's mix/shade) */
RGB cmix(RGB a, RGB b, float t);
RGB cshade(RGB c, float k);
RGB cgradient(const float *ts, const RGB *cs, int n, float t);
RGB chsv(float h, float s, float v);

/* 15-bit blending: a in 0..1 */
Px pxmix(Px a, Px b, float t);

void surf_fill(Surf *s, Px c);
void surf_rect(Surf *s, int x, int y, int w, int h, Px c);
void surf_rect_blend(Surf *s, int x, int y, int w, int h, RGB c, float a);
static inline void surf_put(Surf *s, int x, int y, Px c) { if ((unsigned)x < (unsigned)s->w && (unsigned)y < (unsigned)s->h) s->px[y * s->stride + x] = c; }
void surf_blend(Surf *s, int x, int y, RGB c, float a);
void surf_dim(Surf *s, float k);
void surf_tint(Surf *s, int x, int y, int w, int h, RGB c, float a);

/* text: UTF-8, 5x8 glyphs; bg may be NULL for transparent. Returns the advance in pixels. */
int  text_px(Surf *s, int x, int y, const char *utf8, RGB fg, const RGB *bg);
int  text_len(const char *utf8);                 /* glyph count */
/* cell-grid helpers (51 x 24 cells per screen) */
void cell_text(Surf *s, int col, int row, const char *utf8, RGB fg, const RGB *bg);
void cell_center(Surf *s, int row, const char *utf8, RGB fg, const RGB *bg, int col0, int width);
void cell_fill(Surf *s, int col, int row, int w, int h, RGB bg);
void cell_box(Surf *s, int col, int row, int w, int h, int style, RGB fg, RGB bg, const char *title, RGB title_fg);
int  wrap_text(const char *text, int width, char lines[][64], int max_lines);
int  cell_wrap_center(Surf *s, int row, const char *text, int width, RGB fg, const RGB *bg, int col0, int w, int lines);
enum { BOX_DOUBLE = 0, BOX_SINGLE, BOX_ROUND, BOX_HEAVY };

/* font access (glyph index = position in the generated table) */
int font_index(uint32_t cp);
const uint8_t *font_bits(int idx);
uint32_t utf8_next(const char **p);

/* y-up canvas helpers (a canvas is a Surf whose row 0 is the top; world y=0 is the bottom row) */
static inline void cv_plot(Surf *cv, int x, int y, Px c) {
    int py = cv->h - 1 - y;
    if ((unsigned)x < (unsigned)cv->w && (unsigned)py < (unsigned)cv->h) cv->px[py * cv->stride + x] = c;
}
static inline Px cv_get(const Surf *cv, int x, int y) {
    int py = cv->h - 1 - y;
    if ((unsigned)x < (unsigned)cv->w && (unsigned)py < (unsigned)cv->h) return cv->px[py * cv->stride + x];
    return 0;
}
void cv_blend(Surf *cv, int x, int y, RGB c, float a);
void cv_plotf(Surf *cv, float x, float y, RGB c);
void cv_blendf(Surf *cv, float x, float y, RGB c, float a);

/* fast trig for visuals (radians), LUT based */
float fsin(float a);
float fcos(float a);
float fsqrt(float v);
float fhypot(float x, float y);

/* palette (Palette class, l.138) */
extern const RGB P_BG, P_PANEL, P_PANEL_HI, P_LINE, P_PRIMARY, P_PRIMARY_DIM, P_AMBER, P_TEXT, P_MUTED, P_DANGER, P_HEART,
    P_HEART_OFF, P_OK, P_WHITE, P_INK, P_GOLD;

#endif
