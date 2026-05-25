#ifndef SYSCALL_H
#define SYSCALL_H

#include "isr.h"

// Syscall numbers — used by both kernel and user programs. Keep these
// stable and append-only; user binaries hard-code them.
#define SYS_EXIT      0
#define SYS_WRITE     1
#define SYS_READ      2
#define SYS_GETPID    3
#define SYS_YIELD     4
#define SYS_SBRK      5
#define SYS_SETCOLOR  6
#define SYS_FORK      7
#define SYS_EXEC      8
#define SYS_WAIT      9

// Install the int 0x80 IDT entry with DPL=3 so user code can invoke it.
// Call this from kmain after idt_init().
void syscall_init(void);

// Called from syscall_stub. Reads the syscall number from regs->eax
// and arguments from ebx/ecx/edx/esi/edi. The return value is written
// back into regs->eax — when the stub does `popa` and `iret`, the
// user program resumes with that value in EAX.
void syscall_handler(struct registers* regs);

#endif