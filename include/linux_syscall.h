#ifndef LINUX_SYSCALL_H
#define LINUX_SYSCALL_H

#include "stdint.h"

/*
 * Linux x86-64 syscall compatibility layer.
 *
 * linux_syscall_init() sets up the SYSCALL/SYSRET MSRs so the CPU
 * dispatches `syscall` instructions to linux_syscall_entry (in
 * syscall_asm.asm).
 *
 * linux_syscall_handler() is the C-level dispatcher called from asm.
 */

/* Frame passed from linux_syscall_entry (push order: r15 first) */
/* Frame layout — matches kernel stack after 6 callee-saved reg pushes.
   Pointer passed to linux_syscall_handler points here (at r9). */
typedef struct {
    uint64_t r9,  r8,  r10, rdx, rsi, rdi;
    uint64_t rax;       /* syscall number on entry; return value on exit */
    uint64_t rcx;       /* saved user RIP */
    uint64_t r11;       /* saved user RFLAGS */
    uint64_t rbx;       /* saved user RBX — explicitly restored by iretq path */
    uint64_t user_rsp;
} linux_frame_t;

/* Set up STAR, LSTAR, SFMASK MSRs. Call after GDT is loaded. */
void linux_syscall_init(void);

/* C dispatcher — called from linux_syscall_entry with frame pointer. */
void linux_syscall_handler(linux_frame_t* frame);

#endif /* LINUX_SYSCALL_H */