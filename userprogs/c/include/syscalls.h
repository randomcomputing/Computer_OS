#ifndef _USER_SYSCALLS_H
#define _USER_SYSCALLS_H

// Raw kernel-syscall stubs for Computer_OS user programs.
//
// These mirror the dispatch table in src/syscall.c. Calling convention
// is fixed by the kernel:
//
//   eax  -> syscall number
//   ebx  -> arg 1
//   ecx  -> arg 2
//   edx  -> arg 3
//   int $0x80
//   eax  <- return value
//
// All four registers are clobbered by the syscall as far as the C
// compiler is concerned; we list them explicitly. We also clobber
// "memory" so the compiler doesn't reorder loads/stores across the
// trap (e.g. it must not assume a buffer's contents are unchanged
// after a sys_read returns).

typedef unsigned int size_t;

#define SYS_EXIT      0
#define SYS_WRITE     1
#define SYS_READ      2
#define SYS_GETPID    3
#define SYS_YIELD     4
#define SYS_SBRK      5
#define SYS_SETCOLOR  6

// VGA color values (mirror of kernel's enum vga_color).
#define COLOR_BLACK         0
#define COLOR_BLUE          1
#define COLOR_GREEN         2
#define COLOR_CYAN          3
#define COLOR_RED           4
#define COLOR_MAGENTA       5
#define COLOR_BROWN         6
#define COLOR_LIGHT_GREY    7
#define COLOR_DARK_GREY     8
#define COLOR_LIGHT_BLUE    9
#define COLOR_LIGHT_GREEN   10
#define COLOR_LIGHT_CYAN    11
#define COLOR_LIGHT_RED     12
#define COLOR_LIGHT_MAGENTA 13
#define COLOR_YELLOW        14
#define COLOR_WHITE         15

static inline int _syscall0(int num) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(num)
                      : "memory");
    return ret;
}

static inline int _syscall1(int num, int a) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(num), "b"(a)
                      : "memory");
    return ret;
}

static inline int _syscall3(int num, int a, int b, int c) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(num), "b"(a), "c"(b), "d"(c)
                      : "memory");
    return ret;
}

// Convenience wrappers — match the kernel's signature exactly.

static inline void exit(int code) {
    _syscall1(SYS_EXIT, code);
    // exit doesn't return, but the compiler doesn't know that.
    for (;;) { }
}

static inline int write(int fd, const void* buf, size_t len) {
    return _syscall3(SYS_WRITE, fd, (int)buf, (int)len);
}

static inline int read(int fd, void* buf, size_t len) {
    return _syscall3(SYS_READ, fd, (int)buf, (int)len);
}

static inline int getpid(void) {
    return _syscall0(SYS_GETPID);
}

static inline void sched_yield(void) {
    _syscall0(SYS_YIELD);
}

static inline void set_color(int fg, int bg) {
    _syscall3(SYS_SETCOLOR, fg, bg, 0);
}

static inline void reset_color(void) {
    set_color(COLOR_WHITE, COLOR_BLACK);
}

#endif