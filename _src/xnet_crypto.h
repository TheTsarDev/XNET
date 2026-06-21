/**
 * xnet_crypto.h
 * AES-128 CBC encrypt/decrypt + SHA-256 key derivation
 *
 * Uses tiny-AES-c (public domain) bundled in vendor/
 * SHA-256 implemented inline — no external deps.
 *
 * Key derivation:
 *   SHA256(token_string) → take first 16 bytes → AES-128 key
 *   IV is random 16 bytes prepended to every ciphertext frame.
 *
 * Frame layout (ciphertext):
 *   [16 bytes] random IV
 *   [N bytes]  AES-128 CBC ciphertext (PKCS7 padded)
 */

#ifndef XNET_CRYPTO_H
#define XNET_CRYPTO_H

#include <stdint.h>

/**
 * Encrypt plaintext with AES-128 CBC.
 * Prepends random IV to output.
 * Returns ciphertext length (IV + encrypted data), or -1 on error.
 */
int xnet_crypto_encrypt(const uint8_t* key,
                        const uint8_t* plaintext, int plain_len,
                        uint8_t* out, int out_max);

/**
 * Decrypt ciphertext with AES-128 CBC.
 * Reads IV from first 16 bytes of input.
 * Returns plaintext length, or -1 on error.
 */
int xnet_crypto_decrypt(const uint8_t* key,
                        const uint8_t* ciphertext, int cipher_len,
                        uint8_t* out, int out_max);

/**
 * Bulk variants for file transfer — same AES-128 CBC + prepended random IV
 * + PKCS7 layout as the text path, but with NO 128-byte plaintext cap and
 * caller-provided output buffers (no fixed internal stack buffer).
 *
 *   encrypt: out must be >= 16 + roundup16(plain_len). Returns ciphertext
 *            length (IV + encrypted), or -1 on error.
 *   decrypt: reads IV from first 16 bytes. Returns plaintext length, or -1.
 *
 * Used for PKT_FILE_META / PKT_FILE_DATA payloads.
 */
int xnet_crypto_encrypt_block(const uint8_t* key,
                              const uint8_t* plaintext, int plain_len,
                              uint8_t* out, int out_max);

int xnet_crypto_decrypt_block(const uint8_t* key,
                              const uint8_t* ciphertext, int cipher_len,
                              uint8_t* out, int out_max);

/**
 * SHA-256 hash of data, take first 16 bytes into key_out.
 * Used for token → AES key derivation.
 */
void xnet_crypto_sha256_16(const char* data, int len, uint8_t* key_out);

#endif /* XNET_CRYPTO_H */
