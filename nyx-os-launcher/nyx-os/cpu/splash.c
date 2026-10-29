// cpu/splash.c — Nyx OS Boot Splash
// VESA pixel mode (matching the desktop's purple cosmic aesthetic),
// with a VGA text fallback for non-VESA boots.
#include <splash.h>
#include <vesa.h>
#include <stdint.h>

// ═══════════════════════════════════════════════════════════════════
//  VGA TEXT FALLBACK (unchanged behavior when VESA is unavailable)
// ═══════════════════════════════════════════════════════════════════
#define VGA_WIDTH  80
#define VGA_HEIGHT 25
#define VGA_MEM    ((uint16_t*)0xB8000)

static inline uint16_t make_entry(char c, uint8_t fg, uint8_t bg) {
    return (uint16_t)c | (uint16_t)(fg | bg << 4) << 8;
}
static void sput(int x, int y, char c, uint8_t fg, uint8_t bg) {
    if (x < 0 || x >= VGA_WIDTH || y < 0 || y >= VGA_HEIGHT) return;
    VGA_MEM[y * VGA_WIDTH + x] = make_entry(c, fg, bg);
}
static void sputs(int x, int y, const char* s, uint8_t fg, uint8_t bg) {
    for (int i = 0; s[i]; i++) sput(x + i, y, s[i], fg, bg);
}
static void sfill(int x, int y, int w, int h, char c, uint8_t fg, uint8_t bg) {
    for (int dy = 0; dy < h; dy++)
        for (int dx = 0; dx < w; dx++)
            sput(x + dx, y + dy, c, fg, bg);
}
static void sfill_row(int y, char c, uint8_t fg, uint8_t bg) {
    for (int x = 0; x < VGA_WIDTH; x++) sput(x, y, c, fg, bg);
}
static void splash_clear(uint8_t bg) {
    sfill(0, 0, VGA_WIDTH, VGA_HEIGHT, ' ', 0, bg);
}
static void splash_center(int y, const char* s, uint8_t fg, uint8_t bg) {
    int len = 0; while (s[len]) len++;
    sputs((VGA_WIDTH - len) / 2, y, s, fg, bg);
}
static void draw_loading_bar(int percent) {
    int bar_w = 40, bar_x = (VGA_WIDTH - bar_w) / 2, bar_y = 18;
    int filled = bar_w * percent / 100;
    sput(bar_x - 1, bar_y, '[', 7, 0);
    sput(bar_x + bar_w, bar_y, ']', 7, 0);
    for (int i = 0; i < bar_w; i++)
        sput(bar_x + i, bar_y, i < filled ? '|' : '-', i < filled ? 10 : 8, 0);
    char pct[5];
    pct[0]='0'+(percent/100); pct[1]='0'+(percent/10%10);
    pct[2]='0'+(percent%10); pct[3]='%'; pct[4]='\0';
    sputs(bar_x + bar_w + 2, bar_y, pct, 14, 0);
}
static void draw_logo_vga(uint8_t color) {
    int x = 16;
    sputs(x, 4,  "  _   _            ____  _____  ", color, 0);
    sputs(x, 5,  " | \\ | |_   ___  _/ __ \\/ ____| ", color, 0);
    sputs(x, 6,  " |  \\| | | | \\ \\/ / |  | (___   ", color, 0);
    sputs(x, 7,  " | . ` | | | |\\  /| |  |\\___ \\  ", color, 0);
    sputs(x, 8,  " | |\\  | |_| |/  \\| |__| ____) | ", color, 0);
    sputs(x, 9,  " |_| \\_|\\__, /_/\\_\\\\____/|_____/  ", color, 0);
    sputs(x, 10, "          __/ |                    ", color, 0);
    sputs(x, 11, "         |___/                     ", color, 0);
}
static void draw_stars_vga(void) {
    int stars[][2] = {
        {5,2},{15,3},{25,1},{35,2},{45,3},{55,1},{65,2},{75,3},
        {3,4},{70,5},{8,6},{72,7},{4,8},{76,9},{10,10},{68,11},
        {2,12},{78,13},{6,14},{74,15},{12,16},{66,17},{1,20},{79,21},
    };
    for (int i = 0; i < 24; i++) sput(stars[i][0], stars[i][1], '.', 8, 0);
}

