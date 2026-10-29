// cpu/sysmon.c  –  Nyx OS System Monitor  (VGA + VESA)
#include <sysmon.h>
#include <vesa.h>
#include <rtc.h>
#include <speaker.h>
#include <stdint.h>
#include <desktop.h>
#include <mouse.h>
#include <task.h>

extern void vesa_draw_char_scaled(int x,int y,char c,uint32_t fg,uint32_t bg,int scale);
extern int file_count;

// ═══════════════════════════════════════════════════════════════════
//  SHARED DATA
// ═══════════════════════════════════════════════════════════════════

typedef struct {
    int         pid;
    const char* name;
    const char* status;
    int         cpu;
    int         mem;
    uint8_t     color;
} Process;

static Process processes[] = {
    {1,  "kernel",      "Running", 12, 256,  10},
    {2,  "desktop",     "Running",  8, 128,  11},
    {3,  "keyboard",    "Waiting",  1,  32,   7},
    {4,  "timer",       "Running",  5,  16,  14},
    {5,  "speaker",     "Idle",     0,   8,   8},
    {6,  "rtc",         "Running",  2,   8,  13},
    {7,  "music",       "Idle",     0,  16,   5},
    {8,  "filesystem",  "Waiting",  1,  64,   9},
    {9,  "shell",       "Waiting",  0,  32,   7},
    {10, "splash",      "Stopped",  0,  48,   8},
    {11, "login",       "Stopped",  0,  24,   8},
    {12, "wallpaper",   "Running",  3,  16,  10},
};
#define PROC_COUNT 12

#define GRAPH_W 60
static int cpu_history[GRAPH_W];
static int mem_history[GRAPH_W];
static int graph_pos = 0;

#define TAB_CPU   0
#define TAB_MEM   1
#define TAB_PROC  2
#define TAB_DISK  3
#define TAB_COUNT 4

static int cur_tab     = TAB_CPU;
static int proc_sel    = 0;
static int tick_count  = 0;
static int uptime_secs = 0;
static int cpu_usage   = 0;
static int mem_used    = 0;
static int mem_total   = 1024*1024;   // matches QEMU -m 1G
static int rand_seed   = 54321;

static int rand_next(){rand_seed=rand_seed*1103515245+12345;return(rand_seed>>16)&0x7FFF;}

static void update_stats(){
    int delta=(rand_next()%11)-5;
    cpu_usage+=delta; if(cpu_usage<5)cpu_usage=5; if(cpu_usage>95)cpu_usage=95;
    mem_used=32768+(uptime_secs*10)+(rand_next()%1024);
    if(mem_used>mem_total/2)mem_used=mem_total/4;
    for(int i=0;i<PROC_COUNT;i++){
        if(processes[i].cpu>0){
            processes[i].cpu+=(rand_next()%5)-2;
            if(processes[i].cpu<0)processes[i].cpu=0;
            if(processes[i].cpu>30)processes[i].cpu=30;
        }
    }
    cpu_history[graph_pos]=cpu_usage;
    mem_history[graph_pos]=mem_used*100/mem_total;
    graph_pos=(graph_pos+1)%GRAPH_W;
}

// ═══════════════════════════════════════════════════════════════════
//  STRING HELPERS
// ═══════════════════════════════════════════════════════════════════

static void n2s(int n,char*buf){
    if(n==0){buf[0]='0';buf[1]='\0';return;}
    char t[12];int p=0;while(n>0){t[p++]='0'+n%10;n/=10;}
    for(int i=0;i<p;i++)buf[i]=t[p-1-i];buf[p]='\0';
}
static int s_len(const char*s){int i=0;while(s[i])i++;return i;}

static void uptime_str(char*buf){
    int h=uptime_secs/3600,m=(uptime_secs%3600)/60,s=uptime_secs%60;
    int p=0;char tmp[4];
    n2s(h,tmp);for(int i=0;tmp[i];i++)buf[p++]=tmp[i];buf[p++]='h';buf[p++]=' ';
    n2s(m,tmp);for(int i=0;tmp[i];i++)buf[p++]=tmp[i];buf[p++]='m';buf[p++]=' ';
    n2s(s,tmp);for(int i=0;tmp[i];i++)buf[p++]=tmp[i];buf[p++]='s';buf[p]='\0';
}

// ═══════════════════════════════════════════════════════════════════
//  VGA FALLBACK
// ═══════════════════════════════════════════════════════════════════

#define VGA_WIDTH  80
#define VGA_HEIGHT 25
#define VGA_MEM    ((uint16_t*)0xB8000)

