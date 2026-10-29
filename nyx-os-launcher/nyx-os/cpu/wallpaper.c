// cpu/wallpaper.c  –  VGA text + VESA pixel wallpapers
#include <wallpaper.h>
#include <vesa.h>
#include <stdint.h>

// ─── VGA helpers ──────────────────────────────────────────────────
#define VGA_WIDTH  80
#define VGA_HEIGHT 25
#define VGA_MEM    ((uint16_t*)0xB8000)
static inline uint16_t make_entry(char c,uint8_t fg,uint8_t bg){
    return (uint16_t)c|(uint16_t)(fg|bg<<4)<<8;
}
static void wput(int x,int y,char c,uint8_t fg,uint8_t bg){
    if(x<0||x>=VGA_WIDTH||y<0||y>=VGA_HEIGHT)return;
    VGA_MEM[y*VGA_WIDTH+x]=make_entry(c,fg,bg);
}
static void wputs(int x,int y,const char*s,uint8_t fg,uint8_t bg){
    for(int i=0;s[i];i++) wput(x+i,y,s[i],fg,bg);
}
static void wfill(int x,int y,int w,int h,char c,uint8_t fg,uint8_t bg){
    for(int dy=0;dy<h;dy++) for(int dx=0;dx<w;dx++) wput(x+dx,y+dy,c,fg,bg);
}

// ─── VESA drawing helpers ─────────────────────────────────────────
static void vgrad(int x,int y,int w,int h,uint32_t c1,uint32_t c2){
    for(int dy=0;dy<h;dy++){
        uint32_t r=((c1>>16)&0xFF)+(int)(((c2>>16)&0xFF)-((c1>>16)&0xFF))*dy/h;
        uint32_t g=((c1>>8)&0xFF)+(int)(((c2>>8)&0xFF)-((c1>>8)&0xFF))*dy/h;
        uint32_t b=(c1&0xFF)+(int)((c2&0xFF)-(c1&0xFF))*dy/h;
        vesa_hline(x,y+dy,w,(r<<16)|(g<<8)|b);
    }
}
static void vcircle(int cx,int cy,int r,uint32_t c){
    for(int dy=-r;dy<=r;dy++)
        for(int dx=-r;dx<=r;dx++)
            if(dx*dx+dy*dy<=r*r) vesa_put_pixel(cx+dx,cy+dy,c);
}
static void vstar(int x,int y,uint32_t c){
    vesa_put_pixel(x,y,c);
    vesa_put_pixel(x-1,y,c>>1&0x7F7F7F); vesa_put_pixel(x+1,y,c>>1&0x7F7F7F);
    vesa_put_pixel(x,y-1,c>>1&0x7F7F7F); vesa_put_pixel(x,y+1,c>>1&0x7F7F7F);
}

// ═══════════════════════════════════════════════════════════════════
//  VGA WALLPAPERS  (text mode, 80×25)
// ═══════════════════════════════════════════════════════════════════

static void draw_space(){
    wfill(0,1,VGA_WIDTH,VGA_HEIGHT-2,' ',0,0);
    int stars[][3]={{2,1,8},{7,2,7},{15,1,15},{23,3,8},{31,1,7},{42,2,15},
        {50,1,8},{61,3,7},{70,1,15},{78,2,8},{5,4,7},{14,5,8},
        {25,4,15},{35,6,7},{45,4,8},{55,5,15},{65,4,7},{75,6,8},
        {3,7,15},{12,8,7},{22,7,8},{33,9,15},{43,7,7},{53,8,8},
        {63,7,15},{73,9,7},{8,10,8},{18,11,15},{28,10,7},{38,12,8},
        {48,10,15},{58,11,7},{68,10,8},{77,12,15},{1,13,7},{11,14,8},
        {21,13,15},{32,15,7},{41,13,8},{51,14,15},{62,15,7},{72,13,8},
        {4,16,15},{13,17,7},{24,16,8},{34,18,15},{44,16,7},{54,17,8},
        {64,16,15},{74,18,7},{9,19,8},{19,20,15},{29,19,7},{39,21,8},
        {49,19,15},{59,20,7},{69,19,8},{6,22,15},{16,23,7},{26,22,8}};
    for(int i=0;i<60;i++){
        char sc=(stars[i][2]==15)?'*':(stars[i][2]==8)?'.':'+';
        wput(stars[i][0],stars[i][1],sc,stars[i][2],0);
    }
    wputs(68,3,"  .---.",14,0); wputs(68,4," /     \\",14,0);
    wputs(68,5,"|   ()  |",14,0); wputs(68,6," \\     /",14,0); wputs(68,7,"  `---'",14,0);
    wputs(8,9,"   _____",11,0); wputs(8,10,"  /     \\",11,0);
    wputs(8,11," | ~~~~~ |",11,0); wputs(8,12," | ~~~~~ |",9,0);
    wputs(8,13,"  \\_____/",11,0);
    wputs(5,12,"~~~",8,0); wputs(18,12,"~~~",8,0);
    wputs(30,5,"*---->",15,0);
    wputs(40,10,".:*:.",13,0); wputs(38,11,".*::*:.",5,0); wputs(40,12,"':*:'",13,0);
    wputs(55,8," _(_)_",10,0); wputs(55,9,"(#####)",10,0); wputs(55,10," `---'",10,0);
}

static void draw_nature(){
    wfill(0,1,VGA_WIDTH,8,' ',7,1); wfill(0,9,VGA_WIDTH,2,' ',7,2);
    wputs(65,2,"  \\|/  ",14,1); wputs(65,3,"- (o) -",14,1); wputs(65,4,"  /|\\  ",14,1);
    wputs(5,2," ___  ",15,1); wputs(4,3,"(   ) ",15,1); wputs(3,4,"(___)  ",15,1);
    wputs(25,3,"  ____  ",15,1); wputs(24,4,"(      )",15,1); wputs(25,5,"(____) ",15,1);
    wputs(45,2," ___",15,1); wputs(44,3,"(   )",15,1); wputs(45,4,"(___)  ",15,1);
    for(int x=0;x<VGA_WIDTH;x++) wput(x,9,' ',2,2);
    wputs(10,7,"   /\\   ",8,1); wputs(10,8,"  /  \\  ",8,1);
    wputs(15,6,"  /\\  ",7,1);  wputs(15,7," /  \\ ",7,1);
    wputs(30,7,"  /\\  ",8,1);  wputs(30,8," /  \\ ",8,1);
    wputs(35,6,"   /\\   ",7,1); wputs(35,7,"  /  \\  ",7,1);
    wputs(50,7,"  /\\  ",8,1);  wputs(50,8," /  \\ ",8,1);
    wfill(0,10,VGA_WIDTH,13,' ',2,2);
    const int tree_x[]={3,12,22,32,42,52,62,72};
    for(int i=0;i<8;i++){
        int tx=tree_x[i];
        wputs(tx,10,"  ^  ",10,2); wputs(tx,11," /|\\ ",10,2);
        wputs(tx,12,"/|||\\",2,2); wputs(tx+1,13,"|||",10,2);
        wputs(tx+1,14,"|||",6,2); wputs(tx+2,15,"|",6,2);
    }
    for(int x=0;x<VGA_WIDTH;x++){
        char rc=(x%3==0)?'~':'-'; wput(x,18,rc,11,2);
    }
    for(int x=0;x<VGA_WIDTH;x++){
        char gc=(x%2==0)?',':'\'';
        wput(x,19,gc,(x%4<2)?10:2,2); wput(x,20,gc,(x%4<2)?2:10,2);
    }
    wputs(20,3,"v v",0,1); wputs(35,4,"v v",0,1); wputs(50,3,"v",0,1);
}

