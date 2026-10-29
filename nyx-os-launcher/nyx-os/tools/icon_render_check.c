// tools/icon_render_check.c — Host-side verification that every app
// icon SVG in host-files/icons/ decodes with the REAL kernel decoder
// (cpu/svgdec.c) and produces non-trivial, non-empty content.
//
// Build:  gcc -O2 -I include tools/icon_render_check.c cpu/svgdec.c -o /tmp/icon_render_check
// Run:    /tmp/icon_render_check      (must be run from nyx-os/ — paths are relative)
//
// NOTE: this proves the SVGs decode, but NOT that all 19 are embedded
// into the kernel.  That is guaranteed separately: grep the generated
// cpu/embedded_fs.c for the 19 <app>.svg basenames (embed-files.py
// silently drops files past MAX_FILES, so check the count there too).
//
// This mirrors exactly what the kernel does in icon_img_scan_step():
// same svg_decode(), same 1024x1024 max, then it downscales to the
// 56x56 desktop thumb.  We report per-icon: dims, opaque pixel
// count, distinct-ish color count, and a center-pixel sample, so a
// missing/broken icon (which would render as the vector glyph
// fallback on the desktop) shows up here as decode failure or empty
// content.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <svgdec.h>

#define MAX_DIM 1024

static const char* apps[] = {
    "Terminal","Files","Editor","AppLib","Trash","Settings","Music",
    "Calc","Games","Paint","Monitor","Clock","Calendar","Weather",
    "Organizer","Security","Gallery","Browser","QuickSettings"
};
#define APP_COUNT ((int)(sizeof(apps)/sizeof(apps[0])))

static unsigned char* read_file(const char* path, int* outlen) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 4*1024*1024) { fclose(f); return NULL; }
    unsigned char* buf = (unsigned char*)malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *outlen = (int)sz;
    return buf;
}

static void lower(char* s) { for (; *s; s++) if (*s >= 'A' && *s <= 'Z') *s += 32; }

int main(void) {
    int fail = 0;
    for (int a = 0; a < APP_COUNT; a++) {
        char path[256];
        // app name -> filename: lower-case + .svg
        char low[32];
        strncpy(low, apps[a], sizeof(low)-1); low[sizeof(low)-1] = 0;
        lower(low);
        snprintf(path, sizeof(path), "host-files/icons/%s.svg", low);

        int len = 0;
        unsigned char* data = read_file(path, &len);
        if (!data) {
            printf("✗ %-14s FILE MISSING: %s\n", apps[a], path);
            fail = 1;
            continue;
        }

        int w = 0, h = 0;
        if (!svg_dims(data, len, &w, &h)) {
            printf("✗ %-14s svg_dims FAILED\n", apps[a]);
            fail = 1;
            free(data);
            continue;
        }
        if (w <= 0 || h <= 0 || w > MAX_DIM || h > MAX_DIM) {
            printf("✗ %-14s bad dims %dx%d\n", apps[a], w, h);
            fail = 1;
            free(data);
            continue;
        }

        uint8_t* px = (uint8_t*)calloc((size_t)w * h, 4);
        SvgImage img; img.width = 0; img.height = 0; img.pixels = px;
        int ok = svg_decode(data, len, &img);
        if (!ok) {
            printf("✗ %-14s svg_decode FAILED (%dx%d)\n", apps[a], w, h);
            fail = 1;
            free(px); free(data);
            continue;
        }
        w = (int)img.width; h = (int)img.height;

        // Stats over the decoded RGBA buffer
        long opaque = 0, semi = 0, total = (long)w * h;
        long colored = 0;  // pixels where max-min channel diff > 24
        int r_c = 0, g_c = 0, b_c = 0;  // center pixel
        for (long i = 0; i < total; i++) {
            int r = px[i*4], g = px[i*4+1], b = px[i*4+2], al = px[i*4+3];
            if (al == 255) opaque++;
            else if (al > 0) semi++;
            int mx = r, mn = r;
            if (g > mx) mx = g; if (b > mx) mx = b;
            if (g < mn) mn = g; if (b < mn) mn = b;
            if (mx - mn > 24) colored++;
        }
        long ci = ((long)(h/2) * w + (w/2)) * 4;
        r_c = px[ci]; g_c = px[ci+1]; b_c = px[ci+2];
        // opaque center of content: pick the bounding box of opaque pixels
        int minx = w, miny = h, maxx = -1, maxy = -1;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                if (px[((long)y*w+x)*4+3] > 0) {
                    if (x < minx) minx = x; if (x > maxx) maxx = x;
                    if (y < miny) miny = y; if (y > maxy) maxy = y;
                }
            }
        int bw = maxx - minx + 1, bh = maxy - miny + 1;
        const char* verdict = "ok";
        if (opaque == 0 && semi == 0) { verdict = "EMPTY"; fail = 1; }
        if (bw < 8 || bh < 8)         { verdict = "TINY";  fail = 1; }

        // Report the content-bbox ORIGIN too, so host-side tooling
        // can compute a centering translate+scale for the stripped
        // (background-less) glyphs.
        printf("%s %-14s %3dx%-3d opaque=%-6ld semi=%-5ld colored=%-6ld "
               "contentbbox=%d,%d %dx%d center=#%02x%02x%02x\n",
               (ok && verdict[0]=='o') ? "✓" : "✗", apps[a], w, h,
               opaque, semi, colored, minx, miny, bw, bh, r_c, g_c, b_c);
        free(px); free(data);
    }
    printf(fail ? "\nRESULT: FAIL — %d icon(s) broken\n" : "\nRESULT: ALL 19 ICONS DECODE OK\n", fail);
    return fail;
}
