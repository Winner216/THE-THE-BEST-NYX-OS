// cpu/lockscreen.c — Nyx OS Lock Screen
// Full VESA pixel rendering with cosmic purple theme,
// matching the login screen's aesthetic. VGA text fallback
// preserved for non-VESA boots.
#include <lockscreen.h>
#include <auth.h>
#include <speaker.h>
#include <rtc.h>
#include <stdint.h>
#include <desktop.h>
#include <vesa.h>

// ═══════════════════════════════════════════════════════════════════
//  VGA FALLBACK (unchanged)
// ═══════════════════════════════════════════════════════════════════
#define VGA_WIDTH  80
#define VGA_HEIGHT 25
#define VGA_MEM    ((uint16_t*)0xB8000)

static inline uint16_t make_entry(char c, uint8_t fg, uint8_t bg) {
    return (uint16_t)c | (uint16_t)(fg | bg << 4) << 8;
}

static void lput(int x, int y, char c, uint8_t fg, uint8_t bg) {
    if (x < 0 || x >= VGA_WIDTH || y < 0 || y >= VGA_HEIGHT) return;
    VGA_MEM[y * VGA_WIDTH + x] = make_entry(c, fg, bg);
}

static void lputs(int x, int y, const char* s, uint8_t fg, uint8_t bg) {
    for (int i = 0; s[i]; i++)
        lput(x + i, y, s[i], fg, bg);
}

static void lfill(int x, int y, int w, int h, char c, uint8_t fg, uint8_t bg) {
    for (int dy = 0; dy < h; dy++)
        for (int dx = 0; dx < w; dx++)
            lput(x + dx, y + dy, c, fg, bg);
}

static void lcenter(int y, const char* s, uint8_t fg, uint8_t bg) {
    int len = 0; while (s[len]) len++;
    lputs((VGA_WIDTH - len) / 2, y, s, fg, bg);
}

// ═══════════════════════════════════════════════════════════════════
//  VESA COLORS (matching login.c's cosmic purple aesthetic)
// ═══════════════════════════════════════════════════════════════════
#define C_BG      0x0C0820
#define C_BG2     0x150E30
#define C_BG3     0x1A1140
#define C_CARD    0x140C30
#define C_FLD     0x0E0824
#define C_ACC     0xB088FF
#define C_ACC2    0x6A4FBF
#define C_ADIM    0x3D2A66
#define C_AFAINT  0x1A1230
#define C_WHT     0xF0E8FF
#define C_GRY     0x554477
#define C_DGRY    0x2A1F40
#define C_YEL     0xFFCC00
#define C_RED     0xFF3344
#define C_GRN     0x33EE77
#define C_CYAN    0x55EEFF

// ═══════════════════════════════════════════════════════════════════
//  VESA DRAWING PRIMITIVES
// ═══════════════════════════════════════════════════════════════════
static int lk_sl(const char* s) { int l = 0; while (s[l]) l++; return l; }

static void lk_vgrad(int x, int y, int w, int h, uint32_t c1, uint32_t c2) {
    for (int dy = 0; dy < h; dy++) {
        uint32_t r = ((c1>>16)&0xFF) + (int)(((c2>>16)&0xFF)-((c1>>16)&0xFF)) * dy / h;
        uint32_t g = ((c1>>8)&0xFF)  + (int)(((c2>>8)&0xFF)-((c1>>8)&0xFF)) * dy / h;
        uint32_t b = (c1&0xFF)       + (int)((c2&0xFF)-(c1&0xFF)) * dy / h;
        vesa_hline(x, y+dy, w, (r<<16)|(g<<8)|b);
    }
}

static void lk_circ(int cx, int cy, int r, uint32_t c) {
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx*dx + dy*dy <= r*r)
                vesa_put_pixel(cx+dx, cy+dy, c);
}

static void lk_ring(int cx, int cy, int r, int t, uint32_t c) {
    int ri = r - t;
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++) {
            int d = dx*dx + dy*dy;
            if (d <= r*r && d >= ri*ri)
                vesa_put_pixel(cx+dx, cy+dy, c);
        }
}

static void lk_rr(int x, int y, int w, int h, uint32_t c) {
    vesa_fill_rect(x+2, y, w-4, h, c);
    vesa_fill_rect(x, y+2, w, h-4, c);
    vesa_fill_rect(x+1, y+1, w-2, h-2, c);
}

