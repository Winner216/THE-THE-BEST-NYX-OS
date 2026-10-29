// cpu/vesa.c  — VESA/VBE framebuffer driver for Nyx OS
// Works alongside the existing VGA text mode.
// When GRUB sets up a linear framebuffer (via multiboot flags),
// this driver provides pixel-level drawing primitives.

#include <vesa.h>
#include <vbe.h>
#include <wallpaper.h>
#include <stdint.h>
#include <io.h>

// Present-generation counters (defined in cpu/mouse.c).  vesa_present()
// bumps them at the END of the back-buffer copy, which wipes every
// IRQ-drawn overlay (selection box, cursor background) off vesa.fb.  The
// mouse IRQ handler's gen-gated box-erase / cursor-restore then treats
// any pre-present state as stale and skips it — so a PS/2 packet that
// arrives between the wipe and the state invalidation can't XOR-tint
// clean desktop pixels (phantom box) or paint stale pre-present pixels
// (stale cursor copy).
extern int sel_box_present_gen;
extern int cursor_present_gen;
// Set by vesa_present() for the duration of the back-buffer copy +
// overlay compositing, so the mouse IRQ handler (200 Hz) skips drawing
// overlays that would be torn by the overwrite.  Defined in cpu/mouse.c.
extern volatile int __present_in_progress;

vesa_info_t vesa = {0};

// ─── Double buffer ────────────────────────────────────────────────
// Static back buffer in .bss, sized for up to 1920x1080x32bpp
// (8,294,400 bytes ~= 8MB). Covers 1080p with a small safety margin.
#define VESA_BACKBUF_MAX_BYTES (1920u * 1080u * 4u)
// 64 B-aligned back buffer — its row pitch is a 64 B multiple at
// every supported mode (e.g. 1920×4=7680 = 120 lines), so aligned
// rows keep every blit/fill streaming whole cache lines.  NOTE: the
// visible vesa.fb is the physical VBE framebuffer at a hardware-fixed
// address and CANNOT be aligned — only the back buffer can, and that
// is the buffer all blits/fills read and write.
static uint8_t vesa_backbuf[VESA_BACKBUF_MAX_BYTES] __attribute__((aligned(64)));

void vesa_enable_double_buffer(void) {
    if (!vesa.active) return;
    uint32_t needed = vesa.pitch * vesa.height;
    if (needed > VESA_BACKBUF_MAX_BYTES) {
        // Buffer too small for this mode -- stay single-buffered
        // rather than overflow into adjacent memory.
        return;
    }
    vesa.draw = vesa_backbuf;
}

// ─── Overlay plane compositing helper ────────────────────────────
// Composite an overlay rectangle from an offscreen BGRA buffer onto
// the visible framebuffer (vesa.fb).  Zero-pixels are skipped (they
// represent transparent/empty areas in the overlay plane).
static void ovly_composite(uint32_t* buf, int buf_stride,
                            int dx, int dy, int w, int h) {
    if (w <= 0 || h <= 0) return;
    int fb_w = (int)vesa.width;
    int fb_h = (int)vesa.height;
    int pitch = (int)vesa.pitch;
    int bpp = (int)(vesa.bpp / 8);
    // Clip destination to framebuffer
    if (dx < 0) { w += dx; dx = 0; }
    if (dy < 0) { h += dy; dy = 0; }
    if (dx + w > fb_w) w = fb_w - dx;
    if (dy + h > fb_h) h = fb_h - dy;
    if (w <= 0 || h <= 0) return;
    for (int row = 0; row < h; row++) {
        uint32_t* src = buf + (uint32_t)row * (uint32_t)buf_stride;
        uint8_t* dst = vesa.fb + (uint32_t)(dy + row) * (uint32_t)pitch + (uint32_t)dx * (uint32_t)bpp;
        for (int col = 0; col < w; col++) {
            uint32_t c = src[col];
            if (c) {  // skip zero (transparent) pixels
                dst[col*(uint32_t)bpp + 0] = c & 0xFF;
                dst[col*(uint32_t)bpp + 1] = (c >> 8) & 0xFF;
                dst[col*(uint32_t)bpp + 2] = (c >> 16) & 0xFF;
                if (bpp == 4) dst[col*4 + 3] = 0;
            }
        }
    }
}

// ─── SSE2 wide copy ───────────────────────────────────────────────
// 16-byte unaligned moves, 4× unrolled (64 B per iteration).  This is
// the kernel's biggest single memory op: vesa_present copies
// pitch×height ≈ 8.3 MB every frame.  Boot-time CR4.OSFXSR (set in
// boot/boot.asm) makes the SSE instructions legal; movdqu needs no
// alignment on src/dst.
//
// SSE confinement: the target("sse2") attribute makes GCC accept the
// XMM clobbers HERE while the rest of the kernel stays compiled
// SSE-free — so no other code (incl. IRQ handlers) can touch XMM
// state.  The copy itself is reentrant-safe: each iteration loads and
// stores its own 64 B with no state carried across iterations or
// calls, so an interrupt mid-copy cannot corrupt anything.
__attribute__((target("sse2")))
void vesa_copy16(uint8_t* dst, const uint8_t* src, uint32_t bytes) {
    uint32_t q = bytes >> 6;
    __asm__ volatile (
        "test %2, %2\n\t"
        "jz 2f\n\t"
        "1:\n\t"
        "movdqu  0(%0), %%xmm0\n\t"
        "movdqu 16(%0), %%xmm1\n\t"
        "movdqu 32(%0), %%xmm2\n\t"
        "movdqu 48(%0), %%xmm3\n\t"
        "movdqu %%xmm0,  0(%1)\n\t"
        "movdqu %%xmm1, 16(%1)\n\t"
        "movdqu %%xmm2, 32(%1)\n\t"
        "movdqu %%xmm3, 48(%1)\n\t"
        "add $64, %0\n\t"
        "add $64, %1\n\t"
        "dec %2\n\t"
        "jnz 1b\n\t"
        "2:\n\t"
        : "+r"(src), "+r"(dst), "+r"(q)
        :
        : "xmm0", "xmm1", "xmm2", "xmm3", "memory");
    bytes &= 63;
    while (bytes >= 16) {
        __asm__ volatile (
            "movdqu (%0), %%xmm0\n\t"
            "movdqu %%xmm0, (%1)\n\t"
            : "+r"(src), "+r"(dst)
            :
            : "xmm0", "memory");
        src += 16;
        dst += 16;
        bytes -= 16;
    }
    while (bytes--) *dst++ = *src++;   // scalar tail (<16 B)
}

void vesa_present(void) {
    if (!vesa.active) return;
    if (vesa.draw == vesa.fb) return; // not double-buffered, nothing to copy

    // Flag the copy + compositing window: the IRQ draw chain skips
    // overlay drawing (box, ghost, cursor) while this is set, so no
    // IRQ write can tear vesa.fb mid-copy.  finish_frame() redraws
    // the overlays immediately after this function returns.
    __present_in_progress = 1;

    uint32_t total = vesa.pitch * vesa.height;

    // 16-byte SSE2 copy — ~4× fewer memory instructions than the old
    // 4-byte word loop (8294400 B at 1920×1080×32bpp, 64 B/iteration).
    vesa_copy16((uint8_t*)vesa.fb, (uint8_t*)vesa.draw, total);

    // ── Composite overlay plane ───────────────────────────────────
    // Snapshot pixels were captured by the IRQ handler at 200 Hz;
    // now we restore them after the back-buffer copy erased them.
    // w/h of 0 means that overlay is not currently active.
    // Stride constants come from include/vesa.h (shared with mouse.c).
    if (__ovly_cell_w > 0 && __ovly_cell_h > 0)
        ovly_composite(__ovly_cell_buf, OVLY_CELL_W,
                       __ovly_cell_x, __ovly_cell_y,
                       __ovly_cell_w, __ovly_cell_h);

    // Snap preview has a 2-frame fade-out on drag-end.  The IRQ
    // handler sets __ovly_prev_fade = 2 when drag_active transitions
    // to 0 and a preview snapshot is available.  Each vesa_present()
    // composites the stored preview at progressively dimmer levels,
    // then decrements the fade counter.  At the last frame (fade=1)
    // we clear w=0 so the next present skips the composite entirely.
    // The dim factor progresses: frame 1: 128 (50%), frame 2: 64 (25%).
    //
    // XOR-delta compositing: each stored pixel is compared against
    // a dedicated clean-background snapshot taken by the IRQ handler
    // IMMEDIATELY before drawing the overlay (not against the fragile
    // framebuffer, which might have changed since the snapshot was
    // taken — clock tick, wallpaper animation, etc.).  If they match,
    // the pixel is background and we skip it — letting the pristine
    // desktop show through.  If they differ, the pixel is overlay
    // content and we dim only that pixel.  This guarantees the
    // background never gets darkened during the fade, regardless of
    // desktop content changes between snapshot and present.
    if (__ovly_prev_fade > 0) {
        int dim = __ovly_prev_fade * 64;  // frame 1: 128 (50%), frame 2: 64 (25%)
        if (__ovly_prev_w > 0 && __ovly_prev_h > 0) {
            uint32_t* buf = __ovly_prev_buf;
            uint32_t* bg = __ovly_prev_bg_buf;
            int bstride = OVLY_PREV_W;
            int d_pitch = (int)vesa.pitch;
            int d_bpp = (int)(vesa.bpp / 8);
            int dx = __ovly_prev_x, dy = __ovly_prev_y;
            int fb_w = (int)vesa.width, fb_h = (int)vesa.height;
            for (int y = 0; y < __ovly_prev_h; y++) {
                int sy = dy + y;
                if (sy < 0 || sy >= fb_h) continue;
                for (int x = 0; x < __ovly_prev_w; x++) {
                    int sx = dx + x;
                    if (sx < 0 || sx >= fb_w) continue;
                    uint32_t stored = buf[y * bstride + x];
                    uint32_t bg_px = bg[y * bstride + x];
                    if (stored == bg_px || stored == 0) continue;  // background
                    // Overlay pixel — dim by fade factor and composite
                    uint32_t b = (stored & 0xFF) * (uint32_t)dim / 256;
                    uint32_t g = ((stored >> 8) & 0xFF) * (uint32_t)dim / 256;
                    uint32_t r = ((stored >> 16) & 0xFF) * (uint32_t)dim / 256;
                    uint32_t dimmed = (r << 16) | (g << 8) | b;
                    uint8_t* fbp = vesa.fb + (uint32_t)sy * (uint32_t)d_pitch
                                           + (uint32_t)sx * (uint32_t)d_bpp;
                    fbp[0] = dimmed & 0xFF;
                    fbp[1] = (dimmed >> 8) & 0xFF;
                    fbp[2] = (dimmed >> 16) & 0xFF;
                    // Update stored value for next frame's comparison
                    buf[y * bstride + x] = dimmed;
                }
            }
        }
        __ovly_prev_fade--;
        if (__ovly_prev_fade <= 0) __ovly_prev_w = 0;
    } else if (__ovly_prev_w > 0 && __ovly_prev_h > 0) {
        ovly_composite(__ovly_prev_buf, OVLY_PREV_W,
                       __ovly_prev_x, __ovly_prev_y,
                       __ovly_prev_w, __ovly_prev_h);
    }

    if (__ovly_ghost_w > 0 && __ovly_ghost_h > 0)
        ovly_composite(__ovly_ghost_buf, OVLY_GHOST_W,
                       __ovly_ghost_x, __ovly_ghost_y,
                       __ovly_ghost_w, __ovly_ghost_h);

    // ── Present-generation bump ────────────────────────────────────
    // The bulk copy above just overwrote vesa.fb, wiping any IRQ-time
    // selection box and cursor background that were XOR'd/restored
    // there since the last present.  Bump the generation counters so
    // the mouse IRQ handler (200 Hz) knows its saved state is stale:
    // it must NOT erase a box that is no longer on screen, and must
    // NOT restore a pre-present cursor background over the fresh frame.
    // desktop.c finish_frame() redraws the box right after this call
    // when a select-drag is still active.
    sel_box_present_gen++;
    cursor_present_gen++;

    // Copy + compositing complete; the IRQ may draw overlays again.
    __present_in_progress = 0;
}

