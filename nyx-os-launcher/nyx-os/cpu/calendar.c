// cpu/calendar.c - Nyx OS Calendar App (VGA + VESA)
#include <calendar.h>
#include <rtc.h>
#include <speaker.h>
#include <vesa.h>
#include <stdint.h>
#include <mouse.h>
#include <desktop.h>

extern void vesa_draw_char_scaled(int x,int y,char c,uint32_t fg,uint32_t bg,int scale);

#define VGA_WIDTH  80
#define VGA_HEIGHT 25
#define VGA_MEM    ((uint16_t*)0xB8000)

static inline uint16_t make_entry(char c, uint8_t fg, uint8_t bg) {
    return (uint16_t)c | (uint16_t)(fg | bg << 4) << 8;
}
static void vp(int x, int y, char c, uint8_t fg, uint8_t bg) {
    if(x<0||x>=VGA_WIDTH||y<0||y>=VGA_HEIGHT) return;
    VGA_MEM[y*VGA_WIDTH+x] = make_entry(c,fg,bg);
}
static void vs(int x, int y, const char* s, uint8_t fg, uint8_t bg) {
    for(int i=0;s[i];i++) vp(x+i,y,s[i],fg,bg);
}
static void vf(int x, int y, int w, int h, char c, uint8_t fg, uint8_t bg) {
    for(int dy=0;dy<h;dy++)
        for(int dx=0;dx<w;dx++)
            vp(x+dx,y+dy,c,fg,bg);
}
static void vfr(int y, char c, uint8_t fg, uint8_t bg) {
    for(int x=0;x<VGA_WIDTH;x++) vp(x,y,c,fg,bg);
}
static void vc(int y, const char* s, uint8_t fg, uint8_t bg) {
    int l=0; while(s[l])l++;
    vs((VGA_WIDTH-l)/2,y,s,fg,bg);
}
static void n2(int n, char* b) {
    if(n<0)n=0;
    b[0]='0'+n/10; b[1]='0'+n%10; b[2]='\0';
}
static void n2s(int n, char* b) {
    if(n==0){b[0]='0';b[1]='\0';return;}
    char t[12];int p=0;
    while(n>0){t[p++]='0'+n%10;n/=10;}
    for(int i=0;i<p;i++)b[i]=t[p-1-i];
    b[p]='\0';
}
static void s_cpy(char* d,const char* s,int max){
    int i=0;while(s[i]&&i<max-1){d[i]=s[i];i++;}d[i]='\0';
}

// ─── Calendar state ───────────────────────────────────────────────
static int view_year  = 2026;
static int view_month = 6;    // 1-12
static int sel_day    = 1;
static int today_y    = 2026;
static int today_m    = 6;
static int today_d    = 4;

// UI modes
#define CAL_VIEW   0   // Monthly view
#define CAL_ADD    1   // Add event
#define CAL_LIST   2   // List events for selected day
#define CAL_DEL    3   // Confirm delete
static int cal_mode = CAL_VIEW;

// Add event form
static char  add_title[MAX_EVT_LEN];
static int   add_title_pos = 0;
static uint8_t add_color   = 10;
static int   add_field     = 0; // 0=title 1=color

// Event list / delete
static int   list_sel    = 0;
static int   del_confirm = 0;
static char  status_msg[48];
static int   status_timer = 0;

// ─── Events storage ───────────────────────────────────────────────
CalEvent cal_events[MAX_EVENTS];
int      cal_event_count = 0;

// ─── Month names ──────────────────────────────────────────────────
static const char* month_names[] = {
    "","January","February","March","April","May","June",
    "July","August","September","October","November","December"
};

// ─── Days in month ────────────────────────────────────────────────
static int is_leap(int y) {
    return (y%4==0&&y%100!=0)||(y%400==0);
}
static int days_in_month(int m, int y) {
    int days[]={0,31,28,31,30,31,30,31,31,30,31,30,31};
    if(m==2&&is_leap(y)) return 29;
    return days[m];
}

// ─── Day of week (0=Sun) Zeller's ────────────────────────────────
static int day_of_week(int d, int m, int y) {
    if(m<3){m+=12;y--;}
    int k=y%100, j=y/100;
    int h=d+(13*(m+1)/5)+k+(k/4)+(j/4)-(2*j);
    h=((h%7)+7)%7;
    return (h+6)%7;
}

// ─── Event helpers ────────────────────────────────────────────────
static int events_on_day(int y, int m, int d) {
    int n=0;
    for(int i=0;i<cal_event_count;i++)
        if(cal_events[i].year==y&&cal_events[i].month==m&&cal_events[i].day==d)
            n++;
    return n;
}
static void set_status(const char* msg) {
    s_cpy(status_msg,msg,48);
    status_timer=50;
}
static void goto_next_day(){
    int dim=days_in_month(view_month,view_year);
    sel_day++;
    if(sel_day>dim){
        sel_day=1;
        view_month++;
        if(view_month>12){view_month=1;view_year++;}
    }
}
static void goto_prev_day(){
    sel_day--;
    if(sel_day<1){
        view_month--;
        if(view_month<1){view_month=12;view_year--;}
        sel_day=days_in_month(view_month,view_year);
    }
}
static void goto_next_week(){
    sel_day+=7;
    int dim=days_in_month(view_month,view_year);
    if(sel_day>dim){
        sel_day-=dim;
        view_month++;
        if(view_month>12){view_month=1;view_year++;}
    }
}
static void goto_prev_week(){
    sel_day-=7;
    if(sel_day<1){
        view_month--;
        if(view_month<1){view_month=12;view_year--;}
        sel_day=days_in_month(view_month,view_year)+sel_day;
    }
}
static void goto_prev_month(){
    view_month--;
    if(view_month<1){view_month=12;view_year--;}
    int dim=days_in_month(view_month,view_year);
    if(sel_day>dim) sel_day=dim;
}
static void goto_next_month(){
    view_month++;
    if(view_month>12){view_month=1;view_year++;}
    int dim=days_in_month(view_month,view_year);
    if(sel_day>dim) sel_day=dim;
}
static void open_add_form(){
    cal_mode=CAL_ADD;
    add_title[0]='\0'; add_title_pos=0;
    add_color=10; add_field=0;
}
static int do_add_event() {
    if(add_title[0]=='\0'){ set_status("Title cannot be empty!"); return 0; }
    if(cal_event_count>=MAX_EVENTS){ set_status("Max events reached!"); return 0; }
    CalEvent* e=&cal_events[cal_event_count++];
    e->year =view_year; e->month=view_month; e->day=sel_day; e->color=add_color;
    s_cpy(e->title,add_title,MAX_EVT_LEN);
    
    set_status("Event added!");
    return 1;
}
static void do_delete_event() {
    int ci=0;
    for(int i=0;i<cal_event_count;i++){
        if(cal_events[i].year==view_year&&cal_events[i].month==view_month&&cal_events[i].day==sel_day){
            if(ci==list_sel){
                for(int j=i;j<cal_event_count-1;j++) cal_events[j]=cal_events[j+1];
                cal_event_count--;
                if(list_sel>0) list_sel--;
                
                set_status("Event deleted!");
                return;
            }
            ci++;
        }
    }
}

