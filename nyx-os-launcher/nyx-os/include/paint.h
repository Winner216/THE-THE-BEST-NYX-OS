// include/paint.h
#ifndef PAINT_H
#define PAINT_H

void paint_init(void);
void paint_draw(void);
void paint_handle_key(char c);
int  paint_handle_mouse(int mx, int my);

// Called from the drag-and-drop system when a file is dropped onto
// the Paint canvas. Reads the file's content and stamps it onto the
// canvas cells as text (each character maps to a cell).
void paint_drop_file(int file_idx);

#endif
