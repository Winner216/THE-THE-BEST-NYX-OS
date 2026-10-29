// cpu/clock.c  –  Nyx OS Clock  (VGA fallback + VESA pixel UI)
#include <clock.h>
#include <vesa.h>
#include <rtc.h>
#include <speaker.h>
#include <desktop.h>
#include <timezone.h>
#include <stdint.h>

// ─── VESA scaled char (declared in vesa.c) ───────────────────────
extern void vesa_draw_char_scaled(int x,int y,char c,
                                   uint32_t fg,uint32_t bg,int scale);

// ═══════════════════════════════════════════════════════════════════
//  SHARED STATE
// ═══════════════════════════════════════════════════════════════════

#define TAB_CLOCK     0
#define TAB_STOPWATCH 1
#define TAB_TIMER     2
#define TAB_ALARM     3

static int cur_tab     = TAB_CLOCK;
static int clock_active= 0;

// ─── Stopwatch ────────────────────────────────────────────────────
static int sw_run=0, sw_cs=0;
#define LAP_MAX 8
static int sw_laps[LAP_MAX][3];
static int sw_lap_n=0;

// ─── Timer ────────────────────────────────────────────────────────
static int tmr_run=0, tmr_done=0;
static int tmr_set_h=0, tmr_set_m=5, tmr_set_s=0;
static int tmr_total=0, tmr_rem=0, tmr_field=0;

// ─── Alarms ───────────────────────────────────────────────────────
#define ALARM_MAX 4
typedef struct { int h,m,on,fired; char label[24]; } Alarm;
static Alarm alarms[ALARM_MAX];
static int al_sel=0, al_edit=0, al_field=0;

// ─── String helpers (no libc) ─────────────────────────────────────
static int  ck_len(const char*s){int i=0;while(s[i])i++;return i;}
static void n2(int n,char*b){
    if(n<0)n=0;if(n>99)n=99;
    b[0]='0'+n/10;b[1]='0'+n%10;b[2]='\0';
}
static void n2s(int n,char*b){
    if(n==0){b[0]='0';b[1]='\0';return;}
    char t[12];int p=0;
    while(n>0){t[p++]='0'+n%10;n/=10;}
    for(int i=0;i<p;i++)b[i]=t[p-1-i];
    b[p]='\0';
}

// ═══════════════════════════════════════════════════════════════════
//  VGA FALLBACK  (identical to the original)
// ═══════════════════════════════════════════════════════════════════

#define VGA_WIDTH  80
#define VGA_HEIGHT 25
#define VGA_MEM    ((uint16_t*)0xB8000)

static inline uint16_t make_entry(char c,uint8_t fg,uint8_t bg){
    return (uint16_t)c|(uint16_t)(fg|bg<<4)<<8;
}
static void cp(int x,int y,char c,uint8_t fg,uint8_t bg){
    if(x<0||x>=VGA_WIDTH||y<0||y>=VGA_HEIGHT)return;
    VGA_MEM[y*VGA_WIDTH+x]=make_entry(c,fg,bg);
}
static void cs(int x,int y,const char*s,uint8_t fg,uint8_t bg){
    for(int i=0;s[i];i++) cp(x+i,y,s[i],fg,bg);
}
static void cf(int x,int y,int w,int h,char c,uint8_t fg,uint8_t bg){
    for(int dy=0;dy<h;dy++)
        for(int dx=0;dx<w;dx++)
            cp(x+dx,y+dy,c,fg,bg);
}
static void cfr(int y,char c,uint8_t fg,uint8_t bg){
    for(int x=0;x<VGA_WIDTH;x++) cp(x,y,c,fg,bg);
}
static void cc(int y,const char*s,uint8_t fg,uint8_t bg){
    int l=ck_len(s);
    cs((VGA_WIDTH-l)/2,y,s,fg,bg);
}
static const char* BIG[10][5]={
    {"###","# #","# #","# #","###"},
    {" ##","  #","  #","  #","###"},
    {"###","  #","###","#  ","###"},
    {"###","  #","###","  #","###"},
    {"# #","# #","###","  #","  #"},
    {"###","#  ","###","  #","###"},
    {"###","#  ","###","# #","###"},
    {"###","  #"," ##","  #","  #"},
    {"###","# #","###","# #","###"},
    {"###","# #","###","  #","###"},
};
static void draw_digit(int x,int y,int d,uint8_t col){
    if(d<0||d>9)d=0;
    for(int row=0;row<5;row++)
        for(int col2=0;col2<3;col2++){
            if(BIG[d][row][col2]=='#') cp(x+col2,y+row,' ',col,col);
            else                       cp(x+col2,y+row,' ',0,0);
        }
}
static void draw_time_big(int x,int y,int h,int m,int s,uint8_t col){
    draw_digit(x,   y,h/10,col); draw_digit(x+4, y,h%10,col);
    cp(x+8,y+1,':',col,0);       cp(x+8,y+3,':',col,0);
    draw_digit(x+10,y,m/10,col); draw_digit(x+14,y,m%10,col);
    cp(x+18,y+1,':',col,0);      cp(x+18,y+3,':',col,0);
    draw_digit(x+20,y,s/10,col); draw_digit(x+24,y,s%10,col);
}
static void draw_box(int x,int y,int w,int h,const char*title,uint8_t col){
    cf(x,y,w,h,' ',7,0);
    for(int i=x;i<x+w;i++){cp(i,y,'-',col,0);cp(i,y+h-1,'-',col,0);}
    for(int i=y;i<y+h;i++){cp(x,i,'|',col,0);cp(x+w-1,i,'|',col,0);}
    cp(x,y,'+',col,0);cp(x+w-1,y,'+',col,0);
    cp(x,y+h-1,'+',col,0);cp(x+w-1,y+h-1,'+',col,0);
    if(title){cp(x+1,y,' ',col,0);cs(x+2,y,title,col,0);}
}
static void draw_tabs_vga(){
    cfr(0,' ',0,1);
    cs(2,0,"[ Nyx Clock ]",15,1);
    char ts[9]; rtc_get_time_str(ts);
    cs(58,0,ts,14,1); cs(70,0,"ESC:back",7,1);
    cfr(1,' ',0,8);
    const char*tabs[]={"[1]Clock","[2]Stopwatch","[3]Timer","[4]Alarm"};
    int tx=1;
    for(int i=0;i<4;i++){
        uint8_t fg=(i==cur_tab)?0:15, bg=(i==cur_tab)?11:8;
        cp(tx,1,' ',fg,bg);
        int tl=ck_len(tabs[i]); cs(tx+1,1,tabs[i],fg,bg);
        cp(tx+tl+1,1,' ',fg,bg); tx+=tl+3;
    }
    cfr(2,'-',8,0);
}

