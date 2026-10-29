// include/mouse.h
#ifndef MOUSE_H
#define MOUSE_H
#include <stdint.h>

// ─── Cursor sprite dimensions ─────────────────────────────────────
// Shared between mouse.c (sprite data + draw functions) and
// desktop.c (dirty-cursor save/restore buffer). Both must agree
// on the sprite size or the save buffer will be wrong.
#define CURSOR_W 16
#define CURSOR_H 16

// ─── Dirty-cursor save buffer dimensions ──────────────────────────
// The save buffer stores each pixel as a padded uint32_t (BGRA) so
// the stride is CURSOR_BG_W * sizeof(uint32_t) regardless of the VESA
// framebuffer's actual bytes-per-pixel (the save/restore loops in
// mouse.c convert between the two formats).
// Increased from 16 to 20 to accommodate the 2px glow border drawn
// around the cursor when hovering over a draggable desktop icon.
// 20×20 with the save offset at the cursor position (no -2px shift).
// The extra capture is a harmless margin that also covers the drag
// path (where cursor_save_bg() saves at ghost_filter, which tracks
// mouse.px/py 1:1), so a fast flick can't leave a leading-edge trail.
// (This region was 20×20 with -2 offsets for the removed 2px hover
// glow; the -2 is gone but the size stays.)
#define CURSOR_BG_W       20
#define CURSOR_BG_H       20
#define CURSOR_BG_STRIDE  (CURSOR_BG_W * (int)sizeof(uint32_t))

// Drag source types — tells the desktop what kind of item is being
// dragged so it can dispatch the drop correctly.
#define DRAG_NONE  0
#define DRAG_ICON  1   // dragging a desktop icon (rearrange)
#define DRAG_FILE  2   // dragging a file from the Files app
#define DRAG_WINDOW 3   // dragging a window by its title bar
#define DRAG_SELECT 4   // dragging from EMPTY desktop = rubber-band selection box
#define DRAG_GROUP 5   // dragging one of several box-selected icons = move the whole group

// ─── Group drag (multi-selection move) ────────────────────────────
// After a rubber-band box-select, dragging any selected icon moves the
// entire group together, preserving relative layout.  The IRQ sets
// g_group_drag_active at threshold-crossing when the pressed slot is
// occupied AND selected (box_sel) AND more than one icon is selected;
// desktop.c's drop dispatch (DRAG_GROUP) then translates every
// selected slot by the same (dcol,drow) as the anchor slot.
#define GROUP_MAX_SLOTS 40   // max icons in one group move (grid fits 112)// Accumulated PS/2 wheel (Z) deltas; the desktop heartbeat consumes
// and clears this each tick, routing it to the open list's scroll.
// Signed: positive = scroll up (wheel up).
extern int mouse_wheel_delta;
extern int g_group_drag_active;   // 1 while a group drag is live
// Anchor = the selected slot that was pressed; the group ghost and the
// drop translation are both computed relative to it.  g_group_slots[]
// snapshots the selected slots at drag start (drop may re-read box_sel,
// but this is stable and avoids racing desktop.c's ring updates).
extern int g_group_anchor;
extern int g_group_count;
extern int g_group_slots[GROUP_MAX_SLOTS];

// Forget the last-drawn group-ghost rect.  Called by desktop.c after a
// full redraw presents a clean frame, so the next IRQ packet doesn't
// restore stale background thinking a cluster is still on screen.
void group_ghost_invalidate(void);

// ─── Rubber-band selection box (IRQ-time) ─────────────────────────
// Dragging from EMPTY desktop draws a live selection box at 200 Hz.
// The box is XOR-drawn on vesa.fb (draw twice to erase), so no
// save buffer is needed at any size.  desktop.c finalizes which icons
// are inside the box on drop (DRAG_SELECT dispatch).
extern int g_sel_drag_active;   // 1 while a selection-box drag is live
// Press-down corner of the box (the opposite corner is mouse.px/py).
extern int g_sel_x0, g_sel_y0;