// ─── Game-mode present: copy ONLY the game canvas rect ───────────
// In MODE_GAME the desktop content is just the 640×400 game canvas
// (+ title bar) centered on the screen.  A full vesa_present() copies
// the whole 1920×1080 back buffer (~8.3 MB) 35×/s — evicting the CPU
// cache and stalling IRQ0 for the whole copy, which visibly lags the
// game AND delays PS/2 input delivery.  This variant copies just the
// game window rect (640 rows × 2560 B ≈ 1.6 MB → 4 MB incl. title
// bar), about 4× less bandwidth and cache churn.  Callers guarantee
// nothing else is composited (no alt-tab overlay, no desktop modes).
// Present-generation counters still bump: any IRQ-drawn overlay state
// must be invalidated exactly as in the full present.
void vesa_present_rect(int dx, int dy, int w, int h) {
    if (!vesa.active || vesa.draw == vesa.fb) return;
    if (w <= 0 || h <= 0) return;
    if (dx < 0) { w += dx; dx = 0; }        // clip to screen
    if (dy < 0) { h += dy; dy = 0; }
    if (dx + w > (int)vesa.width)  w = (int)vesa.width  - dx;
    if (dy + h > (int)vesa.height) h = (int)vesa.height - dy;
    if (w <= 0 || h <= 0) return;

    __present_in_progress = 1;
    int bpp = (int)(vesa.bpp / 8);
    for (int y = 0; y < h; y++) {
        const uint8_t* src = vesa.draw + (uint32_t)(dy + y) * vesa.pitch
                                 + (uint32_t)dx * (uint32_t)bpp;
        uint8_t* dst = vesa.fb + (uint32_t)(dy + y) * vesa.pitch
                              + (uint32_t)dx * (uint32_t)bpp;
        vesa_copy16(dst, src, (uint32_t)(w * bpp));
    }
    sel_box_present_gen++;
    cursor_present_gen++;
    __present_in_progress = 0;
}

// ─── Multiboot flags ──────────────────────────────────────────────
#define MB_FLAG_FRAMEBUFFER (1 << 12)

// ─── Init ─────────────────────────────────────────────────────────
void vesa_init(multiboot_info_t* mbi) {
    if (!mbi) return;

    // Check if multiboot provided framebuffer info
    if (!(mbi->flags & MB_FLAG_FRAMEBUFFER)) {
        vesa.active = 0;
        return;
    }

    // Only support 32bpp or 24bpp linear framebuffer (type 1 = RGB)
    if (mbi->framebuffer_type != 1) {
        vesa.active = 0;
        return;
    }

    if (mbi->framebuffer_bpp != 32 && mbi->framebuffer_bpp != 24) {
        vesa.active = 0;
        return;
    }

    vesa.fb     = (uint8_t*)(uint32_t)mbi->framebuffer_addr;
    vesa.draw   = vesa.fb;  // draw directly to screen until double-buffered
    vesa.width  = mbi->framebuffer_width;
    vesa.height = mbi->framebuffer_height;
    vesa.pitch  = mbi->framebuffer_pitch;
    vesa.bpp    = mbi->framebuffer_bpp;
    vesa.active = 1;
}

// ─── Runtime mode switching ───────────────────────────────────────
// After a successful VBE mode switch, recalculate vesa_info_t fields
// and invalidate subsystems that depend on framebuffer dimensions.
// The framebuffer address stays the same (bochs VBE uses a fixed LFB).
int vesa_set_mode(uint16_t width, uint16_t height, uint16_t bpp) {
    // Try the Bochs VBE interface
    if (!vbe_set_mode(vbe_find_mode(width, height, bpp))) return 0;

    // Update vesa_info_t with the new parameters.
    // pitch = width * bytes_per_pixel (rounded up to 4-byte boundary).
    int bytes_pp = bpp / 8;
    uint32_t pitch = (uint32_t)width * (uint32_t)bytes_pp;
    // Some VBE modes use larger pitch for alignment; bochs VBE doesn't,
    // but we keep the old pitch calculation for safety.

    vesa.width  = width;
    vesa.height = height;
    vesa.pitch  = pitch;
    vesa.bpp    = (uint8_t)bpp;
    vesa.draw   = vesa.fb;  // reset to direct draw (re-enable double buffer if needed)

    // Attempt to re-enable double-buffering (will be skipped if back buffer
    // is too small for the new resolution).
    vesa_enable_double_buffer();

    // Invalidate wallpaper cache so it re-decodes at the new resolution
    wallpaper_set_dim(0);       // reset dim on mode change
    wallpaper_set_custom(-1);   // revert to built-in wallpaper (invalidates cache)

    // Clear the framebuffer
    vesa_clear(vesa_rgb(24, 8, 16));  // dark accent color

    return 1;
}

// ─── Color helper ─────────────────────────────────────────────────
uint32_t vesa_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

// ─── Pixel ────────────────────────────────────────────────────────
void vesa_put_pixel(int x, int y, uint32_t color) {
    if (!vesa.active) return;
    if ((uint32_t)x >= vesa.width || (uint32_t)y >= vesa.height) return;

    uint8_t* p = vesa.draw + (uint32_t)y * vesa.pitch + (uint32_t)x * (vesa.bpp / 8);

    if (vesa.bpp == 32) {
        p[0] = (uint8_t)(color & 0xFF);         // B
        p[1] = (uint8_t)((color >> 8) & 0xFF);  // G
        p[2] = (uint8_t)((color >> 16) & 0xFF); // R
        p[3] = 0xFF;                             // A
    } else { // 24bpp
        p[0] = (uint8_t)(color & 0xFF);
        p[1] = (uint8_t)((color >> 8) & 0xFF);
        p[2] = (uint8_t)((color >> 16) & 0xFF);
    }
}

