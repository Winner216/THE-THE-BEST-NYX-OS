// cpu/calc.c  –  Scientific calculator  (VGA fallback + VESA pixel UI)
#include <calc.h>
#include <vesa.h>
#include <stdint.h>
#include <mouse.h>
#include <desktop.h>

// ─── VESA scaled-text (defined in vesa.c, not yet in header) ──────
extern void vesa_draw_char_scaled(int x,int y,char c,uint32_t fg,uint32_t bg,int scale);

// ─── VGA helpers (text-mode fallback) ─────────────────────────────
#define VGA_WIDTH  80
#define VGA_HEIGHT 25
#define VGA_MEM    ((uint16_t*)0xB8000)
static inline uint16_t make_entry(char c,uint8_t fg,uint8_t bg){
    return (uint16_t)c|(uint16_t)(fg|bg<<4)<<8;
}
static void cput(int x,int y,char c,uint8_t fg,uint8_t bg){
    if(x<0||x>=VGA_WIDTH||y<0||y>=VGA_HEIGHT)return;
    VGA_MEM[y*VGA_WIDTH+x]=make_entry(c,fg,bg);
}
static void cputs(int x,int y,const char*s,uint8_t fg,uint8_t bg){
    for(int i=0;s[i];i++) cput(x+i,y,s[i],fg,bg);
}
static void cfill(int x,int y,int w,int h,char c,uint8_t fg,uint8_t bg){
    for(int dy=0;dy<h;dy++) for(int dx=0;dx<w;dx++) cput(x+dx,y+dy,c,fg,bg);
}

// ═══════════════════════════════════════════════════════════════════
//  FIXED-POINT MATH  (scale = 1000, so 1.5 = 1500)
// ═══════════════════════════════════════════════════════════════════

#define SCALE    1000
#define LN10     2303   /* ln(10) * SCALE */
#define PI_FIXED 3142   /* π * SCALE      */

typedef int fixed_t;
static int calc_error = 0;   /* set on math errors; checked in key handler */

static fixed_t fp_mul(fixed_t a, fixed_t b){
    return (fixed_t)((long long)a * b / SCALE);
}
static fixed_t fp_div(fixed_t a, fixed_t b){
    if(b==0){ calc_error=1; return 0; }
    return (fixed_t)((long long)a * SCALE / b);
}
// √x : returns y s.t.  y/SCALE = sqrt(x/SCALE)
static fixed_t fp_sqrt(fixed_t x){
    if(x < 0){ calc_error=1; return 0; }
    if(x == 0) return 0;
    long long target=(long long)x*SCALE;
    long long y=target/2+1;
    for(int i=0;i<50;i++){
        long long nxt=(y+target/y)/2;
        if(nxt>=y) break;
        y=nxt;
    }
    return (fixed_t)y;
}
// ln(x) with range reduction via repeated ÷10
static fixed_t fp_log_series(fixed_t x){
    fixed_t u=(fixed_t)((long long)(x-SCALE)*SCALE/(x+SCALE));
    fixed_t u2=(fixed_t)((long long)u*u/SCALE);
    fixed_t term=u,sum=u;
    for(int n=1;n<=14;n++){
        term=(fixed_t)((long long)term*u2/SCALE);
        if(term==0) break;
        sum+=term/(2*n+1);
    }
    return 2*sum;
}
static fixed_t fp_log(fixed_t x){
    if(x<=0){ calc_error=1; return 0; }
    int adj=0;
    while(x>10*SCALE){ x/=10; adj++; }
    while(x<SCALE)   { x*=10; adj--; }
    return fp_log_series(x)+(fixed_t)adj*LN10;
}
// sin(degrees) – Bhaskara I, ±0.0016 max error
static fixed_t fp_sin_deg(int deg){
    while(deg<0)   deg+=360;
    while(deg>=360) deg-=360;
    int sign=1;
    if(deg>180){ deg-=180; sign=-1; }
    int x=deg;
    int num=4*x*(180-x);
    int den=40500-x*(180-x);
    if(den==0) return 0;
    return sign*(fixed_t)((long long)num*SCALE/den);
}
static fixed_t fp_cos_deg(int deg){ return fp_sin_deg(deg+90); }
static fixed_t fp_tan_deg(int deg){
    fixed_t s=fp_sin_deg(deg), c=fp_cos_deg(deg);
    if(c==0){ calc_error=1; return 0; }
    return fp_div(s,c);
}
// x² and |x|
static fixed_t fp_sq(fixed_t x)  { return fp_mul(x,x); }
static fixed_t fp_abs(fixed_t x) { return x<0?-x:x; }

