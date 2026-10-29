// cpu/window.c  –  Nyx OS native window manager (see include/window.h).
//
// A small, allocation-free window table.  Each open app window has a
// mode, a display title, an accent color, and an MRU position; the
// window at index 0 is the most recently focused.  The taskbar tray
// and the mouse hit-testing consume this table, so the desktop's
// "which apps are open / which is focused" answers all come from one
// place instead of being re-derived per app.
#include <window.h>
#include <desktop.h>
#include <stdint.h>
#include <stddef.h>

typedef struct win_entry {
    int  used;
    int  mode;          // MODE_* of the app
    int  focused;       // 1 = active window
    int  x, y, w, h;    // last known window rect (for hit-testing)
} win_entry_t;

static win_entry_t wins[WIN_MAX];

static int  win_n   = 0;   // open app windows
static int  focused = MODE_DESKTOP;   // MODE_DESKTOP = none

void win_init(void) {
    for (int i = 0; i < WIN_MAX; i++) wins[i].used = 0;
    win_n = 0;
    focused = MODE_DESKTOP;
}

// ─── Mode → display name / accent (single source of truth) ───────
const char* win_title(int mode) {
    switch (mode) {
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
        default:                 return "App";
    }
}

uint32_t win_accent(int mode) {
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
        default:                 return 0x8A6FBF;
    }
}

// ─── Table management (MRU order, index 0 = most recent) ─────────
static int win_find(int mode) {
    for (int i = 0; i < WIN_MAX; i++)
        if (wins[i].used && wins[i].mode == mode) return i;
    return -1;
}

// Move entry `i` to index 0 (bubble the rest down).
static void win_bubble(int i) {
    win_entry_t t = wins[i];
    for (int j = i; j > 0; j--) wins[j] = wins[j - 1];
    wins[0] = t;
}

int win_open(int mode) {
    if (mode <= MODE_DESKTOP) return -1;         // desktop is not a window
    int i = win_find(mode);
    if (i >= 0) {                                // already open → just focus
        win_bubble(i);
        focused = mode;
        for (int k = 0; k < WIN_MAX; k++) wins[k].focused = (wins[k].used && wins[k].mode == mode);
        return 0;
    }
    if (win_n >= WIN_MAX) return -1;             // table full
    int slot = -1;
    for (int k = WIN_MAX - 1; k >= 0; k--)
        if (!wins[k].used) { slot = k; break; }
    // Insert at the FRONT of the used region: shift used entries down.
    for (int k = slot; k > 0; k--) wins[k] = wins[k - 1];
    wins[0].used = 1;
    wins[0].mode = mode;
    wins[0].focused = 1;
    wins[0].x = wins[0].y = wins[0].w = wins[0].h = 0;
    win_n++;
    focused = mode;
    for (int k = 1; k < WIN_MAX; k++)
        if (wins[k].used) wins[k].focused = 0;
    return 0;
}

// Remove `mode` and return the mode that should now be shown on
// screen: the next open window (Windows-like — closing the focused
// window brings the next one to front) or MODE_DESKTOP when none
// remain.
int win_close(int mode) {
    int i = win_find(mode);
    if (i < 0) return MODE_DESKTOP;
    for (int j = i; j < WIN_MAX - 1; j++) wins[j] = wins[j + 1];
    wins[WIN_MAX - 1].used = 0;
    win_n--;
    if (focused != mode) return win_focused_mode();
    if (win_n == 0) {
        focused = MODE_DESKTOP;
        return MODE_DESKTOP;
    }
    // Focus the new MRU front (index 0 is the most recently used
    // among the remaining windows).
    focused = wins[0].mode;
    wins[0].focused = 1;
    for (int k = 1; k < WIN_MAX; k++) wins[k].focused = 0;
    return focused;
}

void win_focus(int mode) {
    if (mode <= MODE_DESKTOP) {                  // desktop clears focus
        focused = MODE_DESKTOP;
        for (int k = 0; k < WIN_MAX; k++) wins[k].focused = 0;
        return;
    }
    int i = win_find(mode);
    if (i < 0) { win_open(mode); return; }       // not open → open it
    win_bubble(i);
    focused = mode;
    for (int k = 0; k < WIN_MAX; k++)
        wins[k].focused = (wins[k].used && wins[k].mode == mode);
}

int win_count(void) { return win_n; }

int win_mode_at(int i) {
    if (i < 0 || i >= WIN_MAX || !wins[i].used) return -1;
    return wins[i].mode;
}

int win_is_focused(int mode) {
    int i = win_find(mode);
    return (i >= 0) ? wins[i].focused : 0;
}

int win_focused_mode(void) { return focused; }

void win_sync_rect(int mode, int x, int y, int w, int h) {
    int i = win_find(mode);
    if (i < 0) return;
    if (x >= 0 && w > 0) { wins[i].x = x; wins[i].w = w; }
    if (y >= 0 && h > 0) { wins[i].y = y; wins[i].h = h; }
}

int win_close_btn_hit(int x, int y) {
    int m = focused;
    if (m <= MODE_DESKTOP) return -1;
    int i = win_find(m);
    if (i < 0) return -1;
    // Fall back to a sane rect if the window never synced one.
    int wx = wins[i].w > 0 ? wins[i].x : 0;
    int wy = wins[i].h > 0 ? wins[i].y : 0;
    int ww = wins[i].w > 0 ? wins[i].w : 0;
    if (ww <= 0) return -1;
    int in_x = (x >= wx + ww - 40 && x < wx + ww - 4);
    int in_y_self  = (y >= wy + 4  && y < wy + 38);
    int in_y_compo = (y >= wy - 28 && y < wy - 4);
    return (in_x && (in_y_self || in_y_compo)) ? m : -1;
}
