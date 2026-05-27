#ifndef CONSOLE_H
#define CONSOLE_H

// Console abstraction.
//
// Everything that draws text — printf, the shell's line editor, the text
// editor, panic handlers — talks to the console through this interface
// instead of poking the VGA driver directly. The point is a single swappable
// seam: today the backend is vga.c (hardware 80x25 text mode); later it can
// become a linear-framebuffer renderer (fbcon.c) without touching any of the
// ~80 call sites that just want to "put a character on the screen".
//
// The function set mirrors what callers actually used from vga.h: colored
// output, a movable cursor for in-place line editing, scrollback, and the
// screen dimensions. A backend implements all of these; console.c currently
// forwards each one straight to the matching vga_* call.

// Console colors. These intentionally match the classic 16-color VGA palette
// indices so the existing call sites (and the setcolor syscall, which passes
// raw 0..15 values from user space) keep working unchanged.
enum con_color {
    CON_BLACK         = 0,
    CON_BLUE          = 1,
    CON_GREEN         = 2,
    CON_CYAN          = 3,
    CON_RED           = 4,
    CON_MAGENTA       = 5,
    CON_BROWN         = 6,
    CON_LIGHT_GREY    = 7,
    CON_DARK_GREY     = 8,
    CON_LIGHT_BLUE    = 9,
    CON_LIGHT_GREEN   = 10,
    CON_LIGHT_CYAN    = 11,
    CON_LIGHT_RED     = 12,
    CON_LIGHT_MAGENTA = 13,
    CON_YELLOW        = 14,
    CON_WHITE         = 15,
};

// Bring up the console: clear the screen, reset cursor, default colors.
void con_init(void);

// Clear the screen with the current background color.
void con_clear(void);

// Set foreground/background color for subsequent writes.
void con_set_color(enum con_color fg, enum con_color bg);

// Write a single character at the cursor, handling '\n', '\r', '\b', '\t'.
void con_putchar(char c);

// Write a null-terminated string.
void con_puts(const char* s);

// ---- cursor / inline-edit support ----
// Get the current cursor position. row/col may be NULL if not wanted.
void con_get_cursor(int* row, int* col);

// Move the cursor to (row, col). Out-of-range values are clamped.
void con_set_cursor(int row, int col);

// Write a character at the cursor WITHOUT moving the cursor and WITHOUT
// scrolling. Used by line editors to redraw an edited line in place.
void con_putchar_at_cursor(char c);

// Screen dimensions in character cells.
int con_rows(void);
int con_cols(void);

// ---- scrollback ----
// PageUp/PageDown view-history. While scrolled away from "live", new output
// is still recorded but the display stays put until con_scroll_reset().
void con_scroll_up(int rows);
void con_scroll_down(int rows);
void con_scroll_reset(void);
int  con_is_scrolled(void);

// Switch the console to the framebuffer backend. Requires that a VBE linear
// mode has already been set (bochs_vbe_set_mode). Sets up the framebuffer
// console, clears it, and routes all subsequent con_* output there. Returns
// 1 on success, 0 if no framebuffer is available (console stays on VGA text).
int con_use_framebuffer(void);

#endif