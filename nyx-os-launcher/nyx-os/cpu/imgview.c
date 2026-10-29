// cpu/imgview.c — VESA image viewer for Nyx OS
//
// Desktop-mode app. After imgview_init() is called with a decoded
// image, imgview_draw() renders the VESA panel and
// imgview_handle_key() handles scroll/zoom/exit/adjust.

#include <imgview.h>
#include <vesa.h>
#include <desktop.h>
#include <mouse.h>
#include <stdint.h>
#include <stddef.h>

// ─── Viewer state ────────────────────────────────────────────────
static int          iv_active = 0;
static int          iv_img_w  = 0;
static int          iv_img_h  = 0;
static const uint8_t* iv_pixels = NULL;
static char         iv_title[64] = {0};

// Scroll / layout
static int iv_scroll_x = 0;
static int iv_scroll_y = 0;
static int iv_content_w = 0;
static int iv_content_h = 0;
static int iv_content_x = 0;
static int iv_content_y = 0;
static int iv_needs_scroll = 0;
static int iv_zoom_fit = 0;  // 1 = scale to fit screen, 0 = 1:1 pixels

// Brightness/contrast adjustment
static int iv_brightness   = 0;   // -100 .. +100
static int iv_contrast     = 0;   // -100 .. +100
static int iv_adjust_active = 0;  // 1 = showing adjust overlay
static int iv_adjust_param  = 0;  // 0 = brightness, 1 = contrast

#define IV_TITLE_H  28
#define IV_STATUS_H 20
#define IV_ADJUST_H 60  // height of the adjust slider panel

// ─── Draw helpers ────────────────────────────────────────────────
static void iv_draw_image(int sx, int sy, int pw, int ph) {
    // Check if adjustments are active
    if (iv_brightness != 0 || iv_contrast != 0) {
        vesa_blit_rgba_adjusted(iv_content_x, iv_content_y,
                                iv_pixels, iv_img_w, iv_img_h,
                                sx, sy, pw, ph,
                                iv_brightness, iv_contrast);
    } else {
        vesa_blit_rgba(iv_content_x, iv_content_y,
                       iv_pixels, iv_img_w, iv_img_h,
                       sx, sy, pw, ph);
    }
}

// ─── Adjust slider overlay ──────────────────────────────────────
#define BC_SLIDER_W  300
#define BC_SLIDER_H  16
#define BC_SLIDER_Y  10
#define BC_KNOB_R    7

