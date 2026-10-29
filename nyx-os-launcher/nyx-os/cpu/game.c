// cpu/game.c — Game mode implementation (see include/game.h).
//
// The desktop enters MODE_GAME with game_start() when a ring-3 game
// process (DOOM.EXE) calls SYS_GAMESTART.  The game paints 640×400
// BGRA frames via SYS_GAMEBLIT (the canvas is kernel-owned, so a
// misbehaving game can only trash its own copy — the syscall copies),
// and the desktop compositor draws the canvas full-screen via
// game_draw() from desktop_draw() and the desktop heartbeat.
//
// Keyboard: keyboard_callback() pushes make/break scancode events
// while game mode is active (before any desktop handling), so the
// game gets raw hardware events.
//
// Mouse: while game mode is active the PS/2 IRQ accumulates raw
// (unclamped, unaccelerated) deltas and button state into
// game_mouse_dx/dy and game_mouse_buttons (game_mouse_feed(),
// called from mouse.c before any desktop drag logic).  Ring 3 polls
// and clears them with SYS_GETMOUSE — one aggregated DOOM ev_mouse
// event per tic, the shape G_Responder expects.  The cursor is not
// drawn in MODE_GAME (the game is the pointer).

#include <game.h>
#include <vesa.h>
#include <string.h>
#include <stdint.h>

// ─── State ────────────────────────────────────────────────────────
static void game_draw_titlebar(int gx, int gy);   // fwd: used by game_draw
// BGRA canvas, sized for the LARGEST supported game canvas (full-screen
// 1920×1080 for CHESS.EXE).  DOOM keeps the 640×400 default.  ~8.3 MB
// of .bss — same footprint class as the VESA back buffer.
static uint8_t  game_canvas_buf[GAME_MAX_W * GAME_MAX_H * 4]
    __attribute__((aligned(64)));      // BGRA canvas (kernel-owned)
static volatile int game_cw = GAME_W;  // current canvas width
static volatile int game_ch = GAME_H;  // current canvas height
static volatile int game_on     = 0;   // MODE_GAME owns the screen
static volatile int game_newfrm = 0;   // game pushed a frame not shown yet
static volatile int game_head   = 0;   // key ring read index
static volatile int game_tail   = 0;   // key ring write index
static uint16_t game_ring[GAME_KEY_RING];
static int      game_owner = -1;       // pid of the game task
static char     game_title[40];

// ─── Mouse accumulators (IRQ producer, SYS_GETMOUSE consumer) ─────
// Raw PS/2 deltas summed between polls.  Clamped so a stuck packet
// stream can't overflow int (±8k/tic is already ~64× a fast swipe).
static volatile int gm_dx = 0, gm_dy = 0;   // raw movement this poll
static volatile int gm_btn = 0;             // bit0 L, bit1 R, bit2 M

// ─── Lifecycle ────────────────────────────────────────────────────
void game_init(void) {
    game_on = 0; game_newfrm = 0;
    game_head = game_tail = 0;
    game_owner = -1;
    game_title[0] = 0;
}

int game_start_sized(const char* title, int width, int height) {
    game_head = game_tail = 0;         // drop stale input
    game_newfrm = 0;
    game_cw = (width > 0) ? width  : GAME_W;
    game_ch = (height > 0) ? height : GAME_H;
    if (game_cw > GAME_MAX_W) game_cw = GAME_MAX_W;
    if (game_ch > GAME_MAX_H) game_ch = GAME_MAX_H;
    int i = 0;
    if (title) while (title[i] && i < 39) { game_title[i] = title[i]; i++; }
    game_title[i] = 0;
    game_on = 1;
    return 0;
}

int game_start(const char* title) {
    return game_start_sized(title, GAME_W, GAME_H);
}

void game_stop(void) {
    game_on = 0;
    game_cw = GAME_W;                  // reset for the next game
    game_ch = GAME_H;
    game_newfrm = 0;
    game_owner = -1;
    game_flush_keys();
    gm_dx = gm_dy = gm_btn = 0;
}

int  game_active(void)      { return game_on; }
// Whether the compositor's present must include the title-bar strip
// above the canvas (see game_draw_titlebar).
int  game_draws_titlebar(void) { return game_on; }
int  game_dirty(void)       { return game_on && game_newfrm; }
int  game_canvas_w(void)    { return game_on ? game_cw : 0; }
int  game_canvas_h(void)    { return game_on ? game_ch : 0; }
void game_clear_dirty(void) { game_newfrm = 0; }
int  game_pid(void)         { return game_owner; }

