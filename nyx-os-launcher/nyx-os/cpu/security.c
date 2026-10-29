// cpu/security.c — Nyx OS Security Tools Suite
// Provides a VESA pixel UI (with VGA text fallback) for port scanning,
// network status, and system security auditing.
#include <security.h>
#include <desktop.h>
#include <vesa.h>
#include <stdint.h>
#include <mouse.h>
#include <fs.h>
#include <rtc.h>

// ─── VGA text-mode helpers (self-contained, no dependency on desktop.c) ─
#define VGA_MEM ((uint16_t*)0xB8000)
#define VGA_W 80
#define VGA_H 25
static void vgaput(int x,int y,char c,uint8_t fg,uint8_t bg){
    if(x<0||x>=VGA_W||y<0||y>=VGA_H)return;
    VGA_MEM[y*VGA_W+x]=(uint16_t)c|(uint16_t)(fg|bg<<4)<<8;
}
static void vgaputs(int x,int y,const char*s,uint8_t fg,uint8_t bg){
    for(int i=0;s[i];i++)vgaput(x+i,y,s[i],fg,bg);
}
static void vgafill(int x,int y,int w,int h,char c,uint8_t fg,uint8_t bg){
    for(int dy=0;dy<h;dy++)for(int dx=0;dx<w;dx++)vgaput(x+dx,y+dy,c,fg,bg);
}
static void vgarow(int y,char c,uint8_t fg,uint8_t bg){
    for(int x=0;x<VGA_W;x++)vgaput(x,y,c,fg,bg);
}

// ─── Tabs ─────────────────────────────────────────────────────────
#define TAB_PORTSCAN  0
#define TAB_NETSTAT   1
#define TAB_SECURITY  2
#define TAB_VAULT     3
#define TAB_FILESCAN  4
#define SEC_TAB_COUNT 5

static const char* sec_tab_names[] = {
    "Port Scanner",
    "NetStat",
    "Security Audit",
    "Passwords & Vault",
    "File Scanner",
};

// ─── Colors ───────────────────────────────────────────────────────
#define SEC_BG       0x0E0824
#define SEC_PANEL    0x140C30
#define SEC_BORDER   0x6A4FBF
#define SEC_TITLE    0x2E2050
#define SEC_HEADING  0xB088FF
#define SEC_TEXT     0xD8C8F0
#define SEC_DIM      0x6A5890
#define SEC_GREEN    0x55FF88
#define SEC_RED      0xFF5566
#define SEC_ORANGE   0xFFAA44
#define SEC_HINT     0x3D2A66

// ─── Common ports for scanner ─────────────────────────────────────
#define SCAN_PORT_COUNT 14
static const uint16_t scan_ports[SCAN_PORT_COUNT] = {
    21, 22, 23, 25, 53, 80, 110, 135, 139, 443, 445, 8080, 3306, 3389
};
static const char* scan_port_names[SCAN_PORT_COUNT] = {
    "FTP",     "SSH",     "Telnet",  "SMTP",   "DNS",
    "HTTP",    "POP3",    "RPC",     "NetBIOS","HTTPS",
    "SMB",     "HTTP-Alt","MySQL",   "RDP"
};

static int      sec_tab      = 0;
static int      sec_sel      = 0;
static int      sec_scroll   = 0;
static int      sec_scanning = 0;
static int      sec_scan_prog= 0;
static int      sec_results[SCAN_PORT_COUNT];  // 0=untested, 1=open, 2=closed, 3=filtered
static int      sec_audit_done = 0;

// ─── Vault state ─────────────────────────────────────────────────
#define MAX_VAULT_ENTRIES 12
#define VAULT_FIELD_LEN 32

typedef struct {
    char website[VAULT_FIELD_LEN];
    char username[VAULT_FIELD_LEN];
    char password[VAULT_FIELD_LEN];
} VaultEntry;

// Vault state machine
#define VAULT_LOCKED   0   // need master password to unlock
#define VAULT_UNLOCKED 1   // showing entry list
#define VAULT_ADDING   2   // adding new entry
#define VAULT_VIEWING  3   // viewing a single entry detail
#define VAULT_SETUP    4   // setting up master password first time

static VaultEntry vault_entries[MAX_VAULT_ENTRIES];
static int        vault_count = 0;
static char       vault_master[VAULT_FIELD_LEN] = "";
static int        vault_locked = 1;
static int        vault_state = VAULT_LOCKED;
static int        vault_setup_step = 0;  // 0=enter, 1=confirm

// Input buffers for master password entry/setup
static char vault_input[VAULT_FIELD_LEN];
static int  vault_input_pos = 0;
static char vault_confirm[VAULT_FIELD_LEN];
static int  vault_confirm_pos = 0;

// Add-entry form fields
static char vault_new_website[VAULT_FIELD_LEN];
static char vault_new_username[VAULT_FIELD_LEN];
static char vault_new_password[VAULT_FIELD_LEN];
static int  vault_new_field = 0;  // 0=website, 1=username, 2=password
static int  vault_new_pos = 0;

// List view state
static int  vault_list_sel = 0;
static int  vault_list_scroll = 0;
static int  vault_show_pass = 0;

// Detail view
static int  vault_view_idx = 0;

// ─── File Scanner state ─────────────────────────────────────────
#define SCAN_RESULT_MAX 16
// Per-file scan result flags
#define SCAN_FLAG_NONE      0
#define SCAN_FLAG_BINARY    1  // contains non-printable bytes
#define SCAN_FLAG_SHELL    2  // contains shell/command strings
#define SCAN_FLAG_EXEC     4  // suspicious extension or header
#define SCAN_FLAG_LARGE    8  // unusually large
#define SCAN_FLAG_SUSP_STR 16 // suspicious keywords like "eval", "exec"
#define SCAN_FLAG_SCRIPT   32 // script-like content (.sh, .py patterns)

static int  scan_done = 0;
static int  scan_results[MAX_FILES];  // bitmask of SCAN_FLAG_*
static int  scan_scores[MAX_FILES];   // 0-10 threat score
static int  scan_sel = 0;
static int  scan_scroll = 0;
static int  scan_view_idx = -1;  // -1 = list, >=0 = detail view
static int  scan_anim = 0;  // animation frame for scanning

static const char* scan_flag_names[] = {
    "Binary content detected",
    "Shell command patterns",
    "Executable signature",
    "Suspicious file size",
    "Suspicious code keywords",
    "Script-like content",
};

// ─── Real-time protection admin ─────────────────────────────────
static int  scan_protection_on = 1;     // real-time scanning enabled
static int  scan_alert_total   = 0;     // total alerts this session

// Alert log ring buffer (most recent first)
#define SCAN_LOG_FILE_LEN 32
static char scan_log_files[SCAN_LOG_MAX][SCAN_LOG_FILE_LEN];
static int  scan_log_scores[SCAN_LOG_MAX];

// Forward declaration for timeline recording (defined below)
static void security_timeline_record(int new_state);

int security_protection_status(void) { return scan_protection_on; }
void security_protection_toggle(void) { scan_protection_on = !scan_protection_on; security_config_save(); security_timeline_record(scan_protection_on); }
void security_protection_set(int on)  { scan_protection_on = on ? 1 : 0; security_config_save(); security_timeline_record(scan_protection_on); }

// ─── Prompt indicator state (persisted in .scanprotect) ───────-
static int prompt_indicator_on = 0;

int  security_prompt_indicator_status(void) { return prompt_indicator_on; }
void security_prompt_indicator_toggle(void) { prompt_indicator_on = !prompt_indicator_on; security_config_save(); }

// ─── Protection timeline ring buffer ──────────────────────────
#define TIMELINE_MAX 16
#define TIMELINE_TIME_LEN 9

// Each entry: timestamp "HH:MM:SS" + new protection state (0/1)
static char  timeline_times[TIMELINE_MAX][TIMELINE_TIME_LEN];
static int   timeline_states[TIMELINE_MAX];
static int   timeline_count = 0;

// Record a protection state change with current timestamp
static void security_timeline_record(int new_state) {
    // Shift existing entries down
    for (int i = TIMELINE_MAX - 1; i > 0; i--) {
        for (int j = 0; j < TIMELINE_TIME_LEN; j++)
            timeline_times[i][j] = timeline_times[i-1][j];
        timeline_states[i] = timeline_states[i-1];
    }
    // Capture current time
    rtc_get_time_str(timeline_times[0]);
    timeline_states[0] = new_state;
    if (timeline_count < TIMELINE_MAX) timeline_count++;
}

int  security_timeline_count(void) { return timeline_count; }
const char* security_timeline_time(int idx) {
    if (idx < 0 || idx >= timeline_count) return "";
    return timeline_times[idx];
}
int  security_timeline_state(int idx) {
    if (idx < 0 || idx >= timeline_count) return 0;
    return timeline_states[idx];
}

int  security_alert_count(void) { return scan_alert_total; }
const char* security_alert_file(int idx) {
    if (idx < 0 || idx >= SCAN_LOG_MAX) return "";
    return scan_log_files[idx];
}
int  security_alert_score(int idx) {
    if (idx < 0 || idx >= SCAN_LOG_MAX) return 0;
    return scan_log_scores[idx];
}

// Add an entry to the alert log (ring buffer, newest at [0])
static void scan_log_add(const char* fname, int score) {
    // Shift existing entries down
    for (int i = SCAN_LOG_MAX - 1; i > 0; i--) {
        for (int j = 0; j < SCAN_LOG_FILE_LEN; j++)
            scan_log_files[i][j] = scan_log_files[i-1][j];
        scan_log_scores[i] = scan_log_scores[i-1];
    }
    // Insert new entry at [0]
    int si = 0;
    for (int k = 0; fname[k] && k < SCAN_LOG_FILE_LEN - 1; k++)
        scan_log_files[0][si++] = fname[k];
    scan_log_files[0][si] = '\0';
    scan_log_scores[0] = score;
    scan_alert_total++;
}

// ─── Config persistence — save/load state to .scanprotect file ──
#define CONFIG_FILE ".scanprotect"

void security_config_save(void) {
    // Find or create the config file
    int slot = -1;
    for (int i = 0; i < MAX_FILES; i++) {
        if (filesystem[i].used) {
            int match = 1;
            for (int k = 0; ; k++) {
                if (CONFIG_FILE[k] == '\0' && filesystem[i].name[k] == '\0') break;
                if (CONFIG_FILE[k] != filesystem[i].name[k]) { match = 0; break; }
            }
            if (match) { slot = i; break; }
        }
    }
    if (slot < 0) {
        // Need to create the file
        for (int i = 0; i < MAX_FILES; i++) {
            if (!filesystem[i].used) { slot = i; break; }
        }
        if (slot < 0) return;
        int ni;
        for (ni = 0; CONFIG_FILE[ni] && ni < MAX_FILENAME - 1; ni++)
            filesystem[slot].name[ni] = CONFIG_FILE[ni];
        filesystem[slot].name[ni] = '\0';
        filesystem[slot].used = 1;
        file_count++;
    }
    // Write: "protect=1\nprompt=1\n"
    char* buf = filesystem[slot].content;
    int p = 0;
    buf[p++] = 'p'; buf[p++] = 'r'; buf[p++] = 'o'; buf[p++] = 't';
    buf[p++] = 'e'; buf[p++] = 'c'; buf[p++] = 't'; buf[p++] = '=';
    buf[p++] = scan_protection_on ? '1' : '0';
    buf[p++] = '\n';
    buf[p++] = 'p'; buf[p++] = 'r'; buf[p++] = 'o'; buf[p++] = 'm';
    buf[p++] = 'p'; buf[p++] = 't'; buf[p++] = '=';
    buf[p++] = prompt_indicator_on ? '1' : '0';
    buf[p++] = '\n';
    buf[p] = '\0';
}

