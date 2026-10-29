// cpu/paging64.c — 64-bit paging support for Nyx OS
//
// In long mode, paging uses 4-level page tables:
//   PML4 → PDPT → PD → PT
// Each level has 512 entries, each entry is 8 bytes.
// Virtual address is split into 5 lookup fields (9 bits each) + 12-bit offset.

#include <stdint.h>
#include <stddef.h>

// Page table entry flags
#define PTE_PRESENT    (1ULL << 0)
#define PTE_WRITABLE   (1ULL << 1)
#define PTE_USER       (1ULL << 2)
#define PTE_WRITE_THRU (1ULL << 3)
#define PTE_CACHE_DIS  (1ULL << 4)
#define PTE_ACCESSED   (1ULL << 5)
#define PTE_DIRTY      (1ULL << 6)
#define PTE_PS         (1ULL << 7)   // Page Size (large page at PD level)
#define PTE_GLOBAL     (1ULL << 8)
#define PTE_NX         (1ULL << 63)  // No-Execute (if supported)

#define PAGE_SIZE_4K   0x1000
#define PAGE_SIZE_2M   0x200000
#define PAGE_SIZE_1G   0x40000000

#define ENTRIES_PER_TABLE 512

// ─── Page table structures ──────────────────────────────────────
// Allocated statically in BSS (identity-mapped by boot stub)

static uint64_t pml4[ENTRIES_PER_TABLE] __attribute__((aligned(PAGE_SIZE_4K)));
static uint64_t pdpt[ENTRIES_PER_TABLE] __attribute__((aligned(PAGE_SIZE_4K)));
static uint64_t pd[ENTRIES_PER_TABLE]   __attribute__((aligned(PAGE_SIZE_4K)));

// ─── Address decomposition ──────────────────────────────────────
// Extract PML4, PDPT, PD, PT indices from a virtual address
#define PML4_INDEX(addr) (((uint64_t)(addr) >> 39) & 0x1FF)
#define PDPT_INDEX(addr) (((uint64_t)(addr) >> 30) & 0x1FF)
#define PD_INDEX(addr)   (((uint64_t)(addr) >> 21) & 0x1FF)
#define PT_INDEX(addr)   (((uint64_t)(addr) >> 12) & 0x1FF)
#define PAGE_OFFSET(addr) ((uint64_t)(addr) & 0xFFF)

// ─── Physical memory management ─────────────────────────────────
// Simple bitmap allocator for the first 4 GiB
#define PMM_MAX_FRAMES (4096ULL * 1024 * 1024 / PAGE_SIZE_4K)  // 1M frames
static uint64_t pmm_bitmap[PMM_MAX_FRAMES / 64];

static void pmm_set_frame(uint64_t frame, int used) {
    uint64_t idx = frame / 64;
    uint64_t bit = frame % 64;
    if (used) pmm_bitmap[idx] |= (1ULL << bit);
    else      pmm_bitmap[idx] &= ~(1ULL << bit);
}

static int pmm_is_used(uint64_t frame) {
    return (pmm_bitmap[frame / 64] >> (frame % 64)) & 1;
}

// Allocate a single 4K page (returns physical address)
static uint64_t pmm_alloc_page(void) {
    for (uint64_t f = 0; f < PMM_MAX_FRAMES; f++) {
        if (!pmm_is_used(f)) {
            pmm_set_frame(f, 1);
            return f * PAGE_SIZE_4K;
        }
    }
    return 0;  // out of memory
}

// ─── Virtual memory mapping ─────────────────────────────────────

// Map a single 4K virtual page to a physical page
// `vaddr` must be page-aligned
int paging_map_4k(uint64_t vaddr, uint64_t paddr, uint64_t flags) {
    uint64_t pml4_idx = PML4_INDEX(vaddr);
    uint64_t pdpt_idx = PDPT_INDEX(vaddr);
    uint64_t pd_idx   = PD_INDEX(vaddr);
    uint64_t pt_idx   = PT_INDEX(vaddr);

    // Ensure PML4 entry exists
    if (!(pml4[pml4_idx] & PTE_PRESENT)) {
        uint64_t pt_phys = pmm_alloc_page();
        if (!pt_phys) return -1;
        pml4[pml4_idx] = pt_phys | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
    }

    // Get PDPT physical address from PML4 entry
    uint64_t pdpt_phys = pml4[pml4_idx] & ~0xFFFULL;
    uint64_t* pdpt_ptr = (uint64_t*)pdpt_phys;  // identity-mapped

    // Ensure PDPT entry exists
    if (!(pdpt_ptr[pdpt_idx] & PTE_PRESENT)) {
        uint64_t pd_phys = pmm_alloc_page();
        if (!pd_phys) return -1;
        pdpt_ptr[pdpt_idx] = pd_phys | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
    }

    // Get PD physical address from PDPT entry
    uint64_t pd_phys = pdpt_ptr[pdpt_idx] & ~0xFFFULL;
    uint64_t* pd_ptr = (uint64_t*)pd_phys;

    // Ensure PD entry exists
    if (!(pd_ptr[pd_idx] & PTE_PRESENT)) {
        uint64_t pt_phys = pmm_alloc_page();
        if (!pt_phys) return -1;
        pd_ptr[pd_idx] = pt_phys | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
    }

    // Get PT physical address from PD entry
    uint64_t pt_phys = pd_ptr[pd_idx] & ~0xFFFULL;
    uint64_t* pt_ptr = (uint64_t*)pt_phys;

    // Set the final mapping
    pt_ptr[pt_idx] = (paddr & ~0xFFFULL) | (flags | PTE_PRESENT);

    return 0;
}

