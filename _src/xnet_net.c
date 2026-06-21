/**
 * xnet_net.c
 * TCP networking for XNET — lwIP wrapper
 *
 * Handles:
 *   - DHCP init + wait
 *   - Non-blocking TCP connect to relay
 *   - Binary packet framing (send + recv with reassembly buffer)
 *
 * Frame format:
 *   [1] type  [1] slot  [2] payload_len (big-endian)  [N] payload
 */

#include "xnet_net.h"
#include "xnet_log.h"

#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <lwip/tcpip.h>
#include <lwip/dhcp.h>
#include <lwip/netif.h>
#include <lwip/ip_addr.h>
#include <nxdk/net.h>
#include <hal/xbox.h>

#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <windows.h>

/* ── CONSTANTS ───────────────────────────────────────────────────────────────── */
#define CONNECT_TIMEOUT_MS  5000  /* 5 seconds to connect to relay */
#define RECV_BUF_SIZE       8196

/* ── RECV REASSEMBLY BUFFER (one per connection) ────────────────────────────── */
typedef struct {
    uint8_t  buf[RECV_BUF_SIZE];
    int      fill;   /* bytes currently in buffer */
} RecvBuf;

static RecvBuf g_recv; /* single connection — one reassembly buffer */

/* ═══════════════════════════════════════════════════════════════════════════════
 * INIT — DHCP
 * ═══════════════════════════════════════════════════════════════════════════════ */
int xnet_net_init(void) {
    /* nxNetInit blocks until DHCP lease is acquired or times out */
    int ret = nxNetInit(NULL);

    memset(&g_recv, 0, sizeof(g_recv));

    return (ret != 0) ? -1 : 0;
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * CONNECT
 * ═══════════════════════════════════════════════════════════════════════════════ */
int xnet_net_connect(const char* host, int port) {
    struct addrinfo hints, *res = NULL;
    char port_str[8];
    int  sock, ret;
    int  val;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    sprintf(port_str, "%d", port);

    xnet_logf("connect: resolving %s:%d", host, port);
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || res == NULL) {
        xnet_logf("connect: getaddrinfo FAILED");
        return -1;
    }

    sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        freeaddrinfo(res);
        return -1;
    }

    /* set non-blocking for connect with timeout */
    val = 1;
    ioctlsocket(sock, FIONBIO, (void*)&val);

    ret = connect(sock, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

    if (ret < 0) {
        /* EWOULDBLOCK / EINPROGRESS expected on non-blocking connect */
        int err = errno;
        if (err != EWOULDBLOCK && err != EINPROGRESS) {
            closesocket(sock);
            return -1;
        }

        /* wait for connect with select() timeout */
        fd_set wset;
        struct timeval tv;
        FD_ZERO(&wset);
        FD_SET(sock, &wset);
        tv.tv_sec  = CONNECT_TIMEOUT_MS / 1000;
        tv.tv_usec = (CONNECT_TIMEOUT_MS % 1000) * 1000;

        ret = select(sock + 1, NULL, &wset, NULL, &tv);
        if (ret <= 0) {
            /* timeout or error */
            closesocket(sock);
            return -1;
        }

        /* verify connect succeeded */
        int       so_err = 0;
        socklen_t so_len = sizeof(so_err);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&so_err, &so_len);
        if (so_err != 0) {
            closesocket(sock);
            return -1;
        }
    }

    /* reset reassembly buffer for new connection */
    memset(&g_recv, 0, sizeof(g_recv));

    xnet_logf("connect: established, sock=%d", sock);
    return sock;
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * SEND PACKET
 * ═══════════════════════════════════════════════════════════════════════════════ */
int xnet_net_send_pkt(int sock, uint8_t type, uint8_t slot,
                      const uint8_t* payload, uint16_t pay_len) {
    uint8_t  header[4];
    uint8_t  frame[4 + 8192];
    int      frame_len;

    if (sock < 0) return -1;

    header[0] = type;
    header[1] = slot;
    header[2] = (uint8_t)(pay_len >> 8);
    header[3] = (uint8_t)(pay_len & 0xFF);

    memcpy(frame, header, 4);
    frame_len = 4;

    if (payload && pay_len > 0) {
        if (pay_len > 8192) return -1;
        memcpy(frame + 4, payload, pay_len);
        frame_len += pay_len;
    }

    /* Send the whole frame. The socket is non-blocking, so a sustained push
     * (file transfer) can fill the TX buffer and make send() return
     * EWOULDBLOCK mid-frame. Returning here would leave a partial frame on
     * the wire and desync the peer's parser, so instead we select() for
     * writability and retry until the frame is fully flushed (bounded). */
    int sent = 0;
    int stalls = 0;
    while (sent < frame_len) {
        int n = send(sock, (char*)frame + sent, frame_len - sent, 0);
        if (n > 0) { sent += n; stalls = 0; continue; }

        int err = errno;
        if (n < 0 && (err == EWOULDBLOCK || err == EAGAIN)) {
            fd_set wset;
            struct timeval tv;
            FD_ZERO(&wset);
            FD_SET(sock, &wset);
            tv.tv_sec  = 5;   /* per-stall cap */
            tv.tv_usec = 0;
            int s = select(sock + 1, NULL, &wset, NULL, &tv);
            if (s <= 0) {
                if (++stalls >= 3) return -1; /* ~15s with no progress = dead */
                continue;
            }
            continue; /* writable now — retry send */
        }
        return -1; /* real error / disconnect */
    }

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * RECV PACKET (non-blocking, with reassembly)
 * ═══════════════════════════════════════════════════════════════════════════════ */
int xnet_net_recv_pkt(int sock,
                      int* type_out, int* slot_out,
                      uint8_t* payload_out, int pay_max,
                      int* len_out) {
    if (sock < 0) return -1;

    /* drain available bytes into reassembly buffer */
    {
        int space = RECV_BUF_SIZE - g_recv.fill;
        if (space > 0) {
            int n = recv(sock,
                         (char*)g_recv.buf + g_recv.fill,
                         space, 0);
            if (n < 0) {
                int err = errno;
                if (err == EWOULDBLOCK || err == EAGAIN) {
                    /* no data available right now — not an error */
                } else {
                    return -1; /* real error / disconnect */
                }
            } else if (n == 0) {
                return -1; /* graceful disconnect */
            } else {
                g_recv.fill += n;
            }
        }
    }

    /* need at least 4-byte header */
    if (g_recv.fill < 4) return 0;

    uint8_t  pkt_type   = g_recv.buf[0];
    uint8_t  pkt_slot   = g_recv.buf[1];
    uint16_t pkt_paylen = ((uint16_t)g_recv.buf[2] << 8) | g_recv.buf[3];

    /* safety cap */
    if (pkt_paylen > 8192) {
        /* corrupt frame — reset buffer */
        memset(&g_recv, 0, sizeof(g_recv));
        return -1;
    }

    /* wait for full frame */
    int total = 4 + (int)pkt_paylen;
    if (g_recv.fill < total) return 0;

    /* copy payload out */
    if (pkt_paylen > 0) {
        int copy = (pkt_paylen <= (uint16_t)pay_max) ? pkt_paylen : pay_max;
        memcpy(payload_out, g_recv.buf + 4, copy);
        *len_out = copy;
    } else {
        *len_out = 0;
    }

    *type_out = pkt_type;
    *slot_out = pkt_slot;

    /* consume frame from buffer */
    int remaining = g_recv.fill - total;
    if (remaining > 0) {
        memmove(g_recv.buf, g_recv.buf + total, remaining);
    }
    g_recv.fill = remaining;

    return 1; /* packet ready */
}
