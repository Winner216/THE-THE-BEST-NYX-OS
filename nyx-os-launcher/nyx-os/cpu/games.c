// cpu/games.c  –  Nyx OS Games Center (VGA + VESA)
#include <games.h>
#include <physics.h>
#include <vesa.h>
#include <speaker.h>
#include <stdint.h>
#include <desktop.h>

// ─── VGA helpers ──────────────────────────────────────────────────
#define VGA_W 80
#define VGA_H 25
#define VMEM  ((uint16_t*)0xB8000)
static inline uint16_t VE(char c,uint8_t f,uint8_t b){return(uint16_t)c|(uint16_t)(f|b<<4)<<8;}
static void GP(int x,int y,char c,uint8_t f,uint8_t b){if(x>=0&&x<VGA_W&&y>=0&&y<VGA_H)VMEM[y*VGA_W+x]=VE(c,f,b);}
static void GS(int x,int y,const char*s,uint8_t f,uint8_t b){for(int i=0;s[i];i++)GP(x+i,y,s[i],f,b);}
static void GF(int x,int y,int w,int h,char c,uint8_t f,uint8_t b){for(int dy=0;dy<h;dy++)for(int dx=0;dx<w;dx++)GP(x+dx,y+dy,c,f,b);}
static void GFR(int y,char c,uint8_t f,uint8_t b){for(int x=0;x<VGA_W;x++)GP(x,y,c,f,b);}
static void GC(int y,const char*s,uint8_t f,uint8_t b){int l=0;while(s[l])l++;GS((VGA_W-l)/2,y,s,f,b);}
static void n2s(int n,char*b){if(n==0){b[0]='0';b[1]=0;return;}if(n<0){b[0]='-';n=-n;}char t[12];int p=0;while(n>0){t[p++]='0'+n%10;n/=10;}for(int i=0;i<p;i++)b[i]=t[p-1-i];b[p]=0;}

// ─── VESA helpers ─────────────────────────────────────────────────
#define GK_BG      0x0E0C22
#define GK_TITLE   0x1A1440
#define GK_BORDER  0x2B2448
#define GK_CARD    0x1A1838
#define GK_SEL     0x352878
#define GK_TEXT    0xE0D8FF
#define GK_DIM     0x6858A0
#define GK_ACCENT  0x9B7FFF
#define GK_SUCCESS 0x55EE88
#define GK_DANGER  0xFF6655
#define GK_WARN    0xFFAA00
#define GK_HINT    0x504870

static const uint32_t VGA_RGB[16]={
    0x000000,0x0000AA,0x00AA00,0x00AAAA,
    0xAA0000,0xAA00AA,0xAA5500,0xAAAAAA,
    0x555555,0x5555FF,0x55FF55,0x55FFFF,
    0xFF5555,0xFF55FF,0xFFFF55,0xFFFFFF
};
static int gk_len(const char*s){int i=0;while(s[i])i++;return i;}
static void gk_str_r(int rx,int y,const char*s,uint32_t f,uint32_t b){
    vesa_draw_string(rx-gk_len(s)*8,y,s,f,b);
}
static void gk_pill(int x,int y,const char*s,uint32_t f,uint32_t b){
    int w=gk_len(s)*8+16;
    vesa_fill_rect(x,y,w,22,b);vesa_draw_rect(x,y,w,22,f);
    vesa_draw_string(x+8,y+3,s,f,b);
}
static uint32_t gk_mix(uint32_t c,int d){
    int r=(int)(c>>16&0xFF)+d;if(r<0)r=0;if(r>255)r=255;
    int g=(int)(c>>8 &0xFF)+d;if(g<0)g=0;if(g>255)g=255;
    int b=(int)(c    &0xFF)+d;if(b<0)b=0;if(b>255)b=255;
    return((uint32_t)r<<16)|((uint32_t)g<<8)|(uint32_t)b;
}
static void gk_num(int x,int y,int n,uint32_t f,uint32_t b){char buf[12];n2s(n,buf);vesa_draw_string(x,y,buf,f,b);}
extern void vesa_draw_char_scaled(int x,int y,char c,uint32_t fg,uint32_t bg,int scale);

// ─── Shared particle system ─────────────────────────────────────────
#define MAX_PARTICLES 48
#define MAX_FX 12
typedef struct{int x,y,vx,vy;int life,max_life;uint32_t color;int active;}Particle;
static Particle particles[MAX_PARTICLES];
static int shake_x=0,shake_y=0,shake_intensity=0;
static int fx_lock_flash=0,fx_celeb_t=0;
// Tetris line clear animation (declared here so tdo_clear can reach it)
static int tet_clear_flash=0,tet_clear_rows[4]={0},tet_clear_count=0;

static int g_rand=77777;
static int grng(){g_rand=g_rand*1103515245+12345;return(g_rand>>16)&0x7FFF;}

static void spawn_particles(int cx,int cy,int count,uint32_t color,int speed,int life){
    for(int i=0;i<MAX_PARTICLES&&count>0;i++){
        if(!particles[i].active){
            Particle*p=&particles[i];p->x=cx;p->y=cy;
            int a=grng()%628;int sp=(grng()%speed)+speed/3;
            p->vx=sp*(a<157?1:a<314?1:a<471?-1:-1)*(a%100)/100;
            p->vy=-sp*((a%157)/78)+grng()%3;
            p->life=life;p->max_life=life;p->color=color;p->active=1;count--;
        }
    }
}
static void update_particles(){
    for(int i=0;i<MAX_PARTICLES;i++){
        if(!particles[i].active)continue;
        particles[i].x+=particles[i].vx;particles[i].y+=particles[i].vy;
        particles[i].vy+=2;particles[i].vx=particles[i].vx*9/10;
        particles[i].life--;if(particles[i].life<=0)particles[i].active=0;
    }
    if(shake_intensity>0){shake_intensity=shake_intensity*3/4;
        if(shake_intensity<=1){shake_intensity=0;shake_x=0;shake_y=0;}
        else{shake_x=(grng()%shake_intensity*2)-shake_intensity;shake_y=(grng()%shake_intensity*2)-shake_intensity;}}
}
static void draw_particles(){
    for(int i=0;i<MAX_PARTICLES;i++){
        if(!particles[i].active)continue;
        int a=particles[i].life*255/particles[i].max_life;
        uint32_t col=gk_mix(particles[i].color,-(255-a));
        vesa_fill_rect(particles[i].x-2,particles[i].y-2,4,4,col);
    }
}

// ─── State ────────────────────────────────────────────────────────
static int games_selected=0;
static int in_snake=0,in_tetris=0,in_physics=0,in_minesweeper=0;
#define GAME_COUNT 4

// ═══════════════════════════════════════════════════════════════════
//  GAMES MENU
// ═══════════════════════════════════════════════════════════════════

static void vesa_menu(){
    int W=(int)vesa.width,H=(int)vesa.height;

    // ── Window position (preserves dragged position) ──────────────
    int wx = (g_win_x >= 0) ? g_win_x : 16;
    int wy = (g_win_y >= 0) ? g_win_y : 40;
    int ww = W - 32;
    int wh = H - wy - 16;
    g_win_x = wx; g_win_y = wy; g_win_w = ww; g_win_h = wh;

    vesa_fill_rect(wx, wy, ww, wh, GK_BG);

    // Title bar — offset by window position
    int th = 52;
    vesa_fill_rect(wx, wy, ww, th, GK_TITLE);
    vesa_hline(wx, wy + th, ww, GK_BORDER);
    const char*t="Nyx Games";
    for(int i=0;t[i];i++) vesa_draw_char_scaled(wx + 24 + i*24, wy + 14, t[i], GK_TEXT, GK_TITLE, 2);
    vesa_draw_string(wx + ww - 88, wy + 20, "ESC back", GK_DIM, GK_TITLE);

    // Cards (positioned within the windowed area)
    typedef struct{const char*icon;const char*name;const char*desc;const char*tag;uint32_t col;}GCard;
    GCard cards[GAME_COUNT]={
        {">>","Snake",      "Eat food, grow longer, avoid walls!", "5 levels", 0x55FF55},
        {"##","Tetris",     "Stack blocks, clear lines to score!","10 levels", 0xFFFF55},
        {"**","Physics",    "Spawn and bounce 2D objects freely!","Sandbox",   0x55FFFF},
        {"MS","Minesweeper","Find all mines without triggering!","3 diffs",    0xFF5555},
    };

    int side_pad = ww > 900 ? 60 : 24;
    int card_h = 96, card_gap = 14;
    int cards_total = GAME_COUNT * card_h + (GAME_COUNT - 1) * card_gap;
    int bottom_h = 34;
    int avail_h = wh - th - bottom_h;
    int cy = wy + th + (avail_h - cards_total) / 2;
    if (cy < wy + th + 16) cy = wy + th + 16;

    for(int i=0;i<GAME_COUNT;i++){
        int rx = wx + side_pad, ry = cy + i * (card_h + card_gap), rw = ww - side_pad * 2;
        int sel=(i==games_selected);
        uint32_t bg=sel?GK_SEL:GK_CARD;
        uint32_t fg=sel?GK_TEXT:0xA090C8;
        vesa_fill_rect(rx,ry,rw,card_h,bg);
        vesa_draw_rect(rx,ry,rw,card_h,sel?GK_ACCENT:GK_BORDER);
        if(sel) vesa_fill_rect(rx,ry,5,card_h,GK_ACCENT);

        int icon_sz=64;
        uint32_t icon_bg=sel?gk_mix(cards[i].col,-80):0x0C0A1C;
        vesa_fill_rect(rx+16,ry+16,icon_sz,icon_sz,icon_bg);
        vesa_draw_rect(rx+16,ry+16,icon_sz,icon_sz,cards[i].col);
        int il=gk_len(cards[i].icon);
        int ix=rx+16+(icon_sz-il*16)/2;
        for(int k=0;k<il;k++) vesa_draw_char_scaled(ix+k*16,ry+28,cards[i].icon[k],cards[i].col,icon_bg,2);

        int tx=rx+16+icon_sz+24;
        for(int k=0;cards[i].name[k];k++)
            vesa_draw_char_scaled(tx+k*16,ry+16,cards[i].name[k],GK_TEXT,bg,2);
        vesa_draw_string(tx,ry+42,cards[i].desc,sel?0xC0B8F0:GK_DIM,bg);
        gk_pill(tx,ry+66,cards[i].tag,sel?cards[i].col:gk_mix(cards[i].col,-60),sel?gk_mix(cards[i].col,-100):0x0C0A1C);
        if(sel) gk_str_r(rx+rw-16,ry+card_h/2-11,"ENTER to play",GK_SUCCESS,bg);
        (void)fg;
    }

    // Bottom hint — offset by window position
    int by = wy + wh - bottom_h;
    vesa_fill_rect(wx, by, ww, bottom_h, GK_TITLE);
    vesa_hline(wx, by, ww, GK_BORDER);
    vesa_draw_string(wx + 24, by + 7, "UP/DOWN  select     ENTER  play     ESC  back", GK_HINT, GK_TITLE);
}