// ─── VGA clock/stopwatch/timer/alarm draw (unchanged originals) ───
static void vga_draw_clock(){
    rtc_time_t t; rtc_read(&t);
    draw_box(20,4,40,8," Digital Clock ",11);
    draw_time_big(22,5,t.hour,t.minute,t.second,11);
    cs(48,7,t.hour<12?"AM":"PM",14,0);
    char ds[9]; rtc_get_date_str(ds);
    draw_box(2,13,22,5," Date ",11);
    cs(4,15,"Date:",8,0); cs(10,15,ds,15,0);
    draw_box(26,13,52,9," World Clocks ",13);
    const char*cities[]={"London  ","Moscow  ","Tashkent",
                          "Beijing ","New York","Tokyo   "};
    const char*utc[]={"UTC+0","UTC+3","UTC+5","UTC+8","UTC-5","UTC+9"};
    int offsets[]={0,3,5,8,-5,9};
    // rtc_read() already applies the system's configured timezone
    // offset to t.hour, so it is LOCAL time, not UTC. Subtract the
    // system's own offset first to recover true UTC, then apply each
    // city's offset — otherwise every city was silently off by
    // however many hours the system itself is offset from UTC.
    int sys_off = timezones[current_timezone].offset;
    int utc_hour = ((t.hour - sys_off) % 24 + 24) % 24;
    for(int i=0;i<6;i++){
        int wh=(utc_hour+offsets[i]+24)%24;
        char wh2[3],wm2[3]; n2(wh,wh2); n2(t.minute,wm2);
        char wt[6]; int p=0;
        for(int j=0;wh2[j];j++) wt[p++]=wh2[j]; wt[p++]=':';
        for(int j=0;wm2[j];j++) wt[p++]=wm2[j]; wt[p]='\0';
        cs(28,14+i,cities[i],7,0); cs(37,14+i,utc[i],8,0);
        cs(44,14+i,wt,14,0); cs(50,14+i,wh<12?"AM":"PM",wh<12?11:13,0);
    }
}
static void vga_draw_stopwatch(){
    int cs2=sw_cs%100, ss=(sw_cs/100)%60, mm=sw_cs/6000;
    draw_box(10,3,60,9," Stopwatch ",sw_run?10:14);
    draw_time_big(22,4,0,mm,ss,sw_run?10:14);
    char cstr[5]; cstr[0]='.'; cstr[1]='0'+cs2/10;
    cstr[2]='0'+cs2%10; cstr[3]='\0';
    cs(48,8,cstr,sw_run?10:14,0);
    cf(2,13,76,1,' ',0,8);
    cs(4,13," SPACE:start/stop ",sw_run?0:15,sw_run?10:8);
    cs(24,13," R:reset           ",15,8);
    cs(45,13," L:lap             ",15,8);
    cc(15,sw_run?">> RUNNING <<":" || STOPPED ||",sw_run?10:8,0);
    if(sw_lap_n>0){
        draw_box(2,16,76,7," Laps ",8);
        for(int i=0;i<sw_lap_n&&i<4;i++){
            char ln[4]; n2s(i+1,ln); cs(4,17+i,ln,14,0); cs(7,17+i,"| ",8,0);
            char lt[12];
            lt[0]='0'+sw_laps[i][0]/10;lt[1]='0'+sw_laps[i][0]%10;lt[2]=':';
            lt[3]='0'+sw_laps[i][1]/10;lt[4]='0'+sw_laps[i][1]%10;lt[5]='.';
            lt[6]='0'+sw_laps[i][2]/10;lt[7]='0'+sw_laps[i][2]%10;lt[8]='\0';
            cs(9,17+i,lt,15,0);
        }
    }
}
static void vga_draw_timer(){
    cf(0,3,VGA_WIDTH,VGA_HEIGHT-3,' ',0,0);
    if(tmr_done){
        draw_box(20,6,40,8," TIMER FINISHED! ",12);
        cc(9,"  *** TIME'S UP! ***  ",12,0);
        cc(11,"Press any key to dismiss",8,0); return;
    }
    if(!tmr_run){
        draw_box(2,3,76,9," Set Timer ",11);
        cc(5,"TAB: switch field    UP/DOWN: adjust value    ENTER: start",7,0);
        uint8_t hfg=(tmr_field==0)?0:11,hbg=(tmr_field==0)?11:0;
        draw_box(8,6,14,5,tmr_field==0?" >Hours< ":" Hours  ",tmr_field==0?11:8);
        cf(9,7,12,3,' ',hfg,hbg);
        cp(13,7,'0'+tmr_set_h/10,tmr_field==0?15:11,hbg);
        cp(14,7,'0'+tmr_set_h/10,tmr_field==0?15:11,hbg);
        cp(15,7,':',7,hbg);
        cp(16,7,'0'+tmr_set_h%10,tmr_field==0?15:11,hbg);
        cp(17,7,'0'+tmr_set_h%10,tmr_field==0?15:11,hbg);
        if(tmr_field==0) cs(14,9,"^ v",11,0);
        uint8_t mfg=(tmr_field==1)?0:13,mbg=(tmr_field==1)?13:0;
        draw_box(26,6,14,5,tmr_field==1?" >Mins< ":" Mins   ",tmr_field==1?13:8);
        cf(27,7,12,3,' ',mfg,mbg);
        cp(31,7,'0'+tmr_set_m/10,tmr_field==1?15:13,mbg);
        cp(32,7,'0'+tmr_set_m/10,tmr_field==1?15:13,mbg);
        cp(33,7,':',7,mbg);
        cp(34,7,'0'+tmr_set_m%10,tmr_field==1?15:13,mbg);
        cp(35,7,'0'+tmr_set_m%10,tmr_field==1?15:13,mbg);
        if(tmr_field==1) cs(32,9,"^ v",13,0);
        uint8_t sfg=(tmr_field==2)?0:9,sbg=(tmr_field==2)?9:0;
        draw_box(44,6,14,5,tmr_field==2?" >Secs< ":" Secs   ",tmr_field==2?9:8);
        cf(45,7,12,3,' ',sfg,sbg);
        cp(49,7,'0'+tmr_set_s/10,tmr_field==2?15:9,sbg);
        cp(50,7,'0'+tmr_set_s/10,tmr_field==2?15:9,sbg);
        cp(51,7,':',7,sbg);
        cp(52,7,'0'+tmr_set_s%10,tmr_field==2?15:9,sbg);
        cp(53,7,'0'+tmr_set_s%10,tmr_field==2?15:9,sbg);
        if(tmr_field==2) cs(50,9,"^ v",9,0);
        draw_box(2,13,40,5," Preview ",8);
        cs(4,15,"Timer set to: ",8,0);
        char ph[3],pm[3],ps[3]; n2(tmr_set_h,ph);n2(tmr_set_m,pm);n2(tmr_set_s,ps);
        cs(18,15,ph,15,0);cp(20,15,':',7,0);cs(21,15,pm,15,0);
        cp(23,15,':',7,0);cs(24,15,ps,15,0);
        cf(2,19,76,1,' ',0,8);
        cs(3,19," TAB:next field ",15,8);cs(21,19," UP:+1 DOWN:-1 ",15,8);
        cs(38,19," ENTER:start timer ",15,8);cs(59,19," ESC:back ",15,8);
    } else {
        int rh=tmr_rem/3600,rm=(tmr_rem%3600)/60,rs=tmr_rem%60;
        uint8_t tcol=tmr_rem<60?12:(tmr_rem<300?14:10);
        draw_box(10,3,60,9," Countdown ",tcol);
        draw_time_big(22,4,rh,rm,rs,tcol);
        int total=tmr_total>0?tmr_total:1;
        int done=(tmr_total-tmr_rem)*60/total;
        cs(9,13,"[",8,0);
        for(int i=0;i<60;i++) cp(10+i,13,i<done?'#':'-',i<done?tcol:8,0);
        cs(70,13,"]",8,0);
        int pct=(tmr_total-tmr_rem)*100/total;
        char pstr[8]; n2s(pct,pstr); int pp=0;
        while(pstr[pp])pp++;pstr[pp++]='%';pstr[pp]='\0';
        cs(72,13,pstr,tcol,0);
        cs(2,15,"Remaining: ",11,0);
        char rhs[3],rms[3],rss[3]; n2(rh,rhs);n2(rm,rms);n2(rs,rss);
        cs(13,15,rhs,15,0);cp(15,15,'h',7,0);cp(16,15,' ',7,0);
        cs(17,15,rms,15,0);cp(19,15,'m',7,0);cp(20,15,' ',7,0);
        cs(21,15,rss,15,0);cp(23,15,'s',7,0);
        cc(17,tmr_run?">> COUNTING DOWN <<":"|| PAUSED ||",tmr_run?10:14,0);
        cf(2,19,76,1,' ',0,8);
        cs(3,19," SPACE:pause/resume ",15,8);
        cs(25,19," R:reset            ",15,8);
        cs(47,19," ESC:cancel         ",15,8);
    }
}
static void vga_draw_alarm(){
    cf(0,3,VGA_WIDTH,VGA_HEIGHT-3,' ',0,0);
    draw_box(2,3,76,14," Alarms ",11);
    cf(3,4,74,1,' ',0,1);
    cs(4,4," # ",15,1);cs(9,4,"Time ",15,1);
    cs(18,4,"Label                  ",15,1);cs(44,4,"Status   ",15,1);
    cs(56,4,"Fires at  ",15,1);
    for(int i=0;i<ALARM_MAX;i++){
        uint8_t fg=(i==al_sel)?0:15,bg=(i==al_sel)?11:0;
        cf(3,5+i*3,74,2,' ',fg,bg);
        char an[3]; n2s(i+1,an); cs(5,5+i*3,an,fg,bg);
        char th[3],tm2[3]; n2(alarms[i].h,th);n2(alarms[i].m,tm2);
        cs(9,5+i*3,th,fg,bg);cp(11,5+i*3,':',fg,bg);cs(12,5+i*3,tm2,fg,bg);
        if(alarms[i].label[0]) cs(18,5+i*3,alarms[i].label,fg,bg);
        else cs(18,5+i*3,"(no label)",8,bg);
        if(alarms[i].fired){ cf(44,5+i*3,10,1,' ',0,12); cs(44,5+i*3,"** FIRED **",0,12); }
        else{ cf(44,5+i*3,8,1,' ',0,alarms[i].on?10:8); cs(45,5+i*3,alarms[i].on?" ACTIVE ":" OFF    ",0,alarms[i].on?10:8); }
        cs(56,5+i*3,th,14,bg);cp(58,5+i*3,':',7,bg);cs(59,5+i*3,tm2,14,bg);cs(62,5+i*3,"daily",7,bg);
    }
    cf(2,17,76,1,' ',0,8);
    cs(3,17," UP/DN:select  ",15,8);cs(19,17," ENTER:edit    ",15,8);
    cs(35,17," SPACE:on/off  ",15,8);cs(51,17," ESC:back      ",15,8);
    if(al_edit){
        draw_box(12,8,56,13," Edit Alarm ",14);
        cs(14,9,"Adjust with UP/DOWN, switch fields with TAB",7,0);
        cs(14,10,"Press ESC to save and close",7,0);
        uint8_t hfg=(al_field==0)?0:14,hbg=(al_field==0)?14:0;
        cs(14,12,"Hour   : ",11,0); cf(24,12,5,1,' ',hfg,hbg);
        char ah[3]; n2(alarms[al_sel].h,ah); cs(25,12,ah,hfg,hbg);
        if(al_field==0) cs(31,12,"<< UP/DOWN",11,0);
        uint8_t mfg=(al_field==1)?0:13,mbg=(al_field==1)?13:0;
        cs(14,14,"Minute : ",11,0); cf(24,14,5,1,' ',mfg,mbg);
        char am[3]; n2(alarms[al_sel].m,am); cs(25,14,am,mfg,mbg);
        if(al_field==1) cs(31,14,"<< UP/DOWN",13,0);
        uint8_t lfg=(al_field==2)?0:15,lbg=(al_field==2)?15:0;
        cs(14,16,"Label  : ",11,0); cf(24,16,30,1,' ',lfg,lbg);
        cs(24,16,alarms[al_sel].label,lfg,lbg);
        if(al_field==2){ int ll=0; while(alarms[al_sel].label[ll])ll++; cp(24+ll,16,'_',0,15); }
        if(al_field==2) cs(55,16,"BACKSPACE",8,0);
        cs(14,18,"Preview: Alarm will fire at ",8,0);
        char ph[3],pm[3]; n2(alarms[al_sel].h,ph);n2(alarms[al_sel].m,pm);
        cs(42,18,ph,14,0);cp(44,18,':',7,0);cs(45,18,pm,14,0);
        cf(14,19,52,1,' ',0,8);
        cs(15,19," TAB:next field  UP/DN:adjust  BS:del  ESC:SAVE ",15,8);
    }
}

