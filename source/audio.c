/* Music and sound effects (see audio.h).
 *
 * Music: sound channel 0 loops over a 16-bit PCM ring forever. Timers 0+1 count the samples the hardware has played
 * (timer 0 overflows once per sample at exactly the channel's rate, timer 1 cascades), so the VBlank interrupt knows
 * where playback is and decodes ADPCM blocks up to LEAD samples ahead of it. The blocks come from a RAM queue that the
 * main loop tops up from the music pack, so a slow frame (or a save) never starves the music: the queue holds about
 * four seconds. No song = the ring is filled with silence; nothing is ever restarted, so there are no clicks.
 *
 * Effects: loaded on first use into a RAM cache (least recently used ones are dropped), played on channels 1..11 by
 * the hardware's own ADPCM decoder. */
#include <nds.h>
#include <math.h>
#include <fat.h>
#include <filesystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "audio.h"
#include "platform.h"
#include "../core/ima.h"

#define RING 8192                 /* PCM ring, samples (0.5 s) */
#define LEAD 4096                 /* decode this far ahead of the play position (0.25 s) */
#define QBLK 16                   /* queued ADPCM blocks (about 4 s) */
#define SFX_CH0 1
#define SFX_NCH 11
#define SFX_BUDGET (256 * 1024)
#define SFX_K 0.8415f
#define AUDIO_RATE 16542u          /* what the hardware actually plays: 16756991 / AUDIO_TIMER */             /* the PC mixes effects at sfx 0.9 * master 0.85 * 1.1 */

static int16_t g_ring[RING] __attribute__((aligned(32)));
static uint8_t g_q[QBLK * MUS_BLOCK_BYTES] __attribute__((aligned(32)));
static volatile uint32_t g_qw, g_qr;          /* blocks queued / consumed (absolute counts) */

static FILE *g_mf, *g_sf;
static MusEntry *g_ment;
static SfxEntry *g_sent;
static int g_nsongs, g_nsfx;
static bool g_ok, g_started;

/* decoder state, owned by the interrupt while g_active is set */
static volatile int g_active;
static volatile int g_loop_song = 1, g_ended;   /* Jukebox AUTO-PLAY: stop at the end of the song instead of looping */
static uint32_t g_pos, g_le, g_ls, g_inblk;
static ImaState g_st;
static uint32_t g_w, g_p;                     /* samples written / played (absolute) */
static uint16_t g_tlast;

/* main-loop state */
static int g_song = -1, g_want = -1, g_rdnext, g_rdlast;
static long g_fpos = -1;
static float g_fade_out = 0.3f, g_fade_in = 0.06f, g_gain, g_duck;
static int g_mood, g_mode = AUDIO_ON, g_mvol = 10, g_svol = 10, g_sent_vol = -1;

static uint8_t *g_sdata[512];
static uint32_t g_sused[512], g_sbusy[512], g_sbytes_total;
static uint32_t g_vend[SFX_NCH];
static uint32_t g_frames;

/* ---------------------------------------------------------------- decoding (interrupt side) */
static void decode(int16_t *out, int n) {
    int j = 0;
    while (j < n) {
        if (!g_active || g_qr == g_qw) {           /* nothing to play (or the disk fell behind): silence, hold the place */
            memset(out + j, 0, (size_t)(n - j) * 2);
            return;
        }
        const uint8_t *blk = g_q + (g_qr % QBLK) * MUS_BLOCK_BYTES;
        if (g_inblk == 0) {
            g_st.pcm = (int16_t)(blk[0] | (blk[1] << 8));
            g_st.index = blk[2] > 88 ? 88 : blk[2];
        }
        uint32_t k = (uint32_t)(n - j);
        if (k > MUS_BLOCK_SAMPLES - g_inblk) k = MUS_BLOCK_SAMPLES - g_inblk;
        if (k > g_le - g_pos) k = g_le - g_pos;
        const uint8_t *nb = blk + 4;
        uint32_t bi = g_inblk;
        for (uint32_t i = 0; i < k; i++, bi++)
            out[j++] = (int16_t)ima_step(&g_st, (nb[bi >> 1] >> ((bi & 1) << 2)) & 15);
        g_inblk += k;
        g_pos += k;
        if (g_pos >= g_le) {                       /* loop: the reader queued loop_start's block next */
            if (!g_loop_song) {                    /* one pass only: finish in silence and say so */
                g_active = 0;
                g_ended = 1;
                memset(out + j, 0, (size_t)(n - j) * 2);
                return;
            }
            g_pos = g_ls;
            g_inblk = 0;
            g_qr++;
        } else if (g_inblk >= MUS_BLOCK_SAMPLES) {
            g_inblk = 0;
            g_qr++;
        }
    }
}

