; tools/bench/boot.asm — Minimal multiboot stub for the TCG decode benchmark.
; Reuses the kernel's flat GDT/IDT (cpu/gdt.c, cpu/idt.c) and the real
; jpegdec.c, so the measured decode time is the kernel's actual code path.
MBALIGN   equ 1 << 0
MEMINFO   equ 1 << 1
FLAGS     equ MBALIGN | MEMINFO
MAGIC     equ 0x1BADB002
CHECKSUM  equ -(MAGIC + FLAGS)

section .multiboot
align 4
    dd MAGIC
    dd FLAGS
    dd CHECKSUM

section .bss
align 16
stack_bottom:
    ; 256 KB — jpeg_decode() needs ~15 KB itself plus the deep
    ; IRQ0-nested call chain; the kernel ships a 96 KB stack for the
    ; same reason.  Give the bench generous headroom so a decode
    ; failure can never be blamed on stack depth.
    resb 262144
stack_top:

section .text
global _start
_start:
    mov esp, stack_top

    ; Enable SSE2 (CR4.OSFXSR|OSXMMEXCPT) so the PRESENT copy bench
    ; can use movdqu — mirrors boot/boot.asm in the main kernel.
    mov eax, cr4
    or  eax, 0x600
    mov cr4, eax
    mov eax, cr0
    and eax, ~((1 << 2) | (1 << 3))
    or  eax, (1 << 1)
    mov cr0, eax

    extern kernel_main
    call kernel_main
.hlt:
    hlt
    jmp .hlt
