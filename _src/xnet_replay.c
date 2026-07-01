/**
 * xnet_replay.c — see xnet_replay.h
 */
#include "xnet_replay.h"
#include "xnet_crypto.h"
#include <string.h>

/* ── byte helpers ──────────────────────────────────────────────────────── */
static void put_u64_be(uint8_t* o, uint64_t v) {
    o[0]=(uint8_t)(v>>56); o[1]=(uint8_t)(v>>48);
    o[2]=(uint8_t)(v>>40); o[3]=(uint8_t)(v>>32);
    o[4]=(uint8_t)(v>>24); o[5]=(uint8_t)(v>>16);
    o[6]=(uint8_t)(v>>8);  o[7]=(uint8_t)(v);
}
static uint64_t get_u64_be(const uint8_t* i) {
    return ((uint64_t)i[0]<<56)|((uint64_t)i[1]<<48)
         | ((uint64_t)i[2]<<40)|((uint64_t)i[3]<<32)
         | ((uint64_t)i[4]<<24)|((uint64_t)i[5]<<16)
         | ((uint64_t)i[6]<<8) | ((uint64_t)i[7]);
}

/* ── lifecycle ─────────────────────────────────────────────────────────── */
void xnet_replay_init(xnet_replay_t* r) {
    int s;
    if (!r) return;
    memset(r, 0, sizeof(*r));
    for (s = 0; s < XNET_STREAM_COUNT; s++)
        r->tx[s] = 1;                 /* seq 0 reserved, never emitted */
    /* rx[][].highest = 0 (no packet yet); session_id zeroed, session_valid 0.
     * Leaving session_id all-zero is a safe degraded mode if an old relay
     * never sends a nonce: two updated peers still agree on the (zero) value,
     * so intra-session replay protection works; only cross-session is lost. */
}

void xnet_replay_set_session(xnet_replay_t* r, const uint8_t sid[XNET_SESSION_ID_LEN]) {
    if (!r || !sid) return;
    memcpy(r->session_id, sid, XNET_SESSION_ID_LEN);
    r->session_valid = 1;
}

/* ── send counter ──────────────────────────────────────────────────────── */
uint64_t xnet_replay_tx_next(xnet_replay_t* r, xnet_stream_t s) {
    uint64_t v;
    if (!r || (unsigned)s >= XNET_STREAM_COUNT) return 0;
    v = r->tx[s];
    if (v == 0) return 0;             /* exhausted -> caller must start a new room */
    r->tx[s] = v + 1;                 /* wraps after UINT64_MAX; caught next call */
    return v;
}

/* ── sliding-window anti-replay ────────────────────────────────────────── */
static void window_shift_left(xnet_replay_window_t* w, uint64_t s) {
    uint32_t word_shift, bit_shift;
    int i;
    if (s >= XNET_REPLAY_WINDOW_BITS) {
        memset(w->bitmap, 0, sizeof(w->bitmap));
        w->bitmap[0] = 1u;
        return;
    }
    word_shift = (uint32_t)(s / 64);
    bit_shift  = (uint32_t)(s % 64);
    for (i = XNET_REPLAY_WINDOW_WORDS - 1; i >= 0; i--) {
        int src = i - (int)word_shift;
        uint64_t val = 0;
        if (src >= 0) {
            val = w->bitmap[src] << bit_shift;
            if (bit_shift != 0 && src - 1 >= 0)
                val |= w->bitmap[src - 1] >> (64 - bit_shift);
        }
        w->bitmap[i] = val;
    }
    w->bitmap[0] |= 1u;
}

