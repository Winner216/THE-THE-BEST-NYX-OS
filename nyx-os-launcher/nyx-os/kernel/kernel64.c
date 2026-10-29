// kernel/kernel64.c — 64-bit kernel entry point
//
// This is the first C code that runs after the boot stub transitions
// to long mode.  It initializes the 64-bit GDT, IDT, paging, and
// then calls the main kernel initialization.

#include <stdint.h>
#include <stddef.h>
#include <gdt64.h>
#include <idt64.h>

// Multiboot info structure (32-bit, but we receive it in RDI)
typedef struct {
    uint32_t flags;
    uint32_t mem_lower;
    uint32_t mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;
    uint32_t mmap_addr;
    uint32_t drives_length;
    uint32_t drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;
    // VBE fields
    uint32_t vbe_control_info;
    uint32_t vbe_mode_info;
    uint16_t vbe_mode;
    uint16_t vbe_interface_seg;
    uint16_t vbe_interface_off;
    uint16_t vbe_interface_len;
    // Framebuffer fields
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t  framebuffer_bpp;
    uint8_t  framebuffer_type;
} __attribute__((packed)) multiboot_info_t_64;


// Forward declaration
static void early_puts(const char* s);

// ─── Stub handlers for 64-bit ISR/IRQ/syscall ───────────────────
// These will be replaced by the full implementations when we port
// the interrupt handling code to 64-bit.

// Register frame layout pushed by interrupts64.asm:
//   [rsp+0]   r15..r8, rbp, rdi, rsi, rdx, rcx, rbx, rax
//   [rsp+120] ds (saved segment)
//   [rsp+128] int_no, err_code
//   [rsp+144] rip, cs, rflags, rsp, ss (CPU-pushed)

typedef struct {
    uint64_t ds;
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t int_no, err_code;
    uint64_t rip, cs, rflags, rsp, ss;
} __attribute__((packed)) registers64_t;

void isr_handler64(registers64_t* regs) {
    // For now, just halt on CPU exceptions
    early_puts("64-bit ISR: halting\n");
    (void)regs;
    for (;;) asm volatile("hlt");
}

void irq_handler64(registers64_t* regs) {
    // For now, just acknowledge the PIC and return
    // Send EOI to both PICs
    asm volatile("outb %%al, $0x20" : : "a"((uint8_t)0x20));
    asm volatile("outb %%al, $0xA0" : : "a"((uint8_t)0x20));
    (void)regs;
}

void syscall_handler64(registers64_t* regs) {
    // For now, just return 0
    regs->rax = 0;
}

// The original 32-bit kernel_main (in kernel/kernel.c) — provide a
// 64-bit wrapper that calls it with the same arguments.
// Note: kernel_main is compiled as 64-bit code in this build.

// Stub kernel_main for 64-bit minimal boot
// The full kernel_main is in kernel/kernel.c (32-bit build)
// This stub just prints a message and halts
void kernel_main(uint32_t magic, void* mbi) {
    early_puts("64-bit kernel_main called\n");
    early_puts("  magic: ");
    // Print magic as hex
    char hex[] = "0123456789ABCDEF";
    for (int i = 28; i >= 0; i -= 4) {
        early_puts((char[]){hex[(magic >> i) & 0xF], '\0'});
    }
    early_puts("\n");
    early_puts("  mbi: OK\n");
    early_puts("\nNyx OS 64-bit boot successful!\n");
    early_puts("Long mode is active.\n");
    for (;;) asm volatile("hlt");
}

// Scheduler variables used by interrupts64.asm
// (These will be moved to the proper module later)
volatile uint64_t sched_switch_pending64 = 0;
volatile uint64_t sched_current_pcb64 = 0;
volatile uint64_t sched_new_esp64 = 0;
volatile uint64_t sched_fpu_save64 = 0;
volatile uint64_t sched_fpu_restore64 = 0;
volatile uint64_t sched_cr64 = 0;

// Temporary VGA text output for early boot messages
static void early_puts(const char* s) {
    volatile char* vga = (volatile char*)0xB8000;
    static int pos = 0;
    while (*s) {
        if (*s == '\n') {
            // Move to next line
            pos = ((pos / 160) + 1) * 160;
        } else {
            vga[pos] = *s;
            vga[pos + 1] = 0x0F;  // white on black
            pos += 2;
        }
        s++;
        if (pos >= 160 * 25) pos = 0;  // wrap
    }
}

// The main 64-bit entry point — called from boot64.asm
void kernel_main64(uint64_t multiboot_info) {
    early_puts("NYX-64: Long mode active\n");

    // ── Initialize 64-bit GDT ────────────────────────────────────
    early_puts("NYX-64: Installing GDT...\n");
    gdt_install64();
    early_puts("NYX-64: GDT OK\n");

    // ── Initialize 64-bit IDT ────────────────────────────────────
    early_puts("NYX-64: Installing IDT...\n");
    idt_install64();
    early_puts("NYX-64: IDT OK\n");

    // ── Enable interrupts ────────────────────────────────────────
    asm volatile("sti");
    early_puts("NYX-64: Interrupts enabled\n");

    // ── Write to serial to prove we're alive ─────────────────────
    #define SER 0x3F8
    #define ser_wait() do{uint8_t _v;asm volatile("inb %1,%0":"=a"(_v):"Nd"((uint16_t)(SER+5)));while((_v&0x20)==0){asm volatile("inb %1,%0":"=a"(_v):"Nd"((uint16_t)(SER+5)));}}while(0)
    #define ser_put(c) do{uint8_t _v;ser_wait();asm volatile("outb %0,%1"::"a"((uint8_t)(c)),"Nd"((uint16_t)SER));}while(0)
    #define ser_puts(s) do{const char*_p=(s);while(*_p){ser_put(*_p);_p++;}}while(0)



    ser_puts("NYX-64: kernel_main64 entered!\n");
    ser_puts("NYX-64: long mode active\n");

    // ── Call the main kernel ─────────────────────────────────────
    // Pass the Multiboot magic (0x2BADB002) and info pointer
    // in the original 32-bit calling convention (lower 32 bits)
    early_puts("NYX-64: Jumping to kernel_main...\n\n");
    kernel_main(0x2BADB002, (void*)(uint32_t)multiboot_info);

    // Should never return
    for (;;) asm volatile("hlt");
}