// ═══════════════════════════════════════════════════════════════════
//  SHARED STATE
// ═══════════════════════════════════════════════════════════════════
static void splash_delay(uint32_t ms) {
    for (uint32_t i = 0; i < ms * 100000; i++) asm volatile("nop");
}

static const char* loading_msgs[] = {
    "Initializing kernel...",
    "Loading GDT & IDT...",
    "Setting up interrupts...",
    "Loading keyboard driver...",
    "Initializing file system...",
    "Loading desktop environment...",
    "Starting music player...",
    "Applying settings...",
    "Welcome to Nyx OS!",
};
#define MSG_COUNT 9

// ═══════════════════════════════════════════════════════════════════
//  VESA PIXEL SPLASH
// ═══════════════════════════════════════════════════════════════════
#define S_BG     0x0C0820
#define S_ACC    0xB088FF
#define S_ACC2   0x6A4FBF
#define S_ADIM   0x3D2A66
#define S_AFAINT 0x1A1230
#define S_WHT    0xF0E8FF
#define S_GRY    0x554477
#define S_DGRY   0x2A1F40

static int s_sl(const char* s) { int l=0; while(s[l]) l++; return l; }

static void s_vgrad(int x,int y,int w,int h,uint32_t c1,uint32_t c2){
    for(int dy=0;dy<h;dy++){
        uint32_t r=((c1>>16)&0xFF)+(int)(((c2>>16)&0xFF)-((c1>>16)&0xFF))*dy/h;
        uint32_t g=((c1>>8)&0xFF)+(int)(((c2>>8)&0xFF)-((c1>>8)&0xFF))*dy/h;
        uint32_t b=(c1&0xFF)+(int)((c2&0xFF)-(c1&0xFF))*dy/h;
        vesa_hline(x,y+dy,w,(r<<16)|(g<<8)|b);
    }
}
static void s_hgrad(int x,int y,int w,int h,uint32_t c1,uint32_t c2){
    for(int dx=0;dx<w;dx++){
        uint32_t r=((c1>>16)&0xFF)+(int)(((c2>>16)&0xFF)-((c1>>16)&0xFF))*dx/w;
        uint32_t g=((c1>>8)&0xFF)+(int)(((c2>>8)&0xFF)-((c1>>8)&0xFF))*dx/w;
        uint32_t b=(c1&0xFF)+(int)((c2&0xFF)-(c1&0xFF))*dx/w;
        vesa_vline(x+dx,y,h,(r<<16)|(g<<8)|b);
    }
}
static void s_glow(int x,int y,const char*s,uint32_t fg,uint32_t gc,int spacing){
    uint32_t d1=(gc>>3)&0x1F1F1F, d2=(gc>>1)&0x7F7F7F;
    int cx=x;
    for(int i=0;s[i];i++){
        vesa_draw_char(cx-2,y,s[i],d1,S_BG); vesa_draw_char(cx+2,y,s[i],d1,S_BG);
        vesa_draw_char(cx,y-1,s[i],d2,S_BG); vesa_draw_char(cx,y+1,s[i],d2,S_BG);
        vesa_draw_char(cx-1,y,s[i],d2,S_BG); vesa_draw_char(cx+1,y,s[i],d2,S_BG);
        vesa_draw_char(cx,y,s[i],fg,S_BG);
        cx += spacing;
    }
}
static void s_ctr(int y,const char*s,uint32_t fg){
    int x = ((int)vesa.width - s_sl(s)*8)/2;
    vesa_draw_string(x, y, s, fg, 0);
}

