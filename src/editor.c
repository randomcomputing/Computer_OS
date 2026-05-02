// Full-screen text editor, nano-style.
//
// ---- Buffer model ------------------------------------------------------
//
// A gap buffer: one contiguous byte array split into [text-before-gap]
// [GAP][text-after-gap]. The cursor sits at the start of the gap. Insert
// = write into the gap and shrink it. Delete-back = enlarge the gap to
// the left. Move-cursor = slide bytes across the gap. All mutations are
// O(1) at the cursor; cursor moves are O(distance) but distances are
// human-sized and the screen redraw dominates anyway.
//
// We *don't* maintain a separate line index. Each redraw (which only
// happens once per keystroke) walks the buffer once to figure out what
// to paint. With an 80x25 screen this is far below any threshold worth
// optimising.
//
// ---- Coordinate systems ------------------------------------------------
//
// `pos`        : byte offset into the logical text (0..len). `gap_start`
//                always equals the cursor's `pos`.
// `cur_row/col`: where the cursor is in the file, in lines and columns.
// `top_line`   : first line of the file currently visible on screen.
// `left_col`   : first column visible (for horizontal scroll).

#include "editor.h"
#include "vga.h"
#include "keyboard.h"
#include "kheap.h"
#include "string.h"
#include "printf.h"
#include "fat12.h"

// ---- screen layout -----------------------------------------------------
// Row 0       = title bar
// Rows 1..N-2 = text area
// Row N-1     = hint / status bar

#define TITLE_ROW   0
#define HINT_ROW    (vga_rows() - 1)
#define TEXT_TOP    1
#define TEXT_BOT    (vga_rows() - 2)
#define TEXT_ROWS   (TEXT_BOT - TEXT_TOP + 1)
#define TEXT_COLS   (vga_cols())

#define TAB_WIDTH   4
#define INITIAL_CAP (64 * 1024)
#define MAX_PATH    128
#define CUT_MAX     1024

// ---- gap buffer --------------------------------------------------------

static char*        buf;
static unsigned int cap;          // total bytes allocated
static unsigned int gap_start;    // == cursor pos in logical text
static unsigned int gap_end;      // first byte after the gap
// Logical text length = gap_start + (cap - gap_end).

static int  modified;
static char path[MAX_PATH];
static int  has_path;

// Cursor position in (line, column-in-line).
static int  cur_row;
static int  cur_col;

// View scroll.
static int  top_line;
static int  left_col;

// Status line message ("Saved.", "No file name", ...).
static char status[64];

// Cut/paste buffer for Ctrl-K / Ctrl-U (one line at a time, nano-style).
static char cut[CUT_MAX];
static int  cut_len;

// ---- local memmove (string.h only exports memcpy/memset) ---------------
//
// The gap-buffer slide overlaps when we move the gap, so we need a real
// memmove. Trivial byte-by-byte; correctness > speed at editor scale.
static void* mem_move(void* dst, const void* src, unsigned int n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    if (d == s || n == 0) return dst;
    if (d < s) {
        for (unsigned int i = 0; i < n; i++) d[i] = s[i];
    } else {
        for (unsigned int i = n; i > 0; i--) d[i - 1] = s[i - 1];
    }
    return dst;
}

// ---- gap buffer primitives ---------------------------------------------

static unsigned int text_len(void) {
    return gap_start + (cap - gap_end);
}

// Read the byte at logical position `i`. Skips over the gap.
static char text_at(unsigned int i) {
    if (i < gap_start)            return buf[i];
    return buf[i - gap_start + gap_end];
}

// Grow the buffer when the gap is empty.
static int grow(void) {
    unsigned int new_cap = cap * 2;
    char* nb = (char*)kmalloc(new_cap);
    if (!nb) return -1;
    unsigned int left  = gap_start;
    unsigned int right = cap - gap_end;
    memcpy(nb, buf, left);
    memcpy(nb + new_cap - right, buf + gap_end, right);
    kfree(buf);
    buf = nb;
    gap_end = new_cap - right;
    cap = new_cap;
    return 0;
}

