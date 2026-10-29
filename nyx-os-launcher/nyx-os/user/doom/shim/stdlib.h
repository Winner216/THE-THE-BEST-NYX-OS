// Shim stdlib.h — see shim/stdio.h notes.  Implementations in nyx_doom.c.
#ifndef NYX_SHIM_STDLIB_H
#define NYX_SHIM_STDLIB_H
#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX     0x7FFFFFFF

void*  malloc(size_t n);
void*  calloc(size_t a, size_t b);
void*  realloc(void* p, size_t n);
void   free(void* p);
char*  strdup(const char* s);
int    atoi(const char* s);
long   atol(const char* s);
long   strtol(const char* s, char** end, int base);
unsigned long strtoul(const char* s, char** end, int base);
void   srand(unsigned seed);
int    rand(void);
int    abs(int v);
long   labs(long v);
void   qsort(void* base, size_t n, size_t w,
             int (*cmp)(const void*, const void*));
void   exit(int code) __attribute__((noreturn));
void   abort(void) __attribute__((noreturn));
void   _exit(int code) __attribute__((noreturn));
void   _Exit(int code) __attribute__((noreturn));
int    atexit(void (*f)(void));
char*  getenv(const char* name);
int    setenv(const char* a, const char* b, int o);
void   unsetenv(const char* a);
int    putenv(const char* s);
int    system(const char* c);
#endif