// ─── Brightness/contrast adjusted RGBA blit ─────────────────────
// Same as vesa_blit_rgba but applies brightness (offset -100..+100)
// and contrast (factor -100..+100, 0 = no change) to every pixel
// during the RGBA→BGRA conversion.
//
// Contrast formula: factor = 128 + C*128/100 (range 28..228)
//   px = clamp(((px - 128) * factor / 128) + 128 + brightness, 0, 255)
//   where C = -100 (min contrast) → factor 28 → nearly gray
//         C = 0   (no change)    → factor 128 → identity
//         C = +100 (max contrast) → factor 228 → boosted
void vesa_blit_rgba_adjusted(int dx, int dy,
                             const uint8_t* rgba, int src_w, int src_h,
                             int src_x, int src_y, int w, int h,
                             int brightness, int contrast) {
    if (!vesa.active || !rgba || src_w <= 0 || src_h <= 0) return;
    if (w <= 0 || h <= 0) return;

    // ── Clip source rectangle to source image bounds ──
    if (src_x < 0) { w += src_x; dx -= src_x; src_x = 0; }
    if (src_y < 0) { h += src_y; dy -= src_y; src_y = 0; }
    if (src_x + w > src_w) w = src_w - src_x;
    if (src_y + h > src_h) h = src_h - src_y;
    if (w <= 0 || h <= 0) return;

    // ── Clip destination rectangle to framebuffer bounds ──
    if (dx < 0) { w += dx; src_x -= dx; dx = 0; }
    if (dy < 0) { h += dy; src_y -= dy; dy = 0; }
    int fb_w = (int)vesa.width;
    int fb_h = (int)vesa.height;
    if (dx + w > fb_w) w = fb_w - dx;
    if (dy + h > fb_h) h = fb_h - dy;
    if (w <= 0 || h <= 0) return;

    // Pre-compute contrast factor: 0..256 where 128 = identity.
    // Clamp to 28..228 so even extreme values produce visible results.
    int cf = 128 + (contrast * 128 / 100);
    if (cf < 28) cf = 28;
    if (cf > 228) cf = 228;
    int bc = brightness;  // -100..+100

    int src_stride = src_w * 4;
    int fb_bpp = (int)(vesa.bpp / 8);

    for (int row = 0; row < h; row++) {
        int si = ((src_y + row) * src_stride) + src_x * 4;
        uint8_t* dst = vesa.draw +
            (uint32_t)(dy + row) * vesa.pitch +
            (uint32_t)dx * (uint32_t)fb_bpp;

        if (fb_bpp == 4) {
            for (int col = 0; col < w; col++) {
                // Apply brightness + contrast
                int r = ((int)rgba[si + 0] - 128) * cf / 128 + 128 + bc;
                int g = ((int)rgba[si + 1] - 128) * cf / 128 + 128 + bc;
                int b = ((int)rgba[si + 2] - 128) * cf / 128 + 128 + bc;
                if (r < 0) r = 0; if (r > 255) r = 255;
                if (g < 0) g = 0; if (g > 255) g = 255;
                if (b < 0) b = 0; if (b > 255) b = 255;
                dst[col*4 + 0] = (uint8_t)b;  // B (framebuffer BGRA)
                dst[col*4 + 1] = (uint8_t)g;  // G
                dst[col*4 + 2] = (uint8_t)r;  // R
                dst[col*4 + 3] = 0xFF;
                si += 4;
            }
        } else {
            // 24bpp fallback
            for (int col = 0; col < w; col++) {
                int r = ((int)rgba[si + 0] - 128) * cf / 128 + 128 + bc;
                int g = ((int)rgba[si + 1] - 128) * cf / 128 + 128 + bc;
                int b = ((int)rgba[si + 2] - 128) * cf / 128 + 128 + bc;
                if (r < 0) r = 0; if (r > 255) r = 255;
                if (g < 0) g = 0; if (g > 255) g = 255;
                if (b < 0) b = 0; if (b > 255) b = 255;
                dst[col*3 + 0] = (uint8_t)b;
                dst[col*3 + 1] = (uint8_t)g;
                dst[col*3 + 2] = (uint8_t)r;
                si += 4;
            }
        }
    }
}

// ─── In-place framebuffer brightness/contrast adjustment ─────────
// Reads a rectangle (dx, dy, w, h) from the current vesa.draw buffer,
// applies brightness and contrast to every pixel, and writes the
// adjusted pixels back.  The buffer is already in BGRA byte order
// (byte 0=B, 1=G, 2=R, 3=A), so each channel is adjusted independently
// using the same formula as vesa_blit_rgba_adjusted.
//
// Use this to post-process a bilinear-scaled image in the framebuffer
// rather than falling back to nearest-neighbor when both zoom and
// adjustments are active.
void vesa_adjust_fb_rect(int dx, int dy, int w, int h,
                          int brightness, int contrast) {
    if (!vesa.active || w <= 0 || h <= 0) return;

    // Pre-compute contrast factor (28..228, 128 = identity)
    int cf = 128 + (contrast * 128 / 100);
    if (cf < 28) cf = 28;
    if (cf > 228) cf = 228;
    int bc = brightness;

    // Clip to framebuffer
    if (dx < 0) { w += dx; dx = 0; }
    if (dy < 0) { h += dy; dy = 0; }
    int fb_w = (int)vesa.width;
    int fb_h = (int)vesa.height;
    if (dx + w > fb_w) w = fb_w - dx;
    if (dy + h > fb_h) h = fb_h - dy;
    if (w <= 0 || h <= 0) return;

    int bpp = (int)(vesa.bpp / 8);

    for (int row = 0; row < h; row++) {
        uint8_t* p = vesa.draw +
            (uint32_t)(dy + row) * vesa.pitch +
            (uint32_t)dx * (uint32_t)bpp;

        if (bpp == 4) {
            for (int col = 0; col < w; col++) {
                // Framebuffer is BGRA: p[0]=B, p[1]=G, p[2]=R
                int r = ((int)p[2] - 128) * cf / 128 + 128 + bc;
                int g = ((int)p[1] - 128) * cf / 128 + 128 + bc;
                int b = ((int)p[0] - 128) * cf / 128 + 128 + bc;
                if (r < 0) r = 0; if (r > 255) r = 255;
                if (g < 0) g = 0; if (g > 255) g = 255;
                if (b < 0) b = 0; if (b > 255) b = 255;
                p[0] = (uint8_t)b;
                p[1] = (uint8_t)g;
                p[2] = (uint8_t)r;
                p += 4;
            }
        } else {
            // 24bpp fallback
            for (int col = 0; col < w; col++) {
                int r = ((int)p[2] - 128) * cf / 128 + 128 + bc;
                int g = ((int)p[1] - 128) * cf / 128 + 128 + bc;
                int b = ((int)p[0] - 128) * cf / 128 + 128 + bc;
                if (r < 0) r = 0; if (r > 255) r = 255;
                if (g < 0) g = 0; if (g > 255) g = 255;
                if (b < 0) b = 0; if (b > 255) b = 255;
                p[0] = (uint8_t)b;
                p[1] = (uint8_t)g;
                p[2] = (uint8_t)r;
                p += 3;
            }
        }
    }
}

// ─── Bulk RGBA blit ──────────────────────────────────────────────
void vesa_blit_rgba(int dx, int dy,
                    const uint8_t* rgba, int src_w, int src_h,
                    int src_x, int src_y, int w, int h) {
    if (!vesa.active || !rgba || src_w <= 0 || src_h <= 0) return;
    if (w <= 0 || h <= 0) return;

    // ── Clip source rectangle to source image bounds ──
    if (src_x < 0) { w += src_x; dx -= src_x; src_x = 0; }
    if (src_y < 0) { h += src_y; dy -= src_y; src_y = 0; }
    if (src_x + w > src_w) w = src_w - src_x;
    if (src_y + h > src_h) h = src_h - src_y;
    if (w <= 0 || h <= 0) return;

    // ── Clip destination rectangle to framebuffer bounds ──
    if (dx < 0) { w += dx; src_x -= dx; dx = 0; }
    if (dy < 0) { h += dy; src_y -= dy; dy = 0; }
    int fb_w = (int)vesa.width;
    int fb_h = (int)vesa.height;
    if (dx + w > fb_w) w = fb_w - dx;
    if (dy + h > fb_h) h = fb_h - dy;
    if (w <= 0 || h <= 0) return;

    int src_stride = src_w * 4;
    int fb_bpp = (int)(vesa.bpp / 8);

    for (int row = 0; row < h; row++) {
        int si = ((src_y + row) * src_stride) + src_x * 4;
        uint8_t* dst = vesa.draw +
            (uint32_t)(dy + row) * vesa.pitch +
            (uint32_t)dx * (uint32_t)fb_bpp;

        if (fb_bpp == 4) {
            // 32bpp: swap R↔B during copy
            for (int col = 0; col < w; col++) {
                dst[col*4 + 0] = rgba[si + 2];  // B = source R
                dst[col*4 + 1] = rgba[si + 1];  // G = source G
                dst[col*4 + 2] = rgba[si + 0];  // R = source B
                dst[col*4 + 3] = 0xFF;
                si += 4;
            }
        } else {
            // 24bpp fallback
            for (int col = 0; col < w; col++) {
                dst[col*3 + 0] = rgba[si + 2];  // B = source R
                dst[col*3 + 1] = rgba[si + 1];  // G = source G
                dst[col*3 + 2] = rgba[si + 0];  // R = source B
                si += 4;
            }
        }
    }
}

// ─── Bulk BGRA blit (uint32_t word copy, no byte swapping) ────────
void vesa_blit_bgra(int dx, int dy,
                    const uint8_t* bgra, int img_w, int img_h,
                    int src_x, int src_y, int w, int h) {
    if (!vesa.active || !bgra || img_w <= 0 || img_h <= 0) return;
    if (w <= 0 || h <= 0) return;

    // ── Clip source to image bounds ──
    if (src_x < 0) { w += src_x; dx -= src_x; src_x = 0; }
    if (src_y < 0) { h += src_y; dy -= src_y; src_y = 0; }
    if (src_x + w > img_w) w = img_w - src_x;
    if (src_y + h > img_h) h = img_h - src_y;
    if (w <= 0 || h <= 0) return;

    // ── Clip destination to framebuffer ──
    if (dx < 0) { w += dx; src_x -= dx; dx = 0; }
    if (dy < 0) { h += dy; src_y -= dy; dy = 0; }
    int fb_w = (int)vesa.width;
    int fb_h = (int)vesa.height;
    if (dx + w > fb_w) w = fb_w - dx;
    if (dy + h > fb_h) h = fb_h - dy;
    if (w <= 0 || h <= 0) return;

    int src_stride = img_w * 4;

    // Fast uint32_t word copy per row — the BGRA byte order matches
    // the 32bpp VESA framebuffer exactly, so no per-pixel processing
    // is needed; just align to 4-byte words and bulk-transfer.
    for (int row = 0; row < h; row++) {
        const uint32_t* src = (const uint32_t*)(
            bgra + ((src_y + row) * src_stride) + src_x * 4);
        uint32_t* dst = (uint32_t*)(
            vesa.draw + (uint32_t)(dy + row) * vesa.pitch + (uint32_t)dx * 4);
        for (int col = 0; col < w; col++)
            dst[col] = src[col];
    }
}

