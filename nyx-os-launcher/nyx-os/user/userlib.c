// user/userlib.c  –  ring-3 syscall wrappers (see userlib.h).
#include "userlib.h"

// eax = number, ebx = arg0, ecx = arg1, edx = arg2; ret in eax.
static int sc3(int n, unsigned a, unsigned b, unsigned c) {
    int ret;
    asm volatile("int $0x80"
                 : "=a" (ret)
                 : "a" ((unsigned)n), "b" (a), "c" (b), "d" (c)
                 : "memory");
    return ret;
}

int  sys_getpid(void) { return sc3(4, 0, 0, 0); }
void sys_sleep(int ticks) { sc3(2, (unsigned)ticks, 0, 0); }
void sys_exit(void) { sc3(0, 0, 0, 0); for (;;) { } }
int  sys_write(const char* buf, int len) {
    return sc3(8, (unsigned)buf, (unsigned)len, 0);
}
int  sys_socket(void) { return sc3(9, 0, 0, 0); }
int  sys_connect(int fd, unsigned ip, int port) {
    return sc3(10, (unsigned)fd, ip, (unsigned)port);
}
int  sys_send(int fd, const char* buf, int len) {
    return sc3(11, (unsigned)fd, (unsigned)buf, (unsigned)len);
}
int  sys_recv(int fd, char* buf, int max_len) {
    return sc3(12, (unsigned)fd, (unsigned)buf, (unsigned)max_len);
}
int  sys_close(int fd) { return sc3(13, (unsigned)fd, 0, 0); }
int  sys_poll(int fd) { return sc3(14, (unsigned)fd, 0, 0); }
unsigned sys_resolve(const char* host) {
    return (unsigned)sc3(15, (unsigned)host, 0, 0);
}

// ─── Tiny freestanding string/print helpers ──────────────────────
int u_strlen(const char* s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

void u_print(const char* s) {
    sys_write(s, u_strlen(s));
}

int u_dec(unsigned v, char* buf) {
    char tmp[12];
    int p = 0;
    if (v == 0) { buf[0] = '0'; buf[1] = 0; return 1; }
    while (v > 0) { tmp[p++] = (char)('0' + v % 10); v /= 10; }
    for (int i = 0; i < p; i++) buf[i] = tmp[p - 1 - i];
    buf[p] = 0;
    return p;
}

int u_hex(unsigned v, char* buf, int min_digits) {
    const char* hx = "0123456789abcdef";
    char tmp[12];
    int p = 0;
    do { tmp[p++] = hx[v & 0xF]; v >>= 4; } while (v);
    while (p < min_digits) tmp[p++] = '0';
    for (int i = 0; i < p; i++) buf[i] = tmp[p - 1 - i];
    buf[p] = 0;
    return p;
}
