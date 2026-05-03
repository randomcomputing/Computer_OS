#include "syscall.h"
#include "idt.h"
#include "vga.h"
#include "printf.h"
#include "task.h"
#include "keyboard.h"
#include "uaccess.h"

// Stub from syscall_asm.asm.
extern void syscall_stub(void);

// Implemented in idt.c. Same as the regular set_gate but DPL=3 so a
// ring-3 program can fire `int $0x80` without taking a GP fault.
extern void idt_set_user_gate(int num, unsigned int handler);

void syscall_init(void) {
    idt_set_user_gate(0x80, (unsigned int)syscall_stub);
}

// Standard-ish error codes returned to user space. Negative so a user
// program can do `if (n < 0)`. We don't yet expose an errno table to
// userspace — this is the start of one.
#define EFAULT  (-14)
#define EBADF   (-9)
#define EINVAL  (-22)

// --- individual syscalls ----------------------------------------------

static int sys_write(int fd, const char* buf, unsigned int len) {
    // Only stdout (fd=1) and stderr (fd=2) for now — both go to the VGA
    // text console. fd=0 (stdin) writes are nonsensical; reject them.
    if (fd != 1 && fd != 2) return EBADF;
    if (len == 0) return 0;

    // Copy in fixed-size chunks so we don't need a kernel buffer the
    // size of the user write. Each chunk gets the page-fault recovery
    // treatment via copy_from_user; if the user pointer is bad we
    // return -EFAULT and report how many bytes made it (which is
    // 0 here because we abort on the first failure).
    char chunk[128];
    unsigned int total = 0;
    while (total < len) {
        unsigned int n = len - total;
        if (n > sizeof(chunk)) n = sizeof(chunk);
        if (copy_from_user(chunk, buf + total, n) != 0) {
            // If we already wrote some bytes, return that; otherwise -EFAULT.
            return (total > 0) ? (int)total : EFAULT;
        }
        for (unsigned int i = 0; i < n; i++) vga_putchar(chunk[i]);
        total += n;
    }
    return (int)total;
}

static int sys_read(int fd, char* buf, unsigned int len) {
    // Only stdin (fd=0) — blocking single-line read from the keyboard.
    if (fd != 0) return EBADF;
    if (len == 0) return 0;

    // Range-check upfront so we fail fast on obviously bogus pointers.
    // We still rely on copy_to_user to catch unmapped pages mid-read.
    if (!user_range_ok(buf, len)) return EFAULT;

    // Pull characters from the keyboard buffer one at a time, yielding
    // between attempts so the rest of the system keeps running. We stop
    // on a newline (which we include in the output) or when the buffer
    // is full. This mirrors the read-line behavior of the kernel shell.
    unsigned int i = 0;
    while (i < len) {
        char c;
        if (keyboard_try_getchar(&c)) {
            if (copy_to_user(buf + i, &c, 1) != 0) {
                return (i > 0) ? (int)i : EFAULT;
            }
            i++;
            if (c == '\n') break;
        } else {
            yield();
        }
    }
    return (int)i;
}

void syscall_handler(struct registers* regs) {
    unsigned int num = regs->eax;
    unsigned int a1  = regs->ebx;
    unsigned int a2  = regs->ecx;
    unsigned int a3  = regs->edx;

    int ret = -1;

    switch (num) {
        case SYS_EXIT:
            // a1 is the exit code; we don't surface it yet.
            (void)a1;
            task_exit();
            // task_exit doesn't return, but if we got here something is
            // very wrong — just sit and hope.
            for (;;) __asm__ volatile ("hlt");

        case SYS_WRITE:
            ret = sys_write((int)a1, (const char*)a2, a3);
            break;

        case SYS_READ:
            ret = sys_read((int)a1, (char*)a2, a3);
            break;

        case SYS_GETPID:
            ret = task_current()->id;
            break;

        case SYS_YIELD:
            yield();
            ret = 0;
            break;

        case SYS_SBRK:
            ret = -1;
            break;

        case SYS_SETCOLOR:
            // a1 = fg (0-15), a2 = bg (0-15)
            if (a1 < 16 && a2 < 16)
                vga_set_color((enum vga_color)a1, (enum vga_color)a2);
            ret = 0;
            break;

        default:
            printf("[syscall] unknown number %u\n", num);
            ret = -1;
            break;
    }

    regs->eax = (unsigned int)ret;
}