static void lk_rrb(int x, int y, int w, int h, uint32_t c) {
    vesa_hline(x+2, y, w-4, c); vesa_hline(x+2, y+h-1, w-4, c);
    vesa_vline(x, y+2, h-4, c); vesa_vline(x+w-1, y+2, h-4, c);
    vesa_put_pixel(x+1, y+1, c);   vesa_put_pixel(x+w-2, y+1, c);
    vesa_put_pixel(x+1, y+h-2, c); vesa_put_pixel(x+w-2, y+h-2, c);
}

static void lk_glow_text(int x, int y, const char* s, uint32_t fg, uint32_t gc) {
    uint32_t d1 = (gc>>3)&0x1F1F1F;
    uint32_t d2 = (gc>>1)&0x7F7F7F;
    vesa_draw_string(x-2, y, s, d1, 0);
    vesa_draw_string(x+2, y, s, d1, 0);
    vesa_draw_string(x, y-1, s, d2, 0);
    vesa_draw_string(x, y+1, s, d2, 0);
    vesa_draw_string(x-1, y, s, d2, 0);
    vesa_draw_string(x+1, y, s, d2, 0);
    vesa_draw_string(x, y, s, fg, 0);
}

static void lk_center(int y, const char* s, uint32_t fg) {
    int x = ((int)vesa.width - lk_sl(s)*8) / 2;
    if (x < 0) x = 0;
    vesa_draw_string(x, y, s, fg, 0);
}

// ─── Stars ────────────────────────────────────────────────────────
static void lk_stars(void) {
    static const uint16_t sx[] = {
        38,92,148,204,268,322,376,432,488,542,596,650,
        14,68,124,180,236,290,350,408,464,518,572,628,
        55,110,166,222,278,338,394,450,506,560,614,664,
        26,80,136,192,248,308,364,420,476,530,584,638
    };
    static const uint16_t sy[] = {
        22,48,16,64,38,54,26,44,18,58,34,50,
        90,75,105,82,96,72,88,100,78,94,70,108,
        530,515,545,522,538,508,524,540,512,528,500,518,
        580,565,595,572,588,558,574,590,562,578,548,568
    };
    int W = (int)vesa.width, H = (int)vesa.height;
    for (int i = 0; i < 48; i++) {
        int x = sx[i], y = sy[i];
        if (x >= W || y >= H) continue;
        uint32_t c = (i%7==0) ? 0xE0D0FF : (i%4==0) ? 0x9988CC : (i%2==0) ? 0x4A3A66 : 0x281A40;
        vesa_put_pixel(x, y, c);
        if (i%8 == 0) {
            vesa_put_pixel(x-2, y, c/4); vesa_put_pixel(x+2, y, c/4);
            vesa_put_pixel(x, y-2, c/4); vesa_put_pixel(x, y+2, c/4);
            vesa_put_pixel(x-1, y, c/2); vesa_put_pixel(x+1, y, c/2);
            vesa_put_pixel(x, y-1, c/2); vesa_put_pixel(x, y+1, c/2);
        } else if (i%4 == 0) {
            vesa_put_pixel(x-1, y, c/3); vesa_put_pixel(x+1, y, c/3);
            vesa_put_pixel(x, y-1, c/3); vesa_put_pixel(x, y+1, c/3);
        }
    }
}

// ─── Padlock icon ─────────────────────────────────────────────────
static void lk_padlock(int cx, int cy, int s) {
    // Shackle (curved top)
    int sh = s/2;
    lk_ring(cx, cy-sh, sh+2, 3, C_ACC);
    lk_ring(cx, cy-sh, sh+1, 1, 0x4A3680);
    // Shackle has a cut at the bottom-right for the body join
    for (int dx = -sh-2; dx <= sh+2; dx++) {
        int dy = cy-sh - (sh+2);
        (void)dy;
    }
    // Lock body rectangle
    int bw = s, bh = s*6/7;
    int bx = cx - bw/2, by = cy + sh/3;
    lk_rr(bx, by, bw, bh, C_CARD);
    lk_rrb(bx, by, bw, bh, C_ACC);
    // Keyhole
    lk_circ(cx, by+bh/2, 4, C_ACC);
    vesa_fill_rect(cx-2, by+bh/2+1, 4, bh/3, C_ACC);
    // Shine highlight
    vesa_hline(bx+4, by+2, bw-8, 0x4A3680);
}

