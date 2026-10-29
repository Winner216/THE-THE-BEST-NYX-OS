// include/task.h  –  Nyx OS multitasking (PCB + round-robin scheduler)
//
// The kernel is a single-address-space, ring-0 OS with no paging, so
// "multitasking" here means preemptive context switching between
// kernel tasks, each with its OWN:
//   • PCB          — the Process Control Block below
//   • kernel stack — a private stack region (unique memory per task)
//   • arena        — a private bump-allocated workspace (unique memory)
//
// Context switches happen on the PIT timer IRQ (50 Hz) from
// scheduler_tick(), called in irq_handler() just before the stub's
// iret.  The actual stack swap is done by a small hook in
// cpu/interrupts.asm (irq_common_stub): when sched_switch_pending is
// set, it fxsave's the outgoing task's FPU/SSE state, stores the
// current task's register frame pointer into [sched_current_pcb] (the
// PCB's FIRST member — see pcb_t.esp), loads sched_new_esp, and
// fxrstor's the incoming task's FPU/SSE state, so the stub's popa/
// add esp,8/iret lands in the next task with ALL its saved context
// (GPRs, segment registers, x87 + XMM).
//
// SAFETY CONTRACT for background tasks: they are preempted at ring 0
// and share the identity-mapped kernel address space.  They must
// therefore NOT touch vesa.fb, the filesystem, desktop globals, or any
// I/O.  They may use their private arena, private statics, and the
// task API only.
//
// MEMORY ISOLATION: since paging was enabled (cpu/paging.c) each task
// has its OWN page directory (pcb_t.cr3, switched via CR3 on every
// context switch) and task_alloc_page() hands out pages mapped ONLY in
// the calling task's page directory — cross-task access to that memory
// faults (#PF).  Isolation is per-task-ALLOCATED memory; all tasks still
// run at ring 0, so kernel/shared memory (BSS, vesa.fb, the desktop) is
// readable by any task by design.  True protection of ALL memory is the
// ring-3 step (user segments + DPL-3 int 0x80 are already in place).
//
// SCHEDULING: the scheduler keeps a ready queue — an index-linked
// list over the tasks[] table (rq_head/rq_tail/rq_len in task.c) —
// holding every READY task waiting for CPU time.  The RUNNING task is
// never in the queue.  The run-next algorithm is selectable at runtime:
//   • SCHED_RR       (default) — round-robin: fixed timeslice
//     (TASK_TIMESLICE), preempted tasks re-enter at the queue tail.
//   • SCHED_FCFS     — first-come-first-served: NO timeslice
//     preemption; a task runs until it blocks (task_sleep/yield/exit).
//     CAVEAT: if the running task never blocks (main never does), the
//     other tasks are starved — that's what "run until it blocks"
//     means, so don't expect the demo tasks to tick in FCFS.
//   • SCHED_PRIORITY — strict priority: the queue is kept sorted by
//     pcb_t.priority (higher first), so the head is always the most
//     preferred task; a higher-priority running task is never handed
//     down a slice.
// Switch it with sched_set_algorithm() or the shell `sched` command.
// NOTE: switching algorithms (or changing a task's priority) does NOT
// re-sort tasks already sitting in the queue — the new order takes
// effect as each task re-enters (requeue / wakeup).

#ifndef TASK_H
#define TASK_H

#include <stdint.h>

#define TASK_MAX          8
#define TASK_STACK_SIZE   16384   // 16 KiB private stack per task
#define TASK_ARENA_SIZE   1024    // 1 KiB private workspace per task
#define TASK_TIMESLICE    2       // ticks per slice (40 ms at 50 Hz)

typedef enum {
    TASK_READY   = 0,
    TASK_RUNNING = 1,
    TASK_WAITING = 2,             // sleeping (task_sleep)
    TASK_DONE    = 3,             // exited (task_exit)
} task_state_t;

// ─── Process Control Block ───────────────────────────────────────
// esp MUST remain the FIRST member: the scheduler asm hook writes the
// saved frame pointer directly to offset 0 of the current PCB
// (mov [sched_current_pcb], esp).  The "frame pointer" is the value
// of ESP inside irq_common_stub at the point of the switch — i.e. the
// address of the saved-ds slot of the task's interrupt frame, such
// that the stub's pop eax / popa / add esp,8 / iret sequence restores
// the task exactly.
typedef struct pcb {
    uint32_t     esp;          // saved frame pointer (MUST be first)
    int          id;
    const char*  name;
    task_state_t state;
    uint32_t     entry;        // program counter (initial EIP)
    uint32_t     cr3;          // physical address of THIS task's page directory
                                // (paging_task_pd(id); loaded into CR3 on
                                // every context switch by the asm hook)
    uint8_t*     stack;        // base of the private kernel stack
    uint32_t     stack_size;
    uint8_t*     arena;        // base of the private workspace
    uint32_t     arena_size;
    uint32_t     arena_used;
    int          timeslice;    // ticks remaining in the current slice
    int          priority;     // 0-255, higher = preferred (SCHED_PRIORITY)
    int          q_next;       // ready-queue link: index of next task, -1 = none
    uint32_t     total_ticks;  // CPU time charged (stats)
    uint32_t     switches;     // context switches into this task (stats)
    uint32_t     wake_tick;    // WAITING tasks wake at >= this tick
    uint32_t     charge_tick;  // tick when the last CPU slice began
    uint32_t     created;      // tick at creation
    int          used;
    // FPU/SSE save area (FXSAVE image, 512 B, 16-byte aligned).  The
    // asm switch hook fxsave's the outgoing task into this and
    // fxrstor's the incoming task's back out, so x87 + XMM registers
    // survive context switches.  This is REQUIRED, not optional: the
    // vesa.c framebuffer blit uses SSE inline asm (movdqu/xmm0-3),
    // so XMM state is genuinely live across PIT interrupts.  Kept
    // at the END so esp stays at offset 0 for the asm hook.
    uint8_t      fpu_area[512] __attribute__((aligned(16)));
} pcb_t;