// ═══════════════════════════════════════════════════════════════════
//  VESA PIXEL UI
// ═══════════════════════════════════════════════════════════════════

// ── Palette ───────────────────────────────────────────────────────
#define CK_BG       0x0E0C22
#define CK_TITLE    0x1A1440
#define CK_TAB_ACT  0x352878
#define CK_TAB_IN   0x14122A
#define CK_BORDER   0x2B2448
#define CK_DIVIDER  0x201D38
#define CK_TEXT     0xE0D8FF
#define CK_DIM      0x6858A0
#define CK_ACCENT   0x9B7FFF
#define CK_SUCCESS  0x55EE88
#define CK_DANGER   0xFF6655
#define CK_WARN     0xFFAA00
#define CK_CYAN     0x60D8FF
#define CK_PANEL    0x12102A
#define CK_CARD     0x1A1838
#define CK_HINT     0x504870

// ── Helpers ───────────────────────────────────────────────────────
static uint32_t ck_dim(uint32_t c,int d){
    int r=(c>>16&0xFF)+d; if(r<0)r=0;if(r>255)r=255;
    int g=(c>>8 &0xFF)+d; if(g<0)g=0;if(g>255)g=255;
    int b=(c    &0xFF)+d; if(b<0)b=0;if(b>255)b=255;
    return((uint32_t)r<<16)|((uint32_t)g<<8)|(uint32_t)b;
}
static void ck_str_r(int rx,int y,const char*s,uint32_t fg,uint32_t bg){
    vesa_draw_string(rx-ck_len(s)*8,y,s,fg,bg);
}
/* Draw big digit using 3× scaled font */
static void vk_digit(int x,int y,int d,uint32_t fg,uint32_t bg){
    if(d<0||d>9)d=0;
    char dc[2]; dc[0]='0'+d; dc[1]='\0';
    vesa_draw_char_scaled(x,y,dc[0],fg,bg,3);
}
/* Draw HH:MM:SS at 3× scale */
static void vk_time(int x,int y,int h,int m,int s,uint32_t fg,uint32_t bg){
    int cw=8*3; /* each char 24px wide */
    vk_digit(x+0*cw,y,h/10,fg,bg); vk_digit(x+1*cw,y,h%10,fg,bg);
    vesa_draw_char_scaled(x+2*cw,y,':',fg,bg,3);
    vk_digit(x+3*cw,y,m/10,fg,bg); vk_digit(x+4*cw,y,m%10,fg,bg);
    vesa_draw_char_scaled(x+5*cw,y,':',fg,bg,3);
    vk_digit(x+6*cw,y,s/10,fg,bg); vk_digit(x+7*cw,y,s%10,fg,bg);
}
/* Draw HH:MM:SS.cs at 2× scale (stopwatch) */
static void vk_sw_time(int x,int y,int m,int s,int cs2,uint32_t fg,uint32_t bg){
    int cw=8*2;
    vesa_draw_char_scaled(x+0*cw,y,'0'+m/10,fg,bg,2);
    vesa_draw_char_scaled(x+1*cw,y,'0'+m%10,fg,bg,2);
    vesa_draw_char_scaled(x+2*cw,y,':',fg,bg,2);
    vesa_draw_char_scaled(x+3*cw,y,'0'+s/10,fg,bg,2);
    vesa_draw_char_scaled(x+4*cw,y,'0'+s%10,fg,bg,2);
    vesa_draw_char_scaled(x+5*cw,y,'.',fg,bg,2);
    vesa_draw_char_scaled(x+6*cw,y,'0'+cs2/10,fg,bg,2);
    vesa_draw_char_scaled(x+7*cw,y,'0'+cs2%10,fg,bg,2);
}
/* Draw tab bar */
static void vk_tabs(int wx,int wy,int ww){
    vesa_fill_rect(wx,wy,ww,36,CK_TITLE);
    vesa_hline(wx,wy+36,ww,CK_BORDER);
    vesa_draw_string(wx+16,wy+10,"Nyx Clock",CK_TEXT,CK_TITLE);
    /* Live clock in title bar */
    char ts[9]; rtc_get_time_str(ts);
    ck_str_r(wx+ww-80,wy+10,ts,CK_ACCENT,CK_TITLE);
    draw_close_btn(wx+ww-40, wy+10, CK_TITLE, "Clock");
    /* Tab row */
    int ty=wy+36;
    vesa_fill_rect(wx,ty,ww,36,CK_TAB_IN);
    vesa_hline(wx,ty+36,ww,CK_BORDER);
    const char*tabs[]={"Clock","Stopwatch","Timer","Alarms"};
    int tx=wx+16;
    for(int i=0;i<4;i++){
        int tw=ck_len(tabs[i])*8+24;
        if(i==cur_tab){
            vesa_fill_rect(tx,ty+4,tw,28,CK_TAB_ACT);
            vesa_draw_rect(tx,ty+4,tw,28,CK_ACCENT);
            vesa_hline(tx,ty+31,tw,CK_ACCENT);
            vesa_draw_string(tx+12,ty+12,tabs[i],CK_TEXT,CK_TAB_ACT);
        } else {
            vesa_draw_string(tx+12,ty+12,tabs[i],CK_DIM,CK_TAB_IN);
        }
        tx+=tw+8;
    }
}
/* Progress bar helper */
static void vk_bar(int x,int y,int w,int h,int pct,uint32_t filled,uint32_t empty){
    int f=w*pct/100;
    if(f>0) vesa_fill_rect(x,y,f,h,filled);
    if(f<w) vesa_fill_rect(x+f,y,w-f,h,empty);
    vesa_draw_rect(x,y,w,h,CK_BORDER);
}
/* Pill badge */
static void vk_pill(int x,int y,const char*s,uint32_t fg,uint32_t bg){
    int w=ck_len(s)*8+16;
    vesa_fill_rect(x,y,w,22,bg);
    vesa_draw_rect(x,y,w,22,fg);
    vesa_draw_string(x+8,y+3,s,fg,bg);
}
/* Spin field (hours/minutes/seconds selector) */
static void vk_spin(int x,int y,int w,int h,const char*label,int val,
                    int active,uint32_t acc){
    uint32_t bg=active?ck_dim(CK_CARD,+12):CK_CARD;
    vesa_fill_rect(x,y,w,h,bg);
    vesa_draw_rect(x,y,w,h,active?acc:CK_BORDER);
    if(active) vesa_fill_rect(x,y,w,3,acc);
    /* label */
    vesa_draw_string(x+w/2-ck_len(label)*4,y+8,label,active?acc:CK_DIM,bg);
    /* up/down arrows */
    vesa_draw_string(x+w/2-8,y+22,"^",active?CK_TEXT:CK_DIM,bg);
    vesa_draw_string(x+w/2-8,y+h-22,"v",active?CK_TEXT:CK_DIM,bg);
    /* value at 3× scale */
    char v[3]; n2(val,v);
    int vx=x+w/2-24, vy=y+h/2-24;
    vesa_draw_char_scaled(vx,   vy,v[0],active?CK_TEXT:CK_DIM,bg,3);
    vesa_draw_char_scaled(vx+24,vy,v[1],active?CK_TEXT:CK_DIM,bg,3);
}