static void draw_geometric(){
    wfill(0,1,VGA_WIDTH,VGA_HEIGHT-2,' ',0,0);
    for(int y=1;y<VGA_HEIGHT-1;y++) for(int x=0;x<VGA_WIDTH;x++){
        int px=x%8,py=y%4; char c=' '; uint8_t fg=8;
        if(px==0||px==4){c='|';fg=9;}
        else if(py==0){c='-';fg=5;}
        else if(px==2&&py==2){c='+';fg=13;}
        wput(x,y,c,fg,0);
    }
    wputs(32,5,"    *    ",14,0); wputs(30,6,"  *   *  ",14,0);
    wputs(28,7,"*       *",14,0); wputs(30,8,"  *   *  ",14,0); wputs(32,9,"    *    ",14,0);
    wputs(0,1,"+-+",11,0); wputs(0,2,"| |",11,0); wputs(0,3,"+-+",11,0);
    wputs(77,1,"+-+",11,0); wputs(77,2,"| |",11,0); wputs(77,3,"+-+",11,0);
    wputs(0,21,"+-+",11,0); wputs(0,22,"| |",11,0); wputs(0,23,"+-+",11,0);
    wputs(77,21,"+-+",11,0); wputs(77,22,"| |",11,0); wputs(77,23,"+-+",11,0);
    wputs(55,10,"  / \\  ",12,0); wputs(55,11," |   | ",12,0);
    wputs(55,12," |   | ",12,0); wputs(55,13,"  \\ /  ",12,0);
    wputs(15,14,"    *    ",10,0); wputs(15,15,"   * *   ",10,0);
    wputs(15,16,"  *   *  ",10,0); wputs(15,17," *     * ",10,0); wputs(15,18,"*********",10,0);
}

static void draw_ocean(){
    wfill(0,1,VGA_WIDTH,10,' ',7,1);
    wputs(38,2,"  \\|/  ",14,1); wputs(38,3,"- oOo -",14,1); wputs(38,4,"  /|\\  ",14,1);
    wputs(5,3,"~~~",15,1); wputs(20,4,"~~~~~",15,1); wputs(55,3,"~~~~",15,1); wputs(65,4,"~~~",15,1);
    for(int x=0;x<VGA_WIDTH;x++) wput(x,11,'=',11,1);
    const char*wave_chars[]={"~^~^~^~^~^~^~^~^~^~^~^~^~^~^~^~^~^~^~^~^","^~^~^~^~^~^~^~^~^~^~^~^~^~^~^~^~^~^~^~^~"};
    for(int y=12;y<VGA_HEIGHT-1;y++){
        uint8_t col=(y<16)?11:(y<19)?9:1, bg=(y<16)?1:(y<19)?1:0;
        for(int x=0;x<VGA_WIDTH;x++) wput(x,y,wave_chars[y%2][x%40],col,bg);
    }
    wputs(30,9,"    |    ",6,1); wputs(28,10,"   /_\\   ",6,1);
    wputs(26,11,"  /___\\  ",6,1); wputs(25,12," /|   |\\",6,1);
    wputs(10,15,"><>",14,1); wputs(50,17,"<><",12,1); wputs(20,19,"><>",14,1); wputs(60,16,"<><",10,1);
    wputs(15,5,"v v",0,1); wputs(55,6,"v",0,1); wputs(70,5,"v v",0,1);
    wputs(62,10,"/\\",2,1); wputs(60,11,"(~~)",2,1);
    wputs(63,8," )",10,1); wputs(62,9,"\\|",10,1); wputs(63,10,"|",6,1);
}

static void draw_city(){
    wfill(0,1,VGA_WIDTH,VGA_HEIGHT-2,' ',0,0);
    int sx[]={2,8,15,22,30,38,45,52,60,67,74,5,18,35,50,65,78};
    int sy[]={1,2,1,3,2,1,2,1,3,2,1,3,2,3,2,1,2};
    for(int i=0;i<17;i++) wput(sx[i],sy[i],'.',7+(i%2),0);
    wputs(70,2,"( )",14,0);
    wfill(0,8,8,16,' ',8,8); wputs(0,8,"  __  ",8,8); wputs(0,9," |  | ",8,8);
    for(int y=10;y<22;y+=2){ wput(1,y,'[',14,8); wput(2,y,']',14,8); wput(4,y,'[',14,8); wput(5,y,']',14,8); }
    wfill(8,5,10,19,' ',8,4); wputs(8,4,"  /\\  ",8,4); wputs(8,5," |  | ",8,4);
    for(int y=6;y<22;y+=2){ wput(9,y,'[',14,4); wput(10,y,']',14,4); wput(13,y,'[',14,4); wput(14,y,']',14,4); }
    wfill(18,10,8,14,' ',8,1); wputs(18,9,"[__]",8,1);
    for(int y=11;y<22;y+=2){ wput(19,y,'[',11,1); wput(20,y,']',11,1); wput(22,y,'[',11,1); wput(23,y,']',11,1); }
    wfill(26,3,8,21,' ',8,8); wputs(26,2,"  **  ",14,0); wputs(26,3," |  | ",8,8);
    for(int y=4;y<22;y+=2){ wput(27,y,'[',14,8); wput(28,y,']',14,8); wput(30,y,'[',14,8); wput(31,y,']',14,8); }
    wfill(34,7,8,17,' ',8,2);
    for(int y=8;y<22;y+=2){ wput(35,y,'[',10,2); wput(36,y,']',10,2); wput(38,y,'[',10,2); wput(39,y,']',10,2); }
    wfill(42,6,10,18,' ',8,5); wputs(42,5,"  /\\  ",13,0);
    for(int y=7;y<22;y+=2){ wput(43,y,'[',13,5); wput(44,y,']',13,5); wput(47,y,'[',13,5); wput(48,y,']',13,5); }
    wfill(52,9,8,15,' ',8,4);
    for(int y=10;y<22;y+=2){ wput(53,y,'[',14,4); wput(54,y,']',14,4); wput(56,y,'[',14,4); wput(57,y,']',14,4); }
    wfill(60,7,10,17,' ',8,1); wputs(60,6,"  /~~\\  ",15,0); wputs(60,7," /    \\ ",15,0);
    for(int y=8;y<22;y+=2){ wput(61,y,'[',15,1); wput(62,y,']',15,1); wput(65,y,'[',15,1); wput(66,y,']',15,1); }
    wfill(70,11,10,13,' ',8,8);
    for(int y=12;y<22;y+=2){ wput(71,y,'[',14,8); wput(72,y,']',14,8); wput(75,y,'[',14,8); wput(76,y,']',14,8); }
    for(int x=0;x<VGA_WIDTH;x++) wput(x,22,'=',8,0);
    for(int x=0;x<VGA_WIDTH;x+=4) wput(x,22,'-',7,0);
}

