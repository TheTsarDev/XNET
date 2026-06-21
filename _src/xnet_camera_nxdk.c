/*
 * xnet_camera_nxdk.c
 *
 * Real camera capture backend for XNET Secure Video.
 *
 * OV519 bridge + OV7620 sensor (silver US EyeToy, 054C:0155) and the Japanese
 * Xbox Video Camera (045E:028C). Produces 320x240 baseline-JPEG frames.
 *
 * Two sources fused here:
 *   - OV519 register init + MJPEG frame framing: transcribed verbatim from
 *     Darkone83's Xbox-live-camera-research-project (RXDK). Those bytes are
 *     hardware-proven; do not "tidy" them.
 *   - USB transport: libusbohci, modeled on xnet_xblc.c (our XBLC mic driver,
 *     which already runs isochronous IN on this stack). Probe/disconnect run in
 *     task context; the iso completion callback runs in USB INTERRUPT context
 *     (no Sleep, no logging churn there).
 *
 * Frame framing (OV519): image data spans ~25 iso packets delimited by 16-byte
 * headers. A packet starting FF FF FF 50 is start-of-frame (strip 16 bytes,
 * begin accumulating); FF FF FF 51 is end-of-frame (publish what we have as one
 * JPEG); anything else mid-frame is appended. The REAL per-packet length is
 * iso_xlen[i] — copying the full maxpkt drags stale bytes in and buries the EOI.
 *
 * Bring-up reality: USB enumeration rarely works first try. Every stage logs to
 * xnet.log so a failed boot tells us exactly where it stopped. The spots most
 * likely to need a hardware-confirmed tweak are flagged [HW].
 */

#include "xnet_camera.h"
#include "xnet_log.h"

#include <string.h>
#include <windows.h>          /* Sleep */
#include <usbh_lib.h>
#include <usb.h>

/* ── camera identity ─────────────────────────────────────────────────────────── */
#define CAM_VID_EYETOY   0x054C
#define CAM_PID_EYETOY   0x0155
#define CAM_VID_XBOX     0x045E
#define CAM_PID_XBOX     0x028C

/* [HW] validated stream path from the research: iso IN ep 0x81, alt 3,
   maxpkt 768. We try alt 3 first, then fall back to scanning. */
#define CAM_WANT_ALT     3
#define CAM_ISO_PKT_MAX  768

/* frame accumulator capacity: 320*240*2 + slack for headers/overrun */
/* OV519 frames are baseline JPEG (~3.5KB typical, well under 16KB even for a
   busy scene). Sizing this for raw YUV (320x240x2) wasted ~160KB per buffer and
   pushed 64MB consoles over the edge. 32KB is a generous JPEG ceiling. */
#define CAM_FRAME_CAP    32768

static int cam_is_id(int vid, int pid) {
    if (vid == CAM_VID_EYETOY && pid == CAM_PID_EYETOY) return 1;
    if (vid == CAM_VID_XBOX   && pid == CAM_PID_XBOX)   return 1;
    return 0;
}

/* ── device state ────────────────────────────────────────────────────────────── */
/* number of iso UTRs kept in flight. More = more tolerance for the main thread
   stalling (decode/log) before the iso ring starves and drops packets. */
#define CAM_NUM_UTR  3

typedef struct {
    IFACE_T*  iface;
    UDEV_T*   udev;
    EP_INFO_T* ep;
    UTR_T*    utr[CAM_NUM_UTR];
    uint8_t*  iso_buf;       /* pkt_size * IF_PER_UTR * CAM_NUM_UTR */
    int       pkt_size;
    int       streaming;
} cam_dev;

static cam_dev g_cam = {0};

/* control scratch must be DMA-safe; alloc once from the USB heap */
static uint8_t* g_ctrl = 0;

/* frame assembly (written from IRQ context, read from app via get_frame) */
static uint8_t        s_frame[CAM_FRAME_CAP];        /* in-progress accumulator */
static uint8_t        s_ready[2][CAM_FRAME_CAP];      /* completed JPEG double-buffer */
static volatile int   s_ready_len[2] = {0, 0};
static volatile int   s_pub = -1;                     /* published buffer idx (-1=none) */
static volatile int   s_completed = 0;                /* frames accepted */
static volatile int   s_dropped = 0;                  /* frames rejected (torn/short) */
static int            s_frame_w = 0;
static int            s_in_frame = 0;
static int            s_delivered = -1;                /* last get_frame() handed out */

