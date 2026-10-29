// cpu/gallery.c — Image Gallery for Nyx OS
//
// Desktop-mode app that scans the filesystem for PNG/JPEG files,
// decodes 32x32 thumbnails, displays them in a scrollable grid,
// or shows a full-screen slideshow.
//
// Press S in grid view → slideshow mode. S/ESC to return to grid.
// Slides advance automatically every ~5 seconds using the system
// tick counter (~18 Hz timer). Crossfade blends slides over ~1 sec.

#include <gallery.h>
#include <vesa.h>
#include <mouse.h>
#include <fs.h>
#include <pngdec.h>
#include <jpegdec.h>
#include <bmpdec.h>
#include <gifdec.h>
#include <svgdec.h>
#include <imgview.h>
#include <desktop.h>
#include <stdint.h>
#include <stddef.h>

// ─── Constants ───────────────────────────────────────────────────
#define THUMB_W          32
#define THUMB_H          32
#define THUMB_SIZE       (THUMB_W * THUMB_H * 4)
#define GALLERY_MAX      MAX_FILES
#define PIXBUF_SIZE      (1920 * 1080 * 4)

#define COLUMNS          5
#define TITLE_H          36
#define THUMB_GAP_X      24
#define THUMB_GAP_Y      16
#define LABEL_H          20

// Slideshow timing (system tick ≈ 18 Hz)
#define TICKS_PER_SLIDE  90     // ~5 seconds
#define FADE_TICKS       18     // ~1 second crossfade

extern volatile uint32_t tick;

// ─── GallerySlot ─────────────────────────────────────────────────
typedef struct {
    int     file_idx;
    int     has_image;
    int     is_png;
    int     is_bmp;
    int     is_gif;
    int     is_svg;
    int     img_w;
    int     img_h;
    char    name[MAX_FILENAME];
    uint8_t thumb[THUMB_SIZE];
} GallerySlot;

static GallerySlot slots[GALLERY_MAX];
static int slot_count = 0;
static int selected   = 0;
static int scroll_y   = 0;
static uint8_t pixbuf[PIXBUF_SIZE] __attribute__((aligned(64)));

// ─── Slideshow state ─────────────────────────────────────────────
static int      ss_active    = 0;
static int      ss_index     = 0;
static int      ss_img_w     = 0;
static int      ss_img_h     = 0;
static uint32_t ss_tick      = 0;
static uint8_t  ss_current[PIXBUF_SIZE] __attribute__((aligned(64)));
static uint8_t  ss_old[PIXBUF_SIZE] __attribute__((aligned(64)));
static int      ss_fading    = 0;
static uint32_t ss_fade_tick = 0;

// ─── Thumbnail scaler ────────────────────────────────────────────
static void make_thumb(const uint8_t* src, int sw, int sh, uint8_t* dst) {
    for (int ty = 0; ty < THUMB_H; ty++)
        for (int tx = 0; tx < THUMB_W; tx++) {
            int sx = tx * sw / THUMB_W, sy = ty * sh / THUMB_H;
            if (sx >= sw) sx = sw - 1;
            if (sy >= sh) sy = sh - 1;
            int si = (sy * sw + sx) * 4;
            int di = (ty * THUMB_W + tx) * 4;
            dst[di+0] = src[si+0]; dst[di+1] = src[si+1];
            dst[di+2] = src[si+2]; dst[di+3] = 255;
        }
}

