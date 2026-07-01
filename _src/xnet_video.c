/*
 * xnet_video.c
 *
 * Secure Video session plumbing.
 *
 * Capture side: pull a JPEG frame from the camera backend (xnet_camera.h),
 * AES-encrypt it, send it as one PKT_VIDEO packet. One frame fits in a single
 * packet (OV519 JPEG frames are ~2.4-3.8 KB, under the 8 KB payload cap), so
 * there is no chunking here — unlike file transfer.
 *
 * Receive side: a peer's decrypted JPEG comes in via xnet_video_on_frame();
 * picojpeg decodes it into that slot's persistent ARGB buffer. The UI grid
 * renderer (xnet_ui_draw_video_grid) blits those buffers into tiles.
 *
 * Pixel format: XNET's back buffer is 32-bit 0xAARRGGBB, so we decode straight
 * to that — no swizzle, no D3D (the original research targeted a D3D8 texture;
 * here we own a linear software framebuffer).
 */

#include "xnet_video.h"
#include "xnet_camera.h"
#include "xnet_crypto.h"
#include "xnet_net.h"
#include "xnet_log.h"
#include "main.h"

#include <string.h>
#include <windows.h>          /* GetTickCount */
#include "vendor/picojpeg.h"

/* PKT_VIDEO must match the value in main.c / the relay. */
#define PKT_VIDEO        0x18

/* Largest JPEG we will send/accept in one packet. Encrypted size = len rounded
   up to 16 + 16-byte IV, must stay under the 8192 payload cap with margin. */
#define XVID_MAX_JPEG    7600

/* ── per-slot decoded frame store ────────────────────────────────────────────── */
static uint32_t s_pixels[MAX_SLOTS][XVID_DW * XVID_DH];
static int      s_has[MAX_SLOTS];
static int      s_camoff[MAX_SLOTS];

/* ── per-slot PENDING (received but not yet decoded) frame ──────────────────────
   The receive path must stay cheap: decoding every incoming JPEG inline in the
   recv loop starves the render (frozen grid, "connecting"). Instead recv stashes
   the latest frame here (overwriting any older un-decoded one = automatic frame
   drop), and the draw loop decodes just the newest, throttled. */
static uint8_t  s_pending[MAX_SLOTS][XVID_MAX_JPEG];
static int      s_pending_len[MAX_SLOTS];

/* on-screen debug counters (no logs available on hardware) */
static int      s_rxN[MAX_SLOTS];     /* frames stashed (received) */
static int      s_okN[MAX_SLOTS];     /* frames decoded successfully */
static int      s_lastRc[MAX_SLOTS];  /* last pjpeg init rc (-1 = none yet) */

void xnet_video_slot_stats(int slot, int* rx, int* ok, int* rc, int* pend) {
    if (slot < 0 || slot >= MAX_SLOTS) return;
    if (rx)   *rx   = s_rxN[slot];
    if (ok)   *ok   = s_okN[slot];
    if (rc)   *rc   = s_lastRc[slot];
    if (pend) *pend = s_pending_len[slot];
}

/* scratch for an outgoing encrypted frame: IV + padded plaintext (+margin) */
static uint8_t  s_tx_cipher[8 + 16 + XVID_MAX_JPEG + 16 + 32]; /* seq + IV + jpeg + pad + MAC */

/* ── picojpeg feed state (decode is synchronous + single-threaded) ───────────── */
static const uint8_t* s_jpg_ptr;
static int            s_jpg_rem;

static unsigned char jpg_feed(unsigned char* pBuf, unsigned char bufSize,
                              unsigned char* pBytesRead, void* pData) {
    int n = bufSize;
    (void)pData;
    if (n > s_jpg_rem) n = s_jpg_rem;
    for (int i = 0; i < n; i++) pBuf[i] = s_jpg_ptr[i];
    s_jpg_ptr += n;
    s_jpg_rem -= n;
    *pBytesRead = (unsigned char)n;
    return 0;
}

void xnet_video_reset(void) {
    memset(s_has, 0, sizeof(s_has));
    memset(s_camoff, 0, sizeof(s_camoff));
    memset(s_pending_len, 0, sizeof(s_pending_len));
    memset(s_rxN, 0, sizeof(s_rxN));
    memset(s_okN, 0, sizeof(s_okN));
    for (int i = 0; i < MAX_SLOTS; i++) s_lastRc[i] = -1;
    /* leave pixel buffers as-is; s_has gates reads */
}

void xnet_video_clear_slot(int slot) {
    if (slot < 0 || slot >= MAX_SLOTS) return;
    s_has[slot] = 0;
    s_pending_len[slot] = 0;
}

