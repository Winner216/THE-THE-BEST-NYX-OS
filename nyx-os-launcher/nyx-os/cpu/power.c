// cpu/power.c
#include <power.h>
#include <speaker.h>
#include <rtc.h>
#include <auth.h>
#include <vesa.h>
#include <fs.h>
#include <stdint.h>

#define VGA_WIDTH  80
#define VGA_HEIGHT 25
#define VGA_MEM    ((uint16_t*)0xB8000)

static inline uint16_t make_entry(char c, uint8_t fg, uint8_t bg) {
    return (uint16_t)c | (uint16_t)(fg | bg << 4) << 8;
}

// Standard 16-color VGA palette mapped to 24-bit RGB for VESA rendering
static const uint32_t p_vga16_rgb[16] = {
    0x000000,0x0000AA,0x00AA00,0x00AAAA,0xAA0000,0xAA00AA,0xAA5500,0xAAAAAA,
    0x555555,0x5555FF,0x55FF55,0x55FFFF,0xFF5555,0xFF55FF,0xFFFF55,0xFFFFFF,
};
static int p_off_x(void){ return ((int)vesa.width  - VGA_WIDTH*8)  / 2; }
static int p_off_y(void){ return ((int)vesa.height - VGA_HEIGHT*16) / 2; }

static void pput(int x, int y, char c, uint8_t fg, uint8_t bg) {
    if (x<0||x>=VGA_WIDTH||y<0||y>=VGA_HEIGHT) return;

    if (vesa.active) {
        int ox = p_off_x() + x*8;
        int oy = p_off_y() + y*16;
        vesa_draw_char(ox, oy, c, p_vga16_rgb[fg & 0x0F], p_vga16_rgb[bg & 0x0F]);
        return;
    }

    VGA_MEM[y*VGA_WIDTH+x] = make_entry(c,fg,bg);
}

// ── Pixel primitives for the redesigned power menu ─────────────────
static void pm_rrect(int x,int y,int w,int h,uint32_t c){
    vesa_fill_rect(x+3,y,w-6,h,c);
    vesa_fill_rect(x,y+3,w,h-6,c);
    vesa_fill_rect(x+1,y+1,w-2,h-2,c);
}
static void pm_rrect_border(int x,int y,int w,int h,uint32_t c){
    vesa_hline(x+3,y,w-6,c); vesa_hline(x+3,y+h-1,w-6,c);
    vesa_vline(x,y+3,h-6,c); vesa_vline(x+w-1,y+3,h-6,c);
    vesa_put_pixel(x+1,y+1,c); vesa_put_pixel(x+w-2,y+1,c);
    vesa_put_pixel(x+1,y+h-2,c); vesa_put_pixel(x+w-2,y+h-2,c);
    vesa_put_pixel(x+2,y,c); vesa_put_pixel(x+w-3,y,c);
    vesa_put_pixel(x+2,y+h-1,c); vesa_put_pixel(x+w-3,y+h-1,c);
}
static void pm_hgrad(int x,int y,int w,int h,uint32_t c1,uint32_t c2){
    for(int dx=0;dx<w;dx++){
        uint32_t r=((c1>>16)&0xFF)+(int)(((c2>>16)&0xFF)-((c1>>16)&0xFF))*dx/w;
        uint32_t g=((c1>>8)&0xFF)+(int)(((c2>>8)&0xFF)-((c1>>8)&0xFF))*dx/w;
        uint32_t b=(c1&0xFF)+(int)((c2&0xFF)-(c1&0xFF))*dx/w;
        vesa_vline(x+dx,y,h,(r<<16)|(g<<8)|b);
    }
}
static int pm_slen(const char*s){int l=0;while(s[l])l++;return l;}
static void pputs(int x, int y, const char* s, uint8_t fg, uint8_t bg) {
    for (int i=0;s[i];i++) pput(x+i,y,s[i],fg,bg);
}
static void pfill(int x, int y, int w, int h, char c, uint8_t fg, uint8_t bg) {
    for (int dy=0;dy<h;dy++)
        for (int dx=0;dx<w;dx++)
            pput(x+dx,y+dy,c,fg,bg);
}
static void pcenter(int y, const char* s, uint8_t fg, uint8_t bg) {
    int len=0; while(s[len])len++;
    pputs((VGA_WIDTH-len)/2,y,s,fg,bg);
}
static void delay_p(uint32_t ms) {
    for (uint32_t i=0;i<ms*100000;i++) asm volatile("nop");
}