void games_draw(){
    if(in_snake)      {snake_draw();       return;}
    if(in_tetris)     {tetris_draw();      return;}
    if(in_physics)    {physics_draw();     return;}
    if(in_minesweeper){minesweeper_draw(); return;}

    if(vesa.active){vesa_menu();return;}

    // VGA fallback menu
    GF(0,0,VGA_W,VGA_H,' ',0,0);
    GFR(0,' ',0,2);GS(3,0,"[ Nyx Games ]",15,2);GS(74,0,"[X]",15,1);
    GC(3,"N Y X   G A M E S",11,0);
    const char*gnames[]={"Snake","Tetris","Physics","Minesweeper"};
    const char*gdesc[]={"Eat food, avoid walls!","Stack blocks, clear lines!","2D physics sandbox!","Find all the mines!"};
    for(int i=0;i<GAME_COUNT;i++){
        uint8_t fg=(i==games_selected)?0:15,bg=(i==games_selected)?10:0;
        GF(4,8+i*3,72,2,' ',fg,bg);
        GS(5,8+i*3,gnames[i],fg,bg);GS(20,8+i*3,gdesc[i],fg,bg);
    }
    GS(2,23,"UP/DOWN:select    ENTER:play    ESC:back",7,0);
}

void games_handle_key(char c){
    if(in_snake){
        if(c==27){snake_init();in_snake=0;games_draw();return;}
        snake_handle_key(c);return;
    }
    if(in_tetris){
        if(c==27){in_tetris=0;games_draw();return;}
        tetris_handle_key(c);return;
    }
    if(in_physics){
        if(c==27){physics_stop();in_physics=0;games_draw();return;}
        physics_handle_key(c);return;
    }
    if(in_minesweeper){
        if(c==27){in_minesweeper=0;games_draw();return;}
        minesweeper_handle_key(c);return;
    }
    if(c=='A'&&games_selected>0){games_selected--;games_draw();return;}
    if(c=='B'&&games_selected<GAME_COUNT-1){games_selected++;games_draw();return;}
    if(c==13){
        
        if(games_selected==0){snake_init();  in_snake=1;}
        if(games_selected==1){tetris_init(); in_tetris=1;}
        if(games_selected==2){physics_init();in_physics=1;}
        if(games_selected==3){minesweeper_init();in_minesweeper=1;}
        games_draw();return;
    }
}

void games_init(){
    games_selected=0;in_snake=0;in_tetris=0;in_physics=0;in_minesweeper=0;
}

// ═══════════════════════════════════════════════════════════════════
//  SNAKE
// ═══════════════════════════════════════════════════════════════════

#define BOARD_X   2
#define BOARD_Y   2
#define BOARD_W  52
#define BOARD_H  18
#define MAX_SNAKE 300
#define MAX_WALLS  20

typedef struct{int x,y;}Point;
typedef struct{
    const char*name;int speed,wall_count;
    Point walls[MAX_WALLS];
    uint8_t color_head,color_body,color_border,color_food;
}Level;

static const Level levels[]={
    {"Meadow", 5,0,{{}},10,2,2,12},
    {"Forest",  4,4,{{BOARD_X+10,BOARD_Y+5},{BOARD_X+10,BOARD_Y+6},{BOARD_X+40,BOARD_Y+14},{BOARD_X+40,BOARD_Y+15}},10,2,3,14},
    {"Desert",  3,8,{{BOARD_X+10,BOARD_Y+5},{BOARD_X+11,BOARD_Y+5},{BOARD_X+40,BOARD_Y+5},{BOARD_X+41,BOARD_Y+5},{BOARD_X+10,BOARD_Y+14},{BOARD_X+11,BOARD_Y+14},{BOARD_X+40,BOARD_Y+14},{BOARD_X+41,BOARD_Y+14}},14,6,6,14},
    {"Ice Cave",2,12,{{BOARD_X+15,BOARD_Y+4},{BOARD_X+16,BOARD_Y+4},{BOARD_X+17,BOARD_Y+4},{BOARD_X+33,BOARD_Y+4},{BOARD_X+34,BOARD_Y+4},{BOARD_X+35,BOARD_Y+4},{BOARD_X+15,BOARD_Y+15},{BOARD_X+16,BOARD_Y+15},{BOARD_X+17,BOARD_Y+15},{BOARD_X+33,BOARD_Y+15},{BOARD_X+34,BOARD_Y+15},{BOARD_X+35,BOARD_Y+15}},11,3,9,11},
    {"Volcano", 1,16,{{BOARD_X+10,BOARD_Y+3},{BOARD_X+20,BOARD_Y+3},{BOARD_X+30,BOARD_Y+3},{BOARD_X+40,BOARD_Y+3},{BOARD_X+10,BOARD_Y+16},{BOARD_X+20,BOARD_Y+16},{BOARD_X+30,BOARD_Y+16},{BOARD_X+40,BOARD_Y+16},{BOARD_X+5,BOARD_Y+8},{BOARD_X+5,BOARD_Y+9},{BOARD_X+45,BOARD_Y+8},{BOARD_X+45,BOARD_Y+9},{BOARD_X+25,BOARD_Y+8},{BOARD_X+25,BOARD_Y+9},{BOARD_X+25,BOARD_Y+10},{BOARD_X+25,BOARD_Y+11}},12,4,9,12},
};
#define LEVEL_COUNT 5

static Point snake_body[MAX_SNAKE];
static int snake_len=0,dir_x=1,dir_y=0,next_dir_x=1,next_dir_y=0;
static Point food,bonus_food;
static int has_bonus=0,bonus_timer=0;
static int score=0,high_score=0,game_over=0;
static int snake_run=0,speed_tick=0,current_level=0,paused=0,lives=3;
static int combo=0,combo_timer=0,death_flash=0,eat_flash=0;
static int rand_seed=42317;
static int rng(){rand_seed=rand_seed*1103515245+12345;return(rand_seed>>16)&0x7FFF;}

static int is_wall(int x,int y){
    const Level*lv=&levels[current_level];
    for(int i=0;i<lv->wall_count;i++)if(lv->walls[i].x==x&&lv->walls[i].y==y)return 1;
    return 0;
}
static void place_food(Point*f){
    for(int att=0;att<200;att++){
        int fx=BOARD_X+1+(rng()%(BOARD_W-2)),fy=BOARD_Y+1+(rng()%(BOARD_H-2));
        int ok=1;for(int i=0;i<snake_len;i++)if(snake_body[i].x==fx&&snake_body[i].y==fy){ok=0;break;}
        if(ok&&!is_wall(fx,fy)){f->x=fx;f->y=fy;return;}
    }
}

void snake_init(){
    snake_len=4;dir_x=1;dir_y=0;next_dir_x=1;next_dir_y=0;
    score=0;game_over=0;snake_run=1;speed_tick=0;paused=0;has_bonus=0;lives=3;
    combo=0;combo_timer=0;death_flash=0;eat_flash=0;
    int sx=BOARD_X+BOARD_W/2,sy=BOARD_Y+BOARD_H/2;
    for(int i=0;i<snake_len;i++){snake_body[i].x=sx-i;snake_body[i].y=sy;}
    place_food(&food);
}

// VESA Snake — dynamic cells based on resolution

