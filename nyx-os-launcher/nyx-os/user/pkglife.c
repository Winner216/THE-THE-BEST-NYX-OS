// user/pkglife.c — Nyx App Store package: "Game of Life".
//
// Conway's Game of Life rendered as ASCII animation on the serial
// console (COM1).  The universe is a 40x18 torus (edges wrap).  It
// seeds an R-pentomino, a glider and a blinker, then evolves them for
// 40 generations — printing each generation as one screen of text and
// sleeping between frames so the motion is actually visible.
//
// Pure ring-3 code: no kernel help beyond sleep/write/exit, so it is
// a good stress test that a downloaded package can run for a while
// and chew through real computation (grid updates) without crashing.
//
// Install:   pkg install pkglife
// Run:       pkg run pkglife    → watch the animation on COM1
#include "userlib.h"

#define GW 40
#define GH 18
#define GENS 40

static unsigned char grid[GH][GW];
static unsigned char next[GH][GW];

static int alive(void) {
    int n = 0;
    for (int y = 0; y < GH; y++)
        for (int x = 0; x < GW; x++)
            if (grid[y][x]) n++;
    return n;
}

static int nbr(int x, int y) {
    int n = 0;
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            if (dx == 0 && dy == 0) continue;
            int xx = (x + dx + GW) % GW;
            int yy = (y + dy + GH) % GH;
            if (grid[yy][xx]) n++;
        }
    }
    return n;
}

static void step(void) {
    for (int y = 0; y < GH; y++) {
        for (int x = 0; x < GW; x++) {
            int n = nbr(x, y);
            next[y][x] = (unsigned char)
                ((grid[y][x] && (n == 2 || n == 3)) || (!grid[y][x] && n == 3));
        }
    }
    for (int y = 0; y < GH; y++)
        for (int x = 0; x < GW; x++)
            grid[y][x] = next[y][x];
}

static void set(int x, int y) {
    grid[(y + GH) % GH][(x + GW) % GW] = 1;
}

int main_anim(void) {
    // Seed: R-pentomino (left), glider (upper right), blinker (right).
    set(8, 7);  set(9, 7);  set(7, 8);  set(8, 8);  set(8, 9);   // R-pentomino
    set(28, 3); set(29, 4); set(27, 5); set(28, 5); set(29, 5);  // glider
    set(34, 12); set(35, 12); set(36, 12);                        // blinker

    char nb[16];
    u_print("[PKGLIFE] Conway's Game of Life (40x18 torus, pid=");
    u_dec((unsigned)sys_getpid(), nb);
    u_print(nb);
    u_print(")\n");

    static char frame[GW * GH + GH + 16];
    for (int g = 0; g < GENS; g++) {
        if (g > 0) {
            sys_sleep(2);                    // ~40 ms between frames
            step();
        }
        int a = alive();
        // header line:  [PKGLIFE] gen N  alive=M
        char hdr[64];
        int h = 0;
        const char* pre = "[PKGLIFE] gen ";
        while (pre[h]) { hdr[h] = pre[h]; h++; }
        h += u_dec((unsigned)g, hdr + h);
        const char* mid = "  alive=";
        for (int i = 0; mid[i]; i++) hdr[h++] = mid[i];
        h += u_dec((unsigned)a, hdr + h);
        hdr[h++] = '\n';
        sys_write(hdr, h);
        // one full frame: GH rows of GW chars, '#' alive, '.' dead
        int p = 0;
        for (int y = 0; y < GH; y++) {
            for (int x = 0; x < GW; x++)
                frame[p++] = grid[y][x] ? '#' : '.';
            frame[p++] = '\n';
        }
        sys_write(frame, p);
    }
    u_print("[PKGLIFE] 40 generations done — the store's liveliest app.\n");
    sys_exit();
    return 0;
}

void _start(void) {
    main_anim();
}