static void iv_draw_adjust_panel(int wx, int wy, int ww) {
    if (!iv_adjust_active) return;
    int ph = (int)vesa.height;
    int panel_y = (wy + g_win_h - IV_STATUS_H - IV_ADJUST_H);

    // Panel background — dark glass
    for (int y = 0; y < IV_ADJUST_H; y++) {
        int t = y * 255 / IV_ADJUST_H;
        int r = 0x18 + (0x0C - 0x18) * t / 255;
        int g = 0x18 + (0x0A - 0x18) * t / 255;
        int b = 0x30 + (0x20 - 0x30) * t / 255;
        vesa_hline(wx, panel_y + y, ww, (r << 16) | (g << 8) | b);
    }
    vesa_hline(wx, panel_y, ww, 0x6A4FBF);
    vesa_hline(wx, panel_y + IV_ADJUST_H - 1, ww, 0x060310);

    // Center of the panel within the window
    int cx = wx + ww / 2;

    // ── Brightness slider ────────────────────────────────────────
    int bright_active = (iv_adjust_param == 0);
    uint32_t label_col = bright_active ? 0xFFCC66 : 0x8899BB;
    vesa_draw_string(cx - 220, panel_y + BC_SLIDER_Y, "Brightness", label_col, 0);

    // Slider track
    int bx = cx - BC_SLIDER_W / 2;
    int by = panel_y + BC_SLIDER_Y + 16;
    uint32_t track_col = 0x2A2060;
    vesa_fill_rect(bx, by, BC_SLIDER_W, 6, track_col);
    vesa_draw_rect(bx, by, BC_SLIDER_W, 6, 0x4A3870);

    // Fill active portion (left = dark, right = bright)
    int fill_w = (iv_brightness + 100) * BC_SLIDER_W / 200;
    if (fill_w > 0) {
        uint32_t fill_c = bright_active ? 0x8866FF : 0x4A3870;
        vesa_fill_rect(bx, by, fill_w, 6, fill_c);
    }

    // Knob
    int knob_x = bx + fill_w;
    int knob_y = by + 3;
    if (bright_active) {
        vesa_fill_rect(knob_x - BC_KNOB_R, knob_y - BC_KNOB_R,
                       BC_KNOB_R * 2 + 1, BC_KNOB_R * 2 + 1, 0xE8D8FF);
        vesa_draw_rect(knob_x - BC_KNOB_R, knob_y - BC_KNOB_R,
                       BC_KNOB_R * 2 + 1, BC_KNOB_R * 2 + 1, 0xFFFFFF);
    } else {
        vesa_fill_rect(knob_x - BC_KNOB_R, knob_y - BC_KNOB_R,
                       BC_KNOB_R * 2 + 1, BC_KNOB_R * 2 + 1, 0x6A4FBF);
        vesa_draw_rect(knob_x - BC_KNOB_R, knob_y - BC_KNOB_R,
                       BC_KNOB_R * 2 + 1, BC_KNOB_R * 2 + 1, 0x8A6FDF);
    }

    // Value label
    char bright_str[12]; int bi = 0;
    if (iv_brightness < 0) { bright_str[bi++] = '-'; }
    int bv = iv_brightness < 0 ? -iv_brightness : iv_brightness;
    if (bv >= 100) bright_str[bi++] = '0' + bv / 100;
    if (bv >= 10)  bright_str[bi++] = '0' + (bv / 10) % 10;
    bright_str[bi++] = '0' + bv % 10;
    bright_str[bi] = '\0';
    vesa_draw_string(bx + BC_SLIDER_W + 12, by - 3, bright_str, label_col, 0);

    // ── Contrast slider ──────────────────────────────────────────
    int contrast_active = (iv_adjust_param == 1);
    label_col = contrast_active ? 0xFFCC66 : 0x8899BB;
    vesa_draw_string(cx - 220, panel_y + BC_SLIDER_Y + 30, "Contrast", label_col, 0);

    by = panel_y + BC_SLIDER_Y + 16 + 28;
    vesa_fill_rect(bx, by, BC_SLIDER_W, 6, track_col);
    vesa_draw_rect(bx, by, BC_SLIDER_W, 6, 0x4A3870);

    fill_w = (iv_contrast + 100) * BC_SLIDER_W / 200;
    if (fill_w > 0) {
        uint32_t fill_c = contrast_active ? 0x8866FF : 0x4A3870;
        vesa_fill_rect(bx, by, fill_w, 6, fill_c);
    }

    knob_x = bx + fill_w;
    knob_y = by + 3;
    if (contrast_active) {
        vesa_fill_rect(knob_x - BC_KNOB_R, knob_y - BC_KNOB_R,
                       BC_KNOB_R * 2 + 1, BC_KNOB_R * 2 + 1, 0xE8D8FF);
        vesa_draw_rect(knob_x - BC_KNOB_R, knob_y - BC_KNOB_R,
                       BC_KNOB_R * 2 + 1, BC_KNOB_R * 2 + 1, 0xFFFFFF);
    } else {
        vesa_fill_rect(knob_x - BC_KNOB_R, knob_y - BC_KNOB_R,
                       BC_KNOB_R * 2 + 1, BC_KNOB_R * 2 + 1, 0x6A4FBF);
        vesa_draw_rect(knob_x - BC_KNOB_R, knob_y - BC_KNOB_R,
                       BC_KNOB_R * 2 + 1, BC_KNOB_R * 2 + 1, 0x8A6FDF);
    }

    char cont_str[12]; int ci = 0;
    if (iv_contrast < 0) { cont_str[ci++] = '-'; }
    int cv = iv_contrast < 0 ? -iv_contrast : iv_contrast;
    if (cv >= 100) cont_str[ci++] = '0' + cv / 100;
    if (cv >= 10)  cont_str[ci++] = '0' + (cv / 10) % 10;
    cont_str[ci++] = '0' + cv % 10;
    cont_str[ci] = '\0';
    vesa_draw_string(bx + BC_SLIDER_W + 12, by - 3, cont_str, label_col, 0);

    // Hint text at bottom (offset by window position)
    vesa_draw_string(cx - 18 * 8, panel_y + IV_ADJUST_H - 14,
                     "B:switch  Arrow:adjust  ESC:done", 0x6A5890, 0);
}