// ─── IO helpers ───────────────────────────────────────────────────
static inline void outw_p(uint16_t port, uint16_t val) {
    asm volatile("outw %1,%0"::"dN"(port),"a"(val));
}

// ─── Power Menu State ─────────────────────────────────────────────
static int menu_open     = 0;
static int menu_selected = 0;

// Session action flag. Lock/Restart are selected from deep inside
// the keyboard IRQ handler (power_menu_handle_key, called via
// desktop_handle_key, called via keyboard_callback) — there's no
// call stack there to "return" through back to kernel_main. So
// instead they just set this flag, and kernel_main's top-level hlt
// loop polls it once per cycle and reacts at the right scope.
static int session_action = SESSION_RUNNING;

int power_get_session_action(void) {
    return session_action;
}
void power_clear_session_action(void) {
    session_action = SESSION_RUNNING;
}
int power_session_pending(void) {
    return session_action != SESSION_RUNNING;
}

#define MENU_ITEMS 6
static const char* menu_labels[] = {
    "  Shutdown  ",
    "  Restart   ",
    "  Sleep     ",
    "  Hibernate ",
    "  Lock      ",
    "  Cancel    ",
};
static const uint8_t menu_colors[] = {
    12, // Red    - Shutdown
    14, // Yellow - Restart
    11, // Cyan   - Sleep
    13, // Magenta- Hibernate
    9,  // Blue   - Lock
    7,  // Grey   - Cancel
};
static const char* menu_icons[] = {
    "[X]", "[R]", "[Z]", "[H]", "[L]", "[ ]"
};
static const char* menu_descs[] = {
    "Turn off the computer",
    "Restart the computer",
    "Low power sleep mode",
    "Save state and power off",
    "Lock screen",
    "Return to desktop",
};
// True-color icon glyph + accent color per menu item, used by the
// VESA pixel rendering (the 16-color menu_colors[] above is kept
// for the VGA text fallback only).
static const char  menu_glyph[] = { 'P','R','Z','H','L','X' };
static const uint32_t menu_rgb[] = {
    0xFF6B6B, // Shutdown  - red
    0xFFD166, // Restart   - amber
    0x8FE0FF, // Sleep     - cyan
    0xE0A8FF, // Hibernate - lavender
    0x8FC4FF, // Lock      - sky blue
    0x9999AA, // Cancel    - grey
};

// ─── Sleep animation ──────────────────────────────────────────────
static void sleep_animation() {
    // Fade screen to dark
    for (int step=0;step<4;step++) {
        // Draw dimming overlay
        for (int y=0;y<VGA_HEIGHT;y++)
            for (int x=0;x<VGA_WIDTH;x++)
                pput(x,y,' ',0,0);
        delay_p(100);
    }

    // Draw sleep screen
    pfill(0,0,VGA_WIDTH,VGA_HEIGHT,' ',0,1);

    // Nyx logo faded

   pcenter(6,  "  _   _            ____  _____ ", 8, 0);
   pcenter(7,  " | \\ | |_   ___  _/ __ \\/ ____|", 8, 0);
   pcenter(8,  " |  \\| | | | \\ \\/ / |  | (___  ", 8, 0);
   pcenter(9,  " | . ` | | | |\\  /| |  |\\___ \\ ", 8, 0);
   pcenter(10, " | |\\  | |_| |/  \\| |__| ____) |", 8, 0);
   pcenter(11, " |_| \\_|\\__, /_/\\_\\\\____/|_____/ ", 8, 0);
   pcenter(12, "          __/ |                  ", 8, 0);
   pcenter(13, "         |___/   O S             ", 8, 0);
    // Sleep Z's animation
    pcenter(13, "Z z z . . .", 11, 1);
    pcenter(15, "System is sleeping...", 7, 1);
    pcenter(16, "Press any key to wake up", 8, 1);

    // Time
    char time_str[9]; rtc_get_time_str(time_str);
    pcenter(18, time_str, 14, 1);

    // Breathing animation
    uint8_t colors[] = {8, 8, 7, 11, 11, 7, 8};
    for (int rep=0;rep<3;rep++) {
        for (int i=0;i<7;i++) {
            pcenter(13, "Z z z . . .", colors[i], 1);
            delay_p(200);
        }
    }
}