static inline uint16_t make_entry(char c,uint8_t fg,uint8_t bg){return(uint16_t)c|(uint16_t)(fg|bg<<4)<<8;}
static void sput(int x,int y,char c,uint8_t fg,uint8_t bg){if(x>=0&&x<VGA_WIDTH&&y>=0&&y<VGA_HEIGHT)VGA_MEM[y*VGA_WIDTH+x]=make_entry(c,fg,bg);}
static void sputs(int x,int y,const char*s,uint8_t fg,uint8_t bg){for(int i=0;s[i];i++)sput(x+i,y,s[i],fg,bg);}
static void sfill(int x,int y,int w,int h,char c,uint8_t fg,uint8_t bg){for(int dy=0;dy<h;dy++)for(int dx=0;dx<w;dx++)sput(x+dx,y+dy,c,fg,bg);}
static void sfill_row(int y,char c,uint8_t fg,uint8_t bg){for(int x=0;x<VGA_WIDTH;x++)sput(x,y,c,fg,bg);}

static void vga_draw_bar(int x,int y,int w,int val,int max,uint8_t color){
    int filled=val*w/(max?max:1);if(filled>w)filled=w;
    for(int i=0;i<w;i++)sput(x+i,y,' ',0,(i<filled)?color:8);
}
static void vga_draw_graph(int x,int y,int w,int h,int*data,int count,uint8_t color,const char*label){
    for(int i=x;i<x+w;i++){sput(i,y,'-',8,0);sput(i,y+h+1,'-',8,0);}
    for(int i=y;i<=y+h+1;i++){sput(x-1,i,'|',8,0);sput(x+w,i,'|',8,0);}
    sputs(x,y,label,color,0);
    for(int i=0;i<count&&i<w;i++){
        int idx=(graph_pos-count+i+GRAPH_W)%GRAPH_W;
        int val=data[idx];int bar=val*h/100;if(bar>h)bar=h;
        for(int j=0;j<h;j++){int gy=y+h-j;sput(x+i,gy,j<bar?'#':'.',j<bar?color:8,0);}
    }
}