static void draw_matrix(){
    wfill(0,1,VGA_WIDTH,VGA_HEIGHT-2,' ',0,0);
    int seed=42317;
    for(int y=1;y<VGA_HEIGHT-1;y++) for(int x=0;x<VGA_WIDTH;x++){
        seed=seed*1103515245+12345; int r=(seed>>16)&0x7FFF;
        if(r%4==0){ char c='0'+(r%10); uint8_t fg=(r%8==0)?15:((r%4==0)?10:2); wput(x,y,c,fg,0); }
    }
    int cols[]={5,15,25,35,45,55,65,75};
    for(int i=0;i<8;i++) for(int y=1;y<VGA_HEIGHT-1;y++) wput(cols[i],y,'1'+(y+i)%9,(y%4==0)?15:10,0);
    wfill(25,10,30,5,' ',10,0);
    wputs(27,11,"  _   _  __   __ __  ",10,0);
    wputs(27,12,"  |\\  | \\ \\  / / \\ \\ ",10,0);
    wputs(27,13,"  | \\ |  \\ \\/ /   > >",15,0);
    wputs(27,14,"  |  \\|   \\__/   /_/ ",10,0);
}

static void draw_synthwave_vga(){
    wfill(0,1,VGA_WIDTH,VGA_HEIGHT-2,' ',0,5);
    // Horizon line
    for(int x=0;x<VGA_WIDTH;x++) wput(x,12,'=',13,5);
    // Grid lines receding to horizon
    const int grid_x[]={10,20,30,40,50,60,70};
    for(int i=0;i<7;i++) for(int y=12;y<VGA_HEIGHT-1;y++) wput(grid_x[i],y,'|',13,5);
    // Sun
    wputs(36,5,"  ___  ",14,5); wputs(36,6," /   \\ ",14,5);
    wputs(36,7,"|     |",14,5); wputs(36,8,"|     |",12,5);
    wputs(36,9," \\___/ ",12,5);
    // Stars
    int stx[]={3,10,17,25,55,63,71,78,6,60};
    int sty[]={2,4,2,3,2,3,2,4,5,5};
    for(int i=0;i<10;i++) wput(stx[i],sty[i],'*',15,5);
    // Scan lines
    for(int y=13;y<VGA_HEIGHT-1;y+=2) for(int x=0;x<VGA_WIDTH;x++) wput(x,y,'-',5,5);
}

static void draw_aurora_vga(){
    wfill(0,1,VGA_WIDTH,VGA_HEIGHT-2,' ',0,0);
    // Stars
    int sx[]={3,9,16,24,32,41,49,57,65,73,79,6,19,36,52,67};
    int sy[]={1,2,1,2,1,2,1,2,1,2,1,3,3,3,3,3};
    for(int i=0;i<16;i++) wput(sx[i],sy[i],'.',15-(i%2),0);
    // Aurora bands
    for(int x=0;x<VGA_WIDTH;x++){
        wput(x,5,'~',10,0); wput(x,6,'~',2,0);
        wput(x,7,'~',11,0); wput(x,8,'~',10,0); wput(x,9,'~',2,0);
    }
    // Brighter patches
    wputs(10,6,"~~~~~~~~~~",10,0); wputs(30,7,"~~~~~~~~~~",11,0);
    wputs(50,5,"~~~~~~~~~~",10,0); wputs(60,8,"~~~~~~~~~~",11,0);
    // Snowy ground
    wfill(0,18,VGA_WIDTH,5,' ',15,7);
    for(int x=0;x<VGA_WIDTH;x++) wput(x,18,(x%3==0)?'_':' ',15,7);
    // Trees
    const int tx[]={5,18,30,45,58,70};
    for(int i=0;i<6;i++){
        wputs(tx[i],13,"  /\\  ",2,0); wputs(tx[i],14," /  \\ ",2,0);
        wputs(tx[i],15,"/    \\",2,0); wputs(tx[i]+2,16,"||",6,0);
        wputs(tx[i]+2,17,"||",6,0);
    }
}

// ═══════════════════════════════════════════════════════════════════
//  VESA WALLPAPERS  (pixel-perfect, full color)
// ═══════════════════════════════════════════════════════════════════

// ── 0: Space / Purple Cosmic ──────────────────────────────────────
static void draw_vesa_space(void){
    int W=(int)vesa.width, H=(int)vesa.height;
    vgrad(0,0,W,H,0x2A1855,0x0C0820);
    // Moon
    int mx=W*72/100, my=H*16/100, mr=H*5/100; if(mr<18)mr=18;
    for(int rr=mr+22;rr>mr;rr-=4){ uint32_t g=0x1A0F38+(uint32_t)(mr+22-rr)*0x040208; vcircle(mx,my,rr,g); }
    for(int dy=-mr;dy<=mr;dy++) for(int dx=-mr;dx<=mr;dx++){
        int d2=dx*dx+dy*dy; if(d2>mr*mr)continue;
        int pct=d2*100/(mr*mr);
        uint32_t ic=0xE8D5FF,oc=0xA888D8;
        uint32_t r=((ic>>16)&0xFF)+(int)(((oc>>16)&0xFF)-((ic>>16)&0xFF))*pct/100;
        uint32_t g2=((ic>>8)&0xFF)+(int)(((oc>>8)&0xFF)-((ic>>8)&0xFF))*pct/100;
        uint32_t b=(ic&0xFF)+(int)((oc&0xFF)-(ic&0xFF))*pct/100;
        vesa_put_pixel(mx+dx,my+dy,(r<<16)|(g2<<8)|b);
    }
    // Stars
    static const uint16_t star_x[]={30,80,140,200,260,320,380,440,500,560,620,680,740,800,860,920,15,95,175,255,335,415,495,575,655,735,815,895,60,160,260,360,460,560,660,760,860,960};
    static const uint16_t star_y[]={20,55,15,70,35,60,25,45,18,65,30,50,22,58,38,48,90,110,80,120,95,105,75,115,85,100,70,108,140,160,135,155,145,150,138,158,142,148};
    int ns=(int)(sizeof(star_x)/sizeof(star_x[0]));
    for(int i=0;i<ns;i++){
        int x=star_x[i],y=star_y[i]; if(x>=W||y>=H*55/100)continue;
        uint32_t c=(i%5==0)?0xE0D0FF:(i%3==0)?0x9988CC:0x5A4A88;
        vstar(x,y,c);
    }
    // Mountain silhouette
    int base=H*62/100;
    for(int x=0;x<W;x++){
        int seg=x%240, half=120, peak=(seg<half)?seg:240-seg;
        int ridge1=base-40-peak/3;
        int seg2=(x+90)%320, half2=160, peak2=(seg2<half2)?seg2:320-seg2;
        int ridge2=base+10-peak2/4;
        vesa_vline(x,ridge2,H-ridge2,0x140A2E);
        vesa_vline(x,ridge1,H-ridge1,0x0E0822);
    }
    vgrad(0,H-H*8/100,W,H*8/100,0x0E0822,0x040312);
}