// Move the gap so that gap_start == pos. Slides bytes across the gap.
static void move_gap(unsigned int pos) {
    if (pos < gap_start) {
        unsigned int n = gap_start - pos;
        mem_move(buf + gap_end - n, buf + pos, n);
        gap_start -= n;
        gap_end   -= n;
    } else if (pos > gap_start) {
        unsigned int n = pos - gap_start;
        mem_move(buf + gap_start, buf + gap_end, n);
        gap_start += n;
        gap_end   += n;
    }
}

static int insert_char(char c) {
    if (gap_start == gap_end && grow() < 0) return -1;
    buf[gap_start++] = c;
    modified = 1;
    return 0;
}

// Delete the byte just before the gap (i.e. the one we'd call "to the
// left of the cursor"). No-op at start of buffer.
static void delete_back(void) {
    if (gap_start == 0) return;
    gap_start--;
    modified = 1;
}

// Delete the byte just after the gap.
static void delete_forward(void) {
    if (gap_end == cap) return;
    gap_end++;
    modified = 1;
}

// ---- line geometry helpers --------------------------------------------
//
// These walk the logical text. They're O(n) but n is bounded by file
// size and we only call them at human cadence.

// Logical position of the start of line `target_line` (0-indexed).
// Returns text_len() if target_line is past EOF.
static unsigned int pos_of_line(int target_line) {
    unsigned int n = text_len();
    int line = 0;
    for (unsigned int i = 0; i < n; i++) {
        if (line == target_line) return i;
        if (text_at(i) == '\n') line++;
    }
    return n;
}

// Length of the line starting at logical position `start` (excluding \n).
static int line_length_from(unsigned int start) {
    unsigned int n = text_len();
    int len = 0;
    for (unsigned int i = start; i < n; i++) {
        if (text_at(i) == '\n') break;
        len++;
    }
    return len;
}

// Recompute (cur_row, cur_col) from gap_start. Cheap enough to redo on
// every keystroke and saves us from having to keep them in sync manually.
static void recompute_cursor(void) {
    int row = 0, col = 0;
    for (unsigned int i = 0; i < gap_start; i++) {
        if (buf[i] == '\n') { row++; col = 0; }
        else                col++;
    }
    cur_row = row;
    cur_col = col;
}

// Total number of lines in the buffer.
static int line_count(void) {
    unsigned int n = text_len();
    int lines = 1;
    for (unsigned int i = 0; i < n; i++) {
        if (text_at(i) == '\n') lines++;
    }
    return lines;
}

// Make sure the cursor is visible on screen by adjusting top_line/left_col.
static void scroll_to_cursor(void) {
    if (cur_row < top_line)                  top_line = cur_row;
    if (cur_row >= top_line + TEXT_ROWS)     top_line = cur_row - TEXT_ROWS + 1;
    if (cur_col < left_col)                  left_col = cur_col;
    if (cur_col >= left_col + TEXT_COLS)     left_col = cur_col - TEXT_COLS + 1;
}

// ---- rendering ---------------------------------------------------------

static int strlen_local(const char* s) {
    int n = 0; while (s[n]) n++; return n;
}

static void draw_bar(int row, const char* text, enum vga_color fg, enum vga_color bg) {
    vga_set_color(fg, bg);
    int cols = vga_cols();
    int n = strlen_local(text);
    if (n > cols) n = cols;
    for (int c = 0; c < cols; c++) {
        vga_set_cursor(row, c);
        vga_putchar_at_cursor(c < n ? text[c] : ' ');
    }
}

