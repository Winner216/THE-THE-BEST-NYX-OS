// cpu/string.c — minimal memory helpers for kernel code (see
// include/string.h).  The kernel is freestanding (-nostdinc), so
// these four functions back the memcpy/memset/memcmp/memmove calls
// in the crypto stack (tls/aes/bigint/sha1/sha256/ec).
#include <string.h>
#include <stdint.h>

void* memcpy(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

void* memset(void* s, int c, size_t n) {
    uint8_t* p = (uint8_t*)s;
    for (size_t i = 0; i < n; i++) p[i] = (uint8_t)c;
    return s;
}

int memcmp(const void* a, const void* b, size_t n) {
    const uint8_t* x = (const uint8_t*)a;
    const uint8_t* y = (const uint8_t*)b;
    for (size_t i = 0; i < n; i++) {
        if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    }
    return 0;
}

void* memmove(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    if (d <= s || d >= s + n) {
        // No overlap (or d before s): forward copy is safe.
        for (size_t i = 0; i < n; i++) d[i] = s[i];
    } else {
        // Overlap with d after s: copy backwards.
        for (size_t i = n; i > 0; i--) d[i - 1] = s[i - 1];
    }
    return dst;
}
