; tools/bench/irq.asm — Minimal IRQ0 stub for the decode benchmark.
; Only the PIT timer is needed (to advance the tick counter used by the
; same ms_since() math as cpu/video.c).  All other IRQs are unhandled.

[BITS 32]
global irq0_stub
extern bench_tick_handler

irq0_stub:
    cli
    cld
    pusha
    call bench_tick_handler
    popa
    mov al, 0x20
    out 0x20, al
    iret
