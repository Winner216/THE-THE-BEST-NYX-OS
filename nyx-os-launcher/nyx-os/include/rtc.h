// include/rtc.h
#ifndef RTC_H
#define RTC_H

#include <stdint.h>

typedef struct {
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint8_t year;
} rtc_time_t;

void        rtc_read(rtc_time_t* t);
void        rtc_get_time_str(char* buf);  // "HH:MM:SS"
void        rtc_get_date_str(char* buf);  // "DD/MM/YY"

#endif
