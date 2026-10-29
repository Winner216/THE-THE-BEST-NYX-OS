// cpu/organizer.c — Application Organizer for Nyx OS
#include <mouse.h>
extern volatile uint32_t tick;
// A full-screen two-panel app that lets users create custom folders
// and organize installed apps into them. VESA pixel UI with VGA text
// fallback, matching the OS's purple cosmic aesthetic.
//
// ── Panels ────────────────────────────────────────────────────────
//   LEFT (32%): folder list — "All Apps", custom folders, "Unorganized"
//   RIGHT (68%): app grid inside the selected folder
//
// ── Controls ──────────────────────────────────────────────────────
//   TAB       : toggle between left (folders) and right (apps) panel
//   UP/DOWN   : navigate within the active panel
//   LEFT/RIGHT: same as UP/DOWN in grid mode (wraps by row)
//   ENTER     : launch selected app / open selected folder
//   INS       : create a new folder
//   DEL       : delete the selected folder (with confirmation)
//   R         : rename the selected folder
//   M         : move selected app to another folder
//   ESC       : back to desktop

#include <organizer.h>
#include <desktop.h>
#include <vesa.h>
#include <speaker.h>
#include <stdint.h>

// ─── VESA scaled char (from vesa.c) ───────────────────────────────
extern void vesa_draw_char_scaled(int x,int y,char c,
                                   uint32_t fg,uint32_t bg,int scale);

// ═══════════════════════════════════════════════════════════════════
//  IMPLEMENTATION DATA
// ═══════════════════════════════════════════════════════════════════

// ─── App definitions (mirrored from applib.c for self-containment) ─
// launch_id → name lookup, for display.
static const char* org_app_names[] = {
    "Terminal","Files","About","Editor","AppLib",
    "Trash","Settings","Music","Calc","Games",
    "Paint","Monitor","Clock","Calendar","Weather","Security"
};
#define ORG_APP_COUNT 16

// Per-app accent colors (mirrors desktop.c's icon_glyph_rgb)
static const uint32_t org_app_colors[] = {
    0x7FE8C8, 0xFFD166, 0x8FC4FF, 0xFF9FD6, 0x8FF0A8,
    0xFF8FA0, 0xC8C8E0, 0xE0A8FF, 0x8FE0FF, 0xFFB088,
    0x8FC4FF, 0x9FFFC8, 0x8FE8FF, 0xE0A8FF, 0x8FC4FF,
    0xFF4455,
};

// VGA palette indices per app
static const uint8_t org_app_vga[] = {
    10,14,9,13,10,12,7,13,11,14,9,10,11,13,9,12,
};

// ─── Folder storage ─────────────────────────────────────────────
OrgFolder org_folders[MAX_ORG_FOLDERS];
int       org_folder_count = 0;

// ─── UI state ─────────────────────────────────────────────────────
static int  active_panel    = 0;   // 0 = left (folders), 1 = right (apps)
static int  sel_folder      = 0;   // selected folder index (0 = "All Apps")
static int  sel_app_in      = 0;   // selected app index within the folder
static int  folder_scroll   = 0;   // scroll offset for folder list
static int  app_scroll      = 0;   // scroll offset for app grid

// ─── Modal states ─────────────────────────────────────────────────
static int  modal_type      = 0;   // 0 = none, 1 = rename, 2 = create,
                                    // 3 = move, 4 = confirm delete
static char modal_input[17];       // text input buffer
static int  modal_pos       = 0;
static int  modal_move_fol  = 0;   // target folder index during move
static int  modal_move_app  = 0;   // app index being moved

// ─── String helpers ───────────────────────────────────────────────
static int og_len(const char* s) { int i=0; while(s[i]) i++; return i; }
static void og_cpy(char* dst, const char* src, int max) {
    int i=0; while(src[i] && i<max-1) { dst[i]=src[i]; i++; }
    dst[i] = '\0';
}

// ─── Color helper ─────────────────────────────────────────────────
static uint32_t og_mix(uint32_t c, int d) {
    int r=(int)(c>>16&0xFF)+d; if(r<0)r=0;if(r>255)r=255;
    int g=(int)(c>>8 &0xFF)+d; if(g<0)g=0;if(g>255)g=255;
    int b=(int)(c    &0xFF)+d; if(b<0)b=0;if(b>255)b=255;
    return ((uint32_t)r<<16)|((uint32_t)g<<8)|(uint32_t)b;
}

// ═══════════════════════════════════════════════════════════════════
//  DEFAULT FOLDERS (pre-populated)
// ═══════════════════════════════════════════════════════════════════

// Maps each app (by launch_id) to a default category index.
// Categories: 0=System, 1=Productivity, 2=Media, 3=Games, 4=Utilities
static const int app_default_cat[ORG_APP_COUNT] = {
    0, // 0  Terminal    → System
    0, // 1  Files       → System
    0, // 2  About       → System
    1, // 3  Editor      → Productivity
    0, // 4  AppLib      → System
    0, // 5  Trash       → System
    0, // 6  Settings    → System
    2, // 7  Music       → Media
    4, // 8  Calc        → Utilities
    3, // 9  Games       → Games
    2, // 10 Paint       → Media
    0, // 11 Monitor     → System
    4, // 12 Clock       → Utilities
    1, // 13 Calendar    → Productivity
    4, // 14 Weather     → Utilities
    5, // 15 Security     → Security
};

static const char* default_folder_names[] = {
    "System","Productivity","Media","Games","Utilities","Security"
};
#define DEFAULT_FOLDER_COUNT 6

static const uint32_t default_folder_colors[] = {
    0x6A4FBF, // System       – purple
    0x55EE88, // Productivity – green
    0xFF66AA, // Media        – pink
    0xFFAA00, // Games        – orange
    0x55AAFF, // Utilities    – blue
    0xFF3355, // Security     – alert red
};

static const uint8_t default_folder_vga[] = {
    5,  // System       – magenta
    10, // Productivity – light green
    13, // Media        – light magenta
    14, // Games        – yellow
    9,  // Utilities    – light blue
    12, // Security     – light red
};

// ═══════════════════════════════════════════════════════════════════
//  INIT
// ═══════════════════════════════════════════════════════════════════

void organizer_init(void) {
    // Clear all folders
    for (int i=0; i<MAX_ORG_FOLDERS; i++) {
        org_folders[i].name[0] = '\0';
        org_folders[i].app_count = 0;
    }
    org_folder_count = 0;

    // Create default folders
    for (int i=0; i<DEFAULT_FOLDER_COUNT; i++) {
        og_cpy(org_folders[i].name, default_folder_names[i], ORG_FOLDER_NAME_LEN);
        org_folders[i].color   = default_folder_colors[i];
        org_folders[i].vga_col = default_folder_vga[i];
        org_folders[i].app_count = 0;
        org_folder_count++;
    }

    // Assign apps to default folders
    for (int a=0; a<ORG_APP_COUNT; a++) {
        int cat = app_default_cat[a];
        if (cat < org_folder_count) {
            OrgFolder* f = &org_folders[cat];
            if (f->app_count < MAX_APPS_PER_FOLDER)
                f->app_ids[f->app_count++] = a;
        }
    }

    // Reset UI state
    active_panel   = 0;
    sel_folder     = 0;
    sel_app_in     = 0;
    folder_scroll  = 0;
    app_scroll     = 0;
    modal_type     = 0;
}

