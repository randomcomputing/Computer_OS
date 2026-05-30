#ifndef ISR_H
#define ISR_H

#include <stdint.h>

/*
 * Saved CPU state at interrupt time (64-bit).
 *
 * isr_common/irq_common push registers in this order (last push = lowest addr):
 *
 *   r15, r14, r13, r12, r11, r10, r9, r8,
 *   rdi, rsi, rbp, rbx, rdx, rcx, rax,    <- GP regs (15 * 8 = 120 bytes)
 *   int_no, err_code,                      <- stub fields  (2 * 8 = 16 bytes)
 *   rip, cs, rflags, rsp, ss               <- CPU frame    (5 * 8 = 40 bytes)
 *
 * Total: 176 bytes.  rsp points at r15 (the first field).
 */
struct registers {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rbx, rdx, rcx, rax;   /* offset 120 = rax */
    uint64_t int_no, err_code;                      /* offset 136 = rip */
    uint64_t rip, cs, rflags, rsp, ss;
};

/* Called from the common ISR stub. */
void isr_handler(struct registers* regs);

#endif