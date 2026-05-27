#include "console_backend.h"
#include "bochs_vbe.h"
#include "font8x16.h"

// Framebuffer text console with real scrollback.
// This version does NOT copy framebuffer pixels when scrolling.  It keeps
// every line as text cells in a ring buffer, then redraws the framebuffer
// from those cells.  That fixes the white/purple smeared letters.

#define CELL_W  FONT8X16_WIDTH
#define CELL_H  FONT8X16_HEIGHT
#define MAX_COLS 256
#define MAX_ROWS 96
#define FB_HISTORY_ROWS 500

static const unsigned int palette[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
    0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
    0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

typedef struct {
    unsigned char ch;
    unsigned char fg;
    unsigned char bg;
} cell_t;

static volatile unsigned int* g_fb;
static unsigned int g_stride;
static int g_cols, g_rows;
static int g_cur_row, g_cur_col;
static unsigned char g_fg = 7, g_bg = 0;
static int g_ready = 0;

// Ring buffer of text rows.  The live screen is always the window
// [g_top ... g_top + g_rows - 1].  Rows before that are scrollback.
static cell_t g_ring[FB_HISTORY_ROWS][MAX_COLS];
static int g_top = 0;
static int g_history = 0;      // available rows above live, max FB_HISTORY_ROWS - g_rows
static int g_view_offset = 0;  // 0 = live; >0 = viewing older rows

static int history_cap(void) {
    int cap = FB_HISTORY_ROWS - g_rows;
    return cap < 0 ? 0 : cap;
}

static int ring_row(int row) {
    int r = row % FB_HISTORY_ROWS;
    if (r < 0) r += FB_HISTORY_ROWS;
    return r;
}

static int live_ring_row(int live_row) {
    return ring_row(g_top + live_row);
}

static cell_t blank_cell(void) {
    cell_t c;
    c.ch = ' ';
    c.fg = g_fg;
    c.bg = g_bg;
    return c;
}

static void clear_ring_row(int rr) {
    cell_t b = blank_cell();
    for (int c = 0; c < g_cols; c++) g_ring[rr][c] = b;
}

static cell_t get_live_cell(int row, int col) {
    return g_ring[live_ring_row(row)][col];
}

static void set_live_cell(int row, int col, cell_t cell) {
    g_ring[live_ring_row(row)][col] = cell;
}

static void draw_cell_value(int screen_row, int col, cell_t cell, int invert) {
    if (screen_row < 0 || screen_row >= g_rows || col < 0 || col >= g_cols) return;

    unsigned int fg = palette[(invert ? cell.bg : cell.fg) & 0x0F];
    unsigned int bg = palette[(invert ? cell.fg : cell.bg) & 0x0F];
    const unsigned char* glyph = font8x16_glyph(cell.ch);

    int x0 = col * CELL_W;
    int y0 = screen_row * CELL_H;

    for (int gy = 0; gy < CELL_H; gy++) {
        unsigned char bits = glyph[gy];
        volatile unsigned int* line = g_fb + (unsigned int)(y0 + gy) * g_stride + x0;
        for (int gx = 0; gx < CELL_W; gx++) {
            line[gx] = (bits & (0x80 >> gx)) ? fg : bg;
        }
    }
}

static void draw_live_cell(int row, int col, int invert) {
    draw_cell_value(row, col, get_live_cell(row, col), invert);
}

static void repaint_visible(void) {
    for (int sr = 0; sr < g_rows; sr++) {
        int past = g_view_offset - sr;

        if (past > g_history) {
            cell_t b = blank_cell();
            for (int c = 0; c < g_cols; c++) draw_cell_value(sr, c, b, 0);
            continue;
        }

        int rr = ring_row(g_top + sr - g_view_offset);
        for (int c = 0; c < g_cols; c++) {
            draw_cell_value(sr, c, g_ring[rr][c], 0);
        }
    }

    if (g_view_offset == 0) draw_live_cell(g_cur_row, g_cur_col, 1);
}

static void hide_cursor(void) {
    if (g_view_offset == 0) draw_live_cell(g_cur_row, g_cur_col, 0);
}

static void show_cursor(void) {
    if (g_view_offset == 0) draw_live_cell(g_cur_row, g_cur_col, 1);
}

static void snap_to_live(void) {
    if (g_view_offset != 0) {
        g_view_offset = 0;
        repaint_visible();
    }
}

static void scroll_one_row(void) {
    g_top = ring_row(g_top + 1);

    if (g_history < history_cap()) g_history++;

    // The new bottom live row is now free.  Blank it in the ring.
    clear_ring_row(live_ring_row(g_rows - 1));

    if (g_view_offset == 0) repaint_visible();
}

static void newline(void) {
    g_cur_col = 0;
    if (g_cur_row + 1 >= g_rows) {
        scroll_one_row();
    } else {
        g_cur_row++;
    }
}

static void fb_clear(void) {
    snap_to_live();
    hide_cursor();

    for (int r = 0; r < g_rows; r++) clear_ring_row(live_ring_row(r));

    g_cur_row = 0;
    g_cur_col = 0;
    repaint_visible();
}

static void fb_set_color(enum con_color fg, enum con_color bg) {
    g_fg = (unsigned char)(fg & 0x0F);
    g_bg = (unsigned char)(bg & 0x0F);
}

static void put_glyph(char c) {
    cell_t cell;
    cell.ch = (unsigned char)c;
    cell.fg = g_fg;
    cell.bg = g_bg;
    set_live_cell(g_cur_row, g_cur_col, cell);
    if (g_view_offset == 0) draw_live_cell(g_cur_row, g_cur_col, 0);

    g_cur_col++;
    if (g_cur_col >= g_cols) newline();
}

static void fb_putchar(char c) {
    snap_to_live();
    hide_cursor();

    switch (c) {
        case '\n':
            newline();
            break;
        case '\r':
            g_cur_col = 0;
            break;
        case '\b':
            if (g_cur_col > 0) {
                g_cur_col--;
            } else if (g_cur_row > 0) {
                g_cur_row--;
                g_cur_col = g_cols - 1;
            }
            set_live_cell(g_cur_row, g_cur_col, blank_cell());
            if (g_view_offset == 0) draw_live_cell(g_cur_row, g_cur_col, 0);
            break;
        case '\t': {
            int next = (g_cur_col + 8) & ~7;
            while (g_cur_col < next && g_cur_col < g_cols) put_glyph(' ');
            break;
        }
        default:
            put_glyph(c);
            break;
    }

    show_cursor();
}

static void fb_puts(const char* s) {
    while (*s) fb_putchar(*s++);
}

static void fb_get_cursor(int* row, int* col) {
    if (row) *row = g_cur_row;
    if (col) *col = g_cur_col;
}

static void fb_set_cursor(int row, int col) {
    snap_to_live();
    hide_cursor();

    if (row < 0) row = 0;
    if (row >= g_rows) row = g_rows - 1;
    if (col < 0) col = 0;
    if (col >= g_cols) col = g_cols - 1;

    g_cur_row = row;
    g_cur_col = col;
    show_cursor();
}

static void fb_putchar_at_cursor(char c) {
    if (c < 0x20 || c > 0x7E) return;

    snap_to_live();
    hide_cursor();

    cell_t cell;
    cell.ch = (unsigned char)c;
    cell.fg = g_fg;
    cell.bg = g_bg;
    set_live_cell(g_cur_row, g_cur_col, cell);
    draw_live_cell(g_cur_row, g_cur_col, 0);

    show_cursor();
}

static int fb_rows(void) { return g_rows; }
static int fb_cols(void) { return g_cols; }

static void fb_scroll_up(int rows) {
    if (rows <= 0) return;

    int old = g_view_offset;
    g_view_offset += rows;
    if (g_view_offset > g_history) g_view_offset = g_history;

    if (g_view_offset != old) repaint_visible();
}

static void fb_scroll_down(int rows) {
    if (rows <= 0) return;

    int old = g_view_offset;
    g_view_offset -= rows;
    if (g_view_offset < 0) g_view_offset = 0;

    if (g_view_offset != old) repaint_visible();
}

static void fb_scroll_reset(void) {
    if (g_view_offset != 0) {
        g_view_offset = 0;
        repaint_visible();
    }
}

static int fb_is_scrolled(void) {
    return g_view_offset != 0;
}

static const console_backend_t fb_backend = {
    fb_clear, fb_set_color, fb_putchar, fb_puts,
    fb_get_cursor, fb_set_cursor, fb_putchar_at_cursor,
    fb_rows, fb_cols,
    fb_scroll_up, fb_scroll_down, fb_scroll_reset, fb_is_scrolled,
};

const console_backend_t* fbcon_init(void) {
    const bochs_vbe_mode_t* m = bochs_vbe_current();
    if (!m || !m->ok) return 0;

    g_fb     = (volatile unsigned int*)m->virt;
    g_stride = m->pitch / 4;
    g_cols   = m->width / CELL_W;
    g_rows   = m->height / CELL_H;

    if (g_cols > MAX_COLS) g_cols = MAX_COLS;
    if (g_rows > MAX_ROWS) g_rows = MAX_ROWS;
    if (g_rows > FB_HISTORY_ROWS) g_rows = FB_HISTORY_ROWS;

    g_fg = 7;
    g_bg = 0;
    g_cur_row = 0;
    g_cur_col = 0;
    g_top = 0;
    g_history = 0;
    g_view_offset = 0;
    g_ready = 1;

    // Clear the entire ring at boot so old rows are never garbage.
    cell_t b = blank_cell();
    for (int r = 0; r < FB_HISTORY_ROWS; r++) {
        for (int c = 0; c < MAX_COLS; c++) {
            g_ring[r][c] = b;
        }
    }

    repaint_visible();
    return &fb_backend;
}

int fbcon_ready(void) {
    return g_ready;
}
