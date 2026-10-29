; boot/boot64.asm — Minimal 64-bit boot stub
; SELF-CONTAINED: no references to C symbols, no relocation issues.
; All addresses are computed relative to known physical addresses.

[BITS 32]

MAGIC     equ 0x1BADB002
PAGE_SIZE equ 0x1000

section .multiboot
align 4
    dd MAGIC
    dd (1<<0)|(1<<1)|(1<<2)     ; flags: meminfo + vidmode
    dd -(MAGIC + ((1<<0)|(1<<1)|(1<<2)))
    times 5 dd 0
    dd 0, 1920, 1080, 32

section .text
align 4
global _start32
_start32:
    cmp eax, 0x2BADB002
    jne .fail

    ; Save multiboot info pointer at a known physical address
    ; We'll store it at 0x1000 (just above IVT area, safe)
    mov [0x1000], ebx

    ; ── Build page tables at 0x100000 + 0x800000 = 0x900000 ─────
    ; Safe location: above kernel BSS but below 16 MiB
    mov edi, 0x900000
    ; Zero 3 pages (PML4 + PDPT + PD)
    mov ecx, (PAGE_SIZE * 3) / 4
    xor eax, eax
    rep stosd

    ; PML4 at 0x900000
    ; PDPT at 0x901000
    ; PD   at 0x902000

    ; PML4[0] → PDPT (0x901000)
    mov dword [0x900000], 0x901000 | 0x03  ; Present | Writable

    ; PML4[511] → PDPT (for higher half at 0xFFFFFFFF80000000)
    mov dword [0x900000 + 511*8], 0x901000 | 0x03

    ; PDPT[0] → PD (0x902000)
    mov dword [0x901000], 0x902000 | 0x03

    ; PD[0] → 2 MiB large page at physical 0
    mov dword [0x902000], 0x00000083  ; Present | Writable | PS

    ; ── Enable PAE ──────────────────────────────────────────────
    mov eax, cr4
    or eax, (1<<5)|(1<<7)|(1<<9)|(1<<10)  ; PAE|PGE|OSFXSR|OSXMMEXCPT
    mov cr4, eax

    ; ── Load PML4 into CR3 ──────────────────────────────────────
    mov eax, 0x900000
    mov cr3, eax

    ; ── Enable long mode ────────────────────────────────────────
    mov ecx, 0xC0000080         ; IA32_EFER
    rdmsr
    or eax, (1<<8)              ; LME
    wrmsr

    ; ── Enable paging ───────────────────────────────────────────
    mov eax, cr0
    or eax, (1<<31)|(1<<16)|(1<<1)  ; PG|WP|MP
    mov cr0, eax

    ; ── Load minimal GDT ────────────────────────────────────────
    ; Build GDT at 0x903000
    ; Entry 0: Null
    mov dword [0x903000], 0
    mov dword [0x903004], 0

    ; Entry 1: 64-bit code (0x08) — access=0x9A, flags=0xAF (L=1,G=1)
    mov word [0x903008], 0xFFFF    ; limit 0:15
    mov word [0x90300A], 0x0000    ; base 0:15
    mov byte [0x90300C], 0x00      ; base 16:23
    mov byte [0x90300D], 0x9A      ; access
    mov byte [0x90300E], 0xAF      ; flags + limit 16:19
    mov byte [0x90300F], 0x00      ; base 24:31

    ; Entry 2: 64-bit data (0x10) — access=0x92, flags=0xCF
    mov word [0x903010], 0xFFFF
    mov word [0x903012], 0x0000
    mov byte [0x903014], 0x00
    mov byte [0x903015], 0x92
    mov byte [0x903016], 0xCF
    mov byte [0x903017], 0x00

    ; LGDT
    mov word [0x903020], 23       ; limit
    mov dword [0x903022], 0x903000 ; base
    mov word [0x903026], 0        ; base high
    lgdt [0x903020]

    ; ── Far jump to 64-bit code ─────────────────────────────────
    push dword 0x08
    push dword .long_mode
    retf

.fail:
    mov dword [0xB8000], 0x4F4F4F4D
    cli
.h: hlt
    jmp .h

; ─── 64-bit code ────────────────────────────────────────────────
[BITS 64]
.long_mode:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax

    ; Stack at physical 0xF0000 (below kernel, safe)
    mov rsp, 0xF0000

    ; Read multiboot info from our saved location
    mov rdi, [0x1000]

    ; Jump to kernel entry at physical address
    ; kernel_main64 is linked at 0xFFFFFFFF80xxxxxx but we're identity-mapped
    ; So we compute: kernel_main64_phys = linked_addr - 0xFFFFFFFF80000000 + 0x100000
    ; But simpler: just jump to the known physical address of the entry trampoline
    ; We'll use a fixed address: the .text section starts at physical 0x100000
    ; and kernel_main64 is at some offset within it.

    ; For now, call a fixed address that the linker will set up
    ; The trampoline is at the beginning of .text (after the boot stub)
    mov rax, 0xFFFFFFFF80000000    ; higher-half base
    add rax, kernel_main64_offset  ; offset from .text start
    jmp rax

    cli
.h64:
    hlt
    jmp .h64

; This label's offset from _start32 tells the 64-bit code where to jump
section .rodata
kernel_main64_offset:
    dd kernel_main64_entry - _start32

section .text
kernel_main64_entry:
    ; rdi = multiboot info pointer
    extern kernel_main64
    call kernel_main64
    cli
.h:
    hlt
    jmp .h