// ═══════════════════════════════════════════════════════════════════
//  VGA FALLBACK
// ═══════════════════════════════════════════════════════════════════

static void draw_calendar() {
    vf(0,0,VGA_WIDTH,VGA_HEIGHT,' ',0,0);
    vfr(0,' ',0,1);
    vs(2,0,"[ Nyx Calendar ]",15,1);
    char date_str[9]; rtc_get_date_str(date_str);
    vs(52,0,"Today:",8,1); vs(59,0,date_str,14,1);
    vs(70,0,"ESC:back",7,1);

    vfr(1,' ',0,8);
    vs(2,1,"< PREV",14,8);
    char my_str[32];
    const char* mn = month_names[view_month];
    int p=0;
    for(int i=0;mn[i];i++) my_str[p++]=mn[i];
    my_str[p++]=' ';
    char yr[8]; n2s(view_year,yr);
    for(int i=0;yr[i];i++) my_str[p++]=yr[i];
    my_str[p]='\0';
    vc(1,my_str,15,8);
    vs(70,1,"NEXT >",14,8);

    vfr(2,'-',8,0);
    const char* dnames[]={"  Sun ","  Mon ","  Tue ","  Wed ","  Thu ","  Fri ","  Sat "};
    for(int i=0;i<7;i++) {
        uint8_t fg=(i==0||i==6)?12:15;
        vs(2+i*11,3,dnames[i],fg,0);
    }
    vfr(4,'-',8,0);

    int first_dow = day_of_week(1,view_month,view_year);
    int dim = days_in_month(view_month,view_year);
    int col = first_dow;
    int row = 0;

    for(int d=1;d<=dim;d++){
        int cx = 1  + col*11;
        int cy = 5  + row*4;
        int is_today=(d==today_d&&view_month==today_m&&view_year==today_y);
        int is_sel  =(d==sel_day);
        int is_wkend=(col==0||col==6);
        uint8_t bg = is_sel?11:(is_today?1:0);
        uint8_t fg = is_sel?0:(is_today?15:(is_wkend?12:15));

        vf(cx,cy,11,3,' ',fg,bg);
        for(int x=cx;x<cx+11;x++){vp(x,cy,'.',8,bg);}
        for(int y=cy;y<cy+3;y++){vp(cx,y,'.',8,bg); vp(cx+10,y,'.',8,bg);}

        char dn[4]; n2s(d,dn);
        if(d<10) {vp(cx+1,cy+1,' ',fg,bg); vp(cx+2,cy+1,'0'+d,fg,bg);}
        else      vs(cx+1,cy+1,dn,fg,bg);

        if(is_today) vs(cx+4,cy+1,"TODAY",14,bg);
        else if(is_sel) vs(cx+4,cy+1," SEL ",0,11);

        int evt_n = events_on_day(view_year,view_month,d);
        if(evt_n>0){
            int shown=0;
            for(int i=0;i<cal_event_count&&shown<3;i++){
                if(cal_events[i].year==view_year&&cal_events[i].month==view_month&&cal_events[i].day==d){
                    vp(cx+3+shown*2,cy+2,'*',cal_events[i].color,bg);
                    shown++;
                }
            }
            if(evt_n>3) vp(cx+9,cy+2,'+',14,bg);
        }
        col++;
        if(col==7){col=0;row++;}
    }

    vfr(22,'-',8,0);
    int evt_n = events_on_day(view_year,view_month,sel_day);
    char sd[3],sm[3]; n2(sel_day,sd); n2(view_month,sm);
    vs(2,23,"Selected: ",8,0);
    vs(12,23,sd,14,0); vp(14,23,'/',7,0);
    vs(15,23,sm,14,0); vp(17,23,'/',7,0);
    char sy[8]; n2s(view_year,sy); vs(18,23,sy,14,0);
    char en[4]; n2s(evt_n,en);
    vs(25,23,"Events:",8,0); vs(33,23,en,14,0);
    vs(38,23,"A:add  L:list  LEFT/RIGHT:day  UP/DN:week",7,0);

    if(status_timer>0){
        vf(2,22,76,1,' ',0,0);
        vs(2,22,status_msg,10,0);
        status_timer--;
    }
}