static void vesa_snake_draw(){
    update_particles();
    int W=(int)vesa.width,H=(int)vesa.height;
    // Apply screen shake offset
    int ox=shake_x,oy=shake_y;

    uint32_t bg_tint=0x060410;
    if(death_flash>0) bg_tint = (death_flash%4<2) ? 0x2A0808 : 0x060410;
    else if(eat_flash>0) bg_tint = 0x081E0C;
    vesa_fill_rect(0,0,W,H,bg_tint);
    const Level*lv=&levels[current_level];
    uint32_t head_col=VGA_RGB[lv->color_head];
    uint32_t body_col=VGA_RGB[lv->color_body];
    uint32_t border_col=VGA_RGB[lv->color_border];
    uint32_t food_col=VGA_RGB[lv->color_food];

    // Dynamic cell size — fill most of the available vertical space
    int title_h=36,bottom_h=20,side_w=180;
    int avail_w=W-side_w-32,avail_h=H-title_h-bottom_h-12;
    int cell_w=avail_w/BOARD_W,cell_h=avail_h/BOARD_H;
    int cell=(cell_w<cell_h?cell_w:cell_h);
    if(cell<6)cell=6;
    if(cell>24)cell=24;
    int bpw=BOARD_W*cell,bph=BOARD_H*cell;
    int bx=(W-bpw)/2-40,by=title_h+8;
    if(bx+ox<8)bx=8-ox;
    if(by+oy<4)by=4-oy;

    // Title bar with shake
    vesa_fill_rect(0,0,W,title_h,GK_TITLE);vesa_hline(0,title_h,W,GK_BORDER);
    vesa_draw_string(16+ox,8+oy,"Snake",GK_TEXT,GK_TITLE);
    char sc[12];n2s(score,sc);
    vesa_draw_string(100+ox,8+oy,"Score:",GK_DIM,GK_TITLE);vesa_draw_string(152+ox,8+oy,sc,head_col,GK_TITLE);
    char hs[12];n2s(high_score,hs);
    vesa_draw_string(240+ox,8+oy,"Best:",GK_DIM,GK_TITLE);vesa_draw_string(290+ox,8+oy,hs,GK_ACCENT,GK_TITLE);
    char lv2[4];n2s(current_level+1,lv2);
    vesa_draw_string(370+ox,8+oy,"Level:",GK_DIM,GK_TITLE);vesa_draw_string(420+ox,8+oy,lv2,GK_WARN,GK_TITLE);
    if(combo>1){
        char cb[8];int cp=0;cb[cp++]='x';
        char cn[4];n2s(combo,cn);for(int i=0;cn[i];i++)cb[cp++]=cn[i];
        cb[cp]=0;
        gk_pill(500+ox,4+oy,cb,GK_WARN,0x3A2808);
    }
    vesa_draw_string(W-140+ox,8+oy,"Lives:",GK_DIM,GK_TITLE);
    for(int i=0;i<lives;i++) vesa_fill_rect(W-80+i*20+ox,10+oy,14,12,0xFF3030);

    // Board with shake
    vesa_fill_rect(bx+ox,by+oy,bpw,bph,0x08060F);
    // Subtle grid lines
    for(int x=0;x<=BOARD_W;x++){if(x%5==0)vesa_vline(bx+x*cell+ox,by+oy,bph,0x181430);else vesa_vline(bx+x*cell+ox,by+oy,bph,0x0E0C1C);}
    for(int y=0;y<=BOARD_H;y++){if(y%5==0)vesa_hline(bx+ox,by+y*cell+oy,bpw,0x181430);else vesa_hline(bx+ox,by+y*cell+oy,bpw,0x0E0C1C);}
    vesa_draw_rect(bx-2+ox,by-2+oy,bpw+4,bph+4,border_col);
    vesa_draw_rect(bx-3+ox,by-3+oy,bpw+6,bph+6,gk_mix(border_col,-60));

    // Glowing border pulse
    int pulse=score%10;
    uint32_t glow_col=gk_mix(border_col,pulse*3);
    if(pulse<5)vesa_draw_rect(bx-5+ox,by-5+oy,bpw+10,bph+10,gk_mix(glow_col,-100));

    // Walls
    for(int i=0;i<lv->wall_count;i++){
        int wx2=(lv->walls[i].x-BOARD_X)*cell+bx+ox;
        int wy2=(lv->walls[i].y-BOARD_Y)*cell+by+oy;
        vesa_fill_rect(wx2+1,wy2+1,cell-2,cell-2,0x303050);
        vesa_draw_rect(wx2+1,wy2+1,cell-2,cell-2,0x6060A0);
        vesa_fill_rect(wx2+2,wy2+2,cell-4,2,0x505080);
    }

    // Pulsing food with glow
    {int pulse_f=(score*3)%12;
     uint32_t food_glow=gk_mix(food_col,-pulse_f*8);
     int fx=(food.x-BOARD_X)*cell+bx+ox+(cell/2);
     int fy=(food.y-BOARD_Y)*cell+by+oy+(cell/2);
     int fr=cell/3;
     // Glow ring
     vesa_fill_rect(fx-fr-2,fy-fr-2,fr*2+4,fr*2+4,food_glow);
     vesa_fill_rect(fx-fr,fy-fr,fr*2,fr*2,food_col);
     // Highlight
     vesa_fill_rect(fx-fr/2,fy-fr/2,fr,fr,gk_mix(food_col,60));}
    if(has_bonus){
        uint32_t bc=(bonus_timer%4<2)?0xFFFF55:0xFF8800;
        int fx=(bonus_food.x-BOARD_X)*cell+bx+ox+(cell/2);
        int fy=(bonus_food.y-BOARD_Y)*cell+by+oy+(cell/2);
        int fr=cell/3+1;
        vesa_fill_rect(fx-fr-3,fy-fr-3,fr*2+6,fr*2+6,gk_mix(bc,-60));
        vesa_fill_rect(fx-fr,fy-fr,fr*2,fr*2,bc);
        vesa_fill_rect(fx-fr/2,fy-fr/2,fr,fr,gk_mix(bc,80));
    }

    // Snake with rounded segments and gradient
    for(int i=snake_len-1;i>=0;i--){
        int sx2=(snake_body[i].x-BOARD_X)*cell+bx+ox;
        int sy2=(snake_body[i].y-BOARD_Y)*cell+by+oy;
        uint32_t col=(i==0)?head_col:body_col;
        int shrink=(i==0)?0:(i==snake_len-1)?1:2;
        // Rounded look — fill entire cell then add highlights
        vesa_fill_rect(sx2+shrink,sy2+shrink,cell-shrink*2,cell-shrink*2,col);
        // Top-left highlight for 3D effect
        if(i==0){
            vesa_fill_rect(sx2+shrink,sy2+shrink,cell-shrink*2,2,gk_mix(col,50));
            vesa_fill_rect(sx2+shrink,sy2+shrink,2,cell-shrink*2,gk_mix(col,30));
            vesa_fill_rect(sx2+shrink+4,sy2+shrink+3,4,4,0xFFFFFF);
            vesa_fill_rect(sx2+cell-8+shrink,sy2+shrink+3,4,4,0xFFFFFF);
            // Direction-indicating pupil
            if(dir_x==1)vesa_fill_rect(sx2+cell-6+shrink,sy2+shrink+5,2,2,0x222222);
            else if(dir_x==-1)vesa_fill_rect(sx2+4+shrink,sy2+shrink+5,2,2,0x222222);
            else if(dir_y==-1)vesa_fill_rect(sx2+5+shrink,sy2+shrink+3,2,2,0x222222);
            else vesa_fill_rect(sx2+5+shrink,sy2+cell-4+shrink,2,2,0x222222);
        } else {
            vesa_fill_rect(sx2+shrink,sy2+shrink,cell-shrink*2,1,gk_mix(col,30));
        }
    }

    // Particles
    draw_particles();

    // Side panel
    int px=bx+bpw+12+ox;
    int spw=W-px-8;
    if(px+120<W&&spw>100){
        vesa_fill_rect(px,by+oy,spw,bph,GK_CARD);
        vesa_draw_rect(px,by+oy,spw,bph,GK_BORDER);
        int py=by+8+oy;
        vesa_draw_string(px+8,py,"Controls",GK_DIM,GK_CARD);py+=20;
        vesa_hline(px+4,py,spw-8,GK_BORDER);py+=8;
        const char*hints[]={"WASD / arrows","P pause","ESC quit"};
        for(int i=0;i<3;i++){vesa_draw_string(px+8,py,hints[i],GK_DIM,GK_CARD);py+=18;}
        py+=8;vesa_hline(px+4,py,spw-8,GK_BORDER);py+=8;
        vesa_draw_string(px+8,py,"Goal",GK_DIM,GK_CARD);py+=18;
        char g[12];n2s((current_level+1)*50,g);
        vesa_draw_string(px+8,py,g,GK_SUCCESS,GK_CARD);py+=20;
        py+=8;vesa_hline(px+4,py,spw-8,GK_BORDER);py+=8;
        vesa_draw_string(px+8,py,"Speed",GK_DIM,GK_CARD);py+=18;
        int bw_b=spw-24;int spd=10-levels[current_level].speed+1;
        vesa_fill_rect(px+8,py,bw_b,10,0x0C0A1C);
        vesa_fill_rect(px+8,py,bw_b*spd/10,10,GK_ACCENT);
        vesa_draw_rect(px+8,py,bw_b,10,GK_BORDER);
    }

    // Overlays with shake
    if(paused){
        int ow=200,oh=80,ox2=(W-ow)/2,oy2=(H-oh)/2;
        vesa_fill_rect(ox2,oy2,ow,oh,0x1A1440);vesa_draw_rect(ox2,oy2,ow,oh,GK_ACCENT);
        vesa_draw_string(ox2+40,oy2+16,"PAUSED",GK_TEXT,0x1A1440);
        vesa_draw_string(ox2+20,oy2+40,"P to resume",GK_DIM,0x1A1440);
    }
    if(game_over){
        int ow=300,oh=130,ox2=(W-ow)/2,oy2=(H-oh)/2;
        vesa_fill_rect(ox2,oy2,ow,oh,0x3A0808);vesa_draw_rect(ox2,oy2,ow,oh,GK_DANGER);
        vesa_fill_rect(ox2,oy2,ow,5,GK_DANGER);
        vesa_draw_string(ox2+60,oy2+16,"GAME OVER",GK_DANGER,0x3A0808);
        vesa_draw_string(ox2+16,oy2+52,"Score:",GK_DIM,0x3A0808);vesa_draw_string(ox2+80,oy2+52,sc,GK_TEXT,0x3A0808);
        vesa_draw_string(ox2+16,oy2+84,"ENTER restart   ESC quit",GK_DIM,0x3A0808);
    }
    int bottom=by+bph+12+oy;
    if(bottom+20<H)
        vesa_draw_string(16+ox,bottom,"WASD/arrows move   P pause   ESC back",GK_HINT,bg_tint);
}

void snake_draw(){
    if(vesa.active){vesa_snake_draw();return;}
    const Level*lv=&levels[current_level];
    GF(0,0,VGA_W,VGA_H,' ',0,0);GFR(0,' ',0,lv->color_border);
    GS(3,0,"[ Snake ]",15,lv->color_border);
    char sc[12];n2s(score,sc);GS(15,0,"Score:",11,lv->color_border);GS(22,0,sc,15,lv->color_border);
    for(int x=BOARD_X;x<=BOARD_X+BOARD_W;x++){GP(x,BOARD_Y,'-',lv->color_border,0);GP(x,BOARD_Y+BOARD_H,'-',lv->color_border,0);}
    for(int y=BOARD_Y;y<=BOARD_Y+BOARD_H;y++){GP(BOARD_X,y,'|',lv->color_border,0);GP(BOARD_X+BOARD_W,y,'|',lv->color_border,0);}
    for(int i=0;i<lv->wall_count;i++)GP(lv->walls[i].x,lv->walls[i].y,'#',7,0);
    GP(food.x,food.y,'@',lv->color_food,0);
    if(has_bonus)GP(bonus_food.x,bonus_food.y,'$',14,0);
    for(int i=snake_len-1;i>=0;i--){
        char ch=i==0?(dir_x==1?'>':dir_x==-1?'<':dir_y==-1?'^':'v'):(i==snake_len-1?'o':'O');
        GP(snake_body[i].x,snake_body[i].y,ch,i==0?lv->color_head:lv->color_body,0);
    }
    if(paused)GC(11," PAUSED ",15,5);
    if(game_over){GC(10,"GAME OVER!",15,4);GC(12,"ENTER:restart ESC:quit",15,4);}
}

void snake_handle_key(char c){
    if(game_over){if(c==13){current_level=0;snake_init();}return;}
    if(c=='p'||c=='P'){paused=!paused;return;}
    if(paused)return;
    if((c=='w'||c=='W'||c=='A')&&dir_y!=1){next_dir_x=0;next_dir_y=-1;}
    else if((c=='s'||c=='S'||c=='B')&&dir_y!=-1){next_dir_x=0;next_dir_y=1;}
    else if((c=='a'||c=='D')&&dir_x!=1){next_dir_x=-1;next_dir_y=0;}
    else if((c=='d'||c=='C')&&dir_x!=-1){next_dir_x=1;next_dir_y=0;}
}