static void s_stars(void){
    static const uint16_t sx[]={
        38,92,148,204,268,322,376,432,488,542,596,650,
        14,68,124,180,236,290,350,408,464,518,572,628,
        55,110,166,222,278,338,394,450,506,560,614,664
    };
    static const uint16_t sy[]={
        22,48,16,64,38,54,26,44,18,58,34,50,
        90,75,105,82,96,72,88,100,78,94,70,108,
        140,160,135,155,145,150,138,158,142,148,128,118
    };
    int W=(int)vesa.width, H=(int)vesa.height;
    for(int i=0;i<36;i++){
        int x=sx[i], y=sy[i];
        if(x>=W||y>=H)continue;
        uint32_t c=(i%6==0)?0xE0D0FF:(i%3==0)?0x9988CC:0x4A3A66;
        vesa_put_pixel(x,y,c);
        if(i%8==0){
            vesa_put_pixel(x-1,y,c/3); vesa_put_pixel(x+1,y,c/3);
            vesa_put_pixel(x,y-1,c/3); vesa_put_pixel(x,y+1,c/3);
        }
    }
}

static void s_corner_brackets(void){
    int W=(int)vesa.width, H=(int)vesa.height, bl=18;
    vesa_hline(14,14,bl,S_ADIM);    vesa_vline(14,14,bl,S_ADIM);
    vesa_hline(W-15-bl,14,bl,S_ADIM); vesa_vline(W-15,14,bl,S_ADIM);
    vesa_hline(14,H-15,bl,S_ADIM);  vesa_vline(14,H-15-bl,bl,S_ADIM);
    vesa_hline(W-15-bl,H-15,bl,S_ADIM); vesa_vline(W-15,H-15-bl,bl,S_ADIM);
}

// Draw one frame of the splash at a given boot percentage + message.
// logo_alpha: 0=dim, 1=mid, 2=bright (controls fade-in/out stages)
static void circ(int cx,int cy,int r,uint32_t c){
    for(int dy=-r;dy<=r;dy++)
        for(int dx=-r;dx<=r;dx++)
            if(dx*dx+dy*dy<=r*r)
                vesa_put_pixel(cx+dx,cy+dy,c);
}
static void ring(int cx,int cy,int r,int t,uint32_t c){
    int ri=r-t;
    for(int dy=-r;dy<=r;dy++)
        for(int dx=-r;dx<=r;dx++){
            int d=dx*dx+dy*dy;
            if(d<=r*r&&d>=ri*ri)
                vesa_put_pixel(cx+dx,cy+dy,c);
        }
}

