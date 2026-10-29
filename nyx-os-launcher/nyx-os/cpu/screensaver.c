// cpu/screensaver.c - Nyx OS Screensaver
// Activates after 30 seconds idle
// Features: Starfield, Matrix rain, Bouncing logo, Plasma
#include <screensaver.h>
#include <vesa.h>
#include <rtc.h>
#include <speaker.h>
#include <stdint.h>

#define VGA_WIDTH  80
#define VGA_HEIGHT 25
#define VGA_MEM    ((uint16_t*)0xB8000)

static inline uint16_t make_entry(char c, uint8_t fg, uint8_t bg) {
    return (uint16_t)c | (uint16_t)(fg | bg << 4) << 8;
}
static void sp(int x, int y, char c, uint8_t fg, uint8_t bg) {
    if(x<0||x>=VGA_WIDTH||y<0||y>=VGA_HEIGHT) return;
    VGA_MEM[y*VGA_WIDTH+x] = make_entry(c,fg,bg);
}
static void ss2(int x, int y, const char* s, uint8_t fg, uint8_t bg) {
    for(int i=0;s[i];i++) sp(x+i,y,s[i],fg,bg);
}
static void sf(int x, int y, int w, int h, char c, uint8_t fg, uint8_t bg) {
    for(int dy=0;dy<h;dy++)
        for(int dx=0;dx<w;dx++)
            sp(x+dx,y+dy,c,fg,bg);
}
static void sc(int y, const char* s, uint8_t fg, uint8_t bg) {
    int l=0; while(s[l])l++;
    ss2((VGA_WIDTH-l)/2,y,s,fg,bg);
}

// ─── State ────────────────────────────────────────────────────────
static int  ss_active    = 0;
static int  ss_mode      = 0;  // 0=starfield 1=matrix 2=bounce 3=plasma
static int  ss_ticks     = 0;
static int  idle_ticks   = 0;
#define IDLE_TIMEOUT  (18*30)  // 30 seconds at 18 ticks/sec

// Random
static int ss_rand = 31337;
static int sr() {
    ss_rand=ss_rand*1103515245+12345;
    return (ss_rand>>16)&0x7FFF;
}

// ─── MODE 0: STARFIELD ────────────────────────────────────────────
#define STAR_COUNT 60
typedef struct { int x,y,speed,bright; char c; } Star;
static Star stars[STAR_COUNT];
static int  star_init_done=0;

static void starfield_init() {
    for(int i=0;i<STAR_COUNT;i++){
        stars[i].x      = sr()%VGA_WIDTH;
        stars[i].y      = sr()%VGA_HEIGHT;
        stars[i].speed  = 1+(sr()%3);
        stars[i].bright = sr()%4;
        const char chars[]=".*+oO";
        stars[i].c = chars[sr()%5];
    }
    star_init_done=1;
}

static void starfield_tick() {
    // Clear old positions
    for(int i=0;i<STAR_COUNT;i++)
        sp(stars[i].x,stars[i].y,' ',0,0);

    // Move stars left (parallax)
    for(int i=0;i<STAR_COUNT;i++){
        stars[i].x -= stars[i].speed;
        if(stars[i].x<0){
            stars[i].x=VGA_WIDTH-1;
            stars[i].y=sr()%VGA_HEIGHT;
            stars[i].bright=sr()%4;
        }
    }

    // Draw stars
    const uint8_t blevels[]={8,7,15,14};
    for(int i=0;i<STAR_COUNT;i++)
        sp(stars[i].x,stars[i].y,stars[i].c,
           blevels[stars[i].bright],0);

    // Draw time in center
    char ts[9]; rtc_get_time_str(ts);
    int tx=(VGA_WIDTH-8)/2;
    sf(tx-2,11,12,3,' ',0,1);
    ss2(tx-1,11,"----------",8,1);
    ss2(tx,12,ts,15,1);
    ss2(tx-1,13,"----------",8,1);

    // Logo
    sc(1," N Y X  O S ",8,0);
}

// ─── MODE 1: MATRIX RAIN ──────────────────────────────────────────
#define COL_COUNT VGA_WIDTH
static int  mat_head[COL_COUNT];   // head position
static int  mat_speed[COL_COUNT];  // ticks between moves
static int  mat_timer[COL_COUNT];  // current timer
static int  mat_len[COL_COUNT];    // trail length
static int  mat_active[COL_COUNT]; // is column active
static int  mat_init_done=0;

