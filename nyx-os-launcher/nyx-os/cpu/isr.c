// cpu/isr.c
#include <isr.h>
#include <idt.h>
#include <io.h>
#include <terminal.h>
#include <speaker.h>
#include <audio.h>
#include <stdint.h>
#include <music.h>
#include <games.h>
#include <sysmon.h>
#include <clock.h>
#include <physics.h>
#include <weather.h>
#include <screensaver.h>
#include <fs.h>
#include <desktop.h>
#include <task.h>


void desktop_update_clock(void);

static const char *exception_names[] = {
    "Division By Zero",
    "Debug",
    "Non Maskable Interrupt",
    "Breakpoint",
    "Overflow",
    "Out of Bounds",
    "Invalid Opcode",
    "No FPU",
    "Double Fault",
    "FPU Segment Overrun",
    "Bad TSS",
    "Segment Not Present",
    "Stack Fault",
    "General Protection Fault",
    "Page Fault",
    "Unknown Interrupt",
    "FPU Fault",
    "Alignment Check",
    "Machine Check",
    "Reserved",
};

extern void isr0();  extern void isr1();  extern void isr2();
extern void isr3();  extern void isr4();  extern void isr5();
extern void isr6();  extern void isr7();  extern void isr8();
extern void isr9();  extern void isr10(); extern void isr11();
extern void isr12(); extern void isr13(); extern void isr14();
extern void isr15(); extern void isr16(); extern void isr17();
extern void isr18(); extern void isr19();
extern void irq0();  extern void irq1();  extern void irq2();
extern void irq3();  extern void irq4();  extern void irq5();
extern void irq6();  extern void irq7();  extern void irq8();
extern void irq9();  extern void irq10(); extern void irq11();
extern void irq12(); extern void irq13(); extern void irq14();
extern void irq15();

extern void desktop_draw(void);

static void (*irq_handlers[16])(registers_t) = {0};

void irq_install_handler(int irq, void (*handler)(registers_t)) {
    irq_handlers[irq] = handler;
}

// Small hex printer for the fault diagnostics below.
static void hex32(uint32_t v) {
    const char* hex = "0123456789ABCDEF";
    terminal_print("0x");
    for (int i = 7; i >= 0; i--)
        terminal_putchar(hex[(v >> (i * 4)) & 0xF]);
}

// Debug: dump a value to COM1 so a VESA-mode fault (whose VGA text
// diagnostics are invisible) is still readable in the QEMU serial log.
// Bounded retry: never spin forever on a machine without a UART.
static void pf_serial(uint32_t v) {
    const char* hex = "0123456789ABCDEF";
    for (int i = 7; i >= 0; i--) {
        int spins = 0;
        while ((inb(0x3F8 + 5) & 0x20) == 0 && spins++ < 100000) { }
        outb(0x3F8, (uint8_t)hex[(v >> (i * 4)) & 0xF]);
    }
    int spins = 0;
    while ((inb(0x3F8 + 5) & 0x20) == 0 && spins++ < 100000) { }
    outb(0x3F8, '\n');
}

