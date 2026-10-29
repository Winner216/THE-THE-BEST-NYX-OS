// include/pdfview.h — VESA PDF text viewer for Nyx OS (desktop mode)
#ifndef PDFVIEW_H
#define PDFVIEW_H

#include <stdint.h>

// Initialize the viewer with extracted PDF text.
// Call before switching to MODE_PDFVIEW.
void pdfview_init(const char* text, int text_len, const char* filename,
                  int page_count);

// Returns 1 while the viewer is open.
int  pdfview_is_active(void);

// Draw the viewer panel (called by desktop_draw when mode = MODE_PDFVIEW).
void pdfview_draw(void);

// Handle keyboard input (called by desktop_handle_key).
void pdfview_handle_key(char c);

// Handle mouse click — scrollbar, close button, content area.
int  pdfview_handle_mouse(int mx, int my);

#endif
