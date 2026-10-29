// cpu/pdfview.c — VESA PDF text viewer for Nyx OS
//
// Desktop-mode app that renders extracted PDF text in a scrollable
// VESA panel with a title bar (filename + page count) and a status
// bar (scroll position + controls). Arrow keys / WASD to scroll,
// ESC to exit.
//
// The caller (desktop.c ENTER handler) calls pdfview_init() with
// the text extracted by pdftext_extract(), and switches to
// MODE_PDFVIEW. pdfview_draw() renders the text word-wrapped to
// the panel width, and pdfview_handle_key() handles scrolling/exit.

#include <pdfview.h>
#include <vesa.h>
#include <desktop.h>
#include <mouse.h>
#include <stdint.h>
#include <stddef.h>

// ─── Viewer state ────────────────────────────────────────────────
static int          pv_active = 0;
static char         pv_text[262144];   // extracted PDF text buffer
static int          pv_text_len = 0;
static char         pv_title[64] = {0};
static int          pv_pages = 0;

// Scroll / layout
static int pv_scroll_y = 0;
static int pv_content_h = 0;

#define PV_TITLE_H  28
#define PV_STATUS_H 20
#define PV_LINE_H   18          // 8x16 font, with 2px line spacing
#define PV_CHAR_W   8

// ─── Word-wrap helper ────────────────────────────────────────────
// Compute line start indices for a given panel width.
// Returns number of lines.
#define PV_MAX_LINES 2048

static int pv_line_starts[PV_MAX_LINES];
static int pv_line_count = 0;

static void pv_wrap(int panel_w) {
    int max_chars = panel_w / PV_CHAR_W;
    if (max_chars < 1) max_chars = 1;
    pv_line_count = 0;
    int pos = 0;
    while (pos < pv_text_len && pv_line_count < PV_MAX_LINES) {
        pv_line_starts[pv_line_count++] = pos;
        // Find end of this physical line
        int next_newline = pos;
        while (next_newline < pv_text_len && pv_text[next_newline] != '\n')
            next_newline++;
        int line_len = next_newline - pos;
        // Wrap: if line is longer than max_chars, we need to word-wrap
        if (line_len > max_chars) {
            // Try to find a space within max_chars
            int break_pos = pos + max_chars;
            // Scan backward from break_pos for a space
            int space_pos = -1;
            for (int i = break_pos; i > pos; i--) {
                if (pv_text[i] == ' ') { space_pos = i; break; }
            }
            if (space_pos > pos) {
                pos = space_pos + 1;  // skip the space
            } else {
                pos = break_pos;       // no space found, hard break
            }
        } else {
            pos = next_newline + 1;    // skip the newline
        }
    }
}

// ─── Public API ──────────────────────────────────────────────────

void pdfview_init(const char* text, int text_len, const char* filename,
                  int page_count) {
    // Copy text into buffer
    int copy_len = text_len;
    if (copy_len > (int)sizeof(pv_text) - 1)
        copy_len = (int)sizeof(pv_text) - 1;
    for (int i = 0; i < copy_len; i++)
        pv_text[i] = text[i];
    pv_text[copy_len] = '\0';
    pv_text_len = copy_len;
    pv_pages = page_count;
    pv_scroll_y = 0;

    // Copy title
    int ti = 0;
    while (filename[ti] && ti < 55) { pv_title[ti] = filename[ti]; ti++; }
    if (page_count > 0) {
        pv_title[ti++] = ' ';
        pv_title[ti++] = '(';
        int n = page_count;
        if (n >= 100) { pv_title[ti++] = '0' + n / 100; n %= 100; }
        if (n >= 10)  { pv_title[ti++] = '0' + n / 10; n %= 10; }
        pv_title[ti++] = '0' + n;
        pv_title[ti++] = ' ';
        pv_title[ti++] = 'p';
        pv_title[ti++] = 'g';
        if (page_count > 1) pv_title[ti++] = 's';
        pv_title[ti++] = ')';
    }
    pv_title[ti] = '\0';

    // Compute available height for text
    // Will be recomputed in pdfview_draw() for the windowed position
    pv_content_h = (int)vesa.height - PV_TITLE_H - PV_STATUS_H - 20;

    // Word-wrap (re-done in draw with actual panel width)
    pv_wrap((int)vesa.width - 16);

    pv_active = 1;
}

