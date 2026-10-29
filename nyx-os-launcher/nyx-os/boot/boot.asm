; boot/boot.asm — Original simple boot stub (VESA via GRUB, no real-mode trampoline)

MBALIGN   equ 1 << 0
MEMINFO   equ 1 << 1
VIDMODE   equ 1 << 2
FLAGS     equ MBALIGN | MEMINFO | VIDMODE
MAGIC     equ 0x1BADB002
CHECKSUM  equ -(MAGIC + FLAGS)

section .multiboot
align 4
    dd MAGIC
    dd FLAGS
    dd CHECKSUM
    times 5 dd 0
    dd 0
    dd 1920
    dd 1080
    dd 32

section .bss
align 16
stack_bottom:
    ; 96 KB kernel stack.  jpeg_decode() alone needs ~15 KB (its four
    ; 512-node Huffman trees live on the stack), and it can be called
    ; from deep chains (keyboard IRQ -> file open -> decode -> draw).
    ; The old 16 KB was overflowing at video open and hard-freezing
    ; the whole OS.
    resb 98304
stack_top:

section .text
global _start

_start:
    mov esp, stack_top

    ; Push the multiboot args FIRST — kernel_main(magic, mbi) needs the
    ; original EAX (0x2BADB002) and EBX (mbi pointer), and the SSE
    ; enable below clobbers EAX.  (Pushing the CR4 value instead of the
    ; magic made the kernel skip vesa_init → black screen; fixed by
    ; saving both registers before the clobbering code.)
    push ebx
    push eax

    ; ── Enable SSE2 ─────────────────────────────────────────────────
    ; The kernel uses SSE2 wide copies (vesa_copy16 in cpu/vesa.c) for
    ; the memory-bound paths (vesa_present copies ~8.3 MB/frame).
    ; Without CR4.OSFXSR every SSE instruction #UDs, so set it here
    ; before the first C code runs.  OSXMMEXCPT lets us use movdqu
    ; without SIMD exception traps.
    ;
    ; We deliberately do NOT enable AVX (CR4.OSXSAVE + XCR0 + YMM
    ; save/restore) — the byte-copy win is mostly captured by 16-byte
    ; SSE2 moves, and IRQ handlers must stay YMM-free.  Future
    ; engineers wanting AVX must also handle XMM/YMM state across the
    ; ISR entry path.
    mov eax, cr4
    or  eax, 0x600          ; OSFXSR(9) | OSXMMEXCPT(10)
    mov cr4, eax

    ; CR0: clear EM(2)+TS(3) so no #NM on first SSE use, set MP(1).
    ; GRUB normally leaves these right, but be explicit — a stray TS
    ; or EM would make the first movdqu fault.
    mov eax, cr0
    and eax, ~((1 << 2) | (1 << 3))
    or  eax, (1 << 1)
    mov cr0, eax

    extern kernel_main
    call kernel_main
    cli
.hang:
    hlt
    jmp .hang
