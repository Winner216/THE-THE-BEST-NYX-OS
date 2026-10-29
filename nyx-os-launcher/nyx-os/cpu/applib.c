// cpu/applib.c  –  App Library  (VGA fallback + VESA pixel UI)
#include <applib.h>
#include <vesa.h>
#include <speaker.h>
#include <stdint.h>
#include <mouse.h>
#include <desktop.h>
#include <fs.h>
#include <pkg.h>

int applib_launch_request = -1;
char applib_pkg_launch[PKG_NAME_MAX];

// ─── VGA helpers ──────────────────────────────────────────────────
#define VGA_WIDTH  80
#define VGA_HEIGHT 25
#define VGA_MEM    ((uint16_t*)0xB8000)
static inline uint16_t vmk(char c,uint8_t fg,uint8_t bg){
    return (uint16_t)c|(uint16_t)(fg|bg<<4)<<8;
}
static void vp(int x,int y,char c,uint8_t fg,uint8_t bg){
    if(x<0||x>=VGA_WIDTH||y<0||y>=VGA_HEIGHT) return;
    VGA_MEM[y*VGA_WIDTH+x]=vmk(c,fg,bg);
}
static void vs(int x,int y,const char*s,uint8_t fg,uint8_t bg){
    for(int i=0;s[i];i++) vp(x+i,y,s[i],fg,bg);
}
static void vf(int x,int y,int w,int h,char c,uint8_t fg,uint8_t bg){
    for(int dy=0;dy<h;dy++) for(int dx=0;dx<w;dx++) vp(x+dx,y+dy,c,fg,bg);
}
static void vfr(int y,uint8_t fg,uint8_t bg){
    for(int x=0;x<VGA_WIDTH;x++) vp(x,y,' ',fg,bg);
}

// ─── String helpers ───────────────────────────────────────────────
static int al_len(const char*s){int i=0;while(s[i])i++;return i;}
static int al_eq(const char*a,const char*b){
    int i=0;
    while(a[i]&&b[i]&&a[i]==b[i]) i++;
    return !a[i]&&!b[i];
}

// ─── VESA color helpers ───────────────────────────────────────────
static uint32_t vcol_add(uint32_t c,int d){
    int r=(int)(c>>16&0xFF)+d; if(r<0)r=0;if(r>255)r=255;
    int g=(int)(c>>8 &0xFF)+d; if(g<0)g=0;if(g>255)g=255;
    int b=(int)(c    &0xFF)+d; if(b<0)b=0;if(b>255)b=255;
    return((uint32_t)r<<16)|((uint32_t)g<<8)|(uint32_t)b;
}

// VGA-palette-index → 32-bit RGB
static const uint32_t vpal[16]={
    0x000000,0x0000AA,0x00AA00,0x00AAAA,
    0xAA0000,0xAA00AA,0xAA5500,0xAAAAAA,
    0x555555,0x5555FF,0x55FF55,0x55FFFF,
    0xFF5555,0xFF55FF,0xFFFF55,0xFFFFFF
};

// ═══════════════════════════════════════════════════════════════════
//  App definitions
//  launch_id = index passed to do_launch() in desktop.c
//   0=Terminal 1=Files 2=About   3=Editor 4=AppLib 5=Trash
//   6=Settings 7=Music 8=Calc    9=Games 10=Paint 11=Sysmon
//  12=Clock   13=Calendar 14=Weather
// ═══════════════════════════════════════════════════════════════════

typedef struct {
    const char* name;
    const char* version;
    const char* category;
    const char* desc;          /* one-liner */
    const char* long_desc;
    const char* author;
    const char* icon;          /* 3-4 char ASCII */
    uint8_t     color;         /* VGA palette index for icon bg */
    int         size_kb;
    const char* features[4];
    int         launch_id;     /* ← correct do_launch() index */
} AppEntry;

#define APP_COUNT 17
#define PKG_MAX    12        // max installed packages shown in the launcher