/* IRQ-context diagnostics */
static volatile uint32_t g_iso_irqs, g_iso_bytes, g_sof, g_eof;

/* set when a video session is active; gates the per-packet assembly work so the
   iso ring can keep running cheaply when no one is watching */
static volatile int g_want_frames = 0;

/* which sensor init path ran (1 = OV7648/76xx). Needed to re-apply the right
   window on a re-kick. */
static int g_sensor76 = 0;

/* per-session auto re-kick tracking (set in xnet_camera_init) */
static unsigned long s_sess_start = 0;
static int           s_sess_completed0 = 0;
static int           s_sess_kicked = 0;

/* diagnostic snapshot for the on-console DEBUG CAMERA screen */
void xnet_camera_debug_stats(uint32_t* irqs, uint32_t* bytes,
                             uint32_t* sof, uint32_t* eof, int* completed) {
    if (irqs)      *irqs      = g_iso_irqs;
    if (bytes)     *bytes     = g_iso_bytes;
    if (sof)       *sof       = g_sof;
    if (eof)       *eof       = g_eof;
    if (completed) *completed = s_completed;
}

/* ── OV519 control-register helpers (task context) ───────────────────────────────
 * bridge write: vendor OUT (0x41), bRequest 1, wValue 0, wIndex=reg, 1 data byte
 * bridge read : vendor IN  (0xC1), bRequest 1, wValue 0, wIndex=reg, 1 data byte
 */
static int cam_ovw(int reg, int val) {
    uint32_t xlen = 0;
    g_ctrl[0] = (uint8_t)val;
    int rc = usbh_ctrl_xfer(g_cam.udev, 0x41, 0x01, 0x0000, (uint16_t)reg,
                            1, g_ctrl, &xlen, 100);
    return rc;
}
static int cam_ovr(int reg, uint8_t* out) {
    uint32_t xlen = 0;
    g_ctrl[0] = 0;
    int rc = usbh_ctrl_xfer(g_cam.udev, 0xC1, 0x01, 0x0000, (uint16_t)reg,
                            1, g_ctrl, &xlen, 100);
    *out = g_ctrl[0];
    return rc;
}
static void cam_ovwmask(int reg, int val, int mask) {
    if (mask == 0xFF) { cam_ovw(reg, val); return; }
    uint8_t old = 0;
    if (cam_ovr(reg, &old) == 0) cam_ovw(reg, (old & ~mask) | (val & mask));
    else                         cam_ovw(reg, val);
}
/* OV7620 sensor write via the OV519 I2C master */
static int cam_i2cw(int reg, int val) {
    int s1 = cam_ovw(0x42, reg);    /* I2C sub-address = sensor register */
    int s2 = cam_ovw(0x45, val);    /* I2C data */
    int s3 = cam_ovw(0x47, 0x01);   /* I2C control = initiate write */
    Sleep(1);
    return s1 | s2 | s3;
}
static int cam_i2cr(int reg, uint8_t* out) {
    uint8_t ctl = 0; int t, rc;
    *out = 0;
    cam_ovw(0x43, reg);             /* read sub-address */
    cam_ovw(0x47, 0x03);            /* control = 3 */
    Sleep(1);
    for (t = 0; t < 4; t++) { cam_ovr(0x47, &ctl); if (ctl & 0x01) break; }
    cam_ovw(0x47, 0x05);            /* control = 5 */
    Sleep(1);
    for (t = 0; t < 4; t++) { cam_ovr(0x47, &ctl); if (ctl & 0x01) break; }
    rc = cam_ovr(0x45, out);        /* data */
    cam_ovw(0x42, 0xff);            /* commit/cleanup */
    cam_ovw(0x45, 0x00);
    cam_ovw(0x40, 0x01);
    return rc;
}

/* OV7620/7648 sensor masked write (read-modify-write via I2C when mask != 0xff) */
static void cam_i2cwmask(int reg, int val, int mask) {
    if (mask == 0xFF) { cam_i2cw(reg, val); return; }
    uint8_t old = 0;
    cam_i2cr(reg, &old);
    cam_i2cw(reg, (old & ~mask) | (val & mask));
}

