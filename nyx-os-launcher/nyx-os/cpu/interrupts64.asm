; cpu/interrupts64.asm — 64-bit ISR/IRQ/syscall entry stubs
;
; In long mode, the interrupt frame is different from 32-bit:
;   - CPU pushes SS, RSP, RFLAGS, CS, RIP (and maybe error code)
;   - We push all GPRs (64-bit: rax, rbx, rcx, rdx, rsi, rdi, rbp, r8-r15)
;   - We push ds, es, fs, gs
;   - Total frame size: ~180 bytes (much larger than 32-bit)

[BITS 64]

; ─── External C handlers ─────────────────────────────────────────
extern isr_handler64
extern irq_handler64
extern syscall_handler64

; ─── Scheduler variables (64-bit pointers) ───────────────────────
extern sched_switch_pending64
extern sched_current_pcb64
extern sched_new_esp64
extern sched_fpu_save64
extern sched_fpu_restore64
extern sched_cr64

; ─── Macro: ISR without error code ──────────────────────────────
%macro ISR_NOERRCODE64 1
global isr64_%1
isr64_%1:
    ; CPU pushed: SS, RSP, RFLAGS, CS, RIP
    push qword 0           ; dummy error code
    push qword %1          ; interrupt number
    jmp isr64_common_stub
%endmacro

; ─── Macro: ISR with error code ─────────────────────────────────
%macro ISR_ERRCODE64 1
global isr64_%1
isr64_%1:
    ; CPU pushed: SS, RSP, RFLAGS, CS, RIP, error_code
    push qword %1          ; interrupt number
    jmp isr64_common_stub
%endmacro

; ─── Macro: IRQ ──────────────────────────────────────────────────
%macro IRQ64 2
global irq64_%1
irq64_%1:
    push qword 0           ; dummy error code
    push qword %2          ; interrupt number (32 + IRQ)
    jmp irq64_common_stub
%endmacro

; ─── Define ISRs (0-19) ─────────────────────────────────────────
ISR_NOERRCODE64 0
ISR_NOERRCODE64 1
ISR_NOERRCODE64 2
ISR_NOERRCODE64 3
ISR_NOERRCODE64 4
ISR_NOERRCODE64 5
ISR_NOERRCODE64 6
ISR_NOERRCODE64 7
ISR_ERRCODE64   8
ISR_NOERRCODE64 9
ISR_ERRCODE64   10
ISR_ERRCODE64   11
ISR_ERRCODE64   12
ISR_ERRCODE64   13
ISR_ERRCODE64   14
ISR_NOERRCODE64 15
ISR_NOERRCODE64 16
ISR_ERRCODE64   17
ISR_NOERRCODE64 18
ISR_NOERRCODE64 19

; ─── Define IRQs (0-15 → IDT 32-47) ─────────────────────────────
IRQ64 0, 32
IRQ64 1, 33
IRQ64 2, 34
IRQ64 3, 35
IRQ64 4, 36
IRQ64 5, 37
IRQ64 6, 38
IRQ64 7, 39
IRQ64 8, 40
IRQ64 9, 41
IRQ64 10, 42
IRQ64 11, 43
IRQ64 12, 44
IRQ64 13, 45
IRQ64 14, 46
IRQ64 15, 47

; ─── Common ISR stub ─────────────────────────────────────────────
isr64_common_stub:
    ; Save all general-purpose registers (64-bit)
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; Save segment registers
    mov ax, ds
    push rax

    ; Load kernel data segment
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; Call C handler with pointer to register frame as first argument
    mov rdi, rsp        ; rdi = pointer to the saved registers
    call isr_handler64

    jmp isr64_common_restore

; ─── Common IRQ stub ─────────────────────────────────────────────
irq64_common_stub:
    ; Save all general-purpose registers (64-bit)
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; Save segment registers
    mov ax, ds
    push rax

    ; Load kernel data segment
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; Call C handler
    mov rdi, rsp        ; rdi = pointer to the saved registers
    call irq_handler64

    ; ── Preemptive multitasking switch ───────────────────────────
    ; Check if scheduler wants to switch tasks
    mov rax, [sched_switch_pending64]
    test rax, rax
    jz .no_sched_switch

    ; Save outgoing FPU/SSE state (fxsave)
    mov rax, [sched_fpu_save64]
    fxsave [rax]

    ; Save outgoing task's frame pointer
    mov rax, [sched_current_pcb64]
    mov [rax], rsp      ; pcb->rsp = current frame

    ; Load incoming task's frame pointer
    mov rax, [sched_new_esp64]
    mov rsp, rax

    ; Switch page tables
    mov rax, [sched_cr64]
    mov cr3, rax

    ; Restore incoming FPU/SSE state
    mov rax, [sched_fpu_restore64]
    fxrstor [rax]

    ; Clear switch flag
    mov qword [sched_switch_pending64], 0

.no_sched_switch:

isr64_common_restore:
    ; Restore segment registers
    pop rax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; Restore all general-purpose registers
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    ; Remove interrupt number and error code from stack
    add rsp, 16

    sti
    iretq

; ─── INT 0x80 system call stub ──────────────────────────────────
global syscall64_stub
syscall64_stub:
    ; Save all general-purpose registers
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; Save segment registers
    mov ax, ds
    push rax

    ; Load kernel data segment
    mov ax, 0x10
    mov ds, ax
    mov es, ax

    ; Call C syscall handler with pointer to register frame
    mov rdi, rsp
    call syscall_handler64

    ; Restore segment registers
    pop rax
    mov ds, ax
    mov es, ax

    ; Restore all general-purpose registers
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    add rsp, 16
    sti
    iretq
