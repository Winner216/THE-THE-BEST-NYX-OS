// include/idt.h
#ifndef IDT_H
#define IDT_H

#include <stdint.h>

// One IDT entry — points to an interrupt handler function
struct idt_entry {
    uint16_t base_low;     // Lower 16 bits of handler address
    uint16_t selector;     // Kernel code segment selector
    uint8_t  zero;         // Always zero
    uint8_t  flags;        // Type and attributes
    uint16_t base_high;    // Upper 16 bits of handler address
} __attribute__((packed));

// Pointer to IDT — loaded into CPU with lidt instruction
struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

void idt_install();
void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags);

#endif
