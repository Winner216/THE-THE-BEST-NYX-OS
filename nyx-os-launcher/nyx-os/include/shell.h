// include/shell.h
#ifndef SHELL_H
#define SHELL_H

void shell_prompt(void);
void shell_process(char* cmd);
void shell_history_up(char* buf, int* pos);
void shell_history_down(char* buf, int* pos);

#endif
