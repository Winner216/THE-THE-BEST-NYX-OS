// cpu/heap.c — Simple physical-page heap allocator for Nyx OS
//
// Layout: a contiguous identity-mapped region of physical RAM is
// divided into variable-size blocks, each preceded by a 16-byte
// header (size + flags).  A free-list links all free blocks.
// When the free list is empty, a new 4 KiB page is grabbed from
// the PMM and mapped into the identity-map via k_pt (which covers
// 0 .. 256 MiB — see paging.c).
//
// Constraints:
//   - Only called from kernel context (ring 0).
//   - No locks needed: the PIT ISR never calls kmalloc/kfree.
//   - Alignment: every allocation is 16-byte aligned (header is 16 B).

#include <heap.h>
#include <paging.h>
#include <stdint.h>
#include <stddef.h>

// ─── Block header (lives immediately before every allocation) ─────
// Size includes the header itself and padding; the usable pointer
// is hdr+16.  The low bit of `size` is the FREE flag.
#define HDR_SIZE     16u
#define HDR_FREE     0x01u
#define HDR_SIZE_MASK (~HDR_FREE)

typedef struct BlockHdr {
    uint32_t size;          // total block size (incl header), FREE bit set when free
    uint32_t prev;          // offset from heap_base to previous block (0 = first)
    uint32_t next;          // offset from heap_base to next block (0 = last)
    uint32_t _pad;          // alignment
} BlockHdr;

// ─── Heap state ───────────────────────────────────────────────────
static uint8_t* heap_base   = 0;     // identity-mapped base of heap region
static uint32_t heap_phys   = 0;     // physical base address
static uint32_t heap_end    = 0;     // current end of mapped region (grows)
static uint32_t heap_cap    = 0;     // maximum heap size (bytes)
static uint32_t used_bytes  = 0;     // sum of allocated block sizes

// ─── Page mapping (extend identity-map by one page) ───────────────
// The kernel page tables (k_pt) are set up by paging_init() to
// identity-map 0 .. 256 MiB.  We just need to set the P|W bits.
extern uint32_t k_pd[];
extern uint32_t k_pt[][1024];

static void map_page(uint32_t phys) {
    uint32_t pd_idx = phys >> 22;
    uint32_t pt_idx = (phys >> 12) & 0x3FF;
    if (pd_idx >= 256) return;  // out of identity-map range
    // Ensure the page-table entry for this PD slot exists
    if (!(k_pd[pd_idx] & 1)) {
        // Allocate a fresh page table from the BSS (zeroed by startup)
        // We'll use a static pool of 32 extra page tables (128 KiB BSS)
        // for heap pages — enough for 32 × 4 MiB = 128 MiB of heap.
        static uint32_t extra_pt[32][1024] __attribute__((aligned(4096)));
        static int extra_pt_used = 0;
        if (extra_pt_used >= 32) return;  // out of page tables
        k_pd[pd_idx] = (uint32_t)&extra_pt[extra_pt_used][0] | 0x03;
        extra_pt_used++;
    }
    uint32_t* pt = (uint32_t*)(k_pd[pd_idx] & ~0xFFFu);
    pt[pt_idx] = (phys & ~0xFFFu) | 0x03;  // P | W
}

// ─── PMM helper: mark a range of physical frames as used ──────────
// We prevent the PMM from handing out pages inside the heap region.
extern void pmm_mark_range(uint32_t start, uint32_t end);

// ─── Heap init ────────────────────────────────────────────────────
// `heap_phys_start` = first physical page available after the kernel
// image.  We round up to page boundary and claim a contiguous region.
#define HEAP_MAX_SIZE  (256u * 1024u * 1024u)   // 256 MiB max

