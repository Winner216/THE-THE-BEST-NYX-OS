// cpu/htmlview.c  –  Nyx OS HTML Viewer
//
// A simple HTML renderer that parses basic tags and draws styled
// content to a scrollable VESA panel (with VGA text-mode fallback).
//
// Supported tags:
//   h1–h6, p, a, ul/ol/li, b/strong, i/em, br, hr, pre
//
// State machine HTML parser with a style stack; outputs a "rich text"
// line buffer that the panel renderer paints to the screen.

#include <htmlview.h>
#include <vesa.h>
#include <terminal.h>
#include <pngdec.h>
#include <jpegdec.h>
#include <bmpdec.h>
#include <gifdec.h>
#include <stdint.h>
#include <stddef.h>
#include <io.h>

// ─── Clamp helper ─────────────────────────────────────────────────

// ─── Rich-text buffer ────────────────────────────────────────────
// Each line is an array of "rich characters" (char + fg + bg +
// underline flag).  The parser fills this; the renderer draws it.

// 1500 lines: with HTTP_MAX_RESPONSE at 1 MB the full Wikipedia
// article buffers, and at the observed render density (~72 lines per
// 255 KB of raw HTML) a 970 KB body needs ~270 lines — 1500 keeps a
// wide margin so the parser never truncates before the buffer does.
#define HV_MAX_LINES   1500
#define HV_LINE_W      200    // max characters per line

typedef struct {
    char     ch;
    uint32_t fg;
    uint32_t bg;
    int      underline;
} HvChar;

typedef struct {
    HvChar   chars[HV_LINE_W];
    int      len;
    int      img;    // image slot to draw on this line, -1 = text
} HvLine;

static HvLine hv_lines[HV_MAX_LINES];
static int    hv_line_count = 0;

// ─── Images ──────────────────────────────────────────────────────
// <img> tags become boxed slots.  Each image owns a decoded RGBA
// buffer; oversized images are scaled into it at decode time.  The
// parse reserves a fixed box (HV_IMG_BOX_LINES rows) per image so
// the layout and scrollbar stay deterministic even before the bytes
// arrive.  The browser fetches the URL and calls htmlview_img_decode.
#define HV_MAX_IMGS        8
#define HV_IMG_MAX_W       512
#define HV_IMG_MAX_H       384
#define HV_IMG_BOX_LINES   10    // ~160 px tall box per image
#define HV_IMG_STATE_PEND  0     // waiting for bytes
#define HV_IMG_STATE_OK    1     // decoded, ready to draw
#define HV_IMG_STATE_FAIL  2     // fetch/decode failed → placeholder box

typedef struct {
    char     src[256];                 // raw src= attribute
    int      line;                     // line where the box starts
    int      state;
    int      w, h;                     // decoded (scaled) size
    uint8_t  pixels[HV_IMG_MAX_W * HV_IMG_MAX_H * 4];  // RGBA
} HvImg;

static HvImg hv_imgs[HV_MAX_IMGS];
static int   hv_img_count = 0;

// Scratch for decoding images larger than a slot buffer; the result
// is scaled into the slot.  1600×1600×4 = 10.2 MB BSS.
static uint8_t hv_img_scratch[1600 * 1600 * 4];
static char   hv_title[64];
static char   hv_page_title[64];  // extracted from <title> tag
static int    hv_active = 0;      // 1 = viewer open
static int    hv_scroll_y = 0;

// ─── Window mode ──────────────────────────────────────────────────
// When hv_win_w > 0 the panel is drawn inside the given rect instead
// of a full-screen centered panel, and `hv_win_top` pixels at the top
// are reserved for the caller's chrome (browser address/status bars).
static int hv_win_x = 0, hv_win_y = 0, hv_win_w = 0, hv_win_h = 0;
static int hv_win_top = 0;

// ─── Link tracking ───────────────────────────────────────────────
// Stores href attributes encountered during parsing so the user can
// navigate to them with UP/DOWN and follow with ENTER.

#define HV_MAX_LINKS  64
#define HV_URL_MAX    256

typedef struct {
    int  line;              // line index where the link text starts
    int  col;               // column where the link text starts
    int  len;               // length of link text in characters
    char href[HV_URL_MAX];  // href attribute value
} HvLink;

static HvLink hv_links[HV_MAX_LINKS];
static int    hv_link_count = 0;
static int    hv_link_cursor = -1;    // index into hv_links[], -1 = none
static char   hv_pending_url[HV_URL_MAX]; // URL to follow on next fetch

// Build array of link indices visible within [top, bot).
// Returns the count (0 if none), writes indices into out[].
static int hv_visible_links(int* out, int max_out, int top, int bot) {
    int n = 0;
    for (int i = 0; i < hv_link_count && n < max_out; i++) {
        if (hv_links[i].line >= top && hv_links[i].line < bot)
            out[n++] = i;
    }
    return n;
}

// ─── Style tracking ──────────────────────────────────────────────
// The parser uses a simple counter-based style stack.
#define HV_STYLE_DEPTH 32
static int  hv_style_sp = 0;  // stack pointer
// For each style we track:
//   fg, bg, underline, scale + bold/italic flags
// The "scale" concept: instead of separate font sizes (we only have
// 8×16), we use larger background blocks for headings.

typedef struct {
    uint32_t fg;
    uint32_t bg;
    int      underline;
    int      bold;     // 0/1
    int      italic;   // 0/1
    int      pre;      // 0/1 — monospace mode
    int      heading;  // 1-6 or 0
} HvStyle;

static HvStyle hv_default_style(void) {
    HvStyle s;
    s.fg        = 0xE0D8FF;
    s.bg        = 0x0E0C22;
    s.underline = 0;
    s.bold      = 0;
    s.italic    = 0;
    s.pre       = 0;
    s.heading   = 0;
    return s;
}

static HvStyle hv_cur;   // current style (set by push/pop)
static HvStyle hv_style_stack[HV_STYLE_DEPTH];

static void hv_push_style(void) {
    if (hv_style_sp < HV_STYLE_DEPTH) {
        hv_style_stack[hv_style_sp] = hv_cur;
        hv_style_sp++;
    }
}

static void hv_pop_style(void) {
    if (hv_style_sp > 0) {
        hv_style_sp--;
        hv_cur = hv_style_stack[hv_style_sp];
    }
}

// ─── Parser helpers ───────────────────────────────────────────────
static int hv_match(const char* html, int pos, const char* s) {
    for (int i = 0; s[i]; i++)
        if (html[pos + i] != s[i]) return 0;
    return 1;
}


// Decode a single HTML entity at pos, write decoded char to *out,
// return new pos (past the entity).
static int hv_decode_entity(const char* html, int pos, char* out) {
    if (html[pos] != '&') { *out = html[pos]; return pos + 1; }
    if (hv_match(html, pos, "&amp;"))  { *out = '&';  return pos + 5; }
    if (hv_match(html, pos, "&lt;"))   { *out = '<';  return pos + 4; }
    if (hv_match(html, pos, "&gt;"))   { *out = '>';  return pos + 4; }
    if (hv_match(html, pos, "&quot;")) { *out = '"';  return pos + 6; }
    if (hv_match(html, pos, "&apos;")) { *out = '\''; return pos + 6; }
    if (hv_match(html, pos, "&nbsp;")) { *out = ' ';  return pos + 6; }
    // Numeric entities &#[0-9]+;  (simple version)
    if (html[pos+1] == '#') {
        int num = 0, np = pos + 2;
        while (html[np] >= '0' && html[np] <= '9') {
            num = num * 10 + (html[np] - '0');
            np++;
        }
        if (html[np] == ';') {
            *out = (char)(num > 127 ? '?' : num);
            return np + 1;
        }
    }
    // Fallback: keep the &
    *out = '&';
    return pos + 1;
}

