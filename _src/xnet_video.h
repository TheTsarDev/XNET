#ifndef XNET_VIDEO_H
#define XNET_VIDEO_H

#include <stdint.h>
#include "main.h"   /* MAX_SLOTS, XNetState */

/* Decoded tile geometry (matches the camera capture size). */
#define XVID_W 320
#define XVID_H 240

/* Decoded frames are STORED at half resolution to fit a 64MB console. The
   tiles are small on screen, so 160x120 upscales fine. Decode is still native
   320x240; on_frame subsamples 2:1 into these buffers. */
#define XVID_DW 160
#define XVID_DH 120

/* Clear all per-slot decoded frames (call when entering/leaving a session). */
void xnet_video_reset(void);

/* Clear one slot's frame (e.g. local camera toggled off). */
void xnet_video_clear_slot(int slot);

/* A peer's encrypted JPEG arrived: decrypt already done by caller — this takes
   the plaintext JPEG, decodes it, and stores the result for that slot. */
void xnet_video_on_frame(int slot, const uint8_t* jpeg, int len);

/* Receive path: stash the latest decrypted JPEG for a slot WITHOUT decoding
   (cheap memcpy). Call xnet_video_decode_pending() from the draw loop to decode
   the newest stashed frame per slot, throttled — keeps recv from starving render. */
void xnet_video_stash_frame(int slot, const uint8_t* jpeg, int len);
void xnet_video_decode_pending(void (*between)(void));

/* On-screen debug: per-slot received/decoded counts, last pjpeg rc, pending len. */
void xnet_video_slot_stats(int slot, int* rx, int* ok, int* rc, int* pend);

/* Mark a slot as camera-off (peer present but not sending video). */
void xnet_video_set_camoff(int slot, int off);

/* Renderer accessors. */
int             xnet_video_slot_has(int slot);     /* 1 if a decoded frame exists */
int             xnet_video_slot_camoff(int slot);  /* 1 if peer flagged camera off */
const uint32_t* xnet_video_slot_pixels(int slot);  /* XVID_W*XVID_H ARGB, or NULL */

/* Capture side: poll the camera; if a new frame is ready, encrypt and send it
   as PKT_VIDEO. Returns 1 if a frame was sent this call, 0 otherwise. Safe to
   call every loop iteration — it self-throttles to the camera's frame rate. */
int xnet_video_capture_and_send(XNetState* st);

#endif /* XNET_VIDEO_H */