void heap_init(uint32_t heap_phys_start) {
    // Round up to page boundary
    heap_phys = (heap_phys_start + 0xFFFu) & ~0xFFFu;
    heap_cap  = HEAP_MAX_SIZE;
    heap_end  = heap_phys;        // no pages mapped yet

    // Reserve the WHOLE heap window in the PMM up front, not just the
    // first page.  pmm_alloc() returns the LOWEST free frame, so task
    // private pages (allocated after boot) would otherwise land exactly
    // in this region — and ensure_space() would later map+zero pages
    // the task already owns, smashing its zone/stack (seen as ring-3
    // faults with a trashed ESP while DOOM uploaded its 9th sfx lump).
    pmm_mark_range(heap_phys, heap_phys + heap_cap);

    // The reservation is safe now that the PMM covers the full 1 GiB
    // (PMM_RAM_TOP = 0x40000000): even with all 256 MiB of heap
    // reserved, ~640 MiB of frames remain for paging_map_device()
    // (NIC MMIO) and paging_alloc_private() (user task pages).
    // map_page() uses its own static page-table pool, and pages taken
    // by the heap stay marked USED so pmm_alloc() never hands a heap
    // frame to a task's private page — the classic "WORK task's count
    // lands on the heap header" corruption.

    // Map and zero the first page so the initial header is valid
    map_page(heap_phys);
    pmm_mark_range(heap_phys, heap_phys + 4096);
    heap_end = heap_phys + 4096;

    // Place the initial free block covering the whole first page
    BlockHdr* h = (BlockHdr*)heap_phys;
    h->size = 4096 | HDR_FREE;
    h->prev = 0;
    h->next = 0;
    h->_pad = 0;

    heap_base = (uint8_t*)heap_phys;
    used_bytes = 0;
}

// ─── Ensure enough pages are mapped for `need` bytes from base ────
static int ensure_space(uint32_t offset_from_base, uint32_t need) {
    uint32_t addr = (uint32_t)heap_base + offset_from_base + need;
    while (heap_end < addr) {
        if (heap_end >= heap_phys + heap_cap) return 0;  // OOM
        map_page(heap_end);
        pmm_mark_range(heap_end, heap_end + 4096);   // keep PMM in sync
        // Zero the new page (important: the free-list sentinel must be clean)
        uint8_t* p = (uint8_t*)heap_end;
        for (uint32_t i = 0; i < 4096; i++) p[i] = 0;
        heap_end += 4096;
    }
    return 1;
}

// ─── Split a block if it's much larger than needed ────────────────
static void split_block(BlockHdr* h, uint32_t need) {
    uint32_t total = h->size & HDR_SIZE_MASK;
    if (total < need + HDR_SIZE + 64) return;  // not worth splitting
    uint32_t remain = total - need;
    // carve out the tail as a new free block
    BlockHdr* tail = (BlockHdr*)((uint8_t*)h + need);
    tail->size = remain | HDR_FREE;
    tail->prev = (uint32_t)((uint8_t*)h - heap_base);
    tail->next = h->next;
    if (h->next) {
        BlockHdr* nx = (BlockHdr*)(heap_base + h->next);
        nx->prev = (uint32_t)((uint8_t*)tail - heap_base);
    }
    h->size = need;
    h->next = (uint32_t)((uint8_t*)tail - heap_base);
}

// ─── Merge a block with its successor if both are free ────────────
static void merge_forward(BlockHdr* h) {
    if (!(h->size & HDR_FREE)) return;
    if (!h->next) return;
    BlockHdr* nx = (BlockHdr*)(heap_base + h->next);
    if (!(nx->size & HDR_FREE)) return;
    // merge
    h->size += (nx->size & HDR_SIZE_MASK);
    h->next = nx->next;
    if (nx->next) {
        BlockHdr* nnx = (BlockHdr*)(heap_base + nx->next);
        nnx->prev = (uint32_t)((uint8_t*)h - heap_base);
    }
}

// ─── kmalloc ──────────────────────────────────────────────────────
// First-fit search of the free list.  If no block fits, grows the
// heap by mapping new pages.
void* kmalloc(uint32_t size) {
    if (size == 0) return NULL;
    // Align up to 16 bytes, add header
    uint32_t need = ((size + 15u) & ~15u) + HDR_SIZE;

    // First-fit scan
    BlockHdr* best = NULL;
    uint32_t off = 0;
    while (off < (uint32_t)((uint8_t*)heap_end - heap_base)) {
        BlockHdr* h = (BlockHdr*)(heap_base + off);
        uint32_t total = h->size & HDR_SIZE_MASK;
        if (total == 0) break;  // corrupt sentinel
        if ((h->size & HDR_FREE) && total >= need) {
            best = h;
            break;
        }
        off += total;
    }

    if (!best) {
        // No free block big enough — extend heap and try the tail
        uint32_t cur_end_off = (uint32_t)((uint8_t*)heap_end - heap_base);
        // Round up to page boundary
        uint32_t page_need = (need + 0xFFFu) & ~0xFFFu;
        if (!ensure_space(cur_end_off, page_need)) return NULL;
        // The last block in the heap should be free (sentinel) or we
        // create one at the old end.
        BlockHdr* tail = (BlockHdr*)(heap_base + cur_end_off - 4096 + (((uint8_t*)heap_end - heap_base - cur_end_off + 4096) & 0xFFFu));
        // Re-scan from end to find the last block
        off = 0;
        while (off < (uint32_t)((uint8_t*)heap_end - heap_base)) {
            BlockHdr* h = (BlockHdr*)(heap_base + off);
            uint32_t total = h->size & HDR_SIZE_MASK;
            if (total == 0) break;
            if (h->next == 0) {
                // This is the last block
                if (h->size & HDR_FREE) {
                    // Extend the last free block to cover the new pages
                    uint32_t new_total = (uint32_t)((uint8_t*)heap_end - (uint8_t*)h);
                    h->size = new_total | HDR_FREE;
                    best = h;
                } else {
                    // Last block is allocated; place a free block after it
                    uint32_t after_off = off + total;
                    uint32_t new_total = (uint32_t)((uint8_t*)heap_end - (heap_base + after_off));
                    if (new_total >= need) {
                        BlockHdr* nb = (BlockHdr*)(heap_base + after_off);
                        nb->size = new_total | HDR_FREE;
                        nb->prev = off;
                        nb->next = 0;
                        nb->_pad = 0;
                        h->next = after_off;
                        best = nb;
                    }
                }
                break;
            }
            off += total;
        }
    }

    if (!best) return NULL;

    // Allocate from the best fit
    split_block(best, need);
    best->size &= HDR_SIZE_MASK;  // clear FREE bit
    used_bytes += best->size;
    return (uint8_t*)best + HDR_SIZE;
}