// ─── Token parsing ────────────────────────────────────────────────
typedef enum {
    HV_TAG_H1, HV_TAG_H2, HV_TAG_H3, HV_TAG_H4, HV_TAG_H5, HV_TAG_H6,
    HV_TAG_P,
    HV_TAG_A,
    HV_TAG_B, HV_TAG_STRONG,
    HV_TAG_I, HV_TAG_EM,
    HV_TAG_UL, HV_TAG_OL,
    HV_TAG_LI,
    HV_TAG_BR,
    HV_TAG_HR,
    HV_TAG_PRE,
    HV_TAG_DIV,
    HV_TAG_SPAN,
    HV_TAG_IMG,
    HV_TAG_TABLE, HV_TAG_TR, HV_TAG_TD, HV_TAG_TH,
    HV_TAG_THEAD, HV_TAG_TBODY, HV_TAG_TFOOT, HV_TAG_CAPTION,
    HV_TAG_HEAD, HV_TAG_TITLE, HV_TAG_BODY,
    HV_TAG_HTML, HV_TAG_SCRIPT, HV_TAG_STYLE,
    HV_TAG_OTHER,
} HvTagId;



// ─── Parser context ───────────────────────────────────────────────
// We parse the HTML and produce HvLines. The parser uses the
// hv_cur style and hv_lines[] directly (global state).

// Write a single rich character to the current line
static void hv_putch(char ch, uint32_t fg, uint32_t bg, int underline) {
    if (hv_line_count >= HV_MAX_LINES) return;
    HvLine* line = &hv_lines[hv_line_count];
    if (line->len >= HV_LINE_W) {
        // Wrap to next line
        hv_line_count++;
        if (hv_line_count >= HV_MAX_LINES) return;
        line = &hv_lines[hv_line_count];
        line->img = -1;
    }
    line->chars[line->len].ch        = ch;
    line->chars[line->len].fg        = fg;
    line->chars[line->len].bg        = bg;
    line->chars[line->len].underline = underline;
    line->len++;
}

// Write a plain string with current style
static void hv_puts(const char* s) {
    for (int i = 0; s[i]; i++) {
        hv_putch(s[i], hv_cur.fg, hv_cur.bg, hv_cur.underline);
    }
}

// Start a new line
static void hv_newline(void) {
    if (hv_line_count < HV_MAX_LINES) {
        hv_line_count++;
        if (hv_line_count < HV_MAX_LINES)
            hv_lines[hv_line_count].img = -1;
    }
}

// Write spaces for list indent
static void hv_indent(int n) {
    for (int i = 0; i < n; i++)
        hv_putch(' ', hv_cur.fg, hv_cur.bg, 0);
}

// ─── Heading style helpers ────────────────────────────────────────
static uint32_t hv_heading_fg(int level) {
    switch (level) {
        case 1: return 0xFFFFFF;
        case 2: return 0xF0E8FF;
        case 3: return 0xBBCCFF;
        case 4: return 0x88DDFF;
        case 5: return 0xFFDD88;
        case 6: return 0xFFAA66;
        default: return 0xE0D8FF;
    }
}

static uint32_t hv_heading_bg(int level) {
    switch (level) {
        case 1: return 0x2A2060;
        case 2: return 0x221848;
        case 3: return 0x181440;
        case 4: return 0x14123A;
        case 5: return 0x1A1430;
        case 6: return 0x1E1428;
        default: return 0x0E0C22;
    }
}

// ─── Main HTML parser ─────────────────────────────────────────────
// Walks the HTML string and fills hv_lines[] with styled output.

