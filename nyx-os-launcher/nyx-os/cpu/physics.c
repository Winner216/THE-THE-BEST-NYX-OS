// cpu/physics.c – Nyx OS 2D Physics Sandbox (VGA cell-space + VESA pixel-space)
// Fixed-point math throughout — no floating point in a freestanding kernel.
// Fixed point: 1 unit = 256 (8 bits of fraction), same FP scale used by
// both simulations below so the tuning constants stay comparable.
#include <physics.h>
#include <vesa.h>
#include <speaker.h>
#include <stdint.h>
#include <desktop.h>

// ─── Fixed point ──────────────────────────────────────────────────
#define FP      256   // 1.0 in fixed point
#define FP_MUL(a,b) (((a)*(b))/FP)

typedef enum { OBJ_BALL, OBJ_BOX, OBJ_TRIANGLE } ObjType;

// Integer square root (Newton's method) — used by the VESA sim's
// real circle-circle distance collision. No sqrt() in freestanding
// libc, so this is a small hand-rolled version.
static int isqrt(int n){
    if(n<=0) return 0;
    int x=n, y=(x+1)/2;
    while(y<x){ x=y; y=(x+n/x)/2; }
    return x;
}

static int phys_rand = 98765;
static int prand(){ phys_rand=phys_rand*1103515245+12345; return (phys_rand>>16)&0x7FFF; }

static void n2s_p(int n, char* b) {
    if(n<0){b[0]='-';n=-n;int p=1;char t[12];int tp=0;
        while(n>0){t[tp++]='0'+n%10;n/=10;}
        for(int i=0;i<tp;i++)b[p+i]=t[tp-1-i];b[p+tp]='\0';return;}
    if(n==0){b[0]='0';b[1]='\0';return;}
    char t[12];int p=0;
    while(n>0){t[p++]='0'+n%10;n/=10;}
    for(int i=0;i<p;i++)b[i]=t[p-1-i];b[p]='\0';
}

static int phys_active = 0;

// ═══════════════════════════════════════════════════════════════════
//  VGA CELL-SPACE SIMULATION  (unchanged behavior — text mode fallback)
// ═══════════════════════════════════════════════════════════════════

#define VGA_WIDTH  80
#define VGA_HEIGHT 25
#define VGA_MEM    ((uint16_t*)0xB8000)

static inline uint16_t make_entry(char c, uint8_t fg, uint8_t bg) {
    return (uint16_t)c | (uint16_t)(fg | bg << 4) << 8;
}
static void pp(int x, int y, char c, uint8_t fg, uint8_t bg) {
    if(x<0||x>=VGA_WIDTH||y<0||y>=VGA_HEIGHT) return;
    VGA_MEM[y*VGA_WIDTH+x] = make_entry(c,fg,bg);
}
static void ps(int x, int y, const char* s, uint8_t fg, uint8_t bg) {
    for(int i=0;s[i];i++) pp(x+i,y,s[i],fg,bg);
}
static void pf(int x, int y, int w, int h, char c, uint8_t fg, uint8_t bg) {
    for(int dy=0;dy<h;dy++) for(int dx=0;dx<w;dx++) pp(x+dx,y+dy,c,fg,bg);
}
static void pfr(int y, char c, uint8_t fg, uint8_t bg) {
    for(int x=0;x<VGA_WIDTH;x++) pp(x,y,c,fg,bg);
}

#define WORLD_X   1
#define WORLD_Y   2
#define WORLD_W   54
#define WORLD_H   22
#define WORLD_R   (WORLD_X+WORLD_W-1)
#define WORLD_B   (WORLD_Y+WORLD_H-1)
#define WFX(x) ((x)*FP)
#define WFY(y) ((y)*FP)
#define MAX_OBJECTS 12

typedef struct {
    int      alive;
    ObjType  type;
    int      x, y, vx, vy;
    int      w, h;
    uint8_t  color;
    char     ch;
    int      trail_x[4], trail_y[4], trail_n;
} PhysObj;

static PhysObj objs[MAX_OBJECTS];
static int obj_count = 0;
static int paddle_x = WORLD_X + WORLD_W/2 - 5;
static int paddle_y = WORLD_B - 2;
static int paddle_w = 10;
static int gravity=2, friction=250, elasticity=200;
static int sim_running=1, sim_ticks=0, spawn_type=OBJ_BALL;
static int spawn_x=WORLD_X+WORLD_W/2, spawn_y=WORLD_Y+2;
static int show_help=0, show_trails=1;
static const uint8_t obj_colors[] = {12,10,14,11,13,9,6,15,4,2,3,5};
static const char    obj_chars[]  = {'O','#','^','@','*','0','Q','&','%','$','?','!'};
static int color_idx = 0;

