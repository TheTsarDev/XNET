/**
 * xnet_net.h
 * TCP networking for XNET — lwIP wrapper
 */

#ifndef XNET_NET_H
#define XNET_NET_H

#include <stdint.h>

/** Init lwIP + DHCP. Blocks until IP acquired (or timeout).
 *  Returns 0 on success, -1 on failure (caller shows error, no reboot). */
int xnet_net_init(void);

/**
 * Connect to relay host:port.
 * Returns socket fd on success, -1 on failure.
 * Socket is set non-blocking after connect.
 */
int xnet_net_connect(const char* host, int port);

/**
 * Send a framed packet.
 *   type     — PKT_* constant
 *   slot     — sender slot (0-3)
 *   payload  — may be NULL for empty packets
 *   pay_len  — payload length
 * Returns 0 on success, -1 on error.
 */
int xnet_net_send_pkt(int sock, uint8_t type, uint8_t slot,
                      const uint8_t* payload, uint16_t pay_len);

/**
 * Non-blocking receive of one framed packet.
 * Fills type_out, slot_out, payload_out (up to pay_max bytes), len_out.
 * Returns 1 if packet received, 0 if no data, -1 on error/disconnect.
 */
int xnet_net_recv_pkt(int sock,
                      int* type_out, int* slot_out,
                      uint8_t* payload_out, int pay_max,
                      int* len_out);

#endif /* XNET_NET_H */