static void hv_parse(const char* html) {
    hv_line_count = 0;
    for (int i = 0; i < HV_MAX_LINES; i++) hv_lines[i].img = -1;
    hv_img_count = 0;
    hv_cur = hv_default_style();
    hv_style_sp = 0;
    hv_link_count = 0;
    hv_link_cursor = -1;
    hv_pending_url[0] = '\0';

    // State
    int in_pre = 0;
    int in_a_tag = 0;
    char a_href[HV_URL_MAX];
    int a_start_line = 0;
    int a_start_col = 0;
    int a_first_char = 1;
    int list_type = 0;     // 0=none, 1=ul, 2=ol
    int list_counter = 0;
    int list_indent = 0;
    int table_cell_count = 0;  // cells emitted in the current <tr>
    int need_para_space = 0;
    int in_script = 0;
    int in_style_block = 0;
    int in_title = 0;
    int page_title_pos = 0;
    hv_page_title[0] = '\0';

    int pos = 0;
    while (html[pos]) {
        // ── Handle tag ──
        if (html[pos] == '<') {
            pos++;  // skip '<'

            // Comment: <!-- ... -->
            if (hv_match(html, pos, "!--")) {
                while (html[pos] && !(html[pos]=='-' && html[pos+1]=='-' && html[pos+2]=='>'))
                    pos++;
                if (html[pos]) pos += 3;  // skip -->
                continue;
            }

            int closing = 0;
            if (html[pos] == '/') { closing = 1; pos++; }

            // Extract tag name (upto space, /, or >)
            int tname_start = pos;
            while (html[pos] && html[pos] != '>' && html[pos] != ' ' &&
                   html[pos] != '/' && html[pos] != '\n')
                pos++;

            int tname_len = pos - tname_start;
            // Lowercase the name for lookup
            char tname_lower[32];
            int tl = tname_len;
            if (tl > 30) tl = 30;
            for (int i = 0; i < tl; i++) {
                char c = html[tname_start + i];
                tname_lower[i] = (c >= 'A' && c <= 'Z') ? c + 32 : c;
            }
            tname_lower[tl] = '\0';

            // Determine tag before attribute parsing so we can
            // extract href from <a> tags
            HvTagId tag = HV_TAG_OTHER;
            int is_a_tag = 0;
            int is_img_tag = 0;
            if (tname_len == 1 && tname_lower[0] == 'a') is_a_tag = 1;
            if (tname_len == 3 && tname_lower[0] == 'i' &&
                tname_lower[1] == 'm' && tname_lower[2] == 'g')
                is_img_tag = 1;
            if (tname_len == 1) {
                switch (tname_lower[0]) {
                    case 'p': tag = HV_TAG_P; break;
                    case 'a': tag = HV_TAG_A; break;
                    case 'b': tag = HV_TAG_B; break;
                    case 'i': tag = HV_TAG_I; break;
                }
            }

            // Extract href from <a> attributes, then skip
            // remaining attributes to the end of the tag
            int href_extracted = 0;
            char href_val[HV_URL_MAX];
            href_val[0] = '\0';
            int src_extracted = 0;
            char src_val[HV_URL_MAX];
            src_val[0] = '\0';

            while (html[pos] && html[pos] != '>') {
                // Check for href= in <a> tags
                if (is_a_tag && !closing && !href_extracted &&
                    ((html[pos] == 'h' || html[pos] == 'H') &&
                     (html[pos+1] == 'r' || html[pos+1] == 'R') &&
                     (html[pos+2] == 'e' || html[pos+2] == 'E') &&
                     (html[pos+3] == 'f' || html[pos+3] == 'F') &&
                     html[pos+4] == '=')) {
                    pos += 5;  // skip "href="
                    char quote = html[pos];
                    if (quote == '"' || quote == '\'') {
                        pos++;
                        int hi = 0;
                        while (html[pos] && html[pos] != quote && hi < HV_URL_MAX-1) {
                            href_val[hi++] = html[pos++];
                        }
                        href_val[hi] = '\0';
                        if (html[pos]) pos++;  // skip closing quote
                        href_extracted = 1;
                    }
                } else if (is_img_tag && !closing && !src_extracted &&
                           ((html[pos] == 's' || html[pos] == 'S') &&
                            (html[pos+1] == 'r' || html[pos+1] == 'R') &&
                            (html[pos+2] == 'c' || html[pos+2] == 'C') &&
                            html[pos+3] == '=')) {
                    pos += 4;  // skip "src="
                    char quote = html[pos];
                    if (quote == '"' || quote == '\'') {
                        pos++;
                        int si = 0;
                        while (html[pos] && html[pos] != quote && si < HV_URL_MAX-1) {
                            src_val[si++] = html[pos++];
                        }
                        src_val[si] = '\0';
                        if (html[pos]) pos++;  // skip closing quote
                        src_extracted = 1;
                    }
                } else if (html[pos] == '"') {
                    pos++;
                    while (html[pos] && html[pos] != '"') pos++;
                    if (html[pos]) pos++;
                } else if (html[pos] == '\'') {
                    pos++;
                    while (html[pos] && html[pos] != '\'') pos++;
                    if (html[pos]) pos++;
                } else {
                    pos++;
                }
            }
            if (html[pos] == '>') pos++;  // skip '>'

            // Determine tag for multi-char tag names
            if (tag == HV_TAG_OTHER) {
                if (tname_len == 2) {
                    if (tname_lower[0]=='b'&&tname_lower[1]=='r') tag = HV_TAG_BR;
                    else if (tname_lower[0]=='h'&&tname_lower[1]=='r') tag = HV_TAG_HR;
                    else if (tname_lower[0]=='u'&&tname_lower[1]=='l') tag = HV_TAG_UL;
                    else if (tname_lower[0]=='o'&&tname_lower[1]=='l') tag = HV_TAG_OL;
                    else if (tname_lower[0]=='l'&&tname_lower[1]=='i') tag = HV_TAG_LI;
                    else if (tname_lower[0]=='d'&&tname_lower[1]=='i') tag = HV_TAG_DIV;
                    else if (tname_lower[0]=='e'&&tname_lower[1]=='m') tag = HV_TAG_EM;
                    else if (tname_lower[0]=='t'&&tname_lower[1]=='r') tag = HV_TAG_TR;
                    else if (tname_lower[0]=='t'&&tname_lower[1]=='d') tag = HV_TAG_TD;
                    else if (tname_lower[0]=='t'&&tname_lower[1]=='h') tag = HV_TAG_TH;
                }
                if (tname_len == 3) {
                    if (tname_lower[0]=='p'&&tname_lower[1]=='r'&&tname_lower[2]=='e')
                        tag = HV_TAG_PRE;
                    else if (tname_lower[0]=='d'&&tname_lower[1]=='i'&&tname_lower[2]=='v')
                        tag = HV_TAG_DIV;
                    else if (tname_lower[0]=='s'&&tname_lower[1]=='p'&&tname_lower[2]=='a')
                        tag = HV_TAG_SPAN;
                    else if (tname_lower[0]=='i'&&tname_lower[1]=='m'&&tname_lower[2]=='g')
                        tag = HV_TAG_IMG;
                }
                if (tname_len == 4) {
                    if (tname_lower[0]=='b'&&tname_lower[1]=='o'&&tname_lower[2]=='d'&&tname_lower[3]=='y')
                        tag = HV_TAG_BODY;
                    else if (tname_lower[0]=='h'&&tname_lower[1]=='e'&&tname_lower[2]=='a'&&tname_lower[3]=='d')
                        tag = HV_TAG_HEAD;
                    else if (tname_lower[0]=='s'&&tname_lower[1]=='t'&&tname_lower[2]=='r'&&tname_lower[3]=='o')
                        tag = HV_TAG_STRONG;
                }
                if (tname_len == 5) {
                    if (tname_lower[0]=='t'&&tname_lower[1]=='i'&&tname_lower[2]=='t'&&tname_lower[3]=='l'&&tname_lower[4]=='e')
                        tag = HV_TAG_TITLE;
                    else if (tname_lower[0]=='s'&&tname_lower[1]=='t'&&tname_lower[2]=='y'&&tname_lower[3]=='l'&&tname_lower[4]=='e')
                        tag = HV_TAG_STYLE;
                    else if (tname_lower[0]=='t'&&tname_lower[1]=='a'&&tname_lower[2]=='b'&&tname_lower[3]=='l'&&tname_lower[4]=='e')
                        tag = HV_TAG_TABLE;
                    else if (tname_lower[0]=='t'&&tname_lower[1]=='h'&&tname_lower[2]=='e'&&tname_lower[3]=='a'&&tname_lower[4]=='d')
                        tag = HV_TAG_THEAD;
                    else if (tname_lower[0]=='t'&&tname_lower[1]=='b'&&tname_lower[2]=='o'&&tname_lower[3]=='d'&&tname_lower[4]=='y')
                        tag = HV_TAG_TBODY;
                    else if (tname_lower[0]=='t'&&tname_lower[1]=='f'&&tname_lower[2]=='o'&&tname_lower[3]=='o'&&tname_lower[4]=='t')
                        tag = HV_TAG_TFOOT;
                    else if (tname_lower[0]=='c'&&tname_lower[1]=='a'&&tname_lower[2]=='p'&&tname_lower[3]=='t'&&tname_lower[4]=='i')
                        tag = HV_TAG_CAPTION;
                }
                if (tname_len == 6) {
                    if (tname_lower[0]=='s'&&tname_lower[1]=='c'&&tname_lower[2]=='r'&&tname_lower[3]=='i'&&tname_lower[4]=='p'&&tname_lower[5]=='t')
                        tag = HV_TAG_SCRIPT;
                }
            }

            // ── Handle closing tags ──
            if (closing) {
                switch (tag) {
                    case HV_TAG_H1:
                    case HV_TAG_H2:
                    case HV_TAG_H3:
                    case HV_TAG_H4:
                    case HV_TAG_H5:
                    case HV_TAG_H6:
                    case HV_TAG_P:
                    case HV_TAG_DIV:
                        need_para_space = 1;
                        hv_pop_style();
                        break;
                    case HV_TAG_A: {
                        hv_cur.underline = 0;
                        hv_pop_style();
                        // Record the link if it has valid text
                        if (in_a_tag && a_href[0] && hv_link_count < HV_MAX_LINKS) {
                            HvLink* l = &hv_links[hv_link_count];
                            l->line = a_start_line;
                            l->col = a_start_col;
                            l->len = hv_lines[hv_line_count].len - a_start_col;
                            if (l->len < 0) l->len = 0;
                            int hi = 0;
                            while (a_href[hi] && hi < HV_URL_MAX-1) {
                                l->href[hi] = a_href[hi];
                                hi++;
                            }
                            l->href[hi] = '\0';
                            hv_link_count++;
                        }
                        in_a_tag = 0;
                        break;
                    }
                    case HV_TAG_B:
                    case HV_TAG_STRONG:
                        hv_cur.bold = 0;
                        hv_pop_style();
                        break;
                    case HV_TAG_I:
                    case HV_TAG_EM:
                        hv_cur.italic = 0;
                        hv_pop_style();
                        break;
                    case HV_TAG_PRE:
                        in_pre = 0;
                        hv_pop_style();
                        break;
                    case HV_TAG_UL:
                    case HV_TAG_OL:
                        list_type = 0;
                        list_counter = 0;
                        list_indent = 0;
                        break;
                    case HV_TAG_TABLE:
                        need_para_space = 1;
                        table_cell_count = 0;
                        break;
                    case HV_TAG_TR:
                        hv_newline();
                        table_cell_count = 0;
                        break;
                    case HV_TAG_TD:
                    case HV_TAG_TH:
                    case HV_TAG_CAPTION:
                        hv_pop_style();
                        break;
                    case HV_TAG_THEAD:
                    case HV_TAG_TBODY:
                    case HV_TAG_TFOOT:
                        break;
                    case HV_TAG_SCRIPT:
                        in_script = 0;
                        break;
                    case HV_TAG_STYLE:
                        in_style_block = 0;
                        break;
                    case HV_TAG_TITLE:
                        in_title = 0;
                        hv_page_title[page_title_pos] = '\0';
                        break;
                    default:
                        break;
                }
                continue;
            }

            // ── Handle opening tags ──
            switch (tag) {
                case HV_TAG_H1:
                case HV_TAG_H2:
                case HV_TAG_H3:
                case HV_TAG_H4:
                case HV_TAG_H5:
                case HV_TAG_H6: {
                    int level = tag - HV_TAG_H1 + 1;
                    if (need_para_space) { hv_newline(); need_para_space = 0; }
                    hv_newline();
                    hv_push_style();
                    hv_cur.fg = hv_heading_fg(level);
                    hv_cur.bg = hv_heading_bg(level);
                    hv_cur.bold = 1;
                    hv_cur.heading = level;
                    break;
                }
                case HV_TAG_P:
                    if (need_para_space) { hv_newline(); }
                    hv_newline();
                    hv_push_style();
                    need_para_space = 0;
                    break;
                case HV_TAG_DIV:
                    if (need_para_space) { hv_newline(); }
                    hv_push_style();
                    need_para_space = 0;
                    break;
                case HV_TAG_A:
                    hv_push_style();
                    hv_cur.fg = 0x66BBFF;
                    hv_cur.underline = 1;
                    // Start tracking the link
                    in_a_tag = 1;
                    a_first_char = 1;
                    if (href_extracted && href_val[0]) {
                        int hi = 0;
                        while (href_val[hi] && hi < HV_URL_MAX-1) {
                            a_href[hi] = href_val[hi];
                            hi++;
                        }
                        a_href[hi] = '\0';
                    } else {
                        a_href[0] = '\0';
                    }
                    break;
                case HV_TAG_B:
                case HV_TAG_STRONG:
                    hv_push_style();
                    hv_cur.bold = 1;
                    break;
                case HV_TAG_I:
                case HV_TAG_EM:
                    hv_push_style();
                    hv_cur.italic = 1;
                    hv_cur.fg = 0xCCAAFF;
                    break;
                case HV_TAG_PRE:
                    if (need_para_space) { hv_newline(); need_para_space = 0; }
                    hv_push_style();
                    hv_cur.pre = 1;
                    hv_cur.bg = 0x0E1828;
                    in_pre = 1;
                    break;
                case HV_TAG_UL:
                    list_type = 1;
                    list_counter = 0;
                    list_indent = 4;
                    if (need_para_space) { hv_newline(); need_para_space = 0; }
                    hv_newline();
                    break;
                case HV_TAG_OL:
                    list_type = 2;
                    list_counter = 0;
                    list_indent = 4;
                    if (need_para_space) { hv_newline(); need_para_space = 0; }
                    hv_newline();
                    break;
                case HV_TAG_TABLE:
                    if (need_para_space) { hv_newline(); need_para_space = 0; }
                    hv_newline();
                    table_cell_count = 0;
                    break;
                case HV_TAG_TR:
                    if (need_para_space) { hv_newline(); need_para_space = 0; }
                    hv_newline();
                    table_cell_count = 0;
                    break;
                case HV_TAG_TD:
                case HV_TAG_TH: {
                    // Separate cells with a pipe so columns stay readable
                    if (table_cell_count == 0) {
                        hv_indent(2);
                    } else {
                        hv_puts(" | ");
                    }
                    table_cell_count++;
                    hv_push_style();
                    hv_cur.bold = 1;
                    hv_cur.fg = (tag == HV_TAG_TH) ? 0xFFDD88 : 0xE0D8FF;
                    break;
                }
                case HV_TAG_THEAD:
                case HV_TAG_TBODY:
                case HV_TAG_TFOOT:
                    break;
                case HV_TAG_CAPTION:
                    if (need_para_space) { hv_newline(); need_para_space = 0; }
                    hv_newline();
                    hv_push_style();
                    hv_cur.italic = 1;
                    hv_cur.fg = 0xBBCCFF;
                    break;
                case HV_TAG_LI:
                    hv_newline();
                    hv_indent(list_indent);
                    if (list_type == 1) {
                        hv_putch(0x9F, 0xFFDD66, hv_cur.bg, 0);
                        hv_putch(' ', 0xFFDD66, hv_cur.bg, 0);
                    } else if (list_type == 2) {
                        list_counter++;
                        char num[8];
                        int np = 0;
                        int n = list_counter;
                        while (n > 0 && np < 6) { num[np++] = '0' + n%10; n /= 10; }
                        for (int ni = np-1; ni >= 0; ni--)
                            hv_putch(num[ni], 0xFFDD66, hv_cur.bg, 0);
                        hv_putch('.', 0xFFDD66, hv_cur.bg, 0);
                        hv_putch(' ', 0xFFDD66, hv_cur.bg, 0);
                    }
                    break;
                case HV_TAG_BR:
                    hv_newline();
                    break;
                case HV_TAG_HR:
                    if (need_para_space) { hv_newline(); need_para_space = 0; }
                    hv_newline();
                    for (int i = 0; i < HV_LINE_W; i++)
                        hv_putch(0xC4, 0x665588, 0x0E0C22, 0);
                    hv_newline();
                    break;
                case HV_TAG_BODY:
                    break;
                case HV_TAG_SCRIPT:
                    in_script = 1;
                    break;
                case HV_TAG_STYLE:
                    in_style_block = 1;
                    break;
                case HV_TAG_IMG: {
                    // Collect the image into a slot whose box is drawn by
                    // the panel renderer (VESA); in VGA mode (or when the
                    // slot table is full) fall back to the old text
                    // placeholder so the image is at least visible.
                    if (vesa.active && hv_img_count < HV_MAX_IMGS) {
                        int slot = hv_img_count++;
                        HvImg* im = &hv_imgs[slot];
                        im->state = HV_IMG_STATE_PEND;
                        im->line = hv_line_count;
                        im->w = 0; im->h = 0;
                        int si = 0;
                        if (src_extracted) {
                            while (src_val[si] && si < HV_URL_MAX - 1) {
                                im->src[si] = src_val[si];
                                si++;
                            }
                        }
                        im->src[si] = '\0';
                        // Reserve the box: the image line plus the rows
                        // below it (kept blank so text never overlaps the
                        // picture).  Fixed reservation keeps the layout
                        // and scrollbar deterministic before bytes arrive.
                        hv_lines[hv_line_count].img = slot;
                        for (int b = 0; b < HV_IMG_BOX_LINES; b++)
                            hv_newline();
                    } else {
                        // Show placeholder with src filename
                        char img_buf[80];
                        int ip = 0;
                        img_buf[ip++] = '[';
                        img_buf[ip++] = 'i';
                        img_buf[ip++] = 'm';
                        img_buf[ip++] = 'g';
                        img_buf[ip++] = ':';
                        img_buf[ip++] = ' ';
                        if (src_extracted && src_val[0]) {
                            // Extract just the filename from the path
                            int last_slash = -1;
                            int si = 0;
                            while (src_val[si]) {
                                if (src_val[si] == '/' || src_val[si] == '\\')
                                    last_slash = si;
                                si++;
                            }
                            int fn_start = last_slash + 1;
                            while (src_val[fn_start] && ip < 70) {
                                img_buf[ip++] = src_val[fn_start++];
                            }
                        } else {
                            // No src extracted — show generic
                            img_buf[ip++] = '?';
                        }
                        img_buf[ip++] = ']';
                        img_buf[ip] = '\0';
                        hv_puts(img_buf);
                    }
                    break;
                }
                case HV_TAG_TITLE:
                    in_title = 1;
                    page_title_pos = 0;
                    hv_page_title[0] = '\0';
                    break;
                case HV_TAG_HEAD:
                case HV_TAG_HTML:
                case HV_TAG_SPAN:
                case HV_TAG_OTHER:
                default:
                    break;
            }
            continue;
        }

        // ── Handle text content ──
        // Skip content in <script>, <style>
        if (in_script || in_style_block) {
            pos++;
            continue;
        }

        // If we're inside <title>, capture text without rendering
        if (in_title) {
            char decoded;
            int new_pos = hv_decode_entity(html, pos, &decoded);
            if (decoded != '\n' && decoded != '\r' && decoded != '\t') {                    if (page_title_pos < 62) {
                        hv_page_title[page_title_pos++] = decoded;
                        hv_page_title[page_title_pos] = '\0';
                    }
            }
            pos = new_pos;
            continue;
        }

        // Decode HTML entity or get character
        char decoded;
        int new_pos = hv_decode_entity(html, pos, &decoded);
        pos = new_pos;

        if (in_pre) {
            // Pre mode: preserve whitespace
            if (decoded == '\n') {
                hv_newline();
            } else if (decoded == '\t') {
                for (int i = 0; i < 4; i++)
                    hv_putch(' ', hv_cur.fg, hv_cur.bg, 0);
            } else {
                hv_putch(decoded, hv_cur.fg, hv_cur.bg, hv_cur.underline);
            }
        } else {
            // Normal mode: collapse whitespace
            int is_space = (decoded == ' ' || decoded == '\t' ||
                            decoded == '\n' || decoded == '\r');

            if (is_space) {
                // Collapse multiple whitespace into single space
                // BUT don't start a line with space
                HvLine* line = &hv_lines[hv_line_count];
                if (line->len > 0 && line->chars[line->len-1].ch != ' ') {
                    hv_putch(' ', hv_cur.fg, hv_cur.bg, 0);
                }
            } else {
                // If we're in an <a> tag, record the start position
                // of the first non-space character
                if (in_a_tag && a_first_char) {
                    if (decoded != ' ') {
                        a_start_line = hv_line_count;
                        a_start_col = hv_lines[hv_line_count].len;
                        a_first_char = 0;
                    }
                }
                hv_putch(decoded, hv_cur.fg, hv_cur.bg, hv_cur.underline);
            }
        }
    }

    // Done. Ensure we have at least one line.
    if (hv_line_count == 0) hv_newline();
}

