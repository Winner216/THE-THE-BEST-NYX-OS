	// cpu/desktop.c
#include <desktop.h>
#include <terminal.h>
#include <shell.h>
#include <keyboard.h>
#include <isr.h>
#include <timezone.h>
#include <rtc.h>
#include <speaker.h>
#include <audio.h>
#include <net.h>
#include <music.h>
#include <calc.h>
#include <auth.h>
#include <compositor.h>
#include <stdint.h>
#include <crt.h>
#include <io.h>
#include <fs.h>
#include <games.h>
#include <editor.h>
#include <power.h>
#include <paint.h>
#include <wallpaper.h>
#include <sysmon.h>
#include <clock.h>
#include <calendar.h>
#include <theme.h>
#include <weather.h>
#include <launcher.h>
#include <applib.h>
#include <pkg.h>
#include <peload.h>
#include <window.h>
#include <screensaver.h>
#include <organizer.h>
#include <security.h>
#include <imgview.h>
#include <vbe.h>
#include <gallery.h>
#include <bmpdec.h>
#include <gifdec.h>
#include <pngdec.h>
#include <jpegdec.h>
#include <svgdec.h>
#include <pdfview.h>
#include <pdftext.h>
#include <video.h>
#include <game.h>
#include <task.h>
#include <browser.h>
#include <htmlview.h>
#include <mouse.h>
#include <store.h>
#include <ata.h>

// ─── VGA (text-mode emulation layer: renders via VESA pixels when ─
// ─── VESA is active, falls back to real VGA text memory otherwise)─
#include <vesa.h>
#include <nic.h>
#include <smbus.h>
#define VGA_WIDTH   80
#define VGA_HEIGHT  25
#define VGA_MEM     ((uint16_t*)0xB8000)

static inline uint8_t make_vga_color(uint8_t fg, uint8_t bg){return fg|bg<<4;}
static inline uint16_t make_vga_entry(char c,uint8_t fg,uint8_t bg){
    return (uint16_t)c|(uint16_t)make_vga_color(fg,bg)<<8;
}

// Standard 16-color VGA palette, mapped to 24-bit RGB for VESA rendering
static const uint32_t vga16_rgb[16] = {
    0x000000, // 0  black
    0x0000AA, // 1  blue
    0x00AA00, // 2  green
    0x00AAAA, // 3  cyan
    0xAA0000, // 4  red
    0xAA00AA, // 5  magenta
    0xAA5500, // 6  brown
    0xAAAAAA, // 7  light grey
    0x555555, // 8  dark grey
    0x5555FF, // 9  light blue
    0x55FF55, // 10 light green
    0x55FFFF, // 11 light cyan
    0xFF5555, // 12 light red
    0xFF55FF, // 13 light magenta
    0xFFFF55, // 14 yellow
    0xFFFFFF, // 15 white
};

// Each VGA text cell maps to an 8x16 pixel block in VESA mode.
// 80 cols * 8px = 640, 25 rows * 16px = 400 -- centered within larger
// VESA resolutions (e.g. 1024x768) with a margin border.
static int vga_emu_offset_x(void){
    return ((int)vesa.width - VGA_WIDTH*8) / 2;
}
static int vga_emu_offset_y(void){
    return ((int)vesa.height - VGA_HEIGHT*16) / 2;
}

static void vga_put(int x,int y,char c,uint8_t fg,uint8_t bg){
    if(x<0||x>=VGA_WIDTH||y<0||y>=VGA_HEIGHT)return;

    if (vesa.active) {
        int ox = vga_emu_offset_x() + x*8;
        int oy = vga_emu_offset_y() + y*16;
        uint32_t fgc = vga16_rgb[fg & 0x0F];
        uint32_t bgc = vga16_rgb[bg & 0x0F];
        vesa_draw_char(ox, oy, c, fgc, bgc);
        return;
    }

    VGA_MEM[y*VGA_WIDTH+x]=make_vga_entry(c,fg,bg);
}
static void vga_puts(int x,int y,const char*s,uint8_t fg,uint8_t bg){
    for(int i=0;s[i];i++)vga_put(x+i,y,s[i],fg,bg);
}
static void vga_fill_row(int y,char c,uint8_t fg,uint8_t bg){
    for(int x=0;x<VGA_WIDTH;x++)vga_put(x,y,c,fg,bg);
}
static void vga_fill_rect(int x,int y,int w,int h,char c,uint8_t fg,uint8_t bg){
    for(int dy=0;dy<h;dy++)
        for(int dx=0;dx<w;dx++)
            vga_put(x+dx,y+dy,c,fg,bg);
}
static int str_len(const char*s){int i=0;while(s[i])i++;return i;}
static void str_copy(char*dst,const char*src,int max){
    int i=0;while(src[i]&&i<max-1){dst[i]=src[i];i++;}dst[i]='\0';
}
static int str_eq(const char*a,const char*b){
    int i=0;while(a[i]&&b[i]&&a[i]==b[i])i++;return a[i]==b[i];
}

// ─── Theme shortcut ───────────────────────────────────────────────
#define TH (theme_get())

// ─── App names & modes for search ────────────────────────────────
const char* app_names[]={
    "Terminal","Files","Editor","AppLib",
    "Trash","Settings","Music","Calc","Games",
    "Paint","Monitor","Clock","Calendar","Weather",
    "Organizer","Security","Gallery","Browser",
    "QuickSettings","Doom","Chess"
};
#define DESKTOP_APP_COUNT 21

// ─── Forward declarations ─────────────────────────────────────────

static void draw_settings_window(void);
static void draw_files_window(void);
static void draw_trash_window(void);
static void draw_vesa_trash_panel(void);
static void draw_vesa_music_panel(void);
static void draw_terminal_window(void);
static void draw_icons(void);
static void draw_file_viewer(int file_idx);
static void move_to_trash(int file_idx);
static void restore_from_trash(int trash_idx);
static void delete_one_from_trash(int trash_idx);
static void empty_trash(void);
static void draw_quicksettings_window(void);
static void draw_users_window(void);
static void do_launch(int icon_idx);
static void u_clear_form(void);

// Forward declaration for the file-open helper (used by both keyboard
// Enter handler and mouse click handler, so file-opens are consistent).
static void files_open_file(int file_idx);

// Declared early: desktop_new_folder()/desktop_new_file() register the
// new file in the desktop icon list and persist it via icon_layout_save().
static void icon_layout_save(void);

// Files app folder navigation helpers (defined after desktop_make_unique).
static void desktop_redraw_no_click(void);
static void files_create_here(const char* base, int is_dir);

// Title bar height (in pixels) for panels with manually-drawn title bars.
// Settings, Music, and Trash draw their own title bar at the window origin
// and push content down by this amount.
#define TB_H 32

// ─── Icon drawing primitives (defined later, used here by context menu)
static void icon_rrect_border(int x,int y,int w,int h,uint32_t c);
static void icon_rrect_border_r(int x,int y,int w,int h,int r,uint32_t c);
static void icon_rrect_grad(int x,int y,int w,int h,uint32_t c1,uint32_t c2);
static void icon_rrect_grad_r(int x,int y,int w,int h,int r,uint32_t c1,uint32_t c2);
static void qs_glyph_bars(int cx, int cy, int filled, uint32_t c);
static void qs_glyph_lock_mini(int cx, int cy, uint32_t c);
static void qs_glyph_check(int cx, int cy, uint32_t c);

// ─── Quick Settings panel (defined later; the heartbeat drag handler
// and the click dispatch use the shared layout so hit boxes match)
typedef struct {
    int wx, wy, ww, wh;      // panel rect
    int pill_w, pill_h;      // battery pill
    int circ_d, circ_r;      // circle action buttons
    int circ_y;              // top row y
    int vol_y, bri_y;        // slider row tops
    int track_x, track_w, track_y, track_h;  // slider track geometry
    int grid_x0, grid_y0;    // quick-action grid origin
    int btn_w, btn_h, grid_gap;
} QsLayout;
static void qs_layout(QsLayout* L);
static void qs_panel_draw(void);
static void qs_drag_handle(void);
static void qs_take_screenshot(void);
static void qs_wifi_flyout_draw(void);
static void qs_wifi_flyout_rect(int* fx, int* fy, int* fw, int* fh);

// ─── Visual feedback ──────────────────────────────────────────────
static char status_msg[64] = "";
static int  status_timer   = 0;
static int  flash_r, flash_g, flash_b, flash_frames = 0;

void desktop_set_status(const char* msg) {
    str_copy(status_msg, msg, 64);
    status_timer = 90; // ~1.5 seconds at 60fps
}

void desktop_vfx_flash(int r, int g, int b, int frames) {
    flash_r = r; flash_g = g; flash_b = b;
    flash_frames = frames;
}

// ─── Desktop "New Folder" / "New File" helpers ───────────────────
// Create entries in the flat user filesystem (filesystem[]). Folders
// are marked by a trailing '/' in the name — the Files app renders
// those rows with a folder glyph and treats them as containers. Names
// get a numeric suffix ("New Folder 2") when the base name is taken.
static int fs_name_exists(const char* name) {
    for (int i = 0; i < MAX_FILES; i++)
        if (filesystem[i].used && str_eq(filesystem[i].name, name))
            return 1;
    return 0;
}

// Fill `out` with the first unique name built from base: "base",
// "base 2", "base 3", ... Appends '/' for folders (is_dir).
static void desktop_make_unique(char* out, int max, const char* prefix, const char* base, int is_dir) {
    int n = 0;
    do {
        char tmp[32];
        int t = 0;
        // Path prefix first ("" at the desktop root, "Folder/" when the
        // Files app creates inside a folder).  Uniqueness is checked on
        // the FULL name so nested files can't collide across levels.
        while (prefix[t] && t < max - 5) { tmp[t] = prefix[t]; t++; }
        int pb = t;   // bytes consumed by the prefix
        while (base[t - pb] && t < max - 5) { tmp[t] = base[t - pb]; t++; }
        if (n > 0) {
            tmp[t++] = ' ';
            int v = n + 1;
            char digits[8]; int d = 0;
            while (v) { digits[d++] = '0' + (v % 10); v /= 10; }
            while (d) tmp[t++] = digits[--d];
        }
        if (is_dir) tmp[t++] = '/';
        tmp[t] = 0;
        str_copy(out, tmp, max);
        n++;
    } while (fs_name_exists(out) && n < 100);
}

// ─── Desktop file icons (created via the desktop right-click menu) ─
// New Folder / New File place a real icon on the desktop (in an empty
// grid slot), like other OSes.  desktop_files[] holds the filesystem
// indices shown; the list persists in the layout sector (v6).
#define MAX_DESKTOP_FILES 16
static int  desktop_files[MAX_DESKTOP_FILES];
static int  desktop_files_count = 0;
static int  desktop_file_selected = -1;   // filesystem idx of the selected file icon
static int  last_click_file  = -1;        // for double-click detection
static int  last_click_file_tick = 0;

static void desktop_file_add(int fi) {
    if (fi < 0 || fi >= MAX_FILES) return;
    for (int i = 0; i < desktop_files_count; i++)
        if (desktop_files[i] == fi) return;
    // If the 16-slot list is full, first compact out entries whose
    // files no longer exist (trashed / deleted).  This keeps the cap
    // from silently swallowing new icons after a mass trash — while
    // still letting a trashed file keep its slot (and its icon) as
    // long as there's room, so restore brings the icon back.
    if (desktop_files_count >= MAX_DESKTOP_FILES) {
        int w = 0;
        for (int r = 0; r < desktop_files_count; r++) {
            int v = desktop_files[r];
            if (v >= 0 && v < MAX_FILES && filesystem[v].used)
                desktop_files[w++] = v;
        }
        desktop_files_count = w;
    }
    if (desktop_files_count < MAX_DESKTOP_FILES)
        desktop_files[desktop_files_count++] = fi;
}

// ─── Double-click tracking ───────────────────────────────────────
static int last_click_tick   = 0;  // tick value of the most recent desktop icon click
static int last_click_icon   = -1; // which icon was clicked (-1 = none)
#define DBL_CLICK_THRESHOLD  18   // ~300ms at 60fps (18 frames)

// ─── Icon grid layout helper ────────────────────────────────
// The grid covers the entire desktop area between the taskbar (36px)
// and the statusbar (28px), edge-to-edge, with 10 columns × 7 rows = 70
// slots. 19 apps fill the first 19 slots by default; the remaining 51
// slots are empty (-1 in icon_order[]), giving users a generous canvas
// to arrange icons anywhere across the full screen.
#define ICON_GRID_COLS 14
#define ICON_GRID_ROWS_MAX 8
#define ICON_GRID_SLOTS (ICON_GRID_COLS * ICON_GRID_ROWS_MAX)  // 112 — max array size

// Runtime grid dimensions — computed from screen height so taller
// displays fit more rows.  Set inside compute_icon_grid() each frame.
int g_grid_rows  = 7;   // exposed to mouse.c for IRQ-time snap preview
int g_grid_slots = 98;   // exposed to mouse.c for IRQ-time snap preview
int g_grid_cols  = ICON_GRID_COLS;  // exposed to mouse.c

#define ICON_TILE_W 64
#define ICON_TILE_H 64
// Corner radius of the desktop tiles — matches the round squircle
// SVGs (rx≈100/448 ≈ 22%) so tiles look round like their icons.
#define ICON_TILE_R 14
#define ICON_LABEL_H 24

// ─── Rubber-band multi-selection ──────────────────────────────────
// When the user drags from EMPTY desktop, a translucent selection box
// follows the cursor (drawn by the mouse IRQ at 200 Hz).  On drop,
// every app whose tile intersects the box gets box_sel[slot]=1 and is
// drawn with a teal ring until the next click clears the selection.
static int box_sel[ICON_GRID_SLOTS];

// Number of currently box-selected icons (slots with box_sel == 1).
// Used by mouse.c's IRQ to decide single-icon vs whole-group drag.
int desktop_sel_count(void) {
    int n = 0;
    for (int i = 0; i < g_grid_slots; i++)
        if (box_sel[i]) n++;
    return n;
}

// 1 if the given grid slot is box-selected.
int desktop_sel_has(int slot) {
    if (slot < 0 || slot >= g_grid_slots) return 0;
    return box_sel[slot] ? 1 : 0;
}

// Translated slot index for a group member under a uniform (dcol,drow)
// grid shift — the target of a group drop.  (s % COLS + dcol) is the new
// column and (s / COLS + drow) the new row; multiplying the row back by
// COLS gives the flat slot index.  Using s % COLS / s / COLS (not bare
// s + dcol) is essential — bare s + dcol would double-count the row term.
static int group_shift_slot(int s, int dcol, int drow) {
    return (s % ICON_GRID_COLS + dcol) +
           (s / ICON_GRID_COLS + drow) * ICON_GRID_COLS;
}

typedef struct {
    int cell_w, cell_h;
    int start_x, start_y;
} IconGrid;

static IconGrid compute_icon_grid(int W, int H) {
    IconGrid g;
    // Full-screen grid: spans edge-to-edge from below the taskbar
    // (36px) to above the statusbar (28px). The COLUMN count is fixed
    // (14), but the ROW count is computed from the available height so
    // taller monitors fit more icons.  Capped at ICON_GRID_ROWS_MAX (8)
    // to stay within the 512-byte layout sector.
    g.cell_w = W / ICON_GRID_COLS;
    if (g.cell_w < ICON_TILE_W + 8) g.cell_w = ICON_TILE_W + 8;
    int avail_h = H - 28 - 28;  // between taskbar (28px) and statusbar (28px)
    int min_cell_h = ICON_TILE_H + ICON_LABEL_H + 4;
    g_grid_rows = avail_h / min_cell_h;
    if (g_grid_rows < 1) g_grid_rows = 1;
    if (g_grid_rows > ICON_GRID_ROWS_MAX) g_grid_rows = ICON_GRID_ROWS_MAX;
    g.cell_h = avail_h / g_grid_rows;
    // Update the global slot count so all draw / hit-test loops are in sync.
    g_grid_slots = ICON_GRID_COLS * g_grid_rows;
    g.start_x = 0;              // flush left edge
    g.start_y = 28;             // flush below taskbar (TASKBAR_H)
    // Expose the computed layout to mouse.c IRQ handler for ghost
    // identification (press-position → grid slot) and snap highlight.
    g_grid_start_x = g.start_x;
    g_grid_start_y = g.start_y;
    g_grid_cell_w  = g.cell_w;
    g_grid_cell_h  = g.cell_h;
    return g;
}

// Pixel position (top-left of the tile) for grid slot `index` (0 = first,
// row-major). The tile is ICON_TILE_W wide and sits centered in the cell.
static inline void icon_grid_slot_pos(const IconGrid* g, int index, int* px, int* py) {
    int col = index % ICON_GRID_COLS, row = index / ICON_GRID_COLS;
    *px = g->start_x + col * g->cell_w + (g->cell_w - ICON_TILE_W) / 2;
    *py = g->start_y + row * g->cell_h;
}

// Find which grid slot contains screen coordinate (mx, my).
// Returns the slot index (0..ICON_GRID_SLOTS-1) or -1 if outside all slots.
// Pass skip_idx >= 0 to skip a particular slot (e.g. the drag source).
static inline int icon_grid_hit_test(const IconGrid* g, int mx, int my, int skip_idx) {
    if (mx < g->start_x || my < g->start_y) return -1;
    int col = (mx - g->start_x) / g->cell_w;
    int row = (my - g->start_y) / g->cell_h;
    if (col >= 0 && col < ICON_GRID_COLS && row >= 0 && row < g_grid_rows) {
        int idx = col + row * ICON_GRID_COLS;
        if (idx != skip_idx) return idx;
    }
    return -1;
}

// ─── State ────────────────────────────────────────────────────────
int current_mode      = MODE_DESKTOP;    int selected_icon     = 0;

// ─── Compositor active-window surface ────────────────────────────
// A single full-screen compositor surface used by whichever app
// window is currently active.  The window draws its body and content
// to this surface, and compositor_composite() blends it onto the
// framebuffer with drop shadows, per-window alpha, and decorations.
// -1 means the surface hasn't been created yet.
static int active_surface = -1;
int selected_file     = 0;
int viewing_file       = -1; // -1 = showing file list; else index into filesystem[] being viewed
// When a file is opened from the Files app in VESA mode, it opens in
// the Editor (MODE_EDITOR) so the user can actually type and save.
// This flag remembers that the editor came FROM Files, so ESC returns
// to the file list instead of dropping to the desktop (a "viewed
// file" should never kick you out of the Files app entirely).
int editor_return_files = 0;
// Same "return to the app you came from" pattern for the Image
// Viewer: opening an image from Files sets this to MODE_FILES, and
// opening one from the Gallery sets MODE_GALLERY, so ESC returns to
// the app that opened the image instead of dropping to the desktop.
// -1 = no origin recorded (return to desktop, the old behavior).
int imgview_return_mode = -1;
// Same origin-tracking for the PDF Viewer: set to MODE_FILES when a
// PDF is opened from the Files app so ESC returns to the file list
// instead of the desktop.  -1 = no origin recorded (return to
// desktop, the old behavior).  (PDFs are only opened via
// files_open_file today, so this is always freshly stamped on entry.)
int pdfview_return_mode = -1;
int video_return_mode      = -1;   // Video player origin (MODE_FILES or -1)
static int files_scroll = 0; // scroll offset into the file-list rows (0 = top)

// ─── Files app folder navigation ─────────────────────────────────
// The filesystem is flat; folders are entries whose name ends in '/',
// and a file "lives inside" a folder when its name starts with that
// folder's name ("Folder/readme.txt").  files_cur_path is the current
// browse location ("" = root, otherwise it always ends in '/').  All
// row math (draw, click, drag, keys) goes through the same helpers so
// the visible list can never drift between input methods.
static char files_cur_path[MAX_FILENAME] = "";

// ─── Files "This PC" view state ────────────────────────────────
// The Files app opens on a Windows-style "This PC" screen: a sidebar
// (Home, Desktop, Documents, Downloads, Music, Pictures, Local Disk,
// Trash) plus a main pane with "Folders" and "Devices and drives"
// sections.  Every entry is interconnected with the real flat
// filesystem — Desktop shows the actual desktop icons
// (desktop_files[]), the four document folders live at
// "Documents/"/"Downloads/"/"Music/"/"Pictures/" and Local Disk
// (C:) is the filesystem root.  All row/tile math is shared between
// draw, click and drag so the views can never drift.
static int files_home_view = 1;     // 1 = showing the This PC sections
static int files_desktop_view = 0;  // 1 = listing the desktop icons

// ─── Files rename mode (F2) ─────────────────────────────────────
static int  rename_mode = 0;       // 1 = inline rename active
static int  rename_file_idx = -1;  // filesystem[] index being renamed
static char rename_buf[MAX_FILENAME]; // new name being typed
static int  rename_pos = 0;        // cursor position in rename_buf
static char rename_prefix[MAX_FILENAME]; // dir part of the entry ("Documents/")
static int  rename_is_dir = 0;     // entry being renamed is a folder

#define FILES_SB_W 170              // sidebar width — draw/click/drag share this
#define FILES_SB_LABEL_H 18
#define FILES_SB_ITEM_H 26
#define FSI_HOME 0
#define FSI_DESKTOP 1
#define FSI_DOCS 2
#define FSI_DL 3
#define FSI_MUSIC 4
#define FSI_PICS 5
#define FSI_DRIVE 6
#define FSI_TRASH 7
#define FSI_COUNT 8
#define FILES_TILE_W 108
#define FILES_TILE_H 76
#define FILES_TILE_GAP 12
#define FILES_TILE_PER_ROW 3
#define FILES_TILE_R 10            // home-tile corner radius (glass body)

// ─── Local Disk capacity-bar animation ────────────────────────────
// The drive tile's usage bar glides toward the real percentage
// instead of jumping when files are created/deleted.  drive_pct_target
// is the true used % (recomputed per frame); drive_pct_disp is the
// animated value.  desktop_heartbeat() advances disp toward target on
// the PIT clock (~18 Hz) with an ease-out curve, redrawing only while
// it's still moving; draw_files_window() just renders disp, so the
// fill and its label stay in lockstep.
#define DRIVE_PCT_EASE 25          // % of remaining gap closed per tick
static int drive_pct_disp = -1;    // animated fill %, -1 = not initialized
static int drive_pct_target = 0;   // true used %

// 1 if name starts with prefix (prefix may be "").
static int files_name_has_prefix(const char* name, const char* prefix) {
    int k = 0;
    while (prefix[k] && name[k] && prefix[k] == name[k]) k++;
    return prefix[k] == '\0';
}

// 1 if entry fi is visible at the current level: its name starts with
// files_cur_path and the remainder contains no '/' except a trailing
// one (a file or a direct subfolder, never a grandchild).  The folder
// itself (name == path) is never listed inside itself.
static int files_entry_visible(int fi) {
    const char* n = filesystem[fi].name;
    const char* p = files_cur_path;
    int k = 0;
    while (p[k] && n[k] && p[k] == n[k]) k++;
    if (p[k]) return 0;                 // path isn't a prefix
    if (n[k] == '\0') return 0;         // exact match = the folder itself
    while (n[k]) {
        if (n[k] == '/') return (n[k+1] == '\0');   // only trailing '/'
        k++;
    }
    return 1;
}

// Visible row count.  The This PC overview shows tiles, not rows; the
// Desktop location lists the real desktop icons (plus the up-row back
// to This PC); folder views count one up-row when inside a folder.
static int files_visible_count(void) {
    if (files_home_view) return 0;                       // tiles instead
    if (files_desktop_view) return desktop_files_count + 1;  // + up row
    int n = files_cur_path[0] ? 1 : 0;
    for (int i = 0; i < MAX_FILES; i++)
        if (filesystem[i].used && files_entry_visible(i)) n++;
    return n;
}

// Map visible row → filesystem index; -1 means the "up" row.  In the
// Desktop view the rows ARE desktop_files[] — the real desktop icons.
static int files_visible_row(int row, int* out_fi) {
    if (files_desktop_view) {
        if (row == 0) { *out_fi = -1; return 1; }        // up row → This PC
        if (row - 1 < desktop_files_count) { *out_fi = desktop_files[row - 1]; return 1; }
        return 0;
    }
    int r = 0;
    if (files_cur_path[0]) {
        if (row == 0) { *out_fi = -1; return 1; }
        r = 1;
    }
    for (int i = 0; i < MAX_FILES; i++) {
        if (!filesystem[i].used || !files_entry_visible(i)) continue;
        if (r == row) { *out_fi = i; return 1; }
        r++;
    }
    return 0;
}

// Display label for a row: the name with the current path prefix
// stripped (folders keep their trailing '/' here; callers hide it).
static void files_short_name(const char* full, char* out) {
    int k = 0; while (files_cur_path[k]) k++;
    int i = k, o = 0;
    while (full[i] && o < MAX_FILENAME - 1) out[o++] = full[i++];
    out[o] = '\0';
}

// Number of real items (files + folders) inside the current location,
// excluding the synthetic up-row.  Feeds the "N items" breadcrumb line.
static int files_item_count(void) {
    if (files_home_view) return 0;
    if (files_desktop_view) return desktop_files_count;
    int n = 0;
    for (int i = 0; i < MAX_FILES; i++)
        if (filesystem[i].used && files_entry_visible(i)) n++;
    return n;
}

// "N items" / "N item" label for the breadcrumb count line.
static void files_item_label(int n, char* out) {
    char d[8]; int k = 0;
    int nn = n;
    if (nn == 0) d[k++] = '0';
    while (nn > 0 && k < 6) { d[k++] = '0' + (nn % 10); nn /= 10; }
    int o = 0;
    while (k > 0) out[o++] = d[--k];
    out[o++] = ' '; out[o++] = 'i'; out[o++] = 't'; out[o++] = 'e'; out[o++] = 'm';
    if (n != 1) out[o++] = 's';
    out[o] = 0;
}

// True Local Disk usage percentage (used bytes / total capacity),
// clamped to [0,100].  Shared by the heartbeat (tween target) and
// draw_files_window (initialization + display), so the two can never
// disagree about what the bar means.
static int files_drive_pct(void) {
    int used_kb = 0, cap_kb = (256 * 1024);  // 256 MiB heap budget
    for (int i = 0; i < MAX_FILES; i++) {
        if (!filesystem[i].used) continue;
        int sz = filesystem[i].size;
        if (sz <= 0 && filesystem[i].content) { sz = 0; while (filesystem[i].content[sz]) sz++; }
        used_kb += sz / 1024;
    }
    if (used_kb > cap_kb) used_kb = cap_kb;
    return cap_kb ? used_kb * 100 / cap_kb : 0;
}

// Y of the first content row in the Files window (VESA).  Must match
// draw_files_window()'s header layout exactly:
//   home / Desktop / folder views: 14 (path) + 26 + 14 = 54
//   drive root adds sysdirs: 4*24 + 6 + 16 = 118 → 172
static int files_rows_top(int win_y) {
    if (files_home_view || files_desktop_view || files_cur_path[0]) return win_y + 54;
    return win_y + 172;
}

// How many file-list rows fit between the content top (files_rows_top)
// and the bottom hint bar (win_y+win_h-26, with an 8px hline above).
// Rows are 26px tall with a 4px gap (30px pitch).  Mirrors the Trash
// window's scroll math so draw / click / drag / keyboard all agree.
static int files_visible_rows(int win_y, int win_h) {
    int top    = files_rows_top(win_y);
    int bottom = win_y + win_h - 26 - 8;   // above the hint-bar hline
    int avail  = bottom - top - 4;
    if (avail < 30) avail = 30;
    return avail / 30;
}

// Enter a folder: browse its contents in the Files app.  Used by both
// the Files app rows and desktop folder icons (via files_open_file).
static void files_enter_folder(int fi) {
    str_copy(files_cur_path, filesystem[fi].name, MAX_FILENAME);
    files_home_view = 0;
    files_desktop_view = 0;
    selected_file = 0;
    files_scroll = 0;
    viewing_file = -1;
    current_mode = MODE_FILES;
    desktop_redraw_no_click();
}

// Go up one level (root → no-op).
static void files_go_up(void) {
    if (files_desktop_view) { files_desktop_view = 0; files_home_view = 1; files_cur_path[0] = 0; selected_file = 0; files_scroll = 0; desktop_redraw_no_click(); return; }
    int plen = 0; while (files_cur_path[plen]) plen++;
    if (plen == 0) return;                  // already at root
    int e = plen - 1;                       // trailing '/'
    int s = e - 1;
    while (s >= 0 && files_cur_path[s] != '/') s--;
    if (s <= 0) files_cur_path[0] = '\0';   // back to root
    else files_cur_path[s] = '\0';          // parent (keeps its '/')
    selected_file = 0;
    files_scroll = 0;
    desktop_redraw_no_click();
}

// Create a new file/folder INSIDE the folder being browsed (root when
// files_cur_path is empty).  Root-level files also become desktop
// icons, matching the right-click menu behaviour.
static void files_create_here(const char* base, int is_dir) {
    int slot = -1;
    for (int i = 0; i < MAX_FILES; i++) if (!filesystem[i].used) { slot = i; break; }
    if (slot < 0) { desktop_set_status("Filesystem full!"); return; }
    char nm[MAX_FILENAME];
    desktop_make_unique(nm, MAX_FILENAME, files_cur_path, base, is_dir);
    str_copy(filesystem[slot].name, nm, MAX_FILENAME);
    file_content_alloc(slot, 1);
    if (filesystem[slot].content) filesystem[slot].content[0] = '\0';
    filesystem[slot].size = 0;
    filesystem[slot].used = 1;
    file_count++;
    if (!files_cur_path[0]) desktop_file_add(slot);
    icon_layout_save();
    fs_save_to_disk();
    desktop_set_status(is_dir ? "Folder created" : "File created");
    // The new item is the LAST row — scroll down so it's visible.
    files_scroll = 0x7FFFFFFF;   // clamped by draw to (nrows - visible)
    selected_file = 0;
    desktop_redraw_no_click();
}

// Desktop right-click menu creators — always target the ROOT, even if
// a stale files_cur_path lingers from a previous Files session.
// (Defined after the folder-nav block so they can call files_create_here.)
static void desktop_new_folder(void) {
    files_cur_path[0] = '\0';   // desktop context always creates at root
    files_create_here("New Folder", 1);
}

static void desktop_new_file(void) {
    files_cur_path[0] = '\0';   // desktop context always creates at root
    files_create_here("New File.txt", 0);
}

// ─── Files "This PC" helpers (shared by draw + click + drag) ──────
// Sidebar item rect (items 0..5 under "Quick access", 6..7 under
// "Devices and drives").  The rects are computed identically in draw
// and click so hover/selection and hit-testing can't drift.
static void files_sb_item_rect(int win_x, int win_y, int idx,
                               int* rx, int* ry, int* rw, int* rh) {
    int sb_x = win_x + 1;
    *rx = sb_x + 4;
    *rw = FILES_SB_W - 8;
    *rh = FILES_SB_ITEM_H;
    int items_top = win_y + 36 + 8 + FILES_SB_LABEL_H;   // below title bar
    if (idx < 6) {
        *ry = items_top + idx * FILES_SB_ITEM_H;
    } else {
        // second section label + 8px gap before the devices items
        *ry = items_top + 6 * FILES_SB_ITEM_H + 8 + FILES_SB_LABEL_H
              + (idx - 6) * FILES_SB_ITEM_H;
    }
}

// Y of the This PC home tile grid (Folders section, 3 tiles per row).
static void files_home_tile_rect(int win_x, int win_y, int idx,
                                 int* rx, int* ry) {
    int content_left = win_x + FILES_SB_W + 15;
    int base_y = win_y + 14 + 26 + 14 + 22 + 8;  // below "Folders" label
    int col = idx % FILES_TILE_PER_ROW, row = idx / FILES_TILE_PER_ROW;
    *rx = content_left + col * (FILES_TILE_W + FILES_TILE_GAP);
    *ry = base_y + row * (FILES_TILE_H + FILES_TILE_GAP);
}

// Local Disk (C:) tile — sits in the "Devices and drives" section.
static void files_home_drive_rect(int win_x, int win_y, int* rx, int* ry) {
    int content_left = win_x + FILES_SB_W + 15;
    int dev_y = win_y + 14 + 26 + 14 + 22 + 8
                + 2 * (FILES_TILE_H + FILES_TILE_GAP) + 16 + 22;
    *rx = content_left;
    *ry = dev_y;
}

// Which sidebar item matches the current browse location (-1 = none).
static int files_sb_active(void) {
    if (files_home_view) return FSI_HOME;
    if (files_desktop_view) return FSI_DESKTOP;
    if (!files_cur_path[0]) return FSI_DRIVE;
    if (str_eq(files_cur_path, "Documents/")) return FSI_DOCS;
    if (str_eq(files_cur_path, "Downloads/")) return FSI_DL;
    if (str_eq(files_cur_path, "Music/"))     return FSI_MUSIC;
    if (str_eq(files_cur_path, "Pictures/"))  return FSI_PICS;
    return -1;
}

// Make sure a standard folder (e.g. "Documents/") exists in the flat
// filesystem so files created inside it are real, browsable entries.
static void files_ensure_dir(const char* dir) {
    for (int i = 0; i < MAX_FILES; i++)
        if (filesystem[i].used && str_eq(filesystem[i].name, dir)) return;
    int slot = -1;
    for (int i = 0; i < MAX_FILES; i++) if (!filesystem[i].used) { slot = i; break; }
    if (slot < 0) return;
    str_copy(filesystem[slot].name, dir, MAX_FILENAME);
    file_content_alloc(slot, 1);
    if (filesystem[slot].content) filesystem[slot].content[0] = 0;
    filesystem[slot].size = 0;
    filesystem[slot].used = 1;
    file_count++;
    fs_save_to_disk();
}

// Navigate to a sidebar location.
static void files_sb_nav(int item) {
    files_desktop_view = 0;
    files_home_view = 0;
    switch (item) {
    case FSI_HOME:    files_home_view = 1; files_cur_path[0] = 0; break;
    case FSI_DESKTOP: files_desktop_view = 1; files_cur_path[0] = 0; break;
    case FSI_DOCS:    files_ensure_dir("Documents/"); str_copy(files_cur_path, "Documents/", MAX_FILENAME); break;
    case FSI_DL:      files_ensure_dir("Downloads/"); str_copy(files_cur_path, "Downloads/", MAX_FILENAME); break;
    case FSI_MUSIC:   files_ensure_dir("Music/");     str_copy(files_cur_path, "Music/", MAX_FILENAME); break;
    case FSI_PICS:    files_ensure_dir("Pictures/");  str_copy(files_cur_path, "Pictures/", MAX_FILENAME); break;
    case FSI_DRIVE:   files_cur_path[0] = 0; break;
    case FSI_TRASH:   current_mode = MODE_TRASH; break;
    }
    selected_file = 0;
    files_scroll = 0;
    viewing_file = -1;
    desktop_redraw_no_click();
}

// 16×16 pixel-art glyphs for the sidebar items / home tiles.
static void files_sb_glyph(int x, int y, int item, uint32_t c) {
    switch (item) {
    case FSI_HOME:      // house
        vesa_hline(x+4, y, 8, c);
        vesa_hline(x+3, y+1, 10, c);
        vesa_hline(x+2, y+2, 12, c);
        vesa_fill_rect(x+4, y+4, 8, 9, c);
        vesa_fill_rect(x+6, y+8, 4, 5, 0x160E30);
        break;
    case FSI_DESKTOP:   // monitor
        vesa_draw_rect(x+1, y+2, 14, 9, c);
        vesa_fill_rect(x+6, y+12, 4, 2, c);
        vesa_fill_rect(x+4, y+14, 8, 2, c);
        vesa_fill_rect(x+3, y+4, 10, 5, 0x160E30);
        break;
    case FSI_DOCS:      // page
        vesa_fill_rect(x+5, y+1, 7, 13, 0x160E30);
        vesa_draw_rect(x+5, y+1, 7, 13, c);
        vesa_hline(x+7, y+4, 3, c);
        vesa_hline(x+7, y+7, 3, c);
        vesa_hline(x+7, y+10, 2, c);
        break;
    case FSI_DL:        // down-arrow into a tray
        vesa_vline(x+7, y+1, 7, c);
        vesa_hline(x+5, y+6, 6, c);
        vesa_hline(x+6, y+8, 4, c);
        vesa_fill_rect(x+3, y+11, 10, 3, c);
        break;
    case FSI_MUSIC:     // note
        vesa_fill_rect(x+5, y+3, 2, 9, c);
        vesa_fill_rect(x+7, y+3, 6, 2, c);
        vesa_fill_rect(x+7, y+5, 2, 2, c);
        vesa_draw_rect(x+3, y+10, 5, 4, c);
        vesa_draw_rect(x+9, y+11, 4, 4, c);
        break;
    case FSI_PICS:      // picture frame
        vesa_draw_rect(x+2, y+2, 12, 12, c);
        vesa_fill_rect(x+4, y+4, 8, 8, 0x9ED3F5);
        vesa_fill_rect(x+6, y+5, 2, 2, 0xFFD166);
        vesa_fill_rect(x+4, y+9, 8, 3, 0x6BBF6B);
        break;
    case FSI_DRIVE:     // hard disk
        vesa_draw_rect(x+1, y+4, 14, 8, c);
        vesa_hline(x+4, y+7, 8, c);
        vesa_fill_rect(x+11, y+5, 2, 2, 0x55FF55);
        break;
    case FSI_TRASH:     // bin
        vesa_hline(x+3, y+1, 10, c);
        vesa_fill_rect(x+5, y+3, 6, 11, c);
        vesa_fill_rect(x+7, y+5, 2, 7, 0x160E30);
        break;
    default: break;
    }
}

// 32×32 pixel-art glyphs for the This PC home tiles.  Same motifs as
// files_sb_glyph() but drawn larger so the Folders / Devices sections
// look like a real app grid.
static void files_tile_glyph(int x, int y, int item, uint32_t c) {
    switch (item) {
    case FSI_DESKTOP:   // monitor
        vesa_draw_rect(x+4, y+4, 24, 15, c);
        vesa_fill_rect(x+6, y+6, 20, 10, 0x160E30);
        vesa_fill_rect(x+9, y+8, 14, 6, 0x6A4FBF);
        vesa_fill_rect(x+13, y+20, 6, 3, c);
        vesa_fill_rect(x+9, y+23, 14, 3, c);
        break;
    case FSI_DOCS:      // page
        vesa_fill_rect(x+11, y+3, 12, 26, 0x160E30);
        vesa_draw_rect(x+11, y+3, 12, 26, c);
        vesa_hline(x+14, y+9, 6, c);
        vesa_hline(x+14, y+14, 6, c);
        vesa_hline(x+14, y+19, 4, c);
        break;
    case FSI_DL:        // down-arrow into a tray
        vesa_vline(x+15, y+3, 14, c);
        vesa_hline(x+11, y+13, 10, c);
        vesa_hline(x+13, y+17, 6, c);
        vesa_fill_rect(x+7, y+22, 18, 6, c);
        break;
    case FSI_MUSIC:     // double eighth note (beamed)
        // Beam connecting the stems at the top
        vesa_fill_rect(x+9, y+4, 14, 4, c);
        // Stems down from the beam
        vesa_fill_rect(x+10, y+6, 3, 14, c);
        vesa_fill_rect(x+20, y+6, 3, 14, c);
        // Left note head — filled oval
        vesa_hline(x+3, y+20, 8, c);
        vesa_hline(x+2, y+21, 10, c);
        vesa_hline(x+2, y+22, 10, c);
        vesa_hline(x+3, y+23, 8, c);
        vesa_hline(x+4, y+24, 6, c);
        // Right note head — filled oval
        vesa_hline(x+13, y+21, 10, c);
        vesa_hline(x+13, y+22, 10, c);
        vesa_hline(x+14, y+23, 8, c);
        vesa_hline(x+15, y+24, 6, c);
        break;
    case FSI_PICS:      // picture frame
        vesa_draw_rect(x+4, y+4, 24, 24, c);
        vesa_fill_rect(x+7, y+7, 18, 18, 0x9ED3F5);
        vesa_fill_rect(x+12, y+10, 4, 4, 0xFFD166);
        vesa_fill_rect(x+7, y+19, 18, 6, 0x6BBF6B);
        break;
    case FSI_DRIVE:     // hard disk with C badge
        // Dark drive face, light body border
        vesa_fill_rect(x+4, y+9, 24, 14, 0x160E30);
        vesa_draw_rect(x+4, y+9, 24, 14, c);
        // Drive letter C
        vesa_draw_string(x+12, y+11, "C", c, 0x160E30);
        // Platter line
        vesa_hline(x+8, y+19, 12, c);
        // Activity LED (green)
        vesa_fill_rect(x+24, y+10, 2, 2, 0x55FF55);
        break;
    default: break;
    }
}

int music_tab          = 0;  // 0 = Playlist, 1 = Albums
int settings_selected = 0;
int settings_section  = -1;
int sound_volume      = 75;

// ─── Quick Settings panel state ───────────────────────────────────
static int qs_brightness  = 100;  // 0-100, drives wallpaper brightness
static int qs_bluetooth   = 0;    // 0/1 toggles (visual state)
static int qs_nightlight  = 0;
static int qs_airplane    = 0;
static int qs_power_mode  = 0;    // 0 Balanced, 1 Performance, 2 Eco

// ─── Wi-Fi flyout state ───────────────────────────────────────────
// A Windows-style network picker attached to the right of the panel.
// The kernel has no wireless hardware, so the picker shows a scan of
// the simulated home network list; selecting a row updates the
// connected network shown on the Wi-Fi quick-action button.
#define QS_WIFI_NETS 6
static const char* qs_wifi_names[QS_WIFI_NETS] = {
    "Winnerbek", "FiberHome-5G", "NETGEAR67",
    "TP-Link_2G", "Starbucks WiFi", "Galaxy-A54"
};
static const int  qs_wifi_signal[QS_WIFI_NETS] = { 4, 4, 3, 3, 2, 1 };  // 1-4 bars
static const int  qs_wifi_sec[QS_WIFI_NETS]    = { 0, 1, 1, 1, 0, 1 };  // lock icon
static int qs_wifi_open = 0;    // flyout visible
static int qs_wifi_on   = 1;    // Wi-Fi radio
static int qs_wifi_scan = 0;    // ticks of "scanning" remaining (50 Hz)
static int qs_wifi_cur  = 0;    // index of the connected network

// ─── Dirty cursor tracking ────────────────────────────────────────
// Cursor save/restore buffer and functions live in mouse.c so the
// IRQ handler can update the cursor at the full 200 Hz PS/2 rate.
// desktop.c uses the extern declarations from include/mouse.h.

// ─── Window dragging state ─────────────────────────────────────────
// g_win_x/y = current active window's top-left corner (-1 = use default
// position for the current mode). Updated by draw functions after they
// compute their layout, and by the window-drag handler on mouse move.
int g_win_x = -1;
int g_win_y = -1;
int g_win_w = 0;
int g_win_h = 0;
static int g_win_off_x = 0;  // mouse click offset within title bar
static int g_win_off_y = 0;

// ─── Drag-and-drop state ───────────────────────────────────────────
static int drag_source_type = DRAG_NONE;  // DRAG_ICON, DRAG_FILE, or DRAG_WINDOW
static int drag_source_idx  = -1;         // which icon or file index

// ─── Icon grid layout (exposed to mouse.c for IRQ-time ghost) ────
int g_grid_start_x = 0, g_grid_start_y = 0;
int g_grid_cell_w = 0, g_grid_cell_h = 0;

// ─── Drag ghost state (set/cleared by desktop.c, read by mouse.c ─
// ─── IRQ handler to draw the lightweight ghost at 200 Hz) ───────
int g_ghost_app_idx = -1;  // -1 = no ghost, 0-18 = app index for icon + label
const char* g_ghost_label = 0;  // points into app_names[g_ghost_app_idx]; 0 = none

// ─── Drag app info (shared with mouse.c IRQ handler for snap preview) ─
int g_drag_app_idx   = -1;          // app index (0-18) of icon being dragged
const char* g_drag_app_name = 0;    // app name string for the dragged icon

// ─── Squish-drop animation ────────────────────────────────────────
// When an icon lands in a new slot, a brief squish animation plays on
// the target slot to give the user visual confirmation. anim_slot is
// the grid position (-1 = none), anim_frame counts down to 0.
#define ANIM_FRAMES 2
static int anim_slot  = -1;
static int anim_frame = 0;

// ─── Spring-loaded snap-back animation ────────────────────────────
// When an icon is dropped on or near its own slot, it plays a brief
// spring-back animation: the ghost flies from the drop position back
// to the source tile center with a subtle overshoot (easing table in
// 1/256 units).  The icon_order[] array is NOT modified — the icon
// was already at its slot; we're just animating the visual.
#define SNAP_FRAMES 10
static int  snap_active = 0;      // 1 = animation in progress
static int  snap_slot   = -1;     // which grid slot is being snapped
static int  snap_frame  = 0;      // 0 → SNAP_FRAMES (counts up)
static int  snap_sx, snap_sy;     // start (drop point)
static int  snap_ex, snap_ey;     // end (target tile center)

// Easing table: interpolation factor (0-256) for frame 0..SNAP_FRAMES.
// Starts at 0 (drop position), ends at 256 (slot center), overshoots
// to ~270 mid-animation for a juicy spring feel.
static const int snap_ease[SNAP_FRAMES + 1] = {
    0,    // f0:  at drop position
    8,    // f1
    28,   // f2
    65,   // f3
    120,  // f4:  halfway there
    190,  // f5
    250,  // f6:  almost home
    270,  // f7:  overshoot
    262,  // f8:  come back
    258,  // f9:  nearly settled
    256,  // f10: settled
};

// ─── Slide-in animation on drop (200 Hz sub-tick ghost) ──────────
// When the user releases an icon over a valid target slot, the ghost
// slides from its last position g_ghost_filter_px/py to the target
// grid centre with quadratic ease-out over ~200 ms, driven by a
// 0-255 sub-tick interpolation clock (mag_fraction).  The PS/2 IRQ
// advances the clock per packet at 200 Hz; the heartbeat advances it
// as an idle fallback (only when no packets arrived since the last
// tick — mag_pkt_count vs mag_pkt_last_seen) and is the SINGLE
// renderer, drawing the ghost post-present via
// mouse_flying_ghost_render().  The heartbeat reads mag_commit_pending
// to commit icon_order[] once the animation is done.
int  mag_active   = 0;     // 1 = animation in progress
int  mag_fraction = 0;     // 0 → MAG_FRACTION_MAX (sub-tick clock)
int  mag_pkt_count = 0;    // packets seen while anim active (IRQ advances)
static int mag_pkt_last_seen = 0;  // heartbeat's last-seen count (stall detect)
int  mag_sx, mag_sy;       // start (ghost position at drop)
int  mag_ex, mag_ey;       // end (target tile center)
int  mag_src_slot;         // source grid slot
int  mag_tgt_slot;         // target grid slot
int  mag_src_app;          // app index at source slot
int  mag_tgt_type;         // 0 = move to empty, 1 = swap
int  mag_tgt_occupied;     // app index at target (-1 if empty)
int  mag_commit_pending = 0;  // 1 when animation done, commit swap
// (MAG_STALL_TICKS / mag_last_advance_tick were part of a previous
//  stall-recovery design where the IRQ drove mag_frame at 200 Hz —
//  now the sub-tick clock advances from both sides with packet-count
//  stall detection, so these are unused.)
uint32_t mag_last_advance_tick = 0;

// ─── Context menu state ────────────────────────────────────────────
#define CTX_MAX_ITEMS 8
// Dark surface colors — shared by the panel fill AND the label bg.
// vesa_draw_char fills every non-glyph pixel with bg, so the label bg
// MUST equal the surface under it (never 0, which would paint black
// blocks). Keeping both in one place prevents silent seams.
#define CTX_PANEL_BG   0x1E1438   // flat dark-purple panel + non-hover label bg
#define CTX_HOVER_BG   0x3A2E70   // purple hover fill + hover label bg
static int  ctx_active   = 0;  // 1 = context menu is open
static int  ctx_x, ctx_y;      // top-left corner of the menu panel
static int  ctx_w, ctx_h;      // computed panel dimensions
static int  ctx_count    = 0;  // number of items in the menu
static int  ctx_sel      = -1; // hovered item index, -1 = none
static char ctx_labels[CTX_MAX_ITEMS][20];
static int  ctx_actions[CTX_MAX_ITEMS];   // action ID per item
static int  ctx_sep_after[CTX_MAX_ITEMS]; // 1 = draw a group separator below this item
static int  ctx_payload_type; // 0 = desktop icon, 1 = sidebar trash, 2 = desktop
static int  ctx_payload_idx;  // icon grid position or 0

#define CTX_ACT_OPEN        1
#define CTX_ACT_RENAME      2
#define CTX_ACT_PROPERTIES  3
#define CTX_ACT_OPEN_TRASH  4
#define CTX_ACT_EMPTY_TRASH 5
#define CTX_ACT_NEW_FOLDER  6
#define CTX_ACT_NEW_FILE    7
#define CTX_ACT_REFRESH     8
#define CTX_ACT_WALLPAPER   9
#define CTX_ACT_RESET_LAYOUT 10
#define CTX_ACT_DELETE_FILE 11   // remove a desktop file/folder icon

static void ctxmenu_init(int px, int py, int ptype, int pidx) {
    ctx_x = px;
    ctx_y = py;
    ctx_payload_type = ptype;
    ctx_payload_idx = pidx;
    ctx_sel = -1;
    ctx_count = 0;
    for (int i = 0; i < CTX_MAX_ITEMS; i++) ctx_sep_after[i] = 0;

    if (ptype == 0) {
        // Desktop icon context menu
        if (ctx_count < CTX_MAX_ITEMS) { ctx_actions[ctx_count] = CTX_ACT_OPEN;
            int i=0; const char*s="Open"; for(i=0;s[i];i++)ctx_labels[ctx_count][i]=s[i]; ctx_labels[ctx_count][i]=0; ctx_count++; }
        if (ctx_count < CTX_MAX_ITEMS) { ctx_actions[ctx_count] = CTX_ACT_RENAME;
            int i=0; const char*s="Rename"; for(i=0;s[i];i++)ctx_labels[ctx_count][i]=s[i]; ctx_labels[ctx_count][i]=0; ctx_count++; }
        if (ctx_count < CTX_MAX_ITEMS) { ctx_actions[ctx_count] = CTX_ACT_PROPERTIES;
            int i=0; const char*s="Properties"; for(i=0;s[i];i++)ctx_labels[ctx_count][i]=s[i]; ctx_labels[ctx_count][i]=0; ctx_count++; }
    } else if (ptype == 1) {
        // Files sidebar Trash context menu
        if (ctx_count < CTX_MAX_ITEMS) { ctx_actions[ctx_count] = CTX_ACT_OPEN_TRASH;
            int i=0; const char*s="Open Trash"; for(i=0;s[i];i++)ctx_labels[ctx_count][i]=s[i]; ctx_labels[ctx_count][i]=0; ctx_count++; }
        if (ctx_count < CTX_MAX_ITEMS) { ctx_actions[ctx_count] = CTX_ACT_EMPTY_TRASH;
            int i=0; const char*s="Empty Trash"; for(i=0;s[i];i++)ctx_labels[ctx_count][i]=s[i]; ctx_labels[ctx_count][i]=0; ctx_count++; }
    } else if (ptype == 2) {
        // Desktop empty-space context menu
        if (ctx_count < CTX_MAX_ITEMS) { ctx_actions[ctx_count] = CTX_ACT_NEW_FOLDER;
            int i=0; const char*s="New Folder"; for(i=0;s[i];i++)ctx_labels[ctx_count][i]=s[i]; ctx_labels[ctx_count][i]=0; ctx_count++; }
        if (ctx_count < CTX_MAX_ITEMS) { ctx_actions[ctx_count] = CTX_ACT_NEW_FILE;
            int i=0; const char*s="New File"; for(i=0;s[i];i++)ctx_labels[ctx_count][i]=s[i]; ctx_labels[ctx_count][i]=0; ctx_count++; }
        if (ctx_count < CTX_MAX_ITEMS) { ctx_actions[ctx_count] = CTX_ACT_REFRESH;
            int i=0; const char*s="Refresh"; for(i=0;s[i];i++)ctx_labels[ctx_count][i]=s[i]; ctx_labels[ctx_count][i]=0; ctx_count++; }
        if (ctx_count < CTX_MAX_ITEMS) { ctx_actions[ctx_count] = CTX_ACT_WALLPAPER;
            int i=0; const char*s="Change Wallpaper"; for(i=0;s[i];i++)ctx_labels[ctx_count][i]=s[i]; ctx_labels[ctx_count][i]=0; ctx_count++; }
        if (ctx_count < CTX_MAX_ITEMS) { ctx_actions[ctx_count] = CTX_ACT_RESET_LAYOUT;
            int i=0; const char*s="Reset Icon Layout"; for(i=0;s[i];i++)ctx_labels[ctx_count][i]=s[i]; ctx_labels[ctx_count][i]=0; ctx_count++; }
        // Group separators: [New Folder / New File] [Refresh] [Wallpaper / Reset]
        ctx_sep_after[1] = 1;
        ctx_sep_after[2] = 1;
    } else if (ptype == 3) {
        // Desktop file/folder icon menu (New Folder / New File icons)
        if (ctx_count < CTX_MAX_ITEMS) { ctx_actions[ctx_count] = CTX_ACT_OPEN;
            int i=0; const char*s="Open"; for(i=0;s[i];i++)ctx_labels[ctx_count][i]=s[i]; ctx_labels[ctx_count][i]=0; ctx_count++; }
        if (ctx_count < CTX_MAX_ITEMS) { ctx_actions[ctx_count] = CTX_ACT_DELETE_FILE;
            int i=0; const char*s="Delete"; for(i=0;s[i];i++)ctx_labels[ctx_count][i]=s[i]; ctx_labels[ctx_count][i]=0; ctx_count++; }
    }

    // Compute panel dimensions
    int max_w = 0;
    for (int i = 0; i < ctx_count; i++) {
        int len = 0; while (ctx_labels[i][len]) len++;
        if (len * 8 > max_w) max_w = len * 8;
    }
    ctx_w = max_w + 40;   // room for the 16px icon column + label gutter
    ctx_h = ctx_count * 26 + 12;
    // Keep the panel fully on-screen — desktop right-clicks can land
    // near the right/bottom edge and the menu would otherwise clip.
    if (vesa.width > 0 && vesa.height > 0) {
        if (ctx_x + ctx_w > (int)vesa.width)  ctx_x = (int)vesa.width - ctx_w - 4;
        if (ctx_y + ctx_h > (int)vesa.height) ctx_y = (int)vesa.height - ctx_h - 4;
        if (ctx_x < 0) ctx_x = 0;
        if (ctx_y < 0) ctx_y = 0;
    }
    ctx_active = 1;
}

// ─── Context-menu item icons (16×16 pixel art) ───────────────────
// Small glyphs drawn with the VESA primitives, one per action, so
// every context menu matches the light Windows-11 style of the panel.
static void ctx_draw_item_icon(int x, int y, int act) {
    switch (act) {
    case CTX_ACT_NEW_FOLDER: {          // amber folder with sheen
        uint32_t c = 0xF2B73D;
        vesa_fill_rect(x,     y,     7, 3, c);        // tab
        vesa_fill_rect(x,     y + 3, 16, 9, c);       // body
        vesa_fill_rect(x,     y + 3, 16, 1, 0xFFD877); // top sheen
        vesa_fill_rect(x + 2, y + 6, 12, 4, 0xFFE3A8); // inner light
        break;
    }
    case CTX_ACT_NEW_FILE: {            // white page, folded corner
        uint32_t c = 0x9AA0A6;
        vesa_fill_rect(x + 3, y, 10, 16, 0xFFFFFF);
        vesa_draw_rect(x + 3, y, 10, 16, c);
        vesa_fill_rect(x + 10, y, 3, 3, 0xD5D8DD);  // fold
        vesa_fill_rect(x + 5, y + 4, 6, 1, 0xC8CDD4);
        vesa_fill_rect(x + 5, y + 7, 6, 1, 0xC8CDD4);
        vesa_fill_rect(x + 5, y + 10, 4, 1, 0xC8CDD4);
        break;
    }
    case CTX_ACT_REFRESH: {             // blue circular arrows
        uint32_t c = 0x4A90D9;
        vesa_fill_rect(x + 3, y + 1, 10, 3, c);      // top
        vesa_fill_rect(x + 1, y + 4, 3, 8, c);       // left
        vesa_fill_rect(x + 12, y + 4, 3, 8, c);      // right
        vesa_fill_rect(x + 3, y + 12, 10, 3, c);     // bottom
        vesa_fill_rect(x + 5, y + 1, 3, 2, 0xF0F6FC); // arrow notches
        vesa_fill_rect(x + 8, y + 13, 3, 2, 0xF0F6FC);
        break;
    }
    case CTX_ACT_WALLPAPER: {           // picture frame with sun + hills
        vesa_draw_rect(x + 1, y + 1, 14, 14, 0x7A8FA6);
        vesa_fill_rect(x + 3, y + 3, 10, 10, 0x9ED3F5);
        vesa_fill_rect(x + 5, y + 4, 3, 3, 0xFFD34D);
        vesa_fill_rect(x + 3, y + 8, 10, 5, 0x6BBF6B);
        vesa_fill_rect(x + 4, y + 7, 4, 1, 0x3E8E5A);
        break;
    }
    case CTX_ACT_RESET_LAYOUT: {        // 2x2 grid of tiles
        uint32_t c = 0x8A6FBF;
        vesa_draw_rect(x + 1, y + 1, 6, 6, c);
        vesa_draw_rect(x + 9, y + 1, 6, 6, c);
        vesa_draw_rect(x + 1, y + 9, 6, 6, c);
        vesa_draw_rect(x + 9, y + 9, 6, 6, c);
        break;
    }
    case CTX_ACT_OPEN: {                // green play triangle
        uint32_t c = 0x3E9B4F;
        vesa_fill_rect(x + 3, y + 2, 3, 12, c);
        vesa_fill_rect(x + 6, y + 5, 3, 6, c);
        vesa_fill_rect(x + 9, y + 6, 3, 4, c);
        break;
    }
    case CTX_ACT_RENAME: {              // purple pencil
        uint32_t c = 0x6A4FBF;
        vesa_fill_rect(x + 2, y + 11, 12, 3, c);   // base
        vesa_fill_rect(x + 4, y + 4, 2, 7, c);
        vesa_fill_rect(x + 6, y + 3, 2, 8, c);
        vesa_fill_rect(x + 8, y + 2, 3, 3, c);     // tip
        vesa_fill_rect(x + 3, y + 6, 1, 2, 0xFFE3A8); // highlight
        break;
    }
    case CTX_ACT_PROPERTIES: {          // gray sliders with blue knobs
        uint32_t c = 0x6A6F78;
        vesa_fill_rect(x + 1, y + 3, 14, 2, c);
        vesa_fill_rect(x + 4, y + 2, 4, 4, 0x4A90D9);
        vesa_fill_rect(x + 1, y + 8, 14, 2, c);
        vesa_fill_rect(x + 8, y + 7, 4, 4, 0x4A90D9);
        vesa_fill_rect(x + 1, y + 13, 14, 2, c);
        vesa_fill_rect(x + 6, y + 12, 4, 4, 0x4A90D9);
        break;
    }
    case CTX_ACT_OPEN_TRASH:
    case CTX_ACT_EMPTY_TRASH: {         // trash can; red when emptying
        uint32_t c = (act == CTX_ACT_EMPTY_TRASH) ? 0xFF6655 : 0x9AA0A6;
        vesa_hline(x + 2, y + 1, 12, c);
        vesa_fill_rect(x + 4, y + 3, 8, 11, c);
        vesa_fill_rect(x + 5, y + 4, 2, 9, 0xF4F5F7);
        vesa_fill_rect(x + 9, y + 4, 2, 9, 0xF4F5F7);
        break;
    }
    case CTX_ACT_DELETE_FILE: {         // red X — delete the icon
        uint32_t c = 0xFF6655;
        vesa_fill_rect(x + 2, y + 7, 12, 2, c);
        vesa_fill_rect(x + 7, y + 2, 2, 12, c);
        break;
    }
    default: break;
    }
}

static void ctxmenu_draw(void) {
    if (!ctx_active) return;

    // Soft drop shadow under the panel
    vesa_fill_rect(ctx_x + 3, ctx_y + 4, ctx_w, ctx_h, 0x0A0618);
    // Dark panel — Nyx style: flat deep-purple surface with a purple
    // hairline border and rounded corners (r≈6).  Flat (not gradient)
    // so the label background color matches the panel exactly — no
    // seams behind the text glyphs.
    icon_rrect_grad_r(ctx_x, ctx_y, ctx_w, ctx_h, 6, CTX_PANEL_BG, CTX_PANEL_BG);
    icon_rrect_border(ctx_x, ctx_y, ctx_w, ctx_h, 0x5A4A8C);

    // Items — check hover state
    int mx = mouse.px, my = mouse.py;
    ctx_sel = -1;
    for (int i = 0; i < ctx_count; i++) {
        int iy = ctx_y + 6 + i * 26;
        int ih = 24;
        int hover = (mx >= ctx_x + 2 && mx < ctx_x + ctx_w - 2 &&
                     my >= iy && my < iy + ih);
        if (hover) ctx_sel = i;

        // Rounded purple hover highlight, solid so the label
        // background matches it with no seams.
        if (hover) {
            icon_rrect_grad_r(ctx_x + 2, iy, ctx_w - 4, ih, 5,
                              CTX_HOVER_BG, CTX_HOVER_BG);
        }

        // 16px icon column at the left of the item
        ctx_draw_item_icon(ctx_x + 8, iy + 4, ctx_actions[i]);

        // Label — light text on the dark panel. The bg must be the
        // panel/hover color (NOT 0): vesa_draw_char fills every
        // non-glyph pixel with bg, so 0 would paint solid black blocks.
        vesa_draw_string(ctx_x + 30, iy + 5, ctx_labels[i],
                         hover ? 0xFFFFFF : 0xE8E0F8,
                         hover ? CTX_HOVER_BG : CTX_PANEL_BG);

        // Group separator (dark purple hairline below the item)
        if (i < ctx_count - 1 && ctx_sep_after[i]) {
            vesa_hline(ctx_x + 8, iy + ih + 1, ctx_w - 16, 0x2A2250);
        }
    }
}

// Bounding box of the Files window's Trash sidebar drop zone, set
// by draw_files_window() each frame and read by the drop dispatch
// in desktop_draw(). Initialized to -1 to mean "not computed yet."
static int files_sidebar_trash_x = -1;
static int files_sidebar_trash_y = -1;
static int files_sidebar_trash_w = 120;
static int files_sidebar_trash_h = 46;

// Desktop Trash icon bounding box — set during drag source detection
// in MODE_DESKTOP so the drop dispatch can check against it.
static int desktop_trash_icon_x = -1;
static int desktop_trash_icon_y = -1;
static int desktop_trash_icon_w = 64;
static int desktop_trash_icon_h = 88; // tile_h + label_h + 4

// Icon rearrangement: icon_order[i] = which app index sits at grid
// position i (row-major, 10 cols × 7 rows). Initialized to identity
// mapping (icon_order[i] == i) so the default grid has no changes.
// Dragging an icon to another position swaps the two entries.
int icon_order[ICON_GRID_SLOTS];  // -1 = empty slot (exposed for mouse.c snap preview)
static int icon_order_inited = 0;

// ─── Undo/redo ring buffers ──────────────────────────────────────
// Each relocation saves a snapshot of icon_order[] to the undo stack.
// Ctrl+Z pops the most recent entry and pushes current state to the
// redo stack.  Ctrl+Shift+Z reverses.  The buffer depth is 12 — old
// entries are shifted out when full.  A new relocation clears redo.
#define UNDO_DEPTH 32
static int undo_stack[UNDO_DEPTH][ICON_GRID_SLOTS];
static int undo_ptr   = -1;   // index of most recent undo entry (-1 = empty)
static int redo_stack[UNDO_DEPTH][ICON_GRID_SLOTS];
static int redo_ptr   = -1;   // index of most recent redo entry (-1 = empty)

// ─── Undo browse mode ─────────────────────────────────────────
// Hold Ctrl and tap Z to cycle through undo history — each tap
// shows the previous state as a live preview overlay. Enter commits
// the preview; Esc cancels and restores the original layout.
// undo_browse_save[] captures icon_order[] at the moment the first
// Ctrl+Z press entered browsing mode, so Cancel can restore it.
static int undo_browse_active = 0;
static int undo_browse_idx    = -1;
static int undo_browse_save[ICON_GRID_SLOTS];

// ─── Icon layout persistence (ATA sector past the filesystem's data ─
// ─── 1 superblock + 2 directory + 64 data sectors = 67 used sectors) ─
// We save/restore icon_order[] directly to/from a single ATA sector so
// icon positions survive a reboot — separate from the user-visible file
// system, so deleting/renaming files can't corrupt the layout.
//
// desktop_trash_icon_x/y are NOT stored independently — they are derived
// at runtime from icon_order[] (finding the grid slot with app index 5
// and computing its pixel position).  Persisting icon_order[] implicitly
// restores the Trash position to the correct grid slot.
#define ICON_LAYOUT_MAGIC     0x4E594958  // "NYIX" in hex — validation token
// The layout lives in the disk sector RESERVED for it (v7, NYXFS002):
// sector FS_ICON_LAYOUT_SECTOR (12) sits immediately after the
// directory and BEFORE the growing file-data frontier.  fs_save_to_disk()
// never writes there — file data starts at FS_DATA_START (13) and only
// grows upward — so desktop state can never be overwritten by files,
// and the icon-layout sector keeps a small, fixed address even as the
// disk auto-grows to many MB.
//
// This MUST NOT sit inside any file's data range.  The old value (67)
// was inside file 0's data (sectors 4..1027): every fs_save_to_disk()
// — file creation, the ~11s periodic autosave, editor saves, shutdown
// — wiped the saved layout, so the next boot failed the magic check
// and fell back to the default icon order.  That was the "icons reset
// after creating a file" bug.
#define ICON_LAYOUT_SECTOR    FS_ICON_LAYOUT_SECTOR

// Write the current desktop state to a dedicated ATA sector (v4).
// Silently does nothing if no disk is present (ata_disk_present == 0).
static void icon_layout_save(void) {
    if (!ata_disk_present) return;
    uint8_t buf[512];
    for (int i = 0; i < 512; i++) buf[i] = 0;
    // Magic + version
    buf[0] = (uint8_t)(ICON_LAYOUT_MAGIC >> 24);
    buf[1] = (uint8_t)(ICON_LAYOUT_MAGIC >> 16);
    buf[2] = (uint8_t)(ICON_LAYOUT_MAGIC >> 8);
    buf[3] = (uint8_t)(ICON_LAYOUT_MAGIC);
    buf[4] = 6;  // version (v6 = wallpaper at 8-15, icon_order at 16+,
                 //            desktop-file list after icon_order)
    // v5+ uses buf[5-6] as a 16-bit slot count so resolutions with
    // more than 255 slots are handled correctly.
    buf[5] = (uint8_t)(g_grid_slots & 0xFF);
    buf[6] = (uint8_t)((g_grid_slots >> 8) & 0xFF);
    // v5: wallpaper fields first (before icon_order) so the icon_order
    // array can grow without overlapping them.
    // current_wallpaper at buf[8..11], custom_wallpaper_file at buf[12..15]
    {   int v = current_wallpaper;
        buf[8]  = (uint8_t)(v & 0xFF);
        buf[9]  = (uint8_t)((v >> 8) & 0xFF);
        buf[10] = (uint8_t)((v >> 16) & 0xFF);
        buf[11] = (uint8_t)((v >> 24) & 0xFF);
    }
    {   int v = custom_wallpaper_file;
        buf[12] = (uint8_t)(v & 0xFF);
        buf[13] = (uint8_t)((v >> 8) & 0xFF);
        buf[14] = (uint8_t)((v >> 16) & 0xFF);
        buf[15] = (uint8_t)((v >> 24) & 0xFF);
    }
    // Pack icon_order[] starting at buf[16] — each int is 4 bytes.
    // Only the visible (active) slots are saved so the sector is
    // resolution-agnostic. (512-16)/4 = 124 slots max per sector.
    int save_count = g_grid_slots < 124 ? g_grid_slots : 124;
    for (int i = 0; i < save_count; i++) {
        int idx = 16 + i * 4;
        int v = icon_order[i];
        buf[idx]     = (uint8_t)(v & 0xFF);
        buf[idx + 1] = (uint8_t)((v >> 8) & 0xFF);
        buf[idx + 2] = (uint8_t)((v >> 16) & 0xFF);
        buf[idx + 3] = (uint8_t)((v >> 24) & 0xFF);
    }

    // v6: desktop file list packed right after icon_order[] — a 1-byte
    // count followed by up to MAX_DESKTOP_FILES filesystem indices (each
    // fits in a byte since MAX_FILES=127 < 256).  With the 112-slot cap
    // this ends at 16+448+17=481 < 512, so it never overflows the sector.
    {
        int dfl_ofs = 16 + save_count * 4;
        if (dfl_ofs + 1 + MAX_DESKTOP_FILES <= 512) {
            int dc = desktop_files_count > MAX_DESKTOP_FILES ? MAX_DESKTOP_FILES : desktop_files_count;
            buf[dfl_ofs] = (uint8_t)dc;
            for (int i = 0; i < dc; i++)
                buf[dfl_ofs + 1 + i] = (uint8_t)desktop_files[i];
        }
    }
    ata_write_sector(ICON_LAYOUT_SECTOR, buf);
}

// Restore desktop state from the ATA layout sector (v1-v4).
// Returns 1 on success, 0 if no disk/unsaved/corrupted — caller
// falls back to defaults.
// v1 (version 1): icon_order[19] at bytes 8-83
// v2 (version 2): icon_order[19] at bytes 8-83, wallpaper at 84-91
// v3 (version 3): icon_order[30] at bytes 8-127, wallpaper at 128-135
// v4 (version 4): icon_order[70] at bytes 8-287, wallpaper at 288-295
static int icon_layout_load(void) {
    if (!ata_disk_present) return 0;
    uint8_t buf[512];
    if (!ata_read_sector(ICON_LAYOUT_SECTOR, buf)) return 0;
    // Validate magic
    uint32_t magic = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
                     ((uint32_t)buf[2] << 8)  |  (uint32_t)buf[3];
    if (magic != ICON_LAYOUT_MAGIC) return 0;
    uint8_t ver = buf[4];
    if (ver < 1 || ver > 6) return 0;          // unknown version

    // ── Read slot count from the sector ───────────────────────────
    // v1/v2: 19 slots, v3: 30 slots, v4: 70 slots
    // v5: buf[5-6] = 16-bit count (resolution-dependent)
    int slot_count;
    if (ver >= 5) {
        slot_count = (int)buf[5] | ((int)buf[6] << 8);
        if (slot_count < 1 || slot_count > ICON_GRID_SLOTS)
            return 0;  // corrupted
    } else if (ver >= 4) {
        slot_count = 70;
        if ((int)buf[5] != slot_count) return 0;
    } else if (ver >= 3) {
        slot_count = 30;
        if ((int)buf[5] != slot_count) return 0;
    } else {
        slot_count = DESKTOP_APP_COUNT;
        if ((int)buf[5] != slot_count) return 0;
    }

    // v5: icon_order starts at buf[16]; v1-v4: starts at buf[8]
    int data_ofs = (ver >= 5) ? 16 : 8;
    for (int i = 0; i < slot_count && i < ICON_GRID_SLOTS; i++) {
        int idx = data_ofs + i * 4;
        int val = (int)buf[idx] | ((int)buf[idx + 1] << 8) |
                  ((int)buf[idx + 2] << 16) | ((int)buf[idx + 3] << 24);
        if (ver >= 3) {
            // v3+: -1 is valid (empty slot), app indices 0..18 valid
            if (val < -1 || val >= DESKTOP_APP_COUNT) return 0;
        } else {
            // v1/v2: all slots must be filled with valid app indices
            if (val < 0 || val >= DESKTOP_APP_COUNT) return 0;
        }
        icon_order[i] = val;
    }
    // Fill remaining slots (loading from a smaller or different-
    // resolution save — off-screen entries become empty).
    for (int i = slot_count; i < ICON_GRID_SLOTS; i++)
        icon_order[i] = -1;

    // ── Desktop file list (v6) ──────────────────────────────────
    // Restore which filesystem entries show as desktop icons.  Each
    // index is validated against the live filesystem so stale entries
    // (file deleted, slot reused) are silently dropped.
    desktop_files_count = 0;
    if (ver >= 6) {
        int dfl_ofs = 16 + slot_count * 4;
        if (dfl_ofs + 1 <= 512) {
            int dc = (int)buf[dfl_ofs];
            if (dc > MAX_DESKTOP_FILES) dc = MAX_DESKTOP_FILES;
            for (int i = 0; i < dc; i++) {
                int fi = (int)buf[dfl_ofs + 1 + i];
                if (fi >= 0 && fi < MAX_FILES && filesystem[fi].used)
                    desktop_files[desktop_files_count++] = fi;
            }
        }
    }

    // ── Wallpaper state ─────────────────────────────────────────
    // v5: wallpaper fields at buf[8..11] and buf[12..15]
    // v4: wallpaper fields at buf[288..295]
    // v3: wallpaper fields at buf[128..135]
    // v2: wallpaper fields at buf[84..91]
    if (ver >= 2) {
        int wp_o, cf_o;
        if (ver >= 5)       { wp_o = 8;  cf_o = 12; }
        else if (ver >= 4)  { wp_o = 288; cf_o = 292; }
        else if (ver >= 3)  { wp_o = 128; cf_o = 132; }
        else                { wp_o = 84;  cf_o = 88; }
        int wp_idx  = (int)buf[wp_o]     | ((int)buf[wp_o + 1] << 8) |
                      ((int)buf[wp_o + 2] << 16) | ((int)buf[wp_o + 3] << 24);
        int cust_fi = (int)buf[cf_o]     | ((int)buf[cf_o + 1] << 8) |
                      ((int)buf[cf_o + 2] << 16) | ((int)buf[cf_o + 3] << 24);
        if (wp_idx < 0 || wp_idx >= WALLPAPER_COUNT) wp_idx = 0;
        if (cust_fi < -1 || cust_fi >= MAX_FILES) cust_fi = -1;
        if (cust_fi >= 0 && (!filesystem[cust_fi].used)) cust_fi = -1;
        current_wallpaper = wp_idx;
        custom_wallpaper_file = cust_fi;
    }
    return 1;
}

static void icon_order_init(void) {
    if (icon_order_inited) return;
    // Start with identity mapping for app slots, -1 for empty slots,
    // then try to overwrite with any saved layout from disk.
    for (int i = 0; i < DESKTOP_APP_COUNT; i++)
        icon_order[i] = i;
    // g_grid_slots is now set (compute_icon_grid was called once
    // during init), so fill only the visible slots as empty.
    for (int i = DESKTOP_APP_COUNT; i < g_grid_slots; i++)
        icon_order[i] = -1;
    for (int i = g_grid_slots; i < ICON_GRID_SLOTS; i++)
        icon_order[i] = -1;  // off-screen area: empty
    icon_layout_load();

    // ── Heal: new apps added in newer builds must still appear ────
    // A saved layout from an older build has no slot for apps that
    // didn't exist then (their slots are -1 or occupied).  For every
    // app index that is nowhere in the visible layout, append it in
    // the first empty visible slot so it shows up after an upgrade.
    for (int a = 0; a < DESKTOP_APP_COUNT; a++) {
        int found = 0;
        for (int i = 0; i < g_grid_slots; i++)
            if (icon_order[i] == a) { found = 1; break; }
        if (found) continue;
        for (int i = 0; i < g_grid_slots; i++) {
            if (icon_order[i] == -1) { icon_order[i] = a; break; }
        }
    }

    // ── Default wallpaper: use the embedded "wallpaper" JPEG ──────
    // If no saved wallpaper state exists (first boot or disk missing),
    // auto-select the "wallpaper" file from the embedded filesystem
    // so the user's image is always the desktop background until they
    // explicitly change it in Settings.
    if (custom_wallpaper_file < 0) {
        for (int i = 0; i < MAX_FILES; i++) {
            if (filesystem[i].used &&
                !strcmp(filesystem[i].name, "wallpaper")) {
                custom_wallpaper_file = i;
                break;
            }
        }
    }

    icon_order_inited = 1;
}

static int trash_count       = 0;
static int trash_selected    = 0;
static int settings_scroll   = 0;

// ─── Window body color presets (for compositor body_color picker) ─
// 8 preset colors indexed by keys 1-8 in the Theme settings panel.
// Hoisted to file scope so the UI renderer and keyboard handler
// share a single source of truth.
static const uint32_t body_pal[8] = {
    0x140C30,  // 1: deep purple (default)
    0x1A0A28,  // 2: dark violet
    0x0C1828,  // 3: midnight blue
    0x1C1010,  // 4: dark burgundy
    0x0E1818,  // 5: dark teal
    0x201810,  // 6: dark bronze
    0x181018,  // 7: charcoal plum
    0x0E0E1E   // 8: near-black
};
static const char* body_names[8] = {
    "Purple", "Violet", "Midnight", "Burgundy",
    "Teal", "Bronze", "Charcoal", "Near Black"
};

// Pixel buffer for decoding images in the file manager
#define FM_PIXBUF_SIZE (1920 * 1080 * 4)
static uint8_t fm_pixbuf[FM_PIXBUF_SIZE] __attribute__((aligned(64)));

// Async JPEG open state — files_open_file() hands a JPEG to the
// background jpegdec worker task instead of decoding synchronously
// (the open path can run inside the keyboard/mouse IRQ chain).
// desktop_async_pump() completes the open when the worker is done.
static int  fm_async_open = 0;          // 1 while a JPEG is decoding
static char fm_async_name[64];          // file name for imgview_init

// Buffer for extracted PDF text (used by ENTER handler in MODE_FILES).
// Must be file-scope static — the kernel stack is only 16 KB, a 64 KB
// local would overflow and corrupt memory.
#define PDFVIEW_BUF_SIZE 262144
static char pdfview_buf[PDFVIEW_BUF_SIZE];

// Quick header scan: get image dimensions WITHOUT full decode, so we
// can reject oversized images before the decoder writes past the pixel
// buffer and corrupts adjacent BSS (filesystem names, etc.).
static int img_get_dimensions(const uint8_t* data, int datalen,
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
    // BMP: "BM" at bytes 0-1, width at 18-21, height at 22-25 (signed)
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

// Search
static int  search_active = 0;
static char search_query[32];
static int  search_qpos   = 0;
static int  search_results[16];
static int  search_rcount = 0;

// Terminal — real shell output buffer. The live input line itself
// lives in keyboard.c's cmd_buffer/cmd_pos (it's the code that
// actually receives every keystroke); this just captures output.
static term_redirect_t term_buf;
static int  term_initialized = 0;
static int  term_scroll      = 0; // 0 = pinned to bottom (latest output)

void desktop_terminal_clear(void) {
    terminal_redirect_clear(&term_buf);
}

// Users
static int  users_selected  = 0;
int  users_sub       = 0;
static char u_new_name[17];
static char u_new_pass[17];
static char u_old_pass[17];
static char u_confirm[17];
static int  u_field         = 0;
static int  u_name_pos      = 0;
static int  u_pass_pos      = 0;
static int  u_old_pos       = 0;
static int  u_confirm_pos   = 0;
static char u_msg[48];

#define MAX_TRASH 16
static File trash[MAX_TRASH];

// ─── Launch helper ────────────────────────────────────────────────
static void do_launch(int idx){
    // Reset window position so the new app opens at its default position
    g_win_x = -1;
    g_win_y = -1;
    g_win_off_x = 0;
    g_win_off_y = 0;
    
    switch(idx){
        case 0:  current_mode=MODE_TERMINAL; break;
        case 1:  files_cur_path[0]='\0'; files_home_view=1; files_desktop_view=0; selected_file=0; current_mode=MODE_FILES; break;
        case 2:
            // Desktop-launched editor: never inherit the "came from
            // Files" flag (cleared again here so a mouse-close of a
            // Files-opened editor can't leak it into a later launch).
            editor_return_files = 0;
            editor_init();
            current_mode = MODE_EDITOR;
            break;
        case 3:  applib_init();   current_mode=MODE_APPLIB;   break;
        case 4:  trash_selected=0; current_mode=MODE_TRASH; break;
        case 5:
            if(vesa.active){
                settings_section=0;
                settings_selected=current_timezone;
                settings_scroll=current_timezone>9?current_timezone-9:0;
            } else {
                settings_section=-1;
                settings_selected=0;
            }
            current_mode=MODE_SETTINGS; break;
        case 6:  current_mode=MODE_MUSIC;    break;
        case 7:  calc_init();     current_mode=MODE_CALC;     break;
        case 8:  games_init();    current_mode=MODE_GAMES;    break;
        case 9:  paint_init();    current_mode=MODE_PAINT;    break;
        case 10: sysmon_init();   current_mode=MODE_SYSMON;   break;
        case 11: clock_init();    current_mode=MODE_CLOCK;    break;
        case 12: calendar_init(); current_mode=MODE_CALENDAR; break;
        case 13: current_mode=MODE_WEATHER; break;
        case 14: organizer_init(); current_mode=MODE_ORGANIZER; break;
        case 15: security_init();  current_mode=MODE_SECURITY;  break;
        case 16: gallery_init();   current_mode=MODE_GALLERY;   break;
        case 17: browser_init();   current_mode=MODE_BROWSER;   break;
        case 18:
            current_mode=MODE_QUICKSETTINGS;
            break;
        case 19: {
            // DOOM — launch the embedded DOOM.EXE PE directly (one
            // click instead of Files → Local Disk → DOOM.EXE).
            for (int fi = 0; fi < MAX_FILES; fi++) {
                if (filesystem[fi].used &&
                    str_eq(filesystem[fi].name, "DOOM.EXE")) {
                    const uint8_t* c = (const uint8_t*)filesystem[fi].content;
                    int clen = filesystem[fi].size;
                    if (clen <= 0) { while (c[clen]) clen++; }
                    if (clen > 64 && c[0]=='M' && c[1]=='Z') {
                        pe_dump_header(c, clen);
                        if (pe_exec(c, clen, "DOOM.EXE", 0) >= 0)
                            terminal_print("PE: DOOM.EXE loaded successfully\n");
                        else
                            terminal_print("PE: failed to load DOOM.EXE\n");
                    } else {
                        terminal_print("DOOM.EXE is not a valid PE\n");
                    }
                    break;
                }
            }
            break;
        }
        case 20: {
            // CHESS — launch the embedded CHESS.EXE PE directly (same
            // one-click ring-3 flow as DOOM.EXE above).
            for (int fi = 0; fi < MAX_FILES; fi++) {
                if (filesystem[fi].used &&
                    str_eq(filesystem[fi].name, "CHESS.EXE")) {
                    const uint8_t* c = (const uint8_t*)filesystem[fi].content;
                    int clen = filesystem[fi].size;
                    if (clen <= 0) { while (c[clen]) clen++; }
                    if (clen > 64 && c[0]=='M' && c[1]=='Z') {
                        pe_dump_header(c, clen);
                        if (pe_exec(c, clen, "CHESS.EXE", 0) >= 0)
                            terminal_print("PE: CHESS.EXE loaded successfully\n");
                        else
                            terminal_print("PE: failed to load CHESS.EXE\n");
                    } else {
                        terminal_print("CHESS.EXE is not a valid PE\n");
                    }
                    break;
                }
            }
            break;
        }
        case 21:
            store_open();
            current_mode=MODE_STORE;
            break;
    }
    // Register the newly opened app with the native window manager
    // (it becomes the focused window; the taskbar tray renders it).
    win_open(current_mode);
}

// ─── Search helpers ───────────────────────────────────────────────
static void do_search(){
    search_rcount=0;
    if(!search_query[0]) return;
    for(int i=0;i<DESKTOP_APP_COUNT;i++){
        const char*name=app_names[i];
        const char*q=search_query;
        int nl=str_len(name),ql=str_len(q);
        if(ql>nl) continue;
        for(int j=0;j<=nl-ql;j++){
            int ok=1;
            for(int k=0;k<ql;k++){
                char nc=name[j+k],qc=q[k];
                if(nc>='A'&&nc<='Z')nc+=32;
                if(qc>='A'&&qc<='Z')qc+=32;
                if(nc!=qc){ok=0;break;}
            }
            if(ok){search_results[search_rcount++]=i;break;}
        }
        if(search_rcount>=8) break;
    }
}

static void draw_search_bar(){
    const Theme*t=TH;
    vga_fill_rect(5,9,70,3,' ',15,1);
    for(int x=5;x<75;x++){vga_put(x,9,'-',t->accent,1);vga_put(x,11,'-',t->accent,1);}
    for(int y=9;y<=11;y++){vga_put(5,y,'|',t->accent,1);vga_put(74,y,'|',t->accent,1);}
    vga_put(5,9,'+',t->accent,1);vga_put(74,9,'+',t->accent,1);
    vga_put(5,11,'+',t->accent,1);vga_put(74,11,'+',t->accent,1);
    vga_puts(7,9,"[ Search Apps - type to filter ]",t->accent,1);

    vga_fill_rect(7,10,50,1,' ',0,7);
    vga_puts(7,10,search_query,0,7);
    vga_put(7+search_qpos,10,'_',0,15);
    vga_puts(59,10,"ESC:close",7,1);

    if(search_rcount>0){
        int ry=12;
        vga_fill_rect(5,ry,70,search_rcount+2,' ',15,8);
        for(int x=5;x<75;x++){vga_put(x,ry,'-',8,8);vga_put(x,ry+search_rcount+1,'-',8,8);}
        for(int y=ry;y<=ry+search_rcount+1;y++){vga_put(5,y,'|',8,8);vga_put(74,y,'|',8,8);}
        for(int i=0;i<search_rcount;i++){
            int ai=search_results[i];
            uint8_t fg=(i==selected_icon)?0:15;
            uint8_t bg=(i==selected_icon)?t->highlight:8;
            vga_fill_rect(6,ry+1+i,68,1,' ',fg,bg);
            vga_puts(8,ry+1+i,app_names[ai],fg,bg);
            vga_puts(22,ry+1+i,"- ENTER to open",fg,bg);
        }
    } else if(search_query[0]){
        vga_fill_rect(5,12,70,3,' ',8,8);
        vga_puts(20,13,"No apps found. Try a different name.",8,8);
    }
}

// ─── Status bar icon glyphs (GNOME-style top panel) ─────────────
// Small pixel-art icons drawn with vesa_put_pixel, each ~14×14.

// WiFi: three concentric arcs radiating upward from a dot.
static void tb_icon_wifi(int x, int y, uint32_t c) {
    // Solid dot at bottom center
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++)
            if (dx*dx + dy*dy <= 2)
                vesa_put_pixel(x+6+dx, y+11+dy, c);
    // Three concentric arcs (thicker, crisper)
    for (int dy = -10; dy <= 0; dy++) {
        for (int dx = -10; dx <= 10; dx++) {
            int d = dx*dx + dy*dy;
            if ((d >= 5 && d <= 12) || (d >= 26 && d <= 36) || (d >= 62 && d <= 78)) {
                vesa_put_pixel(x+6+dx, y+11+dy, c);
            }
        }
    }
}

// Volume: speaker cone + two sound waves.
static void tb_icon_volume(int x, int y, uint32_t c) {
    // Speaker body (trapezoid)
    vesa_fill_rect(x+1, y+5, 3, 5, c);
    vesa_fill_rect(x+3, y+3, 2, 9, c);
    vesa_fill_rect(x+4, y+4, 2, 7, c);
    // Sound wave 1 (small arc)
    for (int dy = -3; dy <= 3; dy++) {
        int wx = 7;
        if (dy == -3 || dy == 3) wx = 8;
        vesa_put_pixel(x+wx, y+6+dy, c);
    }
    // Sound wave 2 (larger arc)
    for (int dy = -5; dy <= 5; dy++) {
        int wx = 9;
        if (dy <= -4 || dy >= 4) wx = 11;
        else if (dy <= -2 || dy >= 2) wx = 10;
        vesa_put_pixel(x+wx, y+6+dy, c);
    }
    // Sound wave 3 (outermost, faint)
    for (int dy = -7; dy <= 7; dy++) {
        int wx = 12;
        if (dy <= -6 || dy >= 6) wx = 14;
        else if (dy <= -4 || dy >= 4) wx = 13;
        vesa_put_pixel(x+wx, y+6+dy, c);
    }
}

// Battery: horizontal bar with fill level + percentage text.
static void tb_icon_battery(int x, int y, uint32_t c, int pct) {
    // Rounded battery body
    vesa_fill_rect(x, y+3, 16, 8, 0x1A1424);
    vesa_draw_rect(x, y+3, 16, 8, c);
    // Positive terminal nub
    vesa_fill_rect(x+16, y+5, 2, 4, c);
    // Fill level with color coding
    int fill = pct * 14 / 100;
    uint32_t fill_c;
    if (pct > 50)      fill_c = 0x44DD66;  // green
    else if (pct > 20) fill_c = 0xFFCC33;  // amber
    else               fill_c = 0xFF4444;  // red
    if (fill > 0)
        vesa_fill_rect(x+1, y+4, fill, 6, fill_c);
}

// Bell: notification bell icon.
// ─── Volume popup state ─────────────────────────────────────────
// Toggled by clicking the volume icon in the top taskbar.
static int vol_popup_open = 0;     // 1 = slider visible
static int vol_popup_x = 0;       // left edge of popup (set by draw_taskbar)
static int vol_popup_y = 0;       // top edge (just below taskbar)
static int vol_popup_drag = 0;    // 1 = user is dragging the slider thumb
static int vol_popup_anim = 0;    // 0–255 animation progress (slide-down)
static int vol_popup_closing = 0; // 1 = animating closed
static int vol_popup_just_opened = 0; // skip close check on open frame
static int vol_popup_was_open = 0; // prevents icon drag on close frame
#define VOL_POPUP_W 200
#define VOL_POPUP_H 56
#define VOL_SLIDER_X_OFF 36       // slider track left offset inside popup
#define VOL_SLIDER_W     148      // slider track width
#define VOL_SLIDER_H      6       // slider track height

// Draw the volume slider popup (called from desktop_draw after taskbar)
static void draw_vol_popup(void) {
    if (!vol_popup_open) {
        vol_popup_drag = 0;
        return;
    }
    // ── Live drag: check if mouse is held on the slider track ───
    if (mouse.left && vol_popup_drag) {
        int tx = vol_popup_x + VOL_SLIDER_X_OFF;
        int vol = (mouse.px - tx) * 100 / VOL_SLIDER_W;
        if (vol < 0) vol = 0;
        if (vol > 100) vol = 100;
        audio_set_volume(vol);
    }
    if (!mouse.left) vol_popup_drag = 0;
    int W = (int)vesa.width;
    int px = vol_popup_x;
    int py = vol_popup_y;
    // Clamp to screen bounds
    if (px + VOL_POPUP_W > W) px = W - VOL_POPUP_W;
    if (px < 0) px = 0;

    // ── Popup background (dark rounded rect) ───────────────────
    for (int yy = 0; yy < VOL_POPUP_H; yy++) {
        int trim = 0;
        if (yy < 4) trim = 4 - yy;       // rounded top corners
        if (yy >= VOL_POPUP_H - 4) trim = yy - (VOL_POPUP_H - 5); // rounded bottom
        int rr = 0x1E, gg = 0x18, bb = 0x30;
        vesa_hline(px + trim, py + yy, VOL_POPUP_W - trim * 2,
                   (rr << 16) | (gg << 8) | bb);
    }
    vesa_draw_rect(px, py, VOL_POPUP_W, VOL_POPUP_H, 0x3A2E58);

    // ── Speaker icon (left side) ───────────────────────────────
    tb_icon_volume(px + 8, py + 16, 0xC8B8E0);

    // ── Slider track ───────────────────────────────────────────
    int tx = px + VOL_SLIDER_X_OFF;
    int ty = py + 24;
    // Track background (dark)
    for (int i = 0; i < VOL_SLIDER_H; i++) {
        int rr = 0x28, gg = 0x22, bb = 0x40;
        vesa_hline(tx, ty + i, VOL_SLIDER_W, (rr << 16) | (gg << 8) | bb);
    }
    // Filled portion (orange accent)
    int vol = audio_get_volume();
    int fill = VOL_SLIDER_W * vol / 100;
    if (fill > 0) {
        for (int i = 0; i < VOL_SLIDER_H; i++) {
            int rr = 0xFF, gg = 0x8C, bb = 0x3A;  // GNOME orange
            vesa_hline(tx, ty + i, fill, (rr << 16) | (gg << 8) | bb);
        }
    }
    // Thumb (white circle at fill end)
    int thumb_x = tx + fill;
    if (thumb_x < tx) thumb_x = tx;
    if (thumb_x > tx + VOL_SLIDER_W) thumb_x = tx + VOL_SLIDER_W;
    int thumb_cy = ty + VOL_SLIDER_H / 2;
    for (int dy = -5; dy <= 5; dy++)
        for (int dx = -5; dx <= 5; dx++)
            if (dx*dx + dy*dy <= 25)
                vesa_put_pixel(thumb_x, thumb_cy + dy, 0xFFFFFF);

    // ── Volume percentage text ─────────────────────────────────
    char vstr[5];
    vstr[0] = '0' + (vol / 100);
    vstr[1] = '0' + (vol / 10) % 10;
    vstr[2] = '0' + vol % 10;
    vstr[3] = '%';
    vstr[4] = '\0';
    vesa_draw_string(px + VOL_POPUP_W - 40, py + 18, vstr, 0xDDD0F0, 0);

    // ── Label ──────────────────────────────────────────────────
    vesa_draw_string(px + VOL_SLIDER_X_OFF, py + 8, "Volume", 0x9A8CC0, 0);
}

// Handle click inside the volume popup.  Returns 1 if consumed.
static int vol_popup_click(int mx, int my) {
    if (!vol_popup_open) return 0;
    int px = vol_popup_x;
    int py = vol_popup_y;
    if (mx < px || mx >= px + VOL_POPUP_W || my < py || my >= py + VOL_POPUP_H)
        return 0;  // click outside → close

    // Check if click is on the slider track area
    int tx = px + VOL_SLIDER_X_OFF;
    int ty = py + 24;
    if (my >= ty - 4 && my <= ty + VOL_SLIDER_H + 4 &&
        mx >= tx - 4 && mx <= tx + VOL_SLIDER_W + 4) {
        // Map x position to volume (0-100)
        int vol = (mx - tx) * 100 / VOL_SLIDER_W;
        if (vol < 0) vol = 0;
        if (vol > 100) vol = 100;
        audio_set_volume(vol);
        vol_popup_drag = 1;
        return 1;
    }
    // Click inside popup but not on slider = consume (don't close)
    return 1;
}

// Public: close volume popup (e.g. on ESC or click elsewhere) — animated
void vol_popup_close(void) {
    if (vol_popup_open && !vol_popup_closing) vol_popup_closing = 1;
    else { vol_popup_open = 0; vol_popup_drag = 0; vol_popup_anim = 0; }
}

// ─── Wi-Fi flyout state ────────────────────────────────────────
// Toggled by clicking the Wi-Fi icon in the top taskbar.
static int wifi_popup_open = 0;   // 1 = flyout visible
static int wifi_popup_x = 0;     // left edge (set by draw_taskbar)
static int wifi_popup_y = 0;     // top edge (just below taskbar)
static int wifi_popup_anim = 0;  // 0–255 animation progress (slide-down)
static int wifi_popup_closing = 0; // 1 = animating closed
static int wifi_popup_just_opened = 0; // skip close check on open frame
static int wifi_popup_was_open = 0; // prevents icon drag on close frame
#define WIFI_POPUP_W 260
#define WIFI_POPUP_ROW_H 42
#define WIFI_POPUP_HEADER 52     // header height (title + toggle + divider)

// ─── Popup animation tick ───────────────────────────────────────
// Instant open/close — no slide animation for now.
static void popup_anim_tick(void) {
    // Track whether popup was open last frame (for drag guard)
    vol_popup_was_open = vol_popup_open;
    wifi_popup_was_open = wifi_popup_open;
    if (vol_popup_open && !vol_popup_closing) {
        vol_popup_just_opened = 0;
        vol_popup_anim = 255;  // instantly full
    } else if (vol_popup_closing) {
        vol_popup_open = 0;
        vol_popup_closing = 0;
        vol_popup_drag = 0;
        vol_popup_anim = 0;
    }
    if (wifi_popup_open && !wifi_popup_closing) {
        wifi_popup_just_opened = 0;
        wifi_popup_anim = 255;  // instantly full
    } else if (wifi_popup_closing) {
        wifi_popup_open = 0;
        wifi_popup_closing = 0;
        wifi_popup_anim = 0;
    }
}

// Draw the Wi-Fi flyout panel (called from desktop_draw after taskbar)
static void draw_wifi_popup(void) {
    if (!wifi_popup_open) return;
    int W = (int)vesa.width;
    int px = wifi_popup_x;
    int py = wifi_popup_y;
    int rows = QS_WIFI_NETS;
    int fh = WIFI_POPUP_HEADER + rows * WIFI_POPUP_ROW_H + 12;
    if (px + WIFI_POPUP_W > W) px = W - WIFI_POPUP_W;
    if (px < 0) px = 0;
    if (py + fh > (int)vesa.height - 28) fh = (int)vesa.height - 28 - py;

    // ── Shadow + surface ──────────────────────────────────────
    vesa_fill_rect(px + 3, py + 4, WIFI_POPUP_W, fh, 0x120F0F);
    icon_rrect_grad_r(px, py, WIFI_POPUP_W, fh, 12, 0x1E1830, 0x1E1830);
    icon_rrect_border_r(px, py, WIFI_POPUP_W, fh, 12, 0x3A2E58);

    // ── Header: "Wi-Fi" title + toggle ───────────────────────
    vesa_draw_string(px + 16, py + 14, "Wi-Fi", 0xDDD0F0, 0x1E1830);
    int tx = px + WIFI_POPUP_W - 48, ty = py + 12;
    uint32_t tog_c = qs_wifi_on ? 0x44CC66 : 0x555555;
    vesa_fill_rect(tx, ty, 32, 16, tog_c);
    int knob_x = qs_wifi_on ? tx + 18 : tx + 2;
    vesa_fill_rect(knob_x, ty + 2, 12, 12, 0xFFFFFF);
    vesa_fill_rect(px + 14, py + 38, WIFI_POPUP_W - 28, 1, 0x3A2E58);

    if (!qs_wifi_on) {
        vesa_draw_string(px + 16, py + 56, "Wi-Fi is off", 0x8A8A8A, 0x1E1830);
        vesa_draw_string(px + 16, py + 76, "Turn on to see networks", 0x5A5A5A, 0x1E1830);
        return;
    }

    if (qs_wifi_scan > 0) {
        int dots = ((60 - qs_wifi_scan) / 5) % 4;
        char msg[40]; int k = 0;
        const char* m = "Scanning";
        while (m[k] && k < 24) { msg[k] = m[k]; k++; }
        for (int d = 0; d < dots && k < 27; d++) msg[k++] = '.';
        msg[k] = '\0';
        vesa_draw_string(px + 16, py + 56, msg, 0xC8C8C8, 0x1E1830);
        vesa_draw_string(px + 16, py + 76, "Looking for networks...", 0x6A6A6A, 0x1E1830);
        return;
    }

    // ── Network rows ──────────────────────────────────────────
    for (int i = 0; i < QS_WIFI_NETS; i++) {
        int rx = px + 10, ry = py + WIFI_POPUP_HEADER + i * WIFI_POPUP_ROW_H;
        int rw = WIFI_POPUP_W - 20, rh = WIFI_POPUP_ROW_H - 4;
        int hover = (mouse.px >= rx && mouse.px < rx + rw &&
                     mouse.py >= ry && mouse.py < ry + rh);
        int connected = (i == qs_wifi_cur);
        uint32_t bg = hover ? 0x2A2048 : 0x1E1830;
        icon_rrect_grad_r(rx, ry, rw, rh, 6, bg, bg);
        if (connected || hover)
            icon_rrect_border_r(rx, ry, rw, rh, 6,
                                connected ? 0xFF8C3A : 0x3A2E58);

        // Signal bars glyph
        qs_glyph_bars(rx + 16, ry + rh / 2, qs_wifi_signal[i],
                      connected ? 0xFF8C3A : 0xC8C8C8);

        // Network name
        vesa_draw_string(rx + 38, ry + 8, qs_wifi_names[i],
                         connected ? 0xFFFFFF : (hover ? 0xFFFFFF : 0xDCDCDC), bg);

        // Lock icon (if secured)
        if (qs_wifi_sec[i]) {
            int nx = rx + 38 + 8 * str_len(qs_wifi_names[i]) + 6;
            qs_glyph_lock_mini(nx, ry + 16, 0x9A9A9A);
        }

        // Connected checkmark
        if (connected) {
            vesa_draw_string(rx + 38, ry + 24, "Connected", 0xFF8C3A, bg);
            qs_glyph_check(rx + rw - 20, ry + rh / 2 - 4, 0xFF8C3A);
        }
    }
}

// Handle click inside the Wi-Fi popup.  Returns 1 if consumed.
static int wifi_popup_click(int mx, int my) {
    if (!wifi_popup_open) return 0;
    int px = wifi_popup_x, py = wifi_popup_y;
    int rows = QS_WIFI_NETS;
    int fh = WIFI_POPUP_HEADER + rows * WIFI_POPUP_ROW_H + 12;
    if (mx < px || mx >= px + WIFI_POPUP_W || my < py || my >= py + fh)
        return 0;  // click outside

    // Toggle switch click (top-right corner)
    int tx = px + WIFI_POPUP_W - 48, ty = py + 12;
    if (mx >= tx && mx < tx + 32 && my >= ty && my < ty + 16) {
        qs_wifi_on = !qs_wifi_on;
        if (qs_wifi_on) qs_wifi_scan = 60;  // start scanning
        return 1;
    }

    // Network row click → select/connect
    for (int i = 0; i < QS_WIFI_NETS; i++) {
        int ry = py + WIFI_POPUP_HEADER + i * WIFI_POPUP_ROW_H;
        int rh = WIFI_POPUP_ROW_H - 4;
        if (my >= ry && my < ry + rh) {
            qs_wifi_cur = i;
            return 1;
        }
    }
    return 1;  // click inside popup = consume
}

// ─── Taskbar ─────────────────────────────────────────────────────
// Public wrapper so mouse.c can launch apps without needing static do_launch
void desktop_launch(int idx){ do_launch(idx); }

#define TASKBAR_H 28

static void draw_taskbar(){
    const Theme*t=TH;

    if (vesa.active) {
        int W = (int)vesa.width;
        int bar_h = TASKBAR_H;

        // ── Dark GNOME-style background ─────────────────────────
        // Nearly solid dark with a very subtle vertical gradient.
        // Rounded bottom corners (4px radius) for the pill shape.
        for (int yy = 0; yy < bar_h; yy++) {
            int prog = yy * 255 / bar_h;
            int rr = 0x1A + (0x12 - 0x1A) * prog / 255;
            int gg = 0x14 + (0x0E - 0x14) * prog / 255;
            int bb = 0x24 + (0x1C - 0x24) * prog / 255;
            uint32_t col = (rr << 16) | (gg << 8) | bb;
            // Rounded bottom corners: trim 4px from left/right on last 4 rows
            int trim = 0;
            if (yy >= bar_h - 4) trim = bar_h - yy;  // 4,3,2,1
            vesa_hline(trim, yy, W - trim * 2, col);
        }
        // Subtle top highlight
        vesa_hline(3, 0, W - 6, 0x2A2240);
        // Bottom edge divider
        vesa_hline(4, bar_h - 1, W - 8, 0x2A2240);

        // ── Left: Nyx OS logo ──────────────────────────────────
        int lx = 14;
        // Small green dot (protection status)
        int prot_on = security_protection_status();
        uint32_t dot_c = prot_on ? 0x44DD66 : 0xFF4455;
        vesa_fill_rect(lx, 10, 7, 7, dot_c);
        lx += 12;
        vesa_draw_string(lx, 8, "Nyx OS", 0xDDD0F0, 0); lx += 56;

        // Username
        if (logged_in_user >= 0) {
            vesa_draw_string(lx, 8, users[logged_in_user].username, 0x9A8CC0, 0);
            int ul = 0; while (users[logged_in_user].username[ul]) ul++;
            lx += ul * 8 + 6;
        }

        // ── Center: date + time ────────────────────────────────
        char time_str[9]; char date_str[9];
        rtc_get_time_str(time_str); rtc_get_date_str(date_str);
        // Format: "Sep 1  00:53" (date + space + time)
        char center_str[24];
        int ci = 0;
        for (int i = 0; date_str[i] && i < 8; i++) center_str[ci++] = date_str[i];
        center_str[ci++] = ' ';
        center_str[ci++] = ' ';
        for (int i = 0; time_str[i] && i < 8; i++) center_str[ci++] = time_str[i];
        center_str[ci] = '\0';
        int ctw = ci * 8;
        vesa_draw_string(W/2 - ctw/2, 8, center_str, 0xDDD0F0, 0);

        // ── Right: status icons ────────────────────────────────
        // Layout from right edge: [battery%] [volume] [wifi]
        int rx = W - 16;

        // Battery percentage + icon (real SMBus data)
        int batt_pct = smbus_battery_percent();
        if (batt_pct < 0) batt_pct = 0;
        tb_icon_battery(rx - 20, 7, 0xB8A8D8, batt_pct);
        rx -= 42;
        // Percentage text (bright when low, muted otherwise)
        char batt_str[5];
        batt_str[0] = '0' + (batt_pct / 10) % 10;
        batt_str[1] = '0' + batt_pct % 10;
        batt_str[2] = '%';
        batt_str[3] = '\0';
        vesa_draw_string(rx, 8, batt_str,
                         batt_pct <= 20 ? 0xFF6B6B : 0xB8A8D8, 0);
        rx -= 30;

        // Volume icon (tracked for click detection)
        vol_popup_x = rx - 4;
        vol_popup_y = TASKBAR_H;
        tb_icon_volume(rx, 7, vol_popup_open ? 0xFFE08A : 0xB8A8D8);
        rx -= 24;

        // WiFi icon (tracked for click detection)
        wifi_popup_x = rx - 4;
        wifi_popup_y = TASKBAR_H;
        tb_icon_wifi(rx, 7, wifi_popup_open ? 0xFFE08A : 0xB8A8D8);
        rx -= 24;

        return;
    }

    // ── VGA text fallback ──
    vga_fill_row(0,' ',t->taskbar_fg,t->taskbar_bg);
    vga_puts(1,0,"* Nyx OS",t->taskbar_accent,t->taskbar_bg);
    uint8_t dot_fg = security_protection_status() ? 10 : 12;
    vga_put(9, 0, 'o', dot_fg, t->taskbar_bg);
    vga_put(10, 0, ' ', t->taskbar_fg, t->taskbar_bg);
    if(logged_in_user>=0) {
        vga_puts(11,0,users[logged_in_user].username,t->taskbar_fg,t->taskbar_bg);
        uint32_t last = mouse_last_activity();
        int grab = (last != 0 && (tick - last) < 54);
        int u_len = str_len(users[logged_in_user].username);
        int vga_col = 11 + u_len + 1;
        uint8_t dot_fg_grab = grab ? 10 : 8;
        vga_put(vga_col, 0, 'o', dot_fg_grab, t->taskbar_bg);
        vga_put(vga_col+1, 0, ' ', t->taskbar_fg, t->taskbar_bg);
        uint8_t lbl_fg = grab ? t->taskbar_accent : 8;
        vga_puts(vga_col+2, 0, grab ? "IN" : "--", lbl_fg, t->taskbar_bg);
        vga_put(vga_col+4, 0, ' ', t->taskbar_fg, t->taskbar_bg);
    }
    weather_draw_widget();
    char time_str[9];char date_str[9];
    rtc_get_time_str(time_str);rtc_get_date_str(date_str);
    vga_puts(52,0,date_str,t->taskbar_fg,t->taskbar_bg);
    vga_put(60,0,' ',t->taskbar_fg,t->taskbar_bg);
    vga_puts(61,0,time_str,t->taskbar_accent,t->taskbar_bg);
}

static void draw_wallpaper(){
    wallpaper_draw();
}

// ─── Tiny pixel-drawn music note for the status bar ───────────────
// Two connected note heads with stems and a joining bar, sized to
// fit within the status bar's 28px height alongside text.
// Forward declaration for the animated status-bar music note
static void sb_note(int x,int y,uint32_t c);

static void draw_statusbar(){
    const Theme*t=TH;

    if (vesa.active) {
        int W = (int)vesa.width;
        int H = (int)vesa.height;
        int bar_h = 28;
        int by = H - bar_h;

        // ── Glassmorphism background ──────────────────────────────
        // Frosted glass: vertical gradient from lighter purple at the
        // top (catching light) to deep purple at the bottom, with a
        // subtle dark hairline divider at the top edge (kept low-key
        // so it doesn't slice across the last row of desktop icons).
        for (int y = 0; y < bar_h; y++) {
            int t = y * 255 / bar_h;  // 0→255 top→bottom
            int r = 0x22 + (0x10 - 0x22) * t / 255;
            int g = 0x1A + (0x0C - 0x1A) * t / 255;
            int b = 0x50 + (0x30 - 0x50) * t / 255;
            vesa_hline(0, by + y, W, (r << 16) | (g << 8) | b);
        }
        // Top glass edge — subtle dark hairline divider (was a bright
        // lavender line that sliced across the last row of icons).
        vesa_hline(0, by, W, 0x241A52);
        vesa_hline(0, by + 1, W, 0x1A1140);
        // Bottom inner shadow
        vesa_hline(0, by + bar_h - 1, W, 0x060310);

        // ── Animated sweep highlight ──────────────────────────────
        // A glowing band that glides left-to-right across the bar,
        // bouncing when it reaches either edge.
        int sw_w = 100, sw_cycle = 65;
        int sp = (tick * 2) % (sw_cycle * 2);
        if (sp >= sw_cycle) sp = sw_cycle * 2 - sp;  // triangle bounce
        int sx = (W - sw_w) * sp / sw_cycle;
        for (int row = 0; row < 3; row++) {
            int row_y = by + 2 + row;
            if (row_y >= by + bar_h - 1) break;
            for (int dx = 0; dx < sw_w; dx++) {
                int dist = dx < 15 ? dx : (dx > sw_w - 15 ? sw_w - dx : 15);
                int fade = dist * 200 / 15; if (fade > 200) fade = 200;
                fade = fade * (3 - row) / 3;
                int fc = fade * 180 / 255 + 40;
                vesa_put_pixel(sx + dx, row_y, (fc << 16) | (fc * 170 / 255 << 8) | fc);
            }
        }

        int lx = 14;  // left-side drawing cursor
        int music_active = music_is_playing();

        // ── Beat-synced music note ────────────────────────────────
        if (music_active) {
            // note_progress 0–255: 0 = note just started (brightest),
            // 255 = note about to end (dim). Each new note triggers
            // a fresh bright flash that decays, syncing the glow to
            // the actual beat.
            int np = music_get_note_progress();
            int glow = (255 - np);  // 255→0 over the note's duration
            if (glow < 40) glow = 40;  // never go fully dark

            // Expand color range: interpolate between warm purple
            // (dim) and pure white-purple (brightest at note start)
            uint32_t c_dim = 0x8050B0, c_bright = 0xF0E8FF;
            int gr = ((c_dim>>16)&0xFF) + (((c_bright>>16)&0xFF)-((c_dim>>16)&0xFF)) * glow / 255;
            int gg = ((c_dim>>8)&0xFF)  + (((c_bright>>8)&0xFF)-((c_dim>>8)&0xFF))  * glow / 255;
            int gb = (c_dim&0xFF)       + ((c_bright&0xFF)-(c_dim&0xFF))            * glow / 255;
            uint32_t note_col = (gr << 16) | (gg << 8) | gb;

            // Glow halo behind the note: expands and fades with the beat
            if (glow > 60) {
                int halo_r = 1 + glow * 3 / 255;  // 1–4px halo
                for (int dy = -halo_r; dy <= halo_r; dy++) {
                    for (int dx = -halo_r; dx <= halo_r; dx++) {
                        if (dx * dx + dy * dy <= halo_r * halo_r) {
                            int hf = glow * (halo_r * 2 - (dx<0?-dx:dx) - (dy<0?-dy:dy)) / (halo_r * 4);
                            if (hf < 0) hf = 0;
                            uint32_t hc = (hf * 140 / 255 << 16) | (hf * 100 / 255 << 8) | hf;
                            vesa_put_pixel(lx + 6 + dx, by + 8 + dy, hc);
                        }
                    }
                }
            }

            sb_note(lx, by + 4, note_col);
            lx += 16;
        }

        // ── Spinner + status message ──────────────────────────────
        if (status_timer > 0 && status_msg[0]) {
            const char* spin = "-\\|/";
            char sc[2] = {spin[(tick / 4) % 4], '\0'};
            vesa_draw_string(lx, by + 6, sc, 0xB088FF, 0);
            lx += 12;
            vesa_draw_string(lx, by + 6, status_msg, 0xE8D8FF, 0);
        }

        // ── Real-time protection indicator ────────────────────────
        if (!security_protection_status() && (tick / 12) % 2 == 0) {
            vesa_draw_string(lx, by + 6, "[!]", 0xFF6644, 0);
            lx += 22;
            vesa_draw_string(lx, by + 6, "PROT OFF", 0xFF4455, 0);
        }

        // ── Version (right-aligned) ───────────────────────────────
        const char* label = "Nyx v0.1";
        int lw = str_len(label) * 8;
        // Version text pulses subtly with the beat when music is playing
        uint32_t ver_col = 0xB088FF;
        if (music_active && status_timer == 0) {
            int np = music_get_note_progress();
            int vg = 0xB0 + (0xD0 - 0xB0) * (255 - np) / 255;
            ver_col = (vg << 16) | (0x88 << 8) | (0xFF);
        }
        vesa_draw_string(W - lw - 16, by + 6, label, ver_col, 0);
        return;
    }

    // ── VGA text fallback ──
    vga_fill_row(VGA_HEIGHT-1,' ',t->status_fg,t->status_bg);
    int vx = 1;

    // Music: color-cycle the letter M
    if (music_is_playing()) {
        int ci = (tick / 6) % 4;
        uint8_t mcols[] = {5, 13, 15, 13};  // magenta→pink→white→pink
        uint8_t vga_c = mcols[ci];
        vga_put(vx, VGA_HEIGHT-1, 'M', vga_c, t->status_bg); vx += 2;
        vga_put(vx, VGA_HEIGHT-1, 14, vga_c, t->status_bg); vx++;  // ♫ note
    }

    const char* sb_spin = "-\\|/";
    if (status_timer > 0 && status_msg[0]) {
        vga_put(vx, VGA_HEIGHT-1, sb_spin[(tick/4)%4], t->accent, t->status_bg); vx += 2;
        vga_puts(vx, VGA_HEIGHT-1, status_msg, t->accent, t->status_bg);
    }
    // Blinking protection-off indicator
    if (!security_protection_status() && (tick / 12) % 2 == 0) {
        vga_puts(vx, VGA_HEIGHT-1, "[!]PROT OFF", 12, t->status_bg);
    }
    vga_puts(66,VGA_HEIGHT-1,"Nyx v0.1",t->accent,t->status_bg);
}

// ─── Icon grid (pixel-drawn tiles) ─────────────────────────────────
// Each icon used to be ASCII text in brackets (e.g. "[$_]") rendered
// through the VGA-text-emulation layer. That reads as "blocky" next
// to the smooth lunar logo on the splash/login screens, so icons are
// now drawn as real pixel tiles: a rounded gradient box with a top
// light-sheen, a thin glowing border, and a hand-drawn line-art
// symbol unique to each app. Labels stay on the character grid,
// since text is what that grid renders best.

// Per-icon accent color, now full 24-bit RGB (no longer constrained
// to the 16-color VGA palette) so the line art can glow properly.
// Yaru/GNOME-inspired accent palette (these drive the tile hues; the
// tiles themselves are derived at ~80% strength + desaturation, so the
// grid reads as a soft GNOME app grid rather than a neon wall).
static const uint32_t icon_glyph_rgb[]={
    0x2EC27E, // Terminal  - Yaru/GNOME green
    0xE95420, // Files     - Ubuntu/Yaru orange
    0x9B6DF2, // Editor    - soft violet
    0x57C16A, // AppLib    - leaf green
    0xE35D6A, // Trash     - muted coral red
    0x7A8699, // Settings  - cool slate grey
    0xA56DE2, // Music     - Yaru purple
    0x3D9BFF, // Calc      - Yaru blue
    0xE8823C, // Games     - warm amber orange
    0x4E9BD8, // Paint     - sky blue
    0x2FB39C, // Monitor   - teal
    0x4C8FF7, // Clock     - Yaru blue
    0xE05C4C, // Calendar  - soft red
    0x57A7E8, // Weather   - sky blue
    0xCE7AA8, // Organizer - rose gold
    0xE5484D, // Security  - alert red
    0xEF9E3B, // Gallery   - warm gold
    0x8A63D2, // Browser   - Yaru purple
    0x3EC6B0, // QuickSettings - teal
    0xD84315, // Doom      - classic DOOM fire red-orange
};

// ── Pixel primitives (local to icon drawing) ───────────────────────
static void icon_circ(int cx,int cy,int r,uint32_t c){
    for(int dy=-r;dy<=r;dy++)
        for(int dx=-r;dx<=r;dx++)
            if(dx*dx+dy*dy<=r*r)
                vesa_put_pixel(cx+dx,cy+dy,c);
}
static void icon_ring(int cx,int cy,int r,int t,uint32_t c){
    int ri=r-t;
    for(int dy=-r;dy<=r;dy++)
        for(int dx=-r;dx<=r;dx++){
            int d=dx*dx+dy*dy;
            if(d<=r*r&&d>=ri*ri)
                vesa_put_pixel(cx+dx,cy+dy,c);
        }
}

// ─── Tiny music note for the animated status-bar widget ───────────
static void sb_note(int x,int y,uint32_t c){
    icon_circ(x+2,y+7,2,c);
    icon_circ(x+7,y+5,2,c);
    for(int dy=-2;dy<=8;dy++)vesa_put_pixel(x+3,y+dy,c);
    for(int dy=-2;dy<=6;dy++)vesa_put_pixel(x+8,y+dy,c);
    vesa_hline(x+3,y-2,6,c);
}

// Thick line via Bresenham-ish stepping + a small square stamp,
// since we have no native line primitive with width.
static void icon_line(int x0,int y0,int x1,int y1,int w,uint32_t c){
    int dx=x1-x0, dy=y1-y0;
    int steps = (dx<0?-dx:dx) > (dy<0?-dy:dy) ? (dx<0?-dx:dx) : (dy<0?-dy:dy);
    if (steps==0) steps=1;
    int hw = w/2; if (hw<1) hw=1;
    for(int i=0;i<=steps;i++){
        int x = x0 + dx*i/steps;
        int y = y0 + dy*i/steps;
        for(int oy=-hw;oy<=hw;oy++)
            for(int ox=-hw;ox<=hw;ox++)
                vesa_put_pixel(x+ox,y+oy,c);
    }
}
static void icon_rrect_border(int x,int y,int w,int h,uint32_t c){
    vesa_hline(x+2,y,w-4,c); vesa_hline(x+2,y+h-1,w-4,c);
    vesa_vline(x,y+2,h-4,c); vesa_vline(x+w-1,y+2,h-4,c);
    vesa_put_pixel(x+1,y+1,c);   vesa_put_pixel(x+w-2,y+1,c);
    vesa_put_pixel(x+1,y+h-2,c); vesa_put_pixel(x+w-2,y+h-2,c);
}
// Rounded-corner vertical gradient: combines the corner-cut shape
// of icon_rrect with a top-to-bottom color blend in a single pass,
// so the tile body is both rounded AND gradient-filled (rather than
// a sharp rectangle, which icon_hgrad alone would produce).
static void icon_rrect_grad(int x,int y,int w,int h,uint32_t c1,uint32_t c2){
    for(int dy=0;dy<h;dy++){
        uint32_t r=((c1>>16)&0xFF)+(int)(((c2>>16)&0xFF)-((c1>>16)&0xFF))*dy/h;
        uint32_t g=((c1>>8)&0xFF)+(int)(((c2>>8)&0xFF)-((c1>>8)&0xFF))*dy/h;
        uint32_t b=(c1&0xFF)+(int)((c2&0xFF)-(c1&0xFF))*dy/h;
        uint32_t col=(r<<16)|(g<<8)|b;
        // Inset the row by 2px on the corner rows (top/bottom 2px),
        // matching icon_rrect's corner-cut pattern.
        if (dy < 2 || dy >= h-2)
            vesa_hline(x+2,y+dy,w-4,col);
        else
            vesa_hline(x,y+dy,w,col);
    }
}

// ─── Proper rounded-corner tiles ────────────────────────────────
// The desktop tiles host round squircle SVG icons (corner radius
// ~22% of the tile), so the tiles themselves get a real circular
// corner profile instead of the 2px cuts above.  ICON_TILE_R is the
// corner radius in px; the small 2px helpers stay for menus/windows.
// Integer sqrt (Newton) — no FPU/libm in the kernel.
static int icon_isqrt(int n) {
    int x = n, y = (x + 1) / 2;
    while (y < x) { x = y; y = (x + n / x) / 2; }
    return x;
}

// Horizontal inset (px from each edge) of a circular corner at row
// dy (0-based from the top/bottom edge) for corner radius r.
// Circle arc: x = r − sqrt(r² − (r−dy)²).  Returns 0 past the corner.
static int icon_rr_inset(int dy, int r) {
    if (dy <= 0) return r;
    if (dy >= r) return 0;
    int d = r - dy;
    return r - icon_isqrt(r * r - d * d);
}

static void icon_rrect_grad_r(int x,int y,int w,int h,int r,
                              uint32_t c1,uint32_t c2){
    if (r < 1) r = 1;
    if (r > w/2) r = w/2;
    if (r > h/2) r = h/2;
    for(int dy=0;dy<h;dy++){
        uint32_t rr=((c1>>16)&0xFF)+(int)(((c2>>16)&0xFF)-((c1>>16)&0xFF))*dy/h;
        uint32_t gg=((c1>>8)&0xFF)+(int)(((c2>>8)&0xFF)-((c1>>8)&0xFF))*dy/h;
        uint32_t bb=(c1&0xFF)+(int)((c2&0xFF)-(c1&0xFF))*dy/h;
        uint32_t col=(rr<<16)|(gg<<8)|bb;
        int ins = 0;
        if (dy < r) ins = icon_rr_inset(dy, r);
        else if (dy >= h - r) ins = icon_rr_inset(h - 1 - dy, r);
        vesa_hline(x + ins, y + dy, w - 2*ins, col);
    }
}

static void icon_rrect_border_r(int x,int y,int w,int h,int r,uint32_t c){
    if (r < 1) r = 1;
    if (r > w/2) r = w/2;
    if (r > h/2) r = h/2;
    // Straight edges (top/bottom/left/right)
    vesa_hline(x+r, y, w-2*r, c);
    vesa_hline(x+r, y+h-1, w-2*r, c);
    vesa_vline(x, y+r, h-2*r, c);
    vesa_vline(x+w-1, y+r, h-2*r, c);
    // Corner arcs (mirrored top and bottom)
    for (int dy = 0; dy < r; dy++) {
        int ins = icon_rr_inset(dy, r);
        vesa_put_pixel(x+ins, y+dy, c);
        vesa_put_pixel(x+w-1-ins, y+dy, c);
        vesa_put_pixel(x+ins, y+h-1-dy, c);
        vesa_put_pixel(x+w-1-ins, y+h-1-dy, c);
    }
}

// ── Symbol drawing: each app gets its own hand-drawn line-art icon.
// Coordinates are relative to (cx,cy) = the tile's center, on a
// roughly -10..+10 canvas (tile is 32x32px, symbol fits ~24x24px).
static void sym_terminal(int cx,int cy,uint32_t c){
    icon_rrect_border(cx-11,cy-9,22,18,c);
    icon_line(cx-6,cy-3,cx-2,cy,2,c);
    icon_line(cx-2,cy,cx-6,cy+3,2,c);
    icon_line(cx,cy+4,cx+6,cy+4,2,c);
}
static void sym_files(int cx,int cy,uint32_t c){
    icon_line(cx-11,cy-5,cx-11,cy+8,2,c);
    icon_line(cx-11,cy+8,cx+11,cy+8,2,c);
    icon_line(cx+11,cy+8,cx+11,cy-1,2,c);
    icon_line(cx+11,cy-1,cx,cy-1,2,c);
    icon_line(cx,cy-1,cx-3,cy-5,2,c);
    icon_line(cx-3,cy-5,cx-11,cy-5,2,c);
}
static void sym_editor(int cx,int cy,uint32_t c){
    icon_line(cx-8,cy+8,cx-7,cy+3,2,c);
    icon_line(cx-7,cy+3,cx+5,cy-9,2,c);
    icon_line(cx+5,cy-9,cx+9,cy-5,2,c);
    icon_line(cx+9,cy-5,cx-3,cy+7,2,c);
    icon_line(cx-3,cy+7,cx-8,cy+8,2,c);
    icon_line(cx+2,cy-6,cx+6,cy-2,1,c);
}
static void sym_applib(int cx,int cy,uint32_t c){
    icon_rrect_border(cx-10,cy-10,8,8,c);
    icon_rrect_border(cx+2,cy-10,8,8,c);
    icon_rrect_border(cx-10,cy+2,8,8,c);
    icon_rrect_border(cx+2,cy+2,8,8,c);
}
static void sym_trash(int cx,int cy,uint32_t c){
    icon_line(cx-7,cy-7,cx+7,cy-7,2,c);
    icon_line(cx-3,cy-9,cx+3,cy-9,2,c);
    icon_line(cx-6,cy-7,cx-5,cy+9,2,c);
    icon_line(cx-5,cy+9,cx+5,cy+9,2,c);
    icon_line(cx+5,cy+9,cx+6,cy-7,2,c);
}
static void sym_settings(int cx,int cy,uint32_t c){
    icon_ring(cx,cy,5,2,c);
    icon_line(cx,cy-9,cx,cy-6,2,c);
    icon_line(cx,cy+6,cx,cy+9,2,c);
    icon_line(cx-9,cy,cx-6,cy,2,c);
    icon_line(cx+6,cy,cx+9,cy,2,c);
    icon_line(cx-6,cy-6,cx-4,cy-4,2,c);
    icon_line(cx+4,cy+4,cx+6,cy+6,2,c);
    icon_line(cx-6,cy+6,cx-4,cy+4,2,c);
    icon_line(cx+4,cy-4,cx+6,cy-6,2,c);
}
static void sym_music(int cx,int cy,uint32_t c){
    icon_circ(cx-7,cy+6,3,c);
    icon_circ(cx+6,cy+3,3,c);
    icon_line(cx-4,cy+6,cx-4,cy-7,2,c);
    icon_line(cx+9,cy+3,cx+9,cy-8,2,c);
    icon_line(cx-4,cy-7,cx+9,cy-8,2,c);
}
static void sym_calc(int cx,int cy,uint32_t c){
    icon_rrect_border(cx-10,cy-9,20,18,c);
    icon_line(cx-10,cy-4,cx+10,cy-4,1,c);
    icon_line(cx-4,cy-4,cx-4,cy+9,1,c);
    icon_line(cx+2,cy-4,cx+2,cy+9,1,c);
}
static void sym_games(int cx,int cy,uint32_t c){
    static const int sx[10]={0,3,10,5,7,0,-7,-5,-10,-3};
    static const int sy[10]={-10,-3,-3,2,9,5,9,2,-3,-3};
    for(int i=0;i<10;i++){
        int j=(i+1)%10;
        icon_line(cx+sx[i],cy+sy[i],cx+sx[j],cy+sy[j],1,c);
    }
}
static void sym_paint(int cx,int cy,uint32_t c){
    icon_line(cx-7,cy+9,cx-4,cy+3,2,c);
    icon_line(cx-4,cy+3,cx+6,cy-8,2,c);
    icon_line(cx+6,cy-8,cx+9,cy-5,2,c);
    icon_line(cx+9,cy-5,cx-1,cy+6,2,c);
    icon_line(cx-1,cy+6,cx-7,cy+9,2,c);
    icon_circ(cx+6,cy-8,2,c);
}
static void sym_monitor(int cx,int cy,uint32_t c){
    icon_rrect_border(cx-11,cy-8,22,14,c);
    icon_line(cx-7,cy-9,cx+7,cy-9,1,c);
    icon_line(cx-7,cy+2,cx-3,cy-3,1,c);
    icon_line(cx-3,cy-3,cx+1,cy,1,c);
    icon_line(cx+1,cy,cx+7,cy-6,1,c);
    icon_line(cx-4,cy+9,cx+4,cy+9,2,c);
}
static void sym_clock(int cx,int cy,uint32_t c){
    icon_ring(cx,cy,10,2,c);
    icon_line(cx,cy,cx,cy-6,2,c);
    icon_line(cx,cy,cx+4,cy+2,2,c);
}
static void sym_calendar(int cx,int cy,uint32_t c){
    icon_rrect_border(cx-10,cy-7,20,17,c);
    icon_line(cx-10,cy-2,cx+10,cy-2,1,c);
    icon_line(cx-5,cy-10,cx-5,cy-5,2,c);
    icon_line(cx+5,cy-10,cx+5,cy-5,2,c);
}
static void sym_weather(int cx,int cy,uint32_t c, uint32_t sun){
    icon_ring(cx+5,cy-5,3,2,sun);
    icon_line(cx-7,cy+8,cx-4,cy-1,2,c);
    icon_line(cx-4,cy-1,cx+1,cy+1,2,c);
    icon_line(cx+1,cy+1,cx+5,cy-3,2,c);
    icon_line(cx+5,cy-3,cx+9,cy+8,2,c);
    icon_line(cx-7,cy+8,cx+9,cy+8,2,c);
}

static void sym_organizer(int cx,int cy,uint32_t c){
    // A grid of small squares symbolizing groups/folders organization
    // Two stacked rows of 3 small boxes each
    icon_rrect_border(cx-10,cy-10,9,9,c);
    icon_rrect_border(cx+1,cy-10,9,9,c);
    icon_rrect_border(cx-10,cy+2,9,9,c);
    icon_rrect_border(cx+1,cy+2,9,9,c);
    icon_rrect_border(cx-5,cy-5,10,10,0xFFFFFF); // center highlight
}

// Picture frame symbol for Gallery app — a frame with a landscape
static void sym_gallery(int cx,int cy,uint32_t c){
    // Outer frame
    icon_rrect_border(cx-11,cy-9,22,18,c);
    // Inner landscape: mountain
    icon_line(cx-6,cy+5,cx-2,cy-2,2,c);   // left mountain slope
    icon_line(cx-2,cy-2,cx+3,cy+3,2,c);   // right mountain slope
    icon_line(cx+3,cy+3,cx+7,cy-1,2,c);   // small hill
    icon_line(cx-6,cy+5,cx+7,cy+5,2,c);   // ground line
    // Sun in corner
    icon_circ(cx+5,cy-5,2,0xFFD166);
}

// Crescent moon symbol for Browser app — a purple half-moon
static void sym_browser(int cx,int cy,uint32_t c){
    // Outer arc: ring offset left to form the moon's outer curve
    icon_ring(cx-2, cy, 10, 2, c);
    // Inner arc: smaller ring offset right to carve the crescent
    icon_ring(cx+2, cy, 6, 2, c);
    // Star accents: tiny dots near the moon
    icon_circ(cx-7, cy-9, 1, 0xFFDD88);
    icon_circ(cx+3, cy-10, 1, 0xFFDD88);
    icon_circ(cx+8, cy-4, 1, 0xFFDD88);
}

// Shield symbol for Security app — a simple rounded shield shape
static void sym_security(int cx,int cy,uint32_t c){
    // Shield body (trapezoid + arc)
    icon_line(cx-10,cy-2,cx-10,cy+4,2,c);
    icon_line(cx+10,cy-2,cx+10,cy+4,2,c);
    icon_line(cx-10,cy-2,cx-6,cy-9,2,c);
    icon_line(cx+10,cy-2,cx+6,cy-9,2,c);
    icon_line(cx-6,cy-9,cx+6,cy-9,2,c);
    icon_circ(cx,cy+4,6,c);
    // Checkmark inside
    icon_line(cx-4,cy+3,cx-1,cy+6,2,c);
    icon_line(cx-1,cy+6,cx+5,cy-1,2,c);
}

// Three horizontal slider bars with knobs — Quick Settings icon
static void sym_quicksettings(int cx,int cy,uint32_t c){
    // Top slider: short bar + knob
    icon_line(cx-9,cy-7,cx+5,cy-7,2,c);
    icon_circ(cx+5,cy-7,3,c);
    // Middle slider: medium bar + knob
    icon_line(cx-9,cy-1,cx+8,cy-1,2,c);
    icon_circ(cx+8,cy-1,3,c);
    // Bottom slider: long bar + knob
    icon_line(cx-9,cy+5,cx+3,cy+5,2,c);
    icon_circ(cx+3,cy+5,3,c);
}

static void sym_doom(int cx,int cy,uint32_t c){
    // Cacodemon: big sphere, one wide eye, jagged horns.
    icon_circ(cx,cy,11,c);                       // body
    // Horns (two short spikes at 10 and 2 o'clock)
    icon_line(cx-8,cy-8,cx-11,cy-13,2,c);
    icon_line(cx-11,cy-13,cx-6,cy-11,1,c);
    icon_line(cx+8,cy-8,cx+11,cy-13,2,c);
    icon_line(cx+11,cy-13,cx+6,cy-11,1,c);
    // Eye: almond outline + pupil
    icon_line(cx-6,cy-2,cx+6,cy-2,2,c);
    icon_line(cx-6,cy-2,cx-4,cy-5,1,c);
    icon_line(cx+6,cy-2,cx+4,cy-5,1,c);
    icon_circ(cx,cy-2,2,c);                      // pupil
    // Grin: three downward fangs
    icon_line(cx-6,cy+6,cx-3,cy+9,2,c);
    icon_line(cx-1,cy+7,cx+1,cy+10,2,c);
    icon_line(cx+4,cy+6,cx+6,cy+9,2,c);
}

// Chess icon: a pawn — head ball, collar flare, tapered body, base.
static void sym_chess(int cx,int cy,uint32_t c){
    icon_circ(cx,cy-5,4,c);                    // head
    icon_line(cx-6,cy+1,cx+6,cy+1,2,c);        // collar
    icon_line(cx-4,cy+1,cx-6,cy+7,2,c);        // tapered body
    icon_line(cx+4,cy+1,cx+6,cy+7,2,c);
    icon_line(cx-8,cy+9,cx+8,cy+9,3,c);        // base
}

static void draw_icon_symbol(int idx,int cx,int cy,uint32_t c){
    switch(idx){
        case 0:  sym_terminal(cx,cy,c);   break;
        case 1:  sym_files(cx,cy,c);      break;
        case 2:  sym_editor(cx,cy,c);     break;
        case 3:  sym_applib(cx,cy,c);     break;
        case 4:  sym_trash(cx,cy,c);      break;
        case 5:  sym_settings(cx,cy,c);   break;
        case 6:  sym_music(cx,cy,c);      break;
        case 7:  sym_calc(cx,cy,c);       break;
        case 8:  sym_games(cx,cy,c);      break;
        case 9:  sym_paint(cx,cy,c);      break;
        case 10: sym_monitor(cx,cy,c);    break;
        case 11: sym_clock(cx,cy,c);      break;
        case 12: sym_calendar(cx,cy,c);   break;
        case 13: sym_weather(cx,cy,c,0xFFD166); break;
        case 14: sym_organizer(cx,cy,c);      break;
        case 15: sym_security(cx,cy,c);       break;
        case 16: sym_gallery(cx,cy,c);        break;
        case 17: sym_browser(cx,cy,c);        break;
        case 18: sym_quicksettings(cx,cy,c);  break;
        case 19: sym_doom(cx,cy,c);           break;
        case 20: sym_chess(cx,cy,c);          break;
        default: break;
    }
}

// ── Icon color helpers ─────────────────────────────────────────────
// Scale an RGB color toward black (f=255 → unchanged, 0 → black).
static uint32_t icon_scale(uint32_t c, int f) {
    uint32_t r = ((c >> 16) & 0xFF) * (uint32_t)f / 255;
    uint32_t g = ((c >> 8) & 0xFF) * (uint32_t)f / 255;
    uint32_t b = (c & 0xFF) * (uint32_t)f / 255;
    return (r << 16) | (g << 8) | b;
}
// Blend an RGB color toward white (a=0 → unchanged, 255 → white).
static uint32_t icon_lighten(uint32_t c, int a) {
    uint32_t r = ((c >> 16) & 0xFF) + (255 - ((c >> 16) & 0xFF)) * (uint32_t)a / 255;
    uint32_t g = ((c >> 8) & 0xFF) + (255 - ((c >> 8) & 0xFF)) * (uint32_t)a / 255;
    uint32_t b = (c & 0xFF) + (255 - (c & 0xFF)) * (uint32_t)a / 255;
    return (r << 16) | (g << 8) | b;
}

// ─── Custom icon pictures ──────────────────────────────────────────
// Drop an image named after an app into host-files/ (any subfolder —
// e.g. host-files/icons/terminal.png) and it gets embedded at build
// time.  The desktop tile for that app then shows the picture instead
// of the vector glyph.  Matching is case-insensitive on the basename
// without the extension ("terminal.png" → Terminal, "FILES.JPG" →
// Files).  PNG / JPEG / BMP / GIF are all supported.  Images are
// decoded lazily, once per boot, and cached as a small RGBA thumb.
#define ICON_IMG_MAX_DIM  1024                // reject larger sources
// Decode workspace — 1024x1024x4 = 4 MB static BSS.  Acceptable next
// to the existing 16 MB filesystem[] and ~25 MB gallery buffers.
#define ICON_IMG_SCRATCH  (ICON_IMG_MAX_DIM * ICON_IMG_MAX_DIM * 4)
#define ICON_IMG_PAD      4                   // inset inside the tile
#define ICON_IMG_W        (ICON_TILE_W - 2 * ICON_IMG_PAD)   // 56
#define ICON_IMG_H        (ICON_TILE_H - 2 * ICON_IMG_PAD)   // 56
#define ICON_IMG_SZ       (ICON_IMG_W * ICON_IMG_H * 4)

typedef enum { ICON_IMG_NONE, ICON_IMG_PNG, ICON_IMG_JPG,
               ICON_IMG_BMP, ICON_IMG_GIF, ICON_IMG_SVG } IconImgType;

static uint8_t icon_img_scratch[ICON_IMG_SCRATCH] __attribute__((aligned(64)));  // decode workspace
static uint8_t icon_img_thumb[DESKTOP_APP_COUNT][ICON_IMG_SZ]; // cached thumbs
static int     icon_img_ok[DESKTOP_APP_COUNT];      // 1 = picture available
// Aspect-fit content rect inside the 56x56 thumb (opaque region only)
// — lets the draw path use a single bulk blit instead of 3136
// per-pixel calls per icon.
static int     icon_img_cw[DESKTOP_APP_COUNT];
static int     icon_img_ch[DESKTOP_APP_COUNT];
static int     icon_img_cox[DESKTOP_APP_COUNT];
static int     icon_img_coy[DESKTOP_APP_COUNT];
static int     icon_img_scan_count = -1;            // fingerprint at last scan
static int     icon_img_scan_fi = 0;                // cursor: next file index
static int     icon_img_scan_pending = 0;           // 1 = incremental scan in progress

// Cheap fingerprint of the file set (count + sizes) so no-op saves
// don't trigger a wasteful full re-decode of every app picture.
static int icon_img_fp(void) {
    int fp = file_count;
    for (int i = 0; i < MAX_FILES; i++)
        if (filesystem[i].used) fp += filesystem[i].size;
    return fp;
}

// Does filename `name` (with extension) match app name `app` (no
// extension), case-insensitively?  "terminal.png" vs "Terminal" → 1.
static int icon_img_matches(const char* name, const char* app) {
    int nl = 0, al = 0;
    while (name[nl]) nl++;
    while (app[al]) al++;
    int dot = nl;                      // strip extension
    for (int i = 0; i < nl; i++) if (name[i] == '.') dot = i;
    if (dot != al) return 0;           // ext must start right after app name
    for (int i = 0; i < al; i++) {
        char a = name[i], b = app[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return 1;
}

static IconImgType icon_img_type(const char* name) {
    int nl = 0; while (name[nl]) nl++;
    int dot = -1;
    for (int i = 0; i < nl; i++) if (name[i] == '.') dot = i;
    if (dot < 0) return ICON_IMG_NONE;
    char e[6]; int ei = 0;
    for (int i = dot + 1; name[i] && ei < 5; i++) {
        char ch = name[i];
        if (ch >= 'A' && ch <= 'Z') ch += 32;
        e[ei++] = ch;
    }
    e[ei] = '\0';
    if (ei == 3 && e[0]=='p' && e[1]=='n' && e[2]=='g') return ICON_IMG_PNG;
    if (ei == 3 && e[0]=='b' && e[1]=='m' && e[2]=='p') return ICON_IMG_BMP;
    if (ei == 3 && e[0]=='g' && e[1]=='i' && e[2]=='f') return ICON_IMG_GIF;
    if (ei == 3 && e[0]=='s' && e[1]=='v' && e[2]=='g') return ICON_IMG_SVG;
    if ((ei == 3 && e[0]=='j' && e[1]=='p' && e[2]=='g') ||
        (ei == 4 && e[0]=='j' && e[1]=='p' && e[2]=='e' && e[3]=='g'))
        return ICON_IMG_JPG;
    return ICON_IMG_NONE;
}

// Quick header scan for image dimensions (mirrors gallery's check)
// so oversized files are rejected BEFORE the decoder writes past the
// scratch buffer.
static int icon_img_dims(const uint8_t* d, int n, int* w, int* h) {
    if (!d || n < 8) return 0;
    if (d[0]==0x89 && d[1]=='P' && d[2]=='N' && d[3]=='G') {
        if (n < 24) return 0;
        *w = ((int)d[16]<<24)|((int)d[17]<<16)|((int)d[18]<<8)|d[19];
        *h = ((int)d[20]<<24)|((int)d[21]<<16)|((int)d[22]<<8)|d[23];
        return (*w > 0 && *h > 0);
    }
    if (d[0]==0xFF && d[1]==0xD8 && d[2]==0xFF) {
        int pos = 2;
        while (pos + 1 < n) {
            if (d[pos] != 0xFF) { pos++; continue; }
            while (pos < n && d[pos] == 0xFF) pos++;
            if (pos >= n) break;
            uint8_t marker = d[pos++];
            if (marker == 0xD9) break;
            if (marker == 0x00 || marker == 0xD8) continue;
            if (pos + 1 >= n) break;
            int seg = ((int)d[pos]<<8)|(int)d[pos+1];
            if (seg < 2) continue;
            if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2) {
                if (seg < 7) return 0;
                *h = ((int)d[pos+3]<<8)|d[pos+4];
                *w = ((int)d[pos+5]<<8)|d[pos+6];
                return (*w > 0 && *h > 0);
            }
            pos += seg;
        }
    }
    if (d[0]=='B' && d[1]=='M' && n >= 26) {
        uint32_t hsz = (uint32_t)d[14]|((uint32_t)d[15]<<8)|((uint32_t)d[16]<<16)|((uint32_t)d[17]<<24);
        if (hsz == 12) {
            *w = (int)d[18]|((int)d[19]<<8);
            *h = (int)d[20]|((int)d[21]<<8);
            if (*h < 0) *h = -*h;
            return (*w > 0 && *h > 0);
        } else if (hsz >= 40) {
            *w = (int)(d[18]|((int)d[19]<<8)|((int)d[20]<<16)|((int)d[21]<<24));
            *h = (int)(d[22]|((int)d[23]<<8)|((int)d[24]<<16)|((int)d[25]<<24));
            if (*h < 0) *h = -*h;
            return (*w > 0 && *h > 0 && *w <= 4096 && *h <= 4096);
        }
    }
    if (d[0]=='G' && d[1]=='I' && d[2]=='F' && n >= 10) {
        *w = (int)d[6]|((int)d[7]<<8);
        *h = (int)d[8]|((int)d[9]<<8);
        return (*w > 0 && *h > 0 && *w <= 4096 && *h <= 4096);
    }
    // SVG: text-based, ask the decoder for the declared dimensions
    if (n >= 4 && d[0]=='<' && (d[1]=='s'||d[1]=='S') &&
        (d[2]=='v'||d[2]=='V') && (d[3]=='g'||d[3]=='G'))
        return svg_dims(d, n, w, h);
    return 0;
}

// Begin (or restart) a lazy scan for app-named pictures.  Cheap:
// it only stamps the fingerprint and clears the "decoded" flags.
// The slow decode work happens one picture at a time in
// icon_img_scan_step(), driven from desktop_heartbeat() so the very
// first desktop frame after login renders instantly with vector
// glyphs instead of blocking for seconds on SVG/PNG decode.
static void icon_img_scan_begin(void) {
    icon_img_scan_count = icon_img_fp();
    for (int a = 0; a < DESKTOP_APP_COUNT; a++) {
        icon_img_ok[a] = 0;
        icon_img_cw[a] = icon_img_ch[a] = 0;
        icon_img_cox[a] = icon_img_coy[a] = 0;
    }
    icon_img_scan_fi = 0;
    icon_img_scan_pending = 1;
}

// Decode at most ONE app picture (the slow part) and return 1 if a
// picture was decoded this call, 0 when the scan is complete or not
// pending.  Called once per PIT tick from desktop_heartbeat() so the
// desktop stays responsive while pictures pop in progressively.
static int icon_img_scan_step(void) {
    if (!icon_img_scan_pending) return 0;
    for (; icon_img_scan_fi < MAX_FILES; icon_img_scan_fi++) {
        int fi = icon_img_scan_fi;
        if (!filesystem[fi].used) continue;
        IconImgType t = icon_img_type(filesystem[fi].name);
        if (t == ICON_IMG_NONE) continue;

        // A file's basename matches at most one app name, and each
        // app is decoded at most once, so find the first undecoded
        // app this file pictures (if any).
        int target = -1;
        for (int a = 0; a < DESKTOP_APP_COUNT; a++) {
            if (icon_img_ok[a]) continue;
            if (icon_img_matches(filesystem[fi].name, app_names[a])) { target = a; break; }
        }
        if (target < 0) continue;

        const uint8_t* c = (const uint8_t*)filesystem[fi].content;
        int clen = filesystem[fi].size;
        if (clen <= 0) { while (c[clen]) clen++; }
        int iw = 0, ih = 0;
        if (!icon_img_dims(c, clen, &iw, &ih)) continue;
        if (iw <= 0 || ih <= 0 || iw > ICON_IMG_MAX_DIM || ih > ICON_IMG_MAX_DIM) continue;
        if (iw * ih * 4 > ICON_IMG_SCRATCH) continue;

        int ok = 0;
        if (t == ICON_IMG_PNG) {
            PngImage img; img.pixels = icon_img_scratch;
            ok = png_decode(c, clen, &img);
            if (ok) { iw = (int)img.width; ih = (int)img.height; }
        } else if (t == ICON_IMG_BMP) {
            BmpImage img; img.pixels = icon_img_scratch;
            ok = bmp_decode(c, clen, &img);
            if (ok) { iw = (int)img.width; ih = (int)img.height; }
        } else if (t == ICON_IMG_GIF) {
            GifImage img; img.pixels = icon_img_scratch;
            ok = gif_decode(c, clen, &img);
            if (ok) { iw = (int)img.width; ih = (int)img.height; }
        } else if (t == ICON_IMG_SVG) {
            SvgImage img; img.pixels = icon_img_scratch;
            ok = svg_decode(c, clen, &img);
            if (ok) { iw = (int)img.width; ih = (int)img.height; }
        } else {
            JpegImage img = {0}; img.pixels = icon_img_scratch;
            ok = jpeg_decode(c, clen, &img);
            if (ok) { iw = (int)img.width; ih = (int)img.height; }
        }
        if (!ok || iw <= 0 || ih <= 0 || iw * ih * 4 > ICON_IMG_SCRATCH) continue;

        // Downscale to a 56x56 thumb, aspect-fit and centered,
        // with transparent letterbox pixels (alpha 0 = skip).
        uint8_t* dst = icon_img_thumb[target];
        for (int i = 0; i < ICON_IMG_SZ; i++) dst[i] = 0;
        int dw = ICON_IMG_W, dh = ICON_IMG_H;
        if (iw * dh > ih * dw) dh = ih * dw / iw;
        else                   dw = iw * dh / ih;
        int ox = (ICON_IMG_W - dw) / 2, oy = (ICON_IMG_H - dh) / 2;
        for (int y = 0; y < dh; y++)
            for (int x = 0; x < dw; x++) {
                int sx = x * iw / dw, sy = y * ih / dh;
                if (sx >= iw) sx = iw - 1;
                if (sy >= ih) sy = ih - 1;
                int si = (sy * iw + sx) * 4;
                int di = ((oy + y) * ICON_IMG_W + ox + x) * 4;
                dst[di]   = icon_img_scratch[si];
                dst[di+1] = icon_img_scratch[si+1];
                dst[di+2] = icon_img_scratch[si+2];
                // Preserve real transparency only for formats that
                // carry a meaningful alpha channel: SVG squircle
                // corners and PNG alpha.  JPEG/GIF always decode
                // opaque, and 32-bit BMPs frequently store 0 in
                // the unused alpha byte — preserving that would
                // make the whole icon vanish — so both keep the
                // forced-255 behavior.
                dst[di+3] = (t == ICON_IMG_SVG || t == ICON_IMG_PNG)
                                ? icon_img_scratch[si+3] : 255;
            }
        icon_img_cw[target] = dw; icon_img_ch[target] = dh;
        icon_img_cox[target] = ox; icon_img_coy[target] = oy;
        icon_img_ok[target] = 1;
        icon_img_scan_fi++;   // move past this file
        return 1;             // one picture decoded this call
    }
    icon_img_scan_pending = 0;
    return 0;
}

// Blit the opaque (aspect-fit) region of a cached 56x56 RGBA thumb
// onto the tile — a single bulk blit.  The transparent letterbox
// stays glass, so no per-pixel loop is needed.
// Alpha-composite the aspect-fit region of a cached 56x56 RGBA thumb
// onto the tile.  Transparent pixels (the SVG squircle corners, the
// PNG letterbox) let the rounded tile border and glass body show
// through, so the glyph never paints a black square over the tile's
// rounded corner arc.
static void icon_img_blit(int px, int py, int idx) {
    int ox = icon_img_cox[idx], oy = icon_img_coy[idx];
    int cw = icon_img_cw[idx], ch = icon_img_ch[idx];
    const uint8_t* src = icon_img_thumb[idx];
    int fb_bpp = (int)(vesa.bpp / 8);
    int fb_w = (int)vesa.width, fb_h = (int)vesa.height;

    for (int y = 0; y < ch; y++) {
        int dy = py + oy + y;
        if (dy < 0 || dy >= fb_h) continue;
        for (int x = 0; x < cw; x++) {
            int dx = px + ox + x;
            if (dx < 0 || dx >= fb_w) continue;
            // The thumb stores the aspect-fit image at rows oy..oy+ch,
            // cols ox..ox+cw (letterbox outside), so the source index
            // must include that offset — not (0,0).
            int si = (((oy + y) * ICON_IMG_W) + (ox + x)) * 4;
            uint8_t a = src[si + 3];
            if (a == 0) continue;   // transparent: keep tile underneath
            uint8_t* d = vesa.draw +
                (uint32_t)dy * vesa.pitch +
                (uint32_t)dx * (uint32_t)fb_bpp;
            if (fb_bpp == 4) {
                if (a == 255) {
                    d[0] = src[si+2]; d[1] = src[si+1]; d[2] = src[si+0];
                    d[3] = 255;
                } else {
                    int inv = 255 - a;
                    d[0] = (uint8_t)((src[si+2]*a + d[0]*inv) / 255);
                    d[1] = (uint8_t)((src[si+1]*a + d[1]*inv) / 255);
                    d[2] = (uint8_t)((src[si+0]*a + d[2]*inv) / 255);
                    d[3] = 255;   // keep the back-buffer alpha opaque
                }
            } else if (fb_bpp == 3) {
                if (a == 255) {
                    d[0] = src[si+2]; d[1] = src[si+1]; d[2] = src[si+0];
                } else {
                    int inv = 255 - a;
                    d[0] = (uint8_t)((src[si+2]*a + d[0]*inv) / 255);
                    d[1] = (uint8_t)((src[si+1]*a + d[1]*inv) / 255);
                    d[2] = (uint8_t)((src[si+0]*a + d[2]*inv) / 255);
                }
            }
        }
    }
}

static void draw_icon_px(int px,int py,int icon_idx,const char*label,int sel){
    const Theme*t=TH;
    uint32_t glyph = icon_glyph_rgb[icon_idx];

    int tile_w = 64, tile_h = 64;
    int cx = px + tile_w/2;
    int cy = py + tile_h/2 - 2;

    if (vesa.active) {
        if (icon_img_ok[icon_idx]) {
            // Custom picture icon — clean look: NO tile or shadow
            // behind it.  The SVG backgrounds were stripped, so the
            // picture is a transparent glyph floating directly on
            // the wallpaper.  (A shadow rect was tried but read as
            // a dark tile again, so it was removed.)
            icon_img_blit(px + ICON_IMG_PAD, py + ICON_IMG_PAD, icon_idx);
        } else {
            // Picture still decoding — draw the vector glyph on the
            // classic accent-tinted glass tile as a fallback.

            // Deep drop shadow (two offset layers for softness) —
            // rounded like the tile body.
            icon_rrect_grad_r(px+3, py+4, tile_w, tile_h, ICON_TILE_R+1, 0x020106, 0x020106);
            icon_rrect_grad_r(px+1, py+2, tile_w, tile_h, ICON_TILE_R+1, 0x05030C, 0x05030C);

            // Glass body — accent gradient, brighter when selected.
            uint32_t top    = icon_scale(glyph, sel ? 175 : 115);
            uint32_t bottom = icon_scale(glyph, sel ? 95  : 48);
            icon_rrect_grad_r(px, py, tile_w, tile_h, ICON_TILE_R, top, bottom);

            // Frosted sheen: white-blended band across the top, fading,
            // clipped to the rounded corner profile.
            int hband = tile_h / 3;
            for (int yy = 0; yy < hband; yy++) {
                int st = (hband - yy) * 70 / hband;   // 70 → 0
                int ins = icon_rr_inset(yy, ICON_TILE_R);
                if (ins < 5) ins = 5;
                vesa_hline(px+ins, py+yy, tile_w-2*ins, icon_lighten(top, st));
            }

            // Depth rims: bright top edge, shaded bottom edge — follow
            // the corner curve (row 1 inset) like the sheen above.
            int rim_ins = icon_rr_inset(1, ICON_TILE_R);
            if (rim_ins < 3) rim_ins = 3;
            vesa_hline(px+rim_ins, py+1, tile_w-2*rim_ins, icon_lighten(top, 80));
            vesa_hline(px+rim_ins, py+tile_h-2, tile_w-2*rim_ins, icon_scale(bottom, 70));

            // Accent border (near-white when selected).
            uint32_t border = sel ? icon_lighten(glyph, 140) : icon_scale(glyph, 200);
            icon_rrect_border_r(px, py, tile_w, tile_h, ICON_TILE_R, border);

            // Vector glyph: a soft dark shadow, then the bright
            // accent glyph (white when selected).
            draw_icon_symbol(icon_idx, cx + 1, cy + 2, icon_scale(glyph, 45));
            draw_icon_symbol(icon_idx, cx, cy, sel ? 0xFFFFFF : icon_lighten(glyph, 55));
        }

        // Selection: single smooth pulsing accent glow ring.
        if (sel) {
            int pulse_phase = (tick * 5) % 90;
            int pulse_val = (pulse_phase < 45) ? pulse_phase * 255 / 45 : (90 - pulse_phase) * 255 / 45;
            icon_rrect_border_r(px-1, py-1, tile_w+2, tile_h+2, ICON_TILE_R+1,
                              icon_lighten(glyph, 80 + pulse_val * 130 / 255));
        }

        // Label: dark pill with light text; accent-tinted when selected.
        int label_len = str_len(label);
        int label_w   = label_len * 8;
        int label_x   = cx - label_w/2;
        int label_y   = py + tile_h + 8;
        if (sel) {
            uint32_t pill = icon_scale(glyph, 70);
            vesa_fill_rect(label_x-4, label_y-2, label_w+8, 20, pill);
            vesa_draw_string(label_x, label_y, label, 0xFFFFFF, pill);
        } else {
            vesa_fill_rect(label_x-3, label_y-1, label_w+6, 18, 0x0A0618);
            vesa_draw_string(label_x, label_y, label, 0xE8E0F8, 0x0A0618);
        }
        return;
    }

    // ── VGA text fallback (no VESA): keep the original bracket
    // tile look, using the nearest character-grid cell for this
    // pixel position, since there's no pixel canvas to draw on.
    int x = px/8, y = py/16;
    uint8_t bg=sel?t->icon_sel_bg:t->desktop_bg;
    uint8_t fg=sel?t->icon_sel_fg:7;
    vga_puts(x,y,  "[======]",fg,bg);
    vga_puts(x,y+1,"[  **  ]",fg,bg);
    vga_puts(x,y+2,"[======]",fg,bg);
    int lx=x+(8-str_len(label))/2;
    vga_puts(lx,y+3,label,sel?t->icon_sel_fg:t->taskbar_fg,t->desktop_bg);
}

// ─── Desktop file/folder icons (created via the right-click menu) ─
// Hit-test a desktop file icon: returns the filesystem index under
// (mx,my), or -1.  Desktop files live in the FIRST empty grid slots
// (icon_order == -1), in order of desktop_files[].
static int desktop_file_hit_test(int mx, int my) {
    if (!vesa.active || desktop_files_count <= 0) return -1;
    IconGrid ig = compute_icon_grid((int)vesa.width, (int)vesa.height);
    int empties = 0;
    for (int s = 0; s < g_grid_slots; s++) {
        if (icon_order[s] >= 0) continue;          // occupied by an app
        if (empties >= desktop_files_count) break;
        int fi = desktop_files[empties];
        int px, py;
        icon_grid_slot_pos(&ig, s, &px, &py);
        if (mx >= px && mx < px + ICON_TILE_W &&
            my >= py && my < py + ICON_TILE_H + ICON_LABEL_H + 4)
            return fi;
        empties++;
    }
    return -1;
}

// Draw one desktop file/folder tile (amber folder for dirs, blue page
// for files), matching the app-icon tile style with a label pill.
static void draw_desktop_file_icon(int px, int py, int fi, int sel) {
    int tile_w = ICON_TILE_W, tile_h = ICON_TILE_H;
    int cx = px + tile_w/2, cy = py + tile_h/2 - 2;
    int is_dir = 0;
    int nl = 0; while (filesystem[fi].name[nl]) nl++;
    if (nl > 0 && filesystem[fi].name[nl-1] == '/') is_dir = 1;

    uint32_t glyph = is_dir ? 0xF2B73D : 0x4A90D9;   // amber folder / blue page
    uint32_t top = icon_scale(glyph, sel ? 175 : 115);
    uint32_t bottom = icon_scale(glyph, sel ? 95 : 48);
    icon_rrect_grad_r(px, py, tile_w, tile_h, ICON_TILE_R, top, bottom);
    icon_rrect_border_r(px, py, tile_w, tile_h, ICON_TILE_R, icon_scale(glyph, 200));

    if (is_dir) {
        // Folder glyph: tab + body + sheen
        vesa_fill_rect(cx - 16, cy - 10, 12, 6, icon_lighten(glyph, 60));
        vesa_fill_rect(cx - 18, cy - 4, 36, 20, icon_lighten(glyph, 25));
        vesa_hline(cx - 16, cy - 2, 32, icon_lighten(glyph, 70));
    } else {
        // Page glyph: white sheet with folded corner + text lines
        vesa_fill_rect(cx - 12, cy - 16, 24, 32, 0xF4F6FA);
        vesa_draw_rect(cx - 12, cy - 16, 24, 32, 0x9AA0A6);
        vesa_fill_rect(cx + 2, cy - 16, 10, 10, 0xD5D8DD);  // fold
        vesa_hline(cx - 8, cy - 8, 16, 0xC8CDD4);
        vesa_hline(cx - 8, cy - 2, 16, 0xC8CDD4);
        vesa_hline(cx - 8, cy + 4, 10, 0xC8CDD4);
    }

    // Label — strip the trailing '/' off folder names
    char dlabel[MAX_FILENAME];
    int di = 0;
    for (int i = 0; filesystem[fi].name[i] && di < MAX_FILENAME - 1; i++) {
        if (filesystem[fi].name[i] == '/') break;
        dlabel[di++] = filesystem[fi].name[i];
    }
    dlabel[di] = 0;
    int label_w = di * 8;
    int label_x = cx - label_w/2;
    int label_y = py + tile_h + 8;
    if (sel) {
        uint32_t pill = icon_scale(glyph, 70);
        vesa_fill_rect(label_x - 4, label_y - 2, label_w + 8, 20, pill);
        vesa_draw_string(label_x, label_y, dlabel, 0xFFFFFF, pill);
    } else {
        vesa_fill_rect(label_x - 3, label_y - 1, label_w + 6, 18, 0x0A0618);
        vesa_draw_string(label_x, label_y, dlabel, 0xE8E0F8, 0x0A0618);
    }
}

static void draw_icons(){
    int W = vesa.active ? (int)vesa.width  : VGA_WIDTH*8;
    int H = vesa.active ? (int)vesa.height : VGA_HEIGHT*16;

    // Lazy custom-picture scan: the incremental scan is driven from
    // desktop_heartbeat() (one picture per tick), never here, so the
    // first frame after login draws instantly with vector glyphs.
    // Icons whose picture isn't decoded yet simply fall back to the
    // glyph (icon_img_ok[] check in draw_icon_px).

    IconGrid ig = compute_icon_grid(W, H);
    int tile_w = ICON_TILE_W, tile_h = ICON_TILE_H;
    int label_h = ICON_LABEL_H;

    for (int i = 0; i < g_grid_slots; i++) {
        // In undo-browse mode, render from the preview stack instead
        // of the live icon_order[] so the user sees historical layouts
        // without committing to them.  If the preview slot is empty,
        // skip drawing (the icon wasn't there at that point in history).
        int app_idx;
        if (undo_browse_active) {
            app_idx = undo_stack[undo_browse_idx][i];
            if (app_idx < 0) continue;
        } else {
            app_idx = icon_order[i];
            if (app_idx < 0) continue;
        }
        int px, py;
        icon_grid_slot_pos(&ig, i, &px, &py);
        int sel = (selected_icon == i);

        // ── Drop-flash overlay ───────────────────────────────────
        // A brief 2-frame white flash on the tile when an icon lands
        // in a new slot — visual confirmation without any position
        // shift.  av = anim_frame (2 → 0), bright = 255 → 0.
        int anim_here = (i == anim_slot && anim_frame > 0);
        if (anim_here) {
            int av = anim_frame;
            int bright = av * 255 / ANIM_FRAMES;  // 255 → 127 → 0
            uint32_t flash = (bright << 16) | ((bright * 220 / 255) << 8) | bright;
            icon_rrect_grad_r(px, py, tile_w, tile_h, ICON_TILE_R, flash, 0);
        }

        // ── Hover highlight ──────────────────────────────────────
        // Slim ring around the tile (matching the tile's 2px rounded
        // corners) with light top/left edges — "lit from above-left".
        if (vesa.active && !sel && !anim_here && mouse_in_rect(px, py, tile_w, tile_h + label_h + 4)) {
            icon_rrect_border_r(px-1, py-1, tile_w+2, tile_h+2, ICON_TILE_R+1, 0x9B7FFF);
            vesa_hline(px-1+ICON_TILE_R+1, py-1, tile_w-2*ICON_TILE_R, 0xD8C0FF);
            vesa_vline(px-1, py-1+ICON_TILE_R+1, tile_h-2*ICON_TILE_R, 0xD8C0FF);
        }
        // ── Rubber-band selection ring ───────────────────────────
        // Apps captured by the box-select drag stay highlighted with
        // a teal ring (distinct from the purple hover ring) until the
        // next click clears the multi-selection.
        if (vesa.active && box_sel[i] && !sel && !anim_here) {
            icon_rrect_border_r(px-2, py-2, tile_w+4, tile_h+4, ICON_TILE_R+2, 0x2FD8C0);
        }
        // ── Spring-loaded snap ghost ─────────────────────────────
        // When the user drops the icon on its own slot, the ghost
        // flies from the release point back to the slot's center.
        // Skip drawing the icon at grid position (it's already there
        // in icon_order[] — we didn't swap anything), and draw the
        // ghost at the interpolated position with a glowing trail.
        int snap_here = (snap_active && i == snap_slot);
        if (snap_here) {
            // Interpolate ghost position using easing table
            int ep = snap_ease[snap_frame <= SNAP_FRAMES ? snap_frame : SNAP_FRAMES];
            int gx = snap_sx + (snap_ex - snap_sx) * ep / 256;
            int gy = snap_sy + (snap_ey - snap_sy) * ep / 256;
            int gpx = gx - tile_w / 2;
            int gpy = gy - tile_h / 2;

            // Glow trail: expanding ring behind the ghost
            int trail_r = (SNAP_FRAMES - snap_frame) * 4 / SNAP_FRAMES;
            int trail_b = snap_frame * 128 / SNAP_FRAMES;
            if (trail_r > 0 && trail_b > 20) {
                uint32_t trail_col = (trail_b << 16) | ((trail_b*180/255) << 8) | trail_b;
                icon_rrect_border_r(gpx - trail_r, gpy - trail_r,
                                  tile_w + trail_r*2, tile_h + trail_r*2,
                                  ICON_TILE_R + trail_r, trail_col);
            }

            draw_icon_px(gpx, gpy, app_idx, app_names[app_idx], 0);
        } else {
            draw_icon_px(px, py, app_idx, app_names[app_idx], sel);
        }
    }

    // ── Desktop file/folder icons ───────────────────────────────
    // Files created via the desktop right-click menu (New Folder /
    // New File) sit in the first empty grid slots, after the apps.
    if (vesa.active && !undo_browse_active) {
        // Skipped in undo-browse mode: apps render from the preview
        // stack there, and live file icons would overlap them.
        int empties = 0;
        for (int s = 0; s < g_grid_slots && empties < desktop_files_count; s++) {
            if (icon_order[s] >= 0) continue;
            int fi = desktop_files[empties];
            if (fi >= 0 && fi < MAX_FILES && filesystem[fi].used) {
                int px, py;
                icon_grid_slot_pos(&ig, s, &px, &py);
                draw_desktop_file_icon(px, py, fi, (desktop_file_selected == fi));
            }
            empties++;
        }
    }

    // ── Slide-in flying ghost ────────────────────────────────────
    // The ghost is drawn by the IRQ handler at 200 Hz on vesa.fb as
    // an overlay (via ghost_bg save/restore and the overlay plane).
    // This function only draws icons at their GRID positions — the
    // flying ghost is handled in mouse.c's IRQ callback instead.
}

// ─── App windows ─────────────────────────────────────────────────
static void draw_terminal_window(){
    const Theme*t=TH;

    // First time this window opens, hook up the redirect buffer so
    // shell.c's terminal_print() calls land here instead of raw VGA
    // memory, and print the welcome banner once.
    if (!term_initialized) {
        terminal_redirect_clear(&term_buf);
        terminal_set_redirect(&term_buf);
        terminal_set_color(COLOR_LIGHT_CYAN, COLOR_BLACK);
        terminal_print("Nyx OS Shell v2.0\n");
        terminal_print("Type 'help' for commands. TAB=autocomplete  UP/DOWN=history\n");
        terminal_print("\n");
        term_initialized = 1;
    }
    terminal_set_redirect(&term_buf);

    if (!vesa.active) {
        // ── VGA text fallback: simple static-ish rendering of the
        // last 17 buffered lines plus the live input line.
        vga_fill_rect(2,2,76,20,' ',t->win_fg,t->win_bg);
        vga_fill_row(2,' ',t->win_title_fg,t->win_title_bg);
        vga_puts(3,2,"[ Terminal - Nyx Shell v2.0 ]",t->win_title_fg,t->win_title_bg);
        vga_puts(74,2,"[X]",t->danger,t->win_title_bg);

        int visible_rows = 17;
        int total = term_buf.line_count;
        int first = total - visible_rows; if (first < 0) first = 0;
        int row = 4;
        for (int i = first; i < total && row < 21; i++) {
            vga_puts(3, row, term_buf.lines[i % TERM_REDIRECT_ROWS], t->win_fg, t->win_bg);
            row++;
        }
        vga_puts(3, 21, "nyx>", t->accent, t->win_bg);
        vga_puts(8, 21, cmd_buffer, t->win_fg, t->win_bg);
        return;
    }

    // ── VESA pixel rendering ──────────────────────────────────────
    int W = (int)vesa.width, H = (int)vesa.height;
    // ── Window position (draggable via title bar) ─────────────────
    if (g_win_x < 0) { g_win_x = W/10; g_win_y = 70; }
    int win_x = g_win_x, win_y = g_win_y;
    int win_w = W*8/10, win_h = H - 160;
    g_win_w = win_w; g_win_h = win_h;

    // ── Phase 1: backdrop effects drawn directly to the framebuffer
    // ── Window frame (body gradient, border, shadow, title bar) ──
    // Self-contained window decorations drawn directly to the framebuffer.
    vesa_fill_rect(win_x+5, win_y+5, win_w, win_h, 0x05030C);
    icon_rrect_grad(win_x, win_y, win_w, win_h, 0x140C30, 0x0E0824);
    icon_rrect_border(win_x, win_y, win_w, win_h, 0x6A4FBF);

    int title_h = 36;
    icon_rrect_grad(win_x+1, win_y+1, win_w-2, title_h, 0x2E2050, 0x1E1240);
    vesa_hline(win_x+1, win_y+title_h, win_w-2, 0xB088FF);
    vesa_draw_string(win_x+18, win_y+10, "Terminal - Nyx Shell v2.0", 0xE8D8FF, 0);
    draw_close_btn(win_x+win_w-32, win_y+10, 0, "Terminal");

    // Output area: render the last N lines that fit, monospace 8x16
    int line_h   = 16;
    int pad      = 16;
    int text_x   = win_x + pad;
    int text_top = win_y + 10;
    int input_h  = 28;
    int avail_h  = win_h - 10 - input_h - 16;
    int visible_rows = avail_h / line_h;
    if (visible_rows < 1) visible_rows = 1;

    int total = term_buf.line_count;
    int first = total - visible_rows - term_scroll;
    if (first < 0) first = 0;

    int y = text_top;
    for (int i = first; i < total && i < first + visible_rows; i++) {
        const char* ln = term_buf.lines[i % TERM_REDIRECT_ROWS];
        uint32_t fg = 0xC8B8E8;
        // Lightly highlight the prompt lines for readability.
        if (ln[0]=='n' && ln[1]=='y' && ln[2]=='x' && ln[3]=='>') fg = 0xB088FF;
        vesa_draw_string(text_x, y, ln, fg, 0);
        y += line_h;
    }

    // Input line, pinned to the bottom of the window
    int input_y = win_y + win_h - input_h - 8;
    vesa_hline(win_x+pad, input_y-6, win_w-pad*2, 0x3D2A66);
    vesa_draw_string(text_x, input_y, "nyx>", 0xB088FF, 0);
    vesa_draw_string(text_x+40, input_y, cmd_buffer, 0xF0E8FF, 0);

    // Blinking-style cursor (always drawn; real blinking would need
    // a frame timer we don't have wired to redraw on its own yet)
    int cur_x = text_x + 40 + str_len(cmd_buffer)*8;
    vesa_fill_rect(cur_x, input_y-1, 2, 14, 0xB088FF);

}

// ── This PC home tile body: glass look, matching desktop icons ─────
// Rounded glass tile with a per-folder accent hue (Nautilus style),
// drop shadow, frosted sheen, depth rims and accent border.  Same
// visual language as draw_icon_px() so the home grid reads as part
// of the desktop rather than flat dark boxes.
static void files_home_tile_body(int rx, int ry, uint32_t accent,
                                 int hover, int sel) {
    int w = FILES_TILE_W, h = FILES_TILE_H;
    int R = FILES_TILE_R;
    // Deep drop shadow (two offset layers for softness)
    icon_rrect_grad_r(rx+3, ry+4, w, h, R+1, 0x020106, 0x020106);
    icon_rrect_grad_r(rx+1, ry+2, w, h, R+1, 0x05030C, 0x05030C);
    // Glass body — accent gradient, brighter when hovered/selected
    uint32_t top    = icon_scale(accent, sel ? 185 : (hover ? 145 : 110));
    uint32_t bottom = icon_scale(accent, sel ? 100 : (hover ? 75 : 45));
    icon_rrect_grad_r(rx, ry, w, h, R, top, bottom);
    // Frosted sheen band across the top, clipped to the corner profile
    int hband = h / 3;
    for (int yy = 0; yy < hband; yy++) {
        int st = (hband - yy) * 70 / hband;
        int ins = icon_rr_inset(yy, R);
        if (ins < 5) ins = 5;
        vesa_hline(rx + ins, ry + yy, w - 2*ins, icon_lighten(top, st));
    }
    // Depth rims: bright top edge, shaded bottom edge
    int rim = icon_rr_inset(1, R);
    if (rim < 3) rim = 3;
    vesa_hline(rx + rim, ry + 1, w - 2*rim, icon_lighten(top, 80));
    vesa_hline(rx + rim, ry + h - 2, w - 2*rim, icon_scale(bottom, 70));
    // Accent border (near-white when selected)
    uint32_t border = sel ? icon_lighten(accent, 140)
                          : (hover ? icon_lighten(accent, 85)
                                   : icon_scale(accent, 200));
    icon_rrect_border_r(rx, ry, w, h, R, border);
}

static void draw_files_window(){
    const Theme*t=TH;

    if (!vesa.active) {
        // ── VGA text fallback ──
        vga_fill_rect(2,2,76,20,' ',t->win_fg,t->win_bg);
        vga_fill_row(2,' ',t->win_title_fg,t->win_title_bg);
        vga_puts(3,2,"[ File Manager ]",t->win_title_fg,t->win_title_bg);
        vga_puts(74,2,"[X]",t->danger,t->win_title_bg);
        char loc[MAX_FILENAME+8]; int lp=0; loc[lp++]='/';
        { int q=0; while (files_cur_path[q]&&lp<MAX_FILENAME+6) loc[lp++]=files_cur_path[q++]; }
        loc[lp]=0;
        vga_puts(3,4,"Location: ",t->highlight,t->win_bg);
        if (files_home_view) {
            // VGA fallback for the This PC home — list the same six
            // entries as the home tiles, in the SAME order the key
            // handler maps them (tile_items[]), so ENTER always opens
            // what the row says.
            static const char* hv[6] = {"Desktop","Documents","Downloads","Music","Pictures","Local Disk (C:)"};
            vga_puts(13,4,"This PC",t->highlight,t->win_bg);
            vga_puts(3,5,"-----------------------------------------",t->win_border,t->win_bg);
            for (int i = 0; i < 6 && 6+i < 21; i++) {
                uint8_t fg=(i==selected_file)?t->win_bg:t->accent;
                uint8_t bg=(i==selected_file)?t->accent:t->win_bg;
                vga_puts(3,6+i,i<5?"[DIR]  ":"[DRV]  ",fg,bg);
                vga_puts(10,6+i,hv[i],fg,bg);
            }
            vga_puts(3,21,"ENTER:open  A/B:select  ESC:back",t->win_border,t->win_bg);
            return;
        }
        vga_puts(13,4,loc,t->highlight,t->win_bg);
        {
            // Item count, right-aligned on the Location row.
            char cb[16]; files_item_label(files_item_count(), cb);
            vga_puts(76 - str_len(cb), 4, cb, t->win_fg, t->win_bg);
        }
        vga_puts(3,5,"-----------------------------------------",t->win_border,t->win_bg);
        if (!files_desktop_view && !files_cur_path[0]) {
            vga_puts(3,6,"[SYS]  boot/",   t->accent,t->win_bg);
            vga_puts(3,7,"[SYS]  kernel/", t->accent,t->win_bg);
            vga_puts(3,8,"[SYS]  cpu/",    t->accent,t->win_bg);
            vga_puts(3,9,"[SYS]  include/",t->accent,t->win_bg);
        }
        vga_puts(3,10,"-----------------------------------------",t->win_border,t->win_bg);
        int nrows = files_visible_count();
        if (nrows==0) {
            vga_puts(3,11,"No files here.",t->win_border,t->win_bg);
            vga_puts(3,12,"Use N:new file  M:new folder.",t->win_fg,t->win_bg);
        } else {
            int only_up = (files_cur_path[0] != 0 && nrows == 1);
            if (only_up) { int f0; files_visible_row(0, &f0); only_up = (f0 < 0); }
            int row = only_up ? 13 : 11;
            if (only_up) {
                vga_puts(3,11,"This folder is empty.",t->win_border,t->win_bg);
                vga_puts(3,12,"Use N:new file  M:new folder.",t->win_fg,t->win_bg);
            }
            // VGA has ~9 usable rows (11..19); clamp the same way the
            // VESA branch does so a stale/large files_scroll (e.g. the
            // 0x7FFFFFFF sentinel set on file creation) can't blank the
            // whole list.
            int vga_vis = 9;
            if (files_scroll > nrows - vga_vis) files_scroll = (nrows > vga_vis) ? nrows - vga_vis : 0;
            if (files_scroll < 0) files_scroll = 0;
            for(int r=files_scroll;r<nrows&&row<20;r++){
                int fi; files_visible_row(r,&fi);
                uint8_t fg=(r==selected_file)?t->win_bg:t->accent;
                uint8_t bg=(r==selected_file)?t->accent:t->win_bg;
                if (fi < 0) {
                    vga_puts(3,row,"[UP]  ..",fg,bg);
                } else {
                    char shortname[MAX_FILENAME];
                    files_short_name(filesystem[fi].name, shortname);
                    int nl=0; while(shortname[nl])nl++;
                    int is_dir=(nl>0&&shortname[nl-1]=='/');
                    vga_puts(3,row,is_dir?"[DIR] ":"[FILE] ",fg,bg);
                    if (is_dir) shortname[nl-1]=0;
                    vga_puts(10,row,shortname,fg,bg);
                }
                row++;
            }
            vga_puts(3,21,"ENTER:open  A/B:select  X:delete  ESC:up",t->win_border,t->win_bg);
        }
        return;
    }

    // ── VESA pixel rendering ──────────────────────────────────────
    int W=(int)vesa.width, H=(int)vesa.height;
    // ── Window position (draggable via title bar) ─────────────────
    if (g_win_x < 0) { g_win_x = W/10; g_win_y = 70; }
    int win_x=g_win_x, win_y=g_win_y, win_w=W*8/10, win_h=H-160;
    g_win_w = win_w; g_win_h = win_h;

    // ── Window frame (body gradient, border, shadow, title bar) ──
    vesa_fill_rect(win_x+5, win_y+5, win_w, win_h, 0x05030C);
    icon_rrect_grad(win_x, win_y, win_w, win_h, 0x140C30, 0x0E0824);
    icon_rrect_border(win_x, win_y, win_w, win_h, 0x6A4FBF);

    int title_h = 36;
    icon_rrect_grad(win_x+1, win_y+1, win_w-2, title_h, 0x2E2050, 0x1E1240);
    vesa_hline(win_x+1, win_y+title_h, win_w-2, 0xB088FF);
    vesa_draw_string(win_x+18, win_y+10, "File Manager", 0xE8D8FF, 0);
    draw_close_btn(win_x+win_w-32, win_y+10, 0, "File Manager");

    // ── Navigation pane (Windows-style sidebar) ─────────────────
    int sb_x = win_x + 1;
    int sb_y = win_y + 1;
    int sb_w = FILES_SB_W;
    int sb_h = win_h - 2;

    // Sidebar background — slightly darker to distinguish from content
    for (int dy = 0; dy < sb_h; dy++) {
        int blend = dy * 5 / sb_h;
        int r = 0x0C + (0x0A - 0x0C) * blend / 5;
        int g = 0x06 + (0x04 - 0x06) * blend / 5;
        int b = 0x1C + (0x14 - 0x1C) * blend / 5;
        vesa_hline(sb_x, sb_y + dy, sb_w, (r<<16) | (g<<8) | b);
    }
    // Sidebar right border
    vesa_vline(sb_x + sb_w - 1, sb_y, sb_h, 0x3D2A66);

    // ── Section labels ────────────────────────────────────────────
    int mx = mouse.px, my = mouse.py;
    int active_sb = files_sb_active();
    int items_top = win_y + 36 + 8 + FILES_SB_LABEL_H;   // matches files_sb_item_rect
    vesa_draw_string(sb_x + 12, win_y + 36 + 8, "Quick access", 0x6A5890, 0);
    vesa_draw_string(sb_x + 12, items_top + 6 * FILES_SB_ITEM_H + 8,
                     "Devices and drives", 0x6A5890, 0);

    // ── Quick access items (Home, Desktop, Documents, ...) ──────
    static const char* qa_names[6] = {"Home","Desktop","Documents","Downloads","Music","Pictures"};
    for (int i = 0; i < 6; i++) {
        int rx, ry, rw, rh;
        files_sb_item_rect(win_x, win_y, i, &rx, &ry, &rw, &rh);
        int hover = (mx >= rx && mx < rx + rw && my >= ry && my < ry + rh);
        int act = (i == active_sb);
        if (hover || act) {
            icon_rrect_grad(rx, ry, rw, rh, act ? 0x2E2050 : 0x1A1234, act ? 0x241A40 : 0x140C28);
            if (act) icon_rrect_border(rx, ry, rw, rh, 0xB088FF);
        }
        files_sb_glyph(rx + 4, ry + 5, i, (act||hover) ? 0xFFFFFF : 0x9A88C0);
        vesa_draw_string(rx + 24, ry + 6, qa_names[i], (act||hover) ? 0xFFFFFF : 0xD8C8F0, 0);
    }

    // ── Devices and drives items (Local Disk, Trash) ────────────
    for (int i = 6; i < FSI_COUNT; i++) {
        int rx, ry, rw, rh;
        files_sb_item_rect(win_x, win_y, i, &rx, &ry, &rw, &rh);
        int hover = (mx >= rx && mx < rx + rw && my >= ry && my < ry + rh);
        int act = (i == active_sb);
        if (hover || act) {
            icon_rrect_grad(rx, ry, rw, rh, act ? 0x2E2050 : 0x1A1234, act ? 0x241A40 : 0x140C28);
            if (act) icon_rrect_border(rx, ry, rw, rh, 0xB088FF);
        }
        uint32_t ic = ((i == FSI_TRASH && trash_count > 0) || act || hover) ? 0xFFFFFF : 0x9A88C0;
        if (i == FSI_TRASH && trash_count > 0) ic = 0xFF8899;
        files_sb_glyph(rx + 4, ry + 5, i, ic);
        vesa_draw_string(rx + 24, ry + 6, (i == FSI_DRIVE) ? "Local Disk (C:)" : "Trash",
                         (act||hover) ? 0xFFFFFF : 0xD8C8F0, 0);
        // Trash count badge
        if (i == FSI_TRASH && trash_count > 0) {
            char cnt[4];
            int n = trash_count;
            cnt[0] = '0' + (n / 10); cnt[1] = '0' + (n % 10); cnt[2] = 0;
            if (cnt[0] == '0') { cnt[0] = cnt[1]; cnt[1] = 0; }
            int cw = (cnt[1] ? 2 : 1) * 8;
            vesa_fill_rect(rx + rw - cw - 16, ry + 6, cw + 6, 12, 0xFF4455);
            vesa_draw_string(rx + rw - cw - 13, ry + 7, cnt, 0xFFFFFF, 0xFF4455);
        }
    }

    // Trash item doubles as the drag-to-trash drop zone
    {
        int rx, ry, rw, rh;
        files_sb_item_rect(win_x, win_y, FSI_TRASH, &rx, &ry, &rw, &rh);
        files_sidebar_trash_x = rx;
        files_sidebar_trash_y = ry;
        files_sidebar_trash_w = rw;
        files_sidebar_trash_h = rh;
    }

    // Separator between sidebar and content
    vesa_vline(sb_x + sb_w, sb_y, sb_h, 0x2B2448);

    // ── Main content area (offset by sidebar) ─────────────────────
    int content_left = sb_x + sb_w + 15;
    int px = content_left, py = win_y + 14;

    // ── This PC home: Folders + Devices and drives sections ────
    if (files_home_view) {
        // Title: small monitor glyph + brighter text
        files_sb_glyph(px, py - 2, FSI_DESKTOP, 0xA88CFF);
        vesa_draw_string(px + 20, py, "This PC", 0xC0ACF0, 0);
        py += 26;
        vesa_hline(win_x + FILES_SB_W + 16, py, win_w - FILES_SB_W - 32, 0x3D2A66);
        py += 14;

        // ── Folders section — accent-tinted glass tiles ────────
        files_sb_glyph(px, py - 2, FSI_DOCS, 0xEFB93B);
        vesa_draw_string(px + 20, py, "Folders", 0xA090C8, 0);
        static const char* folder_names[5] = {"Desktop","Documents","Downloads","Music","Pictures"};
        static const int   folder_items[5] = {FSI_DESKTOP, FSI_DOCS, FSI_DL, FSI_MUSIC, FSI_PICS};
        static const uint32_t folder_acc[5] = {
            0x4C8FF7,  // Desktop   — blue
            0xEFB93B,  // Documents — amber
            0x2EC27E,  // Downloads — green
            0xA56DE2,  // Music     — purple
            0xCE7AA8,  // Pictures  — rose
        };
        for (int i = 0; i < 5; i++) {
            int rx, ry;
            files_home_tile_rect(win_x, win_y, i, &rx, &ry);
            int hover = (mx >= rx && mx < rx + FILES_TILE_W && my >= ry && my < ry + FILES_TILE_H);
            int sel = (selected_file == i);
            files_home_tile_body(rx, ry, folder_acc[i], hover, sel);
            files_tile_glyph(rx + FILES_TILE_W/2 - 16, ry + 10, folder_items[i],
                             sel ? 0xFFFFFF : icon_lighten(folder_acc[i], 200));
            // Label: dark pill with light text (accent pill when selected)
            int nl = 0; while (folder_names[i][nl]) nl++;
            int lx = rx + FILES_TILE_W/2 - nl*4;
            int ly = ry + FILES_TILE_H - 16;
            uint32_t pill = sel ? icon_scale(folder_acc[i], 70) : 0x0A0618;
            vesa_fill_rect(lx - 3, ly - 2, nl*8 + 6, 13, pill);
            vesa_draw_string(lx, ly, folder_names[i], 0xFFFFFF, pill);
        }

        // ── Devices and drives section ──
        int rx, ry;
        files_home_drive_rect(win_x, win_y, &rx, &ry);
        files_sb_glyph(px, ry - 24, FSI_DRIVE, 0x8FA8C8);
        vesa_draw_string(px + 20, ry - 22, "Devices and drives", 0xA090C8, 0);
        int hover = (mx >= rx && mx < rx + FILES_TILE_W && my >= ry && my < ry + FILES_TILE_H);
        int sel = (selected_file == 5);
        files_home_tile_body(rx, ry, 0x7A8699, hover, sel);
        files_tile_glyph(rx + FILES_TILE_W/2 - 16, ry + 4, FSI_DRIVE,
                         sel ? 0xFFFFFF : icon_lighten(0x7A8699, 210));
        // Label: same dark pill treatment as the folder tiles
        {
            const char* dn = "Local Disk";
            int nl = 0; while (dn[nl]) nl++;
            int lx = rx + FILES_TILE_W/2 - nl*4;
            int ly = ry + 38;
            uint32_t pill = sel ? icon_scale(0x7A8699, 70) : 0x0A0618;
            vesa_fill_rect(lx - 3, ly - 2, nl*8 + 6, 13, pill);
            vesa_draw_string(lx, ly, dn, 0xFFFFFF, pill);
        }
        // Capacity bar: used bytes vs total filesystem capacity.
        // The displayed % is animated toward the true value by
        // desktop_heartbeat(); render the tweened fill and label so
        // they glide together.  First frame snaps to the real value.
        drive_pct_target = files_drive_pct();
        if (drive_pct_disp < 0) drive_pct_disp = drive_pct_target;
        int pct = drive_pct_disp;
        int bw = FILES_TILE_W - 20;
        icon_rrect_grad_r(rx + 10, ry + 52, bw, 7, 3, 0x0A0618, 0x080410);
        int fw = (pct > 0 && pct < 4) ? 4 : bw * pct / 100;
        if (fw > 3)
            icon_rrect_grad_r(rx + 10, ry + 52, fw, 7, 3, 0x8FE3B8, 0x2EC27E);
        // Percent label under the bar
        char ptxt[8]; int pk = 0;
        int pn = pct;
        if (pn == 0) ptxt[pk++] = '0';
        while (pn > 0 && pk < 3) { ptxt[pk++] = '0' + (pn % 10); pn /= 10; }
        char prev[8]; int pr = 0;
        while (pk > 0) prev[pr++] = ptxt[--pk];
        prev[pr++] = '%'; prev[pr] = 0;
        vesa_draw_string(rx + FILES_TILE_W/2 - pr*4, ry + 62, prev, 0x9A88C0, 0);

        int hint_y = win_y+win_h-26;
        vesa_hline(win_x + FILES_SB_W + 16, hint_y-8, win_w - FILES_SB_W - 32, 0x1E1240);
        vesa_draw_string(px, hint_y, "ENTER open   A/B select   ESC back", 0x3D2A66, 0);
        return;
    }

    // ── Browse view: breadcrumb + listing ───────────────────────
    char pathlbl[MAX_FILENAME + 8];
    int pp = 0;
    if (files_desktop_view) {
        const char* dn = "Desktop"; int dq = 0;
        while (dn[dq] && pp < MAX_FILENAME + 6) pathlbl[pp++] = dn[dq++];
    } else {
        pathlbl[pp++] = '/';
        { int q=0; while (files_cur_path[q] && pp < MAX_FILENAME + 6) pathlbl[pp++] = files_cur_path[q++]; }
    }
    pathlbl[pp] = 0;
    vesa_draw_string(px, py, pathlbl, 0x9580C0, 0);
    // Item count, right-aligned inside the breadcrumb band so the
    // content rows (files_rows_top) never shift.
    {
        char cb[16]; files_item_label(files_item_count(), cb);
        vesa_draw_string(win_x + win_w - 28 - str_len(cb) * 8, py, cb, 0x6A5890, 0);
    }
    py += 26;
    vesa_hline(win_x + FILES_SB_W + 16, py, win_w - FILES_SB_W - 32, 0x3D2A66);
    py += 14;

    // System folders (drive root only — fixed, non-interactive)
    if (!files_desktop_view && !files_cur_path[0]) {
        const char* sysdirs[] = {"boot","kernel","cpu","include"};
        for (int i=0;i<4;i++) {
            vesa_draw_char(px+2, py, '>', 0xFFD166, 0);
            vesa_draw_string(px+18, py, sysdirs[i], 0xD8C8F0, 0);
            vesa_draw_string(px+18+str_len(sysdirs[i])*8+4, py, "/", 0xD8C8F0, 0);
            vesa_draw_string(win_x+win_w-100, py, "system", 0x554477, 0);
            py += 24;
        }
        py += 6;
        vesa_hline(win_x + FILES_SB_W + 16, py, win_w - FILES_SB_W - 32, 0x3D2A66);
        py += 16;
    }        int nrows = files_visible_count();
        int visible = files_visible_rows(win_y, win_h);
        if (files_scroll > nrows - visible) files_scroll = (nrows > visible) ? nrows - visible : 0;
        if (files_scroll < 0) files_scroll = 0;
        if (nrows==0) {
            vesa_draw_string(px, py, "No files here.", 0x6A5890, 0);
            vesa_draw_string(px, py+20, "Press N for new file, M for folder.", 0x554477, 0);
        } else {
            // Inside a folder whose only entry is the up-row, explain the
            // empty state before drawing it — otherwise the lone ".. (up)"
            // row looks like a bug.  Gated on a real folder path so the
            // Desktop view listing never shows "This folder is empty.".
            int only_up = (files_cur_path[0] != 0 && nrows == 1);
            if (only_up) { int f0; files_visible_row(0, &f0); only_up = (f0 < 0); }
            if (only_up) {
                vesa_draw_string(px, py, "This folder is empty.", 0x6A5890, 0);
                vesa_draw_string(px, py+20, "Press N for new file, M for folder.", 0x554477, 0);
                py += 52;
            }
            const int row_h = 26, gap = 4;
            for (int r=files_scroll; r<nrows && r<files_scroll+visible; r++) {
                int fi; files_visible_row(r, &fi);
                int sel = (r==selected_file);
                int row_y = py + (r - files_scroll) * (row_h + gap);
                if (sel) {
                    icon_rrect_grad(px-6, row_y-3, win_w - FILES_SB_W - 40, row_h, 0x2E2050, 0x241A40);
                    icon_rrect_border(px-6, row_y-3, win_w - FILES_SB_W - 40, row_h, 0xB088FF);
                }
                if (fi < 0) {
                    // Up row — back to the parent folder (This PC at root).
                    uint32_t uc = sel ? 0xFFFFFF : 0xFFD166;
                    vesa_draw_char(px+2, row_y+2, '^', uc, 0);
                    vesa_draw_string(px+20, row_y+2, ".. (up)", sel?0xFFFFFF:0xD8C8F0, 0);
                } else {
                    // ── Inline rename: show editable text field ──
                    if (rename_mode && rename_file_idx == fi) {
                        // Icon
                        int nl = 0; while (filesystem[fi].name[nl]) nl++;
                        int is_dir = (nl > 0 && filesystem[fi].name[nl-1] == '/');
                        if (is_dir) {
                            uint32_t fc = 0xFFD166;
                            vesa_hline(px + 1, row_y + 3, 5, fc);
                            vesa_draw_rect(px, row_y + 5, 8, 7, fc);
                        } else {
                            vesa_draw_char(px+2, row_y+2, 'F', 0x8FE0FF, 0);
                        }
                        // Editable text field
                        int field_x = px + 20;
                        int field_w = 200;
                        vesa_fill_rect(field_x - 2, row_y, field_w + 4, row_h, 0x1A1030);
                        vesa_hline(field_x - 2, row_y, field_w + 4, 0xB088FF);
                        vesa_hline(field_x - 2, row_y + row_h - 1, field_w + 4, 0xB088FF);
                        vesa_vline(field_x - 2, row_y, row_h, 0xB088FF);
                        vesa_vline(field_x + field_w + 2, row_y, row_h, 0xB088FF);
                        // Render typed characters
                        vesa_draw_string(field_x, row_y + 2, rename_buf, 0xFFFFFF, 0x1A1030);
                        // Blinking cursor
                        if ((tick / 8) % 2 == 0) {
                            int cx = field_x + rename_pos * 8;
                            vesa_vline(cx, row_y + 2, 16, 0xFFFFFF);
                        }
                    } else {
                    // ── Normal file row ──
                    char shortname[MAX_FILENAME];
                    files_short_name(filesystem[fi].name, shortname);
                    int nl = 0; while (shortname[nl]) nl++;
                    int is_dir = (nl > 0 && shortname[nl-1] == '/');
                    if (is_dir) {
                        uint32_t fc = sel ? 0xFFFFFF : 0xFFD166;
                        vesa_hline(px + 1, row_y + 3, 5, fc);        // folder tab
                        vesa_draw_rect(px, row_y + 5, 8, 7, fc);     // folder body
                        shortname[nl - 1] = 0;                       // hide trailing '/'
                        vesa_draw_string(px+20, row_y+2, shortname, sel?0xFFFFFF:0xD8C8F0, 0);
                    } else {
                        vesa_draw_char(px+2, row_y+2, 'F', sel?0xFFFFFF:0x8FE0FF, 0);
                        vesa_draw_string(px+20, row_y+2, shortname, sel?0xFFFFFF:0xD8C8F0, 0);
                    }
                    }  // end normal file row
                }
            }
            // Scrollbar — only when the list overflows the window.
            if (nrows > visible) {
                int track_x = win_x + win_w - 20;
                int top    = files_rows_top(win_y);
                int bottom = win_y + win_h - 26 - 8;
                int avail  = bottom - top - 4;
                vesa_vline(track_x, top, avail, 0x2B2448);
                int bar_h = avail * visible / nrows; if (bar_h < 10) bar_h = 10;
                int bar_y = top + (avail - bar_h) * files_scroll / (nrows - visible);
                vesa_fill_rect(track_x - 2, bar_y, 4, bar_h, 0x8A7FC0);
            }

        int hint_y = win_y+win_h-26;
        vesa_hline(win_x + FILES_SB_W + 16, hint_y-8, win_w - FILES_SB_W - 32, 0x1E1240);
        vesa_draw_string(px, hint_y, "ENTER open   A/B select   F2 rename   X delete   N file   M folder", 0x3D2A66, 0);
    }}

static void draw_file_viewer(int file_idx) {
    const Theme*t=TH;

    if (!vesa.active) {
        // ── VGA text fallback ──
        vga_fill_rect(2,2,76,20,' ',t->win_fg,t->win_bg);
        vga_fill_row(2,' ',t->win_title_fg,t->win_title_bg);
        vga_puts(3,2,"[ File Viewer - ",t->win_title_fg,t->win_title_bg);
        vga_puts(19,2,filesystem[file_idx].name,t->accent,t->win_title_bg);
        vga_puts(74,2,"[X]",t->danger,t->win_title_bg);
        vga_puts(3,4,"-----------------------------------------",t->win_border,t->win_bg);
        const char*content=filesystem[file_idx].content;
        int x=3,y=5;
        for(int i=0;content[i]&&y<21;i++){
            if(content[i]=='\n'||x>=76){x=3;y++;if(content[i]=='\n')continue;}
            vga_put(x++,y,content[i],t->win_fg,t->win_bg);
        }
        vga_puts(3,22,"ESC: back to files",t->win_border,t->win_bg);
        return;
    }

    // ── VESA pixel rendering ──────────────────────────────────────
    int W=(int)vesa.width, H=(int)vesa.height;
    int win_x=W/10, win_y=70, win_w=W*8/10, win_h=H-160;

    // ── Phase 1: backdrop effects drawn directly to the framebuffer
    if (compositor_is_enabled()) {
        compositor_blur_region(win_x - 8, win_y - 4, win_w + 16, win_h + 44, 6, 60);
    }

    // ── Phase 2: bind compositor surface — compositor handles body,
    // border, shadow, and title bar uniformly.
    // ── Window frame (body gradient, border, shadow, title bar) ──
    vesa_fill_rect(win_x+5, win_y+5, win_w, win_h, 0x05030C);
    icon_rrect_grad(win_x, win_y, win_w, win_h, 0x140C30, 0x0E0824);
    icon_rrect_border(win_x, win_y, win_w, win_h, 0x6A4FBF);

    int title_h = 36;
    icon_rrect_grad(win_x+1, win_y+1, win_w-2, title_h, 0x2E2050, 0x1E1240);
    vesa_hline(win_x+1, win_y+title_h, win_w-2, 0xB088FF);
    vesa_draw_string(win_x+18, win_y+10, filesystem[file_idx].name, 0xE8D8FF, 0);
    draw_close_btn(win_x+win_w-32, win_y+10, 0, "File Viewer");

    // Content area
    const char* content = filesystem[file_idx].content;
    int text_x = win_x+22, text_y = win_y+18 + title_h;
    int max_x  = win_x+win_w-22;
    int cx = text_x;
    for (int i=0; content[i] && text_y < win_y+win_h-30; i++) {
        if (content[i]=='\n') { cx=text_x; text_y+=18; continue; }
        if (cx+8 > max_x) { cx=text_x; text_y+=18; }
        vesa_draw_char(cx, text_y, content[i], 0xC8B8E8, 0);
        cx += 8;
    }

    int hint_y = win_y+win_h-26;
    vesa_hline(win_x+16, hint_y-8, win_w-32, 0x1E1240);    vesa_draw_string(win_x+22, hint_y, "ESC: back to file list", 0x3D2A66, 0);

    // ── Phase 3: unbind compositor surface
    if (compositor_is_enabled() && active_surface >= 0)
        compositor_surface_unbind();
 }

// Compact lunar eclipse logo for the About window — same motif as
static void draw_trash_window(){
    if(vesa.active){ draw_vesa_trash_panel(); return; }
    const Theme*t=TH;
    vga_fill_rect(2,2,76,20,' ',t->win_fg,t->win_bg);
    vga_fill_row(2,' ',t->win_title_fg,t->win_title_bg);
    vga_puts(3,2,"[ Trash Bin ]",t->win_title_fg,t->win_title_bg);
    vga_puts(74,2,"[X]",t->danger,t->win_title_bg);
    vga_puts(3,4,"-----------------------------------------",t->win_border,t->win_bg);
    if(trash_count==0){
        vga_puts(3,6,"Trash is empty.",t->win_border,t->win_bg);
    } else {
        int row=5;
        for(int i=0;i<MAX_TRASH&&row<20;i++){
            if(trash[i].used){
                vga_puts(3,row,"[DEL]  ",t->danger,t->win_bg);
                vga_puts(10,row,trash[i].name,t->win_fg,t->win_bg);
                row++;
            }
        }
        vga_puts(3,21,"R:restore   X:empty trash",t->win_border,t->win_bg);
    }
}

static void draw_music_window(){
    if(vesa.active){ draw_vesa_music_panel(); return; }
    const Theme*t=TH;
    vga_fill_rect(2,2,76,20,' ',t->win_fg,t->win_bg);
    vga_fill_row(2,' ',t->win_title_fg,t->win_title_bg);
    vga_puts(3,2,"[ Music Player ]",t->win_title_fg,t->win_title_bg);
    vga_puts(74,2,"[X]",t->danger,t->win_title_bg);
    vga_puts(3,4,"-----------------------------------------",t->win_border,t->win_bg);
    const Song*s=&playlist[music_current_song()];
    vga_puts(3,6,"Now Playing:",t->highlight,t->win_bg);
    vga_puts(3,7,s->title, t->accent,t->win_bg);
    vga_puts(3,8,s->artist,t->win_fg,t->win_bg);
    vga_puts(3,10,"Status:",t->highlight,t->win_bg);
    if(music_is_playing())vga_puts(12,10,">> PLAYING",t->success,t->win_bg);
    else                  vga_puts(12,10,"|| PAUSED ",t->warning,t->win_bg);
    vga_puts(3,12,"-----------------------------------------",t->win_border,t->win_bg);
    vga_puts(3,14,"Playlist:",t->highlight,t->win_bg);
    for(int i=0;i<playlist_count;i++){
        uint8_t fg=(i==music_current_song())?t->win_bg:t->win_fg;
        uint8_t bg=(i==music_current_song())?t->success:t->win_bg;
        vga_puts(3,15+i,(i==music_current_song())?">":" ",fg,bg);
        vga_puts(5,15+i,playlist[i].title,fg,bg);
    }
    vga_puts(3,23,"P:play/pause  N:next  B:prev  S:stop  ESC:back",t->win_border,t->win_bg);
}




// ══════════════════════════════════════════════════════════════════

#define SP_BG        0x12102A
#define SP_SIDEBAR   0x0D0B1F
#define SP_TITLEBAR  0x1A1440
#define SP_BORDER    0x2B2448
#define SP_DIVIDER   0x201D38
#define SP_ACT_BG    0x352878
#define SP_ACT_LINE  0x7050CC
#define SP_TEXT      0xE0D8FF
#define SP_TEXT_DIM  0x68589A
#define SP_TEXT_VAL  0x9B7FEE
#define SP_TEXT_LBL  0x9080B8
#define SP_SUCCESS   0x55EE88
#define SP_DANGER    0xFF6655
#define SP_HINT      0x504870

static int sp_strlen(const char*s){int i=0;while(s[i])i++;return i;}

extern void vesa_draw_char_scaled(int x,int y,char c,uint32_t fg,uint32_t bg,int scale);

static void sp_str_right(int rx,int y,const char*s,uint32_t fg,uint32_t bg){
    int len=sp_strlen(s);
    vesa_draw_string(rx-len*8,y,s,fg,bg);
}

// ══════════════════════════════════════════════════════════════════
//  VESA-native Music Player
// ══════════════════════════════════════════════════════════════════

static void draw_vesa_music_panel(void){
    int W=(int)vesa.width, H=(int)vesa.height;
    // ── Window position (draggable via title bar) ─────────────────
    if (g_win_x < 0) { g_win_x = 16; g_win_y = 40; }
    int wx=g_win_x, wy=g_win_y, ww=W-32, wh=H-50;
    g_win_w = ww; g_win_h = wh;
    int bh=44;

    // ── Backdrop blur + title bar ─────────────────────────────────
    if (compositor_is_enabled()) {
        compositor_blur_region(wx - 8, wy - 4, ww + 16, wh + 44, 6, 60);
    }

    // ── Title bar (drawn AFTER the backdrop blur so it stays crisp)
    icon_rrect_grad(wx, wy, ww, TB_H, 0x2E2050, 0x1E1240);
    vesa_hline(wx, wy + TB_H - 1, ww, 0xB088FF);
    vesa_draw_string(wx + 16, wy + (TB_H - 16) / 2, "Music Player", 0xE8D8FF, 0);
    draw_close_btn(wx + ww - 32, wy + (TB_H - 16) / 2, 0, "Music");

    int cy=wy + TB_H;
    int ch=wh - bh - TB_H;

    const Song*s=&playlist[music_current_song()];

    // ── Now-playing hero block
    int hero_h = ch*40/100; if(hero_h<140)hero_h=140;
    vesa_fill_rect(wx,cy,ww,hero_h,0x0D0B22);
    vesa_hline(wx,cy+hero_h,ww,SP_DIVIDER);

    // Big "album art" placeholder square with a waveform-ish glyph
    int art=hero_h-32; if(art>140)art=140;
    int ax=wx+24, ay=cy+(hero_h-art)/2;
    vesa_fill_rect(ax,ay,art,art,SP_ACT_BG);
    vesa_draw_rect(ax,ay,art,art,SP_ACT_LINE);
    // simple bar-equalizer glyph, animates with chord_phase via style
    {
        int bars=5;
        int bw=art/3, bgap=(art-bars*0)/1; (void)bgap;
        int base_x=ax+art/2-(bars*10)/2;
        int style=music_current_style();
        for(int b=0;b<bars;b++){
            int h2;
            if(music_is_playing()){
                int seed=(b*7+ (int)(s->length) + style*3) % 5;
                h2 = art*30/100 + seed*art*10/100;
            } else {
                h2 = art*20/100;
            }
            if(h2>art-16) h2=art-16;
            vesa_fill_rect(base_x+b*10, ay+art-12-h2, 6, h2, SP_TEXT);
        }
        (void)bw;
    }

    // Title / artist / status, to the right of the art square
    int tx=ax+art+24;
    int ty=ay+8;
    for(int i=0;s->title[i];i++)
        vesa_draw_char_scaled(tx+i*16,ty,s->title[i],SP_TEXT,0x0D0B22,2);
    vesa_draw_string(tx,ty+34,s->artist,SP_TEXT_VAL,0x0D0B22);

    // Status pill
    int sy2=ty+62;
    if(music_is_playing()){
        vesa_fill_rect(tx,sy2,110,26,0x123A1C);
        vesa_draw_rect(tx,sy2,110,26,SP_SUCCESS);
        vesa_draw_string(tx+12,sy2+5,">> Playing",SP_SUCCESS,0x123A1C);
    } else {
        vesa_fill_rect(tx,sy2,110,26,0x3A2E08);
        vesa_draw_rect(tx,sy2,110,26,0xFFAA00);
        vesa_draw_string(tx+12,sy2+5,"|| Paused",0xFFAA00,0x3A2E08);
    }

    // Harmony indicator — shows the chord engine actually doing
    // something, not just a flat beep, so the upgrade is visible.
    {
        int hx=tx+126, hy2=sy2;
        const char*label; uint32_t col; uint32_t bg2;
        switch(music_current_style()){
            case 1: label="Chord";   col=0x60D8FF; bg2=0x0C2840; break;
            case 2: label="Arpeggio";col=0xFFC060; bg2=0x3A2808; break;
            case 3: label="Vibrato"; col=0xFF90D0; bg2=0x3A0828; break;
            default:label=0;        col=0;        bg2=0; break;
        }
        if(label && music_is_playing()){
            int lw=sp_strlen(label)*8+24;
            vesa_fill_rect(hx,hy2,lw,26,bg2);
            vesa_draw_rect(hx,hy2,lw,26,col);
            vesa_draw_string(hx+12,hy2+5,label,col,bg2);
        }
    }

    // ── Tab bar ────────────────────────────────────────────────
    int py=cy+hero_h+16;
    int tab_y=py;
    int tab_h=30;
    const char* mtabs[]={"Playlist","Albums"};
    int mtx=wx+24;
    for(int ti=0;ti<2;ti++){
        int sel=(music_tab==ti);
        int tw=str_len(mtabs[ti])*8+24;
        if(sel){
            icon_rrect_grad(mtx,tab_y,tw,tab_h,0x2E2050,0x241A40);
            icon_rrect_border(mtx,tab_y,tw,tab_h,0xB088FF);
        } else {
            icon_rrect_border(mtx,tab_y,tw,tab_h,0x3D2A66);
        }
        vesa_draw_string(mtx+12,tab_y+7,mtabs[ti],sel?0xFFFFFF:0x9580C0,sel?0x2E2050:0x140C30);
        mtx+=tw+10;
    }
    vesa_hline(wx+12,tab_y+tab_h+6,ww-24,SP_DIVIDER);

    // ── Tab content ────────────────────────────────────────────
    int content_y=tab_y+tab_h+14;
    int row_h=38;

    if(music_tab==0){
        // Playlist view
        int list_y=content_y;
        for(int i=0;i<playlist_count;i++){
            int ry=list_y+i*row_h;
            if(ry+row_h>cy+ch) break;
            int cur=(i==music_current_song());
            uint32_t bg3=cur?SP_ACT_BG:SP_BG;
            uint32_t fg=cur?SP_TEXT:SP_TEXT_LBL;
            vesa_fill_rect(wx,ry,ww,row_h-2,bg3);
            if(cur) vesa_fill_rect(wx,ry,4,row_h-2,SP_ACT_LINE);
            const char*marker=cur?(music_is_playing()?">>":"||"):"  ";
            vesa_draw_string(wx+20,ry+11,marker,cur?SP_SUCCESS:SP_TEXT_DIM,bg3);
            vesa_draw_string(wx+56,ry+11,playlist[i].title,fg,bg3);
            sp_str_right(wx+ww-20,ry+11,playlist[i].artist,
                         cur?SP_TEXT_VAL:SP_TEXT_DIM,bg3);
            if(!cur) vesa_hline(wx+12,ry+row_h-3,ww-24,SP_DIVIDER);
        }
    } else {
        // Albums view — group songs by artist
        int ay=content_y;
        int shown[5]={0,0,0,0,0};  // tracks which songs have been displayed
        for(int a=0;a<playlist_count;a++){
            if(shown[a]) continue;
            // Group all songs by this artist
            const char* artist=playlist[a].artist;
            // Draw artist header
            {
                uint32_t ah_col=0x6A5890;
                vesa_draw_string(wx+24,ay,artist,ah_col,SP_BG);
                int hl=str_len(artist)*8;
                vesa_hline(wx+24+hl+8,ay+10,ww-24-hl-16,SP_DIVIDER);
                ay+=24;
            }
            // Draw this artist's songs
            for(int s=0;s<playlist_count;s++){
                if(shown[s]) continue;
                if(!str_eq(playlist[s].artist,artist)) continue;
                int cur=(s==music_current_song());
                uint32_t bg3=cur?SP_ACT_BG:SP_BG;
                uint32_t fg=cur?SP_TEXT:SP_TEXT_LBL;
                int ry=ay;
                if(ry+row_h>cy+ch) break;
                vesa_fill_rect(wx,ry,ww,row_h-2,bg3);
                if(cur) vesa_fill_rect(wx,ry,4,row_h-2,SP_ACT_LINE);
                const char*marker=cur?(music_is_playing()?">>":"||"):"  ";
                vesa_draw_string(wx+32,ry+11,marker,cur?SP_SUCCESS:SP_TEXT_DIM,bg3);
                vesa_draw_string(wx+68,ry+11,playlist[s].title,fg,bg3);
                shown[s]=1;
                ay+=row_h;
            }
            ay+=4;
        }
    }

    // ── Bottom transport bar
    int by=wy+wh-bh;
    vesa_fill_rect(wx,by,ww,bh,SP_TITLEBAR);
    vesa_hline(wx,by,ww,SP_BORDER);
    vesa_draw_string(wx+16,by+14,
        "P  play/pause     N  next     B  prev     S  stop     ESC  back",
        SP_HINT,SP_TITLEBAR);

    // ── Phase 3: unbind compositor surface
    if (compositor_is_enabled() && active_surface >= 0)
        compositor_surface_unbind();
}

static void sp_row(int x,int y,int w,const char*label,const char*value,
                   uint32_t lc,uint32_t vc,uint32_t bg){
    vesa_fill_rect(x,y,w,34,bg);
    vesa_draw_string(x+16,y+9,label,lc,bg);
    if(value&&value[0]) sp_str_right(x+w-16,y+9,value,vc,bg);
    vesa_hline(x+12,y+33,w-24,SP_DIVIDER);
}

static void sp_nav_item(int sx,int iy,int w,int h,
                        const char*icon,const char*label,int active){
    uint32_t bg=active?SP_ACT_BG:SP_SIDEBAR;
    uint32_t ic=active?SP_TEXT:SP_TEXT_DIM;
    uint32_t lc=active?SP_TEXT:SP_TEXT_LBL;
    vesa_fill_rect(sx,iy,w,h,bg);
    if(active){
        vesa_fill_rect(sx,iy,4,h,SP_ACT_LINE);
        vesa_draw_rect(sx,iy,w,h,SP_ACT_LINE);
    }
    int ty=iy+h/2-8;
    vesa_draw_string(sx+14,ty,icon,ic,bg);
    vesa_draw_string(sx+14+32,ty,label,lc,bg);
}

static void sp_heading(int cx,int cy,int cw,const char*title){
    vesa_draw_string(cx+16,cy+14,title,SP_TEXT,SP_BG);
    vesa_hline(cx+8,cy+34,cw-16,SP_DIVIDER);
}

static void sp_content_datetime(int cx,int cy,int cw,int ch){
    char ts[9];char ds[9];
    rtc_get_time_str(ts);rtc_get_date_str(ds);
    sp_heading(cx,cy,cw,"Date & Time");
    int y=cy+42;
    sp_row(cx,y,     cw,"Current Time",ts,SP_TEXT_LBL,SP_TEXT_VAL,SP_BG);
    sp_row(cx,y+34,  cw,"Current Date",ds,SP_TEXT_LBL,SP_TEXT_VAL,SP_BG);
    char tzdisp[48]; int p=0;
    const char*tz_name=timezones[current_timezone].name;
    const char*tz_city=timezones[current_timezone].city;
    for(int i=0;tz_name[i];i++) tzdisp[p++]=tz_name[i];
    tzdisp[p++]=' '; tzdisp[p++]='*'; tzdisp[p++]=' ';
    for(int i=0;tz_city[i]&&p<46;i++) tzdisp[p++]=tz_city[i];
    tzdisp[p]='\0';
    sp_row(cx,y+68,cw,"Timezone",tzdisp,SP_TEXT_LBL,SP_TEXT_VAL,SP_BG);
    int ly=y+115;
    vesa_draw_string(cx+16,ly+8,"Select Timezone",SP_TEXT,SP_BG);
    vesa_hline(cx+8,ly+28,cw-16,SP_DIVIDER);
    int list_y=ly+35; int row_h=34;
    int avail=(cy+ch)-list_y;
    int visible=avail/row_h; if(visible<2)visible=2; if(visible>12)visible=12;
    int start=settings_scroll;
    int end=start+visible; if(end>timezone_count)end=timezone_count;
    for(int i=start;i<end;i++){
        int ry=list_y+(i-start)*row_h;
        int sel=(i==settings_selected); int cur=(i==current_timezone);
        uint32_t bg=sel?SP_ACT_BG:SP_BG;
        uint32_t nfc=sel?SP_TEXT:SP_TEXT_VAL;
        uint32_t cfc=sel?SP_TEXT:(cur?SP_SUCCESS:SP_TEXT_DIM);
        vesa_fill_rect(cx,ry,cw,row_h,bg);
        if(sel) vesa_fill_rect(cx,ry,4,row_h,SP_ACT_LINE);
        vesa_draw_string(cx+16,ry+9,timezones[i].name,nfc,bg);
        sp_str_right(cx+cw-16,ry+9,timezones[i].city,cfc,bg);
        if(!sel) vesa_hline(cx+12,ry+row_h-1,cw-24,SP_DIVIDER);
    }
    if(start>0)
        vesa_draw_string(cx+cw/2-4,list_y-14,"^",SP_TEXT_VAL,SP_BG);
    if(end<timezone_count)
        vesa_draw_string(cx+cw/2-4,list_y+visible*row_h+2,"v",SP_TEXT_VAL,SP_BG);
}

static void sp_content_display(int cx,int cy,int cw,int ch){
    (void)ch;
    sp_heading(cx,cy,cw,"Display");
    int y=cy+42;
    char res_str[24]; int rp=0;
    int rw=(int)vesa.width, rh=(int)vesa.height;
    // Build "WxH" string
    {char tmp[8]; int tp=0; int v=rw;
     if(v==0){tmp[tp++]='0';}else{char t2[8];int t2p=0;while(v>0){t2[t2p++]='0'+v%10;v/=10;}for(int i=t2p-1;i>=0;i--)tmp[tp++]=t2[i];}
     for(int i=0;i<tp;i++) res_str[rp++]=tmp[i];}
    res_str[rp++]=' '; res_str[rp++]='x'; res_str[rp++]=' ';
    {char tmp[8]; int tp=0; int v=rh;
     if(v==0){tmp[tp++]='0';}else{char t2[8];int t2p=0;while(v>0){t2[t2p++]='0'+v%10;v/=10;}for(int i=t2p-1;i>=0;i--)tmp[tp++]=t2[i];}
     for(int i=0;i<tp;i++) res_str[rp++]=tmp[i];}
    res_str[rp]='\0';
    sp_row(cx,y,      cw,"Display Mode", "VESA Framebuffer",  SP_TEXT_LBL,SP_TEXT_VAL,SP_BG);
    sp_row(cx,y+34,   cw,"Resolution",   res_str,             SP_TEXT_LBL,SP_TEXT_VAL,SP_BG);
    sp_row(cx,y+68,   cw,"Color Depth",  "32 bpp",            SP_TEXT_LBL,SP_TEXT_VAL,SP_BG);
    sp_row(cx,y+102,  cw,"Font",         "8 x 16 bitmap",     SP_TEXT_LBL,SP_TEXT_VAL,SP_BG);

    // ── Resolution list ──────────────────────────────────────────
    int list_y = y + 140;
    vesa_draw_string(cx+16, list_y, "Available Resolutions (ENTER to switch):", SP_TEXT_LBL, SP_BG);
    int row_h = 24;
    int max_visible = (cy + ch - list_y - 40) / row_h;
    if (max_visible > vbe_mode_count) max_visible = vbe_mode_count;

    for (int i = 0; i < max_visible; i++) {
        int ry = list_y + 24 + i * row_h;
        int sel = (i == settings_selected);
        int cur = (i == vbe_active_mode);
        uint32_t bg = sel ? SP_ACT_BG : SP_BG;
        uint32_t fg = sel ? SP_TEXT : SP_TEXT_LBL;
        vesa_fill_rect(cx, ry, cw, row_h, bg);
        if (sel) vesa_fill_rect(cx, ry, 4, row_h, SP_ACT_LINE);

        // Resolution string
        char rbuf[24]; int ri = 0;
        int rw2 = vbe_mode_list[i].width;
        int rh2 = vbe_mode_list[i].height;
        { char t2[8]; int tp = 0; int v = rw2;
          while (v > 0) { t2[tp++] = '0' + v % 10; v /= 10; }
          for (int j = tp-1; j >= 0; j--) rbuf[ri++] = t2[j]; }
        rbuf[ri++] = ' '; rbuf[ri++] = 'x'; rbuf[ri++] = ' ';
        { char t2[8]; int tp = 0; int v = rh2;
          while (v > 0) { t2[tp++] = '0' + v % 10; v /= 10; }
          for (int j = tp-1; j >= 0; j--) rbuf[ri++] = t2[j]; }
        rbuf[ri] = '\0';

        vesa_draw_string(cx+24, ry + 4, rbuf, fg, bg);
        if (cur) vesa_draw_string(cx + 24 + 14 * 8, ry + 4, "[ACTIVE]", SP_SUCCESS, bg);
        if (!sel) vesa_hline(cx + 12, ry + row_h - 1, cw - 24, SP_DIVIDER);
    }

    // Color palette
    int py = list_y + max_visible * row_h + 40;
    if (py + 60 < cy + ch) {
        vesa_draw_string(cx+16, py, "Color Palette", SP_TEXT_LBL, SP_BG);
        static const uint32_t pal16[16]={
            0x000000,0x0000AA,0x00AA00,0x00AAAA,
            0xAA0000,0xAA00AA,0xAA5500,0xAAAAAA,
            0x555555,0x5555FF,0x55FF55,0x55FFFF,
            0xFF5555,0xFF55FF,0xFFFF55,0xFFFFFF};
        int px_start=cx+16;
        for(int i=0;i<16;i++){
            int sx=px_start+i*28;
            vesa_fill_rect(sx,py+22,24,24,pal16[i]);
            vesa_draw_rect(sx,py+22,24,24,SP_BORDER);
        }
    }
}

static void sp_content_sound(int cx,int cy,int cw,int ch){
    (void)ch;
    sp_heading(cx,cy,cw,"Sound");
    int y=cy+60;
    vesa_draw_string(cx+16,y,"System Volume",SP_TEXT_LBL,SP_BG);
    int by=y+30; int bar_x=cx+16;
    int bar_w=cw-32-60; int bar_h=18;
    vesa_fill_rect(bar_x,by,bar_w,bar_h,0x1A1840);
    vesa_draw_rect(bar_x,by,bar_w,bar_h,SP_BORDER);
    int filled=bar_w*sound_volume/100;
    if(filled>0) vesa_fill_rect(bar_x,by,filled,bar_h,SP_ACT_LINE);
    char vol_s[8]; int vp=0;
    int v=sound_volume;
    if(v==100){vol_s[vp++]='1';vol_s[vp++]='0';vol_s[vp++]='0';}
    else{vol_s[vp++]='0'+(v/10);vol_s[vp++]='0'+(v%10);}
    vol_s[vp++]='%'; vol_s[vp]='\0';
    vesa_draw_string(bar_x+bar_w+12,by+1,vol_s,SP_TEXT_VAL,SP_BG);
    vesa_draw_string(cx+16,by+36,"PC Speaker audio active",SP_SUCCESS,SP_BG);
    vesa_draw_string(cx+16,by+64,"LEFT / RIGHT  adjust volume    ENTER  test tone",SP_TEXT_DIM,SP_BG);
}

static void sp_content_network(int cx,int cy,int cw,int ch){
    (void)ch;
    sp_heading(cx,cy,cw,"Network");
    int y=cy+50;

    // Run scan every time the panel is shown so it always reflects
    // the current hardware state (scan is fast — 256 PCI reads max).
    net_scan_pci();

    // ── Scan badge
    {
        char badge[32]; int p=0;
        const char*pre="PCI Scan  ";
        for(int i=0;pre[i];i++) badge[p++]=pre[i];
        if(net_adapter_count==0){
            const char*s="0 adapters found";
            for(int i=0;s[i];i++) badge[p++]=s[i];
        } else {
            int n=net_adapter_count;
            badge[p++]='0'+n/10; badge[p++]='0'+n%10;
            const char*s=" adapter(s) found";
            for(int i=0;s[i];i++) badge[p++]=s[i];
        }
        badge[p]='\0';
        uint32_t bc=net_adapter_count>0?SP_SUCCESS:SP_TEXT_DIM;
        uint32_t bb=net_adapter_count>0?0x0A280E:0x0E0C20;
        int bw=sp_strlen(badge)*8+20;
        vesa_fill_rect(cx+16,y-28,bw,22,bb);
        vesa_draw_rect(cx+16,y-28,bw,22,bc);
        vesa_draw_string(cx+24,y-24,badge,bc,bb);
    }

    if(net_adapter_count==0){
        // ── No network hardware found
        vesa_draw_string(cx+16,y+8,
            "No PCI network controller detected",SP_DANGER,SP_BG);
        vesa_draw_string(cx+16,y+36,
            "PCI class 0x02 (Network) not found on buses 0-7",SP_TEXT_DIM,SP_BG);
        vesa_hline(cx+12,y+62,cw-24,SP_DIVIDER);
        vesa_draw_string(cx+16,y+74,"Possible reasons:",SP_TEXT_LBL,SP_BG);
        vesa_draw_string(cx+16,y+96, "  No physical NIC installed in this machine",SP_TEXT_DIM,SP_BG);
        vesa_draw_string(cx+16,y+114,"  Running inside a VM with networking disabled",SP_TEXT_DIM,SP_BG);
        vesa_draw_string(cx+16,y+132,"  NIC on a bus > 7 (rare) or USB-only network",SP_TEXT_DIM,SP_BG);
        vesa_draw_string(cx+16,y+160,"Drivers planned for Nyx OS v0.2",0x7060A8,SP_BG);
        return;
    }

    // ── Adapter list
    int row_h=90;
    for(int i=0;i<net_adapter_count;i++){
        NetAdapter*a=&net_adapters[i];
        int ry=y+i*row_h;
        if(ry+row_h>cy+ch-8) break;

        // Card background
        uint32_t card_bg = a->supported ? 0x101A14 : 0x100E1C;
        uint32_t acc     = a->supported ? SP_SUCCESS : 0xFF6655;
        vesa_fill_rect(cx+8,ry,cw-16,row_h-6,card_bg);
        vesa_fill_rect(cx+8,ry,4,row_h-6,acc);
        vesa_draw_rect(cx+8,ry,cw-16,row_h-6,a->supported?0x204820:SP_BORDER);

        // Type badge (Ethernet / WiFi / etc.)
        {
            int bw=sp_strlen(a->type_name)*8+16;
            uint32_t tc=a->subclass==0x80?0x60D8FF:SP_SUCCESS;
            uint32_t tb=a->subclass==0x80?0x0C2840:0x0A280E;
            vesa_fill_rect(cx+20,ry+8,bw,20,tb);
            vesa_draw_rect(cx+20,ry+8,bw,20,tc);
            vesa_draw_string(cx+28,ry+11,a->type_name,tc,tb);
        }

        // Vendor + device name
        {
            char title[60]; int p=0;
            for(int k=0;a->vendor_name[k]&&p<23;k++) title[p++]=a->vendor_name[k];
            title[p++]=' ';
            for(int k=0;a->device_name[k]&&p<58;k++) title[p++]=a->device_name[k];
            title[p]='\0';
            vesa_draw_string(cx+20,ry+34,title,SP_TEXT,card_bg);
        }

        // PCI address  (Bus:Slot.Func)
        {
            char addr[24]; int p=0;
            const char*pre="PCI ";
            for(int k=0;pre[k];k++) addr[p++]=pre[k];
            addr[p++]='0'+a->bus/10; addr[p++]='0'+a->bus%10;
            addr[p++]=':';
            addr[p++]='0'+a->slot/10; addr[p++]='0'+a->slot%10;
            addr[p++]='.'; addr[p++]='0'+a->func;
            addr[p]='\0';
            vesa_draw_string(cx+20,ry+54,addr,SP_TEXT_DIM,card_bg);

            // Vendor:Device hex IDs
            char ids[16]="  "; int ip=2;
            const char*hx="0123456789ABCDEF";
            ids[ip++]='[';
            ids[ip++]=hx[(a->vendor_id>>12)&0xF];
            ids[ip++]=hx[(a->vendor_id>>8)&0xF];
            ids[ip++]=hx[(a->vendor_id>>4)&0xF];
            ids[ip++]=hx[(a->vendor_id)&0xF];
            ids[ip++]=':';
            ids[ip++]=hx[(a->device_id>>12)&0xF];
            ids[ip++]=hx[(a->device_id>>8)&0xF];
            ids[ip++]=hx[(a->device_id>>4)&0xF];
            ids[ip++]=hx[(a->device_id)&0xF];
            ids[ip++]=']'; ids[ip]='\0';
            vesa_draw_string(cx+20+sp_strlen(addr)*8,ry+54,ids,0x504870,card_bg);
        }

        // Driver status
        {
            const char*status=a->supported?"Driver: planned (v0.2)":"No driver (WiFi needs WPA stack)";
            uint32_t sc=a->supported?SP_TEXT_VAL:0xFF8040;
            vesa_draw_string(cx+20,ry+70,status,sc,card_bg);
        }
    }
}

static void sp_content_system(int cx,int cy,int cw,int ch){
    (void)ch;
    sp_heading(cx,cy,cw,"System Info");
    int y=cy+42;
    sp_row(cx,y,       cw,"OS Name",       "Nyx OS",           SP_TEXT_LBL,SP_TEXT_VAL,SP_BG);
    sp_row(cx,y+34,    cw,"Version",        "0.1",              SP_TEXT_LBL,SP_TEXT_VAL,SP_BG);
    sp_row(cx,y+68,    cw,"Architecture",   "x86 32-bit",       SP_TEXT_LBL,SP_TEXT_VAL,SP_BG);
    sp_row(cx,y+102,   cw,"Kernel",         "Monolithic",       SP_TEXT_LBL,SP_TEXT_VAL,SP_BG);
    sp_row(cx,y+136,   cw,"Bootloader",     "GRUB Multiboot",   SP_TEXT_LBL,SP_TEXT_VAL,SP_BG);
    sp_row(cx,y+170,   cw,"Display",        "VESA Framebuffer", SP_TEXT_LBL,SP_TEXT_VAL,SP_BG);
    vesa_draw_string(cx+16,y+215,"Built from scratch in C & x86 Assembly",SP_TEXT_DIM,SP_BG);

    // ── Reset Icon Layout button ─────────────────────────────────
    int is_default = 1;
    for (int i = 0; i < g_grid_slots; i++) {
        int expected = (i < DESKTOP_APP_COUNT) ? i : -1;
        if (icon_order[i] != expected) { is_default = 0; break; }
    }
    uint32_t reset_fg = is_default ? 0x554466 : 0xFF8FA0;
    uint32_t reset_bg = is_default ? 0x201838 : 0x3A1040;
    uint32_t reset_bd = is_default ? 0x38304C : 0xCC5080;
    int bx = cx + 16, by = y + 245, bw = 220, bh = 30;
    vesa_fill_rect(bx, by, bw, bh, reset_bg);
    icon_rrect_border(bx, by, bw, bh, reset_bd);
    vesa_draw_string(bx + 16, by + 7, "Reset Icon Layout", reset_fg, reset_bg);
    if (!is_default) {
        // Animated glow pulse
        int gp = (tick * 4) % 60;
        int glow = gp < 30 ? gp * 6 : (60 - gp) * 6;
        if (glow > 128) glow = 128;
        uint32_t gl_col = (glow << 16) | (glow * 70 / 255 << 8) | glow;
        icon_rrect_border(bx - 1, by - 1, bw + 2, bh + 2, gl_col);
    }
}

/* Draw a text input field with label, value, cursor, and active highlight. */
static void sp_input(int cx,int y,int fw,
                     const char*label,const char*value,int vlen,
                     int active,int masked){
    /* label */
    vesa_draw_string(cx+16,y,label,active?SP_TEXT:SP_TEXT_LBL,SP_BG);
    /* box */
    int fy=y+20;
    uint32_t box_bg    = active ? SP_ACT_BG  : 0x0C0A20;
    uint32_t box_bord  = active ? SP_ACT_LINE : SP_BORDER;
    vesa_fill_rect(cx+16,fy,fw,32,box_bg);
    vesa_draw_rect(cx+16,fy,fw,32,box_bord);
    /* content */
    char show[20];
    if(masked){ for(int i=0;i<vlen;i++) show[i]='*'; show[vlen]='\0'; }
    else      { for(int i=0;i<vlen;i++) show[i]=value[i]; show[vlen]='\0'; }
    /* blinking cursor on active field */
    if(active && vlen<18){ show[vlen]='_'; show[vlen+1]='\0'; }
    vesa_draw_string(cx+24,fy+8,show,active?SP_TEXT:SP_TEXT_DIM,box_bg);
}

static void sp_content_users(int cx,int cy,int cw,int ch){
    (void)ch;
    int fw=cw-48;   /* input field width */

    /* ── Sub 0: user list ───────────────────────────────── */
    if(users_sub==0){
        sp_heading(cx,cy,cw,"Users");
        int y=cy+42;
        int row=0;
        for(int i=0;i<MAX_USERS;i++){
            if(!users[i].active) continue;
            int sel=(i==users_selected);
            uint32_t bg=sel?SP_ACT_BG:SP_BG;
            uint32_t fg=sel?SP_TEXT:SP_TEXT_LBL;
            int ry=y+row*52;
            vesa_fill_rect(cx,ry,cw,50,bg);
            if(sel) vesa_fill_rect(cx,ry,4,50,SP_ACT_LINE);
            /* role badge */
            uint32_t badge=(i==0)?SP_DANGER:SP_SUCCESS;
            const char*role=(i==0)?"[admin]":"[user] ";
            vesa_draw_string(cx+16,ry+17,role,badge,bg);
            /* username */
            vesa_draw_string(cx+80,ry+17,users[i].username,fg,bg);
            /* (you) tag */
            if(i==logged_in_user){
                int nx=cx+80+sp_strlen(users[i].username)*8+14;
                vesa_draw_string(nx,ry+17,"(you)",0x60E080,bg);
            }
            vesa_hline(cx+12,ry+49,cw-24,SP_DIVIDER);
            row++;
        }
        int hy=cy+42+row*52+14;
        /* action hints */
        vesa_draw_string(cx+16,hy,  "A  add user",    0x8080C8,SP_BG);
        vesa_draw_string(cx+16,hy+20,"C  change password",0x8080C8,SP_BG);
        vesa_draw_string(cx+16,hy+40,"D  delete user",  0x8080C8,SP_BG);
        /* status message */
        if(u_msg[0]){
            uint32_t mc=SP_SUCCESS;
            /* detect error messages */
            if(u_msg[0]=='C'&&u_msg[1]=='a') mc=SP_DANGER; /* "Cannot..." */
            if(u_msg[0]=='F') mc=SP_DANGER;                   /* "Failed!"   */
            if(u_msg[0]=='M') mc=0xFFAA00;                    /* "Max users" */
            vesa_draw_string(cx+16,hy+66,u_msg,mc,SP_BG);
        }
        return;
    }

    /* ── Sub 1: add user form ───────────────────────────── */
    if(users_sub==1){
        sp_heading(cx,cy,cw,"Add New User");
        int y=cy+56;
        sp_input(cx,y,    fw,"Username",u_new_name,u_name_pos,(u_field==0),0);
        sp_input(cx,y+72, fw,"Password",u_new_pass,u_pass_pos,(u_field==1),1);
        /* hints */
        int hy=y+160;
        vesa_draw_string(cx+16,hy,   "TAB  switch field",    0x8080C8,SP_BG);
        vesa_draw_string(cx+16,hy+20,"ENTER  create user",   0x60C060,SP_BG);
        vesa_draw_string(cx+16,hy+40,"ESC  cancel",          0x8080C8,SP_BG);
        if(u_msg[0]){
            uint32_t mc=(u_msg[0]=='U'||u_msg[0]=='P'||u_msg[0]=='F')?SP_DANGER:SP_SUCCESS;
            vesa_draw_string(cx+16,hy+66,u_msg,mc,SP_BG);
        }
        return;
    }

    /* ── Sub 2: change password form ────────────────────── */
    if(users_sub==2){
        /* heading shows whose password */
        char title[40];
        int tp=0;
        const char*pre="Change Password: ";
        for(int i=0;pre[i];i++) title[tp++]=pre[i];
        const char*uname=users[users_selected].username;
        for(int i=0;uname[i]&&tp<38;i++) title[tp++]=uname[i];
        title[tp]='\0';
        sp_heading(cx,cy,cw,title);
        int y=cy+56;
        sp_input(cx,y,     fw,"Current Password", u_old_pass,u_old_pos,    (u_field==0),1);
        sp_input(cx,y+72,  fw,"New Password",     u_new_pass,u_pass_pos,   (u_field==1),1);
        sp_input(cx,y+144, fw,"Confirm Password", u_confirm, u_confirm_pos,(u_field==2),1);
        /* hints */
        int hy=y+234;
        vesa_draw_string(cx+16,hy,   "TAB  switch field",  0x8080C8,SP_BG);
        vesa_draw_string(cx+16,hy+20,"ENTER  save change", 0x60C060,SP_BG);
        vesa_draw_string(cx+16,hy+40,"ESC  cancel",        0x8080C8,SP_BG);
        if(u_msg[0]){
            uint32_t mc=(u_msg[0]=='W'||u_msg[0]=='P'||u_msg[0]=='F')?SP_DANGER:
                         (u_msg[0]=='C'&&u_msg[1]=='h')?SP_SUCCESS:SP_DANGER;
            vesa_draw_string(cx+16,hy+66,u_msg,mc,SP_BG);
        }
        return;
    }
}

static void sp_content_wallpaper(int cx,int cy,int cw,int ch){
    (void)ch;
    sp_heading(cx,cy,cw,"Wallpaper");
    int y=cy+42;
    vesa_draw_string(cx+16,y+6,"Select a wallpaper  (ENTER to apply)",SP_TEXT_LBL,SP_BG);

    // ── Dim overlay slider ───────────────────────────────────────
    int dim_y = y + 30;
    vesa_draw_string(cx+16, dim_y, "Dim overlay:", SP_TEXT_LBL, SP_BG);

    int track_x = cx + 140;
    int track_y = dim_y + 4;
    int track_w = 180;
    int track_h = 8;

    // Track background
    uint32_t track_bg = 0x2A2060;
    vesa_fill_rect(track_x, track_y, track_w, track_h, track_bg);
    vesa_draw_rect(track_x, track_y, track_w, track_h, 0x4A3870);

    // Filled portion
    int fill_w = wallpaper_dim_level * track_w / 100;
    if (fill_w > 0) {
        vesa_fill_rect(track_x, track_y, fill_w, track_h, 0x8866FF);
    }

    // Knob
    int knob_x = track_x + fill_w;
    int knob_y = track_y + track_h / 2;
    int knob_r = 6;
    vesa_fill_rect(knob_x - knob_r, knob_y - knob_r, knob_r*2+1, knob_r*2+1, 0xE8D8FF);
    vesa_draw_rect(knob_x - knob_r, knob_y - knob_r, knob_r*2+1, knob_r*2+1, 0xFFFFFF);

    // Percentage label
    char dim_str[12];
    int di = 0;
    int dl = wallpaper_dim_level;
    // Show "Off" for 0, else percentage
    if (dl == 0) {
        dim_str[di++] = 'O'; dim_str[di++] = 'f'; dim_str[di++] = 'f';
    } else {
        if (dl >= 100) { dim_str[di++] = '1'; dl -= 100; }
        if (dl >= 10)  { dim_str[di++] = '0' + dl / 10; dl %= 10; }
        dim_str[di++] = '0' + dl;
        dim_str[di++] = '%';
    }
    dim_str[di] = '\0';
    vesa_draw_string(track_x + track_w + 12, dim_y, dim_str, 0xFFCC66, SP_BG);
    vesa_draw_string(track_x + track_w + 12, dim_y + 12, "L/R:adjust", SP_TEXT_DIM, SP_BG);

    // Separator
    vesa_hline(cx + 16, dim_y + 28, cw - 32, SP_DIVIDER);

    // Count image files in filesystem
    int img_files[MAX_FILES];
    int img_count = 0;
    for (int i = 0; i < MAX_FILES; i++) {
        if (!filesystem[i].used) continue;
        const unsigned char* d = (const unsigned char*)filesystem[i].content;
        int sz = filesystem[i].size;
        if (sz <= 0) { while (d[sz]) sz++; }
        if (sz < 4) continue;
        if ((d[0]==0x89 && d[1]=='P' && d[2]=='N' && d[3]=='G') ||
            (d[0]==0xFF && d[1]==0xD8 && d[2]==0xFF) ||
            (d[0]=='B' && d[1]=='M') ||
            (d[0]=='G' && d[1]=='I' && d[2]=='F')) {
            img_files[img_count++] = i;
        }
    }

    int total = WALLPAPER_COUNT + img_count;
    int list_y = dim_y + 36;
    int row_h = 52;

    // Built-in wallpapers
    for(int i=0;i<WALLPAPER_COUNT;i++){
        int ry=list_y+i*row_h;
        if(ry+row_h>cy+ch) break;
        int sel=(i==settings_selected); int cur=(custom_wallpaper_file < 0 && i==current_wallpaper);
        uint32_t bg=sel?SP_ACT_BG:SP_BG;
        uint32_t fg=sel?SP_TEXT:SP_TEXT_LBL;
        uint32_t df=sel?0xC0B0F0:SP_TEXT_DIM;
        vesa_fill_rect(cx,ry,cw,row_h,bg);
        if(sel) vesa_fill_rect(cx,ry,4,row_h,SP_ACT_LINE);
        int sw=24, sh=row_h-12;
        vesa_fill_rect(cx+16,ry+6,sw,sh,wallpapers[i].preview_color);
        vesa_draw_rect(cx+16,ry+6,sw,sh,(sel)?SP_ACT_LINE:SP_BORDER);
        vesa_draw_string(cx+16+sw+12, ry+10, wallpapers[i].name, fg, bg);
        vesa_draw_string(cx+16+sw+12, ry+28, wallpapers[i].desc, df, bg);
        if(cur) sp_str_right(cx+cw-16,ry+18,"[ACTIVE]",SP_SUCCESS,bg);
        if(!sel) vesa_hline(cx+12,ry+row_h-1,cw-24,SP_DIVIDER);
    }

    // Divider + filesystem images
    int base = WALLPAPER_COUNT;
    for(int i=0;i<img_count;i++){
        int idx = base + i;
        int ry = list_y + idx * row_h;
        if(ry+row_h>cy+ch) break;
        int sel=(idx==settings_selected); int cur=(custom_wallpaper_file == img_files[i]);
        uint32_t bg=sel?SP_ACT_BG:SP_BG;
        uint32_t fg=sel?SP_TEXT:SP_TEXT_LBL;
        uint32_t df=sel?0xC0B0F0:SP_TEXT_DIM;
        vesa_fill_rect(cx,ry,cw,row_h,bg);
        if(sel) vesa_fill_rect(cx,ry,4,row_h,SP_ACT_LINE);
        // File icon (text "[img]" placeholder)
        int sw=24, sh=row_h-12;
        vesa_fill_rect(cx+16,ry+6,sw,sh,0x3A2A5E);
        vesa_draw_rect(cx+16,ry+6,sw,sh,(sel)?SP_ACT_LINE:SP_BORDER);
        vesa_draw_string(cx+20,ry+14,"[IMG]",SP_TEXT_LBL,0x3A2A5E);
        // Filename
        vesa_draw_string(cx+16+sw+12, ry+10, filesystem[img_files[i]].name, fg, bg);
        char size_str[16];
        int fn = filesystem[img_files[i]].size;
        int spi = 0;
        if (fn >= 1024) { spi = 1;
            int kb = fn / 1024;
            if (kb >= 1000) { size_str[0] = '0' + kb/1000; size_str[1] = '0' + (kb/100)%10; size_str[2] = 'M'; size_str[3] = 'B'; size_str[4] = 0; }
            else { size_str[0] = '0' + kb/100; size_str[1] = '0' + (kb/10)%10; size_str[2] = '0' + kb%10; size_str[3] = 'K'; size_str[4] = 'B'; size_str[5] = 0; }
        } else {
            size_str[0] = '0' + fn/100; size_str[1] = '0' + (fn/10)%10; size_str[2] = '0' + fn%10; size_str[3] = 'B'; size_str[4] = 0;
        }
        vesa_draw_string(cx+16+sw+12, ry+28, size_str, df, bg);
        if(cur) sp_str_right(cx+cw-16,ry+18,"[ACTIVE]",SP_SUCCESS,bg);
        if(!sel) vesa_hline(cx+12,ry+row_h-1,cw-24,SP_DIVIDER);
    }
}

static void sp_content_theme(int cx,int cy,int cw,int ch){
    (void)ch;
    sp_heading(cx,cy,cw,"Theme");
    int y=cy+42;
    vesa_draw_string(cx+16,y+6,"Select a color theme",SP_TEXT_LBL,SP_BG);
    int list_y=y+32; int row_h=50;
    static const uint32_t pal[16]={
        0x000000,0x0000AA,0x00AA00,0x00AAAA,
        0xAA0000,0xAA00AA,0xAA5500,0xAAAAAA,
        0x555555,0x5555FF,0x55FF55,0x55FFFF,
        0xFF5555,0xFF55FF,0xFFFF55,0xFFFFFF};
    for(int i=0;i<THEME_COUNT;i++){
        int ry=list_y+i*row_h;
        int sel=(i==settings_selected); int cur=(i==current_theme);
        uint32_t bg=sel?SP_ACT_BG:SP_BG;
        uint32_t fg=sel?SP_TEXT:SP_TEXT_LBL;
        vesa_fill_rect(cx,ry,cw,row_h,bg);
        if(sel) vesa_fill_rect(cx,ry,4,row_h,SP_ACT_LINE);
        int sx=cx+16; int sy=ry+15; int sw2=22; int sh=20;
        vesa_fill_rect(sx+0, sy,sw2,sh,pal[themes[i].taskbar_bg &0xF]);
        vesa_fill_rect(sx+24,sy,sw2,sh,pal[themes[i].desktop_bg &0xF]);
        vesa_fill_rect(sx+48,sy,sw2,sh,pal[themes[i].highlight  &0xF]);
        vesa_fill_rect(sx+72,sy,sw2,sh,pal[themes[i].accent     &0xF]);
        vesa_draw_string(sx+100,ry+10, themes[i].name,fg,bg);
        vesa_draw_string(sx+100,ry+28, themes[i].desc,SP_TEXT_DIM,bg);
        if(cur) sp_str_right(cx+cw-16,ry+15,"[ACTIVE]",SP_SUCCESS,bg);
        if(!sel) vesa_hline(cx+12,ry+row_h-1,cw-24,SP_DIVIDER);
    }

    // ── Window body color picker ───────────────────────────────────
    if (compositor_is_enabled()) {
        int by = list_y + THEME_COUNT * row_h + 12;
        sp_heading(cx, by, cw, "Window Body Color");
        by += 42;
        vesa_draw_string(cx+16, by, "Choose a background color for app windows", SP_TEXT_LBL, SP_BG);
        by += 24;

        // 8 preset body colors — from file-scope arrays

        uint32_t cur_body = 0x140C30;
        if (active_surface >= 0)
            cur_body = compositor.surfaces[active_surface].body_color;

        int swatch_w = 50, swatch_h = 34, gap = 8;
        for (int i = 0; i < 8; i++) {
            int sx = cx + 16 + i * (swatch_w + gap);
            int sy = by;
            int sel = (body_pal[i] == cur_body);
            uint32_t border_col = sel ? SP_ACT_LINE : SP_BORDER;
            vesa_fill_rect(sx, sy, swatch_w, swatch_h, body_pal[i]);
            vesa_draw_rect(sx, sy, swatch_w, swatch_h, border_col);
            if (sel) {
                // Highlight ring
                vesa_draw_rect(sx-1, sy-1, swatch_w+2, swatch_h+2, SP_TEXT);
            }
            vesa_draw_string(sx + (swatch_w - str_len(body_names[i])*8) / 2,
                             sy + swatch_h + 4, body_names[i], SP_TEXT_DIM, SP_BG);
        }

        // Hint text showing how to change
        vesa_draw_string(cx+16, by + swatch_h + 24,
            "Press 1-8 to select a body color for the current window",
            SP_HINT, SP_BG);
    }
}

static void draw_vesa_settings_panel(void){
    int W=(int)vesa.width; int H=(int)vesa.height;
    // ── Window position (draggable via title bar) ─────────────────
    if (g_win_x < 0) { g_win_x = 16; g_win_y = 40; }
    int wx=g_win_x, wy=g_win_y;
    int ww=W-32, wh=H-50;
    g_win_w = ww; g_win_h = wh;
    int bh=30, sw=260, ih=56, ip=6;

    int cx=wx+sw+1;
    int cy=wy + TB_H;
    int cw=ww-sw-1;
    int ch=wh - bh - TB_H;

    // ── Backdrop blur + title bar ─────────────────────────────────
    if (compositor_is_enabled()) {
        compositor_blur_region(wx - 8, wy - 4, ww + 16, wh + 44, 6, 60);
        compositor_drop_shadow(wx, wy, ww, wh, 6, 80);
    }

    // ── Title bar (drawn AFTER the backdrop blur so it stays crisp)
    icon_rrect_grad(wx, wy, ww, TB_H, 0x2E2050, 0x1E1240);
    vesa_hline(wx, wy + TB_H - 1, ww, 0xB088FF);
    vesa_draw_string(wx + 16, wy + (TB_H - 16) / 2, "Settings", 0xE8D8FF, 0);
    draw_close_btn(wx + ww - 32, wy + (TB_H - 16) / 2, 0, "Settings");

    // ── Sidebar
    vesa_fill_rect(wx,cy,sw,ch+bh,SP_SIDEBAR);
    vesa_vline(wx+sw,cy,ch+bh,SP_BORDER);

    const char*nav_icons[]={"(o)","[-]","))","<*>"," i ","(@)","===","<|>"};
    const char*nav_labels[]={"Date & Time","Display","Sound","Network",
                              "System Info","Users","Wallpaper","Theme"};

    int highlight=(settings_section>=0)?settings_section:settings_selected;
    int sidebar_y=cy+ip;
    for(int i=0;i<8;i++)
        sp_nav_item(wx,sidebar_y+i*ih,sw,ih,nav_icons[i],nav_labels[i],(i==highlight));

    // ── Content panel
    vesa_fill_rect(cx,cy,cw,ch,SP_BG);
    int show_sec=(settings_section>=0)?settings_section:settings_selected;
    switch(show_sec){
        case 0: sp_content_datetime (cx,cy,cw,ch); break;
        case 1: sp_content_display  (cx,cy,cw,ch); break;
        case 2: sp_content_sound    (cx,cy,cw,ch); break;
        case 3: sp_content_network  (cx,cy,cw,ch); break;
        case 4: sp_content_system   (cx,cy,cw,ch); break;
        case 5: sp_content_users    (cx,cy,cw,ch); break;
        case 6: sp_content_wallpaper(cx,cy,cw,ch); break;
        case 7: sp_content_theme    (cx,cy,cw,ch); break;
        default: sp_content_datetime(cx,cy,cw,ch); break;
    }

    // ── Bottom bar
    int by=wy+wh-bh;
    vesa_fill_rect(wx,by,ww,bh,SP_TITLEBAR);
    vesa_hline(wx,by,ww,SP_BORDER);
    vesa_draw_string(wx+16,by+7,
        "UP/DOWN navigate  *  ENTER apply  *  TAB section  *  ESC back",
        SP_HINT,SP_TITLEBAR);

    // ── Phase 3: unbind compositor surface
    if (compositor_is_enabled() && active_surface >= 0)
        compositor_surface_unbind();
}

static void draw_settings_menu(){
    const Theme*t=TH;
    vga_fill_rect(2,2,76,20,' ',t->win_fg,t->win_bg);
    vga_fill_row(2,' ',t->win_title_fg,t->win_title_bg);
    vga_puts(3,2,"[ Settings ]",t->win_title_fg,t->win_title_bg);
    vga_puts(74,2,"[X]",t->danger,t->win_title_bg);
    vga_puts(3,4,"-----------------------------------------",t->win_border,t->win_bg);
    vga_puts(3,5,"Select a category:",t->win_fg,t->win_bg);
    const char*items[]={
        "  [*]  Date & Time","  [*]  Display","  [*]  Sound",
        "  [*]  Network","  [*]  System Info","  [*]  Users",
        "  [*]  Wallpaper","  [*]  Theme",
    };
    for(int i=0;i<8;i++){
        uint8_t fg=(i==settings_selected)?t->win_bg:t->win_fg;
        uint8_t bg=(i==settings_selected)?t->highlight:t->win_bg;
        vga_puts(3,8+i*2,items[i],fg,bg);
    }
    vga_puts(3,24,"UP/DOWN:navigate  ENTER:open  ESC:back",t->win_border,t->win_bg);
}

static void draw_settings_datetime(){
    const Theme*t=TH;
    vga_fill_rect(2,2,76,20,' ',t->win_fg,t->win_bg);
    vga_fill_row(2,' ',t->win_title_fg,t->win_title_bg);
    vga_puts(3,2,"[ Settings > Date & Time ]",t->win_title_fg,t->win_title_bg);
    char ts[9];char ds[9];rtc_get_time_str(ts);rtc_get_date_str(ds);
    vga_puts(3,4,"Current Time :",t->highlight,t->win_bg);vga_puts(18,4,ts,t->accent,t->win_bg);
    vga_puts(3,5,"Current Date :",t->highlight,t->win_bg);vga_puts(18,5,ds,t->accent,t->win_bg);
    vga_puts(3,6,"Timezone     :",t->highlight,t->win_bg);
    vga_puts(18,6,timezones[current_timezone].name,t->success,t->win_bg);
    vga_puts(26,6,timezones[current_timezone].city,t->win_fg,t->win_bg);
    vga_puts(3,8,"Select Timezone:",t->win_fg,t->win_bg);
    int start=settings_scroll;
    int end=start+9;if(end>timezone_count)end=timezone_count;
    for(int i=start;i<end;i++){
        int row=10+(i-start);
        uint8_t fg=(i==settings_selected)?t->win_bg:t->win_fg;
        uint8_t bg=(i==settings_selected)?t->highlight:t->win_bg;
        if(i==current_timezone&&bg==t->win_bg)fg=t->success;
        vga_puts(3,row,timezones[i].name,fg,bg);
        vga_puts(12,row,timezones[i].city,fg,bg);
        if(i==current_timezone)vga_puts(72,row,"[*]",t->success,bg);
    }
    if(settings_scroll>0) vga_puts(38,9,"[^]",t->accent,t->win_bg);
    if(end<timezone_count)vga_puts(38,19,"[v]",t->accent,t->win_bg);
    vga_puts(3,20,"ENTER:apply  UP/DOWN:scroll  ESC:back",t->win_border,t->win_bg);
}

static void draw_settings_display(){
    const Theme*t=TH;
    vga_fill_rect(2,2,76,20,' ',t->win_fg,t->win_bg);
    vga_fill_row(2,' ',t->win_title_fg,t->win_title_bg);
    vga_puts(3,2,"[ Settings > Display ]",t->win_title_fg,t->win_title_bg);
    vga_puts(3,6,"Display Mode  : VGA Text 80x25",  t->win_fg,t->win_bg);
    vga_puts(3,7,"Colors        : 16 color palette",t->win_fg,t->win_bg);
    vga_puts(3,8,"Resolution    : 720x400 (text)",  t->win_fg,t->win_bg);
    vga_puts(3,9,"Font          : 8x16 VGA ROM",    t->win_fg,t->win_bg);
    vga_puts(3,11,"Color Palette Preview:",t->highlight,t->win_bg);
    for(int i=0;i<16;i++){vga_put(3+i*2,13,' ',0,i);vga_put(4+i*2,13,' ',0,i);}
    vga_puts(3,20,"ESC:back",t->win_border,t->win_bg);
}

static void draw_settings_sound(){
    const Theme*t=TH;
    vga_fill_rect(2,2,76,20,' ',t->win_fg,t->win_bg);
    vga_fill_row(2,' ',t->win_title_fg,t->win_title_bg);
    vga_puts(3,2,"[ Settings > Sound ]",t->win_title_fg,t->win_title_bg);
    vga_puts(3,6,"System Volume:",t->highlight,t->win_bg);
    vga_puts(3,8,"[-]  ",t->win_fg,t->win_bg);
    int bar_len=sound_volume/5;
    for(int i=0;i<20;i++){vga_put(8+i,8,' ',0,(i<bar_len)?t->success:t->win_border);}
    vga_puts(29,8,"  [+]",t->win_fg,t->win_bg);
    char vol[4];vol[0]='0'+(sound_volume/100);vol[1]='0'+(sound_volume/10%10);vol[2]='0'+(sound_volume%10);vol[3]='\0';
    vga_puts(36,8,vol,t->accent,t->win_bg);vga_puts(40,8,"%",t->accent,t->win_bg);
    vga_puts(3,11,"PC Speaker audio active!",t->success,t->win_bg);
    vga_puts(3,20,"LEFT/RIGHT:volume  ENTER:test  ESC:back",t->win_border,t->win_bg);
}

static void draw_settings_network(){
    const Theme*t=TH;
    vga_fill_rect(2,2,76,20,' ',t->win_fg,t->win_bg);
    vga_fill_row(2,' ',t->win_title_fg,t->win_title_bg);
    vga_puts(3,2,"[ Settings > Network ]",t->win_title_fg,t->win_title_bg);
    vga_puts(3,6,"Status      : No network adapter",t->danger,t->win_bg);
    vga_puts(3,7,"IP Address  : 0.0.0.0",t->win_border,t->win_bg);
    vga_puts(3,11,"Coming in Nyx OS v0.2!",t->accent,t->win_bg);
    vga_puts(3,20,"ESC:back",t->win_border,t->win_bg);
}

static void draw_settings_system(){
    const Theme*t=TH;
    vga_fill_rect(2,2,76,20,' ',t->win_fg,t->win_bg);
    vga_fill_row(2,' ',t->win_title_fg,t->win_title_bg);
    vga_puts(3,2,"[ Settings > System Info ]",t->win_title_fg,t->win_title_bg);
    vga_puts(3,6,"OS Name      : Nyx OS",             t->win_fg,t->win_bg);
    vga_puts(3,7,"Version      : 0.1",                t->win_fg,t->win_bg);
    vga_puts(3,8,"Architecture : x86 32-bit",         t->win_fg,t->win_bg);
    vga_puts(3,9,"Kernel       : Monolithic",         t->win_fg,t->win_bg);
    vga_puts(3,10,"Boot         : GRUB Multiboot",    t->win_fg,t->win_bg);
    vga_puts(3,14,"Built from scratch in C & Assembly",t->accent,t->win_bg);

    // ── Reset Icon Layout button ─────────────────────────────────
    // Show the current icon_order[] state so the user can see whether
    // the layout has been customized.  When the layout equals the
    // identity (default) arrangement, the button is dimmed — still
    // clickable, just visually muted to indicate nothing needs doing.
    int is_default = 1;
    for (int i = 0; i < DESKTOP_APP_COUNT; i++) {
        if (icon_order[i] != i) { is_default = 0; break; }
    }
    uint32_t reset_fg = is_default ? 0x554466 : 0xFF8FA0;
    uint32_t reset_bg = is_default ? 0x18142A : 0x2A1030;
    uint32_t reset_bd = is_default ? 0x38304C : 0xCC4060;
    vesa_fill_rect(16, 220, 240, 28, reset_bg);
    icon_rrect_border(16, 220, 240, 28, reset_bd);
    vesa_draw_string(28, 226, "Reset Icon Layout", reset_fg, reset_bg);
    if (!is_default) {
        // Animated glow pulse on the non-default state
        int gp = (tick * 4) % 60;
        int glow = gp < 30 ? gp * 6 : (60 - gp) * 6;
        if (glow > 128) glow = 128;
        uint32_t gl_col = (glow << 16) | (glow * 70 / 255 << 8) | glow;
        icon_rrect_border(15, 219, 242, 30, gl_col);
    }
    vesa_draw_string(16, 262, "[Click to reset desktop icon", 0x8870A0, 0);
    vesa_draw_string(16, 272, " positions to their defaults.]", 0x8870A0, 0);

    vga_puts(3,20,"ESC:back",t->win_border,t->win_bg);
}

static void draw_users_window(){
    const Theme*t=TH;
    vga_fill_rect(2,2,76,20,' ',t->win_fg,t->win_bg);
    vga_fill_row(2,' ',t->win_title_fg,t->win_title_bg);
    vga_puts(74,2,"[X]",t->danger,t->win_title_bg);
    if(users_sub==0){
        vga_puts(3,2,"[ Settings > Users ]",t->win_title_fg,t->win_title_bg);
        vga_puts(3,5,"Manage Users:",t->highlight,t->win_bg);
        int row=6;
        for(int i=0;i<MAX_USERS;i++){
            if(users[i].active){
                uint8_t fg=(i==users_selected)?t->win_bg:t->win_fg;
                uint8_t bg=(i==users_selected)?t->highlight:t->win_bg;
                vga_puts(3,row,"[USER] ",t->success,bg);
                vga_puts(10,row,users[i].username,fg,bg);
                if(i==logged_in_user)vga_puts(28,row,"(you)",t->success,bg);
                if(i==0)vga_puts(35,row,"[admin]",t->danger,bg);
                row++;
            }
        }
        vga_puts(3,15,"A:add  C:change password  D:delete",t->win_fg,t->win_bg);
        if(u_msg[0])vga_puts(3,19,u_msg,t->success,t->win_bg);
        vga_puts(3,20,"UP/DOWN:select  ESC:back",t->win_border,t->win_bg);
    } else if(users_sub==1){
        vga_puts(3,2,"[ Settings > Users > Add User ]",t->win_title_fg,t->win_title_bg);
        vga_puts(3,6,"New Username:",t->highlight,t->win_bg);
        vga_fill_rect(3,7,30,1,' ',u_field==0?t->win_bg:t->win_fg,u_field==0?t->highlight:t->win_border);
        vga_puts(3,7,u_new_name,u_field==0?t->win_bg:t->win_fg,u_field==0?t->highlight:t->win_border);
        vga_puts(3,9,"New Password:",t->highlight,t->win_bg);
        vga_fill_rect(3,10,30,1,' ',u_field==1?t->win_bg:t->win_fg,u_field==1?t->highlight:t->win_border);
        char stars[17];for(int i=0;i<u_pass_pos;i++)stars[i]='*';stars[u_pass_pos]='\0';
        vga_puts(3,10,stars,u_field==1?t->win_bg:t->win_fg,u_field==1?t->highlight:t->win_border);
        if(u_msg[0])vga_puts(3,13,u_msg,t->danger,t->win_bg);
        vga_puts(3,20,"TAB:next  ENTER:create  ESC:back",t->win_border,t->win_bg);
    } else if(users_sub==2){
        vga_puts(3,2,"[ Settings > Users > Change Password ]",t->win_title_fg,t->win_title_bg);
        vga_puts(3,5,"User:",t->highlight,t->win_bg);vga_puts(9,5,users[users_selected].username,t->accent,t->win_bg);
        vga_puts(3,7,"Old Password:",t->highlight,t->win_bg);
        vga_fill_rect(3,8,30,1,' ',u_field==0?t->win_bg:t->win_fg,u_field==0?t->highlight:t->win_border);
        char old_s[17];for(int i=0;i<u_old_pos;i++)old_s[i]='*';old_s[u_old_pos]='\0';
        vga_puts(3,8,old_s,u_field==0?t->win_bg:t->win_fg,u_field==0?t->highlight:t->win_border);
        vga_puts(3,10,"New Password:",t->highlight,t->win_bg);
        vga_fill_rect(3,11,30,1,' ',u_field==1?t->win_bg:t->win_fg,u_field==1?t->highlight:t->win_border);
        char new_s[17];for(int i=0;i<u_pass_pos;i++)new_s[i]='*';new_s[u_pass_pos]='\0';
        vga_puts(3,11,new_s,u_field==1?t->win_bg:t->win_fg,u_field==1?t->highlight:t->win_border);
        vga_puts(3,13,"Confirm:",t->highlight,t->win_bg);
        vga_fill_rect(3,14,30,1,' ',u_field==2?t->win_bg:t->win_fg,u_field==2?t->highlight:t->win_border);
        char cs[17];for(int i=0;i<u_confirm_pos;i++)cs[i]='*';cs[u_confirm_pos]='\0';
        vga_puts(3,14,cs,u_field==2?t->win_bg:t->win_fg,u_field==2?t->highlight:t->win_border);
        if(u_msg[0])vga_puts(3,17,u_msg,t->danger,t->win_bg);
        vga_puts(3,20,"TAB:next  ENTER:save  ESC:back",t->win_border,t->win_bg);
    }
}

static void draw_settings_wallpaper(){
    const Theme*t=TH;
    vga_fill_rect(2,2,76,20,' ',t->win_fg,t->win_bg);
    vga_fill_row(2,' ',t->win_title_fg,t->win_title_bg);
    vga_puts(3,2,"[ Settings > Wallpaper ]",t->win_title_fg,t->win_title_bg);
    vga_puts(3,4,"Select a wallpaper:",t->highlight,t->win_bg);

    // Count image files in filesystem for display
    int img_files[MAX_FILES];
    int img_count = 0;
    for (int i = 0; i < MAX_FILES; i++) {
        if (!filesystem[i].used) continue;
        const unsigned char* d = (const unsigned char*)filesystem[i].content;
        int sz = filesystem[i].size;
        if (sz <= 0) { while (d[sz]) sz++; }
        if (sz < 4) continue;
        if ((d[0]==0x89 && d[1]=='P' && d[2]=='N' && d[3]=='G') ||
            (d[0]==0xFF && d[1]==0xD8 && d[2]==0xFF) ||
            (d[0]=='B' && d[1]=='M') ||
            (d[0]=='G' && d[1]=='I' && d[2]=='F')) {
            img_files[img_count++] = i;
        }
    }

    int total = WALLPAPER_COUNT + img_count;
    int row = 6;

    // Built-in wallpapers
    for (int i = 0; i < WALLPAPER_COUNT && row < 20; i++) {
        uint8_t fg = (settings_selected == i) ? t->win_bg : t->win_fg;
        uint8_t bg = (settings_selected == i) ? t->highlight : t->win_bg;
        vga_fill_rect(3, row, 50, 1, ' ', fg, bg);
        vga_puts(4, row, wallpapers[i].name, fg, bg);
        if (custom_wallpaper_file < 0 && i == current_wallpaper)
            vga_puts(22, row, "[ACTIVE]", t->success, bg);
        row += 2;
    }

    // Divider if there are image files
    if (img_count > 0 && row < 20) {
        vga_puts(3, row, "--- Custom Images ---", t->win_border, t->win_bg);
        row += 2;
    }

    // Filesystem image files
    for (int i = 0; i < img_count && row < 20; i++) {
        int sel_idx = WALLPAPER_COUNT + i;
        uint8_t fg = (settings_selected == sel_idx) ? t->win_bg : t->win_fg;
        uint8_t bg = (settings_selected == sel_idx) ? t->highlight : t->win_bg;
        vga_fill_rect(3, row, 50, 1, ' ', fg, bg);
        vga_puts(4, row, filesystem[img_files[i]].name, fg, bg);
        if (custom_wallpaper_file == img_files[i])
            vga_puts(22, row, "[ACTIVE]", t->success, bg);
        row += 2;
    }

    vga_puts(3, 22, "ENTER:apply  UP/DOWN:select  ESC:back", t->win_border, t->win_bg);
}

static void draw_settings_theme(){
    const Theme*t=TH;
    vga_fill_rect(2,2,76,20,' ',t->win_fg,t->win_bg);
    vga_fill_row(2,' ',t->win_title_fg,t->win_title_bg);
    vga_puts(3,2,"[ Settings > Theme ]",t->win_title_fg,t->win_title_bg);
    vga_puts(3,4,"Select a color theme:",t->highlight,t->win_bg);
    for(int i=0;i<THEME_COUNT;i++){
        uint8_t fg=(i==settings_selected)?0:t->win_fg;
        uint8_t bg=(i==settings_selected)?14:t->win_bg;
        vga_fill_rect(3,6+i*2,72,1,' ',fg,bg);
        vga_put(4,6+i*2,' ',0,themes[i].taskbar_bg);
        vga_put(5,6+i*2,' ',0,themes[i].desktop_bg);
        vga_put(6,6+i*2,' ',0,themes[i].highlight);
        vga_put(7,6+i*2,' ',0,themes[i].accent);
        vga_puts(9,6+i*2,themes[i].name,fg,bg);
        vga_puts(20,6+i*2,themes[i].desc,fg,bg);
        if(i==current_theme)vga_puts(60,6+i*2,"[ACTIVE]",t->success,bg);
    }
    vga_puts(3,22,"ENTER:apply  UP/DOWN:select  ESC:back",t->win_border,t->win_bg);
}

// ══════════════════════════════════════════════════════════════════
//  VESA-native Trash Bin
// ══════════════════════════════════════════════════════════════════

static void draw_vesa_trash_panel(void){
    int W=(int)vesa.width, H=(int)vesa.height;
    // ── Window position (draggable via title bar) ─────────────────
    if (g_win_x < 0) { g_win_x = 16; g_win_y = 40; }
    int wx=g_win_x, wy=g_win_y, ww=W-32, wh=H-50;
    g_win_w = ww; g_win_h = wh;
    int bh=30;

    // ── Backdrop blur + title bar ─────────────────────────────────
    if (compositor_is_enabled()) {
        compositor_blur_region(wx - 8, wy - 4, ww + 16, wh + 44, 6, 60);
    }

    // ── Title bar (drawn AFTER the backdrop blur so it stays crisp)
    icon_rrect_grad(wx, wy, ww, TB_H, 0x2E2050, 0x1E1240);
    vesa_hline(wx, wy + TB_H - 1, ww, 0xB088FF);
    vesa_draw_string(wx + 16, wy + (TB_H - 16) / 2, "Trash Bin", 0xE8D8FF, 0);
    draw_close_btn(wx + ww - 32, wy + (TB_H - 16) / 2, 0, "Trash");

    int cy=wy + TB_H;
    int ch=wh - bh - TB_H;
    vesa_fill_rect(wx,cy,ww,ch,SP_BG);

    // ── Empty state
    if(trash_count==0){
        const char*msg="Trash is empty";
        const char*sub="Deleted files from the File Manager will appear here";
        int mx=wx+(ww-sp_strlen(msg)*16)/2;
        int my=cy+ch/2-30;
        // Big dim icon-ish bracket using existing string drawing only
        vesa_draw_string(wx+ww/2-24,my-40,"[ ]",SP_TEXT_DIM,SP_BG);
        for(int i=0;msg[i];i++)
            vesa_draw_char_scaled(mx+i*16,my,msg[i],SP_TEXT_DIM,SP_BG,2);
        int sx=wx+(ww-sp_strlen(sub)*8)/2;
        vesa_draw_string(sx,my+36,sub,SP_TEXT_LBL,SP_BG);
    } else {
        // ── List header
        vesa_draw_string(wx+20,cy+10,"Name",SP_TEXT_LBL,SP_BG);
        sp_str_right(wx+ww-20,cy+10,"Size",SP_TEXT_LBL,SP_BG);
        vesa_hline(wx+12,cy+30,ww-24,SP_DIVIDER);

        int row_h=40;
        int list_y=cy+36;
        int avail=(cy+ch)-list_y-8;
        int visible=avail/row_h; if(visible<1)visible=1;

        // Build a compact index of used slots so navigation feels like
        // a normal list (no gaps for freed slots in the middle)
        int idxs[MAX_TRASH]; int n=0;
        for(int i=0;i<MAX_TRASH;i++) if(trash[i].used) idxs[n++]=i;
        if(trash_selected>=n) trash_selected=n>0?n-1:0;

        int scroll=0;
        if(trash_selected>=visible) scroll=trash_selected-visible+1;

        for(int row=0;row<visible;row++){
            int li=row+scroll;
            if(li>=n) break;
            int ti=idxs[li];
            int ry=list_y+row*row_h;
            int sel=(li==trash_selected);
            int hover = !sel && mouse_in_rect(wx, ry, ww, row_h - 2);
            uint32_t bg=sel?SP_ACT_BG:(hover?0x1C1640:SP_BG);
            uint32_t fg=sel?SP_TEXT:(hover?0xE8D8FF:SP_TEXT_LBL);
            vesa_fill_rect(wx,ry,ww,row_h-2,bg);
            if(sel) vesa_fill_rect(wx,ry,4,row_h-2,SP_ACT_LINE);
            else if (hover) vesa_fill_rect(wx,ry,4,row_h-2,0x5A3F9F);
            vesa_draw_string(wx+20,ry+12,trash[ti].name,fg,bg);
            // file size = strlen of content, formatted as "N B"
            int sz=trash[ti].size; if(sz<=0 && trash[ti].content){ while(sz<trash[ti].content_cap-1 && trash[ti].content[sz]) sz++; }
            char szs[12]; int sp=0;
            if(sz==0){szs[sp++]='0';}
            else{char t3[8];int tp=0;int v=sz;while(v>0){t3[tp++]='0'+v%10;v/=10;}for(int i=tp-1;i>=0;i--)szs[sp++]=t3[i];}
            szs[sp++]=' ';szs[sp++]='B';szs[sp]='\0';
            sp_str_right(wx+ww-20,ry+12,szs,sel?SP_TEXT:(hover?0xE8D8FF:SP_TEXT_DIM),bg);
            if(!sel && !hover) vesa_hline(wx+12,ry+row_h-3,ww-24,SP_DIVIDER);
        }

        // Scrollbar
        if(n>visible){
            int track_x=wx+ww-8;
            vesa_vline(track_x,list_y,avail,SP_BORDER);
            int bar_h=avail*visible/n; if(bar_h<10)bar_h=10;
            int bar_y=list_y+(avail-bar_h)*scroll/(n-visible>0?n-visible:1);
            vesa_fill_rect(track_x-2,bar_y,4,bar_h,SP_ACT_LINE);
        }
    }

    // ── Bottom hint bar
    int by=wy+wh-bh;
    vesa_fill_rect(wx,by,ww,bh,SP_TITLEBAR);
    vesa_hline(wx,by,ww,SP_BORDER);
    if(trash_count>0)
        vesa_draw_string(wx+16,by+7,
            "UP/DOWN select  *  R restore  *  X empty all  *  DEL/BKSP delete one  *  ESC back",
            SP_HINT,SP_TITLEBAR);
    else
        vesa_draw_string(wx+16,by+7,"ESC back",SP_HINT,SP_TITLEBAR);

    // ── Phase 3: unbind compositor surface
    if (compositor_is_enabled() && active_surface >= 0)
        compositor_surface_unbind();
}

static void draw_settings_window(){
    // VESA mode: use the pixel-perfect panel
    if(vesa.active){ draw_vesa_settings_panel(); return; }
    // VGA text fallback
    if     (settings_section==-1)draw_settings_menu();
    else if(settings_section==0) draw_settings_datetime();
    else if(settings_section==1) draw_settings_display();
    else if(settings_section==2) draw_settings_sound();
    else if(settings_section==3) draw_settings_network();
    else if(settings_section==4) draw_settings_system();
    else if(settings_section==5) draw_users_window();
    else if(settings_section==6) draw_settings_wallpaper();
    else if(settings_section==7) draw_settings_theme();
}

// ─── Heartbeat: timer-driven redraw (called from idle loop) ────
// This runs ~18 times per second (every PIT tick) to drive
// animations that need automatic refresh with no keyboard input,
// such as the gallery slideshow and crossfade transitions.
// ─── Keyboard ring buffer drain (main loop, not ISR) ───────────
// The keyboard ISR pushes converted characters into a ring buffer.
// This function drains them in the main loop (interrupts enabled)
// so that blocking operations (shell_process, VESA drawing) don't
// lock out keyboard/timer interrupts.
//
// Terminal input:   backspace/ENTER/printable → cmd_buffer
//                   Up/Down (arrow 'A'/'B') → history navigation
//                   ESC/TAB → desktop_handle_key
// Login / Power:    direct delegate
// Other modes:      desktop_handle_key

#include <login.h>
#include <power.h>
#include <shell.h>
#include <terminal.h>
#include <keyboard.h>
#include <browser.h>

extern void desktop_terminal_clear(void);  // defined below

// ─── Browser navigation (deferred) ───────────────────────────────
#include <browser.h>

static void desktop_process_deferred(void) {
    browser_process_pending();
}

// Set by desktop_heartbeat() when the video player's frame advanced.
// The actual redraw (which JPEG-decodes the frame) is deferred to
// desktop_video_pump() in main-loop context — decoding inside the PIT
// IRQ would nest jpeg_decode (~15 KB of stack) on top of whatever the
// IRQ interrupted, overflowing the kernel stack and freezing the OS.
static int video_redraw_pending = 0;

// Called from the kernel main loop (mirrors desktop_icon_pump) so the
// video's per-frame redraw+decode never runs inside an IRQ handler.
void desktop_video_pump(void) {
    if (!video_redraw_pending) return;
    video_redraw_pending = 0;
    desktop_redraw_no_click();
    // Decode-ahead: after the present, pre-decode the next frame into
    // the video player's back buffer during this frame's window, so
    // the next present at its PTS is a pure blit (no decode in the
    // present path).  Same main-loop context as the redraw.
    video_pump_ahead();
}

// ─── Game-mode desktop coupling (called from game.c via syscalls) ──
static int game_return_mode = -1;   // mode to restore when the game exits

void desktop_enter_game(const char* title, int w, int h) {
    if (!vesa.active) return;           // game canvas needs graphics mode
    if (game_start_sized(title, w, h) != 0) return;
    game_return_mode = current_mode;    // usually MODE_FILES (the .exe open)
    game_set_pid(task_current_id());
    current_mode = MODE_GAME;
    desktop_redraw_no_click();
}

void desktop_exit_game(void) {
    if (!game_active()) return;
    game_stop();
    int ret = game_return_mode;
    game_return_mode = -1;
    current_mode = (ret >= 0) ? ret : MODE_DESKTOP;
    desktop_redraw_no_click();
    // E2E hook: lets tests observe that a game session actually ended
    // ([X] click or the game's own quit), not just stopped painting.
    { static const char m[] = "GAME CLOSED\n";
      for (const char* p = m; *p; p++) {
          int sp = 0;
          while ((inb(0x3F8 + 5) & 0x20) == 0 && sp++ < 100000) { }
          outb(0x3F8, (uint8_t)*p);
      } }
}

void desktop_heartbeat(void) {
    desktop_process_deferred();

    // ── Live Terminal repaint ────────────────────────────────────
    // When a launched ring-3 app writes to the console (sys_write is
    // mirrored into term_buf), repaint while the Terminal window is
    // focused so its output streams on screen.  Keyboard echoes
    // already trigger their own redraw, so this only catches output
    // that arrives between keypresses (e.g. a running store app).
    if (current_mode == MODE_TERMINAL) {
        static int ht_lc = 0, ht_cc = 0;
        if (term_buf.line_count != ht_lc || term_buf.cur_col != ht_cc) {
            ht_lc = term_buf.line_count;
            ht_cc = term_buf.cur_col;
            desktop_redraw_no_click();
        }
    }

    // ── Quick Settings slider drag ───────────────────────────────
    // While the flyout is open and the left button is held over a
    // slider track, follow the pointer on the PIT clock (50 Hz).
    qs_drag_handle();

    // ── Wi-Fi flyout scan timer ──────────────────────────────────
    // Counts down the "Scanning for networks" animation; a redraw
    // every 4 ticks animates the dots at ~12 Hz without a 50 Hz
    // redraw storm.
    if (qs_wifi_scan > 0 &&
        ((current_mode == MODE_QUICKSETTINGS && qs_wifi_open) ||
         wifi_popup_open)) {
        qs_wifi_scan--;
        if ((qs_wifi_scan & 3) == 0) desktop_redraw_no_click();
    }

    // ── Video player playback clock ─────────────────────────────
    // While the Video app is open, advance the current frame on the
    // PIT clock.  video_tick() only bumps the frame index (cheap);
    // the expensive redraw+decode is flagged here and performed by
    // desktop_video_pump() in main-loop context.
    if (current_mode == MODE_VIDEO) {
        if (video_tick()) {
            video_redraw_pending = 1;
        }
    }

    // ── Game lifecycle watcher ────────────────────────────────
    // The [X] kill path restores the mode explicitly (mouse.c); this
    // catches every other exit (I_Error, crash, external task_kill):
    // when the owning task's PCB is freed, task_name() starts
    // returning "-" — close game mode so the desktop comes back.
    if (current_mode == MODE_GAME && game_active()) {
        int gp = game_pid();
        if (gp < 0 || (gp > 0 && task_name(gp)[0] == '-'))
            desktop_exit_game();
    }

    // ── Game-mode frame pump ─────────────────────────────────
    // The ring-3 game (DOOM.EXE) blits frames via SYS_GAMEBLIT from
    // its own task; the desktop shows the newest canvas here on the
    // 50 Hz heartbeat (DOOM runs 35 fps, so every frame is shown and
    // the game's actual FPS only depends on its own speed).
    if (current_mode == MODE_GAME && game_dirty()) {
        game_clear_dirty();
        desktop_redraw_no_click();
    }

    // ── Local Disk capacity-bar animation ────────────────────────
    // While the This PC home is visible, glide the drive bar toward
    // the real usage % on the PIT clock.  The first frame snaps to
    // the true value (no glide from 0 at boot); afterwards the
    // displayed % eases toward the target at DRIVE_PCT_EASE of the
    // remaining gap per tick and redraws only while still moving,
    // so idle ticks cost nothing.  The target is NOT recomputed here
    // while settled: every file create/delete/save already triggers a
    // redraw that refreshes drive_pct_target in draw_files_window(),
    // so scanning all files at 18 Hz when nothing changed would be
    // wasted work.  Skipped while dragging — the drag ghost owns the
    // frame then.
    if (vesa.active && current_mode == MODE_FILES && files_home_view &&
        viewing_file < 0 && !launcher_is_open() && !mouse.drag_active) {
        if (drive_pct_disp < 0) {
            drive_pct_disp = files_drive_pct();      // first frame: no glide
        } else if (drive_pct_disp != drive_pct_target) {
            drive_pct_target = files_drive_pct();    // refresh while animating
            int diff = drive_pct_target - drive_pct_disp;
            int step = diff * DRIVE_PCT_EASE / 100;
            if (step == 0) step = (diff > 0) ? 1 : -1;   // always progress
            drive_pct_disp += step;
            if ((diff > 0 && drive_pct_disp >= drive_pct_target) ||
                (diff < 0 && drive_pct_disp <= drive_pct_target))
                drive_pct_disp = drive_pct_target;       // snap on arrival
            desktop_redraw_no_click();
        }
    }

    // ── Touchpad two-finger scroll (PS/2 wheel) ──────────────────
    // The mouse IRQ decodes IntelliMouse wheel deltas into
    // mouse_wheel_delta.  Route them to whichever scrollable list
    // is open: the Files file-list (files_scroll) or the Trash
    // (trash_selected drives its scrollbar).  Consumed and cleared
    // every tick so the accumulator can never grow unbounded.
    if (mouse_wheel_delta) {
        int wd = mouse_wheel_delta;
        mouse_wheel_delta = 0;
        int handled = 0;
        // PS/2 wheel convention: positive Z = wheel up = view toward
        // the top, so scroll offsets DECREASE.  The draw paths clamp
        // files_scroll to [0, nrows-visible]; trash_selected is
        // clamped here and again in the draw paths.
        // Each notch is ±1, so scale by 2 for a normal-OS feel
        // (~2 rows per notch instead of a sluggish single row).

        // Launcher overlay takes precedence over whatever window is
        // open below (mirrors the keyboard handler, which checks
        // launcher_is_open() first).  Wheel moves the pinned-app
        // selection, wrapping like the A/B keys.  launcher_draw() is
        // self-contained (paints + presents), same as the keyboard path.
        if (launcher_is_open()) {
            int s = launcher_get_sel();
            s -= wd;
            while (s < 0) s += PINNED_COUNT;
            while (s >= PINNED_COUNT) s -= PINNED_COUNT;
            launcher_set_sel(s);
            launcher_draw();
            return;
        }

        if (current_mode == MODE_FILES && !files_home_view && viewing_file < 0) {
            files_scroll -= wd * 2;
            handled = 1;
        } else if (current_mode == MODE_BROWSER) {
            // Browser — wheel scrolls the rendered page.  Same sign
            // convention as Files/Trash above: a wheel-down event
            // (wd > 0) moves the view toward the top (offset
            // decreases), so `-wd * 3`; htmlview_scroll takes signed
            // lines, positive = down.  Under the host's natural-
            // scrolling touchpad, wheel-down == two-finger swipe
            // down == reveal earlier content, exactly like Files.
            // ~3 lines per notch suits dense article text, and
            // htmlview_scroll returns 0 at the edges so no redraw
            // is wasted.
            if (htmlview_scroll(-wd * 3)) handled = 1;
        } else if (current_mode == MODE_TRASH) {
            trash_selected -= wd * 2;
            if (trash_selected < 0) trash_selected = 0;
            handled = 1;
        } else if (current_mode == MODE_SETTINGS) {
            // Settings app — route the wheel by the open section.
            // Selection lists move 1 item per notch (matching the
            // A/B keys); the long timezone list follows the same
            // 9-row scroll window as the keyboard handler.
            if (settings_section == -1) {          // main menu (8 sections)
                settings_selected -= wd;
                if (settings_selected < 0) settings_selected = 0;
                if (settings_selected > 7) settings_selected = 7;
                handled = 1;
            } else if (settings_section == 0) {    // timezone list
                settings_selected -= wd;
                if (settings_selected < 0) settings_selected = 0;
                if (settings_selected >= timezone_count) settings_selected = timezone_count - 1;
                if (settings_selected < settings_scroll) settings_scroll = settings_selected;
                if (settings_selected >= settings_scroll + 9) settings_scroll = settings_selected - 9 + 1;
                handled = 1;
            } else if (settings_section == 1) {    // display resolutions
                int dm = vbe_mode_count;
                settings_selected -= wd;
                if (settings_selected < 0) settings_selected = 0;
                if (dm > 0 && settings_selected >= dm) settings_selected = dm - 1;
                handled = 1;
            } else if (settings_section == 5 && users_sub == 0) {  // user list
                users_selected -= wd;
                if (users_selected < 0) users_selected = 0;
                if (user_count > 0 && users_selected >= user_count)
                    users_selected = user_count - 1;
                handled = 1;
            } else if (settings_section == 6) {    // wallpaper list
                int img_count = 0;
                for (int i = 0; i < MAX_FILES; i++) {
                    if (!filesystem[i].used) continue;
                    const unsigned char* d = (const unsigned char*)filesystem[i].content;
                    int sz = filesystem[i].size;
                    if (sz <= 0) { while (d[sz]) sz++; }
                    if (sz < 4) continue;
                    if ((d[0]==0x89 && d[1]=='P' && d[2]=='N' && d[3]=='G') ||
                        (d[0]==0xFF && d[1]==0xD8 && d[2]==0xFF) ||
                        (d[0]=='B' && d[1]=='M') ||
                        (d[0]=='G' && d[1]=='I' && d[2]=='F')) img_count++;
                }
                int total = WALLPAPER_COUNT + img_count;
                settings_selected -= wd;
                if (settings_selected < 0) settings_selected = 0;
                if (settings_selected >= total) settings_selected = total - 1;
                handled = 1;
            } else if (settings_section == 7) {    // theme list
                settings_selected -= wd;
                if (settings_selected < 0) settings_selected = 0;
                if (settings_selected >= THEME_COUNT) settings_selected = THEME_COUNT - 1;
                handled = 1;
            } else if (vesa.active && settings_section >= 0 &&
                       settings_section != 0 && settings_section != 1 &&
                       settings_section != 2 && settings_section != 5 &&
                       settings_section != 6 && settings_section != 7) {
                // Network / System Info — no list; wheel cycles the
                // sidebar section exactly like the A/B keys do.
                settings_section -= wd;
                if (settings_section < 0) settings_section = 0;
                if (settings_section > 7) settings_section = 7;
                handled = 1;
            }
        }
        if (handled) {
            desktop_draw();   // re-render with the new selection
            return;
        }
    }

    // ── Mouse-click/drop dispatch ────────────────────────────────
    // Trigger a full desktop_draw() when any mouse event needs
    // processing: clicks, right-clicks, or drops (drag release).
    // Drop_pending is essential — without it, desktop_draw() never
    // runs after the initial click (which consumes click_pending),
    // so the drop dispatch never fires and the icon swap is stranded.
    // mouse_get_click/drop() consume their events inside desktop_draw().
    // NOTE: mouse_is_dragging() is deliberately NOT included here —
    // calling desktop_draw() on every PIT tick during a drag
    // redraws the entire scene (wallpaper, icons, taskbar) and
    // overwhelms the rendering pipeline, freezing the system.

    // ── Decay squish-drop animation ──────────────────────────────
    if (anim_frame > 0) {
        anim_frame--;
        if (anim_frame == 0) anim_slot = -1;
    }

    // ── Spring-loaded snap animation ─────────────────────────────
    // Force a redraw every PIT tick while the ghost is flying back
    // to its source slot.  The interpolation happens inside draw_icons().
    if (snap_active) {
        if (snap_frame > SNAP_FRAMES) {
            snap_active = 0;
            snap_slot = -1;
        } else {
            desktop_draw();
        }
        snap_frame++;
        return;
    }

    // ── Slide-in animation (200 Hz flying ghost) ──────────────────
    // Both the PS/2 IRQ and this heartbeat advance the sub-tick
    // interpolation clock and render the ghost (IRQ at 200 Hz when
    // moving, heartbeat as the idle fallback).  We check
    // mag_commit_pending each PIT tick; when the animation finishes
    // it is set, and we commit the icon_order[] swap/move and play
    // the squish flash.
    if (mag_commit_pending) {
        mag_commit_pending = 0;

        // ── Save undo snapshot BEFORE modifying icon_order ───────
        undo_ptr++;
        if (undo_ptr >= UNDO_DEPTH) {
            for (int _si = 0; _si < UNDO_DEPTH - 1; _si++)
                for (int _sj = 0; _sj < ICON_GRID_SLOTS; _sj++)
                    undo_stack[_si][_sj] = undo_stack[_si + 1][_sj];
            undo_ptr = UNDO_DEPTH - 1;
        }
        for (int _si = 0; _si < ICON_GRID_SLOTS; _si++)
            undo_stack[undo_ptr][_si] = icon_order[_si];
        redo_ptr = -1;

        // ── Commit the rearrangement ─────────────────────────────
        if (mag_tgt_type == 0) {
            icon_order[mag_tgt_slot] = mag_src_app;
            icon_order[mag_src_slot] = -1;
        } else {
            icon_order[mag_src_slot] = mag_tgt_occupied;
            icon_order[mag_tgt_slot] = mag_src_app;
        }

        icon_layout_save();
        anim_slot  = mag_tgt_slot;
        anim_frame = ANIM_FRAMES;
        mag_active = 0;
        if (current_mode == MODE_DESKTOP)
            draw_icons();
        desktop_set_status(mag_tgt_type == 0 ? "Icon moved!" : "Icons swapped!");
        desktop_draw();
        return;
    }

    if (mag_active) {
        // ── Sub-tick flying-ghost animation ─────────────────────────
        // mag_fraction (0-255) is the interpolation clock.  The PS/2
        // IRQ advances it per packet at 200 Hz; here the heartbeat
        // advances it as an idle fallback, but ONLY when no packets
        // arrived since the last tick (mag_pkt_count unchanged) so
        // the two never double-advance.  Either way the slide
        // completes in ~200 ms.
        //
        // Rendering is single-writer: desktop_draw() first presents a
        // clean frame (wiping any previous ghost), then
        // mouse_flying_ghost_render() draws the current ghost directly
        // on top of vesa.fb — like the cursor.  One ghost per frame,
        // no accumulation, no trail.
        if (mag_pkt_count == mag_pkt_last_seen)
            mag_fraction += MAG_FRACTION_STEP_HB;
        mag_pkt_last_seen = mag_pkt_count;

        int prev_mode = current_mode;
        desktop_draw();   // click dispatch + redraw + present + cursor
        // If the click dispatch inside desktop_draw() launched an app
        // (changed current_mode), stop the flying ghost entirely and
        // ensure the swap gets committed — the ghost must never render
        // on top of the app.
        if (current_mode != prev_mode) {
            mag_active = 0;          // stop ghost rendering
            mag_commit_pending = 1;  // ensure swap is committed
            __ovly_ghost_w = 0;      // clear drag-ghost overlay descriptor
            ghost_invalidate_bg();   // invalidate stale ghost background
            return;
        }
        if (mag_fraction >= MAG_FRACTION_MAX) {
            // Animation done — clamp to the end position and render
            // one final frame so the ghost visibly arrives at the
            // slot centre, then signal the commit for the next tick.
            mag_fraction = MAG_FRACTION_MAX;
            mouse_flying_ghost_render();
            mag_commit_pending = 1;
            mag_active = 0;
            __ovly_ghost_w = 0;
            ghost_invalidate_bg();
            return;  // next heartbeat handles the commit
        }
        mouse_flying_ghost_render();
        return;
    }

    if (mouse.click_pending || mouse.rclick_pending ||
        mouse.drop_pending) {
        int mode_before = current_mode;
        desktop_draw();
        // The click dispatch runs against the frame drawn BEFORE the
        // dispatch, so a click that launched a new mode (icon
        // double-click, launcher, quick-settings grid, context menu)
        // would leave the OLD frame on screen until the next input
        // event.  Redraw immediately so the newly opened window
        // appears without waiting for the next keypress/click.
        if (current_mode != mode_before)
            desktop_redraw_no_click();
        return;
    }

    // ── VESA-mode paths: launcher, gallery, dirty cursor ─────────
    if (vesa.active) {
        // Update launcher hover state from mouse position (redraws
        // only when the hovered item changes, to avoid flickering).
        if (launcher_is_open()) {
            launcher_update_hover();
        }

        // Gallery slideshow: redraw the full slide and present.
        // Draw cursor to vesa.fb after present — slideshow transitions
        // are timer-driven (no mouse IRQ fires between them), so the
        // cursor would stay invisible until the next touchpad touch.
        if (current_mode == MODE_GALLERY && gallery_is_slideshow_active()) {
            gallery_draw();
            vesa_present();
            if (current_mode != MODE_GAME) {   // gallery is never MODE_GAME;
                cursor_save_bg(mouse.px, mouse.py, vesa.fb);   // kept for symmetry
                mouse_draw_vesa_cursor_on(vesa.fb);
            }
            mouse_draw_text_cursor();
            return;
        }

        // Dirty cursor update: save/restore/draw on vesa.fb directly
        // (desktop cursor only — game mode draws its own pointer).
        if (current_mode != MODE_GAME &&
            (mouse.px != cursor_bg_x || mouse.py != cursor_bg_y)) {
            cursor_restore_bg(vesa.fb);
            cursor_save_bg(mouse.px, mouse.py, vesa.fb);
            mouse_draw_vesa_cursor_on(vesa.fb);
            mouse_draw_text_cursor();
        }
    } else {
        // ── VGA text-mode: update hardware cursor on every movement
        // The VGA hardware cursor is an overlay — no save/restore
        // needed, just four outb() calls to CRTC registers.
        // Track the last-set position so we don't rewrite the same
        // value on every PIT tick (~18 Hz).
        if (mouse.px != cursor_bg_x || mouse.py != cursor_bg_y) {
            mouse_draw_text_cursor();
            cursor_bg_x = mouse.px;
            cursor_bg_y = mouse.py;
            cursor_bg_valid = 1;
        }
    }
}

// ─── Quick Settings panel (Windows 11 style) ─────────────────────
// Floating rounded panel on #1E1E1E with #2C2C2C elements and an
// orange (#FF5722) active accent: battery pill + circle actions up
// top, volume & brightness sliders, then a 3×2 quick-action grid.
// The desktop stays visible around the panel (like the real flyout).

#define QS_BG      0x1E1E1E
#define QS_ELEM    0x2C2C2C
#define QS_ELEM_HI 0x3A3A3A
#define QS_ACCENT  0xFF5722
#define QS_TEXT    0xFFFFFF
#define QS_SUB     0x9E9E9E
#define QS_BORDER  0x3A3A3A

// Shared layout: every consumer (draw, click, drag) uses this so the
// painted controls and the hit boxes always agree.
static void qs_layout(QsLayout* L) {
    int W = (int)vesa.width, H = (int)vesa.height;
    L->ww = 400; L->wh = 410;
    L->wx = (W - L->ww) / 2;
    L->wy = (H - L->wh) / 2 - 10;
    L->pill_w = 96; L->pill_h = 36;
    L->circ_d = 36; L->circ_r = 18;
    L->circ_y = L->wy + 16;
    L->vol_y  = L->wy + 74;
    L->bri_y  = L->wy + 126;
    L->track_x = L->wx + 46;
    L->track_w = L->ww - 62;
    L->track_y = L->vol_y + 21;   // 42px rows, 6px tall track
    L->track_h = 6;
    L->grid_x0 = L->wx + 16;
    L->grid_y0 = L->wy + 162;
    L->btn_w = (L->ww - 32 - 12) / 2;   // 178
    L->btn_h = 60;
    L->grid_gap = 12;
}

// Integer square root (no FPU in the kernel)
static int qs_isqrt(int n) {
    int x = 0;
    while ((x + 1) * (x + 1) <= n) x++;
    return x;
}

// Filled circle
static void qs_fill_circ(int cx, int cy, int r, uint32_t c) {
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx*dx + dy*dy <= r*r)
                vesa_put_pixel(cx + dx, cy + dy, c);
}

// ─── Small vector glyphs (integer math only) ─────────────────────
static void qs_glyph_wifi(int cx, int cy, uint32_t c) {
    for (int r = 5; r <= 13; r += 4) {
        int r2 = r * r;
        for (int dy = -r; dy <= 0; dy++) {
            int x = qs_isqrt(r2 - dy * dy);
            vesa_put_pixel(cx - x, cy + dy, c);
            vesa_put_pixel(cx + x, cy + dy, c);
        }
    }
    vesa_fill_rect(cx - 1, cy + 5, 3, 3, c);
}

static void qs_glyph_bt(int cx, int cy, uint32_t c) {
    icon_line(cx, cy - 9, cx, cy + 9, 2, c);
    icon_line(cx, cy - 9, cx + 5, cy - 3, 2, c);
    icon_line(cx + 5, cy - 3, cx, cy + 1, 2, c);
    icon_line(cx, cy + 1, cx - 5, cy + 5, 2, c);
    icon_line(cx - 5, cy + 5, cx, cy + 9, 2, c);
}

static void qs_glyph_power(int cx, int cy, uint32_t c) {
    int r = 7;
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++) {
            int d2 = dx*dx + dy*dy;
            if (d2 >= (r-1)*(r-1) && d2 <= r*r) {
                if (dy < -4 && dx > -2 && dx < 2) continue;  // top gap
                vesa_put_pixel(cx + dx, cy + dy, c);
            }
        }
    icon_line(cx, cy - 10, cx, cy - 3, 2, c);
}

static void qs_glyph_lock(int cx, int cy, uint32_t c) {
    icon_rrect_border(cx - 7, cy, 14, 11, c);
    icon_line(cx - 5, cy, cx - 5, cy - 6, 2, c);
    icon_line(cx + 5, cy, cx + 5, cy - 6, 2, c);
    icon_line(cx - 5, cy - 7, cx + 5, cy - 7, 2, c);
    vesa_fill_rect(cx - 1, cy + 4, 2, 4, c);
}

static void qs_glyph_cam(int cx, int cy, uint32_t c) {
    icon_rrect_border(cx - 9, cy - 6, 18, 12, c);
    icon_line(cx - 6, cy - 6, cx - 4, cy - 9, 2, c);
    icon_line(cx - 4, cy - 9, cx + 4, cy - 9, 2, c);
    icon_line(cx + 4, cy - 9, cx + 6, cy - 6, 2, c);
    icon_circ(cx, cy + 1, 3, c);
}

static void qs_glyph_speaker(int cx, int cy, uint32_t c) {
    vesa_fill_rect(cx - 8, cy - 5, 4, 10, c);
    icon_line(cx - 4, cy - 6, cx + 5, cy - 8, 2, c);
    icon_line(cx - 4, cy + 6, cx + 5, cy + 8, 2, c);
    icon_line(cx + 5, cy - 8, cx + 5, cy + 8, 2, c);
    for (int r = 8; r <= 11; r += 3) {
        int r2 = r * r;
        for (int dy = -3; dy <= 3; dy++) {
            int x = qs_isqrt(r2 - dy * dy);
            vesa_put_pixel(cx + x, cy + dy, c);
        }
    }
}

static void qs_glyph_sun(int cx, int cy, uint32_t c) {
    icon_circ(cx, cy, 4, c);
    static const int dirs[8][2] = {{1,0},{1,1},{0,1},{-1,1},{-1,0},{-1,-1},{0,-1},{1,-1}};
    for (int i = 0; i < 8; i++)
        icon_line(cx + dirs[i][0]*6, cy + dirs[i][1]*6,
                  cx + dirs[i][0]*9, cy + dirs[i][1]*9, 2, c);
}

static void qs_glyph_speed(int cx, int cy, uint32_t c) {
    icon_ring(cx, cy, 7, 2, c);
    icon_line(cx, cy, cx + 5, cy - 4, 2, c);
    icon_line(cx - 4, cy - 6, cx - 1, cy - 8, 1, c);
    icon_line(cx + 2, cy - 8, cx + 5, cy - 8, 1, c);
}

static void qs_glyph_moon(int cx, int cy, uint32_t c, uint32_t cut) {
    icon_circ(cx, cy, 6, c);
    for (int dy = -6; dy <= 6; dy++)
        for (int dx = 0; dx <= 6; dx++)
            if (dx*dx + dy*dy <= 36)
                vesa_put_pixel(cx + dx, cy + dy, cut);
}

static void qs_glyph_split(int cx, int cy, uint32_t c) {
    icon_ring(cx, cy, 6, 2, c);
    for (int dy = -6; dy <= 6; dy++)
        for (int dx = -6; dx <= 0; dx++)
            if (dx*dx + dy*dy <= 36)
                vesa_put_pixel(cx + dx, cy + dy, c);
}

static void qs_glyph_plane(int cx, int cy, uint32_t c) {
    icon_line(cx - 9, cy - 2, cx + 8, cy - 2, 2, c);
    icon_line(cx - 9, cy - 2, cx - 9, cy - 6, 2, c);
    icon_line(cx - 2, cy - 2, cx - 6, cy + 4, 2, c);
    icon_line(cx + 3, cy - 2, cx + 6, cy + 4, 2, c);
    icon_line(cx + 8, cy - 2, cx + 10, cy - 2, 2, c);
}

// Battery pill: glyph + real percentage from SMBus (QEMU PIIX4 PM)
static void qs_draw_battery_pill(int x, int y, int w, int h) {
    icon_rrect_grad_r(x, y, w, h, h / 2, QS_ELEM, QS_ELEM);
    icon_rrect_border_r(x, y, w, h, h / 2, QS_BORDER);
    int cx = x + 24, cy = y + h / 2;
    icon_rrect_border(cx - 9, cy - 5, 18, 10, 0xB0B0B0);      // body
    vesa_fill_rect(cx + 9, cy - 2, 2, 4, 0xB0B0B0);           // nub
    // Fill bar proportional to real battery percentage
    int pct = smbus_battery_percent();
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int fill_w = 14 * pct / 100;  // max 14px wide (inside the 18px body)
    if (fill_w > 0) {
        uint32_t fc = pct > 20 ? 0x8FD16A : 0xFF6644;
        vesa_fill_rect(cx - 7, cy - 3, fill_w, 6, fc);
    }
    // Percentage text
    char pct_str[5];
    pct_str[0] = '0' + (pct / 10) % 10;
    pct_str[1] = '0' + pct % 10;
    pct_str[2] = '%';
    pct_str[3] = '\0';
    vesa_draw_string(x + 42, y + (h - 16) / 2, pct_str, QS_TEXT, QS_ELEM);
}

// One slider row: icon glyph + orange filled track + white thumb
static void qs_draw_slider(int row_y, int value, int which) {
    QsLayout L; qs_layout(&L);
    int ico_cy = row_y + 21;
    if (which == 0) qs_glyph_speaker(L.wx + 26, ico_cy, 0xC8C8C8);
    else            qs_glyph_sun(L.wx + 26, ico_cy, 0xFFE08A);

    int ty = row_y + 19;
    int tx = L.track_x, tw = L.track_w, th = L.track_h;
    icon_rrect_grad_r(tx, ty, tw, th, 3, QS_ELEM_HI, QS_ELEM_HI);
    int fill = tw * value / 100;
    if (fill > 0)
        icon_rrect_grad_r(tx, ty, fill, th, 3, QS_ACCENT, QS_ACCENT);
    int thx = tx + fill;
    if (thx < tx) thx = tx;
    if (thx > tx + tw) thx = tx + tw;
    qs_fill_circ(thx, ty + th / 2, 8, QS_TEXT);
}

// 3×2 quick-action grid
static void qs_draw_grid(void) {
    QsLayout L; qs_layout(&L);
    const char* names[6] = {"Wi-Fi", "Bluetooth", "Power Mode",
                            "Night Light", "Dark Style", "Airplane Mode"};
    int wifi_ok = nic_is_up();

    for (int i = 0; i < 6; i++) {
        int col = i % 2, row = i / 2;
        int bx = L.grid_x0 + col * (L.btn_w + L.grid_gap);
        int by = L.grid_y0 + row * (L.btn_h + L.grid_gap);
        int active;
        switch (i) {
            case 0: active = wifi_ok; break;
            case 1: active = qs_bluetooth; break;
            case 2: active = 0; break;                       // Power Mode (gray per ref)
            case 3: active = qs_nightlight; break;
            case 4: active = (current_theme == 1); break;    // Dark Style = Dark theme
            case 5: active = qs_airplane; break;
            default: active = 0;
        }
        uint32_t bg = active ? QS_ACCENT : QS_ELEM;
        icon_rrect_grad_r(bx, by, L.btn_w, L.btn_h, 10, bg, bg);
        icon_rrect_border_r(bx, by, L.btn_w, L.btn_h, 10, QS_BORDER);

        // Chevron (right edge)
        vesa_draw_char(bx + L.btn_w - 18, by + (L.btn_h - 16) / 2, '>',
                       active ? 0xFFFFFF : 0x8A8A8A, bg);

        // Icon
        int gx = bx + 24, gy = by + L.btn_h / 2;
        uint32_t gc = active ? 0xFFFFFF : 0xD8D8D8;
        switch (i) {
            case 0: qs_glyph_wifi(gx, gy, gc); break;
            case 1: qs_glyph_bt(gx, gy, gc); break;
            case 2: qs_glyph_speed(gx, gy, gc); break;
            case 3: qs_glyph_moon(gx, gy, gc, bg); break;
            case 4: qs_glyph_split(gx, gy, gc); break;
            case 5: qs_glyph_plane(gx, gy, gc); break;
        }

        // Title + subtitle
        const char* sub = 0;
        char sub_buf[24];
        if (i == 0 && wifi_ok) sub = qs_wifi_names[qs_wifi_cur];
        else if (i == 2) {
            const char* pm = (qs_power_mode == 0) ? "Balanced" :
                             (qs_power_mode == 1) ? "Performance" : "Eco";
            int k = 0;
            while (pm[k] && k < 23) { sub_buf[k] = pm[k]; k++; }
            sub_buf[k] = '\0';
            sub = sub_buf;
        }
        int tx = bx + 44;
        int title_y = by + (sub ? 13 : (L.btn_h - 16) / 2);
        vesa_draw_string(tx, title_y, names[i],
                         active ? 0xFFFFFF : 0xE2E2E2, bg);
        if (sub)
            vesa_draw_string(tx, by + 33, sub,
                             active ? 0xFFE3D8 : QS_SUB, bg);
    }
}

// ─── Wi-Fi flyout (Windows-style network picker) ──────────────────
// Signal strength bars: 4 bars of increasing height, filled left→right.
static void qs_glyph_bars(int cx, int cy, int filled, uint32_t c) {
    const int hh[4] = {4, 7, 10, 13};
    for (int i = 0; i < 4; i++) {
        int x = cx - 13 + i * 5;
        int bh = hh[i];
        uint32_t col = (i < filled) ? c : 0x555555;
        vesa_fill_rect(x, cy - bh + 1, 3, bh, col);
    }
}

// Small padlock (row lock icon)
static void qs_glyph_lock_mini(int cx, int cy, uint32_t c) {
    vesa_fill_rect(cx - 3, cy - 4, 1, 4, c);   // shackle sides
    vesa_fill_rect(cx + 3, cy - 4, 1, 4, c);
    vesa_fill_rect(cx - 3, cy - 5, 7, 1, c);   // shackle top
    vesa_fill_rect(cx - 4, cy - 1, 9, 6, c);   // body
    vesa_fill_rect(cx, cy + 1, 1, 2, 0x1E1E1E); // keyhole
}

// Checkmark (connected indicator)
static void qs_glyph_check(int cx, int cy, uint32_t c) {
    vesa_fill_rect(cx - 4, cy,     1, 1, c);
    vesa_fill_rect(cx - 3, cy + 1, 1, 1, c);
    vesa_fill_rect(cx - 2, cy + 2, 1, 1, c);
    vesa_fill_rect(cx - 1, cy + 3, 1, 1, c);
    vesa_fill_rect(cx,     cy + 2, 1, 1, c);
    vesa_fill_rect(cx + 1, cy + 1, 1, 1, c);
    vesa_fill_rect(cx + 2, cy,     1, 1, c);
    vesa_fill_rect(cx + 3, cy - 1, 1, 1, c);
    vesa_fill_rect(cx + 4, cy - 2, 1, 1, c);
    vesa_fill_rect(cx + 5, cy - 3, 1, 1, c);
}

// Toggle switch (Wi-Fi on/off)
static void qs_draw_toggle(int x, int y, int on) {
    int w = 44, h = 22;
    icon_rrect_grad_r(x, y, w, h, h / 2, on ? QS_ACCENT : QS_ELEM_HI,
                      on ? QS_ACCENT : QS_ELEM_HI);
    icon_rrect_border_r(x, y, w, h, h / 2, on ? 0xD04A1C : QS_BORDER);
    int knob_x = on ? x + w - 19 : x + 3;
    qs_fill_circ(knob_x + 8, y + h / 2, 8, 0xFFFFFF);
}

// Flyout geometry: attached to the right edge of the main panel
static void qs_wifi_flyout_rect(int* fx, int* fy, int* fw, int* fh) {
    QsLayout L; qs_layout(&L);
    *fw = 330; *fh = L.wh;
    *fx = L.wx + L.ww - 12;
    *fy = L.wy;
}

// The network picker flyout
static void qs_wifi_flyout_draw(void) {
    if (!qs_wifi_open) return;
    int fx, fy, fw, fh; qs_wifi_flyout_rect(&fx, &fy, &fw, &fh);

    // Shadow + surface
    vesa_fill_rect(fx + 4, fy + 6, fw, fh, 0x120F0F);
    vesa_fill_rect(fx + 2, fy + 3, fw, fh, 0x0A0808);
    if (compositor_is_enabled())
        compositor_blur_region(fx - 8, fy - 8, fw + 16, fh + 16, 8, 70);
    icon_rrect_grad_r(fx, fy, fw, fh, 16, QS_BG, QS_BG);
    icon_rrect_border_r(fx, fy, fw, fh, 16, QS_BORDER);

    // Header: title + toggle
    vesa_draw_string(fx + 18, fy + 16, "Wi-Fi", QS_TEXT, QS_BG);
    qs_draw_toggle(fx + fw - 62, fy + 13, qs_wifi_on);
    vesa_fill_rect(fx + 14, fy + 48, fw - 28, 1, QS_BORDER);

    if (!qs_wifi_on) {
        vesa_draw_string(fx + 18, fy + 74, "Wi-Fi is off", QS_SUB, QS_BG);
        vesa_draw_string(fx + 18, fy + 94, "Turn on to see networks", 0x6A6A6A, QS_BG);
        return;
    }

    if (qs_wifi_scan > 0) {
        // "Scanning for networks" + animated dots
        int dots = ((60 - qs_wifi_scan) / 5) % 4;
        char msg[40];
        int k = 0;
        const char* m = "Scanning for networks";
        while (m[k] && k < 24) { msg[k] = m[k]; k++; }
        for (int d = 0; d < dots && k < 27; d++) msg[k++] = '.';
        msg[k] = '\0';
        vesa_draw_string(fx + 18, fy + 74, msg, 0xC8C8C8, QS_BG);
        vesa_draw_string(fx + 18, fy + 94, "Looking for available networks...", 0x6A6A6A, QS_BG);
        return;
    }

    // Network rows
    for (int i = 0; i < QS_WIFI_NETS; i++) {
        int rx = fx + 12, ry = fy + 58 + i * 50, rw = fw - 24, rh = 46;
        int hover = (mouse.px >= rx && mouse.px < rx + rw &&
                     mouse.py >= ry && mouse.py < ry + rh);
        int connected = (i == qs_wifi_cur);
        uint32_t bg = hover ? QS_ELEM_HI : QS_BG;
        icon_rrect_grad_r(rx, ry, rw, rh, 8, bg, bg);
        if (connected || hover)
            icon_rrect_border_r(rx, ry, rw, rh, 8,
                                connected ? QS_ACCENT : QS_BORDER);

        qs_glyph_bars(rx + 18, ry + 26, qs_wifi_signal[i],
                      connected ? QS_ACCENT : 0xE0E0E0);

        vesa_draw_string(rx + 44, ry + 9, qs_wifi_names[i],
                         connected ? 0xFFFFFF : (hover ? 0xFFFFFF : 0xDCDCDC), bg);
        if (qs_wifi_sec[i]) {
            int nx = rx + 44 + 8 * str_len(qs_wifi_names[i]) + 6;
            qs_glyph_lock_mini(nx, ry + 17, 0x9A9A9A);
        }
        if (connected) {
            vesa_draw_string(rx + 44, ry + 27, "Connected", QS_ACCENT, bg);
            qs_glyph_check(rx + rw - 22, ry + 23, QS_ACCENT);
        }
    }
}

// Main panel paint
static void qs_panel_draw(void) {
    QsLayout L; qs_layout(&L);
    g_win_x = L.wx; g_win_y = L.wy; g_win_w = L.ww; g_win_h = L.wh;

    // Soft drop shadow (two offset layers)
    vesa_fill_rect(L.wx + 4, L.wy + 6, L.ww, L.wh, 0x120F0F);
    vesa_fill_rect(L.wx + 2, L.wy + 3, L.ww, L.wh, 0x0A0808);
    if (compositor_is_enabled())
        compositor_blur_region(L.wx - 8, L.wy - 8, L.ww + 16, L.wh + 16, 8, 70);

    // Panel surface
    icon_rrect_grad_r(L.wx, L.wy, L.ww, L.wh, 16, QS_BG, QS_BG);
    icon_rrect_border_r(L.wx, L.wy, L.ww, L.wh, 16, QS_BORDER);

    // Top row: battery pill left, circles right
    qs_draw_battery_pill(L.wx + 16, L.circ_y, L.pill_w, L.pill_h);
    int right = L.wx + L.ww - 16 - L.circ_r;
    for (int i = 0; i < 4; i++) {          // right→left: power, lock, settings, screenshot
        int cx = right - i * (L.circ_d + 10);
        int cy = L.circ_y + L.circ_r;
        qs_fill_circ(cx, cy, L.circ_r, QS_ELEM);
        icon_ring(cx, cy, L.circ_r - 1, 1, QS_BORDER);
        switch (i) {
            case 0: qs_glyph_power(cx, cy, 0xE0E0E0); break;
            case 1: qs_glyph_lock(cx, cy, 0xE0E0E0); break;
            case 2: sym_settings(cx, cy, 0xE0E0E0); break;
            case 3: qs_glyph_cam(cx, cy, 0xE0E0E0); break;
        }
    }

    // Sliders
    qs_draw_slider(L.vol_y, sound_volume, 0);
    qs_draw_slider(L.bri_y, qs_brightness, 1);

    // Quick-action grid
    qs_draw_grid();

    // Wi-Fi network picker flyout (overlaps the right edge)
    qs_wifi_flyout_draw();
}

// Screenshot: dump the visible framebuffer as a half-res PPM file
static void qs_take_screenshot(void) {
    if (!vesa.active) { desktop_set_status("Screenshot: VESA only"); return; }
    int slot = -1;
    for (int i = 0; i < MAX_FILES; i++) if (!filesystem[i].used) { slot = i; break; }
    if (slot < 0) { desktop_set_status("Filesystem full!"); return; }

    // Unique name: Screenshot.ppm, Screenshot2.ppm, ...
    char nm[MAX_FILENAME];
    int n = 1;
    for (;;) {
        int ni = 0;
        const char* base = "Screenshot";
        for (int k = 0; base[k] && ni < MAX_FILENAME - 12; k++) nm[ni++] = base[k];
        if (n > 1) { char dn[4]; int dp = 0; int v = n;
            while (v) { dn[dp++] = '0' + v % 10; v /= 10; }
            while (dp > 0) nm[ni++] = dn[--dp]; }
        nm[ni++] = '.'; nm[ni++] = 'p'; nm[ni++] = 'p'; nm[ni++] = 'm'; nm[ni] = '\0';
        int taken = 0;
        for (int i = 0; i < MAX_FILES; i++)
            if (filesystem[i].used && str_eq(filesystem[i].name, nm)) { taken = 1; break; }
        if (!taken) break;
        n++;
    }

    int W = (int)vesa.width, H = (int)vesa.height;
    int sw = W / 2, sh = H / 2;            // half-res screenshot
    if (sw < 1) sw = 1;
    if (sh < 1) sh = 1;
    // Allocate buffer for PPM: header (~32 bytes) + sw*sh*3 pixels
    int ppm_need = 64 + sw * sh * 3;
    file_content_alloc(slot, ppm_need);
    if (!filesystem[slot].content) { desktop_set_status("OOM screenshot"); return; }
    char* out = filesystem[slot].content;
    int o = 0;
    const char* hdr = "P6\n";
    for (int k = 0; hdr[k] && o < filesystem[slot].content_cap - 3; k++) out[o++] = hdr[k];
    // <sw> <sh>\n255\n
    int nums[3] = {sw, sh, 255};
    for (int ni = 0; ni < 3; ni++) {
        char nb[8]; int np = 0; int v = nums[ni];
        if (!v) nb[np++] = '0';
        while (v) { nb[np++] = '0' + v % 10; v /= 10; }
        for (int k = np - 1; k >= 0; k--) out[o++] = nb[k];
        out[o++] = (ni < 2) ? ' ' : '\n';
    }

    int pitch = (int)vesa.pitch;
    int bpp = (int)(vesa.bpp / 8);
    uint8_t* fb = vesa.fb;                 // visible (presented) buffer
    for (int y = 0; y < sh && o < filesystem[slot].content_cap - 3 * sw; y++) {
        uint8_t* row = fb + (uint32_t)(y * 2) * (uint32_t)pitch;
        for (int x = 0; x < sw; x++) {
            uint8_t* p = row + (uint32_t)(x * 2) * (uint32_t)bpp;
            out[o++] = p[2];               // R
            out[o++] = p[1];               // G
            out[o++] = p[0];               // B
        }
    }

    str_copy(filesystem[slot].name, nm, MAX_FILENAME);
    filesystem[slot].size = o;
    filesystem[slot].used = 1;
    fs_save_to_disk();
    desktop_set_status("Screenshot saved");
}

// Drag the volume/brightness sliders while the left button is held
static void qs_drag_handle(void) {
    if (!vesa.active || current_mode != MODE_QUICKSETTINGS) return;
    if (!mouse.left) return;
    QsLayout L; qs_layout(&L);
    int changed = 0;
    if (mouse.py >= L.vol_y && mouse.py < L.vol_y + 42 &&
        mouse.px >= L.track_x && mouse.px <= L.track_x + L.track_w) {
        int v = (mouse.px - L.track_x) * 100 / L.track_w;
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        if (v != sound_volume) { sound_volume = v; changed = 1; }
    } else if (mouse.py >= L.bri_y && mouse.py < L.bri_y + 42 &&
               mouse.px >= L.track_x && mouse.px <= L.track_x + L.track_w) {
        int v = (mouse.px - L.track_x) * 100 / L.track_w;
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        if (v != qs_brightness) { qs_brightness = v; wallpaper_set_brightness(v); changed = 1; }
    }
    if (changed) desktop_redraw_no_click();
}

// Entry point: VGA fallback or the new panel
static void draw_quicksettings_window(void) {
    if (!vesa.active) {
        vga_fill_rect(2, 2, 76, 20, ' ', 15, 1);
        vga_fill_row(2, ' ', 15, 1);
        vga_puts(3, 2, "[ Quick Settings ]", 15, 1);
        vga_puts(5, 5,  "Volume: ", 15, 1);
        char vv[4]; vv[0]='0'+sound_volume/100; vv[1]='0'+(sound_volume/10)%10; vv[2]='0'+sound_volume%10; vv[3]='\0';
        vga_puts(13, 5, vv, 13, 1);
        vga_puts(5, 7,  "ESC:back", 7, 1);
        return;
    }
    qs_panel_draw();
}

// ─── Alt+Tab task switcher ───────────────────────────────────────
// A real Alt+Tab: hold Alt, tap Tab to cycle through the apps you
// have used recently (most-recent-first), release Alt to switch to
// the highlighted one.  The "tasks" here are the app modes — each
// app is a mode with persistent static state, so switching tasks is
// just changing current_mode (exactly what Alt+Tab does on a real
// desktop).
//
// MRU list: every time current_mode changes (recorded in
// finish_frame(), the single choke point both draw paths go
// through), the mode is moved to the front of alt_tab_mru[].  The
// switcher overlay is drawn at the top of finish_frame() right
// before vesa_present(), so it layers above whichever window is
// currently drawn.
#define ALT_TAB_MAX 10
static int  alt_tab_mru[ALT_TAB_MAX];   // most-recently-used modes
static int  alt_tab_count  = 0;         // valid entries in alt_tab_mru
static int  alt_tab_last_recorded = -1; // last mode seen by the record hook
static int  alt_tab_active  = 0;        // switcher overlay is open
static int  alt_tab_sel     = 0;        // highlighted MRU index

// ─── Alt+Tab window mini-previews ──────────────────────────────
// Each switcher row shows a scaled snapshot of that app's window,
// box-downsampled from the back buffer (vesa.draw) at the moment the
// app was last on screen — so switching feels like a real OS.  The
// thumbnails keep the screen's 16:9 aspect (160×90), so previews are
// never vertically stretched.  Thumbnails are keyed by MODE (small
// ints 0..22) rather than by MRU slot, so no lockstep reshuffle
// bookkeeping is needed when the MRU list reorders: the overlay
// simply looks up the thumbnail of whatever mode a row currently
// holds.  Refreshed in finish_frame() on every mode change and when
// the switcher opens.
#define ALT_TAB_THUMB_W 160
#define ALT_TAB_THUMB_H 90
#define ALT_TAB_MODE_MAX (MODE_GAME + 1)    // 25 modes (0..24)
static uint32_t alt_tab_thumbs[ALT_TAB_MODE_MAX][ALT_TAB_THUMB_W * ALT_TAB_THUMB_H];
static uint8_t  alt_tab_thumbs_valid[ALT_TAB_MODE_MAX]; // 1 once a snapshot exists
static int      alt_tab_refresh_cur = 0;   // recapture the current app on switcher open

// Mode → display name for the switcher rows.
static const char* alt_tab_mode_name(int mode) {
    switch (mode) {
        case MODE_DESKTOP:       return "Desktop";
        case MODE_TERMINAL:      return "Terminal";
        case MODE_FILES:         return "Files";
        case MODE_TRASH:         return "Trash";
        case MODE_SETTINGS:      return "Settings";
        case MODE_MUSIC:         return "Music";
        case MODE_CALC:          return "Calculator";
        case MODE_GAMES:         return "Games";
        case MODE_EDITOR:        return "Editor";
        case MODE_PAINT:         return "Paint";
        case MODE_SYSMON:        return "System Monitor";
        case MODE_CLOCK:         return "Clock";
        case MODE_CALENDAR:      return "Calendar";
        case MODE_WEATHER:       return "Weather";
        case MODE_APPLIB:        return "App Library";
        case MODE_ORGANIZER:     return "Organizer";
        case MODE_SECURITY:      return "Security";
        case MODE_IMGVIEW:       return "Image Viewer";
        case MODE_GALLERY:       return "Gallery";
        case MODE_PDFVIEW:       return "PDF Viewer";
        case MODE_BROWSER:       return "Browser";
        case MODE_QUICKSETTINGS: return "Quick Settings";
        case MODE_VIDEO:         return "Video";
        case MODE_GAME: {
            // Label follows the running game's own title (DOOM, Chess).
            const char* gt = game_current_title();
            return (gt && gt[0]) ? gt : "Game";
        }
        case MODE_STORE:         return "App Store";
        default:                 return "App";
    }
}

// Mode → accent color for the switcher row tiles.
static uint32_t alt_tab_mode_color(int mode) {
    switch (mode) {
        case MODE_TERMINAL:      return 0x2ECC71;
        case MODE_FILES:         return 0xF2B73D;
        case MODE_TRASH:         return 0xE74C3C;
        case MODE_SETTINGS:      return 0x4A90D9;
        case MODE_MUSIC:         return 0xE84393;
        case MODE_CALC:          return 0x00CEC9;
        case MODE_GAMES:         return 0xF39C12;
        case MODE_EDITOR:        return 0x9B59B6;
        case MODE_PAINT:         return 0xFF7043;
        case MODE_SYSMON:        return 0x1ABC9C;
        case MODE_CLOCK:         return 0x3498DB;
        case MODE_CALENDAR:      return 0xE67E22;
        case MODE_WEATHER:       return 0x5DADE2;
        case MODE_APPLIB:        return 0xBB8FCE;
        case MODE_ORGANIZER:     return 0xF5B041;
        case MODE_SECURITY:      return 0x27AE60;
        case MODE_IMGVIEW:       return 0x48C9B0;
        case MODE_GALLERY:       return 0xEC7063;
        case MODE_PDFVIEW:       return 0xAF7AC5;
        case MODE_BROWSER:       return 0x2E86C1;
        case MODE_QUICKSETTINGS: return 0x85C1E9;
        case MODE_VIDEO:         return 0xC0392B;
        case MODE_STORE:         return 0x8A7BFF;
        case MODE_GAME:          return 0xB03030;
        case MODE_DESKTOP:
        default:                 return 0x8A6FBF;
    }
}

// Move `mode` to the front of the MRU list (dedup, cap at ALT_TAB_MAX).
static void alt_tab_record(int mode) {
    int i;
    for (i = 0; i < alt_tab_count; i++)
        if (alt_tab_mru[i] == mode) break;
    if (i == alt_tab_count) {             // new entry
        if (alt_tab_count < ALT_TAB_MAX) alt_tab_count++;
        else {                            // full: drop the oldest, shift left
            for (int j = 0; j < ALT_TAB_MAX - 1; j++)
                alt_tab_mru[j] = alt_tab_mru[j + 1];
            i = ALT_TAB_MAX - 1;
        }
    }
    for (int j = i; j > 0; j--)           // bubble to front
        alt_tab_mru[j] = alt_tab_mru[j - 1];
    alt_tab_mru[0] = mode;
}

// Remove `mode` from the switcher MRU (an app window was closed) so
// Alt+Tab and the taskbar tray never list a closed window.
static void alt_tab_forget(int mode) {
    for (int i = 0; i < alt_tab_count; i++) {
        if (alt_tab_mru[i] != mode) continue;
        for (int j = i; j < alt_tab_count - 1; j++)
            alt_tab_mru[j] = alt_tab_mru[j + 1];
        alt_tab_count--;
        if (alt_tab_sel >= alt_tab_count) alt_tab_sel = 0;
        return;
    }
}

// First Tab press: open the switcher and highlight the previous app.
// Further taps: advance the highlight.  Called from keyboard.c.
void desktop_alt_tab(void) {
    // The current mode must be at the front of the MRU so the
    // switcher always lists it first (highlighted on open).
    if (alt_tab_count == 0 || alt_tab_mru[0] != current_mode)
        alt_tab_record(current_mode);
    if (alt_tab_count < 2) return;        // nothing else to switch to
    if (!alt_tab_active) {
        alt_tab_active = 1;
        alt_tab_sel = 1;                  // first Tab → the previous app
        // Refresh the current app's mini-preview on this redraw
        // (finish_frame() consumes the flag before drawing the panel).
        alt_tab_refresh_cur = 1;
    } else {
        alt_tab_sel = (alt_tab_sel + 1) % alt_tab_count;
    }
    desktop_redraw_no_click();
}

// Alt release: switch to the highlighted app (no-op if the switcher
// isn't open, or if the user cycled back to the current app).
void desktop_alt_tab_commit(void) {
    if (!alt_tab_active) return;
    alt_tab_active = 0;
    int target = alt_tab_mru[alt_tab_sel];
    if (target != current_mode) {
        // Leaving the video player stops its sound (mirrors the ESC
        // path); other apps keep their persistent state.
        if (current_mode == MODE_VIDEO) video_close();
        current_mode = target;
    }
    desktop_redraw_no_click();
}

// Abort the switcher without switching (not currently bound).
void desktop_alt_tab_cancel(void) {
    if (!alt_tab_active) return;
    alt_tab_active = 0;
    desktop_redraw_no_click();
}

// 1 while the switcher overlay is open — keyboard.c swallows all
// other keys then, so the app underneath can't react.
int desktop_alt_tab_active(void) { return alt_tab_active; }

// Draw the centered switcher overlay into the back buffer.  Called
// from finish_frame() right before vesa_present(), so it layers
// above the current window on both the desktop_draw and
// redraw_content paths.
// Exact panel-gradient color at local row yy — the text bgs use this
// instead of hardcoded approximations so vesa_draw_char's bg fill
// matches the hline gradient underneath (no faint block behind text).
static inline uint32_t alt_tab_panel_bg(int ph, int yy) {
    int prog = yy * 255 / ph;
    int rr = 0x22 + (0x14 - 0x22) * prog / 255;
    int gg = 0x1A + (0x0D - 0x1A) * prog / 255;
    int bb = 0x44 + (0x2C - 0x44) * prog / 255;
    return (rr << 16) | (gg << 8) | bb;
}

// Box-downsample the whole back buffer (vesa.draw holds the freshly
// drawn current window at finish_frame time) into the thumbnail slot
// for `mode`.  Each thumbnail pixel averages its source block, giving
// a clean mini-preview without moiré.  Called from finish_frame()
// BEFORE alt_tab_draw_overlay() so the switcher panel itself never
// appears inside a thumbnail.
static void alt_tab_capture_thumb(int mode) {
    // Capture reads 4 bytes/pixel (BGRA).  The whole draw stack assumes
    // 32bpp (back buffer, overlay plane), but guard explicitly so a
    // hypothetical 24bpp mode can't silently produce garbage thumbnails.
    if (!vesa.active || vesa.bpp != 32 || mode < 0 || mode >= ALT_TAB_MODE_MAX) return;
    int W = (int)vesa.width, H = (int)vesa.height;
    if (W < 2 || H < 2) return;
    const uint8_t* src = vesa.draw;
    int pitch = (int)vesa.pitch;
    uint32_t* dst = alt_tab_thumbs[mode];
    for (int ty = 0; ty < ALT_TAB_THUMB_H; ty++) {
        int y0 = ty * H / ALT_TAB_THUMB_H;
        int y1 = (ty + 1) * H / ALT_TAB_THUMB_H;
        if (y1 <= y0) y1 = y0 + 1;
        for (int tx = 0; tx < ALT_TAB_THUMB_W; tx++) {
            int x0 = tx * W / ALT_TAB_THUMB_W;
            int x1 = (tx + 1) * W / ALT_TAB_THUMB_W;
            if (x1 <= x0) x1 = x0 + 1;
            uint32_t r = 0, g = 0, b = 0;
            int n = 0;
            for (int sy = y0; sy < y1; sy++)
                for (int sx = x0; sx < x1; sx++) {
                    const uint8_t* p = src + (uint32_t)sy * pitch + (uint32_t)sx * 4;
                    b += p[0]; g += p[1]; r += p[2];   // back buffer is BGRA
                    n++;
                }
            if (!n) n = 1;
            dst[ty * ALT_TAB_THUMB_W + tx] = ((r / n) << 16) | ((g / n) << 8) | (b / n);
        }
    }
    alt_tab_thumbs_valid[mode] = 1;
}

// Blit a mode's stored thumbnail to the back buffer at (dx, dy),
// framed with a thin border.  No-op when the mode has no snapshot yet
// (e.g. never visited this session).  Uses a direct BGRA write loop
// (the thumbnails are 0x00RRGGBB; the back buffer is BGRA) instead of
// per-pixel vesa_put_pixel — at 160×90 that would be 14,400 calls per
// row, far too slow for the ~10 rows the panel can show.
static void alt_tab_blit_thumb(int mode, int dx, int dy) {
    if (mode < 0 || mode >= ALT_TAB_MODE_MAX || !alt_tab_thumbs_valid[mode]) return;
    if (!vesa.active || vesa.bpp != 32) return;
    int W = (int)vesa.width, H = (int)vesa.height;
    int pitch = (int)vesa.pitch;
    const uint32_t* src = alt_tab_thumbs[mode];
    // Left-clip offset: when dx < 0 the row starts at pixel 0 (and the
    // first tx source columns are skipped); otherwise the row starts at
    // dx and every tx maps 1:1.  sx0 + xoff are precomputed once so the
    // inner loop has no branch or repeated ternary per pixel.
    int xoff = (dx < 0) ? 0 : dx;
    int sx0  = (dx < 0) ? -dx : 0;
    int maxx = (dx + ALT_TAB_THUMB_W <= W) ? ALT_TAB_THUMB_W : W - dx;
    for (int ty = 0; ty < ALT_TAB_THUMB_H; ty++) {
        int y = dy + ty;
        if (y < 0 || y >= H) continue;
        uint8_t* row = vesa.draw + (uint32_t)y * pitch + (uint32_t)xoff * 4;
        for (int tx = sx0; tx < maxx; tx++) {
            uint32_t c = src[ty * ALT_TAB_THUMB_W + tx];
            int px_idx = (dx + tx) - xoff;
            row[(uint32_t)px_idx * 4 + 0] = (uint8_t)(c & 0xFF);
            row[(uint32_t)px_idx * 4 + 1] = (uint8_t)((c >> 8) & 0xFF);
            row[(uint32_t)px_idx * 4 + 2] = (uint8_t)((c >> 16) & 0xFF);
            row[(uint32_t)px_idx * 4 + 3] = 0xFF;
        }
    }
    vesa_draw_rect(dx - 1, dy - 1, ALT_TAB_THUMB_W + 2, ALT_TAB_THUMB_H + 2, 0x3A2E70);
}

static void alt_tab_draw_overlay(void) {
    if (!alt_tab_active || !vesa.active) return;
    int W = (int)vesa.width, H = (int)vesa.height;
    int n = alt_tab_count;
    if (n < 1) return;

    int row_h = 100;    // tall enough for the 90px-high 16:9 preview
    int pad   = 12;
    int title_h = 26;
    int hint_h  = 24;   // reserved strip under the rows for the hint line
    // Panel width: left accent tile + app name + right thumbnail.
    int pw = 360;
    // Scroll window: show as many rows as fit vertically, keeping the
    // highlighted entry visible.  All 10 MRU entries fit on tall
    // screens; short ones (e.g. 800×600) get a sliding window instead
    // of a panel that overflows the display.
    int max_vis = (H - pad - title_h - pad - hint_h) / row_h;
    if (max_vis < 1) max_vis = 1;
    int vis = (n < max_vis) ? n : max_vis;
    int top = alt_tab_sel - vis + 1;    // keep selection on-screen
    if (top < 0) top = 0;
    if (top + vis > n) top = n - vis;
    if (top < 0) top = 0;
    int ph = pad + title_h + vis * row_h + pad + hint_h;
    int px = (W - pw) / 2;
    int py = (H - ph) / 2 - 20;
    // Keep the panel fully on-screen (small resolutions).
    if (px < 4) px = 4;
    if (py < 4) py = 4;
    if (px + pw > W - 4) px = W - pw - 4;
    if (py + ph > H - 4) py = H - ph - 4;

    // ── Translucent dark panel ─────────────────────────────────
    for (int yy = 0; yy < ph; yy++)
        vesa_hline(px, py + yy, pw, alt_tab_panel_bg(ph, yy));
    vesa_draw_rect(px, py, pw, ph, 0xC8A8FF);

    // ── Title bar ─────────────────────────────────────────────
    // bg must match the surface under the glyphs — vesa_draw_char
    // fills every non-glyph pixel with bg (0 would paint black
    // blocks).  Computed from the same gradient as the panel fill
    // so the text block blends in seamlessly.
    vesa_draw_string(px + 12, py + 6, "Alt+Tab — switch app", 0xE8D8FF,
                     alt_tab_panel_bg(ph, 6));
    vesa_hline(px + 8, py + title_h - 4, pw - 16, 0x3A2E70);

    // ── Rows (current app first, MRU order; scrolled window) ──
    for (int v = 0; v < vis; v++) {
        int i = top + v;
        int ry = py + pad + title_h + v * row_h;
        int sel = (i == alt_tab_sel);
        const char* nm = alt_tab_mode_name(alt_tab_mru[i]);
        uint32_t ac = alt_tab_mode_color(alt_tab_mru[i]);
        uint32_t row_bg = sel ? 0x3A2E70 : 0x1E1438;

        // Row background
        if (sel) {
            vesa_fill_rect(px + 6, ry, pw - 12, row_h - 4, row_bg);
            vesa_draw_rect(px + 6, ry, pw - 12, row_h - 4, 0xD8C0FF);
        } else {
            vesa_fill_rect(px + 6, ry, pw - 12, row_h - 4, row_bg);
        }
        // Accent tile (vertically centered)
        vesa_fill_rect(px + 14, ry + (row_h - 22) / 2, 22, 22, ac);
        vesa_draw_rect(px + 14, ry + (row_h - 22) / 2, 22, 22, 0x0E0820);
        // App name (bright when selected; bg = row surface)
        vesa_draw_string(px + 46, ry + (row_h - 16) / 2, nm,
                         sel ? 0xFFFFFF : 0xB8A8D8, row_bg);
        // Window preview (right-aligned in the row, vertically centered)
        int tx = px + pw - 12 - ALT_TAB_THUMB_W;
        int ty = ry + (row_h - ALT_TAB_THUMB_H) / 2;
        int m = alt_tab_mru[i];
        int thumb_ok = (m >= 0 && m < ALT_TAB_MODE_MAX && alt_tab_thumbs_valid[m]);
        alt_tab_blit_thumb(m, tx, ty);
        // Highlight the preview only when there is actually one — a
        // selected-but-unvisited row shouldn't show a bare empty box.
        if (sel && thumb_ok)
            vesa_draw_rect(tx - 1, ty - 1, ALT_TAB_THUMB_W + 2,
                           ALT_TAB_THUMB_H + 2, 0xD8C0FF);
    }

    // ── Hint line ─────────────────────────────────────────────
    vesa_draw_string(px + 12, py + ph - 20, "release Alt to switch", 0x605080,
                     alt_tab_panel_bg(ph, ph - 20));
}

// ─── Window-content redraw (no click dispatch) ───────────────────
// Draws wallpaper, taskbar, statusbar, and the current mode's window
// to the back buffer.  Use instead of recursive desktop_draw() calls
// inside click handlers — avoids the double-present + cursor race.
// Does NOT include the click dispatch (let the outer call handle it).
static void redraw_content(void) {
    // NOTE: The volatile __cursor_bg_dirty guard flag was removed.
    // The IRQ handler reads cursor background from vesa.fb (never
    // touched by this function), so partial back-buffer writes are
    // safe.  If cursor_save_bg is ever changed back to read from
    // vesa.draw, re-add: __cursor_bg_dirty = 1;  here.
    if (current_mode == MODE_DESKTOP)
        draw_wallpaper();
    else
        wallpaper_draw_fast();
    // The taskbar is always visible (Windows-style), in every app
    // mode — its tray shows the real open windows from the WM.
    popup_anim_tick();
    draw_taskbar();
    draw_statusbar();
    switch (current_mode) {
        case MODE_DESKTOP:
            draw_icons();
            if (search_active) draw_search_bar();
            break;
        case MODE_TERMINAL: draw_terminal_window(); break;
        case MODE_FILES:
            if (viewing_file >= 0) draw_file_viewer(viewing_file);
            else draw_files_window();
            break;
        case MODE_TRASH:    draw_trash_window();    break;
        case MODE_SETTINGS: draw_settings_window(); break;
        case MODE_MUSIC:    draw_music_window();    break;
        case MODE_CALC:     calc_draw();            break;
        case MODE_GAMES:    games_draw();           break;
        case MODE_EDITOR:   editor_draw();          break;
        case MODE_PAINT:    paint_draw();           break;
        case MODE_SYSMON:   sysmon_draw();          break;
        case MODE_CLOCK:    clock_draw();           break;
        case MODE_CALENDAR: calendar_draw();        break;
        case MODE_WEATHER:  weather_draw_full();    break;
        case MODE_APPLIB:   applib_draw();          break;
        case MODE_ORGANIZER: organizer_draw();      break;
        case MODE_SECURITY:  security_draw();       break;
        case MODE_IMGVIEW:   imgview_draw();        break;
        case MODE_GALLERY:   gallery_draw();        break;
        case MODE_PDFVIEW:   pdfview_draw();        break;
        case MODE_BROWSER:   browser_draw();        break;
        case MODE_VIDEO:     video_draw();          break;
        case MODE_STORE:     store_draw();          break;
        case MODE_GAME:      game_draw();           break;   // full-screen game canvas
        case MODE_QUICKSETTINGS: draw_quicksettings_window(); break;
    }
    // Popups drawn AFTER all windows/icons so they always appear on top
    draw_vol_popup();
    draw_wifi_popup();
    // Visual feedback: screen flash overlay
    if (flash_frames > 0) {
        int W = (int)vesa.width, H = (int)vesa.height;
        int strength = flash_frames > 4 ? 4 : flash_frames;
        uint32_t fc = (uint32_t)(flash_r * strength / 5) << 16
                    | (uint32_t)(flash_g * strength / 5) << 8
                    | (uint32_t)(flash_b * strength / 5);
        vesa_fill_rect(0, 0, W, H, fc);
        flash_frames--;
    }
}

// ─── Frame finish: present + cursor ──────────────────────────────
// Copies the back buffer to the visible buffer and draws the cursor
// on top. Called after redraw_content() in place of desktop_draw()
// when we don't need the click dispatch (e.g. keyboard handlers).
static void finish_frame(void) {
    if (!vesa.active) return;

    // ── Alt+Tab MRU record hook + thumbnail snapshot ──────────
    // Every mode transition passes through here (both draw paths
    // end in finish_frame), so this is the single place that keeps
    // the switcher's most-recently-used app list in sync with
    // whatever the user has been looking at.
    if (current_mode != alt_tab_last_recorded) {
        alt_tab_last_recorded = current_mode;
        alt_tab_record(current_mode);
        // Keep the native window manager's focus/MRU in sync with
        // the switcher's list (same choke point).
        win_focus(current_mode);
        // The freshly drawn window is in the back buffer — snapshot
        // it for the switcher's mini-preview before anything else
        // paints over it.
        alt_tab_capture_thumb(current_mode);
    } else if (alt_tab_refresh_cur) {
        // Switcher first opened on the same mode: refresh the current
        // app's preview (it may have redrawn since its last capture).
        // else-if keeps a coincident mode-change + open from
        // double-capturing in one frame.
        alt_tab_refresh_cur = 0;
        alt_tab_capture_thumb(current_mode);
    }
    // Keep the WM's copy of the focused window's rect current so
    // [X] hit-testing works after the app moved/resized it.
    if (g_win_x >= 0 && g_win_w > 0)
        win_sync_rect(current_mode, g_win_x, g_win_y, g_win_w, g_win_h);
    // Draw the switcher overlay on top of the freshly drawn window
    // (layers above everything, right before the present copy).
    alt_tab_draw_overlay();

    // NOTE: The volatile __cursor_bg_dirty guard flag was removed.
    // The IRQ reads cursor background from vesa.fb (just written by
    // vesa_present above), so no guard is needed for this path.
    // If re-adding, clear the flag before vesa_present:
    //   __cursor_bg_dirty = 0;
    if (current_mode == MODE_GAME && !alt_tab_active) {
        // Game mode: the frame content is ONLY the centered game canvas
        // (game_draw painted the 640×400 canvas + title bar).  Present
        // just that rect (~1.6 MB) instead of the full 8.3 MB screen —
        // the whole-screen copy 35×/s evicted the CPU cache inside IRQ0
        // and was the main source of in-game lag (and delayed PS/2
        // delivery).  Nothing else is on screen: the desktop arrow is
        // suppressed in MODE_GAME and the alt-tab overlay is excluded
        // above (falling back to the full present while switching).
        int gw = game_canvas_w();
        int gh = game_canvas_h();
        if (gw <= 0) gw = GAME_W;          // game just exited: defaults
        if (gh <= 0) gh = GAME_H;
        int gx = (int)vesa.width / 2 - gw / 2;
        int gy = (int)vesa.height / 2 - gh / 2;
        int tb = game_draws_titlebar();
        vesa_present_rect(gx, gy - tb, gw, gh + tb);
    } else {
        vesa_present();
    }
    // The present copied the clean back buffer over vesa.fb and bumped
    // the present-generation counters (see vesa_present), so any box
    // the IRQ drew before is stale and can't be XOR-erased onto clean
    // desktop pixels.  If a select-drag is STILL live, redraw the box
    // fresh on the pristine frame so it doesn't vanish for a frame.
    // (No-op when no drag is active.)
    mouse_sel_box_redraw_after_present();
    // In game mode the ring-3 game draws its own pointer (game.c:
    // "the game is the pointer"); stamping the desktop arrow here
    // produced TWO cursors over the canvas.
    if (current_mode != MODE_GAME) {
        cursor_save_bg(mouse.px, mouse.py, vesa.fb);
        mouse_draw_vesa_cursor_on(vesa.fb);
    }
}

// ─── Full redraw (minus click dispatch) ───────────────────────────
// Redraws content and presents the frame, skipping the expensive
// click-dispatch section.  Use in keyboard handlers instead of
// desktop_draw() to avoid the unnecessary click-polling overhead.
// Since the keyboard ISR doesn't trigger clicks, the click dispatch
// in desktop_draw() would always be a no-op anyway.
static void desktop_redraw_no_click(void) {
    redraw_content();
    finish_frame();
    // Prevent stale IRQ-level overlay restore on next mouse packet.
}

// ─── Progressive icon-picture loading (main-loop context) ────────
// Custom app pictures (SVG/PNG/BMP...) are decoded lazily, ONE per
// main-loop iteration, so the desktop appears instantly after login
// (vector glyphs) and the real pictures pop in over the next second
// or two.  Called from kernel.c's session loop (interrupts enabled)
// — deliberately NOT from desktop_heartbeat(), which runs inside the
// PIT timer IRQ where a slow SVG decode would block the mouse and
// keyboard IRQs.  Decoding continues in the background even while an
// app window is open; we only redraw when a picture lands on the
// desktop.
// Decode up to this many pictures per main-loop call so the grid
// populates in a fraction of the time (decode is much cheaper now
// that the SVG backgrounds were stripped).
#define ICON_PUMP_BATCH 4

void desktop_icon_pump(void) {
    if (!vesa.active) return;
    if (!icon_img_scan_pending && icon_img_scan_count != icon_img_fp())
        icon_img_scan_begin();
    int decoded = 0;
    while (icon_img_scan_pending && decoded < ICON_PUMP_BATCH &&
           icon_img_scan_step())
        decoded++;
    if (decoded && current_mode == MODE_DESKTOP)
        desktop_draw();
}

// ─── Build-time thumbnail cache ─────────────────────────────────
// host-files/icons.cache is generated at build time by
// tools/icon_cache_gen.c (host-side, using the REAL kernel
// decoders), then embedded like any other host file.  Loading it is
// a plain memcpy — no SVG/PNG decoding at boot.  Returns 1 on
// success, 0 if the cache is missing / corrupt / stale (caller
// falls back to decoding on boot).
//
// Layout (little-endian):
//   [0..3]  magic "NYIC"
//   [4]     version (1)
//   [5]     app count (19)
//   [6..9]  u32 picture fingerprint (sum of app-matching picture
//           sizes) — mirrors tools/icon_cache_gen.c; rejects stale
//           caches when icons were changed but the cache wasn't
//           rebuilt.
//   then per app in app_names[] order:
//     [0]   ok flag, [1..4] cw/ch/cox/coy, [5..] RGBA thumb
#define ICON_CACHE_MAGIC     0x4349594E  // "NYIC" (LE u32 of bytes N-Y-I-C)
#define ICON_CACHE_VERSION   1
#define ICON_CACHE_HEADER    10
#define ICON_CACHE_PER_APP   (5 + ICON_IMG_SZ)

// Sum of byte sizes of every app-matching picture file — mirrors
// tools/icon_cache_gen.c pic_fingerprint().  Excludes non-picture
// files and the cache blob itself (its basename "icons.cache"
// matches no app name), so the value is stable whether or not the
// cache is embedded.
static int icon_img_pic_fp(void) {
    int fp = 0;
    for (int a = 0; a < DESKTOP_APP_COUNT; a++) {
        for (int i = 0; i < MAX_FILES; i++) {
            if (!filesystem[i].used) continue;
            if (icon_img_type(filesystem[i].name) == ICON_IMG_NONE) continue;
            if (icon_img_matches(filesystem[i].name, app_names[a])) {
                fp += filesystem[i].size;
                break;
            }
        }
    }
    return fp;
}

static int icon_img_cache_load(void) {
    // Locate the embedded cache blob.
    int fi = -1;
    for (int i = 0; i < MAX_FILES; i++) {
        if (!filesystem[i].used) continue;
        if (str_eq(filesystem[i].name, "icons.cache")) { fi = i; break; }
    }
    if (fi < 0) return 0;
    const uint8_t* c = (const uint8_t*)filesystem[fi].content;
    if (!c) return 0;
    if (filesystem[fi].size <
        ICON_CACHE_HEADER + DESKTOP_APP_COUNT * ICON_CACHE_PER_APP)
        return 0;
    uint32_t magic = (uint32_t)c[0] | ((uint32_t)c[1] << 8) |
                     ((uint32_t)c[2] << 16) | ((uint32_t)c[3] << 24);
    if (magic != ICON_CACHE_MAGIC) return 0;
    if (c[4] != ICON_CACHE_VERSION || c[5] != DESKTOP_APP_COUNT) return 0;
    uint32_t fp = (uint32_t)c[6] | ((uint32_t)c[7] << 8) |
                  ((uint32_t)c[8] << 16) | ((uint32_t)c[9] << 24);
    if ((uint32_t)icon_img_pic_fp() != fp) return 0;   // stale cache
    for (int a = 0; a < DESKTOP_APP_COUNT; a++) {
        const uint8_t* rec = c + ICON_CACHE_HEADER + a * ICON_CACHE_PER_APP;
        icon_img_ok[a] = rec[0] ? 1 : 0;
        if (rec[0]) {
            int cw = rec[1], ch = rec[2], ox = rec[3], oy = rec[4];
            // The content rect must lie INSIDE the 56x56 thumb.  The
            // fingerprint is a size-sum, not a checksum, so validate
            // the geometry too — a corrupt-but-header-valid record
            // with ox+cw > 56 would make icon_img_blit read past the
            // thumb buffer.  Reject the whole cache on any bad
            // record; the caller falls back to decoding on boot.
            // (Zeroed records — an app with no picture file, i.e.
            // glyph fallback — skip validation entirely.)
            if (cw < 1 || cw > ICON_IMG_W || ch < 1 || ch > ICON_IMG_H ||
                ox < 0 || oy < 0 || ox + cw > ICON_IMG_W ||
                oy + ch > ICON_IMG_H)
                return 0;
            icon_img_cw[a] = cw;
            icon_img_ch[a] = ch;
            icon_img_cox[a] = ox;
            icon_img_coy[a] = oy;
        } else {
            // Glyph fallback — mirror icon_img_scan_begin()'s reset.
            icon_img_cw[a] = icon_img_ch[a] = 0;
            icon_img_cox[a] = icon_img_coy[a] = 0;
        }
        for (int i = 0; i < ICON_IMG_SZ; i++)
            icon_img_thumb[a][i] = rec[5 + i];
    }
    return 1;
}

// ─── Boot-time icon warm-up ─────────────────────────────────────
// Decode ALL app pictures in one synchronous pass, before the login
// screen ever appears.  Called from kernel_main() right after
// fs_load_embedded() and BEFORE the splash, so the boot animation
// covers the whole init pass — no dark pause between splash and
// login.  Decoding only writes to the static icon_img_scratch /
// icon_img_thumb buffers — it never touches the framebuffer.  By the
// time the user finishes typing their password, every desktop tile
// already has its real SVG/PNG picture cached, so the desktop
// renders complete on the very first frame — no progressive pop-in,
// no placeholder glass tiles.
//
// The scan fingerprint (icon_img_fp) still matches what the pump
// expects, so desktop_icon_pump() later sees a completed scan and
// becomes a no-op — no re-decode, no double work.
void desktop_icon_warmup(void) {
    if (!vesa.active) return;
    // Fast path: the build-time thumbnail cache (host-files/icons.cache
    // embedded at compile time) already holds every app's 56x56 RGBA
    // thumb, pre-decoded by tools/icon_cache_gen.c — zero decoding at
    // boot.  Stamping the scan fingerprint here makes the later
    // desktop_icon_pump() a no-op.
    if (icon_img_cache_load()) {
        icon_img_scan_count = icon_img_fp();
        icon_img_scan_pending = 0;
        return;
    }
    // Fallback: no/stale cache — decode every app picture on boot.
    if (!icon_img_scan_pending && icon_img_scan_count != icon_img_fp())
        icon_img_scan_begin();
    while (icon_img_scan_pending && icon_img_scan_step()) {
        // decode every remaining app picture synchronously
    }
}

// ─── Full redraw ──────────────────────────────────────────────────
void desktop_draw(){
    // NOTE: The volatile __cursor_bg_dirty guard flag was removed.
    // The IRQ handler reads cursor background from vesa.fb (never
    // touched by this function), so partial back-buffer writes are
    // safe.  If cursor_save_bg is ever changed back to read from
    // vesa.draw, re-add: __cursor_bg_dirty = 1;  here.
    // No vesa_clear() here: draw_wallpaper()/draw_wallpaper_fast()
    // already repaints the entire framebuffer every call, so an
    // extra clear before it just causes a black flash for no
    // benefit. The one-time clear that wipes stale login-screen
    // margin pixels lives in desktop_init() instead, since that
    // only runs once on login.
    //
    // The full cosmic wallpaper (gradient + stars + mountains) is
    // only drawn when actually looking at the desktop icon grid.
    // Every app window (terminal, calc, files, etc.) covers nearly
    // the whole screen anyway, so recomputing the full scene behind
    // it on every keystroke was pure wasted work and the actual
    // cause of input lag — not a RAM shortage.
    if (current_mode == MODE_DESKTOP)
        draw_wallpaper();
    else
        wallpaper_draw_fast();

    // Always-visible taskbar (GNOME-style top bar), tray shows real windows.
    popup_anim_tick();
    draw_taskbar();
    draw_statusbar();
    switch(current_mode){
        case MODE_DESKTOP:
            draw_icons();
            if(search_active) draw_search_bar();
            break;
        case MODE_TERMINAL: draw_terminal_window(); break;
        case MODE_FILES:
            if (viewing_file >= 0) draw_file_viewer(viewing_file);
            else draw_files_window();
            break;
        case MODE_TRASH:    draw_trash_window();    break;
        case MODE_SETTINGS: draw_settings_window(); break;
        case MODE_MUSIC:    draw_music_window();    break;
        case MODE_CALC:     calc_draw();            break;
        case MODE_GAMES:    games_draw();           break;
        case MODE_EDITOR:   editor_draw();          break;
        case MODE_PAINT:    paint_draw();           break;
        case MODE_SYSMON:   sysmon_draw();          break;
        case MODE_CLOCK:    clock_draw();           break;
        case MODE_CALENDAR: calendar_draw();        break;
case MODE_WEATHER:  weather_draw_full();    break;
case MODE_APPLIB:   applib_draw();          break;
case MODE_ORGANIZER: organizer_draw();      break;
case MODE_SECURITY:  security_draw();       break;
case MODE_IMGVIEW:   imgview_draw();        break;
case MODE_GALLERY:   gallery_draw();        break;
case MODE_PDFVIEW:   pdfview_draw();        break;
case MODE_BROWSER:   browser_draw();        break;
case MODE_VIDEO:     video_draw();          break;
case MODE_STORE:     store_draw();          break;
case MODE_GAME:      game_draw();           break;   // full-screen game canvas
case MODE_QUICKSETTINGS: draw_quicksettings_window(); break;
    }

    // Popups drawn AFTER all windows/icons so they always appear on top
    draw_vol_popup();  // volume slider popup (drawn below taskbar)
    draw_wifi_popup(); // Wi-Fi network picker flyout

    // Visual feedback: screen flash overlay
    if (flash_frames > 0) {
        int W = (int)vesa.width, H = (int)vesa.height;
        int strength = flash_frames > 4 ? 4 : flash_frames;
        uint32_t fc = (uint32_t)(flash_r*strength/5) << 16
                    | (uint32_t)(flash_g*strength/5) << 8
                    | (uint32_t)(flash_b*strength/5);
        vesa_fill_rect(0, 0, W, H, fc);
        flash_frames--;
    }

    // ── Undo browse mode overlay ────────────────────────────────
    // A translucent bar across the top of the screen, drawn above
    // everything except the cursor, showing current step info and
    // commit/cancel instructions.
    if (undo_browse_active && current_mode == MODE_DESKTOP) {
        int W = (int)vesa.width;
        int bar_h = 36;
        int by = 2;  // just below the very top edge
        // Translucent dark purple bar
        for (int yy = 0; yy < bar_h; yy++) {
            int prog = yy * 255 / bar_h;
            int rr = 0x2C + (0x18 - 0x2C) * prog / 255;
            int gg = 0x22 + (0x10 - 0x22) * prog / 255;
            int bb = 0x60 + (0x40 - 0x60) * prog / 255;
            vesa_hline(0, by + yy, W, (rr << 16) | (gg << 8) | bb);
        }
        // Bright accent edges
        vesa_hline(0, by, W, 0xD8C0FF);
        vesa_hline(0, by + bar_h - 1, W, 0xC8A8FF);

        // ── Navigation text ─────────────────────────────────────
        int total = undo_ptr + 1;
        int step  = undo_ptr - undo_browse_idx + 1;
        char browse_text[48];
        int ti = 0;
        const char* nav_prefix = "Undo browse  ";
        for (ti = 0; nav_prefix[ti]; ti++)
            browse_text[ti] = nav_prefix[ti];
        // Append "step M/N" where M = current step, N = total
        if (step >= 10) browse_text[ti++] = '0' + step / 10;
        browse_text[ti++] = '0' + step % 10;
        browse_text[ti++] = '/';  // separator
        if (total >= 10) browse_text[ti++] = '0' + total / 10;
        browse_text[ti++] = '0' + total % 10;
        browse_text[ti] = '\0';
        vesa_draw_string(14, by + 10, browse_text, 0xE8D8FF, 0);

        // ── Small instruction hint (right side) ────────────────
        vesa_draw_string(W - 190, by + 10, "Enter=commit  Esc=cancel", 0x9070C0, 0);

        // ── Remaining steps indicator (rounded progress dots) ───
        int max_dots = total < UNDO_DEPTH ? total : UNDO_DEPTH;
        if (max_dots > 1) {
            int dot_start_x = W / 2 - max_dots * 6;
            for (int di = 0; di < max_dots; di++) {
                int dot_idx = total - 1 - di;  // newest first
                int dot_x = dot_start_x + di * 12;
                int dot_filled = (dot_idx <= undo_browse_idx) ? 1 : 0;
                uint32_t dot_col = dot_filled ? 0xC8A8FF : 0x3A2E70;
                vesa_fill_rect(dot_x, by + 14, 6, 6, dot_col);
                vesa_draw_rect(dot_x, by + 14, 6, 6, 0x1A1238);
            }
        }
    }

    // ── Right-click → context menu ──────────────────────────────
    // Check for a pending right-click and determine what was under
    // the cursor. Open a context menu with relevant actions.
    int rclk_x, rclk_y;
    if (!ctx_active && mouse_get_rightclick(&rclk_x, &rclk_y)) {
        int W = (int)vesa.width;

        if (current_mode == MODE_DESKTOP) {
            // Desktop file/folder icon (created via right-click menu)
            int dff = desktop_file_hit_test(rclk_x, rclk_y);
            if (dff >= 0) {
                ctxmenu_init(rclk_x + 8, rclk_y - 10, 3, dff);
            } else {
                // Check if right-click was on a desktop icon tile.
                // IMPORTANT: only OCCUPIED slots get the app-icon menu.
                // Empty slots (icon_order == -1) fall through to the
                // desktop menu below — otherwise the desktop menu with
                // New Folder / New File would be unreachable.
                IconGrid ig = compute_icon_grid(W, (int)vesa.height);
                int gi = icon_grid_hit_test(&ig, rclk_x, rclk_y, -1);
                if (gi >= 0 && icon_order[gi] >= 0) {
                    ctxmenu_init(rclk_x + 8, rclk_y - 10, 0, gi);
                } else if (rclk_y >= 36 && rclk_y < (int)vesa.height - 28) {
                    // Right-click on empty desktop → desktop context menu
                    // (exclude the taskbar strip and the statusbar).
                    ctxmenu_init(rclk_x + 8, rclk_y - 10, 2, 0);
                }
            }
        } else if (current_mode == MODE_FILES &&
                   files_sidebar_trash_x >= 0 &&
                   rclk_x >= files_sidebar_trash_x &&
                   rclk_x < files_sidebar_trash_x + files_sidebar_trash_w &&
                   rclk_y >= files_sidebar_trash_y &&
                   rclk_y < files_sidebar_trash_y + files_sidebar_trash_h) {
            ctxmenu_init(rclk_x + 8, rclk_y - 10, 1, 0);
        }
    }

    // Close context menu if open and a right-click happens outside it
    if (ctx_active && mouse_get_rightclick(&rclk_x, &rclk_y)) {
        if (rclk_x < ctx_x || rclk_x > ctx_x + ctx_w ||
            rclk_y < ctx_y || rclk_y > ctx_y + ctx_h) {
            ctx_active = 0;
        } // right-click on menu itself = just close (left-click handles execution)
    }

    // ── Drag source detection ────────────────────────────────────
    // When a drag first becomes active, determine what was at the
    // press-down position so we know what we're dragging.
    // ── Drag source detection ────────────────────────────────────
    // MUST also check mouse.drop_pending! The PS/2 IRQ fires at 200 Hz
    // while the PIT tick (desktop draw) fires at ~18 Hz. If the user
    // presses, drags past the 4px threshold, and releases the mouse
    // button all between two PIT ticks, then by the time we reach this
    // code `mouse.drag_active` has already been cleared by the release
    // handler.  Without the `mouse.drop_pending` fallback, the drag
    // source is never identified and the drop dispatch falls through
    // with drag_source_type == DRAG_NONE — the icon swap never happens.
    // Skip icon drag detection when a popup is/was open — dragging the
    // volume slider or Wi-Fi flyout would otherwise grab desktop icons.
    // vol_popup_was_open covers the frame where the popup just closed
    // (popup_anim_tick clears open BEFORE click handlers run).
    if ((mouse_is_dragging() || mouse.drop_pending) && drag_source_type == DRAG_NONE
        && !vol_popup_open && !vol_popup_was_open
        && !wifi_popup_open && !wifi_popup_was_open) {
        int W = (int)vesa.width;
        int sx = mouse.drag_down_x;
        int sy = mouse.drag_down_y;

        if (current_mode == MODE_DESKTOP) {
            // Check if press-down was on a desktop icon
            // Use icon_order to map grid position → real app index
            IconGrid ig = compute_icon_grid(W, (int)vesa.height);

            // Compute and store the Trash icon's tile position (icon_order
            // maps grid position → app index; Trash is app index 5, but
            // its grid position depends on how icons have been rearranged).
            int trash_grid = -1;
            for (int g = 0; g < g_grid_slots; g++) {
                if (icon_order[g] == 5) { trash_grid = g; break; }
            }
            if (trash_grid >= 0) {
                icon_grid_slot_pos(&ig, trash_grid, &desktop_trash_icon_x, &desktop_trash_icon_y);
            } else {
                desktop_trash_icon_x = -1;
                desktop_trash_icon_y = -1;
            }

            int hit_drag = icon_grid_hit_test(&ig, sx, sy, -1);
            if (hit_drag >= 0) {
                int aidx = icon_order[hit_drag];
                // Only OCCUPIED slots are draggable.  The full-screen
                // grid has ~112 slots but only 19 hold apps; the rest
                // are empty (icon_order == -1).  Dragging an empty slot
                // used to read app_names[-1] (OOB → crash) and set
                // mag_src_app = -1 (stalling the slide-in animation).
                if (aidx >= 0) {
                    // Group drag: if the IRQ flagged that this press
                    // landed on one of several box-selected icons, move
                    // the whole selection together instead of just this
                    // icon.  The IRQ already snapshotted the selected
                    // slots into g_group_slots[]; we only classify here
                    // so the drop dispatch runs the DRAG_GROUP branch.
                    if (g_group_drag_active) {
                        drag_source_type = DRAG_GROUP;
                        drag_source_idx = hit_drag;
                        g_ghost_app_idx = -1;
                        g_ghost_label = 0;
                    } else {
                        drag_source_type = DRAG_ICON;
                        drag_source_idx = hit_drag;
                        g_ghost_app_idx = aidx;
                        g_ghost_label = app_names[aidx];
                        g_drag_app_idx = aidx;
                        g_drag_app_name = app_names[aidx];
                    }
                } else {
                    // Press-down on an EMPTY slot → rubber-band
                    // selection box.  The IRQ handler already drew the
                    // box live; this only classifies the drop so the
                    // dispatch below can finalize which apps are inside.
                    drag_source_type = DRAG_SELECT;
                    drag_source_idx = -1;
                }
            }
        } else if (current_mode == MODE_FILES && viewing_file < 0) {
            // Check if press-down was on a file row in the Files list.
            // Row maths share files_rows_top()/files_visible_row() with
            // draw_files_window() and the click handler.  The up-row
            // (fi < 0) can't be dragged.
            int W = (int)vesa.width;
            int win_x = (g_win_x >= 0) ? g_win_x : W/10;
            int win_y = (g_win_y >= 0) ? g_win_y : 70;
            int win_w = W*8/10;
            int content_left = win_x + FILES_SB_W + 15;  // after sidebar
            int py = files_rows_top(win_y);

            int nrows = files_visible_count();
            int row_h = 26, gap = 4;
            int fvis = files_visible_rows(win_y, (int)vesa.height - 160);
            for (int r = files_scroll; r < nrows && r < files_scroll + fvis; r++) {
                int fi; files_visible_row(r, &fi);
                if (fi < 0) continue;        // can't drag the up-row
                int ry = py + (r - files_scroll) * (row_h + gap);
                if (sx >= content_left - 8 && sx < win_x + win_w - 16 &&
                    sy >= ry && sy < ry + row_h) {
                    drag_source_type = DRAG_FILE;
                    drag_source_idx = fi;
                    break;
                }
            }
        } else if (current_mode != MODE_DESKTOP && vesa.active &&
                   g_win_x >= 0 && g_win_w > 0) {
            // Check if press-down was on a window title bar (not on [X])
            int sx = mouse.drag_down_x;
            int sy = mouse.drag_down_y;
            int title_h = 36;
            if (sy >= g_win_y && sy < g_win_y + title_h &&
                sx >= g_win_x && sx < g_win_x + g_win_w - 40) {
                drag_source_type = DRAG_WINDOW;
                g_win_off_x = sx - g_win_x;
                g_win_off_y = sy - g_win_y;
            }
        }
    }

    // Clear drag source when drag ends (but NOT yet — drop handler
    // will clear it after processing)
    if (!mouse_is_dragging() && !mouse.drop_pending) {
        drag_source_type = DRAG_NONE;
        drag_source_idx = -1;
        g_ghost_app_idx = -1;
        g_ghost_label = 0;
    }

    // ── Drag ghost ───────────────────────────────────────────────
    // Draw a visual indicator of what's being dragged, following the
    // cursor position. Drawn on the back buffer before the cursor.
    if (mouse_is_dragging() && drag_source_type != DRAG_NONE) {
        int cx = mouse.px;
        int cy = mouse.py;

        if (drag_source_type == DRAG_ICON) {
            int W = (int)vesa.width;
            int H = (int)vesa.height;

            // Ghost: semi-transparent tile outline at cursor
            // Use icon_order to map grid position → app index
            int app_idx = (drag_source_idx >= 0 && drag_source_idx < g_grid_slots)
                            ? icon_order[drag_source_idx] : drag_source_idx;
            int tile_w = 64, tile_h = 62;
            int gx = cx - tile_w/2, gy = cy - tile_h/2;
            icon_rrect_border(gx, gy, tile_w, tile_h, 0xB088FF);
            icon_rrect_grad(gx, gy, tile_w, tile_h, 0x2E2050, 0x1E1240);
            // Draw the symbol
            int sym_cx = gx + tile_w/2;
            int sym_cy = gy + tile_h/2 - 2;
            draw_icon_symbol(app_idx, sym_cx, sym_cy, 0xFFFFFF);
            // Label below ghost
            vesa_draw_string(gx + 4, gy + tile_h + 2, app_names[app_idx], 0xB088FF, 0);

            // ── Drop-zone snap highlight ─────────────────────────
            // Compute the grid slot the mouse cursor is currently
            // hovering over, and draw a translucent highlight there
            // so the user can see where the icon will land.
            IconGrid ig_snap = compute_icon_grid(W, H);
            int grid_tile_h = ICON_TILE_H;
            int grid_label_h = ICON_LABEL_H;

            int target_grid = -1;
            target_grid = icon_grid_hit_test(&ig_snap, cx, cy, drag_source_idx);

            if (target_grid >= 0) {
                int hx, hy;
                icon_grid_slot_pos(&ig_snap, target_grid, &hx, &hy);
                int ht_w = ICON_TILE_W, ht_h = grid_tile_h + grid_label_h + 4;

                // Translucent fill: semi-transparent bright purple
                // drawn as alternating pixel rows to simulate alpha
                for (int yy = 0; yy < ht_h; yy += 2) {
                    vesa_hline(hx, hy + yy, ht_w, 0x3A2080);
                }
                // Bright outer border
                icon_rrect_border(hx, hy, ht_w, ht_h, 0xC8A0FF);
                // Inner glow border (offset by 1px)
                icon_rrect_border(hx-1, hy-1, ht_w+2, ht_h+2, 0x6040A0);
                // Left accent bar
                vesa_vline(hx, hy, ht_h, 0x9060F0);
                // Corner accent dots
                vesa_put_pixel(hx, hy, 0xE0C8FF);
                vesa_put_pixel(hx + ht_w - 1, hy, 0xE0C8FF);
                vesa_put_pixel(hx, hy + ht_h - 1, 0xE0C8FF);
                vesa_put_pixel(hx + ht_w - 1, hy + ht_h - 1, 0xE0C8FF);

                // Status hint: "Swap with X" label
                int swap_idx = icon_order[target_grid];
                char hint[24];
                int hi = 0;
                const char* sn = app_names[swap_idx];
                const char* src = "Swap with ";
                for (int si = 0; src[si]; si++) hint[hi++] = src[si];
                for (int si = 0; sn[si]; si++) hint[hi++] = sn[si];
                hint[hi] = '\0';
                int hw = hi * 8;
                vesa_fill_rect(hx + ht_w/2 - hw/2 - 4, hy - 14, hw + 8, 14, 0x1A0E30);
                icon_rrect_border(hx + ht_w/2 - hw/2 - 4, hy - 14, hw + 8, 14, 0x8050C0);
                vesa_draw_string(hx + ht_w/2 - hw/2, hy - 12, hint, 0xE0C8FF, 0x1A0E30);
            }
        } else if (drag_source_type == DRAG_FILE) {
            // Ghost: small document icon + filename at cursor
            int doc_w = 40, doc_h = 48;
            int gx = cx - 4, gy = cy - 4;
            // Document body
            vesa_fill_rect(gx, gy, doc_w, doc_h, 0x2E2050);
            icon_rrect_border(gx, gy, doc_w, doc_h, 0xB088FF);
            // Folded corner
            vesa_hline(gx+doc_w-8, gy, 8, 0x1E1240);
            vesa_vline(gx+doc_w-1, gy, 8, 0x1E1240);
            // Filename next to document
            vesa_draw_string(gx + doc_w + 6, gy + 4, filesystem[drag_source_idx].name, 0xD8C8F0, 0);
            // Drop hint
            vesa_draw_string(gx + doc_w + 6, gy + 22, "Drop on Trash to delete", 0x6A5890, 0);
        } else if (drag_source_type == DRAG_WINDOW) {
            // Window drag: no ghost drawn — the window follows the cursor
            // on every desktop_draw() frame. Update the window position
            // to track the cursor from the press-down offset.
            g_win_x = mouse.px - g_win_off_x;
            g_win_y = mouse.py - g_win_off_y;
            // Clamp: keep at least 80px of the window visible on
            // both axes so the title bar / close button are accessible.
            int W = (int)vesa.width, H = (int)vesa.height;
            if (g_win_x + g_win_w < 80)  g_win_x = 80 - g_win_w;
            if (g_win_x > W - 80)        g_win_x = W - 80;
            if (g_win_y + g_win_h < 80)  g_win_y = 80 - g_win_h;
            if (g_win_y > H - 80)        g_win_y = H - 80;
        }
    }

    // ── Drag-hover highlights ────────────────────────────────────
    // When dragging a file, glow the Trash icon (desktop) or the
    // Trash sidebar (Files app) when the cursor is near them,
    // giving the user clear visual feedback of valid drop targets.
    if (mouse_is_dragging() && drag_source_type == DRAG_FILE) {
        int mx = mouse.px, my = mouse.py;

        // ── Desktop Trash icon glow ───────────────────────────────
        if (current_mode == MODE_DESKTOP &&
            desktop_trash_icon_x >= 0 &&
            mx >= desktop_trash_icon_x - 4 &&
            mx < desktop_trash_icon_x + desktop_trash_icon_w + 4 &&
            my >= desktop_trash_icon_y - 4 &&
            my < desktop_trash_icon_y + desktop_trash_icon_h + 4) {
            // Glowing halo around the Trash icon
            int hx = desktop_trash_icon_x - 4;
            int hy = desktop_trash_icon_y - 4;
            int hw = desktop_trash_icon_w + 8;
            int hh = desktop_trash_icon_h + 8;
            for (int d = 1; d <= 3; d++) {
                uint32_t gc = 0xFF4400 + (d * 0x000800);
                icon_rrect_border(hx - d, hy - d, hw + d*2, hh + d*2, gc);
            }
        }
    }

    // ── Context menu (drawn before cursor, after all app panels) ──
    if (ctx_active) {
        ctxmenu_draw();
    }

    // ── Mouse debug overlay (shows position/state, fades after boot) ─
    // Shows for the first ~50 PIT ticks (~2.7 seconds at 18.2 Hz) so
    // we can see if mouse interrupts are firing. Drawn below the
    // taskbar to avoid overlapping it. Displays px, py and button
    // states in green on black.
    if (vesa.active && tick < 50) {
        char dbg[48];
        int p = 0;
        dbg[p++] = 'M'; dbg[p++] = ':'; dbg[p++] = ' ';
        // px (up to 5 digits, space-padded)
        int px = mouse.px;
        if (px >= 10000) { dbg[p++] = '0' + px/10000; px %= 10000; } else dbg[p++] = ' ';
        if (px >= 1000)  { dbg[p++] = '0' + px/1000;  px %= 1000;  } else dbg[p++] = ' ';
        if (px >= 100)   { dbg[p++] = '0' + px/100;   px %= 100;   } else dbg[p++] = ' ';
        if (px >= 10)    { dbg[p++] = '0' + px/10;    px %= 10;    } else dbg[p++] = ' ';
        dbg[p++] = '0' + px;
        dbg[p++] = ','; dbg[p++] = ' ';
        // py
        int py = mouse.py;
        if (py >= 10000) { dbg[p++] = '0' + py/10000; py %= 10000; } else dbg[p++] = ' ';
        if (py >= 1000)  { dbg[p++] = '0' + py/1000;  py %= 1000;  } else dbg[p++] = ' ';
        if (py >= 100)   { dbg[p++] = '0' + py/100;   py %= 100;   } else dbg[p++] = ' ';
        if (py >= 10)    { dbg[p++] = '0' + py/10;    py %= 10;    } else dbg[p++] = ' ';
        dbg[p++] = '0' + py;
        dbg[p++] = ' ';
        // Button states
        dbg[p++] = 'L'; dbg[p++] = ':'; dbg[p++] = mouse.left  ? '1' : '0'; dbg[p++] = ' ';
        dbg[p++] = 'R'; dbg[p++] = ':'; dbg[p++] = mouse.right ? '1' : '0'; dbg[p++] = ' ';
        dbg[p] = 0;

        // Below taskbar (taskbar ends at y=32)
        vesa_fill_rect(4, 36, 200, 14, 0x000000);
        vesa_draw_string(6, 36, dbg, 0x00FF00, 0x000000);
    }

    // ── Drag debug overlay ───────────────────────────────────────
    // While dragging, show the current grid slot (col, row), the app
    // index at that slot, and the app name at a fixed position so the
    // user can verify hit-testing is landing on the right slot.
    if (vesa.active && mouse_is_dragging() && current_mode == MODE_DESKTOP) {
        int W = (int)vesa.width;
        int H = (int)vesa.height;
        IconGrid ig = compute_icon_grid(W, H);
        int col = (mouse.px - ig.start_x) / ig.cell_w;
        int row = (mouse.py - ig.start_y) / ig.cell_h;
        int slot = -1;
        int valid = (col >= 0 && col < ICON_GRID_COLS && row >= 0 && row < g_grid_rows);
        if (valid) slot = col + row * ICON_GRID_COLS;
        int app_idx = (valid && slot >= 0 && slot < g_grid_slots)
                        ? icon_order[slot] : -1;

        // Build debug string: "G:col,row slot=# app=# name"
        // Example: "G:4,2 slot=58 app=7 name=Calc"
        char dbg[48]; int p = 0;
        dbg[p++] = 'G'; dbg[p++] = ':'; dbg[p++] = ' ';
        // col
        dbg[p++] = '0' + (col / 10); dbg[p++] = '0' + (col % 10);
        dbg[p++] = ',';
        // row
        dbg[p++] = '0' + (row / 10); dbg[p++] = '0' + (row % 10);
        dbg[p++] = ' ';
        // slot =
        dbg[p++] = 's'; dbg[p++] = 'l'; dbg[p++] = 'o'; dbg[p++] = 't'; dbg[p++] = '=';
        if (slot >= 100) { dbg[p++] = '0' + slot / 100; }
        if (slot >= 10)  { dbg[p++] = '0' + (slot / 10) % 10; }
        dbg[p++] = '0' + (slot % 10);
        dbg[p++] = ' ';
        // app=
        dbg[p++] = 'a'; dbg[p++] = 'p'; dbg[p++] = 'p'; dbg[p++] = '=';
        if (app_idx >= 0) {
            if (app_idx >= 10) dbg[p++] = '0' + app_idx / 10;
            dbg[p++] = '0' + app_idx % 10;
            dbg[p++] = ' ';
            // name (copy inline, truncated)
            const char* n = app_names[app_idx];
            for (int i = 0; n[i] && p < 44; i++)
                dbg[p++] = n[i];
        } else {
            dbg[p++] = '-'; dbg[p++] = '1'; dbg[p++] = ' ';
            dbg[p++] = '('; dbg[p++] = 'e'; dbg[p++] = 'm'; dbg[p++] = 'p'; dbg[p++] = 't'; dbg[p++] = 'y'; dbg[p++] = ')';
        }
        dbg[p] = '\0';

        // Draw below the mouse debug overlay (same Y if visible, or just below taskbar)
        int dbg_y = 36;
        vesa_fill_rect(4, dbg_y, 280, 14, 0x000000);
        vesa_draw_string(6, dbg_y, dbg, 0xFFCC44, 0x000000);

        // ── Drag source info below ────────────────────────────────
        if (drag_source_idx >= 0) {
            char src[48]; int sp = 0;
            src[sp++] = 'S'; src[sp++] = 'r'; src[sp++] = 'c'; src[sp++] = ':'; src[sp++] = ' ';
            src[sp++] = 's'; src[sp++] = 'l'; src[sp++] = 'o'; src[sp++] = 't'; src[sp++] = '=';
            int si = drag_source_idx;
            if (si >= 100) { src[sp++] = '0' + si / 100; }
            if (si >= 10)  { src[sp++] = '0' + (si / 10) % 10; }
            src[sp++] = '0' + (si % 10);
            src[sp++] = ' ';
            src[sp++] = 'a'; src[sp++] = 'p'; src[sp++] = 'p'; src[sp++] = '=';
            int sa = g_drag_app_idx;
            if (sa >= 0) {
                if (sa >= 10) src[sp++] = '0' + sa / 10;
                src[sp++] = '0' + sa % 10;
                src[sp++] = ' ';
                const char* sn = app_names[sa];
                for (int i = 0; sn[i] && sp < 44; i++)
                    src[sp++] = sn[i];
            } else {
                src[sp++] = '-'; src[sp++] = '1';
            }
            src[sp] = '\0';

            vesa_fill_rect(4, dbg_y + 14, 280, 14, 0x000000);
            vesa_draw_string(6, dbg_y + 14, src, 0x44FF88, 0x000000);
        }
    }

    // ── Drop dispatch (checked BEFORE clicks) ────────────────────
    // When the user releases the mouse button after dragging,
    // the IRQ handler sets drop_pending. We check it here and
    // route the drop to the right handler based on the drop
    // position and current application mode.
    int drop_x, drop_y;
    if (mouse_get_drop(&drop_x, &drop_y)) {
        int W = (int)vesa.width;
        int H = (int)vesa.height;

        if (drag_source_type == DRAG_FILE) {
            int fi = drag_source_idx;
            int trashed = 0;

            // ── Drop on Files sidebar Trash zone ──────────────────
            if (current_mode == MODE_FILES &&
                files_sidebar_trash_x >= 0 &&
                drop_x >= files_sidebar_trash_x &&
                drop_x < files_sidebar_trash_x + files_sidebar_trash_w &&
                drop_y >= files_sidebar_trash_y &&
                drop_y < files_sidebar_trash_y + files_sidebar_trash_h) {
                if (fi >= 0 && fi < MAX_FILES && filesystem[fi].used) {
                    move_to_trash(fi);
                    desktop_set_status("Moved to Trash");
                    trashed = 1;
                }
            }

            // ── Drop on desktop Trash icon ────────────────────────
            // NOTE: Currently unreachable because MODE_FILES is
            // full-screen (user can't see desktop icons while
            // dragging a file). Reserved for a future non-fullscreen
            // Files window that shows the desktop behind it.
            if (!trashed && current_mode == MODE_DESKTOP &&
                desktop_trash_icon_x >= 0 &&
                drop_x >= desktop_trash_icon_x &&
                drop_x < desktop_trash_icon_x + desktop_trash_icon_w &&
                drop_y >= desktop_trash_icon_y &&
                drop_y < desktop_trash_icon_y + desktop_trash_icon_h) {
                if (fi >= 0 && fi < MAX_FILES && filesystem[fi].used) {
                    move_to_trash(fi);
                    desktop_set_status("Moved to Trash");
                    trashed = 1;
                }
            }

            // ── Drop on status bar = trash (fallback) ────────────
            if (!trashed && drop_y > H - 40) {
                if (fi >= 0 && fi < MAX_FILES && filesystem[fi].used) {
                    move_to_trash(fi);
                    desktop_set_status("Moved to Trash");
                }
            }

            // ── Drop in Paint canvas area ─────────────────────────
            if (!trashed && current_mode == MODE_PAINT) {
                paint_drop_file(drag_source_idx);
            }
        } else if (drag_source_type == DRAG_ICON) {
            // ── Icon rearrangement ────────────────────────────────
            // The grid has 30 slots (6x5). 19 slots hold apps, the
            // rest are empty (-1 in icon_order[]).  Dropping on an
            // occupied slot swaps the two entries; dropping on an
            // empty slot moves the dragged icon there (leaving its
            // old slot empty).  Dropping outside any grid slot
            // launches the dragged app.
            IconGrid ig = compute_icon_grid(W, H);
            int hit_grid = -1;

            hit_grid = icon_grid_hit_test(&ig, drop_x, drop_y, drag_source_idx);

            if (hit_grid >= 0) {
                int src_app = icon_order[drag_source_idx];
                // Start slide-in animation instead of immediately
                // teleporting.  The ghost slides from its last drawn
                // position (where the tile was when dropped) to the
                // target slot centre over ~200 ms, driven by the
                // sub-tick clock, then the swap/move is committed by
                // the heartbeat.
                int tgt_app = icon_order[hit_grid];
                int pp, ppy;
                icon_grid_slot_pos(&ig, hit_grid, &pp, &ppy);
                mag_sx = g_ghost_filter_px;
                mag_sy = g_ghost_filter_py;
                mag_ex = pp + ICON_TILE_W / 2;
                mag_ey = ppy + ICON_TILE_H / 2;
                mag_src_slot    = drag_source_idx;
                mag_tgt_slot    = hit_grid;
                mag_src_app     = src_app;
                mag_tgt_type    = (tgt_app < 0) ? 0 : 1;
                mag_tgt_occupied = (tgt_app < 0) ? -1 : tgt_app;
                mag_fraction    = 0;      // restart the sub-tick clock
                mag_pkt_count   = 0;      // reset IRQ stall detection
                mag_pkt_last_seen = 0;
                mag_active      = 1;
                // The drop's desktop_draw() present already wiped the
                // drag ghost; clear the dirty-rectangle state so the
                // animation's first render starts from a clean slate.
                ghost_invalidate_bg();
                // The IRQ draws the flying ghost at 200 Hz while the
                // mouse moves; the heartbeat renders it as the idle
                // fallback.  No immediate draw_icons() needed — the
                // heartbeat's mag_active loop calls desktop_draw() on
                // the next PIT tick, redrawing icons behind the ghost.
                last_click_icon = -1;
                desktop_set_status("");
            } else {
                // Dropped outside any detected grid slot — could be
                // on our own slot (skip_idx filter) or truly outside.
                // Check: is the drop within the source slot's cell?
                int drop_col = (drop_x - ig.start_x) / ig.cell_w;
                int drop_row = (drop_y - ig.start_y) / ig.cell_h;
                int own_slot = 0;
                if (drop_col >= 0 && drop_col < ICON_GRID_COLS &&
                    drop_row >= 0 && drop_row < g_grid_rows) {
                    int slot = drop_col + drop_row * ICON_GRID_COLS;
                    if (slot == drag_source_idx) own_slot = 1;
                }

                if (own_slot) {
                    // Spring-loaded snap-back: ghost flies from the
                    // drop position back to the source slot center.
                    // icon_order[] is NOT modified — the icon never
                    // left its original slot; we're animating the
                    // visual return.
                    int pp, ppy;
                    icon_grid_slot_pos(&ig, drag_source_idx, &pp, &ppy);
                    snap_sx = drop_x;
                    snap_sy = drop_y;
                    snap_ex = pp + ICON_TILE_W / 2;
                    snap_ey = ppy + ICON_TILE_H / 2;
                    snap_slot = drag_source_idx;
                    snap_frame = 0;
                    snap_active = 1;
                    last_click_icon = -1;  // disarm stale click
                    desktop_set_status("");  // suppress stale status
                } else {
                    // Truly outside the grid — launch the app
                    int app_idx = icon_order[drag_source_idx];
                    do_launch(app_idx);
                }
            }
        } else if (drag_source_type == DRAG_GROUP) {
            // ── Group drag: move all selected icons together ──────
            // The IRQ snapshotted the box-selected slots into
            // g_group_slots[] (anchor = the pressed slot).  Translate
            // every selected slot by the same (dcol,drow) as the
            // anchor's drop position, preserving the relative layout.
            // Displaced outsiders move into the vacated group slots
            // (swap semantics, mirroring the single-icon case).
            int W = (int)vesa.width;
            int H = (int)vesa.height;
            IconGrid ig = compute_icon_grid(W, H);
            int target = icon_grid_hit_test(&ig, drop_x, drop_y, -1);
            int anchor = g_group_anchor;
            int grp_n = g_group_count;
            if (target >= 0 && anchor >= 0 && grp_n > 1 &&
                grp_n <= GROUP_MAX_SLOTS) {
                int a_col = anchor % ICON_GRID_COLS;
                int a_row = anchor / ICON_GRID_COLS;
                int t_col = target % ICON_GRID_COLS;
                int t_row = target / ICON_GRID_COLS;
                int dcol = t_col - a_col;
                int drow = t_row - a_row;
                // Validate every translated slot is in-bounds.
                int ok = 1;
                for (int i = 0; i < grp_n && ok; i++) {
                    int s = g_group_slots[i];
                    int c = (s % ICON_GRID_COLS) + dcol;
                    int r = (s / ICON_GRID_COLS) + drow;
                    if (c < 0 || c >= ICON_GRID_COLS ||
                        r < 0 || r >= g_grid_rows) ok = 0;
                }
                if (ok && (dcol != 0 || drow != 0)) {
                    // Save undo snapshot BEFORE modifying icon_order.
                    undo_ptr++;
                    if (undo_ptr >= UNDO_DEPTH) {
                        for (int _si = 0; _si < UNDO_DEPTH - 1; _si++)
                            for (int _sj = 0; _sj < ICON_GRID_SLOTS; _sj++)
                                undo_stack[_si][_sj] = undo_stack[_si + 1][_sj];
                        undo_ptr = UNDO_DEPTH - 1;
                    }
                    for (int _si = 0; _si < ICON_GRID_SLOTS; _si++)
                        undo_stack[undo_ptr][_si] = icon_order[_si];
                    redo_ptr = -1;

                    int tmp[ICON_GRID_SLOTS];
                    for (int i = 0; i < ICON_GRID_SLOTS; i++) tmp[i] = icon_order[i];
                    // 1. Vacate all selected source slots.
                    for (int i = 0; i < grp_n; i++) tmp[g_group_slots[i]] = -1;
                    // 2. Place each selected app at its translated slot;
                    //    any outsider sitting on a target is displaced.
                    int displaced[GROUP_MAX_SLOTS];
                    int nd = 0;
                    for (int i = 0; i < grp_n; i++) {
                        int s = g_group_slots[i];
                        int t = group_shift_slot(s, dcol, drow);
                        int occ = tmp[t];
                        if (occ >= 0 && nd < GROUP_MAX_SLOTS) displaced[nd++] = occ;
                        tmp[t] = icon_order[s];
                    }
                    // 3. Rehome displaced outsiders into the free
                    //    vacated group slots (first-free order).
                    for (int d = 0; d < nd; d++) {
                        for (int i = 0; i < grp_n; i++) {
                            int s = g_group_slots[i];
                            if (tmp[s] == -1) { tmp[s] = displaced[d]; break; }
                        }
                    }
                    for (int i = 0; i < ICON_GRID_SLOTS; i++) icon_order[i] = tmp[i];
                    icon_layout_save();
                    // The teal selection rings follow the moved icons.
                    for (int i = 0; i < g_grid_slots; i++) box_sel[i] = 0;
                    for (int i = 0; i < grp_n; i++) {
                        int s = g_group_slots[i];
                        int t = group_shift_slot(s, dcol, drow);
                        box_sel[t] = 1;
                    }
                    selected_icon = group_shift_slot(anchor, dcol, drow);
                    char msg[24]; int m = 0;
                    const char* pre = "Moved ";
                    for (int i = 0; pre[i]; i++) msg[m++] = pre[i];
                    if (grp_n >= 100) msg[m++] = '0' + grp_n / 100;
                    if (grp_n >= 10) msg[m++] = '0' + (grp_n / 10) % 10;
                    msg[m++] = '0' + grp_n % 10;
                    const char* suf = " icons";
                    for (int i = 0; suf[i]; i++) msg[m++] = suf[i];
                    msg[m] = '\0';
                    desktop_set_status(msg);
                } else {
                    desktop_set_status("");
                }
            } else {
                desktop_set_status("");
            }
            g_group_drag_active = 0;
            g_group_count = 0;
            g_group_anchor = -1;
            group_ghost_invalidate();
            drag_source_type = DRAG_NONE;
            drag_source_idx = -1;
            g_ghost_app_idx = -1;
            g_ghost_label = 0;
            // Reset drag metadata too (the DRAG_ICON path sets it; the
            // group path doesn't, so without this a stale app index
            // from a previous drag lingers in g_drag_app_idx).
            g_drag_app_idx = -1;
            g_drag_app_name = 0;
            // Disarm any stale press-click (parity with the DRAG_ICON
            // drop): the drop's goto skips the click dispatch, so a
            // fast drag could otherwise fire a leftover click on the
            // next heartbeat and wipe the freshly-moved selection.
            last_click_icon = -1;
            g_win_off_x = 0;
            g_win_off_y = 0;
            redraw_content();
            cursor_bg_x = -1;
            goto after_click_dispatch;
        } else if (drag_source_type == DRAG_SELECT) {
            // ── Rubber-band selection finalize ────────────────────
            // The IRQ drew the translucent box live (XOR'd on vesa.fb);
            // the present below wipes those pixels.  Here we compute
            // which apps lie inside the box and keep them highlighted
            // (teal rings in draw_icons) until the next click.
            int bx0 = g_sel_x0, by0 = g_sel_y0;
            int bx1 = drop_x, by1 = drop_y;
            if (bx0 > bx1) { int t = bx0; bx0 = bx1; bx1 = t; }
            if (by0 > by1) { int t = by0; by0 = by1; by1 = t; }

            IconGrid ig_sel = compute_icon_grid(W, H);
            int sel_count = 0, sel_first = -1;
            for (int s = 0; s < g_grid_slots; s++) {
                box_sel[s] = 0;
                if (icon_order[s] < 0) continue;
                int px, py;
                icon_grid_slot_pos(&ig_sel, s, &px, &py);
                int tw = ICON_TILE_W, th = ICON_TILE_H + ICON_LABEL_H;
                if (px < bx1 && px + tw > bx0 && py < by1 && py + th > by0) {
                    box_sel[s] = 1;
                    if (sel_first < 0) sel_first = s;
                    sel_count++;
                }
            }
            if (sel_count > 0) {
                selected_icon = sel_first;
                char msg[24]; int m = 0;
                const char* pre = "Selected: ";
                for (int i = 0; pre[i]; i++) msg[m++] = pre[i];
                if (sel_count >= 100) msg[m++] = '0' + sel_count / 100;
                if (sel_count >= 10) msg[m++] = '0' + (sel_count / 10) % 10;
                msg[m++] = '0' + sel_count % 10;
                const char* suf = " apps";
                for (int i = 0; suf[i]; i++) msg[m++] = suf[i];
                msg[m] = '\0';
                desktop_set_status(msg);
            } else {
                desktop_set_status("No apps in selection");
            }
            // Redraw immediately so the teal selection rings appear
            // (the scene was drawn before the drop was dispatched).
            drag_source_type = DRAG_NONE;
            drag_source_idx = -1;
            g_ghost_app_idx = -1;
            g_ghost_label = 0;
            g_win_off_x = 0;
            g_win_off_y = 0;
            redraw_content();
            cursor_bg_x = -1;
            goto after_click_dispatch;
        }

        // Clear drag state
        drag_source_type = DRAG_NONE;
        drag_source_idx = -1;
        g_ghost_app_idx = -1;
        g_ghost_label = 0;
        g_win_off_x = 0;
        g_win_off_y = 0;
    }

    // ── Click dispatch ───────────────────────────────────────────
    // Check for a pending left-click and route it to the appropriate
    // desktop element. The click event was set by the IRQ12 handler.
    static int __clk_this_frame = 0;  // 1 if a click was processed
    __clk_this_frame = 0;
    int clk_x, clk_y;
    if (mouse_get_click(&clk_x, &clk_y)) {
        __clk_this_frame = 1;
        // ── Snapshot click_count before reset ─────────────────────
        // Capture how many presses happened since the last heartbeat
        // (used for same-heartbeat double-click detection below),
        // then reset so no residue carries across heartbeats or modes.
        int rapid_clicks = mouse.click_count;
        mouse.click_count = 0;
        // Mode at dispatch entry: flyout-style panels (Quick Settings)
        // must NOT treat the click that just opened them as an
        // outside-click.  The panel opens centered while the cursor is
        // still over the icon that launched it — far from the panel.
        int dispatch_mode_before = current_mode;

        int W = (int)vesa.width;
        int H = (int)vesa.height;

        // ── Launcher click ───────────────────────────────────────
        // If the Quick Launch panel is open, check if the click is
        // inside it first — consumes the click and closes if outside.
        if (launcher_is_open()) {
            if (launcher_handle_mouse(clk_x, clk_y)) {
                redraw_content();
                cursor_bg_x = -1;
                goto after_click_dispatch;
            }
            // Click outside launcher — it already closed, redraw
            redraw_content();
            cursor_bg_x = -1;
            goto after_click_dispatch;
        }

        // ── Context menu item click ───────────────────────────────
        // If the context menu is open and the click is inside it,
        // execute the action for the clicked item.
        if (ctx_active &&
            clk_x >= ctx_x && clk_x <= ctx_x + ctx_w &&
            clk_y >= ctx_y && clk_y <= ctx_y + ctx_h) {
            for (int i = 0; i < ctx_count; i++) {
                int iy = ctx_y + 6 + i * 26;
                if (clk_y >= iy && clk_y < iy + 24) {
                    int act = ctx_actions[i];
                    if (act == CTX_ACT_OPEN && ctx_payload_type == 0) {
                        int app_idx = icon_order[ctx_payload_idx];
                        do_launch(app_idx);
                    } else if (act == CTX_ACT_OPEN && ctx_payload_type == 3) {
                        // Open a desktop file/folder icon
                        files_open_file(ctx_payload_idx);
                    } else if (act == CTX_ACT_DELETE_FILE) {
                        // Delete a desktop file/folder icon (to trash).
                        // The icon stays in desktop_files[] — trashed
                        // files are hidden by the draw loop (used==0)
                        // and reappear if restored from the Trash app.
                        if (ctx_payload_idx >= 0 && ctx_payload_idx < MAX_FILES &&
                            filesystem[ctx_payload_idx].used) {
                            move_to_trash(ctx_payload_idx);
                            desktop_set_status("Moved to Trash");
                        }
                    } else if (act == CTX_ACT_RENAME) {
                        desktop_set_status("Rename: use Terminal `mv` command");
                    } else if (act == CTX_ACT_PROPERTIES) {
                        desktop_set_status("Properties: check the About window");
                    } else if (act == CTX_ACT_OPEN_TRASH) {
                        trash_selected = 0;
                        current_mode = MODE_TRASH;
                    } else if (act == CTX_ACT_EMPTY_TRASH) {
                        empty_trash();
                        desktop_set_status("Trash emptied!");
                    } else if (act == CTX_ACT_NEW_FOLDER) {
                        desktop_new_folder();
                    } else if (act == CTX_ACT_NEW_FILE) {
                        desktop_new_file();
                    } else if (act == CTX_ACT_REFRESH) {
                        fs_load_from_disk();   // re-read user files from disk
                        // Keep the selection valid if the reload shrank the list
                        if (selected_file > file_count - 1)
                            selected_file = file_count > 0 ? file_count - 1 : 0;
                        // Drop desktop-file icons whose files vanished
                        for (int di = 0; di < desktop_files_count; di++) {
                            int fi = desktop_files[di];
                            if (fi < 0 || fi >= MAX_FILES || !filesystem[fi].used) {
                                for (int dj = di; dj < desktop_files_count - 1; dj++)
                                    desktop_files[dj] = desktop_files[dj + 1];
                                desktop_files_count--;
                                di--;
                            }
                        }
                        desktop_set_status("Desktop refreshed");
                    } else if (act == CTX_ACT_WALLPAPER) {
                        settings_section = 6;   // wallpaper section
                        current_mode = MODE_SETTINGS;
                    } else if (act == CTX_ACT_RESET_LAYOUT) {
                        for (int i = 0; i < DESKTOP_APP_COUNT; i++)
                            icon_order[i] = i;
                        for (int i = DESKTOP_APP_COUNT; i < g_grid_slots; i++)
                            icon_order[i] = -1;
                        for (int i = g_grid_slots; i < ICON_GRID_SLOTS; i++)
                            icon_order[i] = -1;
                        icon_layout_save();
                        desktop_set_status("Icon layout reset to default");
                    }
                    ctx_active = 0;
                    redraw_content();
                    cursor_bg_x = -1;
                    goto after_click_dispatch;
                }
            }
            ctx_active = 0;
            redraw_content();
            cursor_bg_x = -1;
            goto after_click_dispatch;
        }
        ctx_active = 0; // Click outside closes menu

        // ── Popup clicks (below taskbar) ────────────────────────
        // The volume/WiFi popups extend below y=28, so clicks on
        // them must be caught before the taskbar zone check.
        if (vol_popup_open && vol_popup_click(clk_x, clk_y)) {
            goto after_click_dispatch;
        }
        if (wifi_popup_open && wifi_popup_click(clk_x, clk_y)) {
            goto after_click_dispatch;
        }

        // ── Taskbar click zone (rows 0-27, GNOME-style top bar) ─
        if (clk_y < 28) {
            // Volume icon click → toggle popup (close WiFi first)
            if (clk_x >= vol_popup_x && clk_x < vol_popup_x + 20 &&
                clk_y >= 0 && clk_y < 28) {
                wifi_popup_open = 0; wifi_popup_anim = 0; wifi_popup_closing = 0; // immediate
                if (vol_popup_open) { vol_popup_closing = 1; } // animated close
                else { vol_popup_open = 1; vol_popup_closing = 0; vol_popup_just_opened = 1; } // animated open
                vol_popup_drag = 0;
                drag_source_type = DRAG_NONE; drag_source_idx = -1;
                goto after_click_dispatch;
            }
            // Wi-Fi popup click (if open)
            if (wifi_popup_open && wifi_popup_click(clk_x, clk_y)) {
                goto after_click_dispatch;
            }
            // Wi-Fi icon click → toggle flyout (close volume first)
            if (clk_x >= wifi_popup_x && clk_x < wifi_popup_x + 20 &&
                clk_y >= 0 && clk_y < 28) {
                vol_popup_open = 0; vol_popup_anim = 0; vol_popup_closing = 0; // immediate
                if (wifi_popup_open) { wifi_popup_closing = 1; } // animated close
                else { wifi_popup_open = 1; wifi_popup_closing = 0; wifi_popup_just_opened = 1; } // animated open
                drag_source_type = DRAG_NONE; drag_source_idx = -1;
                if (wifi_popup_open && qs_wifi_on && qs_wifi_scan == 0)
                    qs_wifi_scan = 60;  // trigger scan animation
                goto after_click_dispatch;
            }
            // Click elsewhere on taskbar → close popups (animated)
            if (vol_popup_open && !vol_popup_closing) vol_popup_closing = 1;
            if (wifi_popup_open && !wifi_popup_closing) wifi_popup_closing = 1;
            int nw = win_count();
            int band_x0 = W/2 - 60;
            int band_x1 = band_x0 + nw * (108 + 8);
            if (nw > 0 && clk_x >= band_x0 && clk_x < band_x1) {
                int tb_x = band_x0;
                int hit = -1;
                for (int i = 0; i < nw && i < WIN_TRAY_MAX; i++) {
                    if (clk_x >= tb_x && clk_x < tb_x + 108) { hit = i; break; }
                    tb_x += 108 + 8;
                }
                if (hit >= 0) {
                    int m = win_mode_at(hit);
                    if (m > MODE_DESKTOP && m != current_mode) {
                        win_focus(m);
                        current_mode = m;
                        redraw_content();
                        cursor_bg_x = -1;
                    }
                    goto after_click_dispatch;
                }
                if (current_mode != MODE_DESKTOP) {
                    win_focus(MODE_DESKTOP);
                    current_mode = MODE_DESKTOP;
                    redraw_content();
                    cursor_bg_x = -1;
                }
                goto after_click_dispatch;
            }
        }

        // ── [X] close button in windowed apps ────────────────────
        // Compute the [X] position from the current window's layout
        // and check if the click landed on it. The [X] text is drawn
        // at (origin_x + width - 32, ...) and is ~24×16 px.
        //
        // Two possible Y positions depending on the window type:
        //   a) Self-drawn title bar at g_win_y (title_h=36) -> Y: +4..+38
        //   b) Full-width panels (Settings/Music/Trash) with manual
        //      32px title bar at g_win_y -> Y: +4..+38 (same range
        //      since both types draw the title bar at g_win_y)
        // The compositor-style range (g_win_y-28 to g_win_y-4) is
        // kept for compatibility but currently unused.
        if (current_mode != MODE_DESKTOP) {
            // [X] close — hit-tested by the native window manager
            // (single source of truth for the button geometry).
            // The app's draw just set g_win_* live in THIS frame, so
            // sync them into the WM table first — otherwise the first
            // click after opening an app would test against a stale
            // rect from the previous frame and miss the button.
            if (g_win_x >= 0 && g_win_w > 0)
                win_sync_rect(current_mode, g_win_x, g_win_y, g_win_w, g_win_h);
            int close_mode = win_close_btn_hit(clk_x, clk_y);
            if (close_mode >= 0) {
                if (close_mode == MODE_VIDEO)
                    video_close();          // stop the sound
                // Windows-like: closing the focused window brings the
                // next open window to front (or the desktop if none).
                current_mode = win_close(close_mode);
                alt_tab_forget(close_mode); // drop from switcher MRU too
                redraw_content();
                cursor_bg_x = -1;
            } else if (!vesa.active) {
                // VGA text mode: [X] at column 74, row 2.
                // In real VGA text mode vesa.width is 0, so just
                // divide pixel coords by char cell size directly.
                int vcx = clk_x / 8;
                int vcy = clk_y / 16;
                if (vcx >= 73 && vcx <= 75 && vcy == 2) {
                    current_mode = MODE_DESKTOP;
                    redraw_content();
                    cursor_bg_x = -1;
                    goto after_click_dispatch;
                }
            }
        }

        // ── Clock tab clicks (MODE_CLOCK) ───────────────────────
        if (current_mode == MODE_CLOCK) {
            if (clock_handle_mouse(clk_x, clk_y)) {
                redraw_content();
                cursor_bg_x = -1;
                goto after_click_dispatch;
            }
        }

        // ── Calendar mouse clicks (MODE_CALENDAR) ───────────────
        if (current_mode == MODE_CALENDAR) {
            if (calendar_handle_mouse(clk_x, clk_y)) {
                redraw_content();
                cursor_bg_x = -1;
                goto after_click_dispatch;
            }
        }

        // ── Music tab clicks (MODE_MUSIC) ───────────────────────
        if (current_mode == MODE_MUSIC && vesa.active) {
            int wx = 16, wy = 40, ww = W - 32;
            int wh = H - 50, bh = 44;
            int ch = wh - bh;
            int hero_h = ch * 40 / 100;
            if (hero_h < 140) hero_h = 140;
            int py = wy + hero_h + 16;
            // Tab bar position (same as in draw_vesa_music_panel)
            int tab_y = py;
            int tab_h = 30;
            const char* mtabs[] = {"Playlist", "Albums"};
            int mtx = wx + 24;
            for (int i = 0; i < 2; i++) {
                int tw = str_len(mtabs[i]) * 8 + 24;
                if (clk_x >= mtx && clk_x < mtx + tw &&
                    clk_y >= tab_y && clk_y < tab_y + tab_h) {
                    if (music_tab != i) {
                        music_tab = i;
                        redraw_content();
                        cursor_bg_x = -1;
                        goto after_click_dispatch;
                    }
                }
                mtx += tw + 10;
            }

            // ── Music: status pill (play/pause) ─────────────────
            int cy = wy + bh;
            int art = hero_h - 40; if (art > 140) art = 140;
            int ax = wx + 24;
            int ay = cy + (hero_h - art) / 2;
            int tx = ax + art + 24;
            int ty = ay + 8;
            int sy2 = ty + 62;

            // Status pill (110x26) — play/pause toggle
            if (clk_y >= sy2 && clk_y < sy2 + 26 &&
                clk_x >= tx && clk_x < tx + 110) {
                if (music_is_playing()) {
                    music_pause();
                    desktop_set_status("Paused");
                } else {
                    music_play(music_current_song());
                    desktop_set_status("Playing");
                }
                redraw_content();
                cursor_bg_x = -1;
                goto after_click_dispatch;
            }

            // ── Music: song/album list row clicks ───────────────
            py = wy + hero_h + 16;
            tab_y = py; int tab_h2 = 30;
            int content_y = tab_y + tab_h2 + 14;
            if (music_tab == 0) {  // Playlist view
                int list_y = content_y;
                int row_h2 = 38;
                for (int i = 0; i < playlist_count; i++) {
                    int ry = list_y + i * row_h2;
                    if (clk_x >= wx && clk_x < wx + ww &&
                        clk_y >= ry && clk_y < ry + row_h2) {
                        music_play(i);
                        desktop_set_status("Playing");
                        redraw_content();
                        cursor_bg_x = -1;
                        goto after_click_dispatch;
                    }
                }
            } else {  // Albums view
                int list_y = content_y;
                int row_h2 = 38;
                for (int i = 0; i < playlist_count; i++) {
                    int ry = list_y + i * row_h2;
                    if (clk_x >= wx && clk_x < wx + ww &&
                        clk_y >= ry && clk_y < ry + row_h2) {
                        music_play(i);
                        desktop_set_status("Playing");
                        redraw_content();
                        cursor_bg_x = -1;
                        goto after_click_dispatch;
                    }
                }
            }
        }

        // ── Files — click sidebar nav items + This PC tiles ──
        if (current_mode == MODE_FILES && vesa.active && viewing_file < 0) {
            int fwin_x = (g_win_x >= 0) ? g_win_x : W/10;
            int fwin_y = (g_win_y >= 0) ? g_win_y : 70;

            // Sidebar items (Quick access + Devices and drives)
            for (int i = 0; i < FSI_COUNT; i++) {
                int rx, ry, rw, rh;
                files_sb_item_rect(fwin_x, fwin_y, i, &rx, &ry, &rw, &rh);
                if (clk_x >= rx && clk_x < rx + rw && clk_y >= ry && clk_y < ry + rh) {
                    files_sb_nav(i);
                    goto after_click_dispatch;
                }
            }

            // This PC home tiles (Folders grid + Local Disk tile)
            if (files_home_view) {
                static const int tile_items[6] = {FSI_DESKTOP, FSI_DOCS, FSI_DL, FSI_MUSIC, FSI_PICS, FSI_DRIVE};
                for (int i = 0; i < 5; i++) {
                    int rx, ry;
                    files_home_tile_rect(fwin_x, fwin_y, i, &rx, &ry);
                    if (clk_x >= rx && clk_x < rx + FILES_TILE_W && clk_y >= ry && clk_y < ry + FILES_TILE_H) {
                        selected_file = i;
                        files_sb_nav(tile_items[i]);
                        goto after_click_dispatch;
                    }
                }
                int rx, ry;
                files_home_drive_rect(fwin_x, fwin_y, &rx, &ry);
                if (clk_x >= rx && clk_x < rx + FILES_TILE_W && clk_y >= ry && clk_y < ry + FILES_TILE_H) {
                    selected_file = 5;
                    files_sb_nav(FSI_DRIVE);
                    goto after_click_dispatch;
                }
            }
        }

        // ── Files — click file list rows (MODE_FILES) ───────────
        if (current_mode == MODE_FILES && vesa.active && viewing_file < 0 && !files_home_view) {
            int W = (int)vesa.width;
            int win_x = (g_win_x >= 0) ? g_win_x : W/10;
            int win_y = (g_win_y >= 0) ? g_win_y : 70;
            int win_w = W*8/10;
            int content_left = win_x + FILES_SB_W + 15;  // after sidebar
            int py = files_rows_top(win_y);  // matches draw_files_window()
            int row_h = 26, gap = 4;

            int nrows = files_visible_count();
            int fvis = files_visible_rows(win_y, (int)vesa.height - 160);
            if (files_scroll > nrows - fvis) files_scroll = (nrows > fvis) ? nrows - fvis : 0;
            if (files_scroll < 0) files_scroll = 0;
            for (int r = files_scroll; r < nrows && r < files_scroll + fvis; r++) {
                int fi; files_visible_row(r, &fi);
                int ry = py + (r - files_scroll) * (row_h + gap);
                if (clk_x >= content_left - 8 && clk_x < win_x + win_w - 16 &&
                    clk_y >= ry && clk_y < ry + row_h) {
                    selected_file = r;
                    if (fi < 0) { files_go_up(); return; }
                    files_open_file(fi);
                    return;
                }
            }

            // ── Files: click sysdir folder items in main content ──
            if (!files_desktop_view && !files_cur_path[0]) {
                int base_py = win_y + 54;
                static const char* fs_sysdirs[] = {"boot","kernel","cpu","include"};
                for (int i = 0; i < 4; i++) {
                    int ry = base_py + i * 24;
                    if (clk_x >= content_left && clk_x < content_left + win_w &&
                        clk_y >= ry && clk_y < ry + 24) {
                        desktop_set_status(fs_sysdirs[i]);
                        redraw_content();
                        cursor_bg_x = -1;
                        goto after_click_dispatch;
                    }
                }
            }
        }

        // ── Settings sidebar nav clicks (MODE_SETTINGS) ─────────
        if (current_mode == MODE_SETTINGS && vesa.active) {
            int swx = 16, swy = 40;       // sidebar origin (same as draw_vesa_settings_panel)
            int sw  = 260;                 // sidebar width
            int ip  = 6;                   // vertical padding
            int ih  = 56;                  // item height
            int n   = 8;                   // 8 nav items
            int side_y = swy + ip;
            if (clk_x >= swx && clk_x < swx + sw &&
                clk_y >= side_y && clk_y < side_y + n * ih) {
                int idx = (clk_y - side_y) / ih;
                if (idx >= 0 && idx < n) {
                    settings_section = idx;
                    // Reset sub-selection for the newly opened section
                    if (idx == 0) settings_selected = current_timezone;
                    else if (idx == 6) settings_selected = current_wallpaper;
                    else if (idx == 7) settings_selected = current_theme;
                    else settings_selected = 0;
                    settings_scroll = 0;
                    redraw_content();
                    cursor_bg_x = -1;
                    goto after_click_dispatch;
                }
            }
        }

        // ── Settings content area clicks (MODE_SETTINGS) ────────
        // When a settings section is open, route clicks to the
        // appropriate section handler (timezone list, theme list,
        // body color swatches, etc.).
        if (current_mode == MODE_SETTINGS && vesa.active && settings_section >= 0) {
            int W = (int)vesa.width, H = (int)vesa.height;
            int sw = 260, bh = 30;
            int cx = 16 + sw + 1;
            int cy = 40 + TB_H;  // TB_H is the file-scope define (32)
            int cw = (W - 32) - sw - 1;
            int ch = (H - 50) - bh - TB_H;

            // ── Date & Time section (section 0): timezone list ───
            if (settings_section == 0 && clk_x >= cx && clk_x < cx + cw) {
                int y = cy + 42;
                int ly = y + 115;
                int list_y = ly + 35;
                int row_h = 34;
                int avail = (cy + ch) - list_y;
                int visible = avail / row_h;
                if (visible < 2) visible = 2;
                if (visible > 12) visible = 12;
                int start = settings_scroll;
                for (int i = start; i < timezone_count && i < start + visible; i++) {
                    int ry = list_y + (i - start) * row_h;
                    if (clk_y >= ry && clk_y < ry + row_h) {
                        settings_selected = i;
                        current_timezone = i;
                        settings_scroll = i > 9 ? i - 9 : 0;
                        redraw_content();
                        cursor_bg_x = -1;
                        goto after_click_dispatch;
                    }
                }
            }

            // ── Display section (section 1): resolution list ────
            if (settings_section == 1 && clk_x >= cx && clk_x < cx + cw) {
                int y = cy + 42;
                int list_y = y + 140;
                int row_h = 24;
                int max_visible = (cy + ch - list_y - 40) / row_h;
                if (max_visible > vbe_mode_count) max_visible = vbe_mode_count;
                for (int i = 0; i < max_visible; i++) {
                    int ry = list_y + 24 + i * row_h;
                    if (clk_y >= ry && clk_y < ry + row_h) {
                        settings_selected = i;
                        redraw_content();
                        cursor_bg_x = -1;
                        goto after_click_dispatch;
                    }
                }
            }

            // ── Sound section (section 2): volume bar ───────────
            if (settings_section == 2 && clk_x >= cx && clk_x < cx + cw) {
                int y = cy + 60;
                int by = y + 30;
                int bar_x = cx + 16;
                int bar_w = cw - 32 - 60;
                int bar_h = 18;
                if (clk_y >= by && clk_y < by + bar_h &&
                    clk_x >= bar_x && clk_x < bar_x + bar_w) {
                    int new_vol = (clk_x - bar_x) * 100 / bar_w;
                    if (new_vol < 0) new_vol = 0;
                    if (new_vol > 100) new_vol = 100;
                    sound_volume = new_vol;
                    redraw_content();
                    cursor_bg_x = -1;
                    goto after_click_dispatch;
                }
            }

            // ── System Info section (section 4): reset icon layout ─
            if (settings_section == 4 && clk_x >= cx && clk_x < cx + cw) {
                int y = cy + 42;  // match sp_content_system's y
                int bx = cx + 16, by = y + 245, bw = 220, bh = 30;
                if (clk_x >= bx && clk_x < bx + bw &&
                    clk_y >= by && clk_y < by + bh) {
                    // Reset to identity layout
                    for (int i = 0; i < DESKTOP_APP_COUNT; i++)
                        icon_order[i] = i;
                    for (int i = DESKTOP_APP_COUNT; i < g_grid_slots; i++)
                        icon_order[i] = -1;
                    for (int i = g_grid_slots; i < ICON_GRID_SLOTS; i++)
                        icon_order[i] = -1;
                    icon_layout_save();
                    desktop_set_status("Icon layout reset to default");
                    redraw_content();
                    cursor_bg_x = -1;
                    goto after_click_dispatch;
                }
            }

            // ── Wallpaper section (section 6): dim slider + list ─
            if (settings_section == 6 && clk_x >= cx && clk_x < cx + cw) {
                int y = cy + 42;
                int dim_y = y + 30;
                int track_x = cx + 140, track_y = dim_y + 4;
                int track_w = 180, track_h = 8;
                // Dim slider track
                if (clk_y >= track_y - 4 && clk_y < track_y + track_h + 4 &&
                    clk_x >= track_x && clk_x < track_x + track_w) {
                    int new_dim = (clk_x - track_x) * 100 / track_w;
                    if (new_dim < 0) new_dim = 0;
                    if (new_dim > 100) new_dim = 100;
                    wallpaper_set_dim(new_dim);
                    redraw_content();
                    cursor_bg_x = -1;
                    goto after_click_dispatch;
                }
                // Wallpaper list rows (built-in)
                int list_y = dim_y + 36;
                int row_h = 52;
                for (int i = 0; i < WALLPAPER_COUNT; i++) {
                    int ry = list_y + i * row_h;
                    if (clk_y >= ry && clk_y < ry + row_h) {
                        settings_selected = i;
                        wallpaper_set_custom(-1);
                        current_wallpaper = i;
                        desktop_set_status("Wallpaper changed!");
                        redraw_content();
                        cursor_bg_x = -1;
                        goto after_click_dispatch;
                    }
                }
                // Filesystem image wallpapers (count locally)
                int wf_count = 0;
                for (int fi = 0; fi < MAX_FILES; fi++) {
                    if (!filesystem[fi].used) continue;
                    const char *fn = filesystem[fi].name;
                    int nl = str_len(fn);
                    int is_img = 0;
                    if (nl > 4) {
                        const char *ext = fn + nl - 4;
                        if ((ext[0]=='p'||ext[0]=='P')&&(ext[1]=='n'||ext[1]=='N')&&(ext[2]=='g'||ext[2]=='G')) is_img=1;
                        else if ((ext[0]=='j'||ext[0]=='J')&&(ext[1]=='p'||ext[1]=='P')&&(ext[2]=='e'||ext[2]=='E')&&(ext[3]=='g'||ext[3]=='G')) is_img=1;
                    }
                    if (!is_img && nl > 4) {
                        const char *ext = fn + nl - 4;
                        if ((ext[0]=='b'||ext[0]=='B')&&(ext[1]=='m'||ext[1]=='M')&&(ext[2]=='p'||ext[2]=='P')) is_img=1;
                        if ((ext[0]=='g'||ext[0]=='G')&&(ext[1]=='i'||ext[1]=='I')&&(ext[2]=='f'||ext[2]=='F')) is_img=1;
                    }
                    if (!is_img && nl > 4) {
                        const char *ext = fn + nl - 4;
                        if (ext[0]=='J'&&ext[1]=='P'&&ext[2]=='E'&&ext[3]=='G') is_img=1;
                        if (ext[0]=='j'&&ext[1]=='p'&&ext[2]=='g') is_img=1;
                    }
                    if (is_img) {
                        int idx = WALLPAPER_COUNT + wf_count;
                        int ry = list_y + idx * row_h;
                        if (clk_y >= ry && clk_y < ry + row_h) {
                            settings_selected = idx;
                            wallpaper_set_custom(fi);
                            current_wallpaper = 0;
                            desktop_set_status("Wallpaper changed!");
                            redraw_content();
                            cursor_bg_x = -1;
                            goto after_click_dispatch;
                        }
                        wf_count++;
                    }
                }
            }

            // ── Theme section (section 7): theme list + swatches ─
            if (settings_section == 7 && clk_x >= cx && clk_x < cx + cw) {
                int y = cy + 42;
                int list_y = y + 32;
                int row_h = 50;

                // Theme list rows
                for (int i = 0; i < THEME_COUNT; i++) {
                    int ry = list_y + i * row_h;
                    if (clk_y >= ry && clk_y < ry + row_h) {
                        settings_selected = i;
                        current_theme = i;
                        theme_apply();
                        redraw_content();
                        cursor_bg_x = -1;
                        goto after_click_dispatch;
                    }
                }

                // Body color swatches (if compositor enabled)
                if (compositor_is_enabled()) {
                    int by = list_y + THEME_COUNT * row_h + 12;
                    by += 42 + 24;  // sp_heading + text line
                    int swatch_w = 50, swatch_h = 34, gap = 8;
                    for (int i = 0; i < 8; i++) {
                        int sx = cx + 16 + i * (swatch_w + gap);
                        int sy = by;
                        if (clk_x >= sx && clk_x < sx + swatch_w &&
                            clk_y >= sy && clk_y < sy + swatch_h) {
                            if (active_surface >= 0) {
                                compositor_surface_set_body_color(active_surface, body_pal[i]);
                            }
                            redraw_content();
                            cursor_bg_x = -1;
                            goto after_click_dispatch;
                        }
                    }
                }
            }

            // ── Users section (section 5): user list + actions ───
            if (settings_section == 5 && users_sub == 0 &&
                clk_x >= cx && clk_x < cx + cw) {
                int y = cy + 42;
                // Hit-test user rows
                int row = 0;
                for (int i = 0; i < MAX_USERS; i++) {
                    if (!users[i].active) continue;
                    int ry = y + row * 52;
                    if (clk_y >= ry && clk_y < ry + 50) {
                        users_selected = i;
                        redraw_content();
                        cursor_bg_x = -1;
                        goto after_click_dispatch;
                    }
                    row++;
                }
                // Hit-test action hints area
                int hy = y + row * 52 + 14;
                if (clk_y >= hy && clk_y < hy + 20) {
                    // "A  add user"
                    if (user_count < MAX_USERS) {
                        users_sub = 1;
                        u_clear_form();
                    } else {
                        str_copy(u_msg, "Max users!", 48);
                    }
                    redraw_content();
                    cursor_bg_x = -1;
                    goto after_click_dispatch;
                }
                if (clk_y >= hy + 20 && clk_y < hy + 40) {
                    // "C  change password"
                    users_sub = 2;
                    u_clear_form();
                    redraw_content();
                    cursor_bg_x = -1;
                    goto after_click_dispatch;
                }
                if (clk_y >= hy + 40 && clk_y < hy + 60) {
                    // "D  delete user"
                    if (users_selected == 0)
                        str_copy(u_msg, "Cannot delete admin!", 48);
                    else if (users_selected == logged_in_user)
                        str_copy(u_msg, "Cannot delete yourself!", 48);
                    else {
                        auth_delete_user(users_selected);
                        if (users_selected >= user_count)
                            users_selected = user_count - 1;
                        str_copy(u_msg, "Deleted!", 48);
                    }
                    redraw_content();
                    cursor_bg_x = -1;
                    goto after_click_dispatch;
                }
            }
        }

        // ── Desktop icon clicks (single-click = select, double-click = launch) ─
        if (current_mode == MODE_DESKTOP) {
            IconGrid ig = compute_icon_grid(W, H);

            // ── Desktop file/folder icon clicks ─────────────────
            // Files created via the right-click menu (New Folder /
            // New File) sit in empty grid slots.  Single click
            // selects, double-click opens via files_open_file().
            int dff = desktop_file_hit_test(clk_x, clk_y);
            if (dff >= 0) {
                for (int si = 0; si < g_grid_slots; si++) box_sel[si] = 0;
                if (rapid_clicks >= 2 ||
                    (dff == last_click_file &&
                     (tick - last_click_file_tick) < DBL_CLICK_THRESHOLD)) {
                    last_click_file = -1;
                    files_open_file(dff);
                } else {
                    desktop_file_selected = dff;
                    last_click_file = dff;
                    last_click_file_tick = tick;
                }
                goto after_click_dispatch;
            }
            last_click_file = -1;

            int clicked_icon = -1;
            clicked_icon = icon_grid_hit_test(&ig, clk_x, clk_y, -1);

            // Any plain click (on an icon or on empty space) replaces
            // the rubber-band multi-selection with a single selection
            // — UNLESS the clicked icon is already part of the group
            // selection.  Clicking a selected icon keeps the group so
            // it can be dragged as a unit; the press-click fires here
            // before a slow drag even crosses the threshold, so
            // clearing box_sel now would break group detection in the
            // IRQ (desktop_sel_has returns 0 → falls back to a
            // single-icon drag).
            if (clicked_icon < 0 || !box_sel[clicked_icon]) {
                for (int si = 0; si < g_grid_slots; si++) box_sel[si] = 0;
            }

            if (clicked_icon >= 0) {
                // ── Same-heartbeat double-click detection ────────────
                // If two clicks happened before the heartbeat could
                // process the first one, rapid_clicks >= 2 (captured
                // right after mouse_get_click above). Treat this as
                // an immediate double-click and launch the app.
                // The cross-heartbeat timer below handles the case
                // where clicks span separate heartbeats.
                if (rapid_clicks >= 2) {
                    int app_idx = icon_order[clicked_icon];
                    if (app_idx >= 0) {
                        last_click_icon = -1;
                        do_launch(app_idx);
                    }
                } else if (mouse.left) {
                    // Button still held — the heartbeat caught us mid-
                    // press.  If this is the same icon as the last click
                    // and within the threshold, launch immediately —
                    // unless the press is on a box-selected group member
                    // (that's a group drag in progress — never launch
                    // mid-press, just re-select the group).
                    // Otherwise, arm the timer so the NEXT click on the
                    // same icon launches.  If this turns out to be a drag,
                    // the drag source detection runs separately above.
                    if (!(box_sel[clicked_icon] && desktop_sel_count() > 1) &&
                        clicked_icon == last_click_icon &&
                        (tick - last_click_tick) < DBL_CLICK_THRESHOLD) {
                        int app_idx = icon_order[clicked_icon];
                        if (app_idx >= 0) {
                            last_click_icon = -1;
                            do_launch(app_idx);
                        }
                    } else {
                        selected_icon = clicked_icon;
                        last_click_icon = clicked_icon;
                        last_click_tick = tick;
                    }
                } else if (clicked_icon == last_click_icon &&
                           (tick - last_click_tick) < DBL_CLICK_THRESHOLD) {
                    // Cross-heartbeat double-click (release caught by
                    // heartbeat before next press) — launch the app
                    int app_idx = icon_order[clicked_icon];
                    if (app_idx >= 0) {
                        last_click_icon = -1;
                        do_launch(app_idx);
                    }
                } else {
                    // Single click — just select and arm the double-click timer
                    selected_icon = clicked_icon;
                    last_click_icon = clicked_icon;
                    last_click_tick = tick;
                }
            } else {
                // Clicked empty space — reset double-click tracking
                mouse.click_count = 0;
                last_click_icon = -1;
            }
        }

        // ── Calculator button clicks (MODE_CALC) ─────────────────
        if (current_mode == MODE_CALC && vesa.active) {
            if (calc_handle_mouse(clk_x, clk_y)) {
                redraw_content();
                cursor_bg_x = -1;
                goto after_click_dispatch;
            }
        }

        // ── Gallery thumbnail clicks (MODE_GALLERY) ─────────────
        if (current_mode == MODE_GALLERY && vesa.active) {
            if (gallery_handle_mouse(clk_x, clk_y)) {
                // gallery_handle_mouse already sets current_mode to
                // MODE_IMGVIEW if a valid image was clicked. Our
                // caller will redraw the new mode.
                redraw_content();
                cursor_bg_x = -1;
                goto after_click_dispatch;
            }
        }

        // ── Paint canvas + toolbar clicks (MODE_PAINT) ──────────
        if (current_mode == MODE_PAINT && vesa.active) {
            if (paint_handle_mouse(clk_x, clk_y)) {
                redraw_content();
                cursor_bg_x = -1;
                goto after_click_dispatch;
            }
        }

        // ── Organizer folder list + app grid clicks (MODE_ORGANIZER) ─
        if (current_mode == MODE_ORGANIZER && vesa.active) {
            if (organizer_handle_mouse(clk_x, clk_y)) {
                redraw_content();
                cursor_bg_x = -1;
                goto after_click_dispatch;
            }
        }

        // ── Security app tab + content clicks (MODE_SECURITY) ────
        if (current_mode == MODE_SECURITY && vesa.active) {
            if (security_handle_mouse(clk_x, clk_y)) {
                redraw_content();
                cursor_bg_x = -1;
                goto after_click_dispatch;
            }
        }

        // ── App Library grid + detail view clicks (MODE_APPLIB) ──
        if (current_mode == MODE_APPLIB && vesa.active) {
            if (applib_handle_mouse(clk_x, clk_y)) {
                redraw_content();
                cursor_bg_x = -1;
                goto after_click_dispatch;
            }
        }



        // ── Games card clicks + Minesweeper grid (MODE_GAMES) ────
        if (current_mode == MODE_GAMES) {
            if (games_handle_mouse(clk_x, clk_y)) {
                redraw_content();
                cursor_bg_x = -1;
                goto after_click_dispatch;
            }
        }

        // ── Sysmon tab clicks + process list (MODE_SYSMON) ─────
        if (current_mode == MODE_SYSMON && vesa.active) {
            if (sysmon_handle_mouse(clk_x, clk_y)) {
                redraw_content();
                cursor_bg_x = -1;
                goto after_click_dispatch;
            }
        }

        // ── Weather forecast day clicks (MODE_WEATHER) ──────────
        if (current_mode == MODE_WEATHER && vesa.active) {
            if (weather_handle_mouse(clk_x, clk_y)) {
                redraw_content();
                cursor_bg_x = -1;
                goto after_click_dispatch;
            }
        }

        // ── Terminal mouse clicks (MODE_TERMINAL) ────────────────
        if (current_mode == MODE_TERMINAL && vesa.active) {
            int W3 = (int)vesa.width, H3 = (int)vesa.height;
            int tx = (g_win_x >= 0) ? g_win_x : W3 / 10;
            int ty = (g_win_y >= 0) ? g_win_y : 70;
            int tw = W3 * 8 / 10, th = H3 - 160;
            if (clk_x >= tx && clk_x < tx + tw && clk_y >= ty && clk_y < ty + th) {
                // Click within terminal window — acknowledge
                redraw_content();
                cursor_bg_x = -1;
                goto after_click_dispatch;
            }
        }

        // ── QuickSettings mouse clicks (MODE_QUICKSETTINGS) ─────
        if (current_mode == MODE_QUICKSETTINGS && vesa.active) {
            QsLayout L; qs_layout(&L);
            int fx, fy, fw, fh; qs_wifi_flyout_rect(&fx, &fy, &fw, &fh);

            // ── Wi-Fi flyout interaction ─────────────────────────
            if (qs_wifi_open) {
                int in_fly = (clk_x >= fx && clk_x < fx + fw &&
                              clk_y >= fy && clk_y < fy + fh);
                if (in_fly) {
                    // Header toggle switch
                    if (clk_x >= fx + fw - 62 && clk_x < fx + fw - 18 &&
                        clk_y >= fy + 13 && clk_y < fy + 35) {
                        qs_wifi_on = !qs_wifi_on;
                        qs_wifi_scan = qs_wifi_on ? 60 : 0;
                        desktop_set_status(qs_wifi_on ? "Wi-Fi on" : "Wi-Fi off");
                        redraw_content();
                        cursor_bg_x = -1;
                        goto after_click_dispatch;
                    }
                    // Network rows (visible once the scan completes)
                    if (qs_wifi_scan == 0 && qs_wifi_on) {
                        for (int i = 0; i < QS_WIFI_NETS; i++) {
                            int rx = fx + 12, ry = fy + 58 + i * 50;
                            int rw = fw - 24, rh = 46;
                            if (clk_x >= rx && clk_x < rx + rw &&
                                clk_y >= ry && clk_y < ry + rh) {
                                if (i != qs_wifi_cur) {
                                    qs_wifi_cur = i;
                                    desktop_set_status("Connected to Wi-Fi");
                                }
                                redraw_content();
                                cursor_bg_x = -1;
                                goto after_click_dispatch;
                            }
                        }
                    }
                    // Click inside the flyout on empty space — consume
                    redraw_content();
                    cursor_bg_x = -1;
                    goto after_click_dispatch;
                }
                // Click outside the flyout: close it and keep the
                // panel — except on the Wi-Fi chevron, which toggles
                // the flyout (handled in the grid below).
                int bx0 = L.grid_x0, by0 = L.grid_y0;
                int on_chev = (clk_x >= bx0 + L.btn_w - 26 &&
                               clk_x < bx0 + L.btn_w &&
                               clk_y >= by0 && clk_y < by0 + L.btn_h);
                if (!on_chev) {
                    qs_wifi_open = 0;
                    // The frame was already drawn with the flyout
                    // open (clicks dispatch AFTER the draw), and this
                    // close doesn't change current_mode, so no
                    // mode-change redraw fires — without this the
                    // flyout's pixels stay in the back buffer forever.
                    redraw_content();
                    cursor_bg_x = -1;
                    goto after_click_dispatch;
                }
            }

            // Click outside the panel closes it (flyout behavior) —
            // but only if the panel was already open BEFORE this
            // dispatch.  The click that just launched the panel (the
            // cursor is still on the desktop icon) must not close it.
            if (dispatch_mode_before == MODE_QUICKSETTINGS &&
                (clk_x < L.wx || clk_x >= L.wx + L.ww ||
                 clk_y < L.wy || clk_y >= L.wy + L.wh)) {
                current_mode = MODE_DESKTOP;
                redraw_content();
                goto after_click_dispatch;
            }

            // Top-row circle buttons (right→left: power, lock, settings, screenshot)
            int right = L.wx + L.ww - 16 - L.circ_r;
            for (int i = 0; i < 4; i++) {
                int cx = right - i * (L.circ_d + 10);
                int cy = L.circ_y + L.circ_r;
                int dx = clk_x - cx, dy = clk_y - cy;
                if (dx*dx + dy*dy <= L.circ_r * L.circ_r) {
                    switch (i) {
                        case 0:  // power
                            current_mode = MODE_DESKTOP;
                            power_shutdown();
                            break;
                        case 1:  // lock
                            current_mode = MODE_DESKTOP;
                            desktop_set_status("Locked");
                            break;
                        case 2:  // settings
                            current_mode = MODE_SETTINGS; settings_section = 0;
                            settings_selected = current_timezone;
                            break;
                        case 3:  // screenshot
                            qs_take_screenshot();
                            break;
                    }
                    redraw_content();
                    goto after_click_dispatch;
                }
            }

            // Volume slider track
            if (clk_y >= L.vol_y && clk_y < L.vol_y + 42 &&
                clk_x >= L.track_x && clk_x < L.track_x + L.track_w) {
                int v = (clk_x - L.track_x) * 100 / L.track_w;
                if (v < 0) v = 0;
                if (v > 100) v = 100;
                sound_volume = v;
                redraw_content();
                goto after_click_dispatch;
            }

            // Brightness slider track
            if (clk_y >= L.bri_y && clk_y < L.bri_y + 42 &&
                clk_x >= L.track_x && clk_x < L.track_x + L.track_w) {
                int v = (clk_x - L.track_x) * 100 / L.track_w;
                if (v < 0) v = 0;
                if (v > 100) v = 100;
                qs_brightness = v;
                wallpaper_set_brightness(v);
                redraw_content();
                goto after_click_dispatch;
            }

            // Quick-action grid (3 rows × 2 cols)
            for (int gi = 0; gi < 6; gi++) {
                int col = gi % 2, row = gi / 2;
                int bx = L.grid_x0 + col * (L.btn_w + L.grid_gap);
                int by = L.grid_y0 + row * (L.btn_h + L.grid_gap);
                if (clk_x >= bx && clk_x < bx + L.btn_w &&
                    clk_y >= by && clk_y < by + L.btn_h) {
                    switch (gi) {
                        case 0:  // Wi-Fi
                            if (clk_x >= bx + L.btn_w - 26) {
                                // Chevron: toggle the network picker
                                if (qs_wifi_open) {
                                    qs_wifi_open = 0;
                                    desktop_set_status("");
                                } else {
                                    qs_wifi_open = 1;
                                    qs_wifi_scan = qs_wifi_on ? 60 : 0;
                                    desktop_set_status("Scanning for networks...");
                                }
                            } else if (nic_is_up()) {
                                current_mode = MODE_SETTINGS; settings_section = 3;
                            } else {
                                desktop_set_status("No network adapter");
                            }
                            break;
                        case 1:  // Bluetooth
                            qs_bluetooth = !qs_bluetooth;
                            desktop_set_status(qs_bluetooth ? "Bluetooth on" : "Bluetooth off");
                            break;
                        case 2:  // Power Mode
                            qs_power_mode = (qs_power_mode + 1) % 3;
                            desktop_set_status(qs_power_mode == 0 ? "Power mode: Balanced" :
                                               qs_power_mode == 1 ? "Power mode: Performance" :
                                                                   "Power mode: Eco");
                            break;
                        case 3:  // Night Light
                            qs_nightlight = !qs_nightlight;
                            desktop_set_status(qs_nightlight ? "Night light on" : "Night light off");
                            break;
                        case 4:  // Dark Style
                            current_theme = (current_theme == 1) ? 2 : 1;
                            theme_apply();
                            desktop_set_status(current_theme == 1 ? "Dark style" : "Light style");
                            break;
                        case 5:  // Airplane Mode
                            qs_airplane = !qs_airplane;
                            desktop_set_status(qs_airplane ? "Airplane mode on" : "Airplane mode off");
                            break;
                    }
                    redraw_content();
                    goto after_click_dispatch;
                }
            }
        }

        // ── Image Viewer mouse clicks (MODE_IMGVIEW) ─────────────
        if (current_mode == MODE_IMGVIEW) {
            if (imgview_handle_mouse(clk_x, clk_y)) {
                redraw_content();
                cursor_bg_x = -1;
                goto after_click_dispatch;
            }
        }

        // ── Video player mouse clicks (MODE_VIDEO) ──────────────
        if (current_mode == MODE_VIDEO) {
            if (video_handle_mouse(clk_x, clk_y)) {
                redraw_content();
                cursor_bg_x = -1;
                goto after_click_dispatch;
            }
        }

        // ── Editor mouse clicks (MODE_EDITOR) ────────────────────
        if (current_mode == MODE_EDITOR && vesa.active) {
            if (editor_handle_mouse(clk_x, clk_y)) {
                redraw_content();
                cursor_bg_x = -1;
                goto after_click_dispatch;
            }
        }

        // ── Browser mouse clicks (MODE_BROWSER) ──────────────────
        if (current_mode == MODE_BROWSER && vesa.active) {
            if (browser_handle_mouse(clk_x, clk_y)) {
                redraw_content();
                cursor_bg_x = -1;
                goto after_click_dispatch;
            }
        }

        // ── PdfView mouse clicks (MODE_PDFVIEW) ──────────────────
        if (current_mode == MODE_PDFVIEW && vesa.active) {
            if (pdfview_handle_mouse(clk_x, clk_y)) {
                redraw_content();
                cursor_bg_x = -1;
                goto after_click_dispatch;
            }
        }

        // ── Trash mouse clicks (MODE_TRASH) ──────────────────────
        if (current_mode == MODE_TRASH && vesa.active) {
            int wx = g_win_x, wy = g_win_y, ww = g_win_w, wh = g_win_h;
            if (wx < 0) { wx = 16; wy = 40; ww = (int)vesa.width - 32; wh = (int)vesa.height - 50; }

            // ── Title bar [X] close button — handled globally

            // ── Trash item list rows ──────────────────────────────
            int bh = 30;
            int cy = wy + TB_H;
            int ch = wh - bh - TB_H;

            if (trash_count > 0) {
                // Count used trash slots for bounds checking
                int n = 0;
                for (int i = 0; i < MAX_TRASH; i++) if (trash[i].used) n++;
                if (trash_selected >= n) trash_selected = n > 0 ? n - 1 : 0;
                if (n == 0) { /* should not happen since trash_count > 0 */ }

                int row_h = 40;
                int list_y = cy + 36;
                int avail = (cy + ch) - list_y - 8;
                int visible = avail / row_h; if (visible < 1) visible = 1;
                int scroll = 0;
                if (trash_selected >= visible) scroll = trash_selected - visible + 1;

                // Click on item rows
                for (int row = 0; row < visible; row++) {
                    int li = row + scroll;
                    if (li >= n) break;
                    int ry = list_y + row * row_h;
                    if (clk_x >= wx && clk_x < wx + ww &&
                        clk_y >= ry && clk_y < ry + row_h - 2) {
                        trash_selected = li;
                        redraw_content();
                        cursor_bg_x = -1;
                        goto after_click_dispatch;
                    }
                }
            }

            // ── Bottom hint bar: click on action hints ────────────
            int by = wy + wh - bh;
            if (trash_count > 0 && clk_y >= by && clk_y < by + bh) {
                // Guess action by horizontal position of hint text:
                // "R restore" starts at wx+16, "X empty" further right
                if (clk_x >= wx + 16 && clk_x < wx + 70) {
                    // Restore selected
                    if (trash_count > 0) {
                        int idxs2[MAX_TRASH]; int n2 = 0;
                        for (int i = 0; i < MAX_TRASH; i++) if (trash[i].used) idxs2[n2++] = i;
                        if (trash_selected < n2) {
                            restore_from_trash(idxs2[trash_selected]);
                            if (trash_selected > 0) trash_selected--;
                            desktop_set_status("Restored!");
                        }
                    }
                    redraw_content();
                    cursor_bg_x = -1;
                    goto after_click_dispatch;
                }
                if (clk_x >= wx + 140 && clk_x < wx + 200) {
                    // Empty all
                    empty_trash();
                    trash_selected = 0;
                    desktop_set_status("Trash emptied!");
                    redraw_content();
                    cursor_bg_x = -1;
                    goto after_click_dispatch;
                }
                if (clk_x >= wx + 220 && clk_x < wx + 310) {
                    // Delete one (DEL)
                    if (trash_count > 0) {
                        int idxs3[MAX_TRASH]; int n3 = 0;
                        for (int i = 0; i < MAX_TRASH; i++) if (trash[i].used) idxs3[n3++] = i;
                        if (trash_selected < n3) {
                            delete_one_from_trash(idxs3[trash_selected]);
                            if (trash_selected > 0) trash_selected--;
                            desktop_set_status("Deleted permanently!");
                        }
                    }
                    redraw_content();
                    cursor_bg_x = -1;
                    goto after_click_dispatch;
                }
            }
        }


    }

after_click_dispatch:
    // Close popups when clicking anywhere outside them
    if (vol_popup_open && !vol_popup_closing && !vol_popup_just_opened && __clk_this_frame) {
        int vx = vol_popup_x, vy = vol_popup_y;
        int vw = 200, vh = 56;
        int on_vol_popup = (clk_x >= vx && clk_x < vx + vw &&
                            clk_y >= vy && clk_y < vy + vh);
        int on_vol_icon  = (clk_x >= vol_popup_x && clk_x < vol_popup_x + 20 &&
                            clk_y >= 0 && clk_y < 28);
        if (!on_vol_popup && !on_vol_icon) vol_popup_closing = 1;
    }
    if (wifi_popup_open && !wifi_popup_closing && !wifi_popup_just_opened && __clk_this_frame) {
        int px = wifi_popup_x, py = wifi_popup_y;
        int rows = QS_WIFI_NETS;
        int fh = WIFI_POPUP_HEADER + rows * WIFI_POPUP_ROW_H + 12;
        int on_popup = (clk_x >= px && clk_x < px + WIFI_POPUP_W &&
                        clk_y >= py && clk_y < py + fh);
        int on_icon  = (clk_x >= wifi_popup_x && clk_x < wifi_popup_x + 20 &&
                        clk_y >= 0 && clk_y < 28);
        if (!on_popup && !on_icon) wifi_popup_closing = 1;
    }
    finish_frame();
    // Invalidate all IRQ-level dirty-rectangle buffers so the next
    // mouse packet (200 Hz) doesn't restore stale pre-redraw pixels
    // on top of the freshly drawn frame.
}

void desktop_update_clock(){
    // Status timer countdown (runs on every tick regardless of mode)
    if (status_timer > 0) {
        status_timer--;
        if (status_timer == 0) status_msg[0] = '\0';
    }
    // Also tick down the flash
    if (flash_frames > 0) flash_frames--;

    if(current_mode!=MODE_DESKTOP&&current_mode!=MODE_TERMINAL)return;
    // Skip VGA text redraw when VESA is active — draw_taskbar() already
    // renders the clock in the GNOME-style top bar via VESA pixels.
    // Writing VGA text here would paint over the taskbar and any open
    // volume/Wi-Fi popups on every tick.
    if(vesa.active) return;
    const Theme*t=TH;
    char ts[9];char ds[9];
    rtc_get_time_str(ts);rtc_get_date_str(ds);
    vga_puts(52,0,ds,t->taskbar_fg,    t->taskbar_bg);
    vga_put(60,0,' ',t->taskbar_fg,    t->taskbar_bg);
    vga_puts(61,0,ts,t->taskbar_accent,t->taskbar_bg);
}

// ─── Helpers ─────────────────────────────────────────────────────
static void move_to_trash(int file_idx){
    int slot=-1;
    for(int i=0;i<MAX_TRASH;i++)if(!trash[i].used){slot=i;break;}
    if(slot==-1)return;
    for(int i=0;i<MAX_FILENAME;i++)trash[slot].name[i]   =filesystem[file_idx].name[i];
    // Allocate content for trash slot and copy from filesystem
    file_content_alloc(slot, filesystem[file_idx].size > 0 ? filesystem[file_idx].size + 1 : 1);
    if(trash[slot].content && filesystem[file_idx].content) {
        for(int i=0;i<filesystem[file_idx].size && i<trash[slot].content_cap-1;i++)
            trash[slot].content[i]=filesystem[file_idx].content[i];
        if(filesystem[file_idx].size < trash[slot].content_cap)
            trash[slot].content[filesystem[file_idx].size]=0;
    }
    trash[slot].size = filesystem[file_idx].size;
    trash[slot].used=1;trash_count++;
    file_content_free(file_idx);
    filesystem[file_idx].used=0;file_count--;
    if(selected_file>0)selected_file--;
    // Deleting a folder also trashes everything inside it (entries whose
    // name starts with "Folder/") so no invisible orphans are left.
    {   int flen=0; while (filesystem[file_idx].name[flen]) flen++;
        if (flen>0 && filesystem[file_idx].name[flen-1]=='/') {
            for (int i=0;i<MAX_FILES;i++) {
                if (i==file_idx || !filesystem[i].used) continue;
                if (files_name_has_prefix(filesystem[i].name, filesystem[file_idx].name))
                    move_to_trash(i);
            }
        }
    }
    // NOTE: the file stays in desktop_files[] on purpose — the draw
    // loop skips entries whose filesystem[fi].used == 0, so the icon
    // is hidden while trashed and reappears if the user restores it
    // from the Trash app (restore_from_trash re-sets used=1).  Stale
    // indices are pruned by the Refresh action's revalidation.
}

// ── File open helper ───────────────────────────────────────────────
// Opens a file by its index in filesystem[]: auto-detects images
// (PNG/JPEG/BMP/GIF), PDFs (via companion .txt or C parser), and
// falls back to the raw text viewer. Used by both the keyboard Enter
// handler and the mouse click handler so opening a file always works
// the same way regardless of input method.
static void files_open_file(int file_idx) {
    // Folders (name ends with '/') are containers — open them: browse
    // their contents in the Files app.  This is the shared entry point
    // for desktop folder icons AND Files-app rows.
    int flen = 0; while (filesystem[file_idx].name[flen]) flen++;
    if (flen > 0 && filesystem[file_idx].name[flen-1] == '/') {
        files_enter_folder(file_idx);
        return;
    }
    const uint8_t* cont = (const uint8_t*)filesystem[file_idx].content;
    int clen = filesystem[file_idx].size;
    if (clen <= 0) { while (cont[clen]) clen++; }
    int iw = 0, ih = 0;
    if (img_get_dimensions(cont, clen, &iw, &ih) &&
        (int)(iw * ih * 4) <= FM_PIXBUF_SIZE) {
        if (clen >= 8 && cont[0]==0x89 && cont[1]=='P' && cont[2]=='N' && cont[3]=='G') {
            PngImage pim; pim.pixels = fm_pixbuf;
            if (png_decode(cont, clen, &pim) && pim.width>0 && pim.height>0) {
                imgview_init((int)pim.width, (int)pim.height, fm_pixbuf, filesystem[file_idx].name);
                imgview_return_mode = MODE_FILES;   // ESC returns to Files
                current_mode = MODE_IMGVIEW; desktop_redraw_no_click(); return;
            }
        } else if (clen >= 3 && cont[0]==0xFF && cont[1]==0xD8 && cont[2]==0xFF) {
            // JPEG opens decode on the background jpegdec task so a
            // large image can never freeze the UI (this function can
            // run inside the keyboard-IRQ chain, where a multi-second
            // decode would block input entirely).  desktop_async_pump()
            // switches to imgview once the worker task is done.
            if (!fm_async_open && jpeg_async_submit(cont, clen, fm_pixbuf, 0, 0)) {
                fm_async_open = 1;
                int n = 0;
                const char* src = filesystem[file_idx].name;
                while (src[n] && n < 63) { fm_async_name[n] = src[n]; n++; }
                fm_async_name[n] = 0;
                return;
            }
            // Worker busy (another open in flight) — ignore the click.
            return;
        } else if (clen >= 2 && cont[0]=='B' && cont[1]=='M') {
            BmpImage bim; bim.pixels = fm_pixbuf;
            if (bmp_decode(cont, clen, &bim) && bim.width>0 && bim.height>0) {
                imgview_init((int)bim.width, (int)bim.height, fm_pixbuf, filesystem[file_idx].name);
                imgview_return_mode = MODE_FILES;   // ESC returns to Files
                current_mode = MODE_IMGVIEW; desktop_redraw_no_click(); return;
            }
        } else if (clen >= 6 && cont[0]=='G' && cont[1]=='I' && cont[2]=='F') {
            GifImage gim; gim.pixels = fm_pixbuf;
            if (gif_decode(cont, clen, &gim) && gim.width>0 && gim.height>0) {
                imgview_init((int)gim.width, (int)gim.height, fm_pixbuf, filesystem[file_idx].name);
                imgview_return_mode = MODE_FILES;   // ESC returns to Files
                current_mode = MODE_IMGVIEW; desktop_redraw_no_click(); return;
            }
        } else if (clen >= 4 && cont[0]=='<' && (cont[1]=='s'||cont[1]=='S') &&
                   (cont[2]=='v'||cont[2]=='V') && (cont[3]=='g'||cont[3]=='G')) {
            SvgImage sim; sim.pixels = fm_pixbuf;
            if (svg_decode(cont, clen, &sim) && sim.width>0 && sim.height>0) {
                imgview_init((int)sim.width, (int)sim.height, fm_pixbuf, filesystem[file_idx].name);
                imgview_return_mode = MODE_FILES;   // ESC returns to Files
                current_mode = MODE_IMGVIEW; desktop_redraw_no_click(); return;
            }
        }
    }
    // Check if it's an MP4 video (ftyp box at offset 4)
    if (clen >= 12 && cont[4]=='f' && cont[5]=='t' && cont[6]=='y' && cont[7]=='p') {
        // Attach the audio sidecar: <basename>.pcm (8-bit unsigned mono
        // PCM at AUDIO_PCM_RATE, built by tools/transcode-videos.py).
        const uint8_t* pcm = 0;
        int plen = 0;
        char pcm_name[MAX_FILENAME];
        int ni = 0;
        while (filesystem[file_idx].name[ni] &&
               filesystem[file_idx].name[ni] != '.' && ni < MAX_FILENAME - 5) {
            pcm_name[ni] = filesystem[file_idx].name[ni];
            ni++;
        }
        pcm_name[ni++] = '.'; pcm_name[ni++] = 'p';
        pcm_name[ni++] = 'c'; pcm_name[ni++] = 'm';
        pcm_name[ni] = 0;
        for (int fi = 0; fi < MAX_FILES; fi++) {
            if (filesystem[fi].used && str_eq(filesystem[fi].name, pcm_name)) {
                pcm = (const uint8_t*)filesystem[fi].content;
                plen = filesystem[fi].size;
                break;
            }
        }
        video_init(cont, clen, filesystem[file_idx].name);
        if (video_has_mp3())
            video_set_audio_mp3();        // in-container MP3 track (best)
        else if (pcm && plen > 0)
            video_set_audio(pcm, plen, AUDIO_PCM_RATE);
        video_return_mode = MODE_FILES;   // ESC returns to Files
        current_mode = MODE_VIDEO; desktop_redraw_no_click(); return;
    }

    // Check if it's a PDF
    if (clen >= 4 && cont[0]=='%' && cont[1]=='P' && cont[2]=='D' && cont[3]=='F') {
        char txt_name[MAX_FILENAME];
        int ni = 0;
        while (filesystem[file_idx].name[ni] && filesystem[file_idx].name[ni] != '.' && ni < MAX_FILENAME - 5) {
            txt_name[ni] = filesystem[file_idx].name[ni];
            ni++;
        }
        txt_name[ni++] = '.';
        txt_name[ni++] = 't';
        txt_name[ni++] = 'x';
        txt_name[ni++] = 't';
        txt_name[ni] = '\0';
        for (int fi = 0; fi < MAX_FILES; fi++) {
            if (filesystem[fi].used && str_eq(filesystem[fi].name, txt_name)) {
                int txt_len = filesystem[fi].size;
                if (txt_len <= 0) { while (filesystem[fi].content[txt_len]) txt_len++; }
                if (txt_len > PDFVIEW_BUF_SIZE - 1) txt_len = PDFVIEW_BUF_SIZE - 1;
                for (int ci = 0; ci < txt_len; ci++)
                    pdfview_buf[ci] = filesystem[fi].content[ci];
                pdfview_buf[txt_len] = '\0';
                pdfview_init(pdfview_buf, txt_len, filesystem[file_idx].name, 0);
                pdfview_return_mode = MODE_FILES;   // ESC returns to Files
                current_mode = MODE_PDFVIEW; desktop_redraw_no_click(); return;
            }
        }
        int pflen = pdftext_extract((const char*)cont, clen, pdfview_buf, PDFVIEW_BUF_SIZE);
        if (pflen > 0) {
            int pc = pdftext_get_page_count((const char*)cont, clen);
            if (pc < 0) pc = 0;
            pdfview_init(pdfview_buf, pflen, filesystem[file_idx].name, pc);
            pdfview_return_mode = MODE_FILES;   // ESC returns to Files
            current_mode = MODE_PDFVIEW; desktop_redraw_no_click(); return;
        }
    }
    // Check if it's a PE executable (.exe / .dll with MZ header)
    if (clen >= 64 && cont[0]=='M' && cont[1]=='Z') {
        pe_dump_header(cont, clen);
        int pid = pe_exec(cont, clen, filesystem[file_idx].name, 0);
        if (pid >= 0) {
            terminal_print("PE: .exe loaded successfully\n");
            // DOOM.EXE (and any game PE) calls SYS_GAMESTART within
            // its first ticks and owns the screen from then on.  The
            // task shows its output on COM1 for diagnostics.
        } else {
            terminal_print("PE: failed to load .exe\n");
        }
        return;
    }
    // Not an image or PDF — open as editable text in the Editor app
    // so the user can type into the file and save (Ctrl+S / F2).
    // This also makes desktop-created "New File.txt" icons open in
    // an editor instead of the read-only viewer.  The Editor is
    // VESA-only, so VGA text mode keeps the read-only viewer (which
    // has a working text-mode path).
    if (vesa.active) {
        editor_open_file(file_idx);
        editor_return_files = 1;   // ESC in the editor returns to Files
        current_mode = MODE_EDITOR;
        desktop_redraw_no_click();
    } else {
        viewing_file = file_idx;
        desktop_redraw_no_click();
    }
}

// Completes an asynchronous JPEG open started by files_open_file().
// Runs in the kernel main loop — NEVER in the PIT IRQ or a key/mouse
// handler — so a large decode can't stall input.  If the user left
// Files before the decode finished, the result is discarded.
void desktop_async_pump(void) {
    if (!fm_async_open) return;
    jpeg_async_ensure_worker();          // lazily create the worker task
    if (!jpeg_async_done()) return;      // still decoding — poll next loop

    // Consume the result ATOMICALLY: this runs in main-loop context
    // with interrupts enabled, while jpeg_async_submit can fire inside
    // a key/mouse IRQ.  Without cli, an IRQ between the checks could
    // submit a second job into the SAME fm_pixbuf before we open it.
    int ok;
    uint32_t w, h;
    char name[64];
    asm volatile("cli");
    ok = jpeg_async_ok();
    w = jpeg_async_width();
    h = jpeg_async_height();
    int n = 0;
    while (fm_async_name[n] && n < 63) { name[n] = fm_async_name[n]; n++; }
    name[n] = 0;
    fm_async_open = 0;
    jpeg_async_reset();
    asm volatile("sti");

    if (current_mode != MODE_FILES) return;    // navigated away — discard
    if (!ok) return;                           // decode failed — stay in Files
    imgview_init((int)w, (int)h, fm_pixbuf, name);
    imgview_return_mode = MODE_FILES;          // ESC returns to Files
    current_mode = MODE_IMGVIEW;
    desktop_redraw_no_click();
}

static void restore_from_trash(int trash_idx){
    if (trash_idx<0 || trash_idx>=MAX_TRASH || !trash[trash_idx].used) return;
    // Snapshot the folder's name BEFORE the restore loop (the loop only
    // reads trash[], but this keeps the prefix stable for the child scan).
    char folder_name[MAX_FILENAME];
    for(int j=0;j<MAX_FILENAME;j++) folder_name[j]=trash[trash_idx].name[j];
    int flen=0; while (folder_name[flen]) flen++;
    int is_dir = (flen>0 && folder_name[flen-1]=='/');
    for(int i=0;i<MAX_FILES;i++){
        if(!filesystem[i].used){
            for(int j=0;j<MAX_FILENAME;j++)filesystem[i].name[j]   =trash[trash_idx].name[j];
            // Allocate content and copy from trash
            file_content_alloc(i, trash[trash_idx].size > 0 ? trash[trash_idx].size + 1 : 1);
            if(filesystem[i].content && trash[trash_idx].content) {
                for(int j=0;j<trash[trash_idx].size && j<filesystem[i].content_cap-1;j++)
                    filesystem[i].content[j]=trash[trash_idx].content[j];
                if(trash[trash_idx].size < filesystem[i].content_cap)
                    filesystem[i].content[trash[trash_idx].size]=0;
            }
            filesystem[i].size = trash[trash_idx].size;
            filesystem[i].used=1;file_count++;
            file_content_free(trash_idx);
            trash[trash_idx].used=0;trash_count--;
            break;
        }
    }
    // Mirror of the recursive trash in move_to_trash(): restoring a
    // folder also restores every trashed entry whose name starts with
    // "Folder/" so the folder doesn't come back empty.  Nested folders
    // recurse (their children share the same prefix).
    if (is_dir) {
        for (int t=0;t<MAX_TRASH;t++) {
            if (!trash[t].used) continue;
            if (files_name_has_prefix(trash[t].name, folder_name))
                restore_from_trash(t);
        }
    }
}
static void empty_trash(){
    for(int i=0;i<MAX_TRASH;i++)trash[i].used=0;
    trash_count=0;
}
/* Permanently delete one trashed item (does not restore it to filesystem). */
static void delete_one_from_trash(int trash_idx){
    if(trash_idx<0||trash_idx>=MAX_TRASH) return;
    if(!trash[trash_idx].used) return;
    trash[trash_idx].used=0;
    trash_count--;
}
static void u_clear_form(){
    for(int i=0;i<17;i++){u_new_name[i]='\0';u_new_pass[i]='\0';u_old_pass[i]='\0';u_confirm[i]='\0';}
    u_name_pos=u_pass_pos=u_old_pos=u_confirm_pos=0;
    u_field=0;u_msg[0]='\0';
}

// ─── Key handler ─────────────────────────────────────────────────
void desktop_handle_key(char c){
    // Reset screensaver idle
    screensaver_reset_idle();

    // ── Undo browse mode: Enter commits, Esc cancels ─────────────
    // These must come BEFORE the char 26/25 handlers so browse-mode
    // navigation takes priority over entering/exiting browse mode.
    if (undo_browse_active && current_mode == MODE_DESKTOP) {
        if (c == 13 && undo_browse_idx >= 0) {  // Enter → commit
            for (int _ub = 0; _ub < ICON_GRID_SLOTS; _ub++)
                icon_order[_ub] = undo_stack[undo_browse_idx][_ub];
            undo_ptr = undo_browse_idx;
            redo_ptr = -1;
            icon_layout_save();
            desktop_set_status("Undo — layout restored");
            undo_browse_active = 0;
            desktop_redraw_no_click();
            return;
        }
        if (c == 27) {  // Esc → cancel
            for (int _ub = 0; _ub < ICON_GRID_SLOTS; _ub++)
                icon_order[_ub] = undo_browse_save[_ub];
            desktop_set_status("Cancelled");
            undo_browse_active = 0;
            desktop_redraw_no_click();
            return;
        }
        // Ctrl+Z during browsing → step further back
        if (c == 26 && undo_browse_idx > 0) {
            undo_browse_idx--;
            desktop_redraw_no_click();
            return;
        }
        // Ctrl+Shift+Z during browsing → step forward
        if (c == 25 && undo_browse_idx < undo_ptr) {
            undo_browse_idx++;
            desktop_redraw_no_click();
            return;
        }
    }

    // ── Ctrl+Z (char 26) — enter undo browse mode ──────────────
    // First press captures the current layout and shows the most
    // recent undo entry as a preview. Subsequent taps (while browse
    // mode is active) step further back through history.
    if (c == 26 && current_mode == MODE_DESKTOP && undo_ptr >= 0 && !undo_browse_active) {
        // Save current layout so Esc can restore it
        for (int _uz = 0; _uz < ICON_GRID_SLOTS; _uz++)
            undo_browse_save[_uz] = icon_order[_uz];
        // Start browsing from the most recent undo entry
        undo_browse_active = 1;
        undo_browse_idx = undo_ptr;
        desktop_redraw_no_click();
        return;
    }

    // ── Ctrl+Shift+Z (char 25) — standard redo (only when not browsing) ─
    if (c == 25 && current_mode == MODE_DESKTOP && redo_ptr >= 0 && !undo_browse_active) {
        // Push current state back to undo stack
        undo_ptr++;
        if (undo_ptr >= UNDO_DEPTH) {
            for (int _rz = 0; _rz < UNDO_DEPTH - 1; _rz++)
                for (int _rzj = 0; _rzj < ICON_GRID_SLOTS; _rzj++)
                    undo_stack[_rz][_rzj] = undo_stack[_rz + 1][_rzj];
            undo_ptr = UNDO_DEPTH - 1;
        }
        for (int _rz = 0; _rz < ICON_GRID_SLOTS; _rz++)
            undo_stack[undo_ptr][_rz] = icon_order[_rz];

        // Restore redo entry
        for (int _rz = 0; _rz < ICON_GRID_SLOTS; _rz++)
            icon_order[_rz] = redo_stack[redo_ptr][_rz];
        redo_ptr--;

        icon_layout_save();
        desktop_set_status("Redo — reapplied");
        desktop_redraw_no_click();
        return;
    }

    // Power menu
    if(power_menu_is_open()){
        power_menu_handle_key(c);
        if(!power_menu_is_open() && !power_session_pending())
            desktop_redraw_no_click();
        return;
    }

    // Quick Launch panel
    if(launcher_is_open()){
        launcher_handle_key(c);
        if(!launcher_is_open())
            desktop_redraw_no_click();
        return;
    }

    // App Library
    if(current_mode==MODE_APPLIB){
        if(c==27){current_mode=MODE_DESKTOP;desktop_redraw_no_click();return;}
        applib_handle_key(c);
        if(applib_launch_request>=0){
            int req=applib_launch_request;
            applib_launch_request=-1;
            do_launch(req);
        }
        // Launching an installed package from the launcher.
        if(applib_pkg_launch[0]){
            char nm[PKG_NAME_MAX];
            int k=0;
            for(; k<PKG_NAME_MAX-1 && applib_pkg_launch[k]; k++) nm[k]=applib_pkg_launch[k];
            nm[k]='\0';
            applib_pkg_launch[0]='\0';
            // Hop to the Terminal so the app's mirrored output is
            // visible on screen instead of only on COM1.
            int pid = pkg_run(nm);
            if (pid >= 0) current_mode = MODE_TERMINAL;
        }
        desktop_redraw_no_click();  /* always blit — applib_draw() alone never calls vesa_present() */
        return;
    }

    // App Store
    if(current_mode==MODE_STORE){
        if(c==27){current_mode=MODE_DESKTOP;desktop_redraw_no_click();return;}
        store_handle_key(c);
        // ENTER installed-and-ran (or ran) a package — switch to the
        // Terminal so the user sees the app running on screen.
        if (store_take_auto_launch())
            current_mode = MODE_TERMINAL;
        desktop_redraw_no_click();
        return;
    }

    // Weather
    if(current_mode==MODE_WEATHER){
        if(c==27){current_mode=MODE_DESKTOP;desktop_redraw_no_click();return;}
        weather_handle_key(c);
        desktop_redraw_no_click();
        return;
    }

    // Application Organizer
    if(current_mode==MODE_ORGANIZER){
        if(c==27){current_mode=MODE_DESKTOP;desktop_redraw_no_click();return;}
        organizer_handle_key(c);
        desktop_redraw_no_click();
        return;
    }

    // Security
    if(current_mode==MODE_SECURITY){
        if(c==27){current_mode=MODE_DESKTOP;desktop_redraw_no_click();return;}
        security_handle_key(c);
        desktop_redraw_no_click();
        return;
    }

    // Image Viewer
    if(current_mode==MODE_IMGVIEW){
        if(c==27){
            if (imgview_return_mode >= 0) {
                // Image was opened from Files or the Gallery — return
                // there instead of dropping to the desktop.
                int ret = imgview_return_mode;
                imgview_return_mode = -1;
                viewing_file = -1;
                current_mode = ret;
            } else {
                current_mode = MODE_DESKTOP;
            }
            desktop_redraw_no_click();return;
        }
        imgview_handle_key(c);
        desktop_redraw_no_click();
        return;
    }

    // Video player — ESC returns to the app it was opened from
    // (Files), mirroring the imgview/pdfview origin pattern.
    if(current_mode==MODE_VIDEO){
        if(c==27){
            video_close();                      // stop the sound
            if (video_return_mode >= 0) {
                int ret = video_return_mode;
                video_return_mode = -1;
                viewing_file = -1;
                current_mode = ret;
            } else {
                current_mode = MODE_DESKTOP;
            }
            desktop_redraw_no_click();return;
        }
        video_handle_key(c);
        desktop_redraw_no_click();
        return;
    }

    // Image Gallery
    if(current_mode==MODE_GALLERY){
        if(c==27){current_mode=MODE_DESKTOP;desktop_redraw_no_click();return;}
        gallery_handle_key(c);
        desktop_redraw_no_click();
        return;
    }

    // Web Browser — ESC is handled internally by browser_handle_key
    if(current_mode==MODE_BROWSER){
        browser_handle_key(c);
        desktop_redraw_no_click();
        return;
    }

    // PDF Viewer
    if(current_mode==MODE_PDFVIEW){
        if(c==27){
            if (pdfview_return_mode >= 0) {
                // PDF was opened from the Files app — return to the
                // file list instead of dropping to the desktop.
                int ret = pdfview_return_mode;
                pdfview_return_mode = -1;
                viewing_file = -1;
                current_mode = ret;
            } else {
                current_mode = MODE_DESKTOP;
            }
            desktop_redraw_no_click();return;
        }
        pdfview_handle_key(c);
        desktop_redraw_no_click();
        return;
    }

    // Calendar
    if(current_mode==MODE_CALENDAR){
        if(c==27&&calendar_is_main_view()){current_mode=MODE_DESKTOP;desktop_redraw_no_click();return;}
        calendar_handle_key(c);
        desktop_redraw_no_click();
        return;
    }

    // Quick Settings
    if(current_mode==MODE_QUICKSETTINGS){
        // ESC closes the Wi-Fi flyout first, then the whole panel
        if(c==27){
            if(qs_wifi_open){qs_wifi_open=0;desktop_redraw_no_click();return;}
            current_mode=MODE_DESKTOP;desktop_redraw_no_click();return;
        }
        if(vesa.active){
            if(c=='D'&&sound_volume>0){sound_volume-=5;desktop_redraw_no_click();return;}
            if(c=='C'&&sound_volume<100){sound_volume+=5;desktop_redraw_no_click();return;}
        }
        return;
    }

    // Clock
    if(current_mode==MODE_CLOCK){
        if(c==27&&clock_is_main_view()){current_mode=MODE_DESKTOP;desktop_redraw_no_click();return;}
        clock_handle_key(c);
        desktop_redraw_no_click();
        return;
    }

    // Sysmon
    if(current_mode==MODE_SYSMON){
        if(c==27){current_mode=MODE_DESKTOP;desktop_redraw_no_click();return;}
        sysmon_handle_key(c);
        desktop_redraw_no_click();
        return;
    }

    // Paint
    if(current_mode==MODE_PAINT){
        if(c==27){current_mode=MODE_DESKTOP;desktop_redraw_no_click();return;}
        paint_handle_key(c);  /* paint_draw() → vesa_present() called inside */
        return;
    }

    // Editor
    if(current_mode==MODE_EDITOR){
        if(c==27){
            if (editor_return_files) {
                // File was opened from the Files app — go back to the
                // file list, not the desktop.
                editor_return_files = 0;
                viewing_file = -1;
                current_mode = MODE_FILES;
            } else {
                current_mode = MODE_DESKTOP;
            }
            desktop_redraw_no_click();return;
        }
        editor_handle_key(c);return;
    }

    // Games
    if(current_mode==MODE_GAMES){
        if(c==27){games_init();current_mode=MODE_DESKTOP;desktop_redraw_no_click();return;}
        games_handle_key(c);
        desktop_redraw_no_click();
        return;
    }

    // Music
    if(current_mode==MODE_MUSIC){
        if(c=='p'||c=='P'){
            if(music_is_playing()){music_pause();desktop_set_status("Paused");}
            else{music_play(music_current_song());desktop_set_status("Playing");}
            desktop_redraw_no_click();return;
        }
        if(c=='n'||c=='N'){music_next();desktop_set_status("Next track");desktop_redraw_no_click();return;}
        if(c=='b'||c=='B'){music_prev();desktop_set_status("Previous track");desktop_redraw_no_click();return;}
        if(c=='s'||c=='S'){music_stop();desktop_set_status("Stopped");desktop_redraw_no_click();return;}
        if(vesa.active){
            if(c=='D'&&music_tab>0){music_tab--;desktop_redraw_no_click();return;}
            if(c=='C'&&music_tab<1){music_tab++;desktop_redraw_no_click();return;}
        }
        if(c==27){music_stop();current_mode=MODE_DESKTOP;desktop_redraw_no_click();return;}
        return;
    }

    // Calc
    if(current_mode==MODE_CALC){
        if(c==27){current_mode=MODE_DESKTOP;desktop_redraw_no_click();return;}
        calc_handle_key(c);
        desktop_redraw_no_click();  /* must go through desktop_draw() so vesa_present() fires */
        return;
    }

    // Files
    if(current_mode==MODE_FILES){
        // ── Viewing a single file: only ESC (back to list) does
        // anything here. This sub-screen previously had no state
        // tracking at all, so it would flash open for one frame
        // and immediately get overwritten by the next redraw.
        if (viewing_file >= 0) {
            if (c==27) { viewing_file=-1; desktop_redraw_no_click(); }
            return;
        }

        // ── Inline rename mode (F2): type new name, Enter=confirm, Esc=cancel
        if (rename_mode) {
            if (c == 13) {  // ENTER → commit rename
                if (rename_pos > 0 && rename_file_idx >= 0 && rename_file_idx < MAX_FILES &&
                    filesystem[rename_file_idx].used) {
                    // The user typed the bare name; rebuild the full
                    // stored name from the folder prefix captured at
                    // start ("Documents/" + "foo.txt").  Folders keep
                    // their trailing '/'.  No '/' inside the name is
                    // allowed (it would silently re-home the file).
                    int bad = 0;
                    for (int j = 0; j < rename_pos; j++)
                        if (rename_buf[j] == '/') { bad = 1; break; }
                    if (!bad) {
                        char newfull[MAX_FILENAME];
                        int n = 0;
                        for (int i = 0; rename_prefix[i] && n < MAX_FILENAME-2; i++)
                            newfull[n++] = rename_prefix[i];
                        for (int i = 0; i < rename_pos && n < MAX_FILENAME-2; i++)
                            newfull[n++] = rename_buf[i];
                        if (rename_is_dir && n > 0 && newfull[n-1] != '/') {
                            if (n < MAX_FILENAME-1) newfull[n++] = '/';
                        }
                        newfull[n] = '\0';
                        // Check for duplicate full name
                        int dup = 0;
                        for (int i = 0; i < MAX_FILES; i++) {
                            if (i == rename_file_idx || !filesystem[i].used) continue;
                            int m = 0;
                            while (newfull[m] && filesystem[i].name[m] &&
                                   newfull[m] == filesystem[i].name[m]) m++;
                            if (newfull[m] == '\0' && filesystem[i].name[m] == '\0')
                                { dup = 1; break; }
                        }
                        if (dup) {
                            desktop_set_status("Name already exists!");
                        } else {
                            // Folder rename: rewrite every child entry
                            // whose stored path starts with the old
                            // folder path so contents stay inside.
                            if (rename_is_dir) {
                                char oldfull[MAX_FILENAME];
                                str_copy(oldfull, filesystem[rename_file_idx].name, MAX_FILENAME);
                                int olen = 0; while (oldfull[olen]) olen++;
                                int nlen = n;
                                for (int i = 0; i < MAX_FILES; i++) {
                                    if (i == rename_file_idx || !filesystem[i].used) continue;
                                    int k = 0;
                                    while (oldfull[k] && filesystem[i].name[k] &&
                                           oldfull[k] == filesystem[i].name[k]) k++;
                                    if (k == olen) {  // child lives under old path
                                        // rebuild: newfull + remainder after prefix
                                        int rem = 0;
                                        while (filesystem[i].name[olen+rem]) rem++;
                                        char rebuilt[MAX_FILENAME];
                                        int r = 0;
                                        for (int x = 0; x < nlen && r < MAX_FILENAME-2; x++)
                                            rebuilt[r++] = newfull[x];
                                        for (int x = 0; x < rem && r < MAX_FILENAME-2; x++)
                                            rebuilt[r++] = filesystem[i].name[olen+x];
                                        rebuilt[r] = '\0';
                                        str_copy(filesystem[i].name, rebuilt, MAX_FILENAME);
                                    }
                                }
                            }
                            str_copy(filesystem[rename_file_idx].name, newfull, MAX_FILENAME);
                            fs_save_to_disk();
                            desktop_set_status("Renamed!");
                            {   // Serial marker (E2E test hook): new name.
                                const char* m = "RNRENAME ";
                                for (; *m; m++) { int sp=0; while ((inb(0x3F8+5)&0x20)==0 && sp++<200000){} outb(0x3F8,(uint8_t)*m); }
                                for (const char* q = newfull; *q; q++) { int sp=0; while ((inb(0x3F8+5)&0x20)==0 && sp++<200000){} outb(0x3F8,(uint8_t)*q); }
                                { const char* m2 = "\n"; for (; *m2; m2++) { int sp=0; while ((inb(0x3F8+5)&0x20)==0 && sp++<200000){} outb(0x3F8,(uint8_t)*m2); } }
                            }
                        }
                    } else {
                        desktop_set_status("Name can't contain '/'");
                    }
                }
                rename_mode = 0;
                desktop_redraw_no_click();
                return;
            }
            if (c == 27) {  // ESC → cancel
                rename_mode = 0;
                desktop_set_status("");
                desktop_redraw_no_click();
                return;
            }
            if (c == 8) {  // BACKSPACE
                if (rename_pos > 0) {
                    rename_buf[--rename_pos] = '\0';
                    desktop_redraw_no_click();
                }
                return;
            }
            // Printable character → insert into rename buffer
            if (c >= 32 && c <= 126 && rename_pos < MAX_FILENAME - 2) {
                rename_buf[rename_pos++] = c;
                rename_buf[rename_pos] = '\0';
                desktop_redraw_no_click();
                return;
            }
            return;
        }

        // ── This PC home: A/B cycles tiles, ENTER opens ────────
        if (files_home_view) {
            if (c=='A' && selected_file > 0) { selected_file--; desktop_redraw_no_click(); return; }
            if (c=='B' && selected_file < 5) { selected_file++; desktop_redraw_no_click(); return; }
            if (c==13) {
                static const int tile_items[6] = {FSI_DESKTOP, FSI_DOCS, FSI_DL, FSI_MUSIC, FSI_PICS, FSI_DRIVE};
                if (selected_file >= 0 && selected_file < 6)
                    files_sb_nav(tile_items[selected_file]);
                return;
            }
            if (c==27) { current_mode=MODE_DESKTOP; desktop_redraw_no_click(); return; }
            return;
        }

        int nrows = files_visible_count();
        // Auto-scroll so the selection stays on screen when it leaves
        // the visible window (same pattern as settings_scroll).
        int fvis = files_visible_rows((g_win_y >= 0) ? g_win_y : 70, (int)vesa.height - 160);
        if(c=='A'&&selected_file>0){
            selected_file--;
            if (selected_file < files_scroll) files_scroll = selected_file;
            desktop_redraw_no_click();
            {   // Serial marker (E2E test hook): row selection moved.
                int fi = -1; files_visible_row(selected_file, &fi);
                if (fi >= 0) {
                    const char* m = "SEL ";
                    for (; *m; m++) { int sp = 0; while ((inb(0x3F8+5)&0x20)==0 && sp++<200000){} outb(0x3F8,(uint8_t)*m); }
                    char tmp[12]; int i = 0, v = selected_file;
                    if (v == 0) { outb(0x3F8,'0'); } else { while (v>0) { tmp[i++]='0'+v%10; v/=10; } while (i>0) outb(0x3F8,(uint8_t)tmp[--i]); }
                    outb(0x3F8,' ');
                    const char* n = filesystem[fi].name;
                    for (; *n; n++) { int sp = 0; while ((inb(0x3F8+5)&0x20)==0 && sp++<200000){} outb(0x3F8,(uint8_t)*n); }
                    outb(0x3F8,'\n');
                }
            }
            return;
        }
        if(c=='B'&&selected_file<nrows-1){
            selected_file++;
            if (selected_file >= files_scroll + fvis) files_scroll = selected_file - fvis + 1;
            desktop_redraw_no_click();
            {   // Serial marker (E2E test hook): row selection moved.
                int fi = -1; files_visible_row(selected_file, &fi);
                if (fi >= 0) {
                    const char* m = "SEL ";
                    for (; *m; m++) { int sp = 0; while ((inb(0x3F8+5)&0x20)==0 && sp++<200000){} outb(0x3F8,(uint8_t)*m); }
                    char tmp[12]; int i = 0, v = selected_file;
                    if (v == 0) { outb(0x3F8,'0'); } else { while (v>0) { tmp[i++]='0'+v%10; v/=10; } while (i>0) outb(0x3F8,(uint8_t)tmp[--i]); }
                    outb(0x3F8,' ');
                    const char* n = filesystem[fi].name;
                    for (; *n; n++) { int sp = 0; while ((inb(0x3F8+5)&0x20)==0 && sp++<200000){} outb(0x3F8,(uint8_t)*n); }
                    outb(0x3F8,'\n');
                }
            }
            return;
        }
        if(c==13&&nrows>0){
            int fi = -1;
            if (!files_visible_row(selected_file, &fi)) return;
            if (fi < 0) { files_go_up(); return; }
            files_open_file(fi);
            return;
        }
        if((c=='x'||c=='X')&&nrows>0){
            int fi = -1;
            if (!files_visible_row(selected_file, &fi)) return;
            if (fi >= 0) { move_to_trash(fi); desktop_set_status("Moved to trash!"); desktop_redraw_no_click(); return; }
            return;
        }
        if(c=='n'||c=='N'){ files_create_here("New File.txt", 0); return; }
        if(c=='m'||c=='M'){ files_create_here("New Folder", 1); return; }
        // F2 (char 6) → start inline rename
        if(c==6 && nrows>0){
            int fi = -1;
            if (!files_visible_row(selected_file, &fi)) return;
            if (fi >= 0 && fi < MAX_FILES && filesystem[fi].used) {
                rename_file_idx = fi;
                // Prefill with the SHORT name as displayed on the row
                // (the stored name carries the folder prefix, e.g.
                // "Documents/foo.txt").  Editing happens on the bare
                // name; the prefix is re-attached on commit.
                char shortname[MAX_FILENAME];
                files_short_name(filesystem[fi].name, shortname);
                int full_len = 0;
                while (filesystem[fi].name[full_len]) full_len++;
                int short_len = 0;
                while (shortname[short_len]) short_len++;
                int pfx = full_len - short_len;   // stored dir part
                int end = short_len;
                rename_is_dir = (end > 0 && shortname[end-1] == '/');
                if (rename_is_dir) end--;          // drop trailing '/' for typing
                if (pfx > 0) {
                    for (int i = 0; i < pfx && i < MAX_FILENAME-1; i++)
                        rename_prefix[i] = filesystem[fi].name[i];
                    rename_prefix[pfx] = '\0';
                } else {
                    rename_prefix[0] = '\0';
                }
                for (int i = 0; i < end; i++)
                    rename_buf[i] = shortname[i];
                rename_buf[end] = '\0';
                rename_pos = end;
                rename_mode = 1;
                desktop_set_status("Rename: type new name, Enter=ok, Esc=cancel");
                {   // Serial marker (E2E test hook): F2 reached rename.
                    const char* m = "RNRENAMEMODE\n";
                    for (; *m; m++) { int sp=0; while ((inb(0x3F8+5)&0x20)==0 && sp++<200000){} outb(0x3F8,(uint8_t)*m); }
                }
                desktop_redraw_no_click();
            }
            return;
        }
        if(c==27){
            if (files_desktop_view) { files_sb_nav(FSI_HOME); return; }
            if (files_cur_path[0]) { files_go_up(); return; }
            // At the drive root: ESC climbs back to the This PC home.
            files_sb_nav(FSI_HOME); return;
        }
        return;
    }

    // Trash
    if(current_mode==MODE_TRASH){
        // Build the same compact index used by the VESA panel so
        // "selected item" means the same thing in both places.
        int idxs[MAX_TRASH]; int n=0;
        for(int i=0;i<MAX_TRASH;i++) if(trash[i].used) idxs[n++]=i;
        if(trash_selected>=n) trash_selected=n>0?n-1:0;
        if(trash_selected<0)  trash_selected=0;

        if(c=='A'&&trash_selected>0){trash_selected--;desktop_redraw_no_click();return;}          /* up   */
        if(c=='B'&&trash_selected<n-1){trash_selected++;desktop_redraw_no_click();return;}        /* down */

        if(c=='r'||c=='R'){
            if(n>0){
                desktop_set_status("Restored!");
                restore_from_trash(idxs[trash_selected]);
                if(trash_selected>0) trash_selected--;
            }
            desktop_redraw_no_click();return;
        }
        if((c==8||c==127)&&n>0){   /* DEL/Backspace: permanently delete selected item */
            desktop_set_status("Deleted permanently!");
            delete_one_from_trash(idxs[trash_selected]);
            if(trash_selected>0) trash_selected--;
            desktop_redraw_no_click();return;
        }
        if(c=='x'||c=='X'){empty_trash();trash_selected=0;desktop_set_status("Trash emptied!");desktop_redraw_no_click();return;}
        if(c==27){current_mode=MODE_DESKTOP;desktop_redraw_no_click();return;}
        return;
    }

    // Terminal — keyboard.c owns all the actual input handling
    // (typing, backspace, ENTER, history, autocomplete) since it's
    // the code that genuinely receives every keystroke first; it
    // calls desktop_draw() itself after each change. This handler
    // only needs to close the window on ESC.
    if(current_mode==MODE_TERMINAL){
        if(c==27){
            current_mode=MODE_DESKTOP;
            term_initialized=0; // reset so reopening shows a fresh banner
            terminal_set_redirect((term_redirect_t*)0); // crash/exception
            // messages in isr.c must reach visible raw VGA output,
            // not vanish into this now-closed window's buffer.
            desktop_redraw_no_click();
        }
        return;
    }

    // Settings
    if(current_mode==MODE_SETTINGS){
        // VESA: TAB cycles sidebar sections
        if(vesa.active && c=='	' && !(settings_section==5 && users_sub!=0)){
            int cur_sec=(settings_section>=0)?settings_section:settings_selected;
            int ns=(cur_sec+1)%8;
            settings_section=ns;
            if(ns==0){settings_selected=current_timezone;settings_scroll=current_timezone>9?current_timezone-9:0;}
            else if(ns==6){settings_selected=current_wallpaper;}
            else if(ns==7){settings_selected=current_theme;}
            else{settings_selected=0;}
            users_sub=0;u_clear_form();
            desktop_redraw_no_click();return;
        }
        // VESA: UP/DOWN cycle sections when focused on sidebar (section != 0,2,5,6,7)
        // For sections with sub-navigation (0=tz,2=sound,6=wallpaper,7=theme), arrow keys
        // work on the content; all others treat UP/DOWN as sidebar navigation.
        if(vesa.active && settings_section>=0 &&
           settings_section!=0 && settings_section!=1 && settings_section!=2 &&
           settings_section!=5 && settings_section!=6 && settings_section!=7){
            if(c=='A'&&settings_section>0){settings_section--;desktop_redraw_no_click();return;}
            if(c=='B'&&settings_section<7){settings_section++;desktop_redraw_no_click();return;}
        }
        if(settings_section==1){
            // Display section — resolution selection from vbe_mode_list
            int dm_count = vbe_mode_count;
            if (c=='A' && settings_selected > 0) { settings_selected--; desktop_redraw_no_click(); return; }
            if (c=='B' && settings_selected < dm_count - 1) { settings_selected++; desktop_redraw_no_click(); return; }
            if (c == 13 && settings_selected >= 0 && settings_selected < dm_count) {
                if (settings_selected != vbe_active_mode) {
                    if (vesa_set_mode(vbe_mode_list[settings_selected].width,
                                      vbe_mode_list[settings_selected].height,
                                      vbe_mode_list[settings_selected].bpp)) {
                        desktop_set_status("Resolution changed!");
                    } else {
                        desktop_set_status("Mode switch failed!");
                    }
                }
                desktop_redraw_no_click(); return;
            }
            if (c == 27) { settings_section = -1; settings_selected = 0; desktop_redraw_no_click(); return; }
            return;
        }
        if(settings_section==5){
            if(users_sub==0){
                if(c=='A'&&users_selected>0){users_selected--;desktop_redraw_no_click();return;}
                if(c=='B'&&users_selected<user_count-1){users_selected++;desktop_redraw_no_click();return;}
                if(c=='a'){if(user_count<MAX_USERS){users_sub=1;u_clear_form();desktop_redraw_no_click();}else{str_copy(u_msg,"Max users!",48);desktop_redraw_no_click();}return;}
                if(c=='c'||c=='C'){users_sub=2;u_clear_form();desktop_redraw_no_click();return;}
                if(c=='d'||c=='D'){
                    if(users_selected==0)str_copy(u_msg,"Cannot delete admin!",48);
                    else if(users_selected==logged_in_user)str_copy(u_msg,"Cannot delete yourself!",48);
                    else{auth_delete_user(users_selected);if(users_selected>=user_count)users_selected=user_count-1;str_copy(u_msg,"Deleted!",48);}
                    desktop_redraw_no_click();return;
                }
                if(c==27){
                    settings_section=-1;
                    settings_selected=0;
                    users_sub=0;
                    u_clear_form();
                    desktop_redraw_no_click();return;
                }
                return;
            }
            if(users_sub==1){
                if(c=='\t'){u_field=(u_field+1)%2;desktop_redraw_no_click();return;}
                if(c==27){users_sub=0;u_clear_form();desktop_redraw_no_click();return;}
                if(c==13){
                    if(u_name_pos==0){str_copy(u_msg,"Username required!",48);desktop_redraw_no_click();return;}
                    if(u_pass_pos==0){str_copy(u_msg,"Password required!",48);desktop_redraw_no_click();return;}
                    if(auth_add_user(u_new_name,u_new_pass)){str_copy(u_msg,"User created!",48);users_sub=0;}
                    else str_copy(u_msg,"Failed!",48);
                    desktop_redraw_no_click();return;
                }
                if(c==8){if(u_field==0&&u_name_pos>0)u_new_name[--u_name_pos]='\0';if(u_field==1&&u_pass_pos>0)u_new_pass[--u_pass_pos]='\0';desktop_redraw_no_click();return;}
                if(c>=32&&c<127){
                    u_msg[0]='\0';  /* clear stale message while typing */
                    if(u_field==0&&u_name_pos<16){u_new_name[u_name_pos++]=c;u_new_name[u_name_pos]='\0';}
                    if(u_field==1&&u_pass_pos<16){u_new_pass[u_pass_pos++]=c;u_new_pass[u_pass_pos]='\0';}
                    desktop_redraw_no_click();return;
                }
                return;
            }
            if(users_sub==2){
                if(c=='\t'){u_field=(u_field+1)%3;desktop_redraw_no_click();return;}
                if(c==27){users_sub=0;desktop_redraw_no_click();return;}
                if(c==13){
                    if(!str_eq(u_new_pass,u_confirm)){str_copy(u_msg,"Passwords don't match!",48);desktop_redraw_no_click();return;}
                    if(auth_change_password(users_selected,u_old_pass,u_new_pass)){str_copy(u_msg,"Changed!",48);users_sub=0;}
                    else{str_copy(u_msg,"Wrong old password!",48);}
                    desktop_redraw_no_click();return;
                }
                if(c==8){
                    if(u_field==0&&u_old_pos>0)u_old_pass[--u_old_pos]='\0';
                    if(u_field==1&&u_pass_pos>0)u_new_pass[--u_pass_pos]='\0';
                    if(u_field==2&&u_confirm_pos>0)u_confirm[--u_confirm_pos]='\0';
                    desktop_redraw_no_click();return;
                }
                if(c>=32&&c<127){
                    u_msg[0]='\0';  /* clear stale message while typing */
                    if(u_field==0&&u_old_pos<16){u_old_pass[u_old_pos++]=c;u_old_pass[u_old_pos]='\0';}
                    if(u_field==1&&u_pass_pos<16){u_new_pass[u_pass_pos++]=c;u_new_pass[u_pass_pos]='\0';}
                    if(u_field==2&&u_confirm_pos<16){u_confirm[u_confirm_pos++]=c;u_confirm[u_confirm_pos]='\0';}
                    desktop_redraw_no_click();return;
                }
                return;
            }
            return;
        }
        if(settings_section==6){
            // Count filesystem image files
            int wf_count = 0;
            for (int fi = 0; fi < MAX_FILES; fi++) {
                if (!filesystem[fi].used) continue;
                const unsigned char* d2 = (const unsigned char*)filesystem[fi].content;
                int sz2 = filesystem[fi].size;
                if (sz2 <= 0) { while (d2[sz2]) sz2++; }
                if (sz2 < 4) continue;
                if ((d2[0]==0x89 && d2[1]=='P' && d2[2]=='N' && d2[3]=='G') ||
                    (d2[0]==0xFF && d2[1]==0xD8 && d2[2]==0xFF) ||
                    (d2[0]=='B' && d2[1]=='M') ||
                    (d2[0]=='G' && d2[1]=='I' && d2[2]=='F'))
                    wf_count++;
            }
            int wf_total = WALLPAPER_COUNT + wf_count;
            // LEFT/RIGHT adjust dim overlay (scan codes only — 'A'/'B' are
            // already used for UP/DOWN navigation in this section).
            if (c == 0x4B) {  // LEFT arrow = decrease dim
                int new_dim = wallpaper_dim_level - 5;
                if (new_dim < 0) new_dim = 0;
                wallpaper_set_dim(new_dim);
                desktop_set_status(wallpaper_dim_level > 0
                    ? "Dim overlay set" : "Dim overlay off");
                desktop_redraw_no_click();
                return;
            }
            if (c == 0x4D) {  // RIGHT arrow = increase dim
                int new_dim = wallpaper_dim_level + 5;
                if (new_dim > 100) new_dim = 100;
                wallpaper_set_dim(new_dim);
                desktop_set_status("Dim overlay adjusted");
                desktop_redraw_no_click();
                return;
            }
            if (c=='A' && settings_selected > 0) { settings_selected--; desktop_redraw_no_click(); return; }
            if (c=='B' && settings_selected < wf_total - 1) { settings_selected++; desktop_redraw_no_click(); return; }
            if (c == 13) {
                if (settings_selected < WALLPAPER_COUNT) {
                    // Built-in wallpaper
                    wallpaper_set_custom(-1);
                    current_wallpaper = settings_selected;
                    desktop_set_status("Wallpaper changed!");
                } else {
                    // File-based wallpaper: find which file index
                    int file_idx = -1, found = 0, done = 0;
                    for (int fi = 0; fi < MAX_FILES && !done; fi++) {
                        if (!filesystem[fi].used) continue;
                        const unsigned char* d3 = (const unsigned char*)filesystem[fi].content;
                        int sz3 = filesystem[fi].size;
                        if (sz3 <= 0) { while (d3[sz3]) sz3++; }
                        if (sz3 < 4) continue;
                        if ((d3[0]==0x89 && d3[1]=='P' && d3[2]=='N' && d3[3]=='G') ||
                            (d3[0]==0xFF && d3[1]==0xD8 && d3[2]==0xFF) ||
                            (d3[0]=='B' && d3[1]=='M') ||
                            (d3[0]=='G' && d3[1]=='I' && d3[2]=='F')) {
                            if (found == settings_selected - WALLPAPER_COUNT) {
                                file_idx = fi;
                                done = 1;
                            } else {
                                found++;
                            }
                        }
                    }
                    if (file_idx >= 0) {
                        wallpaper_set_custom(file_idx);
                        desktop_set_status("Custom wallpaper set!");
                    }
                }
                desktop_redraw_no_click();
                return;
            }
            if (c == 27) { settings_section = -1; settings_selected = 0; desktop_redraw_no_click(); return; }
            return;
        }
        if(settings_section==7){
            if(c=='A'&&settings_selected>0){settings_selected--;desktop_redraw_no_click();return;}
            if(c=='B'&&settings_selected<THEME_COUNT-1){settings_selected++;desktop_redraw_no_click();return;}
            if(c==13){current_theme=settings_selected;desktop_set_status("Theme changed!");desktop_vfx_flash(180,140,255,4);desktop_redraw_no_click();return;}
            if(c==27){settings_section=-1;settings_selected=0;desktop_redraw_no_click();return;}
            // Keys 1-8 select a preset window body color for the compositor
            if (compositor_is_enabled() && c >= '1' && c <= '8' && active_surface >= 0) {
                int idx = c - '1';
                compositor_surface_set_body_color(active_surface, body_pal[idx]);
                desktop_set_status("Window body color changed!");
                desktop_redraw_no_click();
                return;
            }
            return;
        }
        if(settings_section==-1){
            if(c=='A'&&settings_selected>0){settings_selected--;desktop_redraw_no_click();return;}
            if(c=='B'&&settings_selected<7){settings_selected++;desktop_redraw_no_click();return;}
            if(c==13){
                settings_section=settings_selected;
                settings_selected=current_timezone;
                settings_scroll=current_timezone>9?current_timezone-9:0;
                u_clear_form();users_sub=0;
                desktop_redraw_no_click();return;
            }
            if(c==27){current_mode=MODE_DESKTOP;settings_section=-1;settings_selected=0;desktop_redraw_no_click();return;}
            return;
        }
        if(settings_section==0){
            if(c=='A'&&settings_selected>0){settings_selected--;if(settings_selected<settings_scroll)settings_scroll--;desktop_redraw_no_click();return;}
            if(c=='B'&&settings_selected<timezone_count-1){settings_selected++;if(settings_selected>=settings_scroll+9)settings_scroll++;desktop_redraw_no_click();return;}
            if(c==13){current_timezone=settings_selected;desktop_set_status("Timezone set!");desktop_redraw_no_click();return;}
            if(c==27){settings_section=-1;settings_selected=0;desktop_redraw_no_click();return;}
            return;
        }
        if(settings_section==2){
            if(c=='D'&&sound_volume>0){sound_volume-=5;desktop_redraw_no_click();return;}
            if(c=='C'&&sound_volume<100){sound_volume+=5;desktop_redraw_no_click();return;}
            if(c==13){desktop_set_status("Sound test");return;}
            if(c==27){settings_section=-1;settings_selected=0;desktop_redraw_no_click();return;}
            return;
        }
        if(c==27){settings_section=-1;settings_selected=0;desktop_redraw_no_click();return;}
        return;
    }

    // Desktop mode
    if(current_mode==MODE_DESKTOP){
        // Search takes precedence over the shortcut keys so the user
        // can type p/l/q (e.g. "AppLib") without opening the power
        // menu / lock / launcher mid-query.
        if(search_active){
            if(c==27){search_active=0;search_query[0]='\0';search_qpos=0;search_rcount=0;selected_icon=0;desktop_redraw_no_click();return;}
            if(c==13&&search_rcount>0){
                int ai=search_results[selected_icon<search_rcount?selected_icon:0];
                search_active=0;search_query[0]='\0';search_qpos=0;search_rcount=0;
                do_launch(ai);desktop_redraw_no_click();return;
            }
            if(c=='A'&&selected_icon>0){selected_icon--;desktop_redraw_no_click();return;}
            if(c=='B'&&selected_icon<search_rcount-1){selected_icon++;desktop_redraw_no_click();return;}
            if(c==8&&search_qpos>0){search_query[--search_qpos]='\0';do_search();selected_icon=0;desktop_redraw_no_click();return;}
            if(c>=32&&c<127&&search_qpos<30){search_query[search_qpos++]=c;search_query[search_qpos]='\0';do_search();selected_icon=0;desktop_redraw_no_click();return;}
            return;
        }

        if(c=='l'||c=='L'){power_lock();return;}
        if(c=='p'||c=='P'){power_menu_show();return;}
        if(c=='q'||c=='Q'){launcher_toggle();return;}

        // Open search
        if(c=='/'){search_active=1;search_query[0]='\0';search_qpos=0;search_rcount=0;selected_icon=0;desktop_redraw_no_click();return;}

        // Navigation: left/right within row — skip empty slots
        if(c=='C'||c=='\t'){
            int start = selected_icon;
            do { selected_icon = (selected_icon + 1) % g_grid_slots; }
            while (icon_order[selected_icon] < 0 && selected_icon != start);
            desktop_redraw_no_click();return;
        }
        if(c=='D'){
            int start = selected_icon;
            do { selected_icon = (selected_icon + g_grid_slots - 1) % g_grid_slots; }
            while (icon_order[selected_icon] < 0 && selected_icon != start);
            desktop_redraw_no_click();return;
        }
        // Up/down between rows (ICON_GRID_COLS icons per row) — skip empty slots
        if(c=='B'){
            int next = selected_icon + ICON_GRID_COLS;
            while (next < g_grid_slots && icon_order[next] < 0) next++;
            if (next < g_grid_slots) { selected_icon = next; desktop_redraw_no_click();return; }
        }
        if(c=='A'){
            int prev = selected_icon - ICON_GRID_COLS;
            while (prev >= 0 && icon_order[prev] < 0) prev--;
            if (prev >= 0) { selected_icon = prev; desktop_redraw_no_click();return; }
        }

        if(c==13){
            int aidx = icon_order[selected_icon];
            if (aidx >= 0) do_launch(aidx);
            desktop_redraw_no_click();return;
        }
    }
}

// ─── Undo browse commit (called from keyboard.c on Ctrl release) ─
// When the user releases the Ctrl key while in undo-browse mode,
// the current preview state is committed — same logic as pressing
// Enter during browsing.  No-op if not browsing.
void desktop_undo_browse_commit(void) {
    if (!undo_browse_active || current_mode != MODE_DESKTOP) return;
    if (undo_browse_idx >= 0) {
        for (int _ub = 0; _ub < ICON_GRID_SLOTS; _ub++)
            icon_order[_ub] = undo_stack[undo_browse_idx][_ub];
        undo_ptr = undo_browse_idx;
        redo_ptr = -1;
        icon_layout_save();
        desktop_set_status("Undo — layout restored");
    }
    undo_browse_active = 0;
    desktop_redraw_no_click();
}

// ─── Init ─────────────────────────────────────────────────────────
void desktop_init(){
    current_mode=MODE_DESKTOP;
    selected_icon=0;selected_file=0;trash_count=0;files_cur_path[0]='\0';
    settings_selected=0;settings_scroll=0;settings_section=-1;
    sound_volume=75;users_selected=0;users_sub=0;
    search_active=0;search_query[0]='\0';search_qpos=0;
    u_clear_form();
    compositor_init();

    // Create the compositor surface for the active app window.
    // Full-screen size so drawing at absolute screen coordinates works.
    // The surface stores only the content pixels; the compositor draws
    // the body background, border, drop shadow, and title bar uniformly
    // via compositor_composite(). decorated=1 and has_shadow=1 (defaults).
    // Compositor surface pipeline removed — all windows draw
    // directly to the framebuffer with their own self-contained
    // window frame (body gradient, border, shadow, title bar).

    weather_init();
    screensaver_init();
    applib_launch_request=-1;
    icon_order_init();
    for (int si = 0; si < g_grid_slots; si++) box_sel[si] = 0;

    // Pre-decode the custom wallpaper cache before the first draw so
    // the PIT IRQ's vesa_present() doesn't snapshot a half-decoded
    // cache (the "top-half glitch" on first login).
    wallpaper_draw();

    desktop_draw();
    {   // TEMP debug: deterministic "desktop is interactive" marker so
        // the E2E harness knows when it's safe to send navigation keys.
        const char* s = "DESKTOP UP\n";
        for (; *s; s++) { while ((inb(0x3F8+5)&0x20)==0){} outb(0x3F8,(uint8_t)*s); }
    }
}
