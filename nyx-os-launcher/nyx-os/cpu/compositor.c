// cpu/compositor.c — Software compositor for translucent UI surfaces
//
// Provides a multi-pass separable box blur (Kawase-style), per-pixel
// alpha blending, and drop-shadow rendering — all in software on the
// CPU.  No GPU required; the blur runs as a simple O(n) sliding-window
// sum across horizontal and vertical passes.
//
// The compositor is designed to be lightweight enough for real-time use
// on a 32-bit x86 kernel at 800×600, while producing glass-morphism
// visuals that rival a GPU-accelerated desktop environment.

#include <compositor.h>
#include <vesa.h>
#include <stdint.h>

// ─── Compositor state and blur scratch buffer ──────────────────
// Enabled by default. When disabled, compositor_blit_surface becomes
// a plain memcpy and blur/shadow operations are no-ops.
//
// The scratch buffer is sized for the maximum compositable region
// (up to the full screen width at max panel height).  This is a
// file-level static so both compositor_blur() and
// compositor_blur_region() can share it without separate allocations.
static int compositor_enabled = 1;

// Ping-pong buffer for blur passes and region capture.
// Max surface: 800 wide x 600 tall x 4 bytes = 1,920,000 bytes
#define COMPOSITOR_SCRATCH_BYTES (COMPOSITOR_MAX_SURFACE_W * COMPOSITOR_MAX_SURFACE_H * 4)
static uint8_t compositor_scratch[COMPOSITOR_SCRATCH_BYTES];
static uint8_t compositor_scratch2[COMPOSITOR_SCRATCH_BYTES];

// ─── Surface pixel buffer pool ──────────────────────────────────
// Static pool for all surface pixel buffers. Each surface gets w*h*4
// bytes allocated from this pool on creation, released on destroy.
// 4 MB = ~4 full-screen surfaces or ~11 half-screen surfaces.
#define COMPOSITOR_POOL_SIZE (4 * 1024 * 1024)
static uint8_t compositor_pool[COMPOSITOR_POOL_SIZE];
static uint32_t compositor_pool_used;

// Saved vesa.draw pointer for surface bind/unbind
static uint8_t* compositor_saved_draw;

// ─── Global compositor state ────────────────────────────────────
CompositorState compositor;

void compositor_init(void) {
    compositor_enabled = 1;
    compositor.quality = COMPOSITOR_QUALITY_HIGH;
    compositor.surface_count = 0;
    compositor.current_bind = -1;
    compositor.dirty = 0;
    compositor.shadow_radius = 6;
    compositor.shadow_opacity = 120;
    compositor.decoration_height = 32;
    compositor_pool_used = 0;
    compositor_saved_draw = 0;

    // Clear all surface slots
    for (int i = 0; i < COMPOSITOR_MAX_SURFACES; i++) {
        compositor.surfaces[i].active = 0;
        compositor.surfaces[i].pixels = 0;
    }
}

int compositor_is_enabled(void) {
    return compositor_enabled;
}

void compositor_set_enabled(int enabled) {
    compositor_enabled = enabled ? 1 : 0;
}

// ═══════════════════════════════════════════════════════════════════
//  Backdrop capture
// ═══════════════════════════════════════════════════════════════════
void compositor_capture(int x, int y, int w, int h, uint8_t* out_bgra) {
    if (!vesa.active || !out_bgra || w <= 0 || h <= 0) return;

    // Clip capture region to framebuffer bounds
    int fb_w = (int)vesa.width;
    int fb_h = (int)vesa.height;
    int pitch = (int)vesa.pitch;

    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > fb_w) w = fb_w - x;
    if (y + h > fb_h) h = fb_h - y;
    if (w <= 0 || h <= 0) return;

    int bpp = (int)(vesa.bpp / 8);

    for (int row = 0; row < h; row++) {
        const uint8_t* src = vesa.draw +
            (uint32_t)(y + row) * (uint32_t)pitch +
            (uint32_t)x * (uint32_t)bpp;
        uint8_t* dst = out_bgra + (uint32_t)row * (uint32_t)w * 4;
        for (int col = 0; col < w; col++) {
            dst[col*4 + 0] = src[col*bpp + 0];  // B
            dst[col*4 + 1] = src[col*bpp + 1];  // G
            dst[col*4 + 2] = src[col*bpp + 2];  // R
            dst[col*4 + 3] = 0xFF;               // A (opaque in backdrop)
        }
    }
}

// ═══════════════════════════════════════════════════════════════════
//  Fast separable box blur (3-pass Kawase-style)
// ═══════════════════════════════════════════════════════════════════
// Algorithm:
//   1. Horizontal pass: for each row, slide a window of width (2R+1)
//      across the row, computing the per-channel sum.  Each output
//      pixel is the average of (2R+1) neighboring pixels.
//   2. Vertical pass: same sliding-window approach on columns.
//   3. Repeat horizontal pass for smoother gaussian-like falloff.
//
// Memory: uses a temp buffer of w*4 bytes for the horizontal pass.
//   The vertical pass reads from the temp buffer back into buf.
//   The final horizontal pass reads from buf into the temp buffer
//   and then copies back.
//
// Sliding-window technique avoids O(R) per pixel — each new pixel
// adds one sample and subtracts one sample, making this O(n) total.

