// include/weather.h
#ifndef WEATHER_H
#define WEATHER_H
#include <stdint.h>
#define WEATHER_COUNT 6

typedef enum {
    WEATHER_SUNNY    = 0,
    WEATHER_CLOUDY   = 1,
    WEATHER_RAINY    = 2,
    WEATHER_STORMY   = 3,
    WEATHER_SNOWY    = 4,
    WEATHER_WINDY    = 5,
} WeatherType;

typedef struct {
    WeatherType type;
    const char* name;
    int         temp;     // Celsius
    int         humidity; // %
    int         wind;     // km/h
    const char* icon;     // single char icon for taskbar
    uint8_t     color;
} Weather;

extern Weather current_weather;
extern int     weather_show_widget;

void weather_init(void);
void weather_draw_widget(void);   // draws in taskbar area (VGA)
void weather_draw_widget_vesa(int x, int y);  // draws in taskbar area (VESA)
void weather_draw_full(void);     // full screen weather app
void weather_handle_key(char c);
void weather_tick(void);          // slowly changes weather
void weather_next(void);          // cycle to next weather

// Mouse handler (called by desktop_draw click dispatch).
// Returns 1 if the click was handled, 0 otherwise.
int weather_handle_mouse(int clk_x, int clk_y);

#endif