// ─── Present-generation guards (IRQ vs vesa_present race) ──────────
// desktop.c's finish_frame() bumps these right after vesa_present()
// copies the clean back buffer over vesa.fb (which wipes any IRQ-drawn
// selection box AND the cursor).  The IRQ only XOR-erases the old box /
// restores the saved cursor background when it was drawn/saved in the
// CURRENT generation — so a mouse packet arriving in the window between
// present() and the state invalidation can't tint clean desktop pixels
// (phantom box) or paint stale pre-present pixels (stale cursor copy).
extern int sel_box_present_gen;
extern int cursor_present_gen;

// Immediately erase the currently-painted selection box from vesa.fb
// (XOR self-restore).  Called on button release so a rapid re-drag can't
// paint a second box on top of the stale one.  No-op when no box is drawn.
void mouse_sel_box_erase_now(void);

// Re-draw the selection box after vesa_present() wiped the screen, if a
// select-drag is still live.  Keeps the IRQ's draw state in sync with the
// actual framebuffer (no flicker, no phantom).  Called from desktop.c
// finish_frame() right after the present.  No-op when no drag is active.
void mouse_sel_box_redraw_after_present(void);

// PS/2 mouse state — updated by the IRQ12 handler, read by the
// desktop renderer and click-dispatch code.
typedef struct {
    int px, py;          // VESA pixel coords (top-left of cursor hotspot)
    int left, right, middle;  // 1 = button currently pressed

    // Click event FIFO (single-entry for simplicity — the desktop
    // draw loop polls this once per frame; no need for a ring)
    int  click_pending;  // 1 if a left-click happened since last read
    int  click_count;    // number of press events since last mouse_get_click() —
                         // used to detect double-clicks that happen between heartbeats
    int  click_x;        // pixel x of the most recent click
    int  click_y;        // pixel y of the most recent click

    // Right-click event FIFO (same single-entry pattern as left-click)
    int  rclick_pending; // 1 if a right-click happened since last read
    int  rclick_x;       // pixel x of the most recent right-click
    int  rclick_y;       // pixel y of the most recent right-click

    // ─── Drag-and-drop state (updated by IRQ12 handler) ────────────
    // Physical drag tracking: the IRQ handler watches left-button
    // hold and movement to detect drag start/end. The desktop layer
    // queries these to manage drag sources and dispatch drops.
    int drag_down_x;     // px where left button was first pressed
    int drag_down_y;     // py where left button was first pressed
    int drag_active;     // 1 = button held and moved past threshold
    int drop_pending;    // 1 = a drop event happened since last read
    int drop_x;          // px where the button was released
    int drop_y;          // py where the button was released
} MouseState;

extern MouseState mouse;

// PS/2 mouse: initialize controller, enable IRQ12, start reporting
void mouse_install(void);

// Draw the 16x16 arrow cursor at mouse.px, mouse.py on the VESA
// back buffer (call after all other drawing, before vesa_present).
void mouse_draw_vesa_cursor(void);

// Draw the cursor sprite directly to an arbitrary pixel buffer
// (e.g. vesa.fb). Used by the dirty cursor path in desktop.c to
// paint the cursor on the visible framebuffer without a back-buffer
// copy. Writes pixel data directly via the buffer pointer.
void mouse_draw_vesa_cursor_on(uint8_t* fb);

// VGA text fallback — draw a 2-char block cursor at VGA coords
void mouse_draw_text_cursor(void);

// Check for a pending click and consume it. Returns 1 if a click
// was consumed (stores coords via pointers), 0 otherwise.
int mouse_get_click(int* out_x, int* out_y);

// Check for a pending right-click and consume it. Returns 1 if a
// right-click was consumed (stores coords via pointers), 0 otherwise.
int mouse_get_rightclick(int* out_x, int* out_y);