// Internal: one horizontal blur pass. Reads from src_w*4 stride,
// writes to dst (w*4 per row).  'w' is the logical pixel width.
static void blur_horiz_pass(const uint8_t* src, int src_stride,
                            uint8_t* dst, int w, int h, int radius) {
    if (radius <= 0 || w <= 0 || h <= 0) {
        // No blur — plain copy
        for (int row = 0; row < h; row++)
            for (int col = 0; col < w; col++)
                for (int ch = 0; ch < 4; ch++)
                    dst[row * w * 4 + col * 4 + ch] =
                        src[row * src_stride + col * 4 + ch];
        return;
    }

    for (int row = 0; row < h; row++) {
        const uint8_t* in_row = src + row * src_stride;
        uint8_t* out_row = dst + row * w * 4;

        // Initialize sliding sums for this row
        uint32_t sum_r = 0, sum_g = 0, sum_b = 0, cnt = 0;

        // Fill the initial window
        for (int k = -radius; k <= radius; k++) {
            int ci = (k < 0) ? 0 : (k >= w ? w - 1 : k);
            sum_b += in_row[ci * 4 + 0];
            sum_g += in_row[ci * 4 + 1];
            sum_r += in_row[ci * 4 + 2];
            cnt++;
        }

        for (int col = 0; col < w; col++) {
            // Write averaged pixel
            out_row[col * 4 + 0] = (uint8_t)(sum_b / cnt);
            out_row[col * 4 + 1] = (uint8_t)(sum_g / cnt);
            out_row[col * 4 + 2] = (uint8_t)(sum_r / cnt);
            out_row[col * 4 + 3] = 0xFF;

            // Slide window: remove left pixel, add right pixel
            int left_idx  = col - radius;
            int right_idx = col + radius + 1;

            if (left_idx >= 0) {
                sum_b -= in_row[left_idx * 4 + 0];
                sum_g -= in_row[left_idx * 4 + 1];
                sum_r -= in_row[left_idx * 4 + 2];
                cnt--;
            }
            if (right_idx < w) {
                sum_b += in_row[right_idx * 4 + 0];
                sum_g += in_row[right_idx * 4 + 1];
                sum_r += in_row[right_idx * 4 + 2];
                cnt++;
            }
        }
    }
}

// Internal: one vertical blur pass. Reads from src (w*4 stride),
// writes to dst (w*h*4 bytes).  'w' and 'h' are the logical dimensions.
static void blur_vert_pass(const uint8_t* src, uint8_t* dst,
                           int w, int h, int radius) {
    if (radius <= 0 || w <= 0 || h <= 0) {
        for (int row = 0; row < h; row++)
            for (int col = 0; col < w; col++)
                for (int ch = 0; ch < 4; ch++)
                    dst[row * w * 4 + col * 4 + ch] =
                        src[row * w * 4 + col * 4 + ch];
        return;
    }

    int stride = w * 4;  // src and dst use w*4 stride (dense rows)

    for (int col = 0; col < w; col++) {
        // Initialize sliding sums for this column
        uint32_t sum_r = 0, sum_g = 0, sum_b = 0, cnt = 0;

        int base_idx = col * 4;

        for (int k = -radius; k <= radius; k++) {
            int ri = (k < 0) ? 0 : (k >= h ? h - 1 : k);
            sum_b += src[ri * stride + base_idx + 0];
            sum_g += src[ri * stride + base_idx + 1];
            sum_r += src[ri * stride + base_idx + 2];
            cnt++;
        }

        for (int row = 0; row < h; row++) {
            uint8_t* out = dst + row * stride + base_idx;
            out[0] = (uint8_t)(sum_b / cnt);
            out[1] = (uint8_t)(sum_g / cnt);
            out[2] = (uint8_t)(sum_r / cnt);
            out[3] = 0xFF;

            int top_idx    = row - radius;
            int bottom_idx = row + radius + 1;

            if (top_idx >= 0) {
                sum_b -= src[top_idx * stride + base_idx + 0];
                sum_g -= src[top_idx * stride + base_idx + 1];
                sum_r -= src[top_idx * stride + base_idx + 2];
                cnt--;
            }
            if (bottom_idx < h) {
                sum_b += src[bottom_idx * stride + base_idx + 0];
                sum_g += src[bottom_idx * stride + base_idx + 1];
                sum_r += src[bottom_idx * stride + base_idx + 2];
                cnt++;
            }
        }
    }
}