// ─── VGA text-mode rendering ──────────────────────────────────────
// In VGA mode we use the VGA color attributes directly.

#define VGA_MEM ((uint16_t*)0xB8000)
#define VGA_W   80
#define VGA_H   25

static inline uint16_t hv_vga_entry(char c, uint8_t fg, uint8_t bg) {
    return (uint16_t)c | (uint16_t)(fg | (bg << 4)) << 8;
}

// Map our rich color to the closest VGA palette index (0-15)
static uint8_t hv_rgb_to_vga_idx(uint32_t rgb) {
    int r = (rgb >> 16) & 0xFF;
    int g = (rgb >> 8) & 0xFF;
    int b = rgb & 0xFF;

    // VGA 16-color palette (standard)
    static const uint8_t vga_r[16] =
        {0,0,0,0,170,170,85,192,128,128,255,255,255,255,255,255};
    static const uint8_t vga_g[16] =
        {0,0,170,170,0,170,85,192,128,255,128,255,128,255,255,255};
    static const uint8_t vga_b[16] =
        {0,170,0,170,0,0,85,192,128,255,255,128,128,128,255,255};

    int best = 7; // default light grey
    int best_dist = 999999;
    for (int i = 0; i < 16; i++) {
        int dr = r - vga_r[i];
        int dg = g - vga_g[i];
        int db = b - vga_b[i];
        int dist = dr*dr + dg*dg + db*db;
        if (dist < best_dist) {
            best_dist = dist;
            best = i;
        }
    }
    return (uint8_t)best;
}

