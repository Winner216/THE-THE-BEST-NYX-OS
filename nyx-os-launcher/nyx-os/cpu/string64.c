// cpu/string64.c — Minimal string functions for the 64-bit kernel
//
// The 32-bit kernel uses cpu/string.c which has 32-bit assumptions.
// This file provides 64-bit equivalents for the few functions needed
// during early boot and kernel initialization.

#include <stdint.h>
#include <stddef.h>

// Memory operations
void* memcpy64(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

void* memset64(void* dst, int val, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    uint8_t v = (uint8_t)val;
    for (size_t i = 0; i < n; i++) d[i] = v;
    return dst;
}

int memcmp64(const void* a, const void* b, size_t n) {
    const uint8_t* pa = (const uint8_t*)a;
    const uint8_t* pb = (const uint8_t*)b;
    for (size_t i = 0; i < n; i++) {
        if (pa[i] != pb[i]) return (int)pa[i] - (int)pb[i];
    }
    return 0;
}

// String operations
size_t strlen64(const char* s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int strcmp64(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

char* strcpy64(char* dst, const char* src) {
    char* r = dst;
    while ((*dst++ = *src++));
    return r;
}

// Provide the standard names that the kernel expects
void* memcpy(void* dst, const void* src, size_t n) __attribute__((alias("memcpy64")));
void* memset(void* dst, int val, size_t n) __attribute__((alias("memset64")));
int memcmp(const void* a, const void* b, size_t n) __attribute__((alias("memcmp64")));
size_t strlen(const char* s) __attribute__((alias("strlen64")));
int strcmp(const char* a, const char* b) __attribute__((alias("strcmp64")));
char* strcpy(char* dst, const char* src) __attribute__((alias("strcpy64")));