const char* game_current_title(void) {
    return game_title;                // static buffer, "" when idle
}
void game_set_pid(int pid)  { game_owner = pid; }

uint8_t* game_canvas(void) {
    return game_on ? game_canvas_buf : (uint8_t*)0;
}

// ─── Keyboard ring (IRQ producer, syscall consumer) ───────────────
void game_key_push(uint16_t ev) {
    int nt = (game_tail + 1) % GAME_KEY_RING;
    if (nt == game_head) return;       // full — drop oldest-in-flight
    game_ring[game_tail] = ev;
    game_tail = nt;
}

int game_key_pop(void) {
    if (game_head == game_tail) return -1;
    uint16_t ev = game_ring[game_head];
    game_head = (game_head + 1) % GAME_KEY_RING;
    return ev;
}

void game_flush_keys(void) { game_head = game_tail = 0; }

// ─── Mouse (IRQ producer: cpu/mouse.c; consumer: SYS_GETMOUSE) ────
// Single-writer (IRQ12) plain adds; the reader drains with an atomic
// exchange-with-zero (i386 xchg — always atomic), so no lost updates.
void game_mouse_feed(int dx, int dy, int buttons) {
    if (!game_on) return;
    int nx = gm_dx + dx;
    if (nx >  8192) nx =  8192;
    if (nx < -8192) nx = -8192;
    gm_dx = nx;
    int ny = gm_dy + dy;
    if (ny >  8192) ny =  8192;
    if (ny < -8192) ny = -8192;
    gm_dy = ny;
    gm_btn = buttons & 7;
}

// Poll-and-clear: writes deltas (raw, unaccelerated) and returns the
// button bitfield (bit0 L, bit1 R, bit2 M).  Returns -1 when game
// mode is off so a stray caller can't read live desktop mouse data.
int game_mouse_poll(int* dx, int* dy) {
    if (!game_on) return -1;
    *dx = __atomic_exchange_n(&gm_dx, 0, __ATOMIC_RELAXED);
    *dy = __atomic_exchange_n(&gm_dy, 0, __ATOMIC_RELAXED);
    return gm_btn & 7;
}

// ─── Title bar (drawn on top edge of the canvas) ──────────────────
// 24 px bar: dark strip, game title, and a red [X] close box at the
// right end — the only mouse target in game mode (click = kill game).
#define GAME_TB_H   24
#define GAME_XBOX_W 16

static void game_draw_titlebar(int gx, int gy) {
    int tx = gx, ty = gy - GAME_TB_H;
    // A full-screen canvas has no room above it — overlay the bar on the
    // canvas top edge instead of drawing it off-screen.
    if (ty < 0) ty = 0;
    vesa_fill_rect(tx, ty, GAME_W, GAME_TB_H, 0x202020);
    vesa_fill_rect(tx, ty + GAME_TB_H - 1, GAME_W, 1, 0x000000);
    vesa_draw_string(tx + 10, ty + 6, game_title[0] ? game_title : "Game",
                     0xEDEDED, 0x202020);
    // [X] box
    int bx = tx + GAME_W - GAME_XBOX_W - 6;
    int by = ty + (GAME_TB_H - GAME_XBOX_W) / 2;
    vesa_fill_rect(bx, by, GAME_XBOX_W, GAME_XBOX_W, 0x8B1E1E);
    // X glyph (two lines through the box center)
    for (int i = 2; i < GAME_XBOX_W - 2; i++) {
        vesa_put_pixel(bx + i, by + i, 0xFFFFFF);
        vesa_put_pixel(bx + i, by + GAME_XBOX_W - 1 - i, 0xFFFFFF);
    }
}

// Hit-test a mouse click against the [X] box (positions are absolute
// screen coordinates; returns 1 when the click closes the game).
int game_hit_close(int mx, int my) {
    if (!game_on) return 0;
    int gx = (int)vesa.width  / 2 - game_cw / 2;
    int gy = (int)vesa.height / 2 - game_ch / 2;
    int bx = gx + game_cw - GAME_XBOX_W - 6;
    int by = gy - GAME_TB_H + (GAME_TB_H - GAME_XBOX_W) / 2;
    if (by < 0) by = (GAME_TB_H - GAME_XBOX_W) / 2;   // clamped bar: same as draw
    return mx >= bx && mx < bx + GAME_XBOX_W &&
           my >= by && my < by + GAME_XBOX_W;
}

