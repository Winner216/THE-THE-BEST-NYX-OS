// cpu/launcher.c — Quick Launch panel
// A toggleable overlay (Q key or desktop icon click) listing pinned
// apps down the left side of the screen for fast access.
// Now features mouse hover detection, animated glow effects, and
// a sleek glass-morphism design matching the new Nyx aesthetic.

#include <launcher.h>
#include <desktop.h>
#include <vesa.h>
#include <mouse.h>
#include <speaker.h>
#include <compositor.h>
#include <stdint.h>

// PINNED_COUNT is defined in include/launcher.h

// ─── Pinned apps ──────────────────────────────────────────────────
typedef struct {
    const char* name;
    const char* icon;
    int         launch_id;
    uint32_t    color;   // VESA RGB accent
    uint8_t     vga_col;
} PinnedApp;

static const PinnedApp pinned_apps[] = {
    {"Terminal",   ">_ ", 0,  0x55FF55, 10},
    {"Files",      "[F]", 1,  0x5599FF, 9 },
    {"Editor",     "[E]", 3,  0xFF55FF, 13},
    {"Calculator", "1+1", 8,  0x55FFFF, 11},
    {"Settings",   "[S]", 6,  0xAAAAAA, 7 },
    {"Music",      ">>|", 7,  0xFF9955, 6 },
    {"Games",      "[G]", 9,  0xFFFF55, 14},
    {"Paint",      "[P]", 10, 0xFF5555, 12},
};
#define PINNED_COUNT 8

static int launcher_open = 0;
static int launcher_sel  = -1;   // -1 = no selection (keyboard/mouse)

int launcher_is_open(void) { return launcher_open; }
int launcher_get_sel(void) { return launcher_sel; }
int launcher_get_launch_id(int idx) {
    if (idx < 0 || idx >= PINNED_COUNT) return -1;
    return pinned_apps[idx].launch_id;
}
void launcher_set_open(int v) { launcher_open = v; }
void launcher_set_sel(int v) { launcher_sel = v; }

// ─── VGA fallback ─────────────────────────────────────────────────
#define VGA_WIDTH  80
#define VGA_HEIGHT 25
#define VGA_MEM    ((uint16_t*)0xB8000)
static inline uint16_t lk_entry(char c,uint8_t fg,uint8_t bg){
    return (uint16_t)c|(uint16_t)(fg|bg<<4)<<8;
}
static void lp(int x,int y,char c,uint8_t fg,uint8_t bg){
    if(x<0||x>=VGA_WIDTH||y<0||y>=VGA_HEIGHT)return;
    VGA_MEM[y*VGA_WIDTH+x]=lk_entry(c,fg,bg);
}
static void ls(int x,int y,const char*s,uint8_t fg,uint8_t bg){
    for(int i=0;s[i];i++) lp(x+i,y,s[i],fg,bg);
}
static void lf(int x,int y,int w,int h,char c,uint8_t fg,uint8_t bg){
    for(int dy=0;dy<h;dy++)for(int dx=0;dx<w;dx++) lp(x+dx,y+dy,c,fg,bg);
}

static void vga_launcher_draw(void){
    int px=0, pw=18, py=1, ph=VGA_HEIGHT-1;
    lf(px,py,pw,ph,' ',15,1);
    for(int x=px;x<px+pw;x++){lp(x,py,'-',14,1);lp(x,py+ph-1,'-',14,1);}
    for(int y=py;y<py+ph;y++){lp(px+pw-1,y,'|',14,1);}
    ls(px+1,py,"Quick Launch",14,1);
    int row=py+2;
    for(int i=0;i<PINNED_COUNT;i++){
        int sel=(i==launcher_sel);
        uint8_t fg=sel?0:pinned_apps[i].vga_col, bg=sel?pinned_apps[i].vga_col:1;
        lf(px+1,row,pw-2,1,' ',fg,bg);
        ls(px+1,row,pinned_apps[i].icon,fg,bg);
        ls(px+5,row,pinned_apps[i].name,fg,bg);
        row+=2;
    }
    ls(px+1,py+ph-2,"UP/DN ENTER",7,1);
    ls(px+1,py+ph-1,"Q close",7,1);
}

