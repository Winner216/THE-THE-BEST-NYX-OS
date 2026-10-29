// cpu/shell.c - Nyx OS Shell v2.0
#include <shell.h>
#include <terminal.h>
#include <fs.h>
#include <rtc.h>
#include <stdint.h>
#include <stddef.h> 
#include <speaker.h>
#include <security.h>
#include <desktop.h>
#include <pdftext.h>
#include <http.h>
#include <nic.h>
#include <netstack.h>
#include <dns.h>
#include <htmlview.h>
#include <pngdec.h>
#include <jpegdec.h>
#include <bmpdec.h>
#include <gifdec.h>
#include <imgview.h>
#include <ata.h>
#include <dhcp.h>
#include <net.h>
#include <task.h>
#include <peload.h>
#include <paging.h>

// ─── Decoded image pixel buffer ───────────────────────────────────
// Shared buffer for decoded PNG/JPEG images. Max 256x256 RGBA.
#define IMG_PIXBUF_MAX (256 * 256 * 4)
static uint8_t img_pixbuf[IMG_PIXBUF_MAX] __attribute__((aligned(64)));

// ─── String helpers ───────────────────────────────────────────────
static int s_len(const char* s){int i=0;while(s[i])i++;return i;}
static int s_eq(const char* a,const char* b){
    int i=0;while(a[i]&&b[i]&&a[i]==b[i])i++;return a[i]==b[i];
}
static int s_ncmp(const char* a,const char* b,int n){
    for(int i=0;i<n;i++){if(a[i]!=b[i])return 1;}return 0;
}
static void s_cpy(char* d,const char* s,int max){
    int i=0;while(s[i]&&i<max-1){d[i]=s[i];i++;}d[i]='\0';
}
static void s_cat(char* d,const char* s,int max){
    int dl=s_len(d);
    int i=0;while(s[i]&&dl+i<max-1){d[dl+i]=s[i];i++;}
    d[dl+i]='\0';
}
static int n2s_sh(int n,char* buf){
    if(n==0){buf[0]='0';buf[1]='\0';return 1;}
    char t[12];int p=0;
    while(n>0){t[p++]='0'+n%10;n/=10;}
    for(int i=0;i<p;i++)buf[i]=t[p-1-i];
    buf[p]='\0';return p;
}

// ─── Command history ──────────────────────────────────────────────
#define HISTORY_MAX 20
#define CMD_MAX     128

static char history[HISTORY_MAX][CMD_MAX];
static int  hist_count = 0;
static int  hist_idx   = -1; // -1 = not browsing

static void history_add(const char* cmd) {
    if(!cmd[0]) return;
    // Don't add duplicate of last command
    if(hist_count>0&&s_eq(history[(hist_count-1)%HISTORY_MAX],cmd)) return;
    s_cpy(history[hist_count%HISTORY_MAX],cmd,CMD_MAX);
    hist_count++;
}

// ─── Helpers ─────────────────────────────────────────────────────
static const char* get_args(const char* cmd){
    while(*cmd&&*cmd!=' ')cmd++;
    while(*cmd==' ')cmd++;
    return cmd;
}
static void get_arg1(const char* args,char* buf,int max){
    int i=0;
    while(args[i]&&args[i]!=' '&&i<max-1){buf[i]=args[i];i++;}
    buf[i]='\0';
}
static const char* get_arg2(const char* args){
    while(*args&&*args!=' ')args++;
    while(*args==' ')args++;
    return args;
}

// ─── Find file index ──────────────────────────────────────────────
static int find_file(const char* name){
    for(int i=0;i<MAX_FILES;i++)
        if(filesystem[i].used&&s_eq(filesystem[i].name,name))
            return i;
    return -1;
}

// ─── Shell prompt ────────────────────────────────────────────────
void shell_prompt(){
    if (security_prompt_indicator_status()) {
        if (security_protection_status()) {
            terminal_set_color(COLOR_LIGHT_GREEN,COLOR_BLACK);
            terminal_print("[ON] ");
        } else {
            terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
            terminal_print("[OFF] ");
        }
    }
    terminal_set_color(COLOR_LIGHT_CYAN,COLOR_BLACK);
    terminal_print("nyx");
    terminal_set_color(COLOR_LIGHT_GREY,COLOR_BLACK);
    terminal_print("> ");
    terminal_set_color(COLOR_WHITE,COLOR_BLACK);
}

// ─── Command implementations ──────────────────────────────────────
static void cmd_help(){
    terminal_set_color(COLOR_LIGHT_CYAN,COLOR_BLACK);
    terminal_print("\n  Nyx OS Shell v2.0 - Commands\n");
    terminal_print("  ==============================\n");

    terminal_set_color(COLOR_YELLOW,COLOR_BLACK);
    terminal_print("  System:\n");
    terminal_set_color(COLOR_WHITE,COLOR_BLACK);
    terminal_print("    help          Show this help\n");
    terminal_print("    about         About Nyx OS\n");
    terminal_print("    version       OS version\n");
    terminal_print("    uname         System info\n");
    terminal_print("    uptime        System uptime\n");
    terminal_print("    date          Current date\n");
    terminal_print("    time          Current time\n");
    terminal_print("    mem           Memory info\n");
    terminal_print("    ps            Process list\n");
    terminal_print("    sched         Scheduler: alg / ready queue\n");
    terminal_print("    sched rr|fcfs|priority  Switch algorithm\n");
    terminal_print("    sched set <pid> <prio>  Set task priority\n");
    terminal_print("    vmem          Address spaces / isolation\n");
    terminal_print("    whoami        Current user\n");
    terminal_print("    reboot        Restart system\n");
    terminal_print("    shutdown      Shutdown system\n");
    terminal_print("    clear/cls     Clear screen\n");
    terminal_print("    halt          Halt system\n");

    terminal_set_color(COLOR_YELLOW,COLOR_BLACK);
    terminal_print("  Files:\n");
    terminal_set_color(COLOR_WHITE,COLOR_BLACK);
    terminal_print("    ls            List files\n");
    terminal_print("    mkfile <n> <t>  Create file\n");
    terminal_print("    open <name>   Open file\n");
    terminal_print("    pe <file.exe> Run PE32 executable\n");
    terminal_print("    cat <name>    Show file contents\n");
    terminal_print("    delf <name>   Delete file\n");
    terminal_print("    cp <src> <dst>  Copy file\n");
    terminal_print("    mv <src> <dst>  Move/rename file\n");
    terminal_print("    rename <o> <n>  Rename file\n");
    terminal_print("    touch <name>  Create empty file\n");
    terminal_print("    pwd           Print directory\n");

    terminal_set_color(COLOR_YELLOW,COLOR_BLACK);
    terminal_print("  Shell:\n");
    terminal_set_color(COLOR_WHITE,COLOR_BLACK);
    terminal_print("    echo <text>   Print text\n");
    terminal_print("    history       Command history\n");
    terminal_print("    calc <expr>   Quick calculator\n");

    terminal_set_color(COLOR_LIGHT_GREY,COLOR_BLACK);
    terminal_print("\n  Security:\n");
    terminal_set_color(COLOR_WHITE,COLOR_BLACK);
    terminal_print("    scanprotect status   Show protection status\n");
    terminal_print("    scanprotect on       Enable real-time scanning\n");
    terminal_print("    scanprotect off      Disable real-time scanning\n");
    terminal_print("    scanprotect log      Show recent threat alerts\n");
    terminal_print("    scanprotect prompt   Toggle [ON]/[OFF] in shell prompt\n");
    terminal_print("    scanprotect defaults Reset to protection ON, prompt OFF\n");
    terminal_print("    scanprotect timeline  Show protection toggle history\n");

    terminal_set_color(COLOR_LIGHT_GREY,COLOR_BLACK);
    terminal_print("\n  Disk:\n");
    terminal_set_color(COLOR_WHITE,COLOR_BLACK);
    terminal_print("    mount           Load files from disk.img (ATA)\n");
    terminal_print("    unmount         Clear disk files, reload embedded\n");

    terminal_set_color(COLOR_LIGHT_GREY,COLOR_BLACK);
    terminal_print("\n  Documents:\n");
    terminal_set_color(COLOR_WHITE,COLOR_BLACK);
    terminal_print("    pdftext <file>       Extract text from a PDF file\n");
    terminal_print("    pdftext --pages <file>  Show page count only\n");

    terminal_set_color(COLOR_LIGHT_GREY,COLOR_BLACK);
    terminal_print("\n  Graphics:\n");
    terminal_set_color(COLOR_WHITE,COLOR_BLACK);
    terminal_print("    imgview <file>      View PNG or JPEG image\n");

    terminal_set_color(COLOR_LIGHT_GREY,COLOR_BLACK);
    terminal_print("\n  Network:\n");
    terminal_set_color(COLOR_WHITE,COLOR_BLACK);
    terminal_print("    pci list           List all PCI devices\n");
    terminal_print("    http init           Initialize RTL8139 NIC\n");
    terminal_print("    http get <host> <path>  HTTP GET request\n");
    terminal_print("    net config <ip> <gw> [dns]  Set IP, gateway & DNS\n");
    terminal_print("    net config dns <ip>  Set DNS server only\n");
    terminal_print("    net config dhcp      Auto-configure via DHCP\n");
    terminal_print("    net status          Show IP, gateway & DNS config\n");
    terminal_print("    net scan            Scan LAN for other devices\n");
    terminal_print("    viewhtml <file>     Render HTML file in viewer\n");

    terminal_set_color(COLOR_LIGHT_GREY,COLOR_BLACK);
    terminal_print("\n  Tips:\n");

    terminal_print("    UP/DOWN = history\n\n");
}

static void cmd_ls(){
    terminal_set_color(COLOR_LIGHT_CYAN,COLOR_BLACK);
    terminal_print("\n  /\n");
    terminal_set_color(COLOR_LIGHT_GREY,COLOR_BLACK);
    terminal_print("  [DIR] boot/\n");
    terminal_print("  [DIR] kernel/\n");
    terminal_print("  [DIR] cpu/\n");
    terminal_print("  [DIR] include/\n");
    if(file_count==0){
        terminal_set_color(COLOR_DARK_GREY,COLOR_BLACK);
        terminal_print("  (no user files)\n");
    } else {
        for(int i=0;i<MAX_FILES;i++){
            if(filesystem[i].used){
                terminal_set_color(COLOR_YELLOW,COLOR_BLACK);
                terminal_print("  [FILE] ");
                terminal_set_color(COLOR_WHITE,COLOR_BLACK);
                terminal_print(filesystem[i].name);
                terminal_set_color(COLOR_DARK_GREY,COLOR_BLACK);
                terminal_print("  (");
                char sz[8];
                n2s_sh(s_len(filesystem[i].content),sz);
                terminal_print(sz);
                terminal_print(" bytes)\n");
            }
        }
    }
    terminal_print("\n");
}

static void cmd_cat(const char* args){
    char name[MAX_FILENAME];
    get_arg1(args,name,MAX_FILENAME);
    if(!name[0]){
        terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
        terminal_print("  Usage: cat <filename>\n");
        return;
    }
    int idx=find_file(name);
    if(idx<0){
        terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
        terminal_print("  File not found: ");
        terminal_print(name);
        terminal_print("\n");
        return;
    }
    terminal_set_color(COLOR_LIGHT_CYAN,COLOR_BLACK);
    terminal_print("  --- ");
    terminal_print(name);
    terminal_print(" ---\n");
    terminal_set_color(COLOR_WHITE,COLOR_BLACK);
    terminal_print("  ");
    terminal_print(filesystem[idx].content);
    terminal_print("\n");
    terminal_set_color(COLOR_LIGHT_CYAN,COLOR_BLACK);
    terminal_print("  ------\n\n");
}

