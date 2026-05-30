#include "console_backend.h"
#include "bochs_vbe.h"
#include "font8x16.h"
#include "serial.h"

/*
 * Framebuffer text console.
 *
 * Clean version:
 *   - no red debug flash
 *   - no green top stripe
 *   - no noisy framebuffer debug output
 *
 * It keeps the simple one-screen cell buffer and fast pixel-row scrolling.
 */

#define CELL_W  FONT8X16_WIDTH
#define CELL_H  FONT8X16_HEIGHT
#define MAX_COLS 256
#define MAX_ROWS 96

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
static int g_cols;
static int g_rows;
static int g_cur_row;
static int g_cur_col;
static unsigned char g_fg = 7;
static unsigned char g_bg = 0;
static int g_ready = 0;

static cell_t g_cells[MAX_ROWS][MAX_COLS];

static cell_t blank_cell(void) {
    cell_t c;
    c.ch = ' ';
    c.fg = g_fg;
    c.bg = g_bg;
    return c;
}

static void draw_cell(int row, int col, cell_t cell, int invert) {
    if (!g_ready) {
        return;
    }

    if (row < 0 || row >= g_rows || col < 0 || col >= g_cols) {
        return;
    }

    unsigned int fg = palette[(invert ? cell.bg : cell.fg) & 0x0F];
    unsigned int bg = palette[(invert ? cell.fg : cell.bg) & 0x0F];

    const unsigned char* glyph = font8x16_glyph(cell.ch);

    int x0 = col * CELL_W;
    int y0 = row * CELL_H;

    for (int gy = 0; gy < CELL_H; gy++) {
        unsigned char bits = glyph[gy];
        volatile unsigned int* line =
            g_fb + (unsigned int)(y0 + gy) * g_stride + x0;

        for (int gx = 0; gx < CELL_W; gx++) {
            line[gx] = (bits & (0x80 >> gx)) ? fg : bg;
        }
    }
}

static void draw_cursor(int invert) {
    if (!g_ready) {
        return;
    }

    draw_cell(g_cur_row, g_cur_col, g_cells[g_cur_row][g_cur_col], invert);
}

static void clear_framebuffer_raw(unsigned int color) {
    if (!g_fb || !g_stride || g_rows <= 0) {
        return;
    }

    unsigned int pixel_rows = (unsigned int)g_rows * CELL_H;
    unsigned int total = pixel_rows * g_stride;

    for (unsigned int i = 0; i < total; i++) {
        g_fb[i] = color;
    }
}

static void scroll_up_one(void) {
    for (int r = 0; r < g_rows - 1; r++) {
        for (int c = 0; c < g_cols; c++) {
            g_cells[r][c] = g_cells[r + 1][c];
        }
    }

    cell_t b = blank_cell();

    for (int c = 0; c < g_cols; c++) {
        g_cells[g_rows - 1][c] = b;
    }

    /*
     * Move framebuffer pixels up by one character row.
     */
    unsigned int row_pixels = g_stride;
    unsigned int move_rows  = (unsigned int)(g_rows - 1) * CELL_H;

    volatile unsigned int* dst = g_fb;
    volatile unsigned int* src = g_fb + (unsigned int)CELL_H * row_pixels;

    unsigned int total = move_rows * row_pixels;

    for (unsigned int i = 0; i < total; i++) {
        dst[i] = src[i];
    }

    for (int c = 0; c < g_cols; c++) {
        draw_cell(g_rows - 1, c, b, 0);
    }
}

static void newline(void) {
    g_cur_col = 0;

    if (g_cur_row + 1 >= g_rows) {
        scroll_up_one();
    } else {
        g_cur_row++;
    }
}

static void fb_clear(void) {
    draw_cursor(0);

    cell_t b = blank_cell();

    for (int r = 0; r < g_rows; r++) {
        for (int c = 0; c < g_cols; c++) {
            g_cells[r][c] = b;
        }
    }

    clear_framebuffer_raw(palette[g_bg & 0x0F]);

    g_cur_row = 0;
    g_cur_col = 0;

    draw_cursor(1);
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

    g_cells[g_cur_row][g_cur_col] = cell;
    draw_cell(g_cur_row, g_cur_col, cell, 0);

    g_cur_col++;

    if (g_cur_col >= g_cols) {
        newline();
    }
}

