/**
 * xnet_audio.c — XNET voice engine (real implementation)
 *
 * Pipeline:
 *   mic:  XBLC 16kHz → decimate 2:1 → 8kHz 20ms frame → RMS gate (open mic
 *         with hang time) → ADPCM encode → AES → PKT_VOICE
 *   spk:  PKT_VOICE_RELAY → AES decrypt (in main.c) → ADPCM decode →
 *         per-slot jitter queue → mixer (saturating add) → interpolate
 *         1:2 → 16kHz → XBLC speaker ring
 *
 * Everything here runs in task context (main loop). The interrupt-context
 * work lives in xnet_xblc.c behind the ring buffers.
 */

#include "xnet_audio.h"
#include "xnet_xblc.h"
#include "xnet_voice.h"
#include "xnet_net.h"
#include "xnet_crypto.h"
#include "xnet_log.h"

#include <string.h>
#include <windows.h>

/* ── tuning ─────────────────────────────────────────────────────────────────── */
#define PKT_VOICE_ID      0x08  /* must match relay protocol */
#define HANG_FRAMES        15   /* gate stays open ~300ms after voice stops   */
#define JITTER_MAX_FRAMES   8   /* per-slot queue depth (160ms); drop beyond  */
#define JITTER_PREFILL      2   /* frames buffered before a stream starts     */
#define SPK_TARGET_QUEUE 1280   /* 16kHz samples (~80ms) kept in spk ring     */

static uint32_t g_mic_gate = 200000;  /* 0 = always transmit */

/* ── state ──────────────────────────────────────────────────────────────────── */
static voice_enc_state g_enc;
static uint8_t  g_tx_seq;
static int      g_hang;
static int      g_talking;            /* gate currently open (for UI)         */

typedef struct {
    int16_t frames[JITTER_MAX_FRAMES][VOICE_FRAME_SAMPLES];
    int     head, tail, count;
    int     started;                  /* prefill reached, mixing active       */
} slot_queue;

static slot_queue g_rx[MAX_SLOTS];

static uint32_t g_tx_frames, g_rx_frames, g_rx_dropped;
static uint32_t g_last_stats_tick;
static uint32_t g_last_energy = 0;     /* most recent mic frame energy (for UI) */
static uint32_t g_last_peak   = 0;     /* most recent raw mic peak |sample| (clip) */
static int      g_mic_gain    = 100;   /* input gain %, applied pre-encode (25..200) */

static void decimate_2to1(const int16_t* in320, int16_t* out160);

/* Measure the pre-gain peak (for clip detection) and apply mic input gain in
   place. Attenuating a hot mic (gain < 100) before ADPCM encode gives the
   encoder headroom and reduces distortion; boosting (>100) helps a weak mic.
   Returns the PRE-gain peak so the UI can flag a railing ADC that software
   gain can't rescue. */
static uint32_t mic_gain_and_peak(int16_t* buf, int n) {
    uint32_t peak = 0;
    for (int i = 0; i < n; i++) {
        int v = buf[i];
        int a = (v < 0) ? -v : v;
        if ((uint32_t)a > peak) peak = (uint32_t)a;
        if (g_mic_gain != 100) {
            int g = (int)(((long)v * g_mic_gain) / 100);
            if (g >  32767) g =  32767;
            if (g < -32768) g = -32768;
            buf[i] = (int16_t)g;
        }
    }
    return peak;
}

/* ── config hooks ───────────────────────────────────────────────────────────── */
void xnet_audio_set_gate(uint32_t energy) {
    g_mic_gate = energy;
    xnet_logf("audio: mic gate set to %lu%s", (unsigned long)energy,
              energy == 0 ? " (always-on)" : "");
}

int xnet_audio_talking(void)   { return g_talking; }

uint32_t xnet_audio_get_gate(void) { return g_mic_gate; }

uint32_t xnet_audio_last_energy(void) { return g_last_energy; }