// Query the PIT tick of the most recent PS/2 mouse packet.
// Returns 0 before the first packet arrives. Used by the desktop
// taskbar to show a QEMU input-grab indicator — if tick delta is
// small, input is being forwarded; if large, the capture is idle.
uint32_t mouse_last_activity(void);

// Drag-and-drop queries — called from desktop_draw() each frame.

// Returns 1 if the user is currently dragging something (button
// held + moved past threshold).
int mouse_is_dragging(void);

// Check for a pending drop and consume it. Returns 1 if a drop
// was consumed (stores coords via pointers), 0 otherwise.
int mouse_get_drop(int* out_x, int* out_y);

// ─── Convenience: check if the cursor is inside a rectangle ─────
// Returns 1 if mouse.px/py is within [x, x+w) × [y, y+h).
// Uses the real-time mouse position — intended for draw functions
// that want to render hover highlights.
int mouse_in_rect(int x, int y, int w, int h);

// ─── Draw a close [X] button with hover glow + tooltip ──────────
// Draws "[X]" at (x, y). When the cursor hovers, shows a red glow
// behind the [X] and a floating tooltip "Close <app_name>" near the
// cursor. `bg` is the title-bar background color used for text draw.
void draw_close_btn(int x, int y, uint32_t bg, const char* app_name);

// ─── Cursor save/restore (shared with desktop.c) ─────────────────
// The mouse IRQ handler does dirty-cursor save/restore/draw at the
// full PS/2 sample rate (200 Hz).  The desktop heartbeat skips its
// own VESA cursor block because the IRQ already handled it.
// These variables and functions are defined in mouse.c and declared
// here so desktop.c can also use them for the first-frame draw.
extern int cursor_bg_x, cursor_bg_y, cursor_bg_valid;
// NOTE: The volatile __cursor_bg_dirty guard flag was removed.  It was
// set by desktop_draw() while writing to vesa.draw and read by the IRQ
// handler to skip cursor_save_bg.  Now the IRQ reads from vesa.fb, so
// no guard is needed.  If a future change re-introduces vesa.draw reads
// from the IRQ handler, re-add: extern volatile int __cursor_bg_dirty;
void cursor_save_bg(int mx, int my, uint8_t* fb);
void cursor_restore_bg(uint8_t* fb);

// ─── Drag ghost overlay (lightweight IRQ-time tile + label) ───────
// The ghost is a pre-rendered tile that follows the cursor during
// drag-and-drop, drawn directly to vesa.fb in the IRQ handler at
// 200 Hz alongside the cursor sprite using a save/restore buffer.
// Grid layout is shared so the IRQ handler can identify which app
// is under the press position when drag_active transitions to 1.
#define GHOST_W 80
#define GHOST_H 90

// Icon grid layout (set by desktop.c's compute_icon_grid, read by
// mouse.c IRQ handler to identify drag source at press-down pos).
extern int g_grid_start_x, g_grid_start_y;
extern int g_grid_cell_w, g_grid_cell_h;
extern int g_grid_rows, g_grid_slots, g_grid_cols;

// Ghost position — the IRQ handler keeps this locked to mouse.px/py
// (1:1 cursor tracking; a low-pass filter was removed for instant
// response).  The ghost tile is drawn at this position, and the
// slide-in animation starts from here to avoid a visual jump on drop.
extern int g_ghost_filter_px, g_ghost_filter_py;

// Ghost app index (set/cleared by desktop.c drag source detection,
// read by mouse.c IRQ handler to draw the correct app tile).
// -1 means no ghost should be drawn.
extern int g_ghost_app_idx;

// Ghost label text (points to app_names[g_ghost_app_idx] in desktop.c).
extern const char* g_ghost_label;

// Ghost save/restore (used by IRQ handler — functions in mouse.c,
// exposed here for potential debug use but not needed externally).
void ghost_restore_bg(uint8_t* fb);
void ghost_save_bg(int mx, int my, uint8_t* fb);
void ghost_invalidate_bg(void);