// ── 1: Forest ─────────────────────────────────────────────────────
static void draw_vesa_forest(void){
    int W=(int)vesa.width, H=(int)vesa.height;
    int horizon=H*55/100;
    // Sky: cerulean → pale at horizon
    vgrad(0,0,W,horizon,0x3A9AE0,0xC0DEFF);
    // Sun disc + halo
    int sx=W*76/100, sy=H*13/100, sr=H*5/100; if(sr<14)sr=14;
    for(int rr=sr+28;rr>sr;rr-=5) vcircle(sx,sy,rr,0xFFF8E8-(uint32_t)(rr-sr)*0x030300);
    vcircle(sx,sy,sr,0xFFFAC0);
    // Clouds
    for(int i=0;i<3;i++){
        int cx=W*(12+i*28)/100, cy=H*(9+(i%2)*5)/100;
        int cw=W*9/100, ch=H*4/100;
        vesa_fill_rect(cx,cy,cw,ch,0xEEF8FF);
        vesa_fill_rect(cx-cw/5,cy+ch/3,cw*7/5,ch*2/3,0xDDEEFF);
    }
    // Distant misty mountains
    for(int x=0;x<W;x++){
        int seg=x%(W/4), half=W/8;
        int pk=(seg<half)?seg:W/4-seg;
        int ridge=horizon-30-pk*H/12/half;
        vesa_vline(x,ridge,horizon-ridge,0x78A090);
    }
    // Ground
    vgrad(0,horizon,W,H-horizon,0x45B030,0x1A5010);
    // Tree silhouettes
    for(int t=0;t<10;t++){
        int tx=W*(2+t*10)/100;
        int theight=H*35/100, twmax=W*4/100;
        int tbase=H*98/100;
        for(int y=0;y<theight;y++){
            int tw=twmax*y/theight;
            vesa_hline(tx+twmax-tw, tbase-theight+y, tw*2, (y<theight/3)?0x0A2008:0x0D2A0A);
        }
        vesa_fill_rect(tx+twmax-5,tbase-16,10,16,0x2A1606);
    }
    // Birds (small v shapes via pixels)
    for(int b=0;b<6;b++){
        int bx=W*(10+b*14)/100, by=H*(12+(b%3)*4)/100;
        vesa_put_pixel(bx,by,0x1A3060); vesa_put_pixel(bx-3,by-2,0x1A3060);
        vesa_put_pixel(bx+3,by-2,0x1A3060);
    }
}

// ── 2: Geometric ─────────────────────────────────────────────────
static void draw_vesa_geometric(void){
    int W=(int)vesa.width, H=(int)vesa.height;
    vesa_clear(0x0A0818);
    // Subtle radial glow from center
    int cx=W/2, cy=H/2;
    for(int r=H*60/100;r>0;r-=12){
        uint32_t glow=0x0A0818+(uint32_t)((H*60/100-r)/16)*0x010008;
        if(glow>0x181A30)glow=0x181A30;
        vesa_draw_rect(cx-r,cy-r,r*2,r*2,glow);
    }
    // Grid lines
    int grid=H/14;
    for(int y=0;y<H;y+=grid) vesa_hline(0,y,W,0x181430);
    for(int x=0;x<W;x+=grid) vesa_vline(x,0,H,0x181430);
    // Grid intersection dots
    for(int y=grid;y<H;y+=grid) for(int x=grid;x<W;x+=grid) vesa_put_pixel(x,y,0x302860);
    // Center diamond
    int dw=W*14/100, dh=H*22/100;
    for(int y=-dh;y<=dh;y++){
        int hw=dw*(dh-((y<0)?-y:y))/dh;
        vesa_hline(cx-hw,cy+y,hw*2,0x201848);
    }
    // Diamond outline
    for(int y=-dh;y<=dh;y++){
        int hw=dw*(dh-((y<0)?-y:y))/dh;
        vesa_put_pixel(cx-hw,cy+y,0x8060CC);
        vesa_put_pixel(cx+hw,cy+y,0x8060CC);
    }
    // Accent circles at corners
    int cr=W*5/100;
    vcircle(W/4,     H/4,     cr,0x201848); vcircle(W/4,     H/4,     cr,0x201848);
    vcircle(W*3/4,   H/4,     cr,0x201848); vcircle(W/4,     H*3/4,   cr,0x201848);
    vcircle(W*3/4,   H*3/4,   cr,0x201848);
    for(int rr=cr;rr>0;rr--) vesa_draw_rect(W/4-rr,H/4-rr,rr*2,rr*2,0x6050A0-(uint32_t)(cr-rr)*0x040020);
    // Corner brackets
    int bs=W*6/100;
    vesa_hline(16,16,bs,0x5040A0); vesa_vline(16,16,bs,0x5040A0);
    vesa_hline(W-bs-16,16,bs,0x5040A0); vesa_vline(W-17,16,bs,0x5040A0);
    vesa_hline(16,H-17,bs,0x5040A0); vesa_vline(16,H-bs-16,bs,0x5040A0);
    vesa_hline(W-bs-16,H-17,bs,0x5040A0); vesa_vline(W-17,H-bs-16,bs,0x5040A0);
    // Scattered accent dots
    static const int gx[]={200,400,600,800,150,350,550,750,250,500,700};
    static const int gy[]={100,150,120,130,250,230,260,240,350,380,320};
    for(int i=0;i<11;i++) if(gx[i]<W&&gy[i]<H) vcircle(gx[i],gy[i],3,0x6050A0);
}

// ── 3: Ocean ─────────────────────────────────────────────────────
static void draw_vesa_ocean(void){
    int W=(int)vesa.width, H=(int)vesa.height;
    int horizon=H*42/100;
    // Sky: pale dawn blue
    vgrad(0,0,W,horizon,0x90C8F0,0xD8EEFF);
    // Sun reflection column at center
    for(int y=horizon-40;y<horizon;y++){
        int glow_w=(horizon-y)*W/10/40;
        vesa_fill_rect(W/2-glow_w,y,glow_w*2,1,0xFFFBD0-(uint32_t)((horizon-y)*0x010100));
    }
    // Sun
    vcircle(W/2,horizon-H*12/100,H*5/100,0xFFF5A0);
    // Horizon shimmer
    vesa_hline(0,horizon,W,0xA0D0E8);
    // Deep ocean gradient
    vgrad(0,horizon,W,H-horizon,0x1A6080,0x061828);
    // Wave layers – lighter bands across the water
    static const int wave_offsets[]={8,22,38,56,76,100,128,160,196,236};
    for(int i=0;i<10;i++){
        int wy=horizon+wave_offsets[i];
        if(wy>=H) break;
        uint32_t wc=0x2888A8-(uint32_t)i*0x020408;
        for(int x=0;x<W;x+=2){
            int phase=(x*3/W+i)%4;
            vesa_put_pixel(x+(phase==0?1:0),wy,wc);
        }
    }
    // Reflection sparkles
    for(int i=0;i<24;i++){
        int rx=W*(5+i*4)/100, ry=horizon+H*(4+i%6)/100;
        if(rx<W&&ry<H) vesa_put_pixel(rx,ry,0xC8E8F8);
    }
    // Distant island/cliff silhouette
    for(int x=W*58/100;x<W*72/100;x++){
        int h2=(x<W*64/100)?(x-W*58/100)*H/12/(W*6/100):
                             (W*72/100-x)*H/12/(W*8/100);
        vesa_vline(x,horizon-h2,h2,0x1A3040);
    }
}