static void render(void) {
    enum vga_color fg = VGA_LIGHT_GREY;
    enum vga_color bg = VGA_BLACK;

    // Title bar.
    char title[80];
    const char* name = has_path ? path : "[untitled]";
    const char* mark = modified ? " (modified)" : "";
    // Center-ish: just show name + flag, padded by draw_bar.
    int pos = 0;
    title[pos++] = ' ';
    for (const char* p = "Computer_OS editor  "; *p && pos < 79; p++) title[pos++] = *p;
    for (const char* p = name; *p && pos < 79; p++) title[pos++] = *p;
    for (const char* p = mark; *p && pos < 79; p++) title[pos++] = *p;
    title[pos] = 0;
    draw_bar(TITLE_ROW, title, VGA_BLACK, VGA_LIGHT_GREY);

    // Text area: walk the buffer and paint visible lines.
    vga_set_color(fg, bg);
    int screen_row = TEXT_TOP;
    int file_line  = 0;
    unsigned int n = text_len();
    unsigned int i = 0;

    // Skip lines above top_line.
    while (i < n && file_line < top_line) {
        if (text_at(i) == '\n') file_line++;
        i++;
    }

    // Paint up to TEXT_ROWS lines.
    while (screen_row <= TEXT_BOT) {
        // Clear the row first so deleted content doesn't ghost.
        for (int c = 0; c < TEXT_COLS; c++) {
            vga_set_cursor(screen_row, c);
            vga_putchar_at_cursor(' ');
        }

        if (i >= n && file_line > 0) {
            // Past EOF: leave row blank. (file_line > 0 lets us still
            // show row 0 when the buffer is empty.)
            screen_row++;
            file_line++;
            continue;
        }

        // Walk this line and paint columns visible after horizontal scroll.
        int col_in_line = 0;
        int screen_col  = 0;
        while (i < n && text_at(i) != '\n') {
            if (col_in_line >= left_col && screen_col < TEXT_COLS) {
                vga_set_cursor(screen_row, screen_col);
                vga_putchar_at_cursor(text_at(i));
                screen_col++;
            }
            col_in_line++;
            i++;
        }
        if (i < n && text_at(i) == '\n') i++;
        screen_row++;
        file_line++;
    }

    // Hint bar.
    draw_bar(HINT_ROW,
             status[0] ? status
                       : " ^S Save   ^X Exit   ^K Cut line   ^U Paste",
             VGA_BLACK, VGA_LIGHT_GREY);
    status[0] = 0;   // status messages are one-shot

    // Place the hardware cursor at the cursor's screen position.
    vga_set_color(fg, bg);
    int sr = TEXT_TOP + (cur_row - top_line);
    int sc = cur_col - left_col;
    if (sr < TEXT_TOP) sr = TEXT_TOP;
    if (sr > TEXT_BOT) sr = TEXT_BOT;
    if (sc < 0) sc = 0;
    if (sc >= TEXT_COLS) sc = TEXT_COLS - 1;
    vga_set_cursor(sr, sc);
}

// ---- prompt at the hint bar -------------------------------------------
//
// Used for "Save modified buffer? (y/n/c)" and "File name: ".
// Returns the character pressed (lowercased) for y/n prompts, or fills
// `out` for text input and returns 0 on enter, -1 on cancel.

static char prompt_yn(const char* msg) {
    draw_bar(HINT_ROW, msg, VGA_BLACK, VGA_YELLOW);
    while (1) {
        unsigned char c = (unsigned char)keyboard_getchar();
        if (c == 'y' || c == 'Y') return 'y';
        if (c == 'n' || c == 'N') return 'n';
        if (c == 27  || c == 0x03 /* Ctrl-C */) return 'c';
    }
}