// ─── Lunar eclipse logo ──────────────────────────────────────────
// A dark circle (the eclipsed moon) with a thin glowing crescent
// arc wrapping around its right side, plus a soft halo behind it.
// All integer math: the crescent is a ring (radius band) masked to
// an angular slice using only dx/dy comparisons, no trig/floats.
//
// alpha: 0=dim, 1=mid, 2=bright (matches the fade stages elsewhere)
static void s_draw_logo_symbol(int cx, int cy, int R, int alpha, uint32_t acc){
    uint32_t arc_bright = (alpha==0)?0x2A1C4A:(alpha==1)?0x7858B8:acc;
    uint32_t arc_glow    = (alpha==0)?0x150E26:(alpha==1)?0x3A2A66:0x6A4FBF;
    uint32_t arc_glow2   = (alpha==0)?0x0A0818:(alpha==1)?0x251A40:0x4A3680;
    uint32_t halo_peak   = (alpha==0)?0x0E0826:(alpha==1)?0x1E1240:0x2A1850;

    // ── Soft outer halo (concentric rings fading outward) ───────
    for(int r=R+34;r>R;r--){
        int t = R+34-r;                  // 0 at outer edge .. 34 at moon edge
        uint32_t rC=(halo_peak>>16)&0xFF, gC=(halo_peak>>8)&0xFF, bC=halo_peak&0xFF;
        rC = rC*t/34; gC = gC*t/34; bC = bC*t/34;
        ring((cx-R/14),cy,r,1,(rC<<16)|(gC<<8)|bC);
    }
    (void)acc;

    // ── Dark moon body (near-black, fully eclipsed) ─────────────
    circ(cx-R/14, cy, R, 0x020108);

    // ── Crescent arc on the right side ───────────────────────────
    // Ring band from R-3..R (3px thick), only lit where the pixel's
    // angle falls within the right-side arc (roughly -100..+100
    // degrees from due-east, i.e. NOT the leftmost ~160 degrees).
    // We test this with dx (horizontal offset from center) being
    // positive enough relative to |dy|, which traces a lens-shaped
    // arc on the right without needing trig.
    int mcx = cx - R/14;  // moon center (slightly left of halo center)
    int band_out = R;
    int band_in  = R-3;
    for(int dy=-band_out;dy<=band_out;dy++){
        for(int dx=-band_out;dx<=band_out;dx++){
            int d2 = dx*dx+dy*dy;
            if (d2 > band_out*band_out || d2 < band_in*band_in) continue;

            // Angular gate: light up the right-hand arc. Using dx
            // vs |dy| asymmetrically creates the "crescent" look
            // (thicker glow near 3 o'clock, tapering top/bottom).
            int ady = (dy<0)?-dy:dy;
            if (dx + R*3/10 < ady) continue;     // exclude far-left band
            if (dx < -R/6) continue;             // hard exclude left half

            // Brightness falls off toward the tips of the crescent
            int taper = dx - ady;                // peaks near 3 o'clock
            uint32_t col;
            if (taper > R/3)       col = arc_bright;
            else if (taper > 0)    col = arc_glow;
            else                   col = arc_glow2;

            vesa_put_pixel(mcx+dx, cy+dy, col);
        }
    }

    // ── Tiny bright highlight near the 2-3 o'clock point ────────
    if (alpha == 2) {
        int hx = mcx + R - R/10, hy = cy - R/4;
        vesa_put_pixel(hx, hy, 0xFFFFFF);
        vesa_put_pixel(hx-1, hy, 0xE8DCFF);
        vesa_put_pixel(hx, hy-1, 0xE8DCFF);
    }
}

static void s_draw_frame(int percent, const char* msg, int logo_alpha){
    int W=(int)vesa.width, H=(int)vesa.height, CX=W/2, CY=H/2;

    s_vgrad(0,0,W,H,0x06031A,0x180C30);
    for(int y=0;y<H;y+=32) vesa_hline(0,y,W,0x130A28);
    for(int x=0;x<W;x+=32) vesa_vline(x,0,H,0x130A28);

    // Soft center glow behind the logo
    for(int r=180;r>0;r-=6){
        uint32_t a = 0x0E0826 + (uint32_t)(180-r)/6*0x020108;
        if (a > 0x251440) a = 0x251440;
        int ww = W*r/360;
        vesa_hline(CX-ww/2, CY-60, ww, a);
    }

    s_stars();
    s_corner_brackets();

    // Logo: lunar eclipse symbol, centered above the tagline
    int logo_r = 64;
    int logo_cy = CY - 110;
    s_draw_logo_symbol(CX, logo_cy, logo_r, logo_alpha, S_ACC);

    // Wordmark beneath the symbol
    uint32_t word_fg = (logo_alpha==0)?0x483A66:(logo_alpha==1)?0x9580C0:S_WHT;
    const char* word = "NYX OS";
    int spacing = 18;
    int total_w = s_sl(word)*spacing;
    int lx = CX - total_w/2;
    int ly = logo_cy + logo_r + 26;
    for(int i=0; word[i]; i++){
        vesa_draw_char(lx + i*spacing, ly, word[i], word_fg, 0);
    }

    // Tagline
    s_ctr(CY-50, "B O O T   S E Q U E N C E", S_GRY);

    // Progress bar
    int bw=320, bh=6;
    int bx=CX-bw/2, by=CY+6;
    vesa_fill_rect(bx,by,bw,bh,0x0A1828);
    vesa_draw_rect(bx-1,by-1,bw+2,bh+2,S_ADIM);
    int fill_w = bw*percent/100;
    if (fill_w > 0) {
        s_hgrad(bx,by,fill_w,bh,S_ACC2,S_ACC);
        vesa_hline(bx,by,fill_w,0xD8C0FF);
    }

    // LOADING label + percentage
    vesa_draw_string(bx, by+14, "LOADING", S_GRY, 0);
    char pct[5];
    pct[0]='0'+(percent/100); pct[1]='0'+(percent/10%10);
    pct[2]='0'+(percent%10); pct[3]='%'; pct[4]='\0';
    int pw = s_sl(pct)*8;
    vesa_draw_string(bx+bw-pw, by+14, pct, S_ACC, 0);

    // Status message (centered below bar)
    if (msg) s_ctr(by+34, msg, 0x9580B8);

    // Version footer
    s_ctr(H-40, "v3.0  -  2026", S_DGRY);

    vesa_present();
}