// ═══════════════════════════════════════════════════════════════════
//  FOLDER MANAGEMENT
// ═══════════════════════════════════════════════════════════════════

int organizer_add_folder(const char* name) {
    if (!name || !name[0]) return 0;
    if (org_folder_count >= MAX_ORG_FOLDERS) return 0;
    OrgFolder* f = &org_folders[org_folder_count];
    og_cpy(f->name, name, ORG_FOLDER_NAME_LEN);
    f->app_count = 0;
    f->color     = 0x9090C0;
    f->vga_col   = 7;
    org_folder_count++;
    
    return 1;
}

int organizer_remove_folder(int idx) {
    if (idx < 0 || idx >= org_folder_count) return 0;
    if (org_folder_count <= 1) return 0; // keep at least one
    // Shift folders down
    for (int i=idx; i<org_folder_count-1; i++)
        org_folders[i] = org_folders[i+1];
    org_folder_count--;
    org_folders[org_folder_count].name[0] = '\0';
    org_folders[org_folder_count].app_count = 0;
    if (sel_folder >= org_folder_count) sel_folder = org_folder_count-1;
    
    return 1;
}

int organizer_rename_folder(int idx, const char* new_name) {
    if (!new_name || !new_name[0]) return 0;
    if (idx < 0 || idx >= org_folder_count) return 0;
    og_cpy(org_folders[idx].name, new_name, ORG_FOLDER_NAME_LEN);
    
    return 1;
}

int organizer_add_app_to_folder(int folder_idx, int app_launch_id) {
    if (folder_idx < 0 || folder_idx >= org_folder_count) return 0;
    if (app_launch_id < 0 || app_launch_id >= ORG_APP_COUNT) return 0;
    OrgFolder* f = &org_folders[folder_idx];
    // Check if already in this folder
    for (int i=0; i<f->app_count; i++)
        if (f->app_ids[i] == app_launch_id) return 0;
    if (f->app_count >= MAX_APPS_PER_FOLDER) return 0;
    f->app_ids[f->app_count++] = app_launch_id;
    return 1;
}

int organizer_remove_app_from_folder(int folder_idx, int app_idx) {
    if (folder_idx < 0 || folder_idx >= org_folder_count) return 0;
    OrgFolder* f = &org_folders[folder_idx];
    if (app_idx < 0 || app_idx >= f->app_count) return 0;
    for (int i=app_idx; i<f->app_count-1; i++)
        f->app_ids[i] = f->app_ids[i+1];
    f->app_count--;
    return 1;
}

// ─── Check if an app is in any folder (i.e., not "unorganized") ───
static int app_is_in_any_folder(int launch_id) {
    for (int f=0; f<org_folder_count; f++)
        for (int a=0; a<org_folders[f].app_count; a++)
            if (org_folders[f].app_ids[a] == launch_id) return 1;
    return 0;
}

// ─── Check if a specific folder contains an app ───────────────────
static int folder_has_app(int folder_idx, int launch_id) {
    if (folder_idx < 0 || folder_idx >= org_folder_count) return 0;
    OrgFolder* f = &org_folders[folder_idx];
    for (int i=0; i<f->app_count; i++)
        if (f->app_ids[i] == launch_id) return 1;
    return 0;
}

// ═══════════════════════════════════════════════════════════════════
//  VGA FALLBACK
// ═══════════════════════════════════════════════════════════════════

#define VGA_WIDTH  80
#define VGA_HEIGHT 25
#define VGA_MEM    ((uint16_t*)0xB8000)

static inline uint16_t og_entry(char c, uint8_t fg, uint8_t bg) {
    return (uint16_t)c | (uint16_t)(fg | bg<<4) << 8;
}
static void og_put(int x, int y, char c, uint8_t fg, uint8_t bg) {
    if (x<0||x>=VGA_WIDTH||y<0||y>=VGA_HEIGHT) return;
    VGA_MEM[y*VGA_WIDTH+x] = og_entry(c, fg, bg);
}
static void og_puts(int x, int y, const char* s, uint8_t fg, uint8_t bg) {
    for (int i=0; s[i]; i++) og_put(x+i, y, s[i], fg, bg);
}
static void og_fill(int x, int y, int w, int h, char c, uint8_t fg, uint8_t bg) {
    for (int dy=0; dy<h; dy++)
        for (int dx=0; dx<w; dx++)
            og_put(x+dx, y+dy, c, fg, bg);
}
static void og_fill_row(int y, char c, uint8_t fg, uint8_t bg) {
    for (int x=0; x<VGA_WIDTH; x++) og_put(x, y, c, fg, bg);
}

// ─── Get apps for current view (All / Folder / Unorganized) ───────
// Returns the list of app launch_ids and count via pointers.
// "view_mode": 0 = All Apps, 1 = specific folder, 2 = Unorganized
static void get_current_apps(int* out_ids, int* out_count, int* view_mode) {
    if (sel_folder == 0) {
        // "All Apps" — show every app
        for (int i=0; i<ORG_APP_COUNT; i++) out_ids[i] = i;
        *out_count = ORG_APP_COUNT;
        *view_mode = 0;
    } else if (sel_folder <= org_folder_count) {
        // A specific custom folder
        OrgFolder* f = &org_folders[sel_folder-1];
        for (int i=0; i<f->app_count; i++) out_ids[i] = f->app_ids[i];
        *out_count = f->app_count;
        *view_mode = 1;
    } else {
        // Unorganized: apps not in any folder
        int n = 0;
        for (int i=0; i<ORG_APP_COUNT; i++)
            if (!app_is_in_any_folder(i)) out_ids[n++] = i;
        *out_count = n;
        *view_mode = 2;
    }
}

