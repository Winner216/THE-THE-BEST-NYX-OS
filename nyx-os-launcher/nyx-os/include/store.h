// include/store.h — Nyx App Store (graphical package manager window)
#ifndef STORE_H
#define STORE_H

// Open the store window (sets MODE_STORE via the caller).  Pulls the
// catalog from the repo the first time it is opened so the shelf is
// populated; the user can refresh with 'U'.
void store_open(void);

// Draw the store window (VESA pixel UI, VGA text fallback).
void store_draw(void);

// Handle a keystroke while the store is focused.
void store_handle_key(char c);

// Read-and-clear: non-zero when the last ENTER ran a package (either
// it was installed and auto-launched, or already installed and was
// launched).  Lets desktop.c switch to the Terminal so the app's
// output is visible on screen.
int store_take_auto_launch(void);

#endif