// ─── Scaled RGBA blit (nearest-neighbor + RGBA→BGRA) ────────────
void vesa_blit_rgba_scaled(int dx, int dy, int dw, int dh,
                           const uint8_t* rgba, int src_w, int src_h) {
    if (!vesa.active || !rgba || dw <= 0 || dh <= 0 || src_w <= 0 || src_h <= 0)
        return;

    // ── Clip destination to framebuffer ──
    if (dx < 0) { dw += dx; dx = 0; }
    if (dy < 0) { dh += dy; dy = 0; }
    int fb_w = (int)vesa.width;
    int fb_h = (int)vesa.height;
    if (dx + dw > fb_w) dw = fb_w - dx;
    if (dy + dh > fb_h) dh = fb_h - dy;
    if (dw <= 0 || dh <= 0) return;

    int fb_bpp = (int)(vesa.bpp / 8);

    for (int py = 0; py < dh; py++) {
        int sy = py * src_h / dh;
        if (sy >= src_h) sy = src_h - 1;

        uint8_t* dst = vesa.draw +
            (uint32_t)(dy + py) * vesa.pitch +
            (uint32_t)dx * (uint32_t)fb_bpp;

        if (fb_bpp == 4) {
            for (int px = 0; px < dw; px++) {
                int sx = px * src_w / dw;
                if (sx >= src_w) sx = src_w - 1;
                int si = (sy * src_w + sx) * 4;
                dst[px*4 + 0] = rgba[si + 2];  // B = src R
                dst[px*4 + 1] = rgba[si + 1];  // G = src G
                dst[px*4 + 2] = rgba[si + 0];  // R = src B
                dst[px*4 + 3] = 0xFF;
            }
        } else {
            for (int px = 0; px < dw; px++) {
                int sx = px * src_w / dw;
                if (sx >= src_w) sx = src_w - 1;
                int si = (sy * src_w + sx) * 4;
                dst[px*3 + 0] = rgba[si + 2];  // B
                dst[px*3 + 1] = rgba[si + 1];  // G
                dst[px*3 + 2] = rgba[si + 0];  // R
            }
        }
    }
}

// ─── RGBA crossfade blend ────────────────────────────────────────
void vesa_blend_rgba(int dx, int dy, int dw, int dh,
                     const uint8_t* from, const uint8_t* to,
                     int src_w, int src_h, int alpha) {
    if (!vesa.active || !from || !to || dw <= 0 || dh <= 0 ||
        src_w <= 0 || src_h <= 0)
        return;
    if (alpha < 0) alpha = 0;
    if (alpha > 255) alpha = 255;
    int ia = 255 - alpha;

    // ── Clip destination to framebuffer ──
    if (dx < 0) { dw += dx; dx = 0; }
    if (dy < 0) { dh += dy; dy = 0; }
    int fb_w = (int)vesa.width;
    int fb_h = (int)vesa.height;
    if (dx + dw > fb_w) dw = fb_w - dx;
    if (dy + dh > fb_h) dh = fb_h - dy;
    if (dw <= 0 || dh <= 0) return;

    int fb_bpp = (int)(vesa.bpp / 8);

    for (int py = 0; py < dh; py++) {
        int sy = py * src_h / dh;
        if (sy >= src_h) sy = src_h - 1;
        int src_row_offset = sy * src_w * 4;

        uint8_t* dst = vesa.draw +
            (uint32_t)(dy + py) * vesa.pitch +
            (uint32_t)dx * (uint32_t)fb_bpp;

        if (fb_bpp == 4) {
            for (int px = 0; px < dw; px++) {
                int sx = px * src_w / dw;
                if (sx >= src_w) sx = src_w - 1;
                int si = src_row_offset + sx * 4;
                int r = ((int)from[si+0]*ia + (int)to[si+0]*alpha) / 255;
                int g = ((int)from[si+1]*ia + (int)to[si+1]*alpha) / 255;
                int b = ((int)from[si+2]*ia + (int)to[si+2]*alpha) / 255;
                dst[px*4 + 0] = (uint8_t)b;  // B = blended B
                dst[px*4 + 1] = (uint8_t)g;  // G = blended G
                dst[px*4 + 2] = (uint8_t)r;  // R = blended R
                dst[px*4 + 3] = 0xFF;
            }
        } else {
            for (int px = 0; px < dw; px++) {
                int sx = px * src_w / dw;
                if (sx >= src_w) sx = src_w - 1;
                int si = src_row_offset + sx * 4;
                int r = ((int)from[si+0]*ia + (int)to[si+0]*alpha) / 255;
                int g = ((int)from[si+1]*ia + (int)to[si+1]*alpha) / 255;
                int b = ((int)from[si+2]*ia + (int)to[si+2]*alpha) / 255;
                dst[px*3 + 0] = (uint8_t)b;
                dst[px*3 + 1] = (uint8_t)g;
                dst[px*3 + 2] = (uint8_t)r;
            }
        }
    }
}

// ─── Bilinear-filtered RGBA scaled blit ──────────────────────────
// Same as vesa_blit_rgba_scaled but uses bilinear interpolation for
// smoother upscaling.  Four surrounding source pixels are blended
// using fixed-point (16.16) sub-pixel weights.
void vesa_blit_rgba_scaled_bilinear(int dx, int dy, int dw, int dh,
                                    const uint8_t* rgba, int src_w, int src_h) {
    if (!vesa.active || !rgba || dw <= 0 || dh <= 0 || src_w <= 0 || src_h <= 0)
        return;

    // ── Clip destination to framebuffer ──
    if (dx < 0) { dw += dx; dx = 0; }
    if (dy < 0) { dh += dy; dy = 0; }
    int fb_w = (int)vesa.width;
    int fb_h = (int)vesa.height;
    if (dx + dw > fb_w) dw = fb_w - dx;
    if (dy + dh > fb_h) dh = fb_h - dy;
    if (dw <= 0 || dh <= 0) return;

    int fb_bpp = (int)(vesa.bpp / 8);

    // Fixed-point 16.16 step per output pixel
    // (step << 16) / step = 1.0 in 16.16 fixed point
    int step_x = (src_w << 16) / dw;
    int step_y = (src_h << 16) / dh;

    // Precompute source-start in 16.16 shifted by half-pixel so that
    // the center of the first output pixel maps to the center of the
    // first source pixel for a symmetric filter.
    int sy_fp = (1 << 15) * src_h / dh;  // half-pixel offset

    for (int py = 0; py < dh; py++) {
        int sx_fp = (1 << 15) * src_w / dw;  // half-pixel offset

        // Integer source row for the four samples: y0 = floor(sy), y1 = y0+1
        int sy0 = sy_fp >> 16;
        int sy1 = sy0 + 1;
        if (sy0 >= src_h) sy0 = src_h - 1;
        if (sy1 >= src_h) sy1 = src_h - 1;
        if (sy0 < 0) sy0 = 0;
        if (sy1 < 0) sy1 = 0;

        // Vertical interpolation weight (fractional part, 0..65535)
        int wy = (sy_fp & 0xFFFF);
        int iwy = 65536 - wy;

        uint8_t* dst = vesa.draw +
            (uint32_t)(dy + py) * vesa.pitch +
            (uint32_t)dx * (uint32_t)fb_bpp;

        if (fb_bpp == 4) {
            for (int px = 0; px < dw; px++) {
                // Integer source column for the four samples
                int sx0 = sx_fp >> 16;
                int sx1 = sx0 + 1;
                if (sx0 >= src_w) sx0 = src_w - 1;
                if (sx1 >= src_w) sx1 = src_w - 1;
                if (sx0 < 0) sx0 = 0;
                if (sx1 < 0) sx1 = 0;

                // Horizontal weight
                int wx = (sx_fp & 0xFFFF);
                int iwx = 65536 - wx;

                // Sample the 4 surrounding pixels (RGBA bytes)
                int si00 = (sy0 * src_w + sx0) * 4;
                int si01 = (sy0 * src_w + sx1) * 4;
                int si10 = (sy1 * src_w + sx0) * 4;
                int si11 = (sy1 * src_w + sx1) * 4;

                // Bilinear interpolation in two 1D passes to avoid 32-bit overflow:
                // Pass 1: horizontal blend → divide by 65536 (max ~33M, fits int32)
                // Pass 2: vertical blend → divide by 65536 (max ~67M, fits int32)
                int h0r = ((int)rgba[si00+0]*iwx + (int)rgba[si01+0]*wx + 32768) / 65536;
                int h0g = ((int)rgba[si00+1]*iwx + (int)rgba[si01+1]*wx + 32768) / 65536;
                int h0b = ((int)rgba[si00+2]*iwx + (int)rgba[si01+2]*wx + 32768) / 65536;
                int h1r = ((int)rgba[si10+0]*iwx + (int)rgba[si11+0]*wx + 32768) / 65536;
                int h1g = ((int)rgba[si10+1]*iwx + (int)rgba[si11+1]*wx + 32768) / 65536;
                int h1b = ((int)rgba[si10+2]*iwx + (int)rgba[si11+2]*wx + 32768) / 65536;

                int r = (h0r*iwy + h1r*wy + 32768) / 65536;
                int g = (h0g*iwy + h1g*wy + 32768) / 65536;
                int b = (h0b*iwy + h1b*wy + 32768) / 65536;

                if (r < 0) r = 0; if (r > 255) r = 255;
                if (g < 0) g = 0; if (g > 255) g = 255;
                if (b < 0) b = 0; if (b > 255) b = 255;

                dst[px*4 + 0] = (uint8_t)b;
                dst[px*4 + 1] = (uint8_t)g;
                dst[px*4 + 2] = (uint8_t)r;
                dst[px*4 + 3] = 0xFF;

                sx_fp += step_x;
            }
        } else {
            // 24bpp fallback — same two-pass bilinear, different dst layout
            for (int px = 0; px < dw; px++) {
                int sx0 = sx_fp >> 16;
                int sx1 = sx0 + 1;
                if (sx0 >= src_w) sx0 = src_w - 1;
                if (sx1 >= src_w) sx1 = src_w - 1;
                if (sx0 < 0) sx0 = 0;
                if (sx1 < 0) sx1 = 0;

                int wx = (sx_fp & 0xFFFF);
                int iwx = 65536 - wx;

                int si00 = (sy0 * src_w + sx0) * 4;
                int si01 = (sy0 * src_w + sx1) * 4;
                int si10 = (sy1 * src_w + sx0) * 4;
                int si11 = (sy1 * src_w + sx1) * 4;

                int h0r = ((int)rgba[si00+0]*iwx + (int)rgba[si01+0]*wx + 32768) / 65536;
                int h0g = ((int)rgba[si00+1]*iwx + (int)rgba[si01+1]*wx + 32768) / 65536;
                int h0b = ((int)rgba[si00+2]*iwx + (int)rgba[si01+2]*wx + 32768) / 65536;
                int h1r = ((int)rgba[si10+0]*iwx + (int)rgba[si11+0]*wx + 32768) / 65536;
                int h1g = ((int)rgba[si10+1]*iwx + (int)rgba[si11+1]*wx + 32768) / 65536;
                int h1b = ((int)rgba[si10+2]*iwx + (int)rgba[si11+2]*wx + 32768) / 65536;

                int r = (h0r*iwy + h1r*wy + 32768) / 65536;
                int g = (h0g*iwy + h1g*wy + 32768) / 65536;
                int b = (h0b*iwy + h1b*wy + 32768) / 65536;

                if (r < 0) r = 0; if (r > 255) r = 255;
                if (g < 0) g = 0; if (g > 255) g = 255;
                if (b < 0) b = 0; if (b > 255) b = 255;

                dst[px*3 + 0] = (uint8_t)b;
                dst[px*3 + 1] = (uint8_t)g;
                dst[px*3 + 2] = (uint8_t)r;

                sx_fp += step_x;
            }
        }
        sy_fp += step_y;
    }
}