static void vga_draw(void) {
    og_fill(0,0,VGA_WIDTH,VGA_HEIGHT,' ',0,0);
    og_fill_row(0,' ',0,1);
    og_puts(2,0,"[ Application Organizer ]",15,1);
    // Count badge
    {
        int tmp[ORG_APP_COUNT];
        int cnt, vm;
        get_current_apps(tmp, &cnt, &vm);
        char cnts[8]; int p=0;
        int n=cnt; if(n==0){cnts[p++]='0';}else{char t[8];int tp=0;while(n>0){t[tp++]='0'+n%10;n/=10;}while(tp>0)cnts[p++]=t[--tp];}
        cnts[p]='\0';
        og_puts(56,0,cnts,14,1); og_puts(60,0,"apps",7,1);
    }
    og_puts(68,0,"ESC",7,1); og_puts(72,0,"back",8,1);

    // ── Left panel: folder list ──────────────────────────────────
    int lx=0, ly=2, lw=22, lh=21;
    og_fill(lx,ly,lw,lh,' ',7,0);
    // Border
    for (int x=lx; x<lx+lw; x++) { og_put(x,ly,'-',5,0); og_put(x,ly+lh-1,'-',5,0); }
    for (int y=ly; y<ly+lh; y++) { og_put(lx,y,'|',5,0); og_put(lx+lw-1,y,'|',5,0); }
    og_put(lx,ly,'+',5,0); og_put(lx+lw-1,ly,'+',5,0);
    og_put(lx,ly+lh-1,'+',5,0); og_put(lx+lw-1,ly+lh-1,'+',5,0);
    og_puts(lx+2,ly," Folders ",5,0);

    int ry=ly+1;
    // "All Apps" entry
    {
        uint8_t bg=(sel_folder==0 && active_panel==0)?5:0;
        uint8_t fg=(sel_folder==0 && active_panel==0)?15:7;
        og_fill(lx+1,ry,lw-2,1,' ',fg,bg);
        og_puts(lx+2,ry,active_panel==0 && sel_folder==0?">All Apps":" All Apps",fg,bg);
        ry++;
    }
    // Custom folders
    for (int i=0; i<org_folder_count; i++) {
        int idx = i+1; // +1 because 0 = "All Apps"
        uint8_t sel = (sel_folder==idx && active_panel==0);
        uint8_t bg = sel ? 5 : 0;
        uint8_t fg = sel ? 15 : org_folders[i].vga_col;
        og_fill(lx+1,ry,lw-2,1,' ',fg,bg);
        og_puts(lx+2,ry, sel ? ">" : " ", fg, bg);
        og_puts(lx+3,ry, org_folders[i].name, fg, bg);
        ry++;
    }
    // "Unorganized" entry
    {
        int idx = org_folder_count+1;
        uint8_t sel = (sel_folder==idx && active_panel==0);
        uint8_t bg = sel ? 5 : 0;
        uint8_t fg = sel ? 15 : 8;
        og_fill(lx+1,ry,lw-2,1,' ',fg,bg);
        og_puts(lx+2,ry,sel?">Unorg":" Unorg",fg,bg);
        ry++;
    }

    // ── Right panel: apps ────────────────────────────────────────
    int rx = lx+lw, rw = VGA_WIDTH-rx, ry2 = ly;

    // Get current app list
    int cur_ids[ORG_APP_COUNT];
    int cur_count, cur_vm;
    get_current_apps(cur_ids, &cur_count, &cur_vm);

    og_fill(rx,ry2,rw,lh,' ',0,0);
    // Title line
    const char* view_label = (cur_vm==0) ? "All Apps" :
                             (cur_vm==2) ? "Unorganized" : org_folders[sel_folder-1].name;
    og_fill(rx,ry2,rw,1,' ',7,8);
    og_puts(rx+1,ry2,view_label,15,8);
    // App grid (2 cols, up to 9 rows)
    int cols=2, rows=9;
    int card_w = rw/cols - 1;
    for (int i=0; i<cur_count && i<cols*rows; i++) {
        int col = i % cols, row = i / cols;
        int cx = rx + col*card_w;
        int cy = ry2 + 1 + row;
        int app_id = cur_ids[i];
        int sel = (active_panel==1 && i==sel_app_in);
        uint8_t bg = sel ? 1 : 0;
        uint8_t fg = sel ? 15 : 7;
        og_fill(cx,cy,card_w,1,' ',fg,bg);
        // Icon marker
        og_put(cx,cy, sel?'>':' ', org_app_vga[app_id], bg);
        // Name (truncate to fit)
        int max_nl = card_w-2;
        if (max_nl > (int)og_len(org_app_names[app_id]))
            max_nl = og_len(org_app_names[app_id]);
        for (int j=0; j<max_nl; j++)
            og_put(cx+1+j, cy, org_app_names[app_id][j], fg, bg);
    }

    // ── Bottom info bar ───────────────────────────────────────────
    int by = VGA_HEIGHT-1;
    og_fill_row(by,' ',0,8);
    og_puts(1,by,"TAB:panel",15,8);
    og_puts(12,by,"UP/DN:nav",15,8);
    og_puts(23,by,"ENTER:open",15,8);
    og_puts(35,by,"INS:folder",15,8);
    og_puts(47,by,"X:remove",15,8);
    og_puts(57,by,"R:rename",15,8);
    og_puts(67,by,"M:move",15,8);

    // ── Modal overlays ────────────────────────────────────────────
    if (modal_type == 1 || modal_type == 2) {
        const char* title = (modal_type==1) ? "Rename Folder" : "New Folder";
        og_fill(20,10,40,6,' ',15,4);
        for (int x=20; x<60; x++) { og_put(x,10,'-',14,4); og_put(x,15,'-',14,4); }
        for (int y=10; y<16; y++) { og_put(20,y,'|',14,4); og_put(59,y,'|',14,4); }
        og_put(20,10,'+',14,4); og_put(59,10,'+',14,4);
        og_put(20,15,'+',14,4); og_put(59,15,'+',14,4);
        og_puts(22,11,title,15,4);
        og_fill(22,12,36,1,' ',0,7);
        og_puts(22,12,modal_input,0,7);
        og_put(22+modal_pos,12,'_',0,15);
        og_puts(22,14,"ENTER:ok  ESC:cancel",7,4);
    }

    if (modal_type == 3) {
        // Move app dialog — pick destination folder
        og_fill(15,8,50,10,' ',15,1);
        for (int x=15; x<65; x++) { og_put(x,8,'-',14,1); og_put(x,17,'-',14,1); }
        for (int y=8; y<18; y++) { og_put(15,y,'|',14,1); og_put(64,y,'|',14,1); }
        og_put(15,8,'+',14,1); og_put(64,8,'+',14,1);
        og_put(15,17,'+',14,1); og_put(64,17,'+',14,1);
        og_puts(17,9,"Move app to folder:",15,1);
        og_puts(17,9+1,"ENTER:confirm  ESC:cancel",7,1);
        for (int i=0; i<org_folder_count && i<6; i++) {
            uint8_t bg = (i==modal_move_fol) ? 5 : 1;
            uint8_t fg = (i==modal_move_fol) ? 15 : org_folders[i].vga_col;
            og_fill(17,11+i,46,1,' ',fg,bg);
            og_puts(18,11+i, org_folders[i].name, fg, bg);
            if (folder_has_app(i, modal_move_app))
                og_puts(40,11+i,"(has it)",8,bg);
        }
    }

    if (modal_type == 4) {
        // Confirm delete
        og_fill(20,10,40,5,' ',15,4);
        for (int x=20; x<60; x++) { og_put(x,10,'-',12,4); og_put(x,14,'-',12,4); }
        for (int y=10; y<15; y++) { og_put(20,y,'|',12,4); og_put(59,y,'|',12,4); }
        og_put(20,10,'+',12,4); og_put(59,10,'+',12,4);
        og_put(20,14,'+',12,4); og_put(59,14,'+',12,4);
        og_puts(22,11,"Delete folder?",15,4);
        og_puts(22,12,"ENTER:yes  ESC:no",7,4);
    }
}

// ═══════════════════════════════════════════════════════════════════
//  VESA UI
// ═══════════════════════════════════════════════════════════════════

