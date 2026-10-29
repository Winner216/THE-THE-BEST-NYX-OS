// user/chess/chess_main.c — CHESS.EXE for Nyx OS: platform layer, UI,
// and game shell.  Built the same way as DOOM.EXE: freestanding 32-bit
// C, static ELF linked at its final runtime base, wrapped into a PE32
// by tools/elf2pe.py, embedded in host-files/ and launched by pe_exec.
//
// Canvas: FULL SCREEN 1920×1080 BGRA (0x00RRGGBB words, little-endian),
// requested via the SYS_GAMESTART w/h args (DOOM keeps its 640×400
// default by passing zeros).  Pieces are the user's SVG art from
// host-files/chess_pieces/, rasterized at build time into pieces.h and
// alpha-blended onto the board (piece_sprites[]).
//
// Layout (1920×1080):
//   top strip y 0..64    (title)
//   board x 480..1440, y 70..1030 (8×8 of 120 px squares)
//   panel x 1476..1864   (status, level list, buttons)
//   bottom strip y 1034  (material / move counter / key hints)
//
// Idle frames skip rendering AND the blit entirely (kernel canvas keeps
// the last frame), so the 50 Hz loop costs ~nothing between inputs.

#include "chess.h"
#include "font8x16.h"
#include "pieces.h"
#include <stdint.h>
#include <stddef.h>

// ─────────────────────────────────────────────────────────────────
// Kernel interface (syscall numbers match include/syscall.h)
// ─────────────────────────────────────────────────────────────────
#define SYS_EXIT        0
#define SYS_SLEEP       2
#define SYS_ALLOC_PAGE  6
#define SYS_WRITE       8
#define SYS_GAMESTART   16
#define SYS_GAMEBLIT    17
#define SYS_GETKEY      18
#define SYS_GAMESTOP    19
#define SYS_SNDLOAD     20
#define SYS_SNDPLAY     21
#define SYS_GETMOUSE    25

static int nyx_sc(int n, int a, int b, int c) {
    int ret;
    asm volatile("int $0x80" : "=a"(ret)
                 : "a"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return ret;
}
static void nyx_write(const char* s, int len) { nyx_sc(SYS_WRITE, (int)s, len, 0); }
static void nyx_log(const char* s) {
    int n = 0; while (s[n]) n++;
    nyx_write(s, n);
}
static void nyx_exit(int code) { nyx_sc(SYS_EXIT, code, 0, 0); for(;;){} }

// ─────────────────────────────────────────────────────────────────
// Minimal libc
// ─────────────────────────────────────────────────────────────────
static void* memset(void* d, int c, size_t n) {
    unsigned char* p = (unsigned char*)d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}
static size_t strlen(const char* s) { size_t n = 0; while (s[n]) n++; return n; }

static void utoa_dec(int v, char* out) {
    unsigned u = (v < 0) ? (unsigned)-v : (unsigned)v;
    char d[12]; int i = 0, n = 0;
    if (v < 0) out[n++] = '-';
    if (!u) d[i++] = '0';
    while (u) { d[i++] = (char)('0' + u % 10); u /= 10; }
    while (i) out[n++] = d[--i];
    out[n] = 0;
}

// ─────────────────────────────────────────────────────────────────
// Zone allocator (SYS_ALLOC_PAGE) — needs the full-screen frame:
// 1920×1080×4 = 8,294,400 B ≈ 2025 pages, plus working headroom.
// ─────────────────────────────────────────────────────────────────
static unsigned zone_base, zone_top;
#define ZONE_PAGES 2560                    // 10 MiB

static void* zone_init(void) {
    unsigned p = (unsigned)nyx_sc(SYS_ALLOC_PAGE, ZONE_PAGES, 0, 0);
    if (!p) return 0;
    zone_base = zone_top = p;
    return (void*)p;
}
static void* zone_alloc(unsigned bytes) {
    bytes = (bytes + 15u) & ~15u;
    if (zone_top + bytes > zone_base + ZONE_PAGES * 4096u) return 0;
    void* p = (void*)zone_top;
    zone_top += bytes;
    return p;
}

// ─────────────────────────────────────────────────────────────────
// Canvas + drawing
// ─────────────────────────────────────────────────────────────────
#define W 1920
#define H 1080
static uint32_t* fb;                       // user frame (0x00RRGGBB)

static inline void px(int x, int y, uint32_t c) {
    if ((unsigned)x < W && (unsigned)y < H) fb[y * W + x] = c;
}
static void fill(int x, int y, int w, int h, uint32_t c) {
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) px(i, j, c);
}
static void rect(int x, int y, int w, int h, uint32_t c) {
    for (int i = x; i < x + w; i++) { px(i, y, c); px(i, y + h - 1, c); }
    for (int j = y; j < y + h; j++) { px(x, j, c); px(x + w - 1, j, c); }
}
static void rect_w(int x, int y, int w, int h, int t, uint32_t c) {
    fill(x, y, w, t, c); fill(x, y + h - t, w, t, c);
    fill(x, y, t, h, c); fill(x + w - t, y, t, h, c);
}
static void disc(int cx, int cy, int r, uint32_t c) {
    for (int j = -r; j <= r; j++)
        for (int i = -r; i <= r; i++)
            if (i * i + j * j <= r * r) px(cx + i, cy + j, c);
}

// Scaled font blit (8×16 glyphs at integer scale).
static void draw_char_s(int x, int y, char c, uint32_t fg, uint32_t bg, int s) {
    unsigned i = (unsigned char)c;
    if (i < 32 || i > 127) i = 32;
    i -= 32;
    for (int r = 0; r < 16; r++) {
        uint8_t bits = font8x16[i][r];
        for (int k = 0; k < 8; k++) {
            uint32_t col = (bits & (0x80 >> k)) ? fg : bg;
            if (s == 1) { px(x + k, y + r, col); continue; }
            for (int sy = 0; sy < s; sy++)
                for (int sx = 0; sx < s; sx++)
                    px(x + k * s + sx, y + r * s + sy, col);
        }
    }
}
static void draw_str_s(int x, int y, const char* s, uint32_t fg, uint32_t bg, int sc) {
    int cx = x;
    for (int i = 0; s[i]; i++) {
        if (s[i] == '\n') { cx = x; y += 16 * sc; }
        else { draw_char_s(cx, y, s[i], fg, bg, sc); cx += 8 * sc; }
    }
}
static int str_w(const char* s, int scale) {
    return (int)strlen(s) * 8 * scale;
}
// Draw text centered in [x, x+w).
static void draw_str_c(int x, int w, int y, const char* s,
                       uint32_t fg, uint32_t bg, int scale) {
    draw_str_s(x + (w - str_w(s, scale)) / 2, y, s, fg, bg, scale);
}

// ─────────────────────────────────────────────────────────────────
// Sound: square-wave beeps synthesized in code (no PCM assets)
// ─────────────────────────────────────────────────────────────────
#define SR 8000
static int snd_move, snd_cap, snd_chk, snd_end;
static int snd_on = 1;

static int snd_load_tone(int f, int ms, uint8_t amp) {
    static uint8_t buf[SR];                // ≤1 s; tones are short
    int n = SR * ms / 1000;
    if (n > SR) n = SR;
    unsigned ph = 0;
    for (int i = 0; i < n; i++) {
        ph += (unsigned)f;
        uint8_t v = ((ph / SR) & 1) ? amp : 0;
        int env = 255;                     // attack + linear decay
        if (i < 40) env = i * 255 / 40;
        if (n - i < n / 4) env = (n - i) * 255 / (n / 4);
        buf[i] = (uint8_t)((v * env) / 255);
    }
    return nyx_sc(SYS_SNDLOAD, (int)buf, n, SR);
}

