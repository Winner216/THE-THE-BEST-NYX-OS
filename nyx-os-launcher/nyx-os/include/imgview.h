// include/imgview.h — VESA image viewer for Nyx OS (desktop mode)
#ifndef IMGVIEW_H
#define IMGVIEW_H

#include <stdint.h>

// Initialize the viewer with a decoded RGBA image.
// Call before switching to MODE_IMGVIEW.
void imgview_init(int width, int height, const uint8_t* pixels, const char* title);

// Returns 1 while the viewer is open.
int  imgview_is_active(void);

// Draw the viewer panel (called by desktop_draw when mode = MODE_IMGVIEW).
void imgview_draw(void);

// Handle keyboard input (called by desktop_handle_key).
void imgview_handle_key(char c);

// Handle mouse click (called by desktop_draw click dispatch).
// Returns 1 if the click was handled, 0 otherwise.
int imgview_handle_mouse(int clk_x, int clk_y);

#endif
