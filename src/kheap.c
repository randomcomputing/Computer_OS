#include "kheap.h"
#include "stdint.h"
#include "vmm.h"
#include "pmm.h"
#include "printf.h"
#include "serial.h"

// Block layout: each block (free or used) starts with a header.
//   [ header | user bytes .................. ]
//   kmalloc returns a pointer just past the header.
// Blocks form a singly-linked list in address order so coalescing is easy.

typedef struct block {
    unsigned int  size;   // size of user area in bytes (not including header)
    unsigned char used;   // 1 if allocated, 0 if free
    struct block* next;   // next block in address order, or 0 at end
} block_t;

static block_t*     heap_head   = 0;
static uint64_t     heap_start  = 0;
static uint64_t     heap_end    = 0;
static uint64_t     heap_limit  = 0;

#define HEADER_SIZE      ((unsigned int) sizeof(block_t))
#define ALIGN8(x)        (((x) + 7u) & ~7u)
#define MIN_GROW_PAGES   16u   // never grow by less than 64 KB at a time
#define PAGE             4096u

// --------------------------------------------------------------------
// Map `pages` fresh physical frames at [virt, virt + pages*4K).
// Returns 1 on success, 0 on failure (and rolls back any partial work).
// --------------------------------------------------------------------
static int map_region(uint64_t virt, unsigned int pages) {
    for (unsigned int i = 0; i < pages; i++) {
        uint64_t phys = pmm_alloc();
        if (phys == 0) goto fail;

        if (!vmm_map(virt + i * PAGE, phys, VMM_PRESENT | VMM_WRITE)) {
            pmm_free(phys);
            goto fail;
        }
        continue;

    fail:
        for (unsigned int j = 0; j < i; j++) {
            uint64_t v = virt + j * PAGE;
            uint64_t p = vmm_resolve(v);
            vmm_unmap(v);
            if (p) pmm_free(p);
        }
        return 0;
    }
    return 1;
}

// --------------------------------------------------------------------
// Walk to the last block in the address-ordered list.
// --------------------------------------------------------------------
static block_t* last_block(void) {
    block_t* b = heap_head;
    while (b && b->next) b = b->next;
    return b;
}

void kheap_init(uint64_t base,
                unsigned int initial_pages,
                unsigned int max_pages) {
    if (max_pages < initial_pages) max_pages = initial_pages;

    if (!map_region(base, initial_pages)) {
        printf("[kheap] init: failed to map initial %u pages\n", initial_pages);
        return;
    }

    heap_start = base;
    heap_end   = base + initial_pages * PAGE;
    heap_limit = base + max_pages     * PAGE;

    // Start with one giant free block covering the whole region.
    heap_head        = (block_t*)base;
    heap_head->size  = initial_pages * PAGE - HEADER_SIZE;
    heap_head->used  = 0;
    heap_head->next  = 0;
}

// --------------------------------------------------------------------
// Grow the heap by enough pages to satisfy `min_bytes` of additional
// free space (or MIN_GROW_PAGES, whichever is larger), capped at
// heap_limit. Returns bytes added on success, 0 on failure.
// --------------------------------------------------------------------
unsigned int kheap_grow(unsigned int min_bytes) {
    if (heap_head == 0) return 0;
    if (heap_end >= heap_limit) return 0;

    // Round bytes-needed up to whole pages, then enforce the minimum.
    unsigned int needed_pages = (min_bytes + PAGE - 1) / PAGE;
    if (needed_pages < MIN_GROW_PAGES) needed_pages = MIN_GROW_PAGES;

    // Don't grow past the cap.
    unsigned int room_pages = (heap_limit - heap_end) / PAGE;
    if (needed_pages > room_pages) needed_pages = room_pages;
    if (needed_pages == 0) return 0;

    uint64_t new_region_start = heap_end;
    if (!map_region(new_region_start, needed_pages)) {
        printf("[kheap] grow: map_region failed (%u pages)\n", needed_pages);
        return 0;
    }

    unsigned int added = needed_pages * PAGE;
    heap_end += added;

    // Stitch the new region into the free list. Two cases:
    block_t* tail = last_block();

    if (tail && !tail->used) {
        // Last block is free: just absorb the new region into it.
        // The new free bytes ARE the bytes we just mapped — no new header.
        tail->size += added;
    } else {
        // Last block is used (or list is empty): make a new free block
        // at the start of the new region.
        block_t* nb = (block_t*)new_region_start;
        nb->size = added - HEADER_SIZE;
        nb->used = 0;
        nb->next = 0;
        if (tail) tail->next = nb;
        else      heap_head  = nb;
    }

    return added;
}

