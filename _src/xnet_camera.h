#ifndef XNET_CAMERA_H
#define XNET_CAMERA_H

#include <stdint.h>

/* Capture geometry — matches the tested OV519/EyeToy QVGA path. */
#define XNET_CAM_W 320
#define XNET_CAM_H 240

/*
 * Camera capture backend contract.
 *
 * The whole point of this header is to isolate XNET from the USB driver. The
 * stub backend (xnet_camera_stub.c) satisfies it today so the network/decode/
 * display pipeline can be built and tested before the driver exists. The real
 * NXDK OV519 driver — ported from Darkone83's RXDK research onto the same
 * libusbohci iso path the XBLC mic driver uses — drops in behind this same
 * interface with no changes to the rest of XNET.
 *
 * The backend produces complete baseline-JPEG frames (the OV519's on-chip JPEG
 * engine output). XNET never decodes on the capture side — it ships the JPEG
 * bytes straight over the wire.
 */

/* Register the capture backend with the USB core. Called ONCE at boot, right
   after the XBLC audio driver is registered (so the USB core is already up and
   we don't re-init it). The stub provides a no-op so the call site is identical
   for both backends. */
void xnet_camera_register(void);

/* Bring up the camera. 0 = ok, negative = failure. */
int  xnet_camera_init(void);

/* Nonzero once frames are actually flowing. init() can return ok before the
   stream is live (see Darkone83's notes on XCam_Init semantics), so poll this
   rather than trusting init's return alone. */
int  xnet_camera_streaming(void);

/* Latest complete JPEG frame. Returns 1 and fills *jpeg / *len when a NEW
   frame is available since the previous call; returns 0 when nothing new.
   The buffer is owned by the backend and stays valid until the next call. */
int  xnet_camera_get_frame(const uint8_t** jpeg, int* len);

void xnet_camera_shutdown(void);

/* Diagnostic counters for the on-console DEBUG CAMERA screen. Any pointer may
   be NULL. On the stub backend these reflect the test-pattern source. */
void xnet_camera_debug_stats(uint32_t* irqs, uint32_t* bytes,
                             uint32_t* sof, uint32_t* eof, int* completed);

#endif /* XNET_CAMERA_H */
