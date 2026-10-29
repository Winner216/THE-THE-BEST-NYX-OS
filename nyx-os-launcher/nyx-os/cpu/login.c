// cpu/login.c — Nyx OS Login Screen v4
// Side-by-side layout: avatar left, card right
// VESA pixel mode with VGA text fallback
#include <login.h>
#include <io.h>
#include <auth.h>
#include <speaker.h>
#include <rtc.h>
#include <vesa.h>
#include <stdint.h>
#include <desktop.h>
#include <isr.h>
#include <terminal.h>

// ─── Boot-time benchmark ─────────────────────────────────────────
// Reports the real boot time once per boot: kernel start (t0, captured
// in kernel_main) → splash done → first successful login.  Ticks are
// ~50 Hz (20 ms each).  Shown as a desktop status line (visible in
// VESA) and a terminal line (visible in VGA / future serial capture)
// so regressions in boot speed are caught on every boot.
static void bmk_cat(char* buf, int* pos, const char* s) {
    while (*s) buf[(*pos)++] = *s++;
}
static int bmk_append_u32(char* buf, int pos, uint32_t v) {
    char tmp[12]; int n = 0;
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n > 0) buf[pos++] = tmp[--n];
    return pos;
}
// Appends "S.Ts" (milliseconds → seconds with one decimal).
static int bmk_append_secs(char* buf, int pos, uint32_t ms) {
    uint32_t s = ms / 1000, frac = (ms % 1000) / 100;
    char tmp[12]; int n = 0;
    do { tmp[n++] = (char)('0' + s % 10); s /= 10; } while (s);
    while (n > 0) buf[pos++] = tmp[--n];
    buf[pos++] = '.'; buf[pos++] = (char)('0' + frac); buf[pos++] = 's';
    return pos;
}
static void bmk_report(void) {
    char msg[64]; int p = 0;
    uint32_t tsplash = boot_tick_splash - boot_tick_start;
    uint32_t tlogin  = boot_tick_login  - boot_tick_start;
    // NOTE: the "login" delta includes the time the user spent reading
    // the login screen and typing their password — the splash delta is
    // the true boot-time figure; the login delta is boot + human time.
    // tlogin*20 ms would overflow uint32 after ~50 days idle at the
    // login screen; clamp to keep the displayed number sane.
    uint32_t ms = (tlogin > 214748364u) ? 0xFFFFFFFFu : tlogin * 20u;
    bmk_cat(msg, &p, "Boot: splash ");
    p = bmk_append_u32(msg, p, tsplash);
    bmk_cat(msg, &p, "t, login ");
    p = bmk_append_u32(msg, p, tlogin);
    bmk_cat(msg, &p, "t (");
    p = bmk_append_secs(msg, p, ms);
    bmk_cat(msg, &p, ")");
    msg[p] = '\0';
    desktop_set_status(msg);
    terminal_print("  [boot] ");
    terminal_print(msg);
    terminal_print("\n");
}

// ═══════════════════════════════════════════════════════════════════
//  VGA FALLBACK
// ═══════════════════════════════════════════════════════════════════
#define VW 80
#define VH 25
#define VM ((uint16_t*)0xB8000)
static void vp(int x,int y,char c,uint8_t f,uint8_t b){if(x<0||x>=VW||y<0||y>=VH)return;VM[y*VW+x]=(uint16_t)(unsigned char)c|(uint16_t)(f|(uint8_t)(b<<4))<<8;}
static void vs(int x,int y,const char*s,uint8_t f,uint8_t b){for(int i=0;s[i];i++)vp(x+i,y,s[i],f,b);}
static void vf(int x,int y,int w,int h,uint8_t f,uint8_t b){for(int dy=0;dy<h;dy++)for(int dx=0;dx<w;dx++)vp(x+dx,y+dy,' ',f,b);}
static void vc(int y,const char*s,uint8_t f,uint8_t b){int l=0;while(s[l])l++;vs((VW-l)/2,y,s,f,b);}

// ═══════════════════════════════════════════════════════════════════
//  STATE
// ═══════════════════════════════════════════════════════════════════
#define PL 16
static char pw[PL+1];
static int pp=0, done=0, fail=0, su=0;
static char last_key_debug = 0;
static int sl(const char*s){int l=0;while(s[l])l++;return l;}
static void ld(uint32_t n){for(uint32_t i=0;i<n*90000;i++)asm volatile("nop");}

