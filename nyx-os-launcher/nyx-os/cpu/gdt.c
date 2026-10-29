// cpu/gdt.c  –  Global Descriptor Table + Task State Segment (TSS)
//
// The TSS is the missing half of user mode: when a ring-3 task is
// interrupted (PIT IRQ, INT 0x80 syscall, or a fault), the CPU needs a
// kernel stack to switch to.  It reads ss0/esp0 from the TSS.  The
// scheduler sets tss.esp0 = the incoming task's kernel-stack top on
// every context switch (cpu/task.c), so a user task always interrupts
// onto ITS OWN kernel stack.  The I/O bitmap base is set past the end
// of the TSS, so ring-3 code gets no I/O permissions at all.
#include <gdt.h>

#define GDT_ENTRIES 6

static struct gdt_entry gdt[GDT_ENTRIES];
static struct gdt_ptr   gp;

// ─── TSS (x86 Task State Segment, 104 bytes) ─────────────────────
struct tss_entry {
    uint32_t prev_tss;   // 0
    uint32_t esp0;       // 4  kernel stack pointer (ring-3→ring-0)
    uint32_t ss0;        // 8  kernel stack segment
    uint32_t esp1;       // 12
    uint32_t ss1;        // 16
    uint32_t esp2;       // 20
    uint32_t ss2;        // 24
    uint32_t cr3;        // 28
    uint32_t eip;        // 32
    uint32_t eflags;     // 36
    uint32_t eax;        // 40
    uint32_t ecx;        // 44
    uint32_t edx;        // 48
    uint32_t ebx;        // 52
    uint32_t esp;        // 56
    uint32_t ebp;        // 60
    uint32_t esi;        // 64
    uint32_t edi;        // 68
    uint32_t es;         // 72
    uint32_t cs;         // 76
    uint32_t ss;         // 80
    uint32_t ds;         // 84
    uint32_t fs;         // 88
    uint32_t gs;         // 92
    uint32_t ldtr;       // 96
    uint16_t trap;       // 100
    uint16_t iomap_base; // 102
} __attribute__((packed));

static struct tss_entry tss;

// Defined in gdt_flush.asm
extern void gdt_flush(uint32_t);

// Set one GDT entry
static void gdt_set_gate(int num, uint32_t base, uint32_t limit,
                          uint8_t access, uint8_t gran) {
    gdt[num].base_low    = (base & 0xFFFF);
    gdt[num].base_middle = (base >> 16) & 0xFF;
    gdt[num].base_high   = (base >> 24) & 0xFF;
    gdt[num].limit_low   = (limit & 0xFFFF);
    gdt[num].granularity = (limit >> 16) & 0x0F;
    gdt[num].granularity |= gran & 0xF0;
    gdt[num].access      = access;
}

void gdt_install() {
    gp.limit = (sizeof(struct gdt_entry) * GDT_ENTRIES) - 1;
    gp.base  = (uint32_t)&gdt;

    gdt_set_gate(0, 0, 0,          0,    0);    // Null segment (required)
    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF); // Kernel code segment
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF); // Kernel data segment
    // Ring-3 segments — used by iret-to-ring-3 user tasks.  Access
    // byte: 0xFA = present, DPL 3, code, readable; 0xF2 = present,
    // DPL 3, data, writable.
    gdt_set_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xCF); // User code segment
    gdt_set_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF); // User data segment
    // TSS descriptor: 32-bit available TSS (access 0x89 = present,
    // DPL 0, type 9), byte-granular limit.  Selector 0x28 (entry 5).
    gdt_set_gate(5, (uint32_t)&tss, (uint32_t)sizeof(struct tss_entry) - 1,
                 0x89, 0x00);

    gdt_flush((uint32_t)&gp); // Load GDT into CPU

    // Zero the TSS and install it as the current task.  ss0 = kernel
    // data; esp0 is set by the scheduler before every switch into a
    // task (cpu/task.c).  iomap_base past the end disables the I/O
    // permission bitmap — ring-3 code can perform NO direct I/O.
    // iomap_base is a 16-bit field at offset 102.
    for (int i = 0; i < (int)sizeof(struct tss_entry); i++)
        ((uint8_t*)&tss)[i] = 0;
    tss.ss0        = 0x10;
    tss.iomap_base = sizeof(struct tss_entry);   // 104 → no bitmap
    asm volatile("ltr %%ax" : : "a" ((uint16_t)0x28));   // 5 << 3
}

// The scheduler calls this on every committed context switch so the
// CPU lands on the INCOMING task's kernel stack when a ring-3 task
// takes an interrupt / syscall / fault.
void tss_set_esp0(uint32_t esp0) { tss.esp0 = esp0; }
