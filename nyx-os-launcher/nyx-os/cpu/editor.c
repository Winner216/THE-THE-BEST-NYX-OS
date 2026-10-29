// cpu/editor.c
// Nyx OS Text Editor - Full Featured v2.0
#include <editor.h>
#include <fs.h>
#include <speaker.h>
#include <vesa.h>
#include <stdint.h>
#include <desktop.h>
#include <mouse.h>
#include <clipboard.h>

// ─── VGA ──────────────────────────────────────────────────────────
#define VGA_WIDTH  80
#define VGA_HEIGHT 25
#define VGA_MEM    ((uint16_t*)0xB8000)

static inline uint16_t make_entry(char c, uint8_t fg, uint8_t bg) {
    return (uint16_t)c | (uint16_t)(fg | bg << 4) << 8;
}

// 16-color VGA palette mapped to 24-bit RGB, purple-tinted to match
// the rest of the OS. Same approach used for desktop.c/power.c.
static const uint32_t e_vga16_rgb[16] = {
    0x0E0824, // 0  black      -> deep purple-black (editor bg)
    0x2A1850, // 1  blue       -> dark purple (panels)
    0x33EE77, // 2  green
    0x8FE0FF, // 3  cyan
    0xFF6B6B, // 4  red
    0xB088FF, // 5  magenta    -> theme accent purple
    0xAA5500, // 6  brown
    0xD8C8F0, // 7  light grey -> soft lavender white (default text)
    0x6A5890, // 8  dark grey  -> muted purple-grey
    0x9580C0, // 9  light blue -> lavender
    0x8FF0A8, // 10 light green
    0x9FFFC8, // 11 light cyan
    0xFF9FD6, // 12 light red  -> pink
    0xE0A8FF, // 13 light magenta -> bright lavender
    0xFFD166, // 14 yellow
    0xFFFFFF, // 15 white
};
static int e_off_x(void){
    if (g_win_x >= 0 && g_win_w == VGA_WIDTH*8 && g_win_h == VGA_HEIGHT*16)
        return g_win_x;
    return ((int)vesa.width  - VGA_WIDTH*8)  / 2;
}
static int e_off_y(void){
    if (g_win_y >= 0 && g_win_w == VGA_WIDTH*8 && g_win_h == VGA_HEIGHT*16)
        return g_win_y;
    return ((int)vesa.height - VGA_HEIGHT*16) / 2;
}

static void eput(int x, int y, char c, uint8_t fg, uint8_t bg) {
    if (x<0||x>=VGA_WIDTH||y<0||y>=VGA_HEIGHT) return;

    if (vesa.active) {
        int ox = e_off_x() + x*8;
        int oy = e_off_y() + y*16;
        vesa_draw_char(ox, oy, c, e_vga16_rgb[fg & 0x0F], e_vga16_rgb[bg & 0x0F]);
        return;
    }

    VGA_MEM[y*VGA_WIDTH+x] = make_entry(c,fg,bg);
}
static void eputs(int x, int y, const char* s, uint8_t fg, uint8_t bg) {
    for (int i=0;s[i];i++) eput(x+i,y,s[i],fg,bg);
}
static void efill(int x, int y, int w, int h, char c, uint8_t fg, uint8_t bg) {
    for (int dy=0;dy<h;dy++) for (int dx=0;dx<w;dx++) eput(x+dx,y+dy,c,fg,bg);
}
static void efill_row(int y, char c, uint8_t fg, uint8_t bg) {
    for (int x=0;x<VGA_WIDTH;x++) eput(x,y,c,fg,bg);
}

// ─── String helpers ───────────────────────────────────────────────
static int e_len(const char* s) { int i=0; while(s[i]) i++; return i; }
static void e_cpy(char* d, const char* s, int max) {
    int i=0; while(s[i]&&i<max-1){d[i]=s[i];i++;} d[i]='\0';
}
static int e_eq(const char* a, const char* b) {
    int i=0; while(a[i]&&b[i]&&a[i]==b[i])i++; return a[i]==b[i];
}
static int e_ncmp(const char* a, const char* b, int n) {
    for(int i=0;i<n;i++){if(!a[i]||a[i]!=b[i])return a[i]-b[i];} return 0;
}
static void n2s(int n, char* buf) {
    if(n==0){buf[0]='0';buf[1]='\0';return;}
    char t[12]; int p=0;
    int neg=(n<0); if(neg)n=-n;
    while(n>0){t[p++]='0'+n%10;n/=10;}
    int s=0; if(neg){buf[s++]='-';}
    for(int i=0;i<p;i++)buf[s+i]=t[p-1-i];
    buf[s+p]='\0';
}

// ─── Constants ────────────────────────────────────────────────────
#define MAX_LINES    80
#define MAX_LINE_W   74
#define EDIT_X        3
#define EDIT_Y        2
#define EDIT_H       19
#define EDIT_W       74
#define UNDO_MAX     30
#define CLIP_LINES   10

// ─── UI Modes ─────────────────────────────────────────────────────
#define UI_EDIT     0
#define UI_SAVE     1
#define UI_SEARCH   2
#define UI_REPLACE  3
#define UI_STATS    4
#define UI_HELP     5
#define UI_GOTO     6

// ─── Undo snapshot ────────────────────────────────────────────────
typedef struct {
    char lines[MAX_LINES][MAX_LINE_W+1];
    int  line_count, cur_line, cur_col;
} Snap;
static Snap undo_stack[UNDO_MAX];
static int  undo_top=0, undo_size=0;

// ─── Clipboard (uses system-wide clipboard) ─────────────────────
// clip[] replaced by clipboard_set/clipboard_get from clipboard.h

// ─── Selection ────────────────────────────────────────────────────
static int sel_on=0;
static int sel_sl=0, sel_sc=0, sel_el=0, sel_ec=0;

// ─── Search/Replace ───────────────────────────────────────────────
static char srch[40], repl[40];
static int  srch_result_ln=-1, srch_result_col=-1;

