#ifndef IDT64_H
#define IDT64_H

#include <stdint.h>

// ─── 64-bit IDT ─────────────────────────────────────────────────
// In long mode, IDT entries are 16 bytes (128 bits) instead of 8.
// The gate descriptor format is:
//   bits 0:15   — offset 0:15 (handler address)
//   bits 16:31  — segment selector (0x08 for kernel code)
//   bits 32:34  — IST (Interrupt Stack Table) index
//   bits 35:39  — reserved (must be 0)
//   bits 40:47  — type + DPL + present
//   bits 48:63  — offset 16:31
//   bits 64:95  — offset 32:63
//   bits 96:127 — reserved (must be 0)

struct idt_entry64 {
    uint16_t base_low;      // offset bits 0:15
    uint16_t selector;      // kernel segment selector
    uint8_t  ist;           // IST index (0 = none)
    uint8_t  type_attr;     // type + DPL + present
    uint16_t base_mid;      // offset bits 16:31
    uint32_t base_high;     // offset bits 32:63
    uint32_t reserved;      // must be 0
} __attribute__((packed));

struct idt_ptr64 {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

// IDT gate type constants
#define IDT64_GATE_INTERRUPT  0x8E  // Present, DPL=0, 64-bit interrupt gate
#define IDT64_GATE_TRAP       0x8F  // Present, DPL=0, 64-bit trap gate
#define IDT64_GATE_USER       0xEE  // Present, DPL=3, 64-bit interrupt gate

// Function prototypes
void idt_install64(void);
void idt_set_gate64(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags);
void idt_set_gate64_ist(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags, uint8_t ist);

#endif /* IDT64_H */
