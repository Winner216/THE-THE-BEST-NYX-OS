// cpu/mouse.c — PS/2 mouse driver for Nyx OS
//
// Talks to the standard 8042 PS/2 controller on ports 0x64/0x60,
// reads 3-byte mouse packets via IRQ12, and exposes a pixel-position
// cursor that the desktop layer draws on the VESA back buffer.
//
// The 16×16 arrow sprite uses a simple transparent-over-data scheme:
// pixels with value 0 are skipped (background shows through), 1 =
// white fill, 2 = black outline.

#include <mouse.h>
#include <desktop.h>
#include <game.h>
#include <screensaver.h>
#include <isr.h>
#include <io.h>
#include <vesa.h>
#include <stdint.h>

// ─── Mouse state (extern, defined here so the linker finds it) ────
MouseState mouse = {0};

// ─── Packet state machine ─────────────────────────────────────────
// The PS/2 mouse sends 3-byte packets on IRQ12. We track which byte
// of the current packet we're expecting in `packet_phase`, and the
// flag byte from phase 0 is used to interpret the deltas in phases
// 1 and 2. Mouse movement is accumulated into `mouse.px`/`mouse.py`
// from the IRQ handler directly (fast, no polling needed).
static int  packet_phase = 0;
static uint8_t packet_flags = 0;

// Accumulated PS/2 wheel (Z) deltas from the IRQ handler.  The
// desktop heartbeat reads and clears it each tick so it can never
// grow unbounded, routing it to whichever list is open.
int mouse_wheel_delta = 0;

// 1 once the device ID confirmed IntelliMouse 4-byte packets
// (device ID 3 or 4).  Only then does the packet machine expect a
// wheel byte as phase 3; otherwise it stays 3-byte and the cursor
// can never desync from a mode the device didn't actually enter.
static int mouse_wheel_mode = 0;

// Tracks the PIT tick when the last complete mouse packet was received.
// Used by the desktop to show a grab indicator — if packets are flowing,
// QEMU has input grabbed; if none arrive for ~3 seconds, input is idle.
static uint32_t last_activity_tick = 0;

// ─── Y-axis polarity ──────────────────────────────────────────
// Y deltas from this laptop's touchpad are inverted relative to
// screen space (positive = finger up). We negate dy in the Y
// packet handler so positive dy = finger down = cursor down.
// X deltas match screen space natively — no negation needed.

// ─── Ghost save/restore (for drag-and-drop overlay at IRQ time) ──
// Larger than the cursor sprite to cover the 64x62 tile + 8x16 label
// (~80x90 px). Uses the same dirty-rectangle pattern: save background,
// draw overlay, restore when the ghost moves.  The ghost is drawn BEFORE
// the cursor so the cursor appears on top when they overlap.
static uint32_t ghost_bg_buf[GHOST_W * GHOST_H];
static int ghost_bg_x = -1, ghost_bg_y = -1, ghost_bg_valid = 0;

// Called from desktop.c's heartbeat when the flying ghost animation
// is cancelled (e.g. user launches an app mid-animation). Invalidates
// the stale ghost background so the next drag doesn't restore old
// wallpaper over the freshly-placed icon.
void ghost_invalidate_bg(void) { ghost_bg_valid = 0; }

// ─── Group drag (multi-selection move) ────────────────────────────
// After a rubber-band box-select (box_sel[] in desktop.c), dragging
// any selected icon moves the WHOLE group together, preserving the
// relative layout.  The IRQ detects this at threshold-crossing: the
// pressed slot must be occupied AND box-selected AND the selection
// must contain more than one icon.  desktop.c's drop dispatch then
// translates every selected slot by the same (dcol,drow).
int g_group_drag_active = 0;   // 1 while a group drag is live
int g_group_anchor = -1;       // pressed slot (relative origin)
int g_group_count = 0;         // number of selected slots
int g_group_slots[GROUP_MAX_SLOTS];  // snapshot of selected slots

// Group ghost: a scaled-down cluster of mini tiles (one per selected
// app) drawn at the cursor so the user sees the whole group moving.
// Uses the same dirty-rectangle save/restore pattern as the single
// ghost.  The cluster is drawn BEFORE the cursor.
#define GROUP_GHOST_W 96
#define GROUP_GHOST_H 80
static uint32_t group_bg_buf[GROUP_GHOST_W * GROUP_GHOST_H];
static int group_bg_x = -1, group_bg_y = -1, group_bg_valid = 0;

void group_ghost_invalidate(void) { group_bg_valid = 0; }

// Restore the desktop under the last group ghost, then mark invalid.
static void group_ghost_restore_bg(uint8_t* fb) {
    if (!group_bg_valid) return;
    int bpp = (int)(vesa.bpp / 8);
    int pitch = (int)vesa.pitch;
    int gx = group_bg_x, gy = group_bg_y;
    int gw = GROUP_GHOST_W, gh = GROUP_GHOST_H;
    if (gx < 0) { gw += gx; gx = 0; }
    if (gy < 0) { gh += gy; gy = 0; }
    if (gx + gw > (int)vesa.width)  gw = (int)vesa.width - gx;
    if (gy + gh > (int)vesa.height) gh = (int)vesa.height - gy;
    if (gw <= 0 || gh <= 0) { group_bg_valid = 0; return; }
    for (int row = 0; row < gh; row++) {
        uint8_t* dst = fb + (uint32_t)(gy + row) * (uint32_t)pitch + (uint32_t)gx * (uint32_t)bpp;
        for (int col = 0; col < gw; col++) {
            uint32_t c = group_bg_buf[(uint32_t)row * GROUP_GHOST_W + (uint32_t)col];
            dst[col * (uint32_t)bpp + 0] = c & 0xFF;
            dst[col * (uint32_t)bpp + 1] = (c >> 8) & 0xFF;
            dst[col * (uint32_t)bpp + 2] = (c >> 16) & 0xFF;
            if (bpp == 4) dst[col * 4 + 3] = 0;
        }
    }
    group_bg_valid = 0;
}

// Save the desktop under the group ghost's region.
static void group_ghost_save_bg(int mx, int my, uint8_t* fb) {
    group_bg_x = mx; group_bg_y = my; group_bg_valid = 1;
    int bpp = (int)(vesa.bpp / 8);
    int pitch = (int)vesa.pitch;
    int gx = mx, gy = my;
    int gw = GROUP_GHOST_W, gh = GROUP_GHOST_H;
    if (gx < 0) { gw += gx; gx = 0; }
    if (gy < 0) { gh += gy; gy = 0; }
    if (gx + gw > (int)vesa.width)  gw = (int)vesa.width - gx;
    if (gy + gh > (int)vesa.height) gh = (int)vesa.height - gy;
    if (gw <= 0 || gh <= 0) { group_bg_valid = 0; return; }
    for (int row = 0; row < gh; row++) {
        const uint8_t* src = fb + (uint32_t)(gy + row) * (uint32_t)pitch + (uint32_t)gx * (uint32_t)bpp;
        for (int col = 0; col < gw; col++) {
            uint32_t b = src[col * (uint32_t)bpp + 0];
            uint32_t g = src[col * (uint32_t)bpp + 1];
            uint32_t r = src[col * (uint32_t)bpp + 2];
            group_bg_buf[(uint32_t)row * GROUP_GHOST_W + (uint32_t)col] = (r << 16) | (g << 8) | b;
        }
    }
}

// Forward declaration — the real definition lives below (with the
// single-ghost helpers); the group block sits above it in this file.
static int mouse_rr_inset(int dy, int h, int r);

// Draw the group ghost cluster at the cursor: a mini tile for each
// selected app arranged at its relative grid offset, scaled down to
// fit GROUP_GHOST_W/H.  Only the cluster's bounding box region is
// drawn (the caller saves/restores the full GROUP_GHOST area).
static void mouse_draw_group_ghost_on(uint8_t* fb) {
    if (!vesa.active || !fb || !g_group_drag_active || g_group_count < 2) return;
    if (!mouse.drag_active) return;
    int bpp = (int)(vesa.bpp / 8);
    int pitch = (int)vesa.pitch;
    // Compute the selected slots' bounding box in grid coordinates.
    int min_c = 999, max_c = -1, min_r = 999, max_r = -1;
    for (int i = 0; i < g_group_count; i++) {
        int s = g_group_slots[i];
        if (s < 0 || s >= g_grid_slots) continue;
        int c = s % g_grid_cols, r = s / g_grid_cols;
        if (c < min_c) min_c = c;
        if (c > max_c) max_c = c;
        if (r < min_r) min_r = r;
        if (r > max_r) max_r = r;
    }
    if (max_c < 0) return;
    int span_c = max_c - min_c + 1, span_r = max_r - min_r + 1;
    // Tile pitch scales so the whole cluster fits the ghost region.
    int tile = 20, gap = 4;
    int avail_w = GROUP_GHOST_W - 8, avail_h = GROUP_GHOST_H - 8;
    int pitch_x = avail_w / span_c, pitch_y = avail_h / span_r;
    if (pitch_x < tile + gap) tile = pitch_x - gap < 6 ? 6 : pitch_x - gap;
    if (pitch_y < tile + gap) tile = pitch_y - gap < 6 ? 6 : pitch_y - gap;
    if (tile < 6) tile = 6;
    gap = 4;
    int step_x = tile + gap, step_y = tile + gap;
    // Center the cluster on the cursor.  (Uses the real variable names
    // here — the ghost_filter_px/py #defines are declared later in the
    // file, below this block.)
    int cx = g_ghost_filter_px, cy = g_ghost_filter_py;
    int cluster_w = span_c * step_x - gap;
    int cluster_h = span_r * step_y - gap;
    int ox = cx - cluster_w / 2;
    int oy = cy - cluster_h / 2;
    // Anchor offset (the pressed slot is drawn at the cursor center).
    int anc_c = g_group_anchor >= 0 ? (g_group_anchor % g_grid_cols) : min_c;
    int anc_r = g_group_anchor >= 0 ? (g_group_anchor / g_grid_cols) : min_r;
    ox = cx - ((anc_c - min_c) * step_x + tile / 2);
    oy = cy - ((anc_r - min_r) * step_y + tile / 2);
    for (int i = 0; i < g_group_count; i++) {
        int s = g_group_slots[i];
        if (s < 0 || s >= g_grid_slots) continue;
        int c = s % g_grid_cols, r = s / g_grid_cols;
        int tx = ox + (c - min_c) * step_x;
        int ty = oy + (r - min_r) * step_y;
        // Mini tile: dark purple rounded gradient + bright border.
        int rad = 4;
        for (int y = 0; y < tile; y++) {
            int py = ty + y;
            if (py < 0 || py >= (int)vesa.height) continue;
            int t = y * 255 / tile;
            uint32_t rr = 0x2E - (0x2E - 0x1E) * t / 255;
            uint32_t gg = 0x20 - (0x20 - 0x12) * t / 255;
            uint32_t bb = 0x50 - (0x50 - 0x40) * t / 255;
            uint32_t color = (rr << 16) | (gg << 8) | bb;
            int ins = mouse_rr_inset(y, tile, rad);
            for (int x = ins; x < tile - ins; x++) {
                int px = tx + x;
                if (px < 0 || px >= (int)vesa.width) continue;
                uint8_t* p = fb + (uint32_t)py * (uint32_t)pitch + (uint32_t)px * (uint32_t)bpp;
                p[0] = color & 0xFF; p[1] = (color >> 8) & 0xFF; p[2] = (color >> 16) & 0xFF;
                if (bpp == 4) p[3] = 0;
            }
        }
        // Border
        uint32_t border = 0xB088FF;
        for (int y = 0; y < tile; y++) {
            int py = ty + y;
            if (py < 0 || py >= (int)vesa.height) continue;
            int ins = mouse_rr_inset(y, tile, rad);
            int x0 = tx + ins, x1 = tx + tile - 1 - ins;
            if (x0 >= 0 && x0 < (int)vesa.width) {
                uint8_t* p = fb + (uint32_t)py * (uint32_t)pitch + (uint32_t)x0 * (uint32_t)bpp;
                p[0] = border & 0xFF; p[1] = (border >> 8) & 0xFF; p[2] = (border >> 16) & 0xFF;
                if (bpp == 4) p[3] = 0;
            }
            if (x1 >= 0 && x1 < (int)vesa.width) {
                uint8_t* p = fb + (uint32_t)py * (uint32_t)pitch + (uint32_t)x1 * (uint32_t)bpp;
                p[0] = border & 0xFF; p[1] = (border >> 8) & 0xFF; p[2] = (border >> 16) & 0xFF;
                if (bpp == 4) p[3] = 0;
            }
        }
    }
}