/* Receive path: stash the latest JPEG for a slot WITHOUT decoding (cheap).
   Overwriting an undecoded frame drops it — we only ever want the newest. */
void xnet_video_stash_frame(int slot, const uint8_t* jpeg, int len) {
    if (slot < 0 || slot >= MAX_SLOTS) return;
    if (!jpeg || len <= 0 || len > XVID_MAX_JPEG) return;
    memcpy(s_pending[slot], jpeg, len);
    s_pending_len[slot] = len;
    s_rxN[slot]++;
}

/* Draw-loop path: decode the newest pending frame for every slot, at most once
   per ~66ms (≈15fps) so decode cost never starves the render. */
void xnet_video_decode_pending(void (*between)(void)) {
    static unsigned long last_ms = 0;
    unsigned long now = GetTickCount();
    if (now - last_ms < 100) return;   /* ~10fps decode cap (receiver CPU) */
    last_ms = now;
    for (int s = 0; s < MAX_SLOTS; s++) {
        int len = s_pending_len[s];
        if (len > 0) {
            s_pending_len[s] = 0;          /* consume */
            xnet_video_on_frame(s, s_pending[s], len);
            /* keep voice serviced between per-slot decodes so a multi-peer
               decode burst can't starve audio for tens of ms */
            if (between) between();
        }
    }
}

void xnet_video_set_camoff(int slot, int off) {
    if (slot < 0 || slot >= MAX_SLOTS) return;
    s_camoff[slot] = off ? 1 : 0;
}

int             xnet_video_slot_has(int slot)    { return (slot>=0 && slot<MAX_SLOTS) ? s_has[slot] : 0; }
int             xnet_video_slot_camoff(int slot) { return (slot>=0 && slot<MAX_SLOTS) ? s_camoff[slot] : 0; }
const uint32_t* xnet_video_slot_pixels(int slot) { return (slot>=0 && slot<MAX_SLOTS) ? s_pixels[slot] : 0; }

/* Decode one JPEG frame into slot's ARGB buffer. MCU-assembly adapted from
   Darkone83's Cam_DecodeJpegToYUY2 (the block-offset tables handle the four
   chroma-subsampling layouts). */