static void iv_draw_status(int wx, int wy, int ww) {
    // When adjust panel is shown, draw it above the status bar
    if (iv_adjust_active) {
        iv_draw_adjust_panel(wx, wy, ww);
    }

    int wh = g_win_h;
    int status_y = iv_adjust_active ? (wy + wh - IV_STATUS_H - IV_ADJUST_H) : (wy + wh - IV_STATUS_H);

    vesa_fill_rect(wx, status_y, ww, IV_STATUS_H, 0x1A1A2E);
    vesa_draw_rect(wx, status_y, ww, IV_STATUS_H, 0x3A3A5E);

    int ctrl_x = wx + 8;
    int has_bc = (iv_brightness != 0 || iv_contrast != 0);

    if (iv_adjust_active) {
        vesa_draw_string(ctrl_x, status_y + 3,
                         "B:switch  Arrows:adjust  ESC:done", 0x8899BB, 0x1A1A2E);
    } else if (iv_zoom_fit) {
        vesa_draw_string(ctrl_x, status_y + 3,
                         "Z:1:1  B:adjust  ESC:exit",
                         0x8899BB, 0x1A1A2E);
    } else if (iv_needs_scroll) {
        vesa_draw_string(ctrl_x, status_y + 3,
                         "WASD:scroll  Z:fit  B:adjust  ESC:exit",
                         0x8899BB, 0x1A1A2E);
    } else {
        vesa_draw_string(ctrl_x, status_y + 3,
                         "Z:fit  B:adjust  ESC:exit",
                         0x8899BB, 0x1A1A2E);
    }

    // BC indicator when adjustments are active but panel is hidden
    if (has_bc && !iv_adjust_active) {
        char bc_ind[20]; int bi = 0;
        bc_ind[bi++] = 'B'; bc_ind[bi++] = ':';
        int bv = iv_brightness;
        if (bv < 0) { bc_ind[bi++] = '-'; bv = -bv; } else { bc_ind[bi++] = '+'; }
        if (bv >= 100) bc_ind[bi++] = '0' + bv / 100;
        if (bv >= 10)  bc_ind[bi++] = '0' + (bv / 10) % 10;
        bc_ind[bi++] = '0' + bv % 10;
        bc_ind[bi++] = ' ';
        int cv = iv_contrast;
        bc_ind[bi++] = 'C'; bc_ind[bi++] = ':';
        if (cv < 0) { bc_ind[bi++] = '-'; cv = -cv; } else { bc_ind[bi++] = '+'; }
        if (cv >= 100) bc_ind[bi++] = '0' + cv / 100;
        if (cv >= 10)  bc_ind[bi++] = '0' + (cv / 10) % 10;
        bc_ind[bi++] = '0' + cv % 10;
        bc_ind[bi] = '\0';
        int ind_w = bi * 8 + 8;
        vesa_draw_string(wx + ww - ind_w - 8, status_y + 3, bc_ind, 0xFFCC66, 0x1A1A2E);
    }

    // Scroll percentages
    if (iv_needs_scroll && !iv_adjust_active && !iv_zoom_fit) {
        char sp[24]; int si = 0;
        int mx = iv_img_w - iv_content_w; if (mx < 0) mx = 0;
        int my = iv_img_h - iv_content_h; if (my < 0) my = 0;
        int px = mx > 0 ? (iv_scroll_x * 100 / mx) : 0;
        int py = my > 0 ? (iv_scroll_y * 100 / my) : 0;

        sp[si++] = 'H'; sp[si++] = ':';
        if (px >= 100) { sp[si++] = '1'; px -= 100; }
        if (px >= 10)  { sp[si++] = '0' + px / 10; px %= 10; }
        sp[si++] = '0' + px; sp[si++] = '%';
        sp[si++] = ' '; sp[si++] = 'V'; sp[si++] = ':';
        if (py >= 100) { sp[si++] = '1'; py -= 100; }
        if (py >= 10)  { sp[si++] = '0' + py / 10; py %= 10; }
        sp[si++] = '0' + py; sp[si++] = '%';
        sp[si] = '\0';
        vesa_draw_string(wx + ww - 8 - si * 8, status_y + 3, sp, 0x8899BB, 0x1A1A2E);
    }
}

