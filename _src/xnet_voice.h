/**
 * xnet_voice.h — voice frame codec (IMA ADPCM 4:1), shared XBE/host
 */

#ifndef XNET_VOICE_H
#define XNET_VOICE_H

#include <stdint.h>

#define VOICE_SAMPLE_RATE    8000
#define VOICE_RATE_IDX       0          /* XBLC rate index for 8000 Hz   */
#define VOICE_FRAME_MS       20
#define VOICE_FRAME_SAMPLES  160        /* 8000 * 20 / 1000              */
#define VOICE_FRAME_ADPCM    (VOICE_FRAME_SAMPLES / 2)   /* 80 bytes     */
#define VOICE_FRAME_BYTES    (6 + VOICE_FRAME_ADPCM)     /* 86 plaintext */

typedef struct {
    int step_index;   /* carried across frames; init to 0 */
} voice_enc_state;

/** Encode 160 PCM samples into an 86-byte self-contained frame.
 *  Returns VOICE_FRAME_BYTES. */
int voice_encode_frame(voice_enc_state* st,
                       const int16_t* pcm, uint8_t seq, uint8_t* out);

/** Decode an 86-byte frame into 160 PCM samples.
 *  Returns sample count, or -1 on malformed frame. */
int voice_decode_frame(const uint8_t* in, int len,
                       int16_t* pcm, uint8_t* seq_out);

/** Mean square energy of a frame, for the mic noise gate. */
uint32_t voice_frame_energy(const int16_t* pcm);

#endif /* XNET_VOICE_H */