void security_config_load(void) {
    int slot = -1;
    for (int i = 0; i < MAX_FILES; i++) {
        if (filesystem[i].used) {
            int match = 1;
            for (int k = 0; ; k++) {
                if (CONFIG_FILE[k] == '\0' && filesystem[i].name[k] == '\0') break;
                if (CONFIG_FILE[k] != filesystem[i].name[k]) { match = 0; break; }
            }
            if (match) { slot = i; break; }
        }
    }
    if (slot < 0) return;  // No config file yet — use defaults
    const char* content = filesystem[slot].content;
    int i = 0;
    // Skip past "protect="
    while (content[i] && content[i] != '=') i++;
    if (content[i] == '=') {
        i++;
        if (content[i] == '0') scan_protection_on = 0;
        else if (content[i] == '1') scan_protection_on = 1;
        while (content[i] && content[i] != '\n') i++;
        if (content[i] == '\n') i++;
        // Skip past "prompt="
        while (content[i] && content[i] != '=') i++;
        if (content[i] == '=') {
            i++;
            if (content[i] == '1') prompt_indicator_on = 1;
            else prompt_indicator_on = 0;
        }
    }
}

// ─── Global tick for cursor blink ───────────────────────────────
extern volatile uint32_t tick;

// ─── Tiny filled circle for list dots ───────────────────────────
static void icon_circ(int cx,int cy,int r,uint32_t c){
    for(int dy=-r;dy<=r;dy++)
        for(int dx=-r;dx<=r;dx++)
            if(dx*dx+dy*dy<=r*r)
                vesa_put_pixel(cx+dx,cy+dy,c);
}

// ─── Forward declarations ────────────────────────────────────────
static void scan_file(int idx);
static void scan_all_files(void);
static void sec_scan_port(int idx);

// ─── Encrypt/decrypt vault entry fields using keyed shift cipher ─
// Uses the master password as a cycling key. Stays within printable
// ASCII (32-126) so null-terminated C strings work correctly.
// Encrypt is done in-place, decrypt requires a separate output buffer.
static void vault_encrypt_field(char *data) {
    int klen = 0;
    while (vault_master[klen]) klen++;
    if (klen == 0) return;
    for (int i = 0; i < VAULT_FIELD_LEN - 1; i++) {
        if (data[i] == '\0') break;
        unsigned char c = (unsigned char)data[i];
        unsigned char key = (unsigned char)vault_master[i % klen];
        int offset = (int)(c - 32 + key) % 95;
        data[i] = (char)(offset + 32);
    }
}

// Decrypt into a separate output buffer (src stays encrypted)
static void vault_decrypt_field(const char *src, char *dst, int max_len) {
    int klen = 0;
    while (vault_master[klen]) klen++;
    if (klen == 0) {
        int i;
        for (i = 0; src[i] && i < max_len - 1; i++) dst[i] = src[i];
        dst[i] = '\0';
        return;
    }
    for (int i = 0; i < max_len - 1; i++) {
        if (src[i] == '\0') { dst[i] = '\0'; break; }
        unsigned char c = (unsigned char)src[i];
        unsigned char key = (unsigned char)vault_master[i % klen];
        int offset = (int)(c - 32 - key) % 95;
        if (offset < 0) offset += 95;
        dst[i] = (char)(offset + 32);
        dst[i + 1] = '\0';
    }
}

// ─── Helper: draw a glass-style panel ─────────────────────────────
static void sec_panel(int x, int y, int w, int h, uint32_t color) {
    for (int dy = 0; dy < h; dy++) {
        int t2 = dy * 255 / h;
        int r = ((color >> 16) & 0xFF) - t2 / 8; if (r < 0) r = 0;
        int g = ((color >> 8) & 0xFF)  - t2 / 10; if (g < 0) g = 0;
        int b = (color & 0xFF)         - t2 / 6;  if (b < 0) b = 0;
        vesa_hline(x, y + dy, w, (r << 16) | (g << 8) | b);
    }
    vesa_draw_rect(x, y, w, h, SEC_BORDER);
}