// ── Clock Tab ─────────────────────────────────────────────────────
static void vk_clock(int cx,int cy,int cw,int ch){
    (void)ch;
    vesa_fill_rect(cx,cy,cw,ch,CK_BG);
    rtc_time_t t; rtc_read(&t);

    /* Big time display */
    int tw=8*8*3; /* 8 chars × 24px */
    int tx=cx+(cw-tw)/2, ty=cy+20;
    vesa_fill_rect(cx+24,ty-10,cw-48,58+20,CK_CARD);
    vesa_draw_rect(cx+24,ty-10,cw-48,58+20,CK_BORDER);
    vesa_fill_rect(cx+24,ty-10,cw-48,3,CK_ACCENT);
    vk_time(tx,ty,t.hour,t.minute,t.second,CK_TEXT,CK_CARD);
    /* AM/PM */
    vk_pill(cx+cw-80,ty+16,t.hour<12?"AM":"PM",
            t.hour<12?CK_CYAN:CK_ACCENT,t.hour<12?0x0C2840:0x1A0848);
    /* Date */
    char ds[9]; rtc_get_date_str(ds);
    vesa_draw_string(cx+(cw-ck_len(ds)*8)/2,ty+62,ds,CK_DIM,CK_BG);

    /* World clocks grid */
    int gy=cy+145;
    vesa_draw_string(cx+24,gy-18,"World Clocks",CK_DIM,CK_BG);
    vesa_hline(cx+16,gy-4,cw-32,CK_DIVIDER);

    const char*cities[]={"London","Moscow","Tashkent","Beijing","New York","Tokyo"};
    const char*utclabel[]={"UTC+0","UTC+3","UTC+5","UTC+8","UTC-5","UTC+9"};
    int offsets[]={0,3,5,8,-5,9};
    // rtc_read() returns LOCAL time (system timezone already applied),
    // so recover true UTC before applying each city's own offset —
    // otherwise every card was off by the system's own UTC offset.
    int sys_off = timezones[current_timezone].offset;
    int utc_hour = ((t.hour - sys_off) % 24 + 24) % 24;
    int cols=3, ccard_w=(cw-48)/cols-8;
    for(int i=0;i<6;i++){
        int col=i%cols, row=i/cols;
        int bx=cx+24+col*(ccard_w+8);
        int by=gy+row*64;
        int wh=(utc_hour+offsets[i]+24)%24;
        char wh2[3],wm2[3]; n2(wh,wh2); n2(t.minute,wm2);
        char wt[6]; int p=0;
        for(int j=0;wh2[j];j++) wt[p++]=wh2[j]; wt[p++]=':';
        for(int j=0;wm2[j];j++) wt[p++]=wm2[j]; wt[p]='\0';
        vesa_fill_rect(bx,by,ccard_w,56,CK_CARD);
        vesa_draw_rect(bx,by,ccard_w,56,CK_BORDER);
        vesa_draw_string(bx+8,by+8,cities[i],CK_TEXT,CK_CARD);
        vesa_draw_string(bx+8,by+26,wt,CK_ACCENT,CK_CARD);
        vesa_draw_string(bx+ccard_w-ck_len(utclabel[i])*8-8,by+8,
                         utclabel[i],CK_DIM,CK_CARD);
        vk_pill(bx+ccard_w-48,by+24,wh<12?"AM":"PM",
                wh<12?CK_CYAN:0xC080FF,wh<12?0x0C2840:0x280848);
    }
}

