// cpu/theme.c - Nyx OS Theme System
#include <theme.h>

int current_theme = 0;

// ─── Theme definitions ────────────────────────────────────────────
// Colors: 0=Black 1=Blue 2=Green 3=Cyan 4=Red 5=Magenta 6=Brown
//         7=LGrey 8=DGrey 9=LBlue 10=LGreen 11=LCyan 12=LRed
//         13=LMag 14=Yellow 15=White

const Theme themes[THEME_COUNT] = {

    // ── 0: Nyx Purple (Default) ─────────────────────────────────────
    {
        "Nyx Purple", "Signature purple cosmic theme",
        0,  13, 11,    // taskbar: black bg, lmagenta fg, lcyan accent
        0,   5, '.',   // desktop: black bg, magenta dots
        0,  15,  5, 5,15,  // window: black bg, magenta border/title
        13,  5, 15,    // icon: lmagenta fg, magenta sel bg, white sel fg
        5,  15,        // status: magenta bg, white fg
        13, 11, 12, 10, 14, // highlights
        0,  13,  5,    // terminal: black bg, lmagenta fg, magenta prompt
    },

    // ── 1: Dark ──────────────────────────────────────────────────
    {
        "Dark",      "Classic dark theme",
        1,  15, 14,    // taskbar: blue bg, white fg, yellow accent
        0,   8, '.',   // desktop: black bg, grey dots
        0,  15,  8, 1,15, // window
        11,  3,  0,    // icon
        8,  15,        // status
        11, 14, 12, 10, 14, // highlights
        0,  7,  11,    // terminal
    },

    // ── 1: Light ──────────────────────────────────────────────────
    {
        "Light",     "Clean light theme",
        7,   0, 1,     // taskbar: lgrey bg, black fg, blue accent
        7,   8, '.',   // desktop: lgrey bg
        7,   0,  1, 7, 1,  // window
        1,   7,  15,   // icon
        7,   0,        // status
        9,   1, 12, 2, 6,  // highlights
        7,   0,  1,    // terminal
    },

    // ── 2: Ocean Blue ─────────────────────────────────────────────
    {
        "Ocean",     "Deep ocean blue",
        1,  11, 14,    // taskbar: blue bg, cyan fg
        1,   9, '~',   // desktop: blue bg, wave dots
        1,  15,  3, 9,15,  // window
        11,  9,  0,    // icon
        9,  15,        // status
        11,  9, 12, 10, 14, // highlights
        1,  11,  9,    // terminal
    },

    // ── 3: Forest Green ───────────────────────────────────────────
    {
        "Forest",    "Deep forest green",
        2,  15, 14,    // taskbar: green bg
        2,  10, ',',   // desktop: green bg, leaf dots
        2,  15,  2,10,15,  // window
        10,  2,  0,    // icon
        10, 15,        // status
        10,  2, 12, 14, 6,  // highlights
        2,  10,  2,    // terminal
    },

    // ── 4: Sunset Red ─────────────────────────────────────────────
    {
        "Sunset",    "Warm sunset colors",
        4,  15, 14,    // taskbar: red bg
        4,  12, '*',   // desktop: dark red, star dots
        0,  15,  4,12,15,  // window
        12,  4,  0,    // icon
        4,  15,        // status
        12,  4, 14, 10, 12, // highlights
        0,  15, 12,    // terminal
    },

    // ── 5: Purple Night ───────────────────────────────────────────
    {
        "Purple",    "Mysterious purple",
        5,  15, 14,    // taskbar: magenta bg
        0,   5, '.',   // desktop
        0,  15,  5,13,15,  // window
        13,  5,  0,    // icon
        5,  15,        // status
        13,  5, 12, 10, 14, // highlights
        0,  13,  5,    // terminal
    },

    // ── 6: Matrix ─────────────────────────────────────────────────
    {
        "Matrix",    "Hacker green on black",
        0,  10,  2,    // taskbar: black bg, green fg
        0,   2, '0',   // desktop: black, green zeros
        0,  10,  2, 0, 2,  // window
        10,  2,  0,    // icon
        0,  10,        // status
        2,   0, 12,  2, 14, // highlights
        0,  10,  2,    // terminal
    },

    // ── 7: Ice ────────────────────────────────────────────────────
    {
        "Ice",       "Cool icy blue-white",
        3,  15,  9,    // taskbar: cyan bg
        3,  11, '-',   // desktop: cyan, dash dots
        3,  15,  9, 3,15,  // window
        15,  3,  0,    // icon
        3,  15,        // status
        9,   3, 12, 15, 14, // highlights
        3,  15,  9,    // terminal
    },
};

const Theme* theme_get() {
    return &themes[current_theme];
}

void theme_apply() {


}
