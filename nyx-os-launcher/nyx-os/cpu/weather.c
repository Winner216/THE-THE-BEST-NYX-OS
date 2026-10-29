// cpu/weather.c – Nyx OS Weather App  (VGA ASCII art + VESA pixel scenes)
#include <weather.h>
#include <vesa.h>
#include <rtc.h>
#include <speaker.h>
#include <timezone.h>
#include <stdint.h>
#include <desktop.h>
#include <mouse.h>

extern void vesa_draw_char_scaled(int x,int y,char c,uint32_t fg,uint32_t bg,int scale);

// ═══════════════════════════════════════════════════════════════════
//  SHARED DATA
// ═══════════════════════════════════════════════════════════════════

Weather current_weather;
int     weather_show_widget = 1;

static int weather_tick_count  = 0;
static int weather_change_time = 4000;
static int rand_seed_w         = 77777;

static int wrand() {
    rand_seed_w=rand_seed_w*1103515245+12345;
    return (rand_seed_w>>16)&0x7FFF;
}

static const struct {
    WeatherType type;
    const char* name;
    const char* desc;
    int         temp;
    int         feels;
    int         humidity;
    int         wind;
    int         uv;
    const char* icon;
    uint8_t     color;
    uint8_t     sky_col;
} WP[] = {
    {WEATHER_SUNNY,  "Sunny",         "Clear skies",        32, 35, 35, 10, 9,  "*", 14, 1},
    {WEATHER_CLOUDY, "Partly Cloudy", "Some clouds",        24, 23, 60, 20, 4,  "~", 7,  8},
    {WEATHER_RAINY,  "Rainy",         "Light rainfall",     18, 16, 85, 25, 1,  "!", 11, 1},
    {WEATHER_STORMY, "Thunderstorm",  "Heavy storm",        15, 11, 95, 60, 0,  "#", 8,  0},
    {WEATHER_SNOWY,  "Snowing",       "Light snowfall",     -2, -6, 70, 15, 1,  "o", 15, 1},
    {WEATHER_WINDY,  "Windy",         "Strong winds",       20, 15, 45, 55, 5,  "~", 7,  1},
};
#define WP_COUNT 6

void weather_next() {
    int next=((int)current_weather.type+1)%WP_COUNT;
    current_weather.type     = WP[next].type;
    current_weather.name     = WP[next].name;
    current_weather.temp     = WP[next].temp;
    current_weather.humidity = WP[next].humidity;
    current_weather.wind     = WP[next].wind;
    current_weather.icon     = WP[next].icon;
    current_weather.color    = WP[next].color;
}

// ─── String helpers ───────────────────────────────────────────────
static void n2sw(int n, char* b) {
    if(n<0){
        b[0]='-'; n=-n;
        char t[8]; int p=1,tp=0;
        while(n>0){t[tp++]='0'+n%10;n/=10;}
        for(int i=0;i<tp;i++) b[p+i]=t[tp-1-i];
        b[p+tp]='\0'; return;
    }
    if(n==0){b[0]='0';b[1]='\0';return;}
    char t[8]; int p=0;
    while(n>0){t[p++]='0'+n%10;n/=10;}
    for(int i=0;i<p;i++) b[i]=t[p-1-i];
    b[p]='\0';
}
/* Convert a 24-hour hour into a correctly-labelled 12-hour string,
   e.g. 20 -> "08:48 PM" rather than the old code's "20:48 PM" (which
   mixed 24-hour and 12-hour notation in the same string). */
static void time12(int h24, int m, char* out) {
    int h12 = h24%12; if(h12==0) h12=12;
    int p=0;
    out[p++]='0'+h12/10; out[p++]='0'+h12%10; out[p++]=':';
    out[p++]='0'+m/10;   out[p++]='0'+m%10;   out[p++]=' ';
    out[p++]=(h24<12)?'A':'P'; out[p++]='M'; out[p]='\0';
}

// ═══════════════════════════════════════════════════════════════════
//  VGA FALLBACK
// ═══════════════════════════════════════════════════════════════════

#define VGA_WIDTH  80
#define VGA_HEIGHT 25
#define VGA_MEM    ((uint16_t*)0xB8000)

static inline uint16_t make_entry(char c, uint8_t fg, uint8_t bg) {
    return (uint16_t)c | (uint16_t)(fg | bg << 4) << 8;
}
static void P(int x, int y, char c, uint8_t fg, uint8_t bg) {
    if(x<0||x>=VGA_WIDTH||y<0||y>=VGA_HEIGHT) return;
    VGA_MEM[y*VGA_WIDTH+x] = make_entry(c,fg,bg);
}
static void S(int x, int y, const char* s, uint8_t fg, uint8_t bg) {
    for(int i=0;s[i];i++) P(x+i,y,s[i],fg,bg);
}
static void F(int x, int y, int w, int h, char c, uint8_t fg, uint8_t bg) {
    for(int dy=0;dy<h;dy++)
        for(int dx=0;dx<w;dx++)
            P(x+dx,y+dy,c,fg,bg);
}
static void FR(int y, uint8_t fg, uint8_t bg) {
    for(int x=0;x<VGA_WIDTH;x++) P(x,y,' ',fg,bg);
}
static void BOX(int x, int y, int w, int h, uint8_t col, uint8_t bg) {
    F(x,y,w,h,' ',col,bg);
    for(int i=x;i<x+w;i++){P(i,y,'-',col,bg);P(i,y+h-1,'-',col,bg);}
    for(int i=y;i<y+h;i++){P(x,i,'|',col,bg);P(x+w-1,i,'|',col,bg);}
    P(x,y,'+',col,bg); P(x+w-1,y,'+',col,bg);
    P(x,y+h-1,'+',col,bg); P(x+w-1,y+h-1,'+',col,bg);
}