static void spawn_object(int sx, int sy, ObjType type) {
    if(obj_count >= MAX_OBJECTS) {
        for(int i=0;i<MAX_OBJECTS-1;i++) objs[i]=objs[i+1];
        obj_count=MAX_OBJECTS-1;
    }
    PhysObj* o = &objs[obj_count++];
    o->alive=1; o->type=type;
    o->x=WFX(sx); o->y=WFY(sy);
    o->vx=(prand()%5-2)*FP/2; o->vy=-FP;
    o->w=1; o->h=1;
    o->color=obj_colors[color_idx%12]; o->ch=obj_chars[color_idx%12];
    o->trail_n=0; color_idx++;
    desktop_set_status("Spawned!");desktop_vfx_flash(80,180,255,3);
}
static void clear_objects() {
    obj_count=0;
    for(int i=0;i<MAX_OBJECTS;i++) objs[i].alive=0;
    desktop_set_status("Cleared");
}
static void draw_world() {
    for(int x=WORLD_X;x<=WORLD_R;x++){ pp(x,WORLD_Y,'-',8,0); pp(x,WORLD_B,'=',7,0); }
    for(int y=WORLD_Y;y<=WORLD_B;y++){ pp(WORLD_X,y,'|',8,0); pp(WORLD_R,y,'|',8,0); }
    pp(WORLD_X,WORLD_Y,'+',8,0); pp(WORLD_R,WORLD_Y,'+',8,0);
    pp(WORLD_X,WORLD_B,'+',7,0); pp(WORLD_R,WORLD_B,'+',7,0);
    for(int x=WORLD_X+1;x<WORLD_R;x++) pp(x,WORLD_B,'#',8,0);
}
static void draw_paddle() {
    for(int i=0;i<paddle_w;i++){ uint8_t col=(i==0||i==paddle_w-1)?14:11; pp(paddle_x+i,paddle_y,'=',col,0); }
}
static void draw_spawn_cursor() {
    char c=(spawn_type==OBJ_BALL)?'O':(spawn_type==OBJ_BOX)?'#':'^';
    pp(spawn_x,spawn_y,c,14,0);
    pp(spawn_x-1,spawn_y,'-',8,0); pp(spawn_x+1,spawn_y,'-',8,0); pp(spawn_x,spawn_y-1,'.',8,0);
}
static void draw_panel() {
    int px=WORLD_R+2, pw=VGA_WIDTH-px-1;
    for(int y=0;y<VGA_HEIGHT;y++) pp(px-1,y,'|',8,0);
    ps(px,0,"PHYSICS",11,0); ps(px,1,"SANDBOX",11,0);
    ps(px,3,"Objects:",8,0);
    char oc[4]; n2s_p(obj_count,oc); ps(px+9,3,oc,15,0);
    ps(px+11,3,"/",8,0);
    char mc[4]; n2s_p(MAX_OBJECTS,mc); ps(px+12,3,mc,8,0);
    ps(px,5,"Gravity:",8,0);
    char gv[8]; n2s_p(gravity,gv); ps(px,6,gv,14,0); ps(px+4,6,"/10",8,0);
    for(int i=0;i<10;i++){ uint8_t bg=(i<gravity)?12:8; pp(px+i,7,' ',0,bg); }
    ps(px,9,"Elasticity:",8,0);
    int el_pct=elasticity*100/256;
    char ep[8]; n2s_p(el_pct,ep); ps(px,10,ep,10,0); ps(px+3,10,"%",8,0);
    for(int i=0;i<10;i++){ uint8_t bg=(i<el_pct/10)?10:8; pp(px+i,11,' ',0,bg); }
    ps(px,13,"Friction:",8,0);
    int fr_pct=(256-friction)*100/256;
    char fp2[8]; n2s_p(fr_pct,fp2); ps(px,14,fp2,13,0); ps(px+3,14,"%",8,0);
    for(int i=0;i<10;i++){ uint8_t bg=(i<fr_pct/10)?13:8; pp(px+i,15,' ',0,bg); }
    ps(px,17,"Spawn:",8,0);
    const char* stypes[]={"Ball ","Box  ","Tri  "};
    ps(px,18,stypes[spawn_type],14,0);
    ps(px,20,sim_running?"RUNNING":"PAUSED ",sim_running?10:14,0);
    ps(px,21,"Tick:",8,0);
    char tk[8]; n2s_p(sim_ticks,tk); ps(px+5,21,tk,7,0);
    (void)pw;
}
static void draw_controls() {
    pfr(0,' ',0,1);
    ps(2,0,"[ Physics Sandbox ]",15,1);
    ps(22,0,"SPACE:spawn  WASD:move  P:pause  c:clear",7,1);
    ps(64,0,"H:help",11,1); ps(72,0,"ESC:back",7,1);
}
static void draw_objects() {
    for(int i=0;i<MAX_OBJECTS;i++){
        PhysObj* o=&objs[i];
        if(!o->alive) continue;
        int sx=o->x/FP, sy=o->y/FP;
        if(sx<=WORLD_X||sx>=WORLD_R||sy<=WORLD_Y||sy>=WORLD_B) continue;
        if(show_trails){
            for(int t=0;t<o->trail_n;t++){
                int tx=o->trail_x[t], ty=o->trail_y[t];
                if(tx>WORLD_X&&tx<WORLD_R&&ty>WORLD_Y&&ty<WORLD_B)
                    pp(tx,ty,'.',o->color-2>0?o->color-2:8,0);
            }
        }
        char c=o->ch;
        if(o->type==OBJ_BALL){
            int spd=(o->vx*o->vx+o->vy*o->vy)/FP;
            if(spd<FP/4) c='o'; else if(spd<FP) c='O'; else c='@';
        }
        pp(sx,sy,c,o->color,0);
    }
}
static void draw_help() {
    pf(10,5,58,16,' ',15,1);
    for(int x=10;x<68;x++){pp(x,5,'-',14,1);pp(x,20,'-',14,1);}
    for(int y=5;y<=20;y++){pp(10,y,'|',14,1);pp(67,y,'|',14,1);}
    ps(12,5,"[ Physics Sandbox - Help ]",15,1);
    ps(12,6,"  Movement:",11,1);
    ps(12,7,"  WASD / Arrow keys  : Move spawn cursor",7,1);
    ps(12,8,"  A/D                : Move paddle left/right",7,1);
    ps(12,9,"  Spawning:",11,1);
    ps(12,10,"  SPACE    : Spawn object at cursor",7,1);
    ps(12,11,"  T        : Cycle spawn type (Ball/Box/Triangle)",7,1);
    ps(12,12,"  Physics:",11,1);
    ps(12,13,"  G/V      : Gravity up/down",7,1);
    ps(12,14,"  E/R      : Elasticity up/down",7,1);
    ps(12,15,"  F/B      : Friction up/down",7,1);
    ps(12,16,"  Simulation:",11,1);
    ps(12,17,"  P        : Pause/Resume",7,1);
    ps(12,18,"  c        : Clear all objects",7,1);
    ps(12,19,"  X        : Toggle trails",7,1);
    ps(12,20,"  H        : Toggle this help",7,1);
}
static void vga_physics_draw() {
    pf(0,0,VGA_WIDTH,VGA_HEIGHT,' ',0,0);
    draw_controls(); draw_world(); draw_panel(); draw_paddle(); draw_objects(); draw_spawn_cursor();
    if(show_help) draw_help();
}
static void physics_step() {
    for(int i=0;i<MAX_OBJECTS;i++){
        PhysObj* o=&objs[i];
        if(!o->alive) continue;
        int sx_old=o->x/FP, sy_old=o->y/FP;
        o->vy += gravity;
        o->vx = FP_MUL(o->vx, friction);
        o->x += o->vx; o->y += o->vy;
        int sx=o->x/FP, sy=o->y/FP;
        if(sx<=WORLD_X){ o->x=WFX(WORLD_X+1); o->vx=FP_MUL(-o->vx,elasticity);  }
        if(sx>=WORLD_R){ o->x=WFX(WORLD_R-1); o->vx=FP_MUL(-o->vx,elasticity);  }
        if(sy<=WORLD_Y){ o->y=WFY(WORLD_Y+1); o->vy=FP_MUL(-o->vy,elasticity); }
        if(sy>=WORLD_B){
            o->y=WFY(WORLD_B-1); o->vy=FP_MUL(-o->vy,elasticity);
            if(o->vy>-FP/4&&o->vy<0) o->vy=0;
            
        }
        sx=o->x/FP; sy=o->y/FP;
        if(sy>=paddle_y && sy<=paddle_y+1 && sx>=paddle_x && sx<=paddle_x+paddle_w){
            o->y=WFY(paddle_y-1); o->vy=FP_MUL(-o->vy,elasticity);
            int paddle_center=paddle_x+paddle_w/2, hit_pos=sx-paddle_center;
            o->vx += hit_pos*FP/4;
            
        }
        for(int j=i+1;j<MAX_OBJECTS;j++){
            PhysObj* b=&objs[j];
            if(!b->alive) continue;
            int bsx=b->x/FP, bsy=b->y/FP, asx=o->x/FP, asy=o->y/FP;
            if(asx==bsx&&asy==bsy){
                int tvx=o->vx, tvy=o->vy;
                o->vx=FP_MUL(b->vx,elasticity); o->vy=FP_MUL(b->vy,elasticity);
                b->vx=FP_MUL(tvx,elasticity);   b->vy=FP_MUL(tvy,elasticity);
                o->x-=FP; b->x+=FP;
            }
        }
        if(show_trails){
            for(int t=2;t>=0;t--){ o->trail_x[t+1]=o->trail_x[t]; o->trail_y[t+1]=o->trail_y[t]; }
            o->trail_x[0]=sx_old; o->trail_y[0]=sy_old;
            if(o->trail_n<4) o->trail_n++;
        }
    }
}
static void vga_physics_handle_key(char c) {
    if(show_help&&c!='h'&&c!='H'){show_help=0;return;}
    if(c=='w'||c=='W'||c=='A'){ if(spawn_y>WORLD_Y+1) spawn_y--; return; }
    if(c=='s'||c=='S'||c=='B'){ if(spawn_y<WORLD_B-1) spawn_y++; return; }
    if(c=='a'||c=='D'){ if(spawn_x>WORLD_X+2) spawn_x--; if(paddle_x>WORLD_X+1) paddle_x--; return; }
    if(c=='d'||c=='C'){ if(spawn_x<WORLD_R-2) spawn_x++; if(paddle_x+paddle_w<WORLD_R-1) paddle_x++; return; }
    if(c==' '){ spawn_object(spawn_x,spawn_y,(ObjType)spawn_type); return; }
    if(c=='t'||c=='T'){ spawn_type=(spawn_type+1)%3; return; }
    if(c=='p'||c=='P'){ sim_running=!sim_running; return; }
    // NOTE: only lowercase 'c' clears now. Uppercase 'C' is the
    // right-arrow keycode (matching the WASD+arrow convention used
    // everywhere else in this codebase) and is already consumed by
    // the 'd'/'C' move-right branch above — sharing it with Clear
    // meant Clear could never actually be reached via that key.
    if(c=='c'){ clear_objects(); return; }
    if(c=='x'||c=='X'){ show_trails=!show_trails; return; }
    if(c=='h'||c=='H'){ show_help=!show_help; return; }
    if(c=='g'||c=='G'){ gravity++; if(gravity>10) gravity=10; return; }
    if(c=='v'||c=='V'){ gravity--; if(gravity<1) gravity=1; return; }
    if(c=='e'||c=='E'){ elasticity+=16; if(elasticity>256) elasticity=256; return; }
    if(c=='r'||c=='R'){ elasticity-=16; if(elasticity<32) elasticity=32; return; }
    if(c=='f'||c=='F'){ friction-=8; if(friction<128) friction=128; return; }
    if(c=='b'){ friction+=8; if(friction>256) friction=256; return; }
}