static const AppEntry apps[APP_COUNT] = {
    {"Terminal",      "2.0","System",
     "Command-line shell with history & autocomplete",
     "Full shell with 25+ commands, TAB autocomplete, command history, pipe support and file management.",
     "Nyx Core",">_ ",2,48,{"25+ commands","TAB autocomplete","History","Pipe support"},0},

    {"File Manager",  "1.0","System",
     "Browse, open and manage your files",
     "Visual file manager to browse, open, copy, move and delete files in the Nyx virtual filesystem.",
     "Nyx Core","[F]",9,32,{"Browse files","Open files","Delete","Trash system"},1},

    {"Text Editor",   "2.0","Productivity",
     "Feature-rich text editor with syntax highlight",
     "Full editor with syntax highlighting, search & replace, 30-level undo/redo and clipboard support.",
     "Nyx Core","[E]",5,96,{"Syntax highlight","Search+replace","Undo 30 levels","Clipboard"},3},

    {"Calculator",    "2.0","Utilities",
     "Scientific calculator with history",
     "Scientific calculator with fixed-point math, trig functions (sin/cos/tan), ln, sqrt, x^2 and pi.",
     "Nyx Core","1+1",3,24,{"Basic arithmetic","sin/cos/tan","ln & sqrt","History"},8},

    {"Music Player",  "1.0","Media",
     "PC speaker music player with 5 songs",
     "Built-in music player using the PC speaker. Play 5 classic built-in songs with play/pause/skip.",
     "Nyx Core",">>|",6,20,{"5 built-in songs","Play/pause/skip","PC speaker","Volume ctrl"},7},

    {"Paint",         "1.0","Creative",
     "ASCII art drawing app with 8 tools",
     "Full ASCII art editor with 8 drawing tools, 16 colors, undo/redo, fill, shapes and text mode.",
     "Nyx Core","[P]",4,64,{"8 drawing tools","16 colors","Fill & shapes","Save to file"},10},

    {"Games",         "2.0","Entertainment",
     "Games center: Snake, Tetris, Minesweeper",
     "Complete games center with Snake (5 levels), Tetris (10 levels), Minesweeper and Physics Sandbox.",
     "Nyx Core","[G]",10,128,{"Snake","Tetris","Minesweeper","Physics sandbox"},9},

    {"System Monitor","1.0","System",
     "Real-time CPU, memory and process monitor",
     "Real-time system monitor showing CPU usage graphs, memory map, process list and disk information.",
     "Nyx Core","[%]",7,48,{"CPU graph","Memory map","Process list","Disk info"},11},

    {"Clock",         "2.0","Utilities",
     "Clock, stopwatch, timer and 4 alarms",
     "Full clock app with digital display, world clocks, stopwatch with laps, countdown and 4 alarms.",
     "Nyx Core","(o)",11,56,{"World clocks","Stopwatch+laps","Countdown","4 alarms"},12},

    {"Calendar",      "1.0","Productivity",
     "Monthly calendar with event management",
     "Monthly calendar view with event management. Add, edit and delete events with colour coding.",
     "Nyx Core","Cal",9,40,{"Monthly view","Add events","Delete events","Colour coding"},13},

    {"Weather",       "2.0","Utilities",
     "Weather with ASCII art & 6-day forecast",
     "Beautiful weather app with 6 weather types, ASCII art animations, statistics and 6-day forecast.",
     "Nyx Core","~~~",11,32,{"6 weather types","ASCII art","Statistics","6-day forecast"},14},

    {"Settings",      "2.0","System",
     "Full system configuration panel",
     "Complete settings: date/time, display, sound, network, user management, 8 wallpapers, 8 themes.",
     "Nyx Core","[S]",8,36,{"8 themes","8 wallpapers","User mgmt","Sound ctrl"},6},

    {"App Library",   "1.0","System",
     "Browse all installed apps and launch them",
     "This app! Browse all installed applications with descriptions, features, version info and categories.",
     "Nyx Core","Lib",13,28,{"App browsing","Descriptions","Categories","Launch apps"},4},

    {"About",         "1.0","System",
     "About Nyx OS",
     "Information about Nyx OS - built from scratch in C and x86 Assembly. Includes version history.",
     "G'olibjon K.","[?]",1,8,{"OS info","Version history","Architecture","Credits"},2},

    {"Doom",          "1.0","Entertainment",
     "The classic 1993 FPS, running as a ring-3 PE",
     "DOOM shareware port (doomgeneric) compiled to a real PE32 executable and launched through the Nyx PE loader. Renders via the game canvas, sounds via the kernel game mixer.",
     "id Software","[D]",15,4800,{"Ring-3 PE process","E1M1 shareware","Game canvas 2x","AC'97 sound"},19},

    {"Chess",         "1.0","Entertainment",
     "Play chess against the built-in AI engine",
     "Full chess with legal move validation (castling, en passant, promotion), four AI levels using alpha-beta search, undo, click or keyboard play and synthesized sounds. Runs as a ring-3 PE on the game canvas.",
     "Nyx Core","[C]",9,96,{"AI opponent 4 levels","Undo + move list","Click to move","Alpha-beta engine"},20},

    {"App Store",     "1.0","Network",
     "Download and install apps from the online store",
     "Browse the online Nyx App Store, download ring-3 apps over the network and install or remove them.",
     "Nyx Core","<i>",5,32,{"Online catalog","Install apps","Run / remove","Network repo"},21},
};

// ─── Categories ───────────────────────────────────────────────────
static const char* cats[]={"All","System","Productivity","Utilities","Media","Creative","Entertainment","Packages"};
#define CAT_COUNT 8

// ─── Installed packages (dynamic entries after the built-ins) ────
// Each package is a filesystem slot named "@<name>"; we surface them
// as ordinary cards so installed apps appear in the launcher.
static int  pkg_count = 0;
static char pkg_names[PKG_MAX][PKG_NAME_MAX];

// App index space: [0, APP_COUNT) = built-in apps, [APP_COUNT, ...) =
// installed packages.  entry_at() resolves a filtered index to the
// right AppEntry (packages get a synthesized entry).
static AppEntry pkg_entries[PKG_MAX];

static const AppEntry* entry_at(int idx){
    if(idx < APP_COUNT) return &apps[idx];
    int pi = idx - APP_COUNT;
    if(pi < 0 || pi >= pkg_count) return &apps[0];
    return &pkg_entries[pi];
}

// Rescan the filesystem for "@name" slots and build launcher cards.
static void refresh_pkgs(){
    pkg_count = 0;
    for(int i=0;i<MAX_FILES && pkg_count<PKG_MAX;i++){
        if(!filesystem[i].used) continue;
        const char* nm=filesystem[i].name;
        if(nm[0]!='@' || !nm[1]) continue;   // package slots are "@name"
        AppEntry* e=&pkg_entries[pkg_count];
        char* dst=pkg_names[pkg_count];
        int k=0;
        for(int j=1; nm[j] && k<PKG_NAME_MAX-1; j++) dst[k++]=nm[j];
        dst[k]='\0';
        e->name     = dst;
        e->version  = "pkg";
        e->category = "Packages";
        e->desc     = "Installed from the App Store";
        e->long_desc= "Downloaded package — press ENTER to run.";
        e->author   = "App Store";
        e->icon     = "Pkg";
        e->color    = 5;
        e->size_kb  = (filesystem[i].size > 0 ? filesystem[i].size : al_len(filesystem[i].content?filesystem[i].content:""))/1024;
        e->features[0]="Ring-3 app";
        e->features[1]="Installed";
        e->features[2]=0;
        e->features[3]=0;
        e->launch_id = -1;   /* not a built-in — handled via pkg launch */
        pkg_count++;
    }
}