// ─── Rubber-band selection box (IRQ-time) ─────────────────────────
// Dragging from EMPTY desktop draws a translucent selection window
// that follows the cursor at 200 Hz (same dirty-rectangle pattern as
// the ghost).  The box is drawn with XOR so drawing the same rect
// twice restores the desktop — no save buffer needed at any size.
// The anchor corner is g_sel_x0/g_sel_y0 (press-down position); the
// moving corner is the cursor.  desktop.c's drop dispatch reads the
// final rect and marks the apps inside it as selected.
int g_sel_drag_active = 0;   // 1 while a selection-box drag is live
int g_sel_x0 = 0, g_sel_y0 = 0;  // press-down anchor corner
static int sel_box_old_x1 = -1, sel_box_old_y1 = -1;  // last moving corner
static int sel_box_drawn = 0;    // 1 = a box is currently XOR'd on vesa.fb
// Present-generation guard (see mouse.h): desktop.c's finish_frame()
// bumps sel_box_present_gen right after vesa_present() wipes the screen,
// so the IRQ knows its last-drawn box was destroyed and must NOT
// XOR-erase clean desktop pixels (phantom box).  sel_box_drawn_gen
// records the generation in which the current box was drawn.
int sel_box_present_gen = 0;
static int sel_box_drawn_gen = -1;

// 1 while vesa_present() is mid-copy (or compositing overlays) on
// vesa.fb.  The IRQ draw chain skips ALL overlay drawing while this is
// set: the present will wipe whatever the IRQ draws anyway, and a box/
// cursor written mid-copy gets torn by the overwrite.  Set/cleared by
// vesa_present() (externed there); read by the IRQ handler here.
volatile int __present_in_progress = 0;

// XOR-tint one pixel toward translucent blue (self-restoring: XOR
// twice = original).  A partial per-channel mask (instead of a full
// 0xFF inversion, which looked like a harsh photographic negative)
// lifts the pixel toward a soft selection-blue while still letting
// the wallpaper tone through.  NOTE: the VESA framebuffer stores
// channels in B,G,R order (p[0]=Blue, p[2]=Red), so the red mask is
// written to byte 2 and the blue mask to byte 0.
static void sel_xor_px(uint8_t* fb, int x, int y, uint8_t mr, uint8_t mg, uint8_t mb) {
    if (x < 0 || y < 0 || x >= (int)vesa.width || y >= (int)vesa.height) return;
    int bpp = (int)(vesa.bpp / 8);
    uint8_t* p = fb + (uint32_t)y * (uint32_t)vesa.pitch + (uint32_t)x * (uint32_t)bpp;
    p[0] ^= mb;   // B
    p[1] ^= mg;   // G
    p[2] ^= mr;   // R
    if (bpp == 4) p[3] = 0;
}

// XOR-draw a translucent selection box from corner (x0,y0) to (x1,y1):
// a bright 2px border plus a 50% checkerboard fill of soft blue, so
// the desktop shows through between tinted pixels (true translucency
// instead of the old every-other-row banding).  Every pixel is still
// XOR'd exactly once per draw, so redrawing the same rect restores
// the desktop — no save buffer needed at any box size.
static void sel_xor_box(uint8_t* fb, int x0, int y0, int x1, int y1) {
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    // Border (2px), bright blue-white; drawn once per pixel (the
    // vertical runs skip rows already covered by the horizontal runs
    // so corners are never double-XOR'd back to the desktop).
    // Boxes narrower/taller than 3px in either axis are degenerate
    // (barely-started drags) — skip them entirely.
    if (y1 - y0 >= 3 && x1 - x0 >= 3) {
        for (int x = x0; x <= x1; x++) {
            sel_xor_px(fb, x, y0, 0x88, 0xC0, 0xF8);
            sel_xor_px(fb, x, y0+1, 0x88, 0xC0, 0xF8);
            sel_xor_px(fb, x, y1, 0x88, 0xC0, 0xF8);
            sel_xor_px(fb, x, y1-1, 0x88, 0xC0, 0xF8);
        }
        for (int y = y0 + 2; y <= y1 - 2; y++) {
            sel_xor_px(fb, x0, y, 0x88, 0xC0, 0xF8);
            sel_xor_px(fb, x0+1, y, 0x88, 0xC0, 0xF8);
            sel_xor_px(fb, x1, y, 0x88, 0xC0, 0xF8);
            sel_xor_px(fb, x1-1, y, 0x88, 0xC0, 0xF8);
        }
        // Fill: 50% checkerboard dither of translucent blue.
        for (int y = y0 + 2; y < y1 - 1; y++)
            for (int x = x0 + 2 + (y & 1); x < x1 - 1; x += 2)
                sel_xor_px(fb, x, y, 0x20, 0x50, 0x90);
    }}

// Erase the previous box (XOR again) and draw the box at the current
// cursor corner.  Called by the IRQ draw chain while a select-drag is
// active, before the cursor is drawn on top.
static void sel_box_update(uint8_t* fb) {
    // XOR-erase the previous box ONLY if it was drawn in the current
    // present-generation.  If vesa_present() wiped the screen since the
    // box was drawn, XOR-erasing would tint clean desktop pixels and
    // leave a phantom box — instead just draw the new box fresh.
    if (sel_box_drawn && sel_box_old_x1 >= 0 &&
        sel_box_drawn_gen == sel_box_present_gen) {
        sel_xor_box(fb, g_sel_x0, g_sel_y0, sel_box_old_x1, sel_box_old_y1);
    }
    sel_xor_box(fb, g_sel_x0, g_sel_y0, mouse.px, mouse.py);
    sel_box_old_x1 = mouse.px;
    sel_box_old_y1 = mouse.py;
    sel_box_drawn = 1;
    sel_box_drawn_gen = sel_box_present_gen;
}

// Erase the currently-painted box immediately (button release).  The
// drop dispatch redraws the whole scene, but a rapid re-drag could
// start before that present runs — erasing here keeps a stale box from
// lingering beside the new one.  No-op if nothing is actually drawn.
void mouse_sel_box_erase_now(void) {
    if (sel_box_drawn && sel_box_old_x1 >= 0 &&
        sel_box_drawn_gen == sel_box_present_gen) {
        sel_xor_box(vesa.fb, g_sel_x0, g_sel_y0, sel_box_old_x1, sel_box_old_y1);
    }
    sel_box_drawn = 0;
    sel_box_old_x1 = -1;
    sel_box_old_y1 = -1;
}

// Re-draw the box after vesa_present() wiped it, keeping the IRQ's
// draw state in sync with the actual screen (no flicker, no phantom).
// Called from desktop.c finish_frame() right after the present.
void mouse_sel_box_redraw_after_present(void) {
    if (!g_sel_drag_active || !vesa.active) return;
    // If the IRQ drew a box in this NEW generation (between the present
    // and here), it is genuinely on screen — erase it before redrawing.
    if (sel_box_drawn && sel_box_old_x1 >= 0 &&
        sel_box_drawn_gen == sel_box_present_gen) {
        sel_xor_box(vesa.fb, g_sel_x0, g_sel_y0, sel_box_old_x1, sel_box_old_y1);
    }
    sel_box_drawn = 0;
    sel_box_update(vesa.fb);
}

// ─── Ghost position (instant cursor tracking) ─────────────────────
// The ghost tile is drawn exactly at the cursor position: every
// IRQ frame the drag block rounds mouse.px/py into ghost_filter_px/
// py (see ghost_round_to_cursor below), so the ghost follows the
// cursor 1:1 with zero lag.
//
// (An earlier velocity-adaptive low-pass filter interpolated the
//  ghost toward the cursor at α≈0.3 until the velocity threshold
//  kicked in — the ghost visibly lagged behind for up to a second
//  at drag start.  Removed per user request for instant tracking.)
int g_ghost_filter_px = 0, g_ghost_filter_py = 0;
#define ghost_filter_px g_ghost_filter_px
#define ghost_filter_py g_ghost_filter_py

// ─── Rounded-corner helpers (match desktop.c ICON_TILE_R) ────────
// The desktop tiles are round squircles (corner radius 14), so the
// drag ghost + snap preview drawn here at IRQ time use the same
// circular corner profile — otherwise the dragged tile looks square
// next to the round resting tiles.
#define MOUSE_TILE_R 14

static int mouse_isqrt(int n) {
    if (n <= 0) return 0;
    int x = n, y = (x + 1) / 2;
    while (y < x) { x = y; y = (x + n / x) / 2; }
    return x;
}

// Horizontal inset (px from each edge) of a circular corner at row
// dy (0-based from the tile top) for a tile of height h and corner
// radius r.  Mirrors the bottom rows so the corner is symmetric.
static int mouse_rr_inset(int dy, int h, int r) {
    int d = dy;
    if (dy >= h - r) d = h - 1 - dy;   // mirror the bottom corner
    if (d <= 0) return r;
    if (d >= r) return 0;
    int dd = r - d;
    return r - mouse_isqrt(r * r - dd * dd);
}

// ─── Per-frame integer rounding (crisp pixel lock) ───────────────
// Rounds the ghost onto the cursor every IRQ frame with integer
// arithmetic.  mouse.px/py are whole pixels, so the per-frame delta
// is always an exact pixel count: when |delta| < 1px (delta == 0 on
// both axes) the ghost is already pixel-locked onto the cursor and
// stays put — crisp, zero lag, no sub-pixel drift.  This is a
// hardening guard: if a future change ever reintroduces fractional
// ghost tracking, the snap branch guarantees the ghost can never
// render a sub-pixel offset (the classic source of "shaky" trails).
static void ghost_round_to_cursor(void) {
    int dx = mouse.px - ghost_filter_px;
    int dy = mouse.py - ghost_filter_py;
    if (dx == 0 && dy == 0) return;   // |delta| < 1px → snapped (locked)
    ghost_filter_px = mouse.px;       // rounded delta = exact whole px
    ghost_filter_py = mouse.py;
}

// ─── Snap-to-grid preview save/restore ───────────────────────────
// Separate buffer for the magnetic snap preview so it doesn't
// interfere with the drag ghost or the cursor save/restore.
static uint32_t preview_bg_buf[PREVIEW_W * PREVIEW_H];
static int preview_bg_x = -1, preview_bg_y = -1, preview_bg_valid = 0;

// ─── Grid-cell highlight save/restore ────────────────────────────
// A third dirty-rectangle layer drawn BELOW the preview ghost,
// marking the grid cell that the cursor is currently hovering over.
// Large enough for any cell size (capped at 128×128 px).
#define CELL_HIGHLIGHT_W 128
#define CELL_HIGHLIGHT_H 128
static uint32_t cell_bg_buf[CELL_HIGHLIGHT_W * CELL_HIGHLIGHT_H];
static int cell_bg_x = -1, cell_bg_y = -1, cell_bg_valid = 0;
static int cell_bg_w = 0, cell_bg_h = 0;  // actual saved region size

void preview_restore_bg(uint8_t* fb) {
    if (!preview_bg_valid) return;
    int bpp = (int)(vesa.bpp / 8);
    int pitch = (int)vesa.pitch;
    int gx = preview_bg_x, gy = preview_bg_y;
    int gw = PREVIEW_W, gh = PREVIEW_H;
    if (gx < 0) { gw += gx; gx = 0; }
    if (gy < 0) { gh += gy; gy = 0; }
    if (gx + gw > (int)vesa.width)  gw = (int)vesa.width - gx;
    if (gy + gh > (int)vesa.height) gh = (int)vesa.height - gy;
    if (gw <= 0 || gh <= 0) { preview_bg_valid = 0; return; }
    for (int row = 0; row < gh; row++) {
        uint8_t* dst = fb + (uint32_t)(gy + row) * (uint32_t)pitch + (uint32_t)gx * (uint32_t)bpp;
        for (int col = 0; col < gw; col++) {
            uint32_t c = preview_bg_buf[(uint32_t)row * PREVIEW_W + (uint32_t)col];
            dst[col * (uint32_t)bpp + 0] = c & 0xFF;
            dst[col * (uint32_t)bpp + 1] = (c >> 8) & 0xFF;
            dst[col * (uint32_t)bpp + 2] = (c >> 16) & 0xFF;
            if (bpp == 4) dst[col * 4 + 3] = 0;
        }
    }
    preview_bg_valid = 0;
}