// ─── Public API ──────────────────────────────────────────────────
void imgview_init(int width, int height, const uint8_t* pixels, const char* title) {
    iv_img_w = width;
    iv_img_h = height;
    iv_pixels = pixels;
    iv_scroll_x = 0;
    iv_scroll_y = 0;

    // Copy title
    int ti = 0;
    while (title[ti] && ti < 63) { iv_title[ti] = title[ti]; ti++; }
    iv_title[ti] = '\0';

    // Compute content area
    iv_content_w = (int)vesa.width;
    iv_content_h = (int)vesa.height - IV_TITLE_H - IV_STATUS_H;
    iv_content_x = 0;
    iv_content_y = IV_TITLE_H;

    iv_needs_scroll = (iv_img_w > iv_content_w || iv_img_h > iv_content_h);
    iv_zoom_fit = 1;  // Default to zoom-to-fit for best visual quality
    iv_active = 1;
    // Reset adjustments on new image
    iv_brightness = 0;
    iv_contrast = 0;
    iv_adjust_active = 0;
    iv_adjust_param = 0;
}

int imgview_is_active(void) {
    return iv_active;
}

void imgview_draw(void) {
    if (!iv_active || !iv_pixels) return;

    int pw = (int)vesa.width;

    // ── Window position (preserves dragged position) ──────────────
    int wx = (g_win_x >= 0) ? g_win_x : 16;
    int wy = (g_win_y >= 0) ? g_win_y : 40;
    int ww = pw - 32;
    g_win_x = wx; g_win_y = wy; g_win_w = ww; g_win_h = (int)vesa.height - wy;

    // Full backdrop so the drag trail shows a solid panel
    vesa_fill_rect(wx, wy, ww, (int)vesa.height - wy, 0x0A0A18);

    vesa_fill_rect(wx, wy, ww, IV_TITLE_H, 0x1A1A2E);
    vesa_fill_rect(wx, wy + IV_TITLE_H - 2, ww, 2, 0x3A3A5E);
    draw_close_btn(wx + ww - 32, wy + 6, 0x1A1A2E, "Image Viewer");
    vesa_draw_string(wx + 8, wy + 6, iv_title, 0xCCDDFF, 0x1A1A2E);

    // Zoom indicator (offset by window position)
    if (iv_zoom_fit) {
        vesa_draw_string(wx + ww - 8 - 8 * 8, wy + 6, "[FIT]", 0xFFCC66, 0x1A1A2E);
    }

    // BC indicator in title bar when adjustments are active
    if (iv_brightness != 0 || iv_contrast != 0) {
        char bc_t[16]; int bi = 0;
        bc_t[bi++] = '['; bc_t[bi++] = 'B'; bc_t[bi++] = 'C'; bc_t[bi++] = ']';
        bc_t[bi] = '\0';
        vesa_draw_string(wx + ww - 8 - (iv_zoom_fit ? 2 : 1) * 8 * 8 - 8, wy + 6, bc_t, 0xFFCC66, 0x1A1A2E);
    }

    // Dimensions in title bar
    char db[24]; int di = 0;
    int w = iv_img_w, h = iv_img_h;
    if (w >= 1000) db[di++] = '0' + w / 1000;
    if (w >= 100)  db[di++] = '0' + (w / 100) % 10;
    if (w >= 10)   db[di++] = '0' + (w / 10) % 10;
    db[di++] = '0' + w % 10;
    db[di++] = 'x';
    if (h >= 1000) db[di++] = '0' + h / 1000;
    if (h >= 100)  db[di++] = '0' + (h / 100) % 10;
    if (h >= 10)   db[di++] = '0' + (h / 10) % 10;
    db[di++] = '0' + h % 10;
    db[di++] = ' '; db[di++] = 'p'; db[di++] = 'x';
    db[di] = '\0';
    int dim_x = wx + ww - 8 - di * 8;
    if (iv_brightness != 0 || iv_contrast != 0)
        dim_x -= 76;
    if (iv_zoom_fit)
        dim_x -= 76;
    vesa_draw_string(dim_x, wy + 6, db, 0x88AACC, 0x1A1A2E);

    // ── Content area (offset by window position) ──
    int content_abs_y = wy + IV_TITLE_H;
    int content_abs_h = (int)vesa.height - content_abs_y - 8;
    if (content_abs_h < 60) content_abs_h = 60;
    vesa_fill_rect(0, IV_TITLE_H, pw, iv_content_h, 0x001122); /* keep fullscreen bg */

    if (iv_zoom_fit) {
        int zoom_w = iv_content_w;
        int zoom_h = iv_content_h;
        if (iv_img_w * zoom_h > iv_img_h * zoom_w) {
            zoom_h = iv_img_h * zoom_w / iv_img_w;
        } else {
            zoom_w = iv_img_w * zoom_h / iv_img_h;
        }
        int ox = (iv_content_w - zoom_w) / 2;
        int oy = (iv_content_h - zoom_h) / 2;
        if (ox < 0) ox = 0;
        if (oy < 0) oy = 0;
        iv_content_x = wx + ox;
        iv_content_y = content_abs_y + oy;
        vesa_blit_rgba_scaled_bilinear(wx + ox, content_abs_y + oy,
                                       zoom_w, zoom_h, iv_pixels, iv_img_w, iv_img_h);
        if (iv_brightness != 0 || iv_contrast != 0) {
            vesa_adjust_fb_rect(wx + ox, content_abs_y + oy, zoom_w, zoom_h,
                                iv_brightness, iv_contrast);
        }
    } else if (iv_needs_scroll) {
        iv_content_x = wx;
        iv_content_y = content_abs_y;
        iv_draw_image(iv_scroll_x, iv_scroll_y, iv_content_w, iv_content_h);
    } else {
        int ox = (iv_content_w - iv_img_w) / 2;
        int oy = (iv_content_h - iv_img_h) / 2;
        if (ox < 0) ox = 0;
        if (oy < 0) oy = 0;
        iv_content_x = wx + ox;
        iv_content_y = content_abs_y + oy;
        iv_draw_image(0, 0, iv_img_w, iv_img_h);
    }

    iv_draw_status(wx, wy, ww);
}

