// user/hello.c  –  the first ring-3 user program.
// Built by the Makefile into an ELF32 linked at USER_LINK_BASE and
// embedded into the kernel; launched from the shell with `user`.
// Every I/O goes through `int $0x80` — ring 3 has no direct hardware
// access (IOPL 0, no I/O bitmap).
#include "userlib.h"

// Tiny decimal formatter (no libc, no division helper — plain / %).
static int u2s(unsigned v, char* out) {
    char tmp[12]; int p = 0;
    if (v == 0) { out[0] = '0'; out[1] = 0; return 1; }
    while (v > 0) { tmp[p++] = (char)('0' + v % 10); v /= 10; }
    for (int i = 0; i < p; i++) out[i] = tmp[p - 1 - i];
    out[p] = 0;
    return p;
}

void _start(void) {
    char buf[64]; int p = 0;
    const char* m = "USER hello from ring 3! pid=";
    while (*m) buf[p++] = *m++;
    p += u2s((unsigned)sys_getpid(), buf + p);
    buf[p++] = '\n';
    sys_write(buf, p);

    for (int i = 0; i < 4; i++) {
        sys_sleep(50);                 // ~1 second at 50 Hz
        char b2[48]; int q = 0;
        const char* t = "USER tick i=";
        while (*t) b2[q++] = *t++;
        q += u2s((unsigned)i, b2 + q);
        b2[q++] = '\n';
        sys_write(b2, q);
    }

    sys_write("USER exiting via sys_exit\n", 26);
    sys_exit();
}
