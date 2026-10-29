// tools/_jpeg_cmyk_test.c — host harness for the kernel JPEG decoder
// Usage: _jpeg_cmyk_test <in.jpg> <out.raw>
// Decodes with jpeg_decode() and writes width,height (two uint32 LE)
// followed by tightly-packed RGB bytes.  Compare against PIL's own
// decode with the companion Python snippet.
#include <jpegdec.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t* slurp(const char* path, int* len) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* buf = (uint8_t*)malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *len = (int)sz;
    return buf;
}

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    int len = 0;
    uint8_t* data = slurp(argv[1], &len);
    if (!data) { fprintf(stderr, "read fail\n"); return 1; }

    JpegImage img;
    memset(&img, 0, sizeof(img));
    // Buffer: full-res worst case
    int cap = 4096 * 4096 * 4;
    uint8_t* px = (uint8_t*)malloc((size_t)cap);
    img.pixels = px;

    int ok = jpeg_decode(data, len, &img);
    if (!ok) { fprintf(stderr, "DECODE FAILED\n"); return 1; }

    FILE* out = fopen(argv[2], "wb");
    if (!out) return 1;
    uint32_t w = img.width, h = img.height;
    fwrite(&w, 4, 1, out);
    fwrite(&h, 4, 1, out);
    // packed RGB, dropping alpha
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t* row = px + (size_t)y * (size_t)img.stride + 0;
        // stride handling: if zero, tightly packed
        const uint8_t* base = px + (size_t)y * (size_t)((size_t)w * 4u);
        (void)row;
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t* p = base + x * 4;
            fputc(p[0], out); fputc(p[1], out); fputc(p[2], out);
        }
    }
    fclose(out);
    fprintf(stderr, "ok %ux%u\n", w, h);
    return 0;
}