// ─── Catmull-Rom bicubic scaled blit ─────────────────────────────
// 4-tap separable cubic filter (16.16 fixed point) — the standard
// "bicubic".  Sharper than bilinear when scaling low-res content:
// edges stay crisp instead of mushy and gradients stay smooth
// instead of blocky.  ~4× the taps of the bilinear path, which is
// fine for the video player's once-per-frame letterbox blit (and it
// only runs when the frame actually changed — the dirty guard in
// video.c).  The mild negative lobes give a little edge overshoot
// (Catmull-Rom ringing); results are clamped to 0..255 like the
// bilinear path.
static void vesa_cr_weights(int f, int w[4]) {
    // f is the 16.16 fractional part (0..65535).  Catmull-Rom cubic
    // weights for the taps at floor(f)-1 .. floor(f)+2, in 16.16
    // fixed point; they always sum to 65536.
    int64_t F = f;
    int F2 = (int)((F * F) >> 16);
    int F3 = (int)((F2 * F) >> 16);   // int64 mul keeps F2*F in range
    w[0] = (int)((-F3 + 2 * F2 - f) >> 1);
    w[1] = (int)((3 * F3 - 5 * F2 + (2 << 16)) >> 1);
    w[2] = (int)((-3 * F3 + 4 * F2 + f) >> 1);
    w[3] = (int)((F3 - F2) >> 1);
}

void vesa_blit_rgba_scaled_bicubic(int dx, int dy, int dw, int dh,
                                   const uint8_t* rgba, int src_w, int src_h) {
    if (!vesa.active || !rgba || dw <= 0 || dh <= 0 || src_w <= 0 || src_h <= 0)
        return;

    // ── Clip destination to framebuffer (same as bilinear) ──
    if (dx < 0) { dw += dx; dx = 0; }
    if (dy < 0) { dh += dy; dy = 0; }
    int fb_w = (int)vesa.width;
    int fb_h = (int)vesa.height;
    if (dx + dw > fb_w) dw = fb_w - dx;
    if (dy + dh > fb_h) dh = fb_h - dy;
    if (dw <= 0 || dh <= 0) return;

    int fb_bpp = (int)(vesa.bpp / 8);
    const int src_bpp = 4;   // source is always packed RGBA

    // Fixed-point 16.16 step per output pixel, half-pixel centered so
    // the filter taps sample source pixel centers (the same mapping
    // as the bilinear blit — only the filter differs).
    int step_x = (src_w << 16) / dw;
    int step_y = (src_h << 16) / dh;
    int sy_fp = (1 << 15) * src_h / dh;

    for (int py = 0; py < dh; py++) {
        int sy = sy_fp >> 16;
        int wy[4];
        vesa_cr_weights(sy_fp & 0xFFFF, wy);
        // Source rows for the 4 taps, clamped to the image bounds.
        int r0 = sy - 1; if (r0 < 0) r0 = 0;
        int r1 = sy;     if (r1 < 0) r1 = 0;
        int r2 = sy + 1; if (r2 >= src_h) r2 = src_h - 1;
        int r3 = sy + 2; if (r3 >= src_h) r3 = src_h - 1;

        uint8_t* dst = vesa.draw +
            (uint32_t)(dy + py) * vesa.pitch +
            (uint32_t)dx * (uint32_t)fb_bpp;

        const uint8_t* rows[4] = {
            rgba + r0 * src_w * src_bpp,
            rgba + r1 * src_w * src_bpp,
            rgba + r2 * src_w * src_bpp,
            rgba + r3 * src_w * src_bpp,
        };

        int sx_fp = (1 << 15) * src_w / dw;

        for (int px = 0; px < dw; px++) {
            int sx = sx_fp >> 16;
            int wx[4];
            vesa_cr_weights(sx_fp & 0xFFFF, wx);
            int c0 = sx - 1; if (c0 < 0) c0 = 0;
            int c1 = sx;     if (c1 < 0) c1 = 0;
            int c2 = sx + 1; if (c2 >= src_w) c2 = src_w - 1;
            int c3 = sx + 2; if (c3 >= src_w) c3 = src_w - 1;

            // 4-tap horizontal blend per source row.  16.16 weights ×
            // 8-bit samples; Σ|w| ≤ 1.875 (worst case at f = 0.5, the
            // negative lobes at max) so a row sum ≤ ~31M — int32-safe.
            int hr[4][3];
            for (int r = 0; r < 4; r++) {
                const uint8_t* p = rows[r];
                hr[r][0] = wx[0]*p[c0*src_bpp+0] + wx[1]*p[c1*src_bpp+0] +
                           wx[2]*p[c2*src_bpp+0] + wx[3]*p[c3*src_bpp+0];
                hr[r][1] = wx[0]*p[c0*src_bpp+1] + wx[1]*p[c1*src_bpp+1] +
                           wx[2]*p[c2*src_bpp+1] + wx[3]*p[c3*src_bpp+1];
                hr[r][2] = wx[0]*p[c0*src_bpp+2] + wx[1]*p[c1*src_bpp+2] +
                           wx[2]*p[c2*src_bpp+2] + wx[3]*p[c3*src_bpp+2];
            }

            // Vertical blend — int64 accumulate (Σ|wy|·|hr| ≤ ~3.9e12),
            // single final scale by 2^32 (keeps full precision; the
            // bilinear rounds mid-pass, we don't need to).
            int64_t ar = (int64_t)wy[0]*hr[0][0] + (int64_t)wy[1]*hr[1][0] +
                         (int64_t)wy[2]*hr[2][0] + (int64_t)wy[3]*hr[3][0];
            int64_t ag = (int64_t)wy[0]*hr[0][1] + (int64_t)wy[1]*hr[1][1] +
                         (int64_t)wy[2]*hr[2][1] + (int64_t)wy[3]*hr[3][1];
            int64_t ab = (int64_t)wy[0]*hr[0][2] + (int64_t)wy[1]*hr[1][2] +
                         (int64_t)wy[2]*hr[2][2] + (int64_t)wy[3]*hr[3][2];

            int r = (int)((ar + 0x80000000LL) >> 32);
            int g = (int)((ag + 0x80000000LL) >> 32);
            int b = (int)((ab + 0x80000000LL) >> 32);

            if (r < 0) r = 0; if (r > 255) r = 255;
            if (g < 0) g = 0; if (g > 255) g = 255;
            if (b < 0) b = 0; if (b > 255) b = 255;

            if (fb_bpp == 4) {
                dst[px*4 + 0] = (uint8_t)b;
                dst[px*4 + 1] = (uint8_t)g;
                dst[px*4 + 2] = (uint8_t)r;
                dst[px*4 + 3] = 0xFF;
            } else {
                dst[px*3 + 0] = (uint8_t)b;
                dst[px*3 + 1] = (uint8_t)g;
                dst[px*3 + 2] = (uint8_t)r;
            }
            sx_fp += step_x;
        }
        sy_fp += step_y;
    }
}