// ─── State ────────────────────────────────────────────────────────
static int  sel_app   = 0;
static int  sel_cat   = 0;
static int  view_mode = 0;   /* 0=grid  1=detail */
static int  srch_mode = 0;
static char srch[32];
static int  srch_pos  = 0;
static int  al_scroll = 0;   /* first visible row in grid */

static int filtered[APP_COUNT + PKG_MAX];
static int filtered_n = 0;

static int app_matches(int i){
    const AppEntry* a = entry_at(i);
    if(sel_cat>0){
        const char*cat=cats[sel_cat];
        if(!al_eq(cat,a->category)) return 0;
    }
    if(srch[0]){
        const char*name=a->name;
        int sl=al_len(srch),nl=al_len(name);
        for(int j=0;j<=nl-sl;j++){
            int ok=1;
            for(int k=0;k<sl;k++){
                char sc=srch[k],nc=name[j+k];
                if(sc>='A'&&sc<='Z')sc+=32;
                if(nc>='A'&&nc<='Z')nc+=32;
                if(sc!=nc){ok=0;break;}
            }
            if(ok) return 1;
        }
        return 0;
    }
    return 1;
}

static void build_filter(){
    filtered_n=0;
    for(int i=0;i<APP_COUNT;i++)
        if(app_matches(i)) filtered[filtered_n++]=i;
    for(int i=0;i<pkg_count;i++)
        if(app_matches(APP_COUNT+i)) filtered[filtered_n++]=APP_COUNT+i;
}

static void ensure_scroll(){
    /* keep selected app's row visible */
    int COLS=4, VIS=3;
    int row=sel_app/COLS;
    if(row<al_scroll) al_scroll=row;
    if(row>=al_scroll+VIS) al_scroll=row-VIS+1;
}

// ═══════════════════════════════════════════════════════════════════
//  VESA UI
// ═══════════════════════════════════════════════════════════════════

// Palette
#define AL_BG       0x0E0C22   /* screen bg */
#define AL_TITLEBAR 0x1A1440   /* title bar */
#define AL_CATBAR   0x14122A   /* category bar */
#define AL_CAT_ACT  0x352878   /* active category */
#define AL_CARD     0x1A1838   /* card bg */
#define AL_CARD_SEL 0x26227A   /* selected card bg */
#define AL_BORDER   0x2B2448
#define AL_SEL_BOR  0x7050CC   /* selected card border */
#define AL_TEXT     0xE0D8FF
#define AL_TEXTDIM  0x7060A0
#define AL_TEXTMED  0x9888CC
#define AL_SUCCESS  0x55EE88
#define AL_DANGER   0xFF6655
#define AL_ACCENT   0x9B7FFF

