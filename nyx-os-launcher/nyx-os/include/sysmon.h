// include/sysmon.h
#ifndef SYSMON_H
#define SYSMON_H

void sysmon_init(void);
void sysmon_draw(void);
void sysmon_handle_key(char c);
void sysmon_tick(void);

// Mouse handler (called by desktop_draw click dispatch).
// Returns 1 if the click was handled, 0 otherwise.
int sysmon_handle_mouse(int clk_x, int clk_y);

#endif
