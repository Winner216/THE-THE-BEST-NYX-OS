// Shim sys/stat.h — stat/fstat are stubs (always fail) in nyx_doom.c,
// so only the mode bits and 2-arg mkdir are declared.
#ifndef NYX_SHIM_SYS_STAT_H
#define NYX_SHIM_SYS_STAT_H

#define S_IFMT  0170000
#define S_IFDIR 0040000
#define S_IFREG 0100000
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)

int mkdir(const char* p, unsigned mode);
int stat(const char* p, void* s);
int fstat(int fd, void* s);
int unlink(const char* p);
int rmdir(const char* p);

#endif