// Draw a single app card
static void draw_vesa_card(int x,int y,int w,int h,int fidx){
    int app_idx=filtered[fidx];
    const AppEntry*a=entry_at(app_idx);
    int sel=(fidx==sel_app);
    int hover = !sel && mouse_in_rect(x, y, w, h);

    uint32_t bg    = sel ? AL_CARD_SEL : (hover ? 0x221E50 : AL_CARD);
    uint32_t bord  = sel ? AL_SEL_BOR  : (hover ? 0x9B7FFF : AL_BORDER);
    uint32_t namec = sel ? AL_TEXT     : AL_TEXT;
    uint32_t decc  = sel ? AL_TEXTMED  : AL_TEXTDIM;

    // Card background + border
    vesa_fill_rect(x,y,w,h,bg);
    vesa_draw_rect(x,y,w,h,bord);
    if(sel){
        vesa_draw_rect(x+1,y+1,w-2,h-2,vcol_add(bord,-30));
        // Top accent line
        vesa_hline(x+2,y,w-4,AL_ACCENT);
    } else if (hover) {
        vesa_draw_rect(x-1,y-1,w+2,h+2,0x6A4FBF);
    }

    // ── Icon box (48×48, top-left of card)
    int icon_sz=48;
    int ix=x+10, iy=y+10;
    uint32_t icon_bg=vpal[a->color&0xF];
    vesa_fill_rect(ix,iy,icon_sz,icon_sz,icon_bg);
    vesa_fill_rect(ix,iy,icon_sz,1,vcol_add(icon_bg,+32));
    vesa_fill_rect(ix,iy,1,icon_sz,vcol_add(icon_bg,+20));
    vesa_draw_rect(ix,iy,icon_sz,icon_sz,vcol_add(icon_bg,+48));
    // Icon text centered
    int il=al_len(a->icon);
    vesa_draw_string(ix+(icon_sz-il*8)/2, iy+(icon_sz-16)/2, a->icon, 0xFFFFFF, icon_bg);

    // ── App name
    int tx=ix+icon_sz+10;
    vesa_draw_string(tx, iy+2,  a->name,    namec, bg);

    // Version + category chips
    {
        char ver[12]; int p=0; ver[p++]='v';
        for(int i=0;a->version[i]&&p<10;i++) ver[p++]=a->version[i];
        ver[p]='\0';
        vesa_draw_string(tx,    iy+22, ver,        0x6868A8, bg);
        vesa_draw_string(tx+40, iy+22, a->category,0x8070B8, bg);
    }

    // Short description (word-wrap into 2 lines)
    {
        const char*d=a->desc;
        int dlen=al_len(d);
        int max_chars=(w-tx-x-12)/8;   /* chars that fit per line */
        if(max_chars<4) max_chars=4;
        // Line 1
        int cut=dlen<max_chars?dlen:max_chars;
        // Try to cut at space
        if(dlen>max_chars){
            int last_sp=0;
            for(int i=0;i<max_chars;i++) if(d[i]==' ') last_sp=i;
            if(last_sp>0) cut=last_sp;
        }
        // draw line 1
        for(int i=0;i<cut;i++)
            if(tx+i*8+x<x+w-8) vesa_draw_string(tx+i*8,iy+42,0,0,0);/* noop placeholder */
        // Actually draw using a buffer
        char line1[48]; int lp=0;
        for(int i=0;i<cut&&lp<47;i++) line1[lp++]=d[i];
        line1[lp]='\0';
        vesa_draw_string(tx, iy+42, line1, decc, bg);
        // Line 2
        if(cut<dlen){
            char line2[48]; lp=0;
            int start=cut; if(d[start]==' ') start++;
            int remain=dlen-start;
            int cut2=remain<max_chars?remain:max_chars;
            for(int i=0;i<cut2&&lp<47;i++) line2[lp++]=d[start+i];
            line2[lp]='\0';
            vesa_draw_string(tx, iy+58, line2, decc, bg);
        }
    }

    // Divider
    vesa_hline(x+8, iy+icon_sz+8, w-16, AL_BORDER);

    // Features (2 × 2 grid, bottom of card)
    int fy=iy+icon_sz+18;
    for(int fi=0;fi<4&&fi<4;fi++){
        if(!a->features[fi]) continue;
        int fx=x+10+(fi%2)*(w/2-10);
        int fyy=fy+(fi/2)*18;
        // bullet
        vesa_put_pixel(fx,   fyy+7, AL_ACCENT);
        vesa_put_pixel(fx+1, fyy+7, AL_ACCENT);
        vesa_draw_string(fx+6, fyy, a->features[fi], 0x7868A8, bg);
    }

    // "ENTER to launch" hint on selected card
    if(sel){
        char hint[]="  ENTER: Launch  ";
        int hl=al_len(hint);
        int hx=x+w-hl*8-8, hy=y+h-20;
        vesa_fill_rect(hx-4,hy-4,hl*8+8,20,AL_CAT_ACT);
        vesa_draw_rect(hx-4,hy-4,hl*8+8,20,AL_SEL_BOR);
        vesa_draw_string(hx,hy-2,hint,AL_SUCCESS,AL_CAT_ACT);
    }
}

