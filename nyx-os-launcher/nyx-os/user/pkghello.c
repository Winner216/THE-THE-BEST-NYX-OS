// user/pkghello.c — first Nyx App Store package (ring 3).
//
// Installed from the online store with `pkg install pkghello` and run
// with `pkg run pkghello`.  Proves that packages downloaded at runtime
// execute as ordinary ring-3 user tasks: sys_write output lands on the
// serial console (COM1), exactly like the embedded `user` demo.
#include "userlib.h"

static int u2s(unsigned v, char* out) {
    char tmp[12]; int p = 0;
    if (v == 0) { out[0] = '0'; out[1] = 0; return 1; }
    while (v > 0) { tmp[p++] = (char)('0' + v % 10); v /= 10; }
    for (int i = 0; i < p; i++) out[i] = tmp[p - 1 - i];
    out[p] = 0;
    return p;
}

void _start(void) {
    char buf[96]; int p = 0;
    const char* m = "[PKGHELLO] store package running! pid=";
    while (*m) buf[p++] = *m++;
    p += u2s((unsigned)sys_getpid(), buf + p);
    buf[p++] = '\n';
    sys_write(buf, p);

    for (int i = 0; i < 3; i++) {
        sys_sleep(50);                 // ~1 second at 50 Hz
        char b2[48]; int q = 0;
        const char* t = "[PKGHELLO] tick i=";
        while (*t) b2[q++] = *t++;
        q += u2s((unsigned)i, b2 + q);
        b2[q++] = '\n';
        sys_write(b2, q);
    }

    sys_write("[PKGHELLO] exiting via sys_exit\n", 30);
    sys_exit();
}