// ═══════════════════════════════════════════════════════════════════
//  COLORS
// ═══════════════════════════════════════════════════════════════════
#define BG      0x0C0820
#define BG2     0x150E30
#define BG3     0x1A1140
#define CARD    0x140C30
#define FLD     0x0E0824
#define ACC     0xB088FF
#define ACC2    0x6A4FBF
#define ADIM    0x3D2A66
#define AFAINT  0x1A1230
#define WHT     0xF0E8FF
#define GRY     0x554477
#define DGRY    0x2A1F40
#define YEL     0xFFCC00
#define RED     0xFF3344
#define GRN     0x33EE77
#define NAVY    0x0C0820
#define MGNT    0x9966FF

// ═══════════════════════════════════════════════════════════════════
//  PRIMITIVES
// ═══════════════════════════════════════════════════════════════════
static void vg(int x,int y,int w,int h,uint32_t c1,uint32_t c2){
    for(int dy=0;dy<h;dy++){
        uint32_t r=((c1>>16)&0xFF)+(int)(((c2>>16)&0xFF)-((c1>>16)&0xFF))*dy/h;
        uint32_t g=((c1>>8)&0xFF)+(int)(((c2>>8)&0xFF)-((c1>>8)&0xFF))*dy/h;
        uint32_t b=(c1&0xFF)+(int)((c2&0xFF)-(c1&0xFF))*dy/h;
        vesa_hline(x,y+dy,w,(r<<16)|(g<<8)|b);
    }
}
static void hg(int x,int y,int w,int h,uint32_t c1,uint32_t c2){
    for(int dx=0;dx<w;dx++){
        uint32_t r=((c1>>16)&0xFF)+(int)(((c2>>16)&0xFF)-((c1>>16)&0xFF))*dx/w;
        uint32_t g=((c1>>8)&0xFF)+(int)(((c2>>8)&0xFF)-((c1>>8)&0xFF))*dx/w;
        uint32_t b=(c1&0xFF)+(int)((c2&0xFF)-(c1&0xFF))*dx/w;
        vesa_vline(x+dx,y,h,(r<<16)|(g<<8)|b);
    }
}
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
static void rr(int x,int y,int w,int h,uint32_t c){
    // rounded rect fill
    vesa_fill_rect(x+2,y,w-4,h,c);
    vesa_fill_rect(x,y+2,w,h-4,c);
    vesa_fill_rect(x+1,y+1,w-2,h-2,c);
}
static void rrb(int x,int y,int w,int h,uint32_t c){
    // rounded rect border
    vesa_hline(x+2,y,w-4,c); vesa_hline(x+2,y+h-1,w-4,c);
    vesa_vline(x,y+2,h-4,c); vesa_vline(x+w-1,y+2,h-4,c);
    vesa_put_pixel(x+1,y+1,c);   vesa_put_pixel(x+w-2,y+1,c);
    vesa_put_pixel(x+1,y+h-2,c); vesa_put_pixel(x+w-2,y+h-2,c);
}
static void glw(int x,int y,const char*s,uint32_t fg,uint32_t gc){
    // glow text: dim halos then bright center
    uint32_t d1=(gc>>3)&0x1F1F1F;
    uint32_t d2=(gc>>1)&0x7F7F7F;
    vesa_draw_string(x-2,y,s,d1,BG);
    vesa_draw_string(x+2,y,s,d1,BG);
    vesa_draw_string(x,y-1,s,d2,BG);
    vesa_draw_string(x,y+1,s,d2,BG);
    vesa_draw_string(x-1,y,s,d2,BG);
    vesa_draw_string(x+1,y,s,d2,BG);
    vesa_draw_string(x,y,s,fg,BG);
}
static void ctr(int y,const char*s,uint32_t fg,uint32_t bg){
    int x=((int)vesa.width-sl(s)*8)/2;
    if(x<0)x=0;
    vesa_draw_string(x,y,s,fg,bg);
}

