// cpu/task.c  –  Nyx OS multitasking: PCB management + round-robin
// scheduler, driven by the PIT timer IRQ (scheduler_tick, called from
// irq_handler in isr.c).  See include/task.h for the design notes and
// the SAFETY CONTRACT for background tasks.
#include <task.h>
#include <isr.h>
#include <paging.h>
#include <syscall.h>
#include <gdt.h>
#include <elfload.h>
#include <terminal.h>
#include <gamesnd.h>
#include <stdint.h>
#include <stddef.h>
#include <io.h>

// ─── Global task table & scheduler state ─────────────────────────
static pcb_t    tasks[TASK_MAX];
static pcb_t*   g_current;                 // task currently executing
// 16-byte alignment: the fake interrupt frames built in
// task_create_slot push uint32_t values onto the stack top, and an
// aligned base keeps ESP 4-aligned throughout (matching what the
// IRQ stub's pusha/pop always produce on a real interrupt).
static uint8_t  task_stacks[TASK_MAX][TASK_STACK_SIZE] __attribute__((aligned(16)));
static uint8_t  task_arenas[TASK_MAX][TASK_ARENA_SIZE] __attribute__((aligned(16)));

// Demo-task COM1 logging is verification scaffolding.  Define to 0
// in production builds to silence the background UART chatter while
// keeping the demo tasks' computation untouched.
#define TASK_DEBUG_COM1 1
static int      sched_enabled = 0;
static uint32_t sched_switches_total = 0;
static int      sched_task_count = 0;

// Shared with the asm hook in cpu/interrupts.asm (must NOT be static).
volatile int          sched_switch_pending = 0;
volatile uint32_t     sched_new_esp       = 0;
volatile pcb_t*       sched_current_pcb   = NULL;
// FPU/SSE save areas for the asm hook: the outgoing task's x87/XMM
// state is fxsave'd into sched_fpu_save and the incoming task's is
// fxrstor'd back from sched_fpu_restore on every committed switch
// (set here in the scheduler alongside sched_current_pcb/sched_new_esp).
volatile uint32_t     sched_fpu_save      = 0;
volatile uint32_t     sched_fpu_restore   = 0;
// Incoming task's page directory — the asm hook writes it to CR3
// right after switching stacks, so each task resumes in its OWN
// address space (see include/paging.h).  Set on every committed
// switch alongside sched_new_esp.
volatile uint32_t     sched_cr3           = 0;

// Demo background tasks (pure computation; defined near the bottom).
static void task_worker_main(void);
static void task_blinker_main(void);
static void task_child_main(void);

// COM1 debug log helpers (defined below task_init).
static void tsk_serial_init(void);
static void tsk_serial_str(const char* s);
static void tsk_serial_dec(uint32_t n);
static void tsk_serial_putc(char c);

// ─── User-mode task slot ─────────────────────────────────────────
// Slot TASK_USER_SLOT (1) is RESERVED for the ring-3 user program:
// its ELF is linked at that slot's private-region base (elfload.h),
// and the ELF loader maps the program there before
// task_create_user().  Kernel tasks (task_create) skip the slot so it
// is always free for the `user` shell command.

// ─── Small helpers ───────────────────────────────────────────────
static int state_str(char* out, task_state_t s) {
    const char* n = (s == TASK_READY)   ? "R"
                  : (s == TASK_RUNNING) ? "N"
                  : (s == TASK_WAITING) ? "W"
                  : "X";                  // DONE
    out[0] = n[0]; out[1] = '\0';
    return 0;
}

// ─── Ready queue ─────────────────────────────────────────────────
// A real queue of active tasks waiting for CPU time: an index-linked
// list threaded through pcb_t.q_next over the static tasks[] table
// (indices, not pointers — no allocator, no pointer churn).  Invariants:
//   • a task is in the queue IFF it is READY and NOT currently running
//   • the RUNNING task (g_current) is never in the queue
//   • WAITING (sleeping) and DONE (exited) tasks are never in it
// SCHED_PRIORITY keeps the queue sorted (highest priority at the
// head) by inserting with rq_insert_priority; RR/FCFS use plain tail
// enqueue, so the head is always the next task to run.
static int      sched_alg  = SCHED_RR;
static int      rq_head    = -1;
static int      rq_tail    = -1;
static int      rq_len     = 0;

static int rq_contains(int idx) {
    for (int i = rq_head; i != -1; i = tasks[i].q_next)
        if (i == idx) return 1;
    return 0;
}

static void rq_enqueue(int idx) {
    if (idx < 0 || idx >= TASK_MAX || !tasks[idx].used) return;
    if (rq_contains(idx)) return;              // already queued
    tasks[idx].q_next = -1;
    if (rq_tail == -1) rq_head = idx; else tasks[rq_tail].q_next = idx;
    rq_tail = idx;
    rq_len++;
}

