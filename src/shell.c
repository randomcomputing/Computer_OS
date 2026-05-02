#include "shell.h"
#include "vga.h"
#include "printf.h"
#include "keyboard.h"
#include "string.h"
#include "pit.h"
#include "memmap.h"
#include "pmm.h"
#include "vmm.h"
#include "kheap.h"
#include "task.h"
#include "fat12.h"
#include "io.h"
#include "userprog.h"
#include "rtc.h"
#include "loader.h"
#include "editor.h"

#define LINE_MAX 128

// ---- command names ----------------------------------------------------
//
// Listed once here, used by both the dispatcher and tab completion. Keep
// in sync with the if/else chain in execute() at the bottom of the file.

static const char* commands[] = {
    "help", "clear", "echo", "about", "uptime", "sleep",
    "meminfo", "pmemstat", "palloc", "vmap", "kmstat", "kmtest",
    "ps", "spawn", "yield", "preempt",
    "ls", "cat", "write", "rm", "cp", "mv", "mkdir", "rmdir",
    "cd", "pwd",
    "history", "date", "time", "tz",
    "user", "run", "edit", "reboot", "shutdown",
    0
};

// ---- command history --------------------------------------------------
#define HIST_SIZE 16
static char hist[HIST_SIZE][LINE_MAX];
static int  hist_next  = 0;
static int  hist_count = 0;

static void history_push(const char* line) {
    if (line[0] == '\0') return;
    if (hist_count > 0) {
        int last = (hist_next - 1 + HIST_SIZE) % HIST_SIZE;
        if (strcmp(hist[last], line) == 0) return;
    }
    int i = 0;
    while (line[i] && i < LINE_MAX - 1) {
        hist[hist_next][i] = line[i];
        i++;
    }
    hist[hist_next][i] = '\0';
    hist_next = (hist_next + 1) % HIST_SIZE;
    if (hist_count < HIST_SIZE) hist_count++;
}

static const char* history_get(int back) {
    if (back < 1 || back > hist_count) return 0;
    int idx = (hist_next - back + HIST_SIZE) % HIST_SIZE;
    return hist[idx];
}

// ---- demo task workers (for `spawn`) ---------------------------------
static void demo_counter(void) {
    for (int i = 0; i < 5; i++) {
        printf("[%s] tick %d\n", task_current()->name, i);
        pit_sleep(500);
    }
    printf("[%s] done\n", task_current()->name);
}

static void demo_spinner(void) {
    const char* frames = "|/-\\";
    for (int i = 0; i < 40; i++) {
        printf("\b%c", frames[i & 3]);
        pit_sleep(100);
    }
    printf("\n[%s] spinner done\n", task_current()->name);
}

// =====================================================================
// Tab completion
// =====================================================================

// Lower-case a char (for case-insensitive prefix match).
static char to_lower(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A' + 'a';
    return c;
}

// Case-insensitive prefix test.
static int starts_with_ci(const char* s, const char* prefix) {
    while (*prefix) {
        if (to_lower(*s) != to_lower(*prefix)) return 0;
        s++; prefix++;
    }
    return 1;
}

// Length of the common (case-insensitive) prefix of two strings.
static int common_prefix_len(const char* a, const char* b) {
    int n = 0;
    while (a[n] && b[n] && to_lower(a[n]) == to_lower(b[n])) n++;
    return n;
}

// Find the start of the "current word" in buf[0..pos). Word = run of
// non-space chars ending at the cursor.
static int word_start(const char* buf, int pos) {
    int i = pos;
    while (i > 0 && buf[i - 1] != ' ') i--;
    return i;
}

// Are we completing the FIRST word on the line? (just whitespace before it)
static int is_first_word(const char* buf, int word_begin) {
    for (int i = 0; i < word_begin; i++) {
        if (buf[i] != ' ') return 0;
    }
    return 1;
}

// Outcomes from a single-pass scan over candidates: the count and, when
// count >= 1, the longest common prefix of all matches and a copy of the
// first match (used as the starting comparison string for the prefix).
typedef struct {
    int  count;
    char first[64];     // first matching name (the LCP starts as this)
    int  lcp_len;       // length of the case-insensitive LCP across matches
    int  first_is_dir;  // 1 if the first match is a directory
} match_result_t;

static void match_init(match_result_t* m) {
    m->count = 0;
    m->first[0] = '\0';
    m->lcp_len = 0;
    m->first_is_dir = 0;
}

// Add a candidate string to the result set, narrowing the LCP.
// `is_dir` only matters when the candidate becomes the unique match.
static void match_add(match_result_t* m, const char* name, int is_dir) {
    if (m->count == 0) {
        int i = 0;
        while (name[i] && i < (int)sizeof(m->first) - 1) {
            m->first[i] = name[i]; i++;
        }
        m->first[i] = '\0';
        m->lcp_len = i;
        m->first_is_dir = is_dir;
    } else {
        int n = common_prefix_len(m->first, name);
        if (n < m->lcp_len) m->lcp_len = n;
    }
    m->count++;
}

// Print all candidates that start with `prefix`, separated by two spaces,
// wrapping at ~76 cols. Used after a tab miss to show the user what's
// available.
static void print_command_matches(const char* prefix) {
    int col = 0;
    for (int i = 0; commands[i]; i++) {
        if (!starts_with_ci(commands[i], prefix)) continue;
        int len = (int)strlen(commands[i]);
        if (col > 0 && col + len + 2 > 76) {
            printf("\n");
            col = 0;
        }
        printf("%s  ", commands[i]);
        col += len + 2;
    }
    if (col > 0) printf("\n");
}