// ─── Number ↔ string ──────────────────────────────────────────────

static int c_strlen(const char*s){ int i=0; while(s[i]) i++; return i; }

/* Convert fixed-point to string, stripping trailing decimal zeros. */
static void fp_to_str(fixed_t val, char*buf){
    int pos=0;
    if(val<0){ buf[pos++]='-'; val=-val; }
    int ip=val/SCALE, dp=val%SCALE;
    // integer part
    if(ip==0){ buf[pos++]='0'; }
    else{
        char tmp[12]; int tp=0, n=ip;
        while(n>0){ tmp[tp++]='0'+n%10; n/=10; }
        for(int i=tp-1;i>=0;i--) buf[pos++]=tmp[i];
    }
    // decimal part – only if non-zero
    if(dp!=0){
        buf[pos++]='.';
        char d[4];
        d[0]='0'+dp/100; d[1]='0'+dp/10%10; d[2]='0'+dp%10; d[3]='\0';
        int dl=3; while(dl>0&&d[dl-1]=='0') dl--;
        for(int i=0;i<dl;i++) buf[pos++]=d[i];
    }
    buf[pos]='\0';
}

static fixed_t str_to_fp(const char*s){
    fixed_t result=0; int neg=0,i=0,decimal=0,dec_mul=SCALE/10;
    if(s[i]=='-'){ neg=1; i++; }
    for(;s[i];i++){
        if(s[i]=='.'){ decimal=1; continue; }
        if(s[i]<'0'||s[i]>'9') break;
        int d=s[i]-'0';
        if(!decimal) result=result*10+d*SCALE;
        else{ result+=d*dec_mul; dec_mul/=10; }
    }
    return neg?-result:result;
}

// ─── Calculator state ─────────────────────────────────────────────

#define DISPLAY_LEN   20
#define HISTORY_ROWS   4
#define HIST_STR_LEN  48

static char    display[DISPLAY_LEN+1];
static int     display_pos   = 0;
static fixed_t accumulator   = 0;
static char    pending_op    = 0;    /* +,-,*,/ */
static fixed_t pending_lhs   = 0;   /* lhs when op was pressed */
static int     new_number    = 1;
static char    expr_str[HIST_STR_LEN]; /* e.g. "3.14 + " */
static char    hist[HISTORY_ROWS][HIST_STR_LEN];
static int     hist_count    = 0;

static void calc_clear(){
    for(int i=0;i<=DISPLAY_LEN;i++) display[i]='\0';
    display_pos=0; new_number=1; calc_error=0;
    expr_str[0]='\0';
}

/* Push a string to top of history ring, shifting others down. */
static void hist_push(const char*s){
    for(int i=HISTORY_ROWS-1;i>0;i--)
        for(int j=0;j<HIST_STR_LEN;j++) hist[i][j]=hist[i-1][j];
    int p=0;
    for(;s[p]&&p<HIST_STR_LEN-1;p++) hist[0][p]=s[p];
    hist[0][p]='\0';
    if(hist_count<HISTORY_ROWS) hist_count++;
}

/* Build "lhs op rhs = result" and push to history. */
static void hist_add_binary(fixed_t lhs, char op, fixed_t rhs, fixed_t result){
    char buf[HIST_STR_LEN]; int p=0;
    char tmp[24];
    fp_to_str(lhs,tmp); for(int i=0;tmp[i]&&p<HIST_STR_LEN-2;i++) buf[p++]=tmp[i];
    buf[p++]=' '; buf[p++]=op; buf[p++]=' ';
    fp_to_str(rhs,tmp); for(int i=0;tmp[i]&&p<HIST_STR_LEN-2;i++) buf[p++]=tmp[i];
    buf[p++]='=';
    fp_to_str(result,tmp); for(int i=0;tmp[i]&&p<HIST_STR_LEN-1;i++) buf[p++]=tmp[i];
    buf[p]='\0';
    hist_push(buf);
}