// ─── VESA panel rendering ─────────────────────────────────────────
// Draws the rich text buffer to a centered panel.

#define HV_PANEL_MARGIN_X  60
#define HV_PANEL_MARGIN_Y  40
#define HV_TITLE_BAR_H     32
#define HV_BOTTOM_BAR_H    28

static void hv_draw_panel(void) {
    if (!vesa.active) {
        // VGA mode: render rich text lines to VGA buffer
        for (int y = 0; y < VGA_H; y++) {
            int line_idx = hv_scroll_y + y;
            // Check if this line has the cursor link
            int is_cursor_line = 0;
            if (hv_link_cursor >= 0 && hv_link_cursor < hv_link_count &&
                hv_links[hv_link_cursor].line == line_idx)
                is_cursor_line = 1;

            for (int x = 0; x < VGA_W; x++) {
                uint8_t fg = 7, bg = 0;
                char ch = ' ';

                if (x == 0 && is_cursor_line) {
                    ch = '>';
                    fg = 15;
                    bg = 1;
                } else if (line_idx < hv_line_count) {
                    int hx = is_cursor_line ? x - 1 : x;
                    if (hx >= 0 && hx < hv_lines[line_idx].len) {
                        HvChar* rc = &hv_lines[line_idx].chars[hx];
                        fg = hv_rgb_to_vga_idx(rc->fg);
                        bg = hv_rgb_to_vga_idx(rc->bg);
                        ch = rc->ch;
                    }
                }
                // Cursor line gets a highlighted background
                if (is_cursor_line && (ch == ' ' || bg == 0)) {
                    bg = 4;  // dark red background for cursor line
                }
                VGA_MEM[y * VGA_W + x] = hv_vga_entry(ch, fg, bg);
            }
        }
        // Title bar
        for (int x = 0; x < VGA_W; x++)
            VGA_MEM[x] = hv_vga_entry(' ', 15, 1);
        for (int i = 0; hv_title[i] && i < VGA_W-2; i++)
            VGA_MEM[1 * VGA_W + 2 + i] = hv_vga_entry(hv_title[i], 15, 1);
        // Bottom bar
        for (int x = 0; x < VGA_W; x++)
            VGA_MEM[(VGA_H-1) * VGA_W + x] = hv_vga_entry(' ', 7, 0);
        return;
    }

    // ── VESA mode ──
    int fb_w = (int)vesa.width;
    int fb_h = (int)vesa.height;

    // Panel area: window mode uses the rect set by the caller (e.g. the
    // browser), otherwise a full-screen centered panel.
    int win_mode = (hv_win_w > 0);
    int px = HV_PANEL_MARGIN_X;
    int py = HV_PANEL_MARGIN_Y;
    int pw = fb_w - HV_PANEL_MARGIN_X * 2;
    int ph = fb_h - HV_PANEL_MARGIN_Y * 2;
    if (win_mode) {
        px = hv_win_x; py = hv_win_y; pw = hv_win_w; ph = hv_win_h;
    }

    if (pw < 320) { pw = fb_w - 20; px = 10; }
    if (ph < 200) { ph = fb_h - 20; py = 10; }

    // Background
    vesa_fill_rect(px, py, pw, ph, 0x0E0C22);
    vesa_draw_rect(px, py, pw, ph, 0x3A2860);
    vesa_draw_rect(px+1, py+1, pw-2, ph-2, 0x201848);

    // Title bar — skipped in window mode (the caller draws its own
    // chrome, e.g. the browser's address + status bars)
    int tby = py + 4;
    int tbh = win_mode ? 0 : HV_TITLE_BAR_H;
    if (!win_mode) {
        vesa_fill_rect(px+4, tby, pw-8, tbh, 0x181440);
        vesa_draw_rect(px+4, tby, pw-8, tbh, 0x3A2860);
        // Accent line under title
        vesa_hline(px+6, tby + tbh - 1, pw-12, 0x7050CC);

        // Title text (truncated if too wide)
        int title_x = px + 14;
        int title_y = tby + (tbh - 16) / 2;
        vesa_draw_string(title_x, title_y, hv_title, 0xFFFFFF, 0x181440);

        // Subtitle showing page info
        char page_info[24];
        int info_pos = 0;
        // Total pages
        if (hv_line_count > 0) {
            int vis_lines = (ph - tbh - HV_BOTTOM_BAR_H - 12) / 16;
            int total_pages = (hv_line_count + vis_lines - 1) / vis_lines;
            int cur_page = hv_scroll_y / vis_lines + 1;

            page_info[info_pos++] = 'P';
            page_info[info_pos++] = 'g';
            page_info[info_pos++] = ' ';
            int cn = cur_page;
            char cb[6]; int cp = 0;
            while (cn > 0 && cp < 5) { cb[cp++] = '0' + cn%10; cn /= 10; }
            for (int ci = cp - 1; ci >= 0; ci--) page_info[info_pos++] = cb[ci];
            page_info[info_pos++] = '/';
            cn = total_pages;
            cp = 0;
            while (cn > 0 && cp < 5) { cb[cp++] = '0' + cn%10; cn /= 10; }
            if (cp == 0) cb[cp++] = '0';
            for (int ci = cp - 1; ci >= 0; ci--) page_info[info_pos++] = cb[ci];
        }
        page_info[info_pos] = '\0';
        int info_x = px + pw - 14 - info_pos * 8;
        vesa_draw_string(info_x, title_y, page_info, 0x8877AA, 0x181440);
    }

    // Content area
    int content_x = px + 10;
    int content_y;
    int content_h;
    if (win_mode) {
        content_y = py + hv_win_top + 2;
        content_h = ph - hv_win_top - HV_BOTTOM_BAR_H - 14;
    } else {
        content_y = tby + tbh + 4;
        content_h = ph - tbh - HV_BOTTOM_BAR_H - 12;
    }
    int content_w = pw - 20;
    int vis_chars = content_w / 8;
    int vis_lines = content_h / 16;

    // Clip scroll
    int max_scroll = hv_line_count - vis_lines;
    if (max_scroll < 0) max_scroll = 0;
    if (hv_scroll_y > max_scroll) hv_scroll_y = max_scroll;

    // Draw content lines
    for (int i = 0; i < vis_lines; i++) {
        int line_idx = hv_scroll_y + i;
        if (line_idx >= hv_line_count) break;

        HvLine* line = &hv_lines[line_idx];
        int ly = content_y + i * 16;

    // ── Image box lines ──────────────────────────────────────
    // The line carrying an <img> slot draws the picture (or its
    // placeholder) spanning HV_IMG_BOX_LINES rows; the rows inside
    // that span draw only background so text never overlaps.
    int img_slot = hv_lines[line_idx].img;
        int in_img_span = 0;
        if (img_slot < 0) {
            for (int s = 0; s < hv_img_count; s++) {
                int il = hv_imgs[s].line;
                if (line_idx > il && line_idx < il + HV_IMG_BOX_LINES) {
                    in_img_span = 1;
                    break;
                }
            }
        }
        if (img_slot >= 0 && img_slot < hv_img_count) {
            int box_h = HV_IMG_BOX_LINES * 16;
            int box_bottom = content_y + content_h;
            if (ly + box_h > box_bottom) box_h = box_bottom - ly;
            if (box_h < 4) box_h = 4;
            int box_w = vis_chars * 8;
            // Background + border for the box
            vesa_fill_rect(content_x, ly, box_w, box_h, 0x0E0C22);
            vesa_draw_rect(content_x, ly, box_w, box_h, 0x3A2A60);
            vesa_draw_rect(content_x + 1, ly + 1, box_w - 2, box_h - 2, 0x221A44);

            HvImg* im = &hv_imgs[img_slot];
            if (im->state == HV_IMG_STATE_OK && im->w > 0 && im->h > 0) {
                // Aspect-fit the picture inside the box (with padding).
                int cw = box_w - 16;
                int ch = box_h - 8;
                if (cw < 8) cw = 8;
                if (ch < 8) ch = 8;
                int dw = cw, dh = ch;
                if (im->w * ch > im->h * cw)
                    dh = im->h * cw / im->w;   // too wide → limit by width
                else
                    dw = im->w * ch / im->h;   // too tall → limit by height
                if (dw < 2) dw = 2;
                if (dh < 2) dh = 2;
                int dx = content_x + (box_w - dw) / 2;
                int dy = ly + (box_h - dh) / 2;
                vesa_blit_rgba_scaled(dx, dy, dw, dh, im->pixels, im->w, im->h);
            } else {
                // Pending or failed: filename + status inside the box.
                char cap[64];
                int ci = 0;
                int last_slash = -1, si = 0;
                while (im->src[si]) {
                    if (im->src[si] == '/' || im->src[si] == '\\')
                        last_slash = si;
                    si++;
                }
                int fs = last_slash + 1;
                while (im->src[fs] && ci < 50) cap[ci++] = im->src[fs++];
                if (ci == 0) { cap[ci++] = '?'; cap[ci++] = '?'; cap[ci++] = '?'; }
                cap[ci] = '\0';
                int cx = content_x + (box_w - ci * 8) / 2;
                int cy = ly + box_h / 2 - 16;
                if (cy < ly) cy = ly;
                vesa_draw_string(cx, cy, cap,
                                 im->state == HV_IMG_STATE_FAIL ? 0xFF8866 : 0x8877AA,
                                 0x0E0C22);
                const char* st = im->state == HV_IMG_STATE_FAIL ? "[failed]" : "[loading...]";
                int sl = 0; while (st[sl]) sl++;
                vesa_draw_string(content_x + (box_w - sl * 8) / 2, cy + 16, st,
                                 0x665588, 0x0E0C22);
            }
            continue;   // box already painted the span rows
        }
        if (in_img_span) {
            vesa_fill_rect(content_x, ly, vis_chars * 8, 16, 0x0E0C22);
            continue;
        }

        // Check if this line contains any link
        int line_has_link = 0;
        int line_is_cursor = 0;
        for (int li = 0; li < hv_link_count; li++) {
            if (hv_links[li].line == line_idx && hv_links[li].href[0]) {
                line_has_link = 1;
                if (hv_link_cursor >= 0 && hv_link_cursor < hv_link_count) {
                    if (li == hv_link_cursor) line_is_cursor = 1;
                }
                break;
            }
        }

        // Draw base background for the line
        uint32_t line_bg = 0x0E0C22;
        if (line_is_cursor) {
            // Brighter highlight for cursor link line
            line_bg = 0x1E1850;
            // Draw a full-row glow
            vesa_fill_rect(content_x - 6, ly, vis_chars * 8 + 6, 16, line_bg);
            // Top/bottom borders for cursor row
            vesa_hline(content_x - 6, ly,       vis_chars * 8 + 6, 0x5038AA);
            vesa_hline(content_x - 6, ly + 15,   vis_chars * 8 + 6, 0x5038AA);
        } else if (line_has_link) {
            line_bg = 0x120E28;
            vesa_fill_rect(content_x, ly, vis_chars * 8, 16, line_bg);
        } else {
            vesa_fill_rect(content_x, ly, vis_chars * 8, 16, line_bg);
        }

        // Draw the ▶ cursor indicator to the left of the line
        if (line_is_cursor) {
            // Draw ▶ using vesa_draw_char at (content_x - 14, ly)
            vesa_draw_char(content_x - 14, ly, 0x10, 0x55CCFF, 0x0E0C22); // ▶ CP437
            // Bright left marker
            vesa_fill_rect(content_x - 3, ly + 2, 2, 12, 0x55CCFF);
        } else if (line_has_link) {
            // Subtle left marker for non-cursor link lines
            vesa_fill_rect(content_x - 3, ly + 2, 2, 12, 0x3A70AA);
        }

        // Draw each character
        for (int ci = 0; ci < line->len && ci < vis_chars; ci++) {
            HvChar* rc = &line->chars[ci];
            int cx = content_x + ci * 8;
            // Draw with background first, then character
            vesa_draw_char(cx, ly, rc->ch, rc->fg, rc->bg);
            // Underline: draw a thin line under the character
            if (rc->underline) {
                vesa_hline(cx, ly + 14, 8, rc->fg);
            }
        }

        // Bold effect: re-draw the character 1px to the right for bold text
        // (Actually we handle bold by using a brighter color during parsing)
    }

    // Scrollbar
    int sb_x = px + pw - 8;
    int sb_y = content_y;
    int sb_h = content_h;
    vesa_fill_rect(sb_x, sb_y, 6, sb_h, 0x14102A);

    if (hv_line_count > vis_lines) {
        int thumb_h = sb_h * vis_lines / hv_line_count;
        if (thumb_h < 8) thumb_h = 8;
        int thumb_y = sb_y + (sb_h - thumb_h) * hv_scroll_y /
                      (hv_line_count - vis_lines);
        if (thumb_y < sb_y) thumb_y = sb_y;
        if (thumb_y + thumb_h > sb_y + sb_h) thumb_y = sb_y + sb_h - thumb_h;
        vesa_fill_rect(sb_x, thumb_y, 6, thumb_h, 0x7050CC);
    }

    // Bottom bar
    int bby = content_y + content_h + 2;
    int bbh = HV_BOTTOM_BAR_H;
    vesa_fill_rect(px+4, bby, pw-8, bbh, 0x14102A);
    vesa_draw_rect(px+4, bby, pw-8, bbh, 0x2A2050);

    // Bottom controls text — show TAB/ENTER hints when links are present
    const char* controls;
    if (hv_link_count > 0) {
        controls = "\x18\x19 Scroll  |  TAB Link  |  ENTER Follow  |  ESC Exit";
    } else {
        controls = "\x18\x19 Scroll  |  ESC Exit";
    }
    int cw = 0;
    for (int ci = 0; controls[ci]; ci++) cw++;
    int ctrl_x = px + (pw - cw * 8) / 2;
    vesa_draw_string(ctrl_x, bby + (bbh - 16) / 2, controls, 0x8877AA, 0x14102A);
}