// ─── In-place RGBA→BGRA conversion ────────────────────────────────
void vesa_convert_rgba_bgra(uint8_t* buf, int stride, int w, int h) {
    if (!buf || w <= 0 || h <= 0 || stride < w * 4) return;
    for (int y = 0; y < h; y++) {
        uint8_t* row = buf + y * stride;
        for (int x = 0; x < w; x++) {
            uint8_t* p = row + x * 4;
            uint8_t tmp = p[0];
            p[0] = p[2];  // B = old R
            p[2] = tmp;   // R = old B
            // p[1] (G) and p[3] (A) stay unchanged
        }
    }
}

// ─── In-place BGRA→RGBA conversion ────────────────────────────────

// ─── Boot-time byte-order self-test ────────────────────────────────
// Writes known R/G/B test pixels through every blit path and reads
// back the raw framebuffer bytes to verify the BGRA order is
// byte-0=B, byte-1=G, byte-2=R. Halts on mismatch with details.
void vesa_self_test(void) {
    if (!vesa.active || vesa.bpp != 32) return;
    // Only test the RAM back buffer.  If double-buffering failed to
    // enable (mode larger than the back buffer), vesa.draw still
    // points at the physical LFB — writing test pixels there and
    // reading them back from write-combining memory can return stale
    // bytes on real hardware, risking a spurious halt.  Skip instead.
    if (vesa.draw == vesa.fb) return;

    int pitch = (int)vesa.pitch;
    int bpp = 4;  // 32 bpp

    // Pointers to the start of the draw buffer + VGA text buffer
    volatile uint16_t* vga = (volatile uint16_t*)0xB8000;
    int vga_col = 0;

    // Helper: print a string to VGA text mode (white on blue)
    #define VGA_PUT(s) do { \
        const char* _s = (s); \
        while (*_s) { vga[vga_col++] = (uint16_t)(*_s++) | 0x1F00; } \
    } while(0)

    // Helper: print a hex nibble
    #define VGA_NIB(n) do { \
        char _c = (n) < 10 ? '0'+(n) : 'A'+(n)-10; \
        vga[vga_col++] = (uint16_t)(_c) | 0x1F00; \
    } while(0)

    // Helper: print a byte as two hex digits
    #define VGA_BYTE(b) do { \
        uint8_t _b = (uint8_t)(b); \
        VGA_NIB(_b >> 4); VGA_NIB(_b & 0xF); \
    } while(0)

    // Helper: check a pixel against expected BGRA bytes and panic
    // on mismatch.  (eb, eg, er) come from the test color being
    // exercised — each channel is verified independently, so the
    // green and blue tests don't trip the red expectation.
    #define CHECK_PIXEL(x, y, label, eb, eg, er) do { \
        uint8_t* _p = vesa.draw + (y) * pitch + (x) * bpp; \
        if (_p[0] != (uint8_t)(eb) || _p[1] != (uint8_t)(eg) || _p[2] != (uint8_t)(er)) { \
            vga_col = 0; \
            VGA_PUT("VESA BYTE-ORDER FAILURE: "); VGA_PUT(label); \
            VGA_PUT(" at ("); VGA_BYTE(x); VGA_PUT(","); VGA_BYTE(y); \
            VGA_PUT(") fb=["); VGA_BYTE(_p[0]); VGA_PUT(" "); \
            VGA_BYTE(_p[1]); VGA_PUT(" "); VGA_BYTE(_p[2]); \
            VGA_PUT("] expected B="); VGA_BYTE(eb); VGA_PUT(" G="); \
            VGA_BYTE(eg); VGA_PUT(" R="); VGA_BYTE(er); \
            for (;;) { __asm__ volatile("cli; hlt"); } \
        } \
    } while(0)

    VGA_PUT("[VESA SELF-TEST] ");

    // Save the 6 pixels (row 0 + row 1, columns 0..2) so we can
    // restore them after and leave zero visual artifacts.
    uint8_t saved[24];
    for (int pi = 0; pi < 6; pi++) {
        int sx = pi % 3, sy = pi / 3;
        uint8_t* p = vesa.draw + sy * pitch + sx * bpp;
        saved[pi*4 + 0] = p[0];
        saved[pi*4 + 1] = p[1];
        saved[pi*4 + 2] = p[2];
        saved[pi*4 + 3] = p[3];
    }

    // ── Test each blit path with red (R=255,G=0,B=0), green (R=0,G=255,B=0),
    // ── and blue (R=0,G=0,B=255) to verify every channel independently.

    // Test colors table — uses the shared BgraPixel type from vesa.h
    // to document the expected BGRA framebuffer bytes for each channel.
    //
    // The 'rgba' field stores decoder-style RGBA bytes (R=0, G=1, B=2, A=3)
    // as the input to blit_rgba / convert_rgba_bgra.
    // The 'bgra' field stores the expected framebuffer output (B=0, G=1, R=2, A=3).
    typedef struct { const char* name; uint8_t rgba[4]; BgraPixel bgra; } TestColor;
    static const TestColor test_colors[] = {
        { "R", { 255, 0,   0,   255 }, BGRA_OF(255, 0,   0)   },
        { "G", { 0,   255, 0,   255 }, BGRA_OF(0,   255, 0)   },
        { "B", { 0,   0,   255, 255 }, BGRA_OF(0,   0,   255) },
    };
    int ntc = (int)(sizeof(test_colors) / sizeof(test_colors[0]));

    for (int ti = 0; ti < ntc; ti++) {
        int col = ti;  // columns 0, 1, 2
        const TestColor* tc = &test_colors[ti];
        uint8_t rgba_buf[4] = { tc->rgba[0], tc->rgba[1], tc->rgba[2], tc->rgba[3] };
        uint8_t bgra_buf[4] = { tc->bgra.b, tc->bgra.g, tc->bgra.r, tc->bgra.a };

        // Test A: vesa_put_pixel
        vesa_put_pixel(col, 0, vesa_rgb(tc->rgba[0], tc->rgba[1], tc->rgba[2]));
        CHECK_PIXEL(col, 0, "vesa_put_pixel", tc->bgra.b, tc->bgra.g, tc->bgra.r);

        // Test B: vesa_blit_rgba (RGBA source → BGRA framebuffer)
        vesa_blit_rgba(col, 0, rgba_buf, 1, 1, 0, 0, 1, 1);
        CHECK_PIXEL(col, 0, "vesa_blit_rgba", tc->bgra.b, tc->bgra.g, tc->bgra.r);

        // Test C: vesa_blit_bgra (pre-converted BGRA source)
        vesa_blit_bgra(col, 0, bgra_buf, 1, 1, 0, 0, 1, 1);
        CHECK_PIXEL(col, 0, "vesa_blit_bgra", tc->bgra.b, tc->bgra.g, tc->bgra.r);

        // Test D: vesa_convert_rgba_bgra in-buffer, then vesa_blit_bgra
        uint8_t cv[4] = { tc->rgba[0], tc->rgba[1], tc->rgba[2], tc->rgba[3] };
        vesa_convert_rgba_bgra(cv, 4, 1, 1);
        if (cv[0] != tc->bgra.b || cv[1] != tc->bgra.g || cv[2] != tc->bgra.r) {
            vga_col = 0;
            VGA_PUT("VESA CONVERSION FAILURE "); VGA_PUT(tc->name);
            VGA_PUT(": got ["); VGA_BYTE(cv[0]); VGA_PUT(" ");
            VGA_BYTE(cv[1]); VGA_PUT(" "); VGA_BYTE(cv[2]);
            VGA_PUT("] exp ["); VGA_BYTE(tc->bgra.b); VGA_PUT(" ");
            VGA_BYTE(tc->bgra.g); VGA_PUT(" ");
            VGA_BYTE(tc->bgra.r); VGA_PUT("]");
            for (;;) { __asm__ volatile("cli; hlt"); }
        }
        vesa_blit_bgra(col, 0, cv, 1, 1, 0, 0, 1, 1);
        CHECK_PIXEL(col, 0, "vesa_convert_rgba_bgra", tc->bgra.b, tc->bgra.g, tc->bgra.r);
    }

    // ── Test E: vesa_blit_rgba_scaled (upscale 1×1 → 2×1) ────────
    // A single red RGBA pixel scaled to 2 pixel wide at row 1.
    // Both output pixels should be pure red (B=0, G=0, R=255).
    {
        uint8_t src_1x1[] = { 255, 0, 0, 255 };  // RGBA red
        vesa_blit_rgba_scaled(0, 1, 2, 1, src_1x1, 1, 1);
        uint8_t* _p0 = vesa.draw + 1*pitch + 0*bpp;
        uint8_t* _p1 = vesa.draw + 1*pitch + 1*bpp;
        if (_p0[0]!=0 || _p0[1]!=0 || _p0[2]!=255) {
            vga_col = 0; VGA_PUT("SCALE FAIL at (0,1): [");
            VGA_BYTE(_p0[0]); VGA_PUT(" "); VGA_BYTE(_p0[1]);
            VGA_PUT(" "); VGA_BYTE(_p0[2]); VGA_PUT("]");
            for (;;) { __asm__ volatile("cli; hlt"); }
        }
        if (_p1[0]!=0 || _p1[1]!=0 || _p1[2]!=255) {
            vga_col = 0; VGA_PUT("SCALE FAIL at (1,1): [");
            VGA_BYTE(_p1[0]); VGA_PUT(" "); VGA_BYTE(_p1[1]);
            VGA_PUT(" "); VGA_BYTE(_p1[2]); VGA_PUT("]");
            for (;;) { __asm__ volatile("cli; hlt"); }
        }
    }

    // ── Test F: vesa_blend_rgba (50% crossfade red → blue) ───────
    // Blend pure red and pure blue at alpha=128. Expected blended
    // RGBA: r=(255*127+0*128)/255=127, g=0, b=(0*127+255*128)/255=128.
    // BGRA framebuffer bytes: [128, 0, 127, 255].
    {
        uint8_t from[] = { 255, 0, 0, 255 };    // RGBA red
        uint8_t to[]   = { 0,   0, 255, 255 };  // RGBA blue
        vesa_blend_rgba(2, 1, 1, 1, from, to, 1, 1, 128);
        uint8_t* _p = vesa.draw + 1*pitch + 2*bpp;
        if (_p[0]!=128 || _p[1]!=0 || _p[2]!=127) {
            vga_col = 0; VGA_PUT("BLEND FAIL at (2,1): [");
            VGA_BYTE(_p[0]); VGA_PUT(" "); VGA_BYTE(_p[1]);
            VGA_PUT(" "); VGA_BYTE(_p[2]); VGA_PUT("]");
            VGA_PUT(" exp [80 00 7F]");
            for (;;) { __asm__ volatile("cli; hlt"); }
        }
    }

    // Restore the original pixel data — the splash screen won't
    // know anything happened.
    for (int pi = 0; pi < 6; pi++) {
        int sx = pi % 3, sy = pi / 3;
        uint8_t* p = vesa.draw + sy * pitch + sx * bpp;
        p[0] = saved[pi*4 + 0];
        p[1] = saved[pi*4 + 1];
        p[2] = saved[pi*4 + 2];
        p[3] = saved[pi*4 + 3];
    }

    VGA_PUT("PASS ");

    // Clean up the macro namespace
    #undef VGA_PUT
    #undef VGA_NIB
    #undef VGA_BYTE
    #undef CHECK_PIXEL
}