void preview_save_bg(int mx, int my, uint8_t* fb) {
    preview_bg_x = mx; preview_bg_y = my; preview_bg_valid = 1;
    int bpp = (int)(vesa.bpp / 8);
    int pitch = (int)vesa.pitch;
    int gx = mx, gy = my;
    int gw = PREVIEW_W, gh = PREVIEW_H;
    if (gx < 0) { gw += gx; gx = 0; }
    if (gy < 0) { gh += gy; gy = 0; }
    if (gx + gw > (int)vesa.width)  gw = (int)vesa.width - gx;
    if (gy + gh > (int)vesa.height) gh = (int)vesa.height - gy;
    if (gw <= 0 || gh <= 0) { preview_bg_valid = 0; return; }
    for (int row = 0; row < gh; row++) {
        const uint8_t* src = fb + (uint32_t)(gy + row) * (uint32_t)pitch + (uint32_t)gx * (uint32_t)bpp;
        for (int col = 0; col < gw; col++) {
            uint32_t b = src[col * (uint32_t)bpp + 0];
            uint32_t g = src[col * (uint32_t)bpp + 1];
            uint32_t r = src[col * (uint32_t)bpp + 2];
            preview_bg_buf[(uint32_t)row * PREVIEW_W + (uint32_t)col] = (r << 16) | (g << 8) | b;
        }
    }
}

// ─── Cell highlight save/restore ────────────────────────────────
void cell_restore_bg(uint8_t* fb) {
    if (!cell_bg_valid) return;
    int bpp = (int)(vesa.bpp / 8);
    int pitch = (int)vesa.pitch;
    int gx = cell_bg_x, gy = cell_bg_y;
    // Use the exact dimensions stored at save time, clamped to screen
    int gw = cell_bg_w, gh = cell_bg_h;
    if (gx < 0) { gw += gx; gx = 0; }
    if (gy < 0) { gh += gy; gy = 0; }
    if (gx + gw > (int)vesa.width)  gw = (int)vesa.width - gx;
    if (gy + gh > (int)vesa.height) gh = (int)vesa.height - gy;
    if (gw <= 0 || gh <= 0) { cell_bg_valid = 0; return; }
    for (int row = 0; row < gh; row++) {
        uint8_t* dst = fb + (uint32_t)(gy + row) * (uint32_t)pitch + (uint32_t)gx * (uint32_t)bpp;
        for (int col = 0; col < gw; col++) {
            uint32_t c = cell_bg_buf[(uint32_t)row * CELL_HIGHLIGHT_W + (uint32_t)col];
            dst[col * (uint32_t)bpp + 0] = c & 0xFF;
            dst[col * (uint32_t)bpp + 1] = (c >> 8) & 0xFF;
            dst[col * (uint32_t)bpp + 2] = (c >> 16) & 0xFF;
            if (bpp == 4) dst[col * 4 + 3] = 0;
        }
    }
    cell_bg_valid = 0;
}

void cell_save_bg(int mx, int my, int cw, int ch, uint8_t* fb) {
    if (cw <= 0 || ch <= 0) return;
    cell_bg_x = mx; cell_bg_y = my; cell_bg_w = cw; cell_bg_h = ch;
    cell_bg_valid = 1;
    int bpp = (int)(vesa.bpp / 8);
    int pitch = (int)vesa.pitch;
    int gx = mx, gy = my;
    int gw = cw, gh = ch;
    if (gx < 0) { gw += gx; gx = 0; }
    if (gy < 0) { gh += gy; gy = 0; }
    if (gx + gw > (int)vesa.width)  gw = (int)vesa.width - gx;
    if (gy + gh > (int)vesa.height) gh = (int)vesa.height - gy;
    if (gw <= 0 || gh <= 0) { cell_bg_valid = 0; return; }
    for (int row = 0; row < gh; row++) {
        const uint8_t* src = fb + (uint32_t)(gy + row) * (uint32_t)pitch + (uint32_t)gx * (uint32_t)bpp;
        for (int col = 0; col < gw; col++) {
            uint32_t b = src[col * (uint32_t)bpp + 0];
            uint32_t g = src[col * (uint32_t)bpp + 1];
            uint32_t r = src[col * (uint32_t)bpp + 2];
            cell_bg_buf[(uint32_t)row * CELL_HIGHLIGHT_W + (uint32_t)col] = (r << 16) | (g << 8) | b;
        }
    }
}

// ─── Grid-cell highlight ─────────────────────────────────────────
// Draws a subtle 1-pixel teal border around the grid cell at (cx,cy)
// with size (cw,ch).  Drawn BELOW the snap preview and drag ghost
// so it appears as a faint background glow showing the drop target.
static void mouse_draw_cell_highlight(uint8_t* fb, int cx, int cy, int cw, int ch) {
    if (!vesa.active || !fb) return;
    if (cw <= 0 || ch <= 0) return;
    int bpp = (int)(vesa.bpp / 8);
    int pitch = (int)vesa.pitch;
    int bw = (int)vesa.width;
    int bh = (int)vesa.height;

    // ── Pulse brightness with a smooth 200Hz triangle wave ───────
    // The border gently breathes in/out over ~2 seconds using a
    // local frame counter (called on every PS/2 packet at 200 Hz),
    // giving 400 smooth steps per cycle instead of 40 chunky PIT
    // ticks — no visible stepping.
    static uint32_t cell_anim_tick = 0;
    int sweep = (++cell_anim_tick) % 400;
    if (sweep >= 200) sweep = 400 - sweep;
    // brightness[128..248] — never goes fully off, peaks at 97% teal
    uint32_t pulse = 128 + (uint32_t)sweep * 120 / 200;

    // Base teal (0x44AACC) scaled by pulse
    uint32_t r = (0x44 * pulse / 256) << 16;
    uint32_t g = (0xAA * pulse / 256) << 8;
    uint32_t b = (0xCC * pulse / 256);
    uint32_t color = r | g | b;

    // ── Draw 1-pixel border around the cell ──────────────────────
    int cxe = cx + cw, cye = cy + ch;
    // Top edge
    for (int x = cx; x < cxe; x++) {
        if (x < 0 || x >= bw || cy < 0 || cy >= bh) continue;
        uint8_t* p = fb + (uint32_t)cy * (uint32_t)pitch + (uint32_t)x * (uint32_t)bpp;
        p[0] = color & 0xFF; p[1] = (color >> 8) & 0xFF; p[2] = (color >> 16) & 0xFF;
        if (bpp == 4) p[3] = 0;
    }
    // Bottom edge
    int by = cye - 1;
    for (int x = cx; x < cxe; x++) {
        if (x < 0 || x >= bw || by < 0 || by >= bh) continue;
        uint8_t* p = fb + (uint32_t)by * (uint32_t)pitch + (uint32_t)x * (uint32_t)bpp;
        p[0] = color & 0xFF; p[1] = (color >> 8) & 0xFF; p[2] = (color >> 16) & 0xFF;
        if (bpp == 4) p[3] = 0;
    }
    // Left edge
    for (int y = cy; y < cye; y++) {
        if (cx < 0 || cx >= bw || y < 0 || y >= bh) continue;
        uint8_t* p = fb + (uint32_t)y * (uint32_t)pitch + (uint32_t)cx * (uint32_t)bpp;
        p[0] = color & 0xFF; p[1] = (color >> 8) & 0xFF; p[2] = (color >> 16) & 0xFF;
        if (bpp == 4) p[3] = 0;
    }
    // Right edge
    int rx = cxe - 1;
    for (int y = cy; y < cye; y++) {
        if (rx < 0 || rx >= bw || y < 0 || y >= bh) continue;
        uint8_t* p = fb + (uint32_t)y * (uint32_t)pitch + (uint32_t)rx * (uint32_t)bpp;
        p[0] = color & 0xFF; p[1] = (color >> 8) & 0xFF; p[2] = (color >> 16) & 0xFF;
        if (bpp == 4) p[3] = 0;
    }
}

void ghost_restore_bg(uint8_t* fb) {
    if (!ghost_bg_valid) return;
    int bpp = (int)(vesa.bpp / 8);
    int pitch = (int)vesa.pitch;
    int gx = ghost_bg_x, gy = ghost_bg_y;
    int gw = GHOST_W, gh = GHOST_H;
    if (gx < 0) { gw += gx; gx = 0; }
    if (gy < 0) { gh += gy; gy = 0; }
    if (gx + gw > (int)vesa.width)  gw = (int)vesa.width - gx;
    if (gy + gh > (int)vesa.height) gh = (int)vesa.height - gy;
    if (gw <= 0 || gh <= 0) { ghost_bg_valid = 0; return; }
    for (int row = 0; row < gh; row++) {
        uint8_t* dst = fb + (uint32_t)(gy + row) * (uint32_t)pitch + (uint32_t)gx * (uint32_t)bpp;
        for (int col = 0; col < gw; col++) {
            uint32_t c = ghost_bg_buf[(uint32_t)row * GHOST_W + (uint32_t)col];
            dst[col * (uint32_t)bpp + 0] = c & 0xFF;
            dst[col * (uint32_t)bpp + 1] = (c >> 8) & 0xFF;
            dst[col * (uint32_t)bpp + 2] = (c >> 16) & 0xFF;
            if (bpp == 4) dst[col * 4 + 3] = 0;
        }
    }
    ghost_bg_valid = 0;
}

void ghost_save_bg(int mx, int my, uint8_t* fb) {
    ghost_bg_x = mx; ghost_bg_y = my; ghost_bg_valid = 1;
    int bpp = (int)(vesa.bpp / 8);
    int pitch = (int)vesa.pitch;
    int gx = mx, gy = my;
    int gw = GHOST_W, gh = GHOST_H;
    if (gx < 0) { gw += gx; gx = 0; }
    if (gy < 0) { gh += gy; gy = 0; }
    if (gx + gw > (int)vesa.width)  gw = (int)vesa.width - gx;
    if (gy + gh > (int)vesa.height) gh = (int)vesa.height - gy;
    if (gw <= 0 || gh <= 0) { ghost_bg_valid = 0; return; }
    for (int row = 0; row < gh; row++) {
        const uint8_t* src = fb + (uint32_t)(gy + row) * (uint32_t)pitch + (uint32_t)gx * (uint32_t)bpp;
        for (int col = 0; col < gw; col++) {
            uint32_t b = src[col * (uint32_t)bpp + 0];
            uint32_t g = src[col * (uint32_t)bpp + 1];
            uint32_t r = src[col * (uint32_t)bpp + 2];
            ghost_bg_buf[(uint32_t)row * GHOST_W + (uint32_t)col] = (r << 16) | (g << 8) | b;
        }
    }
}

