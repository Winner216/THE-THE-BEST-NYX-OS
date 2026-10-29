// Shim strings.h — strcasecmp family lives in string.h too (declared
// in both, defined once in nyx_doom.c).
#ifndef NYX_SHIM_STRINGS_H
#define NYX_SHIM_STRINGS_H
#include <stddef.h>

int strcasecmp(const char* a, const char* b);
int strncasecmp(const char* a, const char* b, size_t n);
int ffs(int v);

#endif
