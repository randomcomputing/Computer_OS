#ifndef CONSOLE_BACKEND_H
#define CONSOLE_BACKEND_H

#include "console.h"

// A console backend is a set of function pointers implementing the con_*
// operations against some output device. console.c keeps a pointer to the
// active backend and forwards every con_* call to it. Stage 1 had exactly
// one backend (VGA, compiled in directly); Stage 4 adds a framebuffer
// backend and lets the kernel switch from one to the other at runtime via
// con_set_backend().
//
// Backends provide raw operations; console.c owns the public con_* names.
typedef struct {
    void (*clear)(void);
    void (*set_color)(enum con_color fg, enum con_color bg);
    void (*putchar)(char c);
    void (*puts)(const char* s);
    void (*get_cursor)(int* row, int* col);
    void (*set_cursor)(int row, int col);
    void (*putchar_at_cursor)(char c);
    int  (*rows)(void);
    int  (*cols)(void);
    void (*scroll_up)(int rows);
    void (*scroll_down)(int rows);
    void (*scroll_reset)(void);
    int  (*is_scrolled)(void);
} console_backend_t;

// Install a backend as the active console. Passing 0 is ignored. The new
// backend is NOT auto-cleared — the caller decides whether to con_clear().
void con_set_backend(const console_backend_t* backend);

// The VGA text backend (always available; the default at boot).
const console_backend_t* vga_console_backend(void);

#endif