// ─── VESA drawing ────────────────────────────────────────────────
static void sec_vesa_draw(void) {
    int W = (int)vesa.width, H = (int)vesa.height;
    int cx = W / 10, cy = 10;
    int cw = W * 8 / 10, ch = H - 50;
    g_win_x = cx; g_win_y = cy; g_win_w = cw; g_win_h = ch;

    vesa_fill_rect(0, 0, W, H, SEC_BG);
    sec_panel(cx, cy, cw, ch, SEC_PANEL);

    // Title bar
    int th = 36;
    for (int dy = 0; dy < th; dy++) {
        int t2 = dy * 255 / th;
        int r = 0x2E + t2 / 10; if (r > 0x38) r = 0x38;
        int g = 0x20 + t2 / 12; if (g > 0x2A) g = 0x2A;
        int b = 0x50 + t2 / 4;  if (b > 0x6A) b = 0x6A;
        vesa_hline(cx + 1, cy + 1 + dy, cw - 2, (r << 16) | (g << 8) | b);
    }
    vesa_hline(cx + 1, cy + th, cw - 2, SEC_HEADING);
    vesa_draw_string(cx + 18, cy + 10, "Security Tools", 0xE8D8FF, 0);
    draw_close_btn(cx + cw - 32, cy + 10, 0, "Security");

    // ── Tab bar ───────────────────────────────────────────────────
    int tab_y = cy + th + 8, tab_h = 28, tab_x = cx + 12;
    for (int i = 0; i < SEC_TAB_COUNT; i++) {
        int tw = 120, tx = tab_x + i * (tw + 8);
        int tab_sel = (i == sec_tab);
        int tab_hover = !tab_sel && mouse_in_rect(tx, tab_y, tw, tab_h);
        uint32_t bg = tab_sel ? 0x2E2050 : (tab_hover ? 0x261848 : 0x1A1240);
        uint32_t fg = tab_sel ? 0xFFFFFF : (tab_hover ? 0xD8C8F0 : SEC_DIM);
        sec_panel(tx, tab_y, tw, tab_h, bg);

        // Protection status dot on File Scanner tab
        int lx = tx + 8;
        if (i == TAB_FILESCAN) {
            uint32_t pcol = security_protection_status() ? 0x44FF88 : 0xFF4455;
            vesa_fill_rect(tx + 6, tab_y + tab_h/2 - 3, 6, 6, pcol);
            lx = tx + 18;
        }

        vesa_draw_string(lx, tab_y + 6, sec_tab_names[i], fg, 0);
        if (tab_hover) vesa_hline(tx, tab_y + tab_h - 1, tw, 0x7A5FBF);
        if (tab_sel) vesa_hline(tx, tab_y + tab_h - 1, tw, SEC_HEADING);
    }

    int cont_x = cx + 16, cont_y = tab_y + tab_h + 12;
    int cont_w = cw - 32, cont_h = ch - th - tab_h - 60;
    int row_h = 24, vis_rows = cont_h / row_h;

    if (sec_tab == TAB_PORTSCAN) {
        vesa_draw_string(cont_x, cont_y, "Port", SEC_HEADING, 0);
        vesa_draw_string(cont_x + 60, cont_y, "Service", SEC_HEADING, 0);
        vesa_draw_string(cont_x + 220, cont_y, "Status", SEC_HEADING, 0);
        vesa_hline(cont_x, cont_y + row_h, cont_w, SEC_BORDER);
        cont_y += row_h + 6;

        int total = SCAN_PORT_COUNT;
        if (sec_scroll > total - vis_rows) sec_scroll = total - vis_rows;
        if (sec_scroll < 0) sec_scroll = 0;

        for (int i = sec_scroll; i < total && i < sec_scroll + vis_rows; i++) {
            int ry = cont_y + (i - sec_scroll) * row_h;
            int sel = (i == sec_sel);
            if (sel) vesa_fill_rect(cont_x - 4, ry - 2, cont_w + 8, row_h, 0x2A1C48);

            char port_str[6]; int pn = scan_ports[i];
            port_str[0]='0'+pn/1000; port_str[1]='0'+(pn/100)%10;
            port_str[2]='0'+(pn/10)%10; port_str[3]='0'+pn%10; port_str[4]='\0';
            char* ps = port_str; while(*ps=='0'&&*(ps+1))ps++;
            vesa_draw_string(cont_x, ry, ps, sel ? 0xFFFFFF : SEC_TEXT, 0);
            vesa_draw_string(cont_x + 60, ry, scan_port_names[i], sel ? 0xFFFFFF : SEC_TEXT, 0);

            const char* st; uint32_t sc;
            if (sec_results[i]==0) { st="---";       sc=SEC_DIM; }
            else if (sec_results[i]==1) { st="OPEN";    sc=SEC_GREEN; }
            else if (sec_results[i]==2) { st="closed";  sc=SEC_DIM; }
            else { st="filtered"; sc=SEC_ORANGE; }
            vesa_draw_string(cont_x + 220, ry, st, sc, 0);

            if (sec_scanning && i == sec_sel) {
                const char* sp = "-\\|/"; char sc2[2] = {sp[(sec_scan_prog/3)%4], '\0'};
                vesa_draw_string(cont_x + 300, ry, sc2, SEC_ORANGE, 0);
            }
        }
        vesa_draw_string(cont_x, cont_y + vis_rows * row_h + 8,
            "ENTER: scan port  |  S: scan all  |  UP/DOWN: navigate  |  TAB: next tab", SEC_HINT, 0);

    } else if (sec_tab == TAB_NETSTAT) {
        vesa_draw_string(cont_x, cont_y, "Network Interfaces", SEC_HEADING, 0); cont_y += row_h + 4;
        const char* lines[] = {
            "Interface    Type        MAC              Status",
"-------------------------------------------------",
            "eth0         Ethernet    A0:B1:C2:D3:E4:F5  Connected",
            "             RX: 1,247 packets (142 KB)",
            "             TX:   892 packets  (98 KB)",
            "",
            "lo           Loopback   00:00:00:00:00:00  Running",
            "             RX: 0 packets (0 B)",
            "             TX: 0 packets (0 B)",
            "",
            "Active Connections:",
            "  No active outbound connections.",
        };
        int lc = sizeof(lines)/sizeof(lines[0]);
        for (int i=0; i<lc && i<vis_rows+2; i++) {
            uint32_t col = SEC_TEXT;
            if (i==0||i==10) col = SEC_HEADING;
            else if (i==1) col = SEC_DIM;
            vesa_draw_string(cont_x, cont_y + i*18, lines[i], col, 0);
        }

    } else if (sec_tab == TAB_SECURITY) {
        vesa_draw_string(cont_x, cont_y, "System Security Audit", SEC_HEADING, 0);
        cont_y += row_h + 4;

        if (!sec_audit_done) {
            vesa_draw_string(cont_x, cont_y + 10, "Press ENTER to run security audit...", SEC_HINT, 0);
        } else {
            struct { const char* c; int p; } audits[] = {
                {"User accounts: all have passwords set",       1},
                {"Lock screen: enabled",                        1},
                {"Login attempts: limited",                     1},
                {"Filesystem: user data protected",             1},
                {"Network: no public shares detected",          1},
                {"System: kernel integrity verified",           1},
                {"Audio: beep sounds disabled",                 1},
                {"Session: active user logged in",              1},
            };
            int ac = sizeof(audits)/sizeof(audits[0]), sc = 0;
            for (int i=0; i<ac && i<vis_rows; i++) {
                int ry=cont_y+12+i*row_h;
                vesa_draw_string(cont_x,ry,audits[i].p?"[OK]":"[!!]",audits[i].p?SEC_GREEN:SEC_RED,0);
                vesa_draw_string(cont_x+50,ry,audits[i].c,audits[i].p?SEC_TEXT:SEC_RED,0);
                if(audits[i].p)sc++;
            }
            char ss[12]; ss[0]='S';ss[1]='c';ss[2]='o';ss[3]='r';ss[4]='e';ss[5]=':';ss[6]=' ';
            ss[7]='0'+sc; ss[8]='/'; ss[9]='0'+ac; ss[10]='\0';
            vesa_draw_string(cont_x+cont_w-120, cont_y-2, ss,
                sc==ac?SEC_GREEN:(sc>=ac/2?SEC_ORANGE:SEC_RED), 0);
        }
        vesa_draw_string(cont_x, cont_y+vis_rows*row_h+14,
            "ENTER: run audit  |  TAB: next tab  |  ESC: exit", SEC_HINT, 0);

    } else if (sec_tab == TAB_VAULT) {
        // ── Vault tab ────────────────────────────────────────────────
        // ── Vault Setup (first time) ─────────────────────────────────
        if (vault_state == VAULT_SETUP) {
            // Draw lock icon (simple shield)
            uint32_t lock_col = 0x6A4FBF;
            int lcx = cont_x + cont_w/2;
            int lcy = cont_y + 10;
            for (int dy = -10; dy <= 10; dy++)
                for (int dx = -12; dx <= 12; dx++) {
                    int d = dx*dx + dy*dy;
                    if (d <= 110 && d >= 64) vesa_put_pixel(lcx+dx, lcy+dy, lock_col);
                }
            vesa_draw_string(cont_x + cont_w/2 - 70, cont_y + 6, "SET MASTER PASSWORD", SEC_HEADING, 0);

            // Password guide
            vesa_draw_string(cont_x + 20, cont_y + 44,
                "Choose a strong master password to protect your vault.", SEC_HINT, 0);
            vesa_draw_string(cont_x + 20, cont_y + 58,
                "This password cannot be recovered if forgotten.", 0xFF6644, 0);

            int field_y = cont_y + 80;
            int field_x = cont_x + 20;

            sec_panel(field_x - 4, field_y - 4, cont_w - 32, 44, 0x0C0820);
            vesa_draw_string(field_x, field_y,
                vault_setup_step == 0 ? "New Master Password:" : "Confirm Password:", SEC_TEXT, 0);

            // Input box
            char input_display[VAULT_FIELD_LEN];
            for (int i = 0; i < vault_input_pos && i < VAULT_FIELD_LEN - 1; i++)
                input_display[i] = '*';  // always mask
            input_display[vault_input_pos < VAULT_FIELD_LEN - 1 ? vault_input_pos : VAULT_FIELD_LEN - 1] = '\0';
            vesa_fill_rect(field_x, field_y + 18, cont_w - 40, 20, 0x1A1240);
            vesa_draw_rect(field_x, field_y + 18, cont_w - 40, 20, SEC_BORDER);
            vesa_draw_string(field_x + 4, field_y + 20, input_display, 0xFFFFFF, 0);
            // Cursor
            int cpos = (vault_input_pos < VAULT_FIELD_LEN - 1 ? vault_input_pos : 0);
            if ((tick / 8) % 2 == 0)
                vesa_draw_string(field_x + 4 + cpos * 8, field_y + 20, "_", 0xB088FF, 0);

            if (vault_setup_step == 1) {
                // Show whether passwords match
                int match = 1;
                if (vault_input_pos != vault_confirm_pos) match = 0;
                else {
                    for (int i = 0; i < vault_input_pos; i++)
                        if (vault_input[i] != vault_confirm[i]) { match = 0; break; }
                }
                vesa_draw_string(field_x + cont_w - 160, field_y + 2,
                    match ? "[PASSWORDS MATCH]" : "[PASSWORDS DIFFER]",
                    match ? SEC_GREEN : SEC_RED, 0);
            }

            vesa_draw_string(cont_x + 20, cont_y + 140,
                "ENTER: confirm  |  TAB: back to tabs  |  ESC: cancel", SEC_HINT, 0);

        // ── Vault Locked ─────────────────────────────────────────────
        } else if (vault_state == VAULT_LOCKED) {
            // Lock icon
            uint32_t lock_col = 0xFF4455;
            int lcx = cont_x + cont_w/2;
            int lcy = cont_y + 30;
            for (int dy = -14; dy <= 10; dy++)
                for (int dx = -12; dx <= 12; dx++) {
                    int d = dx*dx + dy*dy;
                    if (d <= 130 && d >= 90) vesa_put_pixel(lcx+dx, lcy+dy, lock_col);
                }
            // keyhole
            vesa_fill_rect(lcx - 2, lcy + 2, 4, 6, 0x0E0824);

            vesa_draw_string(cont_x + cont_w/2 - 80, cont_y + 6, "VAULT LOCKED", SEC_HEADING, 0);
            vesa_draw_string(cont_x + cont_w/2 - 100, cont_y + 60,
                "Enter master password to unlock your vault.", SEC_TEXT, 0);

            int field_x = cont_x + 60;
            int field_y = cont_y + 90;
            vesa_draw_string(field_x, field_y, "Master Password:", SEC_TEXT, 0);

            char input_display[VAULT_FIELD_LEN];
            for (int i = 0; i < vault_input_pos && i < VAULT_FIELD_LEN - 1; i++)
                input_display[i] = '*';
            input_display[vault_input_pos < VAULT_FIELD_LEN - 1 ? vault_input_pos : VAULT_FIELD_LEN - 1] = '\0';
            vesa_fill_rect(field_x, field_y + 18, cont_w - 120, 20, 0x1A1240);
            vesa_draw_rect(field_x, field_y + 18, cont_w - 120, 20, SEC_BORDER);
            vesa_draw_string(field_x + 4, field_y + 20, input_display, 0xFFFFFF, 0);
            if ((tick / 8) % 2 == 0) {
                int cp = vault_input_pos < VAULT_FIELD_LEN - 1 ? vault_input_pos : 0;
                vesa_draw_string(field_x + 4 + cp * 8, field_y + 20, "_", 0xB088FF, 0);
            }

            vesa_draw_string(cont_x + 20, cont_y + 140,
                "ENTER: unlock  |  TAB: back to tabs", SEC_HINT, 0);

        // ── Vault Unlocked — Entry List ─────────────────────────────
        } else if (vault_state == VAULT_UNLOCKED) {
            // Header row
            int hdr_y = cont_y;
            vesa_draw_string(cont_x, hdr_y, "Saved Credentials", SEC_HEADING, 0);
            vesa_draw_string(cont_x + cont_w - 100, hdr_y,
                vault_count > 0 ? "[A] Add" : "[A] Add New", SEC_GREEN, 0);
            vesa_hline(cont_x, hdr_y + row_h, cont_w, SEC_BORDER);

            int list_y = hdr_y + row_h + 6;
            int entry_h = 28;
            int vis_entries = (cont_h - row_h - 40) / entry_h;
            if (vis_entries < 1) vis_entries = 1;

            if (vault_count == 0) {
                vesa_draw_string(cont_x + 40, list_y + 20,
                    "No saved credentials. Press A to add an entry.", SEC_HINT, 0);
            } else {
                if (vault_list_scroll > vault_count - vis_entries)
                    vault_list_scroll = vault_count - vis_entries;
                if (vault_list_scroll < 0) vault_list_scroll = 0;

                for (int i = vault_list_scroll; i < vault_count && i < vault_list_scroll + vis_entries; i++) {
                    int ey = list_y + (i - vault_list_scroll) * entry_h;
                    int sel = (i == vault_list_sel);

                    if (sel) vesa_fill_rect(cont_x - 4, ey - 2, cont_w + 8, entry_h, 0x2A1C48);

                    // Website icon — small colored dot
                    uint32_t dot_colors[] = {0x6A4FBF, 0x55FF88, 0xFFAA44, 0xFF5566, 0x55AAFF, 0xFF66AA};
                    icon_circ(cont_x + 10, ey + entry_h/2, 4, dot_colors[i % 6]);

                    char tmp_web[VAULT_FIELD_LEN], tmp_user[VAULT_FIELD_LEN];
                    vault_decrypt_field(vault_entries[i].website, tmp_web, VAULT_FIELD_LEN);
                    vault_decrypt_field(vault_entries[i].username, tmp_user, VAULT_FIELD_LEN);
                    vesa_draw_string(cont_x + 24, ey + 2,
                        tmp_web, sel ? 0xFFFFFF : SEC_TEXT, 0);
                    vesa_draw_string(cont_x + 24, ey + 14,
                        tmp_user, sel ? 0xFFFFFF : SEC_DIM, 0);

                    vesa_draw_string(cont_x + cont_w - 80, ey + 4,
                        "[DEL]", 0xFF5566, 0);
                }
            }

            vesa_draw_string(cont_x, cont_y + vis_entries * entry_h + row_h + 14,
                "ENTER: view  |  A: add  |  P: toggle show/hide  |  UP/DOWN: navigate  |  TAB: back",
                SEC_HINT, 0);

        // ── Vault Viewing Entry Detail ────────────────────────────
        } else if (vault_state == VAULT_VIEWING) {
            if (vault_view_idx >= 0 && vault_view_idx < vault_count) {
                VaultEntry* ve = &vault_entries[vault_view_idx];
                char tmp_web[VAULT_FIELD_LEN], tmp_user[VAULT_FIELD_LEN], tmp_pass[VAULT_FIELD_LEN];
                vault_decrypt_field(ve->website, tmp_web, VAULT_FIELD_LEN);
                vault_decrypt_field(ve->username, tmp_user, VAULT_FIELD_LEN);
                vault_decrypt_field(ve->password, tmp_pass, VAULT_FIELD_LEN);

                vesa_draw_string(cont_x, cont_y, "Entry Details", SEC_HEADING, 0);
                vesa_hline(cont_x, cont_y + row_h, cont_w, SEC_BORDER);

                int fy = cont_y + row_h + 20;
                int fw = cont_w;

                // Website field
                vesa_draw_string(cont_x, fy, "Website", SEC_DIM, 0);
                vesa_fill_rect(cont_x, fy + 18, fw, 22, 0x1A1240);
                vesa_draw_rect(cont_x, fy + 18, fw, 22, SEC_BORDER);
                vesa_draw_string(cont_x + 8, fy + 22, tmp_web, 0xFFFFFF, 0);

                // Username field
                fy += 52;
                vesa_draw_string(cont_x, fy, "Username", SEC_DIM, 0);
                vesa_fill_rect(cont_x, fy + 18, fw, 22, 0x1A1240);
                vesa_draw_rect(cont_x, fy + 18, fw, 22, SEC_BORDER);
                vesa_draw_string(cont_x + 8, fy + 22, tmp_user, 0xFFFFFF, 0);

                // Password field
                fy += 52;
                vesa_draw_string(cont_x, fy, "Password", SEC_DIM, 0);
                vesa_fill_rect(cont_x, fy + 18, fw, 22, 0x1A1240);
                vesa_draw_rect(cont_x, fy + 18, fw, 22, SEC_BORDER);

                char pbuf[VAULT_FIELD_LEN];
                if (vault_show_pass) {
                    for (int i = 0; tmp_pass[i] && i < VAULT_FIELD_LEN - 1; i++)
                        pbuf[i] = tmp_pass[i];
                    pbuf[VAULT_FIELD_LEN - 1] = '\0';
                } else {
                    for (int i = 0; tmp_pass[i] && i < VAULT_FIELD_LEN - 1; i++)
                        pbuf[i] = '*';
                    pbuf[VAULT_FIELD_LEN - 1] = '\0';
                }
                vesa_draw_string(cont_x + 8, fy + 22, pbuf, vault_show_pass ? 0xFFFFFF : 0x6A5890, 0);
                if (!vault_show_pass) {
                    vesa_draw_string(cont_x + fw - 60, fy + 22, "[P]", SEC_HINT, 0);
                }

                vesa_draw_string(cont_x, fy + 56,
                    "P: toggle show/hide  |  DEL: delete  |  ESC: back to list", SEC_HINT, 0);
            }

        // ── Vault Adding New Entry ───────────────────────────────
        } else if (vault_state == VAULT_ADDING) {
            vesa_draw_string(cont_x, cont_y, "Add New Entry", SEC_HEADING, 0);
            vesa_hline(cont_x, cont_y + row_h, cont_w, SEC_BORDER);

            int fy = cont_y + row_h + 16;
            int fw = cont_w;
            const char* labels[] = {"Website", "Username", "Password"};
            const char* bufs[] = {vault_new_website, vault_new_username, vault_new_password};

            for (int f = 0; f < 3; f++) {
                int sel = (f == vault_new_field);
                vesa_draw_string(cont_x, fy, labels[f], sel ? SEC_HEADING : SEC_DIM, 0);

                vesa_fill_rect(cont_x, fy + 18, fw, 22, sel ? 0x2A1C48 : 0x1A1240);
                vesa_draw_rect(cont_x, fy + 18, fw, 22,
                    sel ? 0xB088FF : SEC_BORDER);

                char display[VAULT_FIELD_LEN];
                int len = 0;
                for (; bufs[f][len] && len < VAULT_FIELD_LEN - 1; len++);

                if (f == 2 && !vault_show_pass) {
                    for (int i = 0; i < len; i++) display[i] = '*';
                    display[len] = '\0';
                } else {
                    for (int i = 0; i < len; i++) display[i] = bufs[f][i];
                    display[len] = '\0';
                }

                uint32_t text_col = (sel && f == 2 && !vault_show_pass) ? 0xFFFFFF : (sel ? 0xFFFFFF : SEC_TEXT);
                vesa_draw_string(cont_x + 8, fy + 22, display, text_col, 0);

                // Cursor in active field
                if (sel && (tick / 8) % 2 == 0) {
                    int cp = 8 + (len < VAULT_FIELD_LEN - 1 ? len : 0) * 8;
                    vesa_draw_string(cont_x + cp, fy + 22, "_", 0xB088FF, 0);
                }

                fy += 50;
            }

            // Password strength meter (shown while editing password field)
            if (vault_new_field == 2) {
                int pw_len = 0;
                while (vault_new_password[pw_len]) pw_len++;

                int score = 0;
                if (pw_len >= 6) score++;
                if (pw_len >= 10) score++;
                int has_upper = 0, has_lower = 0, has_digit = 0, has_special = 0;
                for (int i = 0; i < pw_len; i++) {
                    char ch = vault_new_password[i];
                    if (ch >= 'A' && ch <= 'Z') has_upper = 1;
                    else if (ch >= 'a' && ch <= 'z') has_lower = 1;
                    else if (ch >= '0' && ch <= '9') has_digit = 1;
                    else has_special = 1;
                }
                if (has_upper && has_lower) score++;
                if (has_digit) score++;
                if (has_special) score++;
                if (pw_len >= 14) score++;

                if (score > 5) score = 5;
                const char* labels_s[] = {"Weak", "Fair", "Good", "Strong", "Very Strong"};
                uint32_t colors_s[] = {0xFF4455, 0xFF8844, 0xFFCC44, 0x88FF44, 0x44FF88};
                int bar_w = fw;
                int filled_w = bar_w * score / 5;

                // Bar background
                int bar_y = fy + 4;
                vesa_fill_rect(cont_x, bar_y, bar_w, 8, 0x0C0820);
                vesa_draw_rect(cont_x, bar_y, bar_w, 8, SEC_BORDER);
                // Filled portion
                if (score > 0) {
                    vesa_fill_rect(cont_x + 1, bar_y + 1, filled_w - 2, 6, colors_s[score-1]);
                }

                const char* sl = score > 0 ? labels_s[score-1] : "";
                vesa_draw_string(cont_x + bar_w + 10, bar_y - 1,
                    sl, score > 0 ? colors_s[score-1] : SEC_DIM, 0);
                fy += 16;
            }

            vesa_draw_string(cont_x, fy + 8,
                "ENTER: next field / save  |  TAB: next field  |  ESC: discard & back",
                SEC_HINT, 0);
        }

    // ── File Scanner Tab ───────────────────────────────────────────
    } else if (sec_tab == TAB_FILESCAN) {
        vesa_draw_string(cont_x, cont_y, "File Malware Scanner", SEC_HEADING, 0);
        vesa_draw_string(cont_x + cont_w - 120, cont_y,
            scan_done ? "[S] Rescan" : "[S] Scan All", SEC_GREEN, 0);
        vesa_hline(cont_x, cont_y + row_h, cont_w, SEC_BORDER);

        if (scan_view_idx >= 0 && scan_view_idx < MAX_FILES && filesystem[scan_view_idx].used) {
            // ── Detail view for a scanned file ───────────────────
            File* f = &filesystem[scan_view_idx];
            int sc = scan_scores[scan_view_idx];
            int flags = scan_results[scan_view_idx];

            vesa_draw_string(cont_x, cont_y + row_h + 6, "Scan Detail", SEC_HEADING, 0);

            int fy = cont_y + row_h + 30;
            char size_str[16]; int sn = 0;
            int cl = 0; while (f->content[cl]) cl++;
            if (cl > 999) { size_str[sn++]='0'+(cl/1000); size_str[sn++]='K'; size_str[sn]='\0'; }
            else { if (cl>99) size_str[sn++]='0'+(cl/100); if(cl>9)size_str[sn++]='0'+((cl/10)%10); size_str[sn++]='0'+(cl%10); size_str[sn++]='B'; size_str[sn]='\0'; }

            // Threat level indicator
            uint32_t threat_col;
            const char* threat_lbl;
            if (sc <= 2) { threat_col = SEC_GREEN; threat_lbl = "SAFE"; }
            else if (sc <= 5) { threat_col = SEC_ORANGE; threat_lbl = "SUSPICIOUS"; }
            else if (sc <= 8) { threat_col = 0xFF6644; threat_lbl = "RISKY"; }
            else { threat_col = SEC_RED; threat_lbl = "MALICIOUS"; }

            vesa_draw_string(cont_x, fy, "File:", SEC_DIM, 0);
            vesa_draw_string(cont_x + 50, fy, f->name, 0xFFFFFF, 0);
            fy += 22;
            vesa_draw_string(cont_x, fy, "Size:", SEC_DIM, 0);
            vesa_draw_string(cont_x + 50, fy, size_str, 0xFFFFFF, 0);
            fy += 22;
            vesa_draw_string(cont_x, fy, "Threat:", SEC_DIM, 0);
            vesa_draw_string(cont_x + 60, fy, threat_lbl, threat_col, 0);
            // Score bar
            int bar_x = cont_x + 130, bar_y = fy + 4;
            vesa_fill_rect(bar_x, bar_y, 100, 10, 0x0C0820);
            vesa_draw_rect(bar_x, bar_y, 100, 10, SEC_BORDER);
            vesa_fill_rect(bar_x + 1, bar_y + 1, sc * 10, 8, threat_col);
            fy += 30;

            vesa_draw_string(cont_x, fy, "Flags Detected:", SEC_HEADING, 0);
            fy += 22;
            int found_any = 0;
            for (int fi = 0; fi < 6; fi++) {
                if (flags & (1 << fi)) {
                    found_any = 1;
                    vesa_draw_string(cont_x + 10, fy, "[!]", SEC_RED, 0);
                    vesa_draw_string(cont_x + 30, fy, scan_flag_names[fi], SEC_TEXT, 0);
                    fy += 20;
                }
            }
            if (!found_any) {
                vesa_draw_string(cont_x + 10, fy, "No suspicious flags detected.", SEC_GREEN, 0);
            }

            vesa_draw_string(cont_x, cont_y + cont_h - 16,
                "ESC: back to list  |  S: rescan", SEC_HINT, 0);

        } else {
            // ── File list ────────────────────────────────────────
            int list_y = cont_y + row_h + 6;
            int entry_h = 26;
            int vis_files = (cont_h - row_h - 20) / entry_h;
            if (vis_files < 1) vis_files = 1;

            // Find used files first
            int used_indices[MAX_FILES];
            int used_count = 0;
            for (int i = 0; i < MAX_FILES; i++) {
                if (filesystem[i].used) {
                    used_indices[used_count++] = i;
                }
            }

            if (used_count == 0) {
                vesa_draw_string(cont_x + 40, list_y + 20,
                    "No files in the filesystem. Create files first.", SEC_HINT, 0);
            } else {
                if (scan_scroll > used_count - vis_files)
                    scan_scroll = used_count - vis_files;
                if (scan_scroll < 0) scan_scroll = 0;

                // Column headers
                vesa_draw_string(cont_x, list_y - 20, "File", SEC_DIM, 0);
                vesa_draw_string(cont_x + 160, list_y - 20, "Size", SEC_DIM, 0);
                vesa_draw_string(cont_x + 220, list_y - 20, "Threat", SEC_DIM, 0);
                vesa_draw_string(cont_x + 300, list_y - 20, "Status", SEC_DIM, 0);

                for (int i = scan_scroll; i < used_count && i < scan_scroll + vis_files; i++) {
                    int fi = used_indices[i];
                    int ey = list_y + (i - scan_scroll) * entry_h;
                    int sel = (i == scan_sel);

                    if (sel) vesa_fill_rect(cont_x - 4, ey - 2, cont_w + 8, entry_h, 0x2A1C48);

                    // File icon — small dot
                    uint32_t dot_col = 0x6A4FBF;
                    if (scan_done) {
                        int sc = scan_scores[fi];
                        if (sc <= 2) dot_col = SEC_GREEN;
                        else if (sc <= 5) dot_col = SEC_ORANGE;
                        else if (sc <= 8) dot_col = 0xFF6644;
                        else dot_col = SEC_RED;
                    }
                    icon_circ(cont_x + 8, ey + entry_h/2, 3, dot_col);

                    vesa_draw_string(cont_x + 18, ey + 4,
                        filesystem[fi].name, sel ? 0xFFFFFF : SEC_TEXT, 0);

                    // Size
                    char sz[8]; int sp = 0;
                    int cl = 0; while (filesystem[fi].content[cl]) cl++;
                    if (cl > 999) { sz[sp++]='0'+(cl/1000); sz[sp++]='K'; sz[sp]='\0'; }
                    else {
                        if (cl>99) sz[sp++]='0'+(cl/100);
                        if (cl>9) sz[sp++]='0'+((cl/10)%10);
                        sz[sp++]='0'+(cl%10); sz[sp++]='B'; sz[sp]='\0';
                    }
                    vesa_draw_string(cont_x + 160, ey + 4, sz, SEC_DIM, 0);

                    if (scan_done) {
                        int sc = scan_scores[fi];
                        const char* lbl;
                        uint32_t col;
                        if (sc <= 2) { lbl = "Safe"; col = SEC_GREEN; }
                        else if (sc <= 5) { lbl = "Suspicious"; col = SEC_ORANGE; }
                        else if (sc <= 8) { lbl = "Risky"; col = 0xFF6644; }
                        else { lbl = "MALICIOUS"; col = SEC_RED; }

                        char sc_str[4];
                        sc_str[0] = '0' + sc; sc_str[1] = '/'; sc_str[2] = '1'; sc_str[3] = '0';
                        vesa_draw_string(cont_x + 220, ey + 4, sc_str, col, 0);
                        vesa_draw_string(cont_x + 260, ey + 4, lbl, col, 0);

                        if (sel) {
                            vesa_draw_string(cont_x + cont_w - 70, ey + 4,
                                "[VIEW]", SEC_HINT, 0);
                        }
                    } else {
                        vesa_draw_string(cont_x + 220, ey + 4, "---", SEC_DIM, 0);
                        vesa_draw_string(cont_x + 260, ey + 4, "Not scanned", SEC_HINT, 0);
                    }
                }
            }

            vesa_draw_string(cont_x, cont_y + vis_files * entry_h + row_h + 16,
                "ENTER: scan  |  S: scan all  |  UP/DOWN: navigate  |  VIEW: enter for details  |  TAB: switch tabs",
                SEC_HINT, 0);
        }
    }
}

