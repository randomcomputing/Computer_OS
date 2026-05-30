#ifndef _USER_SYSCALLS_H
#define _USER_SYSCALLS_H

typedef unsigned long size_t;

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

static inline long _syscall0(long num) {
    long ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(num)
                      : "memory");
    return ret;
}

static inline long _syscall1(long num, long a) {
    long ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(num), "b"(a)
                      : "memory");
    return ret;
}

static inline long _syscall3(long num, long a, long b, long c) {
    long ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(num), "b"(a), "c"(b), "d"(c)
                      : "memory");
    return ret;
}

static inline void exit(int code) {
    _syscall1(SYS_EXIT, code);
    for (;;) { }
}

static inline int write(int fd, const void* buf, size_t len) {
    return (int)_syscall3(SYS_WRITE, fd, (long)buf, (long)len);
}

static inline int read(int fd, void* buf, size_t len) {
    return (int)_syscall3(SYS_READ, fd, (long)buf, (long)len);
}

static inline int getpid(void) {
    return (int)_syscall0(SYS_GETPID);
}

static inline void sched_yield(void) {
    _syscall0(SYS_YIELD);
}

static inline void* sbrk(long delta) {
    long ret = _syscall1(SYS_SBRK, delta);
    return (void*)ret;
}

static inline void set_color(int fg, int bg) {
    _syscall3(SYS_SETCOLOR, fg, bg, 0);
}

static inline void reset_color(void) {
    set_color(COLOR_WHITE, COLOR_BLACK);
}

static inline int fork(void) {
    return (int)_syscall0(SYS_FORK);
}

static inline int exec(const char* path) {
    return (int)_syscall1(SYS_EXEC, (long)path);
}

static inline int wait(int* status) {
    return (int)_syscall1(SYS_WAIT, (long)status);
}

#endif