// ─── Drag ghost overlay (IRQ-time) ────────────────────────────────
// Draws a simple tile border + app label directly to vesa.fb at the
// cursor position when drag_active is 1 and g_ghost_app_idx >= 0.
// The ghost is drawn BEFORE the cursor so the cursor appears on top.
// Uses the font8x16 from vesa.c for the label text.
//
// Position: reads from ghost_filter_px/py, which the IRQ handler
// keeps locked to mouse.px/py — the ghost tracks the cursor 1:1.
static void mouse_draw_ghost_on(uint8_t* fb) {
    if (!vesa.active || !fb || g_ghost_app_idx < 0) return;
    // The ghost draws during two scenarios:
    // 1. Active drag (drag_active == 1) — the user is dragging an icon
    // 2. Flying ghost animation (mag_active == 1) — the 200Hz slide-in
    //    after drop, where the ghost moves to its target slot.
    if (!mouse.drag_active && !mag_active) return;

    int cx = ghost_filter_px, cy = ghost_filter_py;
    int tw = 64, th = 62;  // tile dimensions (matches desktop.c ghost)
    int gx = cx - tw / 2, gy = cy - th / 2;
    int bpp = (int)(vesa.bpp / 8);
    int pitch = (int)vesa.pitch;

    // ── Ghost tile background: vertical gradient (dark purple) ──
    // Rounded corners (radius MOUSE_TILE_R) match the desktop tiles.
    for (int y = 0; y < th; y++) {
        int py = gy + y;
        if (py < 0 || py >= (int)vesa.height) continue;
        int t = y * 255 / th;
        uint32_t r = 0x2E - (0x2E - 0x1E) * t / 255;
        uint32_t g = 0x20 - (0x20 - 0x12) * t / 255;
        uint32_t b = 0x50 - (0x50 - 0x40) * t / 255;
        uint32_t color = (r << 16) | (g << 8) | b;
        int ins = mouse_rr_inset(y, th, MOUSE_TILE_R);
        for (int x = ins; x < tw - ins; x++) {
            int px = gx + x;
            if (px < 0 || px >= (int)vesa.width) continue;
            uint8_t* p = fb + (uint32_t)py * (uint32_t)pitch + (uint32_t)px * (uint32_t)bpp;
            p[0] = color & 0xFF;
            p[1] = (color >> 8) & 0xFF;
            p[2] = (color >> 16) & 0xFF;
            if (bpp == 4) p[3] = 0;
        }
    }
    // ── Border: bright purple, following the rounded profile ────
    uint32_t border = 0xB088FF;
    // Top + bottom straight edges (clipped to the corner profile)
    int insEdge = mouse_rr_inset(0, th, MOUSE_TILE_R);
    for (int x = insEdge; x < tw - insEdge; x++) {
        int pxt = gx + x, pxb = gx + x;
        int pyt = gy, pyb = gy + th - 1;
        if (pxt >= 0 && pxt < (int)vesa.width && pyt >= 0 && pyt < (int)vesa.height) {
            uint8_t* p = fb + (uint32_t)pyt * (uint32_t)pitch + (uint32_t)pxt * (uint32_t)bpp;
            p[0] = border & 0xFF; p[1] = (border >> 8) & 0xFF; p[2] = (border >> 16) & 0xFF;
            if (bpp == 4) p[3] = 0;
        }
        if (pxb >= 0 && pxb < (int)vesa.width && pyb >= 0 && pyb < (int)vesa.height) {
            uint8_t* p = fb + (uint32_t)pyb * (uint32_t)pitch + (uint32_t)pxb * (uint32_t)bpp;
            p[0] = border & 0xFF; p[1] = (border >> 8) & 0xFF; p[2] = (border >> 16) & 0xFF;
            if (bpp == 4) p[3] = 0;
        }
    }
    // Left + right straight edges (skipping corner rows)
    for (int y = 0; y < th; y++) {
        int pxl = gx, pxr = gx + tw - 1;
        int py = gy + y;
        if (mouse_rr_inset(y, th, MOUSE_TILE_R) > 0) continue;   // corner rows handled below
        if (pxl >= 0 && pxl < (int)vesa.width && py >= 0 && py < (int)vesa.height) {
            uint8_t* p = fb + (uint32_t)py * (uint32_t)pitch + (uint32_t)pxl * (uint32_t)bpp;
            p[0] = border & 0xFF; p[1] = (border >> 8) & 0xFF; p[2] = (border >> 16) & 0xFF;
            if (bpp == 4) p[3] = 0;
        }
        if (pxr >= 0 && pxr < (int)vesa.width && py >= 0 && py < (int)vesa.height) {
            uint8_t* p = fb + (uint32_t)py * (uint32_t)pitch + (uint32_t)pxr * (uint32_t)bpp;
            p[0] = border & 0xFF; p[1] = (border >> 8) & 0xFF; p[2] = (border >> 16) & 0xFF;
            if (bpp == 4) p[3] = 0;
        }
    }
    // Corner arcs (top and bottom, mirrored)
    for (int y = 0; y < MOUSE_TILE_R; y++) {
        int ins = mouse_rr_inset(y, th, MOUSE_TILE_R);
        int yyT = gy + y, yyB = gy + th - 1 - y;
        int xs[2] = { gx + ins, gx + tw - 1 - ins };
        int ys[2] = { yyT, yyB };
        for (int a = 0; a < 2; a++)
            for (int b2 = 0; b2 < 2; b2++) {
                int px = xs[a], py2 = ys[b2];
                if (px < 0 || px >= (int)vesa.width || py2 < 0 || py2 >= (int)vesa.height) continue;
                uint8_t* p = fb + (uint32_t)py2 * (uint32_t)pitch + (uint32_t)px * (uint32_t)bpp;
                p[0] = border & 0xFF; p[1] = (border >> 8) & 0xFF; p[2] = (border >> 16) & 0xFF;
                if (bpp == 4) p[3] = 0;
            }
    }

    // ── App label below the ghost tile ───────────────────────────
    if (g_ghost_label) {
        int lx = gx + 4;
        int ly = gy + th + 2;
        uint32_t fg = 0xB088FF;
        uint32_t bg = 0x00000000;  // transparent bg — just skip bg pixels

        for (int si = 0; g_ghost_label[si]; si++) {
            char c = g_ghost_label[si];
            unsigned int idx = (unsigned int)c;
            if (idx < 32 || idx > 127) idx = 32;
            idx -= 32;
            const uint8_t* glyph = font8x16[idx];
            for (int row = 0; row < 16; row++) {
                uint8_t bits = glyph[row];
                int py2 = ly + row;
                if (py2 < 0 || py2 >= (int)vesa.height) continue;
                for (int col = 0; col < 8; col++) {
                    if (!(bits & (0x80 >> col))) continue;  // transparent bg
                    int px2 = lx + col;
                    if (px2 < 0 || px2 >= (int)vesa.width) continue;
                    uint8_t* p = fb + (uint32_t)py2 * (uint32_t)pitch + (uint32_t)px2 * (uint32_t)bpp;
                    p[0] = fg & 0xFF;
                    p[1] = (fg >> 8) & 0xFF;
                    p[2] = (fg >> 16) & 0xFF;
                    if (bpp == 4) p[3] = 0;
                }
            }
            lx += 8;
        }
    }
}


// ─── Snap-to-grid preview (IRQ-time) ──────────────────────────────
// When dragging an icon (drag_active && g_drag_app_idx >= 0), draw
// a subtle ghost of the dragged icon at the grid slot under the
// cursor, so the user can see exactly where the icon will land.
// The tile is 30%% dimmer than the drag ghost and uses a cyan/teal
// border to visually distinguish "current position" (purple ghost)
// from "target slot" (cyan preview).
// `brightness` is a fixed-point factor (0-256) that dims all colors,
// used for the fade-out animation when drag ends.
static void mouse_draw_snap_preview(uint8_t* fb, int px, int py, int brightness) {
    if (!vesa.active || !fb) return;
    if (brightness <= 0) return;
    if (g_grid_cell_w <= 0 || g_grid_cell_h <= 0) return;

    int tile_w = 64, tile_h = 62;
    int bpp = (int)(vesa.bpp / 8);
    int pitch = (int)vesa.pitch;

    // ── Preview tile: dim gradient (70%% of ghost brightness) ────
    // Rounded corners (radius MOUSE_TILE_R) match the desktop tiles.
    for (int y = 0; y < tile_h; y++) {
        int py2 = py + y;
        if (py2 < 0 || py2 >= (int)vesa.height) continue;
        int t = y * 255 / tile_h;
        uint32_t tr = ((0x2E - (0x2E - 0x1E) * t / 255) * 70 / 100) * (uint32_t)brightness / 256;
        uint32_t tg = ((0x20 - (0x20 - 0x12) * t / 255) * 70 / 100) * (uint32_t)brightness / 256;
        uint32_t tb = ((0x50 - (0x50 - 0x40) * t / 255) * 70 / 100) * (uint32_t)brightness / 256;
        uint32_t color = (tr << 16) | (tg << 8) | tb;
        int ins = mouse_rr_inset(y, tile_h, MOUSE_TILE_R);
        for (int x = ins; x < tile_w - ins; x++) {
            int px2 = px + x;
            if (px2 < 0 || px2 >= (int)vesa.width) continue;
            uint8_t* p = fb + (uint32_t)py2 * (uint32_t)pitch + (uint32_t)px2 * (uint32_t)bpp;
            p[0] = color & 0xFF; p[1] = (color >> 8) & 0xFF; p[2] = (color >> 16) & 0xFF;
            if (bpp == 4) p[3] = 0;
        }
    }

    // ── Cyan border: thin glow (dimmed by brightness) ────────────
    uint32_t border_r = 0x44 * (uint32_t)brightness / 256;
    uint32_t border_g = 0xFF * (uint32_t)brightness / 256;
    uint32_t border_b = 0xCC * (uint32_t)brightness / 256;
    uint32_t border = (border_r << 16) | (border_g << 8) | border_b;
    // Top + bottom straight edges (clipped to the corner profile)
    int insEdge = mouse_rr_inset(0, tile_h, MOUSE_TILE_R);
    for (int x = insEdge; x < tile_w - insEdge; x++) {
        int pxt = px + x, pxb = px + x;
        int pyt = py, pyb = py + tile_h - 1;
        if (pxt >= 0 && pxt < (int)vesa.width && pyt >= 0 && pyt < (int)vesa.height) {
            uint8_t* p = fb + (uint32_t)pyt * (uint32_t)pitch + (uint32_t)pxt * (uint32_t)bpp;
            p[0] = border & 0xFF; p[1] = (border >> 8) & 0xFF; p[2] = (border >> 16) & 0xFF;
            if (bpp == 4) p[3] = 0;
        }
        if (pxb >= 0 && pxb < (int)vesa.width && pyb >= 0 && pyb < (int)vesa.height) {
            uint8_t* p = fb + (uint32_t)pyb * (uint32_t)pitch + (uint32_t)pxb * (uint32_t)bpp;
            p[0] = border & 0xFF; p[1] = (border >> 8) & 0xFF; p[2] = (border >> 16) & 0xFF;
            if (bpp == 4) p[3] = 0;
        }
    }
    // Left + right straight edges (skipping corner rows)
    for (int y = 0; y < tile_h; y++) {
        int pxl = px, pxr = px + tile_w - 1;
        int py2 = py + y;
        if (mouse_rr_inset(y, tile_h, MOUSE_TILE_R) > 0) continue;  // corner rows below
        if (pxl >= 0 && pxl < (int)vesa.width && py2 >= 0 && py2 < (int)vesa.height) {
            uint8_t* p = fb + (uint32_t)py2 * (uint32_t)pitch + (uint32_t)pxl * (uint32_t)bpp;
            p[0] = border & 0xFF; p[1] = (border >> 8) & 0xFF; p[2] = (border >> 16) & 0xFF;
            if (bpp == 4) p[3] = 0;
        }
        if (pxr >= 0 && pxr < (int)vesa.width && py2 >= 0 && py2 < (int)vesa.height) {
            uint8_t* p = fb + (uint32_t)py2 * (uint32_t)pitch + (uint32_t)pxr * (uint32_t)bpp;
            p[0] = border & 0xFF; p[1] = (border >> 8) & 0xFF; p[2] = (border >> 16) & 0xFF;
            if (bpp == 4) p[3] = 0;
        }
    }
    // Corner arcs (top and bottom, mirrored)
    for (int y = 0; y < MOUSE_TILE_R; y++) {
        int ins = mouse_rr_inset(y, tile_h, MOUSE_TILE_R);
        int yyT = py + y, yyB = py + tile_h - 1 - y;
        int xs[2] = { px + ins, px + tile_w - 1 - ins };
        int ys[2] = { yyT, yyB };
        for (int a = 0; a < 2; a++)
            for (int b2 = 0; b2 < 2; b2++) {
                int px2 = xs[a], py2 = ys[b2];
                if (px2 < 0 || px2 >= (int)vesa.width || py2 < 0 || py2 >= (int)vesa.height) continue;
                uint8_t* p = fb + (uint32_t)py2 * (uint32_t)pitch + (uint32_t)px2 * (uint32_t)bpp;
                p[0] = border & 0xFF; p[1] = (border >> 8) & 0xFF; p[2] = (border >> 16) & 0xFF;
                if (bpp == 4) p[3] = 0;
            }
    }

    // ── App label below the preview tile (dimmed by brightness) ──
    if (g_drag_app_name) {
        int lx = px + 4;
        int ly = py + tile_h + 2;
        uint32_t fg_r = 0x88 * (uint32_t)brightness / 256;
        uint32_t fg_g = 0xFF * (uint32_t)brightness / 256;
        uint32_t fg_b = 0xDD * (uint32_t)brightness / 256;
        uint32_t fg = (fg_r << 16) | (fg_g << 8) | fg_b;
        for (int si = 0; g_drag_app_name[si]; si++) {
            char c = g_drag_app_name[si];
            unsigned int idx = (unsigned int)c;
            if (idx < 32 || idx > 127) idx = 32;
            idx -= 32;
            const uint8_t* glyph = font8x16[idx];
            for (int row = 0; row < 16; row++) {
                uint8_t bits = glyph[row];
                int py2 = ly + row;
                if (py2 < 0 || py2 >= (int)vesa.height) continue;
                for (int col2 = 0; col2 < 8; col2++) {
                    if (!(bits & (0x80 >> col2))) continue;
                    int px2 = lx + col2;
                    if (px2 < 0 || px2 >= (int)vesa.width) continue;
                    uint8_t* p = fb + (uint32_t)py2 * (uint32_t)pitch + (uint32_t)px2 * (uint32_t)bpp;
                    p[0] = fg & 0xFF;
                    p[1] = (fg >> 8) & 0xFF;
                    p[2] = (fg >> 16) & 0xFF;
                    if (bpp == 4) p[3] = 0;
                }
            }
            lx += 8;
        }
    }
}