/* OV7620 full register table (norm_7620) — verbatim from the research. */
static const unsigned char k_norm7620[] = {
    0x12,0x80, 0x00,0x00, 0x01,0x80, 0x02,0x80, 0x03,0xc0, 0x06,0x60,
    0x07,0x00, 0x0c,0x24, 0x0c,0x24, 0x0d,0x24, 0x11,0x01, 0x12,0x24,
    0x13,0x01, 0x14,0x84, 0x15,0x01, 0x16,0x03, 0x17,0x2f, 0x18,0xcf,
    0x19,0x06, 0x1a,0xf5, 0x1b,0x00, 0x20,0x18, 0x21,0x80, 0x22,0x80,
    0x23,0x00, 0x26,0xa2, 0x27,0xea, 0x28,0x22, 0x29,0x00, 0x2a,0x10,
    0x2b,0x00, 0x2c,0x88, 0x2d,0x91, 0x2e,0x80, 0x2f,0x44, 0x60,0x27,
    0x61,0x02, 0x62,0x5f, 0x63,0xd5, 0x64,0x57, 0x65,0x83, 0x66,0x55,
    0x67,0x92, 0x68,0xcf, 0x69,0x76, 0x6a,0x22, 0x6b,0x00, 0x6c,0x02,
    0x6d,0x44, 0x6e,0x80, 0x6f,0x1d, 0x70,0x8b, 0x71,0x00, 0x72,0x14,
    0x73,0x54, 0x74,0x00, 0x75,0x8e, 0x76,0x00, 0x77,0xff, 0x78,0x80,
    0x79,0x80, 0x7a,0x80, 0x7b,0xe2, 0x7c,0x00
};

/* OV519 bridge + OV7620 sensor bring-up — transcribed from the research. The
   silver EyeToy is OV7620 (sensor PID 0x0a != 0x76), so we take the norm_7620
   path; the 76xx branch is omitted here since this hardware never hits it. */