// ─── Avatar circle ────────────────────────────────────────────────
static void lk_avatar(int cx, int cy, int R) {
    lk_ring(cx, cy, R+10, 2, 0x1A1230);
    lk_ring(cx, cy, R+7,  2, 0x241840);
    lk_ring(cx, cy, R+4,  2, 0x2E2050);
    lk_ring(cx, cy, R+2,  1, C_ADIM);
    // Fill with radial gradient
    for (int dy = -R; dy <= R; dy++) {
        for (int dx = -R; dx <= R; dx++) {
            int d2 = dx*dx + dy*dy;
            if (d2 > R*R) continue;
            int pct = d2 * 100 / (R*R);
            uint32_t ic = 0x281A50, oc = 0x0C0820;
            uint32_t r = ((ic>>16)&0xFF) + (int)(((oc>>16)&0xFF)-((ic>>16)&0xFF)) * pct / 100;
            uint32_t g = ((ic>>8)&0xFF)  + (int)(((oc>>8)&0xFF)-((ic>>8)&0xFF)) * pct / 100;
            uint32_t b = (ic&0xFF)       + (int)((oc&0xFF)-(ic&0xFF)) * pct / 100;
            vesa_put_pixel(cx+dx, cy+dy, (r<<16)|(g<<8)|b);
        }
    }
    lk_ring(cx, cy, R, 2, C_ACC);
    lk_ring(cx, cy, R-1, 1, 0x4A3680);
    // Anonymous mask: angular eye slits
    int ey = cy - R/6;
    int ex1 = cx - R/3, ex2 = cx + R/3;
    int slit_w = R/2, slit_h = R/10;
    if (slit_h < 2) slit_h = 2;
    for (int i = 0; i < slit_w; i++) {
        int taper = (i * slit_h) / slit_w;
        int lx = ex1 - slit_w/2 + i;
        int ly = ey + taper;
        for (int t = 0; t < slit_h; t++) {
            uint32_t c = (t==0 || t==slit_h-1) ? C_ACC : C_CYAN;
            vesa_put_pixel(lx, ly+t, c);
        }
    }
    for (int i = 0; i < slit_w; i++) {
        int taper = ((slit_w - 1 - i) * slit_h) / slit_w;
        int rx = ex2 - slit_w/2 + i;
        int ry = ey + taper;
        for (int t = 0; t < slit_h; t++) {
            uint32_t c = (t==0 || t==slit_h-1) ? C_ACC : C_CYAN;
            vesa_put_pixel(rx, ry+t, c);
        }
    }
    // Mouth line
    int my = cy + R/2;
    int mw = R*3/5;
    for (int dx = -mw/2; dx <= mw/2; dx++) {
        int edge = (mw/2) - (dx<0 ? -dx : dx);
        uint32_t c = (edge < 2) ? 0x227788 : C_ACC;
        vesa_put_pixel(cx+dx, my,   c);
        vesa_put_pixel(cx+dx, my+1, 0x115566);
    }
    // Center seam
    int seam_top = ey - R/2, seam_bot = cy + R*2/3;
    for (int y = seam_top; y < seam_bot; y++)
        if (y % 3 != 0) vesa_put_pixel(cx, y, 0x224455);
}

// ─── Lock State ───────────────────────────────────────────────────
#define INPUT_LEN 16
#define MODE_CLOCK    0
#define MODE_USERSEL  1
#define MODE_PASSWORD 2

static int  locked       = 0;
static int  lock_mode    = MODE_CLOCK;
static int  lock_failed  = 0;
static int  fail_count   = 0;
static int  sel_user     = 0;
static char password[INPUT_LEN + 1];
static int  pass_pos     = 0;

static void clear_password(void) {
    for (int i = 0; i <= INPUT_LEN; i++) password[i] = '\0';
    pass_pos    = 0;
    lock_failed = 0;
}

