#ifndef VGA_H
#define VGA_H

// VGA text mode colors (foreground or background)
enum vga_color {
    VGA_BLACK         = 0,
    VGA_BLUE          = 1,
    VGA_GREEN         = 2,
    VGA_CYAN          = 3,
    VGA_RED           = 4,
    VGA_MAGENTA       = 5,
    VGA_BROWN         = 6,
    VGA_LIGHT_GREY    = 7,
    VGA_DARK_GREY     = 8,
    VGA_LIGHT_BLUE    = 9,
    VGA_LIGHT_GREEN   = 10,
    VGA_LIGHT_CYAN    = 11,
    VGA_LIGHT_RED     = 12,
    VGA_LIGHT_MAGENTA = 13,
    VGA_YELLOW        = 14,
    VGA_WHITE         = 15,
};

// Initialize: clear screen, reset cursor to (0,0), default colors
void vga_init(void);

// Clear the screen with the current background color
void vga_clear(void);

// Set foreground/background color for subsequent writes
void vga_set_color(enum vga_color fg, enum vga_color bg);

// Write a single character at the cursor, handling '\n', '\r', '\b', '\t'
void vga_putchar(char c);

// Write a null-terminated string
void vga_puts(const char* s);

// ---- cursor / inline-edit support ----
// Used by line editors (like the shell readline) to move within a line
// without scrolling or advancing past the end.

// Get the current cursor position. row/col may be NULL if not wanted.
void vga_get_cursor(int* row, int* col);

// Move the cursor to (row, col). Out-of-range values are clamped.
void vga_set_cursor(int row, int col);

// Write a single character at the cursor position WITHOUT moving the
// cursor and WITHOUT scrolling. Used to redraw an edited line.
void vga_putchar_at_cursor(char c);

// Number of rows/columns of the text screen.
int vga_rows(void);
int vga_cols(void);

// ---- scrollback ----
// PageUp/PageDown view-history. The shell calls these so the user can
// scroll back through output that has rolled off the top. While viewing
// history, new output is still recorded but the display stays put until
// vga_scroll_reset() is called (e.g. on the next keystroke or print of
// a prompt).

// Scroll the visible window up/down by N rows (toward older / newer).
void vga_scroll_up(int rows);
void vga_scroll_down(int rows);

// Snap the visible window back to "live" (most recent output).
void vga_scroll_reset(void);

// True if we're currently scrolled away from live output.
int  vga_is_scrolled(void);

#endif