/* Build "fn(arg) = result" and push. */
static void hist_add_fn(const char*fn, fixed_t arg, fixed_t result){
    char buf[HIST_STR_LEN]; int p=0;
    for(int i=0;fn[i]&&p<8;i++) buf[p++]=fn[i];
    buf[p++]='(';
    char tmp[24]; fp_to_str(arg,tmp);
    for(int i=0;tmp[i]&&p<HIST_STR_LEN-8;i++) buf[p++]=tmp[i];
    buf[p++]=')'; buf[p++]='=';
    fp_to_str(result,tmp);
    for(int i=0;tmp[i]&&p<HIST_STR_LEN-1;i++) buf[p++]=tmp[i];
    buf[p]='\0';
    hist_push(buf);
}

static void display_set(fixed_t val){
    fp_to_str(val,display);
    display_pos=c_strlen(display);
}

// ─── Core arithmetic ──────────────────────────────────────────────

static fixed_t do_op(fixed_t a, char op, fixed_t b){
    switch(op){
        case '+': return a+b;
        case '-': return a-b;
        case '*': return fp_mul(a,b);
        case '/': return fp_div(a,b);
        case '%': if(b==0){calc_error=1;return 0;}
                  return a-(fixed_t)((long long)a/b*b);
    }
    return b;
}

// ═══════════════════════════════════════════════════════════════════
//  VESA CALCULATOR UI
// ═══════════════════════════════════════════════════════════════════

// ── Palette ───────────────────────────────────────────────────────
#define CC_BG      0x12102A
#define CC_TITLE   0x1A1440
#define CC_DISP    0x07051A   /* display panel bg */
#define CC_HIST_BG 0x0D0B22
#define CC_BORDER  0x2B2448
#define CC_DIV     0x1E1B36
#define CC_TEXT    0xE8E0FF
#define CC_DIM     0x6858A0
#define CC_ACCENT  0x9B7FFF
#define CC_DNUM    0xF0ECFF   /* display number color */
#define CC_EXPR_C  0x7868B0   /* expression line color */
#define CC_HIST_C  0x5A4E88   /* history text color */

#define CB_NUM     0x1D1B38   /* number button */
#define CB_OP      0x2A1868   /* operator button */
#define CB_SCI     0x17103E   /* scientific button */
#define CB_EQ      0x14481C   /* = button */
#define CB_DEL     0x3A1A08   /* DEL */
#define CB_CLR     0x3A0808   /* CLR */
#define CB_CONST   0x0C2840   /* π, constants */

#define CT_NUM     0xE0D8FF
#define CT_OP      0xC0A0FF
#define CT_SCI     0x90B0FF
#define CT_EQ      0x78FF88
#define CT_DEL     0xFF9840
#define CT_CLR     0xFF5858
#define CT_CONST   0x60D8FF

// ── Drawing helpers ───────────────────────────────────────────────

static uint32_t col_add(uint32_t c, int d){
    int r=(int)(c>>16&0xFF)+d; if(r<0)r=0; if(r>255)r=255;
    int g=(int)(c>>8 &0xFF)+d; if(g<0)g=0; if(g>255)g=255;
    int b=(int)(c    &0xFF)+d; if(b<0)b=0; if(b>255)b=255;
    return ((uint32_t)r<<16)|((uint32_t)g<<8)|(uint32_t)b;
}

/* Draw a scaled string (supports scale=1 or 2). */
static void draw_str_scaled(int x, int y, const char*s, uint32_t fg, uint32_t bg, int sc){
    if(sc<=1){ vesa_draw_string(x,y,s,fg,bg); return; }
    for(int i=0;s[i];i++) vesa_draw_char_scaled(x+i*8*sc,y,s[i],fg,bg,sc);
}

/* Draw a right-aligned string. */
static void draw_str_right(int rx, int y, const char*s, uint32_t fg, uint32_t bg){
    vesa_draw_string(rx-c_strlen(s)*8,y,s,fg,bg);
}

/* Draw a right-aligned SCALED string. */
static void draw_str_right_scaled(int rx, int y, const char*s,
                                   uint32_t fg, uint32_t bg, int sc){
    int x=rx-c_strlen(s)*8*sc;
    if(x<0) x=0;
    draw_str_scaled(x,y,s,fg,bg,sc);
}