// Palette
#define OG_BG       0x0E0C22
#define OG_TITLE    0x1A1440
#define OG_FOLDERBG 0x120E28
#define OG_FOLDER   0x16122E
#define OG_FOLDER_S 0x2E2070
#define OG_APP_CARD 0x1A1838
#define OG_APP_SEL  0x26227A
#define OG_BORDER   0x2B2448
#define OG_ACCENT   0x9B7FFF
#define OG_TEXT     0xE0D8FF
#define OG_TEXTDIM  0x6858A0
#define OG_DIVIDER  0x201D38

// ── Left panel: draw folder list ──────────────────────────────────
static void draw_folder_list(int x, int y, int w, int h) {
    // Background
    vesa_fill_rect(x, y, w, h, OG_FOLDERBG);

    // Title
    vesa_fill_rect(x, y, w, 36, OG_FOLDER);
    vesa_hline(x, y+36, w, OG_DIVIDER);
    vesa_draw_string(x+14, y+10, "Folders", OG_TEXT, OG_FOLDER);

    // Count badge
    char cnts[8]; int p=0;
    int n=org_folder_count; if(n==0){cnts[p++]='0';}else{char t[8];int tp=0;while(n>0){t[tp++]='0'+n%10;n/=10;}while(tp>0)cnts[p++]=t[--tp];}
    cnts[p]='\0';
    int bl = og_len(cnts)*8+16;
    vesa_fill_rect(x+w-bl-10, y+8, bl, 22, OG_ACCENT);
    vesa_draw_rect(x+w-bl-10, y+8, bl, 22, 0xB088FF);
    vesa_draw_string(x+w-bl-10+8, y+11, cnts, 0xFFFFFF, OG_ACCENT);

    // Folder entries
    int ry = y + 44;
    int row_h = 36;

    // "All Apps" entry
    {
        int sel = (sel_folder==0 && active_panel==0);
        if (sel) {
            vesa_fill_rect(x+8, ry, w-16, row_h-4, OG_FOLDER_S);
            vesa_fill_rect(x+8, ry, 4, row_h-4, OG_ACCENT);
            vesa_draw_rect(x+8, ry, w-16, row_h-4, OG_ACCENT);
            // Pulsing glow ring (same as desktop icon glow)
            int pulse = (tick * 3) % 60;
            uint32_t glow = (pulse < 30) ? 0xB088FF : 0x6A4FBF;
            vesa_draw_rect(x+6, ry-2, w-12, row_h, glow);
        }
        uint32_t fg = sel ? OG_TEXT : OG_TEXTDIM;
        uint32_t ic = sel ? OG_TEXT : 0x9080C0;
        // Icon: a star-like symbol for "All"
        vesa_fill_rect(x+16, ry+6, 24, 24, sel ? 0x3A2090 : 0x1E1B40);
        vesa_draw_rect(x+16, ry+6, 24, 24, ic);
        vesa_draw_string(x+22, ry+10, "*", ic, sel ? 0x3A2090 : 0x1E1B40);
        // Name
        vesa_draw_string(x+50, ry+10, "All Apps", fg, sel ? OG_FOLDER_S : OG_FOLDERBG);
        ry += row_h;
    }

    // Custom folders
    for (int i=0; i<org_folder_count; i++) {
        int idx = i+1;
        int sel = (sel_folder==idx && active_panel==0);
        uint32_t bg = sel ? OG_FOLDER_S : (i%2==0 ? 0x0C0A1E : OG_FOLDERBG);
        if (sel) {
            vesa_fill_rect(x+8, ry, w-16, row_h-4, OG_FOLDER_S);
            vesa_fill_rect(x+8, ry, 4, row_h-4, org_folders[i].color);
            vesa_draw_rect(x+8, ry, w-16, row_h-4, org_folders[i].color);
            // Pulsing glow ring (same as desktop icon glow)
            int pulse = (tick * 3) % 60;
            uint32_t glow = (pulse < 30) ? 0xB088FF : 0x6A4FBF;
            vesa_draw_rect(x+6, ry-2, w-12, row_h, glow);
        }
        uint32_t fg = sel ? OG_TEXT : og_mix(org_folders[i].color, +80);
        // Folder icon
        uint32_t ic = org_folders[i].color;
        vesa_fill_rect(x+16, ry+6, 24, 24, sel ? og_mix(ic,-60) : 0x1E1B40);
        // Draw folder-like symbol
        vesa_hline(x+18, ry+8, 20, ic);
        vesa_hline(x+18, ry+26, 20, ic);
        vesa_vline(x+18, ry+8, 19, ic);
        vesa_vline(x+37, ry+8, 19, ic);
        vesa_hline(x+18, ry+16, 4, ic); // tab
        // Name
        vesa_draw_string(x+50, ry+10, org_folders[i].name, fg, sel ? OG_FOLDER_S : bg);
        // App count badge
        char ac[4]; int ap=0; int an=org_folders[i].app_count;
        if(an==0){ac[ap++]='0';}else{char t[8];int tp=0;while(an>0){t[tp++]='0'+an%10;an/=10;}while(tp>0)ac[ap++]=t[--tp];}
        ac[ap]='\0';
        vesa_draw_string(x+w-og_len(ac)*8-20, ry+10, ac, OG_TEXTDIM, sel ? OG_FOLDER_S : bg);
        ry += row_h;
    }

    // "Unorganized" entry
    {
        int idx = org_folder_count+1;
        int sel = (sel_folder==idx && active_panel==0);
        uint32_t bg = sel ? OG_FOLDER_S : OG_FOLDERBG;
        if (sel) {
            vesa_fill_rect(x+8, ry, w-16, row_h-4, OG_FOLDER_S);
            vesa_fill_rect(x+8, ry, 4, row_h-4, 0x888888);
            vesa_draw_rect(x+8, ry, w-16, row_h-4, 0x888888);
            // Pulsing glow ring
            int pulse = (tick * 3) % 60;
            uint32_t glow = (pulse < 30) ? 0xB088FF : 0x6A4FBF;
            vesa_draw_rect(x+6, ry-2, w-12, row_h, glow);
        }
        uint32_t fg = sel ? OG_TEXT : 0x706090;
        // Icon: question mark
        vesa_fill_rect(x+16, ry+6, 24, 24, sel ? 0x3A2A30 : 0x1E1B40);
        vesa_draw_rect(x+16, ry+6, 24, 24, fg);
        vesa_draw_string(x+22, ry+10, "?", fg, sel ? 0x3A2A30 : 0x1E1B40);
        // Name
        vesa_draw_string(x+50, ry+10, "Unorganized", fg, sel ? OG_FOLDER_S : bg);
        ry += row_h;
    }
}

