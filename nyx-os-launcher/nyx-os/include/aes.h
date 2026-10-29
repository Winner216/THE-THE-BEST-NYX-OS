// include/aes.h — AES-128 (block cipher) with CBC helpers
#ifndef AES_H
#define AES_H

#include <stdint.h>

typedef struct {
    uint32_t rk[44];   // 11 round keys
} aes128_ctx;

void aes128_setkey(aes128_ctx* c, const uint8_t key[16]);
void aes128_encrypt(const aes128_ctx* c, const uint8_t in[16], uint8_t out[16]);
void aes128_decrypt(const aes128_ctx* c, const uint8_t in[16], uint8_t out[16]);

// CBC mode over whole buffers (len must be a multiple of 16).
// iv[16] is read (encrypt) / read+written (decrypt).
void aes128_cbc_encrypt(const aes128_ctx* c, const uint8_t* in, uint8_t* out,
                        int len, uint8_t iv[16]);
void aes128_cbc_decrypt(const aes128_ctx* c, const uint8_t* in, uint8_t* out,
                        int len, const uint8_t iv[16]);

// AES-128-GCM (NIST SP 800-38D) with a 12-byte IV — used by the TLS
// 1.2 AEAD cipher suites (e.g. 0xC02B).  `seal` writes ciphertext to
// out (in_len bytes) and the 16-byte auth tag; `open` decrypts and
// verifies the tag, returning 1 on success, 0 on authentication
// failure (out contents then undefined).
void aes128_gcm_seal(const uint8_t key[16], const uint8_t iv[12],
                     const uint8_t* aad, int aad_len,
                     const uint8_t* in, int in_len,
                     uint8_t* out, uint8_t tag[16]);
int  aes128_gcm_open(const uint8_t key[16], const uint8_t iv[12],
                     const uint8_t* aad, int aad_len,
                     const uint8_t* in, int in_len,
                     uint8_t* out, const uint8_t tag[16]);

#endif