// ── 4: City ──────────────────────────────────────────────────────
static void draw_vesa_city(void){
    int W=(int)vesa.width, H=(int)vesa.height;
    // Night sky gradient
    vgrad(0,0,W,H,0x10183A,0x040410);
    // Moon
    int mx=W*15/100, my=H*12/100, mr=H*4/100; if(mr<14)mr=14;
    for(int rr=mr+20;rr>mr;rr-=4) vcircle(mx,my,rr,0x181838+(uint32_t)(mr+20-rr)*0x030302);
    vcircle(mx,my,mr,0xD8D0F0);
    // Stars
    static const uint16_t csx[]={50,130,210,290,380,460,540,620,700,780,860,940,80,200,340,480,600,720,840,960};
    static const uint16_t csy[]={20,45,15,55,30,50,20,40,25,55,35,45,80,90,70,85,75,95,65,80};
    for(int i=0;i<20;i++){ if(csx[i]<(uint16_t)W&&csy[i]<(uint16_t)(H*20/100)) vstar(csx[i],csy[i],0x8878A8); }
    // Building silhouettes + windows
    typedef struct { int x,w,h; uint32_t col; } Bld;
    Bld blds[]={
        {0,   W*8/100,  H*55/100, 0x0E0C20},
        {W*7/100, W*6/100, H*70/100, 0x120E28},
        {W*13/100,W*9/100, H*45/100, 0x0A0818},
        {W*21/100,W*7/100, H*75/100, 0x100C24},
        {W*28/100,W*10/100,H*58/100, 0x0E0C20},
        {W*38/100,W*6/100, H*65/100, 0x12102A},
        {W*43/100,W*8/100, H*50/100, 0x0A0818},
        {W*51/100,W*11/100,H*72/100, 0x100E26},
        {W*62/100,W*7/100, H*48/100, 0x0E0C20},
        {W*69/100,W*9/100, H*62/100, 0x12102A},
        {W*78/100,W*6/100, H*42/100, 0x0A0818},
        {W*84/100,W*8/100, H*55/100, 0x100C24},
        {W*92/100,W*8/100, H*38/100, 0x0E0C20},
    };
    int nb=(int)(sizeof(blds)/sizeof(blds[0]));
    for(int i=0;i<nb;i++){
        int bx=blds[i].x, bw=blds[i].w, bh2=blds[i].h;
        int by=H-bh2;
        vesa_fill_rect(bx,by,bw,bh2,blds[i].col);
        // Antenna
        vesa_vline(bx+bw/2,by-H/20,H/20,blds[i].col+0x040210);
        // Windows
        for(int wy=by+8;wy<H-8;wy+=12){
            for(int wx=bx+4;wx<bx+bw-8;wx+=10){
                // 70% chance lit window
                if((wx+wy+i)%10<7) vesa_fill_rect(wx,wy,6,8,0xFFE890+(uint32_t)((wx+wy)%3)*0x050500);
            }
        }
    }
    // Ground / road
    vesa_fill_rect(0,H-H*5/100,W,H*5/100,0x080810);
    for(int x=0;x<W;x+=40) vesa_hline(x,H-H*3/100,20,0x302830);
}

// ── 5: Matrix ─────────────────────────────────────────────────────
static void draw_vesa_matrix(void){
    int W=(int)vesa.width, H=(int)vesa.height;
    vesa_clear(0x010801);
    int seed=73142;
    int col_w=16;
    for(int col=0;col<W/col_w;col++){
        int cx2=col*col_w+4;
        seed=seed*6364136223846793005+1442695040888963407;
        int drop_len=H/4+(seed&0xFF)*H/8/256;
        int drop_y=(seed>>8)&0xFF; drop_y=drop_y*H/256;
        for(int y=0;y<H;y+=16){
            seed=seed*1103515245+12345;
            int r=(seed>>8)&0xFFFF;
            int yoff=y-drop_y;
            int in_drop=(yoff>=0&&yoff<drop_len);
            uint32_t c;
            if(in_drop){
                int fade=255-(yoff*200/drop_len);
                if(yoff<16) c=0xCCFFCC; // head: bright
                else c=(uint32_t)(fade/4)<<16|(uint32_t)fade<<8|(uint32_t)(fade/8);
            } else {
                if(r%6==0) c=0x002800+(uint32_t)(r%3)*0x001000;
                else continue;
            }
            char ch='0'+(r%10);
            vesa_draw_char(cx2,y,ch,c,0x010801);
        }
    }
    // Scan line overlay
    for(int y=0;y<H;y+=4) vesa_hline(0,y,W,0x010901);
}

// ── 6: Synthwave ─────────────────────────────────────────────────
static void draw_vesa_synthwave(void){
    int W=(int)vesa.width, H=(int)vesa.height;
    int horizon=H*50/100;
    // Sky: deep purple → dark navy
    vgrad(0,0,W,horizon,0x1A0838,0x08041C);
    // Horizon glow: pink/orange band
    vgrad(0,horizon-H/16,W,H/8,0x080420,0xFF4090);
    vgrad(0,horizon,W,H/8,0xFF4090,0x080420);
    // Stars
    static const uint16_t wsx[]={40,100,180,250,320,410,490,560,640,720,800,880,960,60,200,380,540,700,860};
    static const uint16_t wsy[]={15,35,10,45,25,15,40,20,35,15,30,45,20,60,55,50,65,58,62};
    for(int i=0;i<19;i++){
        if(wsx[i]<(uint16_t)W&&wsy[i]<(uint16_t)horizon)
            vstar(wsx[i],wsy[i],(i%3==0)?0xFF88CC:(i%3==1)?0x8888FF:0xFFCCFF);
    }
    // Sun: striped pink circle (classic synthwave)
    int sunx=W/2, suny=horizon-H/8, sunr=H*8/100; if(sunr<20)sunr=20;
    for(int dy=-sunr;dy<=sunr;dy++){
        int dx_max=0; int d2=sunr*sunr-dy*dy;
        for(int dx=0;dx<=sunr;dx++) if(dx*dx<=d2) dx_max=dx;
        if(dx_max<=0) continue;
        uint32_t col;
        int stripe=(dy+sunr)%(sunr/4);
        if(stripe<sunr/8) col=0x080420; // dark stripe
        else if(dy<0)     col=0xFF60A0;
        else              col=0xFF3060+(uint32_t)(dy*50/sunr)*0x010000;
        vesa_hline(sunx-dx_max,suny+dy,dx_max*2,col);
    }
    // Floor: dark purple
    vesa_fill_rect(0,horizon,W,H-horizon,0x080420);
    // Perspective grid
    for(int line=0;line<10;line++){
        int gy=horizon+H*(line+1)*50/100/10;
        if(gy>=H)break;
        vesa_hline(0,gy,W,0x4010A0);
    }
    // Converging vertical grid lines
    for(int v=-8;v<=8;v++){
        if(v==0)continue;
        int x_bottom=W/2+v*W/16;
        for(int y=horizon;y<H;y++){
            int frac=(y-horizon)*256/(H-horizon);
            int x=W/2+(x_bottom-W/2)*frac/256;
            if(x>=0&&x<W) vesa_put_pixel(x,y,0x4010A0);
        }
    }
    // Reflection of sun on floor (thin gradient strip)
    for(int y=horizon;y<horizon+H/8;y++){
        int alpha=(H/8-(y-horizon))*80/(H/8);
        vesa_hline(W/2-20,y,40,(uint32_t)alpha<<16|(uint32_t)(alpha/2)<<8|(uint32_t)alpha);
    }
}