// ═══════════════════════════════════════════════════════════════════
//  VESA panel — Glassmorphism design with hover glow
// ═══════════════════════════════════════════════════════════════════
#define LK_BG      0x120F2A
#define LK_HEADER  0x1A1440
#define LK_BORDER  0x6A4FBF
#define LK_ROW_SEL 0x352878
#define LK_ROW_HVR 0x2E2068
#define LK_TEXT    0xE0D8FF
#define LK_DIM     0x6858A0
#define LK_HINT    0x504870

extern volatile uint32_t tick;

static int lk_len(const char*s){int i=0;while(s[i])i++;return i;}

static void lk_circ(int cx,int cy,int r,uint32_t c){
    for(int dy=-r;dy<=r;dy++)
        for(int dx=-r;dx<=r;dx++)
            if(dx*dx+dy*dy<=r*r)
                vesa_put_pixel(cx+dx,cy+dy,c);
}

static void vesa_launcher_draw(void){
    int H=(int)vesa.height;
    int px=0, pw=240;
    int py=32, ph=H-py;

    // ── Compositor: blurred translucent backdrop ─────────────────
    // Before drawing the panel, capture the desktop behind it, blur
    // the captured region with a glass-morphism radius, dim it
    // slightly, and write it back.  The panel's own background is
    // then drawn on top with partial transparency, creating the
    // frosted-glass effect.  The compositor auto-disables on low-end
    // hardware (or when toggled off in Settings).
    if (compositor_is_enabled()) {
        // Extend capture region slightly for shadow overflow
        compositor_blur_region(px - 8, py, pw + 14, ph, 4, 60);
    }

    // ── Panel shadow (right side cast shadow) ─────────────────────
    for (int sh = 1; sh <= 6; sh++) {
        int s_alpha = 8 - sh; if (s_alpha < 0) s_alpha = 0;
        uint32_t sc = ((uint32_t)s_alpha << 16) | ((uint32_t)s_alpha << 8) | (uint32_t)s_alpha;
        vesa_vline(px + pw + sh - 1, py, ph, sc);
    }

    // ── Glass background with subtle gradient ─────────────────────
    for (int yy = 0; yy < ph; yy++) {
        int prog = yy * 255 / ph;
        int rr = 0x1A + (0x0E - 0x1A) * prog / 255;
        int gg = 0x12 + (0x0A - 0x12) * prog / 255;
        int bb = 0x36 + (0x22 - 0x36) * prog / 255;
        vesa_hline(px, py+yy, pw, (rr << 16) | (gg << 8) | bb);
    }

    // ── Right border (crisp accent edge) ─────────────────────────
    vesa_vline(px+pw-1, py, ph, 0xB088FF);
    vesa_vline(px+pw-2, py, ph, 0x6A4FBF);
    vesa_vline(px+pw-3, py, ph, 0x3A2E70);

    // ── Header section ───────────────────────────────────────────
    int header_h = 48;
    for (int yy = 0; yy < header_h; yy++) {
        int prog = yy * 255 / header_h;
        int rr = 0x28 + (0x1A - 0x28) * prog / 255;
        int gg = 0x1E + (0x14 - 0x1E) * prog / 255;
        int bb = 0x50 + (0x3A - 0x50) * prog / 255;
        vesa_hline(px, py+yy, pw, (rr << 16) | (gg << 8) | bb);
    }
    vesa_hline(px, py+header_h, pw, 0xB088FF);
    vesa_hline(px, py+header_h+1, pw, 0x3A2E70);

    // Nyx star icon in header
    lk_circ(px+22, py+24, 6, 0xB088FF);
    lk_circ(px+22, py+24, 4, 0xE0D8FF);
    vesa_put_pixel(px+22, py+24, 0xFFFFFF);

    // Title
    vesa_draw_string(px+38, py+16, "Quick Launch", 0xE0D8FF, 0);

    // ── App list — clean rows, glow only on selected item ──────
    int row_h = 56, list_y = py + header_h + 6;

    for (int i = 0; i < PINNED_COUNT; i++) {
        int ry = list_y + i * row_h;
        if (ry + row_h > py + ph - 36) break;

        int sel = (i == launcher_sel);
        uint32_t accent = pinned_apps[i].color;

        if (sel) {
            // ── SELECTED: Strong glow with accent ────────────────
            // Rich background + left accent bar + outer glow ring
            vesa_fill_rect(px+6, ry, pw-14, row_h-4, 0x2A1E50);
            // Left accent bar (solid color)
            vesa_fill_rect(px+6, ry, 4, row_h-4, accent);
            // Bottom edge
            vesa_hline(px+6, ry+row_h-5, pw-14, accent);
            // Top highlight
            vesa_hline(px+6, ry, pw-14, 0x4A3870);
            // Outer glow ring
            vesa_draw_rect(px+6, ry, pw-14, row_h-4, accent);
        } else {
            // ── IDLE / HOVER: clean dark background ───────────────
            int mx = mouse.px, my = mouse.py;
            int in_row = (mx >= px+6 && mx < px+pw-8 &&
                          my >= ry && my < ry+row_h-4);
            if (in_row && launcher_open) {
                // Hover: slightly lighter background
                vesa_fill_rect(px+6, ry, pw-14, row_h-4, 0x1E1640);
                vesa_draw_rect(px+6, ry, pw-14, row_h-4, 0x2E1E58);
            } else {
                // Idle: clean dark, no glow
                vesa_fill_rect(px+8, ry+2, pw-20, row_h-8, 0x0E0B1E);
            }
            // Subtle separator between rows
            if (i < PINNED_COUNT - 1)
                vesa_hline(px+10, ry+row_h-5, pw-20, 0x18142E);
        }

        // ── Icon box ──────────────────────────────────────────────
        int isz = 36;
        int ibx = px + 12, iby = ry + (row_h-4 - isz)/2;

        if (sel) {
            // Glowing icon box on selection
            for (int g = 1; g < 4; g++) {
                uint32_t glow_ring = accent - (uint32_t)g * 0x0A0A0A;
                vesa_draw_rect(ibx-g, iby-g, isz+g*2, isz+g*2, glow_ring);
            }
            vesa_fill_rect(ibx, iby, isz, isz, accent);
            vesa_draw_rect(ibx, iby, isz, isz, 0xFFFFFF);
        } else {
            vesa_fill_rect(ibx, iby, isz, isz, 0x120E28);
            vesa_draw_rect(ibx, iby, isz, isz, 0x2A1E4A);
        }

        // Icon text (centered in box)
        const char* ic = pinned_apps[i].icon;
        int il = lk_len(ic);
        int itx = ibx + (isz - il*8)/2;
        int ity = iby + (isz - 16)/2;
        vesa_draw_string(itx, ity, ic,
            sel ? 0x000000 : pinned_apps[i].color,
            sel ? accent : 0x120E28);

        // ── App name ──────────────────────────────────────────────
        uint32_t name_fg = sel ? 0xFFFFFF : 0xB0A0C8;
        vesa_draw_string(px+60, ry + (row_h-4)/2 - 8,
                         pinned_apps[i].name, name_fg,
                         sel ? 0x2A1E50 : 0);

        // ── F-key hint ──────────────────────────────────────────
        {
            char key_hint[4] = {'F', (char)('1'+i), '\0'};
            uint32_t hint_fg = sel ? 0x6A5A90 : 0x3A2A60;
            uint32_t hint_bg = sel ? 0x2A1E50 : 0;
            vesa_draw_string(px+pw-32, ry + (row_h-4)/2 - 8,
                             key_hint, hint_fg, hint_bg);
        }

        // ── Selection arrow indicator ────────────────────────────
        if (sel) {
            vesa_draw_string(px+6, ry + (row_h-4)/2 - 8, ">", accent, 0);
        }
    }

    // ── Footer ──────────────────────────────────────────────────
    int fy = py+ph-32;
    vesa_hline(px, fy, pw, 0x3A2E70);

    // Animated dot on hint
    int dot_on = (tick / 10) % 2;
    lk_circ(px+16, fy+16, 3, dot_on ? 0xB088FF : 0x3A2E70);

    vesa_draw_string(px+28, fy+8, "Click or ENTER to launch", 0x6858A0, 0);
    vesa_draw_string(px+28, fy+8+16, "F1-F8: quick launch  |  Q/ESC: close", 0x403868, 0);

    vesa_present();
}

