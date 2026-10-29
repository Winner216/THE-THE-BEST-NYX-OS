// cpu/idt64.c — 64-bit Interrupt Descriptor Table
//
// In long mode, IDT entries are 16 bytes (128 bits) with a different
// layout than 32-bit.  The handler address is split across 4 fields
// (offset 0:15, 16:31, 32:63) and there's a reserved high32 field.

#include <idt64.h>
#include <gdt64.h>
#include <stdint.h>

#define IDT64_ENTRIES 256

static struct idt_entry64 idt64[IDT64_ENTRIES];
static struct idt_ptr64   ip64;

// Load the 64-bit IDT
static void lidt64(struct idt_ptr64* idt) {
    asm volatile("lidt (%0)" : : "r"(idt));
}

void idt_set_gate64(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags) {
    idt64[num].base_low  = (uint16_t)(base & 0xFFFF);
    idt64[num].base_mid  = (uint16_t)((base >> 16) & 0xFFFF);
    idt64[num].base_high = (uint32_t)((base >> 32) & 0xFFFFFFFF);
    idt64[num].selector  = sel;
    idt64[num].ist       = 0;
    idt64[num].type_attr = flags;
    idt64[num].reserved  = 0;
}

void idt_set_gate64_ist(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags, uint8_t ist) {
    idt64[num].base_low  = (uint16_t)(base & 0xFFFF);
    idt64[num].base_mid  = (uint16_t)((base >> 16) & 0xFFFF);
    idt64[num].base_high = (uint32_t)((base >> 32) & 0xFFFFFFFF);
    idt64[num].selector  = sel;
    idt64[num].ist       = ist & 0x07;
    idt64[num].type_attr = flags;
    idt64[num].reserved  = 0;
}

// Declare the64-bit ISR/IRQ stubs (defined in interrupts64.asm)
extern void isr64_0(void);
extern void isr64_1(void);
extern void isr64_2(void);
extern void isr64_3(void);
extern void isr64_4(void);
extern void isr64_5(void);
extern void isr64_6(void);
extern void isr64_7(void);
extern void isr64_8(void);
extern void isr64_9(void);
extern void isr64_10(void);
extern void isr64_11(void);
extern void isr64_12(void);
extern void isr64_13(void);
extern void isr64_14(void);
extern void isr64_15(void);
extern void isr64_16(void);
extern void isr64_17(void);
extern void isr64_18(void);
extern void isr64_19(void);

extern void irq64_0(void);
extern void irq64_1(void);
extern void irq64_2(void);
extern void irq64_3(void);
extern void irq64_4(void);
extern void irq64_5(void);
extern void irq64_6(void);
extern void irq64_7(void);
extern void irq64_8(void);
extern void irq64_9(void);
extern void irq64_10(void);
extern void irq64_11(void);
extern void irq64_12(void);
extern void irq64_13(void);
extern void irq64_14(void);
extern void irq64_15(void);

extern void syscall64_stub(void);

void idt_install64(void) {
    ip64.limit = (uint16_t)(sizeof(struct idt_entry64) * IDT64_ENTRIES - 1);
    ip64.base  = (uint64_t)&idt64;

    // Clear all entries
    for (int i = 0; i < IDT64_ENTRIES; i++)
        idt_set_gate64(i, 0, 0, 0);

    // Install CPU exception handlers (ISRs 0-19)
    uint64_t isr_stubs[] = {
        (uint64_t)isr64_0,  (uint64_t)isr64_1,  (uint64_t)isr64_2,
        (uint64_t)isr64_3,  (uint64_t)isr64_4,  (uint64_t)isr64_5,
        (uint64_t)isr64_6,  (uint64_t)isr64_7,  (uint64_t)isr64_8,
        (uint64_t)isr64_9,  (uint64_t)isr64_10, (uint64_t)isr64_11,
        (uint64_t)isr64_12, (uint64_t)isr64_13, (uint64_t)isr64_14,
        (uint64_t)isr64_15, (uint64_t)isr64_16, (uint64_t)isr64_17,
        (uint64_t)isr64_18, (uint64_t)isr64_19,
    };

    for (int i = 0; i < 20; i++) {
        // ISR 8, 10-14 have error codes pushed by CPU
        uint8_t flags = IDT64_GATE_INTERRUPT;
        if (i == 8 || (i >= 10 && i <= 14))
            flags = IDT64_GATE_INTERRUPT;  // same, CPU pushes error
        idt_set_gate64(i, isr_stubs[i], GDT64_KERNEL_CODE, flags);
    }

    // Install IRQ handlers (IRQs 0-15 → IDT 32-47)
    uint64_t irq_stubs[] = {
        (uint64_t)irq64_0,  (uint64_t)irq64_1,  (uint64_t)irq64_2,
        (uint64_t)irq64_3,  (uint64_t)irq64_4,  (uint64_t)irq64_5,
        (uint64_t)irq64_6,  (uint64_t)irq64_7,  (uint64_t)irq64_8,
        (uint64_t)irq64_9,  (uint64_t)irq64_10, (uint64_t)irq64_11,
        (uint64_t)irq64_12, (uint64_t)irq64_13, (uint64_t)irq64_14,
        (uint64_t)irq64_15,
    };

    for (int i = 0; i < 16; i++) {
        idt_set_gate64(32 + i, irq_stubs[i], GDT64_KERNEL_CODE, IDT64_GATE_INTERRUPT);
    }

    // INT 0x80 system call gate (DPL=3 so ring-3 can call it)
    idt_set_gate64(0x80, (uint64_t)syscall64_stub, GDT64_KERNEL_CODE, IDT64_GATE_USER);

    lidt64(&ip64);
}
