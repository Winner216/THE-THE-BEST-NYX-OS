// Shim sys/time.h — gettimeofday is a stub; DOOM's real clock is the
// TSC in DG_GetTicksMs.
#ifndef NYX_SHIM_SYS_TIME_H
#define NYX_SHIM_SYS_TIME_H
#include <sys/types.h>

struct timeval {
    time_t      tv_sec;
    long        tv_usec;
};
struct timezone { int tz_minuteswest; int tz_dsttime; };

int gettimeofday(struct timeval* tv, struct timezone* tz);

#endif
