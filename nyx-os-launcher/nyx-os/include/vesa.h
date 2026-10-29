// include/vesa.h
#ifndef VESA_H
#define VESA_H

#include <stdint.h>

// Multiboot info structure (subset we care about)
typedef struct {
    uint32_t flags;
    uint32_t mem_lower;
    uint32_t mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint8_t  syms[16];
    uint32_t mmap_length;
    uint32_t mmap_addr;
    uint32_t drives_length;
    uint32_t drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;
    uint32_t vbe_control_info;
    uint32_t vbe_mode_info;
    uint16_t vbe_mode;
    uint16_t vbe_interface_seg;
    uint16_t vbe_interface_off;
    uint16_t vbe_interface_len;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t  framebuffer_bpp;
    uint8_t  framebuffer_type;
} __attribute__((packed)) multiboot_info_t;

// VESA state
typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint8_t  bpp;
    uint8_t* fb;      // framebuffer base address (real screen)
    uint8_t* draw;    // active draw target (back buffer when double-buffered)
    int      active;  // 1 if VESA is available, 0 = VGA text fallback
} vesa_info_t;

extern vesa_info_t vesa;

// ─── 8x16 bitmapped font (used by vesa_draw_char and by mouse.c's ─
// ─── ghost overlay — exposed so the IRQ handler can draw labels ─
extern const uint8_t font8x16[96][16];

// Call once after vesa_init() succeeds, before any drawing.
// Allocates a static back buffer and routes all drawing through it.
void vesa_enable_double_buffer(void);

// ─── Overlay plane buffer dimensions (shared with mouse.c) ───────
// These define the offscreen buffers that store snapshot pixels of
// each drag overlay (cell highlight, snap preview, drag ghost).
// vesa_present() and the IRQ handler both read these to stride
// correctly through the pixel buffers.
#define OVLY_CELL_W  128
#define OVLY_CELL_H  128
#define OVLY_PREV_W   72
#define OVLY_PREV_H   88
#define OVLY_GHOST_W  80
#define OVLY_GHOST_H  96

// ─── Overlay plane (2-line compositing) ───────────────────────────
// After the back-buffer copy, vesa_present() composites up to three
// overlay rectangles (cell highlight, snap preview, drag ghost) onto
// vesa.fb.  The IRQ handler snapshots overlay pixels from vesa.fb into
// these offscreen buffers every 200 Hz; vesa_present() then restores
// them, completely eliminating the race where redraw_content() erases
// the overlays from the framebuffer.  The cursor tracks independently
// at 200 Hz via its own save/restore on vesa.fb.
//
// Each overlay has a descriptor (x,y,w,h) where w/h = 0 means inactive.
// The pixel buffer stores pre-converted BGRA uint32_t values matching
// the framebuffer byte order (byte 0=B, 1=G, 2=R, 3=A).
extern int           __ovly_cell_x,  __ovly_cell_y;
extern int           __ovly_cell_w,  __ovly_cell_h;
extern uint32_t*     __ovly_cell_buf;
extern int           __ovly_prev_x,  __ovly_prev_y;
extern int           __ovly_prev_w,  __ovly_prev_h;
extern int           __ovly_prev_fade;   // >0 = fading out, decremented each vesa_present()
extern uint32_t*     __ovly_prev_buf;
extern uint32_t*     __ovly_prev_bg_buf;
extern int           __ovly_ghost_x, __ovly_ghost_y;
extern int           __ovly_ghost_w, __ovly_ghost_h;
extern uint32_t*     __ovly_ghost_buf;

// Copies the back buffer to the real screen in one block operation,
// then composits the overlay plane on top.
// Call this exactly once per frame, after all drawing for that
// frame is complete.
void vesa_present(void);
// Rect-only present for game mode: copies just the game canvas rect
// from the back buffer to the framebuffer (see vesa.c for rationale).
// No overlay compositing — game mode draws no desktop overlays.
void vesa_present_rect(int dx, int dy, int w, int h);

// SSE2 16-byte wide memcpy (64 B per unrolled iteration) for the
// memory-bound frame paths — vesa_present's back-buffer copy.  Requires
// CR4.OSFXSR (set in boot/boot.asm).  Safe on unaligned src/dst;
// reentrant-safe (IRQ handlers never use SSE).
void vesa_copy16(uint8_t* dst, const uint8_t* src, uint32_t bytes);

// Initialize VESA from multiboot info pointer
void vesa_init(multiboot_info_t* mbi);

// Try to switch the display to a new resolution at runtime.
// Uses either the Bochs VBE interface (QEMU) or VBE PM entry.
// Returns 0 on failure (caller should keep the old mode).
// On success, updates vesa_info_t and invalidates the wallpaper cache.
int  vesa_set_mode(uint16_t width, uint16_t height, uint16_t bpp);

// Pixel drawing
void vesa_put_pixel(int x, int y, uint32_t color);
uint32_t vesa_rgb(uint8_t r, uint8_t g, uint8_t b);