static void snd_init(void) {
    snd_move = snd_load_tone(440, 60, 110);
    snd_cap  = snd_load_tone(220, 90, 150);
    snd_chk  = snd_load_tone(660, 120, 130);
    snd_end  = snd_load_tone(330, 400, 140);
}
static void snd_play(int h) {
    if (snd_on && h >= 0) nyx_sc(SYS_SNDPLAY, h, 100, (0 << 16) | 128);
}

// ─────────────────────────────────────────────────────────────────
// Piece sprites (pieces.h): 0xAARRGGBB straight-alpha words, 48×48.
// Blend with a bilinear 48→SQ resample (per-axis weights precomputed).
// ─────────────────────────────────────────────────────────────────
static void blend_sprite(int dx, int dy, int size, const uint32_t* spr) {
    if (size <= 0) return;
    // Per-destination-pixel source coordinates (48×48 grid).
    static int    x0[384], y0[384];
    static unsigned fx[384], fy[384];
    static int    cached_size = 0;
    if (cached_size != size) {
        for (int i = 0; i < size; i++) {
            float u = ((float)i + 0.5f) * 48.0f / (float)size - 0.5f;
            if (u < 0) u = 0;
            if (u > 47.0f) u = 47.0f;
            x0[i] = (int)u;
            fx[i] = (unsigned)((u - (float)x0[i]) * 256.0f);
            if (x0[i] > 46) { x0[i] = 46; fx[i] = 255; }
        }
        cached_size = size;
        for (int i = 0; i < size; i++) { y0[i] = x0[i]; fy[i] = fx[i]; }
    }
    for (int j = 0; j < size; j++) {
        const uint32_t* rowA = spr + y0[j] * 48;
        const uint32_t* rowB = spr + (y0[j] + (fy[j] ? 1 : 0)) * 48;
        unsigned wa = fy[j] ? 256 - fy[j] : 256;   // simplification: no v-blend
        (void)wa;
        for (int i = 0; i < size; i++) {
            unsigned a = fx[i] ? 256 - fx[i] : 256;
            uint32_t c = (fx[i] && (a < 128))
                         ? rowB[x0[i] + 1] : rowA[x0[i]];
            unsigned al = (c >> 24) & 0xFF;
            if (!al) continue;
            if (al > 255 - 8) { px(dx + i, dy + j, c & 0xFFFFFF); continue; }
            uint32_t dst = fb[(dy + j) * W + dx + i];
            unsigned b = (c & 0xFF), g = ((c >> 8) & 0xFF), r = ((c >> 16) & 0xFF);
            unsigned db = dst & 0xFF, dg = ((dst >> 8) & 0xFF), dr = ((dst >> 16) & 0xFF);
            unsigned ia = 255 - al;
            unsigned ob = (b * al + db * ia) / 255;
            unsigned og = (g * al + dg * ia) / 255;
            unsigned or_ = (r * al + dr * ia) / 255;
            px(dx + i, dy + j, (or_ << 16) | (og << 8) | ob);
        }
    }
}

static void draw_piece_at(int x, int y, int size, int8_t pc) {
    if (!pc) return;
    int t = pc > 0 ? pc : -pc;
    if (t < 1 || t > 6) return;
    int idx = (pc > 0 ? 0 : 1) * 6 + (t - 1);
    blend_sprite(x + (size - size) / 2, y, size, piece_sprites[idx]);
}

// ─────────────────────────────────────────────────────────────────
// Palette + geometry
// ─────────────────────────────────────────────────────────────────
#define CLR_BG      0x12101C
#define CLR_PANEL   0x1B1830
#define CLR_FRAME   0x443672
#define CLR_LIGHT   0xBEAFEA
#define CLR_DARK    0x7262B0
#define CLR_SEL     0x4FC3F7
#define CLR_LAST    0x86CE8C
#define CLR_TRACE   0xFFB84D                 // orange outline on the PC's reply
#define CLR_YOU     0x6FC8FF                 // your chat lines (sky blue)
#define CLR_CHATBG  0x171528                 // chat sidebar background
#define CLR_TEXT    0xF2ECFF
#define CLR_DIM     0x9C8BC8
#define CLR_BTN     0x2A2350
#define CLR_BTN_HV  0x3D306E
#define CLR_OK      0x55CC88
#define CLR_WARN    0xFFB84D
#define CLR_BAD     0xFF5566

#define SQ        120                     // square size (8×8 = 960 px)
#define BD        ((W - 8 * SQ) / 2)      // board x origin (centered)
#define BD_Y      70                      // board y origin (below title)
#define PANEL_X   1476
#define PANEL_W   388
#define STATUS_Y  1034

// ─────────────────────────────────────────────────────────────────
// Game state
// ─────────────────────────────────────────────────────────────────
typedef struct { chess_pos_t pos; chess_move_t mv; } hist_entry_t;
#define HIST_MAX 600
static hist_entry_t hist[HIST_MAX];
static int hist_n;

static chess_pos_t pos;
static int sel = -1;
static int legal_n;
static chess_move_t legal[CHESS_MAXMOVES];
static int last_from = -1, last_to = -1;
static int ai_from = -1, ai_to = -1;     // PC's reply: traced until you move

static int ai_depth = 3;              // 2..5
static const char* lvl_names[4] = { "Beginner", "Casual", "Club", "Expert" };

static int game_over, over_kind;
static int promo_pending;
static chess_move_t promo_base;

static int spinner;

static int mouse_x = W / 2, mouse_y = H / 2, mouse_btn;
static int hover_sq = -1;

// Cursor damping: the kernel feeds games RAW PS/2 deltas (unclamped,
// unaccelerated — the accel curve is desktop-only), which is what DOOM
// wants but far too twitchy for a pointer-style game cursor.
//
// The damping must be SUM-PRESERVING to stay exact regardless of how
// the kernel packetizes the stream: deltas are accumulated and only
// every CURSOR_DIV-th count is spent on cursor movement, so any N raw
// counts move the cursor by ~N/CURSOR_DIV px (±1 residue) whether they
// arrived as one packet of N or N packets of one.
#define CURSOR_DIV 3

static int mouse_acc_x, mouse_acc_y;     // sub-pixel damping residue

static void cursor_apply(int dx, int dy) {
    // dy arrives in the DOOM convention (dy > 0 = pushed forward/up);
    // screen space wants down = +y, so negate.
    mouse_acc_x += dx;
    mouse_acc_y -= dy;
    int sx = mouse_acc_x / CURSOR_DIV;   // C: truncates toward zero
    int sy = mouse_acc_y / CURSOR_DIV;
    mouse_acc_x -= sx * CURSOR_DIV;
    mouse_acc_y -= sy * CURSOR_DIV;
    mouse_x += sx;
    mouse_y += sy;
    if (mouse_x < 0) mouse_x = 0;
    if (mouse_x >= W) mouse_x = W - 1;
    if (mouse_y < 0) mouse_y = 0;
    if (mouse_y >= H) mouse_y = H - 1;
}

static uint64_t hist_sigs[HIST_MAX];
static int      hist_sig_n;

static void refresh_legal(void) { legal_n = chess_gen(&pos, legal); }