/* Draw a button. `active` = 1 means the button is the pending operator. */
static void draw_btn(int x, int y, int w, int h, const char*label,
                     uint32_t fg, uint32_t bg, int active){
    vesa_fill_rect(x,y,w,h,bg);
    // Top highlight
    vesa_hline(x+1,y+1,w-2,col_add(bg,+28));
    // Left highlight
    vesa_vline(x+1,y+1,h-2,col_add(bg,+16));
    // Bottom shadow
    vesa_hline(x+1,y+h-2,w-2,col_add(bg,-20));
    // Border
    uint32_t border_col = active ? 0x9070E8 : CC_BORDER;
    vesa_draw_rect(x,y,w,h,border_col);
    if(active) vesa_draw_rect(x+1,y+1,w-2,h-2,col_add(border_col,-30));
    // Hover highlight
    if (!active && mouse_in_rect(x, y, w, h)) {
        vesa_draw_rect(x-1, y-1, w+2, h+2, 0x9B7FFF);
        vesa_fill_rect(x, y, w, 2, 0xD8C0FF);
    }
    // Label centered
    int llen=c_strlen(label);
    int tx=x+(w-llen*8)/2;
    int ty=y+(h-16)/2;
    vesa_draw_string(tx,ty,label,fg,bg);
}

