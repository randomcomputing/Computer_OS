#include "stdio.h"
#include "stdlib.h"
#include "syscalls.h"

// Heap growth probe. Allocates progressively larger chunks until
// malloc fails, then frees everything. Reports the address and size
// of each successful allocation.
//
// Useful as a stress test for the kernel's SYS_SBRK and the user-space
// malloc, and as a way to see how high the heap can climb (the kernel
// caps the user heap at LOADER_HEAP_MAX = 16 MB).

#define MAX_BLOCKS 32

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    printf("memstat: probing heap growth\n");

    void* blocks[MAX_BLOCKS];
    int   count = 0;
    unsigned int size = 64;
    unsigned int total = 0;

    while (count < MAX_BLOCKS) {
        void* p = malloc(size);
        if (!p) {
            printf("  malloc(%u) -> NULL  (heap exhausted)\n", size);
            break;
        }
        blocks[count++] = p;
        total += size;
        printf("  malloc(%u) -> %p  (total %u bytes across %d blocks)\n",
               size, p, total, count);

        // Touch the first and last byte so we know the page is mapped
        // and writeable. If sbrk lied, this would page-fault.
        ((char*)p)[0]        = 0xAA;
        ((char*)p)[size - 1] = 0xBB;

        size *= 2;
    }

    printf("freeing %d blocks...\n", count);
    for (int i = 0; i < count; i++) free(blocks[i]);

    // Verify malloc still works after the big free.
    void* tail = malloc(128);
    if (tail) {
        printf("post-free malloc(128) -> %p  (heap reusable)\n", tail);
        free(tail);
    } else {
        printf("post-free malloc(128) -> NULL  (something's wrong)\n");
        return 1;
    }

    printf("memstat done\n");
    return 0;
}