// Chat hooks (defined in the chat module below).
static void chat_event_newgame(void);
static void chat_event_over(void);
static void chat_event_player_move(const chess_move_t* m, int cap_t, int chk);
static void chat_event_ai_move(const chess_move_t* m, int cap_t, int chk);
static void chat_mood_set(int pc_cp);

static void start_game(void) {
    chess_new(&pos);
    hist_n = 0;
    hist_sig_n = 0;
    hist_sigs[hist_sig_n++] = chess_sig(&pos);
    sel = -1; last_from = last_to = -1;
    ai_from = ai_to = -1;
    game_over = 0; over_kind = 0;
    promo_pending = 0;
    refresh_legal();
    chat_event_newgame();
}

static int is_repetition_draw(void) {
    uint64_t s = chess_sig(&pos);
    int count = 0;
    for (int i = 0; i < hist_sig_n; i++)
        if (hist_sigs[i] == s) count++;
    return count >= 3;
}

static void apply_move(const chess_move_t* m) {
    if (hist_n < HIST_MAX) {
        hist[hist_n].pos = pos;
        hist[hist_n].mv  = *m;
        hist_n++;
    }
    if (hist_sig_n < HIST_MAX) hist_sigs[hist_sig_n++] = chess_sig(&pos);
    int mover = pos.side;                    // +1 = you, -1 = the PC
    int cap_t = 0;                           // captured piece type (0 = none)
    if (m->flags & MV_EP) cap_t = PAWN;
    else if (pos.board[m->to]) {
        int t = pos.board[m->to];
        cap_t = (t < 0) ? -t : t;
    }
    chess_apply(&pos, m);
    last_from = m->from; last_to = m->to;
    if (mover < 0) { ai_from = m->from; ai_to = m->to; }   // PC replied: trace it
    else           { ai_from = ai_to = -1; }               // you moved: trace off
    sel = -1;
    refresh_legal();

    int chk = chess_in_check(&pos);
    if (mover < 0) chat_event_ai_move(m, cap_t, chk);
    else           chat_event_player_move(m, cap_t, chk);

    int st = chess_game_state(&pos);
    if (st == 1 || st == 2 || is_repetition_draw() || st == 3) {
        game_over = 1;
        over_kind = (st == 1) ? 1 : (st == 2) ? 2 : (st == 3) ? 4 : 3;
        chat_event_over();
        snd_play(snd_end);
        return;
    }
    if (chk)                   snd_play(snd_chk);
    else if (m->flags & MV_CAP) snd_play(snd_cap);
    else                        snd_play(snd_move);
}

// Material balance in pawns (+ = white ahead).
static int material_sum(const chess_pos_t* p) {
    int s = 0;
    for (int i = 0; i < 64; i++) {
        int8_t pc = p->board[i];
        if (!pc) continue;
        int t = pc > 0 ? pc : -pc;
        int v = (t == PAWN) ? 1 : (t == KNIGHT || t == BISHOP) ? 3
              : (t == ROOK) ? 5 : (t == QUEEN) ? 9 : 0;
        s += (pc > 0) ? v : -v;
    }
    return s;
}

static void ai_move(void) {
    static const int caps[4] = { 20000, 80000, 300000, 1200000 };
    chess_result_t r;
    chess_search(&pos, ai_depth, caps[ai_depth - 2], &r);
    chess_move_t best = r.best;
    // Sanity: fall back to the first legal move if search found none.
    int found = 0;
    for (int i = 0; i < legal_n; i++)
        if (legal[i].from == best.from && legal[i].to == best.to &&
            legal[i].promo == best.promo) { found = 1; break; }
    if (!found) best = legal[0];
    chat_mood_set(r.score);              // mood from the AI's own eval (PC view)
    apply_move(&best);
}

// ─────────────────────────────────────────────────────────────────
// Rendering
// ─────────────────────────────────────────────────────────────────
static int sq_sx(int sq) { return BD + (sq & 7) * SQ; }
static int sq_sy(int sq) { return BD_Y + (7 - (sq >> 3)) * SQ; }
static int screen_to_sq(int mx, int my) {
    int fx = (mx - BD) / SQ, fy = (my - BD_Y) / SQ;
    if (fx < 0 || fx > 7 || fy < 0 || fy > 7) return -1;
    return (7 - fy) * 8 + fx;
}

static void draw_board(void) {
    rect_w(BD - 6, BD_Y - 6, 8 * SQ + 12, 8 * SQ + 12, 6, CLR_FRAME);
    for (int sq = 0; sq < 64; sq++) {
        int x = sq_sx(sq), y = sq_sy(sq);
        int light = ((sq >> 3) + sq) & 1;
        fill(x, y, SQ, SQ, light ? CLR_LIGHT : CLR_DARK);

        if (sq == last_from || sq == last_to) {
            for (int j = 0; j < SQ; j += 4)
                for (int i = 0; i < SQ; i += 4)
                    px(x + i, y + j, CLR_LAST);
        }
        if (sq == sel) {
            for (int j = 0; j < SQ; j++)
                for (int i = (j & 1); i < SQ; i += 2)
                    px(x + i, y + j, CLR_SEL);
        }
        if (hover_sq >= 0 && sq == hover_sq && hover_sq != sel)
            rect_w(x, y, SQ, SQ, 3, 0xFFFFFF);

        draw_piece_at(x, y, SQ, pos.board[sq]);

        // The PC's last move stays outlined until the player's next
        // move.  Drawn AFTER the piece so the ring stays visible over
        // the sprite art.
        if (sq == ai_from || sq == ai_to)
            rect_w(x, y, SQ, SQ, 2, CLR_TRACE);
    }

    // Rank + file labels (inside the board edge).
    for (int r = 0; r < 8; r++) {
        char lab[2] = { (char)('8' - r), 0 };
        draw_str_s(BD + 6, BD_Y + r * SQ + 6, lab, CLR_DIM,
                   ((7 - r) & 1) ? CLR_DARK : CLR_LIGHT, 1);
    }
    for (int f = 0; f < 8; f++) {
        char lab[2] = { (char)('a' + f), 0 };
        draw_str_s(BD + f * SQ + SQ - 14, BD_Y + 8 * SQ - 22, lab,
                   CLR_DIM, (f & 1) ? CLR_LIGHT : CLR_DARK, 1);
    }

    if (sel >= 0) {
        for (int i = 0; i < legal_n; i++) {
            if (legal[i].from != sel) continue;
            int cx = sq_sx(legal[i].to) + SQ / 2;
            int cy = sq_sy(legal[i].to) + SQ / 2;
            int cap = pos.board[legal[i].to] || (legal[i].flags & MV_EP);
            if (cap) {
                disc(cx, cy, 34, CLR_BAD);
                disc(cx, cy, 24, pos.board[legal[i].to] < 0 ? CLR_DARK
                                                            : CLR_LIGHT);
            } else {
                disc(cx, cy, 10, CLR_OK);
            }
        }
    }
}

typedef struct { int x, y, w, h; const char* label; } btn_t;
static btn_t btns[6];
static const char* btn_labels[6] = { "New game", "Undo move",
                                     "Lv1", "Lv2", "Lv3", "Lv4" };
static int btn_hover = -1;

