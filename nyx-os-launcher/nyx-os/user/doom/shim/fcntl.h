// Shim fcntl.h — open() is a stub that always fails; only flag values.
#ifndef NYX_SHIM_FCNTL_H
#define NYX_SHIM_FCNTL_H

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  0100
#define O_TRUNC  01000
#define O_APPEND 02000

int open(const char* p, int flags, ...);

#endif
