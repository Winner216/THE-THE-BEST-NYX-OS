// Shim errno.h for the freestanding DOOM build: glibc's errno.h pulls
// asm/errno.h which doesn't exist for -m32 without gcc-multilib.  The
// actual errno storage lives in nyx_doom.c (errno_ref).
#ifndef NYX_SHIM_ERRNO_H
#define NYX_SHIM_ERRNO_H
extern int* errno_ref(void);
#define errno (*errno_ref())
#define ENOENT   2
#define EACCES  13
#define ENOMEM  12
#define EINVAL  22
#define EIO      5
#define EISDIR   21
#endif
