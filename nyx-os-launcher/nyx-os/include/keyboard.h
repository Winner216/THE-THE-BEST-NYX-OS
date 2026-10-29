// include/keyboard.h
#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>

void keyboard_install();

// Live terminal input state, owned by keyboard.c (it's the only
// code that mutates these). desktop.c reads them to render the
// current input line in the windowed Terminal app.
#define KEYBOARD_CMD_BUFFER_SIZE 256
extern char     cmd_buffer[KEYBOARD_CMD_BUFFER_SIZE];
extern uint32_t cmd_pos;

#endif