// ─── Editor State ─────────────────────────────────────────────────
static char lines[MAX_LINES][MAX_LINE_W+1];
static int  line_count=1, cur_line=0, cur_col=0;
static int  scroll_y=0, modified=0, file_idx=-1;
static char filename[MAX_FILENAME];
static char status_msg[48];
static int  status_timer=0;
static int  ui_mode=UI_EDIT;
static char inp[64], inp2[64];
static int  inp_pos=0, inp2_pos=0, inp_field=0;
static int  syntax_on=1, line_nums=1, word_wrap=0;
static int  tab_width=4;

// ─── Forward declarations ─────────────────────────────────────────
static void set_status(const char* msg);
static void editor_draw_content(void);

// ─── Status ───────────────────────────────────────────────────────
static void set_status(const char* msg) {
    e_cpy(status_msg, msg, 48);
    status_timer = 50;
}

// ─── Undo ─────────────────────────────────────────────────────────
static void undo_push() {
    Snap* s = &undo_stack[undo_top % UNDO_MAX];
    for(int i=0;i<line_count;i++) e_cpy(s->lines[i],lines[i],MAX_LINE_W+1);
    s->line_count=line_count; s->cur_line=cur_line; s->cur_col=cur_col;
    undo_top++; if(undo_size<UNDO_MAX) undo_size++;
}
static void undo_pop() {
    if(undo_size==0){set_status("Nothing to undo!");return;}
    undo_top--; if(undo_top<0)undo_top+=UNDO_MAX;
    undo_size--;
    Snap* s=&undo_stack[undo_top%UNDO_MAX];
    for(int i=0;i<s->line_count;i++) e_cpy(lines[i],s->lines[i],MAX_LINE_W+1);
    line_count=s->line_count; cur_line=s->cur_line; cur_col=s->cur_col;
    modified=1; set_status("Undone!");
}

// ─── Selection helpers ────────────────────────────────────────────
static void sel_normalize(int* sl,int* sc,int* el,int* ec) {
    *sl=sel_sl; *sc=sel_sc; *el=sel_el; *ec=sel_ec;
    if(*sl>*el||((*sl==*el)&&*sc>*ec)){
        int tl=*sl,tc=*sc; *sl=*el;*sc=*ec;*el=tl;*ec=tc;
    }
}
static int is_sel(int ln, int col) {
    if(!sel_on) return 0;
    int sl,sc,el,ec; sel_normalize(&sl,&sc,&el,&ec);
    if(ln<sl||ln>el) return 0;
    if(ln==sl&&col<sc) return 0;
    if(ln==el&&col>=ec) return 0;
    return 1;
}
static void sel_start_at(int ln, int col) {
    sel_on=1; sel_sl=ln; sel_sc=col; sel_el=ln; sel_ec=col;
}
static void sel_update(int ln, int col) {
    sel_el=ln; sel_ec=col;
}

// ─── Syntax Highlighting ─────────────────────────────────────────
static int is_kw(const char* s, int start, int len) {
    const char* kws[]={
        "int","void","char","short","long","float","double",
        "if","else","while","for","do","return","break","continue",
        "static","const","struct","typedef","enum","union",
        "include","define","ifndef","ifdef","endif","pragma",
        "uint8_t","uint16_t","uint32_t","uint64_t","int8_t",
        "int16_t","int32_t","NULL","extern","sizeof","goto",
        "switch","case","default","volatile","register","inline",0
    };
    for(int k=0;kws[k];k++){
        int kl=e_len(kws[k]);
        if(kl==len&&e_ncmp(s+start,kws[k],len)==0) return 1;
    }
    return 0;
}
static uint8_t syn_color(const char* line, int col) {
    if(!syntax_on) return 7;
    int len=e_len(line);
    if(col>=len) return 7;
    char c=line[col];

    // Single-line comment
    for(int i=0;i<=col;i++)
        if(i+1<len&&line[i]=='/'&&line[i+1]=='/') return 8;

    // Preprocessor
    if(len>0&&line[0]=='#') return 13;

    // String literal
    int in_str=0;
    for(int i=0;i<col;i++) if(line[i]=='"'&&(i==0||line[i-1]!='\\')) in_str=!in_str;
    if(in_str||c=='"') return 14;

    // Char literal
    if(c=='\'') return 14;

    // Numbers
    if((c>='0'&&c<='9')||(c=='.'&&col+1<len&&line[col+1]>='0'&&line[col+1]<='9')) return 11;
    if(c=='0'&&col+1<len&&(line[col+1]=='x'||line[col+1]=='X')) return 11;

    // Keywords and identifiers
    if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_') {
        int st=col;
        while(st>0&&((line[st-1]>='a'&&line[st-1]<='z')||(line[st-1]>='A'&&line[st-1]<='Z')||line[st-1]=='_'||(line[st-1]>='0'&&line[st-1]<='9'))) st--;
        int en=col;
        while(en<len&&((line[en]>='a'&&line[en]<='z')||(line[en]>='A'&&line[en]<='Z')||line[en]=='_'||(line[en]>='0'&&line[en]<='9'))) en++;
        if(is_kw(line,st,en-st)) return 12; // keyword cyan
        // ALL_CAPS = macro
        int allcaps=1;
        for(int i=st;i<en;i++) if(line[i]>='a'&&line[i]<='z'){allcaps=0;break;}
        if(allcaps&&en-st>1) return 6;
    }

    // Brackets
    if(c=='('||c==')'||c=='{'||c=='}'||c=='['||c==']') return 10;

    // Operators
    if(c=='+'||c=='-'||c=='*'||c=='/'||c=='='||c=='<'||c=='>'||c=='!'||c=='&'||c=='|'||c=='^'||c=='~'||c=='%'||c==';'||c==','||c=='.') return 9;

    return 7;
}

// ─── Text operations ─────────────────────────────────────────────
static void insert_ch(char c) {
    undo_push();
    char* line=lines[cur_line];
    int len=e_len(line);
    if(len>=MAX_LINE_W) return;
    for(int i=len;i>cur_col;i--) line[i]=line[i-1];
    line[cur_col]=c; line[len+1]='\0';
    cur_col++; modified=1;
}

static void insert_tab() {
    for(int i=0;i<tab_width;i++) insert_ch(' ');
}

static void del_ch() {
    undo_push();
    if(cur_col>0){
        char* line=lines[cur_line];
        int len=e_len(line);
        for(int i=cur_col-1;i<len;i++) line[i]=line[i+1];
        cur_col--; modified=1;
    } else if(cur_line>0){
        char* prev=lines[cur_line-1];
        char* curr=lines[cur_line];
        int plen=e_len(prev), clen=e_len(curr);
        if(plen+clen<=MAX_LINE_W){
            for(int i=0;i<clen;i++) prev[plen+i]=curr[i];
            prev[plen+clen]='\0';
            cur_col=plen;
            for(int i=cur_line;i<line_count-1;i++) e_cpy(lines[i],lines[i+1],MAX_LINE_W+1);
            lines[line_count-1][0]='\0';
            line_count--; cur_line--; modified=1;
        }
    }
}

static void del_forward() {
    undo_push();
    char* line=lines[cur_line];
    int len=e_len(line);
    if(cur_col<len){
        for(int i=cur_col;i<len;i++) line[i]=line[i+1];
        modified=1;
    } else if(cur_line<line_count-1){
        // Join with next line
        char* next=lines[cur_line+1];
        int nlen=e_len(next);
        if(len+nlen<=MAX_LINE_W){
            for(int i=0;i<nlen;i++) line[len+i]=next[i];
            line[len+nlen]='\0';
            for(int i=cur_line+1;i<line_count-1;i++) e_cpy(lines[i],lines[i+1],MAX_LINE_W+1);
            lines[line_count-1][0]='\0';
            line_count--; modified=1;
        }
    }
}

static void new_line() {
    undo_push();
    if(line_count>=MAX_LINES) return;
    for(int i=line_count;i>cur_line+1;i--) e_cpy(lines[i],lines[i-1],MAX_LINE_W+1);
    char* curr=lines[cur_line];
    char* next=lines[cur_line+1];
    // Auto-indent
    int indent=0;
    while(indent<e_len(curr)&&(curr[indent]==' '||curr[indent]=='\t')) indent++;
    e_cpy(next,curr+cur_col,MAX_LINE_W+1);
    curr[cur_col]='\0';
    // Apply indent to new line
    char tmp[MAX_LINE_W+1];
    for(int i=0;i<indent&&i<MAX_LINE_W;i++) tmp[i]=' ';
    int nlen=e_len(next);
    for(int i=0;i<nlen&&i+indent<MAX_LINE_W;i++) tmp[indent+i]=next[i];
    tmp[indent+nlen]='\0';
    e_cpy(next,tmp,MAX_LINE_W+1);
    cur_line++; cur_col=indent; line_count++; modified=1;
}

// ─── Selection copy/cut/delete ────────────────────────────────────
static void sel_copy() {
    if(!sel_on){
        // Copy current line to system clipboard
        clipboard_set(lines[cur_line]);
        set_status("Line copied!");
        return;
    }
    int sl,sc,el,ec; sel_normalize(&sl,&sc,&el,&ec);
    clipboard_clear();
    if(sl==el){
        // Single line selection
        int len=ec-sc; if(len<0)len=0;
        char tmp[MAX_LINE_W+1];
        for(int i=0;i<len&&i<MAX_LINE_W;i++) tmp[i]=lines[sl][sc+i];
        tmp[len]='\0';
        clipboard_set(tmp);
    } else {
        // Multi-line: first line from selection start
        int llen=e_len(lines[sl])-sc;
        char tmp[MAX_LINE_W+1];
        for(int i=0;i<llen&&i<MAX_LINE_W;i++) tmp[i]=lines[sl][sc+i];
        tmp[llen]='\0';
        clipboard_set(tmp);
        // Middle lines
        for(int ln=sl+1;ln<el;ln++){
            clipboard_append_line(lines[ln]);
        }
        // Last line up to selection end
        if(ec>0){
            char tmp2[MAX_LINE_W+1];
            for(int i=0;i<ec&&i<MAX_LINE_W;i++) tmp2[i]=lines[el][i];
            tmp2[ec]='\0';
            clipboard_append_line(tmp2);
        }
    }
    sel_on=0;
    set_status("Copied!");
}

static void sel_cut() {
    undo_push();
    if(!sel_on){
        // Cut current line to system clipboard
        clipboard_set(lines[cur_line]);
        if(line_count>1){
            for(int i=cur_line;i<line_count-1;i++) e_cpy(lines[i],lines[i+1],MAX_LINE_W+1);
            lines[line_count-1][0]='\0'; line_count--;
            if(cur_line>=line_count) cur_line=line_count-1;
        } else {
            lines[0][0]='\0';
        }
        cur_col=0; modified=1;
        set_status("Line cut!");
        return;
    }
    sel_copy();
    // Delete selected region
    int sl,sc,el,ec; sel_normalize(&sl,&sc,&el,&ec);
    if(sl==el){
        char* line=lines[sl];
        int len=e_len(line);
        for(int i=sc;i<len-(ec-sc)+1;i++) line[i]=line[i+(ec-sc)];
        cur_line=sl; cur_col=sc;
    } else {
        // Merge first and last line
        char tmp[MAX_LINE_W+1];
        int p=0;
        for(int i=0;i<sc&&i<MAX_LINE_W;i++) tmp[p++]=lines[sl][i];
        int llen=e_len(lines[el]);
        for(int i=ec;i<llen&&p<MAX_LINE_W;i++) tmp[p++]=lines[el][i];
        tmp[p]='\0';
        e_cpy(lines[sl],tmp,MAX_LINE_W+1);
        // Remove lines sl+1 to el
        int del=el-sl;
        for(int i=sl+1;i<line_count-del;i++) e_cpy(lines[i],lines[i+del],MAX_LINE_W+1);
        for(int i=line_count-del;i<line_count;i++) lines[i][0]='\0';
        line_count-=del;
        cur_line=sl; cur_col=sc;
    }
    sel_on=0; modified=1;
    set_status("Cut!");
}

