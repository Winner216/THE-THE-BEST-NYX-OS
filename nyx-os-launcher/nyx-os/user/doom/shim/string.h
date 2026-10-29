// Shim string.h — implementations in nyx_doom.c.
#ifndef NYX_SHIM_STRING_H
#define NYX_SHIM_STRING_H
#include <stddef.h>

void*  memset(void* d, int c, size_t n);
void*  memcpy(void* d, const void* s, size_t n);
void*  memmove(void* d, const void* s, size_t n);
int    memcmp(const void* a, const void* b, size_t n);
size_t strlen(const char* s);
char*  strcpy(char* d, const char* s);
char*  strncpy(char* d, const char* s, size_t n);
char*  strcat(char* d, const char* s);
char*  strncat(char* d, const char* s, size_t n);
int    strcmp(const char* a, const char* b);
int    strncmp(const char* a, const char* b, size_t n);
int    strcasecmp(const char* a, const char* b);
int    strncasecmp(const char* a, const char* b, size_t n);
char*  strchr(const char* s, int c);
char*  strrchr(const char* s, int c);
char*  strstr(const char* h, const char* n);
char*  strdup(const char* s);
char*  strerror(int e);
#endif