// --------------------------------------------------------------------
// First-fit search. Factored out so kmalloc can call it twice: once
// before growing and once after.
// --------------------------------------------------------------------
static void* find_fit(unsigned int size) {
    for (block_t* b = heap_head; b; b = b->next) {
        if (b->used || b->size < size) continue;

        // If the leftover after splitting is big enough to hold a header
        // plus at least 8 bytes, split. Otherwise give the whole block.
        if (b->size >= size + HEADER_SIZE + 8) {
            block_t* rest = (block_t*)((unsigned char*)b + HEADER_SIZE + size);
            rest->size = b->size - size - HEADER_SIZE;
            rest->used = 0;
            rest->next = b->next;

            b->size = size;
            b->next = rest;
        }

        b->used = 1;
        return (unsigned char*)b + HEADER_SIZE;
    }
    return 0;
}

void* kmalloc(unsigned int size) {
    if (size == 0 || heap_head == 0) return 0;
    size = ALIGN8(size);

    void* p = find_fit(size);
    if (p) return p;

    // No fit. Try to grow and retry once.
    // We need at least `size + HEADER_SIZE` bytes of free space — the
    // header eats some of what we map.
    if (kheap_grow(size + HEADER_SIZE) == 0) return 0;

    return find_fit(size);
}

void kfree(void* ptr) {
    if (!ptr) return;

    block_t* b = (block_t*)((unsigned char*)ptr - HEADER_SIZE);
    b->used = 0;

    // Coalesce forward: if the next block is also free, absorb it.
    if (b->next && !b->next->used) {
        b->size += HEADER_SIZE + b->next->size;
        b->next  = b->next->next;
    }

    // Coalesce backward: walk from head to find our predecessor.
    // (Not elegant — a doubly-linked list would be faster — but the list is
    // short and this keeps the data structure simple.)
    if (b != heap_head) {
        for (block_t* p = heap_head; p; p = p->next) {
            if (p->next == b && !p->used) {
                p->size += HEADER_SIZE + b->size;
                p->next  = b->next;
                break;
            }
        }
    }
}

unsigned int kheap_used(void) {
    unsigned int total = 0;
    for (block_t* b = heap_head; b; b = b->next)
        if (b->used) total += b->size;
    return total;
}

unsigned int kheap_free(void) {
    unsigned int total = 0;
    for (block_t* b = heap_head; b; b = b->next)
        if (!b->used) total += b->size;
    return total;
}

unsigned int kheap_blocks(void) {
    unsigned int n = 0;
    for (block_t* b = heap_head; b; b = b->next) n++;
    return n;
}

unsigned int kheap_size(void) {
    return (unsigned int)(heap_end - heap_start);
}

void kheap_print(void) {
    printf("heap: [0x%x .. 0x%x)  cap=0x%x  used=%u  free=%u  blocks=%u\n",
           heap_start, heap_end, heap_limit,
           kheap_used(), kheap_free(), kheap_blocks());
    int i = 0;
    for (block_t* b = heap_head; b; b = b->next, i++) {
        printf("  [%d] 0x%x  size=%u  %s\n",
               i, (uint64_t)b, b->size, b->used ? "USED" : "free");
    }
}