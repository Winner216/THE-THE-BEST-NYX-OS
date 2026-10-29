// include/power.h
#ifndef POWER_H
#define POWER_H

// Session actions, set from inside the keyboard-IRQ handler (where
// Lock/Restart/Shutdown are actually selected) and polled by
// kernel_main's top-level loop, since that's the only place safe to
// tear down and restart login/splash — the IRQ handler itself has
// no call stack to "return out of" back to kernel_main.
#define SESSION_RUNNING 0
#define SESSION_LOCK    1
#define SESSION_RESTART 2

void power_shutdown(void);
void power_restart(void);
void power_sleep(void);
void power_hibernate(void);
void power_lock(void);
void power_menu_show(void);
void power_menu_handle_key(char c);
void power_menu_draw(void);
int  power_menu_is_open(void);

// Called by kernel_main's session loop. Returns the pending action
// (SESSION_RUNNING if nothing has happened yet) without clearing it.
int  power_get_session_action(void);
// Resets the action back to SESSION_RUNNING once kernel_main has
// finished reacting to it.
void power_clear_session_action(void);
// True if Lock/Restart has just been selected and kernel_main
// hasn't taken over yet. desktop.c uses this to skip its normal
// post-menu-close redraw, avoiding a one-frame flash of the desktop
// in the brief gap before the session loop notices the flag.
int  power_session_pending(void);

#endif
