// Shim stdio.h — the freestanding DOOM build's entire libc surface is
// implemented in nyx_doom.c (functions + the NYXFILE type).  These
// headers only declare it.  Do NOT include host glibc headers.
#ifndef NYX_SHIM_STDIO_H
#define NYX_SHIM_STDIO_H
#include <stddef.h>
#include <stdarg.h>

typedef struct {
    // nyx_doom.c backs FILE with the baked-in WAD (memory image);
    // these fields are its state.  Other TUs treat FILE as opaque.
    const unsigned char* base;
    unsigned size;
    unsigned pos;
} NYX_SHIM_FILE;
typedef NYX_SHIM_FILE FILE;

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define EOF      (-1)
#define stdin    ((FILE*)0)
#define stdout   ((FILE*)1)
#define stderr   ((FILE*)2)
#define BUFSIZ   1024
#define FILENAME_MAX 256

int printf(const char* f, ...);
int sprintf(char* out, const char* f, ...);
int snprintf(char* out, size_t n, const char* f, ...);
int fprintf(void* f, const char* f2, ...);
int vfprintf(void* f, const char* f2, va_list ap);
int vsnprintf(char* out, size_t n, const char* f, va_list ap);
int vsprintf(char* out, const char* f, va_list ap);
int vprintf(const char* f, va_list ap);
int puts(const char* s);
int putchar(int c);
int getchar(void);
int fputs(const char* s, void* f);
int fflush(void* f);
void perror(const char* s);

FILE* fopen(const char* path, const char* mode);
size_t fread(void* buf, size_t sz, size_t cnt, FILE* f);
size_t fwrite(const void* buf, size_t sz, size_t cnt, FILE* f);
int    fseek(FILE* f, long off, int whence);
long   ftell(FILE* f);
int    fclose(FILE* f);
int    feof(FILE* f);
int    fgetc(FILE* f);
int    fputc(int c, FILE* f);
char*  fgets(char* s, int n, FILE* f);
int    ferror(FILE* f);
int    remove(const char* p);
int    rename(const char* a, const char* b);
int    fileno_shim(FILE* f);
#endif
