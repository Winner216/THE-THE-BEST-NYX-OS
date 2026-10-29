// include/game.h — Game mode (MODE_GAME): a full-screen window that a
// ring-3 PE process paints into via SYS_GAMEBLIT and reads input from
// via SYS_GETKEY / SYS_GETMOUSE.  The kernel-side game.c owns the
// 640×400 BGRA canvas, the keyboard event ring, and the game's
// lifecycle.
//
// Ring-3 flow (see user/doom/doomgeneric_nyx.c):
//   SYS_GAMESTART  → open game mode, take the 640×400 canvas
//   SYS_GAMEBLIT   → copy one 640×400 BGRA frame into the canvas,
//                    flag a repaint (desktop shows it next present)
//   SYS_GETKEY     → pop one (pressed, scancode) event, -1 when empty
//   SYS_GETMOUSE   → drain mouse: dx(arg0) dy(arg1) → button bits,
//                    -1 when game mode is off
//   SYS_EXIT / SYS_GAMESTOP → leave game mode
//
// The keyboard ISR (keyboard_callback) appends raw (make/break,
// scancode) events to the ring whenever game mode is active, so the
// game sees full press AND release sequences (DOOM needs releases for
// movement keys).  ESC is delivered to the game like any other key —
// the game's own menu handles it; the user quits the game window with
// Alt+F4 / the [X] button like any app (or the task is killed).
//
// Mouse: the PS/2 IRQ (mouse_callback) accumulates raw deltas and
// button level into game.c while game mode is active; SYS_GETMOUSE
// drains them.  Clicking the title-bar [X] still closes the game
// (checked in the IRQ before any deltas reach the game).

#ifndef GAME_H
#define GAME_H

#include <stdint.h>

// Fixed game canvas: classic DOOM resolution.  The game renders this
// itself; the kernel just shows it (centered, integer aspect letterbox
// happens naturally because the desktop is 16:9 and 640×400 is 16:10 —
// the 20 px vertical margin is filled with the window chrome).
#define GAME_W  640
#define GAME_H  400
// Max supported game canvas (matches the VESA mode).  A game may claim
// a bigger canvas via SYS_GAMESTART(title, w, h) — CHESS.EXE uses
// full screen for a larger board; DOOM.EXE keeps the 640×400 default.
#define GAME_MAX_W  1920
#define GAME_MAX_H  1080

// Keyboard event ring capacity (power of two).  Each event is one
// 16-bit word:  bits0..7 = set-1 scancode, bit8 = E0-prefixed (arrows,
// right Ctrl...), bit9 = 1 on RELEASE (0 on press), bits10..15 unused.
#define GAME_KEY_RING 64

// Mouse button bits returned by game_mouse_poll / SYS_GETMOUSE —
// the same layout DOOM's ev_mouse data1 expects (bit0 left,
// bit1 right, bit2 middle).
#define GAME_MOUSE_L 1
#define GAME_MOUSE_R 2
#define GAME_MOUSE_M 4

void game_init(void);                 // clear state at boot
int  game_start(const char* title);   // enter MODE_GAME, returns 0/-1
// game_start_sized: game_start with an explicit canvas size (clamped to
// GAME_MAX_W×GAME_MAX_H).  width/height ≤ 0 → the 640×400 default.
int  game_start_sized(const char* title, int width, int height);
void game_stop(void);                 // exit game mode (also on task death)
int  game_active(void);
int  game_draws_titlebar(void);   // present must include the title-bar strip               // 1 while MODE_GAME owns the screen
void game_key_push(uint16_t ev);      // keyboard ISR: append one event
int  game_key_pop(void);              // ring-3: pop one event (-1 empty)
void game_flush_keys(void);           // clear pending events (mode exit)

// Mouse (game mode only).  The PS/2 IRQ calls game_mouse_feed with
// raw packet deltas + the button bitfield; ring 3 drains deltas via
// game_mouse_poll (SYS_GETMOUSE).  Buttons are level state, not
// events — the game polls each tic and builds press/release edges.
void game_mouse_feed(int dx, int dy, int buttons);  // IRQ producer
int  game_mouse_poll(int* dx, int* dy);             // -1 when not in game mode
uint8_t* game_canvas(void);           // canvas pointer (or NULL)
int      game_canvas_w(void);                // current canvas width
int      game_canvas_h(void);                // current canvas height
int  game_dirty(void);                // new frame waiting to be shown?
void game_clear_dirty(void);          // desktop took the frame
void game_draw(void);                 // paint canvas into the framebuffer
int  game_pid(void);                  // owning task id (-1 none)
const char* game_current_title(void); // title passed to game_start ("" none)
void game_set_pid(int pid);
int  game_hit_close(int mx, int my);  // [X] box hit-test (mouse click)

// Syscall-side helpers (called from cpu/syscall.c on SYS_GAME*).
// The desktop coupling (mode switch + repaint) lives in desktop.c
// (desktop_enter_game / desktop_exit_game) to keep the layering.
void* game_syscall_start(const char* title, int w, int h);
void  game_syscall_blit(const void* src);
void  game_syscall_stop(void);

#endif // GAME_H