// ═══════════════════════════════════════════════════════════════════
//  VESA DRAW
// ═══════════════════════════════════════════════════════════════════
static void lockscreen_draw_vesa(void) {
    int W = (int)vesa.width, H = (int)vesa.height, CX = W/2, CY = H/2;

    // Deep space background
    lk_vgrad(0, 0, W, H, 0x08051A, 0x1E1240);
    // Subtle grid
    for (int y = 0; y < H; y += 28) vesa_hline(0, y, W, 0x140A28);
    for (int x = 0; x < W; x += 28) vesa_vline(x, 0, H, 0x140A28);
    // Atmospheric bottom glow
    for (int i = 0; i < 100; i++) {
        int ww = W * (100-i) / 200;
        uint32_t intensity = 0x000A16 + (uint32_t)(100-i) * 0x200 / 100;
        vesa_hline(CX - ww/2, H-1-i, ww, intensity);
    }
    lk_stars();

    // Corner brackets
    int bl = 16;
    vesa_hline(10, 10, bl, C_ADIM);    vesa_vline(10, 10, bl, C_ADIM);
    vesa_hline(W-11-bl, 10, bl, C_ADIM); vesa_vline(W-11, 10, bl, C_ADIM);
    vesa_hline(10, H-11, bl, C_ADIM);  vesa_vline(10, H-11-bl, bl, C_ADIM);
    vesa_hline(W-11-bl, H-11, bl, C_ADIM); vesa_vline(W-11, H-11-bl, bl, C_ADIM);

    // Top bar: clock + date
    char time_str[9], date_str[9];
    rtc_get_time_str(time_str);
    rtc_get_date_str(date_str);

    // Top bar gradient
    lk_vgrad(0, 0, W, 28, 0x251440, 0x0F0824);
    vesa_hline(0, 28, W, C_ACC);
    vesa_hline(0, 29, W, C_ADIM);

    // Logo
    lk_glow_text(14, 6, "* LOCKED", C_ACC, C_ACC);
    // Date + time
    vesa_draw_string(W-140, 4, date_str, C_GRY, 0);
    lk_glow_text(W-140, 18, time_str, C_WHT, 0x4A3A66);
    // Status indicators
    vesa_draw_string(W-240, 10, "[SEC]", C_GRN, 0);
    vesa_draw_string(W-190, 10, "[PWR]", C_YEL, 0);

    // Bottom bar
    lk_vgrad(0, H-28, W, 28, 0x0F0824, 0x251440);
    vesa_hline(0, H-29, W, C_ACC);
    vesa_hline(0, H-30, W, C_ADIM);
    vesa_draw_string(12, H-20, "[LOCK]", C_RED, 0);
    vesa_draw_string(68, H-20, "[USER]", C_GRN, 0);
    vesa_draw_string(134, H-20, "[SEC]", C_ACC, 0);
    vesa_draw_string(W-150, H-20, "NYX OS v3.0", C_DGRY, 0);

    // ─── MODE: CLOCK (default) ─────────────────────────────────
    if (lock_mode == MODE_CLOCK) {
        // Large digital clock — center of screen
        int csize = 4; // character scale
        int ch_w = 8 * csize, ch_h = 16 * csize;

        // Padlock icon above the clock
        lk_padlock(CX, CY-140, 36);

        // Render each digit of time
        char big_time[9];
        rtc_get_time_str(big_time);
        int tw = lk_sl(big_time) * ch_w;
        int tx = CX - tw/2, ty = CY - ch_h/2 - 20;

        // Digit background card
        int cw2 = tw + 40, ch2 = ch_h + 24;
        int cx2 = CX - cw2/2, cy2 = ty - 12;
        vesa_fill_rect(cx2+4, cy2+4, cw2, ch2, 0x05030C);
        lk_rr(cx2, cy2, cw2, ch2, C_CARD);
        lk_rrb(cx2, cy2, cw2, ch2, C_ADIM);

        // Glow behind each digit
        for (int i = 0; big_time[i]; i++) {
            int dx = tx + i * ch_w;
            uint32_t gc = 0x2A1850;
            for (int s = -3; s <= 3; s++) {
                for (int t = -3; t <= 3; t++) {
                    if (s*s + t*t > 9) continue;
                    vesa_draw_char(dx+s, ty+t, big_time[i], gc, 0);
                }
            }
        }
        // Actual digits
        for (int i = 0; big_time[i]; i++) {
            int dx = tx + i * ch_w;
            if (big_time[i] == ':') {
                // Pulsing colon
                vesa_draw_char(dx, ty, ':', C_ACC, 0);
            } else {
                vesa_draw_char(dx, ty, big_time[i], C_WHT, 0);
            }
        }

        // Date below clock
        char big_date[9];
        rtc_get_date_str(big_date);
        lk_center(ty + ch_h + 12, big_date, C_GRY);

        // User info
        const char* un = (logged_in_user >= 0) ? users[logged_in_user].username : "User";
        lk_center(ty + ch_h + 36, un, C_ACC);

        // Hint text
        vesa_draw_string(CX - 100, ty + ch_h + 56, "ENTER: unlock   TAB: switch user", C_DGRY, 0);
        vesa_draw_string(CX - 80,  ty + ch_h + 74, "ESC: stay locked", C_AFAINT, 0);
    }

    // ─── MODE: USER SELECTION ──────────────────────────────────
    else if (lock_mode == MODE_USERSEL) {
        // Title
        lk_center(CY-100, "SELECT USER", C_ACC);
        vesa_hline(CX-80, CY-86, 160, C_ADIM);

        // List users
        int row_y = CY-60;
        for (int i = 0; i < MAX_USERS; i++) {
            if (!users[i].active) continue;
            int sel = (i == sel_user);
            uint32_t bg = sel ? C_ACC2 : C_BG3;
            uint32_t fg = sel ? C_WHT : C_GRY;

            int iw = 260, ih = 32;
            int ix = CX - iw/2;
            if (sel) {
                // Highlighted card
                vesa_fill_rect(ix+2, row_y+2, iw, ih, 0x05030C);
                lk_rr(ix, row_y, iw, ih, 0x352878);
                lk_rrb(ix, row_y, iw, ih, C_ACC);
                // Avatar dot
                lk_circ(ix+20, row_y+16, 8, C_ACC);
            } else {
                vesa_fill_rect(ix, row_y, iw, ih, bg);
                vesa_draw_rect(ix, row_y, iw, ih, C_ADIM);
                lk_circ(ix+20, row_y+16, 6, C_DGRY);
            }
            vesa_draw_string(ix+36, row_y+8, users[i].username, fg, bg);
            if (i == logged_in_user)
                vesa_draw_string(ix+200, row_y+8, "(current)", C_GRN, bg);

            row_y += 40;
        }

        lk_center(row_y + 20, "UP/DN: select   ENTER: choose   ESC: back", C_DGRY);
    }

    // ─── MODE: PASSWORD ────────────────────────────────────────
    else if (lock_mode == MODE_PASSWORD) {
        // Avatar
        int av_r = 48;
        lk_avatar(CX, CY-110, av_r);

        // Username
        const char* un = users[sel_user].username;
        int ul = lk_sl(un);
        vesa_draw_string(CX - ul*8/2 - 16, CY-50, "<", C_YEL, 0);
        lk_glow_text(CX - ul*8/2, CY-50, un, C_WHT, C_ACC);
        vesa_draw_string(CX + ul*8/2 + 8, CY-50, ">", C_YEL, 0);

        // "Enter Password" label
        vesa_draw_string(CX - 50, CY-20, "PASSWORD", C_GRY, 0);

        // Password field
        int fw = 260, fh = 32;
        int fx = CX - fw/2, fy = CY;
        vesa_fill_rect(fx+2, fy+2, fw, fh, 0x08051A);
        vesa_fill_rect(fx, fy, fw, fh, C_FLD);
        vesa_draw_rect(fx, fy, fw, fh, C_ACC);
        vesa_draw_rect(fx-1, fy-1, fw+2, fh+2, C_ADIM);
        vesa_draw_rect(fx-2, fy-2, fw+4, fh+4, C_AFAINT);
        vesa_hline(fx+1, fy+1, fw-2, 0x003344);

        // Password dots
        for (int i = 0; i < pass_pos; i++) {
            int dx2 = fx + 14 + i * 18;
            int dy2 = fy + 16;
            if (dx2 + 6 > fx + fw - 8) break;
            lk_circ(dx2, dy2, 5, C_ACC);
            vesa_put_pixel(dx2-1, dy2-2, 0xD8C0FF);
            vesa_put_pixel(dx2-2, dy2-1, C_CYAN);
        }
        // Cursor
        int curx = fx + 14 + pass_pos * 18;
        if (curx < fx + fw - 8) {
            vesa_fill_rect(curx-1, fy+6, 2, 20, C_YEL);
            vesa_put_pixel(curx-1, fy+5, 0x886600);
            vesa_put_pixel(curx, fy+5, 0x886600);
        }

        // Sign in button
        int bw = 200, bh = 36;
        int bx = CX - bw/2, by = fy + fh + 16;
        vesa_fill_rect(bx+3, by+3, bw, bh, 0x140A28);
        lk_vgrad(bx, by, bw, bh, C_ACC2, C_ACC);
        lk_rrb(bx, by, bw, bh, C_WHT);
        vesa_hline(bx+2, by+1, bw-4, 0xE0D0FF);
        vesa_hline(bx+2, by+2, bw-4, 0x4A3870);
        lk_center(by+10, "[ SIGN IN ]", 0x001122);

        // Error / hint
        if (lock_failed) {
            vesa_fill_rect(CX-150, by+bh+16, 300, 22, 0x1C0008);
            vesa_hline(CX-150, by+bh+16, 300, C_RED);
            vesa_hline(CX-150, by+bh+37, 300, 0x880020);
            lk_center(by+bh+18, "! WRONG PASSWORD - TRY AGAIN !", C_RED);
        } else {
            lk_center(by+bh+18, "ENTER to unlock   TAB: switch user", C_DGRY);
        }
    }

    // Version footer
    lk_center(H-50, "default: admin / 1234", C_DGRY);

    vesa_present();
}