static void vga_cpu_tab(){
    sputs(3,3,"CPU Usage:",11,0);
    char pct[8];n2s(cpu_usage,pct);int pp=0;while(pct[pp])pp++;pct[pp++]='%';pct[pp]='\0';
    sputs(3,4,pct,cpu_usage>80?12:(cpu_usage>50?14:10),0);
    vga_draw_bar(3,5,50,cpu_usage,100,cpu_usage>80?4:(cpu_usage>50?6:2));
    sputs(56,3,"CPU Info:",11,0);
    sputs(56,4,"Arch: x86 32bit",7,0);sputs(56,5,"Mode: Protected",7,0);
    sputs(56,6,"Freq: ~1GHz",7,0);sputs(56,7,"Cores: 1",7,0);
    vga_draw_graph(3,7,40,10,cpu_history,40,2,"CPU History");
    sputs(3,19,"Processes:",11,0);char pc[4];n2s(PROC_COUNT,pc);sputs(14,19,pc,15,0);
    sputs(20,19,"Running:",11,0);sputs(29,19,"4",10,0);
    sputs(35,19,"Waiting:",11,0);sputs(44,19,"3",14,0);
    sputs(50,19,"Idle:",11,0);sputs(56,19,"5",8,0);
    sputs(3,20,"Uptime:",11,0);char ut[32];uptime_str(ut);sputs(11,20,ut,14,0);
    char ts[9];rtc_get_time_str(ts);sputs(3,21,"Time:",11,0);sputs(9,21,ts,15,0);
}
static void vga_mem_tab(){
    int mem_pct=mem_used*100/mem_total;
    sputs(3,3,"Memory Usage:",11,0);
    char pct[8];n2s(mem_pct,pct);int pp=0;while(pct[pp])pp++;pct[pp++]='%';pct[pp]='\0';
    sputs(3,4,pct,mem_pct>80?12:(mem_pct>50?14:10),0);
    vga_draw_bar(3,5,50,mem_pct,100,mem_pct>80?4:(mem_pct>50?6:2));
    sputs(3,7,"Total  :",11,0);sputs(3,8,"Used   :",11,0);
    sputs(3,9,"Free   :",11,0);sputs(3,10,"Cached :",11,0);
    char t1[12],t2[12],t3[12],t4[12];
    n2s(mem_total/1024,t1);sputs(12,7,t1,15,0);sputs(16,7,"MB",7,0);
    n2s(mem_used/1024,t2);sputs(12,8,t2,12,0);sputs(16,8,"MB",7,0);
    n2s((mem_total-mem_used)/1024,t3);sputs(12,9,t3,10,0);sputs(16,9,"MB",7,0);
    n2s(16,t4);sputs(12,10,t4,14,0);sputs(16,10,"MB",7,0);
    vga_draw_graph(3,12,40,8,mem_history,40,6,"Mem History");
    sputs(56,3,"Memory Map:",11,0);
    sputs(56,4,"[Kernel ]",2,0);sputs(56,5,"[Desktop]",3,0);
    sputs(56,6,"[Apps   ]",5,0);sputs(56,7,"[Stack  ]",14,0);
    sputs(56,8,"[Heap   ]",9,0);sputs(56,9,"[VGA    ]",4,0);
    sputs(56,10,"[Free   ]",8,0);
}
static void vga_proc_tab(){
    sfill(3,3,74,1,' ',0,1);
    sputs(3,3," PID",15,1);sputs(8,3,"Name",15,1);sputs(22,3,"Status",15,1);
    sputs(34,3,"CPU%",15,1);sputs(42,3,"Memory",15,1);sputs(52,3,"Priority",15,1);
    for(int i=0;i<PROC_COUNT&&i<18;i++){
        Process*p=&processes[i];uint8_t fg=(i==proc_sel)?0:p->color,bg=(i==proc_sel)?14:0;
        sfill(3,4+i,74,1,' ',fg,bg);
        char pid[6];n2s(p->pid,pid);sputs(3,4+i,pid,fg,bg);
        sputs(8,4+i,p->name,fg,bg);sputs(22,4+i,p->status,fg,bg);
        char cpu[6];n2s(p->cpu,cpu);int cp=0;while(cpu[cp])cp++;cpu[cp++]='%';cpu[cp]='\0';
        sputs(34,4+i,cpu,fg,bg);
        char mem[10];n2s(p->mem,mem);int mp=0;while(mem[mp])mp++;mem[mp++]='K';mem[mp++]='B';mem[mp]='\0';
        sputs(42,4+i,mem,fg,bg);
        sputs(52,4+i,(p->pid<=4)?"High":(p->pid<=8)?"Normal":"Low",fg,bg);
    }
    sfill(3,22,74,1,' ',0,8);
    sputs(3,22,"UP/DOWN:select",15,8);sputs(20,22,"K:kill process",12,8);
    sputs(37,22,"R:restart",10,8);sputs(50,22,"S:suspend",14,8);sputs(63,22,"ESC:back",7,8);
    Process*sp=&processes[proc_sel];
    sfill(3,23,74,1,' ',0,1);
    sputs(3,23,"Selected: ",15,1);sputs(13,23,sp->name,14,1);
    sputs(30,23,"PID:",11,1);char spid[6];n2s(sp->pid,spid);sputs(35,23,spid,15,1);
    sputs(42,23,"Status:",11,1);sputs(50,23,sp->status,10,1);
}
static void vga_disk_tab(){
    sputs(3,3,"Disk / Storage:",11,0);sputs(3,5,"Virtual Filesystem (RAM-based):",11,0);
    sputs(3,7,"Files stored :",11,0);char fc[8];n2s(file_count,fc);sputs(18,7,fc,15,0);
    sputs(3,8,"Max files    :",11,0);sputs(18,8,"16",15,0);
    sputs(3,9,"File size max:",11,0);sputs(18,9,"512 bytes",15,0);
    sputs(3,10,"Total storage:",11,0);sputs(18,10,"8192 bytes",15,0);
    sputs(3,12,"Storage used:",11,0);vga_draw_bar(3,13,40,file_count,16,10);
    char up[8];n2s(file_count*100/16,up);int pp=0;while(up[pp])pp++;up[pp++]='%';up[pp]='\0';
    sputs(45,13,up,10,0);
    sputs(3,15,"Loaded Modules:",11,0);
    const char*modules[]={"gdt.o    - Global Descriptor Table","idt.o    - Interrupt Descriptor Table",
        "isr.o    - Interrupt Service Routines","keyboard - PS/2 Keyboard Driver",
        "rtc.o    - Real Time Clock","speaker  - PC Speaker Driver","vga.o    - VGA Text Mode Driver"};
    for(int i=0;i<7;i++)sputs(3,16+i,modules[i],7,0);
    sputs(50,5,"System Info:",11,0);sputs(50,6,"OS: Nyx OS v0.1",15,0);
    sputs(50,7,"Arch: x86 32-bit",15,0);sputs(50,8,"Boot: Multiboot",15,0);
    sputs(50,9,"GDT: 3 segments",15,0);sputs(50,10,"IDT: 256 entries",15,0);
    sputs(50,11,"IRQ: 16 lines",15,0);
}
static void vga_draw(){
    sfill(0,0,VGA_WIDTH,VGA_HEIGHT,' ',0,0);
    sfill_row(0,' ',0,1);sputs(2,0,"[ System Monitor ]",15,1);
    char ts[9];rtc_get_time_str(ts);sputs(60,0,ts,14,1);sputs(70,0,"ESC:back",7,1);
    sfill_row(2,' ',0,8);
    const char*tabs[]={"  CPU  ","  Memory  ","  Processes  ","  Disk  "};
    int tx=2;
    for(int i=0;i<TAB_COUNT;i++){
        uint8_t fg=(i==cur_tab)?0:15,bg=(i==cur_tab)?14:8;
        sputs(tx,2,tabs[i],fg,bg);int tl=s_len(tabs[i]);tx+=tl+1;
    }
    sputs(tx+2,2,"TAB:switch",8,8);
    for(int x=0;x<VGA_WIDTH;x++)sput(x,22,'-',8,0);
    switch(cur_tab){
        case TAB_CPU:  vga_cpu_tab();  break;
        case TAB_MEM:  vga_mem_tab();  break;
        case TAB_PROC: vga_proc_tab(); break;
        case TAB_DISK: vga_disk_tab(); break;
    }
}