// ─── Dirty-cursor save/restore ────────────────────────────────────
// The IRQ handler updates mouse.px/py and draws the cursor to the
// visible framebuffer directly, so the cursor tracks at the PS/2
// sample rate (200 Hz) instead of waiting for the 50 Hz PIT heartbeat.
// Save/restore prevents cursor trails between full redraws.
// CURSOR_BG_* macros are defined in include/mouse.h (shared with
// desktop.c via the exported extern declarations).
static uint8_t cursor_save_buf[CURSOR_BG_H * CURSOR_BG_STRIDE];
int cursor_bg_x = -1;
int cursor_bg_y = -1;
int cursor_bg_valid = 0;
// Present-generation guard for the cursor background (see mouse.h):
// bumped by finish_frame() after vesa_present() wipes the cursor, so
// cursor_restore_bg() won't paint stale pre-present pixels back on a
// fresh frame (the "stale cursor copy" glitch).
int cursor_present_gen = 0;
static int cursor_save_gen = -1;
// NOTE: Previously there was a volatile __cursor_bg_dirty flag set by
// desktop_draw()/redraw_content() while writing to vesa.draw.  The IRQ
// handler used it to skip cursor_save_bg during back-buffer writes.
// Now the IRQ reads cursor background from vesa.fb (never touched by
// desktop_draw), so the flag is removed.  If a future change makes the
// IRQ read from vesa.draw again, re-add the flag and guard here.

void cursor_save_bg(int mx, int my, uint8_t* fb) {
    int bpp = (int)(vesa.bpp / 8);
    int pitch = (int)vesa.pitch;
    int x = mx, y = my;
    int cw = CURSOR_BG_W, ch = CURSOR_BG_H;
    if (x < 0) { cw += x; x = 0; }
    if (y < 0) { ch += y; y = 0; }
    if (x + cw > (int)vesa.width)  cw = (int)vesa.width - x;
    if (y + ch > (int)vesa.height) ch = (int)vesa.height - y;
    if (cw <= 0 || ch <= 0) { cursor_bg_valid = 0; return; }
    for (int row = 0; row < ch; row++) {
        const uint8_t* src = fb + (uint32_t)(y + row) * (uint32_t)pitch + (uint32_t)x * (uint32_t)bpp;
        uint8_t* dst = cursor_save_buf + (uint32_t)row * CURSOR_BG_STRIDE;
        for (int col = 0; col < cw; col++) {
            dst[col*4 + 0] = src[col*bpp + 0];
            dst[col*4 + 1] = src[col*bpp + 1];
            dst[col*4 + 2] = src[col*bpp + 2];
            dst[col*4 + 3] = 0;
        }
    }
    cursor_bg_x = x;
    cursor_bg_y = y;
    cursor_bg_valid = 1;
    cursor_save_gen = cursor_present_gen;
}

void cursor_restore_bg(uint8_t* fb) {
    if (!cursor_bg_valid) return;
    if (cursor_save_gen != cursor_present_gen) {
        // A present wiped the cursor since this background was saved —
        // restoring it would paint stale pre-present pixels.  Forget it.
        cursor_bg_valid = 0;
        return;
    }
    int bpp = (int)(vesa.bpp / 8);
    int pitch = (int)vesa.pitch;
    int x = cursor_bg_x, y = cursor_bg_y;
    int cw = CURSOR_BG_W, ch = CURSOR_BG_H;
    if (x < 0) { cw += x; x = 0; }
    if (y < 0) { ch += y; y = 0; }
    if (x + cw > (int)vesa.width)  cw = (int)vesa.width - x;
    if (y + ch > (int)vesa.height) ch = (int)vesa.height - y;
    if (cw <= 0 || ch <= 0) return;
    for (int row = 0; row < ch; row++) {
        const uint8_t* src = cursor_save_buf + (uint32_t)row * CURSOR_BG_STRIDE;
        uint8_t* dst = fb + (uint32_t)(y + row) * (uint32_t)pitch + (uint32_t)x * (uint32_t)bpp;
        for (int col = 0; col < cw; col++) {
            dst[col*bpp + 0] = src[col*4 + 0];
            dst[col*bpp + 1] = src[col*4 + 1];
            dst[col*bpp + 2] = src[col*4 + 2];
        }
    }
}

// ─── Overlay plane (offscreen save for vesa_present compositing) ──
// After the IRQ handler draws drag overlays (cell, preview, ghost) to
// vesa.fb via save/restore, it snapshots the overlay pixel rectangles
// into these buffers.  vesa_present() then restores them onto vesa.fb
// after the back-buffer copy, eliminating the race where redraw_content()
// erases the overlays.  The snapshots capture the overlay pixels at
// their current position; when composited back they include the desktop
// background that was saved before the overlay was drawn (save_bg), so
// the overlay correctly blends with the freshly-drawn desktop.
//
// Each overlay type has its own buffer sized for the maximum rectangle.
// A descriptor (x,y,w,h) tracks the actual screen position; zero w/h
// means the overlay is inactive (not composited this frame).
// Buffer dimension #defines live in include/vesa.h (shared with
// vesa_present in vesa.c for compositing stride correctness).
static uint32_t ovly_cell_buf[OVLY_CELL_W * OVLY_CELL_H];
static uint32_t ovly_prev_buf[OVLY_PREV_W * OVLY_PREV_H];
static uint32_t ovly_prev_bg_buf[OVLY_PREV_W * OVLY_PREV_H];
static uint32_t ovly_ghost_buf[OVLY_GHOST_W * OVLY_GHOST_H];

// Overlay descriptors — set by IRQ handler after drawing, read by
// vesa_present().  w/h of 0 means the overlay is not active.
static int ovly_cell_x, ovly_cell_y, ovly_cell_w, ovly_cell_h;
static int ovly_prev_x, ovly_prev_y, ovly_prev_w, ovly_prev_h;
static int ovly_ghost_x, ovly_ghost_y, ovly_ghost_w, ovly_ghost_h;

// Snapshot a pixel rectangle from src_fb into the overlay buffer.
// The buffer is 32-bit BGRA (native framebuffer format).
static void ovly_snap(uint32_t* buf, int buf_stride_words,
                       uint8_t* src_fb, int sx, int sy, int w, int h) {
    if (w <= 0 || h <= 0) return;
    int pitch = (int)vesa.pitch;
    int bpp = (int)(vesa.bpp / 8);
    // Clip to framebuffer
    if (sx < 0) { w += sx; sx = 0; }
    if (sy < 0) { h += sy; sy = 0; }
    if (sx + w > (int)vesa.width)  w = (int)vesa.width - sx;
    if (sy + h > (int)vesa.height) h = (int)vesa.height - sy;
    if (w <= 0 || h <= 0) return;
    for (int row = 0; row < h; row++) {
        const uint8_t* src = src_fb + (uint32_t)(sy + row) * (uint32_t)pitch + (uint32_t)sx * (uint32_t)bpp;
        for (int col = 0; col < w && col < buf_stride_words; col++) {
            uint32_t b = src[col * (uint32_t)bpp + 0];
            uint32_t g = src[col * (uint32_t)bpp + 1];
            uint32_t r = src[col * (uint32_t)bpp + 2];
            buf[(uint32_t)row * (uint32_t)buf_stride_words + (uint32_t)col] = (r << 16) | (g << 8) | b;
        }
    }
}

// ─── Overlay plane externs (used by vesa_present in vesa.c) ───────
// These are declared in include/vesa.h and used by vesa_present() to
// composite the overlays onto vesa.fb after the back-buffer copy.
int __ovly_cell_x, __ovly_cell_y, __ovly_cell_w = 0, __ovly_cell_h = 0;
uint32_t* __ovly_cell_buf = ovly_cell_buf;
int __ovly_prev_x, __ovly_prev_y, __ovly_prev_w = 0, __ovly_prev_h = 0;
int __ovly_prev_fade = 0;  // >0 = fading out, decremented each vesa_present()
uint32_t* __ovly_prev_buf = ovly_prev_buf;
uint32_t* __ovly_prev_bg_buf = ovly_prev_bg_buf;
int __ovly_ghost_x, __ovly_ghost_y, __ovly_ghost_w = 0, __ovly_ghost_h = 0;
uint32_t* __ovly_ghost_buf = ovly_ghost_buf;

// Compute the flying-ghost position for the current mag_fraction
// (quad ease-out over the 0-255 sub-tick clock).  Shared by the
// heartbeat renderer and the IRQ branch so both draw the ghost at
// bit-identical positions — any drift would visibly snap the tile
// between the two renderers.
static void mag_ghost_pos(int* gx, int* gy) {
    int t = mag_fraction;
    if (t > MAG_FRACTION_MAX) t = MAG_FRACTION_MAX;
    int eased = 256 - ((256 - t) * (256 - t) / 256);
    if (eased > 256) eased = 256;
    *gx = mag_sx + (mag_ex - mag_sx) * eased / 256;
    *gy = mag_sy + (mag_ey - mag_sy) * eased / 256;
}

// ─── Flying-ghost renderer (single-writer, post-present) ──────────
// Draws the slide-in animation ghost at the current mag_fraction
// interpolation.  The desktop heartbeat calls this AFTER
// desktop_draw() has presented a clean frame (finish_frame), so the
// ghost is drawn directly on top of vesa.fb — exactly like the
// cursor — and the next tick's present() wipes it, keeping at most
// one ghost on screen at all times.
//
// This is the ONLY renderer for the flying ghost.  Previously the
// PS/2 IRQ also drew it at 200 Hz; the two writers shared the same
// dirty-rectangle buffers and overlay snapshot, so the heartbeat's
// un-restored draws and the IRQ's snapshots baked each other's stale
// ghost pixels into the frame — the smeared trail behind the icon as
// it slid into its slot.  The IRQ now only advances the sub-tick
// clock (see the draw chain below); the heartbeat renders.
void mouse_flying_ghost_render(void) {
    // Skip while a NEW drag is in progress: the IRQ drag branch owns
    // the drag ghost during a drag; the animation still commits on
    // schedule via mag_commit_pending.
    if (!mag_active || mag_src_app < 0 || mouse.drag_active) return;
    int gx, gy;
    mag_ghost_pos(&gx, &gy);

    // The caller's present() already cleared the previous tick's
    // ghost (fresh desktop), so there is nothing to restore — just
    // invalidate the stale save state and capture the clean
    // background at the new position before drawing.
    ghost_invalidate_bg();
    ghost_save_bg(gx - 32, gy - 31, vesa.fb);

    // Reuse the drag-ghost drawing path (reads g_ghost_app_idx,
    // g_ghost_label, ghost_filter_px/py — all three overridden here).
    int saved_idx   = g_ghost_app_idx;
    const char* saved_label = g_ghost_label;
    int saved_fx    = g_ghost_filter_px;
    int saved_fy    = g_ghost_filter_py;
    g_ghost_app_idx  = mag_src_app;
    g_ghost_label    = app_names[mag_src_app];
    g_ghost_filter_px = gx;
    g_ghost_filter_py = gy;
    mouse_draw_ghost_on(vesa.fb);
    g_ghost_app_idx   = saved_idx;
    g_ghost_label     = saved_label;
    g_ghost_filter_px = saved_fx;
    g_ghost_filter_py = saved_fy;
}

// ─── 16×16 arrow cursor sprite ────────────────────────────────────
// 0 = transparent (skip), 1 = white fill, 2 = black outline
// Hotspot at (0,0) = top-left corner of the arrow head.
// Pointing left-up, classic OS cursor shape.
static const uint8_t cursor_data[256] = {
    2,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    2,1,2,0,0,0,0,0,0,0,0,0,0,0,0,0,
    2,1,1,2,0,0,0,0,0,0,0,0,0,0,0,0,
    2,1,1,1,2,0,0,0,0,0,0,0,0,0,0,0,
    2,1,1,1,1,2,0,0,0,0,0,0,0,0,0,0,
    2,1,1,1,1,1,2,0,0,0,0,0,0,0,0,0,
    2,1,1,1,1,1,1,2,0,0,0,0,0,0,0,0,
    2,1,1,1,1,1,1,1,2,0,0,0,0,0,0,0,
    2,1,1,1,1,1,1,1,1,2,0,0,0,0,0,0,
    2,1,1,1,1,1,2,0,0,0,0,0,0,0,0,0,
    2,1,1,1,1,2,0,0,0,0,0,0,0,0,0,0,
    2,1,1,1,1,1,2,0,0,0,0,0,0,0,0,0,
    2,1,1,2,2,1,1,2,0,0,0,0,0,0,0,0,
    2,1,2,0,0,2,1,1,2,0,0,0,0,0,0,0,
    2,2,0,0,0,0,2,1,1,2,0,0,0,0,0,0,
    0,0,0,0,0,0,0,2,1,1,2,0,0,0,0,0,
};