void compositor_blur(uint8_t* buf, int w, int h, int stride, int radius) {
    if (!buf || w <= 0 || h <= 0 || radius <= 0) return;
    if (!compositor_enabled) return;

    // temp buffer for the intermediate passes (dense w*4 per row)
    // Maximum surface size is COMPOSITOR_MAX_SURFACE_W * 4 bytes
    // For up to 800px wide, that's 3200 bytes — allocated on stack.
    // The buffer is also used for the second horizontal pass source.
    // Need two rows of temp space for sliding window... actually we
    // need the full blurred image: w*h*4 between passes.
    // For a launcher panel (240x568), that's 240*568*4 = 545,280 bytes.
    // That's too big for the stack! We need a static buffer.

    uint32_t needed = (uint32_t)w * (uint32_t)h * 4u;
    if (needed > COMPOSITOR_SCRATCH_BYTES) return;

    // Use both scratch buffers as ping-pong intermediates.
    // IMPORTANT: t2 is used for the FIRST output pass so that
    // buf is never aliased with the output — the sliding-window
    // horizontal blur reads from buf while writing to t2, and if
    // they were the same buffer the window would read already-
    // overwritten pixels, corrupting the result.
    uint8_t* t1 = compositor_scratch;   // intermediate buffer 1
    uint8_t* t2 = compositor_scratch2;  // intermediate buffer 2

    // Pass 1: horizontal (buf → t2 — never t1, avoids buf==t1 aliasing)
    blur_horiz_pass(buf, stride, t2, w, h, radius);

    // Pass 2: vertical (t2 → t1)
    blur_vert_pass(t2, t1, w, h, radius);

    // Pass 3: horizontal (t1 → t2), then copy t2 → buf
    blur_horiz_pass(t1, w * 4, t2, w, h, radius);
    // Copy back to buf using the original stride
    for (int row = 0; row < h; row++) {
        uint8_t* dst_row = buf + row * stride;
        const uint8_t* src_row = t2 + row * w * 4;
        for (int col = 0; col < w; col++) {
            dst_row[col * 4 + 0] = src_row[col * 4 + 0];
            dst_row[col * 4 + 1] = src_row[col * 4 + 1];
            dst_row[col * 4 + 2] = src_row[col * 4 + 2];
            dst_row[col * 4 + 3] = 0xFF;
        }
    }
}

