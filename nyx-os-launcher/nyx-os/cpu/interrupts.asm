; cpu/interrupts.asm
[BITS 32]

; Macro for ISR without error code
%macro ISR_NOERRCODE 1
global isr%1
isr%1:
    cli
    push byte 0
    push byte %1
    jmp isr_common_stub
%endmacro

; Macro for ISR with error code
%macro ISR_ERRCODE 1
global isr%1
isr%1:
    cli
    push byte %1
    jmp isr_common_stub
%endmacro

; Macro for IRQ
%macro IRQ 2
global irq%1
irq%1:
    cli
    push byte 0
    push byte %2
    jmp irq_common_stub
%endmacro

; Define all ISRs
ISR_NOERRCODE 0
ISR_NOERRCODE 1
ISR_NOERRCODE 2
ISR_NOERRCODE 3
ISR_NOERRCODE 4
ISR_NOERRCODE 5
ISR_NOERRCODE 6
ISR_NOERRCODE 7
ISR_ERRCODE   8
ISR_NOERRCODE 9
ISR_ERRCODE   10
ISR_ERRCODE   11
ISR_ERRCODE   12
ISR_ERRCODE   13
ISR_ERRCODE   14
ISR_NOERRCODE 15
ISR_NOERRCODE 16
ISR_ERRCODE   17
ISR_NOERRCODE 18
ISR_NOERRCODE 19

; Define IRQs — all 16 lines installed as IDT gates in irq_install()
; (isr.c), so any PCI device (e.g. the AC'97 sound card on IRQ 5/11)
; can be serviced, not just the PIT/keyboard/mouse defaults.
IRQ 0, 32
IRQ 1, 33
IRQ 2, 34
IRQ 3, 35
IRQ 4, 36
IRQ 5, 37
IRQ 6, 38
IRQ 7, 39
IRQ 8, 40
IRQ 9, 41
IRQ 10, 42
IRQ 11, 43
IRQ 12, 44
IRQ 13, 45
IRQ 14, 46
IRQ 15, 47

; External C handlers
extern isr_handler
extern irq_handler

; Multitasking scheduler hand-off (defined in cpu/task.c)
extern sched_switch_pending
extern sched_current_pcb
extern sched_new_esp
extern sched_fpu_save
extern sched_fpu_restore
extern sched_cr3
extern scheduler_pick_and_prepare

; System-call dispatcher (defined in cpu/syscall.c)
extern syscall_handler

; Common ISR stub
isr_common_stub:
    pusha
    mov ax, ds
    push eax
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    call isr_handler

    ; ── User-fault task switch ───────────────────────────────────
    ; A fault from a ring-3 task must kill ONLY that task, not the
    ; whole OS.  isr_handler() marks the offending task DONE and
    ; stages a switch (sets sched_switch_pending + friends) exactly
    ; like scheduler_tick() does; this hook performs the actual stack
    ; swap (fxsave → save frame → load next frame → fxrstor → iret)
    ; so the CPU continues in the next READY task.  For ring-0 faults
    ; (kernel bugs) isr_handler halts, sched_switch_pending stays 0,
    ; and this block is a no-op — same code as irq_common_stub's.
    mov  eax, [sched_switch_pending]
    test eax, eax
    jz   .isr_no_sched_switch
    mov  eax, [sched_fpu_save]
    fxsave [eax]
    mov  eax, [sched_current_pcb]
    mov  [eax], esp
    mov  eax, [sched_new_esp]
    mov  esp, eax
    mov  eax, [sched_cr3]
    mov  cr3, eax
    mov  eax, [sched_fpu_restore]
    fxrstor [eax]
    mov  dword [sched_switch_pending], 0
.isr_no_sched_switch:

    pop eax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    popa
    add esp, 8
    sti
    iret

; Common IRQ stub
irq_common_stub:
    pusha
    mov ax, ds
    push eax
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    call irq_handler

    ; ── Preemptive multitasking switch (the 3 steps) ───────────────
    ; irq_handler() → scheduler_tick() may set sched_switch_pending=1,
    ; with sched_new_esp = the next task's saved register frame and
    ; sched_current_pcb = the PCB of the task being preempted.  When
    ; set, swap contexts here:
    ;   1) fxsave the preempted task's FPU/SSE state (x87 + XMM — the
    ;      vesa.c blit uses SSE, so this state is live across IRQs)
    ;   2) store the current frame pointer at offset 0 of the current
    ;      PCB (esp is the FIRST member of pcb_t)
    ;   3) load the next task's frame pointer, fxrstor its FPU/SSE
    ;      state, then let the pop eax / popa / add esp,8 / iret below
    ;      restore its saved GPRs + segment registers and land the CPU
    ;      inside it.
    ;
    ; INVARIANTS: (a) fxsave/fxrstor #UD without CR4.OSFXSR set —
    ; boot.asm enables SSE (cr4 |= 0x600) before the kernel runs;
    ; (b) the FPU image is captured AFTER irq_handler returns, so it
    ; equals the interrupted task's state only because the i386 ABI
    ; (-m32, no -msse) keeps irq_handler and the whole IRQ path free
    ; of FPU/SSE instructions — never add SSE-using code to the
    ; heartbeat/scheduler path without moving the fxsave to stub entry.
    mov  eax, [sched_switch_pending]
    test eax, eax
    jz   .no_sched_switch
    mov  eax, [sched_fpu_save]
    fxsave [eax]                      ; (1) save outgoing FPU/SSE state
    mov  eax, [sched_current_pcb]     ; pcb_t* — esp lives at offset 0
    mov  [eax], esp                   ; (2) save this task's frame pointer
    mov  eax, [sched_new_esp]         ; next task's frame pointer
    mov  esp, eax                     ; (3) switch to the next stack
    mov  eax, [sched_cr3]
    mov  cr3, eax                     ; (4) switch to the next task's
                                        ;     page directory (its own
                                        ;     virtual address space)
    mov  eax, [sched_fpu_restore]
    fxrstor [eax]                     ; restore incoming FPU/SSE state
    mov  dword [sched_switch_pending], 0