// ── 7: Aurora / Winter ───────────────────────────────────────────
static void draw_vesa_aurora(void){
    int W=(int)vesa.width, H=(int)vesa.height;
    // Dark winter sky
    vgrad(0,0,W,H*65/100,0x050C18,0x020810);
    // Stars
    static const uint16_t asx[]={30,90,160,230,310,390,460,540,610,690,760,840,910,50,200,360,520,680,840,70,300,510,730};
    static const uint16_t asy[]={15,30,10,45,20,35,15,40,25,15,35,20,30,55,50,60,55,48,58,80,75,82,78};
    for(int i=0;i<23;i++){
        if(asx[i]<(uint16_t)W&&asy[i]<(uint16_t)(H*35/100)){
            uint32_t c=(i%4==0)?0xE0E8FF:(i%4==1)?0xA0B0D0:0x6070A0;
            vstar(asx[i],asy[i],c);
        }
    }
    // Aurora curtains: undulating horizontal bands of light
    // Green aurora layer
    for(int x=0;x<W;x++){
        int seg=x%300, half=150;
        int wave=(seg<half)?seg:300-seg;
        int ay=H*25/100 - wave*H/8/150;
        int height=H*12/100;
        for(int y=0;y<height;y++){
            int alpha=y<height/2 ? y*255/(height/2) : (height-y)*255/(height/2);
            uint32_t r=(uint32_t)(20+x*5/W);
            uint32_t g=(uint32_t)(alpha*200/255);
            uint32_t b=(uint32_t)(alpha*120/255);
            uint32_t cur_c=(r<<16)|(g<<8)|b;
            int px=x; int py=ay+y; if(py>=0&&py<H&&px>=0&&px<W) vesa_put_pixel(px,py,cur_c);
        }
    }
    // Teal aurora layer (offset)
    for(int x=0;x<W;x++){
        int seg=(x+120)%360, half=180;
        int wave=(seg<half)?seg:360-seg;
        int ay=H*32/100 - wave*H/10/180;
        int height=H*8/100;
        for(int y=0;y<height;y++){
            int alpha=y<height/2 ? y*180/(height/2) : (height-y)*180/(height/2);
            uint32_t r=(uint32_t)(10);
            uint32_t g=(uint32_t)(alpha*140/255);
            uint32_t b=(uint32_t)(alpha*220/255);
            int px=x; int py=ay+y; if(py>=0&&py<H&&px>=0&&px<W) vesa_put_pixel(px,py,(r<<16)|(g<<8)|b);
        }
    }
    // Snowy ground
    vgrad(0,H*65/100,W,H*35/100,0x8899B0,0xC8D8E8);
    // Snow contour (gentle hills)
    for(int x=0;x<W;x++){
        int seg=x%400, half=200;
        int hill=(seg<half)?seg:400-seg;
        int snow_y=H*65/100-hill*H/20/200;
        vesa_vline(x,snow_y,H-snow_y,0x99AABB);
    }
    // Pine tree silhouettes
    for(int t=0;t<7;t++){
        int tx=W*(5+t*14)/100;
        int base=H*70/100, ht=H*22/100, tw=W*3/100;
        for(int y=0;y<ht;y++){
            int w2=tw*y/ht;
            vesa_hline(tx+tw-w2,base-ht+y,w2*2,0x08180A);
        }
        vesa_fill_rect(tx+tw-4,base-8,8,8,0x1A0C06);
    }
}

// ═══════════════════════════════════════════════════════════════════
//  Wallpaper table
// ═══════════════════════════════════════════════════════════════════

int current_wallpaper = 0;

const Wallpaper wallpapers[WALLPAPER_COUNT] = {
    { "Space",     "Deep purple cosmos",  0x0C0820, draw_space     },
    { "Forest",    "Lush green canopy",   0x0A1E08, draw_nature     },
    { "Geometric", "Abstract grid art",   0x0A0818, draw_geometric  },
    { "Ocean",     "Calm open waters",    0x061828, draw_ocean      },
    { "City",      "Night cityscape",     0x050510, draw_city       },
    { "Matrix",    "Code rain",           0x020802, draw_matrix     },
    { "Synthwave", "Neon retro grid",     0x120828, draw_synthwave_vga },
    { "Aurora",    "Northern lights",     0x040C18, draw_aurora_vga },
};

static void (*vesa_draws[WALLPAPER_COUNT])(void) = {
    draw_vesa_space,
    draw_vesa_forest,
    draw_vesa_geometric,
    draw_vesa_ocean,
    draw_vesa_city,
    draw_vesa_matrix,
    draw_vesa_synthwave,
    draw_vesa_aurora,
};

// ═══════════════════════════════════════════════════════════════════
//  Animated Twinkling Stars
// ═══════════════════════════════════════════════════════════════════
// A set of procedurally-placed stars drawn on top of the wallpaper.
// Each star's brightness oscillates independently using a sine-like
// triangle wave driven by the system `tick` counter (~18.2 Hz),
// creating a subtle "breathing sky" effect behind the desktop icons.

// Star definition: each star has a position and a twinkle speed/phase
// derived from its index. Larger stars (even indices) get a 4-point
// cross glow; smaller stars (odd indices) are 1-pixel dots.
#define ANIM_STAR_COUNT 48

static void draw_animated_stars(void) {
    if (!vesa.active) return;

    int W = (int)vesa.width;
    int H = (int)vesa.height;

    // Generate positions once (first call), then reuse.
    // Seed with deterministic values so stars don't jump.
    static int sx[ANIM_STAR_COUNT];
    static int sy[ANIM_STAR_COUNT];
    static int inited = 0;

    if (!inited) {
        unsigned int seed = 0xDEADBEEF;
        for (int i = 0; i < ANIM_STAR_COUNT; i++) {
            seed = seed * 1103515245 + 12345;
            sx[i] = (int)((seed >> 16) & 0xFFFF) % W;
            seed = seed * 1103515245 + 12345;
            sy[i] = (int)((seed >> 16) & 0x7FFF) % (H * 55 / 100);  // upper 55%
            // Clamp a few stray pixels
            if (sx[i] < 0) sx[i] = 0;
            if (sx[i] >= W) sx[i] = W - 1;
            if (sy[i] < 4) sy[i] = 4;
            if (sy[i] > H * 55 / 100) sy[i] = H * 55 / 100 - 1;
        }
        inited = 1;
    }

    extern volatile uint32_t tick;

    for (int i = 0; i < ANIM_STAR_COUNT; i++) {
        int x = sx[i], y = sy[i];

        // ── Brightness oscillation (triangle wave) ────────────────
        // Each star has its own phase offset (derived from index),
        // so they twinkle independently — no two stars pulse in sync.
        // The wave period varies: small stars twinkle faster (phase
        // divider 5..9), large stars slower (3..5), creating a
        // natural-looking mix of quick and slow twinkles.
        int phase_shift = (i * 17 + i * i * 3) & 0xFF;
        int speed_div = (i & 1) ? (5 + (i % 5)) : (3 + (i % 3));
        int raw = (tick * 3 + phase_shift * 4) % (speed_div * 128);

        // Triangle wave: 0 → max → 0
        int half = speed_div * 64;
        int brightness;
        if (raw < half) {
            brightness = raw * 255 / half;           // 0 → 255
        } else {
            brightness = (half * 2 - raw) * 255 / half;  // 255 → 0
        }

        // Never go fully dark — keep a minimum glow so stars don't
        // completely disappear. Larger stars stay brighter overall.
        if (i & 1) {
            // Small star: dimmer, range 20..200
            if (brightness < 20) brightness = 20;
            if (brightness > 200) brightness = 200;

            // Small star: slight blue-white tint
            uint32_t r = brightness;
            uint32_t g = brightness * 180 / 255;
            uint32_t b = brightness * 220 / 255;
            uint32_t c = (r << 16) | (g << 8) | b;
            vesa_put_pixel(x, y, c);
        } else {
            // Large star: brighter, range 40..255
            if (brightness < 40) brightness = 40;
            if (brightness > 255) brightness = 255;

            uint32_t r = brightness;
            uint32_t g = brightness * 190 / 255;
            uint32_t b = brightness * 230 / 255;
            uint32_t c  = (r << 16) | (g << 8) | b;
            uint32_t cd = ((r*2/3) << 16) | ((g*2/3) << 8) | (b*2/3);

            // 4-point cross glow
            vesa_put_pixel(x, y, c);
            vesa_put_pixel(x-1, y, cd);
            vesa_put_pixel(x+1, y, cd);
            vesa_put_pixel(x, y-1, cd);
            vesa_put_pixel(x, y+1, cd);

            // Extra corner sparkles for the brightest stars (top 30%)
            if (brightness > 180) {
                uint32_t cs = ((r/4) << 16) | ((g/4) << 8) | (b/4);
                vesa_put_pixel(x-1, y-1, cs);
                vesa_put_pixel(x+1, y-1, cs);
                vesa_put_pixel(x-1, y+1, cs);
                vesa_put_pixel(x+1, y+1, cs);
            }
        }
    }
}