// ─── Fill rectangle ───────────────────────────────────────────────
void vesa_fill_rect(int x, int y, int w, int h, uint32_t color) {
    if (!vesa.active) return;
    for (int dy = 0; dy < h; dy++)
        for (int dx = 0; dx < w; dx++)
            vesa_put_pixel(x + dx, y + dy, color);
}

// ─── Clear screen ─────────────────────────────────────────────────
void vesa_clear(uint32_t color) {
    if (!vesa.active) return;
    vesa_fill_rect(0, 0, (int)vesa.width, (int)vesa.height, color);
}

// ─── Lines ────────────────────────────────────────────────────────
void vesa_hline(int x, int y, int len, uint32_t color) {
    if (!vesa.active) return;
    for (int i = 0; i < len; i++)
        vesa_put_pixel(x + i, y, color);
}

void vesa_vline(int x, int y, int len, uint32_t color) {
    if (!vesa.active) return;
    for (int i = 0; i < len; i++)
        vesa_put_pixel(x, y + i, color);
}

void vesa_draw_rect(int x, int y, int w, int h, uint32_t color) {
    vesa_hline(x,     y,     w, color);
    vesa_hline(x,     y+h-1, w, color);
    vesa_vline(x,     y,     h, color);
    vesa_vline(x+w-1, y,     h, color);
}

// ─── Built-in 8×16 bitmap font (ASCII 32–127) ─────────────────────
// Each character is 16 bytes (rows), each byte is 8 pixels wide (MSB=left).
// Glyphs cover printable ASCII. Non-printable chars render as space.