static void sn_spawn_eat_particles(){
    const Level*lv=&levels[current_level];
    int fx=(food.x-BOARD_X)*12+24,fy=(food.y-BOARD_Y)*10+24;
    spawn_particles(fx,fy,8,VGA_RGB[lv->color_food],6,20);
}
static void sn_spawn_death_particles(){
    int sx=snake_body[0].x*12,sy=snake_body[0].y*10;
    spawn_particles(sx-6,sy-6,25,0xFF3030,12,30);
    spawn_particles(sx,sy,15,0xFF8800,8,25);
    shake_intensity=8;
    death_flash=20;
}
static void sn_spawn_levelup_particles(){
    int sx=BOARD_X*12+BOARD_W*6,sy=BOARD_Y*10+BOARD_H*5;
    spawn_particles(sx,sy,15,0xFFFF55,10,30);
    spawn_particles(sx-30,sy-20,10,0xFF55FF,8,25);
    spawn_particles(sx+30,sy+10,10,0x55FFFF,8,25);
}

void snake_tick(){
    // Decay flash timers unconditionally so they fade whether or not
    // the game is actively ticking this frame (e.g. right after a
    // non-fatal collision, snake_run stays 1 but we still want the
    // flash to fade over the following ticks, not stay stuck).
    if(death_flash>0)death_flash--;
    if(eat_flash>0)eat_flash--;

    if(!snake_run||game_over||!in_snake||paused) return;
    const Level*lv=&levels[current_level];
    if(combo_timer>0){combo_timer--;if(combo_timer==0)combo=0;}
    if(has_bonus){bonus_timer++;if(bonus_timer>80){has_bonus=0;bonus_timer=0;}}
    if(!has_bonus&&(rng()%200==0)){place_food(&bonus_food);has_bonus=1;bonus_timer=0;}

    // Progressive speed: the snake speeds up gradually as score climbs
    // toward the next level threshold, not just in a sudden jump at
    // the level-up moment. Every 25 points within a level shaves one
    // tick off the move interval, floored so it never becomes instant.
    int level_base = current_level*50;
    int progress = score-level_base; if(progress<0)progress=0;
    int eff_speed = lv->speed - progress/25;
    if(eff_speed < 1) eff_speed = 1;

    speed_tick++;if(speed_tick<eff_speed)return;speed_tick=0;
    dir_x=next_dir_x;dir_y=next_dir_y;
    Point nh;nh.x=snake_body[0].x+dir_x;nh.y=snake_body[0].y+dir_y;
    if(nh.x<=BOARD_X||nh.x>=BOARD_X+BOARD_W||nh.y<=BOARD_Y||nh.y>=BOARD_Y+BOARD_H||is_wall(nh.x,nh.y)){
        lives--;death_flash=12;combo=0;combo_timer=0;
        sn_spawn_death_particles();
        if(lives<=0){game_over=1;if(score>high_score){high_score=score;desktop_set_status("New high score!");desktop_vfx_flash(255,200,50,6);}else desktop_set_status("Game Over");}
        else{int sx=BOARD_X+BOARD_W/2,sy=BOARD_Y+BOARD_H/2;snake_len=4;dir_x=1;dir_y=0;next_dir_x=1;next_dir_y=0;for(int i=0;i<snake_len;i++){snake_body[i].x=sx-i;snake_body[i].y=sy;}}
        snake_draw();return;
    }
    for(int i=1;i<snake_len;i++){if(snake_body[i].x==nh.x&&snake_body[i].y==nh.y){lives--;death_flash=12;combo=0;combo_timer=0;sn_spawn_death_particles();if(lives<=0){game_over=1;if(score>high_score)high_score=score;}snake_draw();return;}}
    int ate=(nh.x==food.x&&nh.y==food.y),ate_b=has_bonus&&(nh.x==bonus_food.x&&nh.y==bonus_food.y);
    if(ate||ate_b){
        if(snake_len<MAX_SNAKE){for(int i=snake_len;i>0;i--)snake_body[i]=snake_body[i-1];snake_body[0]=nh;snake_len++;}
        if(ate){
            if(combo_timer>0){ if(combo<8) combo++; } else combo=0;
            combo_timer=40;
            int bonus = combo*5;
            score += 10+bonus;
            eat_flash=6;
            sn_spawn_eat_particles();
            place_food(&food);
        }
        if(ate_b){score+=30;has_bonus=0;eat_flash=8;desktop_set_status("Bonus!");}
    } else {for(int i=snake_len-1;i>0;i--)snake_body[i]=snake_body[i-1];snake_body[0]=nh;}
    if(score>=(current_level+1)*50&&current_level<LEVEL_COUNT-1){
        current_level++;sn_spawn_levelup_particles();
        desktop_set_status("Level Up!");desktop_vfx_flash(100,255,100,6);
        int sx=BOARD_X+BOARD_W/2,sy=BOARD_Y+BOARD_H/2;snake_len=4;dir_x=1;dir_y=0;next_dir_x=1;next_dir_y=0;
        for(int i=0;i<snake_len;i++){snake_body[i].x=sx-i;snake_body[i].y=sy;}
        place_food(&food);has_bonus=0;
    }
    snake_draw();
}
int snake_is_running(){return snake_run&&!game_over;}

// ═══════════════════════════════════════════════════════════════════
//  TETRIS
// ═══════════════════════════════════════════════════════════════════

#define TET_W 10
#define TET_H 20

static const uint8_t pieces[7][4][4][4]={
    {{{0,0,0,0},{1,1,1,1},{0,0,0,0},{0,0,0,0}},{{0,0,1,0},{0,0,1,0},{0,0,1,0},{0,0,1,0}},{{0,0,0,0},{1,1,1,1},{0,0,0,0},{0,0,0,0}},{{0,0,1,0},{0,0,1,0},{0,0,1,0},{0,0,1,0}}},
    {{{0,1,1,0},{0,1,1,0},{0,0,0,0},{0,0,0,0}},{{0,1,1,0},{0,1,1,0},{0,0,0,0},{0,0,0,0}},{{0,1,1,0},{0,1,1,0},{0,0,0,0},{0,0,0,0}},{{0,1,1,0},{0,1,1,0},{0,0,0,0},{0,0,0,0}}},
    {{{0,1,0,0},{1,1,1,0},{0,0,0,0},{0,0,0,0}},{{0,1,0,0},{0,1,1,0},{0,1,0,0},{0,0,0,0}},{{1,1,1,0},{0,1,0,0},{0,0,0,0},{0,0,0,0}},{{0,1,0,0},{1,1,0,0},{0,1,0,0},{0,0,0,0}}},
    {{{0,1,1,0},{1,1,0,0},{0,0,0,0},{0,0,0,0}},{{1,0,0,0},{1,1,0,0},{0,1,0,0},{0,0,0,0}},{{0,1,1,0},{1,1,0,0},{0,0,0,0},{0,0,0,0}},{{1,0,0,0},{1,1,0,0},{0,1,0,0},{0,0,0,0}}},
    {{{1,1,0,0},{0,1,1,0},{0,0,0,0},{0,0,0,0}},{{0,1,0,0},{1,1,0,0},{1,0,0,0},{0,0,0,0}},{{1,1,0,0},{0,1,1,0},{0,0,0,0},{0,0,0,0}},{{0,1,0,0},{1,1,0,0},{1,0,0,0},{0,0,0,0}}},
    {{{1,0,0,0},{1,1,1,0},{0,0,0,0},{0,0,0,0}},{{0,1,1,0},{0,1,0,0},{0,1,0,0},{0,0,0,0}},{{1,1,1,0},{0,0,1,0},{0,0,0,0},{0,0,0,0}},{{0,1,0,0},{0,1,0,0},{1,1,0,0},{0,0,0,0}}},
    {{{0,0,1,0},{1,1,1,0},{0,0,0,0},{0,0,0,0}},{{0,1,0,0},{0,1,0,0},{0,1,1,0},{0,0,0,0}},{{1,1,1,0},{1,0,0,0},{0,0,0,0},{0,0,0,0}},{{1,1,0,0},{0,1,0,0},{0,1,0,0},{0,0,0,0}}},
};
static const uint8_t  pcol_vga[7]={11,14,13,10,12,9,6};
static const uint32_t pcol_rgb[7]={0x55FFFF,0xFFFF55,0xFF55FF,0x55FF55,0xFF5555,0x5555FF,0xAA5500};

static uint8_t board[TET_H][TET_W];
static int cp2=0,cr=0,cpx=3,cpy=0;
static int next_q[3];               /* upcoming pieces, [0] spawns next */
static int hold_piece=-1,hold_used=0;
static int lock_timer=0;
#define LOCK_DELAY 15   /* ticks a landed piece has before it locks (~0.8s) */
static int tet_score=0,tet_level=1,tet_lines=0;
static int tet_over=0,tet_paused=0,tet_tick=0,tet_speed=30;
static int tet_rand=12345;
static int trng(){tet_rand=tet_rand*1103515245+12345;return(tet_rand>>16)&0x7FFF;}

static int pfits(int p,int r,int px,int py){
    for(int y=0;y<4;y++)for(int x=0;x<4;x++)
        if(pieces[p][r][y][x]){int bx=px+x,by=py+y;if(bx<0||bx>=TET_W||by>=TET_H)return 0;if(by>=0&&board[by][bx])return 0;}
    return 1;
}
static void tplace(){
    for(int y=0;y<4;y++)for(int x=0;x<4;x++)
        if(pieces[cp2][cr][y][x]){int bx=cpx+x,by=cpy+y;if(by>=0&&by<TET_H&&bx>=0&&bx<TET_W)board[by][bx]=cp2+1;}
}
static int tclear(){
    int c=0;
    for(int y=TET_H-1;y>=0;y--){int full=1;for(int x=0;x<TET_W;x++)if(!board[y][x]){full=0;break;}
        if(full){c++;for(int yy=y;yy>0;yy--)for(int x=0;x<TET_W;x++)board[yy][x]=board[yy-1][x];for(int x=0;x<TET_W;x++)board[0][x]=0;y++;}}
    return c;
}
static void tspawn(){
    cp2=next_q[0];
    next_q[0]=next_q[1];
    next_q[1]=next_q[2];
    next_q[2]=trng()%7;
    cr=0;cpx=3;cpy=0;lock_timer=0;
    if(!pfits(cp2,cr,cpx,cpy))tet_over=1;
}
static void tdo_clear(int cl){
    tet_lines+=cl;tet_score+=cl==1?100*tet_level:cl==2?300*tet_level:cl==3?500*tet_level:cl==4?800*tet_level:0;
    tet_level=tet_lines/10+1;if(tet_level>10)tet_level=10;
    tet_speed=15-tet_level*1;if(tet_speed<3)tet_speed=3;
    if(cl>0){            desktop_set_status("Line clear!");
            // Set up line clear animation
            tet_clear_flash=18;
            tet_clear_count=0;
        // Record which rows were cleared for the flash animation
        for(int y=TET_H-1;y>=0&&tet_clear_count<4;y--){
            int full=1;
            for(int x=0;x<TET_W;x++)if(!board[y][x]){full=0;break;}
            if(full){tet_clear_rows[tet_clear_count++]=y;
                // Spawn particles at approximate row center
                spawn_particles(TET_W*14,y*18+9,cl*3,pcol_rgb[cl%7],5,20);
            }
        }
    }
}
/* A piece has just locked into the board — clear the "already held
   this piece" restriction so the next piece can be held again. */
