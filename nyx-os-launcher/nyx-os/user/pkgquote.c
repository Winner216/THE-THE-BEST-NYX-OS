// user/pkgquote.c — Nyx App Store package: prints a short quote.
//
// Demonstrates that store packages can carry arbitrary data/logic —
// this one embeds a tiny quote table and prints one entry chosen by
// a rolling counter, then exits.
#include "userlib.h"

static int u2s(unsigned v, char* out) {
    char tmp[12]; int p = 0;
    if (v == 0) { out[0] = '0'; out[1] = 0; return 1; }
    while (v > 0) { tmp[p++] = (char)('0' + v % 10); v /= 10; }
    for (int i = 0; i < p; i++) out[i] = tmp[p - 1 - i];
    out[p] = 0;
    return p;
}

static const char* const QUOTES[] = {
    "[PKGQUOTE] \"The best way to predict the future is to build it.\"\n",
    "[PKGQUOTE] \"There is no royal road to kernel development.\"\n",
    "[PKGQUOTE] \"640KiB ought to be enough for anybody.\"\n",
};
#define QUOTE_N 3

void _start(void) {
    unsigned pid = (unsigned)sys_getpid();

    // Pick a quote from the pid so different runs print different lines.
    const char* q = QUOTES[pid % QUOTE_N];
    int n = 0;
    while (q[n]) n++;
    sys_write(q, n);

    char b[48]; int p = 0;
    const char* t = "[PKGQUOTE] from ring-3 package, pid=";
    while (*t) b[p++] = *t++;
    p += u2s(pid, b + p);
    b[p++] = '\n';
    sys_write(b, p);
    sys_exit();
}