// ═══════════════════════════════════════════════════════════════════
//  MAIN SPLASH ANIMATION
// ═══════════════════════════════════════════════════════════════════
void splash_show(void) {

    // ── VGA TEXT FALLBACK ────────────────────────────────────────
    if (!vesa.active) {
        splash_clear(0);
        draw_stars_vga();

        uint8_t colors[] = {8, 8, 7, 7, 3, 3, 11, 11, 15};
        for (int i = 0; i < 9; i++) { draw_logo_vga(colors[i]); splash_delay(20); }

        splash_center(13, "Version 0.1 - Built from scratch", 8, 0);
        splash_delay(50);
        splash_center(16, "Loading...", 7, 0);

        for (int i = 0; i < MSG_COUNT; i++) {
            int percent = (i + 1) * 100 / MSG_COUNT;
            sfill_row(20, ' ', 0, 0);
            splash_center(20, loading_msgs[i], 7, 0);
            int prev = i * 100 / MSG_COUNT;
            for (int p = prev; p <= percent; p += 2) { draw_loading_bar(p); splash_delay(5); }
            splash_delay(25);
        }

        sfill_row(20, ' ', 0, 0);
        draw_loading_bar(100);
        splash_center(20, "Ready!", 10, 0);

        splash_delay(80);

        for (int i = 0; i < 3; i++) {
            splash_clear(0); splash_delay(25);
            draw_logo_vga(8); draw_stars_vga(); splash_delay(25);
        }
        splash_clear(0); splash_delay(50);
        return;
    }

    // ── VESA PIXEL SPLASH ────────────────────────────────────────

    // Phase 1: logo fade in (3 stages) — ~75 ms total.  The delays are
    // cosmetic only; the real work (fs_load_embedded, icon warmup) is
    // fast, so a sub-half-second splash reads as a brief flash, just
    // like a normal desktop's boot screen.
    for (int stage = 0; stage <= 2; stage++) {
        s_draw_frame(0, "Initializing kernel...", stage);
        splash_delay(25);
    }

    // Phase 2: loading bar with rotating messages — ~190 ms total
    for (int i = 0; i < MSG_COUNT; i++) {
        int target = (i + 1) * 100 / MSG_COUNT;
        int prev   = i * 100 / MSG_COUNT;
        for (int p = prev; p <= target; p += 4) {
            s_draw_frame(p, loading_msgs[i], 2);
            splash_delay(2);
        }
        splash_delay(15);
    }

    // Phase 3: complete
    s_draw_frame(100, "Ready!", 2);
    splash_delay(80);

    // Phase 4: fade out (dim logo) — ~90 ms total
    for (int stage = 2; stage >= 0; stage--) {
        s_draw_frame(100, "Ready!", stage);
        splash_delay(30);
    }
}