static void layout_buttons(void) {
    // Two full-width action rows + one row of four level buttons.
    for (int i = 0; i < 6; i++) {
        btns[i].label = btn_labels[i];
        btns[i].h = 24;
        if (i < 2) {
            btns[i].x = PANEL_X;
            btns[i].w = PANEL_W;
            btns[i].y = 948 + i * 28;
        } else {
            int k = i - 2;
            btns[i].w = (PANEL_W - 3 * 6) / 4;
            btns[i].x = PANEL_X + k * (btns[i].w + 6);
            btns[i].y = 1004;
        }
    }
}

static void draw_panel(void) {
    fill(PANEL_X - 4, BD_Y, PANEL_W + 8, 8 * SQ + 12, CLR_PANEL);
    rect_w(PANEL_X - 4, BD_Y, PANEL_W + 8, 8 * SQ + 12, 2, CLR_FRAME);
    draw_str_s(PANEL_X + 6, BD_Y + 16, "NYX CHESS", CLR_TEXT, CLR_PANEL, 2);

    int y = BD_Y + 64;
    if (game_over) {
        const char* msg; uint32_t col = CLR_WARN;
        if (over_kind == 1) {
            int white_lost = (pos.side > 0);
            msg = white_lost ? "PC wins" : "You win!";
            col = white_lost ? CLR_BAD : CLR_OK;
        }
        else if (over_kind == 2) msg = "Stalemate";
        else if (over_kind == 3) msg = "Draw: repetition";
        else                     msg = "Draw: 50 moves";
        draw_str_s(PANEL_X + 6, y, msg, col, CLR_PANEL, 2);
    } else if (spinner) {
        static const char sp[4] = { '|', '/', '-', '\\' };
        char buf[28]; int i = 0;
        const char* s = "PC thinking ";
        while (*s) buf[i++] = *s++;
        buf[i++] = sp[spinner & 3];
        buf[i] = 0;
        draw_str_s(PANEL_X + 6, y, buf, CLR_WARN, CLR_PANEL, 2);
    } else {
        draw_str_s(PANEL_X + 6, y,
                   pos.side > 0 ? "Your move" : "PC to move",
                   pos.side > 0 ? CLR_OK : CLR_WARN, CLR_PANEL, 2);
    }
    if (!game_over && chess_in_check(&pos))
        draw_str_s(PANEL_X + 6, y + 40, "CHECK!", CLR_BAD, CLR_PANEL, 2);

    // Level list.
    int ly = BD_Y + 170;
    draw_str_s(PANEL_X + 6, ly - 34, "Level", CLR_DIM, CLR_PANEL, 1);
    for (int l = 0; l < 4; l++) {
        uint32_t c = (l + 2 == ai_depth) ? CLR_OK : CLR_DIM;
        char row[24]; int i = 0;
        row[i++] = (l + 2 == ai_depth) ? '>' : ' ';
        row[i++] = ' ';
        const char* s = lvl_names[l];
        while (*s && i < 20) row[i++] = *s++;
        row[i] = 0;
        draw_str_s(PANEL_X + 6, ly + l * 32, row, c, CLR_PANEL, 1);
    }

    // Buttons.
    for (int b = 0; b < 6; b++) {
        uint32_t bg = (b == btn_hover) ? CLR_BTN_HV : CLR_BTN;
        fill(btns[b].x, btns[b].y, btns[b].w, btns[b].h, bg);
        rect_w(btns[b].x, btns[b].y, btns[b].w, btns[b].h, 2, CLR_FRAME);
        draw_str_c(btns[b].x, btns[b].w, btns[b].y + 4, btns[b].label,
                   CLR_TEXT, bg, 1);
    }
}

static void draw_promo(void) {
    // Dim the board, centered dialog, four clickable piece tiles.
    for (int j = 0; j < H; j += 2)
        for (int i = 0; i < W; i += 2) {
            uint32_t c = fb[j * W + i];
            unsigned b = (c & 0xFF) >> 1, g = ((c >> 8) & 0xFF) >> 1,
                     r = ((c >> 16) & 0xFF) >> 1;
            fb[j * W + i] = (r << 16) | (g << 8) | b;
        }
    int bw = 640, bh = 260;
    int bx = (W - bw) / 2, by = 380;
    fill(bx, by, bw, bh, CLR_PANEL);
    rect_w(bx, by, bw, bh, 4, CLR_OK);
    draw_str_c(bx, bw, by + 20, "Promote to:", CLR_TEXT, CLR_PANEL, 2);
    for (int i = 0; i < 4; i++) {
        int x = bx + 60 + i * 132, y = by + 76;
        fill(x, y, 96, 96, CLR_BG);
        rect_w(x, y, 96, 96, 2, CLR_FRAME);
        draw_piece_at(x + 4, y + 4, 88,
                      (int8_t)((QUEEN - i) * pos.side));
        static const char* pk[4] = { "Q", "R", "B", "N" };
        draw_str_c(x, 96, y + 104, pk[i], CLR_DIM, CLR_PANEL, 2);
    }
}
static int promo_hit(int mx, int my) {
    int bx = (W - 640) / 2, by = 380;
    for (int i = 0; i < 4; i++) {
        int x = bx + 60 + i * 132, y = by + 76;
        if (mx >= x && mx < x + 96 && my >= y && my < y + 96) return i;
    }
    return -1;
}

// ─────────────────────────────────────────────────────────────────
// Chat sidebar — talk to the PC (offline rule-based personality +
// real position analysis: captures, checks, game over, hints).
// Lives in the left margin (x 8..468).
// ─────────────────────────────────────────────────────────────────
#define CHAT_X      8
#define CHAT_W      460
#define CHAT_H      980
#define CHAT_LINES  120                     // ring capacity
#define CHAT_LEN    54                      // max chars per line (fits 440px)
#define CHAT_INPUT  44                      // input box chars

static char  chat_buf[CHAT_LINES][CHAT_LEN];
static uint8_t chat_you[CHAT_LINES];         // 1 = typed by the player
static int   chat_head;                      // next slot to write
static int   chat_n;                         // lines stored
static int   chat_scroll;                    // lines hidden from the bottom
static int   chat_focus;                     // typing (Enter toggles)
static int   chat_shift;                     // raw shift state for typing
static char  chat_in[CHAT_INPUT + 1];
static int   chat_in_n;
static int   chat_needs_frame;               // set by chat_key() to force redraw

static void chat_push(const char* s, int you) {
    // Hard-wrap into CHAT_LEN lines so long answers stay readable.
    int pos_i = 0;
    for (;;) {
        int take = 0;
        while (s[pos_i + take] && take < CHAT_LEN - 1) take++;
        char* d = chat_buf[chat_head];
        for (int i = 0; i < take; i++) d[i] = s[pos_i + i];
        d[take] = 0;
        chat_you[chat_head] = (uint8_t)you;
        chat_head = (chat_head + 1) % CHAT_LINES;
        if (chat_n < CHAT_LINES) chat_n++;
        else chat_scroll = 0;                // newer lines matter more
        pos_i += take;
        if (!s[pos_i]) break;
    }
}

static void chat_printf(const char* s) { chat_push(s, 0); }
static void chat_say(const char* s) { chat_push(s, 1); }

