// include/heap.h — Simple physical-page heap allocator for Nyx OS
//
// Provides kmalloc/kfree/krealloc for dynamic memory allocation in
// kernel space.  The heap is identity-mapped in the low 256 MiB of
// physical RAM (below the VESA LFB at 0xE0000000), managed by a
// free-list allocator that grabs 4 KiB pages from the PMM on demand.
#ifndef HEAP_H
#define HEAP_H

#include <stdint.h>

// Initialise the heap allocator.  Must be called AFTER paging_init()
// (identity-maps the heap region) and AFTER pmm_init() (marks the
// kernel BSS as used).  Takes the physical address of the first
// available page after the kernel image.
void heap_init(uint32_t heap_phys_start);

// Allocate `size` bytes (aligned to 16 bytes).  Returns NULL on OOM.
void* kmalloc(uint32_t size);

// Free a previous kmalloc/krealloc allocation.  NULL is a safe no-op.
void  kfree(void* ptr);

// Reallocate to `new_size` bytes, preserving min(old,new) bytes of
// content.  Returns NULL on OOM (original block is untouched).
void* krealloc(void* ptr, uint32_t new_size);

// Heap statistics (for debug / shell).
uint32_t heap_used_bytes(void);
uint32_t heap_free_bytes(void);

#endif
