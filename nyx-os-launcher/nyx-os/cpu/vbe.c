// cpu/vbe.c — Direct VBE mode enumeration + Bochs VBE runtime switching
//
// Mode enumeration: reads the VBE controller info structure that GRUB
// already filled in as part of the multiboot info.  The controller info
// contains a pointer to the list of supported VESA mode numbers.  Since
// we can't call INT 0x10 from protected mode to get each mode's info
// block, we match mode numbers against a table of well-known VESA modes.
//
// Mode switching: uses the Bochs VBE I/O port interface (0x01CE/0x01CF)
// to set a new resolution at runtime without needing BIOS calls.  This
// requires QEMU to be configured with -vga bochs or -device bochs-display.
// On real hardware this would need the VBE PM entry point or vm86 instead.

#include <vbe.h>
#include <vesa.h>    // for multiboot_info_t
#include <io.h>
#include <stdint.h>

// ─── VBE controller info structure (256 bytes at physical address) ──
typedef struct __attribute__((packed)) {
    char     signature[4];       // "VESA"
    uint16_t version;            // BCD version (e.g. 0x0300 for 3.0)
    uint32_t oem_string_ptr;     // far pointer (segment:offset)
    uint32_t capabilities;
    uint32_t video_mode_ptr;     // far pointer to 0xFFFF-terminated mode list
    uint16_t total_memory;       // in 64 KB blocks
    uint16_t oem_sw_rev;
    uint32_t oem_vendor_ptr;
    uint32_t oem_product_ptr;
    uint32_t oem_revision_ptr;
    uint8_t  reserved[222];
} __attribute__((packed)) VbeControllerInfo;

// ─── Global state ─────────────────────────────────────────────────
VbeMode vbe_mode_list[VBE_MAX_MODES];
int     vbe_mode_count  = 0;
int     vbe_active_mode = -1;
int     vbe_available   = 0;

// ─── Convert a VBE far pointer (segment:offset) to a physical addr ──
static inline uint32_t vbe_far_to_phys(uint32_t far_ptr) {
    uint16_t seg = (uint16_t)(far_ptr >> 16);
    uint16_t off = (uint16_t)(far_ptr & 0xFFFF);
    return ((uint32_t)seg << 4) + off;
}

// ─── Standard VESA mode resolution lookup ─────────────────────────
// Maps VESA mode numbers to their known resolutions and bpp.
// Not all VBE implementations use standard mode numbers (some use
// OEM-defined modes), but QEMU's Bochs VBE follows the standard VBE
// mode assignments for common resolutions.
typedef struct { uint16_t mode; uint16_t w; uint16_t h; uint8_t bpp; } VesaModeEntry;

static const VesaModeEntry known_modes[] = {
    // 8 bpp modes (skipped — we only want 24/32 bpp)
    // 16 bpp modes
    { 0x111, 640,  480,  16 },
    { 0x114, 800,  600,  16 },
    { 0x117, 1024, 768,  16 },
    { 0x11A, 1280, 1024, 16 },
    // 32 bpp modes
    { 0x112, 640,  480,  32 },
    { 0x115, 800,  600,  32 },
    { 0x118, 1024, 768,  32 },
    { 0x11B, 1280, 1024, 32 },
    // Additional 32 bpp (non-standard but common)
    { 0x141, 640,  480,  32 },
    { 0x142, 800,  600,  32 },
    { 0x143, 1024, 768,  32 },
    { 0x144, 1280, 1024, 32 },
    { 0x145, 1600, 1200, 32 },
    { 0x146, 1920, 1080, 32 },
    { 0x147, 1920, 1200, 32 },
    { 0x148, 1366, 768,  32 },
    { 0x149, 1440, 900,  32 },
    { 0x14A, 1680, 1050, 32 },
    { 0x14B, 1280, 720,  32 },
};
static const int known_mode_count = sizeof(known_modes) / sizeof(known_modes[0]);

// ─── Hardcoded fallback resolutions (for manual entry) ────────────
// These are known to work with QEMU's bochs-display device.
// Used when VBE controller info is not available or doesn't contain
// useful mode numbers. Filtered by available video memory at runtime.
static const VbeMode fallback_modes[] = {
    { 640,  480,  32 },
    { 800,  600,  32 },
    { 1024, 768,  32 },
    { 1280, 720,  32 },
    { 1280, 1024, 32 },
    { 1366, 768,  32 },
    { 1440, 900,  32 },
    { 1600, 1200, 32 },
    { 1680, 1050, 32 },
    { 1920, 1080, 32 },
};
static const int fallback_count = sizeof(fallback_modes) / sizeof(fallback_modes[0]);

