// tools/icon_cache_gen.c — Build-time icon thumbnail cache generator.
//
// Decodes every app icon picture in host-files/ (SVG/PNG/JPEG/BMP/GIF)
// with the REAL kernel decoders and packs the downscaled 56x56 RGBA
// thumbs into a single binary blob, host-files/icons.cache.  That file
// is embedded into the kernel like any other host file, and at boot
// desktop_icon_warmup() memcpy's the thumbs straight out of it — so
// the 19 picture decodes that used to run on every boot are done ONCE
// at build time instead.
//
// Build:  gcc -O2 -I include tools/icon_cache_gen.c cpu/svgdec.c \
//             cpu/pngdec.c cpu/jpegdec.c cpu/bmpdec.c cpu/gifdec.c \
//             -o /tmp/icon_cache_gen
// Run:    /tmp/icon_cache_gen      (must be run from nyx-os/ — paths are relative)
//
// The matching (basename == app name, case-insensitive, any image
// extension), the per-format decode, the alpha-preservation rule and
// the downscale all mirror icon_img_scan_step() in cpu/desktop.c
// EXACTLY, so cached thumbs are pixel-identical to what the kernel
// would have decoded at boot.  Keep the two in sync!
//
// Blob layout (little-endian, byte-packed):
//   [0..3]   magic "NYIC"
//   [4]      version (1)
//   [5]      app count (19)
//   [6..9]   u32 picture fingerprint (sum of the byte sizes of every
//            app-matching picture file) — the kernel rejects the
//            cache if this doesn't match its own scan, so a stale
//            cache (icons changed, cache not rebuilt) is re-decoded
//            instead of serving wrong thumbs.
//   then, per app in app_names[] order:
//     [0]      ok flag (1 = thumb present)
//     [1]      cw  (aspect-fit content width, 1..56)
//     [2]      ch  (aspect-fit content height, 1..56)
//     [3]      cox (content x offset inside 56x56 thumb)
//     [4]      coy (content y offset)
//     [5..]    ICON_IMG_SZ bytes of RGBA thumb (56x56x4)
#define CACHE_HEADER   10
#define CACHE_PER_APP  (5 + ICON_IMG_SZ)   // ok + cw + ch + cox + coy + thumb
#define CACHE_TOTAL    (CACHE_HEADER + APP_COUNT * CACHE_PER_APP)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <sys/stat.h>
#include <svgdec.h>
#include <pngdec.h>
#include <jpegdec.h>
#include <bmpdec.h>
#include <gifdec.h>

// ── Constants (must match cpu/desktop.c) ─────────────────────────
#define ICON_IMG_MAX_DIM  1024
#define ICON_IMG_SCRATCH  (ICON_IMG_MAX_DIM * ICON_IMG_MAX_DIM * 4)
#define ICON_IMG_W        56
#define ICON_IMG_H        56
#define ICON_IMG_SZ       (ICON_IMG_W * ICON_IMG_H * 4)
#define MAX_FILES         32
#define MAX_FILENAME      32

static const char* apps[] = {
    "Terminal","Files","Editor","AppLib","Trash","Settings","Music",
    "Calc","Games","Paint","Monitor","Clock","Calendar","Weather",
    "Organizer","Security","Gallery","Browser","QuickSettings","Doom",
    "Chess"
};
#define APP_COUNT ((int)(sizeof(apps)/sizeof(apps[0])))

typedef enum { IMG_NONE, IMG_PNG, IMG_JPG, IMG_BMP, IMG_GIF, IMG_SVG } ImgType;

static void lower(char* s) { for (; *s; s++) if (*s >= 'A' && *s <= 'Z') *s += 32; }

// ── Mirrors icon_img_type() ─────────────────────────────────────
static ImgType img_type(const char* name) {
    int nl = 0; while (name[nl]) nl++;
    int dot = -1;
    for (int i = 0; i < nl; i++) if (name[i] == '.') dot = i;
    if (dot < 0) return IMG_NONE;
    char e[6]; int ei = 0;
    for (int i = dot + 1; name[i] && ei < 5; i++) {
        char ch = name[i];
        if (ch >= 'A' && ch <= 'Z') ch += 32;
        e[ei++] = ch;
    }
    e[ei] = '\0';
    if (ei == 3 && e[0]=='p' && e[1]=='n' && e[2]=='g') return IMG_PNG;
    if (ei == 3 && e[0]=='b' && e[1]=='m' && e[2]=='p') return IMG_BMP;
    if (ei == 3 && e[0]=='g' && e[1]=='i' && e[2]=='f') return IMG_GIF;
    if (ei == 3 && e[0]=='s' && e[1]=='v' && e[2]=='g') return IMG_SVG;
    if ((ei == 3 && e[0]=='j' && e[1]=='p' && e[2]=='g') ||
        (ei == 4 && e[0]=='j' && e[1]=='p' && e[2]=='e' && e[3]=='g'))
        return IMG_JPG;
    return IMG_NONE;
}