int imgview_handle_mouse(int clk_x, int clk_y) {
    if (!iv_active || !iv_pixels) return 0;

    int pw = (int)vesa.width;
    int ph = (int)vesa.height;

    // ── [FIT] indicator in title bar → toggle zoom fit ─────────
    if (iv_zoom_fit) {
        int fit_x = pw - 8 - 8 * 8;  // "[FIT]" = 5 chars × 8px
        if (clk_y >= 6 && clk_y < 22 &&
            clk_x >= fit_x && clk_x < fit_x + 5 * 8) {
            iv_zoom_fit = 0;
            iv_scroll_x = 0;
            iv_scroll_y = 0;
            return 1;
        }
    }

    // ── BC slider panel (when active) ─────────────────────────
    if (iv_adjust_active) {
        int panel_y = ph - IV_STATUS_H - IV_ADJUST_H;
        int cx = pw / 2;
        int bx = cx - BC_SLIDER_W / 2;

        // Brightness slider: track at by = panel_y + BC_SLIDER_Y + 16
        int by = panel_y + BC_SLIDER_Y + 16;
        // Hit area: knob zone + track
        int slider_top = by - BC_KNOB_R;
        int slider_bot = by + 6 + BC_KNOB_R;
        if (clk_y >= slider_top && clk_y < slider_bot &&
            clk_x >= bx && clk_x < bx + BC_SLIDER_W) {
            int new_val = (clk_x - bx) * 200 / BC_SLIDER_W - 100;
            if (new_val < -100) new_val = -100;
            if (new_val > 100) new_val = 100;
            iv_brightness = new_val;
            iv_adjust_param = 0;
            return 1;
        }

        // Contrast slider: 28px below brightness track
        by = panel_y + BC_SLIDER_Y + 16 + 28;
        slider_top = by - BC_KNOB_R;
        slider_bot = by + 6 + BC_KNOB_R;
        if (clk_y >= slider_top && clk_y < slider_bot &&
            clk_x >= bx && clk_x < bx + BC_SLIDER_W) {
            int new_val = (clk_x - bx) * 200 / BC_SLIDER_W - 100;
            if (new_val < -100) new_val = -100;
            if (new_val > 100) new_val = 100;
            iv_contrast = new_val;
            iv_adjust_param = 1;
            return 1;
        }

        // B key → toggle adjust param (click on adjust panel background)
        if (clk_y >= panel_y && clk_y < panel_y + IV_ADJUST_H) {
            iv_adjust_param = !iv_adjust_param;
            return 1;
        }
    }

    // ── B key → toggle adjust panel (click anywhere else in viewer) ─
    if (clk_y < ph - IV_STATUS_H && clk_y > IV_TITLE_H) {
        // Content area click: if not in adjust mode, toggle the panel
        if (!iv_adjust_active) {
            iv_adjust_active = 1;
            iv_adjust_param = 0;
            return 1;
        }
        return 1;  // consume click in content area
    }

    return 0;
}

