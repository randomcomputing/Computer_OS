#include "uaccess.h"
#include "task.h"

// Must match KERNEL_VMA — anything at or above this is kernel space.
#define USER_LIMIT 0xC0000000u

int user_range_ok(const void* p, unsigned int len) {
    unsigned int a = (unsigned int)p;
    if (a >= USER_LIMIT) return 0;
    if (len == 0) return 1;
    // Wrap check: if a + len overflows, the range straddles 4 GB.
    if (a + len < a) return 0;
    if (a + len > USER_LIMIT) return 0;
    return 1;
}

// The three helpers below all share the same shape:
//
//   1. range-check the user pointer
//   2. arm the recovery: set fault_recovery_eip = &&fault_label,
//      uaccess_faulted = 0, in_user_access = 1
//   3. do the byte-by-byte copy through *volatile* pointers so the
//      compiler can't hoist or batch the loads/stores. If a fault fires
//      mid-loop, the page fault handler jumps execution to fault_label.
//   4. clear in_user_access; check uaccess_faulted; return.
//
// We mark the local pointer-into-user-memory as volatile so each load
// (or store) is a real memory access — without this, gcc could merge
// or eliminate accesses, and the EIP we recover to wouldn't correspond
// to a real instruction in the loop.

int copy_from_user(void* dst, const void* user_src, unsigned int len) {
    if (!user_range_ok(user_src, len)) return -1;
    if (len == 0) return 0;

    task_t* cur = task_current();
    if (!cur) return -1;

    cur->fault_recovery_eip = (unsigned int)&&fault_label;
    cur->uaccess_faulted    = 0;
    __asm__ volatile ("" ::: "memory");
    cur->in_user_access     = 1;

    volatile const unsigned char* s = (volatile const unsigned char*)user_src;
    unsigned char* d = (unsigned char*)dst;
    for (unsigned int i = 0; i < len; i++) {
        d[i] = s[i];                // <-- a fault here lands at fault_label
    }

fault_label:
    cur->in_user_access = 0;
    if (cur->uaccess_faulted) return -1;
    return 0;
}

int copy_to_user(void* user_dst, const void* src, unsigned int len) {
    if (!user_range_ok(user_dst, len)) return -1;
    if (len == 0) return 0;

    task_t* cur = task_current();
    if (!cur) return -1;

    cur->fault_recovery_eip = (unsigned int)&&fault_label;
    cur->uaccess_faulted    = 0;
    __asm__ volatile ("" ::: "memory");
    cur->in_user_access     = 1;

    volatile unsigned char* d = (volatile unsigned char*)user_dst;
    const unsigned char* s = (const unsigned char*)src;
    for (unsigned int i = 0; i < len; i++) {
        d[i] = s[i];
    }

fault_label:
    cur->in_user_access = 0;
    if (cur->uaccess_faulted) return -1;
    return 0;
}

int strnlen_user(const char* user_str, unsigned int max) {
    if (max == 0) return -1;
    // Range-check the *first* byte; the loop will fault naturally if the
    // string runs past a page boundary into unmapped memory.
    if (!user_range_ok(user_str, 1)) return -1;

    task_t* cur = task_current();
    if (!cur) return -1;

    cur->fault_recovery_eip = (unsigned int)&&fault_label;
    cur->uaccess_faulted    = 0;
    __asm__ volatile ("" ::: "memory");
    cur->in_user_access     = 1;

    volatile const char* p = (volatile const char*)user_str;
    unsigned int n = 0;
    while (n < max) {
        // Bound each step against USER_LIMIT so a long string can't
        // wander into kernel memory by accident.
        if (((unsigned int)(p + n)) >= USER_LIMIT) {
            cur->uaccess_faulted = 1;
            goto fault_label;
        }
        if (p[n] == '\0') break;
        n++;
    }

fault_label:
    cur->in_user_access = 0;
    if (cur->uaccess_faulted) return -1;
    if (n == max) return -1;     // no NUL within max
    return (int)n;
}