static void tlock(){
    // Spawn lock flash particles at piece position before locking
    if(!tet_over){
        uint32_t col=pcol_rgb[cp2];
        for(int y=0;y<4;y++)for(int x=0;x<4;x++)
            if(pieces[cp2][cr][y][x]){
                int px2=(cpx+x)*28+14,py2=(cpy+y)*18+9;
                spawn_particles(px2,py2,2,col,3,12);
            }
        fx_lock_flash=10;
    }
    tplace();
    int cl=tclear();
    tdo_clear(cl);
    hold_used=0;
    tspawn();
}

void tetris_init(){
    for(int y=0;y<TET_H;y++)for(int x=0;x<TET_W;x++)board[y][x]=0;
    tet_score=0;tet_level=1;tet_lines=0;tet_over=0;tet_paused=0;tet_tick=0;tet_speed=15;
    hold_piece=-1;hold_used=0;lock_timer=0;
    for(int i=0;i<3;i++) next_q[i]=trng()%7;
    tspawn();
}

// VESA Tetris — dynamic cells

static void vesa_tetris_draw(){
    update_particles();
    int W=(int)vesa.width,H=(int)vesa.height;
    vesa_fill_rect(0,0,W,H,0x06040E);

    // Dynamic cell size
    int title_h=38,side_w=170;
    int avail_w=W-side_w-32,avail_h=H-title_h-12;
    int cell_w=avail_w/TET_W,cell_h=avail_h/TET_H;
    int cell=(cell_w<cell_h?cell_w:cell_h);
    if(cell<10)cell=10;
    if(cell>28)cell=28;
    int bx=(W-TET_W*cell)/2-20,by=title_h+4;

    // Title bar
    vesa_fill_rect(0,0,W,title_h,GK_TITLE);vesa_hline(0,title_h,W,GK_BORDER);
    vesa_draw_string(16,10,"Tetris",GK_TEXT,GK_TITLE);
    char sc[12];n2s(tet_score,sc);
    vesa_draw_string(120,10,"Score:",GK_DIM,GK_TITLE);vesa_draw_string(176,10,sc,GK_ACCENT,GK_TITLE);
    char lv[4];n2s(tet_level,lv);
    vesa_draw_string(280,10,"Level:",GK_DIM,GK_TITLE);vesa_draw_string(336,10,lv,GK_WARN,GK_TITLE);
    char ln[4];n2s(tet_lines,ln);
    vesa_draw_string(380,10,"Lines:",GK_DIM,GK_TITLE);vesa_draw_string(436,10,ln,GK_SUCCESS,GK_TITLE);
    vesa_draw_string(W-80,10,"ESC quit",GK_DIM,GK_TITLE);

    // Background grid
    vesa_fill_rect(bx,by,TET_W*cell,TET_H*cell,0x08060C);
    for(int x=0;x<=TET_W;x++) vesa_vline(bx+x*cell,by,TET_H*cell,0x0E0B18);
    for(int y=0;y<=TET_H;y++) vesa_hline(bx,by+y*cell,TET_W*cell,0x0E0B18);
    vesa_draw_rect(bx-2,by-2,TET_W*cell+4,TET_H*cell+4,GK_BORDER);
    vesa_draw_rect(bx-3,by-3,TET_W*cell+6,TET_H*cell+6,gk_mix(GK_BORDER,-40));

    // Line clear flash
    int flash_white=0;
    if(tet_clear_flash>0){
        flash_white=1;
        for(int i=0;i<tet_clear_count;i++){
            int y=tet_clear_rows[i];
            uint32_t flash_col=0xFFFFFF;
            if(tet_clear_flash<8)flash_col=gk_mix(0xFFFFFF,-(8-tet_clear_flash)*25);
            vesa_fill_rect(bx,by+y*cell,TET_W*cell,cell,flash_col);
            if(tet_clear_flash%2){
                for(int x=0;x<TET_W;x++){
                    int cx=bx+x*cell,cy=by+y*cell;
                    vesa_fill_rect(cx+1,cy+1,cell-2,cell-2,0xFFFFFF);
                }
            }
        }
        tet_clear_flash--;
        if(tet_clear_flash==0)tet_clear_count=0;
    }

    if(!flash_white){
        // Placed cells with 3D shading
        for(int y=0;y<TET_H;y++)for(int x=0;x<TET_W;x++){
            if(!board[y][x])continue;
            uint32_t col=pcol_rgb[board[y][x]-1];
            int cx=bx+x*cell,cy2=by+y*cell;
            vesa_fill_rect(cx+1,cy2+1,cell-2,cell-2,col);
            // 3D bevel
            vesa_fill_rect(cx+1,cy2+1,cell-2,2,gk_mix(col,+40));
            vesa_fill_rect(cx+1,cy2+1,2,cell-2,gk_mix(col,+30));
            vesa_fill_rect(cx+1,cy2+cell-3,cell-2,2,gk_mix(col,-40));
            vesa_fill_rect(cx+cell-3,cy2+1,2,cell-2,gk_mix(col,-30));
        }
    }

    // Ghost piece
    int gy=cpy;while(pfits(cp2,cr,cpx,gy+1))gy++;
    if(gy!=cpy)for(int y=0;y<4;y++)for(int x=0;x<4;x++)
        if(pieces[cp2][cr][y][x]&&gy+y>=0&&gy+y<TET_H)
            vesa_draw_rect(bx+(cpx+x)*cell+1,by+(gy+y)*cell+1,cell-2,cell-2,gk_mix(pcol_rgb[cp2],-80));

    // Current piece
    if(!tet_over)for(int y=0;y<4;y++)for(int x=0;x<4;x++)
        if(pieces[cp2][cr][y][x]&&cpy+y>=0){
            uint32_t col=pcol_rgb[cp2];
            if(lock_timer>0) col=gk_mix(col,-lock_timer*4);
            int px2=bx+(cpx+x)*cell,py2=by+(cpy+y)*cell;
            vesa_fill_rect(px2+1,py2+1,cell-2,cell-2,col);
            vesa_fill_rect(px2+1,py2+1,cell-2,2,gk_mix(col,+50));
            vesa_fill_rect(px2+1,py2+1,2,cell-2,gk_mix(col,+30));
            vesa_fill_rect(px2+1,py2+cell-3,cell-2,2,gk_mix(col,-50));
        }

    // Lock flash effect
    if(fx_lock_flash>0){
        int bright=(fx_lock_flash>6)?255:fx_lock_flash*35;
        uint32_t flash_col=gk_mix(pcol_rgb[cp2],bright-60);
        for(int y=0;y<4;y++)for(int x=0;x<4;x++)
            if(pieces[cp2][cr][y][x]&&cpy+y>=0&&cpx+x>=0&&cpx+x<TET_W&&cpy+y<TET_H)
                vesa_fill_rect(bx+(cpx+x)*cell,by+(cpy+y)*cell,cell,cell,gk_mix(flash_col,-40));
        fx_lock_flash--;
    }

    // Particles
    draw_particles();

    // Side panel
    int px=bx+TET_W*cell+12,py=by;
    if(px+120<W){
        int panel_w=W-px-8;
        vesa_fill_rect(px,py,panel_w,TET_H*cell,GK_CARD);
        vesa_draw_rect(px,py,panel_w,TET_H*cell,GK_BORDER);

        // HOLD box
        vesa_draw_string(px+8,py+8,"HOLD",GK_DIM,GK_CARD);
        vesa_fill_rect(px+8,py+26,panel_w-16,48,0x100E20);
        vesa_draw_rect(px+8,py+26,panel_w-16,48,hold_used?GK_BORDER:GK_ACCENT);
        if(hold_piece>=0){
            uint32_t hcol=hold_used?gk_mix(pcol_rgb[hold_piece],-100):pcol_rgb[hold_piece];
            for(int y=0;y<4;y++)for(int x=0;x<4;x++)
                if(pieces[hold_piece][0][y][x])
                    vesa_fill_rect(px+16+x*15,py+32+y*11,13,9,hcol);
        }
        int sy=py+84;

        // NEXT queue
        vesa_draw_string(px+8,sy,"NEXT",GK_DIM,GK_CARD);sy+=18;
        for(int q=0;q<3;q++){
            int qy=sy+q*36;
            vesa_fill_rect(px+8,qy,panel_w-16,32,0x100E20);
            vesa_draw_rect(px+8,qy,panel_w-16,32,GK_BORDER);
            for(int y=0;y<4;y++)for(int x=0;x<4;x++)
                if(pieces[next_q[q]][0][y][x])
                    vesa_fill_rect(px+14+x*13,qy+5+y*7,11,6,pcol_rgb[next_q[q]]);
        }
        sy+=3*36+6;

        vesa_hline(px+4,sy,panel_w-8,GK_BORDER);sy+=8;
        vesa_draw_string(px+8,sy,"Controls",GK_DIM,GK_CARD);sy+=20;
        const char*ctrls[]={"A/< left","D/> right","S/v soft","W/^ rotate","SPC drop","C hold  P pause"};
        for(int i=0;i<6;i++){vesa_draw_string(px+8,sy,ctrls[i],GK_DIM,GK_CARD);sy+=16;}
    }

    if(tet_paused){int ow=220,oh=90,ox=(W-ow)/2,oy=by+TET_H*cell/2-45;
        vesa_fill_rect(ox,oy,ow,oh,0x1A1440);vesa_draw_rect(ox,oy,ow,oh,GK_ACCENT);
        vesa_draw_string(ox+50,oy+20,"PAUSED",GK_TEXT,0x1A1440);
        vesa_draw_string(ox+30,oy+48,"P to resume",GK_DIM,0x1A1440);}
    if(tet_over){int ow=300,oh=120,ox=(W-ow)/2,oy=by+TET_H*cell/2-60;
        vesa_fill_rect(ox,oy,ow,oh,0x3A0808);vesa_draw_rect(ox,oy,ow,oh,GK_DANGER);
        vesa_draw_string(ox+60,oy+16,"GAME OVER",GK_DANGER,0x3A0808);
        vesa_draw_string(ox+16,oy+52,"Score:",GK_DIM,0x3A0808);vesa_draw_string(ox+88,oy+52,sc,GK_TEXT,0x3A0808);
        vesa_draw_string(ox+16,oy+80,"ENTER:new game    ESC:quit",GK_DIM,0x3A0808);}
}