// ─── Filter modes by available video memory ───────────────────────
// The VBE controller info's total_memory field gives the total video
// memory in 64KB blocks.  A mode at width×height×bpp requires
// (width * height * bpp/8) bytes.  We only include modes that fit.
static int vbe_memory_available = 0;  // bytes

static void vbe_filter_by_memory(void) {
    if (vbe_memory_available <= 0) return;  // unknown — accept all
    int kept = 0;
    for (int i = 0; i < vbe_mode_count; i++) {
        uint32_t needed = (uint32_t)vbe_mode_list[i].width *
                          (uint32_t)vbe_mode_list[i].height *
                          (uint32_t)(vbe_mode_list[i].bpp / 8);
        if (needed <= (uint32_t)vbe_memory_available) {
            vbe_mode_list[kept++] = vbe_mode_list[i];
        }
    }
    vbe_mode_count = kept;
    // Re-validate active mode — it may now be stale if the current
    // mode was filtered out (exceeded available VRAM).
    if (vbe_active_mode >= vbe_mode_count)
        vbe_active_mode = -1;
}

// ─── Add a mode to the list if not already present ────────────────
// Only 32bpp modes are included — the VESA framebuffer code assumes
// 32bpp BGRA layout throughout.
static void vbe_add_mode(uint16_t w, uint16_t h, uint16_t bpp) {
    if (bpp != 32) return;  // only 32bpp is supported
    if (vbe_mode_count >= VBE_MAX_MODES) return;
    // Deduplicate
    for (int i = 0; i < vbe_mode_count; i++) {
        if (vbe_mode_list[i].width == w &&
            vbe_mode_list[i].height == h &&
            vbe_mode_list[i].bpp == bpp) return;
    }
    vbe_mode_list[vbe_mode_count].width  = w;
    vbe_mode_list[vbe_mode_count].height = h;
    vbe_mode_list[vbe_mode_count].bpp    = bpp;
    vbe_mode_count++;
}

// ═══════════════════════════════════════════════════════════════════
//  Public API
// ═══════════════════════════════════════════════════════════════════

void vbe_init(multiboot_info_t* mbi) {
    vbe_mode_count  = 0;
    vbe_available   = 0;
    vbe_active_mode = -1;

    if (!mbi) goto use_fallback;

    // Check if multiboot provided VBE info
    if (!(mbi->flags & (1 << 11))) goto use_fallback;  // bit 11 = VBE info present

    uint32_t ctrl_addr = mbi->vbe_control_info;
    if (!ctrl_addr) goto use_fallback;

    // Read VBE controller info from physical memory
    // (our kernel is identity-mapped, so physical == virtual for low mem)
    volatile VbeControllerInfo* ctrl = (volatile VbeControllerInfo*)(uint32_t)ctrl_addr;

    // Validate signature
    if (ctrl->signature[0] != 'V' || ctrl->signature[1] != 'E' ||
        ctrl->signature[2] != 'S' || ctrl->signature[3] != 'A')
        goto use_fallback;

    // Get total video memory in bytes
    vbe_memory_available = (int)ctrl->total_memory * 64 * 1024;

    // Parse the video mode pointer (far pointer to 0xFFFF-terminated mode list)
    uint32_t mode_list_phys = vbe_far_to_phys(ctrl->video_mode_ptr);
    if (!mode_list_phys) goto use_fallback;

    // Walk the mode list and match against known modes
    volatile uint16_t* mode_ptr = (volatile uint16_t*)(uint32_t)mode_list_phys;
    for (int mi = 0; mi < 256; mi++) {  // safety limit: 256 modes max
        uint16_t mode_num = mode_ptr[mi];
        if (mode_num == 0xFFFF) break;  // terminator

        // Check if this mode number is in our known_modes table
        for (int ki = 0; ki < known_mode_count; ki++) {
            if (known_modes[ki].mode == mode_num) {
                vbe_add_mode(known_modes[ki].w, known_modes[ki].h, known_modes[ki].bpp);
                break;
            }
        }
    }

    // If the VBE controller listed some modes, we're done
    if (vbe_mode_count > 0) {
        vbe_available = 1;
        // Try to find the current resolution
        int cur_idx = vbe_find_mode((uint16_t)vesa.width, (uint16_t)vesa.height, vesa.bpp);
        if (cur_idx >= 0) vbe_active_mode = cur_idx;
        // Filter by available memory
        vbe_filter_by_memory();
        return;
    }

use_fallback:
    // VBE controller info wasn't available or didn't have useful modes.
    // Use the hardcoded fallback list (filtered by any known memory limit).
    vbe_available = 0;
    for (int i = 0; i < fallback_count; i++) {
        vbe_add_mode(fallback_modes[i].width, fallback_modes[i].height, fallback_modes[i].bpp);
    }
    // Try to find current resolution
    int cur_idx = vbe_find_mode((uint16_t)vesa.width, (uint16_t)vesa.height, vesa.bpp);
    if (cur_idx >= 0) vbe_active_mode = cur_idx;
    vbe_filter_by_memory();
}

