// include/vbe.h — Direct VBE mode enumeration + Bochs VBE runtime switching
// Provides:
//   1. VBE controller info parsing from the multiboot info structure
//   2. A filtered list of usable video modes (32bpp, LFB-capable)
//   3. Runtime mode switching via the Bochs VBE I/O port interface
//      (requires QEMU's -vga bochs or -device bochs-display)
#ifndef VBE_H
#define VBE_H

#include <stdint.h>
#include <vesa.h>   // for multiboot_info_t

// ─── A single usable VBE mode ────────────────────────────────────
typedef struct {
    uint16_t width;
    uint16_t height;
    uint16_t bpp;        // bits per pixel (24 or 32)
} VbeMode;

// ─── Mode list ───────────────────────────────────────────────────
#define VBE_MAX_MODES 32

extern VbeMode vbe_mode_list[VBE_MAX_MODES];
extern int     vbe_mode_count;       // number of valid modes in the list
extern int     vbe_active_mode;      // index into vbe_mode_list of the current mode
extern int     vbe_available;        // 1 = VBE controller info was found

// Initialize VBE: parse the multiboot VBE controller info and build
// the mode list.  Call from kernel_main() after vesa_init().
void vbe_init(multiboot_info_t* mbi);

// Returns the index of the resolution (width,height,bpp) in the
// vbe_mode_list, or -1 if not found.
int  vbe_find_mode(uint16_t width, uint16_t height, uint16_t bpp);

// Switch video mode at runtime using the Bochs VBE I/O ports.
// Requires QEMU -vga bochs (or -device bochs-display).
// Returns 1 on success, 0 on failure.
// After calling this, the caller must reinitialize vesa_info_t
// and invalidate any framebuffer-dependent caches (wallpaper, etc.).
int  vbe_set_mode(int mode_index);

#endif