// Detail view – full screen
static void draw_vesa_detail(int fidx){
    int app_idx=filtered[fidx];
    const AppEntry*a=entry_at(app_idx);
    uint32_t icon_bg=vpal[a->color&0xF];
    int W=(int)vesa.width, H=(int)vesa.height;

    vesa_fill_rect(0,0,W,H,AL_BG);

    // Title bar
    vesa_fill_rect(0,0,W,36,AL_TITLEBAR);
    vesa_hline(0,36,W,AL_BORDER);
    vesa_draw_string(16,10,"App Library",AL_TEXTDIM,AL_TITLEBAR);
    vesa_draw_string(16+88,10," > ",AL_TEXTDIM,AL_TITLEBAR);
    vesa_draw_string(16+112,10,a->name,AL_TEXT,AL_TITLEBAR);
    vesa_draw_string(W-56,10,"ESC back",AL_TEXTDIM,AL_TITLEBAR);

    // ── Left panel: icon + metadata ──────────────────────────────
    int lx=24, ly=52, lw=280;

    // Big icon (96×96)
    int icz=96;
    vesa_fill_rect(lx,ly,icz,icz,icon_bg);
    vesa_fill_rect(lx,ly,icz,1,vcol_add(icon_bg,+40));
    vesa_fill_rect(lx,ly,1,icz,vcol_add(icon_bg,+24));
    vesa_draw_rect(lx,ly,icz,icz,vcol_add(icon_bg,+60));
    // Icon glyph (scale 2 = 16×32)
    {
        int il=al_len(a->icon);
        int tx2=lx+(icz-il*16)/2, ty2=ly+(icz-32)/2;
        for(int i=0;a->icon[i];i++)
            vesa_draw_char_scaled(tx2+i*16,ty2,a->icon[i],0xFFFFFF,icon_bg,2);
    }

    // Metadata rows below icon
    int my=ly+icz+16;
    // helper: draw a property row
    #define MPROP(label,val,vc) { \
        vesa_draw_string(lx,my,label,AL_TEXTDIM,AL_BG); \
        vesa_draw_string(lx+80,my,val,vc,AL_BG); \
        my+=20; \
    }
    {
        char ver[12]; int p=0; ver[p++]='v';
        for(int i=0;a->version[i]&&p<10;i++) ver[p++]=a->version[i]; ver[p]='\0';
        MPROP("Version",  ver,        AL_TEXT);
        MPROP("Category", a->category,AL_ACCENT);
        MPROP("Author",   a->author,  AL_TEXTMED);
        // Size
        char sz[16]; int sp=0;
        int sv=a->size_kb;
        if(sv==0){sz[sp++]='0';}
        else{char t[8];int tp=0;while(sv>0){t[tp++]='0'+sv%10;sv/=10;}for(int i=tp-1;i>=0;i--)sz[sp++]=t[i];}
        sz[sp++]=' ';sz[sp++]='K';sz[sp++]='B';sz[sp]='\0';
        MPROP("Size",     sz,         AL_TEXTMED);
        MPROP("Status",   "Installed",AL_SUCCESS);
    }
    #undef MPROP

    // ── Right panel: description + features ─────────────────────
    int rx=lx+lw+16, ry=52, rw=W-rx-24;

    // App name (2× scale)
    {
        int nl=al_len(a->name);
        for(int i=0;a->name[i];i++)
            vesa_draw_char_scaled(rx+i*16,ry,a->name[i],AL_TEXT,AL_BG,2);
        (void)nl;
    }
    ry+=44;

    // Category chip
    vesa_fill_rect(rx,ry,al_len(a->category)*8+16,22,AL_CAT_ACT);
    vesa_draw_rect(rx,ry,al_len(a->category)*8+16,22,AL_SEL_BOR);
    vesa_draw_string(rx+8,ry+3,a->category,AL_ACCENT,AL_CAT_ACT);
    ry+=32;

    // Description heading + text
    vesa_draw_string(rx,ry,"Description",AL_TEXTDIM,AL_BG); ry+=20;
    vesa_hline(rx,ry,rw,AL_BORDER); ry+=8;
    // Word-wrap long_desc
    {
        const char*d=a->long_desc;
        int max_chars=rw/8;
        int di=0, dlen=al_len(d);
        int rows=0;
        while(di<dlen&&rows<4){
            int cut=dlen-di;
            if(cut>max_chars) cut=max_chars;
            // find last space within cut
            if(di+cut<dlen){
                int ls=0;
                for(int i=0;i<cut;i++) if(d[di+i]==' ') ls=i;
                if(ls>0) cut=ls;
            }
            char line[80]; int lp=0;
            for(int i=0;i<cut&&lp<79;i++) line[lp++]=d[di+i];
            line[lp]='\0';
            vesa_draw_string(rx,ry,line,AL_TEXT,AL_BG);
            ry+=20; di+=cut; if(d[di]==' ')di++;
            rows++;
        }
    }
    ry+=12;

    // Features heading + grid
    vesa_draw_string(rx,ry,"Features",AL_TEXTDIM,AL_BG); ry+=20;
    vesa_hline(rx,ry,rw,AL_BORDER); ry+=10;
    int fw2=rw/2-8;
    for(int fi=0;fi<4;fi++){
        if(!a->features[fi]) continue;
        int fx2=rx+(fi%2)*(fw2+16);
        int fy2=ry+(fi/2)*28;
        // Pill background
        int pill_w=al_len(a->features[fi])*8+20;
        vesa_fill_rect(fx2,fy2,pill_w,22,AL_CARD);
        vesa_draw_rect(fx2,fy2,pill_w,22,AL_BORDER);
        // Bullet dot
        vesa_fill_rect(fx2+6,fy2+9,4,4,AL_ACCENT);
        vesa_draw_string(fx2+14,fy2+3,a->features[fi],AL_TEXT,AL_CARD);
    }
    ry+=80;

    // ── Launch button ─────────────────────────────────────────────
    int btn_w=200, btn_h=44;
    int bx=rx, by=H-btn_h-32;
    vesa_fill_rect(bx,by,btn_w,btn_h,0x1A5A20);
    vesa_fill_rect(bx,by,btn_w,1,vcol_add(0x1A5A20,+32));
    vesa_draw_rect(bx,by,btn_w,btn_h,0x40CC50);
    vesa_draw_string(bx+(btn_w-al_len("  ENTER  Launch  ")*8)/2,
                     by+(btn_h-16)/2,
                     "  ENTER  Launch  ",0x88FF88,0x1A5A20);

    // ESC hint
    vesa_draw_string(bx+btn_w+16,by+(btn_h-16)/2,"ESC  back",AL_TEXTDIM,AL_BG);
}