void tetris_draw(){
    if(vesa.active){vesa_tetris_draw();return;}
    GF(0,0,VGA_W,VGA_H,' ',0,0);GFR(0,' ',0,2);GS(3,0,"[ TETRIS ]",15,2);
    char sc[12];n2s(tet_score,sc);GS(20,0,"Score:",11,2);GS(27,0,sc,15,2);
    for(int y=0;y<TET_H;y++)for(int x=0;x<TET_W;x++){
        int bx=12+x*2,by=1+y;
        if(board[y][x]){GP(bx,by,'[',pcol_vga[board[y][x]-1],0);GP(bx+1,by,']',pcol_vga[board[y][x]-1],0);}
        else{GP(bx,by,'.',8,0);GP(bx+1,by,'.',8,0);}
    }
    if(!tet_over)for(int y=0;y<4;y++)for(int x=0;x<4;x++)
        if(pieces[cp2][cr][y][x]){int bx=12+(cpx+x)*2,by=1+cpy+y;if(by>=1){GP(bx,by,'[',pcol_vga[cp2],0);GP(bx+1,by,']',pcol_vga[cp2],0);}}
    if(tet_paused)GC(11," PAUSED ",15,5);
    if(tet_over){GC(9,"GAME OVER!",15,4);GC(11,"ENTER:new ESC:quit",15,4);}
}

void tetris_handle_key(char c){
    if(tet_over){if(c==13)tetris_init();return;}
    if(c=='p'||c=='P'){tet_paused=!tet_paused;return;}
    if(tet_paused)return;

    int was_landed = !pfits(cp2,cr,cpx,cpy+1);

    if(c=='a'||c=='D'){if(pfits(cp2,cr,cpx-1,cpy))cpx--;}
    else if(c=='d'||c=='C'){if(pfits(cp2,cr,cpx+1,cpy))cpx++;}
    else if(c=='s'||c=='B'){
        if(pfits(cp2,cr,cpx,cpy+1)){cpy++;tet_score+=1;}  /* soft-drop: +1/row, like hard-drop's +2 */
        else tlock();
    }
    else if(c=='w'||c=='A'){int nr=(cr+1)%4;
        if(pfits(cp2,nr,cpx,cpy))cr=nr;
        else if(pfits(cp2,nr,cpx-1,cpy)){cr=nr;cpx--;}
        else if(pfits(cp2,nr,cpx+1,cpy)){cr=nr;cpx++;}}
    else if(c==' '){
        while(pfits(cp2,cr,cpx,cpy+1)){cpy++;tet_score+=2;}
        tlock();
    }
    else if(c=='c'){
        // Hold: swap the current piece into storage for later use.
        // Classic Tetris rule — only one hold per piece, re-enabled
        // once the current piece actually locks into the board.
        if(!hold_used){
            if(hold_piece==-1){
                hold_piece=cp2;
                tspawn();
            } else {
                int tmp=hold_piece; hold_piece=cp2;
                cp2=tmp; cr=0; cpx=3; cpy=0; lock_timer=0;
                if(!pfits(cp2,cr,cpx,cpy)) tet_over=1;
            }
            hold_used=1;
            
        }
    }

    // If the piece was resting on the stack and the player just moved
    // or rotated it into a position where it can fall further, give
    // it a fresh lock-delay window instead of locking on schedule —
    // this is what lets last-second slides and spins work.
    if(was_landed && pfits(cp2,cr,cpx,cpy+1)) lock_timer=0;
}

void tetris_tick(){
    if(!in_tetris||tet_over||tet_paused)return;
    tet_tick++;

    if(!pfits(cp2,cr,cpx,cpy+1)){
        // Landed: count down the lock-delay grace window instead of
        // locking instantly. Movement/rotation elsewhere resets this
        // timer, so a piece can be slid or spun right up until the
        // window actually runs out.
        lock_timer++;
        if(lock_timer>=LOCK_DELAY){ lock_timer=0; tet_tick=0; tlock(); }
    } else {
        lock_timer=0;
        if(tet_tick>=tet_speed){ tet_tick=0; cpy++; }
    }
    tetris_draw();
}

// ═══════════════════════════════════════════════════════════════════
//  MINESWEEPER
// ═══════════════════════════════════════════════════════════════════

#define MS_MAX_W 16
#define MS_MAX_H 12
typedef struct{const char*name;int w,h,mines;uint32_t color;}MSDiff;
static const MSDiff ms_diffs[]={{"Easy",9,7,10,GK_SUCCESS},{"Medium",13,9,25,GK_WARN},{"Hard",16,12,45,GK_DANGER}};
#define MS_DIFF_COUNT 3
#define MS_HIDDEN   0
#define MS_REVEALED 1
#define MS_FLAGGED  2
#define MS_QUESTION 3

static uint8_t ms_board[MS_MAX_H][MS_MAX_W];
static uint8_t ms_state[MS_MAX_H][MS_MAX_W];
static int ms_cx=0,ms_cy=0,ms_w=9,ms_h=7,ms_mines=10,ms_diff=0;
static int ms_flags=0,ms_revealed=0,ms_over=0,ms_started=0;
static int ms_ticks2=0,ms_time=0,ms_blink=0;
static int ms_best[MS_DIFF_COUNT]={9999,9999,9999};
static int ms_rand_seed=55555;
static int ms_rng(){ms_rand_seed=ms_rand_seed*1103515245+12345;return(ms_rand_seed>>16)&0x7FFF;}

static int ms_adj(int x,int y){
    int c=0;
    for(int dy=-1;dy<=1;dy++)for(int dx=-1;dx<=1;dx++){int nx=x+dx,ny=y+dy;if(nx>=0&&nx<ms_w&&ny>=0&&ny<ms_h&&ms_board[ny][nx]==9)c++;}
    return c;
}
static void ms_place_mines(int sx,int sy){
    int placed=0,tries=0;
    while(placed<ms_mines&&tries<100000){int x=ms_rng()%ms_w,y=ms_rng()%ms_h;tries++;
        if(x>=sx-1&&x<=sx+1&&y>=sy-1&&y<=sy+1)continue;if(ms_board[y][x]==9)continue;ms_board[y][x]=9;placed++;}
    for(int y=0;y<ms_h;y++)for(int x=0;x<ms_w;x++)if(ms_board[y][x]!=9)ms_board[y][x]=ms_adj(x,y);
}
static void ms_reveal(int sx,int sy){
    int qx[MS_MAX_W*MS_MAX_H],qy[MS_MAX_W*MS_MAX_H],head=0,tail=0;
    if(ms_state[sy][sx]!=MS_HIDDEN)return;
    ms_state[sy][sx]=MS_REVEALED;ms_revealed++;qx[tail]=sx;qy[tail]=sy;tail++;
    while(head<tail){int cx=qx[head],cy=qy[head];head++;if(ms_board[cy][cx]!=0)continue;
        for(int dy=-1;dy<=1;dy++)for(int dx=-1;dx<=1;dx++){int nx=cx+dx,ny=cy+dy;
            if(nx<0||nx>=ms_w||ny<0||ny>=ms_h||ms_state[ny][nx]!=MS_HIDDEN)continue;
            ms_state[ny][nx]=MS_REVEALED;ms_revealed++;qx[tail]=nx;qy[tail]=ny;tail++;}}
}

/* Chording: pressing ENTER/SPACE on an already-revealed numbered cell
   whose flagged-neighbor count matches its own number auto-reveals
   every remaining hidden/question neighbor in one move. If any of
   those neighbors turns out to be an un-flagged mine (a mis-flag),
   it's treated exactly like clicking a mine directly — a loss. This
   is the standard efficiency mechanic real Minesweeper has always
   had; without it, clearing large boards means individually clicking
   every safe cell one at a time. */
static void ms_chord(int cx,int cy){
    if(ms_state[cy][cx]!=MS_REVEALED) return;
    int num=ms_board[cy][cx];
    if(num==0||num==9) return;

    int flags=0;
    for(int dy=-1;dy<=1;dy++)for(int dx=-1;dx<=1;dx++){
        int nx=cx+dx,ny=cy+dy;
        if(nx<0||nx>=ms_w||ny<0||ny>=ms_h)continue;
        if(ms_state[ny][nx]==MS_FLAGGED)flags++;
    }
    if(flags!=num) return;  /* flag count doesn't match — do nothing */

    for(int dy=-1;dy<=1;dy++)for(int dx=-1;dx<=1;dx++){
        int nx=cx+dx,ny=cy+dy;
        if(nx<0||nx>=ms_w||ny<0||ny>=ms_h)continue;
        if(ms_state[ny][nx]==MS_HIDDEN||ms_state[ny][nx]==MS_QUESTION){
            if(ms_board[ny][nx]==9){
                for(int y2=0;y2<ms_h;y2++)for(int x2=0;x2<ms_w;x2++)
                    if(ms_board[y2][x2]==9)ms_state[y2][x2]=MS_REVEALED;
                ms_over=2;
                return;
            }
            ms_reveal(nx,ny);
        }
    }
    if(ms_revealed==ms_w*ms_h-ms_mines){
        for(int y=0;y<ms_h;y++)for(int x=0;x<ms_w;x++)
            if(ms_board[y][x]==9&&ms_state[y][x]==MS_HIDDEN)ms_state[y][x]=MS_FLAGGED;
        ms_over=1;if(ms_time<ms_best[ms_diff])ms_best[ms_diff]=ms_time;
    }
}

