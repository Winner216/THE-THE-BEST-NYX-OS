// include/organizer.h — Application Organizer: custom folder-based app
// organization. Lets users create, rename, and delete folders, and
// move apps between them. Shows a two-panel VESA pixel UI with VGA
// text fallback.
#ifndef ORGANIZER_H
#define ORGANIZER_H

#include <stdint.h>

#define MAX_ORG_FOLDERS     8
#define ORG_FOLDER_NAME_LEN 16
#define MAX_APPS_PER_FOLDER 16

typedef struct {
    char     name[ORG_FOLDER_NAME_LEN];
    int      app_ids[MAX_APPS_PER_FOLDER];  // launch_ids (0-14)
    int      app_count;
    uint32_t color;    // VESA RGB accent
    uint8_t  vga_col;  // VGA palette index
} OrgFolder;

extern OrgFolder org_folders[MAX_ORG_FOLDERS];
extern int       org_folder_count;

void organizer_init(void);
void organizer_draw(void);
void organizer_handle_key(char c);
int  organizer_handle_mouse(int mx, int my);

// Folder management — returns 1 on success, 0 on failure (full / name empty).
int  organizer_add_folder(const char* name);
int  organizer_remove_folder(int idx);
int  organizer_rename_folder(int idx, const char* new_name);
int  organizer_add_app_to_folder(int folder_idx, int app_launch_id);
int  organizer_remove_app_from_folder(int folder_idx, int app_idx);

#endif
