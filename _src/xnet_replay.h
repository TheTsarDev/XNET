/**
 * xnet_replay.h
 * Anti-replay for XNET — adds freshness on top of the existing
 * encrypt-then-MAC (AES-128-CBC + HMAC-SHA-256) in xnet_crypto.c.
 *
 * Authentication already stops forgery/tampering. Replay is the remaining gap:
 * a captured-but-valid frame re-injected by a hostile/compromised relay (which
 * terminates the TCP connection) is still a valid frame. Two mechanisms close
 * it, both bound into the existing HMAC so they can't be stripped or rewritten:
 *
 *   1. Per-(stream) monotonic send counter + per-(sender slot, stream) receive
 *      sliding window. Counter rides on the wire (8 bytes ahead of the IV);
 *      window gives intra-session protection and tolerates reorder/drop.
 *
 *   2. A 16-byte per-room session_id, minted by the relay at room creation and
 *      handed to every member (TOKEN/JOINED). NOT transmitted in data frames —
 *      it is mixed into the MAC only. Because XNET keys are token-derived and
 *      therefore reused across sessions, this is what stops a frame captured
 *      from an earlier room (same token) from being replayed into a new one:
 *      the old frame's tag was computed over a different session_id.
 *
 * Wire layout of an encrypted payload changes from:
 *      [IV 16][ciphertext][MAC 32]
 * to:
 *      [seq 8 BE][IV 16][ciphertext][MAC 32]
 * MAC now covers:  session_id(16) || stream(1) || sender_slot(1) || seq(8)
 *                  || IV(16) || ciphertext
 *
 * This is a wire-format break — all peers and the relay must run >= v0.5.0.
 */
#ifndef XNET_REPLAY_H
#define XNET_REPLAY_H

#include <stdint.h>

/* MAX_SLOTS comes from main.h when included after it; standalone fallback keeps
 * this header independent (and avoids a main.h <-> xnet_replay.h include cycle,
 * since main.h embeds xnet_replay_t by value). */
#ifndef MAX_SLOTS
#define MAX_SLOTS 4
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Reorder/drop tolerance in packets. TCP keeps streams in order, so the
 * monotonic fast-path is the norm; this width only matters if a hostile relay
 * reorders. 2 words = 128-packet window. */
#ifndef XNET_REPLAY_WINDOW_WORDS
#define XNET_REPLAY_WINDOW_WORDS 2
#endif
#define XNET_REPLAY_WINDOW_BITS (XNET_REPLAY_WINDOW_WORDS * 64)

#define XNET_SESSION_ID_LEN 16
#define XNET_REPLAY_SEQ_LEN 8                 /* on-wire counter width (BE) */
/* AAD = session_id(16) || stream(1) || slot(1) || seq(8) */
#define XNET_AAD_LEN (XNET_SESSION_ID_LEN + 1 + 1 + 8)

/* One replay window per stream class, per sender slot. Binding stream + slot
 * into the MAC stops a frame being relabelled across either axis. */
typedef enum {
    XNET_STREAM_TEXT = 0,
    XNET_STREAM_VOICE,
    XNET_STREAM_VIDEO,
    XNET_STREAM_FILE,
    XNET_STREAM_COUNT
} xnet_stream_t;

typedef struct {
    uint64_t highest;                          /* highest seq accepted; 0 = none */
    uint64_t bitmap[XNET_REPLAY_WINDOW_WORDS]; /* bit k => (highest-k) seen */
} xnet_replay_window_t;

typedef struct {
    uint8_t  session_id[XNET_SESSION_ID_LEN];
    int      session_valid;                    /* 0 until relay nonce received */
    uint64_t tx[XNET_STREAM_COUNT];                       /* this node's send counters */
    xnet_replay_window_t rx[MAX_SLOTS][XNET_STREAM_COUNT];/* per-peer receive windows */
} xnet_replay_t;

/* ---- lifecycle --------------------------------------------------------- */
void xnet_replay_init(xnet_replay_t* r);
void xnet_replay_set_session(xnet_replay_t* r,
                             const uint8_t sid[XNET_SESSION_ID_LEN]);

/* ---- core checks (exposed for testing; seal/open use them) ------------- */
/* Returns next seq for this stream and advances; 0 means counter exhausted
 * (caller must force a rehandshake / new room and NOT transmit). */
uint64_t xnet_replay_tx_next(xnet_replay_t* r, xnet_stream_t s);
/* Call ONLY after the MAC verifies. 1 = fresh (accept), 0 = replay/too old. */
int xnet_replay_rx_check(xnet_replay_t* r, uint8_t slot,
                         xnet_stream_t s, uint64_t seq);

/* ---- framing helpers --------------------------------------------------- */
/* Seal: encrypt + MAC(aad) + prepend seq. Produces
 *   [seq 8][IV 16][ct][MAC 32]  into out.
 * `block` selects the bulk crypto path (file/video) vs the capped path
 * (text/voice). my_slot is this node's slot. Returns total length or -1. */
int xnet_replay_seal(xnet_replay_t* r, const uint8_t* key,
                     xnet_stream_t s, uint8_t my_slot,
                     const uint8_t* plain, int plain_len,
                     uint8_t* out, int out_max, int block);

/* Open: parse seq, rebuild aad with LOCAL session_id, verify MAC + decrypt,
 * THEN replay-check against (sender_slot, stream). Returns plaintext length,
 * or -1 on MAC failure / malformed, or -2 on replay (already-seen/too-old). */
int xnet_replay_open(xnet_replay_t* r, const uint8_t* key,
                     xnet_stream_t s, uint8_t sender_slot,
                     const uint8_t* in, int in_len,
                     uint8_t* out, int out_max, int block);

#ifdef __cplusplus
}
#endif
#endif /* XNET_REPLAY_H */