// ─── kfree ────────────────────────────────────────────────────────
void kfree(void* ptr) {
    if (!ptr) return;
    BlockHdr* h = (BlockHdr*)((uint8_t*)ptr - HDR_SIZE);
    if (h->size == 0) return;  // double-free or corrupt
    used_bytes -= (h->size & HDR_SIZE_MASK);
    h->size |= HDR_FREE;
    // Merge with predecessor if free
    if (h->prev) {
        BlockHdr* prv = (BlockHdr*)(heap_base + h->prev);
        if (prv->size & HDR_FREE) merge_forward(prv);
    }
    // Merge with successor if free
    merge_forward(h);
}

// ─── krealloc ─────────────────────────────────────────────────────
void* krealloc(void* ptr, uint32_t new_size) {
    if (!ptr) return kmalloc(new_size);
    if (new_size == 0) { kfree(ptr); return NULL; }
    BlockHdr* h = (BlockHdr*)((uint8_t*)ptr - HDR_SIZE);
    uint32_t old_usable = (h->size & HDR_SIZE_MASK) - HDR_SIZE;
    uint32_t need = ((new_size + 15u) & ~15u) + HDR_SIZE;
    uint32_t cur_total = h->size & HDR_SIZE_MASK;

    if (need <= cur_total) {
        // Shrink or same size — try to split
        split_block(h, need);
        h->size &= HDR_SIZE_MASK;
        return ptr;
    }
    // Try to absorb successor if it's free and adjacent
    if (h->next) {
        BlockHdr* nx = (BlockHdr*)(heap_base + h->next);
        if ((nx->size & HDR_FREE) && cur_total + (nx->size & HDR_SIZE_MASK) >= need) {
            // absorb successor
            h->size = cur_total + (nx->size & HDR_SIZE_MASK);
            h->next = nx->next;
            if (nx->next) {
                BlockHdr* nnx = (BlockHdr*)(heap_base + nx->next);
                nnx->prev = (uint32_t)((uint8_t*)h - heap_base);
            }
            split_block(h, need);
            h->size &= HDR_SIZE_MASK;
            return ptr;
        }
    }
    // Fallback: allocate new, copy, free old
    void* new_ptr = kmalloc(new_size);
    if (!new_ptr) return NULL;
    uint32_t copy = old_usable < new_size ? old_usable : new_size;
    uint8_t* dst = (uint8_t*)new_ptr;
    uint8_t* src = (uint8_t*)ptr;
    for (uint32_t i = 0; i < copy; i++) dst[i] = src[i];
    kfree(ptr);
    return new_ptr;
}

// ─── Stats ────────────────────────────────────────────────────────
uint32_t heap_used_bytes(void) { return used_bytes; }
uint32_t heap_free_bytes(void) {
    uint32_t total = 0, off = 0;
    while (off < (uint32_t)((uint8_t*)heap_end - heap_base)) {
        BlockHdr* h = (BlockHdr*)(heap_base + off);
        uint32_t sz = h->size & HDR_SIZE_MASK;
        if (sz == 0) break;
        if (h->size & HDR_FREE) total += sz;
        off += sz;
    }
    return total;
}