// Map a 2 MiB large page (PD entry directly maps physical)
int paging_map_2m(uint64_t vaddr, uint64_t paddr, uint64_t flags) {
    uint64_t pml4_idx = PML4_INDEX(vaddr);
    uint64_t pdpt_idx = PDPT_INDEX(vaddr);
    uint64_t pd_idx   = PD_INDEX(vaddr);

    // Ensure PML4 entry exists
    if (!(pml4[pml4_idx] & PTE_PRESENT)) {
        uint64_t pt_phys = pmm_alloc_page();
        if (!pt_phys) return -1;
        pml4[pml4_idx] = pt_phys | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
    }

    uint64_t pdpt_phys = pml4[pml4_idx] & ~0xFFFULL;
    uint64_t* pdpt_ptr = (uint64_t*)pdpt_phys;

    // Ensure PDPT entry exists
    if (!(pdpt_ptr[pdpt_idx] & PTE_PRESENT)) {
        uint64_t pd_phys = pmm_alloc_page();
        if (!pd_phys) return -1;
        pdpt_ptr[pdpt_idx] = pd_phys | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
    }

    uint64_t pd_phys = pdpt_ptr[pdpt_idx] & ~0xFFFULL;
    uint64_t* pd_ptr = (uint64_t*)pd_phys;

    // Set 2 MiB large page in PD
    pd_ptr[pd_idx] = (paddr & ~(PAGE_SIZE_2M - 1)) | (flags | PTE_PRESENT | PTE_PS);

    return 0;
}

// Get the physical address for a virtual address (page walk)
uint64_t paging_virt_to_phys(uint64_t vaddr) {
    uint64_t pml4_idx = PML4_INDEX(vaddr);
    uint64_t pdpt_idx = PDPT_INDEX(vaddr);
    uint64_t pd_idx   = PD_INDEX(vaddr);
    uint64_t pt_idx   = PT_INDEX(vaddr);

    if (!(pml4[pml4_idx] & PTE_PRESENT)) return 0;
    uint64_t pdpt_phys = pml4[pml4_idx] & ~0xFFFULL;
    uint64_t* pdpt_ptr = (uint64_t*)pdpt_phys;

    if (!(pdpt_ptr[pdpt_idx] & PTE_PRESENT)) return 0;
    // Check for 1 GiB page
    if (pdpt_ptr[pdpt_idx] & PTE_PS) {
        return (pdpt_ptr[pdpt_idx] & ~(PAGE_SIZE_1G - 1)) + (vaddr & (PAGE_SIZE_1G - 1));
    }

    uint64_t pd_phys = pdpt_ptr[pdpt_idx] & ~0xFFFULL;
    uint64_t* pd_ptr = (uint64_t*)pd_phys;

    if (!(pd_ptr[pd_idx] & PTE_PRESENT)) return 0;
    // Check for 2 MiB page
    if (pd_ptr[pd_idx] & PTE_PS) {
        return (pd_ptr[pd_idx] & ~(PAGE_SIZE_2M - 1)) + (vaddr & (PAGE_SIZE_2M - 1));
    }

    uint64_t pt_phys = pd_ptr[pd_idx] & ~0xFFFULL;
    uint64_t* pt_ptr = (uint64_t*)pt_phys;

    if (!(pt_ptr[pt_idx] & PTE_PRESENT)) return 0;
    return (pt_ptr[pt_idx] & ~0xFFFULL) + PAGE_OFFSET(vaddr);
}

// Flush TLB for a specific virtual address
void paging_flush_tlb(uint64_t vaddr) {
    asm volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
}

// Load PML4 into CR3
void paging_load_cr3(uint64_t pml4_phys) {
    asm volatile("mov %0, %%cr3" : : "r"(pml4_phys) : "memory");
}

// Get current CR3
uint64_t paging_get_cr3(void) {
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

// Initialize the kernel's page tables (called from kernel_main64)
void paging64_init(void) {
    // The boot stub already set up identity mapping via the PD.
    // We need to extend this for the full kernel address space.

    // Identity-map the first 2 MiB (already done by boot stub)
    // Map the kernel's higher half (0xFFFFFFFF80000000 → physical 0x100000)
    // The boot stub already set up PML4[511] → PDPT[0] → PD[0] → 2 MiB page
    // which identity-maps the first 2 MiB AND maps higher-half.

    // For now, the boot stub's page tables are sufficient.
    // The kernel runs with identity + higher-half mapping.
    // Full per-task paging will be added when we port the scheduler.

    // The CR3 is already set by the boot stub.
    // We just need to make sure the TLB is fresh.
    { uint64_t cr3; asm volatile("mov %%cr3, %0" : "=r"(cr3)); asm volatile("mov %0, %%cr3" : : "r"(cr3) : "memory"); }
}