static void rq_insert_priority(int idx) {      // keep sorted, highest first
    if (idx < 0 || idx >= TASK_MAX || !tasks[idx].used) return;
    if (rq_contains(idx)) return;
    int prev = -1, cur = rq_head;
    while (cur != -1 && tasks[cur].priority >= tasks[idx].priority) {
        prev = cur;
        cur  = tasks[cur].q_next;
    }
    tasks[idx].q_next = cur;                   // equal priorities keep FIFO
    if (prev == -1) rq_head = idx; else tasks[prev].q_next = idx;
    if (cur == -1) rq_tail = idx;
    rq_len++;
}

static int rq_dequeue(void) {                  // pop the head
    if (rq_head == -1) return -1;
    int idx = rq_head;
    rq_head = tasks[idx].q_next;
    if (rq_head == -1) rq_tail = -1;
    tasks[idx].q_next = -1;
    rq_len--;
    return idx;
}

static void rq_remove(int idx) {               // defensive: drop if queued
    if (rq_head == -1) return;
    if (rq_head == idx) {
        rq_head = tasks[idx].q_next;
        if (rq_head == -1) rq_tail = -1;
        tasks[idx].q_next = -1;
        rq_len--;
        return;
    }
    int prev = rq_head;
    while (tasks[prev].q_next != -1 && tasks[prev].q_next != idx)
        prev = tasks[prev].q_next;
    if (tasks[prev].q_next == idx) {
        tasks[prev].q_next = tasks[idx].q_next;
        if (tasks[prev].q_next == -1) rq_tail = prev;
        tasks[idx].q_next = -1;
        rq_len--;
    }
}

// ─── Pick the next task (READY queue only) ───────────────────────
static pcb_t* pick_next(void) {
    // Head of the queue IS the next task for every algorithm: FIFO
    // for RR/FCFS, and PRIORITY keeps the queue pre-sorted so its
    // head is the highest-priority READY task.
    int idx = rq_dequeue();
    return (idx >= 0) ? &tasks[idx] : NULL;
}

// Wake sleeping tasks whose interval elapsed and put them back into
// the ready queue (sorted by priority in PRIORITY mode, tail in RR).
static void promote_waiting(void) {
    for (int i = 0; i < TASK_MAX; i++) {
        pcb_t* t = &tasks[i];
        if (t->used && t->state == TASK_WAITING &&
            (int)(tick - t->wake_tick) >= 0) {
            t->state = TASK_READY;
            if (sched_alg == SCHED_PRIORITY) rq_insert_priority(i);
            else rq_enqueue(i);
        }
    }
}

// Re-queue the task that is handing over the CPU (preempted or
// yielded) according to the active algorithm.
static void requeue_current(void) {
    if (g_current->state == TASK_WAITING || g_current->state == TASK_DONE)
        return;                                // slept / exited — not READY
    if (sched_alg == SCHED_PRIORITY) rq_insert_priority(g_current->id);
    else rq_enqueue(g_current->id);
}

// ─── Scheduler entry from the PIT IRQ ────────────────────────────
void scheduler_tick(void) {
    if (!sched_enabled || !g_current) return;

    // Charge CPU time honestly: the task that ran the whole interval
    // since its slice began gets every tick (normally 1, since
    // scheduler_tick fires each PIT tick).  On a commit below the
    // incoming task's charge_tick is re-anchored to `tick`.
    g_current->total_ticks += (uint32_t)(tick - g_current->charge_tick);
    g_current->charge_tick  = tick;

    promote_waiting();   // slept tasks re-enter the ready queue

    // FCFS: no timeslice preemption — a task runs until it blocks
    // (task_sleep / task_yield / task_exit).  Only RR and PRIORITY
    // hand the CPU over when the slice expires.
    if (sched_alg == SCHED_FCFS) return;

    // Not time to switch yet: keep running (or stay idle).
    if (g_current->timeslice > 0) {
        g_current->timeslice--;
        return;
    }

    // Re-queue the current task FIRST, then pick: in PRIORITY mode a
    // still-highest-priority task lands back at the head and is picked
    // again immediately (strict priority — never handed down a slice);
    // in RR it goes to the tail, giving everyone a turn.
    requeue_current();
    pcb_t* next = pick_next();                 // pop the ready-queue head
    if (!next || next == g_current) {
        // Nothing else is READY (or PRIORITY re-picked us) — reset the
        // slice and keep running.
        if (g_current->timeslice <= 0) g_current->timeslice = TASK_TIMESLICE;
        return;
    }

    // Commit the switch: the asm hook (interrupts.asm) will fxsave
    // the CURRENT task's FPU/SSE state, save its frame into
    // [sched_current_pcb] (offset 0 = esp), load sched_new_esp and
    // fxrstor the next task's FPU/SSE state, then iret into it.
    sched_new_esp     = next->esp;
    sched_current_pcb = g_current;
    sched_fpu_save    = (uint32_t)g_current->fpu_area;
    sched_fpu_restore = (uint32_t)next->fpu_area;
    sched_cr3         = next->cr3;      // load next task's page directory
    // If the incoming task is (or will be) a ring-3 task, the next
    // interrupt/syscall/fault from it needs ITS kernel stack — the CPU
    // reads this from the TSS.
    tss_set_esp0(next->stack + next->stack_size);
    sched_switch_pending = 1;

    g_current         = next;
    g_current->state  = TASK_RUNNING;
    g_current->timeslice = TASK_TIMESLICE;
    g_current->charge_tick = tick;
    g_current->switches++;
    sched_switches_total++;
}