const uint8_t font8x16[96][16] = {
    // 0x20 space
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    // 0x21 !
    {0x00,0x00,0x18,0x3C,0x3C,0x3C,0x18,0x18,0x18,0x00,0x18,0x18,0x00,0x00,0x00,0x00},
    // 0x22 "
    {0x00,0x66,0x66,0x66,0x24,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    // 0x23 #
    {0x00,0x00,0x00,0x6C,0x6C,0xFE,0x6C,0x6C,0x6C,0xFE,0x6C,0x6C,0x00,0x00,0x00,0x00},
    // 0x24 $
    {0x18,0x18,0x7C,0xC6,0xC2,0xC0,0x7C,0x06,0x06,0x86,0xC6,0x7C,0x18,0x18,0x00,0x00},
    // 0x25 %
    {0x00,0x00,0x00,0x00,0xC2,0xC6,0x0C,0x18,0x30,0x60,0xC6,0x86,0x00,0x00,0x00,0x00},
    // 0x26 &
    {0x00,0x00,0x38,0x6C,0x6C,0x38,0x76,0xDC,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00},
    // 0x27 '
    {0x00,0x30,0x30,0x30,0x60,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    // 0x28 (
    {0x00,0x00,0x0C,0x18,0x30,0x30,0x30,0x30,0x30,0x30,0x18,0x0C,0x00,0x00,0x00,0x00},
    // 0x29 )
    {0x00,0x00,0x30,0x18,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x18,0x30,0x00,0x00,0x00,0x00},
    // 0x2A *
    {0x00,0x00,0x00,0x00,0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00,0x00,0x00,0x00,0x00},
    // 0x2B +
    {0x00,0x00,0x00,0x18,0x18,0x18,0xFF,0x18,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00},
    // 0x2C ,
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x18,0x30,0x00,0x00,0x00},
    // 0x2D -
    {0x00,0x00,0x00,0x00,0x00,0x00,0xFE,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    // 0x2E .
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x00},
    // 0x2F /
    {0x00,0x00,0x00,0x00,0x02,0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00,0x00,0x00,0x00},
    // 0x30 0
    {0x00,0x00,0x7C,0xC6,0xC6,0xCE,0xD6,0xD6,0xE6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    // 0x31 1
    {0x00,0x00,0x18,0x38,0x78,0x18,0x18,0x18,0x18,0x18,0x18,0x7E,0x00,0x00,0x00,0x00},
    // 0x32 2
    {0x00,0x00,0x7C,0xC6,0x06,0x0C,0x18,0x30,0x60,0xC0,0xC6,0xFE,0x00,0x00,0x00,0x00},
    // 0x33 3
    {0x00,0x00,0x7C,0xC6,0x06,0x06,0x3C,0x06,0x06,0x06,0xC6,0x7C,0x00,0x00,0x00,0x00},
    // 0x34 4
    {0x00,0x00,0x0C,0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x0C,0x0C,0x1E,0x00,0x00,0x00,0x00},
    // 0x35 5
    {0x00,0x00,0xFE,0xC0,0xC0,0xC0,0xFC,0x06,0x06,0x06,0xC6,0x7C,0x00,0x00,0x00,0x00},
    // 0x36 6
    {0x00,0x00,0x38,0x60,0xC0,0xC0,0xFC,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    // 0x37 7
    {0x00,0x00,0xFE,0xC6,0x06,0x06,0x0C,0x18,0x30,0x30,0x30,0x30,0x00,0x00,0x00,0x00},
    // 0x38 8
    {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7C,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    // 0x39 9
    {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7E,0x06,0x06,0x06,0x0C,0x78,0x00,0x00,0x00,0x00},
    // 0x3A :
    {0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00},
    // 0x3B ;
    {0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x18,0x18,0x18,0x30,0x00,0x00,0x00,0x00},
    // 0x3C <
    {0x00,0x00,0x06,0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x06,0x00,0x00,0x00,0x00,0x00},
    // 0x3D =
    {0x00,0x00,0x00,0x00,0x00,0xFE,0x00,0xFE,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    // 0x3E >
    {0x00,0x00,0x60,0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x60,0x00,0x00,0x00,0x00,0x00},
    // 0x3F ?
    {0x00,0x00,0x7C,0xC6,0xC6,0x0C,0x18,0x18,0x18,0x00,0x18,0x18,0x00,0x00,0x00,0x00},
    // 0x40 @
    {0x00,0x00,0x7C,0xC6,0xC6,0xDE,0xDE,0xDE,0xDC,0xC0,0xC0,0x7E,0x00,0x00,0x00,0x00},
    // 0x41 A
    {0x00,0x00,0x10,0x38,0x6C,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00},
    // 0x42 B
    {0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x66,0x66,0x66,0x66,0xFC,0x00,0x00,0x00,0x00},
    // 0x43 C
    {0x00,0x00,0x3C,0x66,0xC2,0xC0,0xC0,0xC0,0xC0,0xC2,0x66,0x3C,0x00,0x00,0x00,0x00},
    // 0x44 D
    {0x00,0x00,0xF8,0x6C,0x66,0x66,0x66,0x66,0x66,0x66,0x6C,0xF8,0x00,0x00,0x00,0x00},
    // 0x45 E
    {0x00,0x00,0xFE,0x66,0x62,0x68,0x78,0x68,0x60,0x62,0x66,0xFE,0x00,0x00,0x00,0x00},
    // 0x46 F
    {0x00,0x00,0xFE,0x66,0x62,0x68,0x78,0x68,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00},
    // 0x47 G
    {0x00,0x00,0x3C,0x66,0xC2,0xC0,0xC0,0xDE,0xC6,0xC6,0x66,0x3A,0x00,0x00,0x00,0x00},
    // 0x48 H
    {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00},
    // 0x49 I
    {0x00,0x00,0x3C,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00},
    // 0x4A J
    {0x00,0x00,0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0xCC,0xCC,0xCC,0x78,0x00,0x00,0x00,0x00},
    // 0x4B K
    {0x00,0x00,0xE6,0x66,0x66,0x6C,0x78,0x78,0x6C,0x66,0x66,0xE6,0x00,0x00,0x00,0x00},
    // 0x4C L
    {0x00,0x00,0xF0,0x60,0x60,0x60,0x60,0x60,0x60,0x62,0x66,0xFE,0x00,0x00,0x00,0x00},
    // 0x4D M
    {0x00,0x00,0xC6,0xEE,0xFE,0xFE,0xD6,0xC6,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00},
    // 0x4E N
    {0x00,0x00,0xC6,0xE6,0xF6,0xFE,0xDE,0xCE,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00},
    // 0x4F O
    {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    // 0x50 P
    {0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x60,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00},
    // 0x51 Q
    {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xD6,0xDE,0x7C,0x0C,0x0E,0x00,0x00},
    // 0x52 R
    {0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x6C,0x66,0x66,0x66,0xE6,0x00,0x00,0x00,0x00},
    // 0x53 S
    {0x00,0x00,0x7C,0xC6,0xC6,0x60,0x38,0x0C,0x06,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    // 0x54 T
    {0x00,0x00,0xFF,0xDB,0x99,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00},
    // 0x55 U
    {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    // 0x56 V
    {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x10,0x00,0x00,0x00,0x00},
    // 0x57 W
    {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xD6,0xD6,0xD6,0xFE,0xEE,0x6C,0x00,0x00,0x00,0x00},
    // 0x58 X
    {0x00,0x00,0xC6,0xC6,0x6C,0x7C,0x38,0x38,0x7C,0x6C,0xC6,0xC6,0x00,0x00,0x00,0x00},
    // 0x59 Y
    {0x00,0x00,0xCC,0xCC,0xCC,0xCC,0x78,0x30,0x30,0x30,0x30,0x78,0x00,0x00,0x00,0x00},
    // 0x5A Z
    {0x00,0x00,0xFE,0xC6,0x86,0x0C,0x18,0x30,0x60,0xC2,0xC6,0xFE,0x00,0x00,0x00,0x00},
    // 0x5B [
    {0x00,0x00,0x3C,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x3C,0x00,0x00,0x00,0x00},
    // 0x5C backslash
    {0x00,0x00,0x00,0x80,0xC0,0x60,0x30,0x18,0x0C,0x06,0x02,0x00,0x00,0x00,0x00,0x00},
    // 0x5D ]
    {0x00,0x00,0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00,0x00,0x00,0x00},
    // 0x5E ^
    {0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    // 0x5F _
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0x00,0x00,0x00,0x00},
    // 0x60 `
    {0x30,0x30,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    // 0x61 a
    {0x00,0x00,0x00,0x00,0x00,0x78,0x0C,0x7C,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00},
    // 0x62 b
    {0x00,0x00,0xE0,0x60,0x60,0x78,0x6C,0x66,0x66,0x66,0x66,0x7C,0x00,0x00,0x00,0x00},
    // 0x63 c
    {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0xC0,0xC0,0xC0,0xC6,0x7C,0x00,0x00,0x00,0x00},
    // 0x64 d
    {0x00,0x00,0x1C,0x0C,0x0C,0x3C,0x6C,0xCC,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00},
    // 0x65 e
    {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0xFE,0xC0,0xC0,0xC6,0x7C,0x00,0x00,0x00,0x00},
    // 0x66 f
    {0x00,0x00,0x1C,0x36,0x32,0x30,0x78,0x30,0x30,0x30,0x30,0x78,0x00,0x00,0x00,0x00},
    // 0x67 g
    {0x00,0x00,0x00,0x00,0x00,0x76,0xCC,0xCC,0xCC,0xCC,0xCC,0x7C,0x0C,0xCC,0x78,0x00},
    // 0x68 h
    {0x00,0x00,0xE0,0x60,0x60,0x6C,0x76,0x66,0x66,0x66,0x66,0xE6,0x00,0x00,0x00,0x00},
    // 0x69 i
    {0x00,0x00,0x18,0x18,0x00,0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00},
    // 0x6A j
    {0x00,0x00,0x06,0x06,0x00,0x0E,0x06,0x06,0x06,0x06,0x06,0x06,0x66,0x66,0x3C,0x00},
    // 0x6B k
    {0x00,0x00,0xE0,0x60,0x60,0x66,0x6C,0x78,0x78,0x6C,0x66,0xE6,0x00,0x00,0x00,0x00},
    // 0x6C l
    {0x00,0x00,0x38,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00},
    // 0x6D m
    {0x00,0x00,0x00,0x00,0x00,0xEC,0xFE,0xD6,0xD6,0xD6,0xD6,0xC6,0x00,0x00,0x00,0x00},
    // 0x6E n
    {0x00,0x00,0x00,0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x66,0x66,0x00,0x00,0x00,0x00},
    // 0x6F o
    {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    // 0x70 p
    {0x00,0x00,0x00,0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x66,0x7C,0x60,0x60,0xF0,0x00},
    // 0x71 q
    {0x00,0x00,0x00,0x00,0x00,0x76,0xCC,0xCC,0xCC,0xCC,0xCC,0x7C,0x0C,0x0C,0x1E,0x00},
    // 0x72 r
    {0x00,0x00,0x00,0x00,0x00,0xDC,0x76,0x66,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00},
    // 0x73 s
    {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0x60,0x38,0x0C,0xC6,0x7C,0x00,0x00,0x00,0x00},
    // 0x74 t
    {0x00,0x00,0x10,0x30,0x30,0xFC,0x30,0x30,0x30,0x30,0x36,0x1C,0x00,0x00,0x00,0x00},
    // 0x75 u
    {0x00,0x00,0x00,0x00,0x00,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00},
    // 0x76 v
    {0x00,0x00,0x00,0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x00,0x00,0x00,0x00},
    // 0x77 w
    {0x00,0x00,0x00,0x00,0x00,0xC6,0xC6,0xD6,0xD6,0xD6,0xFE,0x6C,0x00,0x00,0x00,0x00},
    // 0x78 x
    {0x00,0x00,0x00,0x00,0x00,0xC6,0x6C,0x38,0x38,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00},
    // 0x79 y
    {0x00,0x00,0x00,0x00,0x00,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0x76,0x0C,0x18,0xF0,0x00},
    // 0x7A z
    {0x00,0x00,0x00,0x00,0x00,0xFE,0xCC,0x18,0x30,0x60,0xC6,0xFE,0x00,0x00,0x00,0x00},
    // 0x7B {
    {0x00,0x00,0x0E,0x18,0x18,0x18,0x70,0x18,0x18,0x18,0x18,0x0E,0x00,0x00,0x00,0x00},
    // 0x7C |
    {0x00,0x00,0x18,0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x18,0x18,0x00,0x00,0x00,0x00},
    // 0x7D }
    {0x00,0x00,0x70,0x18,0x18,0x18,0x0E,0x18,0x18,0x18,0x18,0x70,0x00,0x00,0x00,0x00},
    // 0x7E ~
    {0x00,0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    // 0x7F DEL (unused, filler)
    {0x00,0x00,0x00,0x00,0x10,0x38,0x6C,0xC6,0xC6,0xFE,0x00,0x00,0x00,0x00,0x00,0x00},
};

// ─── Draw character ───────────────────────────────────────────────
void vesa_draw_char(int x, int y, char c, uint32_t fg, uint32_t bg) {
    if (!vesa.active) return;

    unsigned int idx = (unsigned int)c;
    if (idx < 32 || idx > 127) idx = 32; // default to space
    idx -= 32;

    const uint8_t* glyph = font8x16[idx];

    for (int row = 0; row < 16; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            uint32_t color = (bits & (0x80 >> col)) ? fg : bg;
            vesa_put_pixel(x + col, y + row, color);
        }
    }
}

// Scaled variant: stamps each font pixel as an NxN block instead of
// 1x1. Used by full-screen text apps (the editor) so an 80x25
// character grid can fill the actual display resolution instead of
// staying at native 8x16-per-cell size centered in a small area —
// rewriting every fixed coordinate in those apps to use real pixel
// math would be a much larger, riskier change than scaling the
// existing grid up.
void vesa_draw_char_scaled(int x, int y, char c, uint32_t fg, uint32_t bg, int scale) {
    if (!vesa.active) return;
    if (scale <= 1) { vesa_draw_char(x, y, c, fg, bg); return; }

    unsigned int idx = (unsigned int)c;
    if (idx < 32 || idx > 127) idx = 32;
    idx -= 32;

    const uint8_t* glyph = font8x16[idx];

    for (int row = 0; row < 16; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            uint32_t color = (bits & (0x80 >> col)) ? fg : bg;
            int px = x + col*scale;
            int py = y + row*scale;
            for (int sy = 0; sy < scale; sy++)
                vesa_hline(px, py+sy, scale, color);
        }
    }
}

// ─── Draw string ──────────────────────────────────────────────────
void vesa_draw_string(int x, int y, const char* s, uint32_t fg, uint32_t bg) {
    if (!vesa.active) return;
    int cx = x;
    for (int i = 0; s[i]; i++) {
        if (s[i] == '\n') {
            cx = x;
            y += 16;
        } else {
            vesa_draw_char(cx, y, s[i], fg, bg);
            cx += 8;
        }
    }
}