// ── Stopwatch Tab ─────────────────────────────────────────────────
static void vk_stopwatch(int cx,int cy,int cw,int ch){
    (void)ch;
    vesa_fill_rect(cx,cy,cw,ch,CK_BG);
    int cs2=sw_cs%100, ss=(sw_cs/100)%60, mm=sw_cs/6000;
    uint32_t acc=sw_run?CK_SUCCESS:CK_DIM;

    /* Big display */
    int tw=8*8*2;
    int tx=cx+(cw-tw)/2, ty=cy+20;
    vesa_fill_rect(cx+24,ty-10,cw-48,52+20,CK_CARD);
    vesa_draw_rect(cx+24,ty-10,cw-48,52+20,CK_BORDER);
    vesa_fill_rect(cx+24,ty-10,cw-48,3,acc);
    vk_sw_time(tx,ty,mm,ss,cs2,acc,CK_CARD);

    /* Status pill */
    vk_pill(cx+(cw-120)/2,ty+60,sw_run?">> RUNNING <<":"|| STOPPED ||",
            sw_run?CK_SUCCESS:CK_DIM,sw_run?0x0A280E:0x100E20);

    /* Controls */
    int hy=ty+100;
    vesa_draw_string(cx+24,hy,"SPACE start/stop",CK_DIM,CK_BG);
    vesa_draw_string(cx+24+200,hy,"R reset",CK_DIM,CK_BG);
    vesa_draw_string(cx+24+340,hy,"L lap",CK_DIM,CK_BG);

    /* Laps */
    if(sw_lap_n>0){
        int ly=hy+28;
        vesa_draw_string(cx+24,ly,"Laps",CK_DIM,CK_BG); ly+=20;
        vesa_hline(cx+16,ly,cw-32,CK_DIVIDER); ly+=8;
        int lcard_w=(cw-48)/4-6;
        for(int i=0;i<sw_lap_n&&i<8;i++){
            int col=i%4, row=i/4;
            int bx=cx+24+col*(lcard_w+6);
            int by=ly+row*44;
            char lt[12];
            lt[0]='0'+sw_laps[i][0]/10;lt[1]='0'+sw_laps[i][0]%10;lt[2]=':';
            lt[3]='0'+sw_laps[i][1]/10;lt[4]='0'+sw_laps[i][1]%10;lt[5]='.';
            lt[6]='0'+sw_laps[i][2]/10;lt[7]='0'+sw_laps[i][2]%10;lt[8]='\0';
            char ln[4]; n2s(i+1,ln);
            vesa_fill_rect(bx,by,lcard_w,36,CK_CARD);
            vesa_draw_rect(bx,by,lcard_w,36,CK_BORDER);
            vesa_draw_string(bx+8,by+4,ln,CK_DIM,CK_CARD);
            vesa_draw_string(bx+8,by+18,lt,CK_ACCENT,CK_CARD);
        }
    }
}