static void matrix_init() {
    for(int x=0;x<COL_COUNT;x++){
        mat_head[x]   = -(sr()%VGA_HEIGHT);
        mat_speed[x]  = 1+(sr()%3);
        mat_timer[x]  = 0;
        mat_len[x]    = 5+(sr()%15);
        mat_active[x] = (sr()%3!=0);
    }
    mat_init_done=1;
}

static void matrix_tick() {
    for(int x=0;x<COL_COUNT;x++){
        if(!mat_active[x]) continue;
        mat_timer[x]++;
        if(mat_timer[x]<mat_speed[x]) continue;
        mat_timer[x]=0;

        // Clear tail
        int tail=mat_head[x]-mat_len[x];
        if(tail>=0&&tail<VGA_HEIGHT)
            sp(x,tail,' ',0,0);

        // Move head
        mat_head[x]++;

        // Draw trail
        for(int t=0;t<mat_len[x];t++){
            int y=mat_head[x]-t;
            if(y<0||y>=VGA_HEIGHT) continue;
            char c='0'+(sr()%10);
            if(sr()%3==0) c='A'+(sr()%26);
            uint8_t fg;
            if(t==0)      fg=15; // bright white head
            else if(t<3)  fg=10; // bright green
            else if(t<7)  fg=2;  // green
            else          fg=8;  // dark green trail
            sp(x,y,c,fg,0);
        }

        // Reset if off screen
        if(mat_head[x]-mat_len[x]>=VGA_HEIGHT){
            mat_head[x]  = -(sr()%10);
            mat_len[x]   = 5+(sr()%15);
            mat_speed[x] = 1+(sr()%3);
        }
    }

    // "Wake to continue" hint fades in/out
    if((ss_ticks/30)%2==0){
        int y=VGA_HEIGHT/2;
        sf(10,y-1,60,3,' ',0,0);
        sc(y,"  Press any key to wake  ",0,2);
    }
}

// ─── MODE 2: BOUNCING NYX LOGO ────────────────────────────────────
static int  logo_x=5, logo_y=5;
static int  logo_dx=1, logo_dy=1;
static uint8_t logo_color=10;
static int  logo_color_tick=0;

static const char* logo_lines[]={
    "  _   _            ____  _____ ",
    " | \\ | |_   ___  _/ __ \\/ ____|",
    " |  \\| | | | \\ \\/ / |  | (___  ",
    " | . ` | | | |\\  /| |  |\\___ \\ ",
    " | |\\  | |_| |/  \\| |__| ____) |",
    " |_| \\_|\\__, /_/\\_\\\\____/|_____/ ",
    "          __/ |                  ",
    "         |___/    O S            ",
};
#define LOGO_W 34
#define LOGO_H 8

static void bounce_tick() {
    // Erase old
    for(int i=0;i<LOGO_H;i++)
        sf(logo_x,logo_y+i,LOGO_W,1,' ',0,0);

    // Move
    logo_x+=logo_dx;
    logo_y+=logo_dy;

    // Bounce off walls
    if(logo_x<=0||logo_x+LOGO_W>=VGA_WIDTH){
        logo_dx=-logo_dx;
        logo_color=(logo_color%14)+1;
        if(logo_color==0) logo_color=1;
        
    }
    if(logo_y<=0||logo_y+LOGO_H>=VGA_HEIGHT){
        logo_dy=-logo_dy;
        logo_color=(logo_color%14)+1;
        if(logo_color==0) logo_color=1;
        
    }

    // Clamp
    if(logo_x<0) logo_x=0;
    if(logo_y<0) logo_y=0;
    if(logo_x+LOGO_W>=VGA_WIDTH)  logo_x=VGA_WIDTH-LOGO_W-1;
    if(logo_y+LOGO_H>=VGA_HEIGHT) logo_y=VGA_HEIGHT-LOGO_H-1;

    // Draw logo
    for(int i=0;i<LOGO_H;i++)
        ss2(logo_x,logo_y+i,logo_lines[i],logo_color,0);

    // Color cycle
    logo_color_tick++;
    if(logo_color_tick>60){
        logo_color_tick=0;
        logo_color=(logo_color%14)+2;
        if(logo_color>15)logo_color=2;
    }

    // Stars background
    for(int i=0;i<5;i++){
        int bx=sr()%VGA_WIDTH;
        int by=sr()%VGA_HEIGHT;
        // Don't overwrite logo
        if(bx>=logo_x&&bx<logo_x+LOGO_W&&
           by>=logo_y&&by<logo_y+LOGO_H) continue;
        sp(bx,by,'.',8,0);
    }
}