static void fill(void) {
    uint16_t t = TIMER1_DATA;
    g_p += (uint16_t)(t - g_tlast);
    g_tlast = t;
    if ((int32_t)(g_w - g_p) < 64) g_w = g_p + 64;             /* fell behind (interrupts were off): skip ahead */
    uint32_t target = g_p + LEAD, w0 = g_w;
    while ((int32_t)(target - g_w) > 0) {
        uint32_t off = g_w % RING, n = target - g_w;
        if (n > RING - off) n = RING - off;
        decode(g_ring + off, (int)n);
        g_w += n;
    }
    uint32_t a = w0 % RING, b = g_w % RING;
    if (g_w - w0 >= RING) DC_FlushRange(g_ring, sizeof g_ring);
    else if (b > a) DC_FlushRange(g_ring + a, (b - a) * 2);
    else if (g_w != w0) { DC_FlushRange(g_ring + a, (RING - a) * 2); DC_FlushRange(g_ring, b * 2); }
}

void audio_irq(void) {
    if (g_started) fill();
}

/* ---------------------------------------------------------------- music (main loop) */
static bool rd(FILE *f, long off, void *dst, size_t n, long *cur) {
    if (!cur || *cur != off) {
        if (fseek(f, off, SEEK_SET) != 0) { if (cur) *cur = -1; return false; }
    }
    size_t got = fread(dst, 1, n, f);
    if (cur) *cur = got == n ? off + (long)n : -1;
    return got == n;
}

static void read_ahead(int max_blocks) {
    if (g_song < 0) return;
    const MusEntry *e = &g_ment[g_song];
    while (max_blocks > 0 && g_qw - g_qr < QBLK) {
        int n = (int)(QBLK - (g_qw - g_qr));
        int to_end = QBLK - (int)(g_qw % QBLK);
        if (n > to_end) n = to_end;
        if (n > g_rdlast + 1 - g_rdnext) n = g_rdlast + 1 - g_rdnext;
        if (n > max_blocks) n = max_blocks;
        if (n <= 0) break;
        uint8_t *dst = g_q + (g_qw % QBLK) * MUS_BLOCK_BYTES;
        if (!rd(g_mf, (long)e->off + (long)g_rdnext * MUS_BLOCK_BYTES, dst, (size_t)n * MUS_BLOCK_BYTES, &g_fpos)) {
            g_fpos = -1;
            return;                               /* try again next frame */
        }
        g_qw += (uint32_t)n;
        g_rdnext += n;
        if (g_rdnext > g_rdlast) g_rdnext = (int)(e->loop_start / MUS_BLOCK_SAMPLES);
        max_blocks -= n;
    }
}

static void start_song(int s) {
    int ime = enterCriticalSection();
    g_active = 0;
    leaveCriticalSection(ime);
    g_song = s;
    if (s < 0 || s >= g_nsongs || g_ment[s].nblocks == 0) { g_song = -1; return; }
    const MusEntry *e = &g_ment[s];
    g_qw = g_qr = 0;
    g_rdnext = 0;
    g_rdlast = (int)((e->loop_end - 1) / MUS_BLOCK_SAMPLES);
    read_ahead(3);                                 /* enough to start right away (LEAD < one block) */
    ime = enterCriticalSection();
    g_pos = 0; g_inblk = 0; g_le = e->loop_end; g_ls = e->loop_start;
    g_ended = 0;
    g_w = g_p + 64;                               /* replace what was queued in the ring: the new song starts now */
    g_active = 1;
    fill();
    leaveCriticalSection(ime);
}

static void apply_music_volume(void) {
    float mood = g_mood == MOOD_STORY ? 0.8f : g_mood == MOOD_RESULT ? 0.72f : 1.0f;
    float v = g_mode == AUDIO_ON ? g_gain * mood * (1.0f - 0.45f * g_duck) * (float)g_mvol / 10.0f : 0.0f;
    int iv = (int)(v * 127.0f + 0.5f);
    if (iv > 127) iv = 127;
    if (iv < 0) iv = 0;
    if (iv != g_sent_vol) {
        soundSetVolume(0, (u8)iv);
        g_sent_vol = iv;
    }
}

void audio_music(int song, float fade) {
    if (!g_ok) return;
    if (song == g_want) return;
    g_want = song;
    g_fade_out = fade < 0.05f ? 0.05f : fade > 0.35f ? 0.35f : fade;   /* the PC fades the old song out in <= 0.35 s */
    g_fade_in = 0.06f;
}

