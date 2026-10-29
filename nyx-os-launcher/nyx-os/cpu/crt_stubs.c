// cpu/crt_stubs.c  –  Minimal C runtime stubs for Win32 API compatibility.
//
// Provides stub implementations of common C library functions that
// Windows PE executables expect.  These are not full implementations —
// they return safe defaults or empty results.  A real OS would port
// a libc like musl or newlib.
//
// For Telegram, most of these are never called (Telegram uses its own
// crypto and networking), but they prevent link errors.

#include <stdint.h>
#include <stddef.h>

// ─── String functions ───────────────────────────────────────────

size_t strlen(const char* s) {
    if (!s) return 0;
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char* a, const char* b) {
    if (!a || !b) return (a == b) ? 0 : (a ? 1 : -1);
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char* a, const char* b, size_t n) {
    if (!a || !b) return (a == b) ? 0 : (a ? 1 : -1);
    for (size_t i = 0; i < n && a[i] && a[i] == b[i]; i++) {
        if (i == n - 1) return 0;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

char* strcpy(char* dst, const char* src) {
    if (!dst || !src) return dst;
    char* d = dst;
    while ((*d++ = *src++));
    return dst;
}

char* strncpy(char* dst, const char* src, size_t n) {
    if (!dst || !src) return dst;
    size_t i;
    for (i = 0; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = 0;
    return dst;
}

char* strcat(char* dst, const char* src) {
    if (!dst || !src) return dst;
    char* d = dst;
    while (*d) d++;
    while ((*d++ = *src++));
    return dst;
}

// ─── printf-like (minimal) ─────────────────────────────────────

int sprintf(char* str, const char* format, ...) {
    // Minimal: just copy format string
    if (!str || !format) return 0;
    int n = 0;
    while (format[n] && n < 4095) { str[n] = format[n]; n++; }
    str[n] = 0;
    return n;
}

int snprintf(char* str, size_t size, const char* format, ...) {
    if (!str || !format || size == 0) return 0;
    size_t n = 0;
    while (format[n] && n < size - 1) { str[n] = format[n]; n++; }
    str[n] = 0;
    return (int)n;
}

int sscanf(const char* str, const char* format, ...) {
    (void)str; (void)format;
    return 0;
}

// ─── Integer conversion ─────────────────────────────────────────

int atoi(const char* str) {
    if (!str) return 0;
    int result = 0;
    int sign = 1;
    while (*str == ' ' || *str == '\t') str++;
    if (*str == '-') { sign = -1; str++; }
    else if (*str == '+') str++;
    while (*str >= '0' && *str <= '9') {
        result = result * 10 + (*str - '0');
        str++;
    }
    return result * sign;
}

long atol(const char* str) {
    return (long)atoi(str);
}

long strtol(const char* str, char** endptr, int base) {
    if (!str) { if (endptr) *endptr = (char*)str; return 0; }
    long result = 0;
    int sign = 1;
    while (*str == ' ' || *str == '\t') str++;
    if (*str == '-') { sign = -1; str++; }
    else if (*str == '+') str++;
    if (base == 0) {
        if (*str == '0' && (str[1] == 'x' || str[1] == 'X')) { base = 16; str += 2; }
        else if (*str == '0') { base = 8; str++; }
        else base = 10;
    }
    while (*str) {
        int digit;
        if (*str >= '0' && *str <= '9') digit = *str - '0';
        else if (*str >= 'a' && *str <= 'z') digit = *str - 'a' + 10;
        else if (*str >= 'A' && *str <= 'Z') digit = *str - 'A' + 10;
        else break;
        if (digit >= base) break;
        result = result * base + digit;
        str++;
    }
    if (endptr) *endptr = (char*)str;
    return result * sign;
}

unsigned long strtoul(const char* str, char** endptr, int base) {
    return (unsigned long)strtol(str, endptr, base);
}

// ─── Sorting / searching ───────────────────────────────────────

// Simple bubble sort (not efficient, but works for small arrays)
void qsort(void* base, size_t nmemb, size_t size,
           int (*compar)(const void*, const void*)) {
    if (!base || nmemb < 2 || size == 0 || !compar) return;
    char* arr = (char*)base;
    for (size_t i = 0; i < nmemb - 1; i++) {
        for (size_t j = 0; j < nmemb - 1 - i; j++) {
            if (compar(arr + j * size, arr + (j + 1) * size) > 0) {
                // Swap
                for (size_t k = 0; k < size; k++) {
                    char tmp = arr[j * size + k];
                    arr[j * size + k] = arr[(j + 1) * size + k];
                    arr[(j + 1) * size + k] = tmp;
                }
            }
        }
    }
}

void* bsearch(const void* key, const void* base, size_t nmemb, size_t size,
              int (*compar)(const void*, const void*)) {
    if (!key || !base || nmemb == 0 || size == 0 || !compar) return NULL;
    const char* arr = (const char*)base;
    for (size_t i = 0; i < nmemb; i++) {
        if (compar(key, arr + i * size) == 0) return (void*)(arr + i * size);
    }
    return NULL;
}

// ─── Random numbers ─────────────────────────────────────────────

static unsigned int rand_seed = 12345;

int rand(void) {
    rand_seed = rand_seed * 1103515245 + 12345;
    return (rand_seed >> 16) & 0x7FFF;
}

void srand(unsigned int seed) {
    rand_seed = seed;
}

// ─── Memory allocation (stubs) ─────────────────────────────────
// These should really call kmalloc/kfree from the kernel heap

extern void* kmalloc(size_t size);
extern void kfree(void* ptr);

void* calloc(size_t nmemb, size_t size) {
    void* p = kmalloc(nmemb * size);
    if (p) {
        // Zero the memory
        uint8_t* bytes = (uint8_t*)p;
        for (size_t i = 0; i < nmemb * size; i++) bytes[i] = 0;
    }
    return p;
}

void* realloc(void* ptr, size_t size) {
    if (!ptr) return kmalloc(size);
    // Can't actually realloc — just alloc new and copy (stub)
    void* new_ptr = kmalloc(size);
    if (new_ptr) {
        uint8_t* dst = (uint8_t*)new_ptr;
        uint8_t* src = (uint8_t*)ptr;
        for (size_t i = 0; i < size; i++) dst[i] = src[i];
    }
    return new_ptr;
}

// ─── Process / stdlib ───────────────────────────────────────────

void abort(void) {
    // Halt the system
    for (;;) { asm volatile("hlt"); }
}

void exit(int status) {
    (void)status;
    abort();
}

int system(const char* command) {
    (void)command;
    return -1;
}

int atexit(void (*func)(void)) {
    (void)func;
    return 0;
}

// ─── Environment ────────────────────────────────────────────────

char* getenv(const char* name) {
    (void)name;
    return NULL;
}

// ─── Assert ─────────────────────────────────────────────────────

void __assert_fail(const char* assertion, const char* file,
                   unsigned int line, const char* function) {
    (void)assertion; (void)file; (void)line; (void)function;
    abort();
}
