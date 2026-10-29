// include/crt.h  –  Minimal C runtime declarations for Win32 API.
//
// Provides declarations for common C library functions that PE
// executables expect.  Implementations are in cpu/crt_stubs.c.

#ifndef CRT_H
#define CRT_H

#include <stddef.h>

// String functions
size_t strlen(const char* s);
int    strcmp(const char* a, const char* b);
int    strncmp(const char* a, const char* b, size_t n);
char*  strcpy(char* dst, const char* src);
char*  strncpy(char* dst, const char* src, size_t n);
char*  strcat(char* dst, const char* src);

// Integer conversion
int  atoi(const char* str);
long atol(const char* str);
long strtol(const char* str, char** endptr, int base);
unsigned long strtoul(const char* str, char** endptr, int base);

// Random numbers
int  rand(void);
void srand(unsigned int seed);

// Memory (implemented in crt_stubs.c, use kmalloc internally)
void* calloc(size_t nmemb, size_t size);
void* realloc(void* ptr, size_t size);

// Process
void abort(void) __attribute__((noreturn));
void exit(int status) __attribute__((noreturn));
int  system(const char* command);
int  atexit(void (*func)(void));

// Environment
char* getenv(const char* name);

// Sorting / searching
void qsort(void* base, size_t nmemb, size_t size,
           int (*compar)(const void*, const void*));
void* bsearch(const void* key, const void* base, size_t nmemb, size_t size,
              int (*compar)(const void*, const void*));

// Assert
void __assert_fail(const char* assertion, const char* file,
                   unsigned int line, const char* function);
#define assert(expr) ((expr) ? (void)0 : __assert_fail(#expr, __FILE__, __LINE__, __func__))

#endif
