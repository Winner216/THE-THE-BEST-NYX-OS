// include/string.h — minimal memory helpers for kernel code.
// The kernel builds with -nostdinc (no libc), so the crypto files
// (tls/aes/bigint/sha*/ec) get their memcpy/memset/memcmp/memmove
// from cpu/string.c instead of glibc.
#ifndef STRING_H
#define STRING_H

#include <stddef.h>

void* memcpy(void* dst, const void* src, size_t n);
void* memset(void* s, int c, size_t n);
int   memcmp(const void* a, const void* b, size_t n);
void* memmove(void* dst, const void* src, size_t n);

#endif