// ── Timer Tab ─────────────────────────────────────────────────────
static void vk_timer(int cx,int cy,int cw,int ch){
    (void)ch;
    vesa_fill_rect(cx,cy,cw,ch,CK_BG);

    if(tmr_done){
        /* Full-width alert */
        vesa_fill_rect(cx+16,cy+40,cw-32,ch-80,0x3A0808);
        vesa_draw_rect(cx+16,cy+40,cw-32,ch-80,CK_DANGER);
        vesa_fill_rect(cx+16,cy+40,cw-32,3,CK_DANGER);
        int mx=cx+(cw-ck_len("TIME'S UP!")*16)/2;
        for(int i=0;i<(int)ck_len("TIME'S UP!");i++)
            vesa_draw_char_scaled(mx+i*16,cy+80,"TIME'S UP!"[i],CK_DANGER,0x3A0808,2);
        vesa_draw_string(cx+(cw-ck_len("Press any key to dismiss")*8)/2,
                         cy+130,"Press any key to dismiss",CK_DIM,0x3A0808);
        return;
    }

    if(!tmr_run){
        /* Setup: three spin fields side by side */
        int spin_w=(cw-80)/3-16, spin_h=140;
        int spin_y=cy+24;
        int spin_gap=(cw-80-3*spin_w)/2;
        int bx=cx+40;
        vk_spin(bx,              spin_y,spin_w,spin_h,"Hours",  tmr_set_h,tmr_field==0,0xFF6655);
        vk_spin(bx+spin_w+spin_gap,spin_y,spin_w,spin_h,"Minutes",tmr_set_m,tmr_field==1,CK_WARN);
        vk_spin(bx+2*(spin_w+spin_gap),spin_y,spin_w,spin_h,"Seconds",tmr_set_s,tmr_field==2,CK_ACCENT);

        /* Preview */
        int py=spin_y+spin_h+20;
        char ph[3],pm[3],ps[3]; n2(tmr_set_h,ph);n2(tmr_set_m,pm);n2(tmr_set_s,ps);
        char prev[16]; int pp=0;
        for(int i=0;ph[i];i++) prev[pp++]=ph[i]; prev[pp++]='h'; prev[pp++]=' ';
        for(int i=0;pm[i];i++) prev[pp++]=pm[i]; prev[pp++]='m'; prev[pp++]=' ';
        for(int i=0;ps[i];i++) prev[pp++]=ps[i]; prev[pp++]='s'; prev[pp]='\0';
        vesa_draw_string(cx+(cw-ck_len(prev)*8)/2,py,prev,CK_ACCENT,CK_BG);

        /* Hint */
        int total=tmr_set_h*3600+tmr_set_m*60+tmr_set_s;
        if(total==0)
            vesa_draw_string(cx+(cw-ck_len("Set a time above")*8)/2,
                             py+24,"Set a time above",CK_DIM,CK_BG);
        else{
            vk_pill(cx+(cw-136)/2,py+20,"ENTER  start",CK_SUCCESS,0x0A280E);
        }
        vesa_draw_string(cx+24,cy+ch-28,
            "TAB next field   UP/DOWN adjust   ENTER start",CK_HINT,CK_BG);
    } else {
        /* Countdown running */
        int rh=tmr_rem/3600,rm=(tmr_rem%3600)/60,rs=tmr_rem%60;
        uint32_t acc=tmr_rem<60?CK_DANGER:(tmr_rem<300?CK_WARN:CK_SUCCESS);

        int tw=8*8*3;
        int tx=cx+(cw-tw)/2, ty=cy+20;
        vesa_fill_rect(cx+24,ty-10,cw-48,58+20,CK_CARD);
        vesa_draw_rect(cx+24,ty-10,cw-48,58+20,CK_BORDER);
        vesa_fill_rect(cx+24,ty-10,cw-48,3,acc);
        vk_time(tx,ty,rh,rm,rs,acc,CK_CARD);

        /* Progress bar */
        int total=tmr_total>0?tmr_total:1;
        int pct=(tmr_total-tmr_rem)*100/total;
        int by=ty+88;
        vesa_draw_string(cx+24,by,"Progress",CK_DIM,CK_BG); by+=20;
        vk_bar(cx+24,by,cw-48,18,pct,acc,ck_dim(CK_CARD,-8)); by+=26;

        char pstr[8]; n2s(pct,pstr); int pp2=0;
        while(pstr[pp2])pp2++;pstr[pp2++]='%';pstr[pp2]='\0';
        ck_str_r(cx+cw-24,by-22,pstr,acc,CK_BG);

        /* Status pill */
        vk_pill(cx+(cw-160)/2,by+8,
                tmr_run?">> COUNTING DOWN <<":"|| PAUSED ||",
                acc,tmr_run?0x0A1E0A:0x201C38);

        vesa_draw_string(cx+24,cy+ch-28,
            "SPACE pause/resume   R reset   ESC cancel",CK_HINT,CK_BG);
    }
}

