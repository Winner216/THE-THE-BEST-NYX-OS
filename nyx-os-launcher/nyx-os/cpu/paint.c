// cpu/paint.c  –  Nyx OS Paint  (VGA ASCII art + VESA pixel canvas)
#include <paint.h>
#include <vesa.h>
#include <speaker.h>
#include <fs.h>
#include <stdint.h>
#include <mouse.h>
#include <desktop.h>

// ─── External ─────────────────────────────────────────────────────
extern void vesa_draw_char_scaled(int x,int y,char c,
                                   uint32_t fg,uint32_t bg,int scale);

// ═══════════════════════════════════════════════════════════════════
//  SHARED CONSTANTS & CANVAS
// ═══════════════════════════════════════════════════════════════════

#define VGA_WIDTH  80
#define VGA_HEIGHT 25
#define VGA_MEM    ((uint16_t*)0xB8000)

#define CANVAS_W   66
#define CANVAS_H   21

// Every cell stores char + VGA fg/bg color indices (0-15)
typedef struct { char ch; uint8_t fg; uint8_t bg; } Cell;

static Cell canvas[CANVAS_H][CANVAS_W];
static Cell undo_canvas[CANVAS_H][CANVAS_W];
static int  has_undo = 0;

// ─── Tools ────────────────────────────────────────────────────────
#define TOOL_DRAW   0
#define TOOL_ERASE  1
#define TOOL_FILL   2
#define TOOL_LINE   3
#define TOOL_RECT   4
#define TOOL_CIRCLE 5
#define TOOL_TEXT   6
#define TOOL_PICK   7
#define TOOL_COUNT  8

static const char *tool_names[] = {
    "Draw","Erase","Fill","Line","Rect","Circle","Text","Pick"
};
static const char  tool_keys[]  = {
    'P','E','F','L','R','O','T','K'
};
static const char  tool_icons[] = {
    '*',' ','#','/','+',' ','A','?'
};

// ─── State ────────────────────────────────────────────────────────
static int cur_x = CANVAS_W/2, cur_y = CANVAS_H/2;
static int cur_tool  = TOOL_DRAW;
static uint8_t cur_fg = 15, cur_bg = 0;
static char    cur_char = '*';
static int     is_drawing = 0;

static int start_x=0, start_y=0, tool_active=0;
static int text_mode=0, text_x=0, text_y=0;
static int show_save=0;
static char save_name[32];
static int  save_pos=0;
static char status_msg[48];
static int  status_timer=0;

static const char brush_chars[] = "*#@+.ox=-~^|/\\!?&%$";
static int cur_brush_idx = 0;

// ═══════════════════════════════════════════════════════════════════
//  VGA PALETTE  (indices 0-15 → 32-bit RGB for VESA rendering)
// ═══════════════════════════════════════════════════════════════════