static void print_dir_matches(unsigned int dir, const char* prefix) {
    fat12_dirent_t entries[64];
    int n = fat12_list_dir(dir, entries, 64);
    if (n <= 0) return;
    int col = 0;
    for (int i = 0; i < n; i++) {
        // Skip "." and ".." in the listing — they're useful for cd but
        // noisy in tab completion of file names.
        if (entries[i].name[0] == '.' &&
            (entries[i].name[1] == '\0' ||
             (entries[i].name[1] == '.' && entries[i].name[2] == '\0')))
            continue;
        if (!starts_with_ci(entries[i].name, prefix)) continue;
        int is_dir = (entries[i].attr & 0x10) != 0;
        int len = (int)strlen(entries[i].name) + (is_dir ? 1 : 0);
        if (col > 0 && col + len + 2 > 76) { printf("\n"); col = 0; }
        printf("%s%s  ", entries[i].name, is_dir ? "/" : "");
        col += len + 2;
    }
    if (col > 0) printf("\n");
}

// Walk the directory and find matches against `prefix`. We don't keep
// the list — for unique completion we only need the LCP and one sample.
// The caller separately calls print_dir_matches() if there are multiple.
static int match_dir(unsigned int dir, const char* prefix, match_result_t* m) {
    fat12_dirent_t entries[64];
    int n = fat12_list_dir(dir, entries, 64);
    if (n < 0) return -1;
    for (int i = 0; i < n; i++) {
        if (entries[i].name[0] == '.' &&
            (entries[i].name[1] == '\0' ||
             (entries[i].name[1] == '.' && entries[i].name[2] == '\0')))
            continue;
        if (!starts_with_ci(entries[i].name, prefix)) continue;
        int is_dir = (entries[i].attr & 0x10) != 0;
        match_add(m, entries[i].name, is_dir);
    }
    return 0;
}

static int match_commands(const char* prefix, match_result_t* m) {
    for (int i = 0; commands[i]; i++) {
        if (!starts_with_ci(commands[i], prefix)) continue;
        match_add(m, commands[i], 0);
    }
    return 0;
}

// Split the partial path on its last '/'. Writes the directory portion
// to `dirpath_out` (may be empty for cwd) and the basename portion to
// `base_out`. Returns 0 on success.
static int split_partial_path(const char* partial,
                              char* dirpath_out, int dir_cap,
                              char* base_out, int base_cap) {
    int len = 0; while (partial[len]) len++;
    int last_slash = -1;
    for (int i = 0; i < len; i++) if (partial[i] == '/') last_slash = i;

    if (last_slash < 0) {
        dirpath_out[0] = '\0';
        int i = 0;
        while (partial[i] && i < base_cap - 1) { base_out[i] = partial[i]; i++; }
        base_out[i] = '\0';
        return 0;
    }

    int dir_len = last_slash == 0 ? 1 : last_slash;
    if (dir_len >= dir_cap) return -1;
    for (int i = 0; i < dir_len; i++) dirpath_out[i] = partial[i];
    dirpath_out[dir_len] = '\0';

    int base_len = len - last_slash - 1;
    if (base_len >= base_cap) return -1;
    for (int i = 0; i < base_len; i++) base_out[i] = partial[last_slash + 1 + i];
    base_out[base_len] = '\0';
    return 0;
}

// Forward decl — readline owns the buffer/anchor state we mutate.
static void redraw_line(int anchor_row, int anchor_col,
                        const char* buf, int len, int pos,
                        int prev_len);

// Given a unique match `replacement` for the current word, replace the
// portion of `buf` from `word_begin` to `pos` with `replacement` (and a
// trailing space or '/'). Updates *len, *pos, redraws the line.
static void apply_completion(char* buf, int max_line,
                             int word_begin, int* len, int* pos,
                             const char* replacement, int append_slash,
                             int anchor_row, int anchor_col) {
    int old_len = *len;
    // Tail = chars after cursor — we keep them.
    int tail_len = old_len - *pos;
    char tail[LINE_MAX];
    for (int i = 0; i < tail_len && i < LINE_MAX; i++) tail[i] = buf[*pos + i];

    int new_pos = word_begin;
    for (int i = 0; replacement[i] && new_pos < max_line; i++) {
        buf[new_pos++] = replacement[i];
    }
    char trailer = append_slash ? '/' : ' ';
    if (new_pos < max_line) buf[new_pos++] = trailer;

    // Append the saved tail.
    int new_len = new_pos;
    for (int i = 0; i < tail_len && new_len < max_line; i++) {
        buf[new_len++] = tail[i];
    }

    *len = new_len;
    *pos = new_pos;
    redraw_line(anchor_row, anchor_col, buf, *len, *pos, old_len);
}

// Just extend the current word to `extend` characters (the LCP) without
// trailing space/slash. Used when there are multiple matches but they
// share a longer prefix than what the user typed.
static void apply_extend(char* buf, int max_line,
                         int word_begin, int* len, int* pos,
                         const char* model, int extend_to,
                         int anchor_row, int anchor_col) {
    int word_typed = *pos - word_begin;
    if (extend_to <= word_typed) return;   // nothing to add

    int old_len = *len;
    int tail_len = old_len - *pos;
    char tail[LINE_MAX];
    for (int i = 0; i < tail_len && i < LINE_MAX; i++) tail[i] = buf[*pos + i];

    int new_pos = word_begin + extend_to;
    if (new_pos > max_line) new_pos = max_line;
    for (int i = word_typed; i < extend_to && word_begin + i < max_line; i++) {
        buf[word_begin + i] = model[i];
    }
    int new_len = new_pos;
    for (int i = 0; i < tail_len && new_len < max_line; i++) {
        buf[new_len++] = tail[i];
    }
    *len = new_len;
    *pos = new_pos;
    redraw_line(anchor_row, anchor_col, buf, *len, *pos, old_len);
}