// ─── Public API ───────────────────────────────────────────────────

int htmlview_render(const char* html, const char* title) {
    if (!html || !html[0]) return -1;

    // Copy title
    if (title) {
        int ti = 0;
        while (title[ti] && ti < 63) { hv_title[ti] = title[ti]; ti++; }
        hv_title[ti] = '\0';
    } else {
        const char* def = "HTML Viewer";
        int ti = 0;
        while (def[ti] && ti < 63) { hv_title[ti] = def[ti]; ti++; }
        hv_title[ti] = '\0';
    }

    // Parse HTML
    hv_parse(html);

    // If the page had a <title> tag, use it in the title bar
    // (but fall back to the caller-provided title so http get
    // still shows the hostname when there is no <title>)
    if (hv_page_title[0]) {
        int ti = 0;
        while (hv_page_title[ti] && ti < 63) { hv_title[ti] = hv_page_title[ti]; ti++; }
        hv_title[ti] = '\0';
        // Trim trailing whitespace
        while (ti > 0 && (hv_title[ti-1] == ' ' || hv_title[ti-1] == '\t' ||
               hv_title[ti-1] == '\n' || hv_title[ti-1] == '\r'))
            ti--;
        hv_title[ti] = '\0';
    }

    hv_scroll_y = 0;
    hv_active = 1;

    // Draw the panel
    hv_draw_panel();

    return 0;
}