// ═══════════════════════════════════════════════════════════════════
//  STARS
// ═══════════════════════════════════════════════════════════════════
static void stars(void){
    static const uint16_t sx[]={
        38,92,148,204,268,322,376,432,488,542,596,650,
        14,68,124,180,236,290,350,408,464,518,572,628,
        55,110,166,222,278,338,394,450,506,560,614,664,
        26,80,136,192,248,308,364,420,476,530,584,638
    };
    static const uint16_t sy[]={
        22,48,16,64,38,54,26,44,18,58,34,50,
        90,75,105,82,96,72,88,100,78,94,70,108,
        530,515,545,522,538,508,524,540,512,528,500,518,
        580,565,595,572,588,558,574,590,562,578,548,568
    };
    int W=(int)vesa.width,H=(int)vesa.height;
    for(int i=0;i<48;i++){
        int x=sx[i],y=sy[i];
        if(x>=W||y>=H)continue;
        uint32_t c=(i%7==0)?0xE0D0FF:(i%4==0)?0x9988CC:(i%2==0)?0x4A3A66:0x281A40;
        vesa_put_pixel(x,y,c);
        if(i%8==0){  // large sparkle
            vesa_put_pixel(x-2,y,c/4);
            vesa_put_pixel(x+2,y,c/4);
            vesa_put_pixel(x,y-2,c/4);
            vesa_put_pixel(x,y+2,c/4);
            vesa_put_pixel(x-1,y,c/2);
            vesa_put_pixel(x+1,y,c/2);
            vesa_put_pixel(x,y-1,c/2);
            vesa_put_pixel(x,y+1,c/2);
        } else if(i%4==0){ // small sparkle
            vesa_put_pixel(x-1,y,c/3);
            vesa_put_pixel(x+1,y,c/3);
            vesa_put_pixel(x,y-1,c/3);
            vesa_put_pixel(x,y+1,c/3);
        }
    }
}

// ═══════════════════════════════════════════════════════════════════
//  AVATAR — glowing circle with pixel face
// ═══════════════════════════════════════════════════════════════════
static void avatar(int cx,int cy,int R){
    // Outer glow rings (fade out)
    ring(cx,cy,R+10,2,0x1A1230);
    ring(cx,cy,R+7, 2,0x241840);
    ring(cx,cy,R+4, 2,0x2E2050);
    ring(cx,cy,R+2, 1,ADIM);

    // Fill with radial gradient (dark center tinted blue)
    for(int dy=-R;dy<=R;dy++){
        for(int dx=-R;dx<=R;dx++){
            int d2=dx*dx+dy*dy;
            if(d2>R*R)continue;
            int pct=d2*100/(R*R);
            // Gradient: bright navy at center → deep space at edge
            uint32_t ic=0x281A50, oc=0x0C0820;
            uint32_t r=((ic>>16)&0xFF)+(int)(((oc>>16)&0xFF)-((ic>>16)&0xFF))*pct/100;
            uint32_t g=((ic>>8)&0xFF)+(int)(((oc>>8)&0xFF)-((ic>>8)&0xFF))*pct/100;
            uint32_t b=(ic&0xFF)+(int)((oc&0xFF)-(ic&0xFF))*pct/100;
            vesa_put_pixel(cx+dx,cy+dy,(r<<16)|(g<<8)|b);
        }
    }

    // Cyan border ring
    ring(cx,cy,R,2,ACC);
    ring(cx,cy,R-1,1,0x4A3680);

    // Top shine arc
    for(int dx=-(R-3);dx<=R-3;dx++){
        int d2=(R-2)*(R-2)-dx*dx; int dy=0;
        while((dy+1)*(dy+1)<=d2)dy++;
        if(dy>4){
            vesa_put_pixel(cx+dx,cy-dy,0xC8A8FF);
            if(dx%3==0) vesa_put_pixel(cx+dx,cy-dy-1,0x8A6AC0);
        }
    }

    // ── ANONYMOUS MASK FACE ──────────────────────────────────
    // Angular glowing eye-slits + stoic mouth-line + center seam.
    // No pupils, no blush — a sleek tech/hacker mask aesthetic.

    int ey  = cy - R/6;        // eye-slit row
    int ex1 = cx - R/3;        // left slit center x
    int ex2 = cx + R/3;        // right slit center x
    int slit_w = R/2;          // slit length
    int slit_h = R/10;         // slit thickness
    if (slit_h < 2) slit_h = 2;

    // Left eye-slit: angled parallelogram, outer-down to inner-up
    for (int i = 0; i < slit_w; i++) {
        int taper = (i * slit_h) / slit_w;       // creates the angle
        int lx = ex1 - slit_w/2 + i;
        int ly = ey + taper;
        for (int t = 0; t < slit_h; t++) {
            uint32_t c = (t==0 || t==slit_h-1) ? ACC : 0x55EEFF;
            vesa_put_pixel(lx, ly+t, c);
        }
    }
    // Right eye-slit: mirrored angle (outer-down on the right side)
    for (int i = 0; i < slit_w; i++) {
        int taper = ((slit_w - 1 - i) * slit_h) / slit_w;
        int rx = ex2 - slit_w/2 + i;
        int ry = ey + taper;
        for (int t = 0; t < slit_h; t++) {
            uint32_t c = (t==0 || t==slit_h-1) ? ACC : 0x55EEFF;
            vesa_put_pixel(rx, ry+t, c);
        }
    }
    // Eye-slit glow halo (soft dim outline above/below each slit)
    for (int i = -2; i < slit_w+2; i++) {
        int lx = ex1 - slit_w/2 + i;
        int rx = ex2 - slit_w/2 + i;
        vesa_put_pixel(lx, ey-2, 0x113344);
        vesa_put_pixel(rx, ey-2, 0x113344);
    }

    // Center seam line: subtle vertical division down the mask
    int seam_top = ey - R/2;
    int seam_bot = cy + R*2/3;
    for (int y = seam_top; y < seam_bot; y++) {
        if (y % 3 != 0) // dashed effect
            vesa_put_pixel(cx, y, 0x224455);
    }

    // Side tech-panel marks (small horizontal ticks, left & right of mask)
    vesa_put_pixel(cx-R+4, cy, ACC);
    vesa_put_pixel(cx-R+5, cy, ACC);
    vesa_put_pixel(cx-R+6, cy, 0x113344);
    vesa_put_pixel(cx+R-6, cy, ACC);
    vesa_put_pixel(cx+R-5, cy, ACC);
    vesa_put_pixel(cx+R-4, cy, 0x113344);

    // Mouth: thin glowing horizontal line, neutral/stoic
    int my = cy + R/2;
    int mw = R*3/5;
    for (int dx = -mw/2; dx <= mw/2; dx++) {
        // rounded end-caps via a tiny taper
        int edge = (mw/2) - (dx<0?-dx:dx);
        uint32_t c = (edge < 2) ? 0x227788 : ACC;
        vesa_put_pixel(cx+dx, my,   c);
        vesa_put_pixel(cx+dx, my+1, 0x115566);
    }
}

