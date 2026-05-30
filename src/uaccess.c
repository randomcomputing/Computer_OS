#include "uaccess.h"
#include "task.h"
#include "stdint.h"

/* In 64-bit, user space lives below 0x0000800000000000 (canonical boundary).
   We use a simpler limit that covers all practical user addresses. */
#define USER_LIMIT 0x0000800000000000ULL

int user_range_ok(const void* p, unsigned int len) {
    uint64_t a = (uint64_t)p;
    if (a >= USER_LIMIT) return 0;
    if (len == 0) return 1;
    if (a + len < a) return 0;       /* wraparound */
    if (a + len > USER_LIMIT) return 0;
    return 1;
}

int copy_from_user(void* dst, const void* user_src, unsigned int len) {
    if (!user_range_ok(user_src, len)) return -1;
    if (len == 0) return 0;

    task_t* cur = task_current();
    if (!cur) return -1;

    cur->fault_recovery_rip = (uint64_t)&&fault_label;
    cur->uaccess_faulted    = 0;
    __asm__ volatile ("" ::: "memory");
    cur->in_user_access     = 1;

    volatile const unsigned char* s = (volatile const unsigned char*)user_src;
    unsigned char* d = (unsigned char*)dst;
    for (unsigned int i = 0; i < len; i++)
        d[i] = s[i];

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

    cur->fault_recovery_rip = (uint64_t)&&fault_label;
    cur->uaccess_faulted    = 0;
    __asm__ volatile ("" ::: "memory");
    cur->in_user_access     = 1;

    volatile unsigned char* d = (volatile unsigned char*)user_dst;
    const unsigned char* s = (const unsigned char*)src;
    for (unsigned int i = 0; i < len; i++)
        d[i] = s[i];

fault_label:
    cur->in_user_access = 0;
    if (cur->uaccess_faulted) return -1;
    return 0;
}

int strnlen_user(const char* user_str, unsigned int max) {
    if (max == 0) return -1;
    if (!user_range_ok(user_str, 1)) return -1;

    task_t* cur = task_current();
    if (!cur) return -1;

    cur->fault_recovery_rip = (uint64_t)&&fault_label;
    cur->uaccess_faulted    = 0;
    __asm__ volatile ("" ::: "memory");
    cur->in_user_access     = 1;

    volatile const char* p = (volatile const char*)user_str;
    unsigned int n = 0;
    while (n < max) {
        if ((uint64_t)(p + n) >= USER_LIMIT) {
            cur->uaccess_faulted = 1;
            goto fault_label;
        }
        if (p[n] == '\0') break;
        n++;
    }

fault_label:
    cur->in_user_access = 0;
    if (cur->uaccess_faulted) return -1;
    if (n == max) return -1;
    return (int)n;
}