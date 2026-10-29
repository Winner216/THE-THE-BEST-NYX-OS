// include/launcher.h — Quick Launch panel: a toggleable overlay
// listing pinned apps for fast access, without leaving whatever
// you're currently doing on the desktop.
#ifndef LAUNCHER_H
#define LAUNCHER_H

#define PINNED_COUNT 8

void launcher_toggle(void);          // open if closed, close if open
void launcher_handle_key(char c);
void launcher_draw(void);            // self-contained: paints over the
                                      // current screen and presents it,
                                      // same pattern as the power menu
int  launcher_is_open(void);
int  launcher_get_sel(void);         // current selection index (-1 = none)
int  launcher_get_launch_id(int idx); // launch_id for pinned index 0-7
void launcher_set_open(int v);       // explicitly set open/closed
void launcher_set_sel(int v);        // explicitly set selection
int  launcher_handle_mouse(int x, int y);  // mouse click handler, returns 1 if consumed
void launcher_update_hover(void);    // update hover state from mouse position
void launcher_launch_pinned(int idx); // launch pinned app by index 0-7 (F1-F8)

#endif