// ═══════════════════════════════════════════════════════════════════
//  VESA UI
// ═══════════════════════════════════════════════════════════════════

#define SM_BG      0x0E0C22
#define SM_TITLE   0x1A1440
#define SM_TAB_ACT 0x352878
#define SM_TAB_IN  0x14122A
#define SM_BORDER  0x2B2448
#define SM_DIVIDER 0x201D38
#define SM_TEXT    0xE0D8FF
#define SM_DIM     0x6858A0
#define SM_ACCENT  0x9B7FFF
#define SM_SUCCESS 0x55EE88
#define SM_DANGER  0xFF6655
#define SM_WARN    0xFFAA00
#define SM_HINT    0x504870
#define SM_CARD    0x1A1838

static const uint32_t VRGB[16]={
    0x000000,0x0000AA,0x00AA00,0x00AAAA,
    0xAA0000,0xAA00AA,0xAA5500,0xAAAAAA,
    0x555555,0x5555FF,0x55FF55,0x55FFFF,
    0xFF5555,0xFF55FF,0xFFFF55,0xFFFFFF
};

static uint32_t sm_mix(uint32_t c,int d){
    int r=(int)(c>>16&0xFF)+d;if(r<0)r=0;if(r>255)r=255;
    int g=(int)(c>>8 &0xFF)+d;if(g<0)g=0;if(g>255)g=255;
    int b=(int)(c    &0xFF)+d;if(b<0)b=0;if(b>255)b=255;
    return((uint32_t)r<<16)|((uint32_t)g<<8)|(uint32_t)b;
}
static void sm_str_r(int rx,int y,const char*s,uint32_t f,uint32_t b){
    vesa_draw_string(rx-s_len(s)*8,y,s,f,b);
}
static void sm_pill(int x,int y,const char*s,uint32_t f,uint32_t b){
    int w=s_len(s)*8+16;
    vesa_fill_rect(x,y,w,22,b);vesa_draw_rect(x,y,w,22,f);
    vesa_draw_string(x+8,y+3,s,f,b);
}
static void sm_num(int x,int y,int n,uint32_t f,uint32_t b){
    char buf[12];n2s(n,buf);vesa_draw_string(x,y,buf,f,b);
}
static void sm_bar(int x,int y,int w,int h,int pct,uint32_t filled,uint32_t empty){
    if(pct>100)pct=100;if(pct<0)pct=0;
    int f=w*pct/100;
    if(f>0)vesa_fill_rect(x,y,f,h,filled);
    if(f<w)vesa_fill_rect(x+f,y,w-f,h,empty);
    vesa_draw_rect(x,y,w,h,SM_BORDER);
}
// Line-plot graph from history data (values 0-100)
static void sm_graph(int x,int y,int w,int h,int*data,uint32_t color){
    vesa_fill_rect(x,y,w,h,0x0A081A);
    vesa_draw_rect(x,y,w,h,SM_BORDER);
    // Gridlines at 25/50/75%
    for(int g=1;g<4;g++){int gy=y+h-h*g/4;vesa_hline(x,gy,w,SM_DIVIDER);}
    int step=w/GRAPH_W; if(step<1)step=1;
    for(int i=0;i<GRAPH_W;i++){
        int idx=(graph_pos-GRAPH_W+i+GRAPH_W)%GRAPH_W;
        int val=data[idx]; if(val>100)val=100; if(val<0)val=0;
        int px=x+i*step;
        int py=y+h-(val*h/100);
        // Fill under the line down to the baseline (dimmed)
        vesa_vline(px,py,y+h-py,sm_mix(color,-100));
        // Bright cap at the data point
        vesa_fill_rect(px,py-1,step>0?step:1,2,color);
    }
}