void htmlview_open(const char* html, const char* title) {
    htmlview_render(html, title);
}

void htmlview_draw(void) {
    if (!hv_active) return;
    hv_draw_panel();
}

void htmlview_set_window(int x, int y, int w, int h, int top) {
    hv_win_x = x; hv_win_y = y; hv_win_w = w; hv_win_h = h; hv_win_top = top;
}

void htmlview_clear_window(void) {
    hv_win_w = 0;
}

void htmlview_handle_key(char c) {
    if (!hv_active) return;

    if (!vesa.active) {
        // VGA mode: calculate visible lines
        int vis_lines = VGA_H - 2; // minus top/bottom bars
        int max_scroll = hv_line_count - vis_lines;
        if (max_scroll < 0) max_scroll = 0;

        if (c == 'w' || c == 'W' || c == 0x48) { // UP
            hv_link_cursor = -1;
            if (hv_scroll_y > 0) hv_scroll_y--;
        } else if (c == 's' || c == 'S' || c == 0x50) { // DOWN
            hv_link_cursor = -1;
            if (hv_scroll_y < max_scroll) hv_scroll_y++;
        } else if (c == 27) { // ESC
            hv_active = 0;
        }
        hv_draw_panel();
        return;
    }

    // VESA mode
    int vis_lines;
    if (hv_win_w > 0)
        vis_lines = (hv_win_h - hv_win_top - HV_BOTTOM_BAR_H - 14) / 16;
    else
        vis_lines = ((int)vesa.height - HV_PANEL_MARGIN_Y * 2 - HV_TITLE_BAR_H - HV_BOTTOM_BAR_H - 12) / 16;
    int max_scroll = hv_line_count - vis_lines;
    if (max_scroll < 0) max_scroll = 0;

    if (c == 'w' || c == 'W' || c == 0x48) { // UP arrow
        hv_link_cursor = -1;
        if (hv_scroll_y > 0) {
            hv_scroll_y--;
            hv_draw_panel();
        }
    } else if (c == 's' || c == 'S' || c == 0x50) { // DOWN arrow
        hv_link_cursor = -1;
        if (hv_scroll_y < max_scroll) {
            hv_scroll_y++;
            hv_draw_panel();
        }
    } else if (c == 27) { // ESC
        hv_active = 0;
    } else if (c == 9) { // TAB — cycle to next visible link
        int vi[HV_MAX_LINKS];
        int vn = hv_visible_links(vi, HV_MAX_LINKS, hv_scroll_y, hv_scroll_y + vis_lines);
        if (vn > 0) {
            // Find current cursor position in visible list
            int cur_pos = -1;
            for (int j = 0; j < vn; j++) {
                if (vi[j] == hv_link_cursor) { cur_pos = j; break; }
            }
            // Move to next (wrap around)
            int next_pos = (cur_pos + 1) % vn;
            hv_link_cursor = vi[next_pos];

            // Auto-scroll to make cursor visible if needed
            int cl = hv_links[hv_link_cursor].line;
            if (cl < hv_scroll_y) {
                hv_scroll_y = cl;
            } else if (cl >= hv_scroll_y + vis_lines) {
                hv_scroll_y = cl - vis_lines + 1;
            }
            if (hv_scroll_y < 0) hv_scroll_y = 0;
            if (hv_scroll_y > max_scroll) hv_scroll_y = max_scroll;

            hv_draw_panel();
        }
    } else if (c == '\n' || c == '\r' || c == 13) { // ENTER — follow cursor link
        int follow_idx = -1;
        if (hv_link_cursor >= 0 && hv_link_cursor < hv_link_count) {
            // Follow the cursor-highlighted link
            follow_idx = hv_link_cursor;
        } else {
            // Fallback: find first visible link
            int vi[HV_MAX_LINKS];
            int vn = hv_visible_links(vi, HV_MAX_LINKS, hv_scroll_y, hv_scroll_y + vis_lines);
            if (vn > 0) follow_idx = vi[0];
        }

        if (follow_idx >= 0) {
            int pi = 0;
            while (hv_links[follow_idx].href[pi] && pi < HV_URL_MAX-1) {
                hv_pending_url[pi] = hv_links[follow_idx].href[pi];
                pi++;
            }
            hv_pending_url[pi] = '\0';
            hv_active = 0;
        }
    }
}