void isr_handler(registers_t regs) {
    if (regs.int_no < 20) {
        // A fault whose CS has RPL 3 came from a ring-3 USER task.  The
        // point of user mode is that a misbehaving program kills ONLY
        // itself: kill the task, stage a switch to the next READY task
        // (the isr_common_stub hook in interrupts.asm performs the swap),
        // and the rest of the OS keeps running.  Only ring-0 faults
        // (kernel bugs) halt the machine.
        if ((regs.cs & 3) == 3) {
            terminal_print("\n[USER FAULT] ");
            terminal_print(exception_names[regs.int_no]);
            terminal_print(" in task ");
            terminal_print(task_current_name());
            terminal_print(" (pid ");
            { char d[2]; d[0] = (char)('0' + (task_current_id() & 0xF)); d[1] = 0; terminal_print(d); }
            terminal_print(") — task killed, OS continues\n");
            if (regs.int_no == 14) {
                // Serial diagnostics (terminal_print is invisible in
                // VESA mode): PF! + faulting address + instruction.
                uint32_t ucr2;
                asm volatile("mov %%cr2, %0" : "=r" (ucr2));
                pf_serial(0x50584621u);          // "PFX!" tag
                pf_serial(ucr2);
                pf_serial(regs.eip);
            }
            task_kill_current();
            // Stage the switch away from the dead task; the asm hook
            // swaps stacks + iret's into the next READY task.  If the
            // whole ready queue is empty there is nothing to run — halt.
            if (scheduler_pick_and_prepare() != 0) {
                terminal_print("\nNo runnable tasks left — System Halted!\n");
                for(;;);
            }
            return;
        }
        terminal_print("\n[NYX EXCEPTION] ");
        terminal_print(exception_names[regs.int_no]);
        if (regs.int_no == 14) {
            // Page fault — with paging enabled this is how a task is
            // stopped from touching memory outside its address space.
            uint32_t cr2;
            asm volatile("mov %%cr2, %0" : "=r" (cr2));
            // Visible only via the serial log in VESA mode — see the
            // COM1 dump at the end of this branch.
            pf_serial(0x50584600u | 0x21u);      // "PF!" marker
            pf_serial(cr2);                      // faulting address
            pf_serial(regs.eip);                 // faulting instruction
            terminal_print("\n  faulting task: ");
            terminal_print(task_current_name());
            terminal_print(" (pid ");
            terminal_print("0x");
            // pid as hex (single digit)
            { char d[2]; d[0] = (char)('0' + (task_current_id() & 0xF)); d[1] = 0; terminal_print(d); }
            terminal_print(")");
            terminal_print("\n  unmapped virtual address: ");
            hex32(cr2);
            terminal_print("\n  eip: ");
            hex32(regs.eip);
            terminal_print(" (this is the isolation working — ");
            terminal_print("cross-task access faults here)");
        }
        terminal_print("\nSystem Halted!");
        for(;;);
    }
}

// Not static: exposed via isr.h so the About window can show a
// live uptime. The PIT is reprogrammed to PIT_TARGET_HZ (50) Hz in
// irq_install(), so tick / PIT_TARGET_HZ ≈ seconds.  Volatile: the
// ISR writes it and other code spins on it (see isr.h).
volatile uint32_t tick = 0;
// Boot-time benchmark marks (kept here next to tick; written by
// kernel.c and login.c, read when the user first logs in).
uint32_t boot_tick_start  = 0;
uint32_t boot_tick_splash = 0;
uint32_t boot_tick_login  = 0;