// ── Mirrors icon_img_matches(): basename (before the LAST '.') ───
// equals app, case-insensitive.  The kernel uses the last dot, so a
// multi-dot file like "terminal.2.svg" matches nothing there; we do
// the same so fingerprints stay in sync.
static int matches(const char* name, const char* app) {
    int nl = 0, al = 0;
    while (name[nl]) nl++;
    while (app[al]) al++;
    int dot = nl;
    for (int i = 0; i < nl; i++) if (name[i] == '.') dot = i;
    if (dot != al) return 0;
    for (int i = 0; i < al; i++) {
        char a = name[i], b = app[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return 1;
}

// Collect all files under host-files/ (flattened, deduped by
// basename — first wins, exactly like embed-files.py).
typedef struct { char name[MAX_FILENAME]; unsigned char* data; int size; } FEntry;
static FEntry files[MAX_FILES];
static int nfiles = 0;

static int namecmp(const void* a, const void* b) {
    return strcmp(((const FEntry*)a)->name, ((const FEntry*)b)->name);
}

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

static void walk(const char* dir) {
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent** list = NULL;
    int n = scandir(dir, &list, NULL, alphasort);
    for (int i = 0; i < n; i++) {
        struct dirent* de = list[i];
        if (de->d_name[0] == '.') { free(de); continue; }
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            walk(path);
        } else {
            const char* fname = de->d_name;
            if (!strcmp(fname, "README.txt") || !strcmp(fname, "README.md")) {
                free(de); continue;
            }
            if (nfiles >= MAX_FILES) { free(de); continue; }
            // dedupe by basename (first wins, matching embed-files.py)
            int dup = 0;
            for (int j = 0; j < nfiles; j++)
                if (!strcmp(files[j].name, fname)) { dup = 1; break; }
            if (dup) { free(de); continue; }
            int len = 0;
            unsigned char* data = read_file(path, &len);
            if (!data) { free(de); continue; }
            strncpy(files[nfiles].name, fname, MAX_FILENAME - 1);
            files[nfiles].name[MAX_FILENAME - 1] = '\0';
            files[nfiles].data = data;
            files[nfiles].size = len;
            nfiles++;
        }
        free(de);
    }
    if (list) free(list);
    closedir(d);
}

// Sum of the byte sizes of every app-matching picture file — the
// kernel mirrors this and rejects the cache if the numbers differ.
static long pic_fingerprint(void) {
    long fp = 0;
    for (int a = 0; a < APP_COUNT; a++) {
        for (int i = 0; i < nfiles; i++) {
            if (img_type(files[i].name) != IMG_NONE &&
                matches(files[i].name, apps[a])) { fp += files[i].size; break; }
        }
    }
    return fp;
}

int main(void) {
    walk("host-files");
    qsort(files, (size_t)nfiles, sizeof(FEntry), namecmp);

    uint8_t* blob = (uint8_t*)calloc(CACHE_TOTAL, 1);
    if (!blob) { fprintf(stderr, "OOM\n"); return 1; }
    memcpy(blob, "NYIC", 4);
    blob[4] = 1;               // version
    blob[5] = APP_COUNT;       // app count
    {   long fp = pic_fingerprint();
        blob[6] = (uint8_t)(fp & 0xFF);
        blob[7] = (uint8_t)((fp >> 8) & 0xFF);
        blob[8] = (uint8_t)((fp >> 16) & 0xFF);
        blob[9] = (uint8_t)((fp >> 24) & 0xFF);
    }

    uint8_t* scratch = (uint8_t*)malloc(ICON_IMG_SCRATCH);
    if (!scratch) { fprintf(stderr, "OOM\n"); return 1; }

    int done = 0, fail = 0;
    for (int a = 0; a < APP_COUNT; a++) {
        uint8_t* rec = blob + CACHE_HEADER + a * CACHE_PER_APP;

        // Find the first embedded file whose basename matches this app
        int fi = -1;
        for (int i = 0; i < nfiles; i++)
            if (img_type(files[i].name) != IMG_NONE &&
                matches(files[i].name, apps[a])) { fi = i; break; }
        if (fi < 0) {
            printf("• %-14s no picture file — glyph fallback\n", apps[a]);
            continue;
        }

        const unsigned char* c = files[fi].data;
        int clen = files[fi].size;
        ImgType t = img_type(files[fi].name);

        int ok = 0, w = 0, h = 0;
        if (t == IMG_PNG) { PngImage im; im.pixels = scratch;
            ok = png_decode(c, clen, &im); if (ok) { w = (int)im.width; h = (int)im.height; } }
        else if (t == IMG_BMP) { BmpImage im; im.pixels = scratch;
            ok = bmp_decode(c, clen, &im); if (ok) { w = (int)im.width; h = (int)im.height; } }
        else if (t == IMG_GIF) { GifImage im; im.pixels = scratch;
            ok = gif_decode(c, clen, &im); if (ok) { w = (int)im.width; h = (int)im.height; } }
        else if (t == IMG_SVG) { SvgImage im; im.pixels = scratch;
            ok = svg_decode(c, clen, &im); if (ok) { w = (int)im.width; h = (int)im.height; } }
        else { JpegImage im; im.pixels = scratch;
            ok = jpeg_decode(c, clen, &im); if (ok) { w = (int)im.width; h = (int)im.height; } }

        if (!ok || w <= 0 || h <= 0 || w > ICON_IMG_MAX_DIM || h > ICON_IMG_MAX_DIM ||
            (long)w * h * 4 > ICON_IMG_SCRATCH) {
            printf("✗ %-14s decode failed (%s) — glyph fallback\n", apps[a], files[fi].name);
            fail = 1;
            continue;
        }

        // ── Downscale — mirrors icon_img_scan_step() exactly ────
        uint8_t* dst = rec + 5;
        memset(dst, 0, ICON_IMG_SZ);   // transparent letterbox
        int dw = ICON_IMG_W, dh = ICON_IMG_H;
        if (w * dh > h * dw) dh = h * dw / w;
        else                 dw = w * dh / h;
        int ox = (ICON_IMG_W - dw) / 2, oy = (ICON_IMG_H - dh) / 2;
        for (int y = 0; y < dh; y++)
            for (int x = 0; x < dw; x++) {
                int sx = x * w / dw, sy = y * h / dh;
                if (sx >= w) sx = w - 1;
                if (sy >= h) sy = h - 1;
                int si = (sy * w + sx) * 4;
                int di = ((oy + y) * ICON_IMG_W + ox + x) * 4;
                dst[di]   = scratch[si];
                dst[di+1] = scratch[si+1];
                dst[di+2] = scratch[si+2];
                // Alpha rule: preserve only SVG/PNG, force opaque
                // for JPEG/BMP/GIF — same as the kernel scan.
                dst[di+3] = (t == IMG_SVG || t == IMG_PNG) ? scratch[si+3] : 255;
            }

        rec[0] = 1;          // ok
        rec[1] = (uint8_t)dw;
        rec[2] = (uint8_t)dh;
        rec[3] = (uint8_t)ox;
        rec[4] = (uint8_t)oy;
        done++;
    }

    free(scratch);
    for (int i = 0; i < nfiles; i++) free(files[i].data);

    FILE* f = fopen("host-files/icons.cache", "wb");
    if (!f) { fprintf(stderr, "cannot write host-files/icons.cache\n"); return 1; }
    fwrite(blob, 1, CACHE_TOTAL, f);
    fclose(f);
    printf("✔ wrote host-files/icons.cache (%d bytes, %d/%d icons cached)\n",
           CACHE_TOTAL, done, APP_COUNT);
    free(blob);
    // App count shortfall is NOT a failure: an app with no picture
    // file legitimately keeps the vector-glyph fallback (its record
    // stays ok=0), and the kernel's fingerprint matches on both
    // sides either way.  Only hard-fail on actual decode errors.
    return fail ? 1 : 0;
}
