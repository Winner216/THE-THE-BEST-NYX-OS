// include/terminal.h
#ifndef TERMINAL_H
#define TERMINAL_H

typedef enum {
    COLOR_BLACK         = 0,
    COLOR_BLUE          = 1,
    COLOR_GREEN         = 2,
    COLOR_CYAN          = 3,
    COLOR_RED           = 4,
    COLOR_MAGENTA       = 5,
    COLOR_BROWN         = 6,
    COLOR_LIGHT_GREY    = 7,
    COLOR_DARK_GREY     = 8,
    COLOR_LIGHT_BLUE    = 9,
    COLOR_LIGHT_GREEN   = 10,
    COLOR_LIGHT_CYAN    = 11,
    COLOR_LIGHT_RED     = 12,
    COLOR_LIGHT_MAGENTA = 13,
    COLOR_YELLOW        = 14,
    COLOR_WHITE         = 15,
} vga_color;

void terminal_init();
void terminal_putchar(char c);
void terminal_print(const char* str);
void terminal_scroll();
void terminal_set_color(vga_color fg, vga_color bg);

// ── Output redirect (used by the windowed Terminal app) ───────────
// When a redirect buffer is registered, terminal_print/putchar write
// into it instead of raw VGA memory. This lets shell.c's existing
// command logic stay unchanged while the windowed desktop terminal
// captures and renders its output as real VESA pixels.
#define TERM_REDIRECT_COLS 78
#define TERM_REDIRECT_ROWS 200   // scrollback length

typedef struct {
    char lines[TERM_REDIRECT_ROWS][TERM_REDIRECT_COLS + 1];
    int  line_count;   // total lines ever written (for scrollback math)
    int  cur_col;       // write cursor column on the active line
} term_redirect_t;

// Pass NULL to disable redirect (return to raw VGA output).
void terminal_set_redirect(term_redirect_t* buf);

// Fully clears a redirect buffer (used by the clear/cls command so
// it actually resets the windowed terminal instead of just scrolling
// past the visible 25 lines, which made no sense once scrollback
// grew to 200 lines).
void terminal_redirect_clear(term_redirect_t* buf);

#endif