// ── Draw a single app card ────────────────────────────────────────
static void draw_app_card(int x, int y, int w, int h, int app_id, int sel) {
    int hover = !sel && mouse_in_rect(x, y, w, h);
    uint32_t bg    = sel ? OG_APP_SEL : (hover ? 0x221E50 : OG_APP_CARD);
    uint32_t bord  = sel ? OG_ACCENT  : (hover ? 0x9B7FFF : OG_BORDER);
    uint32_t fg    = sel ? OG_TEXT    : 0xC8B8E8;
    uint32_t acol  = org_app_colors[app_id];

    // Card body
    vesa_fill_rect(x, y, w, h, bg);
    vesa_draw_rect(x, y, w, h, bord);
    if (sel) {
        vesa_fill_rect(x+2, y, w-4, 2, acol);
        vesa_draw_rect(x-1, y-1, w+2, h+2, og_mix(bord,+40));
        // Pulsing glow ring (same as desktop icon glow)
        int pulse = (tick * 3) % 60;
        uint32_t glow = (pulse < 30) ? 0xB088FF : 0x6A4FBF;
        vesa_draw_rect(x-2, y-2, w+4, h+4, glow);
        vesa_put_pixel(x-3, y-3, glow);
        vesa_put_pixel(x+w+2, y-3, glow);
        vesa_put_pixel(x-3, y+h+2, glow);
        vesa_put_pixel(x+w+2, y+h+2, glow);
    } else if (hover) {
        vesa_draw_rect(x-1, y-1, w+2, h+2, 0x6A4FBF);
        vesa_fill_rect(x+2, y, w-4, 1, acol);
    }

    // Icon box (32x32)
    int icon_sz = 32;
    int ix = x+10, iy = y+8;
    vesa_fill_rect(ix, iy, icon_sz, icon_sz, acol);
    vesa_fill_rect(ix, iy, icon_sz, 1, og_mix(acol,+40));
    vesa_fill_rect(ix, iy, 1, icon_sz, og_mix(acol,+24));
    vesa_draw_rect(ix, iy, icon_sz, icon_sz, og_mix(acol,+60));
    // First char of app name as icon (single char only to fit 32x32 box)
    char icon_str[2] = { org_app_names[app_id][0], '\0' };
    vesa_draw_string(ix+12, iy+8, icon_str, 0xFFFFFF, acol);

    // App name (right of icon)
    int tx = ix + icon_sz + 12;
    int max_nl = (w - (tx - x + 8)) / 8;
    if (max_nl < 3) max_nl = 3;
    char disp[17]; og_cpy(disp, org_app_names[app_id], max_nl+1);
    vesa_draw_string(tx, y+12, disp, fg, bg);

    // Bottom: accent bar with app index hint
    char idx_str[4];
    idx_str[0] = '#';
    int n=app_id+1; if(n>9){idx_str[1]='1';idx_str[2]='0'+(n-10);idx_str[3]='\0';}
    else { idx_str[1]='0'+n; idx_str[2]='\0'; }
    vesa_draw_string(x+w-og_len(idx_str)*8-12, y+h-18, idx_str, OG_TEXTDIM, bg);
}

// ── Right panel: draw app grid ────────────────────────────────────
static void draw_app_grid(int x, int y, int w, int h) {
    // Background
    vesa_fill_rect(x, y, w, h, OG_BG);

    // Get current app list
    int cur_ids[ORG_APP_COUNT];
    int cur_count, cur_vm;
    get_current_apps(cur_ids, &cur_count, &cur_vm);

    // Panel header
    const char* view_label = (cur_vm==0) ? "All Apps" :
                             (cur_vm==2) ? "Unorganized" :
                             org_folders[sel_folder-1].name;
    vesa_fill_rect(x, y, w, 36, OG_TITLE);
    vesa_hline(x, y+36, w, OG_BORDER);

    // View label
    vesa_draw_string(x+16, y+10, view_label, OG_TEXT, OG_TITLE);

    // App count
    char cnts[8]; int p=0;
    int n=cur_count; if(n==0){cnts[p++]='0';}else{char t[8];int tp=0;while(n>0){t[tp++]='0'+n%10;n/=10;}while(tp>0)cnts[p++]=t[--tp];}
    cnts[p]='\0';
    int bl = og_len(cnts)*8+8;
    vesa_draw_string(x+w-bl-16, y+10, cnts, OG_ACCENT, OG_TITLE);
    vesa_draw_string(x+w-bl-16-og_len("apps")*8-4, y+10, "apps", OG_TEXTDIM, OG_TITLE);

    // "New Folder" button in header
    {
        int btn_x = x+w/2-60;
        vesa_draw_rect(btn_x, y+6, 120, 24, OG_BORDER);
        vesa_draw_string(btn_x+12, y+10, "+ New Folder", 0x55EE88, OG_TITLE);
    }

    if (cur_count == 0) {
        // Empty state
        int mx = x+w/2-100, my = y+h/2-24;
        vesa_draw_string(mx, my, "No apps here yet", OG_TEXTDIM, OG_BG);
        vesa_draw_string(mx, my+24, "Press M to move apps here", OG_TEXTDIM, OG_BG);
        return;
    }

    // App grid: 3 columns, variable rows
    int cols = 3;
    int card_gap = 8;
    int card_w  = (w - 2*16 - (cols-1)*card_gap) / cols;
    if (card_w < 140) card_w = 140;
    int card_h  = 56;
    int avail_h = h - 36 - 40; // header + bottom padding
    int rows_vis = avail_h / (card_h + card_gap);
    if (rows_vis < 2) rows_vis = 2;
    int grid_h  = rows_vis * (card_h + card_gap);

    // Clamp scroll
    int total_rows = (cur_count + cols - 1) / cols;
    if (app_scroll > total_rows - rows_vis && total_rows > rows_vis)
        app_scroll = total_rows - rows_vis;
    if (app_scroll < 0) app_scroll = 0;

    int gy = y + 44;
    int rendered = 0;
    for (int row = 0; row < rows_vis; row++) {
        for (int col = 0; col < cols; col++) {
            int idx = (app_scroll + row) * cols + col;
            if (idx >= cur_count) break;
            int cx = x + 16 + col * (card_w + card_gap);
            int cy = gy + row * (card_h + card_gap);
            int app_id = cur_ids[idx];
            int sel = (active_panel==1 && idx == sel_app_in);
            draw_app_card(cx, cy, card_w, card_h, app_id, sel);
            rendered++;
        }
    }

    // Scrollbar (if needed)
    if (total_rows > rows_vis) {
        int sb_x = x + w - 6;
        int sb_h = grid_h;
        vesa_vline(sb_x, gy, sb_h, OG_DIVIDER);
        int bar_h = sb_h * rows_vis / total_rows;
        if (bar_h < 12) bar_h = 12;
        int bar_y = gy + (sb_h - bar_h) * app_scroll / (total_rows - rows_vis);
        vesa_fill_rect(sb_x-1, bar_y, 3, bar_h, OG_ACCENT);
    }

    // Bottom hint bar
    int hint_y = y + h - 28;
    vesa_hline(x+8, hint_y-8, w-16, OG_DIVIDER);
    const char* hints = "TAB switch panel   UP/DN navigate   ENTER launch   R rename   M move   INS folder   DEL delete";
    vesa_draw_string(x+16, hint_y, hints, OG_TEXTDIM, OG_BG);
}

