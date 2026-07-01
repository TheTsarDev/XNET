/**
 * xnet_crypto.c
 * AES-128 CBC + SHA-256 for XNET
 *
 * Depends on tiny-AES-c (vendor/aes.h / aes.c) — public domain
 * https://github.com/kokke/tiny-AES-c
 *
 * SHA-256 is a self-contained implementation below.
 */

#include "xnet_crypto.h"
#include "aes.h"  /* tiny-AES-c — lives in vendor/, added to include path via Makefile */
#include <string.h>
#include <stdint.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#define MAX_ENCRYPTED_PLAINTEXT 256

/* ── PKCS7 PADDING ────────────────────────────────────────────────────────────── */
static int pkcs7_pad(const uint8_t* in, int in_len, uint8_t* out, int out_max) {
    int pad_len = 16 - (in_len % 16);
    int total   = in_len + pad_len;
    if (total > out_max) return -1;
    memcpy(out, in, in_len);
    memset(out + in_len, (uint8_t)pad_len, pad_len);
    return total;
}

static int pkcs7_unpad(uint8_t* data, int len) {
    if (len <= 0 || len % 16 != 0) return -1;
    uint8_t pad = data[len - 1];
    if (pad == 0 || pad > 16) return -1;
    for (int i = len - pad; i < len; i++) {
        if (data[i] != pad) return -1;
    }
    return len - pad;
}

/* ── RANDOM IV ───────────────────────────────────────────────────────────────── */
static void random_iv(uint8_t* iv) {
    /* Seed from Xbox performance counter — different every call */
    ULONGLONG t = KeQueryPerformanceCounter();
    uint32_t seed = (uint32_t)(t ^ (t >> 32));
    for (int i = 0; i < 16; i++) {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        iv[i] = (uint8_t)(seed & 0xFF);
    }
}

/* ── ENCRYPT ──────────────────────────────────────────────────────────────────── */
/* ── ENCRYPT ──────────────────────────────────────────────────────────────────── */
/* XNET uses encrypt-then-MAC: every ciphertext is (IV || AES-CBC ciphertext)
   followed by a 32-byte HMAC-SHA-256 tag computed over (IV || ciphertext). The
   receiver verifies the tag in constant time BEFORE decrypting and rejects any
   packet that fails, which detects tampering and avoids decrypting attacker-
   controlled data. The MAC subkey is derived from the AES key with domain
   separation, so callers keep passing a single key. */
#define XNET_MAC_LEN 32

static void sha256(const uint8_t* data, int len, uint8_t* hash_out);

#define XNET_HMAC_AAD_MAX 64
static void hmac_sha256_ad(const uint8_t* key, int klen,
                           const uint8_t* aad, int aad_len,
                           const uint8_t* msg, int mlen, uint8_t* out32) {
    /* single-threaded: encrypt (send) and decrypt (recv) never run reentrantly,
       so a static scratch buffer is safe and keeps this off the stack.
       Sized for ipad(64) || aad(<=64) || msg(<=8192). */
    static uint8_t buf[64 + XNET_HMAC_AAD_MAX + 8192];
    uint8_t k[64];
    uint8_t inner[32];
    int i;

    memset(k, 0, 64);
    if (klen > 64) sha256(key, klen, k);   /* long key -> hash to 32 bytes */
    else           memcpy(k, key, klen);

    if (aad_len < 0)                 aad_len = 0;
    if (aad_len > XNET_HMAC_AAD_MAX) aad_len = XNET_HMAC_AAD_MAX;
    if (mlen < 0)    mlen = 0;
    if (mlen > 8192) mlen = 8192;          /* payloads are capped well under this */

    for (i = 0; i < 64; i++) buf[i] = k[i] ^ 0x36;     /* ipad */
    if (aad_len) memcpy(buf + 64, aad, aad_len);       /* aad authenticated, not stored */
    memcpy(buf + 64 + aad_len, msg, mlen);
    sha256(buf, 64 + aad_len + mlen, inner);

    for (i = 0; i < 64; i++) buf[i] = k[i] ^ 0x5c;     /* opad */
    memcpy(buf + 64, inner, 32);
    sha256(buf, 64 + 32, out32);
}

static void derive_mac_key(const uint8_t* aes_key, uint8_t* mac_key32) {
    uint8_t tmp[16 + 9];
    memcpy(tmp, aes_key, 16);
    memcpy(tmp + 16, "XNET-MAC1", 9);      /* domain separation from the AES key */
    sha256(tmp, 16 + 9, mac_key32);
}

