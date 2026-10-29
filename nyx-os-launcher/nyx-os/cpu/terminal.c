// cpu/terminal.c
#include <terminal.h>
#include <stdint.h>
#include <stddef.h>

#define VGA_WIDTH  80
#define VGA_HEIGHT 25
#define VGA_MEMORY (uint16_t*)0xB8000

static size_t terminal_row;
static size_t terminal_col;
static uint8_t terminal_color;
static uint16_t* terminal_buffer;
static term_redirect_t* redirect = 0;

static inline uint8_t make_color(vga_color fg, vga_color bg) {
    return fg | bg << 4;
}

static inline uint16_t make_vgaentry(char c, uint8_t color) {
    return (uint16_t)c | (uint16_t)color << 8;
}

void terminal_set_color(vga_color fg, vga_color bg) {
    terminal_color = make_color(fg, bg);
}

void terminal_set_redirect(term_redirect_t* buf) {
    redirect = buf;
}

void terminal_redirect_clear(term_redirect_t* buf) {
    if (!buf) return;
    for (int i = 0; i < TERM_REDIRECT_ROWS; i++) buf->lines[i][0] = '\0';
    buf->line_count = 0;
    buf->cur_col = 0;
}

void terminal_init() {
    terminal_row    = 0;
    terminal_col    = 0;
    terminal_color  = make_color(COLOR_LIGHT_CYAN, COLOR_BLACK);
    terminal_buffer = VGA_MEMORY;

    for (size_t y = 0; y < VGA_HEIGHT; y++)
        for (size_t x = 0; x < VGA_WIDTH; x++)
            terminal_buffer[y * VGA_WIDTH + x] =
                make_vgaentry(' ', terminal_color);
}

void terminal_scroll() {
    for (size_t y = 0; y < VGA_HEIGHT - 1; y++)
        for (size_t x = 0; x < VGA_WIDTH; x++)
            terminal_buffer[y * VGA_WIDTH + x] =
                terminal_buffer[(y + 1) * VGA_WIDTH + x];

    for (size_t x = 0; x < VGA_WIDTH; x++)
        terminal_buffer[(VGA_HEIGHT - 1) * VGA_WIDTH + x] =
            make_vgaentry(' ', terminal_color);

    if (terminal_row > 0)
        terminal_row = VGA_HEIGHT - 1;
}

void terminal_putchar(char c) {
    if (redirect) {
        // Ensure there's a "current" line to write into.
        if (redirect->line_count == 0) redirect->line_count = 1;
        int idx = (redirect->line_count - 1) % TERM_REDIRECT_ROWS;

        if (c == '\n') {
            redirect->line_count++;
            redirect->cur_col = 0;
            return;
        }
        if (c == 8) {
            if (redirect->cur_col > 0) {
                redirect->cur_col--;
                redirect->lines[idx][redirect->cur_col] = '\0';
            }
            return;
        }
        if (redirect->cur_col >= TERM_REDIRECT_COLS) {
            // Wrap to a new line when the current one is full.
            redirect->line_count++;
            redirect->cur_col = 0;
            idx = (redirect->line_count - 1) % TERM_REDIRECT_ROWS;
        }
        redirect->lines[idx][redirect->cur_col] = c;
        redirect->cur_col++;
        redirect->lines[idx][redirect->cur_col] = '\0';
        return;
    }

    if (c == '\n') {
        terminal_col = 0;
        terminal_row++;
        if (terminal_row >= VGA_HEIGHT)
            terminal_scroll();
        return;
    }

    if (c == 8) {
        if (terminal_col > 0) terminal_col--;
        terminal_buffer[terminal_row * VGA_WIDTH + terminal_col] =
            make_vgaentry(' ', terminal_color);
        return;
    }

    terminal_buffer[terminal_row * VGA_WIDTH + terminal_col] =
        make_vgaentry(c, terminal_color);

    if (++terminal_col >= VGA_WIDTH) {
        terminal_col = 0;
        terminal_row++;
    }

    if (terminal_row >= VGA_HEIGHT)
        terminal_scroll();
}

void terminal_print(const char* str) {
    while (*str)
        terminal_putchar(*str++);
}