// ── Alarm Tab ─────────────────────────────────────────────────────
static void vk_alarm(int cx,int cy,int cw,int ch){
    (void)ch;
    vesa_fill_rect(cx,cy,cw,ch,CK_BG);

    int row_h=56, list_y=cy+12;
    for(int i=0;i<ALARM_MAX;i++){
        int ry=list_y+i*row_h;
        int sel=(i==al_sel);
        uint32_t bg=sel?0x252068:CK_CARD;
        uint32_t fg=sel?CK_TEXT:0xA090C8;
        vesa_fill_rect(cx+16,ry,cw-32,row_h-4,bg);
        vesa_draw_rect(cx+16,ry,cw-32,row_h-4,sel?CK_ACCENT:CK_BORDER);
        if(sel) vesa_fill_rect(cx+16,ry,4,row_h-4,CK_ACCENT);

        /* Time display */
        char th[3],tm2[3]; n2(alarms[i].h,th);n2(alarms[i].m,tm2);
        char tstr[6]; int tp=0;
        for(int k=0;th[k];k++) tstr[tp++]=th[k]; tstr[tp++]=':';
        for(int k=0;tm2[k];k++) tstr[tp++]=tm2[k]; tstr[tp]='\0';
        vesa_draw_string(cx+28,ry+12,tstr,fg,bg);

        /* Label */
        if(alarms[i].label[0])
            vesa_draw_string(cx+28+72,ry+12,alarms[i].label,fg,bg);
        else
            vesa_draw_string(cx+28+72,ry+12,"(no label)",CK_DIM,bg);

        /* Status badge */
        if(alarms[i].fired)
            vk_pill(cx+cw-130,ry+14,"FIRED",CK_DANGER,0x3A0808);
        else if(alarms[i].on)
            vk_pill(cx+cw-130,ry+14,"ACTIVE",CK_SUCCESS,0x0A280E);
        else
            vk_pill(cx+cw-130,ry+14,"OFF",CK_DIM,0x0E0C1C);
    }

    vesa_draw_string(cx+16,cy+ch-28,
        "UP/DOWN select   ENTER edit   SPACE on/off",CK_HINT,CK_BG);

    /* Edit overlay */
    if(al_edit){
        int ow=cw-80, oh=220;
        int ox=cx+40, oy=cy+(ch-oh)/2;
        vesa_fill_rect(ox,oy,ow,oh,0x12102A);
        vesa_draw_rect(ox,oy,ow,oh,CK_ACCENT);
        vesa_fill_rect(ox,oy,ow,3,CK_ACCENT);
        vesa_draw_string(ox+16,oy+10,"Edit Alarm",CK_TEXT,0x12102A);

        /* Spin fields: hour and minute */
        int sw2=ow/3-16;
        vk_spin(ox+16,         oy+40,sw2,120,"Hour",  alarms[al_sel].h,al_field==0,CK_ACCENT);
        vk_spin(ox+16+sw2+16,  oy+40,sw2,120,"Minute",alarms[al_sel].m,al_field==1,CK_ACCENT);

        /* Label field */
        int lx=ox+16, ly2=oy+170;
        vesa_draw_string(lx,ly2,"Label:",al_field==2?CK_TEXT:CK_DIM,0x12102A);
        int fw=ow-32;
        uint32_t lbg=al_field==2?0x201848:0x0E0C20;
        vesa_fill_rect(lx,ly2+18,fw,26,lbg);
        vesa_draw_rect(lx,ly2+18,fw,26,al_field==2?CK_ACCENT:CK_BORDER);
        vesa_draw_string(lx+8,ly2+22,alarms[al_sel].label,
                         al_field==2?CK_TEXT:CK_DIM,lbg);
        if(al_field==2){
            int ll=0; while(alarms[al_sel].label[ll])ll++;
            vesa_draw_string(lx+8+ll*8,ly2+22,"_",CK_ACCENT,lbg);
        }

        vesa_draw_string(ox+16,oy+oh-20,
            "TAB field   UP/DN adjust   BKSP del   ESC save",CK_HINT,0x12102A);
    }
}

// ── Main VESA draw ────────────────────────────────────────────────
static void vk_draw(){
    int W=(int)vesa.width, H=(int)vesa.height;
    int wx=16, wy=40, ww=W-32, wh=H-50;
    g_win_x = wx; g_win_y = wy; g_win_w = ww; g_win_h = wh;
    int hdr=72;  /* title bar + tab row */
    int cx=wx, cy=wy+hdr, cw=ww, ch=wh-hdr;

    vesa_fill_rect(wx,wy,ww,wh,CK_BG);
    vk_tabs(wx,wy,ww);

    switch(cur_tab){
        case TAB_CLOCK:     vk_clock    (cx,cy,cw,ch); break;
        case TAB_STOPWATCH: vk_stopwatch(cx,cy,cw,ch); break;
        case TAB_TIMER:     vk_timer    (cx,cy,cw,ch); break;
        case TAB_ALARM:     vk_alarm    (cx,cy,cw,ch); break;
    }
}

// ═══════════════════════════════════════════════════════════════════
//  PUBLIC API
// ═══════════════════════════════════════════════════════════════════

void clock_draw() {
    if(vesa.active){ vk_draw(); return; }
    cf(0,0,VGA_WIDTH,VGA_HEIGHT,' ',0,0);
    draw_tabs_vga();
    switch(cur_tab){
        case TAB_CLOCK:     vga_draw_clock();     break;
        case TAB_STOPWATCH: vga_draw_stopwatch();  break;
        case TAB_TIMER:     vga_draw_timer();      break;
        case TAB_ALARM:     vga_draw_alarm();      break;
    }
}

void clock_handle_key(char c){
    if(c=='1'){cur_tab=TAB_CLOCK;    clock_draw();return;}
    if(c=='2'){cur_tab=TAB_STOPWATCH;clock_draw();return;}
    if(c=='3'){cur_tab=TAB_TIMER;    clock_draw();return;}
    if(c=='4'){cur_tab=TAB_ALARM;    clock_draw();return;}

    switch(cur_tab){
        case TAB_CLOCK: break;

        case TAB_STOPWATCH:
            if(c==' '){sw_run=!sw_run;if(sw_run)desktop_set_status("Stopwatch started");else desktop_set_status("Stopwatch paused");clock_draw();}
            else if(c=='r'||c=='R'){sw_run=0;sw_cs=0;sw_lap_n=0;desktop_set_status("Stopwatch reset");clock_draw();}
            else if((c=='l'||c=='L')&&sw_run&&sw_lap_n<LAP_MAX){
                sw_laps[sw_lap_n][0]=sw_cs/6000;
                sw_laps[sw_lap_n][1]=(sw_cs/100)%60;
                sw_laps[sw_lap_n][2]=sw_cs%100;
                sw_lap_n++;desktop_set_status("Lap recorded");clock_draw();
            }
            break;

        case TAB_TIMER:
            if(tmr_done){tmr_done=0;tmr_run=0;desktop_set_status("Timer done!");desktop_vfx_flash(255,100,100,6);clock_draw();return;}
            if(!tmr_run){
                if(c=='\t'){tmr_field=(tmr_field+1)%3;clock_draw();return;}
                if(c=='A'){
                    if(tmr_field==0){tmr_set_h++;if(tmr_set_h>23)tmr_set_h=0;}
                    else if(tmr_field==1){tmr_set_m++;if(tmr_set_m>59)tmr_set_m=0;}
                    else{tmr_set_s++;if(tmr_set_s>59)tmr_set_s=0;}
                    clock_draw();return;
                }
                if(c=='B'){
                    if(tmr_field==0){tmr_set_h--;if(tmr_set_h<0)tmr_set_h=23;}
                    else if(tmr_field==1){tmr_set_m--;if(tmr_set_m<0)tmr_set_m=59;}
                    else{tmr_set_s--;if(tmr_set_s<0)tmr_set_s=59;}
                    clock_draw();return;
                }
                if(c==13){
                    tmr_total=tmr_set_h*3600+tmr_set_m*60+tmr_set_s;
                    if(tmr_total>0){tmr_rem=tmr_total;tmr_run=1;desktop_set_status("Timer started");clock_draw();}
                    return;
                }
            } else {
                if(c==' '){tmr_run=!tmr_run;if(tmr_run)desktop_set_status("Timer resumed");else desktop_set_status("Timer paused");clock_draw();}
                else if(c=='r'||c=='R'){tmr_run=0;tmr_rem=tmr_total;desktop_set_status("Timer reset");clock_draw();}
                else if(c==27){tmr_run=0;tmr_rem=0;desktop_set_status("Timer cancelled");clock_draw();}
            }
            break;

        case TAB_ALARM:
            if(al_edit){
                if(c==27){al_edit=0;clock_draw();return;}
                if(c=='\t'){al_field=(al_field+1)%3;clock_draw();return;}
                if(c=='A'){
                    if(al_field==0){alarms[al_sel].h++;if(alarms[al_sel].h>23)alarms[al_sel].h=0;}
                    else if(al_field==1){alarms[al_sel].m++;if(alarms[al_sel].m>59)alarms[al_sel].m=0;}
                    clock_draw();return;
                }
                if(c=='B'){
                    if(al_field==0){alarms[al_sel].h--;if(alarms[al_sel].h<0)alarms[al_sel].h=23;}
                    else if(al_field==1){alarms[al_sel].m--;if(alarms[al_sel].m<0)alarms[al_sel].m=59;}
                    clock_draw();return;
                }
                if(al_field==2){
                    if(c==8){
                        int ll=0;while(alarms[al_sel].label[ll])ll++;
                        if(ll>0){alarms[al_sel].label[ll-1]='\0';clock_draw();}
                        return;
                    }
                    if(c>=32&&c<127){
                        int ll=0;while(alarms[al_sel].label[ll])ll++;
                        if(ll<22){alarms[al_sel].label[ll]=c;alarms[al_sel].label[ll+1]='\0';clock_draw();}
                        return;
                    }
                }
            } else {
                if(c=='A'&&al_sel>0){al_sel--;clock_draw();}
                else if(c=='B'&&al_sel<ALARM_MAX-1){al_sel++;clock_draw();}
                else if(c==13){al_edit=1;al_field=0;desktop_set_status("Edit alarm");clock_draw();}
                else if(c==' '){alarms[al_sel].on=!alarms[al_sel].on;alarms[al_sel].fired=0;desktop_set_status(alarms[al_sel].on?"Alarm on":"Alarm off");clock_draw();}
            }
            break;
    }
}