// ── RGBA pixel-packing helper ────────────────────────────────────
// Pack separate R, G, B bytes into the 0x00RRGGBB format that
// vesa_put_pixel() expects. Use this instead of hand-coding the
// bit-shift everywhere — it's a single point of truth for correct
// byte ordering and prevents R↔B swap bugs.
#define rgba_pack(r,g,b) (((uint32_t)(r)<<16)|((uint32_t)(g)<<8)|(uint32_t)(b))

// ── BGRA byte-order documentation and compile-time checks ─────────
// The VESA 32bpp framebuffer stores pixels in BGRA order:
//   byte 0 = B (blue), byte 1 = G (green),
//   byte 2 = R (red),  byte 3 = A (alpha = 0xFF).
//
// The color value uint32_t format is 0x00RRGGBB, matching the
// standard vesa_rgb() / rgba_pack() convention. vesa_put_pixel()
// decomposes this into the BGRA byte layout above.
//
// Compile-time assertions verify this encoding never regresses.

// Expected framebuffer bytes for a given RGB color
// (VESA 32bpp: byte 0=B, byte 1=G, byte 2=R, byte 3=A)
typedef struct { uint8_t b, g, r, a; } BgraPixel;

// Build a BgraPixel from R,G,B values (alpha = 255)
#define BGRA_OF(r,g,b) { (uint8_t)(b), (uint8_t)(g), (uint8_t)(r), 255 }

_Static_assert(rgba_pack(255,0,0) == 0xFF0000,
    "rgba_pack(R,0,0): expected 0xFF0000 (R at bits 16-23)");
_Static_assert(rgba_pack(0,255,0) == 0x00FF00,
    "rgba_pack(0,G,0): expected 0x00FF00 (G at bits 8-15)");
_Static_assert(rgba_pack(0,0,255) == 0x0000FF,
    "rgba_pack(0,0,B): expected 0x0000FF (B at bits 0-7)");

// ── Brightness/contrast adjusted RGBA blit ──────────────────────
// Same as vesa_blit_rgba but applies brightness (-100..+100) and
// contrast (-100..+100, 0 = identity) to each pixel during the
// RGBA→BGRA conversion.  Useful for photo viewing with quick
// exposure adjustments.
void vesa_blit_rgba_adjusted(int dx, int dy,
                             const uint8_t* rgba, int src_w, int src_h,
                             int src_x, int src_y, int w, int h,
                             int brightness, int contrast);

// ── In-place framebuffer brightness/contrast adjustment ──────────
// Post-process a region of the framebuffer with brightness (-100..+100)
// and contrast (-100..+100).  Reads BGRA pixels from vesa.draw,
// adjusts them in-place.  Useful for applying exposure tweaks AFTER
// bilinear scaling, avoiding nearest-neighbor artifacts.
void vesa_adjust_fb_rect(int dx, int dy, int w, int h,
                          int brightness, int contrast);

// ── Bulk RGBA blit ───────────────────────────────────────────────
// Copy a rectangle of RGBA pixels (byte 0=R, 1=G, 2=B, 3=A) from
// a source buffer to the VESA framebuffer at (dx, dy).
//
//   rgba      — pointer to the top-left of the RGBA source image
//   src_w     — width of the source image in pixels (for stride)
//   src_h     — height of the source image in pixels
//   src_x, src_y — top-left corner of the source rectangle to copy
//   w, h      — dimensions of the rectangle to copy
//
// Automatically:
//   1. Converts RGBA → BGRA (swaps bytes 0 and 2) for the framebuffer
//   2. Clips the rectangle to both source and framebuffer bounds
//   3. Writes directly to vesa.draw with correct row strides
void vesa_blit_rgba(int dx, int dy,
                    const uint8_t* rgba, int src_w, int src_h,
                    int src_x, int src_y, int w, int h);

// ── Bulk BGRA blit ───────────────────────────────────────────────
// Copy a rectangle of pre-converted BGRA pixels (byte 0=B, 1=G, 2=R,
// 3=A — the native VESA 32bpp format) to the framebuffer at (dx, dy)
// using fast uint32_t word copies without any byte conversion.
//
// Parameters and clipping semantics match vesa_blit_rgba().
void vesa_blit_bgra(int dx, int dy,
                    const uint8_t* bgra, int img_w, int img_h,
                    int src_x, int src_y, int w, int h);

// ── Scaled RGBA blit (nearest-neighbor) ───────────────────────────
// Copy and scale an RGBA source image to (dx, dy, dw, dh) on the
// framebuffer using nearest-neighbor interpolation. The source image
// is treated as a full (src_w × src_h) RGBA buffer; every output
// pixel maps to the closest source pixel (no filtering).
// Automatically converts RGBA → BGRA and clips to framebuffer.
void vesa_blit_rgba_scaled(int dx, int dy, int dw, int dh,
                           const uint8_t* rgba, int src_w, int src_h);

// ── Scaled RGBA blit (bilinear) ───────────────────────────────────
// Same as vesa_blit_rgba_scaled but uses bilinear interpolation for
// smoother upscaling (avoids blocky pixels when a small image fills
// the screen).  Uses fixed-point 16.16 arithmetic — no FPU needed.
void vesa_blit_rgba_scaled_bilinear(int dx, int dy, int dw, int dh,
                                    const uint8_t* rgba, int src_w, int src_h);

