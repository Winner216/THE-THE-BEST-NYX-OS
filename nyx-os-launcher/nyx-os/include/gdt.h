// include/gdt.h
#ifndef GDT_H
#define GDT_H

#include <stdint.h>

// One GDT entry — describes a memory segment
struct gdt_entry {
    uint16_t limit_low;    // Lower 16 bits of limit
    uint16_t base_low;     // Lower 16 bits of base
    uint8_t  base_middle;  // Next 8 bits of base
    uint8_t  access;       // Access flags (ring level etc)
    uint8_t  granularity;  // Granularity + upper limit bits
    uint8_t  base_high;    // Last 8 bits of base
} __attribute__((packed));

// Pointer to the GDT — loaded into CPU with lgdt instruction
struct gdt_ptr {
    uint16_t limit;        // Size of GDT
    uint32_t base;         // Address of GDT
} __attribute__((packed));

// Function declarations
void gdt_install();

#endif