/* Separate key for IV derivation, domain-separated from the AES key exactly as
 * the MAC key is. Must differ from both the AES key and the MAC key. */
static void derive_iv_key(const uint8_t* aes_key, uint8_t* iv_key32) {
    uint8_t tmp[16 + 8];
    memcpy(tmp, aes_key, 16);
    memcpy(tmp + 16, "XNET-IV1", 8);       /* domain separation from the AES key */
    sha256(tmp, 16 + 8, iv_key32);
}

/* Synthetic IV: IV = first 16 bytes of HMAC(K_iv, aad). Unpredictable (an
 * attacker cannot compute it without K_iv) and non-repeating (the aad ==
 * session_id||stream||slot||seq is unique per message, so the HMAC input never
 * repeats under one key). Replaces the perf-counter RNG, whose output is
 * predictable from frame timing — a chosen-plaintext (BEAST) hazard for CBC.
 * Requires a non-empty, per-message-unique aad; callers without one fall back
 * to random_iv(). */
static void synth_iv(const uint8_t* aes_key, const uint8_t* aad, int aad_len,
                     uint8_t* iv) {
    uint8_t iv_key[32];
    uint8_t mac[32];
    derive_iv_key(aes_key, iv_key);
    hmac_sha256_ad(iv_key, 32, 0, 0, aad, aad_len, mac);  /* HMAC over the aad */
    memcpy(iv, mac, 16);
}

/* constant-time equality — no early-out on first mismatched byte */
static int ct_equal(const uint8_t* a, const uint8_t* b, int n) {
    uint8_t d = 0;
    for (int i = 0; i < n; i++) d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}

int xnet_crypto_encrypt(const uint8_t* key,
                        const uint8_t* plaintext, int plain_len,
                        uint8_t* out, int out_max) {
    return xnet_crypto_encrypt_ad(key, 0, 0, plaintext, plain_len, out, out_max);
}

int xnet_crypto_encrypt_ad(const uint8_t* key,
                           const uint8_t* aad, int aad_len,
                           const uint8_t* plaintext, int plain_len,
                           uint8_t* out, int out_max) {
    uint8_t iv[16];
    uint8_t padded[MAX_ENCRYPTED_PLAINTEXT];
    int     padded_len;
    struct AES_ctx ctx;

    if (plain_len <= 0 || plain_len > 128) return -1;

    padded_len = pkcs7_pad(plaintext, plain_len, padded, sizeof(padded));
    if (padded_len < 0) return -1;

    if (16 + padded_len + XNET_MAC_LEN > out_max) return -1;

    if (aad_len > 0) synth_iv(key, aad, aad_len, iv);  /* unpredictable, non-repeating */
    else             random_iv(iv);                    /* aad-less fallback (unused in data path) */
    memcpy(out, iv, 16); /* prepend IV */

    AES_init_ctx_iv(&ctx, key, iv);
    memcpy(out + 16, padded, padded_len);
    AES_CBC_encrypt_buffer(&ctx, out + 16, padded_len);

    /* encrypt-then-MAC: tag over (aad || IV || ciphertext) */
    {
        uint8_t mac_key[32];
        derive_mac_key(key, mac_key);
        hmac_sha256_ad(mac_key, 32, aad, aad_len,
                       out, 16 + padded_len, out + 16 + padded_len);
    }
    return 16 + padded_len + XNET_MAC_LEN;
}

/* ── DECRYPT ──────────────────────────────────────────────────────────────────── */
int xnet_crypto_decrypt(const uint8_t* key,
                        const uint8_t* ciphertext, int cipher_len,
                        uint8_t* out, int out_max) {
    return xnet_crypto_decrypt_ad(key, 0, 0, ciphertext, cipher_len, out, out_max);
}

int xnet_crypto_decrypt_ad(const uint8_t* key,
                           const uint8_t* aad, int aad_len,
                           const uint8_t* ciphertext, int cipher_len,
                           uint8_t* out, int out_max) {
    uint8_t iv[16];
    struct AES_ctx ctx;
    int plain_len;

    if (cipher_len < 16 + 16 + XNET_MAC_LEN) return -1; /* IV + block + tag */

    int data_len = cipher_len - XNET_MAC_LEN;   /* IV || ciphertext */

    /* verify tag before doing anything with the ciphertext */
    {
        uint8_t mac_key[32], tag[32];
        derive_mac_key(key, mac_key);
        hmac_sha256_ad(mac_key, 32, aad, aad_len, ciphertext, data_len, tag);
        if (!ct_equal(tag, ciphertext + data_len, XNET_MAC_LEN)) return -1;
    }

    if (data_len - 16 > out_max) return -1;

    memcpy(iv, ciphertext, 16);

    int enc_len = data_len - 16;
    memcpy(out, ciphertext + 16, enc_len);

    AES_init_ctx_iv(&ctx, key, iv);
    AES_CBC_decrypt_buffer(&ctx, out, enc_len);

    plain_len = pkcs7_unpad(out, enc_len);
    return plain_len;
}