static void do_paste() {
    undo_push();
    const char* clip = clipboard_get();
    if(!clip){set_status("Clipboard empty!");return;}
    // Paste line by line (split on newlines)
    const char* p = clip;
    int first = 1;
    while(*p) {
        if(!first) new_line();
        first = 0;
        // Copy one line into temp buffer
        char tmp[MAX_LINE_W+1];
        int ti = 0;
        while(*p && *p != '\n' && ti < MAX_LINE_W) {
            tmp[ti++] = *p++;
        }
        tmp[ti] = '\0';
        if(*p == '\n') p++;
        // Insert characters
        for(int i = 0; i < ti; i++) insert_ch(tmp[i]);
    }
    set_status("Pasted!");
}

// ─── Search & Replace ─────────────────────────────────────────────
static int do_find(int from_line, int from_col) {
    int tlen=e_len(srch); if(tlen==0) return 0;
    for(int i=from_line;i<line_count;i++){
        int llen=e_len(lines[i]);
        int sc=(i==from_line)?from_col:0;
        for(int j=sc;j<=llen-tlen;j++){
            if(e_ncmp(lines[i]+j,srch,tlen)==0){
                srch_result_ln=i; srch_result_col=j;
                cur_line=i; cur_col=j;
                if(cur_line<scroll_y) scroll_y=cur_line;
                if(cur_line>=scroll_y+EDIT_H) scroll_y=cur_line-EDIT_H+1;
                return 1;
            }
        }
    }
    return 0;
}

static void do_replace_one() {
    if(srch_result_ln<0){set_status("Find first!");return;}
    undo_push();
    int tlen=e_len(srch), rlen=e_len(repl);
    char* line=lines[srch_result_ln];
    int llen=e_len(line);
    char tmp[MAX_LINE_W+1]; int p=0;
    for(int i=0;i<srch_result_col&&p<MAX_LINE_W;i++) tmp[p++]=line[i];
    for(int i=0;i<rlen&&p<MAX_LINE_W;i++) tmp[p++]=repl[i];
    for(int i=srch_result_col+tlen;i<llen&&p<MAX_LINE_W;i++) tmp[p++]=line[i];
    tmp[p]='\0';
    e_cpy(line,tmp,MAX_LINE_W+1);
    modified=1; set_status("Replaced!");
}

static void do_replace_all() {
    int count=0;
    while(do_find(0,0)&&count<500){do_replace_one();count++;}
    char msg[48]="Replaced "; char cn[12]; n2s(count,cn);
    int p=9; for(int i=0;cn[i];i++) msg[p++]=cn[i];
    msg[p++]=' '; msg[p++]='x'; msg[p]='\0';
    srch_result_ln=-1; modified=1; set_status(msg);
}

// ─── Word count ───────────────────────────────────────────────────
static void get_stats(int* wc, int* cc, int* lc) {
    *wc=0; *cc=0; *lc=line_count;
    int iw=0;
    for(int i=0;i<line_count;i++){
        int len=e_len(lines[i]); *cc+=len;
        for(int j=0;j<len;j++){
            char c=lines[i][j];
            if(c==' '||c=='\t'){iw=0;}
            else{if(!iw){(*wc)++;iw=1;}}
        }
        iw=0;
    }
}

// ─── Save/Load ────────────────────────────────────────────────────
static void lines_to_buf(char* buf, int max) {
    int pos=0;
    for(int i=0;i<line_count&&pos<max-2;i++){
        int len=e_len(lines[i]);
        for(int j=0;j<len&&pos<max-2;j++) buf[pos++]=lines[i][j];
        if(i<line_count-1&&pos<max-2) buf[pos++]='\n';
    }
    buf[pos]='\0';
}
static void buf_to_lines(const char* buf) {
    line_count=0; int ln=0,col=0; lines[0][0]='\0';
    for(int i=0;buf[i]&&ln<MAX_LINES;i++){
        if(buf[i]=='\n'){lines[ln][col]='\0';ln++;col=0;lines[ln][0]='\0';line_count=ln;}
        else if(col<MAX_LINE_W){lines[ln][col++]=buf[i];lines[ln][col]='\0';}
    }
    line_count=ln+1;
}
static void do_save(const char* fname) {
    if(!fname||!fname[0]){set_status("No filename!");return;}
    int idx=-1;
    for(int i=0;i<MAX_FILES;i++)
        if(filesystem[i].used&&e_eq(filesystem[i].name,fname)){idx=i;break;}
    if(idx==-1){
        for(int i=0;i<MAX_FILES;i++) if(!filesystem[i].used){idx=i;break;}
        if(idx==-1){set_status("Filesystem full!");return;}
        e_cpy(filesystem[idx].name,fname,MAX_FILENAME);
        filesystem[idx].used=1; file_count++;
    }
    file_content_alloc(idx, 65536);  // 64 KiB for text editor
    if (!filesystem[idx].content) { set_status("OOM!"); return; }
    lines_to_buf(filesystem[idx].content, filesystem[idx].content_cap);
    filesystem[idx].size=0; while(filesystem[idx].content[filesystem[idx].size]&&filesystem[idx].size<filesystem[idx].content_cap-1)filesystem[idx].size++;
    e_cpy(filename,fname,MAX_FILENAME);
    file_idx=idx; modified=0;
    fs_save_to_disk();   // persist edits (no-op without an ATA disk)
    set_status("Saved!");
}

// ─── Cursor scrolling ─────────────────────────────────────────────
static void ensure_visible() {
    if(cur_line<scroll_y) scroll_y=cur_line;
    if(cur_line>=scroll_y+EDIT_H) scroll_y=cur_line-EDIT_H+1;
    if(scroll_y<0) scroll_y=0;
}

