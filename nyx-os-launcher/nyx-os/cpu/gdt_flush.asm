; cpu/gdt_flush.asm
; Loads the GDT into the CPU using lgdt instruction

[BITS 32]
global gdt_flush

gdt_flush:
    mov eax, [esp+4]   ; Get pointer to GDT from argument
    lgdt [eax]         ; Load GDT

    mov ax, 0x10       ; 0x10 = kernel data segment
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    jmp 0x08:.flush    ; 0x08 = kernel code segment — far jump
.flush:
    ret