// ─── Taskbar widget (VGA) ─────────────────────────────────────────
void weather_draw_widget() {
    if(!weather_show_widget) return;
    uint8_t bg = 1;
    uint8_t fg = current_weather.color;
    S(38,0,current_weather.icon,fg,bg);
    const char* sn[]={"SUN","CLD","RAN","STM","SNW","WND"};
    S(40,0,sn[(int)current_weather.type],fg,bg);
    char tmp[8]; n2sw(current_weather.temp,tmp);
    int p=0;while(tmp[p])p++;tmp[p++]='^';tmp[p++]='C';tmp[p]='\0';
    S(44,0,tmp,14,bg);
}

// ─── ASCII art ────────────────────────────────────────────────────
static void art_sun(int x, int y, uint8_t bg) {
    S(x,   y,   "  \\|/  ", 14, bg);
    S(x,   y+1, "- oOo -", 14, bg);
    S(x,   y+2, "  /|\\  ", 14, bg);
    P(x+3, y-1, '*', 14, bg);
    P(x+3, y+3, '*', 14, bg);
}
static void art_cloud(int x, int y, uint8_t col, uint8_t bg) {
    S(x,   y,   "  .---.   ", col, bg);
    S(x,   y+1, " ( ~~~ )  ", col, bg);
    S(x,   y+2, "  `---'   ", col, bg);
}
static void art_cloud_big(int x, int y, uint8_t col, uint8_t bg) {
    S(x,   y,   "   .~~~~~~.   ", col, bg);
    S(x,   y+1, " .(  ~  ~  ). ", col, bg);
    S(x,   y+2, "(  ~  ~  ~  ) ", col, bg);
    S(x,   y+3, " `~~~~~~~~~~' ", col, bg);
}
static void art_rain(int x, int y, uint8_t col, uint8_t bg) {
    S(x, y,   "/ / / / / / / ", col, bg);
    S(x, y+1, " / / / / / / /", col, bg);
    S(x, y+2, "/ / / / / / / ", col, bg);
    S(x, y+3, " / / / / / / /", col, bg);
}
static void art_lightning(int x, int y, uint8_t bg) {
    S(x,   y,   "  /\\  ", 14, bg);
    S(x,   y+1, " /##\\ ", 14, bg);
    S(x,   y+2, "/####\\", 14, bg);
    S(x+1, y+3, "\\##/ ", 14, bg);
    S(x+2, y+4, "\\/ ",  14, bg);
}
static void art_snow(int x, int y, uint8_t bg) {
    S(x, y,   " * . * . * . * ", 15, bg);
    S(x, y+1, ". * . * . * . *", 15, bg);
    S(x, y+2, " * . * . * . * ", 15, bg);
    S(x, y+3, "* . * . * . * .", 15, bg);
}
static void art_wind(int x, int y, uint8_t bg) {
    S(x, y,   "~~~~~~~~~~~~~~~~~~~~~~~~~", 7, bg);
    S(x, y+1, "  ~~~~~~~~~~~~~~~~~~~~~  ", 8, bg);
    S(x, y+2, "    ~~~~~~~~~~~~~~~~~~~  ", 7, bg);
    S(x, y+3, "  ~~~~~~~~~~~~~~~~~~~~~  ", 8, bg);
    S(x, y+4, "~~~~~~~~~~~~~~~~~~~~~~~~~", 7, bg);
}
static void draw_temp_bar(int x, int y, int temp) {
    int range = 65;
    int offset = temp + 20;
    if(offset<0) offset=0;
    if(offset>range) offset=range;
    int bar = offset * 20 / range;
    S(x, y, "Temp: [", 8, 0);
    for(int i=0;i<20;i++){
        uint8_t col;
        if(i < bar){
            if(temp < 0)       col = 11;
            else if(temp < 15) col = 10;
            else if(temp < 28) col = 14;
            else               col = 12;
        } else col = 8;
        P(x+7+i, y, (i<bar)?'#':'-', col, 0);
    }
    S(x+27, y, "]", 8, 0);
    char tmp[8]; n2sw(temp,tmp);
    int p=0;while(tmp[p])p++;tmp[p++]='^';tmp[p++]='C';tmp[p]='\0';
    S(x+29, y, tmp, 14, 0);
}
static void draw_wind_bar(int x, int y, int spd) {
    int bar = spd * 20 / 80;
    if(bar>20) bar=20;
    S(x, y, "Wind: [", 8, 0);
    for(int i=0;i<20;i++){
        uint8_t col = (i<bar)?(spd>50?12:(spd>30?14:9)):8;
        P(x+7+i, y, (i<bar)?'>':'-', col, 0);
    }
    S(x+27, y, "]", 8, 0);
    char wnd[8]; n2sw(spd,wnd);
    int p=0;while(wnd[p])p++;wnd[p++]=' ';wnd[p++]='k';wnd[p++]='m';wnd[p++]='/';wnd[p++]='h';wnd[p]='\0';
    S(x+29, y, wnd, 9, 0);
}
static void draw_hum_bar(int x, int y, int hum) {
    int bar = hum * 20 / 100;
    S(x, y, "Humi: [", 8, 0);
    for(int i=0;i<20;i++){
        uint8_t col = (i<bar)?(hum>80?11:(hum>50?9:3)):8;
        P(x+7+i, y, (i<bar)?'~':'-', col, 0);
    }
    S(x+27, y, "]", 8, 0);
    char hstr[8]; n2sw(hum,hstr);
    int p=0;while(hstr[p])p++;hstr[p++]='%';hstr[p]='\0';
    S(x+29, y, hstr, 11, 0);
}