// ═══════════════════════════════════════════════════════════════════
//  VESA PIXEL-SPACE SIMULATION
//  Real circle-circle collision, filled shapes, dynamic-resolution
//  world bounds — a genuinely richer simulation than the VGA
//  character-cell version above, not just a re-skin of it.
// ═══════════════════════════════════════════════════════════════════

#define VP_MAX_OBJECTS 16

typedef struct {
    int      alive;
    ObjType  type;
    int      x, y, vx, vy;   // fixed-point pixel position/velocity
    int      radius;         // real pixels (not fixed-point)
    uint32_t color;
    int      trail_x[5], trail_y[5], trail_n;
} VPhysObj;

static VPhysObj vobjs[VP_MAX_OBJECTS];
static int vobj_count=0;

static int vworld_x, vworld_y, vworld_w, vworld_h;
static int vpaddle_x, vpaddle_y, vpaddle_w=140;
static int vspawn_x, vspawn_y, vspawn_type=OBJ_BALL;
static int vsim_running=1, vsim_ticks=0, vshow_help=0, vshow_trails=1;
static int vgravity=3, vfriction=250, velasticity=210;
static int vcolor_idx=0;
static int world_ready=0;   /* becomes 1 once vesa_physics_draw has run once */

static const uint32_t vobj_colors[]={
    0xFF5555,0x55FF55,0xFFFF55,0x5599FF,0xFF55FF,0x55FFFF,0xFF9955,0xAA88FF
};