// ═══════════════════════════════════════════════════════════════════
//  MAIN DRAW
// ═══════════════════════════════════════════════════════════════════
void lockscreen_draw(void) {
    if (!locked) return;

    if (vesa.active) {
        lockscreen_draw_vesa();
        return;
    }

    // ─── VGA FALLBACK ──────────────────────────────────────────
    lfill(0, 0, VGA_WIDTH, VGA_HEIGHT, ' ', 0, 0);
    int stars[][2] = {
        {5,0},{15,1},{25,0},{35,1},{45,0},{55,1},{65,0},{75,1},
        {3,21},{70,22},{8,20},{72,19},{78,3},{2,4},{10,17},{68,18},
        {20,10},{60,10},{1,12},{79,12},{30,2},{50,22},
    };
    for (int i = 0; i < 22; i++)
        lput(stars[i][0], stars[i][1], '.', 8, 0);

    char time_str[9]; char date_str[9];
    rtc_get_time_str(time_str);
    rtc_get_date_str(date_str);
    lcenter(3, time_str, 14, 0);
    lcenter(4, date_str, 7, 0);

    lputs(37, 7,  " /-----\\ ", 14, 0);
    lputs(37, 8,  "|       |", 14, 0);
    lputs(37, 9,  "| LOCKED|", 12, 0);
    lputs(37, 10, "|_______|", 14, 0);

    if (lock_mode == MODE_CLOCK) {
        lcenter(13, "Screen Locked", 11, 0);
        if (logged_in_user >= 0)
            lcenter(14, users[logged_in_user].username, 14, 0);
        lcenter(17, "ENTER: unlock   TAB: switch user", 7, 0);
        lcenter(18, "ESC: stay locked", 8, 0);
    } else if (lock_mode == MODE_USERSEL) {
        lcenter(12, "Select User:", 11, 0);
        int row = 13;
        for (int i = 0; i < MAX_USERS; i++) {
            if (users[i].active) {
                uint8_t fg = (i == sel_user) ? 0  : 15;
                uint8_t bg = (i == sel_user) ? 14 : 0;
                int x = (VGA_WIDTH - 20) / 2;
                lfill(x, row, 20, 1, ' ', fg, bg);
                lputs(x + 1, row, users[i].username, fg, bg);
                if (i == logged_in_user)
                    lputs(x + 12, row, "(current)", 10, bg);
                row++;
            }
        }
        lcenter(19, "UP/DOWN:select  ENTER:choose  ESC:back", 7, 0);
    } else if (lock_mode == MODE_PASSWORD) {
        lcenter(12, "Unlock as:", 11, 0);
        lcenter(13, users[sel_user].username, 14, 0);
        int bx = 28, by = 15;
        lfill(bx, by, 24, 3, ' ', 0, 1);
        lputs(bx + 1, by,     "Password:", 14, 1);
        lfill(bx + 1, by + 1, 20, 1, ' ', 0, 7);
        char stars_str[INPUT_LEN + 1];
        for (int i = 0; i < pass_pos; i++) stars_str[i] = '*';
        stars_str[pass_pos] = '\0';
        lputs(bx + 1, by + 1, stars_str, 0, 7);
        if (lock_failed)
            lputs(bx + 1, by + 2, "Wrong password!", 12, 1);
        else
            lputs(bx + 1, by + 2, "ENTER:unlock ESC:back", 8, 1);
    }
    lcenter(23, "Nyx OS v0.1 - Lock Screen", 8, 0);
}