// ═══════════════════════════════════════════════════════════════════
//  DRAW
// ═══════════════════════════════════════════════════════════════════
void launcher_draw(void){
    if(!launcher_open) return;
    if(vesa.active){ vesa_launcher_draw(); }
    else            { vga_launcher_draw(); }
}

// ═══════════════════════════════════════════════════════════════════
//  MOUSE HANDLER — hover detection + click-to-launch
// ═══════════════════════════════════════════════════════════════════
// Called from desktop.c's mouse click dispatch. Returns 1 if the
// click was consumed (inside the launcher), 0 if it should fall
// through to other handlers.
int launcher_handle_mouse(int clk_x, int clk_y) {
    if (!launcher_open) return 0;

    int px = 0, pw = 240;
    int py = 32, row_h = 56, header_h = 48;
    int list_y = py + header_h + 6;

    // Check if click is inside the launcher panel
    if (clk_x < px || clk_x >= px + pw ||
        clk_y < py || clk_y >= py + (int)vesa.height - py) {
        // Click outside → close launcher
        launcher_open = 0;
        return 0;
    }

    // Check if click is on an app item
    for (int i = 0; i < PINNED_COUNT; i++) {
        int ry = list_y + i * row_h;
        if (ry + row_h > (int)vesa.height - 68) break;

        if (clk_y >= ry && clk_y < ry + row_h - 4) {
            // Clicked on item i — launch it
            launcher_sel = i;
            int id = pinned_apps[i].launch_id;
            launcher_open = 0;
            desktop_set_status("Launching...");
            desktop_launch(id);
            return 1;
        }
    }

    return 1;  // Click consumed (inside panel but not on an item)
}