// ─── MODE 3: PLASMA / COLORWAVE ───────────────────────────────────
static int plasma_t=0;

// Simple fixed-point sine approx
static int fsin(int angle){
    // angle 0-255 = 0-2pi
    // returns -128 to 127
    angle&=255;
    if(angle<64)       return angle*2-64+64;  // crude approx
    else if(angle<128) return (127-angle)*2+64-64;
    else if(angle<192) return -(angle-128)*2+64-128;
    else               return -(255-angle)*2-64+128;
}

static void plasma_tick() {
    plasma_t=(plasma_t+3)&255;

    for(int y=0;y<VGA_HEIGHT;y++){
        for(int x=0;x<VGA_WIDTH;x++){
            // Combine sine waves
            int v = fsin((x*4+plasma_t)&255)
                  + fsin((y*8+plasma_t*2)&255)
                  + fsin(((x+y)*3+plasma_t*3)&255)
                  + fsin((x*2-y*3+plasma_t*4)&255);

            // Map to color 0-15
            v = (v+512)&255;
            uint8_t col = (v>>4)&0xF;
            if(col==0) col=1; // avoid black on black

            // Char based on value
            const char chars[]=" .,:;!|*#@$%&";
            char c=chars[(v>>3)%13];
            sp(x,y,c,col,0);
        }
    }

    // Time overlay
    char ts[9]; rtc_get_time_str(ts);
    sf(34,11,12,3,' ',0,1);
    ss2(35,12,ts,15,1);

    sc(1,"~ N Y X  O S ~",14,0);
}

// ─── Screensaver control ──────────────────────────────────────────
void screensaver_reset_idle() {
    idle_ticks=0;
    if(ss_active){
        // Wake up!
        ss_active=0;
          }
}

int screensaver_is_active() {
    return ss_active;
}

void screensaver_start() {
    ss_active=1;
    ss_ticks=0;
    // In VESA (framebuffer) mode the text-mode animations below write
    // to VGA memory that isn't the active display — and the plasma
    // loop can spin forever inside the IRQ handler, freezing the OS
    // after ~30 s idle.  Show a clean blank screen instead and wake
    // on any key.  The starfield/matrix/bounce/plasma animations are
    // VGA-text-only, which is the mode they were designed for.
    if (vesa.active) {
        vesa_clear(0x000000);
        return;
    }
    ss_mode=sr()%4;
    // Init selected mode
    sf(0,0,VGA_WIDTH,VGA_HEIGHT,' ',0,0);
    star_init_done=0;
    mat_init_done=0;
    logo_x=5; logo_y=5;
    logo_dx=1; logo_dy=1;
    logo_color=10;
    plasma_t=0;
    if(ss_mode==0) starfield_init();
    if(ss_mode==1) matrix_init();
}

void screensaver_tick() {
    // Count idle time
    idle_ticks++;
    if(!ss_active&&idle_ticks>=IDLE_TIMEOUT){
        screensaver_start();
        return;
    }
    if(!ss_active) return;
    if (vesa.active) return;   // VESA: blank screen only, wake on key

    ss_ticks++;

    // Switch modes every ~15 seconds
    if(ss_ticks%(18*15)==0){
        ss_mode=(ss_mode+1)%4;
        sf(0,0,VGA_WIDTH,VGA_HEIGHT,' ',0,0);
        if(ss_mode==0&&!star_init_done) starfield_init();
        if(ss_mode==1&&!mat_init_done)  matrix_init();
        logo_x=5;logo_y=5;logo_dx=1;logo_dy=1;
        plasma_t=0;
    }

    // Only animate every 2 ticks (not too fast)
    if(ss_ticks%2!=0) return;

    switch(ss_mode){
        case 0: starfield_tick(); break;
        case 1: matrix_tick();    break;
        case 2: bounce_tick();    break;
        case 3: plasma_tick();    break;
    }
}

void screensaver_init() {
    ss_active=0;
    ss_ticks=0;
    idle_ticks=0;
    ss_mode=0;
    star_init_done=0;
    mat_init_done=0;
}
