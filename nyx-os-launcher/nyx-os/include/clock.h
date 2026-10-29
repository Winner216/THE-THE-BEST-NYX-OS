// include/clock.h
#ifndef CLOCK_H
#define CLOCK_H
int clock_is_main_view(void);
void clock_init(void);
void clock_draw(void);
void clock_handle_key(char c);
void clock_tick(void);
int  clock_handle_mouse(int mx, int my);

#endif
int clock_is_main_view(void);