void irq_handler(registers_t regs) {
    if (regs.int_no >= 8)
        outb(0xA0, 0x20);
    outb(0x20, 0x20);

    if (regs.int_no == 32) {
        tick++;
        speaker_tick();
        music_tick();
        audio_tick();
        snake_tick();
        sysmon_tick();
        clock_tick();
        physics_tick();
        weather_tick();   
        minesweeper_tick();
        screensaver_tick();       
        // Update clock display once per second (50 ticks at 50 Hz)
        if (tick % 50 == 0)
            desktop_update_clock();
        desktop_heartbeat();
        // Periodic autosave — a safety net for unclean exits (killed
        // VM, power loss). Every ~11 seconds at 50 Hz ≈ 550 ticks.
        // fs_save_to_disk() is a no-op if ata_init() never found a
        // disk, so this is always safe.
        if (tick % 550 == 0)
            fs_save_to_disk();
        // Preemptive multitasking: may context-switch to another task
        // (the asm hook in irq_common_stub swaps the stacks right
        // after irq_handler returns, before the stub's iret).
        scheduler_tick();
    }

    int irq = regs.int_no - 32;
    if (irq >= 0 && irq < 16 && irq_handlers[irq])
        irq_handlers[irq](regs);
}
void isrs_install() {
    idt_set_gate(0,  (uint32_t)isr0,  0x08, 0x8E);
    idt_set_gate(1,  (uint32_t)isr1,  0x08, 0x8E);
    idt_set_gate(2,  (uint32_t)isr2,  0x08, 0x8E);
    idt_set_gate(3,  (uint32_t)isr3,  0x08, 0x8E);
    idt_set_gate(4,  (uint32_t)isr4,  0x08, 0x8E);
    idt_set_gate(5,  (uint32_t)isr5,  0x08, 0x8E);
    idt_set_gate(6,  (uint32_t)isr6,  0x08, 0x8E);
    idt_set_gate(7,  (uint32_t)isr7,  0x08, 0x8E);
    idt_set_gate(8,  (uint32_t)isr8,  0x08, 0x8E);
    idt_set_gate(9,  (uint32_t)isr9,  0x08, 0x8E);
    idt_set_gate(10, (uint32_t)isr10, 0x08, 0x8E);
    idt_set_gate(11, (uint32_t)isr11, 0x08, 0x8E);
    idt_set_gate(12, (uint32_t)isr12, 0x08, 0x8E);
    idt_set_gate(13, (uint32_t)isr13, 0x08, 0x8E);
    idt_set_gate(14, (uint32_t)isr14, 0x08, 0x8E);
    idt_set_gate(15, (uint32_t)isr15, 0x08, 0x8E);
    idt_set_gate(16, (uint32_t)isr16, 0x08, 0x8E);
    idt_set_gate(17, (uint32_t)isr17, 0x08, 0x8E);
    idt_set_gate(18, (uint32_t)isr18, 0x08, 0x8E);
    idt_set_gate(19, (uint32_t)isr19, 0x08, 0x8E);
}
void irq_install() {
    // Remap PIC (Programmable Interrupt Controller): IRQ 0-7 → INT 32-39,
    // IRQ 8-15 → INT 40-47. Standard boilerplate for protected-mode x86.
    outb(0x20, 0x11); outb(0xA0, 0x11);
    outb(0x21, 0x20); outb(0xA1, 0x28);
    outb(0x21, 0x04); outb(0xA1, 0x02);
    outb(0x21, 0x01); outb(0xA1, 0x01);
    outb(0x21, 0x0);  outb(0xA1, 0x0);

    idt_set_gate(32, (uint32_t)irq0,  0x08, 0x8E); // Timer
    idt_set_gate(33, (uint32_t)irq1,  0x08, 0x8E); // Keyboard
    idt_set_gate(34, (uint32_t)irq2,  0x08, 0x8E); // PIC cascade
    idt_set_gate(35, (uint32_t)irq3,  0x08, 0x8E);
    idt_set_gate(36, (uint32_t)irq4,  0x08, 0x8E);
    idt_set_gate(37, (uint32_t)irq5,  0x08, 0x8E); // PCI device IRQ (AC'97 = 5 or 11)
    idt_set_gate(38, (uint32_t)irq6,  0x08, 0x8E);
    idt_set_gate(39, (uint32_t)irq7,  0x08, 0x8E);
    idt_set_gate(40, (uint32_t)irq8,  0x08, 0x8E);
    idt_set_gate(41, (uint32_t)irq9,  0x08, 0x8E);
    idt_set_gate(42, (uint32_t)irq10, 0x08, 0x8E);
    idt_set_gate(43, (uint32_t)irq11, 0x08, 0x8E);
    idt_set_gate(44, (uint32_t)irq12, 0x08, 0x8E); // PS/2 Mouse (IRQ12 → INT44)
    idt_set_gate(45, (uint32_t)irq13, 0x08, 0x8E); // FPU
    idt_set_gate(46, (uint32_t)irq14, 0x08, 0x8E); // IDE primary
    idt_set_gate(47, (uint32_t)irq15, 0x08, 0x8E); // IDE secondary

    // ── Program the PIT (the preemption clock) ───────────────────
    // Reprogram PIT channel 0 from its BIOS default (~18.2 Hz) to
    // PIT_TARGET_HZ.  IRQ0 then fires 50 times/second; each fire
    // runs the heartbeat ticks above and then scheduler_tick(), which
    // FORCES a context switch when the current task's timeslice
    // expires — the timer interrupt handing execution from the
    // current task over to the kernel scheduler.  One tick = 20 ms.
    // Mode 3 (square wave), binary count, lobyte-then-hibyte.
    uint16_t pit_div = (uint16_t)PIT_DIVISOR;
    outb(0x43, 0x36);              // channel 0, lobyte/hibyte, mode 3, binary
    outb(0x40, pit_div & 0xFF);    // low byte
    outb(0x40, pit_div >> 8);      // high byte
}