// Main grid view
static void draw_vesa_grid(){
    int W=(int)vesa.width, H=(int)vesa.height;

    // ── Window position (preserves dragged position) ──────────────
    int wx = (g_win_x >= 0) ? g_win_x : 16;
    int wy = (g_win_y >= 0) ? g_win_y : 40;
    int ww = W - 32;
    int wh = H - wy;
    g_win_x = wx; g_win_y = wy; g_win_w = ww; g_win_h = wh;

    // Full backdrop so the drag trail shows a solid panel
    vesa_fill_rect(wx, wy, ww, wh, AL_BG);

    // ── Title bar
    vesa_fill_rect(wx, wy, ww, 36, AL_TITLEBAR);
    vesa_hline(wx, wy + 36, ww, AL_BORDER);
    vesa_draw_string(wx + 16, wy + 10, "App Library", AL_TEXT, AL_TITLEBAR);
    // app count badge
    {
        char cnt[8]; int p=0;
        int n=filtered_n;
        if(n==0){cnt[p++]='0';}
        else{char t[8];int tp=0;while(n>0){t[tp++]='0'+n%10;n/=10;}for(int i=tp-1;i>=0;i--)cnt[p++]=t[i];}
        cnt[p]='\0';
        int bx = wx + 16 + 96;
        vesa_fill_rect(bx, wy + 8, p*8+16, 20, AL_CAT_ACT);
        vesa_draw_rect(bx, wy + 8, p*8+16, 20, AL_SEL_BOR);
        vesa_draw_string(bx+8, wy + 11, cnt, AL_ACCENT, AL_CAT_ACT);
    }
    draw_close_btn(wx + ww - 40, wy + 10, AL_TITLEBAR, "App Library");
    vesa_draw_string(wx + ww - 112, wy + 10, "ESC  back", AL_TEXTDIM, AL_TITLEBAR);

    // ── Category tabs (offset by window position)
    int CY = wy + 36;
    int tab_w = ww;
    vesa_fill_rect(wx, CY, tab_w, 40, AL_CATBAR);
    vesa_hline(wx, CY + 40, tab_w, AL_BORDER);
    {
        int tx = wx + 16;
        for(int i=0;i<CAT_COUNT;i++){
            int cl=al_len(cats[i]);
            int tw=cl*8+20;
            if(i==sel_cat){
                vesa_fill_rect(tx-2, CY+4, tw+4, 32, AL_CAT_ACT);
                vesa_draw_rect(tx-2, CY+4, tw+4, 32, AL_SEL_BOR);
                vesa_hline(tx-2, CY+35, tw+4, AL_ACCENT);
                vesa_draw_string(tx+8, CY+12, cats[i], AL_TEXT, AL_CAT_ACT);
            } else {
                int tab_hover = mouse_in_rect(tx-2, CY+4, tw+4, 32);
                if (tab_hover) {
                    vesa_fill_rect(tx-2, CY+4, tw+4, 32, 0x2A2060);
                    vesa_draw_rect(tx-2, CY+4, tw+4, 32, 0x6A4FBF);
                    vesa_draw_string(tx+8, CY+12, cats[i], 0xE0D8FF, 0x2A2060);
                } else {
                    vesa_draw_string(tx+8, CY+12, cats[i], AL_TEXTDIM, AL_CATBAR);
                }
            }
            tx+=tw+8;
        }
    }

    // ── Search bar (offset by window position)
    int SY = wy + 76 + 6;
    {
        vesa_draw_string(wx + 16, SY + 4, "Search:", AL_TEXTDIM, AL_BG);
        int sx = wx + 72, sw = ww - 100;
        if (sw < 120) sw = 120;
        uint32_t sbg=srch_mode?0x1A1840:0x100E28;
        vesa_fill_rect(sx, SY, sw, 28, sbg);
        vesa_draw_rect(sx, SY, sw, 28, srch_mode?AL_SEL_BOR:AL_BORDER);
        char show[34];
        int si=0;
        for(;srch[si]&&si<32;si++) show[si]=srch[si];
        if(srch_mode){ show[si]='_'; si++; }
        show[si]='\0';
        vesa_draw_string(sx+8, SY+6, show, srch_mode?AL_TEXT:AL_TEXTDIM, sbg);
        if(!srch_mode)
            vesa_draw_string(sx+sw+12, SY+6, "/ to search", AL_TEXTDIM, AL_BG);
    }

    // ── App grid (4 cols × 3 rows visible, offset by window position)
    int COLS=4, VIS_ROWS=3;
    int GY = SY + 36;
    int GH = (wy + wh) - GY - 36;
    int card_gap=8;
    int card_w = (ww - 16 - (COLS-1)*card_gap) / COLS;
    int card_h = (GH - (VIS_ROWS-1)*card_gap) / VIS_ROWS;

    for(int row=0;row<VIS_ROWS;row++){
        for(int col=0;col<COLS;col++){
            int fidx=(al_scroll+row)*COLS+col;
            if(fidx>=filtered_n) break;
            int cx2 = wx + 8 + col*(card_w+card_gap);
            int cy2 = GY + row*(card_h+card_gap);
            draw_vesa_card(cx2, cy2, card_w, card_h, fidx);
        }
    }

    // No results
    if(filtered_n==0){
        int mx = wx + ww/2 - 120;
        int my = GY + GH/2 - 24;
        vesa_draw_string(mx, my,    "No apps found", AL_TEXTDIM, AL_BG);
        vesa_draw_string(mx, my+24, "Try a different search or category", AL_TEXTDIM, AL_BG);
    }

    // ── Scrollbar (offset by window position)
    int total_rows=(filtered_n+COLS-1)/COLS;
    if(total_rows>VIS_ROWS){
        int sb_h=GH;
        int track_x = wx + ww - 8;
        vesa_vline(track_x, GY, sb_h, AL_BORDER);
        int bar_h=sb_h*VIS_ROWS/total_rows;
        int bar_y=GY+(sb_h-bar_h)*al_scroll/(total_rows-VIS_ROWS);
        vesa_fill_rect(track_x-2, bar_y, 4, bar_h, AL_ACCENT);
    }

    // ── Bottom bar (offset by window position)
    int by = wy + wh - 36;
    vesa_fill_rect(wx, by, ww, 36, AL_TITLEBAR);
    vesa_hline(wx, by, ww, AL_BORDER);
    vesa_draw_string(wx + 16, by + 10,
        "Arrows: navigate    ENTER: open    TAB: category    /: search    ESC: back",
        AL_TEXTDIM, AL_TITLEBAR);
}