uint32_t xnet_audio_last_peak(void) { return g_last_peak; }

void xnet_audio_set_gain(int pct) {
    if (pct < 25)  pct = 25;
    if (pct > 200) pct = 200;
    g_mic_gain = pct;
    xnet_logf("audio: mic gain set to %d%%", pct);
}

int xnet_audio_get_gain(void) { return g_mic_gain; }

/* Read the mic and update the live energy/talking readouts WITHOUT transmitting.
   Used by the Settings screen so the user can watch their voice level against the
   gate while tuning it. Safe to call when not connected to a room. */
void xnet_audio_monitor(void) {
    if (!xnet_xblc_connected()) { g_last_energy = 0; g_talking = 0; return; }

    int16_t raw16k[VOICE_FRAME_SAMPLES * 2];
    int16_t pcm8k[VOICE_FRAME_SAMPLES];
    int got = 0;

    while (xnet_xblc_mic_available() >= VOICE_FRAME_SAMPLES * 2) {
        xnet_xblc_read_mic(raw16k, VOICE_FRAME_SAMPLES * 2);
        g_last_peak = mic_gain_and_peak(raw16k, VOICE_FRAME_SAMPLES * 2);
        decimate_2to1(raw16k, pcm8k);
        g_last_energy = voice_frame_energy(pcm8k);
        got = 1;
    }
    if (got)
        g_talking = (g_mic_gate == 0) || (g_last_energy > g_mic_gate);
}
int xnet_audio_headset_ok(void){ return xnet_xblc_connected(); }

/* ── init: just register the USB driver; enumeration happens later ─────────── */
int xnet_audio_init(void) {
    xnet_xblc_register();
    memset(&g_enc, 0, sizeof(g_enc));
    memset(g_rx, 0, sizeof(g_rx));
    return 0;
}

/* ── RX: decoded-frame intake from network (called post-decrypt) ───────────── */
void xnet_audio_queue_playback(uint8_t slot, const uint8_t* frame, int len) {
    if (slot >= MAX_SLOTS) return;

    slot_queue* q = &g_rx[slot];
    int16_t pcm[VOICE_FRAME_SAMPLES];

    if (voice_decode_frame(frame, len, pcm, NULL) != VOICE_FRAME_SAMPLES) {
        return;  /* malformed/foreign frame */
    }
    g_rx_frames++;

    if (q->count >= JITTER_MAX_FRAMES) {
        /* queue full: drop oldest to keep latency bounded */
        q->tail = (q->tail + 1) % JITTER_MAX_FRAMES;
        q->count--;
        g_rx_dropped++;
    }
    memcpy(q->frames[q->head], pcm, sizeof(pcm));
    q->head = (q->head + 1) % JITTER_MAX_FRAMES;
    q->count++;

    if (!q->started && q->count >= JITTER_PREFILL) {
        q->started = 1;
    }
}

