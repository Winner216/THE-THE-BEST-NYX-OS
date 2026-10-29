// include/theme.h
#ifndef THEME_H
#define THEME_H

#include <stdint.h>

typedef struct {
    const char* name;
    const char* desc;

    // Taskbar
    uint8_t taskbar_bg;
    uint8_t taskbar_fg;
    uint8_t taskbar_accent;

    // Desktop
    uint8_t desktop_bg;
    uint8_t desktop_dot_fg;
    uint8_t desktop_dot_char;

    // Windows
    uint8_t win_bg;
    uint8_t win_fg;
    uint8_t win_border;
    uint8_t win_title_bg;
    uint8_t win_title_fg;

    // Icons
    uint8_t icon_fg;
    uint8_t icon_sel_bg;
    uint8_t icon_sel_fg;

    // Status bar
    uint8_t status_bg;
    uint8_t status_fg;

    // Highlights
    uint8_t highlight;
    uint8_t accent;
    uint8_t danger;
    uint8_t success;
    uint8_t warning;

    // Terminal
    uint8_t term_bg;
    uint8_t term_fg;
    uint8_t term_prompt;
} Theme;

#define THEME_COUNT 8
extern const Theme themes[THEME_COUNT];
extern int current_theme;

void theme_apply(void);
const Theme* theme_get(void);

#endif