// ─── Hibernate animation ──────────────────────────────────────────
static void hibernate_animation() {
    pfill(0,0,VGA_WIDTH,VGA_HEIGHT,' ',0,0);
    pcenter(8,  "Hibernating...", 11, 0);
    pcenter(10, "Saving system state", 7, 0);

    // Progress bar
    for (int i=0;i<40;i++) {
        pput(20+i, 12, i<20?'#':'-', i<20?10:8, 0);
        delay_p(50);
    }
    pcenter(14, "State saved. Powering off...", 8, 0);
    delay_p(500);
}

// ─── Restart animation ────────────────────────────────────────────
static void restart_animation() {
    pfill(0,0,VGA_WIDTH,VGA_HEIGHT,' ',0,0);
    pcenter(9,  "Restarting Nyx OS...", 14, 0);
    pcenter(11, "Please wait", 7, 0);

    // Spinning indicator
    const char* spin = "|/-\\";
    for (int i=0;i<20;i++) {
        pput(VGA_WIDTH/2, 13, spin[i%4], 11, 0);
        delay_p(100);
    }
    pcenter(15, "See you soon!", 10, 0);
    delay_p(500);
}

// ─── Shutdown animation ───────────────────────────────────────────
static void shutdown_animation() {
    pfill(0,0,VGA_WIDTH,VGA_HEIGHT,' ',0,0);

   pcenter(6,  "  _   _            ____  _____ ", 8, 0);
   pcenter(7,  " | \\ | |_   ___  _/ __ \\/ ____|", 8, 0);
   pcenter(8,  " |  \\| | | | \\ \\/ / |  | (___  ", 8, 0);
   pcenter(9,  " | . ` | | | |\\  /| |  |\\___ \\ ", 8, 0);
   pcenter(10, " | |\\  | |_| |/  \\| |__| ____) |", 8, 0);
   pcenter(11, " |_| \\_|\\__, /_/\\_\\\\____/|_____/ ", 8, 0);
   pcenter(12, "          __/ |                  ", 8, 0);
   pcenter(13, "         |___/   O S             ", 8, 0);
    pcenter(16, "Shutting down...", 7, 0);

    // Goodbye message
    if (logged_in_user >= 0) {
        char msg[40] = "Goodbye, ";
        int p=9;
        const char* uname = users[logged_in_user].username;
        for(int i=0;uname[i]&&p<38;i++) msg[p++]=uname[i];
        msg[p++]='!'; msg[p]='\0';
        pcenter(15, msg, 11, 0);
    }

    // Play shutdown melody
     delay_p(150);
     delay_p(150);
     delay_p(300);
    

    delay_p(500);
}

// ─── Power actions ────────────────────────────────────────────────
void power_shutdown() {
    fs_save_to_disk();   // guarantee a fresh save before power-off
    shutdown_animation();
    // ACPI shutdown
    asm volatile("cli");
    // Try ACPI S5 (power off) via multiple methods
    outw_p(0x604, 0x2000);  // QEMU
    outw_p(0xB004, 0x2000); // Bochs/older QEMU
    outw_p(0x4004, 0x3400); // VirtualBox
    // Final halt
    asm volatile("hlt");
}