static void cam_init_sensor(void) {
    int i;
    uint8_t rb = 0xAB;

    /* 0) generic bridge bring-up. reg 0x72=0xEE is CRITICAL: GPIO bit4 must be
          cleared or the sensor is invisible (hardware default 0xFF). */
    cam_ovw(0x5a, 0x6d);
    cam_ovw(0x53, 0x9b);
    cam_ovw(0x54, 0xff);
    cam_ovw(0x5d, 0x03);
    cam_ovw(0x49, 0x01);
    cam_ovw(0x48, 0x00);
    cam_ovw(0x72, 0xee);   /* GPIO dir bit4 clear — required for sensor detect */
    cam_ovw(0x51, 0x0f);
    cam_ovw(0x51, 0x00);
    cam_ovw(0x22, 0x00);
    Sleep(10);

    /* sensor slave ids + SCCB reset, then read manufacturer id (0x1c=7F,0x1d=A2) */
    cam_ovw(0x41, 0x42);   /* W_SID */
    cam_ovw(0x44, 0x43);   /* R_SID */
    {
        int tries, synced = 0;
        uint8_t idh, idl, dummy;
        cam_i2cw(0x12, 0x80);  /* COM7 reset */
        Sleep(150);
        for (tries = 0; tries < 5; tries++) {
            idh = 0xCC; idl = 0xCC;
            cam_i2cr(0x1c, &idh);
            cam_i2cr(0x1d, &idl);
            xnet_logf("camera: detect try %d MIDH=0x%02X MIDL=0x%02X (want 7F A2)",
                      tries, idh, idl);
            if (idh == 0x7f && idl == 0xa2) { synced = 1; break; }
            cam_i2cw(0x12, 0x80);
            Sleep(150);
            dummy = 0; cam_i2cr(0x00, &dummy);
        }
        xnet_logf("camera: sensor synced=%d", synced);
    }

    /* identify sensor: reg 0x0a high byte. 0x76 => 76xx (OV7648-class, needs the
       7648 init + window or the bridge streams ALL-ZERO data); else OV7620. */
    int sensor76 = 0;
    {
        uint8_t pidh = 0, pidl = 0;
        cam_i2cr(0x0a, &pidh);
        cam_i2cr(0x0b, &pidl);
        sensor76 = (pidh == 0x76) ? 1 : 0;
        g_sensor76 = sensor76;
        xnet_logf("camera: sensor PID 0x0a=0x%02X VER 0x0b=0x%02X -> %s",
                  pidh, pidl, sensor76 ? "76xx/7648" : "OV7620");
    }

    /* bridge mode-init: YUV path, I2C timing, audio clock, FRAR */
    cam_ovw(0x5d, 0x03); cam_ovw(0x53, 0x9f); cam_ovw(0x54, 0x0f);
    cam_ovw(0xa2, 0x20); cam_ovw(0xa3, 0x18); cam_ovw(0xa4, 0x04);
    cam_ovw(0xa5, 0x28); cam_ovw(0x37, 0x00); cam_ovw(0x55, 0x02);
    cam_ovw(0x22, 0x1d); cam_ovw(0x17, 0x50); cam_ovw(0x40, 0xff);
    cam_ovw(0x46, 0x00); cam_ovw(0x59, 0x04); cam_ovw(0xff, 0x00);

    if (sensor76) {
        /* norm_7640/7648: reset, 0x12=0x14, bridge selects 8-bit input */
        cam_i2cw(0x12, 0x80); Sleep(150);
        cam_i2cw(0x12, 0x14);
        cam_ovwmask(0x20, 0x10, 0x10);   /* OV519_R20_DFR: 8-bit input mode */

        /* mode_init_ov_sensor_regs (OV7648, QVGA): mode + AWB */
        cam_i2cwmask(0x14, 0x20, 0x20);  /* qvga */
        cam_i2cwmask(0x28, 0x00, 0x20);
        cam_i2cwmask(0x2d, 0x40, 0x40);  /* anti-shake */
        cam_i2cwmask(0x67, 0xf0, 0xf0);
        cam_i2cwmask(0x74, 0x20, 0x20);  /* higher auto gain */
        cam_i2cwmask(0x12, 0x04, 0x04);  /* AWB on */
        cam_i2cw(0x11, 0x00);            /* clockdiv 0 (30fps) */

        /* set_ov_sensor_window (OV7648, QVGA): WITHOUT this the OV51x delivers
           all-zero isoc data — exactly the bytes=0 we logged. */
        cam_i2cw(0x17, 0x1a);                  /* HSTART */
        cam_i2cw(0x18, 0x1a + (320 >> 1));     /* HSTOP = 0xba */
        cam_i2cw(0x19, 0x03);                  /* VSTART */
        cam_i2cw(0x1a, 0x03 + (240 >> 0));     /* VSTOP = 0xf3 */
    } else {
        /* full OV7620 sensor table */
        for (i = 0; i + 1 < (int)sizeof(k_norm7620); i += 2) {
            cam_i2cw(k_norm7620[i], k_norm7620[i + 1]);
            if (k_norm7620[i] == 0x12 && k_norm7620[i + 1] == 0x80) Sleep(150);
        }
    }

    /* bridge frame geometry: 320x240, YUV422 (R25=0x03) */
    cam_ovw(0x10, 320 >> 4);   /* H_SIZE = 0x14 */
    cam_ovw(0x11, 240 >> 3);   /* V_SIZE = 0x1e */
    cam_ovw(0x12, 0x00); cam_ovw(0x13, 0x00); cam_ovw(0x14, 0x00);
    cam_ovw(0x15, 0x00); cam_ovw(0x16, 0x00);
    cam_ovw(0x25, 0x03);       /* FORMAT = YUV422 */
    cam_ovw(0x26, 0x00);

    /* frame rate (30fps default) */
    cam_ovw(0xa4, 0x0c); cam_ovw(0x23, 0xff);

    /* ov51x_restart: unblock the stream FIFO, then LED on */
    cam_ovw(0x51, 0x0f); cam_ovw(0x51, 0x00); cam_ovw(0x22, 0x1d);
    cam_ovwmask(0x71, 0x01, 0x01);   /* LED on */

    cam_i2cr(0x1c, &rb); xnet_logf("camera: post MIDH=0x%02X (exp 7F)", rb);
    cam_i2cr(0x1d, &rb); xnet_logf("camera: post MIDL=0x%02X (exp A2)", rb);
    xnet_logf("camera: sensor init applied (%s + geometry + restart)",
              sensor76 ? "norm_7648+window" : "norm_7620");
}

