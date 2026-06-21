/**
 * xnet_xblc.c — Xbox Live Communicator (XBLC) host driver for NXDK's usbh stack
 *
 * Spec source: Ryzee119/hawk firmware (descriptor-faithful XBLC recreation):
 *   - vendor interface class 0x78, two interfaces:
 *       if0: iso OUT ep 0x02  (speaker, Xbox -> headset)
 *       if1: iso IN  ep 0x81  (mic, headset -> Xbox)
 *   - raw 16-bit mono PCM, (rate*2/1000) bytes per 1ms USB frame
 *   - powers up at 16 kHz (32 bytes/frame); we run at that native rate
 *     and resample to the 8 kHz codec in xnet_audio.c — no vendor
 *     control requests needed in v1, fewer unknowns on first contact.
 *
 * Concurrency model:
 *   - probe/disconnect run in task context (usbh_pooling_hubs via SDL poll)
 *   - iso completion callbacks run in USB INTERRUPT context:
 *       * no logging, no malloc, no blocking there — ring buffer ops only
 *   - rings are single-producer/single-consumer with power-of-2 sizes
 */

#include "xnet_xblc.h"
#include "xnet_log.h"

#include <string.h>
#include <usbh_lib.h>
#include <usb.h>

#define XBLC_INTERFACE_CLASS 0x78
#define XBLC_BYTES_PER_MS    32   /* 16 kHz * 2 bytes */

/* ── lock-free sample rings (SPSC) ───────────────────────────────────────────── */
#define RING_SIZE 8192            /* samples; power of 2; 512ms @ 16kHz */
#define RING_MASK (RING_SIZE - 1)

typedef struct {
    int16_t           buf[RING_SIZE];
    volatile uint32_t head;       /* producer writes */
    volatile uint32_t tail;       /* consumer reads  */
} sample_ring;

static sample_ring g_mic_ring;    /* producer: iso-in IRQ,  consumer: app   */
static sample_ring g_spk_ring;    /* producer: app,         consumer: iso-out IRQ */

static uint32_t ring_count(const sample_ring* r) {
    return r->head - r->tail;
}

static void ring_write(sample_ring* r, const int16_t* src, uint32_t n) {
    /* on overflow, oldest data is overwritten implicitly by advancing tail */
    if (ring_count(r) + n > RING_SIZE) {
        r->tail = r->head + n - RING_SIZE;
    }
    for (uint32_t i = 0; i < n; i++) {
        r->buf[(r->head + i) & RING_MASK] = src[i];
    }
    r->head += n;
}

static uint32_t ring_read(sample_ring* r, int16_t* dst, uint32_t n) {
    uint32_t avail = ring_count(r);
    if (n > avail) n = avail;
    for (uint32_t i = 0; i < n; i++) {
        dst[i] = r->buf[(r->tail + i) & RING_MASK];
    }
    r->tail += n;
    return n;
}

/* ── device state ────────────────────────────────────────────────────────────── */
typedef struct {
    IFACE_T*  iface;
    EP_INFO_T* ep;
    UTR_T*    utr[2];             /* NUM_UTR=2 in this stack */
    uint8_t*  buff;
    int       streaming;
} xblc_stream;

static xblc_stream g_mic = {0};   /* iso IN  */
static xblc_stream g_spk = {0};   /* iso OUT */

/* IRQ-context diagnostic counters (read+logged from task context) */
static volatile uint32_t g_mic_irqs, g_mic_bytes, g_mic_errs;
static volatile uint32_t g_spk_irqs, g_spk_bytes, g_spk_errs, g_spk_underruns;

/* ── iso IN (mic): copy arrived PCM into the mic ring, resubmit ─────────────── */
static void iso_in_irq(UTR_T* utr) {
    g_mic_irqs++;
    if (!g_mic.streaming) return;

    utr->bIsoNewSched = 0;
    for (int i = 0; i < IF_PER_UTR; i++) {
        if (utr->iso_status[i] == 0) {
            if (utr->iso_xlen[i] > 0) {
                ring_write(&g_mic_ring,
                           (const int16_t*)utr->iso_buff[i],
                           utr->iso_xlen[i] / 2);
                g_mic_bytes += utr->iso_xlen[i];
            }
        } else {
            g_mic_errs++;
            if (utr->iso_status[i] == USBH_ERR_NOT_ACCESS0 ||
                utr->iso_status[i] == USBH_ERR_NOT_ACCESS1) {
                utr->bIsoNewSched = 1;
            }
        }
        utr->iso_xlen[i] = utr->ep->wMaxPacketSize;
    }
    usbh_iso_xfer(utr);
}