// ── CPU Tab ────────────────────────────────────────────────────────
static void vk_cpu(int cx,int cy,int cw,int ch){
    (void)ch;
    vesa_fill_rect(cx,cy,cw,ch,SM_BG);
    uint32_t ccol=cpu_usage>80?SM_DANGER:(cpu_usage>50?SM_WARN:SM_SUCCESS);

    // Big usage card
    int card_h=100;
    vesa_fill_rect(cx+16,cy+12,cw-32,card_h,SM_CARD);
    vesa_draw_rect(cx+16,cy+12,cw-32,card_h,SM_BORDER);
    vesa_fill_rect(cx+16,cy+12,cw-32,3,ccol);
    vesa_draw_string(cx+32,cy+24,"CPU USAGE",SM_DIM,SM_CARD);
    char pct[6];n2s(cpu_usage,pct);int pp=0;while(pct[pp])pp++;pct[pp++]='%';pct[pp]=0;
    for(int i=0;pct[i];i++) vesa_draw_char_scaled(cx+32+i*24,cy+44,pct[i],ccol,SM_CARD,3);
    sm_bar(cx+32,cy+92,cw-96,14,cpu_usage,ccol,0x0C0A1C);

    // CPU info panel (right side of card)
    int ix=cx+cw-220;
    vesa_draw_string(ix,cy+24,"CPU Info",SM_DIM,SM_CARD);
    vesa_draw_string(ix,cy+44,"x86 32-bit",SM_TEXT,SM_CARD);
    vesa_draw_string(ix,cy+60,"Protected Mode",SM_TEXT,SM_CARD);
    vesa_draw_string(ix,cy+76,"~1 GHz  1 core",SM_TEXT,SM_CARD);

    // Graph
    int gy=cy+card_h+28;
    vesa_draw_string(cx+16,gy-16,"CPU History",SM_DIM,SM_BG);
    sm_graph(cx+16,gy,cw-32,110,cpu_history,ccol);

    // Stats row — REAL values from the multitasking scheduler
    int sy=gy+126;
    typedef struct{const char*label;int val;uint32_t col;}Stat;
    Stat stats[]={{"Tasks",task_count(),SM_TEXT},{"Ready",sched_ready_count(),SM_SUCCESS},
                  {"Waiting",sched_waiting_count(),SM_WARN},{"Blinks",(int)task_global_blinks,SM_ACCENT}};
    int scw=(cw-32)/4;
    for(int i=0;i<4;i++){
        int sx=cx+16+i*scw;
        vesa_fill_rect(sx,sy,scw-8,54,SM_CARD);
        vesa_draw_rect(sx,sy,scw-8,54,SM_BORDER);
        vesa_draw_string(sx+8,sy+8,stats[i].label,SM_DIM,SM_CARD);
        sm_num(sx+8,sy+28,stats[i].val,stats[i].col,SM_CARD);
    }

    // Uptime / time
    int uy=sy+68;
    char ut[32];uptime_str(ut);
    vesa_draw_string(cx+16,uy,"Uptime",SM_DIM,SM_BG);
    vesa_draw_string(cx+16,uy+16,ut,SM_WARN,SM_BG);
    char ts[9];rtc_get_time_str(ts);
    sm_str_r(cx+cw-16,uy,"Current Time",SM_DIM,SM_BG);
    sm_str_r(cx+cw-16,uy+16,ts,SM_ACCENT,SM_BG);

    // Real scheduler line: live task states + context-switch counter
    char schedbuf[64];
    sched_snapshot_str(schedbuf, sizeof schedbuf);
    vesa_draw_string(cx+16,uy+40,schedbuf,SM_ACCENT,SM_BG);
    vesa_draw_string(cx+16,uy+58,"preemptive round-robin @ 50 Hz",SM_HINT,SM_BG);
}

// ── Memory Tab ──────────────────────────────────────────────────────
static void vk_mem(int cx,int cy,int cw,int ch){
    (void)ch;
    vesa_fill_rect(cx,cy,cw,ch,SM_BG);
    int mem_pct=mem_used*100/mem_total;
    uint32_t mcol=mem_pct>80?SM_DANGER:(mem_pct>50?SM_WARN:SM_ACCENT);

    int card_h=100;
    vesa_fill_rect(cx+16,cy+12,cw-32,card_h,SM_CARD);
    vesa_draw_rect(cx+16,cy+12,cw-32,card_h,SM_BORDER);
    vesa_fill_rect(cx+16,cy+12,cw-32,3,mcol);
    vesa_draw_string(cx+32,cy+24,"MEMORY USAGE",SM_DIM,SM_CARD);
    char pct[6];n2s(mem_pct,pct);int pp=0;while(pct[pp])pp++;pct[pp++]='%';pct[pp]=0;
    for(int i=0;pct[i];i++) vesa_draw_char_scaled(cx+32+i*24,cy+44,pct[i],mcol,SM_CARD,3);
    sm_bar(cx+32,cy+92,cw-96,14,mem_pct,mcol,0x0C0A1C);

    // Details on right
    int ix=cx+cw-220;
    char t1[12],t2[12],t3[12];
    n2s(mem_total/1024,t1);n2s(mem_used/1024,t2);n2s((mem_total-mem_used)/1024,t3);
    vesa_draw_string(ix,cy+24,"Total",SM_DIM,SM_CARD);sm_str_r(ix+150,cy+24,t1,SM_TEXT,SM_CARD);
    vesa_draw_string(ix,cy+40,"Used",SM_DIM,SM_CARD); sm_str_r(ix+150,cy+40,t2,mcol,SM_CARD);
    vesa_draw_string(ix,cy+56,"Free",SM_DIM,SM_CARD); sm_str_r(ix+150,cy+56,t3,SM_SUCCESS,SM_CARD);
    vesa_draw_string(ix,cy+72,"Cached",SM_DIM,SM_CARD);sm_str_r(ix+150,cy+72,"16 MB",SM_WARN,SM_CARD);

    // Graph
    int gy=cy+card_h+28;
    vesa_draw_string(cx+16,gy-16,"Memory History",SM_DIM,SM_BG);
    sm_graph(cx+16,gy,cw-32,90,mem_history,mcol);

    // Memory map (segmented bar)
    int my=gy+108;
    vesa_draw_string(cx+16,my,"Memory Map",SM_DIM,SM_BG);my+=22;
    typedef struct{const char*name;int weight;uint32_t col;}Seg;
    Seg segs[]={{"Kernel",15,VRGB[2]},{"Desktop",10,VRGB[3]},{"Apps",15,VRGB[5]},
                {"Stack",10,VRGB[14]},{"Heap",10,VRGB[9]},{"VGA",5,VRGB[4]},{"Free",35,VRGB[8]}};
    int bw=cw-32,bx=cx+16;
    int total_w=0;for(int i=0;i<7;i++)total_w+=segs[i].weight;
    int xx=bx;
    for(int i=0;i<7;i++){
        int sw=bw*segs[i].weight/total_w;
        vesa_fill_rect(xx,my,sw,28,segs[i].col);
        xx+=sw;
    }
    vesa_draw_rect(bx,my,bw,28,SM_BORDER);
    my+=36;
    // Legend
    int lx=bx;
    for(int i=0;i<7;i++){
        vesa_fill_rect(lx,my,10,10,segs[i].col);
        vesa_draw_string(lx+14,my-3,segs[i].name,SM_DIM,SM_BG);
        lx+=14+s_len(segs[i].name)*8+16;
    }
}

