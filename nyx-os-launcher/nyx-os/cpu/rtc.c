#include <rtc.h>
#include <io.h>
#include <stdint.h>
#include <timezone.h>
// RTC ports
#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

// Read a CMOS register
static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_ADDR, reg);
    return inb(CMOS_DATA);
}

// Convert BCD to binary (RTC stores values in BCD format)
static uint8_t bcd_to_bin(uint8_t bcd) {
    return (bcd & 0x0F) + ((bcd >> 4) * 10);
}

// Check if RTC is updating (wait until stable)
static int rtc_updating() {
    outb(CMOS_ADDR, 0x0A);
    return inb(CMOS_DATA) & 0x80;
}

void rtc_read(rtc_time_t* t) {
    while (rtc_updating());

    t->second = bcd_to_bin(cmos_read(0x00));
    t->minute = bcd_to_bin(cmos_read(0x02));
    t->hour   = bcd_to_bin(cmos_read(0x04));
    t->day    = bcd_to_bin(cmos_read(0x07));
    t->month  = bcd_to_bin(cmos_read(0x08));
    t->year   = bcd_to_bin(cmos_read(0x09));

    // Apply timezone offset
    timezone_apply(&t->hour, &t->day, &t->month);
}
// Convert number to 2-digit string
static void num_to_str(uint8_t n, char* buf) {
    buf[0] = '0' + (n / 10);
    buf[1] = '0' + (n % 10);
    buf[2] = '\0';
}

void rtc_get_time_str(char* buf) {
    rtc_time_t t;
    rtc_read(&t);
    // Format: HH:MM:SS
    num_to_str(t.hour,   buf);     buf[2] = ':';
    num_to_str(t.minute, buf + 3); buf[5] = ':';
    num_to_str(t.second, buf + 6); buf[8] = '\0';
}

void rtc_get_date_str(char* buf) {
    rtc_time_t t;
    rtc_read(&t);
    // Format: DD/MM/YY
    num_to_str(t.day,   buf);     buf[2] = '/';
    num_to_str(t.month, buf + 3); buf[5] = '/';
    num_to_str(t.year,  buf + 6); buf[8] = '\0';
}
