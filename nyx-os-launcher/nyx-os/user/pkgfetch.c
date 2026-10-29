// user/pkgfetch.c — Nyx App Store package: "Web Fetch".
//
// A real internet client for ring 3: opens a BSD socket through the
// socket syscalls (sys_socket/sys_resolve/sys_connect/sys_send/
// sys_recv), sends a plain HTTP/1.1 GET to example.com on port 80,
// and streams the response (headers + HTML body) to the serial
// console.  Same machinery as the embedded `sockget` shell demo but
// packaged as an installable app with a fixed target so it needs no
// command-line arguments.
//
// The kernel's socket layer lazily brings the NIC + DNS up on the
// first sys_socket(), so it works right after boot once the host has
// network (QEMU slirp: 10.0.2.x).
//
// Install:   pkg install pkgfetch
// Run:       pkg run pkgfetch   → output on the serial console (COM1)
#include "userlib.h"

#define HOST "example.com"
#define PATH "/"

void _start(void) {
    u_print("\n[PKGFETCH] ring-3 HTTP client starting (pid=");
    char nb[16];
    u_dec((unsigned)sys_getpid(), nb);
    u_print(nb);
    u_print(")\n");

    int fd = sys_socket();                 // also brings up NIC + netstack
    if (fd < 0) {
        u_print("[PKGFETCH] socket() failed\n");
        sys_exit();
    }

    u_print("[PKGFETCH] resolving " HOST " ...\n");
    unsigned ip = sys_resolve(HOST);
    if (ip == 0) {
        u_print("[PKGFETCH] DNS failed (no network? try `http init` then rerun)\n");
        sys_close(fd);
        sys_exit();
    }
    u_print("[PKGFETCH] ip=");
    u_dec(ip & 0xFF, nb);      u_print(nb); u_print(".");
    u_dec((ip >> 8) & 0xFF, nb);  u_print(nb); u_print(".");
    u_dec((ip >> 16) & 0xFF, nb); u_print(nb); u_print(".");
    u_dec((ip >> 24) & 0xFF, nb); u_print(nb);
    u_print("\n");

    u_print("[PKGFETCH] connecting :80 ...\n");
    if (sys_connect(fd, ip, 80) < 0) {
        u_print("[PKGFETCH] connect() failed\n");
        sys_close(fd);
        sys_exit();
    }

    // Build: GET / HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n\r\n
    static char req[192];
    int r = 0;
    {
        const char* parts[] = {
            "GET " PATH " HTTP/1.1\r\nHost: " HOST,
            "\r\nUser-Agent: NyxOS-pkgfetch/1.0\r\nConnection: close\r\n\r\n"
        };
        for (int p = 0; p < 2; p++) {
            const char* s = parts[p];
            while (*s && r < (int)sizeof(req) - 1) req[r++] = *s++;
        }
    }
    req[r] = 0;
    u_print("[PKGFETCH] sending GET (" HOST PATH ") ...\n");
    if (sys_send(fd, req, r) < 0) {
        u_print("[PKGFETCH] send() failed\n");
        sys_close(fd);
        sys_exit();
    }

    u_print("\n===== HTTP RESPONSE =====\n");
    static char chunk[256];
    int total = 0;
    int idle = 0;
    for (;;) {
        int n = sys_recv(fd, chunk, (int)sizeof(chunk));
        if (n > 0) {
            sys_write(chunk, n);
            total += n;
            idle = 0;
        } else if (n == 0) {
            if (++idle > 400) break;
            sys_sleep(1);
        } else {
            break;
        }
        if (total > 20000) break;
    }
    u_print("\n===== END (");
    u_dec((unsigned)total, nb);
    u_print(nb);
    u_print(" bytes) =====\n");
    if (total == 0)
        u_print("[PKGFETCH] note: connection up but server sent nothing — it may "
                "redirect to https; the Nyx browser handles https.\n");
    sys_close(fd);

    u_print("[PKGFETCH] done — try `pkg install all` for the rest of the store.\n");
    sys_exit();
}