// ─── Main VESA draw ───────────────────────────────────────────────
void calc_draw(){
    if(!vesa.active) goto vga_draw;

    {
    int W=(int)vesa.width, H=(int)vesa.height;

    // ── Fullscreen window  (matches Settings panel convention)
    int WX=16, WY=40;
    int WW=W-32, WH=H-50;
    g_win_x = WX; g_win_y = WY; g_win_w = WW; g_win_h = WH;

    // ── Window background
    vesa_fill_rect(WX,WY,WW,WH,CC_BG);

    // ── Title bar
    vesa_fill_rect(WX,WY,WW,36,CC_TITLE);
    vesa_hline(WX,WY+36,WW,CC_BORDER);
    vesa_draw_string(WX+18,WY+10,"Calculator",CC_TEXT,CC_TITLE);
    draw_close_btn(WX+WW-40, WY+10, CC_TITLE, "Calculator");

    // ── Centered content column  (keeps proportions sane on wide screens)
    int PANEL_W = WW>720 ? 680 : WW;
    int PX = WX + (WW-PANEL_W)/2;

    // ── Display panel  (below title) — taller to fill extra vertical room
    int DY=WY+36;
    int disp_h = H>700 ? 130 : 100;
    vesa_fill_rect(WX,DY,WW,disp_h,CC_DISP);
    vesa_hline(WX,DY+disp_h,WW,CC_BORDER);

    // Expression line (small, top of display)
    {
        char exbuf[HIST_STR_LEN];
        int ep=0;
        if(pending_op){
            char tmp[24]; fp_to_str(pending_lhs,tmp);
            for(int i=0;tmp[i]&&ep<HIST_STR_LEN-4;i++) exbuf[ep++]=tmp[i];
            exbuf[ep++]=' '; exbuf[ep++]=pending_op; exbuf[ep++]=' ';
            exbuf[ep]='\0';
        } else {
            exbuf[0]='\0';
        }
        if(exbuf[0]) vesa_draw_string(PX+16,DY+14,exbuf,CC_EXPR_C,CC_DISP);
    }

    // Main display number — scale 3 when it fits, drops to 2 then 1 for long results
    {
        const char*dstr=(calc_error?"ERROR":(display_pos>0?display:"0"));
        uint32_t dcol = calc_error ? 0xFF5555 : CC_DNUM;
        int dlen=c_strlen(dstr);
        int max_w=PANEL_W-32;
        int sc=3;
        while(sc>1 && dlen*8*sc>max_w) sc--;
        int ch_h=16*sc;
        draw_str_right_scaled(PX+PANEL_W-16, DY+disp_h-ch_h-10, dstr, dcol, CC_DISP, sc);
    }

    // ── History panel
    int HY=DY+disp_h;
    int hist_h = H>700 ? 100 : 70;
    vesa_fill_rect(WX,HY,WW,hist_h,CC_HIST_BG);
    vesa_hline(WX,HY+hist_h,WW,CC_BORDER);
    vesa_draw_string(PX+16,HY+8,"History",CC_DIM,CC_HIST_BG);
    int hist_row_h=(hist_h-12)/HISTORY_ROWS; if(hist_row_h>22)hist_row_h=22; if(hist_row_h<14)hist_row_h=14;
    for(int i=0;i<HISTORY_ROWS;i++){
        int ry=HY+8+22+i*hist_row_h;
        if(ry+14 > HY+hist_h) break;
        if(i<hist_count)
            vesa_draw_string(PX+16,ry,hist[i],CC_HIST_C,CC_HIST_BG);
        else
            vesa_draw_string(PX+16,ry,"----",col_add(CC_HIST_BG,+18),CC_HIST_BG);
    }

    // ── Button area layout — scaled to the centered panel width
    // Inner usable width: PANEL_W - 2*16 padding
    int inner_w = PANEL_W-32;
    int BX=PX+16;
    int sci_gap=6, std_gap=8;
    int sci_bw=(inner_w-7*sci_gap)/8;
    int std_bw=(inner_w-3*std_gap)/4;
    int avail_btn_h = WY+WH-(HY+hist_h)-44;   /* leave room for hint strip */
    int sci_bh = avail_btn_h/9; if(sci_bh>56)sci_bh=56; if(sci_bh<34)sci_bh=34;
    int std_bh = sci_bh+ (avail_btn_h-sci_bh*9)/4; if(std_bh<sci_bh)std_bh=sci_bh+4;
    int BY=HY+hist_h+10;

    // ── Sci row  (sin cos tan ln  sqrt  x²  +/-  pi)
    const char*sci_lbl[]={ "sin","cos","tan","ln","sqrt","x^2","+/-","pi" };
    uint32_t    sci_fg[] ={ CT_SCI,CT_SCI,CT_SCI,CT_SCI,CT_SCI,CT_SCI,CT_OP,CT_CONST };
    uint32_t    sci_bg[] ={ CB_SCI,CB_SCI,CB_SCI,CB_SCI,CB_SCI,CB_SCI,CB_OP,CB_CONST };
    for(int i=0;i<8;i++){
        int bx=BX+i*(sci_bw+sci_gap);
        draw_btn(bx,BY,sci_bw,sci_bh,sci_lbl[i],sci_fg[i],sci_bg[i],0);
    }
    BY+=sci_bh+std_gap;

    // ── Row 1: 7 8 9 ÷   +  DEL / CLR stacked on the far right
    {
        const char*lbl[]={"7","8","9","/"};
        uint32_t fg[]={CT_NUM,CT_NUM,CT_NUM,CT_OP};
        uint32_t bg[]={CB_NUM,CB_NUM,CB_NUM,CB_OP};
        for(int i=0;i<4;i++){
            int bx=BX+i*(std_bw+std_gap);
            int act=(i==3&&pending_op=='/');
            draw_btn(bx,BY,std_bw,std_bh,lbl[i],fg[i],bg[i],act);
        }
        draw_btn(PX+PANEL_W-16-std_bw,BY,std_bw,std_bh/2-2,"DEL",CT_DEL,CB_DEL,0);
        draw_btn(PX+PANEL_W-16-std_bw,BY+std_bh/2,std_bw,std_bh/2,"CLR",CT_CLR,CB_CLR,0);
    }
    BY+=std_bh+std_gap;

    // ── Row 2: 4 5 6 ×
    {
        const char*lbl[]={"4","5","6","*"};
        uint32_t fg[]={CT_NUM,CT_NUM,CT_NUM,CT_OP};
        uint32_t bg[]={CB_NUM,CB_NUM,CB_NUM,CB_OP};
        for(int i=0;i<4;i++){
            int bx=BX+i*(std_bw+std_gap);
            int act=(i==3&&pending_op=='*');
            draw_btn(bx,BY,std_bw,std_bh,lbl[i],fg[i],bg[i],act);
        }
    }
    BY+=std_bh+std_gap;

    // ── Row 3: 1 2 3 -
    {
        const char*lbl[]={"1","2","3","-"};
        uint32_t fg[]={CT_NUM,CT_NUM,CT_NUM,CT_OP};
        uint32_t bg[]={CB_NUM,CB_NUM,CB_NUM,CB_OP};
        for(int i=0;i<4;i++){
            int bx=BX+i*(std_bw+std_gap);
            int act=(i==3&&pending_op=='-');
            draw_btn(bx,BY,std_bw,std_bh,lbl[i],fg[i],bg[i],act);
        }
    }
    BY+=std_bh+std_gap;

    // ── Row 4: 0 . = +
    {
        const char*lbl[]={"0",".","=","+"};
        uint32_t fg[]={CT_NUM,CT_NUM,CT_EQ,CT_OP};
        uint32_t bg[]={CB_NUM,CB_NUM,CB_EQ,CB_OP};
        for(int i=0;i<4;i++){
            int bx=BX+i*(std_bw+std_gap);
            int act=(i==3&&pending_op=='+');
            draw_btn(bx,BY,std_bw,std_bh,lbl[i],fg[i],bg[i],act);
        }
    }
    BY+=std_bh+8;

    // ── Key hint strip — pinned to the bottom of the window
    int hint_y = WY+WH-30;
    if(BY<hint_y){ vesa_fill_rect(WX,BY,WW,hint_y-BY,CC_BG); BY=hint_y; }
    vesa_hline(WX,BY,WW,CC_BORDER);
    vesa_fill_rect(WX,BY+1,WW,WY+WH-(BY+1),CC_BG);
    vesa_draw_string(PX+16,BY+8,
        "s:sin  o:cos  t:tan  l:ln  q:sqrt  x:x2  p:pi  n:+/-  ESC:back",
        col_add(CC_BG,+48),CC_BG);

    return;
    }

// ──────────────────────────────────────────────────────────────────
vga_draw:;
    // VGA text-mode fallback (unchanged layout, same keys)
    cfill(2,2,76,21,' ',15,0);
    {
        // title
        uint16_t*m=VGA_MEM;
        for(int x=2;x<78;x++) m[2*VGA_WIDTH+x]=make_entry(' ',0,5);
        cputs(3,2,"[ Calculator ]",15,5); cputs(73,2,"[X]",15,1);
        // display
        cfill(3,4,50,1,' ',0,7);
        if(calc_error) cputs(3,4,"  ERROR - press C to clear",12,7);
        else if(display_pos==0) cputs(3,4,"  0",0,7);
        else{
            int dl=display_pos, dx=3+(47-dl); if(dx<3)dx=3;
            cputs(dx,4,display,0,7);
        }
        if(pending_op){ char op[4]={' ',pending_op,' ','\0'}; cputs(54,4,op,14,7); }
        // history
        cputs(55,4,"History:",11,0);
        for(int i=0;i<HISTORY_ROWS;i++){
            cfill(55,5+i,23,1,' ',7,0);
            if(i<hist_count) cputs(55,5+i,hist[i],7,0);
        }
        cputs(3,5,"-------------------------------------------",8,0);
        // sci row
        cputs(3,7,"[sin]",15,5); cputs(9,7,"[cos]",15,5); cputs(15,7,"[tan]",15,5);
        cputs(21,7,"[ ln]",15,5); cputs(27,7,"[sqt]",15,5); cputs(33,7,"[ x2]",15,5);
        cputs(39,7,"[+/-]",15,8); cputs(45,7,"[ pi]",15,2);
        cputs(3,8,"-------------------------------------------",8,0);
        // rows
        cputs(3,9,"[7]",0,7); cputs(7,9,"[8]",0,7); cputs(11,9,"[9]",0,7);
        cputs(15,9,"[/]",0,3); cputs(19,9,"[DEL]",15,4); cputs(25,9,"[CLR]",15,4);
        cputs(3,11,"[4]",0,7); cputs(7,11,"[5]",0,7); cputs(11,11,"[6]",0,7); cputs(15,11,"[*]",0,3);
        cputs(3,13,"[1]",0,7); cputs(7,13,"[2]",0,7); cputs(11,13,"[3]",0,7); cputs(15,13,"[-]",0,3);
        cputs(3,15,"[0]",0,7); cputs(7,15,"[.]",0,7); cputs(11,15,"[=]",15,10); cputs(15,15,"[+]",0,3);
        cputs(3,17,"-------------------------------------------",8,0);
        cputs(3,18,"s:sin o:cos t:tan l:ln q:sqrt x:x2 p:pi",7,0);
        cputs(3,19,"n:+/-  c:clear  BKSP:del  ESC:back",7,0);
    }
}

