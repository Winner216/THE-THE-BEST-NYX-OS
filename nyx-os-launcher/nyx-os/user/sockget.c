// user/sockget.c  –  ring-3 BSD-socket demo: resolve a hostname, open
// a stream socket, connect, send an HTTP GET, read the response and
// write it to the COM1 console (sys_write).  Proves the whole chain
// user program → int 0x80 syscalls → socket layer → per-connection
// TCP → NIC works from ring 3, with no kernel help besides the
// syscall numbers in userlib.c.
//
//   sockget example.com /          → GET / HTTP/1.1
//   sockget example.com /index     → GET /index HTTP/1.1
#include "userlib.h"

// strlen for a NUL-terminated string (no libc in ring 3).
static int ulen(const char* s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

// Print a NUL-terminated string to COM1.
static void uprint(const char* s) {
    sys_write(s, ulen(s));
}

// Convert 0-9 to a char.
static char udig(int d) {
    return (char)('0' + d);
}

// Format an unsigned into buf (decimal), return length.
static int uitoa(unsigned v, char* buf) {
    char tmp[12];
    int n = 0;
    if (v == 0) { buf[0] = '0'; buf[1] = 0; return 1; }
    while (v > 0) { tmp[n++] = udig((int)(v % 10)); v /= 10; }
    for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    buf[n] = 0;
    return n;
}

void _start(void) {
    // Argument area: the kernel loader (elfload.c) copies the typed
    // command line to USER_ARGS_BASE (see userlib.h).  The command is
    // "sockget <host> <path>"; we skip the program name and parse the
    // host and path tokens that follow.
    static char hostbuf[64];
    static char pathbuf[128];
    const char* a = (const char*)USER_ARGS_BASE;
    const char* host;
    const char* path;
    int i = 0;

    // Skip whitespace, then the program name ("sockget").
    while (a[i] == ' ' || a[i] == '\t') i++;
    while (a[i] && a[i] != ' ' && a[i] != '\t') i++;   // program name
    while (a[i] == ' ' || a[i] == '\t') i++;

    // First token = host, second = path.
    int h = 0;
    while (a[i] && a[i] != ' ' && a[i] != '\t' && h < 63)
        hostbuf[h++] = a[i++];
    hostbuf[h] = 0;
    while (a[i] == ' ' || a[i] == '\t') i++;
    int p = 0;
    while (a[i] && a[i] != ' ' && a[i] != '\t' && p < 127)
        pathbuf[p++] = a[i++];
    pathbuf[p] = 0;

    if (h == 0) { for (int k = 0; "example.com"[k]; k++) hostbuf[k] = "example.com"[k]; }
    if (p == 0) { pathbuf[0] = '/'; pathbuf[1] = 0; }
    host = hostbuf;
    path = pathbuf;

    // Open the socket FIRST: sock_open() lazily brings up the NIC +
    // netstack, which DNS needs to send its UDP query.
    int fd = sys_socket();
    if (fd < 0) {
        uprint("[sockget] socket() failed\n");
        sys_exit();
    }

    uprint("\n[sockget] resolving ");
    uprint(host);
    uprint(" ...\n");
    unsigned ip = sys_resolve(host);
    if (ip == 0) {
        uprint("[sockget] DNS failed (no network? try: nic init)\n");
        sys_close(fd);
        sys_exit();
    }

    uprint("[sockget] ip=");
    {
        char ib[12];
        int n = uitoa(ip & 0xFF, ib);      sys_write(ib, n);
        uprint(".");
        n = uitoa((ip >> 8) & 0xFF, ib);   sys_write(ib, n);
        uprint(".");
        n = uitoa((ip >> 16) & 0xFF, ib);  sys_write(ib, n);
        uprint(".");
        n = uitoa((ip >> 24) & 0xFF, ib);  sys_write(ib, n);
    }
    uprint("\n");

    uprint("[sockget] connecting :80 ...\n");
    if (sys_connect(fd, ip, 80) < 0) {
        uprint("[sockget] connect() failed\n");
        sys_close(fd);
        sys_exit();
    }

    // HTTP GET request
    static char req[512];
    int r = 0;
    const char* g = "GET ";
    while (g[r]) { req[r] = g[r]; r++; }
    for (int k = 0; path[k]; k++) req[r++] = path[k];
    const char* hh = " HTTP/1.1\r\nHost: ";
    for (int k = 0; hh[k]; k++) req[r++] = hh[k];
    for (int k = 0; host[k]; k++) req[r++] = host[k];
    const char* end = "\r\nConnection: close\r\n\r\n";
    for (int k = 0; end[k]; k++) req[r++] = end[k];

    uprint("[sockget] sending request (");
    { char nb[12]; int n = uitoa((unsigned)r, nb); sys_write(nb, n); }
    uprint(" bytes)\n");
    int sent = sys_send(fd, req, r);
    if (sent < 0) {
        uprint("[sockget] send() failed\n");
        sys_close(fd);
        sys_exit();
    }

    // Read the response in chunks, writing each to COM1.
    uprint("\n===== HTTP RESPONSE =====\n");
    static char chunk[256];
    int total = 0;
    int idle = 0;
    for (;;) {
        int n = sys_recv(fd, chunk, sizeof(chunk));
        if (n > 0) {
            sys_write(chunk, n);
            total += n;
            idle = 0;
        } else if (n == 0) {
            // Nothing buffered yet — poll a few times (non-blocking
            // recv is the poll()-style wait here), then give up.
            if (++idle > 400) break;   // ~40ms per poll cycle worst case
            sys_sleep(1);
        } else {
            break;   // connection gone
        }
        if (total > 60000) break;   // safety cap
    }

    uprint("\n===== END (");
    { char nb[12]; int n = uitoa((unsigned)total, nb); sys_write(nb, n); }
    uprint(" bytes) =====\n");
    sys_close(fd);
    sys_exit();
}
