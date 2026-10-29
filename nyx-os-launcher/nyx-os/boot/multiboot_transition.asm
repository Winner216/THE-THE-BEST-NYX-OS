; boot/multiboot_transition.asm — 32-bit Multiboot → 64-bit long mode
;
; This is loaded by GRUB or QEMU's -kernel as a 32-bit Multiboot ELF.
; It sets up long mode and jumps to the 64-bit kernel.

[BITS 32]

MAGIC     equ 0x1BADB002
FLAGS     equ (1<<0)|(1<<1)     ; meminfo + mem_addr (no vidmode — GRUB handles it)
CHECKSUM  equ -(MAGIC + FLAGS)

; ─── Multiboot header (must be in first 8 KiB) ──────────────────
section .multiboot
align 4
    dd MAGIC
    dd FLAGS
    dd CHECKSUM

; ─── BSS: stacks and page tables ────────────────────────────────
section .bss
align 16
stack_bottom:
    resb 65536                  ; 64 KiB temporary stack
stack_top:

; Page tables at fixed physical addresses
align 4096
pml4:
    resb 4096
pdpt:
    resb 4096
pd:
    resb 4096

; ─── 32-bit entry ───────────────────────────────────────────────
section .text
align 4
global _start
_start:
    ; Disable interrupts
    cli

    ; Validate Multiboot
    cmp eax, 0x2BADB002
    jne .fail

    ; Save multiboot info pointer
    mov [mb_info_ptr], ebx

    ; ── Set up stack ────────────────────────────────────────────
    mov esp, stack_top

    ; ── Build page tables ───────────────────────────────────────
    ; Zero all tables
    mov edi, pml4
    mov ecx, (4096 * 3) / 4
    xor eax, eax
    rep stosd

    ; PML4[0] → PDPT
    mov eax, pdpt
    or eax, 0x03                ; Present | Writable
    mov [pml4], eax

    ; PDPT[0] → PD
    mov eax, pd
    or eax, 0x03
    mov [pdpt], eax

    ; PD[0] → 2 MiB large page at physical 0
    mov dword [pd], 0x83        ; Present | Writable | PS

    ; ── Enable PAE ──────────────────────────────────────────────
    mov eax, cr4
    or eax, (1<<5)|(1<<7)|(1<<9)|(1<<10)
    mov cr4, eax

    ; ── Load PML4 into CR3 ──────────────────────────────────────
    mov eax, pml4
    mov cr3, eax

    ; ── Enable long mode ────────────────────────────────────────
    mov ecx, 0xC0000080         ; IA32_EFER
    rdmsr
    or eax, (1<<8)              ; LME
    wrmsr

    ; ── Enable paging ───────────────────────────────────────────
    mov eax, cr0
    or eax, (1<<31)|(1<<16)|(1<<1)
    mov cr0, eax

    ; ── Load 64-bit GDT ─────────────────────────────────────────
    lgdt [gdt64_ptr]

    ; ── Far jump to 64-bit code ─────────────────────────────────
    push dword 0x08             ; 64-bit code segment
    push dword .long_mode_entry
    retf

.fail:
    ; Print 'E' to VGA
    mov dword [0xB8000], 0x4F45
    hlt
    jmp .fail

; ─── 64-bit entry ───────────────────────────────────────────────
[BITS 64]
.long_mode_entry:
    ; Set up 64-bit data segment
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Set up 64-bit stack (at physical0xF0000, below kernel)
    mov rsp, 0xF0000

    ; Load multiboot info pointer into RDI (first argument)
    mov rdi, [mb_info_ptr]

    ; Jump to the 64-bit kernel entry point
    ; kernel_main64 is linked at the address specified by the linker script
    extern kernel_main64
    call kernel_main64

    ; Should never return
    cli
.h:
    hlt
    jmp .h

; ─── Data ───────────────────────────────────────────────────────
section .data
align 4

; Minimal 64-bit GDT
gdt64:
    ; Null
    dq 0
    ; Code (0x08): Present, DPL=0, Code, Exec, Readable, L=1, G=1
    dw 0xFFFF, 0x0000
    db 0x00, 0x9A, 0xAF, 0x00
    ; Data (0x10): Present, DPL=0, Data, Writable, D/B=1, G=1
    dw 0xFFFF, 0x0000
    db 0x00, 0x92, 0xCF, 0x00

gdt64_ptr:
    dw gdt64_ptr - gdt64 - 1   ; limit
    dd gdt64                    ; base

; ─── Storage ────────────────────────────────────────────────────
section .bss
align 8
mb_info_ptr:
    resb 8