static void vga_weather_draw() {
    F(0,0,VGA_WIDTH,VGA_HEIGHT,' ',0,0);
    int wtype = (int)current_weather.type;
    uint8_t sky = WP[wtype].sky_col;
    uint8_t wcol = current_weather.color;

    FR(0, 15, 1);
    S(2, 0, "[ Nyx Weather ]", 15, 1);
    S(52,0, "W:next  ESC:back", 7, 1);

    BOX(1, 1, 40, 14, wcol, 0);
    S(2, 1, "[ Weather ]", wcol, 0);
    F(2,2,38,11,' ',0,sky);

    switch(current_weather.type){
        case WEATHER_SUNNY:
            art_sun(17, 3, sky);
            S(3,  2, "* . . . . . . . . . . . . . . . . .", 14, sky);
            S(3,  12,"* . . . . . . . . . . . . . . . . .", 14, sky);
            S(3,  7, "*",14,sky);
            S(37, 7, "*",14,sky);
            F(2,11,38,2,' ',2,sky);
            S(2,11,"^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^",2,sky);
            S(2,12,"  Green meadows under bright sunshine  ",10,sky);
            break;
        case WEATHER_CLOUDY:
            art_cloud_big(5,  3, 7, sky);
            art_cloud(22,  2, 15, sky);
            art_cloud(5,   8, 8, sky);
            art_cloud(25,  7, 7, sky);
            S(30, 5, "\\|/", 14, sky);
            S(30, 6, "-o-", 14, sky);
            break;
        case WEATHER_RAINY:
            art_cloud_big(5, 2, 7, sky);
            art_cloud(22, 2, 15, sky);
            art_rain(3,  6, 11, sky);
            S(5, 12, "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~", 11, sky);
            S(8, 13, "  splash! splash! splash!  ", 9, sky);
            break;
        case WEATHER_STORMY:
            art_cloud_big(2,  2, 8, sky);
            art_cloud_big(20, 1, 8, sky);
            art_lightning(15, 6, sky);
            art_rain(3,  6, 9, sky);
            art_rain(25, 7, 9, sky);
            S(5, 12, "*** WARNING: SEVERE THUNDERSTORM ***", 12, sky);
            break;
        case WEATHER_SNOWY:
            art_cloud_big(5, 2, 15, sky);
            art_snow(3, 6, sky);
            F(2,11,38,3,' ',15,sky);
            S(2, 11,"~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~",15,sky);
            S(2, 12," *  **  *  *  **  *  *  **  *  *  ** ",15,sky);
            S(2, 13,"  **  *  *  **  *  *  **  *  *  **  *",7,sky);
            break;
        case WEATHER_WINDY:
            art_wind(3, 4, sky);
            S(5,  3, "*", 10, sky); S(15, 5, "*", 2, sky);
            S(25, 3, "*", 10, sky); S(35, 6, "*", 2, sky);
            S(10, 9, "*", 10, sky); S(30, 8, "*", 2, sky);
            S(32, 2, "  (((", 2, sky);
            S(31, 3, " /(  ", 2, sky);
            S(30, 4, "/|   ", 2, sky);
            S(31, 5, " |   ", 6, sky);
            S(31, 6, " |   ", 6, sky);
            S(30, 7, "---  ", 6, sky);
            break;
    }

    BOX(41, 1, 38, 14, wcol, 0);
    S(42, 1, "[ Conditions ]", wcol, 0);
    S(43, 2, "City    : ", 8, 0);
    S(53, 2, timezones[current_timezone].city, 15, 0);
    S(43, 3, "Condition:", 8, 0);
    S(54, 3, current_weather.name, wcol, 0);
    S(43, 4, "Summary : ", 8, 0);
    S(53, 4, WP[wtype].desc, 7, 0);

    char tmp[8]; n2sw(current_weather.temp,tmp);
    int p=0;while(tmp[p])p++;tmp[p++]='^';tmp[p++]='C';tmp[p]='\0';
    S(43, 6, "Temp    : ", 8, 0);
    S(53, 6, tmp, wcol, 0);

    char fls[8]; n2sw(WP[wtype].feels,fls);
    p=0;while(fls[p])p++;fls[p++]='^';fls[p++]='C';fls[p]='\0';
    S(43, 7, "Feels   : ", 8, 0);
    S(53, 7, fls, 7, 0);

    char hum[8]; n2sw(current_weather.humidity,hum);
    p=0;while(hum[p])p++;hum[p++]='%';hum[p]='\0';
    S(43, 8, "Humidity: ", 8, 0);
    S(53, 8, hum, 11, 0);

    char wnd[8]; n2sw(current_weather.wind,wnd);
    p=0;while(wnd[p])p++;wnd[p++]=' ';wnd[p++]='k';wnd[p++]='m';wnd[p++]='/';wnd[p++]='h';wnd[p]='\0';
    S(43, 9, "Wind    : ", 8, 0);
    S(53, 9, wnd, 9, 0);

    char uvs[4]; n2sw(WP[wtype].uv,uvs);
    S(43,10, "UV Index: ", 8, 0);
    S(53,10, uvs, WP[wtype].uv>6?12:WP[wtype].uv>3?14:10, 0);

    // Correctly-labelled 12-hour times (previously "20:48 PM" mixed
    // 24h and 12h notation — fixed via time12()).
    char sr[12],ss[12]; time12(6,15,sr); time12(20,48,ss);
    S(43,11, "Sunrise : ", 8, 0); S(53,11, sr, 14, 0);
    S(43,12, "Sunset  : ", 8, 0); S(53,12, ss, 14, 0);

    BOX(1, 15, 78, 5, 8, 0);
    S(2, 15, "[ Statistics ]", 8, 0);
    draw_temp_bar(3, 16, current_weather.temp);
    draw_wind_bar(3, 17, current_weather.wind);
    draw_hum_bar( 3, 18, current_weather.humidity);

    BOX(1, 20, 78, 4, wcol, 0);
    S(2, 20, "[ 6-Day Forecast ]", wcol, 0);
    const char* days[]={"Today","Tue  ","Wed  ","Thu  ","Fri  ","Sat  "};
    for(int i=0;i<6;i++){
        int fx=3+i*13;
        int fi=(wtype+i)%WP_COUNT;
        // Icon/color now come from WP[fi] (the actual forecast day's
        // weather) instead of a separate hardcoded array that didn't
        // correspond to the displayed name — that mismatch meant the
        // icon glyph and color shown often didn't match the weather
        // named right next to it.
        BOX(fx,21,12,2,WP[fi].color,0);
        S(fx+1,21,days[i],WP[fi].color,0);
        S(fx+1,22,WP[fi].icon,WP[fi].color,0);
        S(fx+3,22,WP[fi].name,WP[fi].color,0);
        char ft[6]; n2sw(WP[fi].temp,ft);
        p=0;while(ft[p])p++;ft[p++]='^';ft[p]='\0';
        S(fx+8,22,ft,14,0);
    }

    FR(24, 7, 8);
    S(2, 24, "W:cycle weather   ESC:back to desktop", 15, 8);
    S(55,24, "Nyx OS Weather", 8, 8);
}

