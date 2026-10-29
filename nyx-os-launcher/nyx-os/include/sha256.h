// include/sha256.h — SHA-256 (FIPS 180-2)
#ifndef SHA256_H
#define SHA256_H

#include <stdint.h>

typedef struct {
    uint32_t h[8];
    uint64_t len;
    uint8_t  buf[64];
    int      buf_len;
} sha256_ctx;

void sha256_init(sha256_ctx* c);
void sha256_update(sha256_ctx* c, const void* data, int len);
void sha256_final(sha256_ctx* c, uint8_t out[32]);

void sha256_hash(const void* data, int len, uint8_t out[32]);

#endif