// ─── Synchronous switch staging (task_yield/sleep/exit) ──────────
// Called from task_switch_asm (interrupts.asm) with IF cleared.
// Returns 0 if a switch to another task was staged, nonzero if the
// caller should simply keep running.
int scheduler_pick_and_prepare(void) {
    promote_waiting();   // slept tasks re-enter the ready queue

    requeue_current();   // yield → back to the queue; sleep/exit → not
    pcb_t* next = pick_next();                 // pop the ready-queue head
    if (!next || next == g_current) {
        // Nothing else READY (or PRIORITY re-picked the yielder) —
        // keep running.
        if (g_current->timeslice <= 0) g_current->timeslice = TASK_TIMESLICE;
        return 1;   // keep running
    }

    // Charge the outgoing task for the (possibly partial) slice it
    // consumed since its last PIT charge — synchronous switches land
    // BETWEEN ticks, and that CPU time belongs to the old task.
    g_current->total_ticks += (uint32_t)(tick - g_current->charge_tick);

    sched_new_esp     = next->esp;
    sched_current_pcb = g_current;   // the asm saves OUR frame into this
    sched_fpu_save    = (uint32_t)g_current->fpu_area;   // fxsave target
    sched_fpu_restore = (uint32_t)next->fpu_area;        // fxrstor source
    sched_cr3         = next->cr3;      // load next task's page directory
    tss_set_esp0(next->stack + next->stack_size);
    g_current         = next;
    g_current->state  = TASK_RUNNING;
    g_current->timeslice = TASK_TIMESLICE;
    g_current->charge_tick = tick;
    g_current->switches++;
    sched_switches_total++;
    return 0;   // switch staged
}

// ─── Task lifecycle ──────────────────────────────────────────────
// Builds a fabricated interrupt frame at the top of a fresh stack so
// that the very first switch into the task "returns" from an interrupt
// into entry().  Layout must match irq_common_stub's unwinding:
//
//   [esp+ 0] ds      (popped by `pop eax; mov ds,ax`)
//   [esp+ 4] edi     ┐
//   [esp+ 8] esi     │
//   [esp+12] ebp     │
//   [esp+16] esp     │  popped by `popa`  (value = &int_no so the
//   [esp+20] ebx     │                  add esp,8 below skips to eip)
//   [esp+24] edx     │
//   [esp+28] ecx     │
//   [esp+32] eax     ┘
//   [esp+36] int_no  ┐ skipped by `add esp, 8`
//   [esp+40] err     ┘
//   [esp+44] eflags  ┐
//   [esp+48] cs      │ popped by `iret`
//   [esp+52] eip     ┘  ← entry()'s program counter
static int task_create_slot(const char* name, void (*entry)(void), int idx) {
    pcb_t* t = &tasks[idx];
    uint8_t* stack = task_stacks[idx];

    // Seed the FPU/SSE save area with the CURRENT (known-good) FPU
    // state so the first fxrstor into it on switch-in loads a valid
    // x87/XMM image instead of garbage.  "+m" declares the write.
    asm volatile("fxsave %0" : "+m" (t->fpu_area[0]) : : "memory");

    // Top of the private stack, then push the fake frame downward.
    uint32_t* f = (uint32_t*)(stack + TASK_STACK_SIZE);
    *--f = 0x202;                    // eflags: IF set (interruptible)
    *--f = 0x08;                     // cs: kernel code segment
    *--f = (uint32_t)entry;          // eip: the program counter
    *--f = 0;                        // err
    *--f = 0;                        // int_no
    uint32_t esp_slot = (uint32_t)f; // pusha's saved ESP = &int_no
    *--f = 0;                        // edi
    *--f = 0;                        // esi
    *--f = 0;                        // ebp
    *--f = esp_slot;                 // esp
    *--f = 0;                        // ebx
    *--f = 0;                        // edx
    *--f = 0;                        // ecx
    *--f = 0;                        // eax
    *--f = 0x10;                     // ds: kernel data segment
    // (the 4 bytes above *--f are the ds slot the stub pops first)

    t->esp        = (uint32_t)f;
    t->id         = idx;
    t->name       = name;
    t->state      = TASK_READY;
    t->entry      = (uint32_t)entry;
    t->stack      = stack;
    t->stack_size = TASK_STACK_SIZE;
    t->arena      = task_arenas[idx];
    t->arena_size = TASK_ARENA_SIZE;
    t->arena_used = 0;
    t->timeslice  = TASK_TIMESLICE;
    t->priority   = 128;          // neutral default for SCHED_PRIORITY
    t->q_next     = -1;           // not yet queued
    t->total_ticks = 0;
    t->switches   = 0;
    t->wake_tick  = 0;
    t->charge_tick = tick;
    t->created    = tick;
    t->used       = 1;

    // Each task gets its own page directory (a copy of the kernel map
    // plus its private region) — switched in via CR3 on every context
    // switch, giving every task its own virtual address space.
    t->cr3 = paging_task_pd(idx);

    // The task's fake frame makes esp valid immediately, so it can be
    // switched into right away: put it in the ready queue (tail for
    // RR/FCFS, priority-sorted for PRIORITY — keeping the invariant
    // that the queue head is always the next task to run).
    if (sched_alg == SCHED_PRIORITY) rq_insert_priority(idx);
    else rq_enqueue(idx);
    return idx;
}