/* ── iso IN completion (IRQ CONTEXT) — OV519 frame assembly ──────────────────── */
static void cam_iso_irq(UTR_T* utr) {
    g_iso_irqs++;
    if (!g_cam.streaming) return;

    utr->bIsoNewSched = 0;
    for (int i = 0; i < IF_PER_UTR; i++) {
        int avail = (int)utr->iso_xlen[i];        /* REAL bytes this packet */
        const uint8_t* pk = (const uint8_t*)utr->iso_buff[i];

        if (utr->iso_status[i] != 0) {
            if (utr->iso_status[i] == USBH_ERR_NOT_ACCESS0 ||
                utr->iso_status[i] == USBH_ERR_NOT_ACCESS1)
                utr->bIsoNewSched = 1;
        } else if (g_want_frames && avail > 0 && avail <= g_cam.pkt_size) {
            g_iso_bytes += avail;

            if (avail >= 4 && pk[0]==0xff && pk[1]==0xff && pk[2]==0xff &&
                (pk[3]==0x50 || pk[3]==0x51)) {
                if (pk[3] == 0x50) {                 /* start of frame */
                    g_sof++;
                    s_frame_w = 0; s_in_frame = 1;
                    pk += 16; avail -= 16;           /* strip 16-byte header */
                    if (avail > 0 && s_frame_w + avail <= CAM_FRAME_CAP) {
                        memcpy(s_frame + s_frame_w, pk, avail);
                        s_frame_w += avail;
                    }
                } else {                             /* end-of-frame marker */
                    g_eof++;
                    int n = s_frame_w;
                    if (n > CAM_FRAME_CAP) n = CAM_FRAME_CAP;
                    /* Publish every frame of plausible size and let picojpeg
                       decode it — OV519 frames carry trailing padding so they
                       rarely end exactly on FF D9, and a strict end-marker check
                       wrongly rejects almost all of them. Only obviously broken
                       (tiny) frames are dropped. The double-buffer + iso UTRs are
                       what actually keep frames intact. */
                    if (n >= 512) {
                        int b = (s_pub == 0) ? 1 : 0;   /* fill the idle buffer */
                        memcpy(s_ready[b], s_frame, n);
                        s_ready_len[b] = n;
                        s_pub = b;                      /* publish (single int write) */
                        s_completed++;
                    } else {
                        s_dropped++;
                    }
                    s_frame_w = 0; s_in_frame = 0;
                }
            } else if (s_in_frame) {                 /* mid-frame image data */
                if (s_frame_w + avail <= CAM_FRAME_CAP) {
                    memcpy(s_frame + s_frame_w, pk, avail);
                    s_frame_w += avail;
                }
            }
        }
        utr->iso_xlen[i] = g_cam.pkt_size;
    }
    usbh_iso_xfer(utr);
}

/* ── streaming start (task context) ──────────────────────────────────────────── */
static int cam_stream_start(void) {
    EP_INFO_T* ep = g_cam.ep;
    g_cam.pkt_size = ep->wMaxPacketSize;
    if (g_cam.pkt_size <= 0 || g_cam.pkt_size > CAM_ISO_PKT_MAX)
        g_cam.pkt_size = CAM_ISO_PKT_MAX;

    g_cam.iso_buf = (uint8_t*)usbh_alloc_mem(g_cam.pkt_size * IF_PER_UTR * CAM_NUM_UTR);
    if (!g_cam.iso_buf) { xnet_logf("camera: iso buf alloc failed"); return -1; }

    for (int u = 0; u < CAM_NUM_UTR; u++) {
        UTR_T* utr = alloc_utr(g_cam.udev);
        if (!utr) { xnet_logf("camera: alloc_utr failed"); return -1; }
        g_cam.utr[u] = utr;
        utr->ep       = ep;
        utr->context  = &g_cam;
        utr->func     = cam_iso_irq;
        utr->buff     = g_cam.iso_buf + g_cam.pkt_size * IF_PER_UTR * u;
        utr->data_len = g_cam.pkt_size * IF_PER_UTR;
        for (int j = 0; j < IF_PER_UTR; j++) {
            utr->iso_buff[j] = utr->buff + g_cam.pkt_size * j;
            utr->iso_xlen[j] = g_cam.pkt_size;
        }
    }

    g_cam.streaming = 1;
    g_cam.utr[0]->bIsoNewSched = 1;
    for (int u = 0; u < CAM_NUM_UTR; u++) {
        int rc = usbh_iso_xfer(g_cam.utr[u]);
        if (rc < 0) {
            xnet_logf("camera: usbh_iso_xfer #%d failed (%d)", u, rc);
            g_cam.streaming = 0;
            return -1;
        }
    }
    xnet_logf("camera: streaming started (ep=0x%02X maxpkt=%d)",
              ep->bEndpointAddress, g_cam.pkt_size);
    return 0;
}