// ── Draw modal dialogs ────────────────────────────────────────────
static void draw_modals(void) {
    if (modal_type == 0) return;
    int W = (int)vesa.width, H = (int)vesa.height;

    // Dimmed overlay (purple tint, matching OS theme)
    vesa_fill_rect(0, 0, W, H, 0x0C0824); // semi-transparent dark

    int dw = 420, dh = 160;
    int dx = (W-dw)/2, dy = (H-dh)/2;

    if (modal_type == 1 || modal_type == 2) {
        // Rename / Create folder dialog
        const char* title = (modal_type==1) ? "Rename Folder" : "New Folder";

        vesa_fill_rect(dx, dy, dw, dh, 0x1A1440);
        vesa_draw_rect(dx, dy, dw, dh, OG_BORDER);
        vesa_fill_rect(dx+1, dy+1, dw-2, 4, OG_ACCENT);

        // Title
        vesa_draw_string(dx+20, dy+16, title, OG_TEXT, 0x1A1440);

        // Input field
        int ifx = dx+20, ify = dy+50, ifw = dw-40, ifh = 36;
        vesa_fill_rect(ifx, ify, ifw, ifh, 0x0E0C22);
        vesa_draw_rect(ifx, ify, ifw, ifh, OG_ACCENT);
        vesa_draw_string(ifx+12, ify+10, modal_input, OG_TEXT, 0x0E0C22);
        // Cursor
        int cur_x = ifx+12 + modal_pos*8;
        vesa_vline(cur_x, ify+8, 20, 0xFFFFFF);

        // Buttons
        int btn_y = dy+100;
        // OK button
        vesa_fill_rect(dx+20, btn_y, 100, 32, 0x1A5A20);
        vesa_draw_rect(dx+20, btn_y, 100, 32, 0x40CC50);
        vesa_draw_string(dx+40, btn_y+8, "OK", 0x88FF88, 0x1A5A20);
        // Cancel button
        vesa_fill_rect(dx+140, btn_y, 120, 32, 0x3A1818);
        vesa_draw_rect(dx+140, btn_y, 120, 32, 0xCC4040);
        vesa_draw_string(dx+160, btn_y+8, "Cancel", 0xFF8888, 0x3A1818);
    }

    if (modal_type == 3) {
        // Move app to folder dialog
        vesa_fill_rect(dx, dy, dw, dh+40, 0x1A1440);
        vesa_draw_rect(dx, dy, dw, dh+40, OG_BORDER);
        vesa_fill_rect(dx+1, dy+1, dw-2, 4, 0x55EE88);

        vesa_draw_string(dx+20, dy+16, "Move app to folder:", OG_TEXT, 0x1A1440);

        int my = dy+50;
        for (int i=0; i<org_folder_count && i<6; i++) {
            int sel = (i==modal_move_fol);
            uint32_t bg = sel ? OG_FOLDER_S : 0x0E0C22;
            vesa_fill_rect(dx+20, my, dw-40, 28, bg);
            if (sel) vesa_fill_rect(dx+20, my, 3, 28, org_folders[i].color);
            uint32_t fg = sel ? OG_TEXT : og_mix(org_folders[i].color, +80);
            vesa_draw_string(dx+36, my+6, org_folders[i].name, fg, bg);
            if (folder_has_app(i, modal_move_app))
                vesa_draw_string(dx+dw-100, my+6, "(has it)", OG_TEXTDIM, bg);
            my += 34;
        }

        // Hint
        vesa_draw_string(dx+20, dy+dh-12,
            "UP/DOWN: select   ENTER: confirm   ESC: cancel", OG_TEXTDIM, 0x1A1440);
    }

    if (modal_type == 4) {
        // Confirm delete dialog
        vesa_fill_rect(dx, dy, dw, dh-40, 0x3A1010);
        vesa_draw_rect(dx, dy, dw, dh-40, 0xFF4444);
        vesa_fill_rect(dx+1, dy+1, dw-2, 4, 0xFF4444);

        vesa_draw_string(dx+20, dy+20, "Delete this folder?", 0xFF8888, 0x3A1010);
        vesa_draw_string(dx+20, dy+44, "Apps inside will become", OG_TEXTDIM, 0x3A1010);
        vesa_draw_string(dx+20, dy+64, "\"Unorganized\".", OG_TEXTDIM, 0x3A1010);

        // Yes button
        vesa_fill_rect(dx+20, dy+90, 100, 32, 0x5A1010);
        vesa_draw_rect(dx+20, dy+90, 100, 32, 0xFF4444);
        vesa_draw_string(dx+48, dy+98, "Yes", 0xFF6666, 0x5A1010);
        // No button
        vesa_fill_rect(dx+140, dy+90, 100, 32, 0x1A1A30);
        vesa_draw_rect(dx+140, dy+90, 100, 32, OG_BORDER);
        vesa_draw_string(dx+168, dy+98, "No", OG_TEXT, 0x1A1A30);
    }
}

// ── Main VESA draw ────────────────────────────────────────────────
static void vesa_draw(void) {
    int W = (int)vesa.width, H = (int)vesa.height;
    int title_h = 36;

    // ── Window position (preserves dragged position) ──────────────
    int wx = (g_win_x >= 0) ? g_win_x : 16;
    int wy = (g_win_y >= 0) ? g_win_y : 40;
    int ww = W - 32;
    int wh = H - wy - 16;
    g_win_x = wx; g_win_y = wy; g_win_w = ww; g_win_h = wh;

    // Full backdrop so the drag trail shows a solid panel
    vesa_fill_rect(wx, wy, ww, wh, OG_BG);

    // ── Title bar ─────────────────────────────────────────────────
    vesa_fill_rect(wx, wy, ww, title_h, OG_TITLE);
    vesa_hline(wx, wy + title_h, ww, OG_BORDER);
    vesa_draw_string(wx + 16, wy + 10, "Application Organizer", OG_TEXT, OG_TITLE);
    draw_close_btn(wx + ww - 40, wy + 10, OG_TITLE, "Organizer");
    vesa_draw_string(wx + ww - 112, wy + 10, "ESC back", OG_TEXTDIM, OG_TITLE);

    // ── Panels ─────────────────────────────────────────────────────
    int panel_y = wy + title_h;
    int panel_h = wh - title_h - 28;

    // Left panel: folder list (32% width)
    int left_w = ww * 32 / 100;
    if (left_w < 180) left_w = 180;
    if (left_w > 320) left_w = 320;

    // Right panel: app grid (remaining width)
    int right_x = wx + left_w;
    int right_w = ww - left_w;

    draw_folder_list(wx, panel_y, left_w, panel_h);
    draw_app_grid(right_x, panel_y, right_w, panel_h);

    // ── Panel indicator ────────────────────────────────────────────
    if (active_panel == 0) {
        vesa_vline(wx + left_w - 1, panel_y, panel_h, OG_ACCENT);
    } else {
        vesa_vline(right_x, panel_y, panel_h, OG_ACCENT);
    }

    // ── Status bar ────────────────────────────────────────────────
    int sb_y = wy + wh - 28;
    vesa_fill_rect(wx, sb_y, ww, 28, OG_TITLE);
    vesa_hline(0, sb_y, W, OG_BORDER);

    const char* status_hint;
    if (modal_type > 0) {
        status_hint = "ENTER: confirm   ESC: cancel   Type to edit name";
    } else if (active_panel == 0) {
        status_hint = "UP/DN: navigate folders   TAB: switch to apps   ENTER: open folder   INS: new folder   R: rename   X: delete";
    } else {
        status_hint = "UP/DN/LEFT/RT: navigate apps   TAB: switch to folders   ENTER: launch app   M: move app   ESC: back";
    }
    vesa_draw_string(16, sb_y+6, status_hint, OG_TEXTDIM, OG_TITLE);

    // ── Modals (drawn on top) ─────────────────────────────────────
    draw_modals();
}

