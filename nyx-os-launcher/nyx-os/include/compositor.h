// include/compositor.h — Nyx OS Compositor
//
// A software compositor that renders each app window as an independent
// off-screen surface. Surfaces are composited onto the framebuffer in
// z-order with alpha blending, drop shadows, and optional background
// blur. Designed for CPU-only rendering (no GPU available in QEMU).
//
// Quality levels let the compositor disable expensive effects on
// low-end hardware — automatically or manually.
//
// Surface lifecycle (for desktop.c):
//   1. compositor_init()           — once at boot
//   2. compositor_surface_create() — when an app launches
//   3. compositor_surface_bind(slot)
//   4. App draws to vesa.draw (redirected to surface pixels)
//   5. compositor_surface_unbind()
//   6. compositor_composite()      — blends all surfaces onto back buffer
//   7. vesa_present()              — flips to screen

#ifndef COMPOSITOR_H
#define COMPOSITOR_H

#include <stdint.h>

// ─── Quality levels ──────────────────────────────────────────────
#define COMPOSITOR_QUALITY_LOW     0  // No effects (fastest)
#define COMPOSITOR_QUALITY_MEDIUM  1  // Shadows only
#define COMPOSITOR_QUALITY_HIGH    2  // Shadows + blur + transparency

// ─── Surface constants ───────────────────────────────────────────
#define COMPOSITOR_MAX_SURFACES      16
#define COMPOSITOR_MAX_SURFACE_W     800   // Max compositable width  (full screen)
#define COMPOSITOR_MAX_SURFACE_H     600   // Max compositable height (full screen)
#define COMPOSITOR_WINDOW_TITLE_LEN  28

// ─── Surface structure ───────────────────────────────────────────
// Each app window gets one of these. The compositor composites all
// visible surfaces onto the framebuffer in z-order.
typedef struct {
    uint8_t* pixels;         // BGRA pixel buffer (VESA-native byte order)
    int      w, h;           // Surface dimensions
    int      x, y;           // Position on screen
    int      z;              // Z-order (higher = on top)
    int      alpha;          // Per-window opacity 0–255
    int      visible;        // 0 = hidden / closed
    int      active;         // 1 = this slot is in use
    float    scale;          // Render scale (1.0 = native, 0.75 = 75% size)
    int      has_shadow;     // Show drop shadow
    int      has_blur;       // Blur background behind this window
    int      decorated;      // Show title bar + close button
    char     title[COMPOSITOR_WINDOW_TITLE_LEN];
    uint32_t accent;         // Title bar accent color (0xRRGGBB)
    uint32_t body_color;     // Window body gradient top color (0xRRGGBB)
    int      moving;         // 1 = being dragged by title bar
    int      move_off_x;     // Cursor offset during drag
    int      move_off_y;
} CompositorSurface;

// ─── Global compositor state ─────────────────────────────────────
typedef struct {
    CompositorSurface surfaces[COMPOSITOR_MAX_SURFACES];
    int    quality;            // COMPOSITOR_QUALITY_*
    int    surface_count;      // Number of active surfaces
    int    current_bind;       // Surface slot currently bound for rendering
    int    dirty;              // 1 = composited frame pending
    int    shadow_radius;      // Drop shadow softness in pixels (0–12)
    int    shadow_opacity;     // Shadow darkness 0–255
    int    decoration_height;  // Title bar height in pixels
} CompositorState;

extern CompositorState compositor;

// ─── Initialization ──────────────────────────────────────────────
// Called once after VESA double-buffer is enabled. Allocates the
// compositor's working buffer and resets surface pool.
void compositor_init(void);

// ─── Surface management ──────────────────────────────────────────
// Allocate a new surface. Returns slot index (0..MAX-1) or -1 on
// failure. The surface starts at (x, y) with the given dimensions,
// z-order, title, and accent color. Initially visible.
int  compositor_surface_create(int x, int y, int w, int h, int z,
                                const char* title, uint32_t accent);

// Free a surface by slot index.
void compositor_surface_destroy(int slot);