/* ── BULK ENCRYPT / DECRYPT (file transfer) ──────────────────────────────────────
 * No 128-byte cap, no internal scratch buffer. Pads in place into out.
 */
int xnet_crypto_encrypt_block(const uint8_t* key,
                              const uint8_t* plaintext, int plain_len,
                              uint8_t* out, int out_max) {
    return xnet_crypto_encrypt_block_ad(key, 0, 0, plaintext, plain_len, out, out_max);
}

int xnet_crypto_encrypt_block_ad(const uint8_t* key,
                                 const uint8_t* aad, int aad_len,
                                 const uint8_t* plaintext, int plain_len,
                                 uint8_t* out, int out_max) {
    struct AES_ctx ctx;
    uint8_t iv[16];
    int pad_len, padded_len;

    if (plain_len <= 0) return -1;

    pad_len    = 16 - (plain_len % 16); /* PKCS7: always 1..16 */
    padded_len = plain_len + pad_len;
    if (16 + padded_len + XNET_MAC_LEN > out_max) return -1;

    if (aad_len > 0) synth_iv(key, aad, aad_len, iv);  /* unpredictable, non-repeating */
    else             random_iv(iv);                    /* aad-less fallback (unused in data path) */
    memcpy(out, iv, 16);                       /* prepend IV */
    memcpy(out + 16, plaintext, plain_len);    /* copy plaintext */
    memset(out + 16 + plain_len, (uint8_t)pad_len, pad_len); /* PKCS7 pad */

    AES_init_ctx_iv(&ctx, key, iv);
    AES_CBC_encrypt_buffer(&ctx, out + 16, padded_len);

    /* encrypt-then-MAC: tag over (aad || IV || ciphertext) */
    {
        uint8_t mac_key[32];
        derive_mac_key(key, mac_key);
        hmac_sha256_ad(mac_key, 32, aad, aad_len,
                       out, 16 + padded_len, out + 16 + padded_len);
    }
    return 16 + padded_len + XNET_MAC_LEN;
}

int xnet_crypto_decrypt_block(const uint8_t* key,
                              const uint8_t* ciphertext, int cipher_len,
                              uint8_t* out, int out_max) {
    return xnet_crypto_decrypt_block_ad(key, 0, 0, ciphertext, cipher_len, out, out_max);
}

int xnet_crypto_decrypt_block_ad(const uint8_t* key,
                                 const uint8_t* aad, int aad_len,
                                 const uint8_t* ciphertext, int cipher_len,
                                 uint8_t* out, int out_max) {
    struct AES_ctx ctx;
    uint8_t iv[16];
    int enc_len;

    if (cipher_len < 16 + 16 + XNET_MAC_LEN) return -1; /* IV + block + tag */

    int data_len = cipher_len - XNET_MAC_LEN;   /* IV || ciphertext */
    if ((data_len - 16) % 16 != 0)       return -1;

    /* verify tag before touching the ciphertext */
    {
        uint8_t mac_key[32], tag[32];
        derive_mac_key(key, mac_key);
        hmac_sha256_ad(mac_key, 32, aad, aad_len, ciphertext, data_len, tag);
        if (!ct_equal(tag, ciphertext + data_len, XNET_MAC_LEN)) return -1;
    }

    enc_len = data_len - 16;
    if (enc_len > out_max)               return -1;

    memcpy(iv, ciphertext, 16);
    memcpy(out, ciphertext + 16, enc_len);

    AES_init_ctx_iv(&ctx, key, iv);
    AES_CBC_decrypt_buffer(&ctx, out, enc_len);

    return pkcs7_unpad(out, enc_len);
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * SHA-256 (self-contained, public domain)
 * ═══════════════════════════════════════════════════════════════════════════════ */
#define ROR32(x,n) (((x)>>(n))|((x)<<(32-(n))))
#define SHR(x,n)   ((x)>>(n))
#define CH(x,y,z)  (((x)&(y))^(~(x)&(z)))
#define MAJ(x,y,z) (((x)&(y))^((x)&(z))^((y)&(z)))
#define EP0(x)     (ROR32(x,2)  ^ ROR32(x,13) ^ ROR32(x,22))
#define EP1(x)     (ROR32(x,6)  ^ ROR32(x,11) ^ ROR32(x,25))
#define SIG0(x)    (ROR32(x,7)  ^ ROR32(x,18) ^ SHR(x,3))
#define SIG1(x)    (ROR32(x,17) ^ ROR32(x,19) ^ SHR(x,10))

static const uint32_t K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,
    0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,
    0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,
    0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,
    0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,
    0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
};