static void cmd_cp(const char* args){
    char src[MAX_FILENAME],dst[MAX_FILENAME];
    get_arg1(args,src,MAX_FILENAME);
    const char* rest=get_arg2(args);
    get_arg1(rest,dst,MAX_FILENAME);
    if(!src[0]||!dst[0]){
        terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
        terminal_print("  Usage: cp <source> <dest>\n");
        return;
    }
    int idx=find_file(src);
    if(idx<0){terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);terminal_print("  Source not found!\n");return;}
    if(find_file(dst)>=0){terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);terminal_print("  Destination exists!\n");return;}
    // Find free slot
    int slot=-1;
    for(int i=0;i<MAX_FILES;i++) if(!filesystem[i].used){slot=i;break;}
    if(slot<0){terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);terminal_print("  Filesystem full!\n");return;}
    s_cpy(filesystem[slot].name,dst,MAX_FILENAME);
    file_content_alloc(slot, filesystem[idx].size > 0 ? filesystem[idx].size + 1 : 1);
    if(filesystem[slot].content && filesystem[idx].content) {
        s_cpy(filesystem[slot].content,filesystem[idx].content,filesystem[idx].size > 0 ? filesystem[idx].size + 1 : 1);
    }
    filesystem[slot].size = filesystem[idx].size;
    filesystem[slot].used=1;
    file_count++;
    terminal_set_color(COLOR_LIGHT_GREEN,COLOR_BLACK);
    terminal_print("  Copied: ");
    terminal_print(src);
    terminal_print(" -> ");
    terminal_print(dst);
    terminal_print("\n");
}

static void cmd_mv(const char* args){
    char src[MAX_FILENAME],dst[MAX_FILENAME];
    get_arg1(args,src,MAX_FILENAME);
    const char* rest=get_arg2(args);
    get_arg1(rest,dst,MAX_FILENAME);
    if(!src[0]||!dst[0]){
        terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
        terminal_print("  Usage: mv <source> <dest>\n");
        return;
    }
    int idx=find_file(src);
    if(idx<0){terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);terminal_print("  Source not found!\n");return;}
    s_cpy(filesystem[idx].name,dst,MAX_FILENAME);
    terminal_set_color(COLOR_LIGHT_GREEN,COLOR_BLACK);
    terminal_print("  Moved: ");
    terminal_print(src);
    terminal_print(" -> ");
    terminal_print(dst);
    terminal_print("\n");
}

static void cmd_rename(const char* args){
    cmd_mv(args);
}

static void cmd_touch(const char* args){
    char name[MAX_FILENAME];
    get_arg1(args,name,MAX_FILENAME);
    if(!name[0]){terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);terminal_print("  Usage: touch <name>\n");return;}
    if(find_file(name)>=0){terminal_set_color(COLOR_YELLOW,COLOR_BLACK);terminal_print("  File already exists!\n");return;}
    int slot=-1;
    for(int i=0;i<MAX_FILES;i++) if(!filesystem[i].used){slot=i;break;}
    if(slot<0){terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);terminal_print("  Filesystem full!\n");return;}
    s_cpy(filesystem[slot].name,name,MAX_FILENAME);
    filesystem[slot].content[0]='\0';
    filesystem[slot].size=0;
    filesystem[slot].used=1;
    file_count++;
    // Real-time protection: scan the new file
    int threat = security_scan_file_at(slot);
    terminal_set_color(COLOR_LIGHT_GREEN,COLOR_BLACK);
    terminal_print("  Created: ");
    terminal_print(name);
    if (threat >= 6) {
        terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
        terminal_print("  [THREAT DETECTED]");
    } else if (threat >= 3) {
        terminal_set_color(COLOR_YELLOW,COLOR_BLACK);
        terminal_print("  [suspicious]");
    }
    terminal_print("\n");
}

static void cmd_history(){
    terminal_set_color(COLOR_LIGHT_CYAN,COLOR_BLACK);
    terminal_print("\n  Command History:\n");
    int start=hist_count>HISTORY_MAX?hist_count-HISTORY_MAX:0;
    for(int i=start;i<hist_count;i++){
        terminal_set_color(COLOR_DARK_GREY,COLOR_BLACK);
        char num[6]; n2s_sh(i+1,num);
        terminal_print("  ");
        terminal_print(num);
        terminal_print("  ");
        terminal_set_color(COLOR_WHITE,COLOR_BLACK);
        terminal_print(history[i%HISTORY_MAX]);
        terminal_print("\n");
    }
    terminal_print("\n");
}

static void cmd_calc(const char* args){
    if(!args[0]){terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);terminal_print("  Usage: calc <num> <op> <num>\n  Example: calc 5 + 3\n");return;}
    // Simple expression parser: num op num
    int a=0,b=0; char op=0;
    int i=0;
    while(args[i]>='0'&&args[i]<='9'){a=a*10+(args[i]-'0');i++;}
    while(args[i]==' ')i++;
    if(args[i]){op=args[i];i++;}
    while(args[i]==' ')i++;
    while(args[i]>='0'&&args[i]<='9'){b=b*10+(args[i]-'0');i++;}
    int result=0;
    int valid=1;
    if(op=='+') result=a+b;
    else if(op=='-') result=a-b;
    else if(op=='*') result=a*b;
    else if(op=='/'){if(b==0){terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);terminal_print("  Division by zero!\n");return;}result=a/b;}
    else{valid=0;terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);terminal_print("  Unknown operator! Use + - * /\n");return;}
    if(valid){
        terminal_set_color(COLOR_LIGHT_GREEN,COLOR_BLACK);
        terminal_print("  = ");
        char res[12]; n2s_sh(result,res);
        terminal_print(res);
        terminal_print("\n");
    }
}

static void cmd_mem(){
    terminal_set_color(COLOR_LIGHT_CYAN,COLOR_BLACK);
    terminal_print("\n  Memory Info:\n");
    terminal_set_color(COLOR_WHITE,COLOR_BLACK);
    terminal_print("  Total  : 512 MB\n");
    terminal_print("  Used   : ~64 MB\n");
    terminal_print("  Free   : ~448 MB\n");
    terminal_print("  Kernel : ~2 MB\n");
    terminal_print("  VGA    : 4 KB\n\n");
}

// ─── Task/process list (REAL: from the multitasking scheduler) ──
static void cmd_ps(){
    terminal_set_color(COLOR_LIGHT_CYAN,COLOR_BLACK);
    terminal_print("\n  PID  Name          Prio State     Ticks   Switches\n");
    terminal_print("  ---- ------------- ---- --------- ------- ---------\n");
    terminal_set_color(COLOR_WHITE,COLOR_BLACK);
    int cur = task_current_id();
    for(int i=0;i<8;i++){
        if(!task_name(i) || s_eq(task_name(i),"-")) continue;
        const char* nm = task_name(i);
        terminal_print("  ");
        terminal_putchar('0'+i);            // PID (single digit)
        terminal_print("    ");
        terminal_print(nm);
        int pl = s_len(nm);
        for(int j=pl;j<14;j++) terminal_putchar(' ');
        // priority (0-255, higher = preferred in PRIORITY mode)
        terminal_set_color(COLOR_YELLOW,COLOR_BLACK);
        { char pbuf[8]; n2s_sh(task_priority(i),pbuf);
          terminal_print(pbuf);
          for(int j=s_len(pbuf);j<5;j++) terminal_putchar(' '); }
        terminal_set_color(COLOR_LIGHT_GREEN,COLOR_BLACK);
        terminal_print(task_state_name(i));
        terminal_set_color(COLOR_WHITE,COLOR_BLACK);
        for(int j=s_len(task_state_name(i));j<11;j++) terminal_putchar(' ');
        // ticks
        char tbuf[12]; int tp=0; uint32_t v=task_ticks(i);
        if(v==0){tbuf[tp++]='0';} else { char tmp[12]; int q=0;
            while(v>0){tmp[q++]=(char)('0'+v%10); v/=10;}
            for(int k=q-1;k>=0;k--) tbuf[tp++]=tmp[k]; }
        tbuf[tp]=0;
        terminal_print(tbuf);
        for(int j=tp;j<8;j++) terminal_putchar(' ');
        // switches
        char sbuf[12]; int sp=0; v=task_switches(i);
        if(v==0){sbuf[sp++]='0';} else { char tmp[12]; int q=0;
            while(v>0){tmp[q++]=(char)('0'+v%10); v/=10;}
            for(int k=q-1;k>=0;k--) sbuf[sp++]=tmp[k]; }
        sbuf[sp]=0;
        terminal_print(sbuf);
        if(i==cur) terminal_print("   <--");
        terminal_print("\n");
    }
    terminal_set_color(COLOR_LIGHT_CYAN,COLOR_BLACK);
    terminal_print("  Total context switches: ");
    terminal_set_color(COLOR_YELLOW,COLOR_BLACK);
    // sched_total_switches() digits
    { uint32_t v=sched_total_switches(); char tmp[12]; int q=0;
        if(v==0) terminal_putchar('0');
        while(v>0){tmp[q++]=(char)('0'+v%10); v/=10;}
        for(int k=q-1;k>=0;k--) terminal_putchar(tmp[k]); }
    terminal_set_color(COLOR_WHITE,COLOR_BLACK);
    terminal_print("\n\n");
}

// ─── Virtual memory / isolation report ───────────────────────────
static void cmd_vmem(){
    const char* hexd = "0123456789ABCDEF";
    uint32_t v;

    terminal_set_color(COLOR_LIGHT_CYAN,COLOR_BLACK);
    terminal_print("\n  Virtual Memory — per-task address spaces\n");
    terminal_print("  =========================================\n");
    terminal_set_color(COLOR_WHITE,COLOR_BLACK);
    terminal_print("  Kernel map : 0x00000000..0x3FFFFFFF identity, 4 KiB pages\n");
    terminal_print("              + VESA LFB above 1 GiB (4 MiB pages)\n");
    terminal_print("  Private    : 0xC0000000 + pid*16 MiB (only owner maps it)\n\n");

    terminal_set_color(COLOR_DARK_GREY,COLOR_BLACK);
    terminal_print("  PID  Name       CR3        PrivPages  First VA\n");
    terminal_print("  ---  ---------  ---------  ---------  -------------\n");
    for (int i = 0; i < 8; i++) {
        if (s_eq(task_name(i),"-")) continue;
        terminal_set_color(COLOR_WHITE,COLOR_BLACK);
        terminal_putchar('0'+i);
        terminal_print("    ");
        terminal_print(task_name(i));
        int pl = s_len(task_name(i));
        for (int j = pl; j < 11; j++) terminal_putchar(' ');
        // CR3 (page directory physical address)
        v = paging_task_pd(i);
        terminal_set_color(COLOR_YELLOW,COLOR_BLACK);
        for (int k = 7; k >= 0; k--) terminal_putchar(hexd[(v>>(k*4))&0xF]);
        terminal_print("  ");
        // Private page count
        int pc = paging_priv_count(i);
        { char pc_s[8]; n2s_sh(pc,pc_s); terminal_print(pc_s);
          for (int j = s_len(pc_s); j < 10; j++) terminal_putchar(' '); }
        // First private VA (or --)
        if (pc > 0) {
            v = paging_priv_va(i, 0);
            terminal_set_color(COLOR_LIGHT_GREEN,COLOR_BLACK);
            terminal_print("0x");
            for (int k = 7; k >= 0; k--) terminal_putchar(hexd[(v>>(k*4))&0xF]);
        } else {
            terminal_set_color(COLOR_DARK_GREY,COLOR_BLACK);
            terminal_print("    --");
        }
        terminal_print("\n");
    }

    // Isolation check: no task's PD may map another task's pages.
    int cross = paging_cross_mappings();
    terminal_set_color(COLOR_LIGHT_CYAN,COLOR_BLACK);
    terminal_print("  Isolation   : ");
    if (cross == 0) {
        terminal_set_color(COLOR_LIGHT_GREEN,COLOR_BLACK);
        terminal_print("OK — 0 cross-mappings (no task can read another's memory)\n");
    } else {
        terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
        terminal_print("VIOLATION — ");
        { char c_s[8]; n2s_sh(cross,c_s); terminal_print(c_s); }
        terminal_print(" cross-mappings!\n");
    }
    terminal_set_color(COLOR_WHITE,COLOR_BLACK);
    terminal_print("\n");
}

// ─── Scheduler control: switch algorithm / set priority / dump queue ─
// Shared setter for `sched set <pid> <prio>` / `sched prio <pid> <prio>`.
static void sched_cmd_set_prio(const char* args){
    char a2[16]; get_arg1(get_arg2(args),a2,16);
    char a3[16]; get_arg1(get_arg2(get_arg2(args)),a3,16);
    int id=0,p=0;
    for(int i=0;a2[i]>='0'&&a2[i]<='9';i++) id=id*10+(a2[i]-'0');
    for(int i=0;a3[i]>='0'&&a3[i]<='9';i++) p=p*10+(a3[i]-'0');
    if (a2[0] && a3[0] && task_set_priority(id,p)==0){
        terminal_set_color(COLOR_LIGHT_GREEN,COLOR_BLACK);
        terminal_print("  Task "); terminal_print(a2);
        terminal_print(" priority = "); terminal_print(a3);
        terminal_print("\n");
    } else {
        terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
        terminal_print("  Usage: sched <set|prio> <pid> <prio>\n");
    }
}

static void cmd_sched(const char* args){
    char sub[16];
    get_arg1(args,sub,16);

    if (s_eq(sub,"rr")||s_eq(sub,"round")||s_eq(sub,"roundrobin")) {
        sched_set_algorithm(SCHED_RR);
        terminal_set_color(COLOR_LIGHT_GREEN,COLOR_BLACK);
        terminal_print("  Scheduler: round-robin (timeslice "
                       "preemption, tail re-enqueue)\n");
    } else if (s_eq(sub,"fcfs")) {
        sched_set_algorithm(SCHED_FCFS);
        terminal_set_color(COLOR_LIGHT_GREEN,COLOR_BLACK);
        terminal_print("  Scheduler: first-come-first-served "
                       "(no timeslice preemption)\n");
        terminal_set_color(COLOR_YELLOW,COLOR_BLACK);
        terminal_print("  NOTE: tasks run until they block — the "
                       "desktop never blocks,\n");
        terminal_print("        so worker/blinker stay frozen in "
                       "FCFS. Use rr or priority.\n");
    } else if (s_eq(sub,"priority")||s_eq(sub,"prio")) {
        // `sched prio <pid> <prio>` sets priority; bare `sched prio`
        // switches the algorithm to strict priority.
        char a2[16]; get_arg1(get_arg2(args),a2,16);
        if (a2[0] >= '0' && a2[0] <= '9') { sched_cmd_set_prio(args); }
        else {
            sched_set_algorithm(SCHED_PRIORITY);
            terminal_set_color(COLOR_LIGHT_GREEN,COLOR_BLACK);
            terminal_print("  Scheduler: priority (higher prio runs first)\n");
        }
    } else if (s_ncmp(sub,"set",3)==0) {
        sched_cmd_set_prio(args);
    } else {
        // No args: dump the scheduler state + ready queue
        terminal_set_color(COLOR_LIGHT_CYAN,COLOR_BLACK);
        terminal_print("\n  Scheduler: ");
        terminal_set_color(COLOR_YELLOW,COLOR_BLACK);
        terminal_print(sched_algorithm_name());
        terminal_set_color(COLOR_WHITE,COLOR_BLACK);
        terminal_print("\n  Ready queue (");
        { char qs[4]; n2s_sh(sched_queue_len(),qs); terminal_print(qs); }
        terminal_print(" waiting for CPU):\n");
        terminal_set_color(COLOR_DARK_GREY,COLOR_BLACK);
        for (int k=0;k<sched_queue_len();k++){
            int id = sched_queue_at(k);
            terminal_set_color(COLOR_WHITE,COLOR_BLACK);
            terminal_print("    [");
            terminal_print(task_name(id));
            terminal_print(" prio=");
            { char ps[4]; n2s_sh(task_priority(id),ps); terminal_print(ps); }
            terminal_print("]");
            if (k==0) {
                terminal_set_color(COLOR_LIGHT_GREEN,COLOR_BLACK);
                terminal_print("  <- next");
            }
            terminal_set_color(COLOR_WHITE,COLOR_BLACK);
            terminal_print("\n");
        }
        terminal_set_color(COLOR_LIGHT_GREY,COLOR_BLACK);
        terminal_print("  Running: ");
        terminal_set_color(COLOR_YELLOW,COLOR_BLACK);
        terminal_print(task_current_name());
        terminal_set_color(COLOR_LIGHT_GREY,COLOR_BLACK);
        terminal_print("\n\n  Usage: sched [rr|fcfs|priority]\n");
        terminal_print("         sched set <pid> <prio>   (or: sched prio <pid> <prio>)\n\n");
    }
}

static void cmd_uname(){
    terminal_set_color(COLOR_LIGHT_CYAN,COLOR_BLACK);
    terminal_print("  Nyx OS 0.1 x86 32-bit Protected Mode\n");
}

static void cmd_version(){
    terminal_set_color(COLOR_LIGHT_CYAN,COLOR_BLACK);
    terminal_print("  Nyx OS v0.1\n");
    terminal_set_color(COLOR_WHITE,COLOR_BLACK);
    terminal_print("  Built from scratch in C & Assembly\n");
    terminal_print("  Features: GDT IDT IRQ VFS RTC\n");
}

static void cmd_whoami(){
    extern int logged_in_user;
    extern char users[][16];
    terminal_set_color(COLOR_LIGHT_GREEN,COLOR_BLACK);
    if(logged_in_user>=0)
        terminal_print(users[logged_in_user]);
    else
        terminal_print("guest");
    terminal_print("\n");
}

static void cmd_date(){
    char date_str[9]; rtc_get_date_str(date_str);
    terminal_set_color(COLOR_LIGHT_CYAN,COLOR_BLACK);
    terminal_print("  Date: ");
    terminal_print(date_str);
    terminal_print("\n");
}

static void cmd_time_cmd(){
    char time_str[9]; rtc_get_time_str(time_str);
    terminal_set_color(COLOR_LIGHT_CYAN,COLOR_BLACK);
    terminal_print("  Time: ");
    terminal_print(time_str);
    terminal_print("\n");
}

static void cmd_uptime(){
    terminal_set_color(COLOR_LIGHT_CYAN,COLOR_BLACK);
    terminal_print("  Uptime: running since boot\n");
}

static void cmd_pwd(){
    terminal_set_color(COLOR_LIGHT_CYAN,COLOR_BLACK);
    terminal_print("  /home/nyx\n");
}

static void cmd_scanprotect(const char* args){
    char sub[16];
    get_arg1(args,sub,16);

    if (s_eq(sub,"status")) {
        int on = security_protection_status();
        terminal_set_color(COLOR_LIGHT_CYAN,COLOR_BLACK);
        terminal_print("\n  Real-Time Protection\n");
        terminal_print("  ====================\n");
        terminal_set_color(COLOR_WHITE,COLOR_BLACK);
        terminal_print("  Status: ");
        if (on) {
            terminal_set_color(COLOR_LIGHT_GREEN,COLOR_BLACK);
            terminal_print("ENABLED\n");
        } else {
            terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
            terminal_print("DISABLED\n");
        }
        terminal_set_color(COLOR_WHITE,COLOR_BLACK);
        terminal_print("  Total alerts: ");
        char n[6]; n2s_sh(security_alert_count(),n);
        terminal_print(n);
        terminal_print("\n");
        terminal_print("  Commands: on | off | log\n\n");

    } else if (s_eq(sub,"on")) {
        security_protection_set(1);
        desktop_vfx_flash(0x44, 0xFF, 0x88, 5);  // green flash confirmation
        terminal_set_color(COLOR_LIGHT_GREEN,COLOR_BLACK);
        terminal_print("  Real-time protection ENABLED\n");

    } else if (s_eq(sub,"off")) {
        security_protection_set(0);
        desktop_vfx_flash(0xFF, 0x44, 0x55, 5);  // red flash confirmation
        terminal_set_color(COLOR_YELLOW,COLOR_BLACK);
        terminal_print("  Real-time protection DISABLED\n");

    } else if (s_eq(sub,"log")) {
        int total = security_alert_count();
        terminal_set_color(COLOR_LIGHT_CYAN,COLOR_BLACK);
        terminal_print("\n  Alert Log");
        if (total == 0) {
            terminal_set_color(COLOR_WHITE,COLOR_BLACK);
            terminal_print(" (no threats detected)\n");
        } else {
            terminal_print("\n");
            terminal_set_color(COLOR_DARK_GREY,COLOR_BLACK);
            terminal_print("  #  Score  File\n");
            terminal_print("  -- -----  ----\n");
            int count = total < SCAN_LOG_MAX ? total : SCAN_LOG_MAX;
            for (int i = 0; i < count; i++) {
                terminal_set_color(COLOR_DARK_GREY,COLOR_BLACK);
                char idx_s[4]; n2s_sh(i+1,idx_s);
                terminal_print("  ");
                if (i+1 < 10) terminal_putchar(' ');
                terminal_print(idx_s);
                terminal_print("  ");
                int sc = security_alert_score(i);
                char sc_s[4]; n2s_sh(sc,sc_s);
                if (sc >= 9)
                    terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
                else if (sc >= 6)
                    terminal_set_color(COLOR_YELLOW,COLOR_BLACK);
                else
                    terminal_set_color(COLOR_WHITE,COLOR_BLACK);
                terminal_print("  ");
                if (sc < 10) terminal_putchar(' ');
                terminal_print(sc_s);
                terminal_print("/10 ");
                terminal_set_color(COLOR_WHITE,COLOR_BLACK);
                terminal_print(security_alert_file(i));
                terminal_print("\n");
            }
        }
        terminal_print("\n");

    } else if (s_eq(sub,"prompt")) {
        security_prompt_indicator_toggle();
        if (security_prompt_indicator_status()) {
            terminal_set_color(COLOR_LIGHT_GREEN,COLOR_BLACK);
            terminal_print("  Prompt indicator ON — next prompt shows [ON]/[OFF]\n");
        } else {
            terminal_set_color(COLOR_YELLOW,COLOR_BLACK);
            terminal_print("  Prompt indicator OFF\n");
        }

    } else if (s_eq(sub,"defaults")) {
        // Reset protection to ON
        security_protection_set(1);
        // Reset prompt indicator to OFF
        if (security_prompt_indicator_status()) {
            security_prompt_indicator_toggle();
        }
        desktop_vfx_flash(0x44, 0xFF, 0x88, 5);  // green flash
        terminal_set_color(COLOR_LIGHT_GREEN,COLOR_BLACK);
        terminal_print("\n  Settings reset to defaults:\n");
        terminal_print("  • Protection: ENABLED\n");
        terminal_set_color(COLOR_DARK_GREY,COLOR_BLACK);
        terminal_print("  • Prompt indicator: OFF\n\n");

    } else if (s_eq(sub,"timeline")) {
        int total = security_timeline_count();
        terminal_set_color(COLOR_LIGHT_CYAN,COLOR_BLACK);
        terminal_print("\n  Protection Timeline");
        if (total == 0) {
            terminal_set_color(COLOR_WHITE,COLOR_BLACK);
            terminal_print(" (no state changes yet)\n");
        } else {
            terminal_print("\n");
            terminal_set_color(COLOR_DARK_GREY,COLOR_BLACK);
            terminal_print("  #  Time      State\n");
            terminal_print("  -- --------  -------\n");
            int count = total < TIMELINE_MAX ? total : TIMELINE_MAX;
            for (int i = 0; i < count; i++) {
                terminal_set_color(COLOR_DARK_GREY,COLOR_BLACK);
                char idx_s[4]; n2s_sh(i+1,idx_s);
                terminal_print("  ");
                if (i+1 < 10) terminal_putchar(' ');
                terminal_print(idx_s);
                terminal_print("  ");
                terminal_set_color(COLOR_WHITE,COLOR_BLACK);
                terminal_print(security_timeline_time(i));
                terminal_print("  ");
                int st = security_timeline_state(i);
                if (st) {
                    terminal_set_color(COLOR_LIGHT_GREEN,COLOR_BLACK);
                    terminal_print("ENABLED");
                } else {
                    terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
                    terminal_print("DISABLED");
                }
                terminal_print("\n");
            }
        }
        terminal_print("\n");

    } else {
        terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
        terminal_print("  Usage: scanprotect <status|on|off|log|prompt|defaults|timeline>\n");
    }
}