static void fb_putchar(char c) {
    draw_cursor(0);

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

            g_cells[g_cur_row][g_cur_col] = blank_cell();
            draw_cell(g_cur_row, g_cur_col, g_cells[g_cur_row][g_cur_col], 0);
            break;

        case '\t': {
            int next = (g_cur_col + 8) & ~7;

            while (g_cur_col < next && g_cur_col < g_cols) {
                put_glyph(' ');
            }

            break;
        }

        default:
            put_glyph(c);
            break;
    }

    draw_cursor(1);
}

static void fb_puts(const char* s) {
    while (*s) {
        fb_putchar(*s++);
    }
}

static void fb_get_cursor(int* row, int* col) {
    if (row) {
        *row = g_cur_row;
    }

    if (col) {
        *col = g_cur_col;
    }
}

static void fb_set_cursor(int row, int col) {
    draw_cursor(0);

    if (row < 0) {
        row = 0;
    }

    if (row >= g_rows) {
        row = g_rows - 1;
    }

    if (col < 0) {
        col = 0;
    }

    if (col >= g_cols) {
        col = g_cols - 1;
    }

    g_cur_row = row;
    g_cur_col = col;

    draw_cursor(1);
}

static void fb_putchar_at_cursor(char c) {
    if (c < 0x20 || c > 0x7E) {
        return;
    }

    draw_cursor(0);

    cell_t cell;
    cell.ch = (unsigned char)c;
    cell.fg = g_fg;
    cell.bg = g_bg;

    g_cells[g_cur_row][g_cur_col] = cell;
    draw_cell(g_cur_row, g_cur_col, cell, 0);

    draw_cursor(1);
}

static int fb_rows(void) {
    return g_rows;
}

static int fb_cols(void) {
    return g_cols;
}

/*
 * Scrollback stubs.
 *
 * These are still no-ops. Normal terminal scrolling works through
 * scroll_up_one(), but mouse-wheel scrollback/history is not implemented here.
 */
static void fb_scroll_up(int rows) {
    (void)rows;
}

static void fb_scroll_down(int rows) {
    (void)rows;
}

static void fb_scroll_reset(void) {
}

static int fb_is_scrolled(void) {
    return 0;
}

static const console_backend_t fb_backend = {
    fb_clear,
    fb_set_color,
    fb_putchar,
    fb_puts,
    fb_get_cursor,
    fb_set_cursor,
    fb_putchar_at_cursor,
    fb_rows,
    fb_cols,
    fb_scroll_up,
    fb_scroll_down,
    fb_scroll_reset,
    fb_is_scrolled,
};

const console_backend_t* fbcon_init(void) {
    const bochs_vbe_mode_t* m = bochs_vbe_current();

    if (!m || !m->ok) {
        return 0;
    }

    g_fb     = (volatile unsigned int*)m->virt;
    g_stride = m->pitch / 4;
    g_cols   = m->width / CELL_W;
    g_rows   = m->height / CELL_H;

    if (g_cols > MAX_COLS) {
        g_cols = MAX_COLS;
    }

    if (g_rows > MAX_ROWS) {
        g_rows = MAX_ROWS;
    }

    if (g_cols <= 0 || g_rows <= 0) {
        return 0;
    }

    g_fg = 7;
    g_bg = 0;
    g_cur_row = 0;
    g_cur_col = 0;
    g_ready = 1;

    cell_t b = blank_cell();

    for (int r = 0; r < g_rows; r++) {
        for (int c = 0; c < g_cols; c++) {
            g_cells[r][c] = b;
        }
    }

    /*
     * Clean init:
     * clear directly to black.
     * No red debug fill.
     * No green first-putchar stripe.
     */
    clear_framebuffer_raw(0x00000000);

    draw_cursor(1);

    return &fb_backend;
}

int fbcon_ready(void) {
    return g_ready;
}