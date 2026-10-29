// include/wallpaper.h
#ifndef WALLPAPER_H
#define WALLPAPER_H

#include <stdint.h>

#define WALLPAPER_COUNT 8

// Maximum resolution for custom file-based wallpaper (cached decoded pixels).
// Supports up to 1920x1080 at 32bpp = ~8 MB.
#define WALLPAPER_MAX_W 1920
#define WALLPAPER_MAX_H 1080

typedef struct {
    const char* name;
    const char* desc;
    uint32_t    preview_color;   /* dominant flat color for swatch */
    void (*draw)(void);
} Wallpaper;

extern const Wallpaper wallpapers[WALLPAPER_COUNT];
extern int current_wallpaper;
extern int custom_wallpaper_file;   // -1 = use built-in, >=0 = filesystem index

void wallpaper_draw(void);
void wallpaper_draw_fast(void);

// Screen brightness (0-100, 100 = full). Applied to the wallpaper layer
// every frame via the dim-overlay pass. Used by the Quick Settings
// brightness slider.
void wallpaper_set_brightness(int level);

// Set custom wallpaper from a filesystem file index.
// Pass -1 to revert to built-in wallpaper.
void wallpaper_set_custom(int file_idx);

// Returns the name to display for the current wallpaper (built-in name
// or filename). Used by the settings UI.
const char* wallpaper_get_name(void);

// ─── Dim overlay ───────────────────────────────────────────────────
// Dim level for the wallpaper overlay (0-100, 0 = no dim, 100 = 50% black).
// Applied during wallpaper_draw() as a semi-transparent black multiply.
extern int wallpaper_dim_level;

// Set the dim level (clamped to 0..100).
void wallpaper_set_dim(int level);

#endif
