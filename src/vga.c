#include "vga.h"
#include "io.h"

#define VGA_MEMORY ((volatile unsigned short*)0xB8000)
#define VGA_WIDTH  80
#define VGA_HEIGHT 25
#define TAB_WIDTH  4

// VGA CRTC registers — writing the cursor position is a two-step dance:
// select register 0x0E for the high byte, then 0x0F for the low byte.
#define CRTC_INDEX 0x3D4
#define CRTC_DATA  0x3D5

// ---- scrollback ring buffer ----
// All writes go through this buffer; the visible VGA memory is just a
// window onto it. SB_ROWS is total rows we can scroll back through —
// keep it modest so we don't eat too much .bss. 200 rows * 80 cols * 2
// bytes = 32 KB, which is fine.
#define SB_ROWS 200

// Each cell is a VGA char + attribute pair, same layout as VGA memory.
static unsigned short sb[SB_ROWS * VGA_WIDTH];

// `sb_top` is the row index in the ring of the *oldest* visible-on-live
// content. Equivalently, when not scrolled, we display sb rows
// [sb_top .. sb_top + VGA_HEIGHT - 1] (mod SB_ROWS).
static int sb_top = 0;
// How many full rows of history we have (saturates at SB_ROWS).
static int sb_filled = 0;
// View offset: 0 = live, positive = scrolled this many rows into the past.
static int view_offset = 0;

static int cursor_row = 0;     // logical row within the live view
static int cursor_col = 0;
static unsigned char color_attr = 0x07;

static unsigned short make_cell(char c, unsigned char attr) {
    return (unsigned short)c | ((unsigned short)attr << 8);
}

// Translate (live row, col) -> sb ring index.
static int sb_index(int live_row, int col) {
    int row = (sb_top + live_row) % SB_ROWS;
    if (row < 0) row += SB_ROWS;
    return row * VGA_WIDTH + col;
}

// Repaint the visible 80x25 window from the scrollback ring, taking
// view_offset into account. When view_offset > 0, we show older rows.
static void repaint(void) {
    for (int r = 0; r < VGA_HEIGHT; r++) {
        // The row in the ring we want to draw at screen row `r`.
        // Live shows rows [sb_top .. sb_top+24]; scrolled-back by N
        // shows rows [sb_top-N .. sb_top-N+24].
        int ring_row = (sb_top + r - view_offset) % SB_ROWS;
        if (ring_row < 0) ring_row += SB_ROWS;

        // If the requested row is older than what we've actually
        // captured, blank it out instead of showing garbage.
        int distance_into_past = view_offset - r;
        // distance_into_past > 0 means this screen row is showing
        // historical content; check we have that much history.
        if (distance_into_past > sb_filled) {
            for (int c = 0; c < VGA_WIDTH; c++) {
                VGA_MEMORY[r * VGA_WIDTH + c] = make_cell(' ', color_attr);
            }
            continue;
        }

        for (int c = 0; c < VGA_WIDTH; c++) {
            VGA_MEMORY[r * VGA_WIDTH + c] = sb[ring_row * VGA_WIDTH + c];
        }
    }
}

// Move the hardware cursor (the blinking underline) to our tracked
// position. Hide it off-screen while scrolled back so it doesn't blink
// on a stale-looking line.
static void update_hw_cursor(void) {
    unsigned short pos;
    if (view_offset != 0) {
        pos = VGA_WIDTH * VGA_HEIGHT;   // off-screen
    } else {
        pos = cursor_row * VGA_WIDTH + cursor_col;
    }
    outb(CRTC_INDEX, 0x0F);
    outb(CRTC_DATA, (unsigned char)(pos & 0xFF));
    outb(CRTC_INDEX, 0x0E);
    outb(CRTC_DATA, (unsigned char)((pos >> 8) & 0xFF));
}

// Drop the oldest row off the live window: in the ring, that means
// advancing sb_top by one row, and blanking the new bottom row.
static void scroll_one(void) {
    sb_top = (sb_top + 1) % SB_ROWS;
    if (sb_filled < SB_ROWS) sb_filled++;

    // Blank the new bottom row in the ring.
    int new_bottom = (sb_top + VGA_HEIGHT - 1) % SB_ROWS;
    for (int c = 0; c < VGA_WIDTH; c++) {
        sb[new_bottom * VGA_WIDTH + c] = make_cell(' ', color_attr);
    }
}

