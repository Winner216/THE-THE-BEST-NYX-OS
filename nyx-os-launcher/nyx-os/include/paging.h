// include/paging.h  –  Nyx OS virtual memory (paging + isolation)
//
// Before paging_init() the kernel ran identity-mapped (physical ==
// virtual) in one shared address space.  paging_init() enables CR0.PG
// with:
//   • a KERNEL map (k_pd): 0 .. 1 GiB identity-mapped with 4 KiB
//     pages (supervisor, RW) — the kernel image, BSS, framebuffer
//     back buffers and MMIO live there, so every existing pointer
//     stays valid; plus the VESA linear framebuffer(s), which sit
//     ABOVE 1 GiB, as 4 MiB large pages.
//   • PER-TASK address spaces: every task gets its own page directory
//     (task_pds[]), initialised as a copy of the kernel map.  The
//     scheduler loads a task's CR3 on every context switch, so each
//     task sees the shared kernel PLUS its own private pages.
//   • ISOLATION: a task's private pages live in a private region
//     (TASK_PRIVATE_BASE .. +TASK_PRIVATE_SIZE per task) that the
//     kernel map leaves unmapped and that ONLY the owning task's page
//     directory maps.  Reading or writing another task's private
//     memory therefore faults (#PF) instead of silently reading it.
//
// This is per-address-space isolation at ring 0 (tasks stay kernel
// tasks).  Full ring-3 user mode — user code/data segments + iret-to-
// ring3 + the DPL-3 int 0x80 gate — is the documented next step; the
// pieces (user segments in the GDT, DPL-3 gate, PF_U private pages)
// are already in place.

#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>

#define PAGE_SIZE       4096u
#define PD_ENTRIES      1024u
#define PT_ENTRIES      1024u

// Per-task private region (only the owning task's PD maps it).
#define TASK_PRIVATE_BASE   0xC0000000u        // 3 GiB
#define TASK_PRIVATE_SIZE   (16u * 1024 * 1024) // 16 MiB per task
#define TASK_PRIVATE_PAGES  (TASK_PRIVATE_SIZE / PAGE_SIZE)

// Page entry flags
#define PAGING_P    0x001u   // present
#define PAGING_W    0x002u   // writable
#define PAGING_U    0x004u   // user-accessible (ring 3)
#define PAGING_PS   0x080u   // page size (4 MiB large page in a PD entry)

// Kernel page tables (identity-map 0 .. 256 MiB).  Exposed so the
// heap allocator (cpu/heap.c) can map new pages on demand.
extern uint32_t k_pd[1024];
extern uint32_t k_pt[256][1024];

// Mark a range of physical frames as used in the PMM bitmap.
// Called by heap_init() to prevent PMM from handing out heap pages.
void pmm_mark_range(uint32_t start, uint32_t end);

// Enable paging: identity-map the kernel + map the VESA LFB(s), build
// per-task page directories, install the physical allocator, set CR3
// and turn on CR0.PG.  Call ONCE, after vesa_init() (fb_phys = the
// framebuffer physical base, i.e. (uint32_t)vesa.fb) and after the
// IDT is loaded so a fault during setup gets a diagnostic.
void paging_init(uint32_t fb_phys);

// Physical page-directory address for a task (index 0 = the kernel
// template PD used by "main").  The scheduler loads this into CR3 on
// every context switch.
uint32_t paging_task_pd(int task);
uint32_t paging_kernel_pd(void);

// Allocate `pages` contiguous private 4 KiB pages for `task`, mapping
// them ONLY in that task's page directory.  Returns the virtual
// address (in the task's private region) or NULL on failure.
void* paging_alloc_private(int task, int pages);

// Map `pages` fresh frames at an EXPLICIT virtual address `va` inside
// `task`'s private region (used by the ELF loader to place user
// program segments at their link-time vaddr).  NULL on failure.
void* paging_map_user_va(int task, uint32_t va, int pages);

// Like paging_map_user_va, but accepts explicit page-flags so the
// caller can set per-segment permissions:
//   flags = PAGING_W | PAGING_U  → writable + user (default)
//   flags = PAGING_U             → read-only + user (code segments)
//   flags = PAGING_W             → writable + supervisor (kernel only)
// Any combination of PAGING_P | PAGING_W | PAGING_U is accepted;
// PAGING_P is forced on (pages are always present).
void* paging_map_user_va_flags(int task, uint32_t va, int pages, uint32_t flags);

// Change the page-table flags on an already-mapped range in `task`'s
// address space.  Useful for tightening permissions after loading:
//   1. Load everything as RW (relocations need writable code)
//   2. After relocation, call this to mark .text as read-only+user
// The range must be page-aligned and already present.
// Returns 0 on success, -1 on error.
int paging_set_user_va_flags(int task, uint32_t va, int pages, uint32_t flags);

// Identity-map a device's MMIO region (PCI BAR physical address,
// which lives in the 0xF0000000+ PCI hole ABOVE the 1 GiB kernel
// identity map) so its registers are readable/writable.  Allocates
// page tables on demand for the PD entries above entry 255, maps
// 4 KiB pages 1:1 (VA == PA), and mirrors the new PD entries into
// every per-task page directory so the device stays accessible even
// while a task's CR3 is active (e.g. an ISR touching the NIC while
// the scheduler runs a background task).  Safe to call any time
// after paging_init(); a no-op for addresses below 1 GiB that are
// already mapped.
void paging_map_device(uint32_t phys, uint32_t size_bytes);

// ─── Introspection (used by the shell `vmem` command) ────────────
int      paging_priv_count(int task);        // private pages mapped for task
void     paging_task_cleanup(int task);      // free all private frames + reset region
uint32_t paging_priv_va(int task, int k);    // VA of the task's k-th page
// Is `va` mapped (present) in `task`'s page directory?
int      paging_priv_mapped(int task, uint32_t va);
// Count of pages of task t2 visible in a DIFFERENT task t1's PD.
// Must be 0 for isolation to hold.
int      paging_cross_mappings(void);

#endif
