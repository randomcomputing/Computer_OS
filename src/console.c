#include "console.h"
#include "vga.h"

// Stage 1 console backend: a thin pass-through to the hardware VGA text
// driver. Every con_* call forwards to its vga_* equivalent. This produces
// zero behavioral change — it exists so that callers depend on console.h
// rather than vga.h, giving us one place to swap in a framebuffer backend
// later. The con_color and vga_color enums share the same numeric values,
// so the casts below are identity conversions.

void con_init(void)  { vga_init(); }
void con_clear(void) { vga_clear(); }

void con_set_color(enum con_color fg, enum con_color bg) {
    vga_set_color((enum vga_color)fg, (enum vga_color)bg);
}

void con_putchar(char c)      { vga_putchar(c); }
void con_puts(const char* s)  { vga_puts(s); }

void con_get_cursor(int* row, int* col) { vga_get_cursor(row, col); }
void con_set_cursor(int row, int col)   { vga_set_cursor(row, col); }
void con_putchar_at_cursor(char c)      { vga_putchar_at_cursor(c); }

int con_rows(void) { return vga_rows(); }
int con_cols(void) { return vga_cols(); }

void con_scroll_up(int rows)   { vga_scroll_up(rows); }
void con_scroll_down(int rows) { vga_scroll_down(rows); }
void con_scroll_reset(void)    { vga_scroll_reset(); }
int  con_is_scrolled(void)     { return vga_is_scrolled(); }