/* ── iso OUT (speaker): fill frames from spk ring (zeros on underrun) ───────── */
static void iso_out_irq(UTR_T* utr) {
    g_spk_irqs++;
    if (!g_spk.streaming) return;

    utr->bIsoNewSched = 0;
    for (int i = 0; i < IF_PER_UTR; i++) {
        if (utr->iso_status[i] != 0) {
            g_spk_errs++;
            if (utr->iso_status[i] == USBH_ERR_NOT_ACCESS0 ||
                utr->iso_status[i] == USBH_ERR_NOT_ACCESS1) {
                utr->bIsoNewSched = 1;
            }
        }
        int16_t* dst = (int16_t*)utr->iso_buff[i];
        uint32_t got = ring_read(&g_spk_ring, dst, XBLC_BYTES_PER_MS / 2);
        if (got < XBLC_BYTES_PER_MS / 2) {
            memset(dst + got, 0, XBLC_BYTES_PER_MS - got * 2);
            g_spk_underruns++;
        }
        g_spk_bytes += XBLC_BYTES_PER_MS;
        utr->iso_xlen[i] = XBLC_BYTES_PER_MS;
    }
    usbh_iso_xfer(utr);
}

/* ── stream start (task context) ─────────────────────────────────────────────── */
static int stream_start(xblc_stream* st, int is_in) {
    EP_INFO_T* ep = st->ep;
    uint8_t* buff = (uint8_t*)usbh_alloc_mem(ep->wMaxPacketSize * IF_PER_UTR * 2);
    if (!buff) return -1;
    st->buff = buff;

    for (int i = 0; i < 2; i++) {
        UTR_T* utr = alloc_utr(st->iface->udev);
        if (!utr) return -1;
        st->utr[i] = utr;
        utr->ep       = ep;
        utr->context  = st;
        utr->func     = is_in ? iso_in_irq : iso_out_irq;
        utr->buff     = buff + ep->wMaxPacketSize * IF_PER_UTR * i;
        utr->data_len = ep->wMaxPacketSize * IF_PER_UTR;
        for (int j = 0; j < IF_PER_UTR; j++) {
            utr->iso_buff[j] = utr->buff + ep->wMaxPacketSize * j;
            utr->iso_xlen[j] = is_in ? ep->wMaxPacketSize : XBLC_BYTES_PER_MS;
            if (!is_in) memset(utr->iso_buff[j], 0, ep->wMaxPacketSize);
        }
    }

    st->streaming = 1;
    st->utr[0]->bIsoNewSched = 1;
    for (int i = 0; i < 2; i++) {
        int ret = usbh_iso_xfer(st->utr[i]);
        if (ret < 0) {
            xnet_logf("xblc: usbh_iso_xfer #%d failed (%d) for %s",
                      i, ret, is_in ? "mic" : "spk");
            st->streaming = 0;
            return -1;
        }
    }
    xnet_logf("xblc: %s streaming started (ep=0x%02X maxpkt=%d)",
              is_in ? "mic" : "spk", ep->bEndpointAddress, ep->wMaxPacketSize);
    return 0;
}

/* ── probe: called once per interface during enumeration ────────────────────── */
static int xblc_probe(IFACE_T* iface) {
    DESC_IF_T* ifd = iface->aif->ifd;

    /* DIAGNOSTIC: log every interface offered, matched or not */
    xnet_logf("xblc: probe OFFERED vid=%04X pid=%04X if=%d class=0x%02X sub=0x%02X proto=0x%02X neps=%d",
              iface->udev->descriptor.idVendor,
              iface->udev->descriptor.idProduct,
              iface->if_num, ifd->bInterfaceClass, ifd->bInterfaceSubClass,
              ifd->bInterfaceProtocol, ifd->bNumEndpoints);

    if (ifd->bInterfaceClass != XBLC_INTERFACE_CLASS) {
        return USBH_ERR_NOT_MATCHED;
    }
    if (ifd->bNumEndpoints < 1) {
        return USBH_ERR_NOT_MATCHED;
    }

    EP_INFO_T* ep = &iface->aif->ep[0];
    int is_in = (ep->bEndpointAddress & 0x80) != 0;

    xnet_logf("xblc: probe vid=%04X pid=%04X if=%d class=0x%02X ep=0x%02X (%s) attr=0x%02X maxpkt=%d",
              iface->udev->descriptor.idVendor,
              iface->udev->descriptor.idProduct,
              iface->if_num, ifd->bInterfaceClass,
              ep->bEndpointAddress, is_in ? "mic-in" : "spk-out",
              ep->bmAttributes, ep->wMaxPacketSize);

    xblc_stream* st = is_in ? &g_mic : &g_spk;
    if (st->streaming) {
        xnet_logf("xblc: duplicate %s interface ignored", is_in ? "mic" : "spk");
        return USBH_ERR_NOT_MATCHED;
    }

    st->iface = iface;
    st->ep    = ep;
    iface->context = st;

    if (stream_start(st, is_in) != 0) {
        xnet_logf("xblc: stream start FAILED for %s", is_in ? "mic" : "spk");
        return USBH_ERR_NOT_MATCHED;
    }
    return USBH_OK;
}

