// include/calendar.h
#ifndef CALENDAR_H
#define CALENDAR_H

void calendar_init(void);
void calendar_draw(void);
void calendar_handle_key(char c);

#define MAX_EVENTS    32
#define MAX_EVT_LEN   30

typedef struct {
    int  year;
    int  month;  // 1-12
    int  day;    // 1-31
    char title[MAX_EVT_LEN];
    uint8_t color;
} CalEvent;

extern CalEvent cal_events[MAX_EVENTS];
extern int      cal_event_count;

#endif