/* find an iso IN endpoint on the active alternate setting. The EyeToy is also a
   USB audio device, so it has a SMALL iso-IN (mic) endpoint we must not grab —
   the video endpoint has a large maxpkt (research: 768), so require a floor. */
#define CAM_ISO_MIN_MAXPKT  256
static EP_INFO_T* cam_find_iso_in(IFACE_T* iface) {
    DESC_IF_T* ifd = iface->aif->ifd;
    for (int i = 0; i < ifd->bNumEndpoints; i++) {
        EP_INFO_T* ep = &iface->aif->ep[i];
        int is_in  = (ep->bEndpointAddress & 0x80) != 0;
        int is_iso = (ep->bmAttributes & 0x03) == 0x01;
        if (is_in && is_iso && ep->wMaxPacketSize >= CAM_ISO_MIN_MAXPKT) return ep;
    }
    return 0;
}

/* ── probe / disconnect ──────────────────────────────────────────────────────── */
static int cam_probe(IFACE_T* iface) {
    int vid = iface->udev->descriptor.idVendor;
    int pid = iface->udev->descriptor.idProduct;
    DESC_IF_T* ifd = iface->aif->ifd;

    xnet_logf("camera: probe OFFERED vid=%04X pid=%04X if=%d class=0x%02X neps=%d",
              vid, pid, iface->if_num, ifd->bInterfaceClass, ifd->bNumEndpoints);

    if (!cam_is_id(vid, pid)) return USBH_ERR_NOT_MATCHED;
    if (g_cam.streaming)      return USBH_ERR_NOT_MATCHED;  /* already have it */

    g_cam.udev = iface->udev;
    g_cam.iface = iface;
    iface->context = &g_cam;

    /* Order matters (from the working driver): wake the sensor while the
       interface is still on the default alt, THEN switch to the bandwidth alt
       that arms the iso stream. Selecting alt 3 first puts the bridge into
       streaming mode before the sensor is configured -> all-zero data. Control
       transfers (sensor init) go over EP0 and work on any alt. */
    cam_init_sensor();

    int rc_alt = usbh_set_interface(iface, CAM_WANT_ALT);
    xnet_logf("camera: set_interface alt=%d -> %d", CAM_WANT_ALT, rc_alt);

    EP_INFO_T* ep = cam_find_iso_in(iface);
    if (!ep) {
        xnet_logf("camera: no iso-IN endpoint on if=%d (alt %d) — not matched here",
                  iface->if_num, CAM_WANT_ALT);
        return USBH_ERR_NOT_MATCHED;   /* another interface/alt may be the one */
    }
    g_cam.ep = ep;
    xnet_logf("camera: iso-IN ep=0x%02X maxpkt=%d attr=0x%02X",
              ep->bEndpointAddress, ep->wMaxPacketSize, ep->bmAttributes);

    /* reset assembly state and start streaming */
    s_frame_w = 0; s_in_frame = 0;
    s_ready_len[0] = 0; s_ready_len[1] = 0; s_pub = -1;
    s_completed = 0; s_dropped = 0; s_delivered = -1;

    if (cam_stream_start() != 0) {
        xnet_logf("camera: stream start FAILED");
        return USBH_ERR_NOT_MATCHED;
    }
    return USBH_OK;
}

static void cam_disconnect(IFACE_T* iface) {
    if (iface->context != &g_cam) return;
    g_cam.streaming = 0;
    for (int u = 0; u < CAM_NUM_UTR; u++) {
        if (g_cam.utr[u]) { usbh_quit_utr(g_cam.utr[u]); g_cam.utr[u] = 0; }
    }
    if (g_cam.iso_buf) {
        usbh_free_mem(g_cam.iso_buf, g_cam.pkt_size * IF_PER_UTR * CAM_NUM_UTR);
        g_cam.iso_buf = 0;
    }
    g_cam.iface = 0; g_cam.udev = 0; g_cam.ep = 0;
    xnet_logf("camera: disconnected");
}

static UDEV_DRV_T cam_driver = {
    cam_probe,
    cam_disconnect,
    NULL,   /* suspend */
    NULL,   /* resume  */
};

/* ── public HAL ──────────────────────────────────────────────────────────────── */

/* Called once at boot (from main, right after XBLC init). Registers the driver
   into the already-initialised USB core — must NOT call usbh_core_init() again,
   which would wipe the XBLC registration. */