// Cursor dimensions live in include/mouse.h so desktop.c shares them.
// (mouse.c #includes mouse.h already.)

// ─── Sensitivity ──────────────────────────────────────────────────
// MOUSE_DIVISOR controls the high-speed scaling factor (3 / DIVISOR).
//   DIV 3 = 100% (1:1, no reduction)
//   DIV 4 = 75%
//   DIV 5 = 60%  ← default
//   DIV 6 = 50%
// MOUSE_PRECISION_MAX sets the 1:1 dead-zone boundary: deltas with
// absolute value ≤ this number pass through unscaled for fine control.
// Default 1 means only |dx|=1 gets raw 1:1; |dx|=2 hits the curve.
#define MOUSE_DIVISOR       5
#define MOUSE_ROUND         (MOUSE_DIVISOR / 2)
#define MOUSE_PRECISION_MAX 1

// ─── PS/2 controller helpers ──────────────────────────────────────

static void mouse_wait_write(void) {
    for (int i = 0; i < 100000; i++) {
        if (!(inb(0x64) & 2)) return;
    }
}

static void mouse_wait_read(void) {
    for (int i = 0; i < 100000; i++) {
        if (inb(0x64) & 1) return;
    }
}

// Write a command to the mouse (through the auxiliary port).
static void mouse_write_cmd(uint8_t cmd) {
    mouse_wait_write();
    outb(0x64, 0xD4);   // tell controller: next byte is for aux device
    mouse_wait_write();
    outb(0x60, cmd);
}

// ─── IRQ12 handler ────────────────────────────────────────────────

// ─── PS/2 packet resync ───────────────────────────────────────
// Tracks the PIT tick of the last byte received. If `packet_phase`
// is non-zero (mid-packet) and more than ~50ms have elapsed since
// the last byte, the phase machine has desynced (e.g. a byte was
// dropped or an extra byte was sent). Resetting to phase 0 treats
// the current byte as a fresh flags byte, recovering synchronisation.
static uint32_t last_byte_tick = 0;

