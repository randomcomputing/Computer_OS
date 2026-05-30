#include "syscall.h"
#include "idt.h"
#include "console.h"
#include "printf.h"
#include "task.h"
#include "keyboard.h"
#include "uaccess.h"
#include "loader.h"
#include "stdint.h"

extern void syscall_stub(void);

void syscall_init(void) {
    idt_set_user_gate(0x80, (uint64_t)syscall_stub);
}

#define EFAULT  (-14)
#define EBADF   (-9)
#define EINVAL  (-22)

static int sys_write(int fd, const char* buf, unsigned int len) {
    if (fd != 1 && fd != 2) return EBADF;
    if (len == 0) return 0;

    char chunk[128];
    unsigned int total = 0;
    while (total < len) {
        unsigned int n = len - total;
        if (n > sizeof(chunk)) n = sizeof(chunk);
        if (copy_from_user(chunk, buf + total, n) != 0)
            return (total > 0) ? (int)total : EFAULT;
        for (unsigned int i = 0; i < n; i++) con_putchar(chunk[i]);
        total += n;
    }
    return (int)total;
}

static int sys_read(int fd, char* buf, unsigned int len) {
    if (fd != 0) return EBADF;
    if (len == 0) return 0;
    if (!user_range_ok(buf, len)) return EFAULT;

    unsigned int i = 0;
    while (i < len) {
        char c;
        if (keyboard_try_getchar(&c)) {
            if (copy_to_user(buf + i, &c, 1) != 0)
                return (i > 0) ? (int)i : EFAULT;
            i++;
            if (c == '\n') break;
        } else {
            yield();
        }
    }
    return (int)i;
}

void syscall_handler(struct registers* regs) {
    uint64_t num = regs->rax;
    uint64_t a1  = regs->rbx;
    uint64_t a2  = regs->rcx;
    uint64_t a3  = regs->rdx;

    int ret = -1;

    switch (num) {
        case SYS_EXIT:
            task_exit_code((int)a1);
            for (;;) __asm__ volatile ("hlt");

        case SYS_WRITE:
            ret = sys_write((int)a1, (const char*)a2, (unsigned int)a3);
            break;

        case SYS_READ:
            ret = sys_read((int)a1, (char*)a2, (unsigned int)a3);
            break;

        case SYS_GETPID:
            ret = task_current()->id;
            break;

        case SYS_YIELD:
            yield();
            ret = 0;
            break;

        case SYS_SBRK:
            ret = loader_sbrk((int)a1);
            break;

        case SYS_SETCOLOR:
            if (a1 < 16 && a2 < 16)
                con_set_color((enum con_color)a1, (enum con_color)a2);
            ret = 0;
            break;

        case SYS_FORK:
            ret = loader_fork(regs);
            break;

        case SYS_EXEC: {
            char path[64];
            if (copy_from_user(path, (const char*)a1, sizeof(path)) != 0) {
                ret = EFAULT;
                break;
            }
            path[sizeof(path) - 1] = '\0';
            ret = loader_exec(path, regs);
            break;
        }

        case SYS_WAIT: {
            int status = 0;
            int pid = task_wait_child(&status);
            if (pid >= 0 && a1 != 0) {
                if (copy_to_user((void*)a1, &status, sizeof(int)) != 0) {
                    ret = EFAULT;
                    break;
                }
            }
            ret = pid;
            break;
        }

        default:
            printf("[syscall] unknown number %llu\n", num);
            ret = -1;
            break;
    }

    regs->rax = (uint64_t)ret;
}