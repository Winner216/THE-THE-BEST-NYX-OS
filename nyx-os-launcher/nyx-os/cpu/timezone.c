// cpu/timezone.c
#include <timezone.h>
#include <stdint.h>

int current_timezone = 0; // Default UTC

const timezone_t timezones[] = {
    { "UTC-12", "Baker Island",        -12 },
    { "UTC-11", "Samoa",               -11 },
    { "UTC-10", "Hawaii",              -10 },
    { "UTC-9",  "Alaska",               -9 },
    { "UTC-8",  "Los Angeles/Vancouver",-8 },
    { "UTC-7",  "Denver/Phoenix",       -7 },
    { "UTC-6",  "Chicago/Mexico City",  -6 },
    { "UTC-5",  "New York/Toronto",     -5 },
    { "UTC-4",  "Santiago/Halifax",     -4 },
    { "UTC-3",  "Buenos Aires/Brasilia",-3 },
    { "UTC-2",  "Mid-Atlantic",         -2 },
    { "UTC-1",  "Azores/Cape Verde",    -1 },
    { "UTC+0",  "London/Lisbon",         0 },
    { "UTC+1",  "Paris/Berlin/Rome",     1 },
    { "UTC+2",  "Cairo/Athens/Kiev",     2 },
    { "UTC+3",  "Moscow/Istanbul",       3 },
    { "UTC+4",  "Dubai/Baku",            4 },
    { "UTC+5",  "Tashkent/Karachi",      5 },
    { "UTC+6",  "Dhaka/Almaty",          6 },
    { "UTC+7",  "Bangkok/Jakarta",       7 },
    { "UTC+8",  "Beijing/Singapore",     8 },
    { "UTC+9",  "Tokyo/Seoul",           9 },
    { "UTC+10", "Sydney/Melbourne",     10 },
    { "UTC+11", "Solomon Islands",      11 },
    { "UTC+12", "Auckland/Fiji",        12 },
};

const int timezone_count = 25;

void timezone_apply(uint8_t* hour, uint8_t* day, uint8_t* month) {
    int8_t offset = timezones[current_timezone].offset;
    int h = (int)*hour + offset;

    // Handle day rollover
    if (h < 0) {
        h += 24;
        if (*day > 1) (*day)--;
    } else if (h >= 24) {
        h -= 24;
        (*day)++;
    }

    *hour = (uint8_t)h;
    (void)month;
}
