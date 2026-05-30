#ifndef KHEAP_H
#define KHEAP_H

#include "stdint.h"

// Kernel heap built on top of vmm + pmm.
// Simple first-fit linked-list allocator with coalescing on free.
// Grows on demand: when kmalloc can't find a fit, maps more pages
// onto the end of the heap region (up to max_pages).

// Initialize the heap. Reserves `initial_pages` pages of virtual address
// space starting at `base` and maps them to freshly-allocated physical
// frames. The heap may later grow up to `max_pages` total via kheap_grow().
// Pass max_pages == initial_pages to disable growth.
// Must be called AFTER vmm_init().
void kheap_init(uint64_t base,
                unsigned int initial_pages,
                unsigned int max_pages);

// Allocate `size` bytes. Returns 0 on failure (out of memory and no room
// left to grow).
void* kmalloc(unsigned int size);

// Free a pointer previously returned by kmalloc. kfree(0) is a no-op.
void kfree(void* ptr);

// Grow the heap by at least `min_bytes` of free space. Returns the number
// of bytes actually added (always a page multiple), or 0 on failure.
// Normally called automatically by kmalloc; exposed in case a caller
// knows it's about to do a big allocation and wants to pre-grow.
unsigned int kheap_grow(unsigned int min_bytes);

// Stats.
unsigned int kheap_used(void);   // bytes currently allocated
unsigned int kheap_free(void);   // bytes currently free
unsigned int kheap_blocks(void); // total block headers (used + free)
unsigned int kheap_size(void);   // current heap size in bytes (mapped region)

// Debug: walk the block list and print each block.
void kheap_print(void);

#endif