void audio_mood(int mood) { g_mood = mood; }
int audio_current_song(void) { return g_want; }
void audio_loop(bool on) { g_loop_song = on; }
bool audio_take_ended(void) { bool e = g_ended; g_ended = 0; return e; }
void audio_position(int *pos_s, int *dur_s) {
    *pos_s = g_song >= 0 ? (int)(g_pos / AUDIO_RATE) : 0;
    *dur_s = g_song >= 0 ? (int)(g_ment[g_song].loop_end / AUDIO_RATE) : 0;
}

/* The Jukebox analyser: 16 log-spaced bands (55 Hz .. 7 kHz, like the PC's) from the 256 samples just played,
 * one Goertzel filter per band in fixed point. 0..8 per band. */
void audio_spectrum(uint8_t out[16]) {
    static int16_t q14[16];
    static bool init;
    if (!init) {
        init = true;
        for (int b = 0; b < 16; b++) {
            float f = 55.0f * powf(2.0f, b * 0.47f);
            q14[b] = (int16_t)(2.0f * cosf(6.2831853f * f / AUDIO_RATE) * 16384.0f);
        }
    }
    if (!g_ok || g_mode != AUDIO_ON || g_song < 0) { memset(out, 0, 16); return; }
    uint32_t end = g_p;
    for (int b = 0; b < 16; b++) {
        int32_t s1 = 0, s2 = 0;
        for (int i = 0; i < 256; i++) {
            int32_t x = g_ring[(end - 256 + (uint32_t)i) % RING] >> 4;
            int32_t s0 = x + (int32_t)(((int64_t)q14[b] * s1) >> 14) - s2;
            s2 = s1;
            s1 = s0;
        }
        int64_t pw = (int64_t)s1 * s1 + (int64_t)s2 * s2 - (((int64_t)q14[b] * s1) >> 14) * s2;
        int lv = 0;
        while (pw > 60000 && lv < 8) { pw >>= 2; lv++; }   /* ~6 dB per step */
        out[b] = (uint8_t)(lv * (float)g_mvol / 10.0f + 0.5f);
    }
}
bool audio_ready(void) { return g_ok; }

void audio_settings(int mode, int music_vol, int sfx_vol) {
    g_mode = mode;
    g_mvol = music_vol < 0 ? 0 : music_vol > 10 ? 10 : music_vol;
    g_svol = sfx_vol < 0 ? 0 : sfx_vol > 10 ? 10 : sfx_vol;
    if (g_ok && mode != AUDIO_ON)
        for (int c = 0; c < SFX_NCH; c++) soundKill(SFX_CH0 + c);
}

/* ---------------------------------------------------------------- effects */
static void sfx_evict(uint32_t need) {
    while (g_sbytes_total + need > SFX_BUDGET) {
        int victim = -1;
        for (int i = 0; i < g_nsfx; i++)
            if (g_sdata[i] && g_sbusy[i] < g_frames && (victim < 0 || g_sused[i] < g_sused[victim])) victim = i;
        if (victim < 0) return;
        free(g_sdata[victim]);
        g_sdata[victim] = NULL;
        g_sbytes_total -= g_sent[victim].bytes;
    }
}

static uint8_t *sfx_load(int id) {
    if (g_sdata[id]) return g_sdata[id];
    uint32_t n = g_sent[id].bytes;
    if (n < 8 || n > 256 * 1024) return NULL;
    sfx_evict(n);
    uint8_t *p = malloc((n + 3) & ~3u);
    if (!p) return NULL;
    long dummy = -1;
    if (!rd(g_sf, (long)g_sent[id].off, p, n, &dummy)) { free(p); return NULL; }
    DC_FlushRange(p, n);
    g_sdata[id] = p;
    g_sbytes_total += n;
    return p;
}

void audio_sfx(int id, float vol, float pan, float rate) {
    if (!g_ok || g_mode != AUDIO_ON || id < 0 || id >= g_nsfx || g_svol == 0) return;
    g_frames = plat_lines() / 263;
    uint8_t *p = sfx_load(id);
    if (!p) return;
    uint32_t bytes = g_sent[id].bytes;
    if (rate < 0.5f) rate = 0.5f;
    if (rate > 2.0f) rate = 2.0f;
    int freq = (int)(AUDIO_HZ * rate + 0.5f);
    uint32_t frames = (uint32_t)(((bytes - 4) * 2) * 60.0f / ((float)AUDIO_HZ * rate)) + 2;
    int ch = 0;                                     /* a free voice, else the one closest to finishing */
    for (int c = 1; c < SFX_NCH; c++)
        if (g_vend[c] < g_vend[ch]) ch = c;
    float g = vol * SFX_K * (float)g_svol / 10.0f;
    int iv = (int)(g * 127.0f + 0.5f);
    if (iv > 127) iv = 127;
    if (iv <= 0) return;
    if (pan < -1) pan = -1;
    if (pan > 1) pan = 1;
    int ipan = 64 + (int)(pan * 54.0f);
    soundPlaySampleChannel(SFX_CH0 + ch, p, SoundFormat_ADPCM, bytes, (u16)freq, (u8)iv, (u8)ipan, false, 0);
    g_vend[ch] = g_frames + frames;
    g_sused[id] = g_frames;
    if (g_sbusy[id] < g_vend[ch]) g_sbusy[id] = g_vend[ch];
    if (SFX_DUCKS[id]) g_duck = 1.0f;
}