// ─── VGA text fallback (direct VGA memory, no dependency on desktop.c) ─
static void sec_vga_draw(void) {
    vgafill(2,2,76,20,' ',7,1);
    vgarow(2,' ',15,4);
    vgaputs(3,2,"[ Security Tools ]",15,4);
    vgaputs(70,2,"[X]",12,4);

    if (sec_tab == TAB_PORTSCAN) {
        vgaputs(3,4,"Port Scanner",14,1);
        vgaputs(3,5,"Port  Service  Status",7,1);
        int row=6;
        for(int i=0;i<SCAN_PORT_COUNT&&row<21;i++){
            char b[32]; int p=0; int pn=scan_ports[i];
            b[p++]='0'+pn/1000; b[p++]='0'+(pn/100)%10;
            b[p++]='0'+(pn/10)%10; b[p++]='0'+pn%10;
            b[p++]=' ';b[p++]=' ';
            int k=0; while(scan_port_names[i][k]&&k<10){b[p++]=scan_port_names[i][k++];}
            while(k<10){b[p++]=' ';k++;}
            b[p++]=' ';b[p++]=' ';
            if(sec_results[i]==0){b[p++]='-';b[p++]='-';b[p++]='-';}
            else if(sec_results[i]==1){b[p++]='O';b[p++]='P';b[p++]='E';b[p++]='N';}
            else if(sec_results[i]==2){b[p++]='c';b[p++]='l';b[p++]='o';b[p++]='s';b[p++]='e';b[p++]='d';}
            else{b[p++]='f';b[p++]='i';b[p++]='l';b[p++]='t';}
            b[p]='\0';
            vgaputs(3,row,b,i==sec_sel?15:7,i==sec_sel?4:1);
            row++;
        }
        vgaputs(3,22,"ENTER:scan  S:scan all  UP/DOWN:nav  TAB:next",7,1);
    } else if (sec_tab == TAB_NETSTAT) {
        vgaputs(3,4,"eth0: Connected  IP: 10.0.2.15",10,1);
        vgaputs(3,6,"RX: 1,247  TX: 892 (packets)",7,1);
        vgaputs(3,8,"lo: Loopback running",7,1);
        vgaputs(3,10,"No active connections",8,1);
        vgaputs(3,22,"TAB:next tab  ESC:exit",7,1);
    } else if (sec_tab == TAB_SECURITY) {
        if(!sec_audit_done){vgaputs(3,4,"Press ENTER to run security audit",14,1);}
        else{for(int i=0;i<8&&i<16;i++)vgaputs(3,4+i,((const char*[]){
            "[OK] Passwords set","[OK] Lock screen","[OK] Login limits",
            "[OK] Filesystem OK","[OK] Network secure","[OK] Kernel clean",
            "[OK] Beeps off","[OK] User logged in"})[i],10,1);}
        vgaputs(3,22,"ENTER:run audit  TAB:next  ESC:exit",7,1);
    } else if (sec_tab == TAB_VAULT) {
        if (vault_state == VAULT_SETUP) {
            vgaputs(3,4,"Set Master Password",14,1);
            if (vault_setup_step == 0) {
                vgaputs(3,6,"Enter master password: ",15,1);
                for(int i=0;i<vault_input_pos;i++)vgaput(27+i,6,'*',15,1);
                vgaputs(3,22,"ENTER:confirm  ESC:cancel  TAB:back",7,1);
            } else {
                vgaputs(3,6,"Confirm master password: ",15,1);
                for(int i=0;i<vault_confirm_pos;i++)vgaput(26+i,6,'*',15,1);
                vgaputs(3,22,"ENTER:confirm  ESC:cancel  TAB:back",7,1);
            }
        } else if (vault_state == VAULT_LOCKED) {
            vgaputs(3,4,"Vault Locked - Enter Master Password",14,1);
            vgaputs(3,6,"Password: ",15,1);
            for(int i=0;i<vault_input_pos;i++)vgaput(10+i,6,'*',15,1);
            vgaputs(3,22,"ENTER:unlock  TAB:back to tabs",7,1);
        } else if (vault_state == VAULT_UNLOCKED) {
            vgaputs(3,4,"Saved Passwords",14,1);
            vgaputs(3,5,"  Website         Username",7,1);
            int row=6;
            for(int i=vault_list_scroll;i<vault_count&&row<20;i++){
                char tmp_w[VAULT_FIELD_LEN], tmp_u[VAULT_FIELD_LEN];
                vault_decrypt_field(vault_entries[i].website, tmp_w, VAULT_FIELD_LEN);
                vault_decrypt_field(vault_entries[i].username, tmp_u, VAULT_FIELD_LEN);
                char b[60]; int p=0;
                const char*w=tmp_w;
                for(int k=0;w[k]&&k<16;k++)b[p++]=w[k];
                while(p<18)b[p++]=' ';
                const char*u=tmp_u;
                for(int k=0;u[k]&&k<16;k++)b[p++]=u[k];
                b[p]='\0';
                vgaputs(3,row,b,i==vault_list_sel?15:7,i==vault_list_sel?4:1);
                row++;
            }
            vgaputs(3,22,"A:add  ENTER:view  DEL:delete  TAB:back",7,1);
        } else if (vault_state == VAULT_VIEWING) {
            if(vault_view_idx>=0&&vault_view_idx<vault_count){
                char tmp_w[VAULT_FIELD_LEN], tmp_u[VAULT_FIELD_LEN], tmp_p[VAULT_FIELD_LEN];
                vault_decrypt_field(vault_entries[vault_view_idx].website, tmp_w, VAULT_FIELD_LEN);
                vault_decrypt_field(vault_entries[vault_view_idx].username, tmp_u, VAULT_FIELD_LEN);
                vault_decrypt_field(vault_entries[vault_view_idx].password, tmp_p, VAULT_FIELD_LEN);
                vgaputs(3,4,"Entry Details",14,1);
                vgaputs(3,6,"Website:  ",15,1);vgaputs(15,6,tmp_w,7,1);
                vgaputs(3,8,"Username: ",15,1);vgaputs(14,8,tmp_u,7,1);
                vgaputs(3,10,"Password: ",15,1);
                const char* pw = tmp_p;
                if(vault_show_pass){
                    vgaputs(14,10,pw,7,1);
                }else{
                    for(int i=0;pw[i];i++)vgaput(14+i,10,'*',7,1);
                }
                vgaputs(3,22,"P:toggle  DEL:delete  ESC:back",7,1);
            }
        } else if (vault_state == VAULT_ADDING) {
            vgaputs(3,4,"Add New Entry",14,1);
            const char* labels[] = {"Website: ","Username:","Password:"};
            const char* bufs[] = {vault_new_website,vault_new_username,vault_new_password};
            int pos = vault_new_pos;
            for(int f=0;f<3;f++){
                int y = 6 + f*2;
                int sel = (f==vault_new_field);
                vgaputs(3,y,labels[f],sel?15:7,1);
                int xs = (f==0)?12:(f==1)?11:11;
                for(int k=0;k<(int)sizeof(VAULT_FIELD_LEN)-1;k++){
                    char ch = bufs[f][k];
                    if(ch=='\0'){
                        if(sel&&k==pos)vgaput(xs+k,y,'_',15,7);
                        break;
                    }
                    if(f==2&&!vault_show_pass){
                        vgaput(xs+k,y,'*',sel?15:7,1);
                    }else{
                        vgaput(xs+k,y,ch,sel?15:7,1);
                    }
                }
            }
            vgaputs(3,22,"ENTER:next  TAB:next field  ESC:back",7,1);
        }
    } else if (sec_tab == TAB_FILESCAN) {
        vgaputs(3,4,"File Malware Scanner",14,1);
        if (scan_view_idx >= 0 && scan_view_idx < MAX_FILES && filesystem[scan_view_idx].used) {
            File* f = &filesystem[scan_view_idx];
            int sc = scan_scores[scan_view_idx];
            vgaputs(3,6,"File: ",15,1); vgaputs(9,6,f->name,7,1);
            int cl = 0; while (f->content[cl]) cl++;
            char sz[16]; int sp=0;
            if(cl>999){sz[sp++]='0'+(cl/1000);sz[sp++]='K';}else{
                if(cl>99)sz[sp++]='0'+(cl/100);
                if(cl>9)sz[sp++]='0'+((cl/10)%10);
                sz[sp++]='0'+(cl%10);sz[sp++]='B';
            }
            sz[sp]='\0';
            vgaputs(3,8,"Size: ",15,1); vgaputs(9,8,sz,7,1);
            vgaputs(3,10,"Score: ",15,1);
            char scs[4]; scs[0]='0'+sc; scs[1]='/'; scs[2]='1'; scs[3]='0';
            vgaputs(10,10,scs,sc<=2?10:(sc<=5?14:12),1);
            int flags = scan_results[scan_view_idx];
            int row = 12;
            for (int fi = 0; fi < 6 && row < 20; fi++) {
                if (flags & (1 << fi)) {
                    vgaputs(3,row,scan_flag_names[fi],12,1);
                    row++;
                }
            }
            if (sc == 0 && flags == 0) { vgaputs(3,12,"No threats found.",10,1); }
            vgaputs(3,22,"ESC:back  S:rescan",7,1);
        } else {
            int used_indices[MAX_FILES];
            int used_count = 0;
            for (int i = 0; i < MAX_FILES; i++) {
                if (filesystem[i].used) used_indices[used_count++] = i;
            }
            vgaputs(3,5,"File            Size  Status",7,1);
            if (used_count == 0) {
                vgaputs(3,7,"No files in filesystem.",8,1);
            } else {
                int row = 6;
                for (int i = scan_scroll; i < used_count && row < 20; i++) {
                    int fi = used_indices[i];
                    char b[60]; int p = 0;
                    const char*n = filesystem[fi].name;
                    for (int k = 0; n[k] && k < 14; k++) b[p++] = n[k];
                    while (p < 16) b[p++] = ' ';
                    int cl = 0; while (filesystem[fi].content[cl]) cl++;
                    if (cl > 99) { b[p++] = '0' + (cl/100); }
                    if (cl > 9) { b[p++] = '0' + ((cl/10)%10); }
                    b[p++] = '0' + (cl%10); b[p++] = 'B'; b[p++] = ' '; b[p++] = ' ';
                    if (scan_done) {
                        int sc = scan_scores[fi];
                        const char* lbl = sc <= 2 ? "Safe" : (sc <= 5 ? "Susp" : (sc <= 8 ? "Risk" : "MAL!"));
                        for (int k = 0; lbl[k] && k < 6; k++) b[p++] = lbl[k];
                    } else {
                        b[p++] = '-'; b[p++] = '-'; b[p++] = '-';
                    }
                    b[p] = '\0';
                    vgaputs(3,row,b,i==scan_sel?15:7,i==scan_sel?4:1);
                    row++;
                }
            }
            vgaputs(3,22,"ENTER:scan  S:scan all  UP/DOWN:nav  VIEW:enter  TAB:next",7,1);
        }
    }
}

