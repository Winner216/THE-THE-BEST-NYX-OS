// Shim unistd.h — stubs implemented in nyx_doom.c.  Note mkdir here is
// the 2-arg POSIX form (sys/stat.h shadow for DOS mkdir is avoided by
// only using POSIX sources).
#ifndef NYX_SHIM_UNISTD_H
#define NYX_SHIM_UNISTD_H
#include <stddef.h>

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4

int    access(const char* p, int m);
int    chdir(const char* p);
char*  getcwd(char* buf, size_t n);
int    close(int fd);
int    read(int fd, void* b, size_t n);
int    write(int fd, const void* b, size_t n);
int    unlink(const char* p);
int    isatty(int fd);
int    getpid(void);
long   sysconf(int n);
unsigned alarm(unsigned s);
int    sleep(int s);
int    usleep(int us);
void*  sbrk(long inc);
char*  strdup(const char* s);

#endif