// ═══════════════════════════════════════════════════════════════════
//  DECORATIVE HORIZONTAL RULE
// ═══════════════════════════════════════════════════════════════════
static void deco_hr(int x,int y,int w){
    // Fade in/out line with center dot
    for(int i=0;i<w;i++){
        int d=(i<w/2)?i:(w-1-i);
        int bright=d*80/w;
        uint32_t c=(uint32_t)bright*0x010101+0x080E18;
        vesa_put_pixel(x+i,y,c);
    }
    // Center accent
    vesa_put_pixel(x+w/2-1,y,ADIM);
    vesa_put_pixel(x+w/2,y,ACC);
    vesa_put_pixel(x+w/2+1,y,ADIM);
    // End dots
    vesa_put_pixel(x,y,ADIM);
    vesa_put_pixel(x+w-1,y,ADIM);
}

// ═══════════════════════════════════════════════════════════════════
//  MAIN DRAW
// ═══════════════════════════════════════════════════════════════════
void login_draw(void){

    // ── VGA FALLBACK ─────────────────────────────────────
    if(!vesa.active){
        vf(0,0,VW,VH,7,1);
        vf(0,0,VW,1,15,1); vs(2,0,"* NYX OS",11,1);
        char ts[9]; rtc_get_time_str(ts); vs(71,0,ts,14,1);
        vf(0,VH-1,VW,1,8,1);
        vc(4,"NYX OS",11,9);
        vc(6,users[su].username,15,9);
        vf(24,9,32,9,0,7); vf(24,9,32,1,0,3);
        vc(9,"SECURE LOGIN",0,3);
        vs(27,11,"Password:",8,7);
        vf(27,12,26,1,0,15);
        for(int i=0;i<pp;i++)vp(27+i,12,'*',1,15);
        vp(27+pp,12,'_',0,14);
        vf(30,14,20,1,0,3); vc(14,"[ SIGN IN ]",0,3);
        if(fail) vc(18,"! Wrong password !",12,1);
        else vc(18,"ENTER to sign in",8,1);
        vc(24,"default: admin / 1234",8,1);
        return;
    }

    int W=(int)vesa.width, H=(int)vesa.height, CX=W/2, CY=H/2;

    // ── Resolution indicator (top-left) ─────────────────────
    char wbuf[6], hbuf[6];
    {
        int n=W,i=4; wbuf[5]='\0';
        if(n==0){wbuf[4]='0';i=3;}
        while(n>0&&i>=0){wbuf[i--]='0'+n%10;n/=10;}
        for(int k=0;k<=i;k++)wbuf[k]=' ';
    }
    {
        int n=H,i=4; hbuf[5]='\0';
        if(n==0){hbuf[4]='0';i=3;}
        while(n>0&&i>=0){hbuf[i--]='0'+n%10;n/=10;}
        for(int k=0;k<=i;k++)hbuf[k]=' ';
    }
    vesa_draw_string(2,52,"RES:",YEL,0);
    vesa_draw_string(40,52,wbuf,WHT,0);
    vesa_draw_string(80,52,"x",YEL,0);
    vesa_draw_string(90,52,hbuf,WHT,0);

    // DEBUG: show last key pressed (numeric code)
    vesa_draw_string(2,68,"KEY:",YEL,0);
    char kbuf[6]; int kc=(int)(unsigned char)last_key_debug;
    kbuf[0]='0'+(kc/100)%10; kbuf[1]='0'+(kc/10)%10; kbuf[2]='0'+kc%10; kbuf[3]='\0';
    vesa_draw_string(40,68,kbuf,GRN,0);
    vesa_draw_string(70,68,"PP:",YEL,0);
    char ppbuf[4]; ppbuf[0]='0'+(pp/10)%10; ppbuf[1]='0'+pp%10; ppbuf[2]='\0';
    vesa_draw_string(95,68,ppbuf,GRN,0);
    vesa_draw_string(115,68,"SU:",YEL,0);
    char subuf[3]; subuf[0]='0'+su; subuf[1]='\0';
    vesa_draw_string(140,68,subuf,GRN,0);
    vesa_draw_string(155,68,"DONE:",YEL,0);
    vesa_draw_string(195,68,done?"Y":"N",GRN,0);

    // ── BACKGROUND ───────────────────────────────────────
    // Deep space top-to-bottom
    vg(0,0,W,H,0x08051A,0x1E1240);
    // Atmospheric glow from bottom center
    for(int i=0;i<150;i++){
        int ww=W*(150-i)/300;
        uint32_t intensity=0x000A16+(uint32_t)(150-i)*0x100/150;
        vesa_hline(CX-ww/2,H-1-i,ww,intensity);
    }
    // Subtle grid
    for(int y=0;y<H;y+=28) vesa_hline(0,y,W,0x140A28);
    for(int x=0;x<W;x+=28) vesa_vline(x,0,H,0x140A28);
    // Stars
    stars();

    // ── TOP BAR ──────────────────────────────────────────
    vg(0,0,W,32,0x251440,0x0F0824);
    vesa_hline(0,32,W,ACC);
    vesa_hline(0,33,W,ADIM);
    // Logo
    glw(14,8,"* NYX OS",ACC,ACC);
    // Date + clock (right side)
    char ts[9]; rtc_get_time_str(ts);
    char ds[9]; rtc_get_date_str(ds);
    vesa_draw_string(W-140,4,ds,GRY,0);
    glw(W-140,18,ts,WHT,0x4A3A66);
    // Status
    vesa_draw_string(W-275,10,"[NET]",GRN,0);
    vesa_draw_string(W-215,10,"[PWR]",YEL,0);

    // ── BOTTOM BAR ───────────────────────────────────────
    vg(0,H-30,W,30,0x0F0824,0x251440);
    vesa_hline(0,H-31,W,ACC);
    vesa_hline(0,H-32,W,ADIM);
    vesa_draw_string(12,H-20,"[PWR]",RED,0);
    vesa_draw_string(68,H-20,"[ACC]",GRN,0);
    vesa_draw_string(124,H-20,"[NET]",ACC,0);
    char ver[]="NYX OS v3.0  2026";
    vesa_draw_string(W-sl(ver)*8-12,H-20,ver,DGRY,0);

    // ── CORNER BRACKETS ──────────────────────────────────
    int bl=18;
    vesa_hline(8,42,bl,ADIM);    vesa_vline(8,42,bl,ADIM);
    vesa_hline(W-9-bl,42,bl,ADIM); vesa_vline(W-9,42,bl,ADIM);
    vesa_hline(8,H-43,bl,ADIM);  vesa_vline(8,H-43-bl,bl,ADIM);
    vesa_hline(W-9-bl,H-43,bl,ADIM); vesa_vline(W-9,H-43-bl,bl,ADIM);

    // ── LAYOUT: left=avatar, right=card ──────────────────
    // Left column center: 1/3 of width
    int LC = W/3;
    // Right column center: 2/3 of width
    int RC = W*2/3;

    // ── AVATAR (left column) ─────────────────────────────
    int av_r=52;
    int av_cy=CY-20;
    avatar(LC,av_cy,av_r);

    // Username below avatar
    const char*un=users[su].username;
    int ul=sl(un);
    int ux=LC-ul*8/2;
    int uy=av_cy+av_r+18;

    // Pill
    rr(ux-18,uy-4,ul*8+36,22,BG3);
    rrb(ux-18,uy-4,ul*8+36,22,ADIM);
    // User arrows
    if(user_count>1){
        vesa_draw_string(ux-36,uy,"<",YEL,0);
        vesa_draw_string(ux+ul*8+20,uy,">",YEL,0);
    }
    glw(ux,uy,un,WHT,ACC);

    // Role
    char role[]="ADMINISTRATOR";
    int rx=LC-sl(role)*8/2;
    vesa_draw_string(rx,uy+24,role,GRY,0);

    // Deco line
    deco_hr(LC-80,uy+44,160);

    // ── LOGIN CARD (right column) ─────────────────────────
    int cw=290, ch=222;
    int cx2=RC-cw/2;
    int cy2=CY-ch/2-10;

    // Card shadow layers
    vesa_fill_rect(cx2+6,cy2+6,cw,ch,0x05030C);
    vesa_fill_rect(cx2+3,cy2+3,cw,ch,0x0A0618);

    // Card body
    rr(cx2,cy2,cw,ch,CARD);
    rrb(cx2,cy2,cw,ch,ADIM);

    // Card header gradient
    hg(cx2+1,cy2+1,cw-2,34,0x004466,ADIM);
    vesa_hline(cx2+1,cy2+34,cw-2,ACC);

    // Header title — center in header. Background matches the
    // gradient's approximate tone at this position instead of
    // literal black, so the text blends into the header instead of
    // showing as a stray black box.
    char htitle[]="SECURE LOGIN";
    int hx=cx2+(cw-sl(htitle)*8)/2;
    vesa_draw_string(hx,cy2+9,htitle,0xE8DCFF,0x2A6080);

    // Small hex decorations left/right of title in header
    vesa_draw_string(hx-20,cy2+9,">>",0xC8B8E8,0x2A6080);
    vesa_draw_string(hx+sl(htitle)*8+4,cy2+9,"<<",0xC8B8E8,0x2A6080);

    // ── PASSWORD SECTION ─────────────────────────────────
    int fx=cx2+24, fw=cw-48;
    int ly=cy2+46, fy=cy2+62;

    vesa_draw_string(fx,ly,"PASSWORD",GRY,CARD);

    // Field shadow
    vesa_fill_rect(fx+2,fy+2,fw,30,0x08051A);
    // Field bg
    vesa_fill_rect(fx,fy,fw,30,FLD);
    // Triple border for depth
    vesa_draw_rect(fx,fy,fw,30,ACC);
    vesa_draw_rect(fx-1,fy-1,fw+2,32,ADIM);
    vesa_draw_rect(fx-2,fy-2,fw+4,34,AFAINT);
    // Inner glow line on top of field
    vesa_hline(fx+1,fy+1,fw-2,0x003344);

    // Password dots
    for(int i=0;i<pp;i++){
        int dx2=fx+12+i*16;
        int dy2=fy+15;
        if(dx2+6>fx+fw-6)break;
        // Glowing dot
        circ(dx2,dy2,5,ACC);
        // Highlight
        vesa_put_pixel(dx2-1,dy2-2,0xD8C0FF);
        vesa_put_pixel(dx2-2,dy2-1,0x44AACC);
    }
    // Cursor
    int curx=fx+12+pp*16;
    if(curx<fx+fw-8){
        vesa_fill_rect(curx-1,fy+6,2,18,YEL);
        vesa_put_pixel(curx-1,fy+5,0x886600);
        vesa_put_pixel(curx,fy+5,0x886600);
    }

    // ── DIVIDER ──────────────────────────────────────────
    deco_hr(cx2+16,cy2+112,cw-32);

    // ── SIGN IN BUTTON ───────────────────────────────────
    int bw=200,bh=36;
    int bx=RC-bw/2, by2=cy2+124;

    // Button shadow
    vesa_fill_rect(bx+3,by2+3,bw,bh,0x140A28);
    // Button gradient
    hg(bx,by2,bw,bh,ACC2,ACC);
    rrb(bx,by2,bw,bh,WHT);
    // Shine
    vesa_hline(bx+2,by2+1,bw-4,0xE0D0FF);
    vesa_hline(bx+2,by2+2,bw-4,0x4A3870);
    // Text
    char bl2[]="[ SIGN IN ]";
    vesa_draw_string(RC-sl(bl2)*8/2,by2+10,bl2,NAVY,0x8A6AC0);

    // ── STATUS / ERROR ────────────────────────────────────
    int sy2=cy2+174;
    if(fail){
        // Red error banner
        vesa_fill_rect(cx2+1,sy2-4,cw-2,22,0x1C0008);
        vesa_hline(cx2+1,sy2-4,cw-2,RED);
        vesa_hline(cx2+1,sy2+17,cw-2,0x880020);
        char em[]="! WRONG PASSWORD - TRY AGAIN !";
        vesa_draw_string(RC-sl(em)*8/2,sy2+1,em,RED,0);
    } else {
        char hint[]="ENTER to sign in   TAB to switch user";
        vesa_draw_string(RC-sl(hint)*8/2,sy2+2,hint,DGRY,BG);
    }

    // ── BOTTOM HINT ──────────────────────────────────────
    ctr(H-48,"default: admin / 1234",DGRY,0);

    vesa_present();
}

