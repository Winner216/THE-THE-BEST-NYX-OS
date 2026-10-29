// include/gallery.h — Image Gallery for Nyx OS (desktop mode)
#ifndef GALLERY_H
#define GALLERY_H

// Initialize the gallery: scan filesystem for images, decode thumbnails.
void gallery_init(void);

// Draw the gallery grid panel (called by desktop_draw).
void gallery_draw(void);

// Handle keyboard input (called by desktop_handle_key).
void gallery_handle_key(char c);

// Returns 1 while the automatic slideshow is running (used by
// desktop_heartbeat to decide whether to redraw each tick).
int gallery_is_slideshow_active(void);

// Handle mouse click in the gallery grid. Returns 1 if the click
// was on a thumbnail (selects it and opens the image viewer).
int gallery_handle_mouse(int mx, int my);

#endif
