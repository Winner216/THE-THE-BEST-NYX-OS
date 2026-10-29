// include/screensaver.h
#ifndef SCREENSAVER_H
#define SCREENSAVER_H

void screensaver_init(void);
void screensaver_tick(void);
void screensaver_reset_idle(void);
int  screensaver_is_active(void);
void screensaver_start(void);

#endif