// ─── Dim overlay + brightness ─────────────────────────────────────
// Applies a semi-transparent black overlay on top of the wallpaper.
// 0 = no dim, 100 = 50% black (max). Stored as percentage for the UI.
int wallpaper_dim_level = 0;

void wallpaper_set_dim(int level) {
    if (level < 0) level = 0;
    if (level > 100) level = 100;
    wallpaper_dim_level = level;
}

// Screen brightness (0-100, 100 = full). Folded into the same overlay
// pass as the dim level, so both compose multiplicatively. 100 is the
// default and skips the pass entirely (no per-frame cost).
int wallpaper_brightness_level = 100;

void wallpaper_set_brightness(int level) {
    if (level < 0) level = 0;
    if (level > 100) level = 100;
    wallpaper_brightness_level = level;
}

// ─── Custom file-based wallpaper ──────────────────────────────────
// When custom_wallpaper_file >= 0, the desktop background is loaded
// from a filesystem image file (PNG, JPEG, BMP, GIF) instead of a
// procedural built-in wallpaper. The image is decoded once and
// cached in wp_cached_pixels[] at a size matching the current VESA
// resolution, so wallpaper_draw() can blit it cheaply every frame.

int custom_wallpaper_file = -1;

// Cached decoded wallpaper pixels (width x height x 4 bytes).
// We use a fixed maximum size that fits the VESA framebuffer.
#define WP_CACHE_MAX (WALLPAPER_MAX_W * WALLPAPER_MAX_H * 4)
static uint8_t  wp_cached_pixels[WP_CACHE_MAX] __attribute__((aligned(16)));
static int      wp_cache_w = 0;
static int      wp_cache_h = 0;
static int      wp_cache_valid = 0;  // 0 = need to decode

// Forward declarations for image decoders (included via desktop.c's
// include chain, but wallpaper.c needs its own includes here):
#include <pngdec.h>
#include <jpegdec.h>
#include <bmpdec.h>
#include <gifdec.h>
#include <fs.h>

// Decode a file from the filesystem and cache it in wp_cached_pixels.
// Returns 1 on success, 0 on failure.
static int wallpaper_decode_file(int file_idx) {
    wp_cache_valid = 0;
    wp_cache_w = 0;
    wp_cache_h = 0;

    if (file_idx < 0 || file_idx >= MAX_FILES) return 0;
    if (!filesystem[file_idx].used) return 0;

    const uint8_t* data = (const uint8_t*)filesystem[file_idx].content;
    int datalen = filesystem[file_idx].size;
    if (datalen <= 0) { while (data[datalen]) datalen++; }
    if (datalen < 4) return 0;

    // Temporary decode buffer (separate from the cache).
    // MUST be static — the kernel stack is only 16 KB and this buffer is 8 MB.
    static uint8_t temp_pixels[WP_CACHE_MAX] __attribute__((aligned(64)));

    int w = 0, h = 0;
    int decoded = 0;

    if (data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G') {
        PngImage img;
        img.pixels = temp_pixels;
        if (png_decode(data, datalen, &img)) {
            w = (int)img.width;
            h = (int)img.height;
            decoded = 1;
        }
    } else if (data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) {
        JpegImage img = {0};
        img.pixels = temp_pixels;
        if (jpeg_decode(data, datalen, &img)) {
            w = (int)img.width;
            h = (int)img.height;
            decoded = 1;
        }
    } else if (data[0] == 'B' && data[1] == 'M') {
        BmpImage img;
        img.pixels = temp_pixels;
        if (bmp_decode(data, datalen, &img)) {
            w = (int)img.width;
            h = (int)img.height;
            decoded = 1;
        }
    } else if (data[0] == 'G' && data[1] == 'I' && data[2] == 'F') {
        GifImage img;
        img.pixels = temp_pixels;
        if (gif_decode(data, datalen, &img)) {
            w = (int)img.width;
            h = (int)img.height;
            decoded = 1;
        }
    }

    if (!decoded || w <= 0 || h <= 0) return 0;

    // Save the original decoded dimensions for temp_pixels stride.
    // temp_pixels was written by the decoder with stride = original_w * 4,
    // which may differ from the clamped w below (for images > 1920×1080).
    int orig_stride_w = w;

    // Clamp dimensions to our cache size
    if (w > WALLPAPER_MAX_W) w = WALLPAPER_MAX_W;
    if (h > WALLPAPER_MAX_H) h = WALLPAPER_MAX_H;

    int screen_w = (int)vesa.width;
    int screen_h = (int)vesa.height;
    if (screen_w <= 0) screen_w = 1024;
    if (screen_h <= 0) screen_h = 768;

    // Stretch the image to fill the entire screen (no black borders).
    int dst_w = screen_w;
    int dst_h = screen_h;

    // No offset — image fills the whole framebuffer
    int off_x = 0;
    int off_y = 0;

    // Scale decoder RGBA into the cache using bilinear interpolation.
    // Uses the same two-pass 1D approach as vesa_blit_rgba_scaled_bilinear()
    // to avoid 32-bit integer overflow while producing smooth results.
    // The cache is still RGBA at this point; the R↔B swap to BGRA happens
    // in a separate pass below via vesa_convert_rgba_bgra().
    // Precompute fixed-point step values and use an accumulator pattern
    // instead of `sy * (h << 16) / dst_h` to avoid int32 overflow.
    int step_x = (w << 16) / dst_w;
    int step_y = (h << 16) / dst_h;
    int src_y_fp = step_y / 2;  // half-pixel offset for symmetric filter

    for (int sy = 0; sy < dst_h; sy++) {
        int src_y0 = src_y_fp >> 16;
        int src_y1 = src_y0 + 1;
        if (src_y0 >= h) src_y0 = h - 1;
        if (src_y1 >= h) src_y1 = h - 1;
        int wy  = (src_y_fp & 0xFFFF);
        int iwy = 65536 - wy;

        int src_x_fp = step_x / 2;  // half-pixel offset for symmetric filter

        for (int sx = 0; sx < dst_w; sx++) {
            int src_x0 = src_x_fp >> 16;
            int src_x1 = src_x0 + 1;
            if (src_x0 >= w) src_x0 = w - 1;
            if (src_x1 >= w) src_x1 = w - 1;
            int wx  = (src_x_fp & 0xFFFF);
            int iwx = 65536 - wx;

            // temp_pixels stride is orig_stride_w * 4 (the original
            // pre-clamp width), not the clamped w.
            int stride = orig_stride_w * 4;
            int si00 = src_y0 * stride + src_x0 * 4;
            int si01 = src_y0 * stride + src_x1 * 4;
            int si10 = src_y1 * stride + src_x0 * 4;
            int si11 = src_y1 * stride + src_x1 * 4;

            int dst_idx = ((off_y + sy) * screen_w + (off_x + sx)) * 4;

            if (dst_idx + 3 < screen_w * screen_h * 4) {
                // Pass 1: horizontal blend (fits int32: 2×255×65536 ≈ 33M)
                int h0r = ((int)temp_pixels[si00+0]*iwx + (int)temp_pixels[si01+0]*wx + 32768) / 65536;
                int h0g = ((int)temp_pixels[si00+1]*iwx + (int)temp_pixels[si01+1]*wx + 32768) / 65536;
                int h0b = ((int)temp_pixels[si00+2]*iwx + (int)temp_pixels[si01+2]*wx + 32768) / 65536;
                int h1r = ((int)temp_pixels[si10+0]*iwx + (int)temp_pixels[si11+0]*wx + 32768) / 65536;
                int h1g = ((int)temp_pixels[si10+1]*iwx + (int)temp_pixels[si11+1]*wx + 32768) / 65536;
                int h1b = ((int)temp_pixels[si10+2]*iwx + (int)temp_pixels[si11+2]*wx + 32768) / 65536;

                // Pass 2: vertical blend (fits int32: 2×510×65536 ≈ 67M)
                int r = (h0r*iwy + h1r*wy + 32768) / 65536;
                int g = (h0g*iwy + h1g*wy + 32768) / 65536;
                int b = (h0b*iwy + h1b*wy + 32768) / 65536;

                if (r < 0) r = 0; if (r > 255) r = 255;
                if (g < 0) g = 0; if (g > 255) g = 255;
                if (b < 0) b = 0; if (b > 255) b = 255;

                // Write RGBA — no swap yet, converted to BGRA below
                wp_cached_pixels[dst_idx + 0] = (uint8_t)r;
                wp_cached_pixels[dst_idx + 1] = (uint8_t)g;
                wp_cached_pixels[dst_idx + 2] = (uint8_t)b;
                wp_cached_pixels[dst_idx + 3] = 0;
            }

            src_x_fp += step_x;
        }
        src_y_fp += step_y;
    }

    // Convert the scaled image region from RGBA to BGRA in-place.
    // The black border pixels (filled above in BGRA) are left unchanged.
    vesa_convert_rgba_bgra(
        &wp_cached_pixels[(off_y * screen_w + off_x) * 4],
        screen_w * 4, dst_w, dst_h);

    wp_cache_w = screen_w;
    wp_cache_h = screen_h;
    wp_cache_valid = 1;
    return 1;
}