#define VP_BG      0x08060E
#define VP_TITLE   0x1A1440
#define VP_BORDER  0x2B2448
#define VP_CARD    0x1A1838
#define VP_TEXT    0xE0D8FF
#define VP_DIM     0x6858A0
#define VP_ACCENT  0x9B7FFF
#define VP_SUCCESS 0x55EE88
#define VP_WARN    0xFFAA00
#define VP_HINT    0x504870

static int vp_len(const char*s){int i=0;while(s[i])i++;return i;}
static uint32_t vp_mix(uint32_t c,int d){
    int r=(int)(c>>16&0xFF)+d;if(r<0)r=0;if(r>255)r=255;
    int g=(int)(c>>8 &0xFF)+d;if(g<0)g=0;if(g>255)g=255;
    int b=(int)(c    &0xFF)+d;if(b<0)b=0;if(b>255)b=255;
    return((uint32_t)r<<16)|((uint32_t)g<<8)|(uint32_t)b;
}
static void vp_fill_circle(int cx,int cy,int r,uint32_t col){
    for(int dy=-r;dy<=r;dy++)
        for(int dx=-r;dx<=r;dx++)
            if(dx*dx+dy*dy<=r*r) vesa_put_pixel(cx+dx,cy+dy,col);
}
/* Simple bounding-box + half-plane rasterizer — triangles here are
   always small (radius ~16px) so an O(w*h) fill per triangle is
   cheap and avoids needing a scanline edge-walker. */
static void vp_fill_triangle(int x0,int y0,int x1,int y1,int x2,int y2,uint32_t col){
    int minx=x0,maxx=x0,miny=y0,maxy=y0;
    if(x1<minx)minx=x1;
    if(x1>maxx)maxx=x1;
    if(x2<minx)minx=x2;
    if(x2>maxx)maxx=x2;
    if(y1<miny)miny=y1;
    if(y1>maxy)maxy=y1;
    if(y2<miny)miny=y2;
    if(y2>maxy)maxy=y2;
    for(int py=miny;py<=maxy;py++){
        for(int px=minx;px<=maxx;px++){
            int w0=(x1-x0)*(py-y0)-(y1-y0)*(px-x0);
            int w1=(x2-x1)*(py-y1)-(y2-y1)*(px-x1);
            int w2=(x0-x2)*(py-y2)-(y0-y2)*(px-x2);
            if((w0>=0&&w1>=0&&w2>=0)||(w0<=0&&w1<=0&&w2<=0))
                vesa_put_pixel(px,py,col);
        }
    }
}