// ═══════════════════════════════════════════════════════════════════
//  KEY HANDLER
// ═══════════════════════════════════════════════════════════════════
void login_handle_key(char c){
    last_key_debug = c;
    {   // TEMP debug: log key to COM1
        char* s = "LOGIN KEY "; const char* p = s;
        for (; *p; p++) { while ((inb(0x3F8+5)&0x20)==0){} outb(0x3F8,(uint8_t)*p); }
        char b[8]; int bi=0; uint32_t v=(uint32_t)(unsigned char)c;
        if(v==0){ outb(0x3F8,'0'); outb(0x3F8,'\n'); return; }
        while(v>0){ b[bi++]='0'+v%10; v/=10; }
        while(bi>0) outb(0x3F8,(uint8_t)b[--bi]);
        outb(0x3F8,'\n');
    }
    if(done)return;
    if(c=='\t'){
        if(user_count>1) su=(su+1)%user_count;
        fail=0; login_draw(); return;
    }
    if(c==8){
        if(pp>0)pw[--pp]='\0';
        fail=0; login_draw(); return;
    }
    if(c==13){
        if(auth_login(users[su].username,pw)){
            // Jingle removed temporarily for debugging
            done=1; fail=0;
            if (boot_tick_login == 0) {   // first login of this boot only
                boot_tick_login = tick;   // benchmark mark 3 — login ready
                bmk_report();
            }
        } else {
            fail=1; desktop_set_status("Wrong password!");
            for(int i=0;i<=PL;i++)pw[i]='\0';
            pp=0; login_draw();
        }
        return;
    }
    if(c>=32&&c<127){
        fail=0;
        if(pp<PL){pw[pp++]=c;pw[pp]='\0';}
        login_draw();
    }
}

int  login_is_done(void){return done;}
void login_show(void){
    done=0; fail=0; su=0; pp=0;
    for(int i=0;i<=PL;i++)pw[i]='\0';
    {   // Deterministic marker for the E2E harness: "LOGIN READY" on
        // COM1 as soon as the login screen is interactive, so tests
        // can poll instead of guessing boot time (BSS size varies).
        const char* s = "LOGIN READY\n";
        for (; *s; s++) { while ((inb(0x3F8+5)&0x20)==0){} outb(0x3F8,(uint8_t)*s); }
    }
    login_draw();
}