static void sha256(const uint8_t* data, int len, uint8_t* hash_out) {
    uint32_t h[8] = {
        0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
        0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19,
    };
    uint8_t  block[64];
    uint64_t bit_len = (uint64_t)len * 8;
    int      i, j;

    /* process full 64-byte blocks */
    while (len >= 64) {
        uint32_t w[64], a,b,c,d,e,f,g,h2,t1,t2;
        for (i = 0; i < 16; i++) {
            w[i] = ((uint32_t)data[i*4]   << 24) | ((uint32_t)data[i*4+1] << 16)
                 | ((uint32_t)data[i*4+2] <<  8) |  (uint32_t)data[i*4+3];
        }
        for (i = 16; i < 64; i++) {
            w[i] = SIG1(w[i-2]) + w[i-7] + SIG0(w[i-15]) + w[i-16];
        }
        a=h[0];b=h[1];c=h[2];d=h[3];e=h[4];f=h[5];g=h[6];h2=h[7];
        for (i = 0; i < 64; i++) {
            t1 = h2 + EP1(e) + CH(e,f,g) + K256[i] + w[i];
            t2 = EP0(a) + MAJ(a,b,c);
            h2=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=h2;
        data += 64; len -= 64;
    }

    /* padding */
    memcpy(block, data, len);
    block[len++] = 0x80;
    if (len > 56) {
        memset(block + len, 0, 64 - len);
        /* process this block */
        uint32_t w[64],a,b,c,d,e,f,g,h2,t1,t2;
        for (i=0;i<16;i++) w[i]=((uint32_t)block[i*4]<<24)|((uint32_t)block[i*4+1]<<16)|((uint32_t)block[i*4+2]<<8)|(uint32_t)block[i*4+3];
        for (i=16;i<64;i++) w[i]=SIG1(w[i-2])+w[i-7]+SIG0(w[i-15])+w[i-16];
        a=h[0];b=h[1];c=h[2];d=h[3];e=h[4];f=h[5];g=h[6];h2=h[7];
        for (i=0;i<64;i++){t1=h2+EP1(e)+CH(e,f,g)+K256[i]+w[i];t2=EP0(a)+MAJ(a,b,c);h2=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;}
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=h2;
        len = 0;
    }
    memset(block + len, 0, 56 - len);
    /* append bit length big-endian */
    for (i = 0; i < 8; i++) block[56 + i] = (uint8_t)(bit_len >> (56 - i*8));
    {
        uint32_t w[64],a,b,c,d,e,f,g,h2,t1,t2;
        for (i=0;i<16;i++) w[i]=((uint32_t)block[i*4]<<24)|((uint32_t)block[i*4+1]<<16)|((uint32_t)block[i*4+2]<<8)|(uint32_t)block[i*4+3];
        for (i=16;i<64;i++) w[i]=SIG1(w[i-2])+w[i-7]+SIG0(w[i-15])+w[i-16];
        a=h[0];b=h[1];c=h[2];d=h[3];e=h[4];f=h[5];g=h[6];h2=h[7];
        for (i=0;i<64;i++){t1=h2+EP1(e)+CH(e,f,g)+K256[i]+w[i];t2=EP0(a)+MAJ(a,b,c);h2=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;}
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=h2;
    }

    for (i = 0; i < 8; i++) {
        hash_out[i*4+0] = (h[i] >> 24) & 0xFF;
        hash_out[i*4+1] = (h[i] >> 16) & 0xFF;
        hash_out[i*4+2] = (h[i] >>  8) & 0xFF;
        hash_out[i*4+3] =  h[i]        & 0xFF;
    }
}

void xnet_crypto_sha256_16(const char* data, int len, uint8_t* key_out) {
    uint8_t full_hash[32];
    sha256((const uint8_t*)data, len, full_hash);
    memcpy(key_out, full_hash, 16); /* first 16 bytes = AES-128 key */
}
