// include/applib.h
#ifndef APPLIB_H
#define APPLIB_H

#include <pkg.h>   // PKG_NAME_MAX for the package launch buffer

extern int applib_launch_request;

// When the user launches an *installed package* from the App Library
// (a dynamic card backed by a filesystem "@name" slot), the package
// name (without the '@') is copied here instead of setting
// applib_launch_request.  desktop.c picks it up and calls pkg_run().
extern char applib_pkg_launch[PKG_NAME_MAX];

void applib_init(void);
void applib_draw(void);
void applib_handle_key(char c);
int  applib_handle_mouse(int mx, int my);

// Re-scan the filesystem for installed packages so the launcher list
// is current.  Called automatically on every draw (so alt-tab / mode
// switches stay fresh) and by pkg install/remove handlers right after
// the operation completes.
void applib_refresh(void);

#endif
