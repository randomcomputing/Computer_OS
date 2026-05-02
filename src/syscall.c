#include "syscall.h"
#include "idt.h"
#include "vga.h"
#include "printf.h"
#include "task.h"
#include "keyboard.h"

// Stub from syscall_asm.asm.
extern void syscall_stub(void);

// Implemented in idt.c. Same as the regular set_gate but DPL=3 so a
// ring-3 program can fire `int $0x80` without taking a GP fault.
extern void idt_set_user_gate(int num, unsigned int handler);

void syscall_init(void) {
    idt_set_user_gate(0x80, (unsigned int)syscall_stub);
}

// --- pointer validation -----------------------------------------------
//
// User pointers must point into the user half of the address space
// (anything below KERNEL_VMA = 0xC0000000). This is a coarse check —
// it doesn't verify the page is actually mapped readable/writable, just
// that the user can't trick us into reading kernel memory on its behalf.
// If the page isn't mapped a page fault will fire while we're touching
// it, which currently halts the kernel; a future improvement is to
// catch that and return -EFAULT to the caller.
#define USER_LIMIT 0xC0000000u

static int user_ptr_ok(const void* p, unsigned int len) {
    unsigned int a = (unsigned int)p;
    if (a >= USER_LIMIT) return 0;
    if (len == 0) return 1;
    // Overflow guard, then ensure the *end* of the buffer is also user.
    if (a + len < a) return 0;
    if (a + len > USER_LIMIT) return 0;
    return 1;
}

// --- individual syscalls ----------------------------------------------

static int sys_write(int fd, const char* buf, unsigned int len) {
    // Only stdout (fd=1) and stderr (fd=2) for now — both go to the VGA
    // text console. fd=0 (stdin) writes are nonsensical; reject them.
    if (fd != 1 && fd != 2) return -1;
    if (!user_ptr_ok(buf, len)) return -1;

    for (unsigned int i = 0; i < len; i++) {
        // putchar handles cursor advance, scrolling, and \n.
        vga_putchar(buf[i]);
    }
    return (int)len;
}

static int sys_read(int fd, char* buf, unsigned int len) {
    // Only stdin (fd=0) — blocking single-line read from the keyboard.
    if (fd != 0) return -1;
    if (!user_ptr_ok(buf, len)) return -1;
    if (len == 0) return 0;

    // Pull characters from the keyboard buffer one at a time, yielding
    // between attempts so the rest of the system keeps running. We stop
    // on a newline (which we include in the output) or when the buffer
    // is full. This mirrors the read-line behavior of the kernel shell.
    unsigned int i = 0;
    while (i < len) {
        char c;
        if (keyboard_try_getchar(&c)) {
            buf[i++] = c;
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