int task_create(const char* name, void (*entry)(void)) {
    if (!sched_enabled) return -1;
    for (int i = 2; i < TASK_MAX; i++) {     // slot 1 reserved for user mode
        if (tasks[i].used) continue;
        task_create_slot(name, entry, i);
        sched_task_count++;
        return i;
    }
    return -1;   // table full
}

// ─── Ring-3 user task ────────────────────────────────────────────
// Same fabricated-frame technique as task_create_slot, but the frame
// ends with the CPU-pushed user portion so the very first iret
// PRIVILEGE-CHANGES into ring 3 (user CS 0x1B / SS 0x23):
//
//   [esp+ 0] ds = 0x23 (user data, RPL 3)
//   [esp+ 4] eax ┐
//   [esp+ 8] ecx │  popa — esp slot = &int_no (see task_create_slot)
//   ...          │
//   [esp+36] int_no ┐ skipped by add esp,8
//   [esp+40] err    ┘
//   [esp+44] eip  ┐ iret pops these, sees cs.RPL=3, and ALSO pops
//   [esp+48] cs   │ the user ESP/SS → ring 3, user stack active.
//   [esp+52] eflags┘
//   [esp+56] useresp ┐ popped by the privilege-changing iret
//   [esp+60] userss  ┘
//
// The selector values carry RPL 3 (0x18|3 = 0x1B, 0x20|3 = 0x23): a
// selector with RPL 0 would make iret treat this as a same-privilege
// return and #GP because the user descriptors have DPL 3.
//
// The frame lives on the task's private KERNEL stack (task_stacks),
// which tss.esp0 points at whenever this task is current — so every
// later interrupt/syscall/fault from ring 3 lands on that same stack.
static int task_create_user_slot(const char* name, uint32_t entry,
                                 uint32_t user_stack_top, int idx) {
    pcb_t* t = &tasks[idx];
    uint8_t* stack = task_stacks[idx];

    asm volatile("fxsave %0" : "+m" (t->fpu_area[0]) : : "memory");

    uint32_t* f = (uint32_t*)(stack + TASK_STACK_SIZE);
    *--f = 0x23;                    // userss: user data segment, RPL 3
    *--f = user_stack_top;          // useresp: top of the user stack
    *--f = 0x202;                   // eflags: IF set, IOPL 0 (no I/O)
    *--f = 0x1B;                    // cs: user code segment, RPL 3
    *--f = entry;                   // eip: the user program's entry point
    *--f = 0;                       // err
    *--f = 0;                       // int_no
    uint32_t esp_slot = (uint32_t)f; // pusha's saved ESP = &int_no
    *--f = 0;                       // edi
    *--f = 0;                       // esi
    *--f = 0;                       // ebp
    *--f = esp_slot;                // esp
    *--f = 0;                       // ebx
    *--f = 0;                       // edx
    *--f = 0;                       // ecx
    *--f = 0;                       // eax
    *--f = 0x23;                    // ds: user data segment, RPL 3

    t->esp        = (uint32_t)f;
    t->id         = idx;
    t->name       = name;
    t->state      = TASK_READY;
    t->entry      = entry;
    t->stack      = stack;
    t->stack_size = TASK_STACK_SIZE;
    t->arena      = task_arenas[idx];
    t->arena_size = TASK_ARENA_SIZE;
    t->arena_used = 0;
    t->timeslice  = TASK_TIMESLICE;
    t->priority   = 128;
    t->q_next     = -1;
    t->total_ticks = 0;
    t->switches   = 0;
    t->wake_tick  = 0;
    t->charge_tick = tick;
    t->created    = tick;
    t->used       = 1;

    t->cr3 = paging_task_pd(idx);

    if (sched_alg == SCHED_PRIORITY) rq_insert_priority(idx);
    else rq_enqueue(idx);
    return idx;
}