// ─── Compositor: paint the game canvas full-screen ────────────────
// Canvas is BGRA (native VESA format), so this is a straight word
// blit through vesa_blit_bgra into the back buffer (or direct to the
// framebuffer when double-buffering is off), then the caller presents.
void game_draw(void) {
    if (!game_on) return;
    int cw = game_cw, ch = game_ch;
    int sw = vesa.width, sh = vesa.height;
    if (sw < cw || sh < ch) {
        // Screen smaller than the canvas: clip-center (never happens on
        // the required 1920×1080 desktop mode — full-screen chess lands
        // here as an exact 1:1 fit when it matches).
        int w = sw < cw ? sw : cw;
        int h = sh < ch ? sh : ch;
        vesa_blit_bgra((sw - w) / 2, (sh - h) / 2, game_canvas_buf,
                       cw, ch, 0, 0, w, h);
        return;
    }
    // Full-screen integer placement: center.  A canvas that matches the
    // screen (chess full-screen) lands at 0,0 and fills everything.
    int gx = (sw - cw) / 2, gy = (sh - ch) / 2;
    vesa_blit_bgra(gx, gy, game_canvas_buf,
                   cw, ch, 0, 0, cw, ch);
    game_draw_titlebar(gx, gy);
}

// ─── Syscall-side helpers (dispatched from cpu/syscall.c) ─────────
//
// game_syscall_start: a ring-3 game asks for the canvas.  The desktop
// coupling (switching current_mode, repaint, remembering the return
// mode) lives in desktop_enter_game() — declared here as a weak hook
// so game.c doesn't need desktop.h (which includes game.h itself).
extern void desktop_enter_game(const char* title, int w, int h);
extern void desktop_exit_game(void);

void* game_syscall_start(const char* title, int w, int h) {
    // Only one game at a time: if one owns the canvas, refuse.
    // BUT: if the owning task is already dead (crashed → user-fault
    // path frees the PCB without touching game state), the claim is
    // stale — clear it so the game can be relaunched instead of
    // silently refusing forever.
    if (game_on && game_owner >= 0) {
        extern int task_current_id(void);
        extern int task_alive(int id);
        if (!task_alive(game_owner)) {
            game_stop();                    // drop stale claim + input state
        } else if (task_current_id() != game_owner) {
            return (void*)0;                // a live game really owns it
        }
    }
    // Canvas size: the game may request its own dimensions (chess
    // full-screen); w/h ≤ 0 (DOOM's two-arg call) = 640×400 default.
    // desktop_enter_game performs the game_start_sized claim AND the
    // desktop coupling (MODE_GAME, pid, repaint) — bypassing it leaves
    // the desktop drawing over the game forever.
    desktop_enter_game(title && title[0] ? title : "Game", w, h);
    if (!game_on) return (void*)0;     // desktop refused (no VESA, etc.)
    return game_canvas_buf;
}

void game_syscall_blit(const void* src) {
    if (!game_on || !src) return;
    int cw = game_cw, ch = game_ch;
    // The game passes its own 640×400 BGRA buffer; the kernel copies
    // so a bad pointer can only fault the caller, never corrupt the
    // canvas between owner checks.  Word-copy when both sides are
    // 4-aligned (the usual case — DG_ScreenBuffer is uint32_t[]).
    const uint8_t* s = (const uint8_t*)src;
    if ((((uintptr_t)s) | ((uintptr_t)game_canvas_buf)) % 4 == 0) {
        const uint32_t* sw = (const uint32_t*)s;
        uint32_t* dw = (uint32_t*)game_canvas_buf;
        for (int i = 0; i < cw * ch; i++) dw[i] = sw[i];
    } else {
        for (int i = 0; i < cw * ch * 4; i++)
            game_canvas_buf[i] = s[i];
    }
    game_newfrm = 1;
}

void game_syscall_stop(void) {
    if (!game_on) return;
    // Only the owner (or the desktop itself) may close the window.
    // task_current_id() needs task.h — read the pid through the
    // current-task helper exported by task.h instead.
    extern int task_current_id(void);
    if (game_owner >= 0 && task_current_id() != game_owner) return;
    desktop_exit_game();
}