static void xblc_disconnect(IFACE_T* iface) {
    xblc_stream* st = (xblc_stream*)iface->context;
    if (st != &g_mic && st != &g_spk) return;

    st->streaming = 0;
    for (int i = 0; i < 2; i++) {
        if (st->utr[i]) {
            usbh_quit_utr(st->utr[i]);
            free_utr(st->utr[i]);
            st->utr[i] = NULL;
        }
    }
    if (st->buff) {
        usbh_free_mem(st->buff, st->ep->wMaxPacketSize * IF_PER_UTR * 2);
        st->buff = NULL;
    }
    st->iface = NULL;
    st->ep    = NULL;
    xnet_logf("xblc: %s disconnected", st == &g_mic ? "mic" : "spk");
}

static UDEV_DRV_T xblc_driver = {
    xblc_probe,
    xblc_disconnect,
    NULL,   /* suspend */
    NULL,   /* resume  */
};

/* ── device-level diagnostics: every USB connect/disconnect ─────────────────── */
static void diag_dev_conn(struct udev_t* udev, int param) {
    (void)param;
    xnet_logf("usb: DEVICE CONNECT vid=%04X pid=%04X class=0x%02X speed=%s",
              udev->descriptor.idVendor, udev->descriptor.idProduct,
              udev->descriptor.bDeviceClass,
              udev->speed == SPEED_LOW ? "low" : "full");
}

static void diag_dev_disconn(struct udev_t* udev, int param) {
    (void)param;
    xnet_logf("usb: DEVICE DISCONNECT vid=%04X pid=%04X",
              udev->descriptor.idVendor, udev->descriptor.idProduct);
}

/* ── public API ──────────────────────────────────────────────────────────────── */
void xnet_xblc_register(void) {
    /*
     * CRITICAL ORDER FIX: usbh_core_init() memsets the driver table and
     * callback pointers, so registering before it = getting wiped. But it
     * also has a run-once guard — so WE call it first (real init happens
     * here), register into the fresh table, and SDL's later call inside
     * SDL_Init becomes a harmless no-op. SDL then registers the XID
     * driver into the same surviving table. Devices only enumerate when
     * usbh_pooling_hubs() runs (SDL's update), so both drivers are in
     * place before any device is offered around.
     */
    usbh_core_init();
    usbh_register_driver(&xblc_driver);
    usbh_install_conn_callback(diag_dev_conn, diag_dev_disconn);
    xnet_logf("xblc: usb core up, driver registered (class 0x78) + diag callbacks");
}

int xnet_xblc_connected(void) {
    return g_mic.streaming && g_spk.streaming;
}

uint32_t xnet_xblc_mic_available(void) {
    return ring_count(&g_mic_ring);
}

uint32_t xnet_xblc_read_mic(int16_t* dst, uint32_t n) {
    return ring_read(&g_mic_ring, dst, n);
}

void xnet_xblc_write_spk(const int16_t* src, uint32_t n) {
    ring_write(&g_spk_ring, src, n);
}

uint32_t xnet_xblc_spk_queued(void) {
    return ring_count(&g_spk_ring);
}

void xnet_xblc_log_stats(void) {
    xnet_logf("xblc stats: mic irq=%lu bytes=%lu err=%lu | spk irq=%lu bytes=%lu err=%lu underrun=%lu",
              (unsigned long)g_mic_irqs, (unsigned long)g_mic_bytes,
              (unsigned long)g_mic_errs, (unsigned long)g_spk_irqs,
              (unsigned long)g_spk_bytes, (unsigned long)g_spk_errs,
              (unsigned long)g_spk_underruns);
}
