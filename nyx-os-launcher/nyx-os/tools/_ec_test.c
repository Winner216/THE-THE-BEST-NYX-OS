// tools/_ec_test.c — host harness for cpu/ec.c.
// Reads lines from stdin:
//   SM <k_hex> <x_hex> <y_hex>          scalar mult check k*G
//   OC <x_hex> <y_hex> <expect>         on-curve check (0/1)
//   VF <qx_hex> <qy_hex> <dig_hex> <der_hex> <expect>   ECDSA verify (0/1)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "ec.h"

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static int parse_hex(const char* s, uint8_t* out, int max) {
    int n = 0;
    while (*s && n < max) {
        if (*s == ':' || *s == ' ') { s++; continue; }
        int hi = hex_val(*s), lo = hex_val(s[1]);
        if (hi < 0 || lo < 0) break;
        out[n++] = (uint8_t)((hi << 4) | lo);
        s += 2;
    }
    return n;
}
static void print_hex(const uint8_t* d, int n) {
    for (int i = 0; i < n; i++) printf("%02x", d[i]);
    printf("\n");
}

int main(void) {
    ec_p256_init();
    char line[4096];
    int fails = 0, tests = 0;
    while (fgets(line, sizeof(line), stdin)) {
        char cmd[16];
        if (sscanf(line, "%15s", cmd) != 1) continue;
        if (strcmp(cmd, "SM") == 0) {
            char kh[130], xh[130], yh[130];
            if (sscanf(line, "%*s %128s %128s %128s", kh, xh, yh) != 3) continue;
            uint8_t k[32], x[32], y[32], out[64];
            parse_hex(kh, k, 32); parse_hex(xh, x, 32); parse_hex(yh, y, 32);
            ec_p256_scalar_mult_g(k, out);
            tests++;
            if (memcmp(out, x, 32) != 0 || memcmp(out + 32, y, 32) != 0) {
                printf("FAIL SM k=%s\n  got x=", kh); print_hex(out, 32);
                printf("  exp x="); print_hex(x, 32);
                printf("  got y="); print_hex(out + 32, 32);
                printf("  exp y="); print_hex(y, 32);
                fails++;
            } else printf("ok SM %s..\n", kh + 24);
        } else if (strcmp(cmd, "OC") == 0) {
            char xh[130], yh[130]; int expect;
            if (sscanf(line, "%*s %128s %128s %d", xh, yh, &expect) != 3) continue;
            uint8_t pt[64];
            parse_hex(xh, pt, 32); parse_hex(yh, pt + 32, 32);
            int r = ec_p256_on_curve(pt);
            tests++;
            if (r != expect) { printf("FAIL OC expect %d got %d\n", expect, r); fails++; }
            else printf("ok OC %d\n", r);
        } else if (strcmp(cmd, "VF") == 0) {
            char qxh[130], qyh[130], dh[130], sh[600]; int expect;
            if (sscanf(line, "%*s %128s %128s %128s %590s %d",
                       qxh, qyh, dh, sh, &expect) != 5) continue;
            uint8_t Q[64], dig[32], der[300];
            parse_hex(qxh, Q, 32); parse_hex(qyh, Q + 32, 32);
            parse_hex(dh, dig, 32);
            int dl = parse_hex(sh, der, 300);
            int r = ec_p256_verify(Q, dig, der, dl);
            tests++;
            if (r != expect) { printf("FAIL VF expect %d got %d\n", expect, r); fails++; }
            else printf("ok VF %d\n", r);
        }
    }
    printf("TOTAL tests=%d fails=%d\n", tests, fails);
    return fails ? 1 : 0;
}