/* ── helpers ────────────────────────────────────────────────────────────────── */
static int16_t sat16(int v) {
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

/* 16kHz -> 8kHz by averaging sample pairs (cheap anti-alias) */
static void decimate_2to1(const int16_t* in320, int16_t* out160) {
    for (int i = 0; i < VOICE_FRAME_SAMPLES; i++) {
        out160[i] = (int16_t)(((int)in320[2 * i] + (int)in320[2 * i + 1]) / 2);
    }
}

/* 8kHz -> 16kHz by linear interpolation */
static void interpolate_1to2(const int16_t* in160, int16_t* out320) {
    for (int i = 0; i < VOICE_FRAME_SAMPLES; i++) {
        int cur = in160[i];
        int nxt = (i + 1 < VOICE_FRAME_SAMPLES) ? in160[i + 1] : cur;
        out320[2 * i]     = (int16_t)cur;
        out320[2 * i + 1] = (int16_t)((cur + nxt) / 2);
    }
}

/* ── per-loop tick ──────────────────────────────────────────────────────────── */
void xnet_audio_tick(XNetState* state) {
    if (!xnet_xblc_connected()) {
        g_talking = 0;
        return;
    }

    /* Voice is live in both text chat and Secure Video. Previously this only
       allowed SCREEN_CHAT, so the mic ring was drained but nothing transmitted
       during video — hence silent video calls. */
    int in_chat = ((state->screen == SCREEN_CHAT) ||
                   (state->screen == SCREEN_VIDEO)) && (state->sock >= 0);

    /* ---- MIC: drain everything captured since last tick ---- */
    int16_t raw16k[VOICE_FRAME_SAMPLES * 2];
    int16_t pcm8k[VOICE_FRAME_SAMPLES];

    while (xnet_xblc_mic_available() >= VOICE_FRAME_SAMPLES * 2) {
        xnet_xblc_read_mic(raw16k, VOICE_FRAME_SAMPLES * 2);

        if (!in_chat) continue;  /* keep ring drained even outside chat */

        g_last_peak = mic_gain_and_peak(raw16k, VOICE_FRAME_SAMPLES * 2);
        decimate_2to1(raw16k, pcm8k);

        uint32_t energy = voice_frame_energy(pcm8k);
        if (g_mic_gate == 0 || energy > g_mic_gate) {
            g_hang = HANG_FRAMES;
        } else if (g_hang > 0) {
            g_hang--;
        }
        g_talking = (g_hang > 0) || (g_mic_gate == 0);
        if (!g_talking) continue;

        uint8_t frame[VOICE_FRAME_BYTES];
        uint8_t cipher[VOICE_FRAME_BYTES + 16 + 16 + 32]; /* frame + IV + pad + MAC */
        voice_encode_frame(&g_enc, pcm8k, g_tx_seq++, frame);

        int clen = xnet_crypto_encrypt(state->aes_key, frame,
                                       VOICE_FRAME_BYTES,
                                       cipher, sizeof(cipher));
        if (clen > 0) {
            xnet_net_send_pkt(state->sock, PKT_VOICE_ID, state->my_slot,
                              cipher, clen);
            g_tx_frames++;
        }
    }

    /* ---- SPEAKER: mix one frame across active slots when ring runs low ---- */
    while (xnet_xblc_spk_queued() < SPK_TARGET_QUEUE) {
        int mix[VOICE_FRAME_SAMPLES];
        int active = 0;
        memset(mix, 0, sizeof(mix));

        for (int s = 0; s < MAX_SLOTS; s++) {
            slot_queue* q = &g_rx[s];
            if (!q->started) continue;
            if (q->count == 0) { q->started = 0; continue; }  /* stream idle */

            const int16_t* f = q->frames[q->tail];
            for (int i = 0; i < VOICE_FRAME_SAMPLES; i++) mix[i] += f[i];
            q->tail = (q->tail + 1) % JITTER_MAX_FRAMES;
            q->count--;
            active++;
        }

        if (active == 0) break;  /* nothing to play; ring drains to silence */

        int16_t mixed[VOICE_FRAME_SAMPLES];
        int16_t out16k[VOICE_FRAME_SAMPLES * 2];
        for (int i = 0; i < VOICE_FRAME_SAMPLES; i++) mixed[i] = sat16(mix[i]);
        interpolate_1to2(mixed, out16k);
        xnet_xblc_write_spk(out16k, VOICE_FRAME_SAMPLES * 2);
    }

    /* ---- periodic stats to xnet.log (every ~10s) ---- */
    uint32_t now = GetTickCount();
    if (now - g_last_stats_tick > 10000) {
        g_last_stats_tick = now;
        if (in_chat) {
            xnet_vlogf("voice: tx=%lu rx=%lu dropped=%lu talking=%d",
                      (unsigned long)g_tx_frames, (unsigned long)g_rx_frames,
                      (unsigned long)g_rx_dropped, g_talking);
            xnet_xblc_log_stats();
        }
    }
}
