/* ctypes helper for tools/gen_audio.py: the DS codec from core/ima.h, compiled for the PC. */
#include "../core/ima.h"

/* Music: one block = state header + MUS_BLOCK_SAMPLES samples. in has nblocks * MUS_BLOCK_SAMPLES samples. */
void enc_blocks(const int16_t *in, int nblocks, uint8_t *out) {
    ImaState s = {0, 0};
    for (int b = 0; b < nblocks; b++) {
        uint8_t *o = out + b * MUS_BLOCK_BYTES;
        int16_t p = (int16_t)s.pcm;
        o[0] = (uint8_t)(p & 255); o[1] = (uint8_t)((uint16_t)p >> 8); o[2] = (uint8_t)s.index; o[3] = 0;
        ima_encode(&s, in + b * MUS_BLOCK_SAMPLES, o + 4, MUS_BLOCK_SAMPLES);
    }
}

void dec_blocks(const uint8_t *in, int nblocks, int16_t *out) {
    for (int b = 0; b < nblocks; b++) {
        const uint8_t *o = in + b * MUS_BLOCK_BYTES;
        ImaState s = {(int16_t)(o[0] | (o[1] << 8)), o[2]};
        ima_decode(&s, o + 4, out + b * MUS_BLOCK_SAMPLES, MUS_BLOCK_SAMPLES);
    }
}

/* Sound effect for the hardware: header word (first sample's pcm16, index) then nibbles; n even. The hardware starts
 * from the header state and decodes every nibble, so the header holds the state before sample 0. */
void enc_hw(const int16_t *in, int n, uint8_t *out) {
    ImaState s = {in[0], 0};
    out[0] = (uint8_t)(in[0] & 255); out[1] = (uint8_t)((uint16_t)in[0] >> 8); out[2] = 0; out[3] = 0;
    ima_encode(&s, in, out + 4, n);
}

void dec_hw(const uint8_t *in, int n, int16_t *out) {
    ImaState s = {(int16_t)(in[0] | (in[1] << 8)), in[2]};
    ima_decode(&s, in + 4, out, n);
}