static void advance_cursor(void) {
    cursor_col++;
    if (cursor_col >= VGA_WIDTH) {
        cursor_col = 0;
        cursor_row++;
    }
    if (cursor_row >= VGA_HEIGHT) {
        scroll_one();
        cursor_row = VGA_HEIGHT - 1;
    }
}

void vga_set_color(enum vga_color fg, enum vga_color bg) {
    color_attr = (unsigned char)fg | ((unsigned char)bg << 4);
}

void vga_clear(void) {
    unsigned short blank = make_cell(' ', color_attr);
    // Wipe just the live window in the ring (don't erase scrollback
    // history above it; that's the whole point of scrollback).
    for (int r = 0; r < VGA_HEIGHT; r++) {
        for (int c = 0; c < VGA_WIDTH; c++) {
            sb[sb_index(r, c)] = blank;
        }
    }
    cursor_row = 0;
    cursor_col = 0;
    view_offset = 0;
    repaint();
    update_hw_cursor();
}

void vga_init(void) {
    // Initialize the ring to blanks before we start using it.
    unsigned short blank = make_cell(' ', 0x07);
    for (int i = 0; i < SB_ROWS * VGA_WIDTH; i++) sb[i] = blank;
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    vga_clear();
}

// Snap back to live view if the user types while scrolled. We do this
// implicitly on every write so output never disappears below the fold.
static void snap_to_live(void) {
    if (view_offset != 0) {
        view_offset = 0;
        repaint();
    }
}

void vga_putchar(char c) {
    snap_to_live();

    switch (c) {
        case '\n':
            cursor_col = 0;
            cursor_row++;
            if (cursor_row >= VGA_HEIGHT) {
                scroll_one();
                cursor_row = VGA_HEIGHT - 1;
                repaint();
            }
            update_hw_cursor();
            return;
        case '\r':
            cursor_col = 0;
            update_hw_cursor();
            return;
        case '\b':
            if (cursor_col > 0) {
                cursor_col--;
                sb[sb_index(cursor_row, cursor_col)] =
                    make_cell(' ', color_attr);
                VGA_MEMORY[cursor_row * VGA_WIDTH + cursor_col] =
                    make_cell(' ', color_attr);
            }
            update_hw_cursor();
            return;
        case '\t':
            for (int i = 0; i < TAB_WIDTH; i++) vga_putchar(' ');
            return;
    }

    sb[sb_index(cursor_row, cursor_col)] = make_cell(c, color_attr);
    VGA_MEMORY[cursor_row * VGA_WIDTH + cursor_col] = make_cell(c, color_attr);
    advance_cursor();
    update_hw_cursor();
}

void vga_puts(const char* s) {
    while (*s) vga_putchar(*s++);
}

// ---- cursor helpers (for inline editing) ----

void vga_get_cursor(int* row, int* col) {
    if (row) *row = cursor_row;
    if (col) *col = cursor_col;
}

void vga_set_cursor(int row, int col) {
    if (row < 0) row = 0;
    if (row >= VGA_HEIGHT) row = VGA_HEIGHT - 1;
    if (col < 0) col = 0;
    if (col >= VGA_WIDTH) col = VGA_WIDTH - 1;
    cursor_row = row;
    cursor_col = col;
    update_hw_cursor();
}

void vga_putchar_at_cursor(char c) {
    // Write a printable char at cursor without advancing or scrolling.
    // For control chars we just no-op; the caller is responsible.
    if (c < 0x20 || c > 0x7E) return;
    snap_to_live();
    sb[sb_index(cursor_row, cursor_col)] = make_cell(c, color_attr);
    VGA_MEMORY[cursor_row * VGA_WIDTH + cursor_col] = make_cell(c, color_attr);
}

int vga_rows(void) { return VGA_HEIGHT; }
int vga_cols(void) { return VGA_WIDTH;  }

// ---- scrollback view controls ----

void vga_scroll_up(int rows) {
    if (rows <= 0) return;
    int max = sb_filled;
    if (view_offset + rows > max) view_offset = max;
    else                          view_offset += rows;
    repaint();
    update_hw_cursor();
}

void vga_scroll_down(int rows) {
    if (rows <= 0) return;
    if (view_offset - rows < 0) view_offset = 0;
    else                        view_offset -= rows;
    repaint();
    update_hw_cursor();
}

void vga_scroll_reset(void) {
    if (view_offset != 0) {
        view_offset = 0;
        repaint();
        update_hw_cursor();
    }
}

int vga_is_scrolled(void) { return view_offset != 0; }