// Quick header scan: get image dimensions WITHOUT full decode, so we
// can reject oversized images before the decoder writes past the pixel
// buffer and corrupts adjacent BSS.
static int gal_img_dims(const uint8_t* data, int datalen,
                         int* w, int* h) {
    if (!data || datalen < 8) return 0;
    if (data[0]==0x89 && data[1]=='P' && data[2]=='N' && data[3]=='G') {
        if (datalen < 24) return 0;
        *w = ((int)data[16]<<24)|((int)data[17]<<16)|((int)data[18]<<8)|data[19];
        *h = ((int)data[20]<<24)|((int)data[21]<<16)|((int)data[22]<<8)|data[23];
        return (*w > 0 && *h > 0);
    }
    if (data[0]==0xFF && data[1]==0xD8 && data[2]==0xFF) {
        int pos = 2;
        while (pos + 1 < datalen) {
            if (data[pos] != 0xFF) { pos++; continue; }
            while (pos < datalen && data[pos] == 0xFF) pos++;
            if (pos >= datalen) break;
            uint8_t marker = data[pos++];
            if (marker == 0xD9) break;
            if (marker == 0x00 || marker == 0xD8) continue;
            if (pos + 1 >= datalen) break;
            int seg_len = ((int)data[pos]<<8)|(int)data[pos+1];
            if (seg_len < 2) continue;
            if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2) {
                if (seg_len < 7) return 0;
                *h = ((int)data[pos+3]<<8)|data[pos+4];
                *w = ((int)data[pos+5]<<8)|data[pos+6];
                return (*w > 0 && *h > 0);
            }
            pos += seg_len;
        }
    }
    // BMP: "BM" at bytes 0-1, width at 18-21, height at 22-25
    if (data[0]=='B' && data[1]=='M' && datalen >= 26) {
        uint32_t hsz = (uint32_t)data[14]|((uint32_t)data[15]<<8)|((uint32_t)data[16]<<16)|((uint32_t)data[17]<<24);
        if (hsz == 12) {
            *w = (int)data[18]|((int)data[19]<<8);
            *h = (int)data[20]|((int)data[21]<<8);
            if (*h < 0) *h = -*h;
            return (*w>0&&*h>0);
        } else if (hsz >= 40) {
            *w = (int)(data[18]|((int)data[19]<<8)|((int)data[20]<<16)|((int)data[21]<<24));
            *h = (int)(data[22]|((int)data[23]<<8)|((int)data[24]<<16)|((int)data[25]<<24));
            if (*h < 0) *h = -*h;
            return (*w>0&&*h>0&&*w<=4096&&*h<=4096);
        }
    }
    // GIF: "GIF87a" or "GIF89a" at bytes 0-5, width at 6-7, height at 8-9
    if (data[0]=='G' && data[1]=='I' && data[2]=='F' && datalen >= 10) {
        *w = (int)data[6]|((int)data[7]<<8);
        *h = (int)data[8]|((int)data[9]<<8);
        return (*w>0&&*h>0&&*w<=4096&&*h<=4096);
    }
    // SVG: text-based — ask the decoder for the declared size
    if (datalen >= 4 && data[0]=='<' && (data[1]=='s'||data[1]=='S') &&
        (data[2]=='v'||data[2]=='V') && (data[3]=='g'||data[3]=='G'))
        return svg_dims(data, datalen, w, h);
    return 0;
}

// ─── Decode a slot's image into a buffer, return dimensions ──────
static int decode_slot(int idx, uint8_t* out, int* w, int* h) {
    if (idx < 0 || idx >= slot_count || !slots[idx].has_image) return 0;
    int fi = slots[idx].file_idx;
    const uint8_t* c = (const uint8_t*)filesystem[fi].content;
    int clen = filesystem[fi].size;
    if (clen <= 0) { while (c[clen]) clen++; }
    // Check dimensions BEFORE decoding — decoder writes to pixbuf
    int iw = 0, ih = 0;
    if (!gal_img_dims(c, clen, &iw, &ih)) return 0;
    if (iw * ih * 4 > PIXBUF_SIZE) return 0;
    int ok = 0;
    if (slots[idx].is_png) {
        PngImage img; img.pixels = pixbuf;
        if (png_decode(c, clen, &img)) { iw = (int)img.width; ih = (int)img.height; ok = 1; }
    } else if (slots[idx].is_bmp) {
        BmpImage img; img.pixels = pixbuf;
        if (bmp_decode(c, clen, &img)) { iw = (int)img.width; ih = (int)img.height; ok = 1; }
    } else if (slots[idx].is_gif) {
        GifImage img; img.pixels = pixbuf;
        if (gif_decode(c, clen, &img)) { iw = (int)img.width; ih = (int)img.height; ok = 1; }
    } else if (slots[idx].is_svg) {
        SvgImage img; img.pixels = pixbuf;
        if (svg_decode(c, clen, &img)) { iw = (int)img.width; ih = (int)img.height; ok = 1; }
    } else {
        JpegImage img = {0}; img.pixels = pixbuf;
        if (jpeg_decode(c, clen, &img)) { iw = (int)img.width; ih = (int)img.height; ok = 1; }
    }
    if (!ok || iw * ih * 4 > PIXBUF_SIZE) return 0;
    int sz = iw * ih * 4;
    for (int i = 0; i < sz; i++) out[i] = pixbuf[i];
    if (w) *w = iw; if (h) *h = ih;
    return 1;
}