void audio_sfx_ammo(bool hit, int ammo, float vol, float pan) {
    audio_sfx((hit ? SFX_HIT_FIRST : SFX_FIRE_FIRST) + ammo, vol, pan, 1.0f);
}

/* ---------------------------------------------------------------- setup / per frame */
static void *read_table(FILE *f, uint32_t magic, int esize, int *count) {
    uint32_t h[2];
    if (fread(h, 4, 2, f) != 2 || h[0] != magic || h[1] == 0 || h[1] > 4096) return NULL;
    void *t = malloc((size_t)h[1] * (size_t)esize);
    if (!t) return NULL;
    if (fread(t, (size_t)esize, h[1], f) != h[1]) { free(t); return NULL; }
    *count = (int)h[1];
    return t;
}

bool audio_init(void) {
    char cwd[64];
    if (!getcwd(cwd, sizeof cwd)) cwd[0] = 0;
    bool ok = nitroFSInit(NULL);
    /* nitroFSInit makes nitro: (the ROM, read-only) the current drive, and the saves use paths like
     * /data/TerminalTanks/...: give the current drive back to the SD card */
    if (!cwd[0] || chdir(cwd) != 0) chdir(fatGetDefaultDrive());
    if (!ok) { plat_log("audio: no NitroFS"); return false; }
    nitroFSInitLookupCache(16 * 1024);
    g_mf = fopen("nitro:/audio/music.pak", "rb");
    g_sf = fopen("nitro:/audio/sfx.pak", "rb");
    if (!g_mf || !g_sf) { plat_log("audio: packs missing"); return false; }
    setvbuf(g_mf, NULL, _IONBF, 0);
    setvbuf(g_sf, NULL, _IONBF, 0);
    g_ment = read_table(g_mf, MUS_MAGIC, sizeof(MusEntry), &g_nsongs);
    g_sent = read_table(g_sf, SFX_MAGIC, sizeof(SfxEntry), &g_nsfx);
    if (!g_ment || !g_sent || g_nsongs != N_SONGS || g_nsfx != N_SFX || g_nsfx > 512) {
        plat_log("audio: pack mismatch (%d songs, %d sfx)", g_nsongs, g_nsfx);
        return false;
    }
    memset(g_ring, 0, sizeof g_ring);
    DC_FlushRange(g_ring, sizeof g_ring);
    /* timer 0 overflows once per sample (the CPU timers run at twice the sound clock), timer 1 counts samples */
    TIMER0_CR = 0;
    TIMER1_CR = 0;
    TIMER1_DATA = 0;
    TIMER1_CR = TIMER_ENABLE | TIMER_CASCADE;
    TIMER0_DATA = (u16)(65536 - 2 * AUDIO_TIMER);
    soundPlaySampleChannel(0, g_ring, SoundFormat_16Bit, sizeof g_ring, AUDIO_HZ, 0, 64, true, 0);   /* waits for the ARM7 */
    int ime = enterCriticalSection();
    TIMER0_CR = TIMER_ENABLE | TIMER_DIV_1;
    g_tlast = 0; g_p = 0; g_w = 0;
    g_sent_vol = 0;
    g_started = true;
    g_ok = true;
    fill();
    leaveCriticalSection(ime);
    return true;
}

void audio_update(float dt) {
    if (!g_ok) return;
    g_frames = plat_lines() / 263;                /* in VBlanks (60 per second) */
    if (dt > 0.1f) dt = 0.1f;
    g_duck -= dt * 1.2f;
    if (g_duck < 0) g_duck = 0;
    int want = g_mode == AUDIO_ON ? g_want : -1;
    if (want != g_song) {                          /* fade the old song out, then switch */
        g_gain -= dt / g_fade_out;
        if (g_song < 0 || g_gain <= 0) {
            g_gain = 0;
            start_song(want);
        }
    } else if (g_gain < 1.0f) {
        g_gain += dt / g_fade_in;
        if (g_gain > 1.0f) g_gain = 1.0f;
    }
    read_ahead(4);
    apply_music_volume();
}
