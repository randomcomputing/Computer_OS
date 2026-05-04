#ifndef _USER_STDLIB_H
#define _USER_STDLIB_H

#include "syscalls.h"

// Tiny first-fit malloc backed by sbrk().
//
//   malloc(n)        return a pointer to n bytes (uninitialized) or NULL.
//   free(p)          release a pointer previously returned by malloc.
//                    Safe to call with NULL.
//   calloc(n, sz)    malloc(n*sz), already zeroed.
//   realloc(p, n)    grow / shrink an existing allocation. May move it.
//
// 8-byte alignment. Not thread-safe (single-threaded user-space). Walks
// the block chain on every op — fine for kilobytes of allocations.

void* malloc(size_t size);
void  free(void* ptr);
void* calloc(size_t count, size_t size);
void* realloc(void* ptr, size_t size);

#endif