void imgview_handle_key(char c) {
    if (!iv_active) return;

    if (c == 27) {  // ESC
        if (iv_adjust_active) {
            // ESC in adjust mode: close adjust panel but keep adjustments
            iv_adjust_active = 0;
            return;
        }
        // Reset adjustments on full exit
        iv_brightness = 0;
        iv_contrast = 0;
        iv_adjust_active = 0;
        iv_active = 0;
        iv_pixels = NULL;
        return;
    }

    // ── Brightness/contrast toggle ────────────────────────
    if (c == 'b' || c == 'B') {
        if (iv_adjust_active) {
            // Toggle between brightness and contrast
            iv_adjust_param = !iv_adjust_param;
        } else {
            // Open adjust panel (resets to brightness)
            iv_adjust_active = 1;
            iv_adjust_param = 0;
        }
        return;
    }

    // ── Adjust mode controls ─────────────────────────────
    if (iv_adjust_active) {
        int step = 5;
        // Bigger steps for larger changes
        if (c == 0x48 || c == 'w' || c == 'W') step = 10;  // Up = +10
        if (c == 0x50 || c == 's' || c == 'S') step = 10;  // Down = -10

        if (c == 0x4B || c == 'a' || c == 'A') {  // Left = decrease
            if (iv_adjust_param == 0) {
                iv_brightness -= step;
                if (iv_brightness < -100) iv_brightness = -100;
            } else {
                iv_contrast -= step;
                if (iv_contrast < -100) iv_contrast = -100;
            }
            return;
        }
        if (c == 0x4D || c == 'd' || c == 'D') {  // Right = increase
            if (iv_adjust_param == 0) {
                iv_brightness += step;
                if (iv_brightness > 100) iv_brightness = 100;
            } else {
                iv_contrast += step;
                if (iv_contrast > 100) iv_contrast = 100;
            }
            return;
        }
        // Any other key in adjust mode just refreshes
        return;
    }

    // ── Zoom toggle ──────────────────────────────────────
    if (c == 'z' || c == 'Z') {
        iv_zoom_fit = !iv_zoom_fit;
        // Reset scroll when toggling fit mode
        iv_scroll_x = 0;
        iv_scroll_y = 0;
        return;
    }

    if (iv_zoom_fit) return;  // No scrolling in zoom-to-fit mode

    if (!iv_needs_scroll) return;

    int ox = iv_scroll_x, oy = iv_scroll_y;

    if (c == 0x48 || c == 'w' || c == 'W') { // Up
        iv_scroll_y -= 24;
        if (iv_scroll_y < 0) iv_scroll_y = 0;
    } else if (c == 0x50 || c == 's' || c == 'S') { // Down
        iv_scroll_y += 24;
        int max_y = iv_img_h - iv_content_h;
        if (max_y < 0) max_y = 0;
        if (iv_scroll_y > max_y) iv_scroll_y = max_y;
    } else if (c == 0x4B || c == 'a' || c == 'A') { // Left
        iv_scroll_x -= 24;
        if (iv_scroll_x < 0) iv_scroll_x = 0;
    } else if (c == 0x4D || c == 'd' || c == 'D') { // Right
        iv_scroll_x += 24;
        int max_x = iv_img_w - iv_content_w;
        if (max_x < 0) max_x = 0;
        if (iv_scroll_x > max_x) iv_scroll_x = max_x;
    }

    if (iv_scroll_x != ox || iv_scroll_y != oy) {
        // Redraw — caller calls desktop_draw() after us
    }
}
