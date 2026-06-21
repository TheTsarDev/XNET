/**
 * xnet_audio_stub.c
 * Stub audio module for builds without Communicator support.
 * Compile with: make XNET_NO_AUDIO=1 — XNET runs in text-only mode.
 */

#include "xnet_audio.h"

int  xnet_audio_init(void)                                               { return 0; }
void xnet_audio_tick(XNetState* s)                                       { (void)s; }
void xnet_audio_queue_playback(uint8_t slot, const uint8_t* f, int len)  { (void)slot; (void)f; (void)len; }
void xnet_audio_set_gate(uint32_t energy)                                { (void)energy; }
int  xnet_audio_talking(void)                                            { return 0; }
int  xnet_audio_headset_ok(void)                                         { return 0; }
