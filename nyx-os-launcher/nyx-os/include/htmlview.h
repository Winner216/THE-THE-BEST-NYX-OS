// include/htmlview.h
#ifndef HTMLVIEW_H
#define HTMLVIEW_H

#include <stdint.h>

// Render HTML content to a scrollable VESA panel.
// If title is NULL, a default is used.
// Returns 0 on success, -1 if HTML is empty.
// This function blocks until the user presses ESC to exit.
int htmlview_render(const char* html, const char* title);

// Non-blocking: initialise the viewer with HTML content.
// Call htmlview_draw() in the main loop to render, and
// htmlview_handle_key() to process input.
void htmlview_open(const char* html, const char* title);
void htmlview_draw(void);
void htmlview_handle_key(char c);

// Scroll the viewer content by `delta` lines (positive = down toward
// later content).  Returns 1 if the position changed, 0 if clamped or
// the viewer is closed.  Used by the desktop heartbeat to route
// touchpad wheel events into the browser page.
int  htmlview_scroll(int delta);
int  htmlview_is_open(void);   // 1 while the viewer is active
void htmlview_close(void);     // force-close

// Optional: constrain the panel to a window rect (x, y, w, h) instead
// of a full-screen centered panel. `top` reserves pixels at the top of
// the rect for the caller's own chrome (e.g. the browser's address and
// status bars); in this mode htmlview skips its own title bar.
// Call htmlview_clear_window() to restore full-screen mode.
void htmlview_set_window(int x, int y, int w, int h, int top);
void htmlview_clear_window(void);

// If the user pressed ENTER on a link, returns the href URL.
// Returns NULL if no link was followed.
// The shell should check this after the viewer closes and
// automatically fetch the URL.
const char* htmlview_get_pending_url(void);

// ─── Images ──────────────────────────────────────────────────────
// <img> tags in the page are collected into numbered slots.  The
// browser fetches each URL (resolving it against the page), then
// feeds the raw bytes back here to be decoded and drawn in place.
// Slots that are never fed stay as placeholder boxes.

// Number of <img> slots collected while parsing the current page.
int  htmlview_img_count(void);

// 1 while slot i still needs its bytes fetched (not yet decoded/failed).
int  htmlview_img_pending(int i);

// Raw src= attribute of slot i (for the browser to resolve + fetch).
const char* htmlview_img_get_src(int i);

// Decode raw image bytes (PNG/JPEG/BMP/GIF by magic) into slot i and
// mark it ready to draw.  Returns 1 on success, 0 on decode failure
// (slot then stays a placeholder box).
int  htmlview_img_decode(int i, const uint8_t* data, int len);

#endif // HTMLVIEW_H
