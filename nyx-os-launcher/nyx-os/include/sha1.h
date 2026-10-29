// include/sha1.h — SHA-1 (FIPS 180-1)
#ifndef SHA1_H
#define SHA1_H

#include <stdint.h>

typedef struct {
    uint32_t h[5];
    uint64_t len;          // total message length in bytes
    uint8_t  buf[64];
    int      buf_len;
} sha1_ctx;

void sha1_init(sha1_ctx* c);
void sha1_update(sha1_ctx* c, const void* data, int len);
void sha1_final(sha1_ctx* c, uint8_t out[20]);

void sha1_hash(const void* data, int len, uint8_t out[20]);

#endif
