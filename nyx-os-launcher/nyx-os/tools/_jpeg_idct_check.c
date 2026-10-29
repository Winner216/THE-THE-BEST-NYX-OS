// tools/_jpeg_idct_check.c — all-C comparison: the kernel's fixed-point
// idct_2d vs a double-precision direct 2D IDCT on the same coefficients.
// Includes the real cpu/jpegdec.c so idct_2d is the actual kernel code.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../cpu/jpegdec.c"

// direct 2D IDCT in double
static void idct_direct(const int* coeffs, uint8_t* out) {
    static const int zig[64] = {
         0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
        12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
        35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
        58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
    };
    double blk[8][8];
    for (int i = 0; i < 64; i++) blk[i/8][i%8] = (double)coeffs[zig[i]];
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            double s = 0.0;
            for (int v = 0; v < 8; v++) {
                for (int u = 0; u < 8; u++) {
                    double cu = (u == 0) ? 1.0/sqrt(2.0) : 1.0;
                    double cv = (v == 0) ? 1.0/sqrt(2.0) : 1.0;
                    s += cu * cv * blk[v][u] *
                         cos(((2*x+1) * u * M_PI) / 16.0) *
                         cos(((2*y+1) * v * M_PI) / 16.0);
                }
            }
            double val = s / 4.0 + 128.0;
            int iv = (int)(val < 0 ? val - 0.5 : val + 0.5);
            if (iv < 0) iv = 0;
            if (iv > 255) iv = 255;
            out[y*8 + x] = (uint8_t)iv;
        }
    }
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    // argv[1] = "c1:461,-271,3,..." full 64 dequantized coeffs for one comp
    int coeffs[64];
    const char* p = strchr(argv[1], ':');
    if (!p) return 2;
    p++;
    for (int i = 0; i < 64; i++) { coeffs[i] = atoi(p); p = strchr(p, ','); if (p) p++; }

    uint8_t kfix[64], dref[64];
    idct_2d(coeffs, kfix);       // real kernel IDCT
    idct_direct(coeffs, dref);   // double reference
    int maxe = 0, tot = 0;
    for (int i = 0; i < 64; i++) {
        int e = abs((int)kfix[i] - (int)dref[i]);
        if (e > maxe) maxe = e;
        tot += e;
    }
    printf("kernel-vs-double: max err=%d avg err=%.2f\n", maxe, tot/64.0);
    printf("row2 kernel = ");
    for (int i = 16; i < 24; i++) printf("%d ", kfix[i]);
    printf("\nrow2 double = ");
    for (int i = 16; i < 24; i++) printf("%d ", dref[i]);
    printf("\n");
    return 0;
}