static const uint32_t vga_pal[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
    0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
    0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

static const char *color_names[16] = {
    "Black","Blue","Green","Cyan","Red","Magenta","Brown","LGrey",
    "DGrey","LBlue","LGreen","LCyan","LRed","LMagenta","Yellow","White"
};

// ═══════════════════════════════════════════════════════════════════
//  STRING HELPERS
// ═══════════════════════════════════════════════════════════════════

static int  p_len(const char*s){int i=0;while(s[i])i++;return i;}
static void p_cpy(char*d,const char*s,int max){
    int i=0;while(s[i]&&i<max-1){d[i]=s[i];i++;}d[i]='\0';
}
static void p_itoa2(int n,char*b){
    if(n<0)n=0;if(n>99)n=99;
    b[0]='0'+n/10;b[1]='0'+n%10;b[2]='\0';
}
static void set_status(const char*m){p_cpy(status_msg,m,48);status_timer=50;}

// ═══════════════════════════════════════════════════════════════════
//  CANVAS OPERATIONS
// ═══════════════════════════════════════════════════════════════════

static void canvas_clear(){
    for(int y=0;y<CANVAS_H;y++)
        for(int x=0;x<CANVAS_W;x++)
            { canvas[y][x].ch=' '; canvas[y][x].fg=7; canvas[y][x].bg=0; }
}
static void save_undo(){
    for(int y=0;y<CANVAS_H;y++)
        for(int x=0;x<CANVAS_W;x++)
            undo_canvas[y][x]=canvas[y][x];
    has_undo=1;
}
static void do_undo(){
    if(!has_undo){set_status("Nothing to undo!");return;}
    for(int y=0;y<CANVAS_H;y++)
        for(int x=0;x<CANVAS_W;x++)
            canvas[y][x]=undo_canvas[y][x];
    set_status("Undone!");
}
static void paint_cell(int x,int y){
    if(x<0||x>=CANVAS_W||y<0||y>=CANVAS_H)return;
    canvas[y][x].ch=cur_char; canvas[y][x].fg=cur_fg; canvas[y][x].bg=cur_bg;
}
static void erase_cell(int x,int y){
    if(x<0||x>=CANVAS_W||y<0||y>=CANVAS_H)return;
    canvas[y][x].ch=' '; canvas[y][x].fg=7; canvas[y][x].bg=0;
}

#define FILL_STACK 600
static void flood_fill(int sx,int sy,char och,uint8_t ofg,uint8_t obg){
    if(sx<0||sx>=CANVAS_W||sy<0||sy>=CANVAS_H)return;
    if(canvas[sy][sx].ch==cur_char&&
       canvas[sy][sx].fg==cur_fg&&
       canvas[sy][sx].bg==cur_bg)return;
    int stx[FILL_STACK],sty[FILL_STACK],top=0;
    stx[top]=sx;sty[top]=sy;top++;
    while(top>0){
        top--;int x=stx[top],y=sty[top];
        if(x<0||x>=CANVAS_W||y<0||y>=CANVAS_H)continue;
        if(canvas[y][x].ch!=och||canvas[y][x].fg!=ofg||canvas[y][x].bg!=obg)continue;
        canvas[y][x].ch=cur_char;canvas[y][x].fg=cur_fg;canvas[y][x].bg=cur_bg;
        if(top+4<FILL_STACK){
            stx[top]=x+1;sty[top]=y;top++;
            stx[top]=x-1;sty[top]=y;top++;
            stx[top]=x;  sty[top]=y+1;top++;
            stx[top]=x;  sty[top]=y-1;top++;
        }
    }
}
static void draw_line(int x0,int y0,int x1,int y1){
    int dx=x1-x0;if(dx<0)dx=-dx;
    int dy=y1-y0;if(dy<0)dy=-dy;
    int sx=(x0<x1)?1:-1,sy=(y0<y1)?1:-1,err=dx-dy;
    while(1){
        paint_cell(x0,y0);
        if(x0==x1&&y0==y1)break;
        int e2=2*err;
        if(e2>-dy){err-=dy;x0+=sx;}
        if(e2< dx){err+=dx;y0+=sy;}
    }
}
static void draw_rect_outline(int x0,int y0,int x1,int y1){
    if(x0>x1){int t=x0;x0=x1;x1=t;}
    if(y0>y1){int t=y0;y0=y1;y1=t;}
    for(int x=x0;x<=x1;x++){paint_cell(x,y0);paint_cell(x,y1);}
    for(int y=y0;y<=y1;y++){paint_cell(x0,y);paint_cell(x1,y);}
}
static void draw_rect_fill(int x0,int y0,int x1,int y1){
    if(x0>x1){int t=x0;x0=x1;x1=t;}
    if(y0>y1){int t=y0;y0=y1;y1=t;}
    for(int y=y0;y<=y1;y++)for(int x=x0;x<=x1;x++)paint_cell(x,y);
}
static void draw_circle_outline(int cx,int cy,int r){
    int x=0,y=r,d=1-r;
    while(x<=y){
        paint_cell(cx+x,cy+y);paint_cell(cx-x,cy+y);
        paint_cell(cx+x,cy-y);paint_cell(cx-x,cy-y);
        paint_cell(cx+y,cy+x);paint_cell(cx-y,cy+x);
        paint_cell(cx+y,cy-x);paint_cell(cx-y,cy-x);
        if(d<0)d+=2*x+3;else{d+=2*(x-y)+5;y--;}x++;
    }
}

// ═══════════════════════════════════════════════════════════════════
//  VGA FALLBACK
// ═══════════════════════════════════════════════════════════════════

static inline uint16_t ve(char c,uint8_t fg,uint8_t bg){
    return (uint16_t)c|(uint16_t)(fg|bg<<4)<<8;
}
static void vp(int x,int y,char c,uint8_t fg,uint8_t bg){
    if(x<0||x>=VGA_WIDTH||y<0||y>=VGA_HEIGHT)return;
    VGA_MEM[y*VGA_WIDTH+x]=ve(c,fg,bg);
}
static void vs2(int x,int y,const char*s,uint8_t fg,uint8_t bg){
    for(int i=0;s[i];i++)vp(x+i,y,s[i],fg,bg);
}
static void vf2(int x,int y,int w,int h,char c,uint8_t fg,uint8_t bg){
    for(int dy=0;dy<h;dy++)for(int dx=0;dx<w;dx++)vp(x+dx,y+dy,c,fg,bg);
}

static void vga_draw_canvas(){
    for(int y=0;y<CANVAS_H;y++)
        for(int x=0;x<CANVAS_W;x++){
            Cell*c=&canvas[y][x];
            if(x==cur_x&&y==cur_y) vp(x,2+y,c->ch,0,15);
            else                    vp(x,2+y,c->ch,c->fg,c->bg);
        }
    if(tool_active)vp(start_x,2+start_y,'+',12,0);
}
static void vga_draw_panel(){
    int px=67;
    for(int y=0;y<VGA_HEIGHT;y++)vp(px-1,y,'|',8,0);
    vs2(px,0,"[TOOLS]",11,0);
    for(int i=0;i<TOOL_COUNT;i++){
        uint8_t fg=(i==cur_tool)?0:7,bg=(i==cur_tool)?10:0;
        vp(px,1+i,tool_icons[i],fg,bg);vp(px+1,1+i,' ',fg,bg);
        vs2(px+2,1+i,tool_names[i],fg,bg);
    }
    vs2(px,9,"FG:",11,0);vp(px+3,9,' ',cur_fg,cur_fg);
    vs2(px,10,"BG:",11,0);vp(px+3,10,' ',cur_bg,cur_bg);
    for(int i=0;i<8;i++){vp(px+i,11,' ',(uint8_t)i,(uint8_t)i);vp(px+i,12,' ',(uint8_t)(i+8),(uint8_t)(i+8));}
    vs2(px,13,"Char:",11,0);vp(px+6,13,cur_char,cur_fg,cur_bg);
    char pos[8];pos[0]='0'+cur_x/10;pos[1]='0'+cur_x%10;pos[2]=',';
    pos[3]='0'+cur_y/10;pos[4]='0'+cur_y%10;pos[5]='\0';
    vs2(px,14,"Pos:",8,0);vs2(px+4,14,pos,14,0);
    vs2(px,15,"Z:undo",7,0);vs2(px,16,"C:clear",7,0);vs2(px,17,"S:save",7,0);
}

// ═══════════════════════════════════════════════════════════════════
//  VESA PIXEL UI
// ═══════════════════════════════════════════════════════════════════

// ── Palette ───────────────────────────────────────────────────────
#define PT_BG     0x0E0C22
#define PT_TITLE  0x1A1440
#define PT_PANEL  0x14122A
#define PT_BORDER 0x2B2448
#define PT_TEXT   0xE0D8FF
#define PT_DIM    0x6858A0
#define PT_ACCENT 0x9B7FFF
#define PT_SEL    0x352878
#define PT_DANGER 0xFF6655
#define PT_OK     0x55EE88
#define PT_HINT   0x504870

// Each canvas cell renders as a CELL_PX × CELL_PX colored square
// Computed at draw time from available space
static int CELL_PX = 10;

static uint32_t pt_mix(uint32_t c,int d){
    int r=(int)(c>>16&0xFF)+d;if(r<0)r=0;if(r>255)r=255;
    int g=(int)(c>>8 &0xFF)+d;if(g<0)g=0;if(g>255)g=255;
    int b=(int)(c    &0xFF)+d;if(b<0)b=0;if(b>255)b=255;
    return((uint32_t)r<<16)|((uint32_t)g<<8)|(uint32_t)b;
}
static void pt_pill(int x,int y,const char*s,uint32_t fg,uint32_t bg){
    int w=p_len(s)*8+16;
    vesa_fill_rect(x,y,w,22,bg);
    vesa_draw_rect(x,y,w,22,fg);
    vesa_draw_string(x+8,y+3,s,fg,bg);
}

// ── VESA canvas ───────────────────────────────────────────────────
static void vesa_draw_canvas(int ox,int oy){
    int W=(int)vesa.width;
    for(int y=0;y<CANVAS_H;y++){
        for(int x=0;x<CANVAS_W;x++){
            Cell*c=&canvas[y][x];
            int px=ox+x*CELL_PX, py=oy+y*CELL_PX;
            uint32_t bg_col = vga_pal[c->bg];
            uint32_t fg_col = vga_pal[c->fg];

            // Cursor highlight
            if(x==cur_x&&y==cur_y){
                bg_col=0xFFFFFF; fg_col=0x000000;
            }
            // Tool start-point marker
            if(tool_active&&x==start_x&&y==start_y){
                bg_col=0xFF3030; fg_col=0xFFFFFF;
            }

            vesa_fill_rect(px,py,CELL_PX,CELL_PX,bg_col);

            // Draw the character inside the cell (only if CELL_PX >= 8)
            if(CELL_PX>=8 && c->ch!=' '){
                // Scale 1 when cell is exactly 8px, else let it clip
                vesa_draw_char(px+(CELL_PX-8)/2, py+(CELL_PX-8)/2,
                               c->ch, fg_col, bg_col);
            }
        }
    }
    // Grid lines (subtle, only when cells are large enough)
    if(CELL_PX>=8){
        uint32_t gl=0x222040;
        for(int x=0;x<=CANVAS_W;x++)
            vesa_vline(ox+x*CELL_PX,oy,CANVAS_H*CELL_PX,gl);
        for(int y=0;y<=CANVAS_H;y++)
            vesa_hline(ox,oy+y*CELL_PX,CANVAS_W*CELL_PX,gl);
    }
    // Canvas border
    vesa_draw_rect(ox-1,oy-1,CANVAS_W*CELL_PX+2,CANVAS_H*CELL_PX+2,PT_ACCENT);
    (void)W;
}

// ── VESA side panel ───────────────────────────────────────────────
static void vesa_draw_panel(int px,int py,int pw,int ph){
    vesa_fill_rect(px,py,pw,ph,PT_PANEL);
    vesa_draw_rect(px,py,pw,ph,PT_BORDER);
    int y=py+8;

    // ── Tools ──────────────────────────────────────────────────
    vesa_draw_string(px+8,y,"TOOLS",PT_DIM,PT_PANEL); y+=18;
    vesa_hline(px+4,y,pw-8,PT_BORDER); y+=6;

    int tool_row_h=(pw-4)/2;
    for(int i=0;i<TOOL_COUNT;i++){
        int col=i%2, row=i/2;
        int bx=px+4+col*((pw-8)/2);
        int by=y+row*26;
        int sel=(i==cur_tool);
        uint32_t tbg=sel?PT_SEL:PT_PANEL;
        uint32_t tfg=sel?PT_TEXT:PT_DIM;
        vesa_fill_rect(bx,by,(pw-12)/2,22,tbg);
        vesa_draw_rect(bx,by,(pw-12)/2,22,sel?PT_ACCENT:PT_BORDER);
        vesa_draw_string(bx+4,by+3,tool_names[i],tfg,tbg);
    }
    y+=(TOOL_COUNT/2)*26+6;
    (void)tool_row_h;

    // ── Colors ─────────────────────────────────────────────────
    vesa_hline(px+4,y,pw-8,PT_BORDER); y+=6;
    vesa_draw_string(px+8,y,"COLORS",PT_DIM,PT_PANEL); y+=18;

    int sw=(pw-12)/8;  // swatch width
    for(int i=0;i<16;i++){
        int col=i%8, row=i/8;
        int bx=px+4+col*sw;
        int by=y+row*(sw+2);
        uint32_t col2=vga_pal[i];
        vesa_fill_rect(bx,by,sw-1,sw-1,col2);
        if(i==cur_fg)
            vesa_draw_rect(bx-1,by-1,sw+1,sw+1,0xFFFFFF);
        else if(i==cur_bg)
            vesa_draw_rect(bx-1,by-1,sw+1,sw+1,0xAAAAAA);
    }
    y+=(sw+2)*2+8;

    // FG/BG labels
    char fgs[4],bgs[4]; p_itoa2(cur_fg,fgs); p_itoa2(cur_bg,bgs);
    vesa_draw_string(px+4,y,"FG",PT_DIM,PT_PANEL);
    vesa_fill_rect(px+24,y,16,14,vga_pal[cur_fg]);
    vesa_draw_string(px+4,y+16,"BG",PT_DIM,PT_PANEL);
    vesa_fill_rect(px+24,y+16,16,14,vga_pal[cur_bg]);
    y+=34;

    // ── Brush chars ─────────────────────────────────────────────
    vesa_hline(px+4,y,pw-8,PT_BORDER); y+=6;
    vesa_draw_string(px+8,y,"BRUSH",PT_DIM,PT_PANEL); y+=18;
    int bchars_per_row=(pw-12)/14;
    for(int i=0;i<(int)p_len(brush_chars)&&i<16;i++){
        int col=i%bchars_per_row, row=i/bchars_per_row;
        int bx=px+4+col*14, by2=y+row*18;
        uint32_t chbg=(i==cur_brush_idx)?PT_SEL:PT_PANEL;
        uint32_t chfg=(i==cur_brush_idx)?PT_TEXT:PT_DIM;
        vesa_fill_rect(bx,by2,12,16,chbg);
        if(i==cur_brush_idx)vesa_draw_rect(bx,by2,12,16,PT_ACCENT);
        char tmp[2]; tmp[0]=brush_chars[i]; tmp[1]='\0';
        vesa_draw_string(bx+2,by2,tmp,chfg,chbg);
    }
    y+=(p_len(brush_chars)/bchars_per_row+1)*18+6;

    // ── Current char display ────────────────────────────────────
    vesa_hline(px+4,y,pw-8,PT_BORDER); y+=6;
    vesa_draw_string(px+8,y,"CHAR",PT_DIM,PT_PANEL); y+=18;
    vesa_fill_rect(px+4,y,28,28,vga_pal[cur_bg]);
    vesa_draw_rect(px+4,y,28,28,PT_ACCENT);
    if(CELL_PX>=8)
        vesa_draw_char_scaled(px+6,y+6,cur_char,vga_pal[cur_fg],vga_pal[cur_bg],2);
    y+=36;

    // ── Position ───────────────────────────────────────────────
    vesa_hline(px+4,y,pw-8,PT_BORDER); y+=6;
    char posx[4],posy[4]; p_itoa2(cur_x,posx); p_itoa2(cur_y,posy);
    char posstr[12]; int pp=0;
    posstr[pp++]='X'; posstr[pp++]=':';
    for(int i=0;posx[i];i++)posstr[pp++]=posx[i];
    posstr[pp++]=' '; posstr[pp++]='Y'; posstr[pp++]=':';
    for(int i=0;posy[i];i++)posstr[pp++]=posy[i];
    posstr[pp]='\0';
    vesa_draw_string(px+4,y,posstr,PT_DIM,PT_PANEL); y+=18;

    // ── Hints ─────────────────────────────────────────────────
    vesa_hline(px+4,y,pw-8,PT_BORDER); y+=6;
    const char*hints[]={"Z undo","C clear","S save","H help"};
    for(int i=0;i<4;i++){
        vesa_draw_string(px+4,y,hints[i],PT_DIM,PT_PANEL);
        y+=16;
    }
}

// ── VESA save dialog ─────────────────────────────────────────────
static void vesa_draw_save(int W,int H){
    int dw=360,dh=140;
    int dx=(W-dw)/2,dy=(H-dh)/2;
    vesa_fill_rect(dx,dy,dw,dh,0x12102A);
    vesa_draw_rect(dx,dy,dw,dh,PT_ACCENT);
    vesa_fill_rect(dx,dy,dw,3,PT_ACCENT);
    vesa_draw_string(dx+16,dy+12,"Save File",PT_TEXT,0x12102A);
    vesa_draw_string(dx+16,dy+36,"Filename:",PT_DIM,0x12102A);
    // Input field
    int fx=dx+16,fy=dy+54,fw=dw-32;
    uint32_t fbg=0x0E0C20;
    vesa_fill_rect(fx,fy,fw,30,fbg);
    vesa_draw_rect(fx,fy,fw,30,PT_ACCENT);
    vesa_draw_string(fx+8,fy+7,save_name,PT_TEXT,fbg);
    // Cursor
    vesa_draw_string(fx+8+save_pos*8,fy+7,"_",PT_ACCENT,fbg);
    pt_pill(dx+16,  dy+96,"ENTER save",  PT_OK,   0x0A280E);
    pt_pill(dx+160, dy+96,"ESC cancel",  PT_DIM,  0x0E0C1C);
}

// ── Main VESA paint_draw ──────────────────────────────────────────
static void vesa_paint_draw(){
    int W=(int)vesa.width, H=(int)vesa.height;

    // Fullscreen window (no margin — maximum canvas space)
    int wx=0, wy=0, ww=W, wh=H;
    g_win_x = wx; g_win_y = wy; g_win_w = ww; g_win_h = wh;

    // Title bar  (32px)
    int title_h=32;
    vesa_fill_rect(wx,wy,ww,title_h,PT_TITLE);
    vesa_draw_string(wx+12,wy+8,"Nyx Paint",PT_TEXT,PT_TITLE);
    draw_close_btn(wx+ww-40, wy+8, PT_TITLE, "Paint");
    pt_pill(wx+104,wy+5,tool_names[cur_tool],PT_ACCENT,0x1A0848);
    if(text_mode) pt_pill(wx+240,wy+5,"TEXT MODE",PT_OK,0x0A280E);

    // Bottom toolbar  (44px) — color swatches + tool buttons in a strip
    int bar_h=44, bar_y=wh-bar_h;
    vesa_fill_rect(wx,bar_y,ww,bar_h,PT_TITLE);
    vesa_hline(wx,bar_y,ww,PT_BORDER);

    // Tools in bottom bar (left side)
    int bx=8;
    for(int i=0;i<TOOL_COUNT;i++){
        int tw=p_len(tool_names[i])*8+12;
        int is_cur=(i==cur_tool);
        int is_hover=!is_cur && mouse_in_rect(bx,bar_y+4,tw,32);
        uint32_t tbg=is_cur?PT_SEL:(is_hover?0x282060:PT_TITLE);
        uint32_t tfg=is_cur?PT_TEXT:(is_hover?0xE0D8FF:PT_DIM);
        vesa_fill_rect(bx,bar_y+4,tw,32,tbg);
        if(is_cur){vesa_draw_rect(bx,bar_y+4,tw,32,PT_ACCENT);}
        else if(is_hover){vesa_draw_rect(bx,bar_y+4,tw,32,0x9B7FFF);}
        vesa_draw_string(bx+6,bar_y+12,tool_names[i],tfg,tbg);
        bx+=tw+4;
    }
    bx+=8;

    // Separator
    vesa_vline(bx,bar_y+4,32,PT_BORDER); bx+=8;

    // 16-color swatches in bottom bar
    int swatch=22;
    for(int i=0;i<16;i++){
        int sx=bx+i*(swatch+2), sy=bar_y+4;
        vesa_fill_rect(sx,sy,swatch,swatch,vga_pal[i]);
        if(i==cur_fg) vesa_draw_rect(sx-1,sy-1,swatch+2,swatch+2,0xFFFFFF);
        else if(i==cur_bg) vesa_draw_rect(sx-1,sy-1,swatch+2,swatch+2,0x888888);
    }
    bx+=16*(swatch+2)+8;

    // Separator + current char preview
    vesa_vline(bx,bar_y+4,32,PT_BORDER); bx+=8;
    vesa_fill_rect(bx,bar_y+4,32,32,vga_pal[cur_bg]);
    vesa_draw_rect(bx,bar_y+4,32,32,PT_ACCENT);
    vesa_draw_char_scaled(bx+8,bar_y+8,cur_char,vga_pal[cur_fg],vga_pal[cur_bg],2);
    bx+=40;

    // Brush chars
    vesa_vline(bx,bar_y+4,32,PT_BORDER); bx+=8;
    int blen=p_len(brush_chars); if(blen>20)blen=20;
    for(int i=0;i<blen;i++){
        uint32_t cbg=(i==cur_brush_idx)?PT_SEL:PT_TITLE;
        uint32_t cfg=(i==cur_brush_idx)?PT_TEXT:PT_DIM;
        vesa_fill_rect(bx+i*14,bar_y+6,12,28,cbg);
        char tmp[2]; tmp[0]=brush_chars[i]; tmp[1]=0;
        vesa_draw_string(bx+i*14+2,bar_y+12,tmp,cfg,cbg);
    }
    bx+=blen*14+8;

    // Status / hint at right end
    int hint_x=bx+8;
    if(status_timer>0){
        vesa_draw_string(hint_x,bar_y+14,status_msg,PT_OK,PT_TITLE);
        status_timer--;
    } else {
        vesa_draw_string(hint_x,bar_y+14,
            "Arrows:move  Space:draw  Tab:tool  F/B:color  []:char  Z:undo  S:save",
            PT_HINT,PT_TITLE);
    }

    // Canvas fills everything between title bar and bottom bar
    int content_y = title_h;
    int content_h = bar_y - title_h;

    // Cell size: fit canvas into full available area
    CELL_PX = (ww-4) / CANVAS_W;
    int cell_from_h = content_h / CANVAS_H;
    if(cell_from_h < CELL_PX) CELL_PX = cell_from_h;
    if(CELL_PX < 4)  CELL_PX = 4;
    if(CELL_PX > 24) CELL_PX = 24;

    int canvas_px_w = CANVAS_W * CELL_PX;
    int canvas_px_h = CANVAS_H * CELL_PX;
    int canvas_ox = (ww - canvas_px_w) / 2;
    int canvas_oy = content_y + (content_h - canvas_px_h) / 2;

    // Fill canvas background
    vesa_fill_rect(0,content_y,W,content_h,0x080814);

    vesa_draw_canvas(canvas_ox, canvas_oy);

    // Save dialog on top
    if(show_save) vesa_draw_save(W,H);
}

// ═══════════════════════════════════════════════════════════════════
//  PUBLIC API
// ═══════════════════════════════════════════════════════════════════

void paint_draw(){
    if(vesa.active){ vesa_paint_draw(); vesa_present(); return; }

    // ── VGA fallback ──────────────────────────────────────────────
    vf2(0,0,VGA_WIDTH,VGA_HEIGHT,' ',7,0);
    // Titlebar
    vf2(0,0,VGA_WIDTH,1,' ',0,1);
    vs2(2,0,"[ Nyx Paint ]",15,1);
    vs2(17,0,"Arrows:move  Space:draw  Tab:tool  F/B:color  ESC:back",7,1);
    if(status_timer>0){vs2(2,0,status_msg,12,1);status_timer--;}
    // Canvas border row
    vf2(0,1,VGA_WIDTH,1,'-',8,0);
    vga_draw_canvas();
    vga_draw_panel();
    if(show_save){
        vf2(20,10,40,5,' ',15,1);
        vs2(22,10,"+----------------------------------+",14,1);
        vs2(22,11,"|  Save as:                        |",14,1);
        vf2(24,12,32,1,' ',0,7);vs2(24,12,save_name,0,7);vp(24+save_pos,12,' ',0,15);
        vs2(22,13,"|  ENTER:save  ESC:cancel          |",14,1);
        vs2(22,14,"+----------------------------------+",14,1);
    }
    if(text_mode) vs2(2,0,"-- TEXT MODE -- Type to draw, ESC exit",12,1);
}

void paint_handle_key(char c){

    // ── Save dialog ───────────────────────────────────────────────
    if(show_save){
        if(c==27){show_save=0;paint_draw();return;}
        if(c==13){
            if(save_pos>0){
                char content[16384]; int pos2=0;  // 16 KiB local buf (canvas << 8 MiB)
                for(int y=0;y<CANVAS_H&&pos2<MAX_FILESIZE-2;y++){
                    int last=0;
                    for(int x=CANVAS_W-1;x>=0;x--)if(canvas[y][x].ch!=' '){last=x+1;break;}
                    for(int x=0;x<last&&pos2<MAX_FILESIZE-2;x++)content[pos2++]=canvas[y][x].ch;
                    content[pos2++]='\n';
                }
                content[pos2]='\0';
                int idx=-1;
                for(int i=0;i<MAX_FILES;i++){
                    if(filesystem[i].used){
                        int m=1;
                        for(int j=0;save_name[j]||filesystem[i].name[j];j++)
                            if(save_name[j]!=filesystem[i].name[j]){m=0;break;}
                        if(m){idx=i;break;}
                    }
                }
                if(idx==-1)for(int i=0;i<MAX_FILES;i++)if(!filesystem[i].used){idx=i;break;}
                if(idx>=0){
                    filesystem[idx].used=1;
                    p_cpy(filesystem[idx].name,   save_name, MAX_FILENAME);
                    file_content_alloc(idx, pos2 + 1);
                    if(filesystem[idx].content) {
                        p_cpy(filesystem[idx].content, content, pos2 + 1);
                    }
                    filesystem[idx].size = pos2;
                    file_count++;
                    set_status("Saved!");
                } else {
                    set_status("Filesystem full!");
                }
            }
            show_save=0;paint_draw();return;
        }
        if(c==8&&save_pos>0){save_name[--save_pos]='\0';paint_draw();return;}
        if(c>=32&&c<127&&save_pos<30){save_name[save_pos++]=c;save_name[save_pos]='\0';paint_draw();return;}
        return;
    }

    // ── Text mode ─────────────────────────────────────────────────
    if(text_mode){
        if(c==27){text_mode=0;paint_draw();return;}
        if(c==13){text_y++;text_x=0;if(text_y>=CANVAS_H)text_y=CANVAS_H-1;paint_draw();return;}
        if(c==8){if(text_x>0){text_x--;erase_cell(text_x,text_y);}paint_draw();return;}
        if(c>=32&&c<127){
            canvas[text_y][text_x].ch=c;
            canvas[text_y][text_x].fg=cur_fg;
            canvas[text_y][text_x].bg=cur_bg;
            text_x++;if(text_x>=CANVAS_W){text_x=0;text_y++;if(text_y>=CANVAS_H)text_y=CANVAS_H-1;}
            paint_draw();return;
        }
        return;
    }

    // ── Movement ──────────────────────────────────────────────────
    int moved=0;
    if(c=='w'||c=='W'||c=='A'){if(cur_y>0)cur_y--;moved=1;}
    else if(c=='s'||c=='S'||c=='B'){if(cur_y<CANVAS_H-1)cur_y++;moved=1;}
    else if(c=='a'||c=='D'){if(cur_x>0)cur_x--;moved=1;}
    else if(c=='d'||c=='C'){if(cur_x<CANVAS_W-1)cur_x++;moved=1;}
    if(moved){
        if(is_drawing&&cur_tool==TOOL_DRAW)  paint_cell(cur_x,cur_y);
        if(is_drawing&&cur_tool==TOOL_ERASE) erase_cell(cur_x,cur_y);
        paint_draw();return;
    }

    // ── Space = action ────────────────────────────────────────────
    if(c==' '){
        save_undo();
        switch(cur_tool){
            case TOOL_DRAW:
                paint_cell(cur_x,cur_y);is_drawing=!is_drawing;break;
            case TOOL_ERASE:
                erase_cell(cur_x,cur_y);break;
            case TOOL_FILL:
                flood_fill(cur_x,cur_y,canvas[cur_y][cur_x].ch,
                           canvas[cur_y][cur_x].fg,canvas[cur_y][cur_x].bg);
                set_status("Filled!");break;
            case TOOL_LINE:
                if(!tool_active){start_x=cur_x;start_y=cur_y;tool_active=1;set_status("Line: press Space at end");}
                else{draw_line(start_x,start_y,cur_x,cur_y);tool_active=0;set_status("Line drawn!");}break;
            case TOOL_RECT:
                if(!tool_active){start_x=cur_x;start_y=cur_y;tool_active=1;set_status("Rect: press Space at corner");}
                else{draw_rect_outline(start_x,start_y,cur_x,cur_y);tool_active=0;set_status("Rect drawn!");}break;
            case TOOL_CIRCLE:
                if(!tool_active){start_x=cur_x;start_y=cur_y;tool_active=1;set_status("Circle: move to set radius");}
                else{
                    int dx=cur_x-start_x,dy=cur_y-start_y;
                    int r2=dx*dx+dy*dy,rad=0;
                    while((rad+1)*(rad+1)<=r2)rad++;
                    draw_circle_outline(start_x,start_y,rad);
                    tool_active=0;set_status("Circle drawn!");
                }break;
            case TOOL_TEXT:
                text_mode=1;text_x=cur_x;text_y=cur_y;set_status("Text mode: type to draw");break;
            case TOOL_PICK:
                cur_char=canvas[cur_y][cur_x].ch;
                cur_fg=canvas[cur_y][cur_x].fg;
                cur_bg=canvas[cur_y][cur_x].bg;
                set_status("Picked!");break;
        }
        paint_draw();return;
    }

    // ── Tab = next tool ───────────────────────────────────────────
    if(c=='\t'){
        cur_tool=(cur_tool+1)%TOOL_COUNT;tool_active=0;is_drawing=0;
        set_status(tool_names[cur_tool]);paint_draw();return;
    }
    if(c=='q'||c=='Q'){
        cur_tool=(cur_tool+TOOL_COUNT-1)%TOOL_COUNT;tool_active=0;is_drawing=0;
        set_status(tool_names[cur_tool]);paint_draw();return;
    }

    // 1-8 direct tool select
    if(c>='1'&&c<='8'){cur_tool=c-'1';tool_active=0;is_drawing=0;set_status(tool_names[cur_tool]);paint_draw();return;}

    // FG/BG color
    if(c=='f'||c=='F'){cur_fg=(cur_fg+1)%16;paint_draw();return;}
    if(c=='g'||c=='G'){cur_fg=(cur_fg+15)%16;paint_draw();return;}
    if(c=='b'){cur_bg=(cur_bg+1)%16;paint_draw();return;}
    if(c=='v'||c=='V'){cur_bg=(cur_bg+15)%16;paint_draw();return;}

    // Brush char
    if(c=='['){cur_brush_idx=(cur_brush_idx+p_len(brush_chars)-1)%p_len(brush_chars);cur_char=brush_chars[cur_brush_idx];paint_draw();return;}
    if(c==']'){cur_brush_idx=(cur_brush_idx+1)%p_len(brush_chars);cur_char=brush_chars[cur_brush_idx];paint_draw();return;}

    // Undo
    if(c=='z'||c=='Z'){do_undo();paint_draw();return;}

    // Clear
    if(c=='c'||c=='C'){save_undo();canvas_clear();set_status("Canvas cleared!");paint_draw();return;}

    // Filled rect shortcut
    if(c=='r'||c=='R'){
        if(!tool_active){cur_tool=TOOL_RECT;start_x=cur_x;start_y=cur_y;tool_active=1;set_status("Fill rect: move to corner");}
        else{save_undo();draw_rect_fill(start_x,start_y,cur_x,cur_y);tool_active=0;set_status("Filled rect drawn!");}
        paint_draw();return;
    }

    // Save  (Ctrl+S = 19)
    if(c=='s'||c=='S'||c==19){
        show_save=1;save_pos=0;save_name[0]='\0';paint_draw();return;
    }
}

void paint_init(){
    canvas_clear();
    cur_x=CANVAS_W/2;cur_y=CANVAS_H/2;
    cur_tool=TOOL_DRAW;cur_fg=15;cur_bg=0;
    cur_char='*';cur_brush_idx=0;
    is_drawing=0;tool_active=0;text_mode=0;
    show_save=0;has_undo=0;
    status_msg[0]='\0';status_timer=0;
}

// ── Mouse click handler ────────────────────────────────────────────
// Handles tool selection, color swatch clicks, brush char selection,
// and canvas cell drawing. Returns 1 if the click was handled.
int paint_handle_mouse(int mx, int my) {
    if (!vesa.active) return 0;
    int W = (int)vesa.width, H = (int)vesa.height;

    // ── Bottom toolbar (44px tall at H-44) ──────────────────────
    int bar_h = 44, bar_y = H - bar_h;
    if (my >= bar_y && my < bar_y + bar_h) {
        // ── Tool buttons (left side of bottom bar) ──────────────
        int bx = 8;
        for (int i = 0; i < TOOL_COUNT; i++) {
            int tw = p_len(tool_names[i]) * 8 + 12;
            if (mx >= bx && mx < bx + tw && my >= bar_y + 4 && my < bar_y + 36) {
                cur_tool = i;
                tool_active = 0;
                is_drawing = 0;
                set_status(tool_names[i]);
                return 1;
            }
            bx += tw + 4;
        }
        bx += 8;

        // Skip separator line
        bx += 8;

        // ── 16 color swatches ───────────────────────────────────
        int swatch = 22;
        for (int i = 0; i < 16; i++) {
            int sx = bx + i * (swatch + 2), sy = bar_y + 4;
            if (mx >= sx && mx < sx + swatch && my >= sy && my < sy + swatch) {
                cur_fg = (uint8_t)i;
                return 1;
            }
        }
        bx += 16 * (swatch + 2) + 8;

        // Skip separator
        bx += 8;

        // Skip char preview (32px)
        bx += 40;

        // Skip separator
        bx += 8;

        // ── Brush char selection ────────────────────────────────
        int blen = p_len(brush_chars);
        if (blen > 20) blen = 20;
        for (int i = 0; i < blen; i++) {
            int sx2 = bx + i * 14;
            if (mx >= sx2 && mx < sx2 + 12 && my >= bar_y + 6 && my < bar_y + 34) {
                cur_brush_idx = i;
                cur_char = brush_chars[i];
                return 1;
            }
        }

        return 0;
    }

    // ── Canvas area (between title bar and bottom bar) ──────────
    int title_h = 32;
    int content_y = title_h;
    int content_h = bar_y - title_h;

    // Compute cell size (same as vesa_paint_draw)
    int CELL_PX_local = (W - 4) / CANVAS_W;
    int cell_from_h = content_h / CANVAS_H;
    if (cell_from_h < CELL_PX_local) CELL_PX_local = cell_from_h;
    if (CELL_PX_local < 4)  CELL_PX_local = 4;
    if (CELL_PX_local > 24) CELL_PX_local = 24;

    int canvas_px_w = CANVAS_W * CELL_PX_local;
    int canvas_px_h = CANVAS_H * CELL_PX_local;
    int canvas_ox = (W - canvas_px_w) / 2;
    int canvas_oy = content_y + (content_h - canvas_px_h) / 2;

    // Check if click is within the canvas area
    if (mx >= canvas_ox && mx < canvas_ox + canvas_px_w &&
        my >= canvas_oy && my < canvas_oy + canvas_px_h) {
        int cell_x = (mx - canvas_ox) / CELL_PX_local;
        int cell_y = (my - canvas_oy) / CELL_PX_local;

        if (cell_x < 0) cell_x = 0;
        if (cell_x >= CANVAS_W) cell_x = CANVAS_W - 1;
        if (cell_y < 0) cell_y = 0;
        if (cell_y >= CANVAS_H) cell_y = CANVAS_H - 1;

        // Move cursor to clicked cell
        cur_x = cell_x;
        cur_y = cell_y;

        save_undo();

        // Execute the current tool at the clicked position
        switch (cur_tool) {
            case TOOL_DRAW:
                paint_cell(cur_x, cur_y);
                break;
            case TOOL_ERASE:
                erase_cell(cur_x, cur_y);
                break;
            case TOOL_FILL:
                flood_fill(cur_x, cur_y,
                           canvas[cur_y][cur_x].ch,
                           canvas[cur_y][cur_x].fg,
                           canvas[cur_y][cur_x].bg);
                set_status("Filled!");
                break;
            case TOOL_LINE:
                if (!tool_active) {
                    start_x = cur_x; start_y = cur_y;
                    tool_active = 1;
                } else {
                    draw_line(start_x, start_y, cur_x, cur_y);
                    tool_active = 0;
                }
                break;
            case TOOL_RECT:
                if (!tool_active) {
                    start_x = cur_x; start_y = cur_y;
                    tool_active = 1;
                } else {
                    draw_rect_outline(start_x, start_y, cur_x, cur_y);
                    tool_active = 0;
                }
                break;
            case TOOL_CIRCLE:
                if (!tool_active) {
                    start_x = cur_x; start_y = cur_y;
                    tool_active = 1;
                } else {
                    int dx = cur_x - start_x, dy = cur_y - start_y;
                    int r2 = dx*dx + dy*dy, rad = 0;
                    while ((rad+1)*(rad+1) <= r2) rad++;
                    draw_circle_outline(start_x, start_y, rad);
                    tool_active = 0;
                }
                break;
            case TOOL_PICK:
                cur_char = canvas[cur_y][cur_x].ch;
                cur_fg = canvas[cur_y][cur_x].fg;
                cur_bg = canvas[cur_y][cur_x].bg;
                set_status("Picked!");
                break;
            default:
                break;
        }
        return 1;
    }

    return 0;
}

// ─── Drag-and-drop handler: import a dropped file into the canvas ──
// Reads the file's text content and stamps it onto the canvas cells
// starting from the cursor position, wrapping at the canvas width.
void paint_drop_file(int file_idx) {
    if (file_idx < 0 || file_idx >= MAX_FILES) return;
    if (!filesystem[file_idx].used) return;

    save_undo();

    const char* content = filesystem[file_idx].content;
    int x = cur_x;
    int y = cur_y;

    for (int i = 0; content[i] && y < CANVAS_H; i++) {
        char c = content[i];
        if (c == '\n') {
            x = cur_x;  // reset to original X (indent)
            y++;
            continue;
        }
        if (c < 32) continue;  // skip non-printable

        canvas[y][x].ch = c;
        canvas[y][x].fg = cur_fg;
        canvas[y][x].bg = cur_bg;

        x++;
        if (x >= CANVAS_W) {
            x = cur_x;
            y++;
        }
    }

    set_status("File dropped!");
    // Don't call paint_draw() here — the caller (desktop_draw()) will
    // present the frame once, avoiding a double-vesa_present flicker.
}