// ─── Wheel scrolling ─────────────────────────────────────────────
// Scroll the viewer by `delta` lines (positive = down toward later
// content).  Clamped to the valid range; returns 1 if the scroll
// position actually changed so the caller can skip a redraw when
// the page is already pinned to an edge.  Used by the desktop
// heartbeat to route touchpad wheel events into the browser page,
// mirroring the arrow-key scrolling in htmlview_handle_key.
int htmlview_scroll(int delta) {
    if (!hv_active) return 0;
    if (delta == 0) return 0;

    int vis_lines;
    if (!vesa.active) {
        vis_lines = VGA_H - 2;  // minus top/bottom bars
    } else if (hv_win_w > 0) {
        vis_lines = (hv_win_h - hv_win_top - HV_BOTTOM_BAR_H - 14) / 16;
    } else {
        vis_lines = ((int)vesa.height - HV_PANEL_MARGIN_Y * 2 -
                     HV_TITLE_BAR_H - HV_BOTTOM_BAR_H - 12) / 16;
    }
    if (vis_lines < 1) vis_lines = 1;

    int max_scroll = hv_line_count - vis_lines;
    if (max_scroll < 0) max_scroll = 0;

    int ns = hv_scroll_y + delta;
    if (ns < 0) ns = 0;
    if (ns > max_scroll) ns = max_scroll;
    if (ns == hv_scroll_y) return 0;

    hv_scroll_y = ns;
    hv_link_cursor = -1;  // wheel scrolling clears the link cursor
    return 1;
}

// ─── Public image API ────────────────────────────────────────────
// <img> slots are collected during parsing; the browser fetches each
// URL and feeds the raw bytes to htmlview_img_decode, which decodes
// (PNG/JPEG/BMP/GIF by magic bytes) and stores them for the renderer.

int htmlview_img_count(void) {
    return hv_img_count;
}

int htmlview_img_pending(int i) {
    if (i < 0 || i >= hv_img_count) return 0;
    return hv_imgs[i].state == HV_IMG_STATE_PEND;
}

const char* htmlview_img_get_src(int i) {
    if (i < 0 || i >= hv_img_count) return "";
    return hv_imgs[i].src;
}

int htmlview_img_decode(int i, const uint8_t* data, int len) {
    if (i < 0 || i >= hv_img_count || !data || len < 4) return 0;
    HvImg* im = &hv_imgs[i];
    if (im->state == HV_IMG_STATE_OK) return 1;

    // Decode into the big scratch; oversized results are scaled into
    // the slot buffer below.
    int w = 0, h = 0;
    if (len >= 8 && data[0] == 0x89 && data[1] == 'P' &&
        data[2] == 'N' && data[3] == 'G') {
        PngImage pi; pi.pixels = hv_img_scratch;
        if (png_decode(data, len, &pi)) { w = (int)pi.width; h = (int)pi.height; }
    } else if (len >= 3 && data[0] == 0xFF && data[1] == 0xD8) {
        JpegImage ji = {0}; ji.pixels = hv_img_scratch;
        if (jpeg_decode(data, len, &ji)) { w = (int)ji.width; h = (int)ji.height; }
    } else if (len >= 2 && data[0] == 'B' && data[1] == 'M') {
        BmpImage bi; bi.pixels = hv_img_scratch;
        if (bmp_decode(data, len, &bi)) { w = (int)bi.width; h = (int)bi.height; }
    } else if (len >= 6 && data[0] == 'G' && data[1] == 'I' && data[2] == 'F') {
        GifImage gi; gi.pixels = hv_img_scratch;
        if (gif_decode(data, len, &gi)) { w = (int)gi.width; h = (int)gi.height; }
    }
    if (w <= 0 || h <= 0 || w * h * 4 > (int)sizeof(hv_img_scratch)) {
        im->state = HV_IMG_STATE_FAIL;
        return 0;
    }

    // Scale into the slot buffer if the decoded image is larger.
    int sw = w, sh = h;
    if (sw > HV_IMG_MAX_W) { sh = sh * HV_IMG_MAX_W / sw; sw = HV_IMG_MAX_W; }
    if (sh > HV_IMG_MAX_H) { sw = sw * HV_IMG_MAX_H / sh; sh = HV_IMG_MAX_H; }
    if (sw == w && sh == h) {
        for (int k = 0; k < w * h * 4; k++)
            im->pixels[k] = hv_img_scratch[k];
    } else {
        // Nearest-neighbour scale (no FPU, no libc).
        const uint8_t* src = hv_img_scratch;
        uint8_t* dst = im->pixels;
        for (int y = 0; y < sh; y++) {
            int sy = y * h / sh;
            const uint8_t* srow = src + sy * w * 4;
            uint8_t* drow = dst + y * sw * 4;
            for (int x = 0; x < sw; x++) {
                int sx = x * w / sw;
                const uint8_t* sp = srow + sx * 4;
                drow[x * 4 + 0] = sp[0];
                drow[x * 4 + 1] = sp[1];
                drow[x * 4 + 2] = sp[2];
                drow[x * 4 + 3] = sp[3];
            }
        }
    }
    im->w = sw;
    im->h = sh;
    im->state = HV_IMG_STATE_OK;
    return 1;
}

int htmlview_is_open(void) {
    return hv_active;
}

void htmlview_close(void) {
    hv_active = 0;
    // Drop any caller-set window rect so a later standalone use (e.g.
    // the terminal's `http get`) falls back to the full-screen panel.
    hv_win_w = 0;
}

const char* htmlview_get_pending_url(void) {
    if (hv_pending_url[0]) return hv_pending_url;
    return NULL;
}