// ─── Public draw entry point ──────────────────────────────────────
void applib_draw(){
    // Keep the installed-package list current on every frame: the
    // library can be re-entered via alt-tab or the window manager
    // without applib_init(), and packages can be installed/removed
    // from the Terminal while the library stays open.  The scan is a
    // 127-slot filesystem walk — negligible vs. the pixel drawing.
    refresh_pkgs();
    build_filter();
    if(sel_app>=filtered_n&&filtered_n>0) sel_app=filtered_n-1;
    ensure_scroll();

    if(vesa.active){
        if(view_mode==1&&filtered_n>0) draw_vesa_detail(sel_app);
        else draw_vesa_grid();
        return;
    }

    // ── VGA fallback ──────────────────────────────────────────────
    if(view_mode==1&&filtered_n>0){
        const AppEntry*a=entry_at(filtered[sel_app]);
        vf(0,0,VGA_WIDTH,VGA_HEIGHT,' ',0,0);
        vfr(0,0,a->color);
        vs(2,0,"[ App Library > ",15,a->color);
        vs(18,0,a->name,15,a->color);
        vs(72,0,"ESC",7,a->color);
        // info
        vs(2,2,"Name:    ",8,0); vs(12,2,a->name,a->color,0);
        vs(2,3,"Version: ",8,0); vs(12,3,a->version,15,0);
        vs(2,4,"Category:",8,0); vs(12,4,a->category,14,0);
        vs(2,5,"Author:  ",8,0); vs(12,5,a->author,11,0);
        vs(2,7,"Description:",8,0);
        // Wrap desc across 3 lines
        {
            const char*d=a->long_desc; int di=0,dlen=al_len(d);
            for(int row=0;row<3&&di<dlen;row++){
                int cut=dlen-di; if(cut>74) cut=74;
                if(di+cut<dlen){ int ls=0;for(int i=0;i<cut;i++)if(d[di+i]==' ')ls=i;if(ls>0)cut=ls;}
                char ln[80];int lp=0;
                for(int i=0;i<cut&&lp<79;i++) ln[lp++]=d[di+i]; ln[lp]='\0';
                vs(2,8+row,ln,7,0); di+=cut; if(d[di]==' ')di++;
            }
        }
        vs(2,12,"Features:",8,0);
        for(int fi=0;fi<4;fi++) if(a->features[fi]) vs(2+fi*20,13,a->features[fi],15,0);
        vfr(VGA_HEIGHT-1,0,10);
        vs(25,VGA_HEIGHT-1,"  ENTER: Launch   ESC: back  ",0,10);
        return;
    }

    // VGA grid
    vf(0,0,VGA_WIDTH,VGA_HEIGHT,' ',0,0);
    vfr(0,0,1);
    vs(2,0,"[ App Library ]",15,1);
    vfr(1,8,0);
    int tx=0;
    for(int i=0;i<CAT_COUNT;i++){
        uint8_t fg=(i==sel_cat)?0:15, bg=(i==sel_cat)?11:8;
        vs(tx+1,1,cats[i],fg,bg); tx+=al_len(cats[i])+2;
    }
    // Search
    vs(2,2,"/ search: [",8,0);
    vf(13,2,20,1,' ',0,srch_mode?14:8);
    vs(13,2,srch,0,srch_mode?14:8);
    vs(33,2,"]",8,0);

    int card_w=38, card_h=6, cols=2;
    for(int row=0;row<4;row++){
        for(int col=0;col<cols;col++){
            int fi=(al_scroll+row)*cols+col;
            if(fi>=filtered_n) break;
            int app_idx=filtered[fi];
            const AppEntry*a=entry_at(app_idx);
            int cx=1+col*(card_w+2), cy=3+row*card_h;
            uint8_t bg=(fi==sel_app)?1:0;
            vf(cx,cy,card_w,card_h,' ',15,bg);
            for(int xi=cx;xi<cx+card_w;xi++){vp(xi,cy,'-',a->color,bg);vp(xi,cy+card_h-1,'-',a->color,bg);}
            for(int yi=cy;yi<cy+card_h;yi++){vp(cx,yi,'|',a->color,bg);vp(cx+card_w-1,yi,'|',a->color,bg);}
            vs(cx+2,cy+1,a->name,  (fi==sel_app)?15:a->color,bg);
            vs(cx+2,cy+2,a->desc,  7,bg);
            vs(cx+2,cy+3,a->category,8,bg);
        }
    }
    vfr(VGA_HEIGHT-1,0,8);
    vs(2,VGA_HEIGHT-1,"Arrows:nav  ENTER:open  TAB:cat  /:search  ESC:back",7,8);
}

// Launch the currently selected entry.  Built-ins set
// applib_launch_request (handled by desktop.c's do_launch); installed
// packages set applib_pkg_launch so desktop.c can pkg_run() them.
static void request_launch(int fidx){
    int app_idx=filtered[fidx];
    if(app_idx < APP_COUNT){
        applib_pkg_launch[0]='\0';
        applib_launch_request=apps[app_idx].launch_id;
    } else {
        applib_launch_request=-1;
        int pi=app_idx-APP_COUNT;
        int k=0;
        for(; k<PKG_NAME_MAX-1 && pkg_names[pi][k]; k++)
            applib_pkg_launch[k]=pkg_names[pi][k];
        applib_pkg_launch[k]='\0';
    }
}