// ─── Public: scan a single file and alert on threat ─────────────
int security_scan_file_at(int file_idx) {
    if (file_idx < 0 || file_idx >= MAX_FILES || !filesystem[file_idx].used) return 0;

    // Honour the real-time protection toggle
    if (!scan_protection_on) return 0;

    scan_file(file_idx);
    scan_done = 1;
    int score = scan_scores[file_idx];
    const char* name = filesystem[file_idx].name;

    if (score >= 6) {
        // Alert: risky or malicious file detected
        char alert_msg[64];
        int i = 0;
        const char* prefix = "[!] Threat detected: ";
        for (int k = 0; prefix[k] && i < 63; k++) alert_msg[i++] = prefix[k];
        for (int k = 0; name[k] && i < 63; k++) alert_msg[i++] = name[k];
        alert_msg[i] = '\0';
        desktop_set_status(alert_msg);
        scan_log_add(name, score);
        if (score >= 9) {
            desktop_vfx_flash(0xFF, 0x44, 0x55, 8);  // red flash for MALICIOUS
        } else {
            desktop_vfx_flash(0xFF, 0xAA, 0x44, 5);  // orange flash for RISKY
        }
    } else if (score >= 3) {
        // Suspicious but not critical — mild status message
        char mild_msg[64];
        int i = 0;
        const char* prefix = "[?] Suspicious file: ";
        for (int k = 0; prefix[k] && i < 63; k++) mild_msg[i++] = prefix[k];
        for (int k = 0; name[k] && i < 63; k++) mild_msg[i++] = name[k];
        mild_msg[i] = '\0';
        desktop_set_status(mild_msg);
        scan_log_add(name, score);
    }
    return score;
}