// ═══════════════════════════════════════════════════════════════════
//  KEY HANDLER
// ═══════════════════════════════════════════════════════════════════

void calc_handle_key(char c){
    if(calc_error && c!='c' && c!='C') return;

    // ── Digits
    if(c>='0' && c<='9'){
        if(new_number){ calc_clear(); new_number=0; }
        if(display_pos < DISPLAY_LEN){
            display[display_pos++]=c; display[display_pos]='\0';
        }
        return;
    }

    // ── Decimal point
    if(c=='.'){
        int has_dot=0;
        for(int i=0;i<display_pos;i++) if(display[i]=='.') has_dot=1;
        if(!has_dot && display_pos<DISPLAY_LEN){
            if(display_pos==0){ display[display_pos++]='0'; }
            display[display_pos++]='.'; display[display_pos]='\0';
            new_number=0;
        }
        return;
    }

    // ── Binary operators  + - * / %
    if(c=='+'||c=='-'||c=='*'||c=='/'||c=='%'){
        fixed_t current=str_to_fp(display);
        if(pending_op && !new_number)
            accumulator=do_op(accumulator,pending_op,current);
        else
            accumulator=current;
        pending_lhs=accumulator;
        pending_op=c; new_number=1;
        display_set(accumulator);
        return;
    }

    // ── Equals
    if(c=='='||c==13){
        if(pending_op){
            fixed_t rhs=str_to_fp(display);
            fixed_t result=do_op(accumulator,pending_op,rhs);
            if(!calc_error){
                hist_add_binary(accumulator,pending_op,rhs,result);
                display_set(result);
                accumulator=result; pending_op=0; pending_lhs=0;
                new_number=1;
            }
        }
        return;
    }

    // ── Backspace
    if(c==8){
        if(display_pos>0){ display[--display_pos]='\0'; }
        return;
    }

    // ── Clear / All clear
    if(c=='c'||c=='C'){
        calc_clear(); accumulator=0; pending_op=0; pending_lhs=0;
        return;
    }

    // ── Negate  (n / N)
    if(c=='n'||c=='N'){
        if(display_pos>0){
            fixed_t val=-str_to_fp(display); display_set(val);
        }
        return;
    }

    // ── √ (q / Q)
    if(c=='q'||c=='Q'){
        fixed_t val=str_to_fp(display);
        fixed_t result=fp_sqrt(val);
        if(!calc_error){ hist_add_fn("sqrt",val,result); display_set(result); new_number=1; }
        return;
    }

    // ── sin  (s / S)
    if(c=='s'||c=='S'){
        fixed_t val=str_to_fp(display);
        int deg=(int)(val/SCALE);
        fixed_t result=fp_sin_deg(deg);
        hist_add_fn("sin",val,result); display_set(result); new_number=1;
        return;
    }

    // ── cos  (o / O)
    if(c=='o'||c=='O'){
        fixed_t val=str_to_fp(display);
        int deg=(int)(val/SCALE);
        fixed_t result=fp_cos_deg(deg);
        hist_add_fn("cos",val,result); display_set(result); new_number=1;
        return;
    }

    // ── tan  (t / T)
    if(c=='t'||c=='T'){
        fixed_t val=str_to_fp(display);
        int deg=(int)(val/SCALE);
        fixed_t result=fp_tan_deg(deg);
        if(!calc_error){ hist_add_fn("tan",val,result); display_set(result); new_number=1; }
        return;
    }

    // ── ln  (l / L)
    if(c=='l'||c=='L'){
        fixed_t val=str_to_fp(display);
        fixed_t result=fp_log(val);
        if(!calc_error){ hist_add_fn("ln",val,result); display_set(result); new_number=1; }
        return;
    }

    // ── x²  (x / X)
    if(c=='x'||c=='X'){
        fixed_t val=str_to_fp(display);
        fixed_t result=fp_sq(val);
        hist_add_fn("x^2",val,result); display_set(result); new_number=1;
        return;
    }

    // ── |x|  (a / A)
    if(c=='a'||c=='A'){
        fixed_t val=str_to_fp(display);
        fixed_t result=fp_abs(val);
        hist_add_fn("|x|",val,result); display_set(result); new_number=1;
        return;
    }

    // ── π  (p / P)
    if(c=='p'||c=='P'){
        display_set(PI_FIXED); new_number=0;
        return;
    }
}