void minesweeper_init(){
    ms_w=ms_diffs[ms_diff].w;ms_h=ms_diffs[ms_diff].h;ms_mines=ms_diffs[ms_diff].mines;
    ms_cx=ms_w/2;ms_cy=ms_h/2;ms_flags=0;ms_revealed=0;ms_over=0;ms_started=0;ms_ticks2=0;ms_time=0;ms_blink=0;
    for(int y=0;y<ms_h;y++)for(int x=0;x<ms_w;x++){ms_board[y][x]=0;ms_state[y][x]=MS_HIDDEN;}
}

// VESA Minesweeper — dynamic cells

static const uint32_t ms_num_cols[9]={0,0x5599FF,0x55EE88,GK_DANGER,0x8855FF,GK_WARN,GK_SUCCESS,0xFFFFFF,GK_DIM};

static void vesa_ms_draw(){
    update_particles();
    int W=(int)vesa.width,H=(int)vesa.height;
    vesa_fill_rect(0,0,W,H,GK_BG);
    uint32_t dcol=ms_diffs[ms_diff].color;

    // Dynamic cell size
    int title_h=54;
    int avail_w=W-40,avail_h=H-title_h-24;
    int cell_w=avail_w/ms_w,cell_h=avail_h/ms_h;
    int cell=(cell_w<cell_h?cell_w:cell_h);
    if(cell<20)cell=20;
    if(cell>48)cell=48;

    // Header
    vesa_fill_rect(0,0,W,title_h,GK_TITLE);vesa_hline(0,title_h,W,GK_BORDER);
    int remaining=ms_mines-ms_flags;if(remaining<0)remaining=0;
    vesa_draw_string(16,8,"Mines",GK_DIM,GK_TITLE);
    gk_num(16,22,remaining,GK_DANGER,GK_TITLE);

    // Smiley (pulsing faster during gameplay)
    const char*face=ms_over==1?"\\o/":ms_over==2?"x_x":"^_^";
    uint32_t fcol=ms_over==1?GK_SUCCESS:ms_over==2?GK_DANGER:GK_WARN;
    int fc=(W-gk_len(face)*16)/2;
    for(int i=0;face[i];i++) vesa_draw_char_scaled(fc+i*16,10,face[i],fcol,GK_TITLE,2);

    // Diff badge
    gk_pill((W-gk_len(ms_diffs[ms_diff].name)*8-16)/2,0,ms_diffs[ms_diff].name,dcol,gk_mix(dcol,-120));

    // Timer
    char tstr[12];n2s(ms_time,tstr);int tl=0;while(tstr[tl])tl++;tstr[tl++]='s';tstr[tl]=0;
    gk_str_r(W-16,8,"Time",GK_DIM,GK_TITLE);gk_str_r(W-16,22,tstr,GK_ACCENT,GK_TITLE);

    // Board — dynamically sized
    int bw=ms_w*cell,bh=ms_h*cell;
    int bx=(W-bw)/2,by=title_h+8;
    vesa_fill_rect(bx-2,by-2,bw+4,bh+4,GK_CARD);
    vesa_draw_rect(bx-3,by-3,bw+6,bh+6,gk_mix(dcol,-40));
    vesa_draw_rect(bx-2,by-2,bw+4,bh+4,dcol);

    for(int y=0;y<ms_h;y++)for(int x=0;x<ms_w;x++){
        int cx=bx+x*cell,cy=by+y*cell;
        int is_cur=(x==ms_cx&&y==ms_cy);
        switch(ms_state[y][x]){
            case MS_HIDDEN:{
                uint32_t bg=is_cur?(ms_blink%2==0?0x2A2868:0x1A1848):0x1E1C38;
                vesa_fill_rect(cx+1,cy+1,cell-2,cell-2,bg);
                vesa_hline(cx+1,cy+1,cell-2,gk_mix(bg,+30));
                vesa_vline(cx+1,cy+1,cell-2,gk_mix(bg,+20));
                vesa_hline(cx+1,cy+cell-2,cell-2,gk_mix(bg,-20));
                if(is_cur)vesa_draw_rect(cx,cy,cell,cell,gk_mix(dcol,+40));
                break;}
            case MS_FLAGGED:{
                vesa_fill_rect(cx+1,cy+1,cell-2,cell-2,0x1E1C38);
                // Flag pole
                vesa_fill_rect(cx+cell/2-1,cy+4,2,cell-10,0xAAAAAA);
                // Flag (animated pulse on cursor)
                uint32_t flag_col=is_cur?(ms_blink%4<2?0xFF5555:0xFF8888):GK_DANGER;
                int fw=(cell/3>10?10:cell/3);
                vesa_fill_rect(cx+cell/2+1,cy+5,fw,cell/3,flag_col);
                if(is_cur)vesa_draw_rect(cx,cy,cell,cell,gk_mix(dcol,+40));
                break;}
            case MS_QUESTION:{
                vesa_fill_rect(cx+1,cy+1,cell-2,cell-2,0x1E1C38);
                vesa_draw_char_scaled(cx+cell/2-8,cy+cell/2-12,'?',GK_WARN,0x1E1C38,2);
                if(is_cur)vesa_draw_rect(cx,cy,cell,cell,gk_mix(dcol,+40));
                break;}
            case MS_REVEALED:{
                if(ms_board[y][x]==9){
                    uint32_t bg=(ms_over==2)?0x3A0808:0x1A0808;
                    vesa_fill_rect(cx+1,cy+1,cell-2,cell-2,bg);
                    // Explosion X shape
                    vesa_hline(cx+4,cy+cell/2,cell-8,0xFF3030);
                    vesa_vline(cx+cell/2,cy+4,cell-8,0xFF3030);
                    vesa_hline(cx+5,cy+cell/2-1,cell-10,0xFF8855);
                    vesa_fill_rect(cx+cell/2-4,cy+cell/2-4,8,8,0xFF5555);
                } else {
                    vesa_fill_rect(cx+1,cy+1,cell-2,cell-2,0x0C0A1C);
                    if(ms_board[y][x]>0){
                        char nc[2];nc[0]='0'+ms_board[y][x];nc[1]=0;
                        int fs=(cell>30)?3:2;
                        vesa_draw_char_scaled(cx+cell/2-8*fs/2,cy+cell/2-12*fs/2,nc[0],ms_num_cols[ms_board[y][x]],0x0C0A1C,fs);
                    }
                }
                break;}
        }
    }

    // Draw particles
    draw_particles();

    // Celebration particles on win
    if(ms_over==1){
        fx_celeb_t++;
        if(fx_celeb_t%6==0)
            spawn_particles(grng()%W,grng()%40,3,ms_num_cols[(fx_celeb_t/6)%8+1],6,30);
    }

    // Bottom hint
    int bottom=by+bh+10;
    if(bottom+20<H)
        vesa_draw_string(16,bottom,"Arrows move   ENTER reveal/chord   F flag   R restart   D difficulty",GK_HINT,GK_BG);

    // Win overlay
    if(ms_over==1){
        int ow=340,oh=120,ox=(W-ow)/2,oy=(H-oh)/2;
        vesa_fill_rect(ox,oy,ow,oh,0x082808);vesa_draw_rect(ox,oy,ow,oh,GK_SUCCESS);vesa_fill_rect(ox,oy,ow,4,GK_SUCCESS);
        vesa_draw_string(ox+90,oy+18,"YOU WIN!",GK_SUCCESS,0x082808);
        char tt[12];n2s(ms_time,tt);int tl2=0;while(tt[tl2])tl2++;tt[tl2++]='s';tt[tl2]=0;
        vesa_draw_string(ox+16,oy+52,"Time:",GK_DIM,0x082808);vesa_draw_string(ox+72,oy+52,tt,GK_TEXT,0x082808);
        if(ms_time<ms_best[ms_diff])vesa_draw_string(ox+140,oy+52,"NEW BEST!",GK_SUCCESS,0x082808);
        vesa_draw_string(ox+16,oy+84,"ENTER play again   D change diff   ESC menu",GK_DIM,0x082808);
    }
    if(ms_over==2){
        int ow=340,oh=100,ox=(W-ow)/2,oy=(H-oh)/2;
        vesa_fill_rect(ox,oy,ow,oh,0x3A0808);vesa_draw_rect(ox,oy,ow,oh,GK_DANGER);vesa_fill_rect(ox,oy,ow,4,GK_DANGER);
        vesa_draw_string(ox+50,oy+18,"BOOM!  You hit a mine!",GK_DANGER,0x3A0808);
        vesa_draw_string(ox+16,oy+56,"ENTER retry   D change difficulty   ESC menu",GK_DIM,0x3A0808);
    }
}

void minesweeper_draw(){
    if(vesa.active){vesa_ms_draw();return;}
    GF(0,0,VGA_W,VGA_H,' ',0,0);GFR(0,' ',0,ms_diffs[ms_diff].color);
    GS(2,0,"[ MINESWEEPER ]",ms_diffs[ms_diff].color,0);
    int bx=(VGA_W-ms_w*3)/2,by=3;
    for(int y=0;y<ms_h;y++)for(int x=0;x<ms_w;x++){
        int sx=bx+x*3,sy=by+y,cur=(x==ms_cx&&y==ms_cy);
        switch(ms_state[y][x]){
            case MS_HIDDEN: GP(sx,sy,'[',cur?0:7,cur?9:0);GP(sx+1,sy,' ',cur?0:7,cur?9:0);GP(sx+2,sy,']',cur?0:7,cur?9:0);break;
            case MS_FLAGGED: GP(sx,sy,'[',12,0);GP(sx+1,sy,'F',12,0);GP(sx+2,sy,']',12,0);break;
            case MS_QUESTION: GP(sx,sy,'[',14,0);GP(sx+1,sy,'?',14,0);GP(sx+2,sy,']',14,0);break;
            case MS_REVEALED:
                if(ms_board[y][x]==9){GP(sx,sy,' ',12,0);GP(sx+1,sy,'*',12,0);GP(sx+2,sy,' ',12,0);}
                else if(ms_board[y][x]==0){GP(sx,sy,' ',8,0);GP(sx+1,sy,'.',8,0);GP(sx+2,sy,' ',8,0);}
                else{const uint8_t nc[]={0,9,10,12,1,4,11,7,8};GP(sx,sy,' ',nc[ms_board[y][x]],0);GP(sx+1,sy,'0'+ms_board[y][x],nc[ms_board[y][x]],0);GP(sx+2,sy,' ',nc[ms_board[y][x]],0);}
                break;
        }
    }
    GS(2,VGA_H-1,"Arrows:move  ENTER:open  F:flag  R:restart  D:diff  ESC:back",7,8);
    if(ms_over==1)GC(12,"YOU WIN!  ENTER:again  ESC:menu",0,10);
    if(ms_over==2)GC(12,"BOOM!  ENTER:retry  ESC:menu",15,4);
}