// ── Mouse click handler for clock tabs ──────────────────────────
// Called from the desktop click dispatch when MODE_CLOCK is active.
// Returns 1 if the click was on a tab (and switches to it), else 0.
int clock_handle_mouse(int mx, int my) {
    if (!vesa.active) return 0;
    int wx = 16, wy = 40;
    int ty = wy + 36;  // tab bar top (same as vk_tabs)
    int th = 36;       // tab bar height
    if (my < ty || my >= ty + th) return 0;
    const char* tabs[] = {"Clock", "Stopwatch", "Timer", "Alarms"};
    int tx = wx + 16;
    for (int i = 0; i < 4; i++) {
        int tw = ck_len(tabs[i]) * 8 + 24;
        if (mx >= tx && mx < tx + tw) {
            if (i != cur_tab) {
                cur_tab = i;
                // Don't redraw here — desktop_draw() will call
                // clock_draw() → vk_draw() with the new cur_tab.
            }
            return 1;
        }
        tx += tw + 8;
    }
    return 0;
}

void clock_tick(){
    // Stopwatch — the PIT/IRQ0 timer fires at ~18.2065 Hz, not exactly
    // 18 Hz or 1 Hz. The old code did `sw_cs++` once per tick, which
    // increased "centiseconds" 18.2 times a SECOND instead of 100
    // times a second — the displayed stopwatch ran about 5.5x slower
    // than a real clock. Fixed-point accumulator paces it correctly:
    // each tick should contribute 100/18.2065 ≈ 5.4929 centiseconds.
    static int sw_accum=0;
    if(sw_run){
        sw_accum += 54929;              // 5.4929 cs, scaled by 10000
        while(sw_accum>=10000){ sw_accum-=10000; sw_cs++; }
    }

    // Timer countdown — same underlying issue in the other direction:
    // decrementing once every 18 ticks (assumed 1 tick = 1/18 sec)
    // instead of the true 18.2065 ticks/sec made countdowns finish
    // about 1.1% early (a 5-minute timer would end ~3s early). Use
    // the same accumulator technique so 1 second really is 1 second.
    static int tmr_accum=0;
    if(tmr_run&&tmr_rem>0){
        tmr_accum += 10000;             // 1 tick, scaled by 10000
        if(tmr_accum>=182065){          // 18.2065 ticks ≈ 1 real second
            tmr_accum-=182065; tmr_rem--;
            if(tmr_rem==0){
                tmr_run=0;tmr_done=1;
                
                 
            }
        }
    }

    // Clock + alarm check every ~18 ticks (≈1 second)
    static int cl_ticks=0;
    cl_ticks++;
    if(cl_ticks>=18){
        cl_ticks=0;
        rtc_time_t t; rtc_read(&t);
        for(int i=0;i<ALARM_MAX;i++){
            if(alarms[i].on&&!alarms[i].fired){
                // NOTE: no "&&t.second==0" gate here. This check runs
                // roughly once per real second via cl_ticks, but that
                // software counter drifts independently from the RTC's
                // own second boundary (18 IRQ0 ticks != exactly 1s).
                // Requiring an exact second==0 match meant the check
                // could drift past it for an entire minute and never
                // fire the alarm at all. Since `fired` latches until
                // the alarm is re-armed, checking on hour/minute alone
                // is safe: it can only trigger once per matching minute.
                if(alarms[i].h==t.hour&&alarms[i].m==t.minute){
                    alarms[i].fired=1;
                    
                     
                }
            }
        }
    }
    // NOTE: clock_tick no longer redraws directly.
    // desktop_update_clock() → desktop_draw() → vesa_present()
    // handles all screen updates from the 18Hz timer IRQ.
}

void clock_init(){
    cur_tab=TAB_CLOCK; clock_active=1;
    sw_run=0;sw_cs=0;sw_lap_n=0;
    tmr_run=0;tmr_done=0;
    tmr_set_h=0;tmr_set_m=5;tmr_set_s=0;
    tmr_total=0;tmr_rem=0;tmr_field=0;
    al_sel=0;al_edit=0;al_field=0;
    for(int i=0;i<ALARM_MAX;i++){
        alarms[i].h=7;alarms[i].m=0;
        alarms[i].on=0;alarms[i].fired=0;
        alarms[i].label[0]='\0';
    }
}

void clock_stop(){ clock_active=0; }
int  clock_is_main_view(){ return !al_edit&&!tmr_done; }