void power_restart() {
    fs_save_to_disk();   // guarantee a fresh save before restart
    restart_animation();
    session_action = SESSION_RESTART;
    // kernel_main's polling loop picks this up on its next hlt-wake
    // (the timer IRQ fires constantly, so this is near-instant) and
    // replays the splash before returning to login_show().
}

void power_lock() {
    
    session_action = SESSION_LOCK;
    // Same hand-off as Restart, but kernel_main skips the splash
    // replay and goes straight back to login_show() — locking and
    // logging out are the same thing from the user's point of view
    // here, so they share the exact same screen.
}

void power_sleep() {
    sleep_animation();
    // In real HW would use ACPI S3
    // In QEMU we just wait for keypress (handled by keyboard interrupt)
    // The sleep screen stays until a key is pressed
    // This is handled by returning and letting keyboard wake us
}

void power_hibernate() {
    fs_save_to_disk();   // guarantee a fresh save, same as shutdown
    hibernate_animation();
    // In real implementation would save RAM to disk
    // For now simulate with shutdown
    asm volatile("cli");
    outw_p(0x604, 0x2000);
    outw_p(0xB004, 0x2000);
    asm volatile("hlt");
}

// ─── Power Menu Draw ──────────────────────────────────────────────
void power_menu_draw() {
    if (!menu_open) return;

    if (!vesa.active) {
        // ── VGA text fallback (unchanged) ──
        pfill(0,0,VGA_WIDTH,VGA_HEIGHT,' ',8,0);
        int bx=28, by=6, bw=24, bh=MENU_ITEMS*2+5;
        pfill(bx,by,bw,bh,' ',15,1);
        for(int x=bx;x<bx+bw;x++){pput(x,by,'-',14,1);pput(x,by+bh-1,'-',14,1);}
        for(int y=by;y<by+bh;y++){pput(bx,y,'|',14,1);pput(bx+bw-1,y,'|',14,1);}
        pput(bx,by,'+',14,1); pput(bx+bw-1,by,'+',14,1);
        pput(bx,by+bh-1,'+',14,1); pput(bx+bw-1,by+bh-1,'+',14,1);
        pputs(bx+2,by,"[ Power Options ]",14,1);
        if(logged_in_user>=0){
            pputs(bx+2,by+1,"User: ",8,1);
            pputs(bx+8,by+1,users[logged_in_user].username,11,1);
        }
        char time_str[9]; rtc_get_time_str(time_str);
        pputs(bx+2,by+2,time_str,7,1);
        pputs(bx+12,by+2,"--------",8,1);
        for(int i=0;i<MENU_ITEMS;i++){
            int iy=by+4+i*2;
            uint8_t fg=(i==menu_selected)?0:menu_colors[i];
            uint8_t bg=(i==menu_selected)?menu_colors[i]:1;
            pfill(bx+1,iy,bw-2,1,' ',fg,bg);
            pputs(bx+2,iy,menu_icons[i],fg,bg);
            pputs(bx+6,iy,menu_labels[i],fg,bg);
        }
        pfill(bx+1,by+bh-3,bw-2,1,' ',7,1);
        pputs(bx+2,by+bh-3,menu_descs[menu_selected],7,1);
        pfill(bx+1,by+bh-2,bw-2,1,' ',8,1);
        pputs(bx+2,by+bh-2,"UP/DN:select ENTER:ok",8,1);
        return;
    }

    // ── VESA pixel rendering ──────────────────────────────────────
    int W=(int)vesa.width, H=(int)vesa.height, CX=W/2, CY=H/2;

    // Dim overlay behind the card so the desktop is still visible
    // but clearly backgrounded, like a real modal dialog.
    vesa_fill_rect(0,0,W,H,0x05030C);

    int cw=300, item_h=44, items_top_pad=14, items_bot_pad=14;
    int ch = 70 + MENU_ITEMS*item_h + items_top_pad + items_bot_pad + 50;
    int cx2=CX-cw/2, cy2=CY-ch/2;

    // Card shadow + body
    vesa_fill_rect(cx2+5,cy2+5,cw,ch,0x020108);
    pm_rrect(cx2,cy2,cw,ch,0x140C30);
    pm_rrect_border(cx2,cy2,cw,ch,0x6A4FBF);

    // Header
    int hdr_h=58;
    pm_hgrad(cx2+2,cy2+2,cw-4,hdr_h,0x2E2050,0x1E1240);
    vesa_hline(cx2+2,cy2+hdr_h,cw-4,0xB088FF);
    vesa_draw_string(cx2+18,cy2+14,"* Power Options",0xE8D8FF,0x271848);

    if (logged_in_user>=0)
        vesa_draw_string(cx2+18,cy2+34,users[logged_in_user].username,0x9580C0,0x201038);
    char time_str[9]; rtc_get_time_str(time_str);
    int tlen=pm_slen(time_str);
    vesa_draw_string(cx2+cw-18-tlen*8,cy2+34,time_str,0x9580C0,0x201038);

    // Menu items
    int iy = cy2+hdr_h+items_top_pad;
    int item_x = cx2+10, item_w = cw-20;
    for (int i=0;i<MENU_ITEMS;i++) {
        int sel = (i==menu_selected);
        if (sel) {
            pm_rrect(item_x,iy,item_w,item_h-6,0x2E2050);
            pm_rrect_border(item_x,iy,item_w,item_h-6,0xB088FF);
        }
        // Icon box
        int ib=30;
        int ibx=item_x+8, iby=iy+(item_h-6-ib)/2;
        vesa_fill_rect(ibx,iby,ib,ib,0x1A1140);
        vesa_draw_rect(ibx,iby,ib,ib,menu_rgb[i]);
        char glyph[2]={menu_glyph[i],'\0'};
        vesa_draw_char(ibx+ib/2-4,iby+ib/2-8,glyph[0],menu_rgb[i],0x1A1140);

        // Label — background matches whichever surface this item
        // is currently drawn on (the rounded highlight when
        // selected, otherwise the plain card body), so the text
        // never shows a mismatched black box behind it.
        uint32_t lbl_col = sel?0xFFFFFF:0xD8C8F0;
        uint32_t lbl_bg  = sel?0x2E2050:0x140C30;
        vesa_draw_string(item_x+50,iy+(item_h-6)/2-8,menu_labels[i],lbl_col,lbl_bg);

        iy += item_h;
    }

    // Description footer
    int desc_y = cy2+ch-items_bot_pad-34;
    vesa_hline(cx2+14,desc_y-8,cw-28,0x1E1240);
    vesa_draw_string(cx2+18,desc_y,menu_descs[menu_selected],0x6A5890,0x140C30);

    // Hint footer
    vesa_draw_string(cx2+18,cy2+ch-20,"UP/DOWN select   ENTER confirm   ESC cancel",0x3D2A66,0x140C30);

    if (vesa.active) vesa_present();
}

void power_menu_handle_key(char c) {
    if (!menu_open) return;

    if (c=='A'&&menu_selected>0){
        menu_selected--;
        power_menu_draw();
        return;
    }
    if (c=='B'&&menu_selected<MENU_ITEMS-1){
        menu_selected++;
        power_menu_draw();
        return;
    }
    if (c==27||menu_selected==MENU_ITEMS-1){
        // Cancel
        menu_open=0;
        return;
    }
    if (c==13){
        menu_open=0;
        
        switch(menu_selected){
            case 0: power_shutdown();  break;
            case 1: power_restart();   break;
            case 2: power_sleep();     break;
            case 3: power_hibernate(); break;
            case 4: power_lock();      break;
            case 5: break; // Cancel
        }
        return;
    }
}

void power_menu_show() {
    menu_open     = 1;
    menu_selected = 0;
    power_menu_draw();
}

int power_menu_is_open() { return menu_open; }