.no_sched_switch:

    pop eax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    popa
    add esp, 8
    sti
    iret

; ─── Synchronous task switch (task_sleep / task_yield / task_exit) ─
; void task_switch_asm(void);
;
; Called from C with interrupts ENABLED.  Builds an interrupt-style
; register frame (identical to the layout irq_common_stub unwinds),
; asks the scheduler to stage a switch, then either restores our own
; frame (nothing else to run) or iret's into the next task.  When this
; task is later switched back in, execution resumes at .resume, which
; returns to the caller of task_switch_asm.
global task_switch_asm
task_switch_asm:
    cli
    ; CPU-pushed part of an interrupt frame (eflags/cs/eip).
    push dword 0x202        ; eflags: IF set on resume
    push dword 0x08         ; cs: kernel code
    push dword .resume      ; eip: where this task continues
    ; stub-pushed part.
    push dword 0            ; err
    push dword 0            ; int_no
    pusha                    ; edi esi ebp esp ebx edx ecx eax
    mov ax, ds
    push eax                 ; ds slot
    ; esp now points at the ds slot — the exact frame layout that
    ; switching expects (same as a real IRQ frame).
    call scheduler_pick_and_prepare   ; stages the switch; sets
                                        ; sched_current_pcb = our PCB
    test eax, eax
    jnz  .restore            ; nothing to switch to — keep running
    mov  eax, [sched_fpu_save]
    fxsave [eax]             ; save OUR FPU/SSE state
    mov  eax, [sched_current_pcb]
    mov  [eax], esp          ; save OUR frame into our PCB
    mov  eax, [sched_new_esp]
    mov  esp, eax            ; switch to the next task's frame
    mov  eax, [sched_cr3]
    mov  cr3, eax            ; switch to the next task's address space
    mov  eax, [sched_fpu_restore]
    fxrstor [eax]            ; restore the next task's FPU/SSE state
.restore:
    pop eax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    popa
    add esp, 8
    sti
    iret
.resume:
    ret

; ─── INT 0x80 system-call gate ────────────────────────────────────
; Installed by syscall_init() as IDT gate 0x80 with DPL=3 (callable
; from ring 3 once user mode lands; used by ring-0 kernel tasks now).
; Builds the same interrupt frame the ISR/IRQ stubs unwind and calls
; syscall_handler(registers_t*), which dispatches on eax, uses
; ebx/ecx/edx as args, and stores the result into the frame's saved
; eax — so the popa/iret below delivers it to the `int $0x80` caller.
global syscall_stub
syscall_stub:
    cli
    push byte 0          ; err
    push dword 0x80      ; int_no (dword: 0x80 would sign-extend as a byte)
    pusha                ; edi esi ebp esp ebx edx ecx eax
    mov ax, ds
    push eax
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov eax, esp         ; registers_t* → the ds slot
    push eax
    call syscall_handler
    add esp, 4
    pop eax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    popa
    add esp, 8
    sti
    iret