// Same as vesa_blit_rgba_scaled_bilinear() but with a 4-tap separable
// Catmull-Rom cubic (bicubic) filter — crisper edges and smoother
// gradients when scaling low-res content, at ~4× the cost.  Used by
// the video player for its once-per-frame letterbox blit.
void vesa_blit_rgba_scaled_bicubic(int dx, int dy, int dw, int dh,
                                   const uint8_t* rgba, int src_w, int src_h);

// ── RGBA crossfade blend ─────────────────────────────────────────
// Blend two RGBA source images into the framebuffer at (dx, dy)
// with dimensions (dw × dh). Each output pixel is a weighted
// average: result = (from * (255-alpha) + to * alpha) / 255.
//
// When (dw, dh) differ from (src_w, src_h), the sources are
// nearest-neighbor scaled during the blend. Automatically
// converts RGBA → BGRA and clips to framebuffer bounds.
void vesa_blend_rgba(int dx, int dy, int dw, int dh,
                     const uint8_t* from, const uint8_t* to,
                     int src_w, int src_h, int alpha);

// ── In-place RGBA→BGRA conversion ────────────────────────────────
// Convert a rectangle of RGBA pixels to BGRA in-place by swapping
// bytes 0 and 2 for every pixel. Useful for converting decoder
// output (RGBA) to VESA-native BGRA in a buffer before blitting.
//
//   buf         — pixel data pointer (top-left of rectangle)
//   stride      — byte stride per row (e.g., parent_width * 4)
//   w, h        — rectangle dimensions in pixels
void vesa_convert_rgba_bgra(uint8_t* buf, int stride, int w, int h);

// ── In-place BGRA→RGBA conversion (macro wrapper) ────────────────
// Convert a rectangle of BGRA pixels to RGBA in-place by swapping
// bytes 0 and 2 — the inverse of vesa_convert_rgba_bgra().
// The swap is its own inverse, so this is a simple alias.
#define vesa_convert_bgra_rgba(buf, stride, w, h) \
    vesa_convert_rgba_bgra((buf), (stride), (w), (h))

// ── Boot-time byte-order self-test ───────────────────────────────
// Writes test pixels through vesa_put_pixel, vesa_blit_rgba,
// vesa_blit_bgra, and vesa_convert_rgba_bgra, then reads back the
// framebuffer bytes to verify every R/G/B channel is in the correct
// BGRA position. Calls vesa_test_panic() with details on mismatch.
// Designed to be called once right after vesa_init() + double-buffer
// enable, before any other drawing occurs.
void vesa_self_test(void);

// Rectangle fill
void vesa_fill_rect(int x, int y, int w, int h, uint32_t color);

// Clear screen
void vesa_clear(uint32_t color);

// Draw a character using built-in 8x16 font
void vesa_draw_char(int x, int y, char c, uint32_t fg, uint32_t bg);

// Draw a string
void vesa_draw_string(int x, int y, const char* s, uint32_t fg, uint32_t bg);

// Draw a horizontal line
void vesa_hline(int x, int y, int len, uint32_t color);

// Draw a vertical line
void vesa_vline(int x, int y, int len, uint32_t color);

// Draw a rectangle outline
void vesa_draw_rect(int x, int y, int w, int h, uint32_t color);

// Common colors
#define VESA_BLACK       0x000000
#define VESA_WHITE       0xFFFFFF
#define VESA_RED         0xFF0000
#define VESA_GREEN       0x00FF00
#define VESA_BLUE        0x0000FF
#define VESA_CYAN        0x00FFFF
#define VESA_MAGENTA     0xFF00FF
#define VESA_YELLOW      0xFFFF00
#define VESA_DARK_GREY   0x444444
#define VESA_LIGHT_GREY  0xAAAAAA
#define VESA_ORANGE      0xFF8800
#define VESA_PURPLE      0x8800FF
#define VESA_NAVY        0x000088
#define VESA_TEAL        0x008888
#define VESA_DARK_BLUE   0x001133
#define VESA_DARK_GREEN  0x003300

_Static_assert(VESA_RED   == 0xFF0000,
    "VESA_RED: expected 0xFF0000");
_Static_assert(VESA_GREEN == 0x00FF00,
    "VESA_GREEN: expected 0x00FF00");
_Static_assert(VESA_BLUE  == 0x0000FF,
    "VESA_BLUE: expected 0x0000FF");
_Static_assert(VESA_CYAN  == 0x00FFFF,
    "VESA_CYAN: expected 0x00FFFF");
_Static_assert(VESA_MAGENTA == 0xFF00FF,
    "VESA_MAGENTA: expected 0xFF00FF");
_Static_assert(VESA_YELLOW == 0xFFFF00,
    "VESA_YELLOW: expected 0xFFFF00");
_Static_assert(VESA_WHITE == 0xFFFFFF,
    "VESA_WHITE: expected 0xFFFFFF");

#endif // VESA_H
