/* A terminal-cell screen, the DS stand-in for the original's Screen class (terminaltanks.py l.573): a grid of
 * (glyph, fg, bg) cells drawn with the 5x8 font. The two DS screens form one tall 51x48 terminal. Pixel canvases are blitted as
 * half-block cells exactly like the original ("▀" with fg = upper pixel, bg = lower pixel).
 * Cells can be TRANSPARENT so a cell layer can be composited over pixel graphics (the battlefield). */
#ifndef TT_TERM_H
#define TT_TERM_H
#include "gfx.h"

#define T_COLS COLS          /* 51 */
#define T_ROWS (2 * ROWS)    /* 48: rows 0-23 are the top LCD, 24-47 the bottom (touch) LCD */
#define T_SPLIT ROWS
#define G_NONE 0xFFFF          /* transparent cell */

typedef struct { uint16_t g; Px fg, bg; } Cell;
typedef struct {
    int cols, rows;
    Cell c[T_ROWS][T_COLS];
} Term;

extern int G_SPACE, G_UPPER;   /* glyph indices of ' ' and '▀' */

void term_init(void);
void term_clear(Term *t, RGB bg);
void term_clear_transparent(Term *t);
void term_put(Term *t, int x, int y, uint32_t cp, RGB fg, const RGB *bg);
void term_putg(Term *t, int x, int y, int g, Px fg, Px bg);
void term_cell(Term *t, int x, int y, RGB col);                     /* the original's _cell(): a solid colour block */
void term_text(Term *t, int x, int y, const char *s, RGB fg, const RGB *bg);
void term_center(Term *t, int y, const char *s, RGB fg, const RGB *bg, int x0, int width);   /* width < 0: full row */
void term_fill(Term *t, int x, int y, int w, int h, RGB bg);
void term_fill_ch(Term *t, int x, int y, int w, int h, RGB bg, uint32_t cp, RGB fg);
enum { TB_DOUBLE = 0, TB_SINGLE, TB_ROUND, TB_HEAVY };
void term_box(Term *t, int x, int y, int w, int h, int style, RGB fg, RGB bg, const char *title, RGB title_fg);
void term_blit(Term *t, const Surf *cv, int x, int y, int dx, int dy);   /* canvas -> half-block cells */
void term_blit_masked(Term *t, const Surf *cv, int x, int y, int x0, int x1);  /* black pixels keep the cell's bg */
void term_tint(Term *t, int x, int y, int w, int h, RGB c, float a);
void term_dim(Term *t, float k);
void term_fade_rows(Term *t, const float *k);
RGB  term_bg(const Term *t, int x, int y);
int  term_wrap_center(Term *t, int y, const char *text, int width, RGB fg, const RGB *bg, int x, int w, int lines);
void term_render(const Term *t, Surf *s, int row0);   /* rows row0..row0+23 -> one LCD */
int  utf8_len(const char *s);

#endif
