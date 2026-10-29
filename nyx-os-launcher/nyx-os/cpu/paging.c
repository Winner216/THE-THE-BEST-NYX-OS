// cpu/paging.c  –  Nyx OS virtual memory: PMM + page tables + per-task
// address spaces.  See include/paging.h for the design.
#include <paging.h>
#include <task.h>
#include <stdint.h>
#include <stddef.h>

// End of the kernel image (linker.ld) — everything below this address
// is kernel text/data/BSS and is marked used by the physical allocator.
extern char _end[];

// ─── Physical memory manager ─────────────────────────────────────
// Bitmap over the low 1 GiB of guest RAM (262144 frames = 32 KiB).
// The heap lives in 16..272 MiB (identity-mapped) and reserves its full
// 256 MiB window up front, so the PMM must cover the whole 1 GiB QEMU
// gives us — with only 256 MiB tracked there would be zero free frames
// left for task private pages.  The VESA LFB(s) live at 0xE0000000+
// (mapped separately via large pages).
#define PMM_RAM_TOP     0x40000000u   // 1 GiB
#define PMM_FRAMES      (PMM_RAM_TOP / PAGE_SIZE)

static uint8_t pmm_bitmap[PMM_FRAMES / 8] __attribute__((aligned(4096)));

static int pmm_used(uint32_t frame) {
    if (frame >= PMM_FRAMES) return 1;
    return (pmm_bitmap[frame >> 3] >> (frame & 7)) & 1;
}
static void pmm_set(uint32_t frame, int used) {
    if (frame >= PMM_FRAMES) return;
    uint8_t bit = (uint8_t)(1u << (frame & 7));
    if (used) pmm_bitmap[frame >> 3] |=  bit;
    else      pmm_bitmap[frame >> 3] &= (uint8_t)~bit;
}
static uint32_t pmm_alloc(void) {
    for (uint32_t f = 0; f < PMM_FRAMES; f++)
        if (!pmm_used(f)) { pmm_set(f, 1); return f * PAGE_SIZE; }
    return 0;                              // out of memory
}
static void pmm_init(void) {
    uint32_t used_end = ((uint32_t)_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    for (uint32_t f = 0; f < PMM_FRAMES; f++)
        pmm_set(f, (f * PAGE_SIZE) < used_end);   // kernel image is used
}

// Mark a range of physical addresses as used in the PMM bitmap.
// Called by heap_init() to prevent the page allocator from handing
// out pages inside the heap region.
void pmm_mark_range(uint32_t start, uint32_t end) {
    uint32_t s = (start + PAGE_SIZE - 1) / PAGE_SIZE;  // round up
    uint32_t e = end / PAGE_SIZE;                        // round down
    for (uint32_t f = s; f < e; f++) pmm_set(f, 1);
}

// ─── Page tables ─────────────────────────────────────────────────
// Kernel map: PD + 256 page tables identity-mapping 0 .. 1 GiB.
// Per-task: a PD per task + 4 page tables per task for its 16 MiB
// private region.  All static (kernel BSS → identity-mapped, so
// physical address == virtual address, which is what CR3 needs).
uint32_t k_pd[PD_ENTRIES]          __attribute__((aligned(4096)));
uint32_t k_pt[256][PT_ENTRIES]     __attribute__((aligned(4096)));
static uint32_t task_pds[TASK_MAX][PD_ENTRIES] __attribute__((aligned(4096)));
static uint32_t task_pts[TASK_MAX][4][PT_ENTRIES] __attribute__((aligned(4096)));
static uint32_t priv_next[TASK_MAX];      // next free 4 KiB slot per task

// Map the VESA linear framebuffer base as three 4 MiB large pages
// (1920x1080x32 = 8.3 MiB < 12 MiB).  Only needed because the LFB
// sits above the 1 GiB identity map.
//
// The LFB MUST be mapped uncached: it is device memory that the
// display controller reads continuously.  With a cacheable (WB)
// mapping the CPU's writes sit in the cache and the visible screen
// never updates (QEMU/real hardware reads the raw memory) — the
// classic "back buffer has the frame, screen shows stale wallpaper"
// bug.  PCD disables caching, PWT makes it write-through; together
// every pixel write lands on the wire immediately.
#define PAGING_PCD 0x010u   // page cache disable (bit 4)
#define PAGING_PWT 0x008u   // page write-through (bit 3)
static void paging_map_lfb(uint32_t base) {
    base &= ~0x3FFFFFu;                            // 4 MiB align down
    uint32_t idx = base >> 22;
    for (int i = 0; i < 3; i++) {
        if (idx + (uint32_t)i < PD_ENTRIES)
            k_pd[idx + (uint32_t)i] =
                (base + (uint32_t)i * 0x400000u) | PAGING_P | PAGING_W | PAGING_PS
                | PAGING_PCD | PAGING_PWT;
    }
}

void paging_init(uint32_t fb_phys) {
    // 1. Identity-map 0 .. 256 MiB with 4 KiB pages.
    //    Covers kernel BSS (~12 MiB) + heap (16..272 MiB) + stack.
    for (uint32_t i = 0; i < 256; i++) {
        for (uint32_t j = 0; j < PT_ENTRIES; j++)
            k_pt[i][j] = ((i << 22) | (j << 12)) | PAGING_P | PAGING_W;
        k_pd[i] = ((uint32_t)&k_pt[i][0]) | PAGING_P | PAGING_W;
    }
    // 2. Map the VESA LFB(s) above 1 GiB (bochs-display / std VGA bases
    //    plus whatever multiboot reported as active).
    paging_map_lfb(0xE0000000u);
    paging_map_lfb(0xFD000000u);
    if (fb_phys >= 0x40000000u) paging_map_lfb(fb_phys);

    // 3. Physical allocator: kernel image below _end is taken.
    pmm_init();

    // 4. Per-task page directories start as copies of the kernel map;
    //    the private region entries stay unmapped until allocated.
    for (int t = 0; t < TASK_MAX; t++) {
        for (uint32_t i = 0; i < PD_ENTRIES; i++)
            task_pds[t][i] = k_pd[i];
        priv_next[t] = 0;
    }

    // 5. Turn on paging (identity map → the current PC stays valid).
    //    CR4.PSE must be enabled for the 4 MiB large pages used for the
    //    VESA LFB — without it the PS bit is ignored and the entry is
    //    treated as a page-table pointer (into VRAM → garbage).
    uint32_t cr4;
    asm volatile("mov %%cr4, %0" : "=r" (cr4));
    cr4 |= 0x10u;                                  // CR4.PSE
    asm volatile("mov %0, %%cr4" :: "r" (cr4));

    asm volatile("mov %0, %%cr3" :: "r" ((uint32_t)k_pd));
    uint32_t cr0;
    asm volatile("mov %%cr0, %0" : "=r" (cr0));
    cr0 |= 0x80000000u;                            // CR0.PG
    asm volatile("mov %0, %%cr0" :: "r" (cr0));
}

uint32_t paging_kernel_pd(void) { return (uint32_t)k_pd; }
uint32_t paging_task_pd(int task) {
    if (task <= 0 || task >= TASK_MAX) return paging_kernel_pd();
    return (uint32_t)&task_pds[task][0];
}

// ─── Device MMIO mapping ─────────────────────────────────────────
// PCI memory BARs live in the 0xF0000000+ hole, above the 1 GiB
// kernel identity map.  Map them 1:1 (VA == PA) with fresh page
// tables so the NIC/GPU registers are reachable, and mirror the new
// PD entries into the per-task directories (the ISR runs under the
// CURRENT task's CR3, so the mapping must exist there too).
//
// We use a static page-table pool (not pmm_alloc) because heap_init
// marks the entire PMM range as used, leaving no free frames.
static uint32_t dev_pt_pool[8][PT_ENTRIES] __attribute__((aligned(4096)));
static int dev_pt_used = 0;

void paging_map_device(uint32_t phys, uint32_t size_bytes) {
    uint32_t va = phys & ~(PAGE_SIZE - 1);
    uint32_t end = phys + size_bytes;
    for (; va < end; va += PAGE_SIZE) {
        uint32_t pdi = va >> 22;
        uint32_t pti = (va >> 12) & 0x3FF;
        if (pdi >= PD_ENTRIES) break;
        if (!(k_pd[pdi] & PAGING_P)) {
            if (dev_pt_used >= 8) break;            // pool exhausted
            uint32_t* pt = dev_pt_pool[dev_pt_used++];
            for (uint32_t j = 0; j < PT_ENTRIES; j++) pt[j] = 0;
            k_pd[pdi] = (uint32_t)pt | PAGING_P | PAGING_W;
            // Keep every per-task address space in sync: the NIC ISR
            // may run under any task's CR3.
            for (int t = 0; t < TASK_MAX; t++)
                task_pds[t][pdi] = k_pd[pdi];
        }
        uint32_t* pt = (uint32_t*)(k_pd[pdi] & 0xFFFFF000u);
        pt[pti] = (va & 0xFFFFF000u) | PAGING_P | PAGING_W;
    }
    // Flush the TLB entries for the newly mapped range in the current
    // address space (harmless if the mapping was already present).
    for (uint32_t a = phys & ~(PAGE_SIZE - 1); a < end; a += PAGE_SIZE)
        asm volatile("invlpg (%0)" :: "r" (a));
}

// Which of the task's 4 private-region page tables covers `va`
// (0..3).  Computed relative to the TASK's own region base, so task 1
// (PD entries 772..775) maps to its own le 0..3 — NOT pdi-768 which
// would walk into the next task's tables.
static uint32_t priv_pt_idx(int task, uint32_t va) {
    uint32_t base = TASK_PRIVATE_BASE + (uint32_t)task * TASK_PRIVATE_SIZE;
    return (va - base) >> 22;                 // 0..3 within the 16 MiB region
}

// Release every physical frame mapped in `task`'s private region and
// reset its bump pointer so the slot can host a fresh task (user
// relaunch: DOOM quit → icon Enter → PE reload).  Walks the 4 private
// page tables directly, so holes in the region don't matter — every
// present PTE's frame goes back to the PMM.  The PD entries for the
// private region are zeroed; every other PD entry is RESTORED to the
// kernel map (each task PD is a copy of k_pd that private mappings
// overwrite in-place — wiping them would triple-fault the relaunched
// task on its first kernel access).  Called when a task slot is freed
// (task_kill / task_kill_current / task_exit).
void paging_task_cleanup(int task) {
    if (task <= 0 || task >= TASK_MAX) return;
    for (int le = 0; le < 4; le++) {
        uint32_t* pt = task_pts[task][le];
        for (int i = 0; i < PT_ENTRIES; i++) {
            if (pt[i] & PAGING_P) {
                pmm_set((pt[i] & 0xFFFFF000u) / PAGE_SIZE, 0);  // free frame
                pt[i] = 0;                                      // unmap
            }
        }
        // Private region starts at 0xC0000000: PD index 768 + le.
        task_pds[task][768 + le] = 0;
    }
    priv_next[task] = 0;
}

void* paging_alloc_private(int task, int pages) {
    if (task <= 0 || task >= TASK_MAX || pages < 1) return NULL;
    if (priv_next[task] + (uint32_t)pages > TASK_PRIVATE_PAGES) return NULL;
    uint32_t start = priv_next[task];
    uint32_t va0 = TASK_PRIVATE_BASE +
                   (uint32_t)task * TASK_PRIVATE_SIZE +
                   start * PAGE_SIZE;
    uint32_t va = va0;
    for (uint32_t i = 0; i < (uint32_t)pages; i++) {
        uint32_t phys = pmm_alloc();
        if (!phys) {
            // Roll back the pages already mapped so the region isn't
            // silently consumed by a failed allocation.
            for (uint32_t k = 0; k < i; k++) {
                uint32_t v = va0 + k * PAGE_SIZE;
                uint32_t le = priv_pt_idx(task, v);
                task_pts[task][le][(v >> 12) & 0x3FF] = 0;   // unmap
            }
            priv_next[task] = start;
            return NULL;
        }
        uint32_t pdi = va >> 22;
        uint32_t pti = (va >> 12) & 0x3FF;
        uint32_t le  = priv_pt_idx(task, va);          // 0..3
        task_pds[task][pdi] = ((uint32_t)&task_pts[task][le][0])
                              | PAGING_P | PAGING_W | PAGING_U;
        task_pts[task][le][pti] = (phys & 0xFFFFF000)
                                  | PAGING_P | PAGING_W | PAGING_U;
        va += PAGE_SIZE;
    }
    priv_next[task] = start + (uint32_t)pages;
    return (void*)va0;
}

// Map `pages` fresh physical frames at an EXPLICIT virtual address
// `va` in `task`'s private region (used by the ELF loader to place a
// user program's segments at their link-time vaddr, e.g. 0xC1000000
// for slot 1).  `va` must be 4 KiB aligned and lie inside the task's
// region; only the owning task's page directory maps the pages, so
// isolation is unchanged.  priv_next is advanced to cover the highest
// mapped page so the vmem introspection stays coherent.  Returns the
// vaddr, or NULL on failure (out of memory / bad range).
void* paging_map_user_va(int task, uint32_t va, int pages) {
    uint32_t base = TASK_PRIVATE_BASE + (uint32_t)task * TASK_PRIVATE_SIZE;
    if (task <= 0 || task >= TASK_MAX || pages < 1) return NULL;
    if (va & (PAGE_SIZE - 1)) return NULL;                 // not aligned
    if (va < base || va + (uint32_t)pages * PAGE_SIZE > base + TASK_PRIVATE_SIZE)
        return NULL;                                       // outside region
    uint32_t start = va;
    for (uint32_t i = 0; i < (uint32_t)pages; i++) {
        uint32_t phys = pmm_alloc();
        if (!phys) {
            for (uint32_t k = 0; k < i; k++) {
                uint32_t v = start + k * PAGE_SIZE;
                uint32_t le = priv_pt_idx(task, v);
                task_pts[task][le][(v >> 12) & 0x3FF] = 0;   // unmap
            }
            return NULL;
        }
        uint32_t pdi = va >> 22;
        uint32_t pti = (va >> 12) & 0x3FF;
        uint32_t le  = priv_pt_idx(task, va);          // 0..3
        task_pds[task][pdi] = ((uint32_t)&task_pts[task][le][0])
                              | PAGING_P | PAGING_W | PAGING_U;
        task_pts[task][le][pti] = (phys & 0xFFFFF000)
                                  | PAGING_P | PAGING_W | PAGING_U;
        va += PAGE_SIZE;
    }
    // Advance the bump pointer so later paging_alloc_private() calls
    // (and the vmem shell command) reflect the highest page mapped.
    uint32_t pages_used = (uint32_t)((va - base) >> 12);
    if (pages_used > priv_next[task]) priv_next[task] = pages_used;
    return (void*)start;
}

// Like paging_map_user_va, but the caller specifies explicit page-table
// flags.  PAGING_P is always forced on.  Typical usage:
//   PAGING_P | PAGING_U             → read-only + user (code)
//   PAGING_P | PAGING_W | PAGING_U  → read-write + user (data/BSS)
//   PAGING_P | PAGING_W             → read-write + supervisor (kernel)
//
// x86 32-bit has no hardware execute-permission bit, so code vs data
// can only be distinguished by the R/W flag.  A read-only + user page
// lets the CPU execute it but prevents user-mode writes — which is
// exactly what PE .text segments need.
void* paging_map_user_va_flags(int task, uint32_t va, int pages,
                               uint32_t flags) {
    uint32_t base = TASK_PRIVATE_BASE + (uint32_t)task * TASK_PRIVATE_SIZE;
    if (task <= 0 || task >= TASK_MAX || pages < 1) return NULL;
    if (va & (PAGE_SIZE - 1)) return NULL;
    if (va < base || va + (uint32_t)pages * PAGE_SIZE > base + TASK_PRIVATE_SIZE)
        return NULL;
    // Force present on; allow caller to set W and U independently.
    flags = (flags | PAGING_P) & (PAGING_P | PAGING_W | PAGING_U);
    uint32_t start = va;
    for (uint32_t i = 0; i < (uint32_t)pages; i++) {
        uint32_t phys = pmm_alloc();
        if (!phys) {
            for (uint32_t k = 0; k < i; k++) {
                uint32_t v = start + k * PAGE_SIZE;
                uint32_t le = priv_pt_idx(task, v);
                task_pts[task][le][(v >> 12) & 0x3FF] = 0;
            }
            return NULL;
        }
        uint32_t pdi = va >> 22;
        uint32_t pti = (va >> 12) & 0x3FF;
        uint32_t le  = priv_pt_idx(task, va);
        task_pds[task][pdi] = ((uint32_t)&task_pts[task][le][0]) | flags;
        task_pts[task][le][pti] = (phys & 0xFFFFF000) | flags;
        va += PAGE_SIZE;
    }
    uint32_t pages_used = (uint32_t)((va - base) >> 12);
    if (pages_used > priv_next[task]) priv_next[task] = pages_used;
    return (void*)start;
}

// Change flags on an already-mapped range.  The pages must be
// present; this just overwrites the PTE/PDE flag bits.
// Typical use: tighten permissions after relocation is done.
int paging_set_user_va_flags(int task, uint32_t va, int pages, uint32_t flags) {
    uint32_t base = TASK_PRIVATE_BASE + (uint32_t)task * TASK_PRIVATE_SIZE;
    if (task <= 0 || task >= TASK_MAX || pages < 1) return -1;
    if (va & (PAGE_SIZE - 1)) return -1;
    if (va < base || va + (uint32_t)pages * PAGE_SIZE > base + TASK_PRIVATE_SIZE)
        return -1;
    flags = (flags | PAGING_P) & (PAGING_P | PAGING_W | PAGING_U);
    for (uint32_t i = 0; i < (uint32_t)pages; i++) {
        uint32_t pdi = va >> 22;
        uint32_t pti = (va >> 12) & 0x3FF;
        uint32_t le  = priv_pt_idx(task, va);
        if (!(task_pts[task][le][pti] & PAGING_P)) return -1;  // not mapped
        task_pts[task][le][pti] = (task_pts[task][le][pti] & 0xFFFFF000u) | flags;
        va += PAGE_SIZE;
    }
    return 0;
}

// ─── Introspection ───────────────────────────────────────────────
int paging_priv_count(int task) {
    if (task <= 0 || task >= TASK_MAX) return 0;
    return (int)priv_next[task];
}
uint32_t paging_priv_va(int task, int k) {
    return TASK_PRIVATE_BASE + (uint32_t)task * TASK_PRIVATE_SIZE +
           (uint32_t)k * PAGE_SIZE;
}
int paging_priv_mapped(int task, uint32_t va) {
    // NOTE for task 0: this reads task_pds[0], but main's real CR3 is
    // k_pd.  They are equal because task_pds[0] is a frozen copy and
    // main never allocates private pages — keep it that way if the
    // kernel map ever becomes mutable after paging_init.
    if (task < 0 || task >= TASK_MAX) return 0;
    uint32_t base = TASK_PRIVATE_BASE + (uint32_t)task * TASK_PRIVATE_SIZE;
    if (va < base || va >= base + TASK_PRIVATE_SIZE) return 0;
    uint32_t pdi = va >> 22;
    uint32_t pti = (va >> 12) & 0x3FF;
    uint32_t le  = priv_pt_idx(task, va);              // 0..3
    if (!(task_pds[task][pdi] & PAGING_P)) return 0;
    return (task_pts[task][le][pti] & PAGING_P) ? 1 : 0;
}
int paging_cross_mappings(void) {
    // For every pair of tasks, count pages of t2's private region that
    // t1's page directory maps.  Isolation requires this to be 0.
    int n = 0;
    for (int t1 = 1; t1 < TASK_MAX; t1++) {
        if (priv_next[t1] == 0) continue;
        for (int t2 = 1; t2 < TASK_MAX; t2++) {
            if (t1 == t2 || priv_next[t2] == 0) continue;
            for (int k = 0; k < (int)priv_next[t2]; k++)
                if (paging_priv_mapped(t1, paging_priv_va(t2, k))) n++;
        }
    }
    return n;
}
