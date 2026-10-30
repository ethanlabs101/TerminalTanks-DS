#include <nds.h>
#include <fat.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "platform.h"

static Px g_buf[2][SCR_W * SCR_H] __attribute__((aligned(32)));
static Surf g_surf[2];
static uint16_t *g_vram[2];
static volatile uint32_t g_vbl;
static uint32_t g_last_vbl;
static double g_dt = 1.0 / 30.0;
static bool g_fat, g_read_only;

static void on_vblank(void) { g_vbl++; }

void plat_init(void) {
    defaultExceptionHandler();          /* a crash shows the guru screen instead of a frozen/white screen */
    powerOn(POWER_ALL_2D);
    videoSetMode(MODE_5_2D);
    videoSetModeSub(MODE_5_2D);
    vramSetBankA(VRAM_A_MAIN_BG);
    vramSetBankC(VRAM_C_SUB_BG);
    int bg = bgInit(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
    int bgs = bgInitSub(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
    g_vram[0] = bgGetGfxPtr(bg);
    g_vram[1] = bgGetGfxPtr(bgs);
    lcdMainOnTop();
    for (int i = 0; i < 2; i++) {
        g_surf[i].px = g_buf[i];
        g_surf[i].w = SCR_W;
        g_surf[i].h = SCR_H;
        g_surf[i].stride = SCR_W;
        surf_fill(&g_surf[i], rgb15(P_BG));
        DC_FlushRange(g_buf[i], sizeof g_buf[i]);
        dmaCopyWords(3, g_buf[i], g_vram[i], sizeof g_buf[i]);   /* never show uninitialised VRAM */
    }
    irqSet(IRQ_VBLANK, on_vblank);
    irqEnable(IRQ_VBLANK);
    keysSetRepeat(8, 2);   /* scanned at 30 fps */
    soundEnable();
    g_last_vbl = g_vbl;
}

/* Called after the first boot screen is visible, so a card problem never leaves the screens blank. */
void plat_init_storage(bool read_only) {
    g_fat = fatInitDefault();
    g_read_only = read_only;
    if (g_fat && !read_only) {
        mkdir("/data", 0777);
        mkdir("/data/TerminalTanks", 0777);
    }
}

/* Shows the back buffers right away (boot progress screens, before the frame loop runs). */
void plat_show_now(void) {
    for (int i = 0; i < 2; i++) {
        DC_FlushRange(g_buf[i], sizeof g_buf[i]);
        dmaCopyWords(3, g_buf[i], g_vram[i], sizeof g_buf[i]);
    }
}

uint32_t plat_keys_now(void) {
    scanKeys();
    uint32_t h = keysHeld(), o = 0;
    if (h & KEY_L) o |= BTN_L;
    if (h & KEY_R) o |= BTN_R;
    if (h & KEY_SELECT) o |= BTN_SELECT;
    if (h & KEY_START) o |= BTN_START;
    return o;
}

Surf *plat_top(void) { return &g_surf[0]; }
Surf *plat_bottom(void) { return &g_surf[1]; }

static bool g_bot_dirty = true, g_top_dirty = true;
void plat_bottom_dirty(void) { g_bot_dirty = true; }
void plat_top_dirty(void) { g_top_dirty = true; }

void plat_present(void) {
    /* 30 fps: frames start every second vblank. The top screen is copied every frame, the bottom one only when
     * a scene redrew it (plat_bottom_dirty). If the frame overran, the copy happens right away (no extra wait). */
    uint32_t target = g_last_vbl + 2;
    if (g_top_dirty) DC_FlushRange(g_buf[0], sizeof g_buf[0]);
    if (g_bot_dirty) DC_FlushRange(g_buf[1], sizeof g_buf[1]);
    while ((int32_t)(g_vbl - target) < 0) swiWaitForVBlank();
    if (g_top_dirty) dmaCopyWords(3, g_buf[0], g_vram[0], sizeof g_buf[0]);
    if (g_bot_dirty) dmaCopyWords(3, g_buf[1], g_vram[1], sizeof g_buf[1]);
    g_bot_dirty = g_top_dirty = false;
    uint32_t now = g_vbl;
    uint32_t n = now - g_last_vbl;
    g_last_vbl = now;
    g_dt = n / 59.8261;
    if (g_dt > 0.1) g_dt = 0.1;
}

double plat_frame_dt(void) { return g_dt; }

/* scanline clock: 263 lines per frame at 59.83 Hz (one line = 63.56 us) */
uint32_t plat_lines(void) {
    uint32_t v, c;
    do { v = g_vbl; c = REG_VCOUNT; } while (v != g_vbl);
    /* the vblank IRQ fires at line 192 */
    return v * 263u + (c >= 192 ? c - 192 : c + 71);
}

void plat_input(Input *in) {
    scanKeys();
    uint32_t d = keysDown(), h = keysHeld(), r = keysDownRepeat(), u = keysUp();
    static const struct { uint32_t k, b; } MAP[] = {{KEY_A, BTN_A}, {KEY_B, BTN_B}, {KEY_SELECT, BTN_SELECT}, {KEY_START, BTN_START},
                                                    {KEY_RIGHT, BTN_RIGHT}, {KEY_LEFT, BTN_LEFT}, {KEY_UP, BTN_UP}, {KEY_DOWN, BTN_DOWN},
                                                    {KEY_R, BTN_R}, {KEY_L, BTN_L}, {KEY_X, BTN_X}, {KEY_Y, BTN_Y}, {KEY_TOUCH, BTN_TOUCH}};
    memset(in, 0, sizeof(*in));
    for (unsigned i = 0; i < sizeof MAP / sizeof MAP[0]; i++) {
        if (d & MAP[i].k) in->down |= MAP[i].b;
        if (h & MAP[i].k) in->held |= MAP[i].b;
        if (r & MAP[i].k) in->repeat |= MAP[i].b;
        if (u & MAP[i].k) in->up |= MAP[i].b;
    }
    if (h & KEY_TOUCH) {
        touchPosition tp;
        touchRead(&tp);
        in->touching = true;
        in->touch_down = (d & KEY_TOUCH) != 0;
        in->tx = tp.px;
        in->ty = tp.py;
    }
}

bool plat_storage_ok(void) { return g_fat; }

/* Saves survive a power cut: every file is kept as two fixed-size slots (<name>.0 / <name>.1) that are
 * overwritten in place. A write goes to the older slot and carries a sequence number and a checksum; loading picks
 * the newest slot whose checksum is good. Because the slot files never change size after creation, a write only
 * touches the file's own data sectors (no FAT chain or directory changes), so switching the DS off mid-write can
 * at worst lose that one write, never the previous save or the card's file system. */
#define SLOT_SIZE 8192
#define SLOT_HDR 16
#define SLOT_MAGIC 0x31535454u   /* "TTS1" */
static uint32_t slot_sum(const uint8_t *b, int n) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}
static void slot_path(const char *name, int k, char *out, int n) { snprintf(out, (size_t)n, "/data/TerminalTanks/%s.%d", name, k); }
static uint8_t g_slot[SLOT_SIZE];

/* reads slot k; returns payload length (payload copied to buf) or -1; *seq gets its sequence number */
static int slot_read(const char *name, int k, uint8_t *buf, int cap, uint32_t *seq) {
    char path[96];
    slot_path(name, k, path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    int n = (int)fread(g_slot, 1, SLOT_SIZE, f);
    fclose(f);
    if (n < SLOT_HDR) return -1;
    uint32_t h[4];
    memcpy(h, g_slot, sizeof h);
    if (h[0] != SLOT_MAGIC || h[2] > (uint32_t)(n - SLOT_HDR) || (int)h[2] > cap) return -1;
    if (slot_sum(g_slot + SLOT_HDR, (int)h[2]) != h[3]) return -1;
    memcpy(buf, g_slot + SLOT_HDR, h[2]);
    *seq = h[1];
    return (int)h[2];
}

typedef struct { char name[24]; uint32_t seq; int slot; } SlotState;
static SlotState g_slots[8];
static SlotState *slot_state(const char *name) {
    for (int i = 0; i < 8; i++) if (g_slots[i].name[0] && !strcmp(g_slots[i].name, name)) return &g_slots[i];
    for (int i = 0; i < 8; i++)
        if (!g_slots[i].name[0]) {
            snprintf(g_slots[i].name, sizeof g_slots[i].name, "%s", name);
            g_slots[i].seq = 0;
            g_slots[i].slot = -1;
            return &g_slots[i];
        }
    return &g_slots[0];
}

int plat_load(const char *name, uint8_t *buf, int cap) {
    if (!g_fat) return -1;
    SlotState *st = slot_state(name);
    static uint8_t tmp[SLOT_SIZE];
    uint32_t s0 = 0, s1 = 0;
    int n0 = slot_read(name, 0, buf, cap, &s0);
    int n1 = slot_read(name, 1, tmp, cap < SLOT_SIZE ? cap : SLOT_SIZE, &s1);
    if (n1 >= 0 && (n0 < 0 || (int32_t)(s1 - s0) > 0)) {
        memcpy(buf, tmp, (size_t)n1);
        st->seq = s1;
        st->slot = 1;
        return n1;
    }
    if (n0 >= 0) {
        st->seq = s0;
        st->slot = 0;
        return n0;
    }
    /* saves written by earlier builds: a single file */
    char path[96];
    snprintf(path, sizeof path, "/data/TerminalTanks/%s", name);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    int n = (int)fread(buf, 1, (size_t)cap, f);
    fclose(f);
    return n;
}

bool plat_save(const char *name, const uint8_t *buf, int len) {
    if (!g_fat || g_read_only || len < 0 || len > SLOT_SIZE - SLOT_HDR) return false;
    SlotState *st = slot_state(name);
    if (st->slot < 0) {   /* not loaded this session: find the newest slot so the other one is overwritten */
        uint32_t s0 = 0, s1 = 0;
        static uint8_t tmp[SLOT_SIZE];
        int n0 = slot_read(name, 0, tmp, SLOT_SIZE, &s0), n1 = slot_read(name, 1, tmp, SLOT_SIZE, &s1);
        if (n1 >= 0 && (n0 < 0 || (int32_t)(s1 - s0) > 0)) { st->slot = 1; st->seq = s1; }
        else if (n0 >= 0) { st->slot = 0; st->seq = s0; }
    }
    int k = st->slot == 0 ? 1 : 0;
    uint32_t h[4] = {SLOT_MAGIC, st->seq + 1, (uint32_t)len, slot_sum(buf, len)};
    memset(g_slot, 0, sizeof g_slot);
    memcpy(g_slot, h, sizeof h);
    memcpy(g_slot + SLOT_HDR, buf, (size_t)len);
    char path[96];
    slot_path(name, k, path, sizeof path);
    FILE *f = fopen(path, "r+b");                /* in place: the file keeps its clusters */
    if (f) {
        fseek(f, 0, SEEK_END);
        if (ftell(f) != SLOT_SIZE) { fclose(f); f = NULL; }
        else fseek(f, 0, SEEK_SET);
    }
    if (!f) f = fopen(path, "wb");               /* first write of this slot: allocate it once */
    if (!f) return false;
    bool ok = fwrite(g_slot, 1, SLOT_SIZE, f) == SLOT_SIZE;
    ok = fclose(f) == 0 && ok;
    if (!ok) return false;
    st->slot = k;
    st->seq = h[1];
    return true;
}

/* ---- PSG beeps: two channels, auto-stop after a few frames */
static int g_snd_ch = -1, g_snd_left;
void plat_beep(int kind) {
    if (g_snd_ch >= 0) soundKill(g_snd_ch);
    int freq = kind == 2 ? 180 : kind == 1 ? 260 : 880;
    g_snd_ch = soundPlayPSG(DutyCycle_25, (u16)freq, kind == 2 ? 90 : 70, 64);
    g_snd_left = kind == 2 ? 9 : 5;
}
void plat_sound_tick(void) {
    if (g_snd_ch >= 0 && --g_snd_left <= 0) {
        soundKill(g_snd_ch);
        g_snd_ch = -1;
    }
}

uint32_t plat_entropy(void) {
    time_t t = time(NULL);
    return (uint32_t)t * 2654435761u ^ (uint32_t)(REG_VCOUNT) * 40503u ^ g_vbl * 2246822519u;
}

void plat_log(const char *fmt, ...) {
    char buf[160];
    va_list a;
    va_start(a, fmt);
    vsnprintf(buf, sizeof buf, fmt, a);
    va_end(a);
    nocashMessage(buf);
}