// ═══════════════════════════════════════════════════════════════════
//  PUBLIC DRAW
// ═══════════════════════════════════════════════════════════════════

void organizer_draw(void) {
    if (vesa.active) { vesa_draw(); return; }
    vga_draw();
}

// ═══════════════════════════════════════════════════════════════════
//  KEY HANDLER
// ═══════════════════════════════════════════════════════════════════

void organizer_handle_key(char c) {
    // ── Modal input ───────────────────────────────────────────────
    if (modal_type == 1 || modal_type == 2) {
        if (c == 27) { // ESC = cancel
            modal_type = 0;
            modal_input[0] = '\0';
            modal_pos = 0;
            
            return;
        }
        if (c == 13) { // ENTER = confirm
            if (modal_input[0]) {
                if (modal_type == 1) {
                    // Rename current folder
                    int fidx = sel_folder - 1;
                    if (fidx >= 0 && fidx < org_folder_count)
                        organizer_rename_folder(fidx, modal_input);
                    desktop_set_status("Renamed!");
                } else {
                    // Create new folder
                    organizer_add_folder(modal_input);
                    sel_folder = org_folder_count; // auto-select new folder
                    desktop_set_status("Folder created!");
                }
            }
            modal_type = 0;
            modal_input[0] = '\0';
            modal_pos = 0;
            return;
        }
        if (c == 8 && modal_pos > 0) { // Backspace
            modal_input[--modal_pos] = '\0';
            return;
        }
        if (c >= 32 && c < 127 && modal_pos < 15) { // Typing
            modal_input[modal_pos++] = c;
            modal_input[modal_pos] = '\0';
            return;
        }
        return;
    }

    if (modal_type == 3) { // Move app dialog
        if (c == 27) { // ESC = cancel
            modal_type = 0;
            
            return;
        }
        if (c == 13) { // ENTER = confirm move
            // Move modal_move_app to folder modal_move_fol
            // First find which folder (if any) currently has this app and remove it
            for (int f=0; f<org_folder_count; f++) {
                for (int a=0; a<org_folders[f].app_count; a++) {
                    if (org_folders[f].app_ids[a] == modal_move_app) {
                        organizer_remove_app_from_folder(f, a);
                        break;
                    }
                }
            }
            // Add to target folder
            if (modal_move_fol >= 0 && modal_move_fol < org_folder_count)
                organizer_add_app_to_folder(modal_move_fol, modal_move_app);
            modal_type = 0;
            sel_app_in = 0;
            desktop_set_status("App moved!");
            return;
        }
        if (c == 'B' && modal_move_fol < org_folder_count-1) { // DOWN
            modal_move_fol++;
            
            return;
        }
        if (c == 'A' && modal_move_fol > 0) { // UP
            modal_move_fol--;
            
            return;
        }
        return;
    }

    if (modal_type == 4) { // Confirm delete
        if (c == 27) { // ESC / No
            modal_type = 0;
            
            return;
        }
        if (c == 13) { // ENTER / Yes
            // Move apps from deleted folder to unorganized
            int fidx = sel_folder - 1;
            if (fidx >= 0 && fidx < org_folder_count)
                organizer_remove_folder(fidx);
            modal_type = 0;
            if (sel_folder >= org_folder_count+1)
                sel_folder = 0;
            sel_app_in = 0;
            desktop_set_status("Folder deleted!");
            return;
        }
        return;
    }

    // ── Normal navigation ─────────────────────────────────────────

    // TAB: Switch panels
    if (c == '\t') {
        active_panel = !active_panel;
        sel_app_in = 0;
        
        return;
    }

    if (active_panel == 0) {
        // ── Left panel: folder navigation ─────────────────────────
        int total_items = 1 + org_folder_count + 1; // All + folders + Unorg

        if (c == 'B') { // DOWN
            if (sel_folder < total_items - 1) {
                sel_folder++;
                sel_app_in = 0;
                
            }
            return;
        }
        if (c == 'A') { // UP
            if (sel_folder > 0) {
                sel_folder--;
                sel_app_in = 0;
                
            }
            return;
        }
        if (c == 13) { // ENTER = open folder (switches to app panel)
            active_panel = 1;
            sel_app_in = 0;
            app_scroll = 0;
            
            return;
        }

        // INS = new folder
        if (c == 'I' || c == 'i') {
            modal_type = 2; // create
            modal_input[0] = '\0';
            modal_pos = 0;
            return;
        }

        // X = delete folder (using X to avoid collision with left-arrow 'D')
        if (c == 'X' || c == 'x') {
            if (sel_folder > 0 && sel_folder <= org_folder_count) {
                modal_type = 4; // confirm delete
                modal_input[0] = '\0';
            }
            return;
        }

        // R = rename folder
        if (c == 'r' || c == 'R') {
            if (sel_folder > 0 && sel_folder <= org_folder_count) {
                modal_type = 1; // rename
                og_cpy(modal_input, org_folders[sel_folder-1].name, 16);
                modal_pos = og_len(modal_input);
            }
            return;
        }
    } else {
        // ── Right panel: app grid navigation ──────────────────────
        int cur_ids[ORG_APP_COUNT];
        int cur_count, cur_vm;
        get_current_apps(cur_ids, &cur_count, &cur_vm);
        int cols = 3;

        if (cur_count == 0) return;

        if (c == 'B') { // DOWN
            if (sel_app_in + cols < cur_count) {
                sel_app_in += cols;
                // Clamp scroll
                int cur_row = sel_app_in / cols;
                int rows_vis = 5; // approximate
                if (cur_row >= app_scroll + rows_vis)
                    app_scroll = cur_row - rows_vis + 1;
                
            }
            return;
        }
        if (c == 'A') { // UP
            if (sel_app_in - cols >= 0) {
                sel_app_in -= cols;
                int cur_row = sel_app_in / cols;
                if (cur_row < app_scroll)
                    app_scroll = cur_row;
                
            }
            return;
        }
        if (c == 'C') { // RIGHT
            if (sel_app_in + 1 < cur_count) {
                sel_app_in++;
                int cur_row = sel_app_in / cols;
                int rows_vis = 5;
                if (cur_row >= app_scroll + rows_vis)
                    app_scroll = cur_row - rows_vis + 1;
                
            }
            return;
        }
        if (c == 'D') { // LEFT
            if (sel_app_in > 0) {
                sel_app_in--;
                int cur_row = sel_app_in / cols;
                if (cur_row < app_scroll)
                    app_scroll = cur_row;
                
            }
            return;
        }

        if (c == 13) { // ENTER = launch app
            if (sel_app_in < cur_count) {
                int app_id = cur_ids[sel_app_in];
                modal_type = 0;
                desktop_set_status("Opening...");
                desktop_launch(app_id);
            }
            return;
        }

        // M = move app
        if (c == 'm' || c == 'M') {
            if (sel_app_in < cur_count) {
                modal_type = 3;
                modal_move_app = cur_ids[sel_app_in];
                modal_move_fol = 0;
            }
            return;
        }
    }

    // ESC = back to desktop (handled by desktop.c dispatch)
}