int xnet_replay_rx_check(xnet_replay_t* r, uint8_t slot, xnet_stream_t s, uint64_t seq) {
    xnet_replay_window_t* w;
    uint64_t offset;
    uint32_t word, bit;

    if (!r || slot >= MAX_SLOTS || (unsigned)s >= XNET_STREAM_COUNT) return 0;
    if (seq == 0) return 0;          /* 0 is reserved, never legitimately sent */
    w = &r->rx[slot][s];

    if (seq > w->highest) {          /* newest so far */
        window_shift_left(w, seq - w->highest);
        w->highest = seq;
        return 1;
    }
    offset = w->highest - seq;
    if (offset >= XNET_REPLAY_WINDOW_BITS) return 0;        /* too old */
    word = (uint32_t)(offset / 64);
    bit  = (uint32_t)(offset % 64);
    if (w->bitmap[word] & ((uint64_t)1 << bit)) return 0;   /* already seen */
    w->bitmap[word] |= ((uint64_t)1 << bit);                /* mark, accept */
    return 1;
}

/* ── associated data ───────────────────────────────────────────────────── */
static void build_aad(uint8_t out[XNET_AAD_LEN], const uint8_t sid[XNET_SESSION_ID_LEN],
                      xnet_stream_t s, uint8_t slot, uint64_t seq) {
    memcpy(out, sid, XNET_SESSION_ID_LEN);   /* [0..15]  */
    out[16] = (uint8_t)s;                    /* [16] stream */
    out[17] = slot;                          /* [17] sender slot */
    put_u64_be(out + 18, seq);               /* [18..25] seq */
}

/* ── seal / open ───────────────────────────────────────────────────────── */
int xnet_replay_seal(xnet_replay_t* r, const uint8_t* key,
                     xnet_stream_t s, uint8_t my_slot,
                     const uint8_t* plain, int plain_len,
                     uint8_t* out, int out_max, int block) {
    uint8_t  aad[XNET_AAD_LEN];
    uint64_t seq;
    int      clen;

    if (!r || out_max < XNET_REPLAY_SEQ_LEN) return -1;

    seq = xnet_replay_tx_next(r, s);
    if (seq == 0) return -1;                 /* exhausted — caller starts new room */

    build_aad(aad, r->session_id, s, my_slot, seq);

    /* crypto blob goes after the 8-byte seq prefix */
    if (block)
        clen = xnet_crypto_encrypt_block_ad(key, aad, XNET_AAD_LEN, plain, plain_len,
                                            out + XNET_REPLAY_SEQ_LEN,
                                            out_max - XNET_REPLAY_SEQ_LEN);
    else
        clen = xnet_crypto_encrypt_ad(key, aad, XNET_AAD_LEN, plain, plain_len,
                                      out + XNET_REPLAY_SEQ_LEN,
                                      out_max - XNET_REPLAY_SEQ_LEN);
    if (clen < 0) return -1;

    put_u64_be(out, seq);                    /* prepend seq */
    return XNET_REPLAY_SEQ_LEN + clen;
}

int xnet_replay_open(xnet_replay_t* r, const uint8_t* key,
                     xnet_stream_t s, uint8_t sender_slot,
                     const uint8_t* in, int in_len,
                     uint8_t* out, int out_max, int block) {
    uint8_t  aad[XNET_AAD_LEN];
    uint64_t seq;
    int      plen;

    if (!r || in_len < XNET_REPLAY_SEQ_LEN) return -1;

    seq = get_u64_be(in);
    /* rebuild aad with the LOCAL session_id — a frame from a different room
     * (or with a tampered seq/slot/stream) won't match and the MAC fails. */
    build_aad(aad, r->session_id, s, sender_slot, seq);

    if (block)
        plen = xnet_crypto_decrypt_block_ad(key, aad, XNET_AAD_LEN,
                                            in + XNET_REPLAY_SEQ_LEN,
                                            in_len - XNET_REPLAY_SEQ_LEN,
                                            out, out_max);
    else
        plen = xnet_crypto_decrypt_ad(key, aad, XNET_AAD_LEN,
                                      in + XNET_REPLAY_SEQ_LEN,
                                      in_len - XNET_REPLAY_SEQ_LEN,
                                      out, out_max);
    if (plen < 0) return -1;                 /* MAC failure / malformed -> drop */

    /* authenticity proven — only now is it safe to touch the window */
    if (!xnet_replay_rx_check(r, sender_slot, s, seq))
        return -2;                           /* replay / too old -> drop */

    return plen;
}