// ═══════════════════════════════════════════════════════════════════
//  VESA UI
// ═══════════════════════════════════════════════════════════════════

#define WX_BG      0x0E0C22
#define WX_TITLE   0x1A1440
#define WX_BORDER  0x2B2448
#define WX_DIVIDER 0x201D38
#define WX_CARD    0x1A1838
#define WX_TEXT    0xE0D8FF
#define WX_DIM     0x6858A0
#define WX_ACCENT  0x9B7FFF
#define WX_SUCCESS 0x55EE88
#define WX_HINT    0x504870

static const uint32_t WRGB[16]={
    0x000000,0x0000AA,0x00AA00,0x00AAAA,
    0xAA0000,0xAA00AA,0xAA5500,0xAAAAAA,
    0x555555,0x5555FF,0x55FF55,0x55FFFF,
    0xFF5555,0xFF55FF,0xFFFF55,0xFFFFFF
};

static int wx_len(const char*s){int i=0;while(s[i])i++;return i;}
static void wx_str_r(int rx,int y,const char*s,uint32_t f,uint32_t b){
    vesa_draw_string(rx-wx_len(s)*8,y,s,f,b);
}
static void wx_pill(int x,int y,const char*s,uint32_t f,uint32_t b){
    int w=wx_len(s)*8+16;
    vesa_fill_rect(x,y,w,22,b);vesa_draw_rect(x,y,w,22,f);
    vesa_draw_string(x+8,y+3,s,f,b);
}
static void wx_bar(int x,int y,int w,int h,int pct,uint32_t filled,uint32_t empty){
    if(pct>100)pct=100;
    if(pct<0)pct=0;
    int f=w*pct/100;
    if(f>0)vesa_fill_rect(x,y,f,h,filled);
    if(f<w)vesa_fill_rect(x+f,y,w-f,h,empty);
    vesa_draw_rect(x,y,w,h,WX_BORDER);
}
static void wx_circle(int cx,int cy,int r,uint32_t col){
    for(int dy=-r;dy<=r;dy++)
        for(int dx=-r;dx<=r;dx++)
            if(dx*dx+dy*dy<=r*r) vesa_put_pixel(cx+dx,cy+dy,col);
}
static void wx_ellipse(int cx,int cy,int rx,int ry,uint32_t col){
    for(int dy=-ry;dy<=ry;dy++)
        for(int dx=-rx;dx<=rx;dx++)
            if(dx*dx*ry*ry+dy*dy*rx*rx<=rx*rx*ry*ry) vesa_put_pixel(cx+dx,cy+dy,col);
}

