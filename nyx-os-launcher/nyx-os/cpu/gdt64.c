// cpu/gdt64.c — 64-bit Global Descriptor Table + Task State Segment
//
// In long mode, the GDT is simpler: segments are mostly flat (base=0,
// limit=ignored), but the L bit in code segments tells the CPU we're
// in 64-bit mode.  The TSS is used only for RSP0 (kernel stack on
// ring-3 → ring-0 transitions) and IST entries (for NMI/double-fault).

#include <gdt64.h>
#include <stdint.h>

#define GDT64_ENTRIES 7  // 6 entries + TSS extension

static struct gdt_entry64 gdt64[GDT64_ENTRIES];
static struct gdt_ptr64   gp64;
static struct tss_entry64 tss64;

// Helper: set a GDT entry
static void gdt64_set_gate(int num, uint32_t base, uint32_t limit,
                           uint8_t access, uint8_t flags) {
    gdt64[num].base_low    = (uint16_t)(base & 0xFFFF);
    gdt64[num].base_middle = (uint8_t)((base >> 16) & 0xFF);
    gdt64[num].base_high   = (uint8_t)((base >> 24) & 0xFF);
    gdt64[num].limit_low   = (uint16_t)(limit & 0xFFFF);
    gdt64[num].granularity  = (uint8_t)((limit >> 16) & 0x0F);
    gdt64[num].granularity |= (flags & 0xF0);
    gdt64[num].access       = access;
}

// Load the 64-bit GDT (LGDT instruction)
static void lgdt64(struct gdt_ptr64* gdt) {
    asm volatile("lgdt (%0)" : : "r"(gdt));
}

// Reload segment registers for 64-bit mode
static void reload_segments64(void) {
    // Reload CS via far jump (64-bit far jump)
    asm volatile(
        "push $0x08\n"
        "lea .reload_cs64(%%rip), %%rax\n"
        "push %%rax\n"
        "lretq\n"
        ".reload_cs64:\n"
        : : : "rax", "memory"
    );
    // Reload data segments
    asm volatile(
        "mov $0x10, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        "mov %%ax, %%ss\n"
        : : : "ax", "memory"
    );
}

void gdt_install64(void) {
    // Build the GDT pointer
    gp64.limit = (uint16_t)(sizeof(struct gdt_entry64) * GDT64_ENTRIES - 1);
    gp64.base  = (uint64_t)&gdt64;

    // Entry 0: Null descriptor
    gdt64_set_gate(0, 0, 0, 0, 0);

    // Entry 1: 64-bit kernel code segment (selector 0x08)
    // Access: Present(1) DPL(00) S(1) Type(1010) = 0x9A
    // Flags:  L(1) D(0) G(1) = 0xA0 (L=1 for 64-bit)
    gdt64_set_gate(1, 0, 0, 0x9A, 0xA0);

    // Entry 2: 64-bit kernel data segment (selector 0x10)
    // Access: Present(1) DPL(00) S(1) Type(0010) = 0x92
    // Flags:  D/B(1) G(1) = 0xC0
    gdt64_set_gate(2, 0, 0, 0x92, 0xC0);

    // Entry 3: 64-bit user code segment (selector 0x18)
    // Access: Present(1) DPL(11) S(1) Type(1010) = 0xFA
    // Flags:  L(1) D(0) G(1) = 0xA0
    gdt64_set_gate(3, 0, 0, 0xFA, 0xA0);

    // Entry 4: 64-bit user data segment (selector 0x20)
    // Access: Present(1) DPL(11) S(1) Type(0010) = 0xF2
    // Flags:  D/B(1) G(1) = 0xC0
    gdt64_set_gate(4, 0, 0, 0xF2, 0xC0);

    // Entry 5: TSS descriptor (selector 0x28)
    // In 64-bit mode, the TSS descriptor is 16 bytes (two GDT entries).
    // Access: Present(1) DPL(00) S(0) Type(1001) = 0x89 (available TSS)
    // The TSS base is split across the descriptor in a complex way:
    //   bytes 0-1:  limit 0:15
    //   bytes 2-3:  base 0:15
    //   byte  4:    base 16:23
    //   byte  5:    access
    //   byte  6:    limit 16:19 | flags
    //   byte  7:    base 24:31
    //   bytes 8-11: base 32:63 (new in 64-bit)
    //   bytes 12-13: reserved (must be 0)
    //   bytes 14-15: reserved (must be 0)
    {
        uint64_t tss_base = (uint64_t)&tss64;
        uint32_t tss_limit = sizeof(struct tss_entry64) - 1;

        // First 8 bytes (standard entry)
        gdt64[5].limit_low   = (uint16_t)(tss_limit & 0xFFFF);
        gdt64[5].base_low    = (uint16_t)(tss_base & 0xFFFF);
        gdt64[5].base_middle = (uint8_t)((tss_base >> 16) & 0xFF);
        gdt64[5].access      = 0x89;  // Present, DPL=0, S=0, Type=9 (avail TSS)
        gdt64[5].granularity  = (uint8_t)((tss_limit >> 16) & 0x0F);
        gdt64[5].base_high   = (uint8_t)((tss_base >> 24) & 0xFF);

        // Second 8 bytes (64-bit extension)
        uint8_t* ext = (uint8_t*)&gdt64[5] + 8;
        ext[0] = (uint8_t)((tss_base >> 32) & 0xFF);
        ext[1] = (uint8_t)((tss_base >> 40) & 0xFF);
        ext[2] = (uint8_t)((tss_base >> 48) & 0xFF);
        ext[3] = (uint8_t)((tss_base >> 56) & 0xFF);
        ext[4] = 0;  // reserved
        ext[5] = 0;  // reserved
        ext[6] = 0;  // reserved
        ext[7] = 0;  // reserved
    }

    // Zero the TSS
    for (int i = 0; i < (int)sizeof(struct tss_entry64); i++)
        ((uint8_t*)&tss64)[i] = 0;

    // Set I/O bitmap base past end (no I/O permissions for ring-3)
    tss64.iomap_base = sizeof(struct tss_entry64);

    // Load GDT and reload segments
    lgdt64(&gp64);
    reload_segments64();

    // Load TSS
    asm volatile("ltr %%ax" : : "a"((uint16_t)GDT64_TSS));
}

// Set RSP0 — called by the scheduler before every ring-3 → ring-0 switch
void tss_set_rsp064(uint64_t rsp0) {
    tss64.rsp0 = (uint32_t)rsp0;
    // For addresses > 4 GiB, the upper32 bits go in a separate field
    // (not needed if kernel stays in low memory)
}