// ─── Init ────────────────────────────────────────────────────────
void gallery_init(void) {
    slot_count = 0; selected = 0; scroll_y = 0; ss_active = 0; ss_fading = 0;
    for (int fi = 0; fi < MAX_FILES && slot_count < GALLERY_MAX; fi++) {
        if (!filesystem[fi].used) continue;
        const char* name = filesystem[fi].name;
        int nl = 0; while (name[nl]) nl++;
        int dot = -1;
        for (int i = 0; i < nl; i++) if (name[i] == '.') dot = i;
        if (dot < 0) continue;
        const char* ext = name + dot;
        int is_png = 0, is_jpg = 0, is_bmp = 0, is_gif = 0, is_svg = 0;
        if ((ext[1]=='p'||ext[1]=='P')&&(ext[2]=='n'||ext[2]=='N')&&(ext[3]=='g'||ext[3]=='G')&&ext[4]=='\0')
            is_png = 1;
        else if ((ext[1]=='j'||ext[1]=='J')&&(ext[2]=='p'||ext[2]=='P')&&(ext[3]=='g'||ext[3]=='G')&&ext[4]=='\0')
            is_jpg = 1;
        else if ((ext[1]=='j'||ext[1]=='J')&&(ext[2]=='p'||ext[2]=='P')&&(ext[3]=='e'||ext[3]=='E')&&
                 (ext[4]=='g'||ext[4]=='G')&&ext[5]=='\0')
            is_jpg = 1;
        else if ((ext[1]=='b'||ext[1]=='B')&&(ext[2]=='m'||ext[2]=='M')&&(ext[3]=='p'||ext[3]=='P')&&ext[4]=='\0')
            is_bmp = 1;
        else if ((ext[1]=='g'||ext[1]=='G')&&(ext[2]=='i'||ext[2]=='I')&&(ext[3]=='f'||ext[3]=='F')&&ext[4]=='\0')
            is_gif = 1;
        else if ((ext[1]=='s'||ext[1]=='S')&&(ext[2]=='v'||ext[2]=='V')&&(ext[3]=='g'||ext[3]=='G')&&ext[4]=='\0')
            is_svg = 1;
        if (!is_png && !is_jpg && !is_bmp && !is_gif && !is_svg) continue;
        const uint8_t* content = (const uint8_t*)filesystem[fi].content;
        int clen = filesystem[fi].size;
        if (clen <= 0) { while (content[clen]) clen++; }
        if (clen < 8) continue;
        int w = 0, h = 0, ok = 0;
        // Check dimensions BEFORE decoding to avoid buffer overflow
        if (gal_img_dims(content, clen, &w, &h) && w*h*4 <= PIXBUF_SIZE) {
            if (is_png) {
                PngImage img; img.pixels = pixbuf;
                ok = png_decode(content, clen, &img);
                if (ok) { w = (int)img.width; h = (int)img.height; }
            } else if (is_bmp) {
                BmpImage img; img.pixels = pixbuf;
                ok = bmp_decode(content, clen, &img);
                if (ok) { w = (int)img.width; h = (int)img.height; }
            } else if (is_gif) {
                GifImage img; img.pixels = pixbuf;
                ok = gif_decode(content, clen, &img);
                if (ok) { w = (int)img.width; h = (int)img.height; }
            } else if (is_svg) {
                SvgImage img; img.pixels = pixbuf;
                ok = svg_decode(content, clen, &img);
                if (ok) { w = (int)img.width; h = (int)img.height; }
            } else {
                JpegImage img = {0}; img.pixels = pixbuf;
                ok = jpeg_decode(content, clen, &img);
                if (ok) { w = (int)img.width; h = (int)img.height; }
            }
        }
        if (!ok) continue;
        int si = slot_count++;
        slots[si].file_idx = fi; slots[si].has_image = 1; slots[si].is_png = is_png; slots[si].is_bmp = is_bmp; slots[si].is_gif = is_gif; slots[si].is_svg = is_svg;
        slots[si].img_w = w; slots[si].img_h = h;
        int ni = 0; while (name[ni] && ni < MAX_FILENAME-1) { slots[si].name[ni]=name[ni]; ni++; }
        slots[si].name[ni] = '\0';
        make_thumb(pixbuf, w, h, slots[si].thumb);
    }
}