// ═══════════════════════════════════════════════════════════════════
//  MOUSE HANDLER
// ═══════════════════════════════════════════════════════════════════

int organizer_handle_mouse(int mx, int my) {
    if (!vesa.active) return 0;  // VGA text mode — no pixel coords

    int W = (int)vesa.width, H = (int)vesa.height;
    int title_h = 36;
    int panel_y = title_h;
    int panel_h = H - title_h - 28;

    // Left panel size
    int left_w = W * 32 / 100;
    if (left_w < 180) left_w = 180;
    if (left_w > 320) left_w = 320;

    int right_x = left_w;
    int right_w = W - right_x;

    // ── Modal clicks (highest priority) ───────────────────────────
    int dw = 420, dh = 160;
    int dx = (W-dw)/2, dy = (H-dh)/2;

    if (modal_type == 1 || modal_type == 2) {
        // Rename / Create folder dialog
        // OK button
        if (mx >= dx+20 && mx < dx+120 && my >= dy+100 && my < dy+132) {
            if (modal_input[0]) {
                if (modal_type == 1) {
                    int fidx = sel_folder - 1;
                    if (fidx >= 0 && fidx < org_folder_count)
                        organizer_rename_folder(fidx, modal_input);
                    desktop_set_status("Renamed!");
                } else {
                    organizer_add_folder(modal_input);
                    sel_folder = org_folder_count;
                    desktop_set_status("Folder created!");
                }
            }
            modal_type = 0;
            modal_input[0] = '\0';
            modal_pos = 0;
            return 1;
        }
        // Cancel button
        if (mx >= dx+140 && mx < dx+260 && my >= dy+100 && my < dy+132) {
            modal_type = 0;
            modal_input[0] = '\0';
            modal_pos = 0;
            return 1;
        }
        // Click on input field — focus by setting cursor at end
        if (mx >= dx+20 && mx < dx+dw-20 && my >= dy+50 && my < dy+86) {
            modal_pos = og_len(modal_input);
            return 1;
        }
        return 1; // any click in modal dismisses keyboard-only actions
    }

    if (modal_type == 3) {
        // Move app dialog — folder selection
        int my0 = dy + 50;
        for (int i=0; i<org_folder_count && i<6; i++) {
            int fy = my0 + i * 34;
            if (mx >= dx+20 && mx < dx+dw-20 && my >= fy && my < fy+28) {
                if (i == modal_move_fol) {
                    // Click same folder again = confirm
                    for (int f=0; f<org_folder_count; f++) {
                        for (int a=0; a<org_folders[f].app_count; a++) {
                            if (org_folders[f].app_ids[a] == modal_move_app) {
                                organizer_remove_app_from_folder(f, a);
                                break;
                            }
                        }
                    }
                    if (modal_move_fol >= 0 && modal_move_fol < org_folder_count)
                        organizer_add_app_to_folder(modal_move_fol, modal_move_app);
                    modal_type = 0;
                    sel_app_in = 0;
                    desktop_set_status("App moved!");
                    return 1;
                }
                modal_move_fol = i;
                return 1;
            }
        }
        // Click outside any row = cancel
        if (mx < dx || mx >= dx+dw || my < dy || my >= dy+dh+40) {
            modal_type = 0;
            return 1;
        }
        return 1;
    }

    if (modal_type == 4) {
        // Confirm delete dialog
        // Yes button
        if (mx >= dx+20 && mx < dx+120 && my >= dy+90 && my < dy+122) {
            int fidx = sel_folder - 1;
            if (fidx >= 0 && fidx < org_folder_count)
                organizer_remove_folder(fidx);
            modal_type = 0;
            if (sel_folder >= org_folder_count+1)
                sel_folder = 0;
            sel_app_in = 0;
            desktop_set_status("Folder deleted!");
            return 1;
        }
        // No button
        if (mx >= dx+140 && mx < dx+240 && my >= dy+90 && my < dy+122) {
            modal_type = 0;
            return 1;
        }
        return 1; // any modal click dismisses
    }

    // ── Title bar — ignore (ESC back is handled by desktop dispatch) ─
    if (my < title_h) return 1;

    // ── Status bar — ignore ───────────────────────────────────────
    if (my >= H - 28) return 1;

    // ── Left panel: folder list clicks ────────────────────────────
    if (mx < left_w && mx >= 0) {
        // Folder entries start at ry = panel_y + 44, row_h = 36
        int ry = panel_y + 44;
        int row_h = 36;
        int total_items = 1 + org_folder_count + 1; // All + folders + Unorg

        for (int idx = 0; idx < total_items; idx++) {
            int entry_y = ry + idx * row_h;
            // Entry bounds (within the panel, with 8px side padding)
            if (my >= entry_y && my < entry_y + row_h) {
                sel_folder = idx;
                sel_app_in = 0;
                app_scroll = 0;
                // Switch to app panel (same as keyboard ENTER behavior)
                active_panel = 1;
                return 1;
            }
        }
        return 1;
    }

    // ── Right panel: header area (New Folder button) ──────────────
    if (mx >= right_x && mx < right_x + right_w) {
        // Header is at y = panel_y, h = 36
        if (my >= panel_y && my < panel_y + 36) {
            // "New Folder" button
            int btn_x = right_x + right_w/2 - 60;
            if (mx >= btn_x && mx < btn_x + 120 && my >= panel_y+6 && my < panel_y+30) {
                modal_type = 2;
                modal_input[0] = '\0';
                modal_pos = 0;
                return 1;
            }
            return 1;
        }
    }

    // ── Right panel: app grid clicks ──────────────────────────────
    if (mx >= right_x && mx < right_x + right_w && my >= panel_y + 44) {
        int cur_ids[ORG_APP_COUNT];
        int cur_count, cur_vm;
        get_current_apps(cur_ids, &cur_count, &cur_vm);
        if (cur_count == 0) return 1;

        int cols = 3;
        int card_gap = 8;
        int card_w  = (right_w - 2*16 - (cols-1)*card_gap) / cols;
        if (card_w < 140) card_w = 140;
        int card_h  = 56;
        int avail_h = panel_h - 36 - 40;
        int rows_vis = avail_h / (card_h + card_gap);
        if (rows_vis < 2) rows_vis = 2;

        int gy = panel_y + 44;

        for (int row = 0; row < rows_vis; row++) {
            for (int col = 0; col < cols; col++) {
                int idx = (app_scroll + row) * cols + col;
                if (idx >= cur_count) break;
                int cx = right_x + 16 + col * (card_w + card_gap);
                int cy = gy + row * (card_h + card_gap);
                if (mx >= cx && mx < cx + card_w && my >= cy && my < cy + card_h) {
                    sel_app_in = idx;
                    active_panel = 1;
                    // Launch the app (same as keyboard ENTER)
                    int app_id = cur_ids[idx];
                    modal_type = 0;
                    desktop_set_status("Opening...");
                    desktop_launch(app_id);
                    return 1;
                }
            }
        }
        return 1;
    }

    return 0;
}