// =====================================================================
// readline
// =====================================================================

// Redraw the visible line and place the cursor.
static void redraw_line(int anchor_row, int anchor_col,
                        const char* buf, int len, int pos,
                        int prev_len) {
    int width = vga_cols();
    int max_len = width - anchor_col;
    if (len > max_len) len = max_len;
    if (prev_len > max_len) prev_len = max_len;

    vga_set_cursor(anchor_row, anchor_col);
    for (int i = 0; i < len; i++) {
        vga_putchar_at_cursor(buf[i]);
        vga_set_cursor(anchor_row, anchor_col + i + 1);
    }
    for (int i = len; i < prev_len; i++) {
        vga_putchar_at_cursor(' ');
        vga_set_cursor(anchor_row, anchor_col + i + 1);
    }
    int target = anchor_col + pos;
    if (target >= width) target = width - 1;
    vga_set_cursor(anchor_row, target);
}

// Print the prompt; returns the cursor position (where the line will
// start) so readline can use it as the editing anchor.
static void print_prompt(int* row_out, int* col_out) {
    vga_set_color(VGA_WHITE, VGA_BLACK);
    printf("> ");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    vga_get_cursor(row_out, col_out);
}

// Try to expand the current word at the cursor. Modifies buf/len/pos
// and may scroll the screen (printing match lists). Returns the new
// (possibly updated) anchor row/col so the caller can keep editing in
// place.
static void handle_tab(char* buf, int max_line,
                       int* len, int* pos,
                       int* anchor_row, int* anchor_col) {
    int wb = word_start(buf, *pos);
    char word[64];
    int word_len = *pos - wb;
    if (word_len >= (int)sizeof(word)) word_len = (int)sizeof(word) - 1;
    for (int i = 0; i < word_len; i++) word[i] = buf[wb + i];
    word[word_len] = '\0';

    match_result_t m;
    match_init(&m);

    int completing_command = is_first_word(buf, wb);

    char dirpath[128];
    char base[64];
    unsigned int dir_cluster = fat12_cwd();

    if (completing_command) {
        // No "/" handling for command names — they're flat.
        match_commands(word, &m);
    } else {
        // Filename completion: split on the last '/'.
        if (split_partial_path(word, dirpath, sizeof(dirpath),
                               base, sizeof(base)) < 0) return;
        if (dirpath[0] != '\0') {
            if (fat12_resolve_dir(dirpath, &dir_cluster) < 0) return;
        }
        match_dir(dir_cluster, base, &m);
    }

    if (m.count == 0) {
        // No match — do nothing. (No bell available.)
        return;
    }

    if (m.count == 1) {
        // Unique match — replace the current word with it.
        // For files, we replace just the basename portion of the word;
        // the directory prefix stays in place. For commands, we replace
        // the whole word.
        if (completing_command) {
            apply_completion(buf, max_line, wb, len, pos,
                             m.first, /*slash=*/0,
                             *anchor_row, *anchor_col);
        } else {
            // Replacement = dirpath + (slash if dirpath nonempty and not "/") + first.
            char repl[200];
            int n = 0;
            for (int i = 0; dirpath[i] && n < (int)sizeof(repl) - 2; i++)
                repl[n++] = dirpath[i];
            // dirpath of "/" already ends in slash; otherwise we append one.
            int dirpath_is_root = (dirpath[0] == '/' && dirpath[1] == '\0');
            if (dirpath[0] != '\0' && !dirpath_is_root) repl[n++] = '/';
            for (int i = 0; m.first[i] && n < (int)sizeof(repl) - 1; i++)
                repl[n++] = m.first[i];
            repl[n] = '\0';
            apply_completion(buf, max_line, wb, len, pos,
                             repl, /*slash=*/m.first_is_dir,
                             *anchor_row, *anchor_col);
        }
        return;
    }

    // Multiple matches. First, extend the typed word to the LCP if that
    // adds anything. Then print the candidate list and redraw.
    if (m.lcp_len > (int)strlen(base) && !completing_command) {
        // For file completion the base part of the word is what we extend.
        int already = (int)strlen(base);
        int extra = m.lcp_len - already;
        // We want the word's basename portion to grow from `base` to
        // m.first[0..lcp_len]. Use apply_extend on the *full* word, but
        // only over the basename region. Easiest: rebuild the word string
        // and use apply_completion-ish logic.
        // Compute the prefix-extended word: dirpath + "/" + first[0..lcp_len].
        char repl[200];
        int n = 0;
        for (int i = 0; dirpath[i] && n < (int)sizeof(repl) - 2; i++)
            repl[n++] = dirpath[i];
        if (dirpath[0] != '\0' &&
            !(dirpath[0] == '/' && dirpath[1] == '\0'))
            repl[n++] = '/';
        for (int i = 0; i < m.lcp_len && n < (int)sizeof(repl) - 1; i++)
            repl[n++] = m.first[i];
        repl[n] = '\0';
        // Replace the whole word with the extended form, no trailer.
        // (apply_completion appends a trailer — work around with a tiny
        //  custom path.)
        int old_len = *len;
        int tail_len = old_len - *pos;
        char tail[LINE_MAX];
        for (int i = 0; i < tail_len && i < LINE_MAX; i++) tail[i] = buf[*pos + i];
        int new_pos = wb;
        for (int i = 0; repl[i] && new_pos < max_line; i++) buf[new_pos++] = repl[i];
        int new_len = new_pos;
        for (int i = 0; i < tail_len && new_len < max_line; i++) buf[new_len++] = tail[i];
        *len = new_len;
        *pos = new_pos;
        redraw_line(*anchor_row, *anchor_col, buf, *len, *pos, old_len);
        (void)extra;
    } else if (completing_command && m.lcp_len > word_len) {
        // Command LCP — same idea, simpler path (no dir prefix).
        apply_extend(buf, max_line, wb, len, pos,
                     m.first, m.lcp_len,
                     *anchor_row, *anchor_col);
    } else {
        // No further extension possible — show the list.
        // Move past current line, print matches, redraw prompt + line.
        vga_set_cursor(*anchor_row, *anchor_col + *len);
        printf("\n");
        if (completing_command) print_command_matches(word);
        else                    print_dir_matches(dir_cluster, base);
        // Reprint prompt; this becomes our new anchor.
        print_prompt(anchor_row, anchor_col);
        // Re-draw the line at the new anchor (treat prev_len = 0 since
        // the new row started blank).
        redraw_line(*anchor_row, *anchor_col, buf, *len, *pos, 0);
    }
}