static int prompt_text(const char* msg, char* out, int max) {
    int len = 0;
    out[0] = 0;
    while (1) {
        // Draw prompt + current input.
        char line[80];
        int p = 0;
        for (const char* s = msg; *s && p < 79; s++) line[p++] = *s;
        for (int i = 0; i < len && p < 79; i++) line[p++] = out[i];
        line[p] = 0;
        draw_bar(HINT_ROW, line, VGA_BLACK, VGA_YELLOW);
        vga_set_cursor(HINT_ROW, p < vga_cols() ? p : vga_cols() - 1);

        unsigned char c = (unsigned char)keyboard_getchar();
        if (c == '\n')        { out[len] = 0; return 0; }
        if (c == 27)          { return -1; }
        if (c == '\b') {
            if (len > 0) { len--; out[len] = 0; }
            continue;
        }
        if (c >= 32 && c < 127 && len < max - 1) {
            out[len++] = c;
            out[len] = 0;
        }
    }
}

// ---- file I/O ----------------------------------------------------------

static int load_file(const char* p) {
    // Earlier versions of this function used a 1 MB `static char` scratch
    // buffer in .bss. That blew up the kernel image: .bss extending past
    // the early-boot stack at 0x90000 corrupted the BIOS memory map and
    // made all sorts of things go wrong later (including reboot). Now we
    // load directly into the gap buffer in chunks, growing on demand.
    //
    // We don't have a fat12_size() helper, so we just keep enlarging the
    // buffer and re-reading until the whole file fits. Wasteful for large
    // files but trivial in practice — FAT12 files we care about are tiny.

    unsigned int try_cap = cap;
    while (1) {
        // Make sure buf has at least try_cap bytes.
        while (cap < try_cap) {
            if (grow() < 0) return -1;
        }
        int got = fat12_read_file(p, buf, cap);
        if (got < 0) return -1;
        // If we filled the whole buffer the file might be larger — try
        // again with a bigger one. Otherwise we got everything.
        if ((unsigned)got < cap) {
            gap_start = got;
            gap_end   = cap;
            return 0;
        }
        try_cap = cap * 2;
        // Sanity cap so a runaway file doesn't eat all kernel heap.
        if (try_cap > 512 * 1024) {
            // Truncate: keep what we have.
            gap_start = got;
            gap_end   = cap;
            return 0;
        }
    }
}

static int save_file(const char* p) {
    // Flatten gap buffer into a contiguous block and hand it to FAT12.
    unsigned int n = text_len();
    char* tmp = (char*)kmalloc(n + 1);
    if (!tmp) return -1;
    memcpy(tmp,             buf,           gap_start);
    memcpy(tmp + gap_start, buf + gap_end, n - gap_start);
    int rc = fat12_write_file(p, tmp, n);
    kfree(tmp);
    return rc < 0 ? -1 : 0;
}

static void set_status(const char* s) {
    int j = 0;
    status[j++] = ' ';
    while (*s && j < (int)sizeof status - 1) status[j++] = *s++;
    status[j] = 0;
}

// ---- editor actions ----------------------------------------------------

static void action_left(void) {
    if (gap_start == 0) return;
    move_gap(gap_start - 1);
}

static void action_right(void) {
    if (gap_end == cap) return;
    move_gap(gap_start + 1);
}

static void action_home(void) {
    // Walk back to start of line.
    unsigned int p = gap_start;
    while (p > 0 && text_at(p - 1) != '\n') p--;
    move_gap(p);
}

static void action_end(void) {
    unsigned int p = gap_start;
    unsigned int n = text_len();
    while (p < n && text_at(p) != '\n') p++;
    move_gap(p);
}

static void action_up(void) {
    if (cur_row == 0) return;
    unsigned int prev_start = pos_of_line(cur_row - 1);
    int prev_len = line_length_from(prev_start);
    int target_col = cur_col < prev_len ? cur_col : prev_len;
    move_gap(prev_start + target_col);
}

static void action_down(void) {
    int total = line_count();
    if (cur_row >= total - 1) return;
    unsigned int next_start = pos_of_line(cur_row + 1);
    int next_len = line_length_from(next_start);
    int target_col = cur_col < next_len ? cur_col : next_len;
    move_gap(next_start + target_col);
}

static void action_pgup(void) {
    for (int i = 0; i < TEXT_ROWS - 1; i++) action_up();
}