// ─── Key handler ─────────────────────────────────────────────────
void applib_handle_key(char c){
    int COLS = vesa.active ? 4 : 2;
    int VIS  = vesa.active ? 3 : 4;

    // Detail view
    if(view_mode==1){
        if(c==27){ view_mode=0; return; }
        if(c==13&&filtered_n>0){
            request_launch(sel_app);
            return;
        }
        if(c=='C'||c=='B'){ sel_app=(sel_app+1)%filtered_n; return; }
        if(c=='D'||c=='A'){ sel_app=(sel_app+filtered_n-1)%filtered_n; return; }
        return;
    }

    // Search mode
    if(srch_mode){
        if(c==27||c==13){ srch_mode=0; build_filter(); sel_app=0; al_scroll=0; return; }
        if(c==8&&srch_pos>0){ srch[--srch_pos]='\0'; build_filter(); sel_app=0; al_scroll=0; return; }
        if(c>=32&&c<127&&srch_pos<30){
            srch[srch_pos++]=c; srch[srch_pos]='\0';
            build_filter(); sel_app=0; al_scroll=0; return;
        }
        return;
    }

    // Grid navigation
    if(c=='C'){  /* right */
        if(sel_app+1<filtered_n){ sel_app++; ensure_scroll(); }
         return;
    }
    if(c=='D'){  /* left */
        if(sel_app>0){ sel_app--; ensure_scroll(); }
         return;
    }
    if(c=='B'){  /* down */
        if(sel_app+COLS<filtered_n){ sel_app+=COLS; ensure_scroll(); }
         return;
    }
    if(c=='A'){  /* up */
        if(sel_app-COLS>=0){ sel_app-=COLS; ensure_scroll(); }
         return;
    }

    // TAB = next category
    if(c=='\t'){
        sel_cat=(sel_cat+1)%CAT_COUNT;
        sel_app=0; al_scroll=0; build_filter();
         return;
    }

    // / = search
    if(c=='/'){
        srch_mode=1; srch[0]='\0'; srch_pos=0;
        return;
    }

    // ENTER = run installed packages immediately; built-in apps open
    // their detail view first (ENTER there launches them too).
    if(c==13&&filtered_n>0){
        if(filtered[sel_app] >= APP_COUNT){
            request_launch(sel_app);
            return;
        }
        view_mode=1; 
        return;
    }

    // Page scroll shortcuts
    if(c==']'){ /* page down */
        int total=(filtered_n+COLS-1)/COLS;
        if(al_scroll+VIS<total){ al_scroll++; sel_app=al_scroll*COLS; }
        return;
    }
    if(c=='['){ /* page up */
        if(al_scroll>0){ al_scroll--; sel_app=al_scroll*COLS; }
        return;
    }
}

// ═══════════════════════════════════════════════════════════════════
//  MOUSE HANDLER
// ═══════════════════════════════════════════════════════════════════

int applib_handle_mouse(int mx, int my) {
    if (!vesa.active) return 0;

    int W = (int)vesa.width, H = (int)vesa.height;
    int CY = 36;
    int SY = 82; // 76+6

    // ── Detail view ─────────────────────────────────────────────
    if (view_mode == 1 && filtered_n > 0) {
        int lx = 24, lw = 280;
        int rx = lx + lw + 16;
        int btn_w = 200, btn_h = 44;
        int bx = rx, by = H - btn_h - 32;

        // Launch button
        if (mx >= bx && mx < bx + btn_w && my >= by && my < by + btn_h) {
            request_launch(sel_app);
            return 1;
        }

        // Click anywhere else = back to grid
        view_mode = 0;
        return 1;
    }

    // ── Grid view ───────────────────────────────────────────────

    // Category tab clicks
    int tx = 16;
    for (int i = 0; i < CAT_COUNT; i++) {
        int cl = al_len(cats[i]);
        int tw = cl * 8 + 20;
        // Each tab is at (tx-2, CY+4, tw+4, 32)
        if (mx >= tx - 2 && mx < tx + tw + 2 && my >= CY + 4 && my < CY + 36) {
            // Avoid re-selecting same category
            if (i == sel_cat && srch_mode) {
                // Toggle search off
                srch_mode = 0;
                build_filter();
                sel_app = 0; al_scroll = 0;
            }
            sel_cat = i;
            sel_app = 0; al_scroll = 0;
            build_filter();
            return 1;
        }
        tx += tw + 8;
    }

    // Search bar click
    int sx = 72, sw = 280;
    if (my >= SY && my < SY + 28 && mx >= sx && mx < sx + sw) {
        srch_mode = 1;
        srch[0] = '\0'; srch_pos = 0;
        build_filter();
        sel_app = 0; al_scroll = 0;
        return 1;
    }

    // App grid clicks
    int COLS = 4, VIS_ROWS = 3;
    int GY = SY + 36;
    int GH = H - GY - 36;
    int card_gap = 8;
    int card_w = (W - 16 - (COLS - 1) * card_gap) / COLS;
    int card_h = (GH - (VIS_ROWS - 1) * card_gap) / VIS_ROWS;

    for (int row = 0; row < VIS_ROWS; row++) {
        for (int col = 0; col < COLS; col++) {
            int fidx = (al_scroll + row) * COLS + col;
            if (fidx >= filtered_n) break;
            int cx2 = 8 + col * (card_w + card_gap);
            int cy2 = GY + row * (card_h + card_gap);
            if (mx >= cx2 && mx < cx2 + card_w && my >= cy2 && my < cy2 + card_h) {
                sel_app = fidx;
                view_mode = 1;  // open detail view (same as ENTER)
                return 1;
            }
        }
    }

    return 0;
}

void applib_init(){
    sel_app=0; sel_cat=0; al_scroll=0;
    view_mode=0; srch_mode=0;
    srch[0]='\0'; srch_pos=0;
    applib_launch_request=-1;
    applib_pkg_launch[0]='\0';
    refresh_pkgs();
    build_filter();
}

void applib_refresh(void){
    refresh_pkgs();
    build_filter();
    if(sel_app>=filtered_n&&filtered_n>0) sel_app=filtered_n-1;
}