// ─── Full-screen slide draw ─────────────────────────────────────
static void draw_slide_full(const uint8_t* pixels, int iw, int ih) {
    int pw = (int)vesa.width, ph = (int)vesa.height;
    vesa_fill_rect(0, 0, pw, ph, 0x000811);
    if (iw <= pw && ih <= ph) {
        int ox = (pw - iw) / 2, oy = (ph - ih) / 2;
        vesa_blit_rgba(ox, oy, pixels, iw, ih, 0, 0, iw, ih);
    } else {
        // Use bilinear interpolation for smoother downscaling
        vesa_blit_rgba_scaled_bilinear(0, 0, pw, ph, pixels, iw, ih);
    }
}

// ─── Crossfade blend ─────────────────────────────────────────────
static void draw_blended(const uint8_t* from, const uint8_t* to,
                         int iw, int ih, int alpha) {
    int pw = (int)vesa.width, ph = (int)vesa.height;
    vesa_fill_rect(0, 0, pw, ph, 0x000811);
    if (iw <= pw && ih <= ph) {
        int ox = (pw - iw) / 2, oy = (ph - ih) / 2;
        vesa_blend_rgba(ox, oy, iw, ih, from, to, iw, ih, alpha);
    } else {
        vesa_blend_rgba(0, 0, pw, ph, from, to, iw, ih, alpha);
    }
}

// ─── Advance slideshow ───────────────────────────────────────────
static void ss_advance(int next_idx) {
    for (int i = 0; i < PIXBUF_SIZE; i++) ss_old[i] = ss_current[i];
    int nw = 0, nh = 0;
    if (!decode_slot(next_idx, ss_current, &nw, &nh)) {
        // Decode failed — restore old and stay on current slide
        for (int i = 0; i < PIXBUF_SIZE; i++) ss_current[i] = ss_old[i];
        return;
    }
    ss_img_w = nw; ss_img_h = nh;
    ss_index = next_idx;
    ss_fading = 1;
    ss_fade_tick = tick;
}

// ─── Grid thumbnail draw ─────────────────────────────────────────
static void draw_thumb(int x, int y, const GallerySlot* s, int sel) {
    uint32_t border = sel ? 0xFFCC66 : 0x4A3870;
    // Hover highlight
    int hover = !sel && mouse_in_rect(x-2, y-2, THUMB_W+10, THUMB_H+22);
    if (hover) {
        vesa_draw_rect(x-3, y-3, THUMB_W+8, THUMB_H+8, 0x9B7FFF);
        border = 0x8060C0;
    }
    vesa_draw_rect(x-2, y-2, THUMB_W+4, THUMB_H+4, border);
    if (sel) vesa_draw_rect(x-3, y-3, THUMB_W+6, THUMB_H+6, 0x806030);
    vesa_blit_rgba(x, y, s->thumb, THUMB_W, THUMB_H, 0, 0, THUMB_W, THUMB_H);
    vesa_draw_string(x, y+THUMB_H+4, s->name, sel ? 0xFFCC66 : (hover ? 0xE0D0FF : 0xC8B8E8), 0);
}