// Invalidate all IRQ-level dirty-rectangle buffers (cell, preview,
// ghost, cursor).  Called after desktop_draw() to prevent the next
// IRQ call from restoring stale pre-redraw background pixels.

// ─── Snap-to-grid preview ─────────────────────────────────────────
// During a drag, a subtle ghost copy of the dragged icon is drawn at
// the grid slot under the cursor to show where the icon will land.
// Uses a separate save/restore buffer so it doesn't interfere with
// the main drag ghost or the cursor.
#define PREVIEW_W 80
#define PREVIEW_H 90

// Snap preview save/restore buffer functions (for IRQ handler use)
void preview_restore_bg(uint8_t* fb);
void preview_save_bg(int mx, int my, uint8_t* fb);

// Exposed desktop.c data needed by the IRQ handler's snap preview:
// icon_order[] maps grid position to app index, app_names[] gives
// the label text, and g_drag_app_idx/name track the current drag.
extern int icon_order[];
extern const char* app_names[];
extern int g_drag_app_idx;
extern const char* g_drag_app_name;

// ─── Slide-in animation state (200 Hz sub-tick interpolation) ────
// The drop position is set by desktop.c's drop handler; the slide
// runs on a 0-255 sub-tick clock (mag_fraction).  The PS/2 IRQ
// (200 Hz) advances it per packet; the desktop HEARTBEAT (50 Hz)
// advances it as an idle fallback — only when no packets arrived
// since the last tick, so the two never double-advance.  The ghost
// itself is rendered exclusively by the heartbeat (single writer,
// post-present, see mouse_flying_ghost_render) so the animation is
// always visible even at zero packet rate and never leaves a trail.
// Either way the slide completes in ~200 ms and the heartbeat
// commits the swap via mag_commit_pending.
#define MAG_FRAMES 10               // nominal duration: 10 heartbeats ≈ 200 ms
#define MAG_FRACTION_MAX 256        // fraction 0-255; 256 = complete
#define MAG_FRACTION_STEP_HB (MAG_FRACTION_MAX / MAG_FRAMES) // 25/tick idle fallback
#define MAG_FRACTION_STEP_IRQ (MAG_FRACTION_STEP_HB / 4)     // 6/packet (~4 pkts/heartbeat)
#define MAG_TRAIL_MAX 6 // px, max trail ring radius
extern int  mag_active;            // 1 = animation in progress
extern int  mag_fraction;          // 0 → MAG_FRACTION_MAX (sub-tick clock)
extern int  mag_pkt_count;         // packets seen while anim active (IRQ advances)
extern int  mag_sx, mag_sy;        // start (ghost position at drop)
extern int  mag_ex, mag_ey;        // end (target tile center)
extern int  mag_src_slot;          // source grid slot
extern int  mag_tgt_slot;          // target grid slot
extern int  mag_src_app;           // app index at source slot
extern int  mag_tgt_type;          // 0 = move to empty, 1 = swap
extern int  mag_tgt_occupied;      // app index at target (-1 if empty)
extern int  mag_commit_pending;    // 1 when animation done, commit swap
// (mag_last_advance_tick was part of the stall-recovery hack in a
//  previous design where the IRQ drove the frame counter — now the
//  heartbeat advances the sub-tick clock directly, so the variable
//  is unused.)
extern uint32_t mag_last_advance_tick;

// Render the flying ghost at the current mag_fraction interpolation.
// Called by the desktop heartbeat every animation tick AFTER
// desktop_draw() has presented a clean frame; the ghost is drawn
// directly on top of vesa.fb (like the cursor) and the next tick's
// present() wipes it — the animation's single writer, so the slide
// never leaves a trail.  The IRQ advances the sub-tick clock only.
extern void mouse_flying_ghost_render(void);

#endif