// ── Weather scene art (pixel space) ────────────────────────────────

static void scene_sun(int x,int y,int w,int h){
    int sx=x+w*72/100, sy=y+h*30/100, sr=h*16/100; if(sr<24)sr=24;
    for(int rr=sr+18;rr>sr;rr-=3) wx_circle(sx,sy,rr,0xFFE060-(uint32_t)(sr+18-rr)*0x030200);
    wx_circle(sx,sy,sr,0xFFD020);
    wx_circle(sx,sy,sr-6,0xFFEC80);
    // Sun rays (simple cross pattern)
    vesa_hline(sx-sr-16,sy,10,0xFFD020); vesa_hline(sx+sr+6,sy,10,0xFFD020);
    vesa_vline(sx,sy-sr-16,10,0xFFD020); vesa_vline(sx,sy+sr+6,10,0xFFD020);
    // Ground
    int gy=y+h*72/100;
    vesa_fill_rect(x,gy,w,y+h-gy,0x2C8C3C);
    vesa_hline(x,gy,w,0x40AA50);
    for(int i=0;i<w;i+=26) vesa_fill_rect(x+i,gy+6,3,h-  (gy-y)-6,0x226A2E);
}
static void scene_cloud_shape(int cx,int cy,int scale,uint32_t col){
    wx_ellipse(cx-scale*12/10,cy,scale,scale*7/10,col);
    wx_ellipse(cx+scale*12/10,cy,scale,scale*7/10,col);
    wx_ellipse(cx,cy-scale*4/10,scale*13/10,scale*9/10,col);
    wx_ellipse(cx,cy+scale*2/10,scale*18/10,scale*6/10,col);
}
static void scene_cloudy(int x,int y,int w,int h){
    int sx=x+w*70/100, sy=y+h*28/100, sr=h*11/100; if(sr<16)sr=16;
    wx_circle(sx,sy,sr,0xFFD866);
    scene_cloud_shape(x+w*30/100,y+h*32/100,h*13/100,0xF0F0F8);
    scene_cloud_shape(x+w*55/100,y+h*48/100,h*10/100,0xD8D8E8);
    scene_cloud_shape(x+w*15/100,y+h*55/100,h*9/100,0xC8C8DC);
    int gy=y+h*72/100;
    vesa_fill_rect(x,gy,w,y+h-gy,0x3A7048);
}
static void scene_rainy(int x,int y,int w,int h){
    scene_cloud_shape(x+w*35/100,y+h*24/100,h*15/100,0x585868);
    scene_cloud_shape(x+w*65/100,y+h*20/100,h*13/100,0x484858);
    int gy=y+h*68/100;
    vesa_fill_rect(x,gy,w,y+h-gy,0x1A2838);
    vesa_hline(x,gy,w,0x3868A0);
    // Rain streaks
    int seed=1234;
    for(int i=0;i<28;i++){
        seed=seed*1103515245+12345;
        int rx=x+((seed>>8)&0x3FF)%w;
        int ry=y+h*32/100+((seed>>16)&0xFF)%(h*30/100);
        if(rx>=x&&rx<x+w&&ry<gy){
            vesa_vline(rx,ry,14,0x5090D8);
            vesa_put_pixel(rx-1,ry+16,0x5090D8);
        }
    }
}
static void scene_stormy(int x,int y,int w,int h){
    scene_cloud_shape(x+w*30/100,y+h*22/100,h*17/100,0x383040);
    scene_cloud_shape(x+w*65/100,y+h*18/100,h*15/100,0x282030);
    int gy=y+h*68/100;
    vesa_fill_rect(x,gy,w,y+h-gy,0x141420);
    // Lightning bolt
    int lx=x+w*48/100, ly=y+h*35/100;
    int bolt[][2]={{0,0},{-8,20},{4,20},{-6,44},{14,16},{2,16},{10,0}};
    for(int i=0;i<6;i++){
        int x0=lx+bolt[i][0],y0=ly+bolt[i][1];
        int x1=lx+bolt[i+1][0],y1=ly+bolt[i+1][1];
        int steps=(y1-y0>0)?(y1-y0):1;
        for(int s=0;s<=steps;s++){
            int px=x0+(x1-x0)*s/(steps?steps:1);
            int py=y0+(y1-y0)*s/(steps?steps:1);
            vesa_fill_rect(px-2,py,4,3,0xFFF060);
        }
    }
    // Rain (heavier)
    int seed=9911;
    for(int i=0;i<36;i++){
        seed=seed*1103515245+12345;
        int rx=x+((seed>>8)&0x3FF)%w;
        int ry=y+h*30/100+((seed>>16)&0xFF)%(h*35/100);
        if(rx>=x&&rx<x+w&&ry<gy) vesa_vline(rx,ry,16,0x4878A8);
    }
}
static void scene_snowy(int x,int y,int w,int h){
    scene_cloud_shape(x+w*45/100,y+h*20/100,h*17/100,0xD8D8E8);
    int gy=y+h*66/100;
    vesa_fill_rect(x,gy,w,y+h-gy,0xE8ECF4);
    vesa_hline(x,gy,w,0xF8FAFF);
    int seed=555;
    for(int i=0;i<40;i++){
        seed=seed*1103515245+12345;
        int fx=x+((seed>>8)&0x3FF)%w;
        int fy=y+h*30/100+((seed>>16)&0xFF)%(h*45/100);
        if(fx>=x&&fx<x+w&&fy<gy) vesa_fill_rect(fx,fy,3,3,0xFFFFFF);
    }
}
static void scene_windy(int x,int y,int w,int h){
    int gy=y+h*70/100;
    vesa_fill_rect(x,gy,w,y+h-gy,0x3A7048);
    // Wavy wind lines
    for(int line=0;line<5;line++){
        int ly=y+h*(20+line*12)/100;
        uint32_t col=(line%2==0)?0x9098A8:0x707888;
        for(int seg=0;seg<w;seg+=40){
            int sw2=(seg%80==0)?30:20;
            vesa_hline(x+seg,ly,sw2,col);
        }
    }
    // Bending tree
    int tx=x+w*75/100, tby=gy;
    vesa_fill_rect(tx,tby-40,4,40,0x5A3C1E);
    for(int i=0;i<3;i++)
        wx_circle(tx+6+i*8,tby-46-i*4,10,0x2C7A38);
    // Flying leaves
    int seed=777;
    for(int i=0;i<6;i++){
        seed=seed*1103515245+12345;
        int lx=x+((seed>>8)&0x3FF)%w, ly=y+h*20/100+((seed>>16)&0xFF)%(h*40/100);
        vesa_fill_rect(lx,ly,4,4,(i%2==0)?0xC89840:0x8A6428);
    }
}

