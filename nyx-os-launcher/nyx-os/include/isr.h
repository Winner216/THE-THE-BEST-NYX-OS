// include/isr.h
#ifndef ISR_H
#define ISR_H

#include <stdint.h>

// CPU register state saved when interrupt fires
typedef struct {
    uint32_t ds;
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax; // pusha
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags, useresp, ss;            // pushed by CPU
} registers_t;

void isrs_install();
void irq_install();
void irq_install_handler(int irq, void (*handler)(registers_t));
void irq_install(void);

// ─── PIT timer — the kernel's preemption clock ───────────────────
// PIT channel 0 is reprogrammed in irq_install() from the BIOS
// default (~18.2 Hz) to a clean PIT_TARGET_HZ.  The PIT's base
// oscillator runs at 1193182 Hz, so the reload divisor is
// BASE / TARGET ≈ 23863 → one IRQ0 every 20 ms.
//
// Each IRQ0 increments `tick`, runs the heartbeat ticks (clock,
// cursor, autosave...), and then scheduler_tick() — the timer
// interrupt that FORCES context switches when a task's timeslice
// expires (see cpu/task.c and the switch hook in interrupts.asm).
#define PIT_BASE_HZ     1193182u
#define PIT_TARGET_HZ   50u
#define PIT_DIVISOR     (PIT_BASE_HZ / PIT_TARGET_HZ)   // 23863

// Timer ticks since boot, incremented by IRQ0 (the PIT fires
// PIT_TARGET_HZ times per second, so tick / PIT_TARGET_HZ ≈ elapsed
// seconds). Used by the About window for a live uptime and the
// boot-time benchmark.  VOLATILE: written by the ISR, and code that
// spins on it (timer_init's TSC calibration) must re-read it — a
// non-volatile read can be hoisted out of the loop by the optimizer,
// spinning forever (seen as an infinite `jmp .` in timer_init).
extern volatile uint32_t tick;

// Boot-time benchmark marks: written by kernel.c (kernel start,
// splash done) and login.c (first successful login), then printed as
// a desktop status + terminal line so real boot time is measurable
// on every boot and regressions are caught.
extern uint32_t boot_tick_start;
extern uint32_t boot_tick_splash;
extern uint32_t boot_tick_login;

#endif
