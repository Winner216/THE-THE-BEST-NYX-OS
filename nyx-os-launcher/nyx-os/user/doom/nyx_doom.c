// nyx_doom.c — DOOM for Nyx OS: doomgeneric port to a ring-3 PE task.
//
// Compile (host gcc, -m32 -ffreestanding -nostdlib) and link as a
// static ELF at 0x401000; tools/elf2pe.py wraps that image into a real
// PE .exe (DOOM.EXE) that pe_exec loads at 0xC1000000.  This file is
// the WHOLE port: libc subset, doomgeneric platform backend (the 5
// DG_* functions), and the C entry point that runs the game loop.
//
// The shareware IWAD (doom1.wad, 4,196,020 bytes, id Software) is
// baked into the image via nyx_wad.h (generated: make doomwad) so the
// .exe is self-contained — no filesystem needed.

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

// ─────────────────────────────────────────────────────────────────
// Syscall interface (matches include/syscall.h numbers)
// ─────────────────────────────────────────────────────────────────
#define SYS_EXIT        0
#define SYS_YIELD       1
#define SYS_SLEEP       2
#define SYS_ALLOC_PAGE  6
#define SYS_WRITE       8

static int  nyx_sc(int n, int a, int b, int c) {
    int ret;
    asm volatile("int $0x80" : "=a"(ret)
                 : "a"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return ret;
}
static void nyx_exit(int code) { nyx_sc(SYS_EXIT, code, 0, 0); for(;;){} }
static void nyx_write(const char* s, int len) {
    nyx_sc(SYS_WRITE, (int)s, len, 0);
}
// Console helpers: everything funnels through nyx_write (SYS_WRITE → COM1).
static void nyx_putc(char c) { nyx_write(&c, 1); }
static void nyx_puts(const char* s) { int n = 0; while (s[n]) n++; if (n) nyx_write(s, n); }
// Nyx: set by G_BuildTiccmd while the jump key is held — the status
// hook samples z densely during a jump so E2Es can verify the arc.
int nyx_jump_held = 0;

// Game-mode syscalls (include/game.h)
#define SYS_GAMESTART   16
#define SYS_GAMEBLIT    17
#define SYS_GETKEY      18
#define SYS_GAMESTOP    19

static void* nyx_gamestart(const char* title) {
    return (void*)nyx_sc(SYS_GAMESTART, (int)title, 0, 0);
}
static void nyx_gameblit(const void* f)   { nyx_sc(SYS_GAMEBLIT, (int)f, 0, 0); }
static int  nyx_getkey(void)              { return nyx_sc(SYS_GETKEY, 0, 0, 0); }
static void nyx_gamestop(void)            { nyx_sc(SYS_GAMESTOP, 0, 0, 0); }

// ─────────────────────────────────────────────────────────────────
// Ring-3 zone allocator (SYS_ALLOC_PAGE pages are zeroed on first
// touch by fresh PMM frames).  DOOM's zones: main 4 MiB + sound 1 MiB
// + a static 640×400×4 screen buffer (1 MiB).
// ─────────────────────────────────────────────────────────────────
#define ZONE_MAX_PAGES 2560                    // 10 MiB of 4 KiB pages:
                    // DOOM's 6 MiB zone (AutoAllocMemory) + the 1 MiB
                    // user frame buffer + ~3 MiB malloc/misc headroom.
                    // Task region: 16 MiB total, image+stack take ~6.
static unsigned nyx_zone_base = 0;             // page-aligned VA
static unsigned nyx_zone_top  = 0;             // bump pointer

static void* nyx_zone_init(int pages) {
    if (pages > ZONE_MAX_PAGES) pages = ZONE_MAX_PAGES;
    unsigned p = (unsigned)nyx_sc(SYS_ALLOC_PAGE, pages, 0, 0);
    if (!p) return 0;
    nyx_zone_base = nyx_zone_top = p;
    return (void*)p;
}
static void* nyx_zone_alloc(unsigned bytes) {
    bytes = (bytes + 15u) & ~15u;
    if (nyx_zone_top + bytes > nyx_zone_base + ZONE_MAX_PAGES * 4096u)
        return 0;
    void* p = (void*)nyx_zone_top;
    nyx_zone_top += bytes;
    return p;
}

// ─────────────────────────────────────────────────────────────────
// Minimal libc for DOOM (freestanding)
// ─────────────────────────────────────────────────────────────────
void* memset(void* d, int c, size_t n) {
    unsigned char* p = (unsigned char*)d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}
void* memcpy(void* d, const void* s, size_t n) {
    unsigned char* a = (unsigned char*)d;
    const unsigned char* b = (const unsigned char*)s;
    if ((((uintptr_t)a ^ (uintptr_t)b) & 3u) == 0) {
        while (n >= 4u) { *(uint32_t*)a = *(const uint32_t*)b;
                          a += 4; b += 4; n -= 4; }
    }
    while (n--) *a++ = *b++;
    return d;
}
void* memmove(void* d, const void* s, size_t n) {
    unsigned char* a = (unsigned char*)d;
    const unsigned char* b = (const unsigned char*)s;
    if (a < b) return memcpy(d, s, n);
    a += n; b += n;
    while (n--) *--a = *--b;
    return d;
}
int memcmp(const void* a, const void* b, size_t n) {
    const unsigned char* x = (const unsigned char*)a;
    const unsigned char* y = (const unsigned char*)b;
    while (n--) { if (*x != *y) return *x - *y; x++; y++; }
    return 0;
}
size_t strlen(const char* s) { size_t n = 0; while (s[n]) n++; return n; }
char*  strcpy(char* d, const char* s) {
    char* r = d; while ((*d++ = *s++)) {} return r;
}
char*  strncpy(char* d, const char* s, size_t n) {
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}
char* strcat(char* d, const char* s) {
    char* r = d; while (*d) d++; while ((*d++ = *s++)) {} return r;
}
int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
int strncmp(const char* a, const char* b, size_t n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    return n ? (unsigned char)*a - (unsigned char)*b : 0;
}
int strcasecmp(const char* a, const char* b) {
    while (*a && *b) {
        int ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    {
        int ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        return ca - cb;
    }
}
int strncasecmp(const char* a, const char* b, size_t n) {
    while (n && *a && *b) {
        int ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return ca - cb;
        a++; b++; n--;
    }
    if (!n) return 0;
    {
        int ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        return ca - cb;
    }
}
char* strchr(const char* s, int c) {
    while (*s) { if (*s == (char)c) return (char*)s; s++; }
    return (c == 0) ? (char*)s : 0;
}
char* strrchr(const char* s, int c) {
    const char* last = 0;
    while (*s) { if (*s == (char)c) last = s; s++; }
    if (c == 0) return (char*)s;
    return (char*)last;
}
// strtol subset: decimal/hex, optional sign (DOOM parses numbers/args)
long strtol(const char* s, char** end, int base) {
    long v = 0; int neg = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;
    if ((base == 16 || base == 0) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2; base = 16;
    }
    if (base == 0) base = 10;
    for (;;) {
        int d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v * base + d; s++;
    }
    if (end) *end = (char*)s;
    return neg ? -v : v;
}
int atoi(const char* s) { return (int)strtol(s, 0, 10); }
static long nyx_rand_seed = 218471;
long atol(const char* s) { return strtol(s, 0, 10); }
void srand(unsigned seed) { nyx_rand_seed = seed; }
int  rand(void) {
    nyx_rand_seed = nyx_rand_seed * 1103515245L + 12345L;
    return (int)((nyx_rand_seed >> 16) & 0x7FFF);
}
int abs(int v) { return v < 0 ? -v : v; }
long labs(long v) { return v < 0 ? -v : v; }
static unsigned nyx_heap_ptr = 0;
static unsigned nyx_heap_end = 0;
void* malloc(size_t n) {
    if (!nyx_heap_end) {
        // Lazily reserve the rest of the zone for malloc.
        nyx_heap_end = nyx_zone_base + ZONE_MAX_PAGES * 4096u;
        nyx_heap_ptr = nyx_zone_top;
    }
    n = (n + 15u) & ~15u;
    if (n == 0) n = 16;
    if (nyx_heap_ptr + n > nyx_heap_end) return 0;
    void* p = (void*)nyx_heap_ptr;
    nyx_heap_ptr += n;
    return p;
}
void free(void* p) { (void)p; }                    // arena: no-op
void* calloc(size_t a, size_t b) {
    void* p = malloc(a * b);
    if (p) memset(p, 0, a * b);
    return p;
}
void* realloc(void* old, size_t n) {
    void* p = malloc(n);
    if (p && old) memcpy(p, old, n);   // conservative (heap headerless)
    return p;
}
char* strdup(const char* s) {
    size_t n = strlen(s) + 1;
    char* p = (char*)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}
char* strstr(const char* h, const char* n) {
    if (!*n) return (char*)h;
    for (; *h; h++) {
        const char* a = h; const char* b = n;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return (char*)h;
    }
    return 0;
}
int tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
int toupper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
int isprint(int c) { return c >= 32 && c < 127; }
int isspace(int c) { return c == ' ' || (c >= 9 && c <= 13); }
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isalpha(int c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int isxdigit(int c) {
    return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
int iscntrl(int c) { return (c >= 0 && c < 32) || c == 127; }
int ispunct(int c) { return isprint(c) && !isalnum(c); }
int isgraph(int c) { return c > 32 && c < 127; }

// qsort (insertion sort is fine for DOOM's small arrays)
void qsort(void* base, size_t n, size_t w,
           int (*cmp)(const void*, const void*)) {
    char* b = (char*)base;
    static char tmp[64];
    for (size_t i = 1; i < n; i++) {
        if (w > sizeof(tmp)) return;
        memcpy(tmp, b + i * w, w);
        size_t j = i;
        while (j > 0 && cmp(b + (j - 1) * w, tmp) > 0) {
            memcpy(b + j * w, b + (j - 1) * w, w);
            j--;
        }
        memcpy(b + j * w, tmp, w);
    }
}

// stdio: printf-family to COM1 (debug console) + a memory WAD "FILE".
//
// One formatting core with a byte sink covers console AND buffer use
// so the two paths can never drift apart again.  Supported conversions
// (measured against DOOM's actual format strings): %d %i %u %o %x %X
// %c %s %p %%, optional '-'/'0' flags, width, '.precision', and the
// l/ll/h length modifiers (ILP32: all reduce to int/unsigned).
typedef struct {
    char*  buf;        // NULL → console sink
    size_t cap;        // buffer capacity (buffer sink only)
    size_t len;        // bytes emitted so far
} nyx_sink_t;

static void sink_putc(nyx_sink_t* k, char c) {
    if (k->buf) {
        if (k->len + 1 < k->cap) k->buf[k->len] = c;   // always NUL-room
        k->len++;
    } else {
        nyx_write(&c, 1);
    }
}
static void sink_puts(nyx_sink_t* k, const char* s, size_t n) {
    while (n--) sink_putc(k, *s++);
}
// Pad `total` width around already-emitted content length `have`.
static void sink_pad(nyx_sink_t* k, int have, int width, char fc, int left) {
    for (int i = have; i < width; i++) {
        if (left) { /* %-flag: pad after content — handled by caller */ }
        else sink_putc(k, fc);
    }
}

static void nyx_emit(nyx_sink_t* k, const char* f, va_list ap) {
    for (; *f; f++) {
        if (*f != '%') { sink_putc(k, *f); continue; }
        f++;
        // flags
        int zero = 0, left = 0;
        for (;; f++) {
            if (*f == '0') zero = 1;
            else if (*f == '-') left = 1;
            else break;
        }
        // width
        int width = 0;
        while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        // precision
        int prec = -1;
        if (*f == '.') {
            f++; prec = 0;
            while (*f >= '0' && *f <= '9') prec = prec * 10 + (*f++ - '0');
        }
        // length modifiers (ILP32: l/ll/h/hh don't change arg size)
        while (*f == 'l' || *f == 'h') f++;
        char b[24];
        int have = 0;
        char tmp[24];
        switch (*f) {
            case 'd': case 'i': {
                int v = va_arg(ap, int);
                unsigned u = (v < 0) ? (unsigned)(-v) : (unsigned)v;
                have = 0;
                if (v < 0) tmp[have++] = '-';
                if (!u) tmp[have++] = '0';
                while (u) { tmp[have++] = '0' + u % 10; u /= 10; }
                // reverse digits (keep any '-' at front)
                for (int a = (v < 0) ? 1 : 0, z = have - 1; a < z; a++, z--) {
                    char t2 = tmp[a]; tmp[a] = tmp[z]; tmp[z] = t2;
                }
                if (prec >= 0) {
                    int digits = have - (v < 0 ? 1 : 0);
                    if (prec > digits && have < 23) {
                        // zero-extend: shift right by needed zeros
                        int add = prec - digits;
                        if (have + add > 23) add = 23 - have;
                        for (int j = have; j >= (v < 0 ? 1 : 0); j--)
                            tmp[j + add] = tmp[j];
                        for (int j = 0; j < add; j++)
                            tmp[(v < 0 ? 1 : 0) + j] = '0';
                        have += add;
                    }
                }
                break;
            }
            case 'u': {
                unsigned u = va_arg(ap, unsigned);
                have = 0;
                if (!u) tmp[have++] = '0';
                while (u) { tmp[have++] = '0' + u % 10; u /= 10; }
                for (int a = 0, z = have - 1; a < z; a++, z--) {
                    char t2 = tmp[a]; tmp[a] = tmp[z]; tmp[z] = t2;
                }
                break;
            }
            case 'o': {
                unsigned u = va_arg(ap, unsigned);
                have = 0;
                if (!u) tmp[have++] = '0';
                while (u) { tmp[have++] = '0' + (u & 7); u >>= 3; }
                for (int a = 0, z = have - 1; a < z; a++, z--) {
                    char t2 = tmp[a]; tmp[a] = tmp[z]; tmp[z] = t2;
                }
                break;
            }
            case 'x': case 'X': {
                unsigned u = va_arg(ap, unsigned);
                have = 0;
                if (!u) tmp[have++] = '0';
                while (u) { int d = u & 15;
                            tmp[have++] = d < 10 ? '0' + d : (*f == 'X' ? 'A' : 'a') + d - 10;
                            u >>= 4; }
                for (int a = 0, z = have - 1; a < z; a++, z--) {
                    char t2 = tmp[a]; tmp[a] = tmp[z]; tmp[z] = t2;
                }
                break;
            }
            case 'p': {
                unsigned long u = (unsigned long)va_arg(ap, void*);
                have = 0;
                if (!u) tmp[have++] = '0';
                while (u) { int d = u & 15;
                            tmp[have++] = d < 10 ? '0' + d : 'a' + d - 10;
                            u >>= 4; }
                // Digits are LSB-first: append "0x" then one full
                // reversal yields "0x<hex>" in the right order.
                tmp[have++] = 'x'; tmp[have++] = '0';
                for (int a = 0, z = have - 1; a < z; a++, z--) {
                    char t2 = tmp[a]; tmp[a] = tmp[z]; tmp[z] = t2;
                }
                break;
            }
            case 'c':
                tmp[have++] = (char)va_arg(ap, int);
                break;
            case 's': {
                const char* s = va_arg(ap, const char*);
                if (!s) s = "(null)";
                size_t sl = 0;
                while (s[sl]) sl++;
                if (prec >= 0 && (int)sl > prec) sl = (size_t)prec;
                if (!left)
                    for (int i = (int)sl; i < width; i++) sink_putc(k, ' ');
                sink_puts(k, s, sl);
                if (left)
                    for (int i = (int)sl; i < width; i++) sink_putc(k, ' ');
                continue;
            }
            case '%':
                tmp[have++] = '%';
                break;
            default:
                tmp[have++] = '%';
                if (*f) tmp[have++] = *f; else f--;
                break;
        }
        // numeric/char/%: width + zero-pad (pad before content, after sign)
        if (!left && width > have) {
            char fc = zero ? '0' : ' ';
            for (int i = have; i < width; i++) sink_putc(k, fc);
        }
        for (int i = 0; i < have; i++) sink_putc(k, tmp[i]);
        if (left && width > have)
            for (int i = have; i < width; i++) sink_putc(k, ' ');
    }
    if (k->buf && k->cap) k->buf[k->len < k->cap ? k->len : k->cap - 1] = 0;
}

static void nyx_vprintf(const char* f, va_list ap) {
    nyx_sink_t k = { .buf = 0, .cap = 0, .len = 0 };
    nyx_emit(&k, f, ap);
}
static int nyx_vsnprintf(char* out, size_t n, const char* f, va_list ap) {
    nyx_sink_t k = { .buf = out, .cap = n ? n : 1, .len = 0 };
    nyx_emit(&k, f, ap);
    return (int)k.len;
}
int printf(const char* f, ...) {
    va_list ap; va_start(ap, f); nyx_vprintf(f, ap); va_end(ap); return 0;
}
int sprintf(char* out, const char* f, ...) {
    va_list ap; va_start(ap, f);
    int r = nyx_vsnprintf(out, (size_t)-1, f, ap);
    va_end(ap); return r;
}
int snprintf(char* out, size_t n, const char* f, ...) {
    va_list ap; va_start(ap, f);
    int r = nyx_vsnprintf(out, n, f, ap);
    va_end(ap); return r;
}
int vsprintf(char* out, const char* f, va_list ap) {
    return nyx_vsnprintf(out, (size_t)-1, f, ap);
}
int vsnprintf(char* out, size_t n, const char* f, va_list ap) {
    return nyx_vsnprintf(out, n, f, ap);
}
int fprintf(void* f, const char* fmt, ...) {
    (void)f;
    va_list ap; va_start(ap, fmt); nyx_vprintf(fmt, ap); va_end(ap); return 0;
}
int vfprintf(void* f, const char* fmt, va_list ap) {
    (void)f; nyx_vprintf(fmt, ap); return 0;
}
int puts(const char* s) { nyx_puts(s); nyx_putc('\n'); return 0; }
int putchar(int c) { nyx_putc((char)c); return c; }
int getchar(void) { return -1; }
int fputs(const char* s, void* f) { (void)f; nyx_puts(s); return 0; }
int fflush(void* f) { (void)f; return 0; }
void perror(const char* s) { nyx_puts(s); nyx_puts(": error\n"); }

// errno stub
int* __errno_location(void) { static int e; return &e; }
int errno_val;
int* errno_ref(void) { return &errno_val; }

// ── stdio FILE on the baked-in WAD (read-only, memory-backed) ─────
// FILE itself comes from the shim <stdio.h> (base/size/pos fields);
// all other TUs include that header, so there must be only ONE FILE
// definition.  We just own the single live object.
#include <stdio.h>
static FILE nyx_wad_file;

FILE* fopen(const char* path, const char* mode) {
    (void)mode;
    // Only the WAD is openable: exact name or any .../doom1.wad
    const char* n = path;
    while (*n) {                       // basename
        if (*n == '/' || *n == '\\') { n++; continue; }
        const char* s = n;
        while (*s && *s != '/' && *s != '\\') s++;
        if ((s - n) == 9) {
            int ok = 1;
            const char* w = "doom1.wad";
            for (int i = 0; i < 9; i++)
                if (tolower((unsigned char)n[i]) != w[i]) { ok = 0; break; }
            if (ok) { nyx_wad_file.pos = 0; return &nyx_wad_file; }
        }
        n = s;
    }
    return 0;
}
size_t fread(void* buf, size_t sz, size_t cnt, FILE* f) {
    if (!f || f != &nyx_wad_file) return 0;
    size_t want = sz * cnt;
    if (f->pos + want > f->size) want = f->size - f->pos;
    memcpy(buf, f->base + f->pos, want);
    f->pos += want;
    return want / sz;
}
int fseek(FILE* f, long off, int whence) {
    if (!f || f != &nyx_wad_file) return -1;
    unsigned np = f->pos;
    if (whence == SEEK_SET) np = (unsigned)off;
    else if (whence == SEEK_CUR) np = f->pos + off;
    else np = f->size + off;
    if (np > f->size) np = f->size;
    f->pos = np;
    return 0;
}
long ftell(FILE* f) { return f ? (long)f->pos : -1; }
int fclose(FILE* f) { (void)f; return 0; }
int feof(FILE* f) { return (f && f->pos >= f->size) ? 1 : 0; }
int fgetc(FILE* f) {
    if (!f || f->pos >= f->size) return EOF;
    return f->base[f->pos++];
}
int ferror(FILE* f) { (void)f; return 0; }
int remove(const char* p) { (void)p; return -1; }
int rename(const char* a, const char* b) { (void)a; (void)b; return -1; }

// ── POSIX-ish stubs DOOM's fixed paths touch ─────────────────────
int  mkdir(const char* p, int m) { (void)p; (void)m; return -1; }
int  rmdir(const char* p) { (void)p; return -1; }
int  unlink(const char* p) { (void)p; return -1; }
int  access(const char* p, int m) { (void)p; (void)m; return 0; }
int  chdir(const char* p) { (void)p; return 0; }
char* getcwd(char* buf, size_t n) {
    if (buf && n) { buf[0] = '/'; buf[1] = 0; }
    return buf;
}
char* getenv(const char* name) {
    // DOOMWADDIR → "" means "current directory", which fopen maps
    // straight to the baked-in WAD.
    if (strcmp(name, "DOOMWADDIR") == 0) return "";
    return 0;
}
int  setenv(const char* a, const char* b, int o) { (void)a; (void)b; (void)o; return 0; }
void unsetenv(const char* a) { (void)a; }
int  putenv(const char* s) { (void)s; return 0; }
int  system(const char* c) { (void)c; return -1; }
void exit(int code) { nyx_gamestop(); nyx_exit(code); }
void abort(void) { nyx_write("ABORT\n", 6); nyx_exit(-1); }
void _exit(int c) { nyx_exit(c); }
void _Exit(int c) { nyx_exit(c); }
int  atexit(void (*f)(void)) { (void)f; return 0; }
long sysconf(int n) { (void)n; return 4096; }
unsigned alarm(unsigned s) { (void)s; return 0; }
int  sleep(int s) { nyx_sc(SYS_SLEEP, s * 50, 0, 0); return 0; }
int  usleep(int us) { nyx_sc(SYS_SLEEP, us / 20000 + 1, 0, 0); return 0; }
int  open(const char* p, int f, ...) { (void)p; (void)f; return -1; }
int  close(int fd) { (void)fd; return -1; }
int  read(int fd, void* b, size_t n) { (void)fd; (void)b; (void)n; return -1; }
int  write(int fd, const void* b, size_t n) {
    if (fd == 1 || fd == 2) { nyx_write((const char*)b, (int)n); return (int)n; }
    return -1;
}
int  getpid_nyx(void) { return nyx_sc(4, 0, 0, 0); }
int  getpid(void) { return getpid_nyx(); }
void* sbrk(long inc) {
    static unsigned brk;
    if (!brk) brk = nyx_zone_top;
    if (inc == 0) return (void*)brk;
    void* p = (void*)brk;
    brk += (unsigned)inc;
    return p;
}
int  fileno(FILE* f) { (void)f; return 1; }
int  isatty(int fd) { return fd == 1; }
long lseek(int fd, long o, int w) { (void)fd; (void)o; (void)w; return -1; }
int  stat(const char* p, void* s) { (void)p; (void)s; return -1; }
int  fstat(int fd, void* s) { (void)fd; (void)s; return -1; }
int  mkdir_nyx(const char* p, int m) { return mkdir(p, m); }

// fwrite (WAD is read-only; config writes are ignored)
size_t fwrite(const void* buf, size_t sz, size_t cnt, FILE* f) {
    (void)buf; (void)f; return 0;
}
int fputc(int c, FILE* f) { (void)f; return c; }
char* fgets(char* s, int n, FILE* f) {
    if (!f || f != &nyx_wad_file) return 0;
    int i = 0;
    while (i < n - 1 && f->pos < f->size) {
        char ch = (char)f->base[f->pos++];
        s[i++] = ch;
        if (ch == '\n') break;
    }
    if (!i) return 0;
    s[i] = 0;
    return s;
}
// (vprintf/vsnprintf/vsprintf live with the console formatter above.)

// strtok (M_ParseOption uses it for comma-separated lists)
static char* s_strtok_pos = 0;
char* strtok(char* s, const char* delim) {
    if (s) s_strtok_pos = s;
    if (!s_strtok_pos) return 0;
    char* p = s_strtok_pos;
    while (*p && strchr(delim, *p)) p++;          // skip leading
    if (!*p) { s_strtok_pos = 0; return 0; }
    char* tok = p;
    while (*p && !strchr(delim, *p)) p++;         // token body
    if (*p) { *p = 0; s_strtok_pos = p + 1; }
    else      s_strtok_pos = 0;
    return tok;
}

// bsearch (not used on hot paths; linear scan suffices)
void* bsearch(const void* key, const void* base, size_t n, size_t w,
              int (*cmp)(const void*, const void*)) {
    const unsigned char* p = (const unsigned char*)base;
    for (size_t i = 0; i < n; i++, p += w)
        if (cmp(key, p) == 0) return (void*)p;
    return 0;
}

// sscanf — M_ParseInt/M_ParseDouble style hex/octal/int parsing only.
// Returns the number of converted fields.
static int sc_int(const char** p, int* out, int base) {
    const char* s = *p;
    while (*s == ' ' || *s == '\t') s++;
    int neg = 0;
    if (*s == '+' || *s == '-') neg = (*s == '-'), s++;
    if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    else if (base == 0) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
        else if (s[0] == '0') { base = 8; s++; }
        else base = 10;
    }
    int v = 0, any = 0;
    for (; *s; s++) {
        int d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v * base + d; any = 1;
    }
    if (!any) return 0;
    *out = neg ? -v : v;
    *p = s;
    return 1;
}
int sscanf(const char* s, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = 0;
    for (; *fmt; fmt++) {
        if (*fmt != '%') continue;
        fmt++;
        if (*fmt == '%') continue;
        if (*fmt == '*') continue;                    // suppressed: unsupported
        if (*fmt == 'x' || *fmt == 'X') {
            int* out = va_arg(ap, int*);
            if (!sc_int(&s, out, 16)) break;
            n++;
        } else if (*fmt == 'd' || *fmt == 'i') {
            int* out = va_arg(ap, int*);
            if (!sc_int(&s, out, (*fmt == 'i') ? 0 : 10)) break;
            n++;
        } else if (*fmt == 'o') {
            int* out = va_arg(ap, int*);
            if (!sc_int(&s, out, 8)) break;
            n++;
        } else if (*fmt == 's') {
            char* out = va_arg(ap, char*);
            while (*s == ' ' || *s == '\t') s++;
            int k = 0;
            while (*s && *s != ' ' && *s != '\t') out[k++] = *s++;
            out[k] = 0;
            n++;
        } else {
            break;                                    // unsupported conversion
        }
    }
    va_end(ap);
    return n;
}
double atof(const char* s) {
    while (*s == ' ' || *s == '\t') s++;
    int neg = 0;
    if (*s == '+' || *s == '-') neg = (*s == '-'), s++;
    double v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    if (*s == '.') {
        s++;
        double frac = 0, div = 1;
        while (*s >= '0' && *s <= '9') { frac = frac * 10 + (*s++ - '0'); div *= 10; }
        v += frac / div;
    }
    return neg ? -v : v;
}
double strtod(const char* s, char** end) {
    const char* start = s;
    double v = atof(s);
    if (end) {
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '+' || *s == '-') s++;
        while ((*s >= '0' && *s <= '9') || *s == '.') s++;
        *end = (char*)(s > start ? s : start);
    }
    return v;
}

// Joystick: no hardware — the game still boots and plays on keys.
void I_InitJoystick(void) {}
void I_ShutdownJoystick(void) {}
void I_UpdateJoystick(void) {}
void I_BindJoystickVariables(void) {}

// time: DOOM only needs wall ms (DG_GetTicksMs) and a few time()
// calls for random seeds; report seconds since boot.
extern uint32_t nyx_ticks(void);   // below (via SYS wrapper-free timer)
typedef long time_t;
time_t time(time_t* t) {
    long s = (long)(nyx_sc(2, 0, 0, 0) ? 0 : 0);   // not used; stub
    // 4 x 250 syscalls/sec is wasteful; use tick math instead:
    static long fake = 1000000;                    // fixed epoch
    s = fake;
    if (t) *t = s;
    return s;
}
struct tm { int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year; };
struct tm* localtime(const time_t* t) {
    static struct tm tm0;
    (void)t; return &tm0;
}
struct tm* gmtime(const time_t* t) { return localtime(t); }
size_t strftime(char* s, size_t n, const char* f, const void* tm) {
    (void)f; (void)tm;
    if (n) s[0] = 0;
    return 0;
}

// math (soft, gcc libgcc covers most; DOOM needs few)
double atan2(double y, double x) { (void)y; (void)x; return 0; }
double sqrt(double x) {
    if (x <= 0) return 0;
    double r = x, last = 0;
    for (int i = 0; i < 32 && r != last; i++) { last = r; r = 0.5 * (r + x / r); }
    return r;
}
double pow(double b, double e) { (void)b; (void)e; return 1; }
double floor(double x) { return (double)(long long)x; }
double ceil(double x)  { double f = floor(x); return f < x ? f + 1 : f; }
double fabs(double x)  { return x < 0 ? -x : x; }
double sin(double x)   { (void)x; return 0; }
double cos(double x)   { (void)x; return 1; }
double tan(double x)   { (void)x; return 0; }
double log(double x)   { (void)x; return 0; }
double exp(double x)   { (void)x; return 1; }
double fmod(double a, double b) { return a - b * floor(a / b); }

// assert stub
void __assert_fail(const char* a, const char* f, unsigned l, const char* fn) {
    (void)a; (void)f; (void)l; (void)fn;
    nyx_write("ASSERT\n", 7);
    nyx_exit(-2);
}

// ─────────────────────────────────────────────────────────────────
// Baked-in shareware WAD (generated byte array, tools/make_doomwad.py)
// ─────────────────────────────────────────────────────────────────
#include "nyx_wad.h"

// ─────────────────────────────────────────────────────────────────
// doomgeneric backend
// ─────────────────────────────────────────────────────────────────
#include "doomgeneric.h"

// PS/2 set-1 scancodes → DOOM keys.  Must return the exact doomkeys.h
// values: M_Responder compares against key_menu_* (m_controls.c init
// from doomkeys.h) and G_BuildTiccmd against key_up/key_fire/…, so any
// code that isn't a doomkeys.h value is silently ignored — which dead-
// ended the ESC menu (arrows/Enter matched nothing; ESC only opened the
// menu because the attract demo pops it on ANY keydown).
#include "doomkeys.h"
static unsigned char sc_to_doom(unsigned code, int e0) {
    if (e0) {
        switch (code) {
            case 0x48: return KEY_UPARROW;
            case 0x50: return KEY_DOWNARROW;
            case 0x4B: return KEY_LEFTARROW;
            case 0x4D: return KEY_RIGHTARROW;
            case 0x1D: return KEY_FIRE;      // right-Ctrl → fire
            case 0x38: return KEY_RALT;      // right-Alt → strafe
            case 0x1C: return KEY_ENTER;     // keypad Enter
            case 0x35: return '/';           // keypad /
            default:   return 0;
        }
    }
    switch (code) {
        case 0x01: return KEY_ESCAPE;
        case 0x1C: return KEY_ENTER;
        case 0x0F: return KEY_TAB;
        case 0x0E: return KEY_BACKSPACE;
        case 0x48: return KEY_UPARROW;
        case 0x50: return KEY_DOWNARROW;
        case 0x4B: return KEY_LEFTARROW;
        case 0x4D: return KEY_RIGHTARROW;
        case 0x1D: return KEY_FIRE;          // left-Ctrl → fire
        case 0x39: return ' ';               // Space → jump (E = use now)
        case 0x2A: case 0x36: return KEY_RSHIFT;
        case 0x38: return KEY_RALT;          // left-Alt → strafe
        case 0x0C: return KEY_MINUS;
        case 0x0D: return KEY_EQUALS;
        case 0x0B: return '0';
        case 0x02: return '1'; case 0x03: return '2'; case 0x04: return '3';
        case 0x05: return '4'; case 0x06: return '5'; case 0x07: return '6';
        case 0x08: return '7'; case 0x09: return '8'; case 0x0A: return '9';
        case 0x1E: return 'a'; case 0x30: return 'b'; case 0x2E: return 'c';
        case 0x20: return 'd'; case 0x12: return 'e'; case 0x21: return 'f';
        case 0x22: return 'g'; case 0x23: return 'h'; case 0x17: return 'i';
        case 0x24: return 'j'; case 0x25: return 'k'; case 0x26: return 'l';
        case 0x32: return 'm'; case 0x31: return 'n'; case 0x18: return 'o';
        case 0x19: return 'p'; case 0x10: return 'q'; case 0x13: return 'r';
        case 0x1F: return 's'; case 0x14: return 't'; case 0x16: return 'u';
        case 0x2F: return 'v'; case 0x11: return 'w'; case 0x2D: return 'x';
        case 0x15: return 'y'; case 0x2C: return 'z';
        case 0x3B: return KEY_F1;
        case 0x3C: return KEY_F2;
        case 0x3D: return KEY_F3;
        case 0x3E: return KEY_F4;
        case 0x3F: return KEY_F5;
        case 0x40: return KEY_F6;
        case 0x41: return KEY_F7;
        case 0x42: return KEY_F8;
        case 0x43: return KEY_F9;
        case 0x44: return KEY_F10;
        case 0x57: return KEY_F11;
        case 0x58: return KEY_F12;
        case 0x33: return ','; case 0x34: return '.'; case 0x35: return '/';
        case 0x27: return ';'; case 0x28: return '\''; case 0x2B: return '\\';
        case 0x1A: return '['; case 0x1B: return ']'; case 0x29: return '`';
        default:   return 0;
    }
}

#define KEYQUEUE_SIZE 64
static unsigned short s_keyq[KEYQUEUE_SIZE];
static unsigned s_keyq_r = 0, s_keyq_w = 0;

static void keyq_add(int pressed, unsigned code, int e0) {
    // Strip the set-1 break bit: the kernel keeps it inside the low byte
    // (break = 0x80|make), and unmapped break codes would return 0 here,
    // dropping every key release (keys stuck down forever).
    unsigned char dk = sc_to_doom(code & 0x7F, e0);
    if (!dk) return;
    s_keyq[s_keyq_w] = (unsigned short)((pressed << 8) | dk);
    s_keyq_w = (s_keyq_w + 1) % KEYQUEUE_SIZE;
}

void DG_Init(void) {
    // Canvas comes from the kernel; DG_ScreenBuffer (allocated by
    // doomgeneric_Create) is what DOOM draws into — we blit BGRA.
    nyx_write("[DOOM] DG_Init\n", 15);
}

// RGBA (doomgeneric) → BGRA (Nyx VESA) + blit to the kernel canvas.
// User-side frame buffer (in the task's private zone): DOOM converts
// into this, then SYS_GAMEBLIT copies it into the kernel canvas.
// Ring 3 must NEVER write the kernel-owned canvas directly.
static uint32_t* g_user_frame = 0;

void DG_DrawFrame(void) {
    if (!g_user_frame) return;
    const uint32_t* src = (const uint32_t*)DG_ScreenBuffer;
    int n = DOOMGENERIC_RESX * DOOMGENERIC_RESY;
    // doomgeneric's rgba8888 mode packs 0x00RRGGBB (little-endian mem:
    // B,G,R,0) and Nyx's canvas is BGRA little-endian (B,G,R,A) — the
    // byte order already matches, so just supply the alpha byte.
    for (int i = 0; i < n; i++)
        g_user_frame[i] = src[i] | 0xFF000000u;
    nyx_gameblit(g_user_frame);
}

// Nyx lag fix: SYS_SLEEP takes KERNEL TICKS (50 Hz), not ms.  The old
// 'ms / 2' treated ms as ticks, so DOOM's I_Sleep(1) tic-sync call slept
// a whole 20 ms tick.  Convert ms→ticks at 50 ticks/s, rounding up.
#define NYX_PIT_HZ 50
void DG_SleepMs(uint32_t ms) {
    int ticks = (int)(((unsigned long long)ms * NYX_PIT_HZ + 999u) / 1000u);
    nyx_sc(SYS_SLEEP, ticks ? ticks : 1, 0, 0);
}

static uint64_t nyx_tsc_ms_base = 0;
uint64_t g_tsc_per_ms = 2000000;   // calibrated in DG_Init

static uint64_t rdtsc64(void) {
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static int s_tick_dbg = 0;
// Mouse accumulators between tics (drained per-tic by I_GetEvent via
// the externs in i_input.c).  Declared before DG_GetTicksMs because
// its 2 s trace references the in-game marker.
int s_mdx = 0, s_mdy = 0;
unsigned s_mbtn = 0;
int s_have_mouse_state = 0;          // DG_GetMouse ran since last post

// ── Nyx E2E status hook ──────────────────────────────────────────
// Every ~2 s of real gameplay, print map, health, armor, and the
// live/total monster counts (MF_COUNTKILL thinkers).  Lets tests
// prove monsters actually exist on the map and observe combat.
#include "doomstat.h"
#include "p_local.h"
#include "p_mobj.h"
static int s_status_t;
static int st_put(char* b, int n, int v) {
    if (v < 0) { b[n++] = '-'; v = -v; }
    char d[12]; int i = 0;
    if (!v) d[i++] = '0';
    while (v) { d[i++] = '0' + v % 10; v /= 10; }
    while (i) b[n++] = d[--i];
    return n;
}
static void nyx_status_tic(uint32_t t) {
    // Dense (250 ms) sampling while the jump key is held — a full arc
    // is ~0.5 s, so E2E jump verification needs the fast cadence.
    // Otherwise every 2 s, to stay quiet on COM1.
    uint32_t period = nyx_jump_held ? 250 : 2000;
    if (t - s_status_t < period) return;
    s_status_t = t;
    if (gamestate != GS_LEVEL) return;
    player_t* pl = &players[consoleplayer];
    if (!pl->mo) return;
    int alive = 0, total = 0;
    thinker_t* th;
    for (th = thinkercap.next; th != &thinkercap; th = th->next) {
        if (th->function.acp1 != (actionf_p1)P_MobjThinker) continue;
        mobj_t* mo = (mobj_t*)th;
        if (!(mo->flags & MF_COUNTKILL)) continue;
        total++;
        if (mo->health > 0) alive++;
    }
    char b[128]; int n = 0;
    const char* pre = "[DOOM] ST E";
    for (const char* p = pre; *p; p++) b[n++] = *p;
    n = st_put(b, n, gameepisode);
    b[n++] = 'M';
    n = st_put(b, n, gamemap);
    const char* mid = " hp=";
    for (const char* p = mid; *p; p++) b[n++] = *p;
    n = st_put(b, n, pl->health);
    const char* mid2 = " arm=";
    for (const char* p = mid2; *p; p++) b[n++] = *p;
    n = st_put(b, n, pl->armorpoints);
    const char* mid3 = " mon=";
    for (const char* p = mid3; *p; p++) b[n++] = *p;
    n = st_put(b, n, alive);
    b[n++] = '/';
    n = st_put(b, n, totalkills);
    // Player z / vertical momentum / floor — sampled densely while
    // the jump key is held (see nyx_jump_held), so E2Es can verify
    // the jump arc directly from the trace.
    const char* zdbg = " z=";
    for (const char* p = zdbg; *p; p++) b[n++] = *p;
    n = st_put(b, n, pl->mo->z >> 16);
    const char* mzdbg = " mz=";
    for (const char* p = mzdbg; *p; p++) b[n++] = *p;
    n = st_put(b, n, pl->mo->momz >> 16);
    const char* fzdbg = " fz=";
    for (const char* p = fzdbg; *p; p++) b[n++] = *p;
    n = st_put(b, n, pl->mo->floorz >> 16);
    const char* mid4 = " pk=";
    for (const char* p = mid4; *p; p++) b[n++] = *p;
    n = st_put(b, n, pl->killcount);
    b[n++] = '\n';
    nyx_write(b, n);
}

uint32_t DG_GetTicksMs(void) {
    // TSC-based ms clock (QEMU/host TSC Hz measured once at startup
    // via the PIT: see nyx_calibrate_tsc).
    uint32_t t = (uint32_t)((rdtsc64() - nyx_tsc_ms_base) / g_tsc_per_ms);
    // E2E probe: every ~2 s report the game clock so a stalled tick
    // (SYS_SLEEP misbehaving under the game's poll loop) is visible.
    if (t - s_tick_dbg >= 2000) {
        s_tick_dbg = t;
        nyx_write("[DOOM] tick ", 12);
        char b[12]; int i = 0; unsigned v = t;
        if (!v) b[i++] = '0';
        while (v) { b[i++] = '0' + v % 10; v /= 10; }
        while (i--) nyx_write(&b[i], 1);
        // E2E hook: IGL = in real gameplay (not title/attract-demo).
        // (usergame comes from doomstat.h, included above.)
        nyx_write(usergame ? " IGL" : "", 4);
        nyx_write("\n", 1);
        nyx_status_tic(t);
    }
    return t;
}

int DG_GetKey(int* pressed, unsigned char* key) {
    if (s_keyq_r == s_keyq_w) return 0;
    unsigned short d = s_keyq[s_keyq_r];
    s_keyq_r = (s_keyq_r + 1) % KEYQUEUE_SIZE;
    *pressed = d >> 8;
    *key = (unsigned char)(d & 0xFF);
    return 1;
}

// ─────────────────────────────────────────────────────────────────
// Mouse: the kernel accumulates raw PS/2 deltas + button level while
// game mode is active (cpu/game.c); we drain once per tic and hand
// DOOM one aggregated ev_mouse event.
//
// Why per-tic aggregation: DOOM's G_Responder ASSIGNS from ev_mouse
// (mousex = ev->data2 * sens / 10), it doesn't accumulate — posting
// per-packet events would drop all but the last packet each tic.
// Aggregating in the user side (the only place that knows tic
// boundaries) preserves every packet's delta.  Buttons are level
// state converted to a bitfield matching ev_mouse data1.
// Y: the kernel already normalizes to screen space (down = +),
// which matches DOOM's data3 = forward/back convention.
// ─────────────────────────────────────────────────────────────────
#define SYS_GETMOUSE 25
static int nyx_getmouse(int* dx, int* dy) {
    int ret;
    asm volatile("int $0x80" : "=a"(ret)
                 : "a"((unsigned)SYS_GETMOUSE), "b"((unsigned)dx),
                   "c"((unsigned)dy), "d"(0u) : "memory");
    return ret;
}

void DG_GetMouse(int* mx, int* my, unsigned* buttons) {
    int dx = 0, dy = 0;
    int b = nyx_getmouse(&dx, &dy);
    if (b < 0) { dx = dy = 0; b = 0; }   // not in game mode: idle
    *mx = dx; *my = dy; *buttons = (unsigned)b;
}

void DG_SetWindowTitle(const char* title) { (void)title; }

// ─────────────────────────────────────────────────────────────────
// Sound: DG_sound_module — DOOM's DS* PCM sfx through the kernel's
// game mixer (SYS_SNDLOAD 20..24 → cpu/gamesnd.c → AC'97 DMA).
//
// Flow: S_Init → I_PrecacheSounds → CacheSounds walks S_sfx, reads
// every DS lump out of the baked-in WAD (8-bit unsigned mono, header
// = u16 format, u16 rate, u32 len), and uploads the PCM via
// SYS_SNDLOAD (the kernel copies it into its own heap — user pages
// are not readable from the IRQ-context mixer).  S_StartSound then
// triggers a voice via SYS_SNDPLAY(handle, vol, sep, chan); the
// kernel mixes all voices into the 48 kHz stereo AC'97 stream.
//
// Music: DOOM's MUS lumps would need an OPL2 emulator to synth; the
// DG_music_module below is an honest no-op so the game runs without
// music rather than crashing.
// ─────────────────────────────────────────────────────────────────
#include "i_sound.h"
#include "z_zone.h"
#include "w_wad.h"

#define SYS_SNDLOAD    20
#define SYS_SNDPLAY    21
#define SYS_SNDSTOP    22
#define SYS_SNDPARAMS  23
#define SYS_SNDISPLAY  24

static int nyx_sndload(const void* p, int len, int rate) {
    return nyx_sc(SYS_SNDLOAD, (int)p, len, rate);
}
static int nyx_sndplay(int h, int vol, int sep, int ch) {
    return nyx_sc(SYS_SNDPLAY, h, vol, (ch << 16) | (sep & 0xFFFF));
}
static int nyx_sndstop(int ch) { return nyx_sc(SYS_SNDSTOP, ch, 0, 0); }
static int nyx_sndparams(int ch, int vol, int sep) {
    return nyx_sc(SYS_SNDPARAMS, ch, vol, sep & 0xFFFF);
}
static int nyx_sndisplay(int ch) { return nyx_sc(SYS_SNDISPLAY, ch, 0, 0); }

// Config vars chocolate-doom registers under FEATURE_SOUND; unused by
// the kernel mixer but must exist for the link.
int   use_libsamplerate = 0;
float libsamplerate_scale = 0.8f;

static int dg_sfx_prefix = 1;
static int dg_sfx_plays  = 0;

// Per-sfx kernel handle cache.  Linked sfx (sfxinfo->link) share the
// same lump, so the cache key is the LINK pointer for both.
#define DG_SFX_CACHE 96
static struct { const sfxinfo_t* info; int handle; } dg_sfx_cache[DG_SFX_CACHE];
static int dg_sfx_count = 0;

static const sfxinfo_t* dg_sfx_key(const sfxinfo_t* info) {
    return info->link ? info->link : info;
}

// Build the 8-char WAD lump name: "DS" + sfx name (doom1.wad uses
// DS-prefixed lumps).  Returns the lump index or -1.
static int dg_sfx_lump(const sfxinfo_t* info) {
    char name[9];
    int i = 0;
    if (dg_sfx_prefix) { name[i++] = 'D'; name[i++] = 'S'; }
    const char* n = info->name;
    while (n && *n && i < 8) name[i++] = *n++;
    name[i] = 0;
    return W_CheckNumForName(name);
}

// ── Direct WAD directory access ─────────────────────────────────
// The IWAD is baked into the image, so sfx lumps are read straight
// from nyx_wad_bytes — no Z_Malloc, no stdio, no W_ReadLump.  (That
// chain corrupted memory in ring 3 and killed the task mid-load; a
// lump pointer into .rodata has neither problem and needs no setup.)
static unsigned dg_wad_numlumps(void) {
    return (unsigned)nyx_wad_bytes[4] | ((unsigned)nyx_wad_bytes[5] << 8)
         | ((unsigned)nyx_wad_bytes[6] << 16) | ((unsigned)nyx_wad_bytes[7] << 24);
}
static unsigned dg_wad_infotable(void) {
    return (unsigned)nyx_wad_bytes[8] | ((unsigned)nyx_wad_bytes[9] << 8)
         | ((unsigned)nyx_wad_bytes[10] << 16) | ((unsigned)nyx_wad_bytes[11] << 24);
}

// WAD lump names are 8 bytes, NUL-padded; case-insensitive compare.
static int dg_wad_name_eq(const uint8_t* lump8, const char* name) {
    for (int j = 0; j < 8; j++) {
        char a = (char)lump8[j];
        if (a == 0) return name[j] == 0;
        char b = name[j];
        if (b == 0) return 1;              // name ended; lump padding ok
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return name[8] == 0;
}

// Find a lump by name; returns the directory index or -1.
static int dg_wad_find(const char* name) {
    unsigned num   = dg_wad_numlumps();
    unsigned info  = dg_wad_infotable();
    unsigned wsize = sizeof(nyx_wad_bytes);
    if (num == 0 || num > 65536) return -1;
    if ((uint64_t)info + (uint64_t)num * 16 > wsize) return -1;
    for (unsigned i = 0; i < num; i++) {
        const uint8_t* e = nyx_wad_bytes + info + i * 16;
        if (dg_wad_name_eq(e + 8, name)) return (int)i;
    }
    return -1;
}

// Pointer to a lump's bytes inside the image (len out); 0 on range error.
static const uint8_t* dg_wad_lump(int lump, int* len) {
    unsigned num   = dg_wad_numlumps();
    unsigned info  = dg_wad_infotable();
    unsigned wsize = sizeof(nyx_wad_bytes);
    if (lump < 0 || (unsigned)lump >= num) return 0;
    const uint8_t* e = nyx_wad_bytes + info + (unsigned)lump * 16;
    unsigned pos = (unsigned)e[0] | ((unsigned)e[1] << 8)
                 | ((unsigned)e[2] << 16) | ((unsigned)e[3] << 24);
    unsigned sz  = (unsigned)e[4] | ((unsigned)e[5] << 8)
                 | ((unsigned)e[6] << 16) | ((unsigned)e[7] << 24);
    if (pos > wsize || sz > wsize - pos) return 0;
    *len = (int)sz;
    return nyx_wad_bytes + pos;
}

// Resolve (and lazily upload) the kernel handle for one sfx.
static int dg_sfx_handle(const sfxinfo_t* info) {
    const sfxinfo_t* key = dg_sfx_key(info);
    for (int i = 0; i < dg_sfx_count; i++)
        if (dg_sfx_cache[i].info == key) return dg_sfx_cache[i].handle;
    if (dg_sfx_count >= DG_SFX_CACHE) return -1;

    char name[9];
    int ni = 0;
    if (dg_sfx_prefix) { name[ni++] = 'D'; name[ni++] = 'S'; }
    const char* n = key->name;
    while (n && *n && ni < 8) name[ni++] = *n++;
    name[ni] = 0;

    int lump = dg_wad_find(name);
    if (lump < 0) return -1;
    int len = 0;
    const uint8_t* raw = dg_wad_lump(lump, &len);
    if (!raw || len < 12) return -1;

    // DSP header: u16 format (=3), u16 sample rate, u32 sample count.
    int rate = raw[2] | (raw[3] << 8);
    if (rate < 4000 || rate > 48000) rate = 11025;   // header paranoia
    int handle = nyx_sndload(raw + 8, len - 8, rate);
    // Quiet progress: one report per 16 resolved sfx.
    if (dg_sfx_count % 16 == 0) {
        nyx_write("[DOOM] snd: n=", 13);
        char b[12]; int j = 0; unsigned v = (unsigned)dg_sfx_count;
        if (!v) b[j++] = '0';
        while (v) { b[j++] = '0' + v % 10; v /= 10; }
        while (j--) nyx_write(&b[j], 1);
        nyx_write("\n", 1);
    }
    if (handle < 0 && handle != -2) {
        // Upload failed (cache full?) — keep the lump, don't retry.
        handle = -1;
    }
    dg_sfx_cache[dg_sfx_count].info   = key;
    dg_sfx_cache[dg_sfx_count].handle = handle;
    dg_sfx_count++;
    return handle;
}

static boolean dg_sfx_Init(boolean use_sfx_prefix) {
    dg_sfx_prefix = use_sfx_prefix;
    dg_sfx_count  = 0;
    dg_sfx_plays  = 0;
    // Probe the kernel: -2 means no AC'97 → sound module unavailable
    // (DOOM runs silent, same as before this feature existed).
    if (nyx_sndload(0, 0, 0) == -2) return false;
    nyx_write("[DOOM] snd: kernel mixer present\n", 33);
    return true;
}

static void dg_sfx_Shutdown(void) {
    // Kernel frees our cache + voices at task exit (gamesnd_task_exit).
}

static int dg_sfx_GetSfxLumpNum(sfxinfo_t* sfxinfo) {
    int lump = dg_sfx_lump(dg_sfx_key(sfxinfo));
    return lump;
}

static void dg_sfx_Update(void) {
    // E2E heartbeat: prove sfx are actually triggering.
    static int last_report = 0;
    if (dg_sfx_plays == 1 || dg_sfx_plays - last_report >= 25) {
        last_report = dg_sfx_plays;
        nyx_write("[DOOM] sfx n=", 13);
        char b[12]; int i = 0; unsigned v = (unsigned)dg_sfx_plays;
        if (!v) b[i++] = '0';
        while (v) { b[i++] = '0' + v % 10; v /= 10; }
        while (i--) nyx_write(&b[i], 1);
        nyx_write("\n", 1);
    }
}

static void dg_sfx_UpdateSoundParams(int channel, int vol, int sep) {
    nyx_sndparams(channel, vol, sep);
}

static int dg_sfx_StartSound(sfxinfo_t* sfxinfo, int channel,
                             int vol, int sep) {
    int h = dg_sfx_handle(sfxinfo);
    if (h < 0) return -1;
    if (nyx_sndplay(h, vol, sep, channel) < 0) return -1;
    dg_sfx_plays++;
    return channel;
}

static void dg_sfx_StopSound(int channel) { nyx_sndstop(channel); }

static boolean dg_sfx_SoundIsPlaying(int channel) {
    return nyx_sndisplay(channel) ? true : false;
}

static void dg_sfx_CacheSounds(sfxinfo_t* sounds, int num_sounds) {
    int loaded = 0;
    for (int i = 1; i < num_sounds; i++) {
        if (!sounds[i].name[0]) continue;
        if (dg_sfx_handle(&sounds[i]) >= 0) loaded++;
    }
    nyx_write("[DOOM] snd: cached ", 19);
    char b[12]; int i = 0; unsigned v = (unsigned)loaded;
    if (!v) b[i++] = '0';
    while (v) { b[i++] = '0' + v % 10; v /= 10; }
    while (i--) nyx_write(&b[i], 1);
    nyx_write(" sfx\n", 5);
}

static snddevice_t dg_sfx_devices[] = {
    SNDDEVICE_SB,
    SNDDEVICE_PCSPEAKER,
    SNDDEVICE_ADLIB,
    SNDDEVICE_AWE32,
};

sound_module_t DG_sound_module = {
    dg_sfx_devices,
    sizeof(dg_sfx_devices) / sizeof(dg_sfx_devices[0]),
    dg_sfx_Init,
    dg_sfx_Shutdown,
    dg_sfx_GetSfxLumpNum,
    dg_sfx_Update,
    dg_sfx_UpdateSoundParams,
    dg_sfx_StartSound,
    dg_sfx_StopSound,
    dg_sfx_SoundIsPlaying,
    dg_sfx_CacheSounds,
};

// ─── Music: honest no-op (MUS→OPL synth is a future project) ────
static snddevice_t dg_music_devices[] = {
    SNDDEVICE_SB,
    SNDDEVICE_ADLIB,
    SNDDEVICE_GENMIDI,
};
static boolean     dg_mus_Init(void)                    { return true; }
static void        dg_mus_Shutdown(void)                { }
static void        dg_mus_SetMusicVolume(int v)         { (void)v; }
static void        dg_mus_PauseMusic(void)              { }
static void        dg_mus_ResumeMusic(void)             { }
static void*       dg_mus_RegisterSong(void* d, int l)  { (void)d; (void)l; return (void*)1; }
static void        dg_mus_UnRegisterSong(void* h)       { (void)h; }
static void        dg_mus_PlaySong(void* h, boolean lp) { (void)h; (void)lp; }
static void        dg_mus_StopSong(void)                { }
static boolean     dg_mus_MusicIsPlaying(void)          { return false; }
static void        dg_mus_Poll(void)                    { }

music_module_t DG_music_module = {
    dg_music_devices,
    sizeof(dg_music_devices) / sizeof(dg_music_devices[0]),
    dg_mus_Init,
    dg_mus_Shutdown,
    dg_mus_SetMusicVolume,
    dg_mus_PauseMusic,
    dg_mus_ResumeMusic,
    dg_mus_RegisterSong,
    dg_mus_UnRegisterSong,
    dg_mus_PlaySong,
    dg_mus_StopSong,
    dg_mus_MusicIsPlaying,
    dg_mus_Poll,
};

// ─────────────────────────────────────────────────────────────────
// Entry: the PE entry point runs DOOM directly (no C runtime).
// ─────────────────────────────────────────────────────────────────
uint8_t* g_nyx_canvas = 0;

static void nyx_pump_keys(void) {
    for (;;) {
        int ev = nyx_getkey();
        if (ev < 0) return;
        keyq_add((ev & 0x200) ? 0 : 1, ev & 0xFF, (ev & 0x100) ? 1 : 0);
    }
}

// Mouse accumulators between tics — D_PostEvent isn't callable here
// (the event queue is drained later, inside the tic), so DG_GetMouse
// state is stashed and posted once per I_GetEvent call (== per tic).

// Drain the kernel's raw deltas.  Called from nyx_main's pump loop
// (~100+ Hz, between tics) — finer than the 35 Hz tic rate, so no
// packet is lost between tics.
static void nyx_pump_mouse(void) {
    int dx, dy;
    unsigned btn;
    DG_GetMouse(&dx, &dy, &btn);
    s_mdx += dx; s_mdy += dy;
    s_mbtn = btn;
    if (dx || dy || btn) s_have_mouse_state = 1;
}

static void nyx_calibrate_tsc(void) {
    // Measure TSC frequency using SYS_SLEEP: sleep 10 ticks (200 ms
    // at the 50 Hz PIT), divide the TSC delta.
    uint64_t t0 = rdtsc64();
    nyx_sc(SYS_SLEEP, 10, 0, 0);
    uint64_t dt = rdtsc64() - t0;
    if (dt > 1000000) g_tsc_per_ms = dt / 200;
}

void nyx_main(void) {
    nyx_write("\n[DOOM] starting (PE ring-3 task)\n", 33);
    if (!nyx_zone_init(ZONE_MAX_PAGES)) {
        nyx_write("[DOOM] no zone pages!\n", 22);
        nyx_exit(1);
    }
    nyx_calibrate_tsc();
    nyx_tsc_ms_base = rdtsc64();

    // Enter game mode.  The kernel canvas pointer is informational
    // (the blit syscall copies — ring 3 cannot write kernel memory);
    // we render into our own user-side frame buffer instead.
    g_nyx_canvas = (uint8_t*)nyx_gamestart("DOOM");
    if (!g_nyx_canvas) {
        nyx_write("[DOOM] GAMESTART refused\n", 25);
        nyx_exit(2);
    }
    g_user_frame = (uint32_t*)nyx_zone_alloc(
        DOOMGENERIC_RESX * DOOMGENERIC_RESY * 4);
    if (!g_user_frame) {
        nyx_write("[DOOM] no user frame buffer\n", 28);
        nyx_exit(3);
    }

    // Feed the WAD "FILE" from the baked-in image.
    nyx_wad_file.base = nyx_wad_bytes;
    nyx_wad_file.size = sizeof(nyx_wad_bytes);
    nyx_wad_file.pos  = 0;

    static char* argv[] = { (char*)"doom.exe", (char*)"-iwad",
                            (char*)"doom1.wad" };
    nyx_write("[DOOM] doomgeneric_Create\n", 26);
    doomgeneric_Create(3, argv);

    nyx_write("[DOOM] loop\n", 12);
    for (;;) {
        doomgeneric_Tick();
        nyx_pump_keys();
        nyx_pump_mouse();
    }
}

// PE entry point (elf2pe.py sets AddressOfEntryPoint to this symbol).
__attribute__((section(".text.entry")))
void nyx_pe_entry(void) {
    nyx_main();
}