// ── Processes Tab ────────────────────────────────────────────────
static void vk_proc(int cx,int cy,int cw,int ch){
    vesa_fill_rect(cx,cy,cw,ch,SM_BG);
    // Header row
    vesa_fill_rect(cx+8,cy+8,cw-16,28,0x14122A);
    vesa_draw_string(cx+20,cy+16,"PID",SM_DIM,0x14122A);
    vesa_draw_string(cx+80,cy+16,"Name",SM_DIM,0x14122A);
    vesa_draw_string(cx+220,cy+16,"Status",SM_DIM,0x14122A);
    vesa_draw_string(cx+340,cy+16,"CPU",SM_DIM,0x14122A);
    vesa_draw_string(cx+420,cy+16,"Memory",SM_DIM,0x14122A);
    sm_str_r(cx+cw-20,cy+16,"Priority",SM_DIM,0x14122A);

    int row_h=32,list_y=cy+40;
    for(int i=0;i<PROC_COUNT;i++){
        Process*p=&processes[i];
        int ry=list_y+i*row_h;
        if(ry+row_h>cy+ch-70)break;
        int sel=(i==proc_sel);
        uint32_t bg=sel?SM_TAB_ACT:(i%2==0?SM_BG:0x100E24);
        uint32_t fg=sel?SM_TEXT:0xA090C8;
        vesa_fill_rect(cx+8,ry,cw-16,row_h-2,bg);
        if(sel)vesa_fill_rect(cx+8,ry,4,row_h-2,SM_ACCENT);
        char pid[6];n2s(p->pid,pid);
        vesa_draw_string(cx+20,ry+8,pid,fg,bg);
        vesa_draw_string(cx+80,ry+8,p->name,fg,bg);
        uint32_t stcol = (p->status[0]=='R')?SM_SUCCESS:(p->status[0]=='W')?SM_WARN:(p->status[0]=='K')?SM_DANGER:SM_DIM;
        vesa_draw_string(cx+220,ry+8,p->status,sel?fg:stcol,bg);
        char cpu[6];n2s(p->cpu,cpu);int cp=0;while(cpu[cp])cp++;cpu[cp++]='%';cpu[cp]=0;
        vesa_draw_string(cx+340,ry+8,cpu,fg,bg);
        char mem[10];n2s(p->mem,mem);int mp=0;while(mem[mp])mp++;mem[mp++]='K';mem[mp++]='B';mem[mp]=0;
        vesa_draw_string(cx+420,ry+8,mem,fg,bg);
        const char*prio=(p->pid<=4)?"High":(p->pid<=8)?"Normal":"Low";
        uint32_t pcol=(p->pid<=4)?SM_DANGER:(p->pid<=8)?SM_WARN:SM_DIM;
        sm_str_r(cx+cw-20,ry+8,prio,sel?fg:pcol,bg);
    }

    // Detail card for selected process
    int dy=cy+ch-64;
    Process*sp=&processes[proc_sel];
    vesa_fill_rect(cx+8,dy,cw-16,52,SM_CARD);
    vesa_draw_rect(cx+8,dy,cw-16,52,SM_ACCENT);
    vesa_draw_string(cx+20,dy+8,"Selected:",SM_DIM,SM_CARD);
    vesa_draw_string(cx+108,dy+8,sp->name,SM_TEXT,SM_CARD);
    vesa_draw_string(cx+20,dy+28,"K",SM_DANGER,SM_CARD);vesa_draw_string(cx+34,dy+28,"kill",SM_DIM,SM_CARD);
    vesa_draw_string(cx+90,dy+28,"R",SM_SUCCESS,SM_CARD);vesa_draw_string(cx+104,dy+28,"restart",SM_DIM,SM_CARD);
    vesa_draw_string(cx+180,dy+28,"S",SM_WARN,SM_CARD);vesa_draw_string(cx+194,dy+28,"suspend",SM_DIM,SM_CARD);
    sm_str_r(cx+cw-20,dy+18,sp->status,
             (sp->status[0]=='R')?SM_SUCCESS:(sp->status[0]=='K')?SM_DANGER:SM_WARN,SM_CARD);
}