int pdfview_is_active(void) {
    return pv_active;
}

void pdfview_draw(void) {
    if (!pv_active) return;

    int pw = (int)vesa.width;
    int ph = (int)vesa.height;

    // ── Window position (preserves dragged position) ──────────────
    int wx = (g_win_x >= 0) ? g_win_x : 16;
    int wy = (g_win_y >= 0) ? g_win_y : 40;
    int ww = pw - 32;
    int wh = ph - wy - 16;
    int content_y = PV_TITLE_H;
    pv_content_h = wh - PV_TITLE_H - PV_STATUS_H;
    if (pv_content_h < 60) pv_content_h = 60;

    // Re-wrap for current panel width
    pv_wrap(ww - 16);

    // ── Title bar ──
    vesa_fill_rect(wx, wy, ww, PV_TITLE_H, 0x1A1A2E);
    vesa_fill_rect(wx, wy + PV_TITLE_H - 2, ww, 2, 0x3A3A5E);
    g_win_x = wx; g_win_y = wy; g_win_w = ww; g_win_h = wh + PV_TITLE_H + PV_STATUS_H;
    draw_close_btn(wx + ww - 32, wy + 6, 0x1A1A2E, "PDF Viewer");
    vesa_draw_string(wx + 8, wy + 6, pv_title, 0xCCDDFF, 0x1A1A2E);

    // ── Content area ──
    int content_y_abs = wy + content_y;
    vesa_fill_rect(wx, content_y_abs, ww, pv_content_h + 20, 0x0A0A18);

    int text_x = wx + 8;
    int max_vis = pv_content_h / PV_LINE_H;
    if (max_vis < 1) max_vis = 1;

    // Draw visible lines
    for (int li = 0; li < max_vis; li++) {
        int line_idx = pv_scroll_y + li;
        if (line_idx >= pv_line_count) break;

        int start = pv_line_starts[line_idx];
        int end = (line_idx + 1 < pv_line_count) ? pv_line_starts[line_idx + 1] : pv_text_len;
        int display_end = end;
        while (display_end > start && (pv_text[display_end - 1] == '\n' || pv_text[display_end - 1] == '\r'))
            display_end--;

        int cx = text_x;
        int cy = content_y_abs + 12 + li * PV_LINE_H;
        for (int i = start; i < display_end; i++) {
            char c = pv_text[i];
            if (c == '\t') c = ' ';
            if (c >= 32 && c <= 126) {
                vesa_draw_char(cx, cy, c, 0xD0D0E8, 0);
                cx += PV_CHAR_W;
            } else {
                cx += PV_CHAR_W;
            }
        }
    }

    // ── Scrollbar (with hover highlight) ──
    if (pv_line_count > max_vis) {
        int sb_x = wx + ww - 10;
        int sb_h = pv_content_h;
        if (sb_h < 1) sb_h = 1;
        int sb_top = content_y_abs + 12;
        int thumb_h = sb_h * max_vis / pv_line_count;
        if (thumb_h < 8) thumb_h = 8;
        int thumb_y = sb_top + (pv_scroll_y * (sb_h - thumb_h) / (pv_line_count - max_vis));
        int sb_hover = mouse_in_rect(sb_x, sb_top, 6, sb_h);
        uint32_t track_col = sb_hover ? 0x1A1A3A : 0x12122A;
        uint32_t thumb_col = sb_hover ? 0x9A7FDF : 0x6A4FBF;
        vesa_fill_rect(sb_x, sb_top, 6, sb_h, track_col);
        vesa_fill_rect(sb_x, thumb_y, 6, thumb_h, thumb_col);
    }

    // ── Status bar ──
    int status_y = wy + wh - PV_STATUS_H;
    vesa_fill_rect(wx, status_y, ww, PV_STATUS_H, 0x1A1A2E);
    vesa_draw_rect(wx, status_y, ww, PV_STATUS_H, 0x3A3A5E);
    vesa_draw_string(wx + 8, status_y + 3,
                     "W/S=line  P/N=page  G=top  E=end  ESC=exit", 0x8899BB, 0x1A1A2E);
    vesa_draw_string(wx + (ww - 14 * 8) / 2, status_y + 3,
                     "ESC to exit", 0x8899BB, 0x1A1A2E);

    if (pv_line_count > max_vis) {
        char sp[16]; int si = 0;
        int max_scroll = pv_line_count - max_vis;
        int pct = pv_scroll_y * 100 / max_scroll;
        sp[si++] = 'V'; sp[si++] = ':';
        if (pct >= 100) { sp[si++] = '1'; pct -= 100; }
        if (pct >= 10)  { sp[si++] = '0' + pct / 10; pct %= 10; }
        sp[si++] = '0' + pct; sp[si++] = '%';
        sp[si] = '\0';
        vesa_draw_string(wx + ww - 8 - si * 8, status_y + 3, sp, 0x8899BB, 0x1A1A2E);
    }
}