// ─── Main process command ─────────────────────────────────────────
void shell_process(char* cmd){
    if(!cmd[0]){ shell_prompt(); return; }

    // DEBUG: isolate freeze step by step
    terminal_print("  [DBG1] cmd='");
    terminal_print(cmd);
    terminal_print("'\n");

    terminal_print("  [DBG2] about to history_add\n");
    history_add(cmd);
    terminal_print("  [DBG3] history_add done\n");

    hist_idx=-1;

    const char* args=get_args(cmd);
    terminal_print("  [DBG4] args parsed\n");

    // Match command
    if(s_eq(cmd,"help")){ terminal_print("  [DBG5] calling cmd_help\n"); cmd_help(); }
    else if(s_eq(cmd,"ls")){ terminal_print("  [DEBUG] calling cmd_ls\n"); cmd_ls(); }
    else if(s_eq(cmd,"clear")||s_eq(cmd,"cls")){
        terminal_print("  [DEBUG] calling clear\n");
        for(int i=0;i<25;i++) terminal_print("\n");
    }
    else if(s_eq(cmd,"halt")){
        terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
        terminal_print("  Halting...\n");
    }
    else if(s_eq(cmd,"reboot")){
        terminal_set_color(COLOR_YELLOW,COLOR_BLACK);
        terminal_print("  Rebooting...\n");
    }
    else if(s_eq(cmd,"shutdown")){
        terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
        terminal_print("  Shutting down...\n");
    }
    else if(s_ncmp(cmd,"echo",4)==0)    { terminal_print("  [DEBUG] calling echo\n"); terminal_set_color(COLOR_WHITE,COLOR_BLACK); terminal_print("  "); terminal_print(args); terminal_print("\n"); }
    else if(s_ncmp(cmd,"cat",3)==0){ terminal_print("  [DEBUG] calling cmd_cat\n"); cmd_cat(args); }
    else if(s_ncmp(cmd,"cp",2)==0){ terminal_print("  [DEBUG] calling cmd_cp\n"); cmd_cp(args); }
    else if(s_ncmp(cmd,"mv",2)==0){ terminal_print("  [DEBUG] calling cmd_mv\n"); cmd_mv(args); }
    else if(s_ncmp(cmd,"rename",6)==0){ terminal_print("  [DEBUG] calling cmd_rename\n"); cmd_rename(args); }
    else if(s_ncmp(cmd,"touch",5)==0){ terminal_print("  [DEBUG] calling cmd_touch\n"); cmd_touch(args); }
    else if(s_ncmp(cmd,"delf",4)==0){
        char name[MAX_FILENAME]; get_arg1(args,name,MAX_FILENAME);
        int idx=find_file(name);
        if(idx<0){terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);terminal_print("  Not found!\n");}
        else{filesystem[idx].used=0;file_count--;terminal_set_color(COLOR_LIGHT_GREEN,COLOR_BLACK);terminal_print("  Deleted!\n");}
    }
    else if(s_ncmp(cmd,"mkfile",6)==0){
        char name[MAX_FILENAME]; get_arg1(args,name,MAX_FILENAME);
        const char* content=get_arg2(args);
        if(!name[0]){terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);terminal_print("  Usage: mkfile <name> <content>\n");}
        else if(find_file(name)>=0){terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);terminal_print("  File exists!\n");}
        else{
            int slot=-1;
            for(int i=0;i<MAX_FILES;i++) if(!filesystem[i].used){slot=i;break;}
            if(slot<0){terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);terminal_print("  Filesystem full!\n");}
            else{
                s_cpy(filesystem[slot].name,name,MAX_FILENAME);
                int clen=0; while(content[clen]) clen++;
                file_content_alloc(slot, clen + 1);
                if(filesystem[slot].content) {
                    s_cpy(filesystem[slot].content,content,clen+1);
                }
                filesystem[slot].size=clen;
                filesystem[slot].used=1; file_count++;
                // Real-time protection: scan the new file
                int threat = security_scan_file_at(slot);
                terminal_set_color(COLOR_LIGHT_GREEN,COLOR_BLACK);
                terminal_print("  Created: "); terminal_print(name);
                if (threat >= 6) {
                    terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
                    terminal_print("  [THREAT DETECTED]");
                } else if (threat >= 3) {
                    terminal_set_color(COLOR_YELLOW,COLOR_BLACK);
                    terminal_print("  [suspicious]");
                }
                terminal_print("\n");
            }
        }
    }
    else if(s_ncmp(cmd,"open",4)==0){
        char name[MAX_FILENAME]; get_arg1(args,name,MAX_FILENAME);
        int idx=find_file(name);
        if(idx<0){terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);terminal_print("  Not found!\n");}
        else{
            const char* content = filesystem[idx].content;
            int clen = filesystem[idx].size;
            if (clen <= 0) { while (content[clen]) clen++; }

            // Auto-detect PNG files
            if (clen >= 8 && (uint8_t)content[0] == 0x89 && (uint8_t)content[1] == 'P' &&
                (uint8_t)content[2] == 'N' && (uint8_t)content[3] == 'G') {
                terminal_set_color(COLOR_LIGHT_GREEN,COLOR_BLACK);
                terminal_print("  Detected PNG — opening in image viewer...\n");
                PngImage img;
                img.pixels = img_pixbuf;
                if (png_decode((const uint8_t*)content, clen, &img)) {
                    int needed = (int)(img.width * img.height * 4);
                    if (needed <= IMG_PIXBUF_MAX) {
                        imgview_init((int)img.width, (int)img.height, img.pixels, name);
                        current_mode = MODE_IMGVIEW;
                    } else {
                        terminal_set_color(COLOR_YELLOW,COLOR_BLACK);
                        terminal_print("  Image too large (max 256x256). Use 'imgview <file>'.\n");
                    }
                } else {
                    terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
                    terminal_print("  Could not decode PNG.\n");
                }
            } else if (clen >= 3 && (uint8_t)content[0] == 0xFF &&
                       (uint8_t)content[1] == 0xD8 && (uint8_t)content[2] == 0xFF) {
                terminal_set_color(COLOR_LIGHT_GREEN,COLOR_BLACK);
                terminal_print("  Detected JPEG — opening in image viewer...\n");
                JpegImage img = {0};
                img.pixels = img_pixbuf;
                if (jpeg_decode((const uint8_t*)content, clen, &img)) {
                    int needed = (int)(img.width * img.height * 4);
                    if (needed <= IMG_PIXBUF_MAX) {
                        imgview_init((int)img.width, (int)img.height, img.pixels, name);
                        current_mode = MODE_IMGVIEW;
                    } else {
                        terminal_set_color(COLOR_YELLOW,COLOR_BLACK);
                        terminal_print("  Image too large (max 256x256). Use 'imgview <file>'.\n");
                    }
                } else {
                    terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
                    terminal_print("  Could not decode JPEG.\n");
                }
            // BMP file auto-detect
            } else if (clen >= 2 && content[0] == 'B' && content[1] == 'M') {
                terminal_set_color(COLOR_LIGHT_GREEN,COLOR_BLACK);
                terminal_print("  Detected BMP — opening in image viewer...\n");
                BmpImage img;
                img.pixels = img_pixbuf;
                if (bmp_decode(content, clen, &img)) {
                    int needed = (int)(img.width * img.height * 4);
                    if (needed <= IMG_PIXBUF_MAX) {
                        imgview_init((int)img.width, (int)img.height, img.pixels, name);
                        current_mode = MODE_IMGVIEW;
                    } else {
                        terminal_set_color(COLOR_YELLOW,COLOR_BLACK);
                        terminal_print("  Image too large (max 256x256). Use 'imgview <file>'.\n");
                    }
                } else {
                    terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
                    terminal_print("  Could not decode BMP.\n");
                }
            // GIF file auto-detect
            } else if (clen >= 6 && content[0] == 'G' && content[1] == 'I' && content[2] == 'F') {
                terminal_set_color(COLOR_LIGHT_GREEN,COLOR_BLACK);
                terminal_print("  Detected GIF — opening in image viewer...\n");
                GifImage img;
                img.pixels = img_pixbuf;
                if (gif_decode(content, clen, &img)) {
                    int needed = (int)(img.width * img.height * 4);
                    if (needed <= IMG_PIXBUF_MAX) {
                        imgview_init((int)img.width, (int)img.height, img.pixels, name);
                        current_mode = MODE_IMGVIEW;
                    } else {
                        terminal_set_color(COLOR_YELLOW,COLOR_BLACK);
                        terminal_print("  Image too large (max 256x256). Use 'imgview <file>'.\n");
                    }
                } else {
                    terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
                    terminal_print("  Could not decode GIF.\n");
                }
            // Auto-detect PDF files by their %PDF header
            } else if (clen >= 4 && content[0] == '%' && content[1] == 'P' &&
                content[2] == 'D' && content[3] == 'F') {
                char pdf_text[65536];
                int result = pdftext_extract(content, clen, pdf_text, 65536);

                terminal_set_color(COLOR_LIGHT_CYAN,COLOR_BLACK);
                terminal_print("  --- "); terminal_print(name);
                terminal_print(" (PDF text) ---\n\n");

                if (result > 0) {
                    terminal_set_color(COLOR_WHITE,COLOR_BLACK);
                    terminal_print("  ");
                    // Print with proper line wrapping
                    int pi = 0;
                    while (pdf_text[pi]) {
                        terminal_putchar(pdf_text[pi]);
                        if (pdf_text[pi] == '\n') terminal_print("  ");
                        pi++;
                    }
                    terminal_print("\n");
                } else if (result == 0) {
                    terminal_set_color(COLOR_YELLOW,COLOR_BLACK);
                    terminal_print("  No extractable text found in this PDF.\n");
                } else {
                    terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
                    terminal_print("  Could not parse this PDF.\n");
                    terminal_set_color(COLOR_LIGHT_GREY,COLOR_BLACK);
                }
            } else {
                // Normal file — show raw content
                terminal_set_color(COLOR_LIGHT_CYAN,COLOR_BLACK);
                terminal_print("  --- "); terminal_print(name); terminal_print(" ---\n");
                terminal_set_color(COLOR_WHITE,COLOR_BLACK);
                terminal_print("  "); terminal_print(content); terminal_print("\n");
            }
            terminal_set_color(COLOR_LIGHT_CYAN,COLOR_BLACK);
            terminal_print("  ------\n");
        }
    }
    else if(s_eq(cmd,"history")){ terminal_print("  [DEBUG] calling cmd_history\n"); cmd_history(); }
    else if(s_ncmp(cmd,"calc",4)==0){ terminal_print("  [DEBUG] calling cmd_calc\n"); cmd_calc(args); }
    else if(s_eq(cmd,"mem")){ terminal_print("  [DEBUG] calling cmd_mem\n"); cmd_mem(); }
    else if(s_eq(cmd,"ps")){ terminal_print("  [DEBUG] calling cmd_ps\n"); cmd_ps(); }
    else if(s_eq(cmd,"vmem")){ cmd_vmem(); }
    else if(s_ncmp(cmd,"sched",5)==0){ cmd_sched(args); }
    else if(s_eq(cmd,"uname")){ terminal_print("  [DEBUG] calling cmd_uname\n"); cmd_uname(); }
    else if(s_eq(cmd,"version")){ terminal_print("  [DEBUG] calling cmd_version\n"); cmd_version(); }
    else if(s_eq(cmd,"whoami")){ terminal_print("  [DEBUG] calling cmd_whoami\n"); cmd_whoami(); }
    else if(s_eq(cmd,"date")){ terminal_print("  [DEBUG] calling cmd_date\n"); cmd_date(); }
    else if(s_eq(cmd,"time")){ terminal_print("  [DEBUG] calling cmd_time_cmd\n"); cmd_time_cmd(); }
    else if(s_eq(cmd,"uptime")){ terminal_print("  [DEBUG] calling cmd_uptime\n"); cmd_uptime(); }
    else if(s_eq(cmd,"pwd")){ terminal_print("  [DEBUG] calling cmd_pwd\n"); cmd_pwd(); }
    else if(s_ncmp(cmd,"pdftext",7)==0) {
        // Check for --pages flag
        char flag[16];
        get_arg1(args,flag,16);
        int show_pages_only = s_eq(flag,"--pages");

        char name[MAX_FILENAME];
        if (show_pages_only) {
            get_arg1(get_arg2(args),name,MAX_FILENAME);
        } else {
            get_arg1(args,name,MAX_FILENAME);
        }

        if(!name[0]){
            terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
            terminal_print("  Usage: pdftext [--pages] <filename>\n");
            terminal_set_color(COLOR_LIGHT_GREY,COLOR_BLACK);
            terminal_print("    pdftext <file>       Extract text from a PDF\n");
            terminal_print("    pdftext --pages <file>  Show page count only\n");
        } else {
            int idx=find_file(name);
            if(idx<0){
                terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
                terminal_print("  File not found: ");
                terminal_print(name);
                terminal_print("\n");
            } else {
                const char* content = filesystem[idx].content;
                int clen = filesystem[idx].size;
                if (clen <= 0) { while (content[clen]) clen++; }

                if (show_pages_only) {
                    int pc = pdftext_get_page_count(content, clen);
                    if (pc > 0) {
                        terminal_set_color(COLOR_LIGHT_CYAN,COLOR_BLACK);
                        terminal_print("\n  ");
                        terminal_print(name);
                        terminal_print(": ");
                        terminal_set_color(COLOR_WHITE,COLOR_BLACK);
                        char pc_s[8]; n2s_sh(pc,pc_s);
                        terminal_print(pc_s);
                        terminal_print(pc == 1 ? " page" : " pages");
                        terminal_print("\n\n");
                    } else {
                        terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
                        terminal_print("  Could not parse this PDF.\n");
                    }
                } else {
                    terminal_set_color(COLOR_LIGHT_CYAN,COLOR_BLACK);
                    terminal_print("\n  --- ");
                    terminal_print(name);
                    terminal_print(" (PDF text) ---\n\n");

                    static char pdf_text[65536];
                    int result = pdftext_extract(content, clen, pdf_text, 65536);

                    if (result > 0) {
                        terminal_set_color(COLOR_WHITE,COLOR_BLACK);
                        terminal_print(pdf_text);
                        terminal_print("\n");
                    } else if (result == 0) {
                        terminal_set_color(COLOR_YELLOW,COLOR_BLACK);
                        terminal_print("  No extractable text found in this PDF.\n");
                    } else {
                        terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
                        terminal_print("  Could not parse this PDF.\n");
                        terminal_set_color(COLOR_LIGHT_GREY,COLOR_BLACK);
                        terminal_print("  The pdftext tool requires simple PDFs with\n");
                        terminal_print("  uncompressed text content streams.\n");
                        terminal_print("  Compressed or encrypted PDFs are not supported.\n");
                    }

                    terminal_set_color(COLOR_LIGHT_CYAN,COLOR_BLACK);
                    terminal_print("\n  ------\n\n");
                }
            }
        }
    }
    else if(s_ncmp(cmd,"viewhtml",8)==0) {
        char name[MAX_FILENAME];
        get_arg1(args,name,MAX_FILENAME);
        if(!name[0]){
            terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
            terminal_print("  Usage: viewhtml <filename>\n");
        } else {
            int idx=find_file(name);
            if(idx<0){
                terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
                terminal_print("  File not found: ");
                terminal_print(name);
                terminal_print("\n");
            } else {
                const char* content = filesystem[idx].content;
                // Check for HTML content
                int looks_like_html = 0;
                for (int ci = 0; content[ci] && ci < 40; ci++) {
                    if (content[ci] == '<') {
                        int next = content[ci+1];
                        if ((next >= 'a' && next <= 'z') ||
                            (next >= 'A' && next <= 'Z') ||
                            next == '!' || next == '/') {
                            looks_like_html = 1;
                        }
                        break;
                    }
                    if (content[ci] != ' ' && content[ci] != '\t' &&
                        content[ci] != '\n' && content[ci] != '\r')
                        break;
                }
                if (looks_like_html) {
                    terminal_set_color(COLOR_LIGHT_GREEN,COLOR_BLACK);
                    terminal_print("  Opening ");
                    terminal_print(name);
                    terminal_print(" in HTML viewer...\n");
                    htmlview_render(content, name);

                    // Check if user pressed ENTER on a link
                    const char* pending = htmlview_get_pending_url();
                    if (pending && pending[0]) {
                        terminal_set_color(COLOR_LIGHT_CYAN,COLOR_BLACK);
                        terminal_print("  Link: ");
                        terminal_print(pending);
                        terminal_print("\n");
                        terminal_set_color(COLOR_LIGHT_GREY,COLOR_BLACK);
                        terminal_print("  Local files cannot navigate to URLs without a host.\n");
                        terminal_print("  Use 'http get <host> <path>' to fetch a page first.\n");
                    }
                } else {
                    terminal_set_color(COLOR_YELLOW,COLOR_BLACK);
                    terminal_print("  ");
                    terminal_print(name);
                    terminal_print(" does not appear to be an HTML file.\n");
                    terminal_set_color(COLOR_LIGHT_GREY,COLOR_BLACK);
                    terminal_print("  Use 'open <file>' to view raw content.\n");
                }
            }
        }
    }
    else if(s_ncmp(cmd,"imgview",7)==0) {
        char name[MAX_FILENAME];
        get_arg1(args,name,MAX_FILENAME);
        if (!name[0]) {
            terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
            terminal_print("  Usage: imgview <filename>\n");
        } else {
            int idx = find_file(name);
            if (idx < 0) {
                terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
                terminal_print("  File not found: ");
                terminal_print(name);
                terminal_print("\n");
            } else {
                const uint8_t* content = (const uint8_t*)filesystem[idx].content;
                int clen = filesystem[idx].size;
                if (clen <= 0) { while (content[clen]) clen++; }

                if (clen < 4) {
                    terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
                    terminal_print("  File is too small to be an image.\n");
                } else if (content[0] == 0x89 && content[1] == 'P' &&
                           content[2] == 'N' && content[3] == 'G') {
                    // PNG file
                    terminal_set_color(COLOR_LIGHT_GREEN,COLOR_BLACK);
                    terminal_print("  Decoding PNG: ");
                    terminal_print(name);
                    terminal_print("...\n");

                    PngImage img;
                    img.pixels = img_pixbuf;
                    if (png_decode(content, clen, &img)) {
                        int needed = img.width * img.height * 4;
                        if (needed <= IMG_PIXBUF_MAX) {
                            imgview_init((int)img.width, (int)img.height, img.pixels, name);
                            current_mode = MODE_IMGVIEW;
                        } else {
                            terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
                            terminal_print("  Image too large! Max 256x256.\n");
                        }
                    } else {
                        terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
                        terminal_print("  Failed to decode PNG.\n");
                        terminal_set_color(COLOR_LIGHT_GREY,COLOR_BLACK);
                        terminal_print("  Requires 8-bit RGB/RGBA, non-interlaced.\n");
                    }
                } else if (content[0] == 0xFF && content[1] == 0xD8 &&
                           content[2] == 0xFF) {
                    // JPEG file
                    terminal_set_color(COLOR_LIGHT_GREEN,COLOR_BLACK);
                    terminal_print("  Decoding JPEG: ");
                    terminal_print(name);
                    terminal_print("...\n");

                    JpegImage img = {0};
                    img.pixels = img_pixbuf;
                    if (jpeg_decode(content, clen, &img)) {
                        int needed = img.width * img.height * 4;
                        if (needed <= IMG_PIXBUF_MAX) {
                            imgview_init((int)img.width, (int)img.height, img.pixels, name);
                            current_mode = MODE_IMGVIEW;
                        } else {
                            terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
                            terminal_print("  Image too large! Max 256x256.\n");
                        }
                    } else {
                        terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
                        terminal_print("  Failed to decode JPEG.\n");
                        terminal_set_color(COLOR_LIGHT_GREY,COLOR_BLACK);
                        terminal_print("  Requires baseline JPEG with YCbCr 4:2:0.\n");
                    }
                } else if (content[0] == 'B' && content[1] == 'M') {
                    // BMP file
                    terminal_set_color(COLOR_LIGHT_GREEN,COLOR_BLACK);
                    terminal_print("  Decoding BMP: ");
                    terminal_print(name);
                    terminal_print("...\n");

                    BmpImage img;
                    img.pixels = img_pixbuf;
                    if (bmp_decode(content, clen, &img)) {
                        int needed = img.width * img.height * 4;
                        if (needed <= IMG_PIXBUF_MAX) {
                            imgview_init((int)img.width, (int)img.height, img.pixels, name);
                            current_mode = MODE_IMGVIEW;
                        } else {
                            terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
                            terminal_print("  Image too large! Max 256x256.\n");
                        }
                    } else {
                        terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
                        terminal_print("  Failed to decode BMP.\n");
                        terminal_set_color(COLOR_LIGHT_GREY,COLOR_BLACK);
                        terminal_print("  Requires 24/32-bit uncompressed BMP.\n");
                    }
                } else if (content[0] == 'G' && content[1] == 'I' && content[2] == 'F') {
                    // GIF file
                    terminal_set_color(COLOR_LIGHT_GREEN,COLOR_BLACK);
                    terminal_print("  Decoding GIF: ");
                    terminal_print(name);
                    terminal_print("...\n");

                    GifImage img;
                    img.pixels = img_pixbuf;
                    if (gif_decode(content, clen, &img)) {
                        int needed = img.width * img.height * 4;
                        if (needed <= IMG_PIXBUF_MAX) {
                            imgview_init((int)img.width, (int)img.height, img.pixels, name);
                            current_mode = MODE_IMGVIEW;
                        } else {
                            terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
                            terminal_print("  Image too large! Max 256x256.\n");
                        }
                    } else {
                        terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
                        terminal_print("  Failed to decode GIF.\n");
                        terminal_set_color(COLOR_LIGHT_GREY,COLOR_BLACK);
                        terminal_print("  Requires standard GIF87a/89a.\n");
                    }
                } else {
                    terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
                    terminal_print("  Not a recognized image format.\n");
                    terminal_set_color(COLOR_LIGHT_GREY,COLOR_BLACK);
                    terminal_print("  Supported: PNG (.png), JPEG (.jpg/.jpeg), BMP (.bmp), GIF (.gif)\n");
                }
            }
        }
    }
    else if(s_ncmp(cmd,"scanprotect",11)==0) cmd_scanprotect(args);
    else if(s_ncmp(cmd,"pci",3)==0){
        char sub[16];
        get_arg1(args,sub,16);
        if (s_eq(sub,"list") || sub[0]=='\0') {
            net_scan_pci();
            if (net_adapter_count == 0) {
                terminal_set_color(COLOR_YELLOW,COLOR_BLACK);
                terminal_print("  No PCI network adapters found.\n");
                terminal_set_color(COLOR_LIGHT_GREY,COLOR_BLACK);
                terminal_print("  Ensure QEMU has a NIC configured:\n");
                terminal_print("    -nic user,model=e1000\n");
                terminal_print("  or for TAP networking:\n");
                terminal_print("    -device e1000,netdev=net0\n");
            } else {
                terminal_set_color(COLOR_LIGHT_CYAN,COLOR_BLACK);
                terminal_print("\n  PCI Network Adapters Found: ");
                char cnt_s[4]; n2s_sh(net_adapter_count,cnt_s);
                terminal_print(cnt_s);
                terminal_print("\n");
                terminal_set_color(COLOR_DARK_GREY,COLOR_BLACK);
                terminal_print("  Bus Slot  Vendor             Device                   Type       IRQ  Supported\n");
                terminal_print("  --- ----  ----------------  -----------------------  ---------  ----  ---------\n");
                for (int i = 0; i < net_adapter_count; i++) {
                    const NetAdapter* a = &net_adapters[i];
                    terminal_set_color(COLOR_WHITE,COLOR_BLACK);
                    terminal_print("  ");
                    char bs[4]; n2s_sh(a->bus,bs); terminal_print(bs);
                    terminal_print("  ");
                    char ss[4]; n2s_sh(a->slot,ss);
                    if (a->slot < 10) terminal_print(" ");
                    terminal_print(ss);
                    terminal_print("    ");
                    // Vendor name (right-padded to 16)
                    terminal_print(a->vendor_name);
                    int vlen = s_len(a->vendor_name);
                    for (int p = vlen; p < 16; p++) terminal_putchar(' ');
                    terminal_print("  ");
                    // Device name (right-padded to 23)
                    terminal_print(a->device_name);
                    int dlen = s_len(a->device_name);
                    for (int p = dlen; p < 23; p++) terminal_putchar(' ');
                    terminal_print("  ");
                    terminal_print(a->type_name);
                    int tlen = s_len(a->type_name);
                    for (int p = tlen; p < 9; p++) terminal_putchar(' ');
                    terminal_print("  ");
                    // IRQ line (0 = none / unassigned)
                    char is[4];
                    if (a->irq_line == 0) { terminal_print("--"); }
                    else { n2s_sh(a->irq_line, is); terminal_print(is); }
                    terminal_print("   ");
                    if (a->supported) {
                        terminal_set_color(COLOR_LIGHT_GREEN,COLOR_BLACK);
                        terminal_print("Yes");
                    } else {
                        terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
                        terminal_print("No ");
                    }
                    terminal_set_color(COLOR_WHITE,COLOR_BLACK);
                    terminal_print("\n");
                }
                terminal_print("\n");
                terminal_set_color(COLOR_LIGHT_GREY,COLOR_BLACK);
                terminal_print("  Check 'http init' to initialize the supported NIC.\n");
            }
            terminal_print("\n");
        } else {
            terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
            terminal_print("  Usage: pci list\n");
        }
    }
    else if(s_ncmp(cmd,"net",3)==0) {
        char sub[16];
        get_arg1(args,sub,16);
        if (s_eq(sub,"config")) {
            // Check if first arg is "dns" → configure DNS separately
            char first_arg[16];
            get_arg1(get_arg2(args),first_arg,16);
            if (s_eq(first_arg,"dns")) {
                char dns_str[20];
                get_arg1(get_arg2(get_arg2(args)),dns_str,20);
                if (!dns_str[0]) {
                    terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
                    terminal_print("  Usage: net config dns <dns_ip>\n");
                    terminal_print("  Example: net config dns 8.8.8.8\n");
                } else {
                    uint32_t dns_ip = 0;
                    int octet = 0, shift = 0;
                    for (int ci = 0; dns_str[ci] && ci < 20; ci++) {
                        if (dns_str[ci] >= '0' && dns_str[ci] <= '9')
                            octet = octet * 10 + (dns_str[ci] - '0');
                        else if (dns_str[ci] == '.') {
                            dns_ip |= (octet << shift);
                            octet = 0; shift += 8;
                        }
                    }
                    dns_ip |= (octet << shift);
                    netstack_set_dns(dns_ip);
                    terminal_set_color(COLOR_LIGHT_GREEN,COLOR_BLACK);
                    terminal_print("  DNS server configured: ");
                    terminal_print(dns_str);
                    terminal_print("\n");
                }
            } else {
                char ip_str[20], gw_str[20], dns_str[20];
                get_arg1(get_arg2(args),ip_str,20);
                get_arg1(get_arg2(get_arg2(args)),gw_str,20);
                get_arg1(get_arg2(get_arg2(get_arg2(args))),dns_str,20);
                if (!ip_str[0] || !gw_str[0]) {
                    terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
                    terminal_print("  Usage: net config <ip> <gateway> [dns_ip]\n");
                    terminal_print("         net config dns <dns_ip>\n");
                    terminal_print("  Examples:\n");
                    terminal_print("    net config 192.168.100.2 192.168.100.1\n");
                    terminal_print("    net config 192.168.100.2 192.168.100.1 8.8.8.8\n");
                    terminal_print("    net config dns 8.8.8.8\n");
                    terminal_set_color(COLOR_LIGHT_GREY,COLOR_BLACK);
                } else {
                    // Parse dotted IPs
                    uint32_t ip = 0, gw = 0;
                    int octet = 0, shift = 0;
                    for (int ci = 0; ip_str[ci] && ci < 20; ci++) {
                        if (ip_str[ci] >= '0' && ip_str[ci] <= '9')
                            octet = octet * 10 + (ip_str[ci] - '0');
                        else if (ip_str[ci] == '.') {
                            ip |= (octet << shift);
                            octet = 0; shift += 8;
                        }
                    }
                    ip |= (octet << shift);
                    octet = 0; shift = 0;
                    for (int ci = 0; gw_str[ci] && ci < 20; ci++) {
                        if (gw_str[ci] >= '0' && gw_str[ci] <= '9')
                            octet = octet * 10 + (gw_str[ci] - '0');
                        else if (gw_str[ci] == '.') {
                            gw |= (octet << shift);
                            octet = 0; shift += 8;
                        }
                    }
                    gw |= (octet << shift);

                    // Parse optional DNS argument
                    uint32_t dns_ip = 0;
                    if (dns_str[0]) {
                        octet = 0; shift = 0;
                        for (int ci = 0; dns_str[ci] && ci < 20; ci++) {
                            if (dns_str[ci] >= '0' && dns_str[ci] <= '9')
                                octet = octet * 10 + (dns_str[ci] - '0');
                            else if (dns_str[ci] == '.') {
                                dns_ip |= (octet << shift);
                                octet = 0; shift += 8;
                            }
                        }
                        dns_ip |= (octet << shift);
                    }

                    netstack_set_ip(ip, gw);
                    if (dns_ip) netstack_set_dns(dns_ip);

                    terminal_set_color(COLOR_LIGHT_GREEN,COLOR_BLACK);
                    terminal_print("  Network configured:\n");
                    terminal_print("    IP:      "); terminal_print(ip_str); terminal_print("\n");
                    terminal_print("    Gateway: "); terminal_print(gw_str); terminal_print("\n");
                    if (dns_ip) {
                        terminal_print("    DNS:     "); terminal_print(dns_str); terminal_print("\n");
                    } else {
                        // Show current DNS
                        uint32_t cdns = netstack_get_dns();
                        terminal_print("    DNS:     ");
                        for (int bi = 0; bi < 4; bi++) {
                            int b = (cdns >> (bi * 8)) & 0xFF;
                            char buf[8]; int bp = 0;
                            if (b >= 100) buf[bp++] = '0' + b/100;
                            if (b >= 10) buf[bp++] = '0' + (b/10)%10;
                            buf[bp++] = '0' + b%10; buf[bp] = '\0';
                            terminal_print(buf);
                            if (bi < 3) terminal_print(".");
                        }
                        terminal_print(" (unchanged)\n");
                    }
                    terminal_print("  (run 'net config dns <ip>' to change DNS)\n");
                    terminal_set_color(COLOR_LIGHT_GREY,COLOR_BLACK);
                }
            }
        } else if (s_eq(sub,"status")) {
            uint32_t ip = netstack_get_ip();
            uint32_t dns = netstack_get_dns();
            terminal_set_color(COLOR_LIGHT_CYAN,COLOR_BLACK);
            terminal_print("  Network Status:\n");
            terminal_print("  ------------------\n");
            terminal_set_color(COLOR_WHITE,COLOR_BLACK);
            terminal_print("  IP:   ");
            // Display IP in dot notation
            for (int bi = 0; bi < 4; bi++) {
                int b = (ip >> (bi * 8)) & 0xFF;
                char buf[8]; int bp = 0;
                if (b >= 100) buf[bp++] = '0' + b/100;
                if (b >= 10) buf[bp++] = '0' + (b/10)%10;
                buf[bp++] = '0' + b%10; buf[bp] = '\0';
                terminal_print(buf);
                if (bi < 3) terminal_print(".");
            }
            terminal_print("\n");
            // Display DNS in dot notation
            terminal_print("  DNS:  ");
            for (int bi = 0; bi < 4; bi++) {
                int b = (dns >> (bi * 8)) & 0xFF;
                char buf[8]; int bp = 0;
                if (b >= 100) buf[bp++] = '0' + b/100;
                if (b >= 10) buf[bp++] = '0' + (b/10)%10;
                buf[bp++] = '0' + b%10; buf[bp] = '\0';
                terminal_print(buf);
                if (bi < 3) terminal_print(".");
            }
            terminal_print("\n\n");
            terminal_print("  Commands:\n");
            terminal_print("    net config <ip> <gw> [dns]  Set IP, gateway, DNS\n");
            terminal_print("    net config dns <ip>         Set DNS server only\n");
            terminal_print("    http init                   Initialize NIC\n");
            terminal_set_color(COLOR_LIGHT_GREY,COLOR_BLACK);
        } else if (s_eq(sub,"dhcp")) {
            // Auto-init NIC if not already done
            if (!nic_is_up()) {
                terminal_set_color(COLOR_LIGHT_GREY,COLOR_BLACK);
                terminal_print("  NIC not initialized — auto-initializing...\n");
                if (!nic_init()) {
                    terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
                    terminal_print("  No supported NIC found!\n");
                    terminal_set_color(COLOR_LIGHT_GREY,COLOR_BLACK);
                    terminal_print("  Run 'pci list' to see available adapters.\n");
                    terminal_print("  Then run 'http init' manually.\n\n");
                    return; // can't continue early, so we skip dhcp
                } else {
                    netstack_init();
                    terminal_set_color(COLOR_LIGHT_GREEN,COLOR_BLACK);
                    terminal_print("  NIC initialized.\n");
                }
            } else {
                // NIC is initialized but stack might need a refresh
                // (netstack_init is safe to call multiple times)
                netstack_init();
            }

            terminal_set_color(COLOR_LIGHT_CYAN,COLOR_BLACK);
            terminal_print("\n  DHCP DISCOVER: sending broadcast...\n");
            terminal_set_color(COLOR_LIGHT_GREY,COLOR_BLACK);
            terminal_print("  (waiting for DHCP OFFER, ~5 seconds)\n\n");

            DhcpResult dhcp_res;
            if (dhcp_discover(&dhcp_res)) {
                // Apply the configuration
                netstack_set_ip(dhcp_res.yiaddr, dhcp_res.gateway);
                if (dhcp_res.dns)
                    netstack_set_dns(dhcp_res.dns);

                terminal_set_color(COLOR_LIGHT_GREEN,COLOR_BLACK);
                terminal_print("  ✅ DHCP OFFER received! Configuring network:\n\n");

                // Display IP
                terminal_set_color(COLOR_WHITE,COLOR_BLACK);
                terminal_print("    IP:      ");
                uint32_t ip = dhcp_res.yiaddr;
                for (int bi = 0; bi < 4; bi++) {
                    int b = (ip >> (bi * 8)) & 0xFF;
                    char buf[8]; int bp = 0;
                    if (b >= 100) buf[bp++] = '0' + b/100;
                    if (b >= 10) buf[bp++] = '0' + (b/10)%10;
                    buf[bp++] = '0' + b%10; buf[bp] = '\0';
                    terminal_print(buf);
                    if (bi < 3) terminal_print(".");
                }
                terminal_print("\n");

                // Display subnet mask
                if (dhcp_res.subnet) {
                    terminal_print("    Mask:    ");
                    uint32_t sm = dhcp_res.subnet;
                    for (int bi = 0; bi < 4; bi++) {
                        int b = (sm >> (bi * 8)) & 0xFF;
                        char buf[8]; int bp = 0;
                        if (b >= 100) buf[bp++] = '0' + b/100;
                        if (b >= 10) buf[bp++] = '0' + (b/10)%10;
                        buf[bp++] = '0' + b%10; buf[bp] = '\0';
                        terminal_print(buf);
                        if (bi < 3) terminal_print(".");
                    }
                    terminal_print("\n");
                }

                // Display gateway
                if (dhcp_res.gateway) {
                    terminal_print("    Gateway: ");
                    uint32_t gw = dhcp_res.gateway;
                    for (int bi = 0; bi < 4; bi++) {
                        int b = (gw >> (bi * 8)) & 0xFF;
                        char buf[8]; int bp = 0;
                        if (b >= 100) buf[bp++] = '0' + b/100;
                        if (b >= 10) buf[bp++] = '0' + (b/10)%10;
                        buf[bp++] = '0' + b%10; buf[bp] = '\0';
                        terminal_print(buf);
                        if (bi < 3) terminal_print(".");
                    }
                    terminal_print("\n");
                }

                // Display DNS
                if (dhcp_res.dns) {
                    terminal_print("    DNS:     ");
                    uint32_t dns_ip = dhcp_res.dns;
                    for (int bi = 0; bi < 4; bi++) {
                        int b = (dns_ip >> (bi * 8)) & 0xFF;
                        char buf[8]; int bp = 0;
                        if (b >= 100) buf[bp++] = '0' + b/100;
                        if (b >= 10) buf[bp++] = '0' + (b/10)%10;
                        buf[bp++] = '0' + b%10; buf[bp] = '\0';
                        terminal_print(buf);
                        if (bi < 3) terminal_print(".");
                    }
                    terminal_print("\n");
                }

                // Display lease time
                if (dhcp_res.lease) {
                    terminal_print("\n    Lease:   ");
                    char ls[16]; n2s_sh((int)dhcp_res.lease, ls);
                    terminal_print(ls);
                    terminal_print(" seconds");
                    int hours = dhcp_res.lease / 3600;
                    if (hours > 0) {
                        terminal_print(" (");
                        char hs[8]; n2s_sh(hours, hs);
                        terminal_print(hs);
                        terminal_print(" hours)");
                    }
                    terminal_print("\n");
                }

                terminal_set_color(COLOR_LIGHT_CYAN,COLOR_BLACK);
                terminal_print("\n  ✅ Network configured! You can now use the Browser.\n");
            } else {
                terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
                terminal_print("  ❌ No DHCP OFFER received.\n");
                terminal_print("  Check that:\n");
                terminal_set_color(COLOR_LIGHT_GREY,COLOR_BLACK);
                terminal_print("    • The NIC is initialized (run 'http init')\n");
                terminal_print("    • A DHCP server is active on your network\n");
                terminal_print("    • QEMU user-mode or TAP networking is configured\n");
            }
            terminal_print("\n");
        } else if (s_eq(sub,"scan")) {
            uint32_t ip = netstack_get_ip();
            // Compute subnet base from our IP (zero out host byte)
            uint32_t subnet_base = ip & 0xFFFFFF00;

            terminal_set_color(COLOR_LIGHT_CYAN,COLOR_BLACK);
            terminal_print("\n  Scanning subnet with ARP requests...\n");
            terminal_set_color(COLOR_LIGHT_GREY,COLOR_BLACK);
            terminal_print("  This will take a few seconds.\n\n");

            int count = netstack_arp_scan(subnet_base);

            if (count == 0) {
                terminal_set_color(COLOR_YELLOW,COLOR_BLACK);
                terminal_print("  No devices responded to ARP requests.\n");
                terminal_print("  Ensure the NIC is initialized (run 'http init')");
                terminal_print(" and check\n");
                terminal_print("  your network config (run 'net status').\n\n");
            } else {
                terminal_set_color(COLOR_LIGHT_GREEN,COLOR_BLACK);
                terminal_print("  Found ");
                char cnt_s[4]; n2s_sh(count,cnt_s);
                terminal_print(cnt_s);
                terminal_print(" device(s):\n\n");

                terminal_set_color(COLOR_DARK_GREY,COLOR_BLACK);
                terminal_print("     IP                MAC\n");
                terminal_print("  --- -------------  ----------------\n");

                for (int i = 0; i < count; i++) {
                    const ArpScanResult* r = netstack_scan_result(i);
                    terminal_set_color(COLOR_WHITE,COLOR_BLACK);
                    terminal_print("  ");
                    char idx_s[4]; n2s_sh(i+1,idx_s);
                    if (i+1 < 10) terminal_print(" ");
                    terminal_print(idx_s);
                    terminal_print("  ");

                    // Display IP in dot notation
                    uint32_t dip = r->ip;
                    for (int bi = 0; bi < 4; bi++) {
                        int b = (dip >> (bi * 8)) & 0xFF;
                        char buf[8]; int bp = 0;
                        if (b >= 100) buf[bp++] = '0' + b/100;
                        if (b >= 10) buf[bp++] = '0' + (b/10)%10;
                        buf[bp++] = '0' + b%10; buf[bp] = '\0';
                        terminal_print(buf);
                        if (bi < 3) terminal_print(".");
                    }
                    terminal_print("  ");

                    // Display MAC
                    char hex[3];
                    for (int mi = 0; mi < 6; mi++) {
                        hex[0] = "0123456789ABCDEF"[r->mac[mi] >> 4];
                        hex[1] = "0123456789ABCDEF"[r->mac[mi] & 0x0F];
                        hex[2] = '\0';
                        terminal_print(hex);
                        if (mi < 5) terminal_print(":");
                    }
                    terminal_print("\n");
                }

                terminal_set_color(COLOR_LIGHT_GREY,COLOR_BLACK);
                terminal_print("\n  (This is an ARP cache scan — only devices that\n");
                terminal_print("   responded to ARP requests are shown.)\n");
            }
            terminal_print("\n");
        } else {
            terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
            terminal_print("  Usage: net config <ip> <gateway> [dns]\n");
            terminal_print("         net config dns <dns_ip>\n");
            terminal_print("         net status\n");
            terminal_print("         net scan\n");
            terminal_print("\n");
            terminal_print("  Examples:\n");
            terminal_print("    net config 192.168.100.2 192.168.100.1\n");
            terminal_print("    net config 192.168.100.2 192.168.100.1 8.8.8.8\n");
            terminal_print("    net config dns 8.8.8.8\n");
            terminal_print("    net config dns 10.0.2.3\n");
            terminal_set_color(COLOR_LIGHT_GREY,COLOR_BLACK);
        }
    }
    else if(s_ncmp(cmd,"http",4)==0) {
        char sub[16];
        get_arg1(args,sub,16);
        if (s_eq(sub,"init")) {
            if (nic_init()) {
                netstack_init();
                terminal_set_color(COLOR_LIGHT_GREEN,COLOR_BLACK);
                terminal_print("  Ethernet NIC initialized (");
                terminal_print(nic_driver_name());
                terminal_print(").\n");
                terminal_set_color(COLOR_DARK_GREY,COLOR_BLACK);
                terminal_print("  MAC: ");
                uint8_t mac[6]; nic_get_mac(mac);
                char hex[3];
                for (int mi = 0; mi < 6; mi++) {
                    hex[0] = "0123456789ABCDEF"[mac[mi] >> 4];
                    hex[1] = "0123456789ABCDEF"[mac[mi] & 0x0F];
                    hex[2] = '\0';
                    terminal_print(hex);
                    if (mi < 5) terminal_print(":");
                }
                terminal_print("\n");
            } else {
                terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
                terminal_print("  No supported NIC found!\n");
                // NIC not found — run PCI scan to show what's available
                net_scan_pci();
                if (net_adapter_count > 0) {
                    terminal_set_color(COLOR_YELLOW,COLOR_BLACK);
                    terminal_print("  However, ");
                    char cnt_s[4]; n2s_sh(net_adapter_count,cnt_s);
                    terminal_print(cnt_s);
                    terminal_print(" PCI network adapter(s) were found.\n");
                    terminal_set_color(COLOR_LIGHT_GREY,COLOR_BLACK);
                    terminal_print("  Run 'pci list' to see them. The built-in NIC drivers\n");
                    terminal_print("  may not support your NIC.\n");
                } else {
                    terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
                    terminal_print("  No PCI network adapters found at all.\n");
                    terminal_set_color(COLOR_LIGHT_GREY,COLOR_BLACK);
                    terminal_print("  Ensure QEMU has a NIC configured:\n");
                    terminal_print("    -nic user,model=e1000\n");
                    terminal_print("  or for TAP networking:\n");
                    terminal_print("    -device e1000,netdev=net0\n");
                    terminal_print("\n");
                    terminal_print("  Current QEMU command used:\n");
                    terminal_print("    make run (user-mode NIC)\n");
                }
            }
        } else if (s_eq(sub,"get")) {
            char ip_or_host[64];
            get_arg1(get_arg2(args),ip_or_host,64);
            char path_str[128];
            get_arg1(get_arg2(get_arg2(args)),path_str,128);
            if (!ip_or_host[0] || !path_str[0]) {
                terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
                terminal_print("  Usage: http get <host> <path>\n");
                terminal_set_color(COLOR_LIGHT_GREY,COLOR_BLACK);
                terminal_print("  Examples:\n");
                terminal_print("    http get 10.0.2.2 /index.html\n");
                terminal_print("    http get example.com /\n");
            } else {
                uint32_t ip = 0;
                int is_dotted = 0;
                // Check if it's a dotted IP or hostname
                for (int ci = 0; ip_or_host[ci] && ci < 64; ci++) {
                    if (ip_or_host[ci] == '.') is_dotted = 1;
                }

                if (is_dotted) {
                    // Parse IP from dot notation
                    int octet = 0, shift = 0;
                    for (int ci = 0; ip_or_host[ci] && ci < 32; ci++) {
                        if (ip_or_host[ci] >= '0' && ip_or_host[ci] <= '9')
                            octet = octet * 10 + (ip_or_host[ci] - '0');
                        else if (ip_or_host[ci] == '.') {
                            ip |= (octet << shift);
                            octet = 0;
                            shift += 8;
                        }
                    }
                    ip |= (octet << shift);
                }

                terminal_set_color(COLOR_LIGHT_CYAN,COLOR_BLACK);
                terminal_print("  Resolving ");
                terminal_print(ip_or_host);
                terminal_print("...\n");

                // Resolve hostname if needed (also handles hostnames
                // containing dots like "example.com" that fail IP parsing)
                if (!is_dotted || ip == 0) {
                    ip = dns_resolve(ip_or_host);
                }

                if (ip == 0) {
                    terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
                    terminal_print("  Could not resolve hostname!\n");
                } else {
                    // Show resolved IP
                    terminal_set_color(COLOR_LIGHT_GREEN,COLOR_BLACK);
                    terminal_print("  Connecting to ");
                    terminal_print(ip_or_host);
                    terminal_print(" (");
                    // Display resolved IP in dot notation
                    char ip_disp[20];
                    int dispi = 0;
                    // Extract bytes from LE wire format (LSB = first octet)
                    for (int bi = 0; bi < 4; bi++) {
                        int b = (ip >> (bi * 8)) & 0xFF;
                        if (b >= 100) ip_disp[dispi++] = '0' + b/100;
                        if (b >= 10) ip_disp[dispi++] = '0' + (b/10)%10;
                        ip_disp[dispi++] = '0' + b%10;
                        if (bi > 0) ip_disp[dispi++] = '.';
                    }
                    ip_disp[dispi] = '\0';
                    terminal_print(ip_disp);
                    terminal_print(")");
                    terminal_print(path_str);
                    terminal_print("...\n\n");

                    HttpResponse resp;
                    int result = http_get(ip, 80, ip_or_host, path_str, &resp);

                    if (result == 0) {
                        terminal_set_color(COLOR_LIGHT_GREEN,COLOR_BLACK);
                        terminal_print("  HTTP ");
                        char sc_s[8]; n2s_sh(resp.status_code,sc_s);
                        terminal_print(sc_s);
                        terminal_print(" ");
                        terminal_print(resp.status_text);
                        terminal_print("\n\n");

                        terminal_set_color(COLOR_WHITE,COLOR_BLACK);

                        // Auto-detect HTML: route to viewer if content looks like HTML
                        int looks_like_html = 0;
                        const char* b = resp.body;
                        int bl = 0;
                        while (b[bl]) bl++;
                        // Check for <html, <!DOCTYPE, <head, <body, or < followed by a letter
                        for (int bi = 0; bi < bl && bi < 80; bi++) {
                            if (b[bi] == '<') {
                                if ((b[bi+1] >= 'a' && b[bi+1] <= 'z') ||
                                    (b[bi+1] >= 'A' && b[bi+1] <= 'Z') ||
                                    b[bi+1] == '!' || b[bi+1] == '/') {
                                    looks_like_html = 1;
                                }
                                break;
                            }
                            if (b[bi] != ' ' && b[bi] != '\t' &&
                                b[bi] != '\n' && b[bi] != '\r')
                                break;  // non-whitespace, non-< → not HTML
                        }

                        if (looks_like_html && bl > 10) {
                            terminal_print("  (rendering in HTML viewer...)\n");
                            htmlview_render(resp.body, ip_or_host);

                            // ── Link navigation ──
                            // Check if user pressed ENTER on a link.
                            // If so, resolve the URL and fetch it.
                            const char* pending = htmlview_get_pending_url();
                            if (pending && pending[0]) {
                                terminal_set_color(COLOR_LIGHT_CYAN,COLOR_BLACK);
                                terminal_print("  Following link: ");
                                terminal_print(pending);
                                terminal_print("\n");

                                // Resolve the URL
                                char target_host[64];
                                char target_path[128];
                                // Check if it's a full URL (http://...)
                                if (s_ncmp(pending, "http://", 7) == 0) {
                                    // Parse host from URL
                                    int hi = 7;
                                    int hp = 0;
                                    while (pending[hi] && pending[hi] != '/' &&
                                           hp < 63) {
                                        target_host[hp++] = pending[hi++];
                                    }
                                    target_host[hp] = '\0';
                                    // Rest is path
                                    int pp = 0;
                                    while (pending[hi] && pp < 127) {
                                        target_path[pp++] = pending[hi++];
                                    }
                                    target_path[pp] = '\0';
                                    if (target_path[0] == '\0') {
                                        target_path[0] = '/';
                                        target_path[1] = '\0';
                                    }
                                } else if (pending[0] == '/') {
                                    // Absolute path — use same host
                                    s_cpy(target_host, ip_or_host, 64);
                                    s_cpy(target_path, pending, 128);
                                } else {
                                    // Relative path — resolve against
                                    // the original path's directory
                                    s_cpy(target_host, ip_or_host, 64);
                                    // Extract directory from original path
                                    int last_slash = -1;
                                    for (int si = 0; path_str[si]; si++) {
                                        if (path_str[si] == '/') last_slash = si;
                                    }
                                    if (last_slash >= 0) {
                                        // Copy directory part
                                        int di = 0;
                                        for (int si = 0; si <= last_slash && di < 127; si++) {
                                            target_path[di++] = path_str[si];
                                        }
                                        target_path[di] = '\0';
                                        // Append relative path
                                        int ri = 0;
                                        while (pending[ri] && di < 127) {
                                            target_path[di++] = pending[ri++];
                                        }
                                        target_path[di] = '\0';
                                    } else {
                                        // No directory — just use "/" + relative
                                        target_path[0] = '/';
                                        int ri = 0;
                                        int di = 1;
                                        while (pending[ri] && di < 127) {
                                            target_path[di++] = pending[ri++];
                                        }
                                        target_path[di] = '\0';
                                    }
                                }

                                // Resolve hostname to IP
                                uint32_t new_ip = 0;
                                int is_dotted = 0;
                                for (int ci = 0; target_host[ci] && ci < 64; ci++) {
                                    if (target_host[ci] == '.') is_dotted = 1;
                                }
                                if (is_dotted) {
                                    int octet = 0, shift = 0;
                                    for (int ci = 0; target_host[ci] && ci < 32; ci++) {
                                        if (target_host[ci] >= '0' && target_host[ci] <= '9')
                                            octet = octet * 10 + (target_host[ci] - '0');
                                        else if (target_host[ci] == '.') {
                                            new_ip |= (octet << shift);
                                            octet = 0;
                                            shift += 8;
                                        }
                                    }
                                    new_ip |= (octet << shift);
                                }
                                if (!is_dotted || new_ip == 0) {
                                    new_ip = dns_resolve(target_host);
                                }

                                if (new_ip == 0) {
                                    terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
                                    terminal_print("  Could not resolve host: ");
                                    terminal_print(target_host);
                                    terminal_print("\n");
                                } else {
                                    terminal_set_color(COLOR_LIGHT_GREEN,COLOR_BLACK);
                                    terminal_print("  Fetching ");
                                    terminal_print(target_host);
                                    terminal_print(target_path);
                                    terminal_print("...\n\n");

                                    HttpResponse resp2;
                                    int result = http_get(new_ip, 80, target_host,
                                                          target_path, &resp2);

                                    if (result == 0) {
                                        terminal_set_color(COLOR_LIGHT_GREEN,COLOR_BLACK);
                                        terminal_print("  HTTP ");
                                        char sc_s[8]; n2s_sh(resp2.status_code,sc_s);
                                        terminal_print(sc_s);
                                        terminal_print(" ");
                                        terminal_print(resp2.status_text);
                                        terminal_print("\n\n");

                                        // Auto-render if the response is HTML
                                        int looks_like_html2 = 0;
                                        for (int bi = 0; bi < 80 && resp2.body[bi]; bi++) {
                                            if (resp2.body[bi] == '<') {
                                                uint8_t nx = (uint8_t)resp2.body[bi+1];
                                                if ((nx >= 'a' && nx <= 'z') ||
                                                    (nx >= 'A' && nx <= 'Z') ||
                                                    nx == '!' || nx == '/') {
                                                    looks_like_html2 = 1;
                                                }
                                                break;
                                            }
                                            if (resp2.body[bi] != ' ' &&
                                                resp2.body[bi] != '\t' &&
                                                resp2.body[bi] != '\n' &&
                                                resp2.body[bi] != '\r')
                                                break;
                                        }
                                        if (looks_like_html2) {
                                            htmlview_render(resp2.body, target_host);
                                        } else {
                                            terminal_set_color(COLOR_WHITE,COLOR_BLACK);
                                            terminal_print(resp2.body);
                                            terminal_print("\n");
                                        }
                                    } else {
                                        terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
                                        terminal_print("  Request failed!\n");
                                    }
                                }
                            }
                        } else {
                            terminal_print(resp.body);
                            terminal_print("\n");
                        }
                        terminal_print("\n");
                    } else {
                        terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
                        terminal_print("  HTTP request failed!\n");
                    }
                }
            }
        } else {
            terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
            terminal_print("  Usage: http <init|get>\n");
        }
    }
    else if(s_eq(cmd,"about")){
        terminal_set_color(COLOR_LIGHT_CYAN,COLOR_BLACK);
        terminal_print("  Nyx OS v0.1 - Built from scratch\n");
        terminal_print("  Architecture: x86 32-bit\n");
    }
    else if(s_eq(cmd,"mount")){
        terminal_set_color(COLOR_YELLOW,COLOR_BLACK);
        terminal_print("  Initializing ATA disk...\n");
        if (ata_init()) {
            terminal_set_color(COLOR_LIGHT_GREEN,COLOR_BLACK);
            terminal_print("  ATA disk detected. Loading filesystem...\n");
            if (fs_load_from_disk()) {
                int total = file_count;
                terminal_set_color(COLOR_LIGHT_GREEN,COLOR_BLACK);
                terminal_print("  Loaded ");
                if (total >= 100) terminal_putchar('0' + total / 100);
                if (total >= 10) terminal_putchar('0' + (total / 10) % 10);
                terminal_putchar('0' + total % 10);
                terminal_print(" file(s) from disk.\n");
                // List loaded files
                terminal_set_color(COLOR_DARK_GREY,COLOR_BLACK);
                for (int fi = 0; fi < MAX_FILES; fi++) {
                    if (filesystem[fi].used) {
                        terminal_print("    ");
                        terminal_print(filesystem[fi].name);
                        terminal_print("\n");
                    }
                }
            } else {
                terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
                terminal_print("  No valid Nyx filesystem found on disk!\n");
                terminal_set_color(COLOR_DARK_GREY,COLOR_BLACK);
                terminal_print("  Run 'pack-files.py' on the host to write files to disk.img.\n");
            }
        } else {
            terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
            terminal_print("  No ATA disk detected. Ensure QEMU was started with\n");
            terminal_set_color(COLOR_DARK_GREY,COLOR_BLACK);
            terminal_print("  -drive file=disk.img,format=raw,if=ide\n");
        }
    }
    else if(s_eq(cmd,"unmount")){
        terminal_set_color(COLOR_YELLOW,COLOR_BLACK);
        terminal_print("  Unmounting disk files...\n");
        // Clear the filesystem array
        for (int i = 0; i < MAX_FILES; i++) {
            filesystem[i].used = 0;
            filesystem[i].name[0] = '\0';
            filesystem[i].content[0] = '\0';
            filesystem[i].size = 0;
        }
        file_count = 0;
        // Reload embedded files from the kernel ELF
        fs_load_embedded();
        terminal_set_color(COLOR_LIGHT_GREEN,COLOR_BLACK);
        terminal_print("  Unmounted. ");
        if (file_count >= 100) terminal_putchar('0' + file_count / 100);
        if (file_count >= 10) terminal_putchar('0' + (file_count / 10) % 10);
        terminal_putchar('0' + file_count % 10);
        terminal_print(" embedded file(s) loaded.\n");
        terminal_set_color(COLOR_DARK_GREY,COLOR_BLACK);
        terminal_print("  Run 'ls' to see available files.\n");
    }
    else if(s_ncmp(cmd,"pe",2)==0){
        char name[MAX_FILENAME]; get_arg1(args,name,MAX_FILENAME);
        if(!name[0]){
            terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
            terminal_print("  Usage: pe <filename.exe>\n");
        } else {
            int idx=find_file(name);
            if(idx<0){
                terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
                terminal_print("  File not found: ");
                terminal_print(name);
                terminal_print("\n");
            } else {
                const uint8_t* content = (const uint8_t*)filesystem[idx].content;
                int clen = filesystem[idx].size;
                if (clen <= 0) { while (content[clen]) clen++; }
                terminal_set_color(COLOR_LIGHT_CYAN,COLOR_BLACK);
                terminal_print("  Loading PE: ");
                terminal_print(name);
                terminal_print(" (");
                { char nb[12]; int np=0; unsigned v=(unsigned)clen;
                  if(!v){nb[np++]='0';} else { char t[12]; int q=0;
                    while(v>0){t[q++]=(char)('0'+v%10); v/=10;}
                    while(q>0) nb[np++]=t[--q]; }
                  nb[np]=0; terminal_print(nb); }
                terminal_print(" bytes)...\n");
                pe_dump_header(content, clen);
                int pid = pe_exec(content, clen, name, 0);
                if (pid >= 0) {
                    terminal_set_color(COLOR_LIGHT_GREEN,COLOR_BLACK);
                    terminal_print("  PE loaded at ring 3 (pid=");
                    { char nb[12]; int np=0; unsigned v=(unsigned)pid;
                      if(!v){nb[np++]='0';} else { char t[12]; int q=0;
                        while(v>0){t[q++]=(char)('0'+v%10); v/=10;}
                        while(q>0) nb[np++]=t[--q]; }
                      nb[np]=0; terminal_print(nb); }
                    terminal_print(")\n");
                } else {
                    terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
                    terminal_print("  PE load failed!\n");
                }
            }
        }
    }
    else {
        terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
        terminal_print("  Command not found: ");
        terminal_set_color(COLOR_WHITE,COLOR_BLACK);
        terminal_print(cmd);
        terminal_print("\n");
        terminal_set_color(COLOR_LIGHT_GREY,COLOR_BLACK);        terminal_print("  Type 'help' for commands.\n");
        }

    terminal_set_color(COLOR_WHITE,COLOR_BLACK);
    shell_prompt();
}

// ─── History navigation (called from keyboard.c) ──────────────────
void shell_history_up(char* buf, int* pos) {
    if(hist_count==0) return;
    if(hist_idx==-1) hist_idx=hist_count;
    if(hist_idx>0){
        hist_idx--;
        s_cpy(buf,history[hist_idx%HISTORY_MAX],CMD_MAX);
        *pos=s_len(buf);
    }
}

void shell_history_down(char* buf, int* pos) {
    if(hist_idx==-1) return;
    hist_idx++;
    if(hist_idx>=hist_count){
        hist_idx=-1;
        buf[0]='\0'; *pos=0;
    } else {
        s_cpy(buf,history[hist_idx%HISTORY_MAX],CMD_MAX);
        *pos=s_len(buf);
    }
}

// shell_autocomplete removed — no longer supported.