// ── Mouse click handler ────────────────────────────────────────────
// Returns 1 if the click was on a button (triggers the same action as
// the keyboard equivalent), 0 otherwise.
int calc_handle_mouse(int mx, int my) {
    if (!vesa.active) return 0;
    int W = (int)vesa.width, H = (int)vesa.height;
    int WX = 16, WY = 40;
    int WW = W - 32, WH = H - 50;

    // Check if click is within the button area (below history panel)
    // ── Layout math matches calc_draw() exactly ─────────────────
    int PANEL_W = WW > 720 ? 680 : WW;
    int PX = WX + (WW - PANEL_W) / 2;
    int DY = WY + 36;
    int disp_h = H > 700 ? 130 : 100;
    int HY = DY + disp_h;
    int hist_h = H > 700 ? 100 : 70;

    int inner_w = PANEL_W - 32;
    int BX = PX + 16;
    int sci_gap = 6, std_gap = 8;
    int sci_bw = (inner_w - 7 * sci_gap) / 8;
    int std_bw = (inner_w - 3 * std_gap) / 4;
    int avail_btn_h = WY + WH - (HY + hist_h) - 44;
    int sci_bh = avail_btn_h / 9;
    if (sci_bh > 56) sci_bh = 56;
    if (sci_bh < 34) sci_bh = 34;
    int std_bh = sci_bh + (avail_btn_h - sci_bh * 9) / 4;
    if (std_bh < sci_bh) std_bh = sci_bh + 4;
    int BY = HY + hist_h + 10;

    // Helper: check if (mx,my) is inside a button area
    #define IN_BTN(bx, by, bw, bh) \
        (mx >= (bx) && mx < (bx) + (bw) && my >= (by) && my < (by) + (bh))

    // ── Sci row: sin cos tan ln sqrt x^2 +/- pi ────────────────
    const char* sci_keys = "sotlqnxp";
    for (int i = 0; i < 8; i++) {
        int bx = BX + i * (sci_bw + sci_gap);
        if (IN_BTN(bx, BY, sci_bw, sci_bh)) {
            if (sci_keys[i] != ' ') calc_handle_key(sci_keys[i]);
            return 1;
        }
    }
    BY += sci_bh + std_gap;

    // ── Row 1: 7 8 9 /  +  DEL / CLR on the right ─────────────
    {
        const char* keys = "789/";
        for (int i = 0; i < 4; i++) {
            int bx = BX + i * (std_bw + std_gap);
            if (IN_BTN(bx, BY, std_bw, std_bh)) {
                calc_handle_key(keys[i]);
                return 1;
            }
        }
        // DEL
        int dx = PX + PANEL_W - 16 - std_bw;
        if (IN_BTN(dx, BY, std_bw, std_bh / 2 - 2)) {
            calc_handle_key(8);  // backspace
            return 1;
        }
        // CLR
        if (IN_BTN(dx, BY + std_bh / 2, std_bw, std_bh / 2)) {
            calc_handle_key('c');
            return 1;
        }
    }
    BY += std_bh + std_gap;

    // ── Row 2: 4 5 6 * ────────────────────────────────────────
    {
        const char* keys = "456*";
        for (int i = 0; i < 4; i++) {
            int bx = BX + i * (std_bw + std_gap);
            if (IN_BTN(bx, BY, std_bw, std_bh)) {
                calc_handle_key(keys[i]);
                return 1;
            }
        }
    }
    BY += std_bh + std_gap;

    // ── Row 3: 1 2 3 - ────────────────────────────────────────
    {
        const char* keys = "123-";
        for (int i = 0; i < 4; i++) {
            int bx = BX + i * (std_bw + std_gap);
            if (IN_BTN(bx, BY, std_bw, std_bh)) {
                calc_handle_key(keys[i]);
                return 1;
            }
        }
    }
    BY += std_bh + std_gap;

    // ── Row 4: 0 . = + ────────────────────────────────────────
    {
        const char* keys = "0.=";
        for (int i = 0; i < 3; i++) {
            int bx = BX + i * (std_bw + std_gap);
            if (IN_BTN(bx, BY, std_bw, std_bh)) {
                calc_handle_key(keys[i]);
                return 1;
            }
        }
        // + button at column 4
        int bx = BX + 3 * (std_bw + std_gap);
        if (IN_BTN(bx, BY, std_bw, std_bh)) {
            calc_handle_key('+');
            return 1;
        }
    }

    #undef IN_BTN
    return 0;
}

void calc_init(){
    calc_clear();
    accumulator=0; pending_op=0; pending_lhs=0;
    hist_count=0;
    for(int i=0;i<HISTORY_ROWS;i++) hist[i][0]='\0';
}