static void wx_draw_scene(int x,int y,int w,int h,WeatherType type){
    // Sky gradient base
    uint32_t sky_top, sky_bot;
    switch(type){
        case WEATHER_SUNNY:  sky_top=0x4AA8E8; sky_bot=0xA8DCF8; break;
        case WEATHER_CLOUDY: sky_top=0x7890A8; sky_bot=0xB8C8D8; break;
        case WEATHER_RAINY:  sky_top=0x384858; sky_bot=0x586878; break;
        case WEATHER_STORMY: sky_top=0x141420; sky_bot=0x282838; break;
        case WEATHER_SNOWY:  sky_top=0x9098B0; sky_bot=0xC8D0E0; break;
        default:              sky_top=0x5888A0; sky_bot=0x98C0D0; break;
    }
    for(int dy=0;dy<h;dy++){
        int r=((sky_top>>16)&0xFF)+(int)(((sky_bot>>16)&0xFF)-((sky_top>>16)&0xFF))*dy/h;
        int g=((sky_top>>8)&0xFF)+(int)(((sky_bot>>8)&0xFF)-((sky_top>>8)&0xFF))*dy/h;
        int b=(sky_top&0xFF)+(int)((sky_bot&0xFF)-(sky_top&0xFF))*dy/h;
        vesa_hline(x,y+dy,w,((uint32_t)r<<16)|((uint32_t)g<<8)|(uint32_t)b);
    }
    switch(type){
        case WEATHER_SUNNY:  scene_sun(x,y,w,h);   break;
        case WEATHER_CLOUDY: scene_cloudy(x,y,w,h);break;
        case WEATHER_RAINY:  scene_rainy(x,y,w,h); break;
        case WEATHER_STORMY: scene_stormy(x,y,w,h);break;
        case WEATHER_SNOWY:  scene_snowy(x,y,w,h); break;
        case WEATHER_WINDY:  scene_windy(x,y,w,h); break;
    }
}