// Scancode → ASCII (US QWERTY, SET-2 make codes; shift handled by the
// caller's state).  0 = not typeable.
static char chat_sc_to_char(int sc, int sh) {
    static const char map_l1[13] = { '1','2','3','4','5','6','7','8','9','0','-','=' };
    static const char map_u1[13] = { '!','@','#','$','%','^','&','*','(',')','_','+' };
    static const char map_l2[26] = { 'q','w','e','r','t','y','u','i','o','p',
                                     '[',']', 0, 0, 'a','s','d','f','g','h',
                                     'j','k','l',';','\'', '`' };
    static const char map_l3[10] = { 'z','x','c','v','b','n','m',',','.','/' };
    if (sc >= 0x02 && sc <= 0x0D) {
        int k = sc - 0x02;
        return sh ? map_u1[k] : map_l1[k];
    }
    if (sc >= 0x10 && sc <= 0x28) {
        char c = map_l2[sc - 0x10];
        if (sh && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if (sh && c == ';') return ':';
        return c;
    }
    if (sc == 0x29) return sh ? '~' : '`';
    if (sc == 0x2B) return sh ? '|' : '\\';
    if (sc >= 0x2C && sc <= 0x35) {
        char c = map_l3[sc - 0x2C];
        if (sh && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        return c;
    }
    return 0;
}

// ── Mood: derived from a cheap periodic search, PC's perspective ──
#define MOOD_NEUT   0
#define MOOD_SMUG   1
#define MOOD_CONF   2
#define MOOD_WARY   3
#define MOOD_PANIC  4
static int    chat_mood = MOOD_NEUT;
static int    chat_mood_cp;              // centipawns, PC's view
static int16_t chat_mood_ply = -1;       // ply the mood was computed at

static const char* const mood_word[5] =
    { "calm", "smug", "cocky", "wary", "panicking" };
static const uint32_t mood_col[5] =
    { CLR_DIM, CLR_OK, CLR_TEXT, CLR_WARN, CLR_BAD };

static unsigned chat_rng_s = 0x1234ABCu;
static int chat_r4(void) {               // 0..3, xorshift32
    chat_rng_s ^= chat_rng_s << 13;
    chat_rng_s ^= chat_rng_s >> 17;
    chat_rng_s ^= chat_rng_s << 5;
    return (int)(chat_rng_s & 3);
}

static void chat_mood_set(int pc_cp) {
    int m = (pc_cp > 200) ? MOOD_SMUG
          : (pc_cp >  80) ? MOOD_CONF
          : (pc_cp > -80) ? MOOD_NEUT
          : (pc_cp > -200) ? MOOD_WARY : MOOD_PANIC;
    chat_mood_cp = pc_cp;
    if (m != chat_mood) chat_needs_frame = 1;   // face changed
    chat_mood = m;
    chat_mood_ply = pos.ply;
}

// Re-evaluate mood at most once per position (depth-2 search is cheap).
static void chat_mood_refresh(void) {
    if (pos.ply == chat_mood_ply) return;
    chess_result_t r;
    chess_search(&pos, 2, 60000, &r);
    int cp = r.score;                    // searching side's view
    if (pos.side > 0) cp = -cp;          // to the PC's (black) view
    chat_mood_set(cp);
}

// Per-mood line pools (4 lines each; rotate per mood).
static const char* const chat_taunts[5][4] = {
    { "Nothing personal.",
      "Keep the tension on the board.",
      "A quiet move hides loud ideas.",
      "Chess is hard. You're doing fine." },
    { "That's chess, not checkers.",
      "Resignation is always an option.",
      "My searches dream of positions like this.",
      "You're playing exactly as I calculated." },
    { "Developing. Slowly. Very slowly.",
      "You walked into my plan. Finally.",
      "I like this position. A lot.",
      "The center belongs to me today." },
    { "Maintaining pressure. Please panic.",
      "This is far from over.",
      "Careful. My pieces are coordinated.",
      "I smell a counterattack coming." },
    { "I meant to do that.",
      "Stalling. Stalling works.",
      "Ha ha. Everything is fine here.",
      "My pawns are strategic sacrifices." }
};

static void chat_taunt(void) {
    static int pick[5];
    chat_printf(chat_taunts[chat_mood][pick[chat_mood]++ % 4]);
}

static void chat_hint_line(void) {
    chess_result_t r;
    chess_search(&pos, 3, 120000, &r);
    {   // the hint's deep eval doubles as a mood update (free)
        int cp = (pos.side > 0) ? -r.score : r.score;
        chat_mood_set(cp);
    }
    char a[3] = "..", b[3] = "..";
    chess_sq_name(r.best.from, a);
    chess_sq_name(r.best.to, b);
    char out[48]; int i = 0;
    const char* p = "Try ";
    while (*p) out[i++] = *p++;
    out[i++] = a[0]; out[i++] = a[1]; out[i++] = ' '; out[i++] = b[0]; out[i++] = b[1];
    if (r.mate) { const char* m = " - looks like mate!"; while (*m && i < 46) out[i++] = *m++; }
    else if (r.score > 150) { const char* m = " - I'm winning here."; while (*m && i < 46) out[i++] = *m++; }
    else if (r.score < -150) { const char* m = "... things look grim."; while (*m && i < 46) out[i++] = *m++; }
    out[i] = 0;
    chat_printf(out);
}

// Case-insensitive substring test on the lowercased message.
static int chat_has(const char* low, const char* sub) {
    for (int i = 0; low[i]; i++) {
        int j = 0;
        while (sub[j] && low[i + j] == sub[j]) j++;
        if (!sub[j]) return 1;
    }
    return 0;
}
static int chat_eq(const char* a, const char* b) {
    int i = 0;
    while (a[i] && a[i] == b[i]) i++;
    return a[i] == b[i];
}
static void chat_mood_line(void) {
    char out[24]; int i = 0;
    const char* p = "Mood: ";
    while (*p) out[i++] = *p++;
    const char* w = mood_word[chat_mood];
    while (*w && i < 22) out[i++] = *w++;
    out[i] = 0;
    chat_printf(out);
}

static void chat_reply(const char* msg) {
    // Normalize to lowercase in place (max 31 chars matched).
    char low[32]; int i = 0;
    while (msg[i] && i < 31) {
        char c = msg[i];
        low[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
        i++;
    }
    low[i] = 0;

    // ── commands ──────────────────────────────────────────────
    if (chat_has(low, "hint")) {
        if (game_over) { chat_printf("Game's over - press N for a new one."); return; }
        if (pos.side < 0) { chat_printf("Hold on - I'm thinking!"); return; }
        chat_hint_line();
        return;
    }
    if (chat_has(low, "mood")) {
        chat_mood_refresh();
        chat_mood_line();
        return;
    }
    if (chat_has(low, "trivia")) {
        static const char* trivia[] = {
            "The longest recorded game lasted 269 moves.",
            "Deep Blue beat Kasparov in 1997. I rest my case.",
            "The word 'stalemate' dates back to the 15th century.",
            "A rook + king can mate. A lone bishop never can.",
            "En passant was invented in the 15th century.",
            "The shortest mate needs two moves: 1.f3 e5 2.g4 Qh4.",
            "Kasparov was world champion at 22 - youngest ever.",
            "More possible games than atoms in the universe."
        };
        static int tp;
        chat_printf(trivia[tp++ % 8]);
        return;
    }
    if (chat_has(low, "help")) {
        chat_printf("Commands: hint, mood, trivia, gg,");
        chat_printf("new (press N). Or just talk.");
        return;
    }
    if (chat_has(low, "gg")) {
        chat_printf("gg wp. Rematch? Press N.");
        return;
    }
    if (chat_has(low, "new game") || chat_eq(low, "new")) {
        start_game();
        chat_printf("Fresh board. You're white - go!");
        return;
    }
    if (chat_has(low, "thank")) { chat_printf("Any time."); return; }
    if (chat_has(low, "luck"))  { chat_printf("Chess is 99% calculation, 1% luck."); return; }

    // ── greetings ────────────────────────────────────────────
    if (chat_has(low, "hello") || chat_has(low, "hi ") ||
        chat_eq(low, "hi") || chat_eq(low, "hi!")) {
        static const char* hellos[5] = {
            "Hello. Prepare to be out-searched.",
            "Greetings. Type 'hint' if scared.",
            "Hi. The board favors me today.",
            "Hey. Bring your best moves.",
            "Hello there. Mind the h-file."
        };
        static int hp;
        chat_printf(hellos[hp++ % 5]);
        return;
    }
    if (chat_has(low, "how are")) {
        static const char* hows[5] = {
            "Running my search - all circuits green.",
            "Never better. More nodes, more power.",
            "My eval says we're both doing great.",
            "A bit computed, but thanks for asking.",
            "Focused. This position demands it."
        };
        static int kp;
        chat_printf(hows[kp++ % 5]);
        return;
    }

    // ── provoke / taunt detection ─────────────────────────────
    if (chat_has(low, "bad at") || chat_has(low, "stupid") ||
        chat_has(low, "weak") || chat_has(low, "scared") ||
        chat_has(low, "slow") || chat_has(low, "dumb") ||
        chat_has(low, "easy") || chat_has(low, "loser")) {
        chat_mood_refresh();
        chat_taunt();
        return;
    }
    if (chat_has(low, "you suck") || chat_has(low, "garbage") ||
        chat_has(low, "trash") || chat_has(low, "terrible") ||
        chat_has(low, "awful")) {
        chat_mood_refresh();
        chat_taunt();
        chat_printf("Bold words for a human.");
        return;
    }

    // ── fallback: mood small talk ─────────────────────────────
    chat_mood_refresh();
    chat_taunt();
}

// Game-event hooks (called from apply_move / start_game).
static void chat_event_newgame(void) {
    chat_mood = MOOD_NEUT;
    chat_mood_cp = 0;
    chat_mood_ply = -1;
    chat_printf("New game. You're white. Enter to chat:");
    chat_printf("'hint', 'mood', 'trivia'. Good luck!");
}

static void chat_event_player_move(const chess_move_t* m, int cap_t, int chk) {
    (void)m;
    chat_mood_refresh();                  // your move changed my outlook
    if (cap_t >= QUEEN) {
        chat_printf(chat_mood == MOOD_PANIC
                        ? "My queen?! I... need a moment."
                        : "My queen! Ruthless.");
    }
    else if (cap_t == ROOK) chat_printf(chat_mood == MOOD_PANIC
                        ? "First the rook. Then the world. Not!"
                        : "Ouch. That rook had a family.");
    else if (cap_t)         chat_printf(chat_mood == MOOD_SMUG
                        ? "A trade. That helps me, actually."
                        : "A trade. Fair enough.");
    else if (chk)           chat_printf("Check?! Not bad, human.");
    else if ((m->flags & MV_CASTLE)) chat_printf("Castling. Safe and sound.");
    else if ((hist_n & 7) == 3) chat_taunt();
}

static void chat_event_ai_move(const chess_move_t* m, int cap_t, int chk) {
    (void)m;
    if (chk) {
        chat_printf(chat_mood == MOOD_SMUG
                        ? "Check. Bow before the h-file."
                        : "Check. Your king, I believe?");
    }
    else if (cap_t >= QUEEN) chat_printf("Queen taken. My sincere apologies.");
    else if (cap_t == ROOK)  chat_printf(chat_mood == MOOD_PANIC
                        ? "Grabbed a rook. Situation stable-ish."
                        : "That rook was weighing me down.");
    else if (cap_t)          chat_printf("Material evened up.");
    else if (chat_mood == MOOD_SMUG || chat_mood == MOOD_CONF) {
        if ((hist_n & 3) == 1) chat_taunt();     // winning: gloat sometimes
    }
    else if (chat_mood == MOOD_PANIC && (hist_n & 3) == 1)
        chat_printf("Don't look at my king. Look away.");
}

static void chat_event_over(void) {
    if (over_kind == 1) {
        int white_lost = (pos.side > 0);
        chat_printf(white_lost ? "Checkmate - the machine prevails!"
                               : "Checkmate... you got me. Well played!");
    }
    else if (over_kind == 2) chat_printf("Stalemate. A draw, then.");
    else if (over_kind == 3) chat_printf("Draw by repetition. Rhythm lovers.");
    else                     chat_printf("Draw - 50 quiet moves.");
    chat_printf("Press N for a rematch.");
    chat_mood_set(0);
}

// Enter toggles focus; while focused, raw SET-2 scancodes become text.    // Returns 1 when the key was consumed by chat.
static int chat_key(int ev) {
    chat_needs_frame = 1;                    // typing state changed
    if (ev & 0x200) {                        // release: only shift breaks matter
        int rsc = ev & 0xFF;
        if (rsc == 0xAA || rsc == 0xB6) chat_shift = 0;
        return chat_focus;                   // focused: swallow releases too
    }
    if (ev & 0x100) {                        // E0-extended (arrows etc.)
        if (!chat_focus) return 0;
        int sc = ev & 0xFF;
        if (sc == 0x48) { if (chat_scroll < chat_n - 4) chat_scroll++; return 1; }
        if (sc == 0x50) { if (chat_scroll > 0) chat_scroll--; return 1; }
        return 1;                            // swallow other extended keys
    }
    int sc = ev & 0xFF;
    if (sc == 0x2A || sc == 0x36) { chat_shift = 1; return chat_focus; }
    if (!chat_focus) {
        if (sc == 0x10) { chat_focus = 1; return 1; }   // Q opens chat
        return 0;
    }
    if (sc == 0x01) {                        // Esc: leave chat (don't quit)
        chat_focus = 0;
        return 1;
    }
    if (sc == 0x1C) {                        // Enter: send
        if (chat_in_n) {
            chat_say(chat_in);
            chat_reply(chat_in);
            chat_in_n = 0; chat_in[0] = 0;
        } else {
            chat_focus = 0;                  // empty Enter leaves chat
        }
        return 1;
    }
    if (sc == 0x0E) {                        // Backspace
        if (chat_in_n) chat_in[--chat_in_n] = 0;
        return 1;
    }
    if (sc == 0x39) {                        // Space
        if (chat_in_n < CHAT_INPUT) { chat_in[chat_in_n++] = ' '; chat_in[chat_in_n] = 0; }
        return 1;
    }
    char c = chat_sc_to_char(sc, chat_shift);
    if (c && chat_in_n < CHAT_INPUT) {
        chat_in[chat_in_n++] = c;
        chat_in[chat_in_n] = 0;
    }
    return 1;
}

static int chat_input_hit(int mx, int my) {
    return mx >= CHAT_X && mx < CHAT_X + CHAT_W &&
           my >= CHAT_H - 46 && my < CHAT_H - 6;
}

static void chat_hist_click(int mx, int my) {
    // Anywhere in the history area: click above center = scroll up.
    (void)mx;
    if (my < CHAT_H / 2) { if (chat_scroll < chat_n - 4) chat_scroll++; }
    else                 { if (chat_scroll > 0) chat_scroll--; }
}

// ── Mood face: a small readable face in the chat header that
// changes expression with the PC's mood (calm / smug / panicking).
// Placed in the header, right of the title, vertically centred with it.
static void draw_mood_face(void) {
    // Face box: 46 wide × 40 tall, right-aligned in the header.
    const int fx = CHAT_X + CHAT_W - 104;     // left edge of face box
    const int fy = 18;                         // top edge (title sits at y=30)
    const int cx = fx + 23;                    // face centre x
    const int cy = fy + 22;                    // face centre y
    uint32_t c = mood_col[chat_mood];

    // Head: outline + mood-tinted fill.
    disc(cx, cy, 21, CLR_FRAME);
    disc(cx, cy, 19, c);
    disc(cx, cy, 17, CLR_CHATBG);

    // Brows — set once, read by each mood.
    static const struct { int y; int h; } brow[5] =
        { { cy - 10, 3 }, { cy - 10, 3 }, { cy - 10, 3 },
          { cy - 11, 4 }, { cy - 11, 4 } };
    int by = brow[chat_mood].y, bh = brow[chat_mood].h;

    // Calm / smug / confident: angled brows.
    if (chat_mood <= MOOD_CONF) {
        // Left brow: down-right. Right brow: down-left.
        rect(fx + 9, by + 1, 7, bh, c);
        rect(fx + 27, by + 1, 7, bh, c);
    } else {
        // Worried / panicking: horizontal worried brows.
        fill(fx + 9, by + 1, 9, bh, c);
        fill(fx + 26, by + 1, 9, bh, c);
    }

    // Eyes (always drawn as an open eye with a pupil, so the face reads).
    // Outer eye = mood-tinted disc; pupil = darker dot; iris hole = bg.
    int ew = 6, eh = 7;                       // eye box 6×7
    int el = cx - 9, er = cx + 3;             // left / right eye left edge
    int ey = cy - 7;                          // eye top
    // left eye
    disc(el + 3, ey + 4, ew, c); disc(el + 3, ey + 4, ew - 2, CLR_CHATBG);
    disc(el + 3, ey + 4, 2, CLR_DIM);        // pupil (reads as an eye)
    // right eye
    disc(er + 3, ey + 4, ew, c); disc(er + 3, ey + 4, ew - 2, CLR_CHATBG);
    disc(er + 3, ey + 4, 2, CLR_DIM);

    // Nose: tiny dot below the eyes, common to every mood.
    fill(cx - 1, cy + 4, 3, 2, CLR_DIM);

    // Mouth — the main expression cue.
    switch (chat_mood) {
    case MOOD_NEUT:                              // calm: closed gentle smile
        fill(cx - 8, cy + 10, 17, 2, c);
        fill(cx - 5, cy + 12, 1, 2, c);
        fill(cx + 4, cy + 12, 1, 2, c);
        break;
    case MOOD_SMUG:                              // smug: one-sided smirk
        fill(cx - 9, cy + 10, 20, 2, c);
        fill(cx - 9, cy + 10, 1, 2, c);
        fill(cx + 10, cy + 10, 1, 2, c);
        // raised corner to the right
        fill(cx + 10, cy + 8, 3, 2, c);
        break;
    case MOOD_CONF:                              // confident: full grin
        fill(cx - 9, cy + 7, 19, 2, c);
        fill(cx - 9, cy + 7, 1, 3, c);
        fill(cx + 9, cy + 7, 1, 3, c);
        fill(cx - 5, cy + 10, 11, 3, c);
        break;
    case MOOD_WARY: {                            // worried: frown + squint
        fill(cx - 9, cy + 12, 19, 2, c);
        fill(cx - 9, cy + 12, 1, 2, c);
        fill(cx + 9, cy + 12, 1, 2, c);
        break;
    }
    case MOOD_PANIC: {                           // panicking: wide open mouth
        disc(cx, cy + 11, 6, c); disc(cx, cy + 11, 3, CLR_CHATBG);
        fill(cx - 2, cy + 14, 5, 2, c);
        // sweat beads (worried detail)
        disc(fx + 2, fy + 2, 2, CLR_SEL);
        disc(fx + 40, fy + 2, 2, CLR_SEL);
        break;
    }
    }
}

static void draw_chat(void) {
    fill(CHAT_X, 0, CHAT_W, CHAT_H, CLR_CHATBG);
    rect_w(CHAT_X, 0, CHAT_W, CHAT_H, 2, CLR_FRAME);
    draw_str_s(CHAT_X + 10, 30, "CHAT with the PC", CLR_TEXT, CLR_CHATBG, 1);
    draw_mood_face();
    draw_str_s(CHAT_X + 10, 50,
               chat_focus ? "Enter sends - Enter clears to leave"
                          : "press Q or click the box to chat",
               CLR_DIM, CLR_CHATBG, 1);

    // History: 15 rows, bottom-anchored above the input box.  Speaker
    // is a 3px color bar (blue = you, white = PC); color, not prefix,
    // so a full 53-char line still fits the 440px sidebar width.
    const int rows = 15, rh = 34;
    int shown = 0;
    for (int k = chat_n - 1 - chat_scroll; k >= 0 && shown < rows; k--, shown++) {
        int idx = k % CHAT_LINES;
        const char* l1 = chat_buf[idx];
        if (!l1[0]) continue;
        uint32_t c1 = chat_you[idx] ? CLR_YOU : CLR_TEXT;
        int y = CHAT_H - 92 - shown * rh;
        fill(CHAT_X + 4, y + 2, 3, 14, c1);
        draw_str_s(CHAT_X + 10, y, l1, c1, CLR_CHATBG, 1);
    }
    if (chat_scroll > 0)
        draw_str_s(CHAT_X + CHAT_W - 90, 30, "...more", CLR_WARN, CLR_CHATBG, 1);

    // Input box.
    int iy = CHAT_H - 46;
    fill(CHAT_X + 8, iy, CHAT_W - 16, 40, CLR_BTN);
    rect_w(CHAT_X + 8, iy, CHAT_W - 16, 40, 2, chat_focus ? CLR_OK : CLR_FRAME);
    char ib[CHAT_INPUT + 2]; int i = 0;
    const char* pre = chat_focus ? "" : "press Q";
    while (pre[i]) { ib[i] = pre[i]; i++; }
    for (int k = 0; k < chat_in_n && i < CHAT_INPUT; k++) ib[i++] = chat_in[k];
    if (chat_focus && (spinner & 8)) ib[i++] = '_';
    ib[i] = 0;
    draw_str_s(CHAT_X + 16, iy + 12, ib, chat_focus ? CLR_TEXT : CLR_DIM,
               CLR_BTN, 1);
}

static void draw_strips(void) {
    // Top strip: title.
    fill(0, 0, W, BD_Y - 6, CLR_BG);
    draw_str_s(20, 16, "NYX CHESS", CLR_TEXT, CLR_BG, 2);
    draw_str_s(280, 26, "click a piece, then a highlighted square",
               CLR_DIM, CLR_BG, 1);
    // Bottom strip: material + move counter + hints.
    fill(0, STATUS_Y, W, H - STATUS_Y, CLR_PANEL);
    char mb[64]; char* q = mb;
    const char* p = "Material ";
    while (*p) *q++ = *p++;
    utoa_dec(material_sum(&pos), q);
    draw_str_s(20, STATUS_Y + 15, mb, CLR_DIM, CLR_PANEL, 1);

    const char* mp = "   Move ";
    q = mb; while (*mp) *q++ = *mp++;
    utoa_dec(pos.ply / 2 + 1, q);
    draw_str_s(280, STATUS_Y + 15, mb, CLR_DIM, CLR_PANEL, 1);

    draw_str_s(W - 480, STATUS_Y + 15,
               "Esc quit · N new · U level · Z undo",
               CLR_DIM, CLR_PANEL, 1);
}

// ─────────────────────────────────────────────────────────────────
// Input
// ─────────────────────────────────────────────────────────────────
#define SC_ESCAPE 0x01
#define SC_Q 0x10
#define SC_R 0x13
#define SC_U 0x16
#define SC_B 0x30
#define SC_Z 0x2C
#define SC_N 0x31

static int mouse_in(int x, int y, int w, int h) {
    return mouse_x >= x && mouse_x < x + w && mouse_y >= y && mouse_y < y + h;
}

static void undo_move(void) {
    if (chat_focus) return;                  // chat owns the keyboard
    if (hist_n == 0) return;
    // Pop until white is to move again (undoes the AI reply too).
    do {
        hist_n--;
        pos = hist[hist_n].pos;
    } while (hist_n > 0 && pos.side < 0);
    hist_sig_n = hist_n + 1;
    if (hist_sig_n > HIST_MAX) hist_sig_n = HIST_MAX;
    sel = -1; last_from = last_to = -1;
    ai_from = ai_to = -1;
    game_over = 0; over_kind = 0;
    promo_pending = 0;
    refresh_legal();
}

static void do_promo(int idx) {
    chess_move_t m = promo_base;
    m.promo = (uint8_t)(QUEEN - idx);
    apply_move(&m);
    promo_pending = 0;
}

static void ui_click(int cx, int cy) {
    if (promo_pending) {
        int i = promo_hit(cx, cy);
        if (i >= 0) do_promo(i);
        else promo_pending = 0;
        return;
    }
    // Chat sidebar clicks (works even when the PC is thinking).
    if (cx >= CHAT_X && cx < CHAT_X + CHAT_W) {
        if (chat_input_hit(cx, cy)) chat_focus = 1;
        else chat_hist_click(cx, cy);
        return;
    }
    for (int b = 0; b < 6; b++)
        if (mouse_in(btns[b].x, btns[b].y, btns[b].w, btns[b].h)) {
            if (b == 0) start_game();
            else if (b == 1) undo_move();
            else { ai_depth = 2 + (b - 2); }
            return;
        }

    int sq = screen_to_sq(cx, cy);
    if (sq < 0) { sel = -1; return; }
    if (pos.side < 0 || game_over) return;

    if (sel >= 0) {
        for (int i = 0; i < legal_n; i++) {
            if (legal[i].from != sel || legal[i].to != sq) continue;
            if (legal[i].promo) { promo_base = legal[i]; promo_pending = 1; }
            else apply_move(&legal[i]);
            return;
        }
    }
    if (pos.board[sq] > 0) sel = sq; else sel = -1;
}

static int pump_input(void) {
    int any = 0;
    int dx, dy;
    int b = nyx_sc(SYS_GETMOUSE, (int)&dx, (int)&dy, 0);
    if (b >= 0) {
        // The kernel feeds the DOOM convention: dy > 0 = pushed forward
        // (physical up).  Screen space wants down = +y, so negate.
        if (dx || dy || (b & 7)) any = 1;
        cursor_apply(dx, dy);
        int nb = b & 7;
        if ((nb & 1) && !(mouse_btn & 1)) ui_click(mouse_x, mouse_y);
        mouse_btn = nb;
    }



    for (;;) {
        int ev = nyx_sc(SYS_GETKEY, 0, 0, 0);
        if (ev < 0) break;
        any = 1;
        int sc = ev & 0xFF;

        if (chat_key(ev)) continue;        // chat consumed the key
        if (ev & 0x200) continue;          // release

        if (promo_pending) {
            if      (sc == SC_Q) do_promo(0);
            else if (sc == SC_R) do_promo(1);
            else if (sc == SC_B) do_promo(2);
            else if (sc == SC_N) do_promo(3);
            else promo_pending = 0;
            continue;
        }
        if (sc == SC_ESCAPE) { nyx_sc(SYS_GAMESTOP, 0, 0, 0); nyx_exit(0); }
        else if (sc == SC_N) start_game();
        else if (sc == SC_Z) undo_move();
        else if (sc == SC_U) ai_depth = (ai_depth < 5) ? ai_depth + 1 : 2;
    }
    return any;
}

static void update_hover(void) {
    hover_sq = screen_to_sq(mouse_x, mouse_y);
    btn_hover = -1;
    for (int b = 0; b < 6; b++)
        if (mouse_in(btns[b].x, btns[b].y, btns[b].w, btns[b].h))
            btn_hover = b;
}

static void render(void) {
    fill(0, 0, W, H, CLR_BG);
    draw_board();
    draw_panel();
    draw_strips();
    draw_chat();
    if (promo_pending) draw_promo();

    // Cursor: crosshair.
    fill(mouse_x, mouse_y, 2, 16, 0xFFFFFF);
    fill(mouse_x - 6, mouse_y + 6, 16, 2, 0xFFFFFF);
}

// ─────────────────────────────────────────────────────────────────
// Main loop
// ─────────────────────────────────────────────────────────────────
int main(void) {
    memset(hist, 0, sizeof(hist));
    if (!zone_init()) { nyx_log("[CHESS] no zone\n"); nyx_exit(1); }
    fb = (uint32_t*)zone_alloc(W * H * 4);
    if (!fb) { nyx_log("[CHESS] no fb\n"); nyx_exit(2); }

    // Full-screen canvas: GAMESTART args b/c = width/height.
    if (!nyx_sc(SYS_GAMESTART, (int)"Chess", W, H)) {
        nyx_log("[CHESS] GAMESTART refused\n"); nyx_exit(3);
    }

    nyx_log("[CHESS] up\n");
    snd_init();
    layout_buttons();
    start_game();

    // Paint the first frame immediately — the idle-skip below only
    // redraws on input/AI activity, so without this the screen stays
    // blank until the first mouse move or keypress.
    render();
    nyx_sc(SYS_GAMEBLIT, (int)fb, 0, 0);

    for (;;) {
        // AI move: run synchronously with a node budget (the search is
        // depth-limited and node-capped, so this stays < ~1 s/frame).
        int ai_done = 0;
        if (!game_over && !promo_pending && pos.side < 0) {
            spinner++;
            if (spinner >= 30) {           // ~0.6 s of visible "thinking"
                spinner = 0;
                ai_move();
                ai_done = 1;
            }
        } else {
            spinner = 0;
        }

        int any = pump_input();
        update_hover();

        // Redraw + blit only when something changed (input, AI move),
        // plus a few frames while the "PC thinking" spinner animates.
        // Idle ticks skip rendering AND the blit entirely — the kernel
        // canvas keeps showing the last frame.
        int redraw = any || ai_done || chat_needs_frame ||
                     (spinner && (spinner & 7) == 0);
        chat_needs_frame = 0;
        if (redraw) {
            render();
            nyx_sc(SYS_GAMEBLIT, (int)fb, 0, 0);
        }
        nyx_sc(SYS_SLEEP, 1, 0, 0);
    }
    return 0;
}

// PE entry point (elf2pe.py maps this symbol).
__attribute__((section(".text.entry")))
void nyx_pe_entry(void) { main(); }
