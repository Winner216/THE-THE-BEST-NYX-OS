#ifndef GDT64_H
#define GDT64_H

#include <stdint.h>

// ─── 64-bit GDT ─────────────────────────────────────────────────
// In long mode, the GDT is simpler: segment bases and limits are
// mostly ignored (flat model), but the L bit in the code segment
// descriptor tells the CPU we're in 64-bit mode.

// GDT entry structure (same as 32-bit, interpretation differs)
struct gdt_entry64 {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed));

// GDT pointer (same as 32-bit, but base is 64-bit in LGDT)
struct gdt_ptr64 {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

// 64-bit TSS (104 bytes, same as 32-bit but with32-bit extensions)
struct tss_entry64 {
    uint32_t prev_tss;    // 0
    uint32_t rsp0;        // 4  — kernel stack pointer (ring-3 → ring-0)
    uint32_t rsp1;        // 8
    uint32_t rsp2;        // 12
    uint32_t reserved0;   // 16
    uint32_t ist1;        // 20 — Interrupt Stack Table entries
    uint32_t ist2;        // 24
    uint32_t ist3;        // 28
    uint32_t ist4;        // 32
    uint32_t ist5;        // 36
    uint32_t ist6;        // 40
    uint32_t ist7;        // 44
    uint32_t reserved1;   // 48
    uint16_t reserved2;   // 52
    uint16_t iomap_base;  // 54 — offset to I/O permission bitmap
} __attribute__((packed));

// GDT selectors
#define GDT64_KERNEL_CODE  0x08  // Entry 1
#define GDT64_KERNEL_DATA  0x10  // Entry 2
#define GDT64_USER_CODE    0x18  // Entry 3
#define GDT64_USER_DATA    0x20  // Entry 4
#define GDT64_TSS          0x28  // Entry 5

// Function prototypes
void gdt_install64(void);
void tss_set_rsp064(uint64_t rsp0);

#endif /* GDT64_H */