static void mouse_callback(registers_t regs) {
    (void)regs;
    // Raw packet deltas (screen-space).  These MUST be static: a PS/2
    // packet arrives as one IRQ PER BYTE, so case 1 (X) and case 2 (Y)
    // run in different interrupt invocations — a local would be freshly
    // zeroed by the time case 2 feeds the game (deltas silently lost,
    // buttons still worked because packet_flags is a file-scope static).
    static int dX = 0, dY = 0;
    uint8_t data = inb(0x60);

    // Any mouse activity wakes the screensaver (like a real OS);
    // the keyboard path already resets idle on every scancode.
    screensaver_reset_idle();

    // If we're mid-packet and too much time has passed, reset
    // the state machine — the byte stream has likely desynced.
    extern volatile uint32_t tick;
    if (packet_phase != 0 && (tick - last_byte_tick) > 3) {
        packet_phase = 0;
    }
    last_byte_tick = tick;

    switch (packet_phase) {
        case 0:
            // Flags byte — bit 0 = left, bit 1 = right, bit 2 = middle
            // bit 4 = X sign, bit 5 = Y sign, bit 6/7 = overflow
            packet_flags = data;
            dX = 0; dY = 0;   // fresh packet: clear carried raw deltas
            packet_phase = 1;
            break;

        case 1:
            // X delta (signed 8-bit, combined with sign bit from flags)
            {
                int dx = data;  // unsigned byte first (0-255)
                if (packet_flags & 0x40) dx = 0;    // X overflow (data invalid)
                else if (packet_flags & 0x10) dx -= 256; // 9-bit two's complement sign extension

                // X polarity: raw PS/2 deltas match screen space
                // (positive dx = finger right = cursor right). No
                // negation needed — only Y is inverted on this hardware.

                // Keep the raw packet delta — game mode wants it
                // unaccelerated (DOOM applies its own sensitivity).
                dX = dx;

        // Acceleration curve: 1:1 for precise small moves (|dx|≤1),
        // 60% rounding-to-nearest for fast swipes (|dx|>1).
        if (dx >= -MOUSE_PRECISION_MAX && dx <= MOUSE_PRECISION_MAX)
            mouse.px += dx;
        else
            mouse.px += (dx * 3 + (dx > 0 ? MOUSE_ROUND : -MOUSE_ROUND)) / MOUSE_DIVISOR;

                // Clamp to screen bounds
                if (mouse.px < 0) mouse.px = 0;
                if (vesa.active) {
                    if (mouse.px >= (int)vesa.width)
                        mouse.px = (int)vesa.width - 1;
                } else {
                    // VGA text mode: snap to 8px character-cell boundary
                    mouse.px = (mouse.px / 8) * 8;
                    if (mouse.px >= 80 * 8) mouse.px = 80 * 8 - 8;
                }
            }
            packet_phase = 2;
            break;

        case 2:
            // Y delta — this touchpad sends Y deltas inverted relative
            // to screen space (positive dy = finger up). Negate so
            // that positive dy = finger down = cursor down.
            {
                int dy = data;  // unsigned byte first (0-255)
                if (packet_flags & 0x80) dy = 0;    // Y overflow (data invalid)
                else if (packet_flags & 0x20) dy -= 256; // 9-bit two's complement sign extension

                // Y polarity: negate so positive dy = finger down
                dy = -dy;

                // Raw (already screen-space) packet delta for the game.
                dY = dy;

        // Acceleration curve: 1:1 for small precise moves (±1),
        // 60% rounding-to-nearest for fast swipes.
        if (dy >= -MOUSE_PRECISION_MAX && dy <= MOUSE_PRECISION_MAX)
            mouse.py += dy;
        else
            mouse.py += (dy * 3 + (dy > 0 ? MOUSE_ROUND : -MOUSE_ROUND)) / MOUSE_DIVISOR;

                // Clamp
                if (mouse.py < 0) mouse.py = 0;
                if (vesa.active) {
                    if (mouse.py >= (int)vesa.height)
                        mouse.py = (int)vesa.height - 1;
                } else {
                    // VGA text mode: snap to 16px character-cell boundary
                    mouse.py = (mouse.py / 16) * 16;
                    if (mouse.py >= 25 * 16) mouse.py = 25 * 16 - 16;
                }
            }

            // Update button states (bit 0 = left, bit 1 = right, bit 2 = middle)
            int new_left   = (packet_flags & 1) ? 1 : 0;
            int new_right  = (packet_flags & 2) ? 1 : 0;
            int new_middle = (packet_flags & 4) ? 1 : 0;

            // Capture press/release edges ONCE against the previous
            // level, then refresh the level.  Both the game path and
            // the desktop path below see the same edges — the game
            // feed's early return used to skip the desktop section
            // where the level was updated, leaving mouse.left stale
            // (so edge detectors fired on every packet while held).
            int press_left   = new_left  && !mouse.left;
            int press_right  = new_right && !mouse.right;
            int release_left = !new_left && mouse.left;
            mouse.left   = new_left;
            mouse.right  = new_right;
            mouse.middle = new_middle;

            // ── Game-mode [X] close box ─────────────────────────
            // Clicking the red X on the game title bar kills the
            // game task and restores the previous mode.  A press that
            // HITS the box is consumed here; a press anywhere else
            // must fall through to the feed below so the game (DOOM
            // fires on left-press) sees its buttons.
            if (press_left && game_active() &&
                game_hit_close(mouse.px, mouse.py)) {
                int gp = game_pid();
                if (gp > 0) task_kill(gp);   // PCB freed → task dies
                desktop_exit_game();
                // Advance the packet phase before leaving — case 2's
                // normal tail is skipped by this early return.
                packet_phase = mouse_wheel_mode ? 3 : 0;
                return;
            }

            // ── Game-mode input feed ────────────────────────────
            // While a game owns the screen, hand the raw deltas and
            // button level to the game module (SYS_GETMOUSE drains
            // them).  Raw, unaccelerated deltas — DOOM applies its
            // own sensitivity.  The on-screen position keeps clamping
            // (the [X] hit-test needs sane coordinates), but the game
            // never sees clamped values.  After this, the desktop
            // drag/click state machines below stay idle: game mode
            // returns before the click/drop bookkeeping runs.
            if (game_active()) {
                // Y is negated to match DOOM's ev_mouse convention
                // (data3 > 0 = mouse pushed forward/up).  The X axis
                // is already in DOOM's orientation.
                game_mouse_feed(dX, -dY, new_left | (new_right << 1) | (new_middle << 2));

                extern volatile uint32_t tick;
                last_activity_tick = tick;   // keep grab indicator honest
                // Advance the packet phase before leaving — this early
                // return skips case 2's normal tail where this also
                // happens.  Forgetting it wedges the state machine at
                // phase 2: every later flags/X byte decodes as Y and
                // the game sees zero X motion forever.
                packet_phase = mouse_wheel_mode ? 3 : 0;
                return;
            }

            // ── Drag-and-drop tracking ────────────────────────────
            // If left button went from released → pressed, record
            // the position for drag threshold checking.
            if (press_left) {
                mouse.drag_down_x = mouse.px;
                mouse.drag_down_y = mouse.py;
            }

            // If left button is held and mouse has moved past the
            // 4-pixel threshold, activate drag mode and identify the
            // source icon at the press-down position for the ghost.
            if (new_left) {
                int dx = mouse.px - mouse.drag_down_x;
                int dy = mouse.py - mouse.drag_down_y;
                if (dx < 0) dx = -dx;
                if (dy < 0) dy = -dy;
                if (dx > 4 || dy > 4) {
                    if (!mouse.drag_active) {
                        // First time crossing threshold — identify
                        // which grid slot was pressed so the ghost
                        // knows which app tile + label to draw.
                        // Reset ghost state FIRST: if the pressed slot
                        // turns out to be empty, the ghost must stay
                        // disabled (a stale value from a previous drag
                        // would otherwise draw a leftover ghost tile).
                        g_ghost_app_idx = -1;
                        g_ghost_label = 0;
                        // Reset rubber-band selection state: it only
                        // becomes active if the press-down lands on an
                        // EMPTY grid slot on the desktop.  Erase any box
                        // left on screen from a previous drag (rapid
                        // re-drag) so the new one starts from a clean
                        // slate.
                        g_sel_drag_active = 0;
                        mouse_sel_box_erase_now();
                        if (g_grid_cell_w > 0 && g_grid_cell_h > 0) {
                            int col = (mouse.drag_down_x - g_grid_start_x) / g_grid_cell_w;
                            int row = (mouse.drag_down_y - g_grid_start_y) / g_grid_cell_h;
                            if (col >= 0 && col < g_grid_cols &&
                                row >= 0 && row < g_grid_rows) {
                                int grid = col + row * g_grid_cols;
                                // Only OCCUPIED slots can start a drag.
                                // The full-screen grid has many empty
                                // slots (icon_order == -1); dragging one
                                // would make desktop.c index app_names[-1]
                                // (OOB read → crash) and set
                                // mag_src_app = -1 (stalling the slide-in).
                                if (grid >= 0 && grid < g_grid_slots &&
                                    icon_order[grid] >= 0) {
                                    // Group drag? If the pressed icon is
                                    // one of several box-selected apps,
                                    // dragging moves the whole group.
                                    // Snapshot the selected slots so the
                                    // drop dispatch can translate them all
                                    // by the same delta as the anchor.
                                    g_group_drag_active = 0;
                                    g_group_count = 0;
                                    // Only start a group drag when the
                                    // whole selection fits in the slot
                                    // snapshot (GROUP_MAX_SLOTS).  If
                                    // the box selected more icons than
                                    // that, fall back to a single-icon
                                    // drag — moving only the first 40
                                    // would silently drop apps.
                                    int gsel_n = desktop_sel_count();
                                    if (current_mode == MODE_DESKTOP &&
                                        desktop_sel_has(grid) &&
                                        gsel_n > 1 &&
                                        gsel_n <= GROUP_MAX_SLOTS) {
                                        g_group_drag_active = 1;
                                        g_group_anchor = grid;
                                        for (int s2 = 0; s2 < g_grid_slots &&
                                                         g_group_count < GROUP_MAX_SLOTS; s2++) {
                                            if (desktop_sel_has(s2))
                                                g_group_slots[g_group_count++] = s2;
                                        }
                                        // No single ghost — the cluster
                                        // ghost takes over.
                                        g_ghost_app_idx = -1;
                                        g_ghost_label = 0;
                                        ghost_filter_px = mouse.px;
                                        ghost_filter_py = mouse.py;
                                    } else {
                                        // Store grid position; desktop.c's
                                        // drag source detection will refine
                                        // to the actual app index when
                                        // desktop_draw() runs.
                                        g_ghost_app_idx = grid;
                                        g_ghost_label = 0;  // no label until desktop.c refines it
                                        // Initialize the ghost position to the
                                        // current cursor position.
                                        ghost_filter_px = mouse.px;
                                        ghost_filter_py = mouse.py;
                                    }
                                } else if (current_mode == MODE_DESKTOP) {
                                    // Pressed an EMPTY grid slot on the
                                    // desktop → start a rubber-band
                                    // selection box anchored at the
                                    // press-down position.  The IRQ draws
                                    // it at 200 Hz (see the draw chain);
                                    // desktop.c finalizes the selection
                                    // on drop.
                                    g_sel_drag_active = 1;
                                    g_sel_x0 = mouse.drag_down_x;
                                    g_sel_y0 = mouse.drag_down_y;
                                }
                            }
                        }
                    mouse.drag_active = 1;
                }
            }
        }

            // If left button went from pressed → released and we
            // were dragging, record a drop event.
            if (release_left && mouse.drag_active) {
                mouse.drop_pending = 1;
                mouse.drop_x = mouse.px;
                mouse.drop_y = mouse.py;
                mouse.drag_active = 0;
                // Stop drawing the selection box.  Erase it immediately
                // so a rapid re-drag can't draw a new box on top of the
                // stale one (the drop dispatch redraws the scene anyway).
                g_sel_drag_active = 0;
                mouse_sel_box_erase_now();
                // Start 2-frame fade-out for snap preview; clear
                // cell and ghost immediately (they cut out faster).
                __ovly_cell_w = 0; __ovly_ghost_w = 0;
                if (__ovly_prev_w > 0) __ovly_prev_fade = 2;
                else __ovly_prev_w = 0;
            }

            // Detect left-click transition
            if (press_left) {
                mouse.click_count++;       // track rapid clicks for double-click detection
                mouse.click_pending = 1;
                mouse.click_x = mouse.px;
                mouse.click_y = mouse.py;
            }

            // Detect right-click transition (press → 1, release → 0)
            if (press_right) {
                mouse.rclick_pending = 1;
                mouse.rclick_x = mouse.px;
                mouse.rclick_y = mouse.py;
            }

            // Record activity time (used by grab indicator)
            // We check tick ± px/py changes to avoid false positives
            // from the initial centre position on boot.
            extern volatile uint32_t tick;
            last_activity_tick = tick;

            // Also clear drag on release even if not dragging
            if (release_left) {
                mouse.drag_active = 0;
                g_sel_drag_active = 0;
                mouse_sel_box_erase_now();
                // Group drag ends on release — the cluster ghost must
                // stop drawing; desktop.c's drop dispatch (DRAG_GROUP)
                // reads the slot snapshot before clearing it.
                group_ghost_invalidate();
                __ovly_cell_w = 0; __ovly_ghost_w = 0;
                if (__ovly_prev_w > 0) __ovly_prev_fade = 2;
                else __ovly_prev_w = 0;
            }

            // ── Draw drag ghost + cursor to vesa.fb at 200 Hz ───
            // The ghost (if dragging) is drawn FIRST so the cursor
            // appears on top.  Both use dirty-rectangle save/restore
            // to prevent trails between full redraws.
            // 
            // Skipped entirely while vesa_present() is mid-copy: any
            // overlay drawn then would be torn by the overwrite, and
            // the generation counters already make stale pre-present
            // state a no-op on the next packet.  finish_frame()
            // redraws the box + cursor right after the present.
            if (vesa.active && !__present_in_progress) {
                // ── Drag overlays (cell → preview → ghost) ─────────
                // Three layers stack: cell highlight (bottom), snap
                // preview (middle), drag ghost (top). The cursor is
                // drawn last (on top of everything).
                if (mouse.drag_active && g_ghost_app_idx >= 0) {
                    int pc = -1, pr = -1;
                    if (g_grid_cell_w > 0 && g_grid_cell_h > 0) {
                        pc = (mouse.px - g_grid_start_x) / g_grid_cell_w;
                        pr = (mouse.py - g_grid_start_y) / g_grid_cell_h;
                    }
                    int cell_valid = (pc >= 0 && pc < g_grid_cols && pr >= 0 && pr < g_grid_rows);

                    // 0. Grid-cell highlight: restore, save, draw border
                    cell_restore_bg(vesa.fb);
                    ovly_cell_w = 0;  // mark inactive unless drawn below
                    if (cell_valid) {
                        int cell_x = g_grid_start_x + pc * g_grid_cell_w;
                        int cell_y = g_grid_start_y + pr * g_grid_cell_h;
                        cell_save_bg(cell_x, cell_y, g_grid_cell_w, g_grid_cell_h, vesa.fb);
                        mouse_draw_cell_highlight(vesa.fb, cell_x, cell_y,
                                                  g_grid_cell_w, g_grid_cell_h);
                        // Snapshot to overlay plane for vesa_present compositing
                        ovly_cell_x = cell_x; ovly_cell_y = cell_y;
                        ovly_cell_w = g_grid_cell_w; ovly_cell_h = g_grid_cell_h;
                        if (ovly_cell_w > OVLY_CELL_W) ovly_cell_w = OVLY_CELL_W;
                        if (ovly_cell_h > OVLY_CELL_H) ovly_cell_h = OVLY_CELL_H;
                        ovly_snap(ovly_cell_buf, OVLY_CELL_W, vesa.fb,
                                  ovly_cell_x, ovly_cell_y, ovly_cell_w, ovly_cell_h);
                        // Sync externs for vesa.c
                        __ovly_cell_x = ovly_cell_x;
                        __ovly_cell_y = ovly_cell_y;
                        __ovly_cell_w = ovly_cell_w;
                        __ovly_cell_h = ovly_cell_h;
                    }

                    // 1. Snap preview: restore, save, draw
                    preview_restore_bg(vesa.fb);
                    ovly_prev_w = 0;
                    if (cell_valid) {
                        int pslot = pc + pr * g_grid_cols;
                        if (pslot >= 0 && pslot < g_grid_slots &&
                            !(pslot == g_ghost_app_idx && g_ghost_app_idx >= 0)) {
                            int pp_x = g_grid_start_x + pc * g_grid_cell_w +
                                       (g_grid_cell_w - 64) / 2;
                            int pp_y = g_grid_start_y + pr * g_grid_cell_h;
                            preview_save_bg(pp_x, pp_y, vesa.fb);
                            // Set descriptor first so position is available for bg snap
                            ovly_prev_x = pp_x; ovly_prev_y = pp_y;
                            int pp_w = 64, pp_h = 62 + 2 + 16;  // tile + margin + font
                            ovly_prev_w = pp_w; ovly_prev_h = pp_h;
                            if (ovly_prev_w > OVLY_PREV_W) ovly_prev_w = OVLY_PREV_W;
                            if (ovly_prev_h > OVLY_PREV_H) ovly_prev_h = OVLY_PREV_H;
                            // Snapshot CLEAN BACKGROUND before drawing overlay
                            // (stored separately so fade can compare against pristine
                            //  desktop regardless of content changes between IRQ and present)
                            ovly_snap(ovly_prev_bg_buf, OVLY_PREV_W, vesa.fb,
                                      ovly_prev_x, ovly_prev_y, ovly_prev_w, ovly_prev_h);
                            mouse_draw_snap_preview(vesa.fb, pp_x, pp_y, 256);
                            // Snapshot composite (bg + overlay) for normal compositing
                            ovly_snap(ovly_prev_buf, OVLY_PREV_W, vesa.fb,
                                      ovly_prev_x, ovly_prev_y, ovly_prev_w, ovly_prev_h);
                            __ovly_prev_x = ovly_prev_x;
                            __ovly_prev_y = ovly_prev_y;
                            __ovly_prev_w = ovly_prev_w;
                            __ovly_prev_h = ovly_prev_h;
                        }
                    }

                    // 2. Ghost: restore old bg (clears the old ghost tile)
                    ghost_restore_bg(vesa.fb);

                    // ── Ghost position: instant cursor tracking ──────
                    // Round the raw cursor position into the ghost each
                    // frame (integer rounding, zero lag — no low-pass
                    // filtering).  Runs BEFORE the cursor bg save so the
                    // save is taken at the same position the ghost + cursor
                    // are about to be drawn — the same alignment the
                    // non-drag path has.
                    ghost_round_to_cursor();

                    // ── Cursor: restore + save BEFORE ghost draw ─────────
                    // ghost_restore just cleared the old ghost position with
                    // ghost_background (desktop+cell+preview, no ghost tile).
                    // Save the cursor background at the ghost/cursor
                    // position (ghost_filter == mouse.px/py, 1:1 tracking).
                    // Since the ghost tile is drawn at that position and the
                    // cursor sits on top, the saved area has ghost_background
                    // (no ghost tile).  Next frame's cursor_restore_bg will
                    // put back ghost_background at the old ghost position —
                    // exactly what ghost_restore_bg already restored.  No fringe.
                    cursor_restore_bg(vesa.fb);
                    cursor_save_bg(ghost_filter_px, ghost_filter_py, vesa.fb);

                    // ── Ghost draw + snap ────────────────────────────────
                    ovly_ghost_w = 0;
                    int gx = ghost_filter_px, gy = ghost_filter_py;
                    ghost_save_bg(gx - 32, gy - 31, vesa.fb);
                    mouse_draw_ghost_on(vesa.fb);
                    // Snapshot to overlay plane
                    ovly_ghost_x = gx - 32; ovly_ghost_y = gy - 31;
                    ovly_ghost_w = GHOST_W; ovly_ghost_h = GHOST_H;
                    if (ovly_ghost_w > OVLY_GHOST_W) ovly_ghost_w = OVLY_GHOST_W;
                    if (ovly_ghost_h > OVLY_GHOST_H) ovly_ghost_h = OVLY_GHOST_H;
                    ovly_snap(ovly_ghost_buf, OVLY_GHOST_W, vesa.fb,
                              ovly_ghost_x, ovly_ghost_y, ovly_ghost_w, ovly_ghost_h);
                    __ovly_ghost_x = ovly_ghost_x;
                    __ovly_ghost_y = ovly_ghost_y;
                    __ovly_ghost_w = ovly_ghost_w;
                    __ovly_ghost_h = ovly_ghost_h;
                } else if (mouse.drag_active && g_group_drag_active) {
                    // ── Group drag cluster ghost ───────────────────────
                    // One of several box-selected icons is being dragged:
                    // draw a mini-tile cluster (the whole selection) at
                    // the cursor so the user sees the group move.  Same
                    // dirty-rectangle pattern as the single ghost.
                    cursor_restore_bg(vesa.fb);
                    group_ghost_restore_bg(vesa.fb);
                    group_ghost_save_bg(ghost_filter_px - GROUP_GHOST_W/2,
                                        ghost_filter_py - GROUP_GHOST_H/2, vesa.fb);
                    mouse_draw_group_ghost_on(vesa.fb);
                    cursor_save_bg(mouse.px, mouse.py, vesa.fb);
                } else if (mouse.drag_active && g_sel_drag_active) {
                    // ── Rubber-band selection box ──────────────────────
                    // Empty-desktop drag: XOR a translucent window from
                    // the press-down corner to the cursor.  XOR-drawing
                    // the same rect twice restores the desktop, so no
                    // save buffer is needed at any size.  The cursor is
                    // restored/saved around it so it stays on top.
                    cursor_restore_bg(vesa.fb);
                    sel_box_update(vesa.fb);
                    cursor_save_bg(mouse.px, mouse.py, vesa.fb);
                } else {
                    // ── Flying-ghost clock advance (200 Hz) ──────────
                    // Only the sub-tick clock runs here; the ghost
                    // tile itself is rendered EXCLUSIVELY by the
                    // desktop heartbeat (mouse_flying_ghost_render,
                    // post-present, single writer).  Drawing it here
                    // too created a second writer whose un-restored
                    // draws and overlay snapshots baked stale ghost
                    // pixels into the frame — the trail behind the
                    // icon as it slid into its slot.  The heartbeat
                    // advances the clock only when the mouse is idle
                    // (packet-count stall detection), so the two never
                    // double-advance and the slide always takes
                    // ~200 ms.
                    // NOTE: while __present_in_progress is set this whole
                    // chain is skipped, so the clock advance below pauses
                    // for one ~5 ms packet per present — imperceptible in
                    // the 200 ms slide (deliberate, not a bug).
                    if (mag_active && mag_src_app >= 0) {
                        mag_pkt_count++;
                        mag_fraction += MAG_FRACTION_STEP_IRQ;
                        if (mag_fraction > MAG_FRACTION_MAX)
                            mag_fraction = MAG_FRACTION_MAX;
                    }
                    // Normal cursor save/restore — the cursor is drawn
                    // on top at the end of the chain.
                    cursor_restore_bg(vesa.fb);
                    cursor_save_bg(mouse.px, mouse.py, vesa.fb);
                }
                // ── Draw cursor on top (always) ──────────────────────────
                mouse_draw_vesa_cursor_on(vesa.fb);
                mouse_draw_text_cursor();
            }

            packet_phase = mouse_wheel_mode ? 3 : 0;
            break;

        case 3:
            // Z wheel byte — signed 8-bit, positive = scroll up
            // (standard IntelliMouse layout).  Accumulate into
            // mouse_wheel_delta; the desktop heartbeat consumes it
            // and routes it to the open list's scroll state.
            {
                int dz = data;
                if (dz & 0x80) dz -= 256;  // two's complement sign
                mouse_wheel_delta += dz;
            }
            packet_phase = 0;
            break;
    }
}