// ─── Scheduler algorithm selection ───────────────────────────────
// Which task runs next.  SCHED_RR is the default; FCFS and PRIORITY
// are selectable at runtime via sched_set_algorithm() / `sched` shell.
typedef enum {
    SCHED_RR       = 0,   // round-robin: fixed timeslice, tail re-enqueue
    SCHED_FCFS     = 1,   // first-come-first-served: run until it blocks
    SCHED_PRIORITY = 2,   // strict priority (higher `priority` runs first)
} sched_alg_t;

int         sched_set_algorithm(int alg);   // 0 on success, -1 on bad alg
int         sched_algorithm(void);
const char* sched_algorithm_name(void);
// Ready-queue introspection (a real queue of READY tasks waiting for
// CPU — not a table scan).  sched_queue_at(k) returns the task id at
// queue position k (head = 0), or -1.
int         sched_queue_len(void);
int         sched_queue_at(int pos);

// ─── Scheduler API ───────────────────────────────────────────────
void        task_init(void);            // set up the "main" PCB + demo workers
int         task_create(const char* name, void (*entry)(void));
void        task_yield(void);           // cooperative: hand over at next tick
void        task_sleep(int ticks);      // WAITING for `ticks` PIT ticks
void        task_exit(void) __attribute__((noreturn));
void        scheduler_tick(void);       // called from the PIT IRQ (isr.c)

// Per-task priority (0-255, higher = preferred) used by SCHED_PRIORITY.
// Default 128.  Returns 0 on success.  Note: changing a priority while
// the task sits in the queue does not re-sort it until its next insert.
int         task_set_priority(int id, int prio);
int         task_priority(int id);

// ─── User-mode tasks (ring 3) ────────────────────────────────────
// Creates a task whose first switch-in iret's to ring 3: its fake
// frame carries the user code/data selectors (0x18/0x20) plus the
// caller-provided user stack pointer, and its interrupt frame lives
// on the task's private kernel stack (tss.esp0 is set by the
// scheduler on every switch).  The ELF loader (cpu/elfload.c) maps
// the program + stack into the task's private region first, then
// calls this.  Returns the task id or -1.  Slot 1 is reserved for
// user programs (task_create skips it), and the ELF is linked at
// slot 1's region base — see cpu/elfload.c.
int         task_create_user(const char* name, uint32_t entry,
                             uint32_t user_stack_top);

// Mark a task DONE and remove it from the ready queue (used by the
// user-fault path in isr.c to kill just the offending ring-3 task).
void        task_kill_current(void);
// Kill task `id` (must not be the currently running task).
int         task_kill(int id);
// COM1 debug-console write (SYS_WRITE target).  Returns bytes written.
int         task_serial_write(const char* s, int len);

// Per-task unique memory: bump-allocates within the CURRENT task's
// private arena.  NULL if the arena is exhausted (arena_size 1 KiB).
void*       task_alloc(int size);

// Per-task VIRTUAL memory: allocate `pages` isolated private 4 KiB
// pages, mapped ONLY in the calling task's page directory (see
// include/paging.h).  Returns the virtual address in the task's
// private region, or NULL.  A different task dereferencing that
// address faults (#PF) — genuine per-address-space isolation.
void*       task_alloc_page(int pages);

// ─── Internal (asm ↔ C hand-off) ─────────────────────────────────
// Synchronous context switch: task_yield/task_sleep/task_exit call it
// so they actually stop running IMMEDIATELY (the timer IRQ preempts
// only on its own 50 Hz cadence otherwise).  Defined in
// cpu/interrupts.asm; iret's into the next task.
extern void task_switch_asm(void);
// Staged by the asm trampoline: promotes slept tasks, picks the next
// READY task, sets sched_new_esp + sched_current_pcb.  Returns 0 if a
// switch is staged, nonzero if the caller should keep running.
int         scheduler_pick_and_prepare(void);

// ─── Stats / introspection (shell `tasks`, sysmon) ───────────────
int         task_count(void);
const char* task_name(int i);
const char* task_state_name(int i);
uint32_t    task_ticks(int i);
uint32_t    task_switches(int i);
int         task_current_id(void);
int         task_alive(int id);       // slot holds a live task
const char* task_current_name(void);
uint32_t    sched_total_switches(void);
int         sched_ready_count(void);
int         sched_waiting_count(void);
// Renders a compact single-line scheduler summary into buf (max n).
void        sched_snapshot_str(char* buf, int n);
// Cross-task demo counter: bumped once per second by the blinker
// task (single atomic 32-bit int — the only shared task state).
extern uint32_t task_global_blinks;

// ─── Internal globals shared with the asm hook (NOT static) ──────
extern volatile int    sched_switch_pending;  // 1 → asm hook swaps stacks
extern volatile uint32_t sched_new_esp;       // next task's frame pointer
extern volatile pcb_t* sched_current_pcb;     // PCB of the task being preempted
// FPU/SSE areas for the switch: the outgoing task's state is fxsave'd
// into sched_fpu_save and the incoming task's is fxrstor'd from
// sched_fpu_restore (set by the scheduler on every committed switch).
extern volatile uint32_t sched_fpu_save;
extern volatile uint32_t sched_fpu_restore;
// Incoming task's page directory: the asm hook writes it to CR3 after
// switching stacks, giving every task its own address space.
extern volatile uint32_t sched_cr3;

#endif