// ─── Draw ─────────────────────────────────────────────────────────
void editor_draw() {
    efill(0,0,VGA_WIDTH,VGA_HEIGHT,' ',7,0);

    // ── Title bar ────────────────────────────────────────────────
    efill_row(0,' ',0,1);
    eputs(2,0,"[ Nyx Editor v2 ]",15,1);
    if(filename[0]){
        eputs(21,0,filename,14,1);
        if(modified) eput(21+e_len(filename),0,'*',12,1);
    } else {
        eputs(21,0,"[New File]",8,1);
        if(modified) eput(31,0,'*',12,1);
    }

    // Cursor position
    char pos[24]; int pp=0;
    char l1[8]; n2s(cur_line+1,l1);
    char c1[8]; n2s(cur_col+1,c1);
    pos[pp++]='L'; pos[pp++]=':';
    for(int i=0;l1[i];i++) pos[pp++]=l1[i];
    pos[pp++]=' '; pos[pp++]='C'; pos[pp++]=':';
    for(int i=0;c1[i];i++) pos[pp++]=c1[i];
    pos[pp]='\0';
    eputs(52,0,pos,11,1);

    // Indicators
    eputs(63,0,syntax_on?"SYN":"   ",10,1);
    eputs(67,0,sel_on?"SEL":"   ",14,1);
    eputs(71,0,modified?"MOD":"   ",12,1);
    eputs(75,0,word_wrap?"WRP":"   ",13,1);

    // Ensure draggable window position is tracked
    if (g_win_x < 0) { g_win_x = e_off_x(); g_win_y = e_off_y(); }
    g_win_w = VGA_WIDTH * 8;
    g_win_h = VGA_HEIGHT * 16;

    // ── Editor area ──────────────────────────────────────────────
    // Compute which VGA row the mouse is hovering over
    int hover_row = -1;
    if (vesa.active) {
        int ox = e_off_x();
        int oy = e_off_y();
        int mx = mouse.px, my = mouse.py;
        if (mx >= ox + EDIT_X*8 && mx < ox + (EDIT_X+EDIT_W)*8 &&
            my >= oy + EDIT_Y*16 && my < oy + (EDIT_Y+EDIT_H)*16) {
            hover_row = (my - (oy + EDIT_Y*16)) / 16;
            if (hover_row < 0) hover_row = 0;
            if (hover_row >= EDIT_H) hover_row = EDIT_H - 1;
        }
    }

    for(int i=0;i<EDIT_H;i++){
        int ln=scroll_y+i;
        int ey=EDIT_Y+i;
        int hover_this = (i == hover_row && ln != cur_line && !sel_on);

        // Line number gutter
        if(line_nums&&ln<line_count){
            char lnum[4];
            lnum[0]='0'+(ln+1)/10;
            lnum[1]='0'+(ln+1)%10;
            lnum[2]='\0';
            uint8_t lnfg=(ln==cur_line)?14:8;
            eputs(0,ey,lnum,lnfg,0);
        }
        eput(2,ey,'|',(ln==cur_line)?14:8,0);

        if(ln<line_count){
            const char* line=lines[ln];
            int len=e_len(line);
            uint8_t line_bg = hover_this ? 1 : (ln==cur_line ? 0 : 0);
            efill(EDIT_X,ey,EDIT_W,1,' ',7,line_bg);

            for(int j=0;j<len&&j<EDIT_W;j++){
                if(is_sel(ln,j)){
                    eput(EDIT_X+j,ey,line[j],0,11); // cyan selection
                } else if(srch_result_ln==ln&&e_len(srch)>0&&
                          j>=srch_result_col&&j<srch_result_col+e_len(srch)){
                    eput(EDIT_X+j,ey,line[j],0,14); // yellow search
                } else {
                    uint8_t fg=syn_color(line,j);
                    eput(EDIT_X+j,ey,line[j],fg,line_bg);
                }
            }

            // Cursor block
            if(ln==cur_line){
                int cx=EDIT_X+cur_col;
                char cc=(cur_col<len)?line[cur_col]:' ';
                eput(cx,ey,cc,0,15);
            }
        } else {
            eput(EDIT_X,ey,'~',8,0);
        }
    }

    // Scrollbar
    if(line_count>EDIT_H){
        int bh=EDIT_H*EDIT_H/line_count; if(bh<1)bh=1;
        int bp=scroll_y*(EDIT_H-bh)/(line_count-EDIT_H);
        if(bp<0)bp=0; if(bp+bh>EDIT_H)bp=EDIT_H-bh;
        for(int i=0;i<EDIT_H;i++){
            char sc=(i>=bp&&i<bp+bh)?'#':'|';
            uint8_t fg=(i>=bp&&i<bp+bh)?15:8;
            eput(79,EDIT_Y+i,sc,fg,0);
        }
    }

    // ── Bottom bars ───────────────────────────────────────────────
    efill_row(21,' ',0,8);
    eputs(2,21,"Ctrl: S=save F=find R=replace Z=undo X=cut C=copy V=paste W=stats T=syn H=help",7,8);

    efill_row(22,' ',0,0);
    // Hint bar
    if(sel_on) eputs(2,22,"[SELECTION ACTIVE] Ctrl+C:copy  Ctrl+X:cut  Arrow:extend  ESC:clear sel",11,0);
    else eputs(2,22,"Shift+Arrow:select  Del:delete-fwd  Tab:indent  Home/End:line  PgUp/PgDn:scroll",8,0);

    // Status bar
    efill_row(23,' ',0,0);
    if(status_timer>0){
        eputs(2,23,status_msg,10,0);
        status_timer--;
    }

    // ── Overlays ──────────────────────────────────────────────────
    if(ui_mode==UI_SAVE){
        efill(15,10,50,5,' ',15,1);
        eputs(17,10,"+----------------------------------------------+",14,1);
        eputs(17,11,"|  Save As (enter filename):                   |",14,1);
        efill(19,12,44,1,' ',0,7);
        eputs(19,12,inp,0,7);
        eput(19+inp_pos,12,' ',0,15);
        eputs(17,13,"|  ENTER:save  ESC:cancel                      |",14,1);
        eputs(17,14,"+----------------------------------------------+",14,1);
    }
    if(ui_mode==UI_SEARCH){
        efill(15,10,50,5,' ',15,3);
        eputs(17,10,"+----------------------------------------------+",14,3);
        eputs(17,11,"|  Find:                                       |",14,3);
        efill(19,12,44,1,' ',0,7);
        eputs(19,12,inp,0,7);
        eput(19+inp_pos,12,' ',0,15);
        eputs(17,13,"|  ENTER:find next  N:find prev  ESC:close     |",14,3);
        eputs(17,14,"+----------------------------------------------+",14,3);
    }
    if(ui_mode==UI_REPLACE){
        efill(13,9,54,8,' ',15,5);
        eputs(15,9, "+--------------------------------------------------+",14,5);
        eputs(15,10,"|  Search & Replace                                |",15,5);
        eputs(15,11,"|  Find   :                                        |",14,5);
        efill(26,11,36,1,' ',0,inp_field==0?15:7);
        eputs(26,11,inp,0,inp_field==0?15:7);
        eputs(15,12,"|  Replace:                                        |",14,5);
        efill(26,12,36,1,' ',0,inp_field==1?15:7);
        eputs(26,12,inp2,0,inp_field==1?15:7);
        eputs(15,13,"|  TAB:switch field  ENTER:find+replace            |",14,5);
        eputs(15,14,"|  A:replace all  ESC:close                        |",14,5);
        eputs(15,15,"|  Replaced so far: 0                              |",14,5);
        eputs(15,16,"+--------------------------------------------------+",14,5);
    }
    if(ui_mode==UI_STATS){
        int wc,cc,lc; get_stats(&wc,&cc,&lc);
        efill(20,8,40,12,' ',15,1);
        eputs(22,8, "+------------------------------------+",14,1);
        eputs(22,9, "|     File Statistics                |",15,1);
        eputs(22,10,"+------------------------------------+",14,1);
        char t[12];
        eputs(24,11,"Lines    :",11,1); n2s(lc,t); eputs(35,11,t,15,1);
        eputs(24,12,"Words    :",11,1); n2s(wc,t); eputs(35,12,t,15,1);
        eputs(24,13,"Chars    :",11,1); n2s(cc,t); eputs(35,13,t,15,1);
        eputs(24,14,"Filename :",11,1); eputs(35,14,filename[0]?filename:"(new)",14,1);
        eputs(24,15,"Modified :",11,1); eputs(35,15,modified?"Yes":"No",modified?12:10,1);
        eputs(24,16,"Syntax   :",11,1); eputs(35,16,syntax_on?"On":"Off",syntax_on?10:8,1);
        eputs(24,17,"Tab width:",11,1); n2s(tab_width,t); eputs(35,17,t,15,1);
        eputs(24,18,"Undo hist:",11,1); n2s(undo_size,t); eputs(35,18,t,15,1);
        eputs(22,19,"+------------------------------------+",14,1);
        eputs(24,20,"Press any key to close",8,1);
    }
    if(ui_mode==UI_HELP){
        efill(6,3,68,19,' ',15,1);
        eputs(8,3, "+----------------------------------------------------------------+",14,1);
        eputs(8,4, "|           Nyx Editor v2 - Keyboard Reference                  |",15,1);
        eputs(8,5, "+----------------------------------------------------------------+",14,1);
        eputs(10,6, "Ctrl+S / F2    Save file",11,1);
        eputs(10,7, "Ctrl+F         Find / Search",11,1);
        eputs(10,8, "Ctrl+R         Search & Replace",11,1);
        eputs(10,9, "Ctrl+Z         Undo (30 levels)",11,1);
        eputs(10,10,"Ctrl+X         Cut line / selection",11,1);
        eputs(10,11,"Ctrl+C         Copy line / selection",11,1);
        eputs(10,12,"Ctrl+V         Paste",11,1);
        eputs(10,13,"Ctrl+W         Word count & stats",11,1);
        eputs(10,14,"Ctrl+T         Toggle syntax highlighting",11,1);
        eputs(10,15,"Shift+Arrow    Select text",11,1);
        eputs(10,16,"Tab            Insert indent",11,1);
        eputs(10,17,"Del            Delete forward",11,1);
        eputs(10,18,"Home / End     Start / End of line",11,1);
        eputs(10,19,"Ctrl+H         This help screen",11,1);
        eputs(10,20,"ESC            Close overlay / back to desktop",11,1);
        eputs(8,21,"+----------------------------------------------------------------+",14,1);
    }
    if(ui_mode==UI_GOTO){
        efill(25,11,30,4,' ',15,1);
        eputs(27,11,"+-------------------------+",14,1);
        eputs(27,12,"|  Go to line:            |",14,1);
        efill(29,13,20,1,' ',0,7);
        eputs(29,13,inp,0,7);
        eput(29+inp_pos,13,' ',0,15);
        eputs(27,14,"+-------------------------+",14,1);
    }

    if (vesa.active) vesa_present();
}