static void draw_event_list() {
    vf(0,0,VGA_WIDTH,VGA_HEIGHT,' ',0,0);
    vfr(0,' ',0,1);
    vs(2,0,"[ Calendar - Events ]",15,1);
    vs(70,0,"ESC:back",7,1);

    char sd[3],sm[3],sy[8];
    n2(sel_day,sd); n2(view_month,sm); n2s(view_year,sy);

    vf(2,2,76,3,' ',0,1);
    char hdr[48];
    const char* mn=month_names[view_month];
    int p=0;
    for(int i=0;mn[i];i++) hdr[p++]=mn[i];
    hdr[p++]=' ';
    for(int i=0;sd[i];i++) hdr[p++]=sd[i];
    hdr[p++]=','; hdr[p++]=' ';
    for(int i=0;sy[i];i++) hdr[p++]=sy[i];
    hdr[p]='\0';
    vc(3,hdr,15,1);

    int row=6, idx=0, shown=0;
    for(int i=0;i<cal_event_count&&row<20;i++){
        if(cal_events[i].year==view_year&&cal_events[i].month==view_month&&cal_events[i].day==sel_day){
            uint8_t fg=(idx==list_sel)?0:15;
            uint8_t bg=(idx==list_sel)?14:0;
            vf(4,row,72,1,' ',fg,bg);
            vp(5,row,'*',cal_events[i].color,bg);
            vs(7,row,cal_events[i].title,fg,bg);
            row++; idx++; shown++;
        }
    }
    if(shown==0){
        vc(10,"No events for this day.",8,0);
        vc(12,"Press A to add an event.",7,0);
    }

    vfr(21,'-',8,0);
    vf(2,22,76,1,' ',0,8);
    vs(3, 22," UP/DOWN:select ",15,8);
    vs(20,22," A:add event    ",15,8);
    vs(37,22," D:delete       ",12,8);
    vs(54,22," ESC:back       ",7,8);

    if(del_confirm){
        vf(20,10,40,6,' ',15,4);
        for(int x=20;x<60;x++){vp(x,10,'=',12,4);vp(x,15,'=',12,4);}
        vc(11,"Delete this event?",15,4);
        if(idx>0&&list_sel<idx){
            int fi=0,ci=0;
            for(int i=0;i<cal_event_count;i++){
                if(cal_events[i].year==view_year&&cal_events[i].month==view_month&&cal_events[i].day==sel_day){
                    if(ci==list_sel){ vs(25,12,cal_events[fi].title,14,4); break; }
                    ci++;
                }
                fi++;
            }
        }
        vc(13,"ENTER:confirm  ESC:cancel",15,4);
    }
}

static void draw_add_event() {
    vf(0,0,VGA_WIDTH,VGA_HEIGHT,' ',0,0);
    vfr(0,' ',0,1);
    vs(2,0,"[ Calendar - Add Event ]",15,1);
    vs(70,0,"ESC:back",7,1);

    char sd[3],sm[3],sy[8];
    n2(sel_day,sd); n2(view_month,sm); n2s(view_year,sy);
    vs(2,2,"Adding event for:",8,0);
    vs(20,2,month_names[view_month],14,0);
    vs(32,2,sd,14,0); vs(35,2,sy,14,0);

    vf(10,4,60,14,' ',15,1);
    for(int x=10;x<70;x++){vp(x,4,'=',14,1);vp(x,17,'=',14,1);}
    for(int y=4;y<=17;y++){vp(10,y,'|',14,1);vp(69,y,'|',14,1);}
    vc(5,"[ New Event ]",15,1);

    vs(12,7,"Event Title:",11,1);
    uint8_t tfg=(add_field==0)?0:15;
    uint8_t tbg=(add_field==0)?14:8;
    vf(12,8,55,1,' ',tfg,tbg);
    vs(12,8,add_title,tfg,tbg);
    if(add_field==0) vp(12+add_title_pos,8,'_',0,15);
    if(add_field==0) vs(12,9,"Type the event name, press TAB to next",7,1);

    vs(12,11,"Event Color:",11,1);
    uint8_t cfgx=(add_field==1)?0:15;
    uint8_t cbg=(add_field==1)?14:0;
    vf(12,12,56,1,' ',cfgx,cbg);
    const uint8_t event_colors[]={10,12,14,11,13,9,15,6};
    const char* color_names[]={"Green","Red  ","Yellw","Cyan ","Mgnt ","Blue ","White","Brown"};
    for(int i=0;i<8;i++){
        uint8_t bg=(event_colors[i]==add_color)?event_colors[i]:0;
        uint8_t fg=(event_colors[i]==add_color)?0:event_colors[i];
        vp(12+i*6,12,' ',fg,bg);
        vs(13+i*6,12,color_names[i],fg,bg);
    }
    vs(12,13,"Preview:",8,1);
    vp(21,13,'*',add_color,1);
    vs(23,13,add_title[0]?add_title:"(your event)",add_color,1);

    vf(12,15,55,1,' ',0,8);
    vs(13,15," TAB:next field  ENTER:save  ESC:cancel ",15,8);
    if(add_field==1) vs(12,9,"LEFT/RIGHT to cycle colors, ENTER to save",7,1);
    if(status_timer>0) vs(12,16,status_msg,10,1);
}

static void vga_calendar_draw() {
    switch(cal_mode){
        case CAL_VIEW: draw_calendar();   break;
        case CAL_ADD:  draw_add_event();  break;
        case CAL_LIST: draw_event_list(); break;
    }
}

// ═══════════════════════════════════════════════════════════════════
//  VESA UI
// ═══════════════════════════════════════════════════════════════════

#define CL_BG      0x0E0C22
#define CL_TITLE   0x1A1440
#define CL_BORDER  0x2B2448
#define CL_DIVIDER 0x201D38
#define CL_CARD    0x1A1838
#define CL_SEL     0x352878
#define CL_TEXT    0xE0D8FF
#define CL_DIM     0x6858A0
#define CL_ACCENT  0x9B7FEE
#define CL_SUCCESS 0x55EE88
#define CL_DANGER  0xFF6655
#define CL_HINT    0x504870

static const uint32_t VRGB[16]={
    0x000000,0x0000AA,0x00AA00,0x00AAAA,
    0xAA0000,0xAA00AA,0xAA5500,0xAAAAAA,
    0x555555,0x5555FF,0x55FF55,0x55FFFF,
    0xFF5555,0xFF55FF,0xFFFF55,0xFFFFFF
};