// ── Disk Tab ────────────────────────────────────────────────────
static void vk_disk(int cx,int cy,int cw,int ch){
    (void)ch;
    vesa_fill_rect(cx,cy,cw,ch,SM_BG);
    vesa_draw_string(cx+16,cy+12,"Disk / Storage — Virtual Filesystem (RAM-based)",SM_DIM,SM_BG);

    // Storage card
    int card_h=90;
    vesa_fill_rect(cx+16,cy+36,cw-32,card_h,SM_CARD);
    vesa_draw_rect(cx+16,cy+36,cw-32,card_h,SM_BORDER);
    vesa_draw_string(cx+32,cy+48,"Storage Used",SM_DIM,SM_CARD);
    int pct=file_count*100/16;
    char pcts[6];n2s(pct,pcts);int pp=0;while(pcts[pp])pp++;pcts[pp++]='%';pcts[pp]=0;
    for(int i=0;pcts[i];i++) vesa_draw_char_scaled(cx+32+i*24,cy+66,pcts[i],SM_SUCCESS,SM_CARD,2);
    sm_bar(cx+32,cy+100,cw-260,16,pct,SM_SUCCESS,0x0C0A1C);

    int ix=cx+cw-220;
    char fc[8];n2s(file_count,fc);
    vesa_draw_string(ix,cy+48,"Files",SM_DIM,SM_CARD);sm_str_r(ix+150,cy+48,fc,SM_TEXT,SM_CARD);
    vesa_draw_string(ix,cy+64,"Max",SM_DIM,SM_CARD);sm_str_r(ix+150,cy+64,"16",SM_TEXT,SM_CARD);
    vesa_draw_string(ix,cy+80,"Max size",SM_DIM,SM_CARD);sm_str_r(ix+150,cy+80,"512 B",SM_TEXT,SM_CARD);
    vesa_draw_string(ix,cy+96,"Total",SM_DIM,SM_CARD);sm_str_r(ix+150,cy+96,"8192 B",SM_TEXT,SM_CARD);

    // Two columns: modules + system info
    int col_y=cy+36+card_h+24;
    int col_w=(cw-48)/2;

    vesa_draw_string(cx+16,col_y-4,"Loaded Modules",SM_DIM,SM_BG);
    vesa_hline(cx+16,col_y+12,col_w,SM_DIVIDER);
    const char*modules[]={"gdt.o    Global Descriptor Table","idt.o    Interrupt Descriptor Table",
        "isr.o    Interrupt Service Routines","keyboard PS/2 Keyboard Driver",
        "rtc.o    Real Time Clock","speaker  PC Speaker Driver","vesa.o   VESA Framebuffer Driver"};
    int my=col_y+22;
    for(int i=0;i<7;i++){vesa_draw_string(cx+16,my,modules[i],0x9080C0,SM_BG);my+=20;}

    int rx=cx+32+col_w;
    vesa_draw_string(rx,col_y-4,"System Info",SM_DIM,SM_BG);
    vesa_hline(rx,col_y+12,col_w,SM_DIVIDER);
    typedef struct{const char*k,*v;uint32_t c;}KV;
    KV info[]={{"OS","Nyx OS v0.1",SM_TEXT},{"Arch","x86 32-bit",SM_TEXT},
               {"Boot","GRUB Multiboot",SM_TEXT},{"GDT","3 segments",SM_TEXT},
               {"IDT","256 entries",SM_TEXT},{"IRQ","16 lines",SM_TEXT},
               {"Display","VESA Framebuffer",SM_ACCENT}};
    int ry=col_y+22;
    for(int i=0;i<7;i++){
        vesa_draw_string(rx,ry,info[i].k,SM_DIM,SM_BG);
        sm_str_r(rx+col_w,ry,info[i].v,info[i].c,SM_BG);
        ry+=20;
    }
}