static void readline(char* buf, unsigned int max,
                     int anchor_row, int anchor_col) {
    int len = 0;
    int pos = 0;
    int hist_view = 0;

    int width = vga_cols();
    int max_line = (int)max - 1;
    int row_cap  = width - anchor_col;
    if (max_line > row_cap) max_line = row_cap;
    if (max_line < 0) max_line = 0;

    while (1) {
        char c = keyboard_getchar();
        unsigned char uc = (unsigned char)c;

        if (uc == KEY_PGUP) { vga_scroll_up(vga_rows() / 2);   continue; }
        if (uc == KEY_PGDN) { vga_scroll_down(vga_rows() / 2); continue; }

        if (vga_is_scrolled()) vga_scroll_reset();

        if (c == '\n') {
            vga_set_cursor(anchor_row, anchor_col + len);
            vga_putchar('\n');
            buf[len] = '\0';
            return;
        }

        if (c == '\b') {
            if (pos > 0) {
                int old_len = len;
                for (int i = pos - 1; i < len - 1; i++) buf[i] = buf[i + 1];
                len--;
                pos--;
                redraw_line(anchor_row, anchor_col, buf, len, pos, old_len);
            }
            continue;
        }

        if (uc == KEY_DELETE) {
            if (pos < len) {
                int old_len = len;
                for (int i = pos; i < len - 1; i++) buf[i] = buf[i + 1];
                len--;
                redraw_line(anchor_row, anchor_col, buf, len, pos, old_len);
            }
            continue;
        }

        if (uc == KEY_LEFT) {
            if (pos > 0) {
                pos--;
                vga_set_cursor(anchor_row, anchor_col + pos);
            }
            continue;
        }
        if (uc == KEY_RIGHT) {
            if (pos < len) {
                pos++;
                vga_set_cursor(anchor_row, anchor_col + pos);
            }
            continue;
        }
        if (uc == KEY_HOME) {
            pos = 0;
            vga_set_cursor(anchor_row, anchor_col);
            continue;
        }
        if (uc == KEY_END) {
            pos = len;
            vga_set_cursor(anchor_row, anchor_col + pos);
            continue;
        }

        if (uc == KEY_UP) {
            if (hist_view + 1 > hist_count) continue;
            int old_len = len;
            const char* h = history_get(hist_view + 1);
            if (!h) continue;
            hist_view++;
            int n = 0;
            while (h[n] && n < max_line) { buf[n] = h[n]; n++; }
            len = n;
            pos = len;
            redraw_line(anchor_row, anchor_col, buf, len, pos, old_len);
            continue;
        }
        if (uc == KEY_DOWN) {
            if (hist_view == 0) continue;
            int old_len = len;
            hist_view--;
            if (hist_view == 0) {
                len = 0;
                pos = 0;
            } else {
                const char* h = history_get(hist_view);
                if (!h) { hist_view = 0; len = 0; pos = 0; }
                else {
                    int n = 0;
                    while (h[n] && n < max_line) { buf[n] = h[n]; n++; }
                    len = n;
                    pos = len;
                }
            }
            redraw_line(anchor_row, anchor_col, buf, len, pos, old_len);
            continue;
        }

        // ---- TAB: completion ----
        if (c == '\t') {
            handle_tab(buf, max_line, &len, &pos, &anchor_row, &anchor_col);
            // After potentially printing match lists the screen may have
            // scrolled, which can shrink our row_cap. Recompute.
            row_cap = width - anchor_col;
            if (max_line > row_cap) max_line = row_cap;
            hist_view = 0;
            continue;
        }

        if (c >= 0x20 && c <= 0x7E) {
            if (len >= max_line) continue;
            int old_len = len;
            for (int i = len; i > pos; i--) buf[i] = buf[i - 1];
            buf[pos] = c;
            len++;
            pos++;
            hist_view = 0;
            redraw_line(anchor_row, anchor_col, buf, len, pos, old_len);
            continue;
        }
    }
}