static void vesa_weather_draw(){
    int W=(int)vesa.width,H=(int)vesa.height;
    int wx=16,wy=40,ww=W-32,wh=H-50;
    int wtype=(int)current_weather.type;
    uint32_t wcol=WRGB[current_weather.color];

    vesa_fill_rect(wx,wy,ww,wh,WX_BG);

    // Title bar
    vesa_fill_rect(wx,wy,ww,36,WX_TITLE);
    vesa_hline(wx,wy+36,ww,WX_BORDER);
    vesa_draw_string(wx+16,wy+10,"Nyx Weather",WX_TEXT,WX_TITLE);
    wx_pill(wx+150,wy+7,current_weather.name,wcol,0x100E20);
    g_win_x = wx; g_win_y = wy; g_win_w = ww; g_win_h = wh;
    draw_close_btn(wx+ww-40, wy+10, WX_TITLE, "Weather");
    wx_str_r(wx+ww-96, wy+10, "W next   ESC back", WX_HINT, WX_TITLE);

    int cy=wy+36+12, ch=wh-36-12;

    // Left: scene panel (~55%)
    int scene_w = ww*55/100 - 20;
    int scene_h = ch*58/100;
    int sx=wx+16, sy=cy;
    wx_draw_scene(sx,sy,scene_w,scene_h,(WeatherType)wtype);
    vesa_draw_rect(sx,sy,scene_w,scene_h,WX_BORDER);
    // Condition label, bottom-left overlay on the scene
    {
        char lbl[40]; int lp=0;
        const char*n=WP[wtype].desc;
        for(int i=0;n[i];i++)lbl[lp++]=n[i];
        lbl[lp]=0;
        vesa_fill_rect(sx+8,sy+scene_h-30,wx_len(lbl)*8+16,22,0x000000);
        vesa_draw_string(sx+16,sy+scene_h-24,lbl,0xFFFFFF,0x000000);
    }

    // Right: conditions card
    int rx=sx+scene_w+16, ry=cy, rw=wx+ww-16-rx, rh=scene_h;
    vesa_fill_rect(rx,ry,rw,rh,WX_CARD);
    vesa_draw_rect(rx,ry,rw,rh,WX_BORDER);
    vesa_fill_rect(rx,ry,rw,3,wcol);

    int py=ry+14;
    vesa_draw_string(rx+16,py,"City",WX_DIM,WX_CARD);
    wx_str_r(rx+rw-16,py,timezones[current_timezone].city,WX_TEXT,WX_CARD);
    py+=26;
    vesa_hline(rx+12,py,rw-24,WX_DIVIDER); py+=14;

    // Big temp
    char tmp[8]; n2sw(current_weather.temp,tmp);
    int tp=0;while(tmp[tp])tp++;tmp[tp++]='C';tmp[tp]=0;
    for(int i=0;tmp[i];i++) vesa_draw_char_scaled(rx+16+i*24,py,tmp[i],wcol,WX_CARD,3);
    // degree mark
    vesa_fill_rect(rx+16+(tp-1)*24-6,py+2,4,4,wcol);
    py+=50;

    char fls[10]; n2sw(WP[wtype].feels,fls);
    int fp=0;while(fls[fp])fp++;fls[fp++]='C';fls[fp]=0;
    char feelstr[24]; int p2=0;
    const char*pre="Feels like ";
    for(int i=0;pre[i];i++)feelstr[p2++]=pre[i];
    for(int i=0;fls[i];i++)feelstr[p2++]=fls[i];
    feelstr[p2]=0;
    vesa_draw_string(rx+16,py,feelstr,WX_DIM,WX_CARD); py+=26;
    vesa_hline(rx+12,py,rw-24,WX_DIVIDER); py+=14;

    typedef struct{const char*k;int v;const char*unit;uint32_t c;}Row;
    Row rows[]={
        {"Humidity",current_weather.humidity,"%",0x5599FF},
        {"Wind",current_weather.wind,"km/h",0x60D8B8},
        {"UV Index",WP[wtype].uv,"",WP[wtype].uv>6?0xFF6655:WP[wtype].uv>3?0xFFAA00:WX_SUCCESS},
    };
    for(int i=0;i<3;i++){
        vesa_draw_string(rx+16,py,rows[i].k,WX_DIM,WX_CARD);
        char vs[12]; n2sw(rows[i].v,vs);
        int vp=0;while(vs[vp])vp++;
        for(int k=0;rows[i].unit[k];k++)vs[vp++]=rows[i].unit[k];
        vs[vp]=0;
        wx_str_r(rx+rw-16,py,vs,rows[i].c,WX_CARD);
        py+=22;
    }
    py+=6; vesa_hline(rx+12,py,rw-24,WX_DIVIDER); py+=14;

    // Sunrise/sunset — correctly labelled 12-hour times
    char sr2[12],ss2[12]; time12(6,15,sr2); time12(20,48,ss2);
    vesa_draw_string(rx+16,py,"Sunrise",WX_DIM,WX_CARD);
    wx_str_r(rx+rw-16,py,sr2,0xFFCC66,WX_CARD); py+=22;
    vesa_draw_string(rx+16,py,"Sunset",WX_DIM,WX_CARD);
    wx_str_r(rx+rw-16,py,ss2,0xFF9966,WX_CARD);

    // Stats bars row
    int by=sy+scene_h+16;
    int bar_w=(ww-32-2*16)/3;
    typedef struct{const char*label;int pct;const char*vs;uint32_t c;}Bar;
    char tv[10]; n2sw(current_weather.temp,tv); int tvp=0;while(tv[tvp])tvp++;tv[tvp++]='C';tv[tvp]=0;
    char wv[10]; n2sw(current_weather.wind,wv); int wvp=0;while(wv[wvp])wvp++;
    const char*kmh=" km/h"; for(int k=0;kmh[k];k++)wv[wvp++]=kmh[k]; wv[wvp]=0;
    char hv[10]; n2sw(current_weather.humidity,hv); int hvp=0;while(hv[hvp])hvp++;hv[hvp++]='%';hv[hvp]=0;
    int temp_pct=(current_weather.temp+20)*100/65; if(temp_pct<0)temp_pct=0; if(temp_pct>100)temp_pct=100;
    Bar bars[]={
        {"Temperature",temp_pct,tv,
         current_weather.temp<0?0x5599FF:current_weather.temp<15?WX_SUCCESS:current_weather.temp<28?0xFFCC55:0xFF6655},
        {"Wind Speed",current_weather.wind*100/80,wv,0x60D8B8},
        {"Humidity",current_weather.humidity,hv,0x5599FF},
    };
    for(int i=0;i<3;i++){
        int bx=sx+i*(bar_w+16);
        vesa_draw_string(bx,by,bars[i].label,WX_DIM,WX_BG);
        wx_str_r(bx+bar_w,by,bars[i].vs,bars[i].c,WX_BG);
        wx_bar(bx,by+18,bar_w,12,bars[i].pct,bars[i].c,0x100E20);
    }

    // Forecast strip
    int fy=by+48;
    vesa_draw_string(sx,fy,"6-Day Forecast",WX_DIM,WX_BG); fy+=22;
    int fcard_w=(ww-32-5*10)/6;
    for(int i=0;i<6;i++){
        int fi=(wtype+i)%WP_COUNT;
        int fx=sx+i*(fcard_w+10);
        int cur=(i==0);
        uint32_t cbg=cur?0x252068:WX_CARD;
        vesa_fill_rect(fx,fy,fcard_w,ch-(fy-cy),cbg);
        vesa_draw_rect(fx,fy,fcard_w,ch-(fy-cy),cur?WX_ACCENT:WX_BORDER);
        const char*dn[]={"Today","Tue","Wed","Thu","Fri","Sat"};
        vesa_draw_string(fx+8,fy+8,dn[i],cur?WX_TEXT:WX_DIM,cbg);
        // small icon glyph
        uint32_t icol=WRGB[WP[fi].color];
        vesa_draw_char_scaled(fx+fcard_w/2-8,fy+28,WP[fi].icon[0],icol,cbg,2);
        char ft[8]; n2sw(WP[fi].temp,ft); int ftp=0;while(ft[ftp])ftp++;ft[ftp++]='C';ft[ftp]=0;
        vesa_draw_string(fx+8,fy+56,ft,icol,cbg);
    }
}

