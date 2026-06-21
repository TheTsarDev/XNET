/**
 * xnet_xblc.h — Xbox Live Communicator host driver (class 0x78)
 * Native 16 kHz 16-bit mono PCM both directions.
 */

#ifndef XNET_XBLC_H
#define XNET_XBLC_H

#include <stdint.h>

/** Register the XBLC class driver with the usbh stack.
 *  MUST be called before the USB core enumerates devices — i.e. before
 *  SDL_Init, which brings the core up. Safe to call before core init. */
void xnet_xblc_register(void);

/** 1 when both mic and speaker interfaces are streaming. */
int xnet_xblc_connected(void);

/** Samples (16 kHz) waiting in the mic ring. */
uint32_t xnet_xblc_mic_available(void);

/** Pull up to n mic samples; returns count actually read. */
uint32_t xnet_xblc_read_mic(int16_t* dst, uint32_t n);

/** Queue speaker samples (16 kHz). Underrun plays silence. */
void xnet_xblc_write_spk(const int16_t* src, uint32_t n);

/** Samples queued for the speaker. */
uint32_t xnet_xblc_spk_queued(void);

/** Dump IRQ counters to xnet.log (call from task context only). */
void xnet_xblc_log_stats(void);

#endif /* XNET_XBLC_H */