void minesweeper_handle_key(char c){
    if(ms_over){
        if(c==13||c==' '){minesweeper_init();minesweeper_draw();}
        if(c=='d'||c=='D'){ms_diff=(ms_diff+1)%MS_DIFF_COUNT;minesweeper_init();minesweeper_draw();}
        return;
    }
    if(c=='A'&&ms_cy>0)ms_cy--;
    else if(c=='B'&&ms_cy<ms_h-1)ms_cy++;
    else if(c=='D'&&ms_cx>0)ms_cx--;
    else if(c=='C'&&ms_cx<ms_w-1)ms_cx++;
    else if((c=='w'||c=='W')&&ms_cy>0)ms_cy--;
    else if((c=='s'||c=='S')&&ms_cy<ms_h-1)ms_cy++;
    else if(c=='a'&&ms_cx>0)ms_cx--;
    else if(c=='d'&&ms_cx<ms_w-1)ms_cx++;
    else if(c==13||c==' '){
        if(ms_state[ms_cy][ms_cx]==MS_FLAGGED){minesweeper_draw();return;}
        if(ms_state[ms_cy][ms_cx]==MS_REVEALED){
            ms_chord(ms_cx,ms_cy);
            minesweeper_draw();return;
        }
        if(!ms_started){ms_place_mines(ms_cx,ms_cy);ms_started=1;}
        if(ms_board[ms_cy][ms_cx]==9){
            for(int y=0;y<ms_h;y++)for(int x=0;x<ms_w;x++)if(ms_board[y][x]==9)ms_state[y][x]=MS_REVEALED;
            ms_over=2;
            // Explosion particles at mine location
            spawn_particles(ms_cx*40+20,ms_cy*32+20,25,0xFF4400,10,30);
            spawn_particles(ms_cx*40+20,ms_cy*32+20,15,0xFF8800,8,25);
            spawn_particles(ms_cx*40+20,ms_cy*32+20,10,0xFFFF00,6,20);
            shake_intensity=10;
        } else {
            ms_reveal(ms_cx,ms_cy);
            if(ms_revealed==ms_w*ms_h-ms_mines){
                for(int y=0;y<ms_h;y++)for(int x=0;x<ms_w;x++)if(ms_board[y][x]==9&&ms_state[y][x]==MS_HIDDEN)ms_state[y][x]=MS_FLAGGED;
                ms_over=1;if(ms_time<ms_best[ms_diff])ms_best[ms_diff]=ms_time;
            }
        }
    }
    else if(c=='f'||c=='F'){
        if(ms_state[ms_cy][ms_cx]==MS_HIDDEN){ms_state[ms_cy][ms_cx]=MS_FLAGGED;ms_flags++;}
        else if(ms_state[ms_cy][ms_cx]==MS_FLAGGED){ms_state[ms_cy][ms_cx]=MS_QUESTION;ms_flags--;}
        else if(ms_state[ms_cy][ms_cx]==MS_QUESTION)ms_state[ms_cy][ms_cx]=MS_HIDDEN;
    }
    else if(c=='r'||c=='R'){minesweeper_init();}
    else if(c=='D'){ms_diff=(ms_diff+1)%MS_DIFF_COUNT;minesweeper_init();}
    minesweeper_draw();
}

void minesweeper_tick(){
    if(!in_minesweeper)return;
    ms_blink++;if(ms_blink>=36)ms_blink=0;
    if(ms_started&&!ms_over){ms_ticks2++;if(ms_ticks2>=18){ms_ticks2=0;ms_time++;}}
}


int games_handle_mouse(int clk_x, int clk_y) {
    if (!vesa.active) return 0;
    int W = (int)vesa.width, H = (int)vesa.height;

    // ── Menu: click on game cards ────────────────────────────────
    if (!in_snake && !in_tetris && !in_physics && !in_minesweeper) {
        int th2 = 52, bottom_h2 = 34, side_pad2 = W > 900 ? 60 : 24;
        int card_h2 = 96, card_gap2 = 14;
        int cards_total2 = GAME_COUNT * card_h2 + (GAME_COUNT - 1) * card_gap2;
        int avail_h2 = H - th2 - bottom_h2;
        int cy2 = th2 + (avail_h2 - cards_total2) / 2;
        if (cy2 < th2 + 16) cy2 = th2 + 16;

        for (int i = 0; i < GAME_COUNT; i++) {
            int rx = side_pad2;
            int ry = cy2 + i * (card_h2 + card_gap2);
            int rw = W - side_pad2 * 2;
            int rh = card_h2;
            if (clk_x >= rx && clk_x < rx + rw &&
                clk_y >= ry && clk_y < ry + rh) {
                if (games_selected == i) {
                    // Already selected — launch
                    if (i == 0) { snake_init();  in_snake = 1; }
                    if (i == 1) { tetris_init(); in_tetris = 1; }
                    if (i == 2) { physics_init(); in_physics = 1; }
                    if (i == 3) { minesweeper_init(); in_minesweeper = 1; }
                } else {
                    games_selected = i;
                }
                return 1;
            }
        }
        return 0;
    }

    // ── Snake: pause/resume, game over restart/quit, direction pad ─
    if (in_snake) {
        // Pause overlay click — resume
        if (paused) {
            int ox2 = (W - 200) / 2, oy2 = (H - 80) / 2;
            if (clk_x >= ox2 && clk_x < ox2 + 200 &&
                clk_y >= oy2 && clk_y < oy2 + 80) {
                paused = 0;
                return 1;
            }
        }
        // Game over overlay — left half restart, right half quit
        if (game_over) {
            int ox2 = (W - 300) / 2, oy2 = (H - 130) / 2;
            if (clk_x >= ox2 && clk_x < ox2 + 300 &&
                clk_y >= oy2 && clk_y < oy2 + 130) {
                if (clk_x < ox2 + 150) {
                    current_level = 0;
                    snake_init();
                } else {
                    snake_init();
                    in_snake = 0;
                }
                return 1;
            }
        }
        // Direction pad: click on game board to steer
        if (!game_over && !paused) {
            int title_h3 = 36, bottom_h3 = 20, side_w3 = 180;
            int avail_w3 = W - side_w3 - 32, avail_h3 = H - title_h3 - bottom_h3 - 12;
            int cell_w3 = avail_w3 / 52, cell_h3 = avail_h3 / 18;
            int cell3 = (cell_w3 < cell_h3 ? cell_w3 : cell_h3);
            if (cell3 < 6) cell3 = 6;
            if (cell3 > 24) cell3 = 24;
            int bpw3 = 52 * cell3, bph3 = 18 * cell3;
            int bx3 = (W - bpw3) / 2 - 40, by3 = title_h3 + 8;
            if (bx3 < 8) bx3 = 8;
            if (by3 < 4) by3 = 4;

            if (clk_x >= bx3 && clk_x < bx3 + bpw3 &&
                clk_y >= by3 && clk_y < by3 + bph3) {
                int rel_x = clk_x - bx3;
                int rel_y = clk_y - by3;
                if (rel_x < bpw3 / 3) {
                    if (dir_x != 1) { next_dir_x = -1; next_dir_y = 0; }
                } else if (rel_x >= 2 * bpw3 / 3) {
                    if (dir_x != -1) { next_dir_x = 1; next_dir_y = 0; }
                } else if (rel_y < bph3 / 2) {
                    if (dir_y != 1) { next_dir_x = 0; next_dir_y = -1; }
                } else {
                    if (dir_y != -1) { next_dir_x = 0; next_dir_y = 1; }
                }
                return 1;
            }
        }
    }

    // ── Tetris: pause/resume, game over new game/quit ────────────
    if (in_tetris) {
        // Recalculate board position matching vesa_tetris_draw()
        int title_h4 = 38, side_w4 = 170;
        int avail_w4 = W - side_w4 - 32, avail_h4 = H - title_h4 - 12;
        int cell4 = (avail_w4 / 10 < avail_h4 / 20) ? avail_w4 / 10 : avail_h4 / 20;
        if (cell4 < 10) cell4 = 10;
        if (cell4 > 28) cell4 = 28;
        int by4 = title_h4 + 4;

        if (tet_paused) {
            int ox2 = (W - 220) / 2;
            int oy2 = by4 + 10 * cell4 - 45;
            if (clk_x >= ox2 && clk_x < ox2 + 220 &&
                clk_y >= oy2 && clk_y < oy2 + 90) {
                tet_paused = 0;
                return 1;
            }
        }
        if (tet_over) {
            int ox2 = (W - 300) / 2;
            int oy2 = by4 + 10 * cell4 - 60;
            if (clk_x >= ox2 && clk_x < ox2 + 300 &&
                clk_y >= oy2 && clk_y < oy2 + 120) {
                if (clk_x < ox2 + 150) {
                    tetris_init();
                } else {
                    tet_over = 0;
                    in_tetris = 0;
                }
                return 1;
            }
        }
    }

    // ── Minesweeper: click on grid cells ─────────────────────────
    if (in_minesweeper) {
        int title_h2 = 54;
        int avail_w2 = W - 40, avail_h2_ms = H - title_h2 - 24;
        int cell_w2 = avail_w2 / ms_w, cell_h2 = avail_h2_ms / ms_h;
        int cell2 = (cell_w2 < cell_h2 ? cell_w2 : cell_h2);
        if (cell2 < 20) cell2 = 20;
        if (cell2 > 48) cell2 = 48;
        int bw2 = ms_w * cell2, bh2 = ms_h * cell2;
        int bx2 = (W - bw2) / 2, by2 = title_h2 + 8;

        if (clk_x >= bx2 && clk_x < bx2 + bw2 &&
            clk_y >= by2 && clk_y < by2 + bh2) {
            int col = (clk_x - bx2) / cell2;
            int row = (clk_y - by2) / cell2;
            if (col >= 0 && col < ms_w && row >= 0 && row < ms_h) {
                ms_cx = col;
                ms_cy = row;
                if (!ms_over) {
                    if (!ms_started) {
                        ms_place_mines(col, row);
                        ms_started = 1;
                    }
                    if (ms_state[row][col] == MS_HIDDEN ||
                        ms_state[row][col] == MS_QUESTION) {
                        ms_reveal(col, row);
                    } else if (ms_state[row][col] == MS_REVEALED) {
                        ms_chord(col, row);
                    }
                }
                return 1;
            }
        }
        return 0;
    }

    return 0;
}


