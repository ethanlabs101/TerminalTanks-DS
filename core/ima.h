/* IMA-ADPCM exactly as the DS sound hardware decodes it (GBATEK "IMA-ADPCM": the shift-add difference, clamped to
 * +-0x7FFF). One codec for both sides: tools/gen_audio.py encodes with it on the PC (through tools/ima_lib.c) and the
 * DS decodes the music stream with it in software, so the two can never disagree. Sound effects use the same nibbles
 * and are played by the hardware directly. */
#ifndef TT_IMA_H
#define TT_IMA_H
#include <stdint.h>

static const int16_t IMA_STEP[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060,
    1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
    7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
static const int8_t IMA_INDEX[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

typedef struct { int pcm, index; } ImaState;

static inline int ima_step(ImaState *s, int nib) {
    int st = IMA_STEP[s->index];
    int d = st >> 3;
    if (nib & 1) d += st >> 2;
    if (nib & 2) d += st >> 1;
    if (nib & 4) d += st;
    if (nib & 8) { s->pcm -= d; if (s->pcm < -0x7FFF) s->pcm = -0x7FFF; }
    else         { s->pcm += d; if (s->pcm > 0x7FFF) s->pcm = 0x7FFF; }
    s->index += IMA_INDEX[nib & 7];
    if (s->index < 0) s->index = 0;
    if (s->index > 88) s->index = 88;
    return s->pcm;
}

/* Decode n samples (n even) from bytes, low nibble first. */
static inline void ima_decode(ImaState *s, const uint8_t *in, int16_t *out, int n) {
    for (int i = 0; i < n; i += 2) {
        int b = in[i >> 1];
        out[i] = (int16_t)ima_step(s, b & 15);
        out[i + 1] = (int16_t)ima_step(s, b >> 4);
    }
}

/* Encode n samples (n even): picks the nibble whose decoded result lands closest (greedy, decoder-exact). */
static inline void ima_encode(ImaState *s, const int16_t *in, uint8_t *out, int n) {
    for (int i = 0; i < n; i++) {
        int best = 0, bd = 1 << 30;
        for (int nib = 0; nib < 16; nib++) {
            ImaState t = *s;
            int e = ima_step(&t, nib) - in[i];
            if (e < 0) e = -e;
            if (e < bd) { bd = e; best = nib; }
        }
        ima_step(s, best);
        if (i & 1) out[i >> 1] |= (uint8_t)(best << 4);
        else out[i >> 1] = (uint8_t)best;
    }
}

/* The music pack (nitro:/audio/music.pak) and the effects pack (sfx.pak). Little-endian. */
#define MUS_MAGIC 0x554D5454u     /* "TTMU" */
#define SFX_MAGIC 0x46535454u     /* "TTSF" */
#define MUS_BLOCK_BYTES 2048      /* 4-byte state (pcm16, index, 0) + 2044 bytes of nibbles */
#define MUS_BLOCK_SAMPLES 4088
#define AUDIO_TIMER 1013          /* sound-channel timer: 16756991 / 1013 = 16542 Hz (the packs are 22050 * 3/4) */
#define AUDIO_HZ 16537            /* what to pass to soundPlaySample so the ARM7 programs AUDIO_TIMER */

typedef struct { uint32_t off, nblocks, loop_end, loop_start; } MusEntry;   /* samples; loop_start is block aligned */
typedef struct { uint32_t off, bytes; } SfxEntry;                            /* hardware ADPCM incl. its 4-byte header */
#endif