// ─── Public draw ──────────────────────────────────────────────────
void security_draw(void) {
    if (vesa.active) { sec_vesa_draw(); return; }
    sec_vga_draw();
}

// ═══════════════════════════════════════════════════════════════════
//  MOUSE HANDLER
// ═══════════════════════════════════════════════════════════════════

int security_handle_mouse(int mx, int my) {
    if (!vesa.active) return 0;

    int W = (int)vesa.width, H = (int)vesa.height;
    int cx = W / 10, cy = 10;
    int cw = W * 8 / 10, ch = H - 50;
    int th = 36;

    // ── Tab bar clicks ──────────────────────────────────────────
    int tab_y = cy + th + 8, tab_h = 28, tab_x = cx + 12;
    if (my >= tab_y && my < tab_y + tab_h) {
        for (int i = 0; i < SEC_TAB_COUNT; i++) {
            int tw = 120, tx = tab_x + i * (tw + 8);
            if (mx >= tx && mx < tx + tw) {
                sec_tab = i;
                sec_sel = 0; sec_scroll = 0;
                return 1;
            }
        }
        return 1;
    }

    int cont_x = cx + 16;
    int cont_y = tab_y + tab_h + 12;
    int cont_w = cw - 32, cont_h = ch - th - tab_h - 60;
    int row_h = 24, vis_rows = cont_h / row_h;
    if (vis_rows < 1) vis_rows = 1;

    // ── Port Scanner tab ────────────────────────────────────────
    if (sec_tab == TAB_PORTSCAN && mx >= cont_x - 4 && mx < cont_x + cont_w + 8) {
        int total = SCAN_PORT_COUNT;
        int list_y = cont_y + row_h + 6;
        for (int i = sec_scroll; i < total && i < sec_scroll + vis_rows; i++) {
            int ry = list_y + (i - sec_scroll) * row_h;
            if (my >= ry && my < ry + row_h) {
                sec_sel = i;
                sec_scanning = 1; sec_scan_prog = 0;
                for(int f=0;f<12;f++){sec_scan_prog=f;sec_vesa_draw();vesa_present();}
                sec_scan_port(sec_sel); sec_scanning = 0;
                desktop_set_status("Port scan complete");
                return 1;
            }
        }
        return 1;
    }

    // ── Security Audit tab ──────────────────────────────────────
    if (sec_tab == TAB_SECURITY && mx >= cont_x && mx < cont_x + cont_w && my >= cont_y && my < cont_y + cont_h) {
        sec_audit_done = 1;
        desktop_set_status("Security audit complete");
        return 1;
    }

    // ── Vault tab ───────────────────────────────────────────────
    if (sec_tab == TAB_VAULT && mx >= cont_x && mx < cont_x + cont_w) {
        if (vault_state == VAULT_UNLOCKED && vault_count > 0) {
            int list_y = cont_y + row_h + 6;
            int entry_h = 28;
            int vis_entries = (cont_h - row_h - 40) / entry_h;
            if (vis_entries < 1) vis_entries = 1;

            for (int i = vault_list_scroll; i < vault_count && i < vault_list_scroll + vis_entries; i++) {
                int ey = list_y + (i - vault_list_scroll) * entry_h;
                if (my >= ey && my < ey + entry_h) {
                    vault_list_sel = i;
                    vault_view_idx = i;
                    vault_state = VAULT_VIEWING;
                    return 1;
                }
            }
            // Check "[A] Add" click in header
            if (my >= cont_y && my < cont_y + row_h &&
                mx >= cont_x + cont_w - 100 && mx < cont_x + cont_w) {
                vault_state = VAULT_ADDING;
                vault_new_field = 0; vault_new_pos = 0;
                vault_new_website[0] = '\0';
                vault_new_username[0] = '\0';
                vault_new_password[0] = '\0';
                return 1;
            }
        }
        // VAULT_VIEWING — click bottom area to go back
        if (vault_state == VAULT_VIEWING && my >= cont_y + cont_h - 40 && my < cont_y + cont_h) {
            vault_state = VAULT_UNLOCKED;
            return 1;
        }
        return 1;
    }

    // ── File Scanner tab ────────────────────────────────────────
    if (sec_tab == TAB_FILESCAN && mx >= cont_x && mx < cont_x + cont_w) {
        if (scan_view_idx >= 0) {
            // Detail view — click anywhere goes back
            scan_view_idx = -1;
            return 1;
        }

        int list_y = cont_y + row_h + 6;
        int entry_h = 26;
        int vis_files = (cont_h - row_h - 20) / entry_h;
        if (vis_files < 1) vis_files = 1;

        int used_indices[MAX_FILES];
        int used_count = 0;
        for (int i = 0; i < MAX_FILES; i++) {
            if (filesystem[i].used) used_indices[used_count++] = i;
        }

        for (int i = scan_scroll; i < used_count && i < scan_scroll + vis_files; i++) {
            int ey = list_y + (i - scan_scroll) * entry_h;
            if (my >= ey && my < ey + entry_h) {
                scan_sel = i;
                if (scan_done) {
                    scan_view_idx = used_indices[i];
                } else {
                    scan_file(used_indices[i]);
                    scan_view_idx = used_indices[i];
                    desktop_set_status("File scanned");
                }
                return 1;
            }
        }

        // Click "[S] Scan All" / "[S] Rescan" in header
        if (my >= cont_y && my < cont_y + row_h &&
            mx >= cont_x + cont_w - 130 && mx < cont_x + cont_w - 10) {
            scan_all_files();
            desktop_set_status("Full scan complete");
            return 1;
        }
        return 1;
    }

    return 0;
}