// ── Main VESA draw ────────────────────────────────────────────────
static void vk_draw(){
    int W=(int)vesa.width,H=(int)vesa.height;
    int wx=16,wy=40,ww=W-32,wh=H-50;

    vesa_fill_rect(wx,wy,ww,wh,SM_BG);

    g_win_x = wx; g_win_y = wy; g_win_w = ww; g_win_h = wh;

    // Title bar
    vesa_fill_rect(wx,wy,ww,36,SM_TITLE);
    vesa_hline(wx,wy+36,ww,SM_BORDER);
    vesa_draw_string(wx+16,wy+10,"System Monitor",SM_TEXT,SM_TITLE);
    char ts[9];rtc_get_time_str(ts);
    sm_str_r(wx+ww-100,wy+10,ts,SM_ACCENT,SM_TITLE);
    draw_close_btn(wx+ww-40, wy+10, SM_TITLE, "System Monitor");

    // Tab bar
    int ty=wy+36;
    vesa_fill_rect(wx,ty,ww,36,SM_TAB_IN);
    vesa_hline(wx,ty+36,ww,SM_BORDER);
    const char*tabs[]={"CPU","Memory","Processes","Disk"};
    int tx=wx+16;
    for(int i=0;i<TAB_COUNT;i++){
        int tw=s_len(tabs[i])*8+24;
        if(i==cur_tab){
            vesa_fill_rect(tx,ty+4,tw,28,SM_TAB_ACT);
            vesa_draw_rect(tx,ty+4,tw,28,SM_ACCENT);
            vesa_hline(tx,ty+31,tw,SM_ACCENT);
            vesa_draw_string(tx+12,ty+12,tabs[i],SM_TEXT,SM_TAB_ACT);
        } else {
            vesa_draw_string(tx+12,ty+12,tabs[i],SM_DIM,SM_TAB_IN);
        }
        tx+=tw+8;
    }
    sm_str_r(wx+ww-16,ty+12,"TAB switch",SM_HINT,SM_TAB_IN);

    int cx=wx,cy=ty+36,cw=ww,ch=wh-72;
    switch(cur_tab){
        case TAB_CPU:  vk_cpu (cx,cy,cw,ch); break;
        case TAB_MEM:  vk_mem (cx,cy,cw,ch); break;
        case TAB_PROC: vk_proc(cx,cy,cw,ch); break;
        case TAB_DISK: vk_disk(cx,cy,cw,ch); break;
    }
}

// ═══════════════════════════════════════════════════════════════════
//  PUBLIC API
// ═══════════════════════════════════════════════════════════════════

void sysmon_draw(){
    if(vesa.active){vk_draw();return;}
    vga_draw();
}

void sysmon_handle_key(char c){
    if(c=='\t'){cur_tab=(cur_tab+1)%TAB_COUNT;return;}
    if(c=='1'){cur_tab=TAB_CPU;return;}
    if(c=='2'){cur_tab=TAB_MEM;return;}
    if(c=='3'){cur_tab=TAB_PROC;return;}
    if(c=='4'){cur_tab=TAB_DISK;return;}

    if(cur_tab==TAB_PROC){
        if(c=='A'&&proc_sel>0){proc_sel--;return;}
        if(c=='B'&&proc_sel<PROC_COUNT-1){proc_sel++;return;}
        if(c=='k'||c=='K'){
            if(proc_sel>3){processes[proc_sel].status="Killed";processes[proc_sel].cpu=0;}
            return;
        }
        if(c=='r'||c=='R'){processes[proc_sel].status="Running";return;}
        if(c=='s'||c=='S'){processes[proc_sel].status="Suspended";processes[proc_sel].cpu=0;return;}
    }
}

void sysmon_tick(){
    tick_count++;
    if(tick_count>=18){tick_count=0;uptime_secs++;update_stats();}
}

void sysmon_init(){
    cur_tab=TAB_CPU;proc_sel=0;tick_count=0;graph_pos=0;
    cpu_usage=25;mem_used=65536;
    for(int i=0;i<GRAPH_W;i++){cpu_history[i]=20+(i%15);mem_history[i]=12+(i%8);}
}


int sysmon_handle_mouse(int clk_x, int clk_y) {
    if (!vesa.active) return 0;
    int W = (int)vesa.width;
    int wx = 16, wy = 40, ww = W - 32;

    // ── Tab bar clicks ───────────────────────────────────────────
    int ty = wy + 36;
    const char* tabs[] = {"CPU", "Memory", "Processes", "Disk"};
    int tx = wx + 16;
    for (int i = 0; i < TAB_COUNT; i++) {
        int tw = s_len(tabs[i]) * 8 + 24;
        if (clk_x >= tx && clk_x < tx + tw &&
            clk_y >= ty && clk_y < ty + 36) {
            cur_tab = i;
            return 1;
        }
        tx += tw + 8;
    }

    // ── Process list row clicks (only on Processes tab) ──────────
    if (cur_tab == TAB_PROC) {
        int cx = wx, cy = ty + 36, cw = ww;
        int row_h = 32;
        int list_y = cy + 40;
        for (int i = 0; i < PROC_COUNT; i++) {
            int ry = list_y + i * row_h;
            if (clk_x >= cx + 8 && clk_x < cx + cw - 8 &&
                clk_y >= ry && clk_y < ry + row_h) {
                proc_sel = i;
                return 1;
            }
        }
    }

    return 0;
}