static int cl_len(const char*s){int i=0;while(s[i])i++;return i;}
static void cl_str_r(int rx,int y,const char*s,uint32_t f,uint32_t b){
    vesa_draw_string(rx-cl_len(s)*8,y,s,f,b);
}
static void cl_pill(int x,int y,const char*s,uint32_t f,uint32_t b){
    int w=cl_len(s)*8+16;
    vesa_fill_rect(x,y,w,22,b);vesa_draw_rect(x,y,w,22,f);
    vesa_draw_string(x+8,y+3,s,f,b);
}

static void vk_month_grid(){
    int W=(int)vesa.width,H=(int)vesa.height;
    int wx=16,wy=40,ww=W-32,wh=H-50;
    vesa_fill_rect(wx,wy,ww,wh,CL_BG);

    // Title bar
    vesa_fill_rect(wx,wy,ww,36,CL_TITLE);
    vesa_hline(wx,wy+36,ww,CL_BORDER);
    g_win_x = wx; g_win_y = wy; g_win_w = ww; g_win_h = wh;
    draw_close_btn(wx+ww-40, wy+10, CL_TITLE, "Calendar");
    vesa_draw_string(wx+16,wy+10,"Nyx Calendar",CL_TEXT,CL_TITLE);
    char ds[9]; rtc_get_date_str(ds);
    cl_str_r(wx+ww-16,wy+10,ds,CL_ACCENT,CL_TITLE);

    // Month nav bar
    int ny=wy+36;
    vesa_fill_rect(wx,ny,ww,36,0x14122A);
    vesa_hline(wx,ny+36,ww,CL_BORDER);
    cl_pill(wx+16,ny+7,"< Prev",CL_DIM,0x100E20);
    char my[32]; int p=0;
    const char*mn=month_names[view_month];
    for(int i=0;mn[i];i++)my[p++]=mn[i]; my[p++]=' ';
    char yr[8]; n2s(view_year,yr);
    for(int i=0;yr[i];i++)my[p++]=yr[i]; my[p]=0;
    for(int i=0;my[i];i++) vesa_draw_char_scaled(wx+ww/2-cl_len(my)*8+i*16,ny+7,my[i],CL_TEXT,0x14122A,2);
    cl_pill(wx+ww-16-(cl_len("Next >")*8+16),ny+7,"Next >",CL_DIM,0x100E20);

    // Day headers
    int gy=ny+36;
    vesa_fill_rect(wx,gy,ww,26,0x100E24);
    vesa_hline(wx,gy+26,ww,CL_BORDER);
    const char*dnames[]={"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    int cell_w=ww/7;
    for(int i=0;i<7;i++){
        uint32_t fg=(i==0||i==6)?0xFF8877:CL_TEXT;
        int tx=wx+i*cell_w+(cell_w-cl_len(dnames[i])*8)/2;
        vesa_draw_string(tx,gy+6,dnames[i],fg,0x100E24);
    }

    // Grid
    int grid_y=gy+26;
    int grid_h=wh-36-36-26-40; /* leave room for bottom panel */
    int first_dow=day_of_week(1,view_month,view_year);
    int dim=days_in_month(view_month,view_year);
    int rows=(first_dow+dim+6)/7;
    int cell_h=grid_h/(rows>0?rows:1);

    int col=first_dow, row=0;
    for(int d=1;d<=dim;d++){
        int cx=wx+col*cell_w, cy=grid_y+row*cell_h;
        int is_today=(d==today_d&&view_month==today_m&&view_year==today_y);
        int is_sel=(d==sel_day);
        int is_wkend=(col==0||col==6);

        // Hover highlight — subtle border when the cursor is over this
        // cell and it's not already the selected day.
        int is_hover = (!is_sel &&
                        mouse.px >= cx+1 && mouse.px < cx+cell_w-1 &&
                        mouse.py >= cy+1 && mouse.py < cy+cell_h-1);

        uint32_t bg = is_sel?CL_SEL:(is_today?0x152838:CL_BG);
        uint32_t fg = is_sel?CL_TEXT:(is_today?CL_SUCCESS:(is_wkend?0xFF8877:CL_TEXT));

        vesa_fill_rect(cx+1,cy+1,cell_w-2,cell_h-2,bg);
        if(is_sel){
            vesa_draw_rect(cx+1,cy+1,cell_w-2,cell_h-2,CL_ACCENT);
        } else if (is_hover) {
            vesa_draw_rect(cx+1,cy+1,cell_w-2,cell_h-2,0x6050A0);
            vesa_hline(cx,cy+cell_h-1,cell_w,CL_DIVIDER);
            vesa_vline(cx+cell_w-1,cy,cell_h,CL_DIVIDER);
        } else {
            vesa_hline(cx,cy+cell_h-1,cell_w,CL_DIVIDER);
            vesa_vline(cx+cell_w-1,cy,cell_h,CL_DIVIDER);
        }

        char dn[4]; n2s(d,dn);
        vesa_draw_string(cx+8,cy+6,dn,fg,bg);
        if(is_today) cl_pill(cx+cell_w-56,cy+4,"today",CL_SUCCESS,0x0A2818);

        // Event dots
        int evt_n=events_on_day(view_year,view_month,d);
        if(evt_n>0){
            int shown=0, dx=cx+8;
            for(int i=0;i<cal_event_count&&shown<4;i++){
                if(cal_events[i].year==view_year&&cal_events[i].month==view_month&&cal_events[i].day==d){
                    vesa_fill_rect(dx,cy+cell_h-14,8,8,VRGB[cal_events[i].color]);
                    dx+=11; shown++;
                }
            }
            if(evt_n>4) vesa_draw_string(dx,cy+cell_h-16,"+",CL_DIM,bg);
        }

        col++;
        if(col==7){col=0;row++;}
    }

    // Bottom panel
    int by=grid_y+grid_h;
    vesa_fill_rect(wx,by,ww,40,CL_TITLE);
    vesa_hline(wx,by,ww,CL_BORDER);
    int evt_n=events_on_day(view_year,view_month,sel_day);
    char sd[3],sm[3]; n2(sel_day,sd); n2(view_month,sm);
    char sy[8]; n2s(view_year,sy);
    char selstr[24]; int sp=0;
    for(int i=0;sd[i];i++)selstr[sp++]=sd[i]; selstr[sp++]='/';
    for(int i=0;sm[i];i++)selstr[sp++]=sm[i]; selstr[sp++]='/';
    for(int i=0;sy[i];i++)selstr[sp++]=sy[i]; selstr[sp]=0;
    vesa_draw_string(wx+16,by+11,"Selected",CL_DIM,CL_TITLE);
    vesa_draw_string(wx+96,by+11,selstr,CL_ACCENT,CL_TITLE);
    char en[8]; n2s(evt_n,en);
    char evtstr[16]; int ep=0;
    for(int i=0;en[i];i++)evtstr[ep++]=en[i];
    const char*suffix=evt_n==1?" event":" events";
    for(int i=0;suffix[i];i++)evtstr[ep++]=suffix[i]; evtstr[ep]=0;
    vesa_draw_string(wx+220,by+11,evtstr,evt_n>0?CL_SUCCESS:CL_DIM,CL_TITLE);

    if(status_timer>0){
        vesa_draw_string(wx+ww-260,by+11,status_msg,CL_SUCCESS,CL_TITLE);
    } else {
        vesa_draw_string(wx+ww-420,by+11,
            "A add   L list   arrows navigate   , . month   ESC back",CL_HINT,CL_TITLE);
    }
}

static void vk_event_list(){
    int W=(int)vesa.width,H=(int)vesa.height;
    int wx=16,wy=40,ww=W-32,wh=H-50;
    vesa_fill_rect(wx,wy,ww,wh,CL_BG);

    vesa_fill_rect(wx,wy,ww,36,CL_TITLE);
    vesa_hline(wx,wy+36,ww,CL_BORDER);
    vesa_draw_string(wx+16,wy+10,"Calendar — Events",CL_TEXT,CL_TITLE);
    cl_str_r(wx+ww-16,wy+10,"ESC back",CL_DIM,CL_TITLE);

    // Date header
    char sd[3],sm[3],sy[8]; n2(sel_day,sd); n2(view_month,sm); n2s(view_year,sy);
    char hdr[48]; int p=0;
    const char*mn=month_names[view_month];
    for(int i=0;mn[i];i++)hdr[p++]=mn[i]; hdr[p++]=' ';
    for(int i=0;sd[i];i++)hdr[p++]=sd[i]; hdr[p++]=','; hdr[p++]=' ';
    for(int i=0;sy[i];i++)hdr[p++]=sy[i]; hdr[p]=0;
    int cy=wy+36+16;
    for(int i=0;hdr[i];i++) vesa_draw_char_scaled(wx+(ww-cl_len(hdr)*16)/2+i*16,cy,hdr[i],CL_ACCENT,CL_BG,2);
    cy+=48;
    vesa_hline(wx+16,cy,ww-32,CL_DIVIDER); cy+=16;

    int row_h=44, idx=0, shown=0;
    for(int i=0;i<cal_event_count;i++){
        if(cal_events[i].year==view_year&&cal_events[i].month==view_month&&cal_events[i].day==sel_day){
            int ry=cy+idx*row_h;
            if(ry+row_h>wy+wh-56) break;
            int sel=(idx==list_sel);
            uint32_t bg=sel?CL_SEL:CL_CARD;
            uint32_t fg=sel?CL_TEXT:0xB0A0D8;
            vesa_fill_rect(wx+16,ry,ww-32,row_h-4,bg);
            vesa_draw_rect(wx+16,ry,ww-32,row_h-4,sel?CL_ACCENT:CL_BORDER);
            if(sel) vesa_fill_rect(wx+16,ry,4,row_h-4,CL_ACCENT);
            vesa_fill_rect(wx+32,ry+(row_h-4)/2-6,12,12,VRGB[cal_events[i].color]);
            vesa_draw_string(wx+56,ry+12,cal_events[i].title,fg,bg);
            idx++; shown++;
        }
    }
    if(shown==0){
        vesa_draw_string(wx+(ww-cl_len("No events for this day")*8)/2,cy+20,
                         "No events for this day",CL_DIM,CL_BG);
        vesa_draw_string(wx+(ww-cl_len("Press A to add one")*8)/2,cy+44,
                         "Press A to add one",CL_HINT,CL_BG);
    }

    int by=wy+wh-40;
    vesa_fill_rect(wx,by,ww,40,CL_TITLE);
    vesa_hline(wx,by,ww,CL_BORDER);

    // ── Pill buttons in bottom bar (hover-highlighted via mouse.px/py) ──
    int pill_y = by + 9;
    int pill_h = 22;
    int pill_gap = 12;

    // Pill 1: "Add"
    int p1w = cl_len("Add") * 8 + 16;
    int p1x = wx + 16;
    int p1_hov = (mouse.px >= p1x && mouse.px < p1x + p1w &&
                  mouse.py >= pill_y && mouse.py < pill_y + pill_h);
    cl_pill(p1x, pill_y, "Add", p1_hov ? CL_SUCCESS : CL_DIM,
            p1_hov ? 0x0A280E : 0x100E20);

    // Pill 2: "Delete"
    int p2w = cl_len("Delete") * 8 + 16;
    int p2x = p1x + p1w + pill_gap;
    int p2_hov = (mouse.px >= p2x && mouse.px < p2x + p2w &&
                  mouse.py >= pill_y && mouse.py < pill_y + pill_h);
    cl_pill(p2x, pill_y, "Delete", p2_hov ? CL_DANGER : CL_DIM,
            p2_hov ? 0x3A0808 : 0x100E20);

    // Text hint between pills
    int tx = p2x + p2w + pill_gap;
    vesa_draw_string(tx, by + 12, "UP/DN select", CL_HINT, CL_TITLE);

    // Pill 3: "ESC back" right-aligned
    int p3w = cl_len("ESC back") * 8 + 16;
    int p3x = wx + ww - 16 - p3w;
    int p3_hov = (mouse.px >= p3x && mouse.px < p3x + p3w &&
                  mouse.py >= pill_y && mouse.py < pill_y + pill_h);
    cl_pill(p3x, pill_y, "ESC back", p3_hov ? CL_ACCENT : CL_DIM,
            p3_hov ? 0x1A0848 : 0x100E20);

    if(del_confirm){
        int ow=380,oh=130,ox=wx+(ww-ow)/2,oy=wy+(wh-oh)/2;
        vesa_fill_rect(ox,oy,ow,oh,0x3A0808);
        vesa_draw_rect(ox,oy,ow,oh,CL_DANGER);
        vesa_fill_rect(ox,oy,ow,3,CL_DANGER);
        vesa_draw_string(ox+16,oy+14,"Delete this event?",CL_DANGER,0x3A0808);
        if(idx>0&&list_sel<idx){
            int fi=0,ci=0;
            for(int i=0;i<cal_event_count;i++){
                if(cal_events[i].year==view_year&&cal_events[i].month==view_month&&cal_events[i].day==sel_day){
                    if(ci==list_sel){ vesa_draw_string(ox+16,oy+44,cal_events[fi].title,CL_TEXT,0x3A0808); break; }
                    ci++;
                }
                fi++;
            }
        }
        vesa_draw_string(ox+16,oy+oh-28,"ENTER confirm   ESC cancel",CL_HINT,0x3A0808);
    }
}

static void vk_add_event(){
    int W=(int)vesa.width,H=(int)vesa.height;
    int wx=16,wy=40,ww=W-32,wh=H-50;
    vesa_fill_rect(wx,wy,ww,wh,CL_BG);

    vesa_fill_rect(wx,wy,ww,36,CL_TITLE);
    vesa_hline(wx,wy+36,ww,CL_BORDER);
    vesa_draw_string(wx+16,wy+10,"Calendar — Add Event",CL_TEXT,CL_TITLE);
    cl_str_r(wx+ww-16,wy+10,"ESC cancel",CL_DIM,CL_TITLE);

    char sd[3],sy[8]; n2(sel_day,sd); n2s(view_year,sy);
    char datestr[32]; int dp=0;
    const char*mn=month_names[view_month];
    for(int i=0;mn[i];i++)datestr[dp++]=mn[i];
    datestr[dp++]=' ';
    for(int i=0;sd[i];i++)datestr[dp++]=sd[i];
    datestr[dp++]=',';
    datestr[dp++]=' ';
    for(int i=0;sy[i];i++)datestr[dp++]=sy[i];
    datestr[dp]=0;

    int fx=wx+(ww-560)/2; if(fx<wx+16)fx=wx+16;
    int fw=ww-32<560?ww-32:560;
    int fy=wy+56;

    vesa_fill_rect(fx,fy,fw,260,CL_CARD);
    vesa_draw_rect(fx,fy,fw,260,CL_BORDER);
    vesa_fill_rect(fx,fy,fw,3,CL_ACCENT);
    vesa_draw_string(fx+16,fy+12,"New Event for",CL_DIM,CL_CARD);
    vesa_draw_string(fx+16,fy+30,datestr,CL_ACCENT,CL_CARD);

    int py=fy+64;
    vesa_draw_string(fx+16,py,"Event Title",add_field==0?CL_TEXT:CL_DIM,CL_CARD); py+=20;
    uint32_t tbg=add_field==0?0x201848:0x100E20;
    vesa_fill_rect(fx+16,py,fw-32,30,tbg);
    vesa_draw_rect(fx+16,py,fw-32,30,add_field==0?CL_ACCENT:CL_BORDER);
    vesa_draw_string(fx+24,py+7,add_title,CL_TEXT,tbg);
    if(add_field==0) vesa_draw_string(fx+24+add_title_pos*8,py+7,"_",CL_ACCENT,tbg);
    py+=48;

    vesa_draw_string(fx+16,py,"Event Color",add_field==1?CL_TEXT:CL_DIM,CL_CARD); py+=22;
    const uint8_t event_colors[]={10,12,14,11,13,9,15,6};
    int sw=(fw-32-7*6)/8;
    for(int i=0;i<8;i++){
        int sx=fx+16+i*(sw+6);
        int sel=(event_colors[i]==add_color);
        vesa_fill_rect(sx,py,sw,sw,VRGB[event_colors[i]]);
        if(sel) vesa_draw_rect(sx-2,py-2,sw+4,sw+4,CL_ACCENT);
    }
    py+=sw+24;

    vesa_draw_string(fx+16,py,"Preview",CL_DIM,CL_CARD); py+=20;
    vesa_fill_rect(fx+16,py,12,12,VRGB[add_color]);
    vesa_draw_string(fx+36,py-2,add_title[0]?add_title:"(your event)",VRGB[add_color],CL_CARD);

    int by=fy+260-40;
    vesa_hline(fx+16,by,fw-32,CL_DIVIDER);
    if(add_field==0)
        vesa_draw_string(fx+16,by+10,"TAB next field",CL_HINT,CL_CARD);
    else
        vesa_draw_string(fx+16,by+10,"LEFT/RIGHT color   ENTER save",CL_HINT,CL_CARD);
    cl_str_r(fx+fw-16,by+10,"ENTER save",CL_SUCCESS,CL_CARD);

    if(status_timer>0)
        vesa_draw_string(fx+16,fy+260+16,status_msg,CL_DANGER,CL_BG);
}

static void vesa_calendar_draw(){
    switch(cal_mode){
        case CAL_VIEW: vk_month_grid();  break;
        case CAL_ADD:  vk_add_event();   break;
        case CAL_LIST: vk_event_list();  break;
    }
}

// ═══════════════════════════════════════════════════════════════════
//  PUBLIC API
// ═══════════════════════════════════════════════════════════════════

void calendar_draw() {
    if(vesa.active){ vesa_calendar_draw(); return; }
    vga_calendar_draw();
}

void calendar_handle_key(char c) {

    if(cal_mode==CAL_ADD){
        if(c==27){cal_mode=CAL_VIEW;return;}
        if(c=='\t'){add_field=(add_field+1)%2;return;}
        if(add_field==0){
            if(c==13){
                if(do_add_event()) cal_mode=CAL_VIEW;
                return;
            }
            if(c==8&&add_title_pos>0){
                add_title[--add_title_pos]='\0';
                return;
            }
            if(c>=32&&c<127&&add_title_pos<MAX_EVT_LEN-1){
                add_title[add_title_pos++]=c;
                add_title[add_title_pos]='\0';
                return;
            }
        }
        if(add_field==1){
            const uint8_t ec[]={10,12,14,11,13,9,15,6};
            int ci=0;
            for(int i=0;i<8;i++) if(ec[i]==add_color){ci=i;break;}
            // Left = D, Right = C — kept as two separate checks (see
            // the day-navigation fix below for why merging these with
            // an || is the exact bug that made left/right do the same
            // thing elsewhere in this file).
            if(c=='C'){ci=(ci+1)%8;add_color=ec[ci];return;}
            if(c=='D'){ci=(ci+7)%8;add_color=ec[ci];return;}
            if(c==13){if(do_add_event()) cal_mode=CAL_VIEW;return;}
        }
        return;
    }

    if(cal_mode==CAL_LIST){
        if(del_confirm){
            if(c==13){do_delete_event();del_confirm=0;return;}
            if(c==27){del_confirm=0;return;}
            return;
        }
        if(c==27){cal_mode=CAL_VIEW;return;}
        if(c=='A'&&list_sel>0){list_sel--;return;}
        if(c=='B'){list_sel++;return;}
        if(c=='a'){ open_add_form(); return; }
        if(c=='d'||c=='D'){del_confirm=1;return;}
        return;
    }

    // CAL_VIEW
    if(c==27) return; // Handled by desktop

    // Day navigation. IMPORTANT: right and left must be two SEPARATE
    // checks. The original code had `if(c=='C'||c=='D'){ next_day }`
    // followed by a separate `if(c=='D'){ prev_day }` — since 'D'
    // (left arrow) matched the first combined check and that branch
    // returned immediately, pressing LEFT always moved to the NEXT
    // day instead of the previous one; the intended prev-day branch
    // was unreachable dead code. Fixed by giving each direction its
    // own check with no key shared between them.
    if(c=='C'){ goto_next_day(); return; }
    if(c=='D'){ goto_prev_day(); return; }
    if(c=='B'){ goto_next_week(); return; }
    if(c=='A'){ goto_prev_week(); return; }

    if(c==','){ goto_prev_month(); return; }
    if(c=='.'){ goto_next_month(); return; }

    if(c=='t'||c=='T'){
        view_year=today_y; view_month=today_m; sel_day=today_d;
        return;
    }

    if(c=='a'){ open_add_form(); return; }

    if(c=='l'||c=='L'||c==13){
        cal_mode=CAL_LIST;
        list_sel=0; del_confirm=0;
        return;
    }

    if(c=='['){ goto_prev_month();  return; }
    if(c==']'){ goto_next_month();  return; }
}

// ── Mouse click handler ────────────────────────────────────────────
// Called from the desktop click dispatch when MODE_CALENDAR is active.
// Returns 1 if the click was handled (redraw needed), else 0.
int calendar_handle_mouse(int mx, int my) {
    if (!vesa.active) return 0;
    int W=(int)vesa.width, H=(int)vesa.height;
    int wx=16, wy=40, ww=W-32, wh=H-50;

    // ── CAL_VIEW: month grid + nav pills ────────────────────────
    if (cal_mode == CAL_VIEW) {
        // ── Prev / Next month pills ─────────────────────────────
        int ny = wy + 36;       // month nav bar Y
        int pill_h = 22;
        int pill_y = ny + 7;
        // "< Prev" pill at (wx+16, pill_y)
        if (my >= pill_y && my < pill_y + pill_h) {
            int prev_x = wx + 16;
            int prev_w = cl_len("< Prev") * 8 + 16;  // 6*8+16=64
            if (mx >= prev_x && mx < prev_x + prev_w) {
                goto_prev_month();
                return 1;
            }
            int next_w = cl_len("Next >") * 8 + 16;  // 6*8+16=64
            int next_x = wx + ww - 16 - next_w;
            if (mx >= next_x && mx < next_x + next_w) {
                goto_next_month();
                return 1;
            }
        }

        // ── Day grid cells ──────────────────────────────────────
        int gy = ny + 36;       // day header Y (ends at gy+26)
        int grid_y = gy + 26;   // grid start Y
        int first_dow = day_of_week(1, view_month, view_year);
        int dim = days_in_month(view_month, view_year);
        int rows = (first_dow + dim + 6) / 7;
        int grid_h = wh - 36 - 36 - 26 - 40;
        int cell_h = rows > 0 ? grid_h / rows : 1;
        int cell_w = ww / 7;

        // Check if click is within the grid area
        if (mx >= wx && mx < wx + ww && my >= grid_y && my < grid_y + grid_h) {
            int col = (mx - wx) / cell_w;
            int row = (my - grid_y) / cell_h;
            if (col < 0) col = 0;
            if (col > 6) col = 6;
            // d = row * 7 + col - first_dow + 1
            int d = row * 7 + col - first_dow + 1;
            if (d < 1) d = 1;
            if (d > dim) d = dim;
            if (d >= 1 && d <= dim) {
                sel_day = d;
                // Open event list for this day (same as pressing Enter / L)
                cal_mode = CAL_LIST;
                list_sel = 0;
                del_confirm = 0;
                return 1;
            }
        }
        return 0;
    }

    // ── CAL_LIST: event rows + action hints ─────────────────────
    if (cal_mode == CAL_LIST) {
        // Count events on this day
        int evt_count = 0;
        for (int i = 0; i < cal_event_count; i++)
            if (cal_events[i].year == view_year &&
                cal_events[i].month == view_month &&
                cal_events[i].day == sel_day)
                evt_count++;

        // Layout — same as vk_event_list()
        int cy = wy + 36 + 16;        // scaled month header
        cy += 48;                     // after 2x title
        // vesa_hline at cy; cy+=16 gap
        int row_h = 44;
        int list_y = cy + 16;         // after divider
        int max_ry = wy + wh - 56 - row_h;  // bottom cutoff

        // ── Event row clicks ────────────────────────────────────
        int idx = 0;
        for (int i = 0; i < cal_event_count; i++) {
            if (!(cal_events[i].year == view_year &&
                  cal_events[i].month == view_month &&
                  cal_events[i].day == sel_day))
                continue;
            int ry = list_y + idx * row_h;
            if (my >= ry && my < ry + row_h - 4 && ry < max_ry) {
                if (del_confirm) {
                    // In delete confirm, clicking elsewhere cancels
                    del_confirm = 0;
                    return 1;
                }
                list_sel = idx;
                return 1;
            }
            idx++;
        }

        // ── Pill buttons in bottom bar ──────────────────────────
        int by = wy + wh - 40;       // bottom bar Y
        if (my >= by && my < by + 40) {
            int pill_y = by + 9;
            int pill_h = 22;
            int pill_gap = 12;

            // Pill 1: "Add"
            int p1w = cl_len("Add") * 8 + 16;
            int p1x = wx + 16;
            if (mx >= p1x && mx < p1x + p1w &&
                my >= pill_y && my < pill_y + pill_h && !del_confirm) {
                open_add_form();
                return 1;
            }

            // Pill 2: "Delete"
            int p2w = cl_len("Delete") * 8 + 16;
            int p2x = p1x + p1w + pill_gap;
            if (mx >= p2x && mx < p2x + p2w &&
                my >= pill_y && my < pill_y + pill_h && !del_confirm) {
                del_confirm = 1;
                return 1;
            }

            // Pill 3: "ESC back" right-aligned
            int p3w = cl_len("ESC back") * 8 + 16;
            int p3x = wx + ww - 16 - p3w;
            if (mx >= p3x && mx < p3x + p3w &&
                my >= pill_y && my < pill_y + pill_h) {
                if (del_confirm) {
                    del_confirm = 0;  // cancel confirm first
                } else {
                    cal_mode = CAL_VIEW;
                }
                return 1;
            }

            return 0;
        }

        // ── Delete confirm dialog buttons ───────────────────────
        if (del_confirm) {
            int dlg_w = 380, dlg_h = 130;
            int dlg_x = wx + (ww - dlg_w) / 2;
            int dlg_y = wy + (wh - dlg_h) / 2;
            // "ENTER confirm" at bottom of dialog
            int btn_y = dlg_y + dlg_h - 28;
            if (mx >= dlg_x + 16 && mx < dlg_x + dlg_w - 16 &&
                my >= btn_y && my < btn_y + 20) {
                do_delete_event();
                del_confirm = 0;
                return 1;
            }
            // "ESC cancel" — treat as cancel
            if (mx >= dlg_x && mx < dlg_x + dlg_w &&
                my >= dlg_y && my < dlg_y + dlg_h) {
                del_confirm = 0;
                return 1;
            }
        }

        return 0;
    }

    // ── CAL_ADD: color swatches ─────────────────────────────────
    if (cal_mode == CAL_ADD) {
        int fx = wx + (ww - 560)/2;
        if (fx < wx + 16) fx = wx + 16;
        int fw = (ww - 32 < 560) ? ww - 32 : 560;
        int fy = wy + 56;

        if (mx >= fx && mx < fx + fw && my >= fy && my < fy + 260) {
            // Layout same as vk_add_event()
            int py = fy + 64;          // "Event Title"
            py += 20;                  // label
            py += 30;                  // title input field
            py += 48;                  // gap
            // py now points to "Event Color" label
            py += 22;                  // color label

            const uint8_t event_colors[] = {10,12,14,11,13,9,15,6};
            int sw = (fw - 32 - 7*6) / 8;
            int sx = fx + 16;
            for (int i = 0; i < 8; i++) {
                int col_x = sx + i * (sw + 6);
                if (mx >= col_x && mx < col_x + sw &&
                    my >= py && my < py + sw) {
                    add_color = event_colors[i];
                    add_field = 1;
                    return 1;
                }
            }
        }
        return 0;
    }

    return 0;
}

// ─── Init ─────────────────────────────────────────────────────────
void calendar_init() {
    rtc_time_t t; rtc_read(&t);
    today_y=2000+t.year; today_m=t.month; today_d=t.day;
    view_year=today_y; view_month=today_m; sel_day=today_d;
    cal_mode=CAL_VIEW;
    list_sel=0; del_confirm=0;
    add_title[0]='\0'; add_title_pos=0;
    add_color=10; add_field=0;
    status_msg[0]='\0'; status_timer=0;

    if(cal_event_count==0){
        CalEvent* e=&cal_events[cal_event_count++];
        e->year=today_y; e->month=today_m; e->day=today_d;
        e->color=10; s_cpy(e->title,"Today - Nyx OS Launch!",MAX_EVT_LEN);

        e=&cal_events[cal_event_count++];
        e->year=today_y; e->month=today_m; e->day=15;
        e->color=12; s_cpy(e->title,"System Update",MAX_EVT_LEN);

        e=&cal_events[cal_event_count++];
        e->year=today_y; e->month=today_m; e->day=20;
        e->color=14; s_cpy(e->title,"Meeting",MAX_EVT_LEN);
    }
}
int calendar_is_main_view() { return cal_mode == CAL_VIEW; }
