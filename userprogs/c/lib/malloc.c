#include "stdlib.h"
#include "syscalls.h"
// Boundary-tag block header. Every allocation is preceded by one of
// these. `size` is the size of the payload (not including the header),
// in bytes, rounded up to the alignment. The block list is singly-
// linked through `next` in address order — the heap is a contiguous
// region that grows upward via sbrk(), so each block's `next` is just
// the address right after its payload.
//
// We keep the free list implicit: malloc walks the chain looking for
// a free block big enough; free flips a flag. This is O(n) per op but
// fine for kilobytes of allocations.

#define ALIGN       8u
#define ALIGN_UP(x) (((x) + (ALIGN - 1u)) & ~(ALIGN - 1u))

typedef struct block {
    struct block* next;     // next block in heap, or NULL
    unsigned int  size;     // payload size in bytes (multiple of ALIGN)
    unsigned int  free;     // 1 if available, 0 if in use
} block_t;

static block_t* heap_head = 0;     // first block in the heap, or NULL

// Round `size` up to ALIGN, ensure non-zero (a 0-byte malloc still
// reserves ALIGN bytes so the returned pointer is meaningful).
static unsigned int normalize(size_t size) {
    if (size == 0) size = 1;
    return ALIGN_UP((unsigned int)size);
}

// Grow the heap by enough to fit `need` payload bytes plus a header.
// Returns the new block (already in-place at the top of the heap),
// or NULL on failure.
static block_t* grow_heap(unsigned int need) {
    // Always grow by at least 4 KB so we don't sbrk on every malloc.
    unsigned int chunk = need + sizeof(block_t);
    if (chunk < 4096u) chunk = 4096u;

    void* old = sbrk((int)chunk);
    if (old == (void*)-1) return 0;

    block_t* b = (block_t*)old;
    b->next = 0;
    b->size = chunk - sizeof(block_t);
    b->free = 1;

    // Append to the chain.
    if (!heap_head) {
        heap_head = b;
    } else {
        block_t* tail = heap_head;
        while (tail->next) tail = tail->next;
        tail->next = b;
    }
    return b;
}

// If a free block is much bigger than we need, carve off the tail into
// a new free block so the rest stays available.
static void split_block(block_t* b, unsigned int want) {
    // Need enough room for: requested payload + a new header + at least
    // ALIGN bytes for the tail to be useful.
    if (b->size < want + sizeof(block_t) + ALIGN) return;

    block_t* tail = (block_t*)((char*)b + sizeof(block_t) + want);
    tail->next = b->next;
    tail->size = b->size - want - sizeof(block_t);
    tail->free = 1;

    b->next = tail;
    b->size = want;
}

// Coalesce `b` with its successor if both are free and adjacent.
static void coalesce_with_next(block_t* b) {
    if (!b->free || !b->next || !b->next->free) return;
    char* end_of_b = (char*)b + sizeof(block_t) + b->size;
    if ((char*)b->next != end_of_b) return;

    b->size += sizeof(block_t) + b->next->size;
    b->next  = b->next->next;
}

void* malloc(size_t size) {
    unsigned int want = normalize(size);

    // First-fit walk.
    for (block_t* b = heap_head; b; b = b->next) {
        if (b->free && b->size >= want) {
            split_block(b, want);
            b->free = 0;
            return (char*)b + sizeof(block_t);
        }
    }

    // No fit — grow the heap.
    block_t* nb = grow_heap(want);
    if (!nb) return 0;

    split_block(nb, want);
    nb->free = 0;
    return (char*)nb + sizeof(block_t);
}

void free(void* ptr) {
    if (!ptr) return;
    block_t* b = (block_t*)((char*)ptr - sizeof(block_t));
    b->free = 1;

    // Coalesce: every call to free walks the chain and merges any
    // adjacent free pairs. O(n) but keeps fragmentation in check.
    for (block_t* p = heap_head; p && p->next; p = p->next) {
        coalesce_with_next(p);
    }
}

void* calloc(size_t count, size_t size) {
    // Overflow check: if count*size wraps, refuse.
    size_t total = count * size;
    if (count != 0 && total / count != size) return 0;

    void* p = malloc(total);
    if (!p) return 0;

    char* c = (char*)p;
    for (size_t i = 0; i < total; i++) c[i] = 0;
    return p;
}

void* realloc(void* ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (size == 0) { free(ptr); return 0; }

    block_t* b = (block_t*)((char*)ptr - sizeof(block_t));
    unsigned int want = normalize(size);

    // Shrinking or same size: keep the block, possibly split off the tail.
    if (b->size >= want) {
        split_block(b, want);
        return ptr;
    }

    // Growing: try to absorb the next block if it's free and adjacent.
    if (b->next && b->next->free) {
        char* end_of_b = (char*)b + sizeof(block_t) + b->size;
        if ((char*)b->next == end_of_b &&
            b->size + sizeof(block_t) + b->next->size >= want) {
            b->size += sizeof(block_t) + b->next->size;
            b->next  = b->next->next;
            split_block(b, want);
            return ptr;
        }
    }

    // Otherwise allocate a new block, copy, free the old one.
    void* np = malloc(size);
    if (!np) return 0;
    char* dst = (char*)np;
    char* src = (char*)ptr;
    for (unsigned int i = 0; i < b->size && i < want; i++) dst[i] = src[i];
    free(ptr);
    return np;
}