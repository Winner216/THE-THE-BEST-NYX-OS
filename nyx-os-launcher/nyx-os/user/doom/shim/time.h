// Shim time.h — time()/localtime()/gmtime()/strftime stubs live in
// nyx_doom.c; DOOM's gameplay clock is the TSC via DG_GetTicksMs.
#ifndef NYX_SHIM_TIME_H
#define NYX_SHIM_TIME_H
#include <stddef.h>

typedef long time_t;

struct tm {
    int tm_sec, tm_min, tm_hour;
    int tm_mday, tm_mon, tm_year;
};

time_t time(time_t* t);
struct tm* localtime(const time_t* t);
struct tm* gmtime(const time_t* t);
size_t strftime(char* s, size_t n, const char* f, const void* tm);

#endif
