/**
 * xnet_voice.c — voice frame codec (IMA ADPCM 4:1)
 * Shared verbatim between the XBE and host-side tools/tests.
 *
 * Frame = 20ms of 8kHz 16-bit mono = 160 samples.
 * Each frame is self-contained (predictor + step index in the header),
 * so a lost packet never corrupts the frames after it.
 *
 * Wire layout (plaintext, before AES):
 *   [0] seq          — rolling frame counter
 *   [1] flags        — bit0..2: sample rate idx (0=8k); rest reserved
 *   [2] predictor hi — initial predictor, int16 BE
 *   [3] predictor lo
 *   [4] step index   — IMA step table index 0..88
 *   [5] reserved
 *   [6..85] 80 bytes ADPCM (two 4-bit codes per byte, low nibble first)
 */

#include "xnet_voice.h"
#include <string.h>

static const int8_t ima_index_table[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8,
};

static const int16_t ima_step_table[89] = {
        7,     8,     9,    10,    11,    12,    13,    14,    16,    17,
       19,    21,    23,    25,    28,    31,    34,    37,    41,    45,
       50,    55,    60,    66,    73,    80,    88,    97,   107,   118,
      130,   143,   157,   173,   190,   209,   230,   253,   279,   307,
      337,   371,   408,   449,   494,   544,   598,   658,   724,   796,
      876,   963,  1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
     2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,
     5894,  6484,  7132,  7845,  8630,  9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};

static int clamp16(int v) {
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return v;
}

static uint8_t ima_encode_sample(int sample, int* predictor, int* index) {
    int step = ima_step_table[*index];
    int diff = sample - *predictor;
    uint8_t code = 0;

    if (diff < 0) { code = 8; diff = -diff; }

    int delta = step >> 3;
    if (diff >= step)        { code |= 4; diff -= step; delta += step; }
    step >>= 1;
    if (diff >= step)        { code |= 2; diff -= step; delta += step; }
    step >>= 1;
    if (diff >= step)        { code |= 1;               delta += step; }

    if (code & 8) *predictor = clamp16(*predictor - delta);
    else          *predictor = clamp16(*predictor + delta);

    *index += ima_index_table[code];
    if (*index < 0)  *index = 0;
    if (*index > 88) *index = 88;

    return code;
}

static int ima_decode_sample(uint8_t code, int* predictor, int* index) {
    int step  = ima_step_table[*index];
    int delta = step >> 3;

    if (code & 4) delta += step;
    if (code & 2) delta += step >> 1;
    if (code & 1) delta += step >> 2;

    if (code & 8) *predictor = clamp16(*predictor - delta);
    else          *predictor = clamp16(*predictor + delta);

    *index += ima_index_table[code];
    if (*index < 0)  *index = 0;
    if (*index > 88) *index = 88;

    return *predictor;
}

/* encoder carries step index across frames for quality, but writes it
   into every header so each frame decodes independently */
int voice_encode_frame(voice_enc_state* st,
                       const int16_t* pcm, uint8_t seq, uint8_t* out) {
    int predictor = pcm[0];
    int index     = st->step_index;

    out[0] = seq;
    out[1] = VOICE_RATE_IDX & 0x07;
    out[2] = (uint8_t)((predictor >> 8) & 0xFF);
    out[3] = (uint8_t)(predictor & 0xFF);
    out[4] = (uint8_t)index;
    out[5] = 0;

    for (int i = 0; i < VOICE_FRAME_SAMPLES; i += 2) {
        uint8_t lo = ima_encode_sample(pcm[i],     &predictor, &index);
        uint8_t hi = ima_encode_sample(pcm[i + 1], &predictor, &index);
        out[6 + i / 2] = (uint8_t)(lo | (hi << 4));
    }

    st->step_index = index;
    return VOICE_FRAME_BYTES;
}

int voice_decode_frame(const uint8_t* in, int len,
                       int16_t* pcm, uint8_t* seq_out) {
    if (len != VOICE_FRAME_BYTES) return -1;

    int predictor = (int16_t)(((int)in[2] << 8) | in[3]);
    int index     = in[4];
    if (index > 88) return -1;
    if ((in[1] & 0x07) != VOICE_RATE_IDX) return -1;

    if (seq_out) *seq_out = in[0];

    for (int i = 0; i < VOICE_FRAME_SAMPLES; i += 2) {
        uint8_t b = in[6 + i / 2];
        pcm[i]     = (int16_t)ima_decode_sample(b & 0x0F, &predictor, &index);
        pcm[i + 1] = (int16_t)ima_decode_sample(b >> 4,   &predictor, &index);
    }
    return VOICE_FRAME_SAMPLES;
}

/* simple RMS energy for the mic noise gate */
uint32_t voice_frame_energy(const int16_t* pcm) {
    uint64_t acc = 0;
    for (int i = 0; i < VOICE_FRAME_SAMPLES; i++) {
        acc += (int64_t)pcm[i] * pcm[i];
    }
    return (uint32_t)(acc / VOICE_FRAME_SAMPLES);
}
