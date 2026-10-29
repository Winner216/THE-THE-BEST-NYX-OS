#ifndef DESKTOP_H
#define DESKTOP_H

extern int current_mode;
// Origin of the currently-open Image Viewer (MODE_FILES, MODE_GALLERY,
// or -1 = unknown/desktop).  Set by the opener; the IMGVIEW ESC
// handler returns to it instead of the desktop.
extern int imgview_return_mode;
// Origin of the currently-open PDF Viewer (MODE_FILES or -1).  Set by
// the opener; the PDFVIEW ESC handler returns to it instead of the
// desktop.
extern int pdfview_return_mode;
// Origin of the currently-open Video player (MODE_FILES or -1).  Set
// by the opener; the VIDEO ESC handler returns to it instead of the
// desktop.
extern int video_return_mode;

// Window position — set by each app's draw function so the global
// [X] close-button dispatch in desktop.c can find the button.
// Initialized to -1 (use default) by desktop_init().
extern int g_win_x, g_win_y, g_win_w, g_win_h;

#define MODE_DESKTOP  0
#define MODE_TERMINAL 1
#define MODE_FILES    2
#define MODE_TRASH    3
#define MODE_SETTINGS 4
#define MODE_MUSIC    5
#define MODE_CALC     6
#define MODE_GAMES    7
#define MODE_EDITOR   8
#define MODE_PAINT    9
#define MODE_SYSMON  10
#define MODE_CLOCK   11
#define MODE_CALENDAR  12
#define MODE_WEATHER  13
#define MODE_APPLIB  14
#define MODE_ORGANIZER 15
#define MODE_SECURITY  16
#define MODE_IMGVIEW   17
#define MODE_GALLERY   18
#define MODE_PDFVIEW   19
#define MODE_BROWSER   20
#define MODE_QUICKSETTINGS 21
#define MODE_VIDEO     22
#define MODE_STORE     23
#define MODE_GAME      24   // full-screen ring-3 game canvas (DOOM.EXE)


void desktop_init(void);
void desktop_draw(void);
void desktop_handle_key(char c);
void desktop_update_clock(void);

// ─── Rubber-band multi-selection queries (for mouse.c IRQ) ────────
// The IRQ handler needs to know, at drag-threshold time, whether the
// pressed slot is one of the box-selected icons and how many are
// selected — that decides single-icon drag vs whole-group drag.
int desktop_sel_count(void);
int desktop_sel_has(int slot);

// Called periodically from the idle loop (~18 Hz via PIT timer) to
// drive timer-based UI animations like the gallery slideshow.
// Automatically redraws gallery frames when a slideshow is active.
void desktop_heartbeat(void);

// Incremental icon-picture loader — call from the kernel session
// loop (main-loop context, interrupts enabled), once per iteration.
// Decodes at most one app picture per call so the first desktop
// frame after login renders instantly; the real SVG/PNG pictures
// pop in progressively. Never called from desktop_heartbeat() (the
// PIT IRQ) to avoid blocking the mouse/keyboard IRQs.
void desktop_icon_pump(void);
// Video playback redraw (JPEG decode) — main-loop context only.
void desktop_video_pump(void);
// Completes an asynchronous JPEG open (files_open_file → background
// jpegdec worker task).  Called from the kernel main loop, never the
// PIT IRQ or a key/mouse handler.  No-op when no open is pending.
void desktop_async_pump(void);

// Decode ALL app pictures in one synchronous pass at boot, right
// after fs_load_embedded() and before the login screen, so the
// desktop renders complete on its first frame.  Safe because
// decoding only writes to static buffers (never the framebuffer)
// and interrupts are already enabled when called.
void desktop_icon_warmup(void);

// Launches an app by its desktop-icon position index (0-14, same
// order as the icon grid: 0=Terminal, 1=Files, 2=About, 3=Editor,
// 4=AppLib, 5=Trash, 6=Settings, 7=Music, 8=Calc, 9=Games, 10=Paint,
// 11=Sysmon, 12=Clock, 13=Calendar, 14=Weather). NOT the same as the
// MODE_* constants above — those are current_mode's value once an
// app is already open, this is which icon to launch as if clicked.
void desktop_launch(int icon_idx);

// Clears the windowed Terminal's scrollback buffer. Called by the
// clear/cls shell command, which can't reach desktop.c's static
// term_buf directly.
void desktop_terminal_clear(void);

// ─── Visual feedback ─────────────────────────────────────────────
// Shows a temporary message in the status bar for ~1.5 seconds.
void desktop_set_status(const char* msg);

// Triggers a brief full-screen colored flash (fades over `frames`
// ticks). Pass r,g,b in 0-255 range; kept subtle by internal dimming.
void desktop_vfx_flash(int r, int g, int b, int frames);

// Commits the current undo-browse preview (called from keyboard.c
// on Ctrl release if browsing is active). No-op if not browsing.
void desktop_undo_browse_commit(void);

// ─── Alt+Tab task switcher ───────────────────────────────────────
// Called from keyboard.c as the user holds Alt and taps Tab:
//   desktop_alt_tab()          — first press opens the switcher (MRU
//                                list of apps, current first) and
//                                advances the selection; each further
//                                Tab tap advances it.
//   desktop_alt_tab_commit()   — called on Alt release: switches to
//                                the highlighted app (no-op if the
//                                switcher isn't open).
//   desktop_alt_tab_cancel()   — closes the switcher without
//                                switching (if ever needed).
//   desktop_alt_tab_active()   — 1 while the switcher overlay is
//                                open (keyboard.c swallows all other
//                                keys then).
void desktop_alt_tab(void);
void desktop_alt_tab_commit(void);
void desktop_alt_tab_cancel(void);
int  desktop_alt_tab_active(void);

// ─── Game mode (MODE_GAME) ──────────────────────────────────────
// Called from cpu/game.c when a ring-3 game process asks for the
// screen via SYS_GAMESTART / exits via SYS_GAMESTOP (or is killed).
// desktop_enter_game switches to MODE_GAME and remembers the return
// mode; desktop_exit_game restores it.
void desktop_enter_game(const char* title, int w, int h);
void desktop_exit_game(void);

#endif
