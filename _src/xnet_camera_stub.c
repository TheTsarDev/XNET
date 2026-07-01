/*
 * xnet_camera_stub.c
 *
 * Placeholder capture backend. Satisfies xnet_camera.h so the rest of the
 * video pipeline (capture loop, encryption, relay, decode, tile display)
 * builds and runs before the real OV519 driver is ported.
 *
 * It "captures" an embedded 320x240 baseline JPEG and hands out a fresh copy
 * on a ~12 fps cadence, so two consoles running this can prove the whole
 * network+decode+display path on real hardware with no camera attached. The
 * far console will show color bars labelled XNET TEST in that peer's tile.
 *
 * Swap this file out for xnet_camera_nxdk.c (the ported driver) in the
 * Makefile when the driver is ready — nothing else changes.
 */

#include "xnet_camera.h"
#include "xnet_log.h"

#include <windows.h>   /* GetTickCount */

#include "_testframe.inc"   /* k_test_jpeg[], k_test_jpeg_len */

#define STUB_FRAME_INTERVAL_MS  80   /* ~12.5 fps */

static int           s_inited = 0;
static unsigned long s_last_ms = 0;
static int           s_emitted = 0;   /* test frames handed out (for debug stats) */

int xnet_camera_init(void) {
    s_inited  = 1;
    s_last_ms = 0;
    xnet_logf("camera(stub): init -> test-pattern source, %d byte frames",
              k_test_jpeg_len);
    return 0;
}

void xnet_camera_register(void) {
    /* stub has no USB driver to register */
}


int xnet_camera_streaming(void) {
    return s_inited;
}

int xnet_camera_get_frame(const uint8_t** jpeg, int* len) {
    if (!s_inited) return 0;

    unsigned long now = GetTickCount();
    if (s_last_ms != 0 && (now - s_last_ms) < STUB_FRAME_INTERVAL_MS)
        return 0;  /* throttle to the stub frame rate */

    s_last_ms = now;
    *jpeg = k_test_jpeg;
    *len  = k_test_jpeg_len;
    s_emitted++;
    return 1;
}

void xnet_camera_shutdown(void) {
    s_inited = 0;
}
