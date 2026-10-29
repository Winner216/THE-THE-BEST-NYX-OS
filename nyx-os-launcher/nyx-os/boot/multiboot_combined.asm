[BITS 32]
MAGIC equ 0x1BADB002
FLAGS equ (1<<0)|(1<<1)
CHECKSUM equ -(MAGIC + FLAGS)

KERN64_PHYS equ 0x200000

section .multiboot
align 4
    dd MAGIC
    dd FLAGS
    dd CHECKSUM

section .rodata
align 4
kernel64_blob_start:
    incbin "/tmp/nyx64.bin"
kernel64_blob_end:
kernel64_blob_size equ kernel64_blob_end - kernel64_blob_start

section .bss
align 16
stack_bottom:
    resb 65536
stack_top:

section .text
align 4
global _start
_start:
    cli
    cmp eax, 0x2BADB002
    jne .fail

    mov esp, stack_top

    ; Copy 64-bit kernel to 0x200000
    mov esi, kernel64_blob_start
    mov edi, KERN64_PHYS
    mov ecx, kernel64_blob_size
    add ecx, 3
    shr ecx, 2
    rep movsd

    ; Build page tables at 0x900000 (zeroed)
    mov edi, 0x900000
    xor eax, eax
    mov ecx, 12288
    rep stosd

    ; PML4[0] → PDPT at 0x901000
    mov dword [0x900000], 0x901003
    mov dword [0x900004], 0

    ; PDPT[0] → 1 GiB page at PA 0
    mov dword [0x901000], 0x00000083
    mov dword [0x901004], 0

    ; Enable PAE
    mov eax, cr4
    or eax, (1<<5)|(1<<7)|(1<<9)|(1<<10)
    mov cr4, eax

    ; Load CR3
    mov eax, 0x900000
    mov cr3, eax

    ; Enable long mode
    mov ecx, 0xC0000080
    rdmsr
    or eax, (1<<8)
    wrmsr

    ; Enable paging
    mov eax, cr0
    or eax, (1<<31)|(1<<16)|(1<<1)
    mov cr0, eax

    ; Load 64-bit GDT
    lgdt [gdt_ptr]
    push dword 0x08
    push dword .long_entry
    retf

.fail:
    mov dword [0xB8000], 0x4F45
    hlt; jmp .fail

[BITS 64]
.long_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov rsp, 0xF0000

    ; Write "64!" to serial to prove long mode works
    mov edx, 0x3F8+5
.w1: in al, dx
    test al, 0x20
    jz .w1
    mov al, '6'
    mov edx, 0x3F8
    out dx, al

    mov edx, 0x3F8+5
.w2: in al, dx
    test al, 0x20
    jz .w2
    mov al, '4'
    mov edx, 0x3F8
    out dx, al

    mov edx, 0x3F8+5
.w3: in al, dx
    test al, 0x20
    jz .w3
    mov al, '!'
    mov edx, 0x3F8
    out dx, al

    mov edx, 0x3F8+5
.w4: in al, dx
    test al, 0x20
    jz .w4
    mov al, 10
    mov edx, 0x3F8
    out dx, al

    ; Jump to 64-bit kernel
    mov rax, 0x200000
    jmp rax

    cli; hlt; jmp $-1

section .data
gdt_ptr:
    dw gdt_end - gdt - 1
    dd gdt
gdt:
    dq 0
    dq 0x00AF9A000000FFFF
    dq 0x00CF92000000FFFF
gdt_end:
