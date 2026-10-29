// include/lockscreen.h
#ifndef LOCKSCREEN_H
#define LOCKSCREEN_H

void lockscreen_show(void);
void lockscreen_handle_key(char c);
void lockscreen_draw(void);
int  lockscreen_is_locked(void);
void lockscreen_lock(void);

#endif
