/* DS hardware layer: video (two 16-bit bitmap screens with RAM back buffers), input, timing, storage, sound. */
#ifndef TT_PLATFORM_H
#define TT_PLATFORM_H
#include <stdbool.h>
#include <stdint.h>
#include "gfx.h"

void plat_init(void);
void plat_init_storage(bool read_only);   /* FAT; read_only: never write (safe start) */
void plat_show_now(void);                 /* copy the back buffers to the screens immediately */
uint32_t plat_keys_now(void);             /* BTN_* held right now (boot key checks) */
Surf *plat_top(void);          /* top screen back buffer */
Surf *plat_bottom(void);       /* bottom (touch) screen back buffer */
void plat_present(void);       /* wait for the next frame slot (30 fps) and show both buffers */
double plat_frame_dt(void);
uint32_t plat_lines(void);     /* monotonic scanline counter (63.56 us per line) for profiling */
void plat_bottom_dirty(void);  /* a buffer changed: copy it at the next present */
void plat_top_dirty(void);

/* input */
enum { BTN_A = 1 << 0, BTN_B = 1 << 1, BTN_SELECT = 1 << 2, BTN_START = 1 << 3, BTN_RIGHT = 1 << 4, BTN_LEFT = 1 << 5,
       BTN_UP = 1 << 6, BTN_DOWN = 1 << 7, BTN_R = 1 << 8, BTN_L = 1 << 9, BTN_X = 1 << 10, BTN_Y = 1 << 11, BTN_TOUCH = 1 << 12 };
typedef struct {
    uint32_t down, held, repeat, up;
    bool touching, touch_down;
    int tx, ty;          /* touch position (bottom screen pixels) */
} Input;
void plat_input(Input *in);

/* storage: a small file per profile on the SD/flashcart (FAT via DLDI). Returns false when unavailable. */
bool plat_storage_ok(void);
int  plat_load(const char *name, uint8_t *buf, int cap);       /* bytes read or -1 */
/* writes /data/TerminalTanks/<name>.<suffix> once: an existing backup is never overwritten */
bool plat_backup(const char *name, const char *suffix, const uint8_t *buf, int len);
bool plat_save(const char *name, const uint8_t *buf, int len);

/* sound: short square-wave beeps (the original's optional terminal bell) */
void plat_beep(int kind);
void plat_sound_tick(void);

uint32_t plat_entropy(void);   /* a seed from the RTC + timers */
void plat_log(const char *fmt, ...);

#endif