// ─── Public API ───────────────────────────────────────────────────

void wallpaper_set_custom(int file_idx) {
    custom_wallpaper_file = file_idx;
    wp_cache_valid = 0;  // Force re-decode on next draw
}

const char* wallpaper_get_name(void) {
    if (custom_wallpaper_file >= 0 && custom_wallpaper_file < MAX_FILES) {
        if (filesystem[custom_wallpaper_file].used)
            return filesystem[custom_wallpaper_file].name;
    }
    // Fall back to built-in name
    int idx = (current_wallpaper >= 0 && current_wallpaper < WALLPAPER_COUNT)
              ? current_wallpaper : 0;
    return wallpapers[idx].name;
}

// ─── Apply dim overlay ─────────────────────────────────────────────
// After the wallpaper is drawn to the framebuffer, multiply every
// pixel by (255 - alpha) / 255 where alpha = dim_level * 128 / 100.
// This gives a semi-transparent black overlay (0% black at dim=0,
// 50% black at dim=100).
static void wallpaper_apply_dim(void) {
    if (wallpaper_dim_level <= 0 && wallpaper_brightness_level >= 100) return;

    int alpha = wallpaper_dim_level * 128 / 100;  // 0..128
    if (alpha > 128) alpha = 128;
    if (alpha < 0) alpha = 0;
    int mul = 255 - alpha;  // 127..255
    // Compose the screen-brightness factor on top of the dim overlay.
    mul = mul * wallpaper_brightness_level / 100;
    if (mul >= 255) return;  // nothing to do (brightness full, no dim)

    int w = (int)vesa.width;
    int h = (int)vesa.height;
    int pitch = (int)vesa.pitch;
    int bpp = (int)(vesa.bpp / 8);

    for (int y = 0; y < h; y++) {
        uint8_t* row = vesa.draw + (uint32_t)y * (uint32_t)pitch;
        if (bpp == 4) {
            for (int x = 0; x < w; x++) {
                row[x*4 + 0] = (uint8_t)((uint32_t)row[x*4 + 0] * mul / 255);  // B
                row[x*4 + 1] = (uint8_t)((uint32_t)row[x*4 + 1] * mul / 255);  // G
                row[x*4 + 2] = (uint8_t)((uint32_t)row[x*4 + 2] * mul / 255);  // R
            }
        } else {
            for (int x = 0; x < w; x++) {
                row[x*3 + 0] = (uint8_t)((uint32_t)row[x*3 + 0] * mul / 255);
                row[x*3 + 1] = (uint8_t)((uint32_t)row[x*3 + 1] * mul / 255);
                row[x*3 + 2] = (uint8_t)((uint32_t)row[x*3 + 2] * mul / 255);
            }
        }
    }
}

void wallpaper_draw(void){
    // ── VESA path ──
    if (vesa.active) {
        // Custom file-based wallpaper takes priority
        if (custom_wallpaper_file >= 0) {
            if (!wp_cache_valid) {
                wallpaper_decode_file(custom_wallpaper_file);
            }
            if (wp_cache_valid && wp_cache_w > 0 && wp_cache_h > 0) {
                vesa_blit_bgra(0, 0, wp_cached_pixels,
                               wp_cache_w, wp_cache_h,
                               0, 0, wp_cache_w, wp_cache_h);
            } else {
                // Fallback to built-in if decode failed
                int idx = (current_wallpaper >= 0 && current_wallpaper < WALLPAPER_COUNT)
                          ? current_wallpaper : 0;
                vesa_draws[idx]();
            }
        } else {
            // Built-in VESA wallpaper
            int idx = (current_wallpaper >= 0 && current_wallpaper < WALLPAPER_COUNT)
                      ? current_wallpaper : 0;
            vesa_draws[idx]();
        }

        // Draw animated twinkling stars on top of the wallpaper
        draw_animated_stars();

        // Apply dim overlay on top of everything
        wallpaper_apply_dim();
        return;
    }

    // ── VGA text fallback (no dim overlay) ──
    int idx = (current_wallpaper >= 0 && current_wallpaper < WALLPAPER_COUNT)
              ? current_wallpaper : 0;
    wallpapers[idx].draw();
}

void wallpaper_draw_fast(void){
    if(!vesa.active) return;
    if (custom_wallpaper_file >= 0 && wp_cache_valid) {
        vesa_blit_bgra(0, 0, wp_cached_pixels,
                       wp_cache_w, wp_cache_h,
                       0, 0, wp_cache_w, wp_cache_h);
    } else {
        int idx = (current_wallpaper >= 0 && current_wallpaper < WALLPAPER_COUNT)
                  ? current_wallpaper : 0;
        vesa_clear(wallpapers[idx].preview_color);
    }
    // Apply dim overlay
    wallpaper_apply_dim();
}