int vbe_find_mode(uint16_t width, uint16_t height, uint16_t bpp) {
    for (int i = 0; i < vbe_mode_count; i++) {
        if (vbe_mode_list[i].width == width &&
            vbe_mode_list[i].height == height &&
            vbe_mode_list[i].bpp == bpp)
            return i;
    }
    return -1;
}

// ─── Bochs VBE I/O port interface ─────────────────────────────────
// The Bochs-display VBE extension provides two I/O ports:
//   0x01CE — Index register  (write register index here)
//   0x01CF — Data register   (read/write register value here)
//
// Register indices:
//   VBE_DISPI_INDEX_ID         = 0   — display ID (read-only)
//   VBE_DISPI_INDEX_XRES       = 1   — horizontal resolution
//   VBE_DISPI_INDEX_YRES       = 2   — vertical resolution
//   VBE_DISPI_INDEX_BPP        = 3   — bits per pixel
//   VBE_DISPI_INDEX_ENABLE     = 4   — display enable
//   VBE_DISPI_INDEX_BANK       = 5   — bank select (windowed modes)
//   VBE_DISPI_INDEX_VIRT_WIDTH = 6   — virtual width
//   VBE_DISPI_INDEX_VIRT_HEIGHT= 7   — virtual height
//   VBE_DISPI_INDEX_X_OFFSET   = 8   — horizontal panning offset
//   VBE_DISPI_INDEX_Y_OFFSET   = 9   — vertical panning offset

#define VBE_DISPI_IOPORT_INDEX 0x01CE
#define VBE_DISPI_IOPORT_DATA  0x01CF

#define VBE_DISPI_INDEX_ID         0
#define VBE_DISPI_INDEX_XRES       1
#define VBE_DISPI_INDEX_YRES       2
#define VBE_DISPI_INDEX_BPP        3
#define VBE_DISPI_INDEX_ENABLE     4
#define VBE_DISPI_INDEX_BANK       5
#define VBE_DISPI_INDEX_VIRT_WIDTH 6
#define VBE_DISPI_INDEX_X_OFFSET   8
#define VBE_DISPI_INDEX_Y_OFFSET   9

// VBE_DISPI_ENABLE values: bit 0 = enable, bit 6 = LFB enabled
#define VBE_DISPI_DISABLED     0x00
#define VBE_DISPI_ENABLED      0x01
#define VBE_DISPI_LFB_ENABLED  0x40  // bit 6: use linear framebuffer

// Expected return value from VBE_DISPI_INDEX_ID for Bochs VBE
#define VBE_DISPI_ID_VALUE     0xB0C0  // "B0C0" = Bochs

// Probes the Bochs VBE I/O ports to see if the interface is present.
// Returns 1 if the Bochs VBE extension is available.
static int bochs_vbe_probe(void) {
    // Write the ID register index (0) to the index port (0x01CE),
    // then read the ID value back from the data port (0x01CF).
    outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_ID);
    uint16_t id = inw(VBE_DISPI_IOPORT_DATA);
    return (id == VBE_DISPI_ID_VALUE) ? 1 : 0;
}

int vbe_set_mode(int mode_index) {
    if (mode_index < 0 || mode_index >= vbe_mode_count) return 0;

    uint16_t w   = vbe_mode_list[mode_index].width;
    uint16_t h   = vbe_mode_list[mode_index].height;
    uint16_t bpp = vbe_mode_list[mode_index].bpp;

    // Probe first — if no Bochs VBE, fail
    if (!bochs_vbe_probe()) return 0;

    // Step 1: disable display
    outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_ENABLE);
    outw(VBE_DISPI_IOPORT_DATA,  VBE_DISPI_DISABLED);

    // Step 2: set resolution
    outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_XRES);
    outw(VBE_DISPI_IOPORT_DATA,  w);

    outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_YRES);
    outw(VBE_DISPI_IOPORT_DATA,  h);

    outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_BPP);
    outw(VBE_DISPI_IOPORT_DATA,  bpp);

    // Step 3: enable with LFB
    outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_ENABLE);
    outw(VBE_DISPI_IOPORT_DATA,  VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);

    vbe_active_mode = mode_index;
    return 1;
}