// ─── Slideshow active query ────────────────────────────────────
int gallery_is_slideshow_active(void) {
    return ss_active;
}

// ─── Draw: slideshow or grid ────────────────────────────────────
void gallery_draw(void) {
    if (!vesa.active) return;
    int pw = (int)vesa.width, ph = (int)vesa.height;

    // ── Slideshow mode ──────────────────────────────────────────
    if (ss_active) {
        // Auto-advance based on tick count
        if (!ss_fading && slot_count > 1) {
            uint32_t elapsed = tick - ss_tick;
            if (elapsed >= TICKS_PER_SLIDE) {
                int slides = elapsed / TICKS_PER_SLIDE;
                int next = (ss_index + slides) % slot_count;
                ss_advance(next);
            }
        }
        // Draw current slide (possibly fading)
        if (ss_fading) {
            uint32_t fe = tick - ss_fade_tick;
            if (fe >= FADE_TICKS) {
                ss_fading = 0;
                ss_tick = tick;
                draw_slide_full(ss_current, ss_img_w, ss_img_h);
            } else {
                int alpha = fe * 255 / FADE_TICKS;
                if (alpha > 255) alpha = 255;
                draw_blended(ss_old, ss_current, ss_img_w, ss_img_h, alpha);
            }
        } else {
            draw_slide_full(ss_current, ss_img_w, ss_img_h);
        }

        // Overlay bar
        char ov[48]; int oi = 0;
        ov[oi++] = '['; int n = ss_index + 1;
        if (n >= 10) ov[oi++] = '0' + n / 10;
        ov[oi++] = '0' + n % 10; ov[oi++] = '/';
        n = slot_count;
        if (n >= 10) ov[oi++] = '0' + n / 10;
        ov[oi++] = '0' + n % 10; ov[oi++] = ']'; ov[oi++] = ' ';
        int ni = 0;
        while (slots[ss_index].name[ni] && oi < 46) { ov[oi++] = slots[ss_index].name[ni]; ni++; }
        ov[oi] = '\0';
        int bar_h = 24;
        vesa_fill_rect(0, ph - bar_h, pw, bar_h, 0x000811);
        vesa_draw_string(8, ph - bar_h + 5, ov, 0xE8D8FF, 0);
        vesa_draw_string(pw - 14 * 8, ph - bar_h + 5, "S:exit  ESC:grid", 0x8899BB, 0);
        return;
    }

    // ── Grid mode ───────────────────────────────────────────────
    int content_h = ph - TITLE_H;
    // ── Window position (preserves dragged position) ──────────────
    int wx = (g_win_x >= 0) ? g_win_x : 16;
    int wy = (g_win_y >= 0) ? g_win_y : 40;
    int ww = pw - 32;
    int wh = ph - wy - 16;
    g_win_x = wx; g_win_y = wy; g_win_w = ww; g_win_h = wh;

    // Full backdrop so the drag trail shows a solid panel
    vesa_fill_rect(wx, wy, ww, wh, 0x0C0820);

    vesa_fill_rect(wx, wy, ww, TITLE_H, 0x1A1A2E);
    vesa_fill_rect(wx, wy + TITLE_H - 2, ww, 2, 0x3A3A5E);
    draw_close_btn(wx + ww - 32, wy + 9, 0x1A1A2E, "Gallery");
    vesa_draw_string(wx + 12, wy + 9, "Image Gallery", 0xE8D8FF, 0x1A1A2E);
    char cnt[20]; int ci = 0;
    cnt[ci++] = '('; int n = slot_count;
    if (n >= 10) cnt[ci++] = '0' + n / 10;
    cnt[ci++] = '0' + n % 10; cnt[ci++] = ' '; cnt[ci++] = 'i';
    cnt[ci++] = 'm'; cnt[ci++] = 'a'; cnt[ci++] = 'g'; cnt[ci++] = 'e';
    if (n != 1) cnt[ci++] = 's'; cnt[ci++] = ')'; cnt[ci] = '\0';
    vesa_draw_string(wx + ww - 12 - ci * 8, wy + 9, cnt, 0x8899BB, 0x1A1A2E);
    vesa_fill_rect(wx, wy + TITLE_H, ww, content_h, 0x0C0820);

    if (slot_count == 0) {
        vesa_draw_string(wx + (ww-26*8)/2, wy + TITLE_H + content_h/2 - 20, "No images found.", 0x6A5890, 0);
        vesa_draw_string(wx + (ww-36*8)/2, wy + TITLE_H + content_h/2 + 6,
                         "Use 'pack-files.py' to add PNG/JPEG files.", 0x554477, 0);
        vesa_fill_rect(0, ph-18, pw, 18, 0x1A1A2E);
        vesa_draw_string(8, ph-16, "ESC to exit", 0x8899BB, 0x1A1A2E);
        return;
    }

    int col_w = THUMB_W + THUMB_GAP_X;
    int row_h = THUMB_H + LABEL_H + THUMB_GAP_Y;
    int total_w = COLUMNS * col_w - THUMB_GAP_X;
    int start_x = wx + (ww - total_w) / 2;
    if (start_x < wx + 20) start_x = wx + 20;
    int start_y = wy + TITLE_H + 20;
    int vis_h = content_h - 40;

    for (int i = 0; i < slot_count; i++) {
        int col = i % COLUMNS, row = i / COLUMNS;
        int tx = start_x + col * col_w, ty = start_y + row * row_h - scroll_y;
        if (ty + THUMB_H < wy + TITLE_H || ty > wy + wh) continue;
        draw_thumb(tx, ty, &slots[i], i == selected);
    }

    int total_rows = (slot_count + COLUMNS - 1) / COLUMNS;
    int vis_rows = vis_h / row_h;
    if (vis_rows < 1) vis_rows = 1;
    int max_scroll = total_rows * row_h - vis_h;
    if (max_scroll < 0) max_scroll = 0;
    if (max_scroll > 0) {
        int sb_x = wx + ww - 10, sb_h = vis_h;
        int thumb_h = sb_h * vis_rows / total_rows;
        if (thumb_h < 10) thumb_h = 10;
        int thumb_y = start_y + (scroll_y * (sb_h - thumb_h) / max_scroll);
        vesa_fill_rect(sb_x, start_y, 6, sb_h, 0x1A1230);
        vesa_fill_rect(sb_x, thumb_y, 6, thumb_h, 0x6A4FBF);
    }

    int bottom_y = wy + wh - 18;
    vesa_fill_rect(wx, bottom_y, ww, 18, 0x1A1A2E);
    vesa_draw_string(wx + 8, bottom_y + 2,
        "Arrows navigate | ENTER open | S slideshow | ESC exit", 0x8899BB, 0x1A1A2E);
    if (slot_count > 0 && selected >= 0 && selected < slot_count) {
        char info[40]; int ii = 0, si2 = 0;
        while (slots[selected].name[si2] && ii < 38) { info[ii++] = slots[selected].name[si2]; si2++; }
        info[ii] = '\0';
        int iw = 0; while (info[iw]) iw++;
        vesa_draw_string(wx + ww - 12 - iw*8, bottom_y + 2, info, 0xCCDDFF, 0x1A1A2E);
    }
}

