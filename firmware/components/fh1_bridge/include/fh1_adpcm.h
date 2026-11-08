// IMA ADPCM (4 bits a sample), the standard tables: the FH1 encodes, a bridge decodes (fh1_bridge_proto.h,
// FH1B_UDP_AUDIO). Two samples a byte, the first in the low nibble. Header-only, shared by both firmwares.
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct {
    int16_t predictor;
    uint8_t index;
} fh1_adpcm_t;

static const int16_t fh1_adpcm_steps[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
    107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
    5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767,
};
static const int8_t fh1_adpcm_index_step[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

static inline int16_t fh1_adpcm_step_one(fh1_adpcm_t *s, uint8_t code)
{
    const int step = fh1_adpcm_steps[s->index];
    int diff = step >> 3;
    if (code & 4) diff += step;
    if (code & 2) diff += step >> 1;
    if (code & 1) diff += step >> 2;
    int p = s->predictor + (code & 8 ? -diff : diff);
    s->predictor = (int16_t)(p > 32767 ? 32767 : p < -32768 ? -32768 : p);
    int i = s->index + fh1_adpcm_index_step[code & 15];
    s->index = (uint8_t)(i < 0 ? 0 : i > 88 ? 88 : i);
    return s->predictor;
}

static inline uint8_t fh1_adpcm_encode_one(fh1_adpcm_t *s, int16_t sample)
{
    const int step = fh1_adpcm_steps[s->index];
    int diff = sample - s->predictor;
    uint8_t code = 0;
    if (diff < 0) {
        code = 8;
        diff = -diff;
    }
    if (diff >= step) code |= 4, diff -= step;
    if (diff >= step >> 1) code |= 2, diff -= step >> 1;
    if (diff >= step >> 2) code |= 1;
    fh1_adpcm_step_one(s, code);       // the decoder's view, so both stay in step
    return code;
}

// n even. out: n / 2 bytes.
static inline void fh1_adpcm_encode(fh1_adpcm_t *s, const int16_t *in, size_t n, uint8_t *out)
{
    for (size_t i = 0; i < n; i += 2) out[i / 2] = (uint8_t)(fh1_adpcm_encode_one(s, in[i]) | fh1_adpcm_encode_one(s, in[i + 1]) << 4);
}

static inline void fh1_adpcm_decode(fh1_adpcm_t *s, const uint8_t *in, size_t n, int16_t *out)
{
    for (size_t i = 0; i < n; i += 2) {
        out[i] = fh1_adpcm_step_one(s, in[i / 2] & 15);
        out[i + 1] = fh1_adpcm_step_one(s, in[i / 2] >> 4);
    }
}