void xnet_camera_register(void) {
    if (!g_ctrl) g_ctrl = (uint8_t*)usbh_alloc_mem(64);  /* DMA-safe ctrl scratch */
    usbh_register_driver(&cam_driver);
    xnet_logf("camera: driver registered (EyeToy 054C:0155 / Xbox 045E:028C)");
}

/* Re-establish streaming on a sensor that drifted to no-output: FIFO restart +
   (7648) window re-apply. Task context only (control transfers). */
static void cam_rekick(void) {
    if (!g_cam.udev) return;
    cam_ovw(0x51, 0x0f);              /* OV51x reset/restart FIFO */
    cam_ovw(0x51, 0x00);
    cam_ovw(0x22, 0x1d);              /* FRAR */
    cam_ovwmask(0x71, 0x01, 0x01);    /* LED on */
    if (g_sensor76) {                 /* re-apply 7648 window */
        cam_i2cw(0x17, 0x1a);
        cam_i2cw(0x18, 0x1a + (320 >> 1));
        cam_i2cw(0x19, 0x03);
        cam_i2cw(0x1a, 0x03 + (240 >> 0));
    }
    xnet_logf("camera: re-kick (s76=%d)", g_sensor76);
}

int xnet_camera_init(void) {
    /* enumeration is event-driven via the probe; if the camera was plugged in
       at boot the probe has already run. Mark that we want frames, and re-kick
       the stream — the OV7648 can drift to a no-output state if it sat running
       but unconsumed for a while after boot. */
    if (!g_ctrl) g_ctrl = (uint8_t*)usbh_alloc_mem(64);
    g_want_frames = 1;
    s_frame_w = 0; s_in_frame = 0;        /* start clean for this session */
    s_sess_start = GetTickCount();        /* baseline for auto re-kick */
    s_sess_completed0 = s_completed;
    s_sess_kicked = 0;
    if (g_cam.streaming) cam_rekick();
    xnet_logf("camera: init (streaming=%d)", g_cam.streaming);
    return 0;
}

int xnet_camera_streaming(void) {
    return g_cam.streaming;
}

int xnet_camera_get_frame(const uint8_t** jpeg, int* len) {
    if (!g_cam.streaming) return 0;

    /* self-heal: if no frames have completed within 1.5s of a session starting,
       re-kick once. Catches the intermittent OV7648 no-output drift without a
       reboot. Baseline is captured per-session in xnet_camera_init(). */
    if (g_want_frames && !s_sess_kicked &&
        (GetTickCount() - s_sess_start) > 1500 &&
        s_completed == s_sess_completed0) {
        xnet_logf("camera: no frames 1.5s into session -> auto re-kick");
        cam_rekick();
        s_sess_kicked = 1;
    }

    /* task-context diagnostics (~ every 5s at 12fps polling): shows whether iso
       packets and SOF/EOF markers are flowing even when no frame completes. */
    static int dbg = 0;
    if ((dbg++ % 64) == 0) {
        xnet_vlogf("camera: iso irqs=%lu bytes=%lu sof=%lu eof=%lu completed=%d",
                  (unsigned long)g_iso_irqs, (unsigned long)g_iso_bytes,
                  (unsigned long)g_sof, (unsigned long)g_eof, s_completed);
    }

    int c = s_completed;            /* sampled once (IRQ may bump it) */
    if (c == s_delivered || s_pub < 0) return 0; /* nothing new */
    s_delivered = c;
    int b = s_pub;                  /* stable: IRQ writes the OTHER buffer next */
    *jpeg = s_ready[b];
    *len  = s_ready_len[b];
    return (*len > 0) ? 1 : 0;
}

void xnet_camera_shutdown(void) {
    /* Stop consuming frames, turn the camera's red LED off, and hold the OV519
       FIFO in reset so the sensor stops producing (saves USB bandwidth/power and
       answers "what is it doing?" — nothing, now). The iso ring stays allocated;
       xnet_camera_init()'s re-kick releases the reset and relights the LED on
       the next session, so re-entry is still instant. */
    g_want_frames = 0;
    if (g_cam.streaming && g_cam.udev) {
        cam_ovwmask(0x71, 0x00, 0x01);   /* LED off */
        cam_ovw(0x51, 0x0f);             /* hold stream FIFO in reset */
        xnet_logf("camera: stopped (LED off, FIFO held)");
    }
}