// ─── Key handler ─────────────────────────────────────────────────
void editor_handle_key(char c) {

    // Stats/Help/Goto overlays — any key closes
    if(ui_mode==UI_STATS||ui_mode==UI_HELP){
        ui_mode=UI_EDIT; editor_draw(); return;
    }

    // Save dialog
    if(ui_mode==UI_SAVE){
        if(c==27){ui_mode=UI_EDIT;editor_draw();return;}
        if(c==13){
            if(inp_pos>0){ui_mode=UI_EDIT;do_save(inp);}
            editor_draw();return;
        }
        if(c==8&&inp_pos>0){inp[--inp_pos]='\0';editor_draw();return;}
        if(c>=32&&c<127&&inp_pos<MAX_FILENAME-1){inp[inp_pos++]=c;inp[inp_pos]='\0';editor_draw();return;}
        return;
    }

    // Goto dialog
    if(ui_mode==UI_GOTO){
        if(c==27){ui_mode=UI_EDIT;editor_draw();return;}
        if(c==13){
            int ln=0;
            for(int i=0;inp[i]>='0'&&inp[i]<='9';i++) ln=ln*10+(inp[i]-'0');
            ln--; if(ln<0)ln=0; if(ln>=line_count)ln=line_count-1;
            cur_line=ln; cur_col=0; ensure_visible();
            ui_mode=UI_EDIT; set_status("Jumped to line!");
            editor_draw();return;
        }
        if(c==8&&inp_pos>0){inp[--inp_pos]='\0';editor_draw();return;}
        if(c>='0'&&c<='9'&&inp_pos<6){inp[inp_pos++]=c;inp[inp_pos]='\0';editor_draw();return;}
        return;
    }

    // Search dialog
    if(ui_mode==UI_SEARCH){
        if(c==27){ui_mode=UI_EDIT;srch_result_ln=-1;editor_draw();return;}
        if(c==13){
            e_cpy(srch,inp,40);
            int found=do_find(cur_line,cur_col+1);
            if(!found) found=do_find(0,0);
            if(!found) set_status("Not found!");
            else set_status("Found! ENTER:next");
            editor_draw();return;
        }
        if(c=='n'||c=='N'){
            e_cpy(srch,inp,40);
            int found=do_find(cur_line,cur_col+1);
            if(!found) found=do_find(0,0);
            if(!found) set_status("Not found!"); else set_status("Found!");
            editor_draw();return;
        }
        if(c==8&&inp_pos>0){inp[--inp_pos]='\0';editor_draw();return;}
        if(c>=32&&c<127&&inp_pos<39){inp[inp_pos++]=c;inp[inp_pos]='\0';editor_draw();return;}
        return;
    }

    // Replace dialog
    if(ui_mode==UI_REPLACE){
        if(c==27){ui_mode=UI_EDIT;editor_draw();return;}
        if(c=='\t'){inp_field=(inp_field+1)%2;editor_draw();return;}
        if(c==13){
            e_cpy(srch,inp,40); e_cpy(repl,inp2,40);
            int found=do_find(cur_line,cur_col);
            if(found) do_replace_one();
            else set_status("Not found!");
            editor_draw();return;
        }
        if(c=='a'||c=='A'){
            e_cpy(srch,inp,40); e_cpy(repl,inp2,40);
            do_replace_all(); ui_mode=UI_EDIT;
            editor_draw();return;
        }
        if(c==8){
            if(inp_field==0&&inp_pos>0) inp[--inp_pos]='\0';
            else if(inp_field==1&&inp2_pos>0) inp2[--inp2_pos]='\0';
            editor_draw();return;
        }
        if(c>=32&&c<127){
            if(inp_field==0&&inp_pos<39){inp[inp_pos++]=c;inp[inp_pos]='\0';}
            else if(inp_field==1&&inp2_pos<39){inp2[inp2_pos++]=c;inp2[inp2_pos]='\0';}
            editor_draw();return;
        }
        return;
    }

    // ── Normal edit mode ─────────────────────────────────────────

    // Ctrl+S / F2 = save
    if(c==19){
        if(filename[0]) do_save(filename);
        else{ui_mode=UI_SAVE;inp_pos=0;inp[0]='\0';}
        editor_draw();return;
    }
    // Ctrl+F = find
    if(c==6){ui_mode=UI_SEARCH;inp_pos=0;inp[0]='\0';set_status("Type to search");editor_draw();return;}
    // Ctrl+R = replace
    if(c==18){ui_mode=UI_REPLACE;inp_pos=0;inp2_pos=0;inp_field=0;inp[0]='\0';inp2[0]='\0';editor_draw();return;}
    // Ctrl+Z = undo
    if(c==26){undo_pop();ensure_visible();editor_draw();return;}
    // Ctrl+X = cut
    if(c==24){sel_cut();ensure_visible();editor_draw();return;}
    // Ctrl+C = copy
    if(c==3){sel_copy();editor_draw();return;}
    // Ctrl+V = paste
    if(c==22){do_paste();ensure_visible();editor_draw();return;}
    // Ctrl+W = stats
    if(c==23){ui_mode=UI_STATS;editor_draw();return;}
    // Ctrl+T = toggle syntax
    if(c==20){syntax_on=!syntax_on;set_status(syntax_on?"Syntax ON":"Syntax OFF");editor_draw();return;}
    // Ctrl+H = help
    if(c==8&&cur_col==0&&cur_line==0){ /* avoid conflict with backspace */ }
    if(c==12){ui_mode=UI_HELP;editor_draw();return;}  // Ctrl+L = help
    // Ctrl+A = home
    if(c==1){sel_on=0;cur_col=0;editor_draw();return;}
    // Ctrl+E = end
    if(c==5){sel_on=0;cur_col=e_len(lines[cur_line]);editor_draw();return;}
    // Ctrl+G = goto line
    if(c==7){ui_mode=UI_GOTO;inp_pos=0;inp[0]='\0';editor_draw();return;}
    // Ctrl+N = new line below
    if(c==14){cur_col=e_len(lines[cur_line]);new_line();ensure_visible();editor_draw();return;}

    // ESC = clear selection or back
    if(c==27){
        if(sel_on){sel_on=0;editor_draw();}
        // ESC handled by desktop to go back
        return;
    }

    // Shift+Arrow keys (lowercase a,b,c,d sent from keyboard.c)
    if(c=='a'){ // Shift+Up
        if(!sel_on) sel_start_at(cur_line,cur_col);
        if(cur_line>0){cur_line--;int len=e_len(lines[cur_line]);if(cur_col>len)cur_col=len;if(cur_line<scroll_y)scroll_y--;}
        sel_update(cur_line,cur_col); editor_draw();return;
    }
    if(c=='b'){ // Shift+Down
        if(!sel_on) sel_start_at(cur_line,cur_col);
        if(cur_line<line_count-1){cur_line++;int len=e_len(lines[cur_line]);if(cur_col>len)cur_col=len;if(cur_line>=scroll_y+EDIT_H)scroll_y++;}
        sel_update(cur_line,cur_col); editor_draw();return;
    }
    if(c=='c'){ // Shift+Right — but 'c' also Ctrl+C conflict resolved by ctrl_pressed in keyboard.c
        if(!sel_on) sel_start_at(cur_line,cur_col);
        int len=e_len(lines[cur_line]);
        if(cur_col<len)cur_col++;
        else if(cur_line<line_count-1){cur_line++;cur_col=0;if(cur_line>=scroll_y+EDIT_H)scroll_y++;}
        sel_update(cur_line,cur_col); editor_draw();return;
    }
    if(c=='d'){ // Shift+Left
        if(!sel_on) sel_start_at(cur_line,cur_col);
        if(cur_col>0)cur_col--;
        else if(cur_line>0){cur_line--;cur_col=e_len(lines[cur_line]);if(cur_line<scroll_y)scroll_y--;}
        sel_update(cur_line,cur_col); editor_draw();return;
    }

    // Regular arrow keys (clear selection)
    if(c=='A'){ // Up
        sel_on=0;
        if(cur_line>0){cur_line--;int len=e_len(lines[cur_line]);if(cur_col>len)cur_col=len;if(cur_line<scroll_y)scroll_y--;}
        editor_draw();return;
    }
    if(c=='B'){ // Down
        sel_on=0;
        if(cur_line<line_count-1){cur_line++;int len=e_len(lines[cur_line]);if(cur_col>len)cur_col=len;if(cur_line>=scroll_y+EDIT_H)scroll_y++;}
        editor_draw();return;
    }
    if(c=='C'){ // Right
        sel_on=0;
        int len=e_len(lines[cur_line]);
        if(cur_col<len)cur_col++;
        else if(cur_line<line_count-1){cur_line++;cur_col=0;if(cur_line>=scroll_y+EDIT_H)scroll_y++;}
        editor_draw();return;
    }
    if(c=='D'){ // Left
        sel_on=0;
        if(cur_col>0)cur_col--;
        else if(cur_line>0){cur_line--;cur_col=e_len(lines[cur_line]);if(cur_line<scroll_y)scroll_y--;}
        editor_draw();return;
    }

    // Enter
    if(c==13){sel_on=0;new_line();ensure_visible();editor_draw();return;}

    // Backspace
    if(c==8){
        if(sel_on){sel_cut();ensure_visible();editor_draw();return;}
        del_ch();ensure_visible();editor_draw();return;
    }

    // Tab
    if(c==9){sel_on=0;insert_tab();editor_draw();return;}

    // Delete forward (sent as special code 127 or 4)
    if(c==127||c==4){sel_on=0;del_forward();editor_draw();return;}

    // Home key (sent as code 2 from keyboard)
    if(c==2){sel_on=0;cur_col=0;editor_draw();return;}

    // End key (sent as code 10 from keyboard)
    if(c==10){sel_on=0;cur_col=e_len(lines[cur_line]);editor_draw();return;}

    // Page Up (code 25)
    if(c==25){
        sel_on=0;
        cur_line-=EDIT_H; if(cur_line<0)cur_line=0;
        ensure_visible(); editor_draw();return;
    }

    // Page Down (code 11)
    if(c==11){
        sel_on=0;
        cur_line+=EDIT_H; if(cur_line>=line_count)cur_line=line_count-1;
        ensure_visible(); editor_draw();return;
    }

    // Regular printable character
    if(c>=32&&c<127){
        if(sel_on){sel_cut();sel_on=0;}
        insert_ch(c); editor_draw();return;
    }
}