void xnet_video_on_frame(int slot, const uint8_t* jpeg, int len) {
    if (slot < 0 || slot >= MAX_SLOTS) return;
    if (!jpeg || len <= 0) return;

    pjpeg_image_info_t info;
    unsigned char rc;
    int mcuX = 0, mcuY = 0;
    int W = XVID_W, H = XVID_H;
    uint32_t* dst = s_pixels[slot];

    s_jpg_ptr = jpeg;
    s_jpg_rem = len;
    rc = pjpeg_decode_init(&info, jpg_feed, 0, 0);
    s_lastRc[slot] = (int)rc;
    if (rc != 0) {
        xnet_logf("video: slot %d pjpeg init rc=%d", slot, (int)rc);
        return;
    }

    /* one-time format report: MCU 8x8=4:4:4, 16x8=4:2:2h, 8x16=4:2:2v, 16x16=4:2:0 */
    static int s_fmt_logged = 0;
    if (!s_fmt_logged) {
        s_fmt_logged = 1;
        xnet_logf("video: jpeg fmt comps=%d mcu=%dx%d %dx%d",
                  info.m_comps, info.m_MCUWidth, info.m_MCUHeight,
                  info.m_width, info.m_height);
    }

    /* Watchdog bound for the MCU loop below. picojpeg.h specifies the image is
       fully decoded after exactly m_MCUSPerRow*m_MCUSPerCol calls to
       pjpeg_decode_mcu(). A malformed or truncated inbound frame can make the
       decoder spin without ever returning PJPG_NO_MORE_BLOCKS, which HARD-
       FREEZES the console (no crash, just a hang in the draw loop). Bounding the
       loop turns a bad frame into a dropped frame. */
    int total_mcus = info.m_MCUSPerRow * info.m_MCUSPerCol;
    int mcu_seen   = 0;
    if (total_mcus <= 0 || total_mcus > 4096) {   /* sane ceiling for <=320x240 */
        xnet_logf("video: slot %d bad mcu count %d, dropping frame", slot, total_mcus);
        return;
    }

    for (;;) {
        int nbx, nby, nblk, bi;
        int ofsTab[4], bxTab[4], byTab[4];
        if (mcu_seen++ > total_mcus) {            /* runaway decoder — bail, drop frame */
            xnet_logf("video: slot %d decode overrun (>%d MCUs), dropping frame",
                      slot, total_mcus);
            return;
        }
        rc = pjpeg_decode_mcu();
        if (rc == PJPG_NO_MORE_BLOCKS) break;
        if (rc != 0) break;

        nbx  = info.m_MCUWidth  >> 3;
        nby  = info.m_MCUHeight >> 3;
        nblk = nbx * nby;
        if (nblk == 1) { ofsTab[0]=0; bxTab[0]=0; byTab[0]=0; }
        else if (nbx==2 && nby==1) { ofsTab[0]=0;bxTab[0]=0;byTab[0]=0; ofsTab[1]=64;bxTab[1]=1;byTab[1]=0; }
        else if (nbx==1 && nby==2) { ofsTab[0]=0;bxTab[0]=0;byTab[0]=0; ofsTab[1]=128;bxTab[1]=0;byTab[1]=1; }
        else { ofsTab[0]=0;bxTab[0]=0;byTab[0]=0; ofsTab[1]=64;bxTab[1]=1;byTab[1]=0;
               ofsTab[2]=128;bxTab[2]=0;byTab[2]=1; ofsTab[3]=192;bxTab[3]=1;byTab[3]=1; }

        for (bi = 0; bi < nblk; bi++) {
            int ofs = ofsTab[bi];
            int bpx = mcuX * info.m_MCUWidth  + bxTab[bi] * 8;
            int bpy = mcuY * info.m_MCUHeight + byTab[bi] * 8;
            for (int r = 0; r < 8; r++) {
                int py = bpy + r;
                if (py >= H || (py & 1)) continue;       /* 2:1 vertical subsample */
                uint32_t* row = dst + (py >> 1) * XVID_DW;
                for (int c = 0; c < 8; c++) {
                    int px = bpx + c;
                    if (px >= W || (px & 1)) continue;    /* 2:1 horizontal subsample */
                    int idx = ofs + r * 8 + c;
                    int R = info.m_pMCUBufR[idx];
                    int G, B;
                    if (info.m_comps == 1) { G = R; B = R; }
                    else { G = info.m_pMCUBufG[idx]; B = info.m_pMCUBufB[idx]; }
                    row[px >> 1] = 0xFF000000u | ((uint32_t)R << 16) | ((uint32_t)G << 8) | (uint32_t)B;
                }
            }
        }

        mcuX++;
        if (mcuX == info.m_MCUSPerRow) { mcuX = 0; mcuY++; }
    }

    s_has[slot]    = 1;
    s_camoff[slot] = 0;
    s_okN[slot]++;
}

int xnet_video_capture_and_send(XNetState* st) {
    if (!st || st->sock < 0) return 0;
    if (!xnet_camera_streaming()) return 0;

    /* Cap send rate to ~10fps. The peer's console freezes if it has to decode
       too many frames/sec (the receiving side is the bottleneck on a 733MHz P3),
       so we throttle the SENDER to protect every receiver. */
    static unsigned long s_last_tx_ms = 0;
    unsigned long now_tx = GetTickCount();
    if (now_tx - s_last_tx_ms < 100) return 0;

    const uint8_t* jpeg = 0;
    int len = 0;
    if (!xnet_camera_get_frame(&jpeg, &len)) return 0;  /* nothing new */
    if (len <= 0) return 0;
    if (len > XVID_MAX_JPEG) {
        /* a frame this large won't fit one packet; skip it, next one is ~80ms out */
        xnet_logf("video: dropping oversize frame (%d > %d)", len, XVID_MAX_JPEG);
        return 0;
    }
    s_last_tx_ms = now_tx;

    /* Local self-preview FIRST, independent of the network — you should see
       yourself even if the relay is down. Throttled to every 4th frame to
       spare the CPU (decoding our own stream at full rate on top of each
       peer's would be too much). */
    static int s_self_div = 0;
    if ((s_self_div++ & 3) == 0)
        xnet_video_on_frame(st->my_slot, jpeg, len);

    int clen = xnet_replay_seal(&st->replay, st->aes_key,
                                XNET_STREAM_VIDEO, st->my_slot,
                                jpeg, len,
                                s_tx_cipher, sizeof(s_tx_cipher), 1);
    if (clen < 0) return 0;

    if (xnet_net_send_pkt(st->sock, PKT_VIDEO, st->my_slot,
                          s_tx_cipher, (uint16_t)clen) != 0) {
        xnet_logf("video: tx send_pkt failed");
        return 0;
    }

    /* throttled tx diagnostic */
    static int s_tx = 0;
    if ((s_tx++ % 30) == 0)
        xnet_vlogf("video: tx frames=%d last_len=%d clen=%d slot=%d",
                  s_tx, len, clen, st->my_slot);

    return 1;
}