// ═══════════════════════════════════════════════════════════════════
//  HOVER UPDATE — called from desktop_draw() to check mouse position
//  and update launcher_sel for highlight rendering
// ═══════════════════════════════════════════════════════════════════
void launcher_update_hover(void) {
    if (!launcher_open) return;

    int px = 0, pw = 240;
    int py = 32, row_h = 56, header_h = 48;
    int list_y = py + header_h + 6;
    int mx = mouse.px, my = mouse.py;

    // Default: no selection
    int new_sel = -1;

    // Check if mouse is inside the panel
    if (mx >= px && mx < px + pw && my >= py) {
        for (int i = 0; i < PINNED_COUNT; i++) {
            int ry = list_y + i * row_h;
            if (ry + row_h > (int)vesa.height - 68) break;
            if (my >= ry && my < ry + row_h - 4) {
                new_sel = i;
                break;
            }
        }
    }

    // Only update selection when mouse is actually over an item.
    // If the mouse is not over any row (new_sel == -1), preserve
    // the current keyboard selection so arrow keys + Enter still
    // work after the heartbeat runs — the hover updater must not
    // overwrite the user's keyboard navigation state.
    if (new_sel >= 0 && new_sel != launcher_sel) {
        launcher_sel = new_sel;
        if (launcher_open && vesa.active) {
            launcher_draw();
        }
    }
}

// ═══════════════════════════════════════════════════════════════════
//  KEYBOARD HANDLER
// ═══════════════════════════════════════════════════════════════════
void launcher_toggle(void){
    launcher_open = !launcher_open;
    if(launcher_open){
        launcher_sel = 0;   // Start with first item selected
        launcher_draw();
    }
}

// ─── Launch a pinned app by index (0-7) ─────────────────────────
// Called from the keyboard ISR when F1-F8 is pressed. If the
// launcher is open, closes it first before launching.
void launcher_launch_pinned(int idx) {
    if (idx < 0 || idx >= PINNED_COUNT) return;
    if (launcher_open) {
        launcher_open = 0;
    }
    int id = pinned_apps[idx].launch_id;
    desktop_set_status("Launching...");
    desktop_launch(id);
}

void launcher_handle_key(char c){
    if(!launcher_open) return;

    if(c=='A'){ launcher_sel=(launcher_sel?launcher_sel-1:PINNED_COUNT-1); launcher_draw(); return; }
    if(c=='B'){ launcher_sel=(launcher_sel<PINNED_COUNT-1)?launcher_sel+1:0; launcher_draw(); return; }

    if(c=='q'||c=='Q'||c==27){
        launcher_open=0;
        return;
    }

    if(c==13 && launcher_sel >= 0){
        int id=pinned_apps[launcher_sel].launch_id;
        launcher_open=0;
        desktop_set_status("Launching...");
        desktop_launch(id);
        return;
    }
}