// ─── Initialization ───────────────────────────────────────────────

void mouse_install(void) {
    // 1. Enable the auxiliary PS/2 device (second port)
    mouse_wait_write();
    outb(0x64, 0xA8);

    // 2. Read the current command byte
    mouse_wait_write();
    outb(0x64, 0x20);
    mouse_wait_read();
    uint8_t cmd = inb(0x60);

    // 3. Set bit 1 (enable IRQ12 for aux) and clear bit 5 (clock)
    cmd |= 0x02;
    cmd &= ~0x20;

    // 4. Write back
    mouse_wait_write();
    outb(0x64, 0x60);
    mouse_wait_write();
    outb(0x60, cmd);

    // 5. Reset the mouse device itself (0xFF = reset)
    mouse_write_cmd(0xFF);  // Reset
    mouse_wait_read();
    (void)inb(0x60);        // ACK (0xFA)
    // After reset, the mouse sends a self-test result (0xAA) and
    // device ID (0x00). Read them to flush the buffer.
    mouse_wait_read();
    (void)inb(0x60);        // Self-test result
    mouse_wait_read();
    (void)inb(0x60);        // Device ID

    // 6. Set default mouse settings
    mouse_write_cmd(0xF6);  // Set Defaults
    mouse_wait_read();      // ACK byte
    (void)inb(0x60);

    // 7. Enable IntelliMouse 4-byte wheel mode.  The magic
    //    sample-rate sequence (200 → 100 → 80) switches the device
    //    to 4-byte packets (device ID 3) whose 4th byte is a signed
    //    wheel delta.  QEMU's PS/2 emulation honors this, so host
    //    touchpad two-finger scrolls arrive as wheel events we can
    //    route to the Files list / Trash scrollbars.
    {
        static const uint8_t magic_rates[3] = { 200, 100, 80 };
        for (int i = 0; i < 3; i++) {
            mouse_write_cmd(0xF3);      // Set Sample Rate
            mouse_wait_read();          // ACK
            (void)inb(0x60);
            mouse_wait_write();
            outb(0x64, 0xD4);
            mouse_wait_write();
            outb(0x60, magic_rates[i]);
            mouse_wait_read();          // ACK
            (void)inb(0x60);
        }
    }
    // 8. Confirm the device actually switched to 4-byte mode by
    //    reading its device ID: 3 = IntelliMouse (wheel), 4 =
    //    IntelliMouse Explorer (wheel + 2 side buttons).  If the
    //    magic sequence didn't take, the ID stays 0 (3-byte mode)
    //    and we must NOT expect a wheel byte later — the packet
    //    machine would otherwise stay permanently misaligned with
    //    a dead cursor.
    mouse_write_cmd(0xF2);  // Read Device Type
    mouse_wait_read();      // ACK (0xFA)
    (void)inb(0x60);
    mouse_wait_read();      // Device ID
    uint8_t dev_id = inb(0x60);
    mouse_wheel_mode = (dev_id == 3 || dev_id == 4) ? 1 : 0;

    // 9. Set sample rate to 200 (responsiveness — the 4-byte mode
    //    latched by the magic sequence survives later rate changes)
    mouse_write_cmd(0xF3);  // Set Sample Rate
    mouse_wait_read();      // ACK
    (void)inb(0x60);
    mouse_wait_write();
    outb(0x64, 0xD4);
    mouse_wait_write();
    outb(0x60, 200);        // 200 samples/sec — more responsive tracking
    mouse_wait_read();
    (void)inb(0x60);

    // 11. Enable data reporting
    mouse_write_cmd(0xF4);  // Enable
    mouse_wait_read();      // ACK byte
    (void)inb(0x60);

    // 12. Register our IRQ12 handler
    irq_install_handler(12, mouse_callback);

    // 13. Enable VGA hardware cursor (block shape, scan lines 0-15)
    //     so mouse_draw_text_cursor() shows a visible block on screen.
    outb(0x3D4, 0x0A);
    outb(0x3D5, 0x00);  // Cursor Start = 0 (enable, scan line 0)
    outb(0x3D4, 0x0B);
    outb(0x3D5, 0x0F);  // Cursor End = 15 (bottom of character cell)

    // Centre the cursor on the framebuffer
    if (vesa.active) {
        mouse.px = (int)vesa.width / 2;
        mouse.py = (int)vesa.height / 2;
    } else {
        // Centre on the VGA text grid (80×25, cell size 8×16)
        mouse.px = 40 * 8;   // column 40, snapped to 8px boundary
        mouse.py = 12 * 16;  // row 12,  snapped to 16px boundary
    }

    mouse.left = mouse.right = mouse.middle = 0;
    mouse.click_pending = 0;
    mouse.click_count = 0;
    mouse.rclick_pending = 0;
    mouse.drag_active = 0;
    mouse.drop_pending = 0;
    mouse.drag_down_x = 0;
    mouse.drag_down_y = 0;
    packet_phase = 0;

    // ── Polarity: both axes negated during packet decode in
    // mouse_callback (X phase 1 and Y phase 2) so that positive
    // deltas = finger right/down = cursor right/down.
}

// ─── Cursor drawing ───────────────────────────────────────────────

void mouse_draw_vesa_cursor(void) {
    if (!vesa.active) return;

    int mx = mouse.px;
    int my = mouse.py;

    for (int row = 0; row < CURSOR_H; row++) {
        for (int col = 0; col < CURSOR_W; col++) {
            uint8_t p = cursor_data[row * CURSOR_W + col];
            if (p == 0) continue; // transparent

            int px = mx + col;
            int py = my + row;
            if (px < 0 || px >= (int)vesa.width) continue;
            if (py < 0 || py >= (int)vesa.height) continue;

            uint32_t color;
            if (p == 1) color = 0xFFFFFF;      // white fill
            else        color = 0x000000;      // black outline

            vesa_put_pixel(px, py, color);
        }
    }
}

// Same sprite as above but writes directly to a caller-provided buffer
// (e.g. vesa.fb for immediate-on-screen cursor updates in the dirty
// cursor path, bypassing the back-buffer copy entirely).
void mouse_draw_vesa_cursor_on(uint8_t* fb) {
    if (!vesa.active || !fb) return;

    int mx = mouse.px;
    int my = mouse.py;
    int bpp = (int)(vesa.bpp / 8);
    int pitch = (int)vesa.pitch;

    // ── Draw normal cursor sprite on top (unchanged) ──────────────
    for (int row = 0; row < CURSOR_H; row++) {
        for (int col = 0; col < CURSOR_W; col++) {
            uint8_t p = cursor_data[row * CURSOR_W + col];
            if (p == 0) continue; // transparent

            int px = mx + col;
            int py = my + row;
            if (px < 0 || px >= (int)vesa.width) continue;
            if (py < 0 || py >= (int)vesa.height) continue;

            uint32_t color;
            if (p == 1) color = 0xFFFFFF;      // white fill
            else        color = 0x000000;      // black outline

            uint32_t off = (uint32_t)py * (uint32_t)pitch + (uint32_t)px * (uint32_t)bpp;
            fb[off + 0] = color & 0xFF;           // B
            fb[off + 1] = (color >> 8) & 0xFF;    // G
            fb[off + 2] = (color >> 16) & 0xFF;   // R
            if (bpp == 4) fb[off + 3] = 0;        // padding
        }
    }
}

void mouse_draw_text_cursor(void) {
    // VGA hardware cursor: set position via CRTC registers 0x0E/0x0F.
    // The hardware cursor is a separate overlay — it does NOT modify
    // text memory, so no trails or save/restore needed. The cursor
    // position is the linear text-buffer index: row * 80 + col.
    int vx = mouse.px / 8;
    int vy = mouse.py / 16;
    if (vx < 0 || vx >= 80 || vy < 0 || vy >= 25) return;

    int pos = vy * 80 + vx;
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)(pos >> 8));
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
}

// ─── Click consumption ────────────────────────────────────────────

int mouse_get_click(int* out_x, int* out_y) {
    if (!mouse.click_pending) return 0;

    mouse.click_pending = 0;
    if (out_x) *out_x = mouse.click_x;
    if (out_y) *out_y = mouse.click_y;
    return 1;
}

int mouse_get_rightclick(int* out_x, int* out_y) {
    if (!mouse.rclick_pending) return 0;

    mouse.rclick_pending = 0;
    if (out_x) *out_x = mouse.rclick_x;
    if (out_y) *out_y = mouse.rclick_y;
    return 1;
}

// ─── Drag query functions ─────────────────────────────────────────

int mouse_is_dragging(void) {
    return mouse.drag_active;
}

int mouse_get_drop(int* out_x, int* out_y) {
    if (!mouse.drop_pending) return 0;

    mouse.drop_pending = 0;
    if (out_x) *out_x = mouse.drop_x;
    if (out_y) *out_y = mouse.drop_y;
    return 1;
}

// ─── Grab/activity indicator ─────────────────────────────────────

uint32_t mouse_last_activity(void) {
    return last_activity_tick;
}

// ═══════════════════════════════════════════════════════════════════
//  DIRTY BUFFER INVALIDATION
// ═══════════════════════════════════════════════════════════════════
// Called after desktop_draw() completes so the next IRQ call skips
// stale restore — the freshly drawn desktop has already cleaned all
// ═══════════════════════════════════════════════════════════════════
//  HOVER HELPERS
// ═══════════════════════════════════════════════════════════════════

int mouse_in_rect(int x, int y, int w, int h) {
    return mouse.px >= x && mouse.px < x + w &&
           mouse.py >= y && mouse.py < y + h;
}

// ═══════════════════════════════════════════════════════════════════
//  CLOSE [X] BUTTON HELPER
// ═══════════════════════════════════════════════════════════════════

void draw_close_btn(int x, int y, uint32_t bg, const char* app_name) {
    int hover = mouse_in_rect(x, y, 24, 16);
    uint32_t col = hover ? 0xFF8888 : 0xFF6B6B;
    if (hover) {
        // Red glow behind [X]
        vesa_fill_rect(x-4, y-4, 32, 24, 0x441010);
        vesa_draw_rect(x-4, y-4, 32, 24, 0xFF4444);
        
        // ── Tooltip ──────────────────────────────────────────────
        // Build "Close <app_name>" string
        char tip[36];
        int ti = 0;
        const char* prefix = "Close ";
        for (int i = 0; prefix[i] && ti < 35; i++) tip[ti++] = prefix[i];
        if (app_name) {
            for (int i = 0; app_name[i] && ti < 35; i++) tip[ti++] = app_name[i];
        }
        tip[ti] = '\0';
        int tw = ti * 8 + 16;  // tooltip pixel width
        int th = 20;            // tooltip pixel height
        
        // Position near cursor, clamp to screen bounds
        int tx = mouse.px + 12;
        int ty = mouse.py + 16;
        if (tx + tw > (int)vesa.width)  tx = mouse.px - tw - 4;
        if (ty + th > (int)vesa.height) ty = mouse.py - th - 4;
        if (tx < 0) tx = 0;
        if (ty < 0) ty = 0;
        
        // Draw tooltip: dark pill with accent border
        vesa_fill_rect(tx, ty, tw, th, 0x1A1240);
        vesa_draw_rect(tx, ty, tw, th, 0x6A4FBF);
        vesa_draw_string(tx+8, ty+2, tip, 0xE0D8FF, 0x1A1240);
    }
    vesa_draw_string(x, y, "[X]", col, bg);
}