int task_create_user(const char* name, uint32_t entry, uint32_t user_stack_top) {
    if (!sched_enabled) return -1;
    if (tasks[TASK_USER_SLOT].used) return -1;     // already running
    task_create_user_slot(name, entry, user_stack_top, TASK_USER_SLOT);
    sched_task_count++;
    return TASK_USER_SLOT;
}

// Mark the CURRENT task DONE without switching (the user-fault path in
// isr.c calls this, then scheduler_pick_and_prepare() stages the switch
// away).  A DONE task is never requeued or picked again; its slot is
// freed so `user` can be relaunched after a crash.
void task_kill_current(void) {
    if (!g_current) return;
    gamesnd_task_exit(g_current->id);       // free game-sfx cache
    paging_task_cleanup(g_current->id);     // free private frames + reset region
    g_current->state = TASK_DONE;
    g_current->timeslice = 0;
    rq_remove(g_current->id);
    g_current->used = 0;               // free the slot (user relaunch)
}

// Kill task `id`.  Killing the CURRENT task is allowed (the [X]
// close runs in the mouse IRQ, which usually interrupts the game
// task itself): the victim is marked DONE and simply never picked
// again, mirroring the fault-path kill.  Returns 0 on success.
int task_kill(int id) {
    if (id <= 0 || id >= TASK_MAX || !tasks[id].used) return -1;
    if (g_current && g_current->id == id) {
        // Killing the CURRENT task — legal from an IRQ (the [X] close
        // runs in the mouse IRQ and usually interrupts the game task
        // itself) and from the task itself.  Do exactly what the
        // fault-path kill does: mark DONE, drop from the queue, free
        // the slot.  The victim finishes its current timeslice, then
        // the scheduler never picks it again (scheduler explicitly
        // skips a DONE g_current).
        gamesnd_task_exit(id);              // free game-sfx cache
        paging_task_cleanup(id);            // free private frames + reset region
        tasks[id].state = TASK_DONE;
        tasks[id].timeslice = 0;
        rq_remove(id);
        tasks[id].used = 0;
        return 0;
    }
    gamesnd_task_exit(id);                  // free game-sfx cache
    paging_task_cleanup(id);            // free private frames + reset region
    tasks[id].state = TASK_DONE;
    tasks[id].timeslice = 0;
    rq_remove(id);
    tasks[id].used = 0;                // free the slot (user relaunch)
    return 0;
}

// COM1 write (SYS_WRITE target): a ring-3 task's only output channel.
// Output from the ring-3 user task is ALSO mirrored into the on-screen
// Terminal window (term_buf redirect) so a launched store app is
// visible on the desktop, not just on COM1.  Kernel tasks (demo
// chatter etc.) stay COM1-only so they don't pollute the console.
int task_serial_write(const char* s, int len) {
    int n = 0;
    if (!s) return 0;
    int mirror_user = (g_current && g_current->id == TASK_USER_SLOT);
    for (int i = 0; i < len && s[i]; i++) {
        tsk_serial_putc(s[i]);
        if (mirror_user) terminal_putchar(s[i]);
        n++;
    }
    return n;
}

// Cooperative yield — switches away IMMEDIATELY (synchronous, via
// task_switch_asm).  If no other task is ready, execution continues.
void task_yield(void) {
    if (!sched_enabled || !g_current) return;
    task_switch_asm();
}

// Sleep for `ticks` PIT ticks (1 tick = 20 ms).  Marks this task
// WAITING and switches away immediately; the scheduler promotes it
// back to READY once tick >= wake_tick.
void task_sleep(int ticks) {
    if (!sched_enabled || !g_current) return;
    if (ticks < 1) ticks = 1;
    g_current->state = TASK_WAITING;
    g_current->wake_tick = tick + (uint32_t)ticks;
    task_switch_asm();   // stop running NOW; return after the sleep ends
}