// ═══════════════════════════════════════════════════════════════════
//  KEY HANDLER
// ═══════════════════════════════════════════════════════════════════
void lockscreen_handle_key(char c) {
    if (!locked) return;

    if (lock_mode == MODE_CLOCK) {
        if (c == 13) {
            sel_user = (logged_in_user >= 0) ? logged_in_user : 0;
            if (users[sel_user].password[0] == '\0') {
                locked = 0; lock_mode = MODE_CLOCK;
            } else {
                lock_mode = MODE_PASSWORD;
                clear_password();
                lockscreen_draw();
            }
            return;
        }
        if (c == '\t') {
            sel_user = (logged_in_user >= 0) ? logged_in_user : 0;
            lock_mode = MODE_USERSEL;
            lockscreen_draw();
            return;
        }
        return;
    }

    if (lock_mode == MODE_USERSEL) {
        if (c == 'A') {
            int prev = sel_user - 1;
            while (prev >= 0 && !users[prev].active) prev--;
            if (prev >= 0) sel_user = prev;
            lockscreen_draw(); return;
        }
        if (c == 'B') {
            int next = sel_user + 1;
            while (next < MAX_USERS && !users[next].active) next++;
            if (next < MAX_USERS) sel_user = next;
            lockscreen_draw(); return;
        }
        if (c == 13) {
            if (users[sel_user].password[0] == '\0') {
                logged_in_user = sel_user;
                locked = 0; lock_mode = MODE_CLOCK;
            } else {
                lock_mode = MODE_PASSWORD;
                clear_password();
                lockscreen_draw();
            }
            return;
        }
        if (c == 27) { lock_mode = MODE_CLOCK; lockscreen_draw(); return; }
        return;
    }

    if (lock_mode == MODE_PASSWORD) {
        if (c == 27) { lock_mode = MODE_CLOCK; clear_password(); lockscreen_draw(); return; }
        if (c == 8) {
            if (pass_pos > 0) password[--pass_pos] = '\0';
            lock_failed = 0; lockscreen_draw(); return;
        }
        if (c == 13) {
            if (auth_login(users[sel_user].username, password)) {
                logged_in_user = sel_user;
                locked = 0; lock_mode = MODE_CLOCK;
                lock_failed = 0; fail_count = 0;
                desktop_vfx_flash(100,255,150,5);
            } else {
                desktop_set_status("Wrong password!");
                fail_count++; lock_failed = 1;
                clear_password();
                lockscreen_draw();
            }
            return;
        }
        if (c >= 32 && c < 127 && pass_pos < INPUT_LEN) {
            password[pass_pos++] = c;
            password[pass_pos] = '\0';
            lock_failed = 0;
            lockscreen_draw();
        }
        return;
    }
}

void lockscreen_lock(void) {
    locked      = 1;
    lock_mode   = MODE_CLOCK;
    lock_failed = 0;
    fail_count  = 0;
    sel_user    = (logged_in_user >= 0) ? logged_in_user : 0;
    clear_password();
    desktop_set_status("Locked");
    desktop_vfx_flash(100,100,255,3);
    lockscreen_draw();
}

int  lockscreen_is_locked(void) { return locked; }
void lockscreen_show(void)      { lockscreen_draw(); }
