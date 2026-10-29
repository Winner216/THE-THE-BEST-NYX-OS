// include/editor.h
#ifndef EDITOR_H
#define EDITOR_H

void editor_init(void);
void editor_draw(void);
void editor_handle_key(char c);
void editor_open_file(int file_idx);
int  editor_handle_mouse(int mx, int my);

#endif