// =====================================================================
// commands
// =====================================================================

static void cmd_help(void) {
    printf("Available commands:\n");
    printf("  help          show this help\n");
    printf("  clear         clear the screen\n");
    printf("  echo <text>   print text\n");
    printf("  about         about this OS\n");
    printf("  uptime        show time since boot\n");
    printf("  sleep <ms>    pause for N milliseconds\n");
    printf("  meminfo       show physical memory map\n");
    printf("  pmemstat      show physical page allocator stats\n");
    printf("  palloc        allocate one page and print its address\n");
    printf("  vmap <virt>   resolve a virtual address to physical\n");
    printf("  kmstat        show kernel heap stats\n");
    printf("  kmtest        run a quick heap sanity test\n");
    printf("  ps            list tasks\n");
    printf("  spawn <kind>  launch a demo task: counter | spinner\n");
    printf("  yield         voluntarily yield the CPU once\n");
    printf("  preempt <on|off>   toggle preemptive scheduling\n");
    printf("  ls [<path>]   list files in a directory\n");
    printf("  cat <file>    print a file's contents\n");
    printf("  write <file> <text>  create/overwrite a file\n");
    printf("  rm <file>     delete a file\n");
    printf("  cp <src> <dst>   copy a file\n");
    printf("  mv <src> <dst>   rename or move a file/directory\n");
    printf("  mkdir <path>  create a directory\n");
    printf("  rmdir <path>  remove an empty directory\n");
    printf("  cd [<path>]   change directory (no arg = root)\n");
    printf("  pwd           print working directory\n");
    printf("  history       show command history\n");
    printf("  date          show current date and time\n");
    printf("  tz [<n>]      show or set timezone (e.g. tz PST)\n");
    printf("  user          run the ring-3 demo program\n");
    printf("  run <file>    load and run an ELF32 or flat-binary program from FAT12\n");
    printf("                  e.g.  run ECHO.ELF    run HELLO.BIN\n");
    printf("  edit [<file>]  open a file in the full-screen text editor\n");
    printf("  reboot        restart the machine\n");
    printf("  shutdown      power off (ACPI S5)\n");
    printf("Editing: Tab=complete, Up/Down=history, Left/Right=move,\n");
    printf("         Home/End=line ends, Delete=del char,\n");
    printf("         PgUp/PgDn=scroll output history.\n");
}

static void cmd_clear(void) { vga_clear(); }

static void cmd_echo(const char* args) {
    while (*args == ' ') args++;
    printf("%s\n", args);
}