// ─── Rendering redirect ──────────────────────────────────────────
// Bind a surface so that all vesa_* drawing calls write to the
// surface's pixel buffer instead of the main framebuffer.
// The caller MUST call compositor_surface_unbind() after done.
void compositor_surface_bind(int slot);

// Restore vesa.draw to the compositor's working buffer.
void compositor_surface_unbind(void);

// Get the currently bound surface slot, or -1 if none.
int  compositor_current_slot(void);

// ─── Compositing ─────────────────────────────────────────────────
// Composite all visible surfaces onto the VESA back buffer:
//   1. Wallpaper pixels stay as-is (drawn by desktop.c first)
//   2. For each surface in z-order:
//      a. Generate and blend drop shadow (if quality >= MEDIUM)
//      b. Generate and blend background blur (if quality == HIGH)
//      c. Blend surface pixels with per-window alpha
//      d. Draw window decorations (title bar, close button)
// This is the main rendering step — call after all apps have drawn
// to their surfaces but before vesa_present().
void compositor_composite(void);

// ─── Window controls ─────────────────────────────────────────────
// Move a surface (e.g. during window drag).
void compositor_surface_move_to(int slot, int x, int y);

// Set its z-order (bring to front).
void compositor_surface_set_z(int slot, int z);

// Set per-window transparency.
void compositor_surface_set_alpha(int slot, int alpha);

// Show or hide a surface.
void compositor_surface_set_visible(int slot, int visible);

// Set the window body background gradient top color (0xRRGGBB).
// The compositor derives a darker bottom color automatically.
void compositor_surface_set_body_color(int slot, uint32_t color);

// ─── Quality control ─────────────────────────────────────────────
void compositor_set_quality(int q);
int  compositor_get_quality(void);

// Toggle quality (LOW→MEDIUM→HIGH→LOW) for runtime switching.
void compositor_cycle_quality(void);

// ─── Decoration rendering helpers ────────────────────────────────
// These are called by compositor_composite() internally, but exposed
// so individual apps can customise their title bars if desired.
void compositor_draw_decorations(int slot);
void compositor_draw_shadow(int slot);

// ─── Compositor enable/disable ─────────────────────────────────────
int  compositor_is_enabled(void);
void compositor_set_enabled(int enabled);

// ─── Backdrop capture ──────────────────────────────────────────────
// Captures a rectangular region from the framebuffer into a BGRA
// pixel buffer (out_bgra must be at least w*h*4 bytes). Used by the
// blur functions internally.
void compositor_capture(int x, int y, int w, int h, uint8_t* out_bgra);

// ─── Blur ─────────────────────────────────────────────────────────
// In-place separable box blur (3-pass Kawase-style) on a BGRA buffer.
// buf: pixel data, stride: bytes per row (>= w*4).
void compositor_blur(uint8_t* buf, int w, int h, int stride, int radius);

// ─── Region blur (glassmorphism helper) ────────────────────────────
// Captures (x,y,w,h) from the VESA backbuffer, blurs it with the given
// radius, optionally dims it, and writes it back.  Uses the internal
// scratch buffer.  The caller then draws content on top.
void compositor_blur_region(int x, int y, int w, int h, int radius, int dim);

// ─── Drop shadow ───────────────────────────────────────────────────
// Draws a soft box shadow around a rectangle by rendering concentric
// rectangles at decreasing opacity outward from the edges.
// 'spread' is the shadow width in pixels, 'opacity' is 0-255.
void compositor_drop_shadow(int x, int y, int w, int h, int spread, int opacity);

// ─── Alpha-blended blit ───────────────────────────────────────────
// Blits a BGRA surface to the VESA backbuffer with per-pixel alpha
// blending and a global alpha modulator (0-255).
void compositor_blit_surface(int dx, int dy, int sw, int sh,
                              const uint8_t* surface, int global_alpha);

// ─── Surface fill ─────────────────────────────────────────────────
// Fills a BGRA surface buffer with a solid translucent color.
void compositor_fill_surface(uint8_t* buf, int w, int h,
                              uint8_t r, uint8_t g, uint8_t b, int opacity);

#endif // COMPOSITOR_H
