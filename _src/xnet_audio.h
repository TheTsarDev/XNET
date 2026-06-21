/**
 * xnet_audio.h
 * XNET voice engine: Xbox Live Communicator capture/playback.
 *
 * Real implementation (xnet_audio.c + xnet_xblc.c): 16kHz XBLC native,
 * 8kHz IMA ADPCM on the wire, open mic with RMS gate, per-slot jitter
 * queues with mixing. Stub (xnet_audio_stub.c) for XNET_NO_AUDIO=1 builds.
 */

#ifndef XNET_AUDIO_H
#define XNET_AUDIO_H

#include <stdint.h>
#include "main.h"

/** Register the XBLC USB driver. MUST run before SDL_Init (i.e. before the
 *  first xnet_ui_flip), so the driver is in place when USB enumerates.
 *  Returns 0. */
int xnet_audio_init(void);

/** Per-main-loop tick: drains mic → encodes/encrypts/sends voice frames
 *  (when in chat), mixes per-slot RX queues → speaker. Task context only. */
void xnet_audio_tick(XNetState* state);

/** Queue a DECRYPTED voice frame from PKT_VOICE_RELAY for playback. */
void xnet_audio_queue_playback(uint8_t slot, const uint8_t* frame, int len);

/** Set mic gate threshold (mean-square frame energy). 0 = always transmit.
 *  Same scale as the PC client's XNET_MIC_GATE. */
void xnet_audio_set_gate(uint32_t energy);

/** Get the current mic gate threshold. */
uint32_t xnet_audio_get_gate(void);

/** Update live mic energy/talking readouts without transmitting (Settings UI). */
void xnet_audio_monitor(void);

/** Most recent mic frame energy, for live metering. */
uint32_t xnet_audio_last_energy(void);

/** Most recent raw mic peak |sample| (0..32767), for clip detection. */
uint32_t xnet_audio_last_peak(void);

/** Mic input gain in percent (25..200). Applied before the ADPCM encoder. */
void xnet_audio_set_gain(int pct);
int  xnet_audio_get_gain(void);

/** 1 while the gate is open and frames are being transmitted (UI). */
int xnet_audio_talking(void);

/** 1 when a Communicator/Hawk is enumerated and streaming (UI). */
int xnet_audio_headset_ok(void);

#endif /* XNET_AUDIO_H */