// ─── Key handler ─────────────────────────────────────────────────
void gallery_handle_key(char c) {
    if (slot_count == 0) return;
    int cols = COLUMNS; if (cols < 1) cols = 1;

    // Slideshow mode
    if (ss_active) {
        if (c == 27 || c == 's' || c == 'S') { ss_active = 0; ss_fading = 0; }
        return;
    }

    // Grid mode
    if (c == 27) return;
    if (c == 's' || c == 'S') {
        if (slot_count > 0) {
            ss_active = 1; ss_index = selected; ss_fading = 0; ss_tick = tick;
            int sw = 0, sh = 0;
            if (!decode_slot(ss_index, ss_current, &sw, &sh)) { ss_active = 0; }
            else { ss_img_w = sw; ss_img_h = sh; }
        }
        return;
    }
    if (c == 0x48 || c == 'w' || c == 'W') { selected -= cols; if (selected < 0) selected = 0; }
    else if (c == 0x50 || c == 's' || c == 'S') {
        selected += cols; if (selected >= slot_count) selected = slot_count - 1;
    }
    else if (c == 0x4B || c == 'a' || c == 'A') { if (selected > 0) selected--; }
    else if (c == 0x4D || c == 'd' || c == 'D') { if (selected < slot_count-1) selected++; }
    else if (c == 13 || c == '\n' || c == '\r') {
        if (selected >= 0 && selected < slot_count && slots[selected].has_image) {
            int ew = 0, eh = 0;
            if (decode_slot(selected, pixbuf, &ew, &eh)) {
                imgview_init(ew, eh, pixbuf, slots[selected].name);
                imgview_return_mode = MODE_GALLERY;   // ESC returns to Gallery
                current_mode = MODE_IMGVIEW;
            }
        }
        return;
    }

    // Auto-scroll
    int row_h = THUMB_H + LABEL_H + THUMB_GAP_Y;
    int total_rows = (slot_count + COLUMNS - 1) / COLUMNS;
    int vis_h = (int)vesa.height - TITLE_H - 60;
    int max_scroll = total_rows * row_h - vis_h;
    if (max_scroll < 0) max_scroll = 0;
    int sel_row = selected / COLUMNS;
    int sel_y = sel_row * row_h;
    if (sel_y < scroll_y) scroll_y = sel_y;
    else if (sel_y + row_h > scroll_y + vis_h) scroll_y = sel_y + row_h - vis_h;
    if (scroll_y < 0) scroll_y = 0;
    if (scroll_y > max_scroll) scroll_y = max_scroll;
}