// ─── Heuristic file scanner ───────────────────────────────────────
// Scans a single file and sets its result flags + score.
static void scan_file(int idx) {
    if (idx < 0 || idx >= MAX_FILES || !filesystem[idx].used) return;
    File* f = &filesystem[idx];
    int flags = SCAN_FLAG_NONE;
    int score = 0;

    // 1. Check extension
    int name_len = 0;
    while (f->name[name_len]) name_len++;
    int has_ext = 0;
    const char* ext = "";
    for (int i = name_len - 1; i >= 0; i--) {
        if (f->name[i] == '.') { ext = f->name + i; has_ext = 1; break; }
    }

    // 2. Check content for binary/non-printable bytes
    int content_len = 0;
    while (f->content[content_len]) content_len++;
    int non_printable = 0;
    int printable = 0;
    for (int i = 0; i < content_len && i < 256; i++) {
        unsigned char ch = (unsigned char)f->content[i];
        if (ch < 32 && ch != '\n' && ch != '\r' && ch != '\t') non_printable++;
        else printable++;
    }
    if (content_len > 0 && non_printable > printable / 2) {
        flags |= SCAN_FLAG_BINARY;
        score += 4;
    }

    // 3. Check for shell/command injection patterns
    const char* shell_patterns[] = {
        "rm -rf", "format", "mkfs", "dd if=", ":(){:",
        ">/dev/sda", "chmod 777", "wget http", "curl http",
        "shutdown -h", "reboot", "poweroff",
    };
    for (int p = 0; p < (int)(sizeof(shell_patterns)/sizeof(shell_patterns[0])); p++) {
        const char* pat = shell_patterns[p];
        int pi = 0;
        for (int ci = 0; ci < content_len; ci++) {
            if (f->content[ci] == pat[pi]) pi++;
            else {
                pi = 0;
                if (f->content[ci] == pat[0]) pi = 1;
            }
            if (pi > 0 && pat[pi] == '\0') { flags |= SCAN_FLAG_SHELL; score += 3; break; }
        }
        if (flags & SCAN_FLAG_SHELL) break;
    }

    // 4. Executable signature check (ELF header)
    if (content_len >= 4 &&
        (unsigned char)f->content[0] == 0x7F &&
        (unsigned char)f->content[1] == 'E' &&
        (unsigned char)f->content[2] == 'L' &&
        (unsigned char)f->content[3] == 'F') {
        flags |= SCAN_FLAG_EXEC;
        score += 3;
    }
    // MZ executable (DOS/Windows)
    if (content_len >= 2 &&
        (unsigned char)f->content[0] == 'M' &&
        (unsigned char)f->content[1] == 'Z') {
        flags |= SCAN_FLAG_EXEC;
        score += 3;
    }

    // 5. Suspicious size
    if (content_len > 400) {
        flags |= SCAN_FLAG_LARGE;
        score += 1;
    }

    // 6. Suspicious keywords
    const char* sus_keywords[] = {
        "eval(", "exec(", "system(", "popen(", "fopen(",
        "malloc(", "free(", "strcpy(", "memcpy(",
        "__asm__", "int 0x80", "syscall",
        "<script>", "javascript:", "onerror=",
    };
    for (int p = 0; p < (int)(sizeof(sus_keywords)/sizeof(sus_keywords[0])); p++) {
        const char* pat = sus_keywords[p];
        int pi = 0;
        for (int ci = 0; ci < content_len; ci++) {
            if (f->content[ci] == pat[pi]) pi++;
            else {
                pi = 0;
                if (f->content[ci] == pat[0]) pi = 1;
            }
            if (pi > 0 && pat[pi] == '\0') { flags |= SCAN_FLAG_SUSP_STR; score += 2; break; }
        }
        if (flags & SCAN_FLAG_SUSP_STR) break;
    }

    // 7. Script-like: text starting with #! or containing script patterns
    if (content_len >= 2 && f->content[0] == '#' && f->content[1] == '!') {
        flags |= SCAN_FLAG_SCRIPT;
        score += 2;
    }
    // .sh, .py, .pl extension with executable-like content
    if (has_ext) {
        if ((ext[0]=='.' && ext[1]=='s' && ext[2]=='h' && ext[3]=='\0') ||
            (ext[0]=='.' && ext[1]=='p' && ext[2]=='y' && ext[3]=='\0') ||
            (ext[0]=='.' && ext[1]=='p' && ext[2]=='l' && ext[3]=='\0')) {
            if (!(flags & SCAN_FLAG_BINARY)) {
                // Scripts are expected, but check for unusual content
                if (content_len > 300) {
                    flags |= SCAN_FLAG_SCRIPT;
                    score += 1;
                }
            }
        }
    }

    if (score > 10) score = 10;
    scan_results[idx] = flags;
    scan_scores[idx] = score;
}

// Scan all files
static void scan_all_files(void) {
    for (int i = 0; i < MAX_FILES; i++) {
        if (filesystem[i].used) {
            scan_file(i);
        } else {
            scan_results[i] = SCAN_FLAG_NONE;
            scan_scores[i] = 0;
        }
    }
    scan_done = 1;
}

// ─── Simulated port scan ──────────────────────────────────────────
static void sec_scan_port(int idx) {
    if (idx < 0 || idx >= SCAN_PORT_COUNT) return;
    uint16_t p = scan_ports[idx];
    if (p == 22 || p == 80 || p == 443 || p == 53 || p == 8080)
        sec_results[idx] = 1;  // open
    else if (p == 21 || p == 23 || p == 25 || p == 110 || p == 135 || p == 139 || p == 445 || p == 3306 || p == 3389)
        sec_results[idx] = 2;  // closed
    else
        sec_results[idx] = 3;  // filtered
}