static void action_pgdn(void) {
    for (int i = 0; i < TEXT_ROWS - 1; i++) action_down();
}

// Cut current line into the cut buffer; replace cut buffer each time
// (nano accumulates consecutive cuts; we keep it simple).
static void action_cut_line(void) {
    action_home();
    unsigned int start = gap_start;
    int len = line_length_from(start);
    if (len > CUT_MAX - 2) len = CUT_MAX - 2;
    for (int i = 0; i < len; i++) cut[i] = text_at(start + i);
    cut[len] = '\n';
    cut_len  = len + 1;
    // Delete the line including its trailing \n.
    for (int i = 0; i < len; i++) delete_forward();
    if (gap_end < cap && buf[gap_end] == '\n') delete_forward();
}

static void action_paste(void) {
    for (int i = 0; i < cut_len; i++) insert_char(cut[i]);
}

// Save flow: if no path, prompt for one. Returns 1 on success, 0 if user
// cancelled or save failed (caller decides what to do).
static int do_save(void) {
    if (!has_path) {
        char tmp[MAX_PATH];
        if (prompt_text(" Save as: ", tmp, MAX_PATH) < 0) return 0;
        if (tmp[0] == 0) return 0;
        // Copy into path.
        int i = 0;
        while (tmp[i] && i < MAX_PATH - 1) { path[i] = tmp[i]; i++; }
        path[i] = 0;
        has_path = 1;
    }
    if (save_file(path) < 0) {
        set_status("Save FAILED");
        return 0;
    }
    modified = 0;
    set_status("Saved.");
    return 1;
}

// ---- main loop ---------------------------------------------------------

int editor_run(const char* p) {
    // Allocate the buffer.
    buf = (char*)kmalloc(INITIAL_CAP);
    if (!buf) return -1;
    cap       = INITIAL_CAP;
    gap_start = 0;
    gap_end   = cap;
    modified  = 0;
    cur_row   = cur_col = 0;
    top_line  = left_col = 0;
    status[0] = 0;
    cut_len   = 0;
    has_path  = (p && p[0]);

    if (has_path) {
        int i = 0;
        while (p[i] && i < MAX_PATH - 1) { path[i] = p[i]; i++; }
        path[i] = 0;
        if (load_file(path) < 0) {
            // File doesn't exist yet: that's fine, we'll create on save.
            set_status("(new file)");
        }
    }

    vga_clear();

    while (1) {
        recompute_cursor();
        scroll_to_cursor();
        render();

        unsigned char c = (unsigned char)keyboard_getchar();

        switch (c) {
            case KEY_LEFT:   action_left();  break;
            case KEY_RIGHT:  action_right(); break;
            case KEY_UP:     action_up();    break;
            case KEY_DOWN:   action_down();  break;
            case KEY_HOME:   action_home();  break;
            case KEY_END:    action_end();   break;
            case KEY_PGUP:   action_pgup();  break;
            case KEY_PGDN:   action_pgdn();  break;
            case KEY_DELETE: delete_forward(); break;

            case '\b':       delete_back();   break;
            case '\t':
                for (int i = 0; i < TAB_WIDTH; i++) insert_char(' ');
                break;
            case '\n':       insert_char('\n'); break;

            case 0x13: /* Ctrl-S */
                do_save();
                break;

            case 0x18: /* Ctrl-X */
                if (modified) {
                    char r = prompt_yn(" Save modified buffer? (Y/N, ESC to cancel) ");
                    if (r == 'c') break;
                    if (r == 'y') {
                        if (!do_save()) break;   // failed/cancelled save
                    }
                }
                vga_clear();
                vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
                kfree(buf);
                buf = 0;
                return 0;

            case 0x0B: /* Ctrl-K */ action_cut_line(); break;
            case 0x15: /* Ctrl-U */ action_paste();    break;

            default:
                if (c >= 32 && c < 127) insert_char((char)c);
                break;
        }
    }
}