// ═══════════════════════════════════════════════════════════════════
//  Composite a surface with per-pixel alpha
// ═══════════════════════════════════════════════════════════════════
void compositor_blit_surface(int dx, int dy, int sw, int sh,
                              const uint8_t* surface, int global_alpha) {
    if (!vesa.active || !surface || sw <= 0 || sh <= 0) return;
    if (global_alpha <= 0) return;  // fully transparent → nothing to do

    // When compositor is enabled, always use per-pixel alpha blending
    // so transparent surface pixels (e.g., areas of a full-screen surface
    // outside the active window) correctly show the background behind.
    // The fast opaque path is only taken when compositor is disabled AND
    // the surface is fully opaque — an optimization that skips the
    // per-pixel alpha loop entirely.
    if (!compositor_enabled) {
        if (global_alpha >= 255) {
            vesa_blit_bgra(dx, dy, surface, sw, sh, 0, 0, sw, sh);
        } else {
            // Dimmed blit: blend surface with background
            int ia = 255 - global_alpha;
            int fb_w = (int)vesa.width;
            int fb_h = (int)vesa.height;
            int pitch = (int)vesa.pitch;
            int bpp = (int)(vesa.bpp / 8);

            // Clip
            int src_x = 0, src_y = 0, w = sw, h = sh;
            if (dx < 0) { w += dx; src_x -= dx; dx = 0; }
            if (dy < 0) { h += dy; src_y -= dy; dy = 0; }
            if (dx + w > fb_w) w = fb_w - dx;
            if (dy + h > fb_h) h = fb_h - dy;
            if (w <= 0 || h <= 0) return;

            int src_stride = sw * 4;
            for (int row = 0; row < h; row++) {
                const uint8_t* src = surface +
                    (uint32_t)(src_y + row) * (uint32_t)src_stride +
                    (uint32_t)src_x * 4;
                uint8_t* dst = vesa.draw +
                    (uint32_t)(dy + row) * (uint32_t)pitch +
                    (uint32_t)dx * (uint32_t)bpp;
                for (int col = 0; col < w; col++) {
                    int sb = src[col*4 + 0];
                    int sg = src[col*4 + 1];
                    int sr = src[col*4 + 2];
                    // Blend with background using global_alpha
                    int db = dst[col*bpp + 0];
                    int dg = dst[col*bpp + 1];
                    int dr = dst[col*bpp + 2];
                    dst[col*bpp + 0] = (uint8_t)((db * ia + sb * global_alpha) / 255);
                    dst[col*bpp + 1] = (uint8_t)((dg * ia + sg * global_alpha) / 255);
                    dst[col*bpp + 2] = (uint8_t)((dr * ia + sr * global_alpha) / 255);
                }
            }
        }
        return;
    }

    // ── Full compositor path: per-pixel alpha blending ───────────
    int fb_w = (int)vesa.width;
    int fb_h = (int)vesa.height;
    int pitch = (int)vesa.pitch;
    int bpp = (int)(vesa.bpp / 8);

    // Clip
    int src_x = 0, src_y = 0, w = sw, h = sh;
    if (dx < 0) { w += dx; src_x -= dx; dx = 0; }
    if (dy < 0) { h += dy; src_y -= dy; dy = 0; }
    if (dx + w > fb_w) w = fb_w - dx;
    if (dy + h > fb_h) h = fb_h - dy;
    if (w <= 0 || h <= 0) return;

    int src_stride = sw * 4;

    for (int row = 0; row < h; row++) {
        const uint8_t* src = surface +
            (uint32_t)(src_y + row) * (uint32_t)src_stride +
            (uint32_t)src_x * 4;
        uint8_t* dst = vesa.draw +
            (uint32_t)(dy + row) * (uint32_t)pitch +
            (uint32_t)dx * (uint32_t)bpp;

        for (int col = 0; col < w; col++) {
            int sa = (int)src[col*4 + 3];  // per-pixel alpha (0-255)
            // Apply global alpha modulation
            sa = sa * global_alpha / 255;
            if (sa <= 0) continue;          // transparent pixel
            if (sa >= 255) {
                // Fully opaque — direct copy
                dst[col*bpp + 0] = src[col*4 + 0];
                dst[col*bpp + 1] = src[col*4 + 1];
                dst[col*bpp + 2] = src[col*4 + 2];
            } else {
                // Per-pixel alpha blend
                int ia = 255 - sa;
                int db = dst[col*bpp + 0];
                int dg = dst[col*bpp + 1];
                int dr = dst[col*bpp + 2];
                int sb = src[col*4 + 0];
                int sg = src[col*4 + 1];
                int sr = src[col*4 + 2];
                dst[col*bpp + 0] = (uint8_t)((db * ia + sb * sa) / 255);
                dst[col*bpp + 1] = (uint8_t)((dg * ia + sg * sa) / 255);
                dst[col*bpp + 2] = (uint8_t)((dr * ia + sr * sa) / 255);
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════════
//  Drop shadow
// ═══════════════════════════════════════════════════════════════════
// Draws a soft shadow by spreading an expanding, dimming box blur
// from the rectangle edges.  Uses the compositor's blur on a shadow
// buffer to produce a gaussian-like falloff.
//
// For simplicity, we render the shadow as a series of concentric
// rectangles at decreasing opacity — a fast approximation of a
// true gaussian shadow that avoids a full blur pass.
void compositor_drop_shadow(int x, int y, int w, int h,
                             int spread, int opacity) {
    if (!vesa.active || !compositor_enabled) return;
    if (spread <= 0 || opacity <= 0) return;
    if (opacity > 255) opacity = 255;

    int fb_w = (int)vesa.width;
    int fb_h = (int)vesa.height;
    int pitch = (int)vesa.pitch;
    int bpp = (int)(vesa.bpp / 8);

    // Render concentric rectangles outward from the surface edges,
    // each layer at progressively lower opacity.
    // The shadow area extends 'spread' pixels in all directions.
    for (int s = 1; s <= spread; s++) {
        // Opacity falls off linearly with distance
        int layer_op = opacity * (spread - s + 1) / (spread * 2);
        if (layer_op <= 0) continue;

        // Shadow rectangle expanded by 's' pixels outward
        int sx = x - s;
        int sy = y - s;
        int sw_ = w + s * 2;
        int sh_ = h + s * 2;

        // Clip to framebuffer
        int draw_x = sx < 0 ? 0 : sx;
        int draw_y = sy < 0 ? 0 : sy;
        int draw_w = sw_;
        int draw_h = sh_;
        if (draw_x + draw_w > fb_w) draw_w = fb_w - draw_x;
        if (draw_y + draw_h > fb_h) draw_h = fb_h - draw_y;
        if (draw_w <= 0 || draw_h <= 0) continue;

        // Fill the entire expanded rectangle with the shadow layer.
        // We need to skip the interior (the surface area itself) so
        // the shadow only appears OUTSIDE the surface edges.

        // Top strip (above the surface)
        int top_h = (sy < y) ? (y - draw_y) : 0;
        if (top_h > 0) {
            for (int row = 0; row < top_h; row++) {
                uint8_t* dst = vesa.draw +
                    (uint32_t)(draw_y + row) * (uint32_t)pitch +
                    (uint32_t)draw_x * (uint32_t)bpp;
                for (int col = 0; col < draw_w; col++) {
                    int db = dst[col*bpp + 0];
                    int dg = dst[col*bpp + 1];
                    int dr = dst[col*bpp + 2];
                    int ia = 255 - layer_op;
                    dst[col*bpp + 0] = (uint8_t)((db * ia) / 255);
                    dst[col*bpp + 1] = (uint8_t)((dg * ia) / 255);
                    dst[col*bpp + 2] = (uint8_t)((dr * ia) / 255);
                }
            }
        }

        // Bottom strip (below the surface)
        int bottom_start = (y + h > draw_y) ? (y + h - draw_y) : draw_h;
        int bottom_h = draw_h - bottom_start;
        if (bottom_h > 0) {
            for (int row = 0; row < bottom_h; row++) {
                uint8_t* dst = vesa.draw +
                    (uint32_t)(draw_y + bottom_start + row) * (uint32_t)pitch +
                    (uint32_t)draw_x * (uint32_t)bpp;
                for (int col = 0; col < draw_w; col++) {
                    int db = dst[col*bpp + 0];
                    int dg = dst[col*bpp + 1];
                    int dr = dst[col*bpp + 2];
                    int ia = 255 - layer_op;
                    dst[col*bpp + 0] = (uint8_t)((db * ia) / 255);
                    dst[col*bpp + 1] = (uint8_t)((dg * ia) / 255);
                    dst[col*bpp + 2] = (uint8_t)((dr * ia) / 255);
                }
            }
        }

        // Left strip (between top and bottom, outside surface left edge)
        int mid_y_start = (y > draw_y) ? (y - draw_y) : 0;
        int mid_y_end = ((y + h) < (draw_y + draw_h)) ?
                         (y + h - draw_y) : draw_h;
        if (mid_y_start < mid_y_end) {
            int left_w = (sx < x) ? (x - draw_x) : 0;
            if (left_w > 0) {
                for (int row = mid_y_start; row < mid_y_end; row++) {
                    uint8_t* dst = vesa.draw +
                        (uint32_t)(draw_y + row) * (uint32_t)pitch +
                        (uint32_t)draw_x * (uint32_t)bpp;
                    for (int col = 0; col < left_w; col++) {
                        int db = dst[col*bpp + 0];
                        int dg = dst[col*bpp + 1];
                        int dr = dst[col*bpp + 2];
                        int ia = 255 - layer_op;
                        dst[col*bpp + 0] = (uint8_t)((db * ia) / 255);
                        dst[col*bpp + 1] = (uint8_t)((dg * ia) / 255);
                        dst[col*bpp + 2] = (uint8_t)((dr * ia) / 255);
                    }
                }
            }

            // Right strip (outside surface right edge)
            int right_start = (x + w > draw_x) ? (x + w - draw_x) : draw_w;
            int right_w = draw_w - right_start;
            if (right_w > 0) {
                for (int row = mid_y_start; row < mid_y_end; row++) {
                    uint8_t* dst = vesa.draw +
                        (uint32_t)(draw_y + row) * (uint32_t)pitch +
                        (uint32_t)(draw_x + right_start) * (uint32_t)bpp;
                    for (int col = 0; col < right_w; col++) {
                        int db = dst[col*bpp + 0];
                        int dg = dst[col*bpp + 1];
                        int dr = dst[col*bpp + 2];
                        int ia = 255 - layer_op;
                        dst[col*bpp + 0] = (uint8_t)((db * ia) / 255);
                        dst[col*bpp + 1] = (uint8_t)((dg * ia) / 255);
                        dst[col*bpp + 2] = (uint8_t)((dr * ia) / 255);
                    }
                }
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════════
//  Blur region in-place on the draw buffer
// ═══════════════════════════════════════════════════════════════════
// Captures (x, y, w, h) from the framebuffer, blurs it, dims it,
// and writes it back.  The region is then ready for UI drawing.
void compositor_blur_region(int x, int y, int w, int h, int radius, int dim) {
    if (!vesa.active || !compositor_enabled) return;
    if (w <= 0 || h <= 0) return;

    // Clip to framebuffer
    int fb_w = (int)vesa.width;
    int fb_h = (int)vesa.height;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > fb_w) w = fb_w - x;
    if (y + h > fb_h) h = fb_h - y;
    if (w <= 0 || h <= 0) return;

    // Capture the region into scratch buffer
    uint32_t needed = (uint32_t)w * (uint32_t)h * 4u;
    if (needed > COMPOSITOR_SCRATCH_BYTES) return;

    uint8_t* buf = compositor_scratch;
    compositor_capture(x, y, w, h, buf);

    // Blur it
    compositor_blur(buf, w, h, w * 4, radius);

    // Apply dim
    if (dim > 0) {
        int pitch_w = w * 4;
        for (int row = 0; row < h; row++) {
            uint8_t* p = buf + row * pitch_w;
            for (int col = 0; col < w; col++) {
                int b = (int)p[0] * (255 - dim) / 255;
                int g = (int)p[1] * (255 - dim) / 255;
                int r = (int)p[2] * (255 - dim) / 255;
                p[0] = (uint8_t)b;
                p[1] = (uint8_t)g;
                p[2] = (uint8_t)r;
                p += 4;
            }
        }
    }

    // Write back to the VESA draw buffer
    int pitch = (int)vesa.pitch;
    int bpp = (int)(vesa.bpp / 8);
    for (int row = 0; row < h; row++) {
        uint8_t* dst = vesa.draw +
            (uint32_t)(y + row) * (uint32_t)pitch +
            (uint32_t)x * (uint32_t)bpp;
        const uint8_t* src = buf + (uint32_t)row * (uint32_t)w * 4;
        for (int col = 0; col < w; col++) {
            dst[col*bpp + 0] = src[col*4 + 0];
            dst[col*bpp + 1] = src[col*4 + 1];
            dst[col*bpp + 2] = src[col*4 + 2];
        }
    }
}

// ═══════════════════════════════════════════════════════════════════
//  Fill a BGRA surface with a solid translucent color
// ═══════════════════════════════════════════════════════════════════
void compositor_fill_surface(uint8_t* buf, int w, int h,
                              uint8_t r, uint8_t g, uint8_t b, int opacity) {
    if (!buf || w <= 0 || h <= 0) return;
    if (opacity < 0) opacity = 0;
    if (opacity > 255) opacity = 255;

    for (int row = 0; row < h; row++) {
        uint8_t* p = buf + (uint32_t)row * (uint32_t)w * 4;
        for (int col = 0; col < w; col++) {
            p[0] = b;          // BGRA: B
            p[1] = g;          // BGRA: G
            p[2] = r;          // BGRA: R
            p[3] = (uint8_t)opacity;  // A
            p += 4;
        }
    }
}

// ═══════════════════════════════════════════════════════════════════
//  Surface management — create
// ═══════════════════════════════════════════════════════════════════
int compositor_surface_create(int x, int y, int w, int h, int z,
                               const char* title, uint32_t accent) {
    if (w <= 0 || h <= 0) return -1;
    if (w > COMPOSITOR_MAX_SURFACE_W) w = COMPOSITOR_MAX_SURFACE_W;
    if (h > COMPOSITOR_MAX_SURFACE_H) h = COMPOSITOR_MAX_SURFACE_H;

    // Find a free slot
    int slot = -1;
    for (int i = 0; i < COMPOSITOR_MAX_SURFACES; i++) {
        if (!compositor.surfaces[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return -1;

    // Allocate pixel buffer from the pool
    uint32_t needed = (uint32_t)w * (uint32_t)h * 4u;
    if (compositor_pool_used + needed > COMPOSITOR_POOL_SIZE) return -1;

    CompositorSurface* surf = &compositor.surfaces[slot];
    surf->pixels = compositor_pool + compositor_pool_used;
    compositor_pool_used += needed;

    surf->w         = w;
    surf->h         = h;
    surf->x         = x;
    surf->y         = y;
    surf->z         = z;
    surf->alpha     = 255;
    surf->visible   = 1;
    surf->active    = 1;
    surf->scale     = 1;
    surf->has_shadow = 1;
    surf->has_blur  = 0;
    surf->decorated = 1;
    surf->moving    = 0;
    surf->move_off_x = 0;
    surf->move_off_y = 0;
    surf->accent    = accent;
    surf->body_color = 0x140C30;  // default deep purple

    // Copy title string
    int ti = 0;
    while (title[ti] && ti < COMPOSITOR_WINDOW_TITLE_LEN - 1) {
        surf->title[ti] = title[ti];
        ti++;
    }
    surf->title[ti] = '\0';

    // Clear the pixel buffer to transparent
    for (uint32_t i = 0; i < needed; i++)
        surf->pixels[i] = 0;

    compositor.surface_count++;
    compositor.dirty = 1;
    return slot;
}

// ═══════════════════════════════════════════════════════════════════
//  Surface management — destroy
// ═══════════════════════════════════════════════════════════════════
void compositor_surface_destroy(int slot) {
    if (slot < 0 || slot >= COMPOSITOR_MAX_SURFACES) return;
    CompositorSurface* surf = &compositor.surfaces[slot];
    if (!surf->active) return;

    // If this surface is currently bound, unbind it first
    if (compositor.current_bind == slot)
        compositor_surface_unbind();

    surf->active = 0;
    surf->pixels = 0;
    compositor.surface_count--;
    compositor.dirty = 1;
    // Note: we don't reclaim pool memory — the slot is just recycled
    // and a new surface will allocate from the current pool offset.
}

// ═══════════════════════════════════════════════════════════════════
//  Surface management — bind / unbind
// ═══════════════════════════════════════════════════════════════════
void compositor_surface_bind(int slot) {
    if (slot < 0 || slot >= COMPOSITOR_MAX_SURFACES) return;
    CompositorSurface* surf = &compositor.surfaces[slot];
    if (!surf->active || !surf->pixels) return;

    // Save the current vesa.draw and redirect to the surface buffer.
    // The surface is expected to be the same size as the framebuffer
    // (full-screen surface), so vesa.pitch and vesa.width stay valid.
    compositor_saved_draw = vesa.draw;
    vesa.draw = surf->pixels;
    compositor.current_bind = slot;
}

void compositor_surface_unbind(void) {
    if (compositor.current_bind < 0) return;
    if (compositor_saved_draw)
        vesa.draw = compositor_saved_draw;
    compositor.current_bind = -1;
    compositor_saved_draw = 0;
}

int compositor_current_slot(void) {
    return compositor.current_bind;
}

// ═══════════════════════════════════════════════════════════════════
//  Window controls
// ═══════════════════════════════════════════════════════════════════
void compositor_surface_move_to(int slot, int x, int y) {
    if (slot < 0 || slot >= COMPOSITOR_MAX_SURFACES) return;
    CompositorSurface* surf = &compositor.surfaces[slot];
    if (!surf->active) return;
    surf->x = x;
    surf->y = y;
    compositor.dirty = 1;
}

void compositor_surface_set_z(int slot, int z) {
    if (slot < 0 || slot >= COMPOSITOR_MAX_SURFACES) return;
    CompositorSurface* surf = &compositor.surfaces[slot];
    if (!surf->active) return;
    surf->z = z;
    compositor.dirty = 1;
}

void compositor_surface_set_alpha(int slot, int alpha) {
    if (slot < 0 || slot >= COMPOSITOR_MAX_SURFACES) return;
    if (alpha < 0) alpha = 0;
    if (alpha > 255) alpha = 255;
    CompositorSurface* surf = &compositor.surfaces[slot];
    if (!surf->active) return;
    surf->alpha = alpha;
    compositor.dirty = 1;
}

void compositor_surface_set_body_color(int slot, uint32_t color) {
    if (slot < 0 || slot >= COMPOSITOR_MAX_SURFACES) return;
    CompositorSurface* surf = &compositor.surfaces[slot];
    if (!surf->active) return;
    surf->body_color = color & 0xFFFFFF;
    compositor.dirty = 1;
}

void compositor_surface_set_visible(int slot, int visible) {
    if (slot < 0 || slot >= COMPOSITOR_MAX_SURFACES) return;
    CompositorSurface* surf = &compositor.surfaces[slot];
    if (!surf->active) return;
    surf->visible = visible ? 1 : 0;
    compositor.dirty = 1;
}

// ═══════════════════════════════════════════════════════════════════
//  Quality control
// ═══════════════════════════════════════════════════════════════════
void compositor_set_quality(int q) {
    if (q < COMPOSITOR_QUALITY_LOW) q = COMPOSITOR_QUALITY_LOW;
    if (q > COMPOSITOR_QUALITY_HIGH) q = COMPOSITOR_QUALITY_HIGH;
    compositor.quality = q;
    compositor.dirty = 1;
}

int compositor_get_quality(void) {
    return compositor.quality;
}

void compositor_cycle_quality(void) {
    compositor.quality++;
    if (compositor.quality > COMPOSITOR_QUALITY_HIGH)
        compositor.quality = COMPOSITOR_QUALITY_LOW;
    compositor.dirty = 1;
}

// ═══════════════════════════════════════════════════════════════════
//  Decoration rendering — title bar + close button
// ═══════════════════════════════════════════════════════════════════
// Draws a gradient title bar with the surface's accent color, a
// close button, and the window title.  Called by compositor_composite()
// when the surface has decorated=1.
void compositor_draw_decorations(int slot) {
    if (slot < 0 || slot >= COMPOSITOR_MAX_SURFACES) return;
    CompositorSurface* surf = &compositor.surfaces[slot];
    if (!surf->active || !surf->visible || !surf->decorated) return;

    int dh = compositor.decoration_height;
    int x = surf->x;
    int y = surf->y - dh;  // title bar sits above the surface
    int w = surf->w;

    // Clip to screen
    if (y < 0) { dh += y; y = 0; if (dh <= 0) return; }

    // Title bar background — accent-to-dark gradient
    uint32_t ar = (surf->accent >> 16) & 0xFF;
    uint32_t ag = (surf->accent >> 8) & 0xFF;
    uint32_t ab = surf->accent & 0xFF;
    for (int yy = 0; yy < dh; yy++) {
        int prog = yy * 255 / dh;
        uint32_t r = (ar * (255 - prog) + 0x18 * prog) / 255;
        uint32_t g = (ag * (255 - prog) + 0x10 * prog) / 255;
        uint32_t b = (ab * (255 - prog) + 0x38 * prog) / 255;
        vesa_hline(x, y + yy, w, (r << 16) | (g << 8) | b);
    }

    // Bottom highlight line
    vesa_hline(x, y + dh - 1, w, 0xB088FF);
    // Top highlight line
    vesa_hline(x, y, w, 0x4A3870);

    // Title text (centered-left, 16px from edge)
    vesa_draw_string(x + 16, y + (dh - 16) / 2, surf->title, 0xE8D8FF, 0);

    // Close button [X]
    vesa_draw_string(x + w - 32, y + (dh - 16) / 2, "[X]", 0xFF6B6B, 0);
}

// ═══════════════════════════════════════════════════════════════════
//  Shadow rendering for a surface slot
// ═══════════════════════════════════════════════════════════════════
// Delegates to compositor_drop_shadow() using the surface's position
// and dimensions, and the compositor's global shadow settings.
void compositor_draw_shadow(int slot) {
    if (slot < 0 || slot >= COMPOSITOR_MAX_SURFACES) return;
    CompositorSurface* surf = &compositor.surfaces[slot];
    if (!surf->active || !surf->visible || !surf->has_shadow) return;

    compositor_drop_shadow(surf->x, surf->y, surf->w, surf->h,
                           compositor.shadow_radius,
                           compositor.shadow_opacity);
}

// ═══════════════════════════════════════════════════════════════════
//  Compositor — blend all visible surfaces onto the back buffer
// ═══════════════════════════════════════════════════════════════════
// Called after all apps have drawn to their surfaces.  Iterates
// surfaces in z-order and blends each one onto the VESA back buffer
// with per-window alpha, drop shadows, and decorations.
void compositor_composite(void) {
    if (!compositor_enabled || !vesa.active) return;

    // Find the maximum z-order among visible surfaces so we can
    // iterate z = 0 .. max_z and render lower z first (bottom-up).
    int max_z = 0;
    int have_visible = 0;
    for (int i = 0; i < COMPOSITOR_MAX_SURFACES; i++) {
        if (compositor.surfaces[i].active && compositor.surfaces[i].visible) {
            have_visible = 1;
            if (compositor.surfaces[i].z > max_z)
                max_z = compositor.surfaces[i].z;
        }
    }
    if (!have_visible) return;

    // Composite in z-order (lowest z first = drawn first = behind)
    for (int z = 0; z <= max_z; z++) {
        for (int i = 0; i < COMPOSITOR_MAX_SURFACES; i++) {
            CompositorSurface* surf = &compositor.surfaces[i];
            if (!surf->active || !surf->visible) continue;
            if (surf->z != z) continue;

            // Step a: Drop shadow (if quality >= MEDIUM)
            if (surf->has_shadow && compositor.quality >= COMPOSITOR_QUALITY_MEDIUM) {
                compositor_drop_shadow(surf->x, surf->y, surf->w, surf->h,
                                       compositor.shadow_radius,
                                       compositor.shadow_opacity);
            }

            // Step a2: Window body background + border (if quality >= MEDIUM)
            // The body is a vertical gradient from surf->body_color (top) to
            // a darker derived shade (bottom), with a thin accent border.
            // This fills the framebuffer area where the window sits, so the
            // surface content (blitted in Step d) renders on top with
            // per-pixel alpha, and transparent surface pixels show the
            // body gradient through.
            if (surf->decorated && compositor.quality >= COMPOSITOR_QUALITY_MEDIUM) {
                int br = (surf->body_color >> 16) & 0xFF;
                int bg_ = (surf->body_color >> 8) & 0xFF;
                int bb = surf->body_color & 0xFF;
                // Bottom color: darken by ~30%
                int dr = br * 70 / 100;
                int dg = bg_ * 70 / 100;
                int db = bb * 70 / 100;
                for (int yy = 0; yy < surf->h; yy++) {
                    int prog = yy * 255 / surf->h;
                    int r = (br * (255 - prog) + dr * prog) / 255;
                    int g = (bg_ * (255 - prog) + dg * prog) / 255;
                    int b = (bb * (255 - prog) + db * prog) / 255;
                    vesa_hline(surf->x, surf->y + yy, surf->w, (r << 16) | (g << 8) | b);
                }
                // Thin rounded border
                vesa_hline(surf->x + 2, surf->y, surf->w - 4, 0x6A4FBF);
                vesa_hline(surf->x + 2, surf->y + surf->h - 1, surf->w - 4, 0x6A4FBF);
                vesa_vline(surf->x, surf->y + 2, surf->h - 4, 0x6A4FBF);
                vesa_vline(surf->x + surf->w - 1, surf->y + 2, surf->h - 4, 0x6A4FBF);
                vesa_put_pixel(surf->x + 1, surf->y + 1, 0x6A4FBF);
                vesa_put_pixel(surf->x + surf->w - 2, surf->y + 1, 0x6A4FBF);
                vesa_put_pixel(surf->x + 1, surf->y + surf->h - 2, 0x6A4FBF);
                vesa_put_pixel(surf->x + surf->w - 2, surf->y + surf->h - 2, 0x6A4FBF);
            }

            // Step b: Background blur behind the title bar (if quality == HIGH)
            if (surf->decorated && surf->has_blur && compositor.quality >= COMPOSITOR_QUALITY_HIGH) {
                compositor_blur_region(surf->x, surf->y - compositor.decoration_height,
                                       surf->w, compositor.decoration_height, 4, 40);
            }

            // Step c: Blend surface pixels with per-window alpha
            compositor_blit_surface(surf->x, surf->y, surf->w, surf->h,
                                     surf->pixels, surf->alpha);

            // Step d: Window decorations (title bar, close button)
            if (surf->decorated && compositor.quality >= COMPOSITOR_QUALITY_MEDIUM) {
                compositor_draw_decorations(i);
            }
        }
    }

    compositor.dirty = 0;
}
