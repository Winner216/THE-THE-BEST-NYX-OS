// include/timezone.h
#ifndef TIMEZONE_H
#define TIMEZONE_H

#include <stdint.h>

typedef struct {
    const char* name;    // "UTC+5"
    const char* city;    // "Tashkent/Karachi"
    int8_t      offset;  // hours offset from UTC
} timezone_t;

extern const timezone_t timezones[];
extern const int        timezone_count;
extern int              current_timezone;

void timezone_apply(uint8_t* hour, uint8_t* day, uint8_t* month);

#endif