void pdfview_handle_key(char c) {
    if (!pv_active) return;

    if (c == 27) {  // ESC
        pv_active = 0;
        return;
    }

    int max_vis = pv_content_h / PV_LINE_H;
    if (max_vis < 1) max_vis = 1;
    int max_scroll = pv_line_count - max_vis;
    if (max_scroll < 0) max_scroll = 0;

    int oy = pv_scroll_y;

    if (c == 'A' || c == 'w' || c == 'W') { // Up
        pv_scroll_y--;
        if (pv_scroll_y < 0) pv_scroll_y = 0;
    } else if (c == 'B' || c == 's' || c == 'S') { // Down
        pv_scroll_y++;
        if (pv_scroll_y > max_scroll) pv_scroll_y = max_scroll;
    } else if (c == 'p' || c == 'P') { // Page Up
        pv_scroll_y -= max_vis;
        if (pv_scroll_y < 0) pv_scroll_y = 0;
    } else if (c == 'n' || c == 'N') { // Page Down
        pv_scroll_y += max_vis;
        if (pv_scroll_y > max_scroll) pv_scroll_y = max_scroll;
    } else if (c == 'g' || c == 'G') { // Go to top
        pv_scroll_y = 0;
    } else if (c == 'e' || c == 'E') { // Go to end
        pv_scroll_y = max_scroll;
    }

    if (pv_scroll_y != oy) {
        // Redraw — caller calls desktop_draw() after us
    }
}

// ─── Mouse handler ───────────────────────────────────────────────
int pdfview_handle_mouse(int mx, int my) {
    if (!vesa.active) return 0;
    if (!pv_active) return 0;

    int wx = (g_win_x >= 0) ? g_win_x : 16;
    int wy = (g_win_y >= 0) ? g_win_y : 40;
    int ww = (int)vesa.width - 32;
    int wh = (int)vesa.height - wy - 16;
    int content_y = PV_TITLE_H;
    int content_y_abs = wy + content_y;
    int max_vis = pv_content_h / PV_LINE_H;
    if (max_vis < 1) max_vis = 1;
    int max_scroll = pv_line_count - max_vis;
    if (max_scroll < 0) max_scroll = 0;

    // ── Scrollbar track ──────────────────────────────────────────
    int sb_x = wx + ww - 10;
    int sb_h = pv_content_h;
    if (sb_h < 1) sb_h = 1;
    int sb_top = content_y_abs + 12;

    if (mx >= sb_x && mx < sb_x + 6 &&
        my >= sb_top && my < sb_top + sb_h) {
        int rel_y = my - sb_top;
        int new_scroll = rel_y * max_scroll / sb_h;
        if (new_scroll < 0) new_scroll = 0;
        if (new_scroll > max_scroll) new_scroll = max_scroll;
        pv_scroll_y = new_scroll;
        return 1;
    }

    // ── Content area click ───────────────────────────────────────
    if (mx >= wx && mx < sb_x &&
        my >= content_y_abs + 12 && my < content_y_abs + 12 + pv_content_h) {
        int rel_y = my - (content_y_abs + 12);
        int clicked_line = rel_y / PV_LINE_H;
        int target = pv_scroll_y + clicked_line - max_vis / 2;
        if (target < 0) target = 0;
        if (target > max_scroll) target = max_scroll;
        pv_scroll_y = target;
        return 1;
    }

    return 0;
}