// ─── Key handler ──────────────────────────────────────────────────
void security_handle_key(char c) {
    int row_h = 24;
    int cont_h = (vesa.active?(int)vesa.height:25) - 50 - 36 - 28 - 60;
    int vis_rows = cont_h / row_h;
    if (vis_rows < 1) vis_rows = 1;

    if (c == '\t' && sec_tab != TAB_VAULT && sec_tab != TAB_FILESCAN) {
        sec_tab = (sec_tab + 1) % SEC_TAB_COUNT;
        sec_sel = 0; sec_scroll = 0;
        security_draw(); return;
    }
    if (c == 27) {
        current_mode = MODE_DESKTOP;
        desktop_draw(); return;
    }

    if (sec_tab == TAB_PORTSCAN) {
        int total = SCAN_PORT_COUNT;
        if (c=='A'&&sec_sel>0){sec_sel--;if(sec_sel<sec_scroll)sec_scroll--;security_draw();return;}
        if (c=='B'&&sec_sel<total-1){sec_sel++;if(sec_sel>=sec_scroll+vis_rows)sec_scroll++;security_draw();return;}
        if (c==13){
            sec_scanning=1; sec_scan_prog=0;
            for(int f=0;f<12;f++){sec_scan_prog=f;security_draw();if(vesa.active)vesa_present();}
            sec_scan_port(sec_sel); sec_scanning=0;
            security_draw(); desktop_set_status("Port scan complete"); return;
        }
        if (c=='s'||c=='S'){
            sec_scanning=1;
            for(int i=0;i<total;i++){sec_sel=i;sec_scan_prog=0;
                for(int f=0;f<6;f++){sec_scan_prog=f;security_draw();if(vesa.active)vesa_present();}
                sec_scan_port(i);}
            sec_scanning=0; sec_sel=0; sec_scroll=0;
            security_draw(); desktop_set_status("Full scan complete"); return;
        }
    }
    if (sec_tab == TAB_SECURITY && c == 13) { sec_audit_done = 1; desktop_set_status("Security audit complete"); security_draw(); return; }

    // ── Vault key handling ──────────────────────────────────────────
    if (sec_tab == TAB_VAULT) {
        // ── Vault Setup ──────────────────────────────────────────────
        if (vault_state == VAULT_SETUP) {
            if (c == 27) {  // ESC
                vault_state = VAULT_LOCKED;
                vault_input_pos = 0; vault_input[0] = '\0';
                vault_confirm_pos = 0; vault_confirm[0] = '\0';
                vault_setup_step = 0;
                security_draw(); return;
            }
            if (c == '\t') {
                sec_tab = (sec_tab + 1) % SEC_TAB_COUNT;
                sec_sel = 0; sec_scroll = 0;
                vault_input_pos = 0; vault_input[0] = '\0';
                vault_confirm_pos = 0; vault_confirm[0] = '\0';
                vault_setup_step = 0;
                security_draw(); return;
            }
            if (c == 8 || c == 127) {  // Backspace
                if (vault_setup_step == 0 && vault_input_pos > 0) {
                    vault_input[--vault_input_pos] = '\0';
                } else if (vault_setup_step == 1 && vault_confirm_pos > 0) {
                    vault_confirm[--vault_confirm_pos] = '\0';
                }
                security_draw(); return;
            }
            if (c == 13) {  // ENTER
                if (vault_setup_step == 0) {
                    if (vault_input_pos > 0) {
                        vault_setup_step = 1;
                        vault_confirm_pos = 0; vault_confirm[0] = '\0';
                    }
                } else {
                    // Confirm: check both match
                    int match = 1;
                    if (vault_input_pos != vault_confirm_pos) match = 0;
                    else for (int i = 0; i < vault_input_pos; i++)
                        if (vault_input[i] != vault_confirm[i]) { match = 0; break; }
                    if (match && vault_input_pos > 0) {
                        // Save master password
                        for (int i = 0; i < vault_input_pos && i < VAULT_FIELD_LEN - 1; i++)
                            vault_master[i] = vault_input[i];
                        vault_master[vault_input_pos < VAULT_FIELD_LEN - 1 ? vault_input_pos : VAULT_FIELD_LEN - 1] = '\0';
                        vault_locked = 0;
                        vault_state = VAULT_UNLOCKED;
                        vault_input_pos = 0; vault_input[0] = '\0';
                        vault_confirm_pos = 0; vault_confirm[0] = '\0';
                        vault_setup_step = 0;
                        desktop_set_status("Vault unlocked!");
                    }
                }
                security_draw(); return;
            }
            // Printable characters
            if (c >= 32 && c < 127) {
                if (vault_setup_step == 0 && vault_input_pos < VAULT_FIELD_LEN - 2) {
                    vault_input[vault_input_pos++] = c;
                    vault_input[vault_input_pos] = '\0';
                } else if (vault_setup_step == 1 && vault_confirm_pos < VAULT_FIELD_LEN - 2) {
                    vault_confirm[vault_confirm_pos++] = c;
                    vault_confirm[vault_confirm_pos] = '\0';
                }
                security_draw(); return;
            }
        }

        // ── Vault Locked (enter master password to unlock) ───────────
        else if (vault_state == VAULT_LOCKED) {
            if (c == '\t') {
                sec_tab = (sec_tab + 1) % SEC_TAB_COUNT;
                sec_sel = 0; sec_scroll = 0;
                vault_input_pos = 0; vault_input[0] = '\0';
                security_draw(); return;
            }
            if (c == 27) {
                current_mode = MODE_DESKTOP;
                desktop_draw(); return;
            }
            if (c == 8 || c == 127) {
                if (vault_input_pos > 0) {
                    vault_input[--vault_input_pos] = '\0';
                }
                security_draw(); return;
            }
            if (c == 13) {
                // Check master password
                int ok = 1;
                if (vault_input_pos != 0) {
                    // Must match vault_master
                    int i = 0;
                    while (vault_master[i] && i < vault_input_pos && vault_input[i] == vault_master[i]) i++;
                    if (vault_master[i] != '\0' || i != vault_input_pos) ok = 0;
                } else ok = 0;

                if (ok && vault_input_pos > 0) {
                    vault_locked = 0;
                    vault_state = VAULT_UNLOCKED;
                    vault_input_pos = 0; vault_input[0] = '\0';
                    desktop_set_status("Vault unlocked!");
                } else {
                    desktop_set_status("Wrong password");
                }
                security_draw(); return;
            }
            if (c >= 32 && c < 127) {
                if (vault_input_pos < VAULT_FIELD_LEN - 2) {
                    vault_input[vault_input_pos++] = c;
                    vault_input[vault_input_pos] = '\0';
                }
                security_draw(); return;
            }
        }

        // ── Vault Unlocked — Entry List ─────────────────────────────
        else if (vault_state == VAULT_UNLOCKED) {
            if (c == '\t') {
                sec_tab = (sec_tab + 1) % SEC_TAB_COUNT;
                sec_sel = 0; sec_scroll = 0;
                vault_input_pos = 0; vault_input[0] = '\0';
                security_draw(); return;
            }
            if (c == 27) {
                // Lock the vault and return to security tab overview
                vault_locked = 1;
                vault_state = VAULT_LOCKED;
                vault_input_pos = 0; vault_input[0] = '\0';
                desktop_set_status("Vault locked");
                security_draw(); return;
            }
            // Lowercase 'a' to add new entry (arrow up is capital 'A')
            if (c == 'a') {
                vault_state = VAULT_ADDING;
                vault_new_field = 0;
                vault_new_pos = 0;
                vault_new_website[0] = '\0';
                vault_new_username[0] = '\0';
                vault_new_password[0] = '\0';
                vault_show_pass = 0;
                security_draw(); return;
            }
            // Arrow up (capital 'A' from keyboard escape seq)
            if (c == 'A' && vault_count > 0) {
                if (vault_list_sel > 0) {
                    vault_list_sel--;
                    if (vault_list_sel < vault_list_scroll) vault_list_scroll--;
                }
                security_draw(); return;
            }
            // Arrow down (capital 'B' from keyboard escape seq)
            if (c == 'B' && vault_count > 0) {
                int entry_h = 28;
                int vis_entries = ((int)vesa.height - 50 - 36 - 28 - 60 - row_h - 40) / entry_h;
                if (vis_entries < 1) vis_entries = 1;
                if (vault_list_sel < vault_count - 1) {
                    vault_list_sel++;
                    if (vault_list_sel >= vault_list_scroll + vis_entries) vault_list_scroll++;
                }
                security_draw(); return;
            }
            // Toggle password show/hide
            if (c == 'P' || c == 'p') {
                vault_show_pass = !vault_show_pass;
                security_draw(); return;
            }
            // ENTER to view entry details
            if (c == 13 && vault_count > 0) {
                vault_state = VAULT_VIEWING;
                vault_view_idx = vault_list_sel;
                security_draw(); return;
            }
            // DEL to delete selected entry
            if ((c == 127 || c == 'd' || c == 'D') && vault_count > 0) {
                if (vault_list_sel >= 0 && vault_list_sel < vault_count) {
                    for (int i = vault_list_sel; i < vault_count - 1; i++)
                        vault_entries[i] = vault_entries[i + 1];
                    vault_count--;
                    if (vault_list_sel >= vault_count) vault_list_sel = vault_count - 1;
                    if (vault_list_sel < 0) vault_list_sel = 0;
                    desktop_set_status("Entry deleted");
                }
                security_draw(); return;
            }
        }

        // ── Vault Viewing Entry Detail ──────────────────────────────
        else if (vault_state == VAULT_VIEWING) {
            if (c == 27) {  // ESC back to list
                vault_state = VAULT_UNLOCKED;
                security_draw(); return;
            }
            if (c == 'P' || c == 'p') {
                vault_show_pass = !vault_show_pass;
                security_draw(); return;
            }
            if (c == 127 || c == 'd' || c == 'D') {  // DEL
                if (vault_view_idx >= 0 && vault_view_idx < vault_count) {
                    for (int i = vault_view_idx; i < vault_count - 1; i++)
                        vault_entries[i] = vault_entries[i + 1];
                    vault_count--;
                    vault_state = VAULT_UNLOCKED;
                    if (vault_list_sel >= vault_count) vault_list_sel = vault_count - 1;
                    if (vault_list_sel < 0) vault_list_sel = 0;
                    desktop_set_status("Entry deleted");
                }
                security_draw(); return;
            }
        }

        // ── Vault Adding New Entry ─────────────────────────────────
        else if (vault_state == VAULT_ADDING) {
            if (c == 27) {  // ESC discard
                vault_state = VAULT_UNLOCKED;
                vault_new_website[0] = '\0';
                vault_new_username[0] = '\0';
                vault_new_password[0] = '\0';
                vault_new_field = 0;
                vault_new_pos = 0;
                security_draw(); return;
            }
            if (c == '\t' || c == 13) {
                if (c == '\t' || vault_new_field < 2) {
                    // Next field or same field
                    if (c == '\t') {
                        vault_new_field = (vault_new_field + 1) % 3;
                        vault_new_pos = 0;
                    } else {
                        vault_new_field++;
                        if (vault_new_field > 2) vault_new_field = 2;
                        vault_new_pos = 0;
                    }
                } else {
                    // On password field, ENTER saves
                    if (vault_new_website[0] && vault_new_username[0] && vault_new_password[0]) {
                        if (vault_count < MAX_VAULT_ENTRIES) {
                            // Copy fields then encrypt
                            int i;
                            for (i = 0; vault_new_website[i] && i < VAULT_FIELD_LEN - 1; i++)
                                vault_entries[vault_count].website[i] = vault_new_website[i];
                            vault_entries[vault_count].website[i] = '\0';
                            vault_encrypt_field(vault_entries[vault_count].website);
                            for (i = 0; vault_new_username[i] && i < VAULT_FIELD_LEN - 1; i++)
                                vault_entries[vault_count].username[i] = vault_new_username[i];
                            vault_entries[vault_count].username[i] = '\0';
                            vault_encrypt_field(vault_entries[vault_count].username);
                            for (i = 0; vault_new_password[i] && i < VAULT_FIELD_LEN - 1; i++)
                                vault_entries[vault_count].password[i] = vault_new_password[i];
                            vault_entries[vault_count].password[i] = '\0';
                            vault_encrypt_field(vault_entries[vault_count].password);
                            vault_count++;
                            vault_list_sel = vault_count - 1;
                            desktop_set_status("Entry saved!");
                        } else {
                            desktop_set_status("Vault full!");
                        }
                    } else {
                        desktop_set_status("All fields required!");
                    }
                    vault_state = VAULT_UNLOCKED;
                    vault_new_website[0] = '\0';
                    vault_new_username[0] = '\0';
                    vault_new_password[0] = '\0';
                    vault_new_field = 0;
                    vault_new_pos = 0;
                }
                security_draw(); return;
            }
            if (c == 8 || c == 127) {  // Backspace
                char* buf = vault_new_field == 0 ? vault_new_website :
                            vault_new_field == 1 ? vault_new_username : vault_new_password;
                int len = 0;
                while (buf[len]) len++;
                if (len > 0) buf[--len] = '\0';
                vault_new_pos = len;
                security_draw(); return;
            }
            if (c >= 32 && c < 127) {
                char* buf = vault_new_field == 0 ? vault_new_website :
                            vault_new_field == 1 ? vault_new_username : vault_new_password;
                int len = 0;
                while (buf[len]) len++;
                if (len < VAULT_FIELD_LEN - 2) {
                    buf[len] = c;
                    buf[len + 1] = '\0';
                    vault_new_pos = len + 1;
                }
                security_draw(); return;
            }
        // ── File Scanner ────────────────────────────────────────
        } else if (sec_tab == TAB_FILESCAN) {
            if (c == '\t') {
                sec_tab = (sec_tab + 1) % SEC_TAB_COUNT;
                scan_sel = 0; scan_scroll = 0; scan_view_idx = -1;
                security_draw(); return;
            }
            if (c == 27) {
                if (scan_view_idx >= 0) {
                    scan_view_idx = -1;
                } else {
                    vault_locked = 1;
                    vault_state = VAULT_LOCKED;
                    current_mode = MODE_DESKTOP;
                    desktop_draw(); return;
                }
                security_draw(); return;
            }

            // Get used file count
            int used_indices[MAX_FILES];
            int used_count = 0;
            for (int i = 0; i < MAX_FILES; i++) {
                if (filesystem[i].used) used_indices[used_count++] = i;
            }

            if (scan_view_idx >= 0) {
                // ── Detail view ──────────────────────────────────
                if (c == 's' || c == 'S') {
                    scan_file(scan_view_idx);
                    desktop_set_status("File rescanned");
                    security_draw(); return;
                }
                if (c == 13 || c == 27) {
                    scan_view_idx = -1;
                    security_draw(); return;
                }
            } else {
                // ── File list ────────────────────────────────────
                if (used_count == 0) {
                    if (c == '\t' || c == 27) {
                        if (c == '\t') {
                            sec_tab = (sec_tab + 1) % SEC_TAB_COUNT;
                        }
                        security_draw(); return;
                    }
                }

                // Arrow up
                if (c == 'A' && scan_sel > 0) {
                    scan_sel--;
                    if (scan_sel < scan_scroll) scan_scroll--;
                    security_draw(); return;
                }
                // Arrow down
                if (c == 'B' && scan_sel < used_count - 1) {
                    scan_sel++;
                    int vis_files = ((int)vesa.height - 50 - 36 - 28 - 60 - 24 - 20) / 26;
                    if (vis_files < 1) vis_files = 1;
                    if (scan_sel >= scan_scroll + vis_files) scan_scroll++;
                    security_draw(); return;
                }
                // ENTER to view detail (if already scanned)
                if (c == 13 && used_count > 0 && scan_done && scan_sel < used_count) {
                    scan_view_idx = used_indices[scan_sel];
                    security_draw(); return;
                }
                // ENTER to scan single file (if not scanned yet)
                if (c == 13 && used_count > 0 && scan_sel < used_count) {
                    int fi = used_indices[scan_sel];
                    scan_file(fi);
                    scan_done = 1;
                    desktop_set_status("File scanned");
                    security_draw(); return;
                }
                // S/s to scan all
                if (c == 's' || c == 'S') {
                    scan_all_files();
                    desktop_set_status("All files scanned");
                    security_draw(); return;
                }
            }
        }
    }

    security_draw();
}

void security_init(void) {
    sec_tab = 0; sec_sel = 0; sec_scroll = 0;
    sec_scanning = 0; sec_scan_prog = 0; sec_audit_done = 0;
    for (int i = 0; i < SCAN_PORT_COUNT; i++) sec_results[i] = 0;

    // Reset vault state but preserve entries if master password is set
    vault_input_pos = 0; vault_input[0] = '\0';
    vault_confirm_pos = 0; vault_confirm[0] = '\0';
    vault_new_website[0] = '\0';
    vault_new_username[0] = '\0';
    vault_new_password[0] = '\0';
    vault_new_field = 0;
    vault_new_pos = 0;
    vault_list_sel = 0;
    vault_list_scroll = 0;
    vault_show_pass = 0;
    vault_view_idx = 0;

    // Reset file scanner state
    scan_done = 0;
    scan_sel = 0;
    scan_scroll = 0;
    scan_view_idx = -1;
    scan_anim = 0;
    for (int i = 0; i < MAX_FILES; i++) {
        scan_results[i] = 0;
        scan_scores[i] = 0;
    }

    if (vault_master[0] == '\0') {
        // No master password yet — prompt to set one
        vault_state = VAULT_SETUP;
        vault_setup_step = 0;
        vault_locked = 1;
    } else {
        // Master password exists — lock the vault
        vault_state = VAULT_LOCKED;
        vault_locked = 1;
    }

    // Load persisted protection + prompt state from .scanprotect
    security_config_load();
}
