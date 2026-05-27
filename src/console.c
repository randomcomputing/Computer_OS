#include "console.h"
#include "console_backend.h"
#include "vga.h"
#include "fbcon.h"
#include "bochs_vbe.h"

// Console dispatch layer.
//
// Public con_* calls forward to whichever backend is currently active. At
// boot the active backend is the VGA text driver (so early [OK] lines render
// in hardware text mode exactly as before). Once the framebuffer is up, the
// kernel calls con_use_framebuffer() to switch the shell to the graphical
// console — see fbcon.c and bochs_vbe.c.

// ---- the VGA backend: thin wrappers over the existing vga_* driver --------
// (con_color and vga_color share numeric values, so the casts are identity.)
static void vga_b_clear(void)                              { vga_clear(); }
static void vga_b_set_color(enum con_color fg, enum con_color bg) {
    vga_set_color((enum vga_color)fg, (enum vga_color)bg);
}
static void vga_b_putchar(char c)                          { vga_putchar(c); }
static void vga_b_puts(const char* s)                      { vga_puts(s); }
static void vga_b_get_cursor(int* row, int* col)           { vga_get_cursor(row, col); }
static void vga_b_set_cursor(int row, int col)             { vga_set_cursor(row, col); }
static void vga_b_putchar_at_cursor(char c)                { vga_putchar_at_cursor(c); }
static int  vga_b_rows(void)                               { return vga_rows(); }
static int  vga_b_cols(void)                               { return vga_cols(); }
static void vga_b_scroll_up(int rows)                      { vga_scroll_up(rows); }
static void vga_b_scroll_down(int rows)                    { vga_scroll_down(rows); }
static void vga_b_scroll_reset(void)                       { vga_scroll_reset(); }
static int  vga_b_is_scrolled(void)                        { return vga_is_scrolled(); }

static const console_backend_t vga_backend = {
    vga_b_clear, vga_b_set_color, vga_b_putchar, vga_b_puts,
    vga_b_get_cursor, vga_b_set_cursor, vga_b_putchar_at_cursor,
    vga_b_rows, vga_b_cols,
    vga_b_scroll_up, vga_b_scroll_down, vga_b_scroll_reset, vga_b_is_scrolled,
};

const console_backend_t* vga_console_backend(void) { return &vga_backend; }

// ---- active backend + dispatch --------------------------------------------
static const console_backend_t* g_backend = &vga_backend;

void con_set_backend(const console_backend_t* backend) {
    if (backend) g_backend = backend;
}

void con_init(void) {
    // The VGA backend is the boot default; initialize the hardware text mode.
    vga_init();
    g_backend = &vga_backend;
}

void con_clear(void)                                   { g_backend->clear(); }
void con_set_color(enum con_color fg, enum con_color bg){ g_backend->set_color(fg, bg); }
void con_putchar(char c)                               { g_backend->putchar(c); }
void con_puts(const char* s)                           { g_backend->puts(s); }
void con_get_cursor(int* row, int* col)                { g_backend->get_cursor(row, col); }
void con_set_cursor(int row, int col)                  { g_backend->set_cursor(row, col); }
void con_putchar_at_cursor(char c)                     { g_backend->putchar_at_cursor(c); }
int  con_rows(void)                                    { return g_backend->rows(); }
int  con_cols(void)                                    { return g_backend->cols(); }
void con_scroll_up(int rows)                           { g_backend->scroll_up(rows); }
void con_scroll_down(int rows)                         { g_backend->scroll_down(rows); }
void con_scroll_reset(void)                            { g_backend->scroll_reset(); }
int  con_is_scrolled(void)                             { return g_backend->is_scrolled(); }

// Switch from the VGA text backend to the framebuffer backend. The VBE mode
// must already be set. On success, fbcon takes over and the screen is cleared
// to the graphical console; on failure we stay on VGA text.
int con_use_framebuffer(void) {
    const console_backend_t* fb = fbcon_init();
    if (!fb) return 0;
    con_set_backend(fb);
    return 1;
}