/* Compact taskbar chip: icon glyph + temperature, drawn wherever the
   caller (desktop.c) places it. Previously weather_draw_widget() was
   fully implemented but never called from anywhere — meaning the
   taskbar weather feature was completely dead code despite existing.
   This VESA counterpart gets wired into draw_taskbar() there. */
void weather_draw_widget_vesa(int x,int y){
    if(!weather_show_widget) return;
    uint32_t col=WRGB[current_weather.color];
    vesa_draw_char_scaled(x,y-2,current_weather.icon[0],col,0,1);
    char tmp[8]; n2sw(current_weather.temp,tmp);
    int p=0;while(tmp[p])p++;tmp[p++]='C';tmp[p]=0;
    vesa_draw_string(x+12,y,tmp,col,0);
}

// ═══════════════════════════════════════════════════════════════════
//  PUBLIC API
// ═══════════════════════════════════════════════════════════════════

void weather_draw_full() {
    if(vesa.active){ vesa_weather_draw(); return; }
    vga_weather_draw();
}

void weather_handle_key(char c) {
    if(c=='w'||c=='W'){
        weather_next();
        
    }
}

void weather_tick() {
    weather_tick_count++;
    if(weather_tick_count>=weather_change_time){
        weather_tick_count=0;
        if((wrand()%4)==0) weather_next();
        current_weather.temp+=(wrand()%3)-1;
        if(current_weather.temp>45) current_weather.temp=45;
        if(current_weather.temp<-20) current_weather.temp=-20;
    }
}

void weather_init() {
    weather_tick_count=0;
    weather_show_widget=1;
    current_weather.type     = WP[0].type;
    current_weather.name     = WP[0].name;
    current_weather.temp     = WP[0].temp;
    current_weather.humidity = WP[0].humidity;
    current_weather.wind     = WP[0].wind;
    current_weather.icon     = WP[0].icon;
    current_weather.color    = WP[0].color;
}


int weather_handle_mouse(int clk_x, int clk_y) {
    if (!vesa.active) return 0;
    int W = (int)vesa.width, H = (int)vesa.height;
    int wx = 16, wy = 40, ww = W - 32, wh = H - 50;
    int cy = wy + 36 + 12;
    int ch = wh - 36 - 12;
    int scene_h = ch * 58 / 100;
    int sx = wx + 16, sy = cy;

    // ── Forecast day card clicks ────────────────────────────────
    int by = sy + scene_h + 16;
    int fy = by + 48;
    int fcard_w = (ww - 32 - 5 * 10) / 6;
    for (int i = 0; i < 6; i++) {
        int fx = sx + i * (fcard_w + 10);
        if (clk_x >= fx && clk_x < fx + fcard_w &&
            clk_y >= fy && clk_y < fy + 70) {
            weather_next();
            return 1;
        }
    }

    return 0;
}
