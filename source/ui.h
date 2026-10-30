/* Shared UI widgets (terminaltanks.py l.6020-6146, 7494-7507, 7870-7900, 6637-6700, 7398-7492): menus, keycaps,
 * panels, hearts, bars, colour swatches, the wallet and the big gradient logos. */
#ifndef TT_UI_H
#define TT_UI_H
#include "app.h"
#include "../core/world.h"

/* MenuList (l.6093). Hotspot ids for its rows are base + index. */
typedef struct {
    const char *items[24];
    int n, index;
} MenuList;
void menu_init(MenuList *m, const char *const *items, int n);
int  menu_handle(MenuList *m, const IEv *e, int hot_base);       /* returns the chosen index or -1 */
void menu_draw(Term *t, const MenuList *m, int cx, int y, float time, int gap, int width, int hot_base);

typedef struct { const char *key, *desc; } Keycap;
void ui_keycaps(Term *t, int x, int y, const Keycap *k, int n, const RGB *bg);
void ui_keycaps_center(Term *t, int y, const Keycap *k, int n, const RGB *bg, int x0, int w);
/* panel centred horizontally; y < 0 centres it inside rows [area_y, area_y + area_h) */
void ui_panel(Term *t, int w, int h, const char *title, int y, int area_y, int area_h, RGB fg, int *ox, int *oy);
int  ui_draw_hp(Term *t, int x, int y, const Tank *tk, RGB bg, bool right);
void ui_hearts(int hp, bool spaced, int total, char *out, int n);
void ui_draw_bar(Term *t, int x, int y, int w, float frac);
void ui_swatches(Term *t, int x, int y, int idx, int taken, const int *anim, int nanim, int cur, float time);
void ui_wallet_text(const SaveData *s, char *out, int n);
void ui_wallet(Term *t, const SaveData *s, float flash);
void ui_prism_text(Term *t, int x, int y, const char *s, float time, float base, float spread, float sat, float val, const RGB *bg);
void fmt_playtime(double sec, char *out, int n);
void fmt_commas(long v, char *out, int n);
void ui_button(Term *t, int x, int y, const char *label, RGB fg, RGB bg, int hot_id);   /* "[ LABEL ]" with a hotspot */

/* On-screen keyboard (bottom screen): d-pad + A, or touch. X = delete, START = OK. Returns a char, K_BKSP, K_ENTER,
 * K_SPACE or 0 (and consumes navigation events). */
typedef struct { int r, c; } Osk;
int  osk_handle(Osk *k, const IEv *e);
void osk_draw(Term *t, const Osk *k, int y, float time);
#define HOT_OSK 2000

/* Big logos. They are drawn in pixels (3x6 per glyph cell, 65 cells wide) over the cell layer after it is rendered.
 * Each returns the next free terminal row, like the originals. */
enum { LOGO_CLASSIC = 0, LOGO_MASTER, LOGO_SECRET, LOGO_THEME };
int  ui_logo(Term *t, int kind, int y, float time, bool tall, int home, bool compact);
bool ui_logo_pending(void);
bool ui_logo_changed(void);
void ui_logo_keep(void);
void ui_logo_flush(Surf *top);

#endif