// ─── Mouse handler ───────────────────────────────────────────────
// Convert pixel coords to VGA cell, place cursor at the clicked
// position within the editor content area.
int editor_handle_mouse(int mx, int my) {
    if (!vesa.active) return 0;

    int ox = e_off_x();
    int oy = e_off_y();

    // Convert to VGA cell coordinates
    int vx = (mx - ox) / 8;
    int vy = (my - oy) / 16;

    // Check if click is within the editor content area (EDIT_X..EDIT_X+EDIT_W,
    // EDIT_Y..EDIT_Y+EDIT_H) which is the main text editing zone
    if (vx >= EDIT_X && vx < EDIT_X + EDIT_W &&
        vy >= EDIT_Y && vy < EDIT_Y + EDIT_H) {
        int ln = scroll_y + (vy - EDIT_Y);
        int col = vx - EDIT_X;
        if (col < 0) col = 0;
        if (ln >= line_count) ln = line_count - 1;
        if (ln < 0) ln = 0;

        int line_len = e_len(lines[ln]);
        if (col > line_len) col = line_len;

        sel_on = 0;
        cur_line = ln;
        cur_col = col;
        editor_draw();
        return 1;
    }

    return 0;
}

// ─── Public init/open ────────────────────────────────────────────
void editor_open_file(int idx) {
    file_idx=idx;
    if(idx>=0&&idx<MAX_FILES&&filesystem[idx].used){
        e_cpy(filename,filesystem[idx].name,MAX_FILENAME);
        buf_to_lines(filesystem[idx].content);
    } else {
        filename[0]='\0'; line_count=1; lines[0][0]='\0';
    }
    cur_line=0;cur_col=0;scroll_y=0;modified=0;
    ui_mode=UI_EDIT;sel_on=0;
    undo_top=0;undo_size=0;
    srch_result_ln=-1;clipboard_clear();
    status_msg[0]='\0';status_timer=0;
}

void editor_init() {
    file_idx=-1; line_count=1;
    cur_line=0;cur_col=0;scroll_y=0;
    modified=0;ui_mode=UI_EDIT;sel_on=0;
    undo_top=0;undo_size=0;
    srch_result_ln=-1;clipboard_clear();
    syntax_on=1;line_nums=1;word_wrap=0;tab_width=4;
    status_timer=0;
    for(int i=0;i<MAX_LINES;i++) lines[i][0]='\0';
    filename[0]='\0';status_msg[0]='\0';
}