// ── Mouse click handler ────────────────────────────────────────────
// Selects a thumbnail and opens the image viewer (same as pressing
// Enter on the keyboard). Returns 1 if a thumbnail was clicked.
int gallery_handle_mouse(int mx, int my) {
    if (!vesa.active || slot_count == 0) return 0;
    int pw = (int)vesa.width, ph = (int)vesa.height;
    int col_w = THUMB_W + THUMB_GAP_X;
    int row_h = THUMB_H + LABEL_H + THUMB_GAP_Y;
    int total_w = COLUMNS * col_w - THUMB_GAP_X;
    int start_x = (pw - total_w) / 2;
    if (start_x < 20) start_x = 20;
    int start_y = TITLE_H + 20;

    for (int i = 0; i < slot_count; i++) {
        int col = i % COLUMNS, row = i / COLUMNS;
        int tx = start_x + col * col_w, ty = start_y + row * row_h - scroll_y;
        if (ty + THUMB_H < TITLE_H || ty > ph) continue;
        // Each thumbnail cell is THUMB_W x (THUMB_H + LABEL_H)
        if (mx >= tx && mx < tx + THUMB_W + THUMB_GAP_X &&
            my >= ty && my < ty + THUMB_H + LABEL_H + THUMB_GAP_Y) {
            selected = i;
            // Same action as pressing Enter — open image viewer
            if (slots[i].has_image) {
                int ew = 0, eh = 0;
                if (decode_slot(i, pixbuf, &ew, &eh)) {
                    imgview_init(ew, eh, pixbuf, slots[i].name);
                    imgview_return_mode = MODE_GALLERY;   // ESC returns to Gallery
                    current_mode = MODE_IMGVIEW;
                }
            }
            return 1;
        }
    }
    return 0;
}