void task_exit(void) {
    if (g_current) {
        gamesnd_task_exit(g_current->id);   // free game-sfx cache
        paging_task_cleanup(g_current->id); // free private frames + reset region
        g_current->state = TASK_DONE;
        g_current->timeslice = 0;
        rq_remove(g_current->id);   // defensive: never pick a DONE task
        g_current->used = 0;        // free the slot (user relaunch)
    }
    task_switch_asm();   // hand off; the scheduler never switches back in
    for (;;) { }         // safety net if nothing else was runnable
}

// Per-task unique memory: bump-alloc in the CURRENT task's private arena.
void* task_alloc(int size) {
    if (!g_current || size < 1) return NULL;
    pcb_t* t = g_current;
    if (t->arena_used + (uint32_t)size > t->arena_size) return NULL;
    void* p = t->arena + t->arena_used;
    t->arena_used += (uint32_t)size;
    return p;
}

// Per-task VIRTUAL memory: isolated private pages mapped only in this
// task's page directory (paging.h).  Cross-task access to the returned
// address faults — real address-space isolation.
void* task_alloc_page(int pages) {
    if (!g_current || pages < 1) return NULL;
    void* p = paging_alloc_private(g_current->id, pages);
    if (!p) {
        // E2E/diagnostic hook: why the zone allocation failed.
        const char* m = "ALLOC_PAGE FAIL pages=";
        for (; *m; m++) { int sp=0; while((inb(0x3F8+5)&0x20)==0&&sp++<200000){} outb(0x3F8,*m); }
        int v = pages; char b[12]; int i = 0;
        if (!v) b[i++]='0';
        while (v) { b[i++] = '0'+v%10; v/=10; }
        while (i--) { int sp=0; while((inb(0x3F8+5)&0x20)==0&&sp++<200000){} outb(0x3F8,b[i]); }
        m = " priv=";
        for (; *m; m++) { int sp=0; while((inb(0x3F8+5)&0x20)==0&&sp++<200000){} outb(0x3F8,*m); }
        v = paging_priv_count(g_current->id); i = 0;
        if (!v) b[i++]='0';
        while (v) { b[i++] = '0'+v%10; v/=10; }
        while (i--) { int sp=0; while((inb(0x3F8+5)&0x20)==0&&sp++<200000){} outb(0x3F8,b[i]); }
        m = "\n";
        for (; *m; m++) { int sp=0; while((inb(0x3F8+5)&0x20)==0&&sp++<200000){} outb(0x3F8,*m); }
    }
    return p;
}

// ─── Init: the kernel's own loop is task 0 ("main") ──────────────
void task_init(void) {
    if (sched_enabled) return;
    // The "main" PCB is registered but NOT given a fake frame: its esp
    // becomes valid the first time the asm hook preempts it.  pick_next
    // skips tasks with esp == 0, so main can't be switched into before
    // that first save.
    pcb_t* m = &tasks[0];
    m->used  = 1;
    m->id    = 0;
    m->name  = "main";
    m->state = TASK_READY;
    m->esp   = 0;                  // becomes valid at the first preemption
    m->timeslice = TASK_TIMESLICE;
    m->priority  = 128;
    m->q_next    = -1;
    m->cr3    = paging_task_pd(0); // main runs on the kernel template PD
    m->stack  = task_stacks[0];    // keep tss.esp0 valid on every commit
    m->stack_size = TASK_STACK_SIZE;
    m->charge_tick = tick;
    m->created   = tick;
    // NOTE: main is NOT enqueued yet — its esp is 0 until the first
    // PIT preemption saves its frame, and switching into an esp==0
    // task would crash.  requeue_current() puts it in the queue the
    // moment it is genuinely preempted, so it is only ever picked
    // with a valid saved frame.
    // Seed main's FPU/SSE save area with the current valid state.
    asm volatile("fxsave %0" : "+m" (m->fpu_area[0]) : : "memory");
    g_current = m;
    sched_task_count = 1;

    sched_enabled = 1;

    // Demo background tasks — pure computation, no I/O, no shared
    // globals beyond their own arenas + the task API.
    tsk_serial_init();
    tsk_serial_str("MT scheduler up: main+worker+blinker\n");
    int wid = task_create("worker",   task_worker_main);
    int bid = task_create("blinker",  task_blinker_main);
    // Distinct priorities so SCHED_PRIORITY has a visible effect:
    // worker (200) outranks blinker (100) and main (128).
    if (wid >= 0) task_set_priority(wid, 200);
    if (bid >= 0) task_set_priority(bid, 100);
}

