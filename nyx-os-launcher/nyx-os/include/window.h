// include/window.h  –  Nyx OS native window manager (the kernel's
// GUI server).
//
// The desktop compositor draws one app at a time (current_mode), but
// the WINDOW MANAGER owns the state of every open app window: its
// mode, display title, accent color, focus, and MRU (most-recently-
// used) ordering.  The taskbar's app tray renders win_count()/
// win_mode_at()/win_title()/win_accent(), and mouse hit-testing
// (win_close_btn_hit, tray buttons) is centralized here instead of
// being re-derived per app.  This is the in-kernel equivalent of the
// "native GUI server" a widget backend translates window creation,
// clicks, and focus to.
#ifndef WINDOW_H
#define WINDOW_H

#include <stdint.h>

#define WIN_MAX 12      // max concurrently open app windows
#define WIN_TRAY_MAX 8  // tray buttons the taskbar renders

void win_init(void);

// Register `mode` as an open app window (no-op if already open).
// MODE_DESKTOP is never a window.  New windows become focused.
int  win_open(int mode);

// Remove `mode` from the window table (an app was closed) and return
// the mode that should now be shown: the next open window, or
// MODE_DESKTOP when none remain.
int win_close(int mode);

// Raise `mode` to the front of the MRU list and make it focused.
// MODE_DESKTOP clears focus (no app window active).
void win_focus(int mode);

// Number of open app windows (MODE_DESKTOP excluded).
int  win_count(void);

// Mode of the i-th open window, most-recently-used first.
int  win_mode_at(int i);

// Is `mode` the currently focused window?
int  win_is_focused(int mode);

// Mode of the focused window, or MODE_DESKTOP when none is.
int  win_focused_mode(void);

// Display title and accent color for `mode` (single source of truth
// for the taskbar tray and window chrome).
const char* win_title(int mode);
uint32_t    win_accent(int mode);

// Keep the WM's copy of the window rect in sync (called from
// finish_frame with the app's current g_win_x/y/w/h).
void win_sync_rect(int mode, int x, int y, int w, int h);

// Hit-test a window's [X] close button: returns the mode whose
// close button contains (x, y), or -1.  Uses the FOCUSED window's
// rect.  The y-range accepts both the self-drawn title bar
// (g_win_y .. g_win_y+36) and the compositor-style bar drawn above
// the window (g_win_y-28 .. g_win_y-4).
int win_close_btn_hit(int x, int y);

#endif