static void cmd_about(void) {
    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    printf("Computer OS\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    printf("A tiny hobby kernel built from scratch.\n");
    printf("Bootloader -> protected mode -> C kernel -> IDT -> PIC -> keyboard -> shell.\n");
}

static void cmd_uptime(void) {
    unsigned int ms      = pit_millis();
    unsigned int total_s = ms / 1000;
    unsigned int hours   = total_s / 3600;
    unsigned int mins    = (total_s % 3600) / 60;
    unsigned int secs    = total_s % 60;
    unsigned int rem_ms  = ms % 1000;
    printf("Up %u:%u:%u.%u  (%u ticks, %u ms)\n",
           hours, mins, secs, rem_ms / 100,
           pit_ticks(), ms);
}

static void cmd_sleep(const char* args) {
    while (*args == ' ') args++;
    unsigned int ms = atou(args);
    if (ms == 0) { printf("usage: sleep <milliseconds>\n"); return; }
    printf("sleeping %u ms...\n", ms);
    pit_sleep(ms);
    printf("done.\n");
}

static void cmd_meminfo(void)  { memmap_print(); }
static void cmd_pmemstat(void) { pmm_print(); }

static void cmd_palloc(void) {
    unsigned int p = pmm_alloc();
    if (p == 0) { printf("pmm_alloc failed (out of memory)\n"); return; }
    printf("allocated page at 0x%x  (%u pages free)\n", p, pmm_free_pages());
}

static void cmd_vmap(const char* args) {
    while (*args == ' ') args++;
    if (*args == '\0') { printf("usage: vmap <hex-virt-addr>\n"); return; }
    if (args[0] == '0' && (args[1] == 'x' || args[1] == 'X')) args += 2;
    unsigned int v = 0;
    while (*args) {
        char c = *args++;
        unsigned int d;
        if      (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else { printf("bad hex digit\n"); return; }
        v = (v << 4) | d;
    }
    unsigned int p = vmm_resolve(v);
    if (p == 0) printf("virt 0x%x  -> NOT MAPPED\n", v);
    else        printf("virt 0x%x  -> phys 0x%x\n", v, p);
}

static void cmd_kmstat(void) { kheap_print(); }

static void cmd_kmtest(void) {
    void* a = kmalloc(64);
    void* b = kmalloc(128);
    void* c = kmalloc(256);
    printf("a=0x%x  b=0x%x  c=0x%x\n",
           (unsigned int)a, (unsigned int)b, (unsigned int)c);
    kfree(b);
    void* d = kmalloc(64);
    printf("freed b, new 64-byte alloc at d=0x%x\n", (unsigned int)d);
    kfree(a); kfree(c); kfree(d);
    printf("all freed. %u bytes free, %u blocks\n",
           kheap_free(), kheap_blocks());
}

// ---- filesystem commands ---------------------------------------------

static void cmd_ls(const char* args) {
    while (*args == ' ') args++;
    unsigned int dir;
    if (*args == '\0') {
        dir = fat12_cwd();
    } else if (fat12_resolve_dir(args, &dir) < 0) {
        printf("ls: %s: not a directory\n", args);
        return;
    }

    fat12_dirent_t entries[64];
    int n = fat12_list_dir(dir, entries, 64);
    if (n < 0) {
        printf("ls: no filesystem mounted\n");
        return;
    }
    if (n == 0) {
        printf("(empty)\n");
        return;
    }
    for (int i = 0; i < n; i++) {
        const char* tag = (entries[i].attr & 0x10) ? "<DIR>" : "     ";
        printf("  %s  %u  %s\n", tag, entries[i].size, entries[i].name);
    }
}

static void cmd_cat(const char* args) {
    while (*args == ' ') args++;
    if (*args == '\0') { printf("usage: cat <file>\n"); return; }

    enum { CAT_MAX = 8192 };
    char* buf = (char*)kmalloc(CAT_MAX);
    if (!buf) { printf("cat: out of memory\n"); return; }

    int n = fat12_read_file(args, buf, CAT_MAX);
    if (n < 0) {
        printf("cat: %s: not found\n", args);
        kfree(buf);
        return;
    }

    for (int i = 0; i < n; i++) {
        char c = buf[i];
        if (c == '\r') continue;
        vga_putchar(c);
    }
    if (n > 0 && buf[n - 1] != '\n') printf("\n");
    kfree(buf);
}

static void cmd_write(const char* args) {
    while (*args == ' ') args++;
    if (*args == '\0') { printf("usage: write <file> <text...>\n"); return; }

    char fname[128];
    int  fi = 0;
    while (*args && *args != ' ' && fi < (int)sizeof(fname) - 1) {
        fname[fi++] = *args++;
    }
    fname[fi] = '\0';
    if (fi == 0) { printf("usage: write <file> <text...>\n"); return; }

    while (*args == ' ') args++;
    const char* text = args;
    unsigned int len = 0;
    while (text[len]) len++;

    char* buf = (char*)kmalloc(len + 1);
    if (!buf) { printf("write: out of memory\n"); return; }
    for (unsigned int i = 0; i < len; i++) buf[i] = text[i];
    buf[len] = '\n';

    int n = fat12_write_file(fname, buf, len + 1);
    kfree(buf);
    if (n < 0) {
        printf("write: %s: failed (disk full, bad name, or read-only)\n", fname);
        return;
    }
    printf("wrote %d bytes to %s\n", n, fname);
}

static void cmd_rm(const char* args) {
    while (*args == ' ') args++;
    if (*args == '\0') { printf("usage: rm <file>\n"); return; }
    if (fat12_delete_file(args) < 0) {
        printf("rm: %s: not found or cannot delete\n", args);
        return;
    }
    printf("deleted %s\n", args);
}

// Strip leading whitespace from `args`, copy at most cap-1 chars of the
// next non-space token into `out`, leave *args pointing past the token.
static void take_arg(const char** args, char* out, int cap) {
    while (**args == ' ') (*args)++;
    int i = 0;
    while (**args && **args != ' ' && i < cap - 1) {
        out[i++] = **args;
        (*args)++;
    }
    out[i] = '\0';
}

static void cmd_cp(const char* args) {
    char src[128], dst[128];
    take_arg(&args, src, sizeof(src));
    take_arg(&args, dst, sizeof(dst));
    if (src[0] == '\0' || dst[0] == '\0') {
        printf("usage: cp <src> <dst>\n"); return;
    }
    if (fat12_cp(src, dst) < 0) {
        printf("cp: failed (missing source, read-only target, or out of space)\n");
        return;
    }
    printf("copied %s -> %s\n", src, dst);
}

static void cmd_mv(const char* args) {
    char src[128], dst[128];
    take_arg(&args, src, sizeof(src));
    take_arg(&args, dst, sizeof(dst));
    if (src[0] == '\0' || dst[0] == '\0') {
        printf("usage: mv <src> <dst>\n"); return;
    }
    if (fat12_mv(src, dst) < 0) {
        printf("mv: failed (missing source, target exists, or invalid path)\n");
        return;
    }
    printf("%s -> %s\n", src, dst);
}

static void cmd_mkdir(const char* args) {
    while (*args == ' ') args++;
    if (*args == '\0') { printf("usage: mkdir <path>\n"); return; }
    if (fat12_mkdir(args) < 0) {
        printf("mkdir: %s: failed (already exists, parent missing, or out of space)\n", args);
        return;
    }
    printf("created %s\n", args);
}

static void cmd_rmdir(const char* args) {
    while (*args == ' ') args++;
    if (*args == '\0') { printf("usage: rmdir <path>\n"); return; }
    if (fat12_rmdir(args) < 0) {
        printf("rmdir: %s: failed (not a directory, not empty, or in use)\n", args);
        return;
    }
    printf("removed %s\n", args);
}

static void cmd_cd(const char* args) {
    while (*args == ' ') args++;
    const char* target = (*args == '\0') ? "/" : args;
    if (fat12_chdir(target) < 0) {
        printf("cd: %s: not a directory\n", target);
    }
}

static void cmd_pwd(void) {
    char buf[128];
    int n = fat12_getcwd(buf, sizeof(buf));
    if (n < 0) printf("pwd: error\n");
    else       printf("%s\n", buf);
}

static void cmd_history(void) {
    if (hist_count == 0) { printf("(no history yet)\n"); return; }
    for (int back = hist_count; back >= 1; back--) {
        const char* line = history_get(back);
        if (line) printf("  %d  %s\n", hist_count - back + 1, line);
    }
}

static void cmd_date(void) {
    rtc_time_t t;
    rtc_read_local(&t);
    static const char* months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    const char* mon = (t.month >= 1 && t.month <= 12)
        ? months[t.month - 1] : "???";

    int off = rtc_tz_offset_minutes();
    const char* zone = rtc_tz_name_for_offset(off);

    printf("%s %u, %u   %u%u:%u%u:%u%u",
           mon, (unsigned int)t.day, t.year,
           t.hour   / 10, t.hour   % 10,
           t.minute / 10, t.minute % 10,
           t.second / 10, t.second % 10);
    if (zone) {
        printf(" %s\n", zone);
    } else if (off == 0) {
        printf(" UTC\n");
    } else {
        int sign = off < 0 ? -1 : 1;
        int abs_min = sign * off;
        printf(" UTC%c%u%u:%u%u\n",
               sign < 0 ? '-' : '+',
               (abs_min / 60) / 10, (abs_min / 60) % 10,
               (abs_min % 60) / 10, (abs_min % 60) % 10);
    }
}

static void cmd_tz(const char* args) {
    while (*args == ' ') args++;
    if (*args == '\0') {
        int off = rtc_tz_offset_minutes();
        const char* current = rtc_tz_name_for_offset(off);
        if (current) printf("Current zone: %s\n", current);
        else         printf("Current zone: UTC%+d minutes\n", off);

        printf("Available zones:\n  ");
        const char* name;
        int  minutes;
        int  col = 2;
        for (int i = 0; rtc_tz_iter(i, &name, &minutes); i++) {
            int len = 0;
            for (const char* p = name; *p; p++) len++;
            if (col + len + 2 > 76) { printf("\n  "); col = 2; }
            printf("%s  ", name);
            col += len + 2;
        }
        printf("\n");
        printf("Usage: tz <NAME>   (e.g. tz PST, tz UTC, tz JST)\n");
        return;
    }
    int minutes;
    if (!rtc_tz_lookup(args, &minutes)) {
        printf("tz: unknown zone '%s'. Type `tz` to list available zones.\n", args);
        return;
    }
    rtc_tz_set_offset_minutes(minutes);
    const char* canonical = rtc_tz_name_for_offset(minutes);
    if (!canonical) canonical = args;
    printf("Timezone set to %s.\n", canonical);
    cmd_date();
}

static void cmd_user(void) {
    int id = userprog_run_hello();
    if (id < 0) { printf("user: failed to spawn ring-3 program\n"); return; }
    printf("user: spawned ring-3 task %d\n", id);
    extern task_t* task_find_by_id(int id);
    for (int i = 0; i < 1000; i++) {
        task_t* t = task_find_by_id(id);
        if (!t || t->state == TASK_DEAD) break;
        yield();
    }
}

static void cmd_run(const char* args) {
    while (*args == ' ') args++;
    if (*args == '\0') { printf("usage: run <file>\n"); return; }

    int id = loader_run(args);
    if (id < 0) return;   // loader_run already printed the reason

    printf("[run] %s started as task %d\n", args, id);

    // Block the shell until the program is fully cleaned up. We poll on
    // loader_is_busy() rather than task state because the busy flag is
    // cleared by the reaper's on_exit hook — and that hook runs from a
    // *different* task's yield(). If we polled task state and exited the
    // loop the moment we saw TASK_DEAD, the reaper might not have run
    // yet, leaving the loader thinking a program is still loaded and
    // refusing the next `run`.
    while (loader_is_busy()) yield();
    printf("[run] %s finished\n", args);
    (void)id;
}

static void cmd_edit(const char* args) {
    while (*args == ' ') args++;
    // Pass null for "no file" so the editor opens an empty buffer and
    // prompts for a filename on save.
    editor_run(args[0] ? args : 0);
    // The editor leaves the screen in whatever state it last drew. Clear
    // it so the shell prompt comes back on a clean screen.
    vga_clear();
}

static void cmd_shutdown(void) {
    printf("Shutting down...\n");
    pit_sleep(200);
    __asm__ volatile ("cli");

    // ACPI S5 (power off): set SLP_TYP=0 and SLP_EN=1 in PM1a_CNT.
    // Port 0x604 is QEMU's PIIX4 ACPI PM1a control block.
    outw(0x604, 0x2000);

    // If still running, the port address may differ by QEMU version —
    // try the Bochs/older-QEMU address as a fallback.
    outw(0xB004, 0x2000);

    // Still here — hardware didn't honour it (real machine, wrong port).
    __asm__ volatile ("sti");
    printf("Shutdown not supported on this hardware.\n");
    printf("You can safely power off the machine.\n");
}

static void cmd_reboot(void) {
    printf("Rebooting...\n");
    pit_sleep(200);
    __asm__ volatile ("cli");
    #define REBOOT_DELAY() do { \
        for (volatile int _i = 0; _i < 1000000; _i++) { } \
    } while (0)
    volatile unsigned short* vga = (volatile unsigned short*)0xB8000;
    vga[0] = 0x0F31;
    for (int i = 0; i < 16; i++) {
        if ((inb(0x64) & 0x01) == 0) break;
        (void)inb(0x60);
    }
    for (int i = 0; i < 100000; i++) {
        if ((inb(0x64) & 0x02) == 0) break;
    }
    vga[1] = 0x0F32;
    outb(0x64, 0xFE);
    REBOOT_DELAY();
    vga[2] = 0x0F33;
    struct { unsigned short limit; unsigned int base; } __attribute__((packed))
        null_idt = { 0, 0 };
    __asm__ volatile ("lidt (%0); int $0x3" : : "r"(&null_idt));
    for (;;) __asm__ volatile ("hlt");
}

static void cmd_ps(void)    { task_list_print(); }

static void cmd_spawn(const char* args) {
    while (*args == ' ') args++;
    if (strcmp(args, "counter") == 0) {
        int id = task_spawn(demo_counter, "counter");
        if (id < 0) printf("spawn failed\n");
        else        printf("spawned task %d (counter)\n", id);
    } else if (strcmp(args, "spinner") == 0) {
        int id = task_spawn(demo_spinner, "spinner");
        if (id < 0) printf("spawn failed\n");
        else        printf("spawned task %d (spinner)\n", id);
    } else {
        printf("usage: spawn counter | spinner\n");
    }
}

static void cmd_yield(void) {
    printf("yielding...\n");
    yield();
    printf("back in %s\n", task_current()->name);
}

static void cmd_preempt(const char* args) {
    while (*args == ' ') args++;
    if (strcmp(args, "on") == 0) {
        tasking_enable_preemption();
        printf("preemption ON\n");
    } else if (strcmp(args, "off") == 0) {
        printf("(disable not supported; reboot to turn off)\n");
    } else {
        printf("usage: preempt on | off\n");
    }
}

// =====================================================================
// dispatcher
// =====================================================================

// Trim leading and trailing ASCII spaces in place. Returns a pointer
// into the same buffer (possibly advanced past leading spaces), with
// any trailing spaces overwritten by '\0'. Important because tab
// completion appends a trailing space after a unique match — without
// this, `cat README.TXT<TAB><CR>` would search for the literal name
// "README.TXT " and fail to find anything.
static char* trim_inplace(char* s) {
    while (*s == ' ') s++;
    int n = 0;
    while (s[n]) n++;
    while (n > 0 && s[n - 1] == ' ') s[--n] = '\0';
    return s;
}

static const char* split_args(char* line) {
    while (*line && *line != ' ') line++;
    if (*line == '\0') return line;
    *line = '\0';
    line++;
    return line;
}

static void execute(char* line) {
    while (*line == ' ') line++;
    if (*line == '\0') return;

    char* args_mut = (char*)split_args(line);
    args_mut = trim_inplace(args_mut);
    const char* args = args_mut;

    if      (strcmp(line, "help")     == 0) cmd_help();
    else if (strcmp(line, "clear")    == 0) cmd_clear();
    else if (strcmp(line, "echo")     == 0) cmd_echo(args);
    else if (strcmp(line, "about")    == 0) cmd_about();
    else if (strcmp(line, "uptime")   == 0) cmd_uptime();
    else if (strcmp(line, "sleep")    == 0) cmd_sleep(args);
    else if (strcmp(line, "meminfo")  == 0) cmd_meminfo();
    else if (strcmp(line, "pmemstat") == 0) cmd_pmemstat();
    else if (strcmp(line, "palloc")   == 0) cmd_palloc();
    else if (strcmp(line, "vmap")     == 0) cmd_vmap(args);
    else if (strcmp(line, "kmstat")   == 0) cmd_kmstat();
    else if (strcmp(line, "kmtest")   == 0) cmd_kmtest();
    else if (strcmp(line, "ps")       == 0) cmd_ps();
    else if (strcmp(line, "spawn")    == 0) cmd_spawn(args);
    else if (strcmp(line, "yield")    == 0) cmd_yield();
    else if (strcmp(line, "preempt")  == 0) cmd_preempt(args);
    else if (strcmp(line, "ls")       == 0) cmd_ls(args);
    else if (strcmp(line, "cat")      == 0) cmd_cat(args);
    else if (strcmp(line, "write")    == 0) cmd_write(args);
    else if (strcmp(line, "rm")       == 0) cmd_rm(args);
    else if (strcmp(line, "cp")       == 0) cmd_cp(args);
    else if (strcmp(line, "mv")       == 0) cmd_mv(args);
    else if (strcmp(line, "mkdir")    == 0) cmd_mkdir(args);
    else if (strcmp(line, "rmdir")    == 0) cmd_rmdir(args);
    else if (strcmp(line, "cd")       == 0) cmd_cd(args);
    else if (strcmp(line, "pwd")      == 0) cmd_pwd();
    else if (strcmp(line, "history")  == 0) cmd_history();
    else if (strcmp(line, "date")     == 0) cmd_date();
    else if (strcmp(line, "time")     == 0) cmd_date();
    else if (strcmp(line, "tz")       == 0) cmd_tz(args);
    else if (strcmp(line, "user")     == 0) cmd_user();
    else if (strcmp(line, "run")      == 0) cmd_run(args);
    else if (strcmp(line, "edit")     == 0) cmd_edit(args);
    else if (strcmp(line, "reboot")   == 0) cmd_reboot();
    else if (strcmp(line, "shutdown") == 0) cmd_shutdown();
    else {
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        printf("unknown command: %s\n", line);
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    }
}

void shell_run(void) {
    char line[LINE_MAX];
    char saved[LINE_MAX];

    while (1) {
        int ar, ac;
        print_prompt(&ar, &ac);
        readline(line, LINE_MAX, ar, ac);

        int i = 0;
        while (line[i] && i < LINE_MAX - 1) { saved[i] = line[i]; i++; }
        saved[i] = '\0';

        execute(line);
        history_push(saved);
    }
}