// ─── Demo background tasks ───────────────────────────────────────
// ── COM1 debug log (verification only) ───────────────────────────
// The scheduler's demo tasks log their progress over COM1 (captured
// by QEMU with -serial file:...).  Watching both tasks' counters grow
// in the log is unambiguous proof of preemptive multitasking.  This
// is debug scaffolding; the real feature is the scheduler itself.
#if TASK_DEBUG_COM1
static void tsk_serial_init(void) {
    outb(0x3F8 + 1, 0x00);   // disable UART interrupts
    outb(0x3F8 + 3, 0x80);   // DLAB on
    outb(0x3F8 + 0, 0x03);   // divisor 3 → 38400 baud
    outb(0x3F8 + 1, 0x00);
    outb(0x3F8 + 3, 0x03);   // 8N1
    outb(0x3F8 + 2, 0xC7);   // enable + clear FIFOs
}
static void tsk_serial_putc(char c) {
    while ((inb(0x3F8 + 5) & 0x20) == 0) { }   // wait TX ready
    outb(0x3F8, (uint8_t)c);
}
static void tsk_serial_str(const char* s) { while (*s) tsk_serial_putc(*s++); }
static void tsk_serial_dec(uint32_t n) {
    char tmp[12]; int p = 0;
    if (n == 0) { tsk_serial_putc('0'); return; }
    while (n > 0) { tmp[p++] = (char)('0' + n % 10); n /= 10; }
    while (p > 0) tsk_serial_putc(tmp[--p]);
}
#else
static void tsk_serial_init(void) { }
static void tsk_serial_str(const char* s) { (void)s; }
static void tsk_serial_dec(uint32_t n) { (void)n; }
#endif

// Busy worker: burns CPU in its ISOLATED private page (mapped only in
// its own page directory) and occasionally sleeps a tick via the INT
// 0x80 syscall — the task requests kernel services the way a process
// would.  `count` is a plain local; the value is written back into the
// task's private page each iteration to exercise per-task memory.
static void task_worker_main(void) {
    uint32_t* slot = (uint32_t*)sys_alloc_page(1);   // isolated page
    uint32_t count = 0;
    uint32_t hits  = 0;       // mask-hit counter (phase-safe gate)
    uint32_t seed = 0x13579BDFu;
    for (;;) {
        seed = seed * 1664525u + 1013904223u;
        count++;
        if (slot) *slot = count;             // isolated page write-back
        // Every ~200k iterations (~a few ms) hand the CPU back.
        if ((seed & 0x3FFFFu) == 0) {
            hits++;
            if ((hits & 0x3Fu) == 0) {       // every 64th sleep (~4 s)
                tsk_serial_str("WORK pid=");
                tsk_serial_dec((uint32_t)sys_getpid());
                tsk_serial_str(" count=");
                tsk_serial_dec(count);
                tsk_serial_str(" tick=");
                tsk_serial_dec(tick);
                tsk_serial_str("\n");
            }
            sys_sleep(1);
        }
    }
}

// Child demo task: spawned via SYS_CREATE, terminates via SYS_EXIT —
// proves a process can create a new task and exit through syscalls.
static void task_child_main(void) {
    tsk_serial_str("CHILD pid=");
    tsk_serial_dec((uint32_t)sys_getpid());
    tsk_serial_str(" exiting via sys_exit\n");
    sys_exit();                     // never returns
}

// Blinker: sleeps for 50 ticks (1 s) via syscall, then bumps a counter
// in its OWN isolated private page — cross-task shared state is gone;
// the only kernel-visible signal is the COM1 log line.  Spawns one
// child task (SYS_CREATE) on its first blink to exercise that path.
uint32_t task_global_blinks = 0;
static void task_blinker_main(void) {
    uint32_t* blinks = (uint32_t*)sys_alloc_page(1);   // isolated page
    int child_spawned = 0;
    for (;;) {
        sys_sleep(50);          // ~1 s at 50 Hz
        if (blinks) *blinks += 1;
        task_global_blinks = (blinks) ? *blinks : 0;
        if (!child_spawned) {
            child_spawned = 1;
            int cid = sys_create("child", task_child_main);
            tsk_serial_str("BLINK spawned child pid=");
            tsk_serial_dec((uint32_t)cid);
            tsk_serial_str("\n");
        }
        if ((task_global_blinks & 0x7u) == 0) {   // every 8th blink
            tsk_serial_str("BLINK pid=");
            tsk_serial_dec((uint32_t)sys_getpid());
            tsk_serial_str(" n=");
            tsk_serial_dec(task_global_blinks);
            tsk_serial_str(" tick=");
            tsk_serial_dec(tick);
            tsk_serial_str("\n");
        }
    }
}

// ─── Introspection ───────────────────────────────────────────────
int task_count(void) { return sched_task_count; }
const char* task_name(int i) {
    return (i >= 0 && i < TASK_MAX && tasks[i].used) ? tasks[i].name : "-";
}
const char* task_state_name(int i) {
    if (i < 0 || i >= TASK_MAX || !tasks[i].used) return "-";
    return (tasks[i].state == TASK_READY)   ? "READY"
         : (tasks[i].state == TASK_RUNNING) ? "RUNNING"
         : (tasks[i].state == TASK_WAITING) ? "WAITING"
         : "DONE";
}
uint32_t task_ticks(int i) {
    return (i >= 0 && i < TASK_MAX && tasks[i].used) ? tasks[i].total_ticks : 0;
}
uint32_t task_switches(int i) {
    return (i >= 0 && i < TASK_MAX && tasks[i].used) ? tasks[i].switches : 0;
}
int task_current_id(void) { return (g_current) ? g_current->id : -1; }

// True when slot `id` holds a live (runnable) task.  Used by game.c to
// detect a stale game-owner claim after the game task died via a path
// that skipped game cleanup (crash → user-fault kill).
int task_alive(int id) {
    return (id >= 0 && id < TASK_MAX && tasks[id].used &&
            tasks[id].state != TASK_DONE);
}
const char* task_current_name(void) { return (g_current) ? g_current->name : "-"; }
uint32_t sched_total_switches(void) { return sched_switches_total; }

int sched_ready_count(void) {
    // The queue holds every READY-but-not-running task; add the one
    // currently RUNNING for parity with the old table-scan semantics.
    return rq_len + (sched_enabled && g_current ? 1 : 0);
}
int sched_waiting_count(void) {
    int n = 0;
    for (int i = 0; i < TASK_MAX; i++)
        if (tasks[i].used && tasks[i].state == TASK_WAITING) n++;
    return n;
}

// ─── Algorithm selection & ready-queue introspection ─────────────
int sched_set_algorithm(int alg) {
    if (alg < SCHED_RR || alg > SCHED_PRIORITY) return -1;
    sched_alg = alg;
    return 0;
}
int sched_algorithm(void) { return sched_alg; }
const char* sched_algorithm_name(void) {
    return sched_alg == SCHED_RR       ? "round-robin"
         : sched_alg == SCHED_FCFS     ? "fcfs"
         : "priority";
}
int sched_queue_len(void) { return rq_len; }
int sched_queue_at(int pos) {
    int i = rq_head;
    for (int k = 0; i != -1 && k < pos; k++) i = tasks[i].q_next;
    return i;
}
int task_set_priority(int id, int prio) {
    if (id < 0 || id >= TASK_MAX || !tasks[id].used) return -1;
    if (prio < 0) prio = 0;
    if (prio > 255) prio = 255;
    tasks[id].priority = prio;
    return 0;
}
int task_priority(int id) {
    return (id >= 0 && id < TASK_MAX && tasks[id].used) ? tasks[id].priority : -1;
}

static void u2s(uint32_t v, char* out) {
    if (v == 0) { out[0] = '0'; out[1] = 0; return; }
    char tmp[12]; int p = 0;
    while (v > 0) { tmp[p++] = (char)('0' + v % 10); v /= 10; }
    for (int i = 0; i < p; i++) out[i] = tmp[p - 1 - i];
    out[p] = 0;
}

// e.g. "Sched 3 tasks sw=42 cur=main [main R][worker R][blinker W]"
void sched_snapshot_str(char* buf, int n) {
    if (!buf || n < 4) return;
    int p = 0;
    const char* head = "Sched ";
    while (*head && p < n - 1) buf[p++] = *head++;
    char num[12]; u2s((uint32_t)task_count(), num);
    for (int i = 0; num[i] && p < n - 1; i++) buf[p++] = num[i];
    const char* mid = " tasks sw=";
    while (*mid && p < n - 1) buf[p++] = *mid++;
    u2s(sched_total_switches(), num);
    for (int i = 0; num[i] && p < n - 1; i++) buf[p++] = num[i];
    const char* cur = " cur=";
    while (*cur && p < n - 1) buf[p++] = *cur++;
    const char* cn = task_current_name();
    while (*cn && p < n - 1) buf[p++] = *cn++;
    for (int i = 0; i < TASK_MAX && p < n - 2; i++) {
        if (!tasks[i].used) continue;
        buf[p++] = ' ';
        buf[p++] = '[';
        const char* tn = tasks[i].name;
        while (*tn && p < n - 4) buf[p++] = *tn++;
        buf[p++] = ' ';
        char st[2];
        state_str(st, tasks[i].state);
        buf[p++] = st[0];
        buf[p++] = ']';
    }
    buf[p] = 0;
}