static void vspawn_object(int sx,int sy,ObjType type){
    if(vobj_count>=VP_MAX_OBJECTS){
        for(int i=0;i<VP_MAX_OBJECTS-1;i++) vobjs[i]=vobjs[i+1];
        vobj_count=VP_MAX_OBJECTS-1;
    }
    VPhysObj*o=&vobjs[vobj_count++];
    o->alive=1; o->type=type;
    o->x=sx*FP; o->y=sy*FP;
    o->vx=(prand()%7-3)*FP;
    o->vy=-FP*3;
    o->radius=(type==OBJ_BALL)?13:15;
    o->color=vobj_colors[vcolor_idx%8];
    o->trail_n=0; vcolor_idx++;
    
}
static void vclear_objects(){
    vobj_count=0;
    for(int i=0;i<VP_MAX_OBJECTS;i++) vobjs[i].alive=0;
    
}

static void vphysics_step(){
    for(int i=0;i<VP_MAX_OBJECTS;i++){
        VPhysObj*o=&vobjs[i];
        if(!o->alive) continue;
        int sx_old=o->x/FP, sy_old=o->y/FP;

        o->vy += vgravity;
        o->vx = FP_MUL(o->vx, vfriction);
        o->x += o->vx; o->y += o->vy;

        int sx=o->x/FP, sy=o->y/FP, r=o->radius;

        if(sx-r <= vworld_x){ o->x=(vworld_x+r)*FP; o->vx=FP_MUL(-o->vx,velasticity);  }
        if(sx+r >= vworld_x+vworld_w){ o->x=(vworld_x+vworld_w-r)*FP; o->vx=FP_MUL(-o->vx,velasticity);  }
        if(sy-r <= vworld_y){ o->y=(vworld_y+r)*FP; o->vy=FP_MUL(-o->vy,velasticity); }
        if(sy+r >= vworld_y+vworld_h){
            o->y=(vworld_y+vworld_h-r)*FP; o->vy=FP_MUL(-o->vy,velasticity);
            if(o->vy>-FP/2 && o->vy<0) o->vy=0;
            
        }

        sx=o->x/FP; sy=o->y/FP;
        if(sy+r>=vpaddle_y && sy+r<=vpaddle_y+18 &&
           sx>=vpaddle_x && sx<=vpaddle_x+vpaddle_w && o->vy>0){
            o->y=(vpaddle_y-r)*FP; o->vy=FP_MUL(-o->vy,velasticity);
            int center=vpaddle_x+vpaddle_w/2, hit=sx-center;
            o->vx += hit*FP/40;
            
        }

        // Real circle-circle collision using actual pixel distance —
        // meaningfully better than "same cell" now that positions
        // are real pixels, not coarse character cells.
        for(int j=i+1;j<VP_MAX_OBJECTS;j++){
            VPhysObj*b=&vobjs[j];
            if(!b->alive) continue;
            int bsx=b->x/FP, bsy=b->y/FP;
            int dx=sx-bsx, dy=sy-bsy;
            int dist2=dx*dx+dy*dy;
            int min_dist=o->radius+b->radius;
            if(dist2>0 && dist2<min_dist*min_dist){
                int dist=isqrt(dist2); if(dist==0)dist=1;
                int overlap=min_dist-dist;
                int push=(overlap>0?overlap:2)/2+1;
                if(dx>=0) o->x+=push*FP; else o->x-=push*FP;
                if(dx<0)  b->x+=push*FP; else b->x-=push*FP;
                if(dy>=0) o->y+=push*FP; else o->y-=push*FP;
                if(dy<0)  b->y+=push*FP; else b->y-=push*FP;
                int tvx=o->vx, tvy=o->vy;
                o->vx=FP_MUL(b->vx,velasticity); o->vy=FP_MUL(b->vy,velasticity);
                b->vx=FP_MUL(tvx,velasticity);   b->vy=FP_MUL(tvy,velasticity);
                
            }
        }

        if(vshow_trails){
            for(int t=3;t>=0;t--){ o->trail_x[t+1]=o->trail_x[t]; o->trail_y[t+1]=o->trail_y[t]; }
            o->trail_x[0]=sx_old; o->trail_y[0]=sy_old;
            if(o->trail_n<5) o->trail_n++;
        }
    }
}

static void vp_pill(int x,int y,const char*s,uint32_t f,uint32_t b){
    int w=vp_len(s)*8+16;
    vesa_fill_rect(x,y,w,22,b); vesa_draw_rect(x,y,w,22,f);
    vesa_draw_string(x+8,y+3,s,f,b);
}
static void vp_bar(int x,int y,int w,int h,int pct,uint32_t filled,uint32_t empty){
    if(pct>100)pct=100;
    if(pct<0)pct=0;
    int f=w*pct/100;
    if(f>0) vesa_fill_rect(x,y,f,h,filled);
    if(f<w) vesa_fill_rect(x+f,y,w-f,h,empty);
    vesa_draw_rect(x,y,w,h,VP_BORDER);
}

static void vp_draw_help(int W,int H){
    int ow=440,oh=360,ox=(W-ow)/2,oy=(H-oh)/2;
    vesa_fill_rect(ox,oy,ow,oh,0x12102A);
    vesa_draw_rect(ox,oy,ow,oh,VP_ACCENT);
    vesa_fill_rect(ox,oy,ow,3,VP_ACCENT);
    vesa_draw_string(ox+16,oy+12,"Physics Sandbox — Help",VP_TEXT,0x12102A);
    int y=oy+44;
    struct{const char*k,*v;}rows[]={
        {"Movement",""},
        {"WASD / Arrows","Move spawn cursor"},
        {"A / D","Move paddle left/right"},
        {"Spawning",""},
        {"SPACE","Spawn object at cursor"},
        {"T","Cycle spawn type (Ball/Box/Triangle)"},
        {"Physics",""},
        {"G / V","Gravity up/down"},
        {"E / R","Elasticity up/down"},
        {"F / B","Friction up/down"},
        {"Simulation",""},
        {"P","Pause / Resume"},
        {"c","Clear all objects"},
        {"X","Toggle trails"},
        {"H","Toggle this help"},
    };
    int n=(int)(sizeof(rows)/sizeof(rows[0]));
    for(int i=0;i<n;i++){
        if(rows[i].v[0]=='\0'){
            vesa_draw_string(ox+16,y,rows[i].k,VP_ACCENT,0x12102A);
            y+=22;
        } else {
            vesa_draw_string(ox+32,y,rows[i].k,VP_WARN,0x12102A);
            vesa_draw_string(ox+180,y,rows[i].v,VP_DIM,0x12102A);
            y+=20;
        }
    }
    vesa_draw_string(ox+16,oy+oh-24,"Press H to close",VP_HINT,0x12102A);
}

static void vesa_physics_draw(){
    int W=(int)vesa.width, H=(int)vesa.height;
    vesa_fill_rect(0,0,W,H,VP_BG);

    // Title bar
    vesa_fill_rect(0,0,W,36,VP_TITLE); vesa_hline(0,36,W,VP_BORDER);
    vesa_draw_string(16,10,"Physics Sandbox",VP_TEXT,VP_TITLE);
    vp_pill(200,6,vsim_running?"RUNNING":"PAUSED",
            vsim_running?VP_SUCCESS:VP_WARN,
            vsim_running?0x0A280E:0x3A2808);
    vesa_draw_string(W-80,10,"ESC quit",VP_DIM,VP_TITLE);

    // World bounds — recomputed each frame from current resolution.
    // The OS doesn't change VESA mode mid-session, so this is stable,
    // and recomputing here keeps physics_tick() (called independently
    // from the timer IRQ) trivially in sync without extra plumbing.
    int panel_w = W>900 ? 200 : 160;
    vworld_x=8; vworld_y=44;
    vworld_w=W-panel_w-24;
    vworld_h=H-vworld_y-16;
    world_ready=1;

    vesa_fill_rect(vworld_x,vworld_y,vworld_w,vworld_h,0x0A0814);
    vesa_draw_rect(vworld_x,vworld_y,vworld_w,vworld_h,VP_BORDER);
    vesa_fill_rect(vworld_x,vworld_y+vworld_h-6,vworld_w,6,0x1A1428);

    // Paddle — clamp into the current world bounds every frame
    vpaddle_y = vworld_y+vworld_h-22;
    if(vpaddle_x < vworld_x) vpaddle_x=vworld_x;
    if(vpaddle_x+vpaddle_w > vworld_x+vworld_w) vpaddle_x=vworld_x+vworld_w-vpaddle_w;
    vesa_fill_rect(vpaddle_x,vpaddle_y,vpaddle_w,10,VP_ACCENT);
    vesa_fill_rect(vpaddle_x,vpaddle_y,vpaddle_w,3,vp_mix(VP_ACCENT,+50));
    vesa_draw_rect(vpaddle_x,vpaddle_y,vpaddle_w,10,vp_mix(VP_ACCENT,-60));

    // Objects: trails first, then bodies
    for(int i=0;i<VP_MAX_OBJECTS;i++){
        VPhysObj*o=&vobjs[i];
        if(!o->alive) continue;
        if(vshow_trails)
            for(int t=0;t<o->trail_n;t++)
                vesa_fill_rect(o->trail_x[t]-2,o->trail_y[t]-2,4,4,vp_mix(o->color,-140));
    }
    for(int i=0;i<VP_MAX_OBJECTS;i++){
        VPhysObj*o=&vobjs[i];
        if(!o->alive) continue;
        int sx=o->x/FP, sy=o->y/FP;
        if(o->type==OBJ_BALL){
            vp_fill_circle(sx,sy,o->radius,o->color);
            vp_fill_circle(sx-o->radius/3,sy-o->radius/3,o->radius/3,vp_mix(o->color,+60));
        } else if(o->type==OBJ_BOX){
            vesa_fill_rect(sx-o->radius,sy-o->radius,o->radius*2,o->radius*2,o->color);
            vesa_fill_rect(sx-o->radius,sy-o->radius,o->radius*2,3,vp_mix(o->color,+50));
            vesa_draw_rect(sx-o->radius,sy-o->radius,o->radius*2,o->radius*2,vp_mix(o->color,-60));
        } else {
            vp_fill_triangle(sx,sy-o->radius, sx-o->radius,sy+o->radius, sx+o->radius,sy+o->radius, o->color);
        }
    }

    // Spawn cursor
    {
        uint32_t ccol = vspawn_type==OBJ_BALL?vobj_colors[0]:vspawn_type==OBJ_BOX?vobj_colors[1]:vobj_colors[2];
        vesa_hline(vspawn_x-10,vspawn_y,8,vp_mix(ccol,+40));
        vesa_hline(vspawn_x+2, vspawn_y,8,vp_mix(ccol,+40));
        vesa_vline(vspawn_x,vspawn_y-10,8,vp_mix(ccol,+40));
        vesa_draw_rect(vspawn_x-6,vspawn_y-6,12,12,ccol);
    }

    // Side panel
    int px=vworld_x+vworld_w+16, py=vworld_y;
    int pw=W-px-8;
    vesa_fill_rect(px,py,pw,vworld_h,VP_CARD);
    vesa_draw_rect(px,py,pw,vworld_h,VP_BORDER);
    int sy=py+10;

    vesa_draw_string(px+8,sy,"Objects",VP_DIM,VP_CARD);
    char oc[6]; n2s_p(vobj_count,oc);
    char full_oc[10]; int op=0;
    for(int i=0;oc[i];i++)full_oc[op++]=oc[i];
    full_oc[op++]='/'; full_oc[op++]='0'+VP_MAX_OBJECTS/10; full_oc[op++]='0'+VP_MAX_OBJECTS%10; full_oc[op]=0;
    vesa_draw_string(px+pw-vp_len(full_oc)*8-8,sy,full_oc,VP_TEXT,VP_CARD);
    sy+=24;

    vesa_hline(px+4,sy,pw-8,VP_BORDER); sy+=10;
    vesa_draw_string(px+8,sy,"Gravity",VP_DIM,VP_CARD); sy+=16;
    vp_bar(px+8,sy,pw-16,10,vgravity*10,0xFF6655,0x0C0A1C); sy+=20;

    vesa_draw_string(px+8,sy,"Elasticity",VP_DIM,VP_CARD); sy+=16;
    vp_bar(px+8,sy,pw-16,10,velasticity*100/256,VP_SUCCESS,0x0C0A1C); sy+=20;

    vesa_draw_string(px+8,sy,"Friction",VP_DIM,VP_CARD); sy+=16;
    vp_bar(px+8,sy,pw-16,10,(256-vfriction)*100/256,VP_WARN,0x0C0A1C); sy+=26;

    vesa_hline(px+4,sy,pw-8,VP_BORDER); sy+=10;
    vesa_draw_string(px+8,sy,"Spawn Type",VP_DIM,VP_CARD); sy+=18;
    const char*stypes[]={"Ball","Box","Triangle"};
    for(int i=0;i<3;i++){
        int sel=(i==vspawn_type);
        uint32_t bg=sel?0x352878:0x100E20;
        vesa_fill_rect(px+8,sy,pw-16,22,bg);
        if(sel) vesa_draw_rect(px+8,sy,pw-16,22,VP_ACCENT);
        vesa_draw_string(px+16,sy+4,stypes[i],sel?VP_TEXT:VP_DIM,bg);
        sy+=26;
    }

    sy+=6; vesa_hline(px+4,sy,pw-8,VP_BORDER); sy+=10;
    vesa_draw_string(px+8,sy,"Controls",VP_DIM,VP_CARD); sy+=20;
    const char*ctrls[]={"WASD move","SPC spawn","T type","P pause","c clear","H help"};
    for(int i=0;i<6 && sy+16<py+vworld_h-8;i++){ vesa_draw_string(px+8,sy,ctrls[i],VP_DIM,VP_CARD); sy+=16; }

    if(vshow_help) vp_draw_help(W,H);
}

static void vesa_physics_handle_key(char c){
    if(vshow_help&&c!='h'&&c!='H'){vshow_help=0;return;}
    if(!world_ready)return;  /* haven't drawn once yet — bounds unknown */

    if(c=='w'||c=='W'||c=='A'){ if(vspawn_y>vworld_y+14) vspawn_y-=8; return; }
    if(c=='s'||c=='S'||c=='B'){ if(vspawn_y<vworld_y+vworld_h-14) vspawn_y+=8; return; }
    if(c=='a'||c=='D'){
        if(vspawn_x>vworld_x+14) vspawn_x-=8;
        if(vpaddle_x>vworld_x) vpaddle_x-=10;
        return;
    }
    if(c=='d'||c=='C'){
        if(vspawn_x<vworld_x+vworld_w-14) vspawn_x+=8;
        if(vpaddle_x+vpaddle_w<vworld_x+vworld_w) vpaddle_x+=10;
        return;
    }
    if(c==' '){ vspawn_object(vspawn_x,vspawn_y,(ObjType)vspawn_type); return; }
    if(c=='t'||c=='T'){ vspawn_type=(vspawn_type+1)%3; return; }
    if(c=='p'||c=='P'){ vsim_running=!vsim_running; return; }
    // Only lowercase 'c' clears — see the note in the VGA handler
    // above for why sharing this with uppercase 'C' (right-arrow)
    // silently made Clear unreachable.
    if(c=='c'){ vclear_objects(); return; }
    if(c=='x'||c=='X'){ vshow_trails=!vshow_trails; return; }
    if(c=='h'||c=='H'){ vshow_help=!vshow_help; return; }
    if(c=='g'||c=='G'){ vgravity++; if(vgravity>10) vgravity=10; return; }
    if(c=='v'||c=='V'){ vgravity--; if(vgravity<1) vgravity=1; return; }
    if(c=='e'||c=='E'){ velasticity+=16; if(velasticity>256) velasticity=256; return; }
    if(c=='r'||c=='R'){ velasticity-=16; if(velasticity<32) velasticity=32; return; }
    if(c=='f'||c=='F'){ vfriction-=8; if(vfriction<128) vfriction=128; return; }
    if(c=='b'){ vfriction+=8; if(vfriction>256) vfriction=256; return; }
}

// ═══════════════════════════════════════════════════════════════════
//  PUBLIC API — dispatches between VGA and VESA simulations
// ═══════════════════════════════════════════════════════════════════

void physics_draw() {
    if(vesa.active){ vesa_physics_draw(); return; }
    vga_physics_draw();
}

void physics_handle_key(char c) {
    if(vesa.active){ vesa_physics_handle_key(c); return; }
    vga_physics_handle_key(c);
}

void physics_tick() {
    if(!phys_active) return;

    if(vesa.active){
        if(!vsim_running) return;
        vsim_ticks++;
        if(vsim_ticks%2==0) vphysics_step();
        return;   /* rendering happens via desktop_draw()'s normal
                     redraw path — physics_tick never draws directly */
    }

    if(!sim_running) return;
    sim_ticks++;
    if(sim_ticks%2==0){
        physics_step();
        // Redraw world area only (not full screen — faster in VGA text mode)
        pf(WORLD_X+1, WORLD_Y+1, WORLD_W-2, WORLD_H-2, ' ', 0, 0);
        draw_paddle(); draw_spawn_cursor(); draw_objects();
        int px = WORLD_R + 2;
        pf(px,3,14,1,' ',0,0);
        ps(px,3,"Objects:",8,0);
        char oc[4]; n2s_p(obj_count,oc); ps(px+9,3,oc,15,0);
        pf(px+5,21,8,1,' ',0,0);
        char tk[8]; n2s_p(sim_ticks%1000,tk); ps(px+5,21,tk,7,0);
    }
}

void physics_init() {
    phys_active=1;

    // VGA sim state
    sim_running=1; sim_ticks=0; obj_count=0; show_help=0; show_trails=1;
    spawn_type=OBJ_BALL; spawn_x=WORLD_X+WORLD_W/2; spawn_y=WORLD_Y+3;
    paddle_x=WORLD_X+WORLD_W/2-5; paddle_y=WORLD_B-2; paddle_w=10;
    gravity=2; friction=248; elasticity=200; color_idx=0;
    for(int i=0;i<MAX_OBJECTS;i++) objs[i].alive=0;

    // VESA sim state — world bounds get set on first vesa_physics_draw()
    // call (need vesa.width/height, which physics_init() has no direct
    // reason to query itself), so spawn positions here are placeholders
    // that get corrected the moment the screen first draws.
    vsim_running=1; vsim_ticks=0; vobj_count=0; vshow_help=0; vshow_trails=1;
    vspawn_type=OBJ_BALL; vgravity=3; vfriction=250; velasticity=210; vcolor_idx=0;
    vpaddle_x=200; vpaddle_w=140;
    vspawn_x=300; vspawn_y=100;
    world_ready=0;
    for(int i=0;i<VP_MAX_OBJECTS;i++) vobjs[i].alive=0;

    if(vesa.active){
        // Demo objects — positions relative to a typical 1024-wide
        // world; vphysics_step()'s wall checks will simply clamp them
        // into whatever the real world bounds turn out to be on the
        // very next tick, so this doesn't need to be exact.
        vspawn_object(300,150,OBJ_BALL);
        vspawn_object(450,120,OBJ_BOX);
        vspawn_object(600,180,OBJ_BALL);
        vspawn_object(750,140,OBJ_TRIANGLE);
    } else {
        spawn_object(WORLD_X+10, WORLD_Y+5, OBJ_BALL);
        spawn_object(WORLD_X+20, WORLD_Y+3, OBJ_BOX);
        spawn_object(WORLD_X+30, WORLD_Y+6, OBJ_BALL);
        spawn_object(WORLD_X+40, WORLD_Y+4, OBJ_TRIANGLE);
    }
}

void physics_stop() {
    phys_active = 0;
}
