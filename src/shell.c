#include "shell.h"
#include "reboot.h"
#include "console.h"
#include "printf.h"
#include "keyboard.h"
#include "string.h"
#include "pit.h"
#include "memmap.h"
#include "pmm.h"
#include "vmm.h"
#include "stdint.h"
#include "kheap.h"
#include "task.h"
#include "vfs.h"
#include "tar.h"
#include "io.h"
#include "userprog.h"
#include "rtc.h"
#include "loader.h"
#include "editor.h"
#include "gfx.h"
#include "pci.h"
#include "e1000.h"
#include "arp.h"
#include "net.h"
#include "tcp.h"
#include "bochs_vbe.h"

#define LINE_MAX 128

// ---- command names ----------------------------------------------------
//
// Listed once here, used by both the dispatcher and tab completion. Keep
// in sync with the if/else chain in execute() at the bottom of the file.

static const char* commands[] = {
    "help", "clear", "echo", "about", "uptime", "sleep",
    "meminfo", "pmemstat", "palloc", "vmap", "kmstat", "kmtest",
    "lspci", "vbe", "nettest", "ping", "resolve", "http",
    "ps", "spawn", "yield", "preempt",
    "ls", "cat", "write", "rm", "cp", "mv", "mkdir", "rmdir",
    "cd", "pwd", "mount", "tar", "stats",
    "history", "date", "time", "tz",
    "user", "run", "edit", "reboot", "shutdown",
    "gfx",
    "gfxmouse",
    "paint",
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

static void print_dir_matches(const char* dirpath, const char* prefix) {
    vfs_dirent_t* entries = (vfs_dirent_t*)kmalloc(64 * sizeof(vfs_dirent_t));
    if (!entries) return;
    int n = vfs_list(dirpath, entries, 64);
    if (n <= 0) { kfree(entries); return; }
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
static int match_dir(const char* dirpath, const char* prefix, match_result_t* m) {
    vfs_dirent_t* entries = (vfs_dirent_t*)kmalloc(64 * sizeof(vfs_dirent_t));
    if (!entries) return -1;
    int n = vfs_list(dirpath, entries, 64);
    if (n < 0) { kfree(entries); return -1; }
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
    int width = con_cols();
    int max_len = width - anchor_col;
    if (len > max_len) len = max_len;
    if (prev_len > max_len) prev_len = max_len;

    con_set_cursor(anchor_row, anchor_col);
    for (int i = 0; i < len; i++) {
        con_putchar_at_cursor(buf[i]);
        con_set_cursor(anchor_row, anchor_col + i + 1);
    }
    for (int i = len; i < prev_len; i++) {
        con_putchar_at_cursor(' ');
        con_set_cursor(anchor_row, anchor_col + i + 1);
    }
    int target = anchor_col + pos;
    if (target >= width) target = width - 1;
    con_set_cursor(anchor_row, target);
}

// Print the prompt; returns the cursor position (where the line will
// start) so readline can use it as the editing anchor.
static void print_prompt(int* row_out, int* col_out) {
    con_set_color(CON_WHITE, CON_BLACK);
    printf("> ");
    con_set_color(CON_LIGHT_GREY, CON_BLACK);
    con_get_cursor(row_out, col_out);
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
    dirpath[0] = '\0';   // empty = current working directory

    if (completing_command) {
        // No "/" handling for command names — they're flat.
        match_commands(word, &m);
    } else {
        // Filename completion: split on the last '/'.
        if (split_partial_path(word, dirpath, sizeof(dirpath),
                               base, sizeof(base)) < 0) return;
        // dirpath now holds the directory portion ("" means cwd). The VFS
        // resolves it itself, so we just pass the path down.
        match_dir(dirpath, base, &m);
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
        con_set_cursor(*anchor_row, *anchor_col + *len);
        printf("\n");
        if (completing_command) print_command_matches(word);
        else                    print_dir_matches(dirpath, base);
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

    int width = con_cols();
    int max_line = (int)max - 1;
    int row_cap  = width - anchor_col;
    if (max_line > row_cap) max_line = row_cap;
    if (max_line < 0) max_line = 0;

    while (1) {
        char c = keyboard_getchar();
        unsigned char uc = (unsigned char)c;

        if (uc == KEY_PGUP) { con_scroll_up(con_rows() / 2);   continue; }
        if (uc == KEY_PGDN) { con_scroll_down(con_rows() / 2); continue; }

        if (con_is_scrolled()) con_scroll_reset();

        if (c == '\n') {
            con_set_cursor(anchor_row, anchor_col + len);
            con_putchar('\n');
            buf[len] = '\0';
            return;
        }

        if ((unsigned char)c == 0x89) {  /* KEY_SHIFT_ENTER */
            if (len < (int)max - 2) {
                for (int i = len; i > pos; i--) buf[i] = buf[i-1];
                buf[pos] = '\n'; len++; pos++;
                con_putchar('\n'); printf("  ");
                anchor_row++; anchor_col = 2;
                row_cap = width - anchor_col;
                for (int i = pos; i < len; i++) con_putchar(buf[i]);
            }
            continue;
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
                con_set_cursor(anchor_row, anchor_col + pos);
            }
            continue;
        }
        if (uc == KEY_RIGHT) {
            if (pos < len) {
                pos++;
                con_set_cursor(anchor_row, anchor_col + pos);
            }
            continue;
        }
        if (uc == KEY_HOME) {
            pos = 0;
            con_set_cursor(anchor_row, anchor_col);
            continue;
        }
        if (uc == KEY_END) {
            pos = len;
            con_set_cursor(anchor_row, anchor_col + pos);
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

/* ------------------------------------------------------------------ */
/* tar                                                                  */
/* ------------------------------------------------------------------ */
/*
 * Usage:
 *   tar -tf <file>           list contents
 *   tar -xf <file>           extract to /
 *   tar -xf <file> -C <dir>  extract to <dir>
 *   tar -xvf <file>          extract verbosely
 */
static void cmd_tar(const char* args) {
    while (*args == ' ') args++;
    if (!args[0]) {
        printf("usage: tar [-tv|-xv]f <file.tar> [-C <dir>]\n");
        return;
    }

    int do_extract = 0;
    int do_list    = 0;
    int verbose    = 0;
    const char* src     = 0;
    const char* dst_dir = "/tmp";  /* safe default — use -C to override */

    /* Parse flags and arguments. */
    char a0[32];
    const char* p = args;
    unsigned int ai = 0;
    while (*p && *p != ' ' && ai < sizeof(a0) - 1) a0[ai++] = *p++;
    a0[ai] = '\0';

    /* Flags can be "-xvf", "-tf", "xvf", "tf", etc. */
    const char* flags = a0;
    if (flags[0] == '-') flags++;

    for (const char* f = flags; *f; f++) {
        if (*f == 'x') do_extract = 1;
        else if (*f == 't') do_list = 1;
        else if (*f == 'v') verbose  = 1;
        else if (*f == 'f') {
            /* Next token is the filename. */
            while (*p == ' ') p++;
            src = p;
            /* Advance p past the filename. */
            while (*p && *p != ' ') p++;
        }
    }

    /* If -f was not in the flags string, the next bare arg is the file. */
    if (!src) {
        while (*p == ' ') p++;
        src = p;
        while (*p && *p != ' ') p++;
    }

    /* Look for -C <dir>. */
    while (*p == ' ') p++;
    if (p[0] == '-' && p[1] == 'C') {
        p += 2;
        while (*p == ' ') p++;
        if (*p) dst_dir = p;
    }

    if (!src || src[0] == '\0') {
        printf("tar: no archive specified\n");
        return;
    }

    /* Copy src to a null-terminated local buffer (it may run to next arg). */
    char srcbuf[256];
    unsigned int si = 0;
    while (src[si] && src[si] != ' ' && si < sizeof(srcbuf) - 1) {
        srcbuf[si] = src[si]; si++;
    }
    srcbuf[si] = '\0';

    if (do_list) {
        int n = tar_list(srcbuf);
        if (n < 0) return;
        con_set_color(CON_LIGHT_GREEN, CON_BLACK);
        printf("\n%d entries\n", n);
        con_set_color(CON_WHITE, CON_BLACK);
    } else if (do_extract) {
        int n = tar_extract(srcbuf, dst_dir, verbose);
        if (n < 0) return;
        con_set_color(CON_LIGHT_GREEN, CON_BLACK);
        printf("\nextracted %d entries to %s\n", n, dst_dir);
        con_set_color(CON_WHITE, CON_BLACK);
    } else {
        printf("tar: specify -t (list) or -x (extract)\n");
    }
}

static void cmd_help(void) {
    con_set_color(CON_LIGHT_GREEN, CON_BLACK);
    printf("Computer OS help\n");
    con_set_color(CON_LIGHT_GREY, CON_BLACK);
    printf("\n");

    printf("General:\n");
    printf("  help                 show this help screen\n");
    printf("  about                show OS version and features\n");
    printf("  clear                clear the screen\n");
    printf("  echo <text>          print text\n");
    printf("  uptime               show time since boot\n");
    printf("  sleep <ms>           pause for N milliseconds\n");
    printf("  reboot               restart the machine\n");
    printf("  shutdown             power off using ACPI S5\n");
    printf("\n");

    printf("Files and disks:\n");
    printf("  mount                show mounted filesystems\n");
    printf("  ls [path]            list files in a directory\n");
    printf("  cat <file>           print a file\n");
    printf("  write <file> <text>  create or overwrite a file\n");
    printf("  rm <file>            delete a file\n");
    printf("  cp <src> <dst>       copy a file\n");
    printf("  mv <src> <dst>       rename or move a file/directory\n");
    printf("  mkdir <path>         create a directory\n");
    printf("  rmdir <path>         remove an empty directory\n");
    printf("  cd [path]            change directory; no arg goes to /\n");
    printf("  pwd                  print working directory\n");
    printf("  tar [-tv|-xv]f <f>   list or extract a .tar archive\n");
    printf("\n");

    printf("Programs and tasks:\n");
    printf("  run <file>           load and run an ELF64 or flat binary\n");
    printf("  user                 run the built-in ring-3 demo\n");
    printf("  ps                   list tasks\n");
    printf("  spawn <kind>         start demo task: counter | spinner\n");
    printf("  yield                voluntarily yield the CPU once\n");
    printf("  preempt <on|off>     toggle preemptive scheduling\n");
    printf("\n");

    printf("Memory and kernel debug:\n");
    printf("  meminfo              show Limine memory map summary\n");
    printf("  pmemstat             show physical page allocator stats\n");
    printf("  palloc               allocate one physical page\n");
    printf("  vmap <virt>          resolve virtual address to physical\n");
    printf("  kmstat               show kernel heap stats\n");
    printf("  kmtest               run a heap sanity test\n");
    printf("\n");

    printf("Hardware and networking:\n");
    printf("  lspci                list PCI devices\n");
    printf("  nettest              show network interface state\n");
    printf("  ping <ip>            send a simple ping/ARP test\n");
    printf("  resolve <host>       resolve a built-in/test hostname\n");
    printf("  http <host/path>     send a tiny HTTP test request\n");
    printf("\n");

    printf("Graphics and editor:\n");
    printf("  vbe                  switch to 1024x768x32 framebuffer test\n");
    printf("  gfx                  draw graphics demo\n");
    printf("  gfxmouse             mouse graphics demo\n");
    printf("  paint                open paint program\n");
    printf("  edit [file]          open text editor\n");
    printf("\n");

    printf("Time:\n");
    printf("  date                 show current date and time\n");
    printf("  time                 same as date\n");
    printf("  tz [name|offset]     show or set timezone, e.g. tz PST\n");
    printf("\n");

    printf("Shell controls:\n");
    printf("  Tab                  autocomplete command/file\n");
    printf("  Up/Down              command history\n");
    printf("  Left/Right           move cursor\n");
    printf("  Home/End             jump to line start/end\n");
    printf("  Delete/Backspace     delete characters\n");
    printf("  PgUp/PgDn            scroll output history\n");
}

static void cmd_clear(void) { con_clear(); }

static void cmd_echo(const char* args) {
    while (*args == ' ') args++;

    const char* redir = 0; int append = 0;
    const char* p = args;
    while (*p) { if (*p == '>') { redir = p; append = (*(p+1) == '>'); break; } p++; }

    char text[512]; unsigned int tlen = 0;
    if (redir) {
        const char* end = redir;
        while (end > args && *(end-1) == ' ') end--;
        for (const char* q = args; q < end && tlen < sizeof(text)-2; q++) text[tlen++] = *q;
    } else {
        for (const char* q = args; *q && tlen < sizeof(text)-2; q++) text[tlen++] = *q;
    }
    text[tlen++] = '\n'; text[tlen] = '\0';

    /* Strip surrounding quotes */
    if (tlen >= 3 && text[0] == '"' && text[tlen-2] == '"') {
        for (unsigned int i = 0; i < tlen-2; i++) text[i] = text[i+1];
        tlen -= 2; text[tlen-1] = '\n'; text[tlen] = '\0';
    }

    if (redir) {
        const char* fname = redir + (append ? 2 : 1);
        while (*fname == ' ') fname++;
        if (*fname == '\0') { printf("echo: missing filename\n"); return; }
        if (append) {
            char* existing = (char*)kmalloc(8192);
            if (!existing) { printf("echo: out of memory\n"); return; }
            int en = vfs_read_file(fname, existing, 8191);
            if (en < 0) en = 0;
            char* combined = (char*)kmalloc(en + tlen + 1);
            if (!combined) { kfree(existing); printf("echo: out of memory\n"); return; }
            for (int i = 0; i < en; i++) combined[i] = existing[i];
            for (unsigned int i = 0; i < tlen; i++) combined[en+i] = text[i];
            vfs_write_file(fname, combined, (unsigned int)(en + tlen));
            kfree(existing); kfree(combined);
        } else {
            vfs_write_file(fname, text, tlen);
        }
    } else {
        const char* out = args;
        if (*out == '"') { out++; while (*out && *out != '"') con_putchar(*out++); printf("\n"); }
        else printf("%s\n", args);
    }
}

static void cmd_about(void) {
    con_set_color(CON_LIGHT_GREEN, CON_BLACK);
    printf("Computer OS 64-bit\n");
    con_set_color(CON_LIGHT_GREY, CON_BLACK);
    printf("A tiny x86_64 hobby OS built from scratch in C and assembly.\n");
    printf("Boot path: Limine UEFI -> long mode -> higher-half kernel -> shell.\n");
    printf("\n");

    con_set_color(CON_LIGHT_GREEN, CON_BLACK);
    printf("Core features:\n");
    con_set_color(CON_LIGHT_GREY, CON_BLACK);
    printf("  - 64-bit kernel with 4-level paging\n");
    printf("  - GDT, IDT, PIC, IRQ/ISR handlers, PIT timer\n");
    printf("  - PMM, VMM, kernel heap, tasking, syscalls\n");
    printf("  - Limine framebuffer / VBE console with colored status lines\n");
    printf("  - ATA PIO disk, FAT12 root filesystem, ramfs at /tmp\n");
    printf("  - PCI scan, e1000 network driver, ARP/IP/TCP experiments\n");
    printf("  - Shell, editor, graphics demos, paint, and user programs\n");
    printf("\n");

    printf("Status colors: ");
    con_set_color(CON_LIGHT_GREEN, CON_BLACK);
    printf("[OK]");
    con_set_color(CON_LIGHT_GREY, CON_BLACK);
    printf(" means working, ");
    con_set_color(CON_LIGHT_RED, CON_BLACK);
    printf("[!!]");
    con_set_color(CON_LIGHT_GREY, CON_BLACK);
    printf(" means missing or failed.\n");
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
static void cmd_pmemstat(void) {
    printf("PMM: %llu pages free (%llu MB), %llu total\n",
           pmm_free_pages(), pmm_free_pages() * 4096 / (1024*1024),
           pmm_total_pages());
}

static void cmd_palloc(void) {
    uint64_t p = pmm_alloc();
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

// ---- PCI -------------------------------------------------------------
// Print one 16-bit value as exactly 4 zero-padded hex digits. The kernel
// printf has no width specifiers, so we lay the nibbles out by hand to keep
// the vendor:device column aligned.
static void print_hex16(unsigned short v) {
    const char* digits = "0123456789ABCDEF";
    char out[5];
    out[0] = digits[(v >> 12) & 0xF];
    out[1] = digits[(v >> 8)  & 0xF];
    out[2] = digits[(v >> 4)  & 0xF];
    out[3] = digits[v & 0xF];
    out[4] = '\0';
    printf("%s", out);
}

static void cmd_lspci(void) {
    int n = pci_device_count();
    if (n == 0) {
        printf("No PCI devices found.\n");
        return;
    }
    printf("%d PCI device%s:\n", n, n == 1 ? "" : "s");
    printf("  B:D.F   VEND:DEV   CLASS  IRQ  DESCRIPTION\n");
    for (int i = 0; i < n; i++) {
        const pci_device_t* d = pci_get_device(i);
        printf("  %u:%u.%u   ", d->bus, d->device, d->function);
        print_hex16(d->vendor_id);
        printf(":");
        print_hex16(d->device_id);
        printf("  %u/%u", d->class_code, d->subclass);
        // IRQ 0xFF means "no legacy interrupt line assigned".
        if (d->irq_line == 0xFF) printf("   --");
        else                     printf("   %u", d->irq_line);
        printf("   %s\n", pci_class_name(d->class_code, d->subclass));
    }
}

// ---- e1000 / ARP network test --------------------------------------------
// Resolves the SLIRP gateway (10.0.2.2) via ARP: broadcast a "who-has"
// request and wait for the reply. Success printing the gateway's MAC proves
// the whole path — descriptor rings, Ethernet framing, ARP — end to end.
static void cmd_nettest(void) {
    unsigned char mac[6];
    e1000_get_mac(mac);
    printf("our MAC %x:%x:%x:%x:%x:%x  (IP 10.0.2.15)\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    unsigned int gw = 0x0A000202u;   // 10.0.2.2, the SLIRP gateway
    printf("ARP: who has 10.0.2.2 ?\n");
    if (!arp_request(gw)) {
        printf("ARP: send failed (TX ring full?)\n");
        return;
    }

    printf("ARP: waiting for reply ~2s...\n");
    unsigned char gw_mac[6];
    unsigned int gw_ip;
    for (int t = 0; t < 200; t++) {          // 200 * 10ms = 2s
        if (arp_poll_reply(gw_mac, &gw_ip)) {
            printf("ARP reply: %u.%u.%u.%u is at %x:%x:%x:%x:%x:%x\n",
                   (gw_ip >> 24) & 0xFF, (gw_ip >> 16) & 0xFF,
                   (gw_ip >> 8) & 0xFF, gw_ip & 0xFF,
                   gw_mac[0], gw_mac[1], gw_mac[2],
                   gw_mac[3], gw_mac[4], gw_mac[5]);
            printf("nettest: SUCCESS - rings + Ethernet + ARP all work.\n");
            return;
        }
        pit_sleep(10);
    }
    printf("ARP: no reply received\n");
    printf("nettest done.\n");
}

// Parse a dotted-quad like "10.0.2.2" into a host-order IP. Returns 0 on bad
// input. Empty/NULL yields the gateway default.
static unsigned int parse_ip(const char* s) {
    if (!s || !*s) return NET_GATEWAY;
    unsigned int parts[4] = {0, 0, 0, 0};
    int pi = 0, seen = 0;
    unsigned int cur = 0;
    for (const char* p = s; ; p++) {
        if (*p >= '0' && *p <= '9') {
            cur = cur * 10 + (unsigned int)(*p - '0');
            if (cur > 255) return 0;
            seen = 1;
        } else if (*p == '.' || *p == '\0') {
            if (!seen || pi > 3) return 0;
            parts[pi++] = cur;
            cur = 0; seen = 0;
            if (*p == '\0') break;
        } else {
            return 0;   // invalid character
        }
    }
    if (pi != 4) return 0;
    return (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
}

static void cmd_ping(const char* args) {
    if (!args || !*args) { ping(NET_GATEWAY, 4, 1000); return; }

    // A leading digit means a dotted-quad; otherwise treat it as a hostname
    // and resolve it via DNS first.
    unsigned int ip;
    if (args[0] >= '0' && args[0] <= '9') {
        ip = parse_ip(args);
        if (ip == 0) { printf("usage: ping [a.b.c.d | hostname]\n"); return; }
    } else {
        printf("resolving %s ...\n", args);
        if (!dns_resolve(args, &ip)) {
            printf("ping: could not resolve %s\n", args);
            return;
        }
        printf("%s -> %u.%u.%u.%u\n", args,
               (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
    }
    ping(ip, 4, 1000);
}

static void cmd_resolve(const char* args) {
    if (!args || !*args) { printf("usage: resolve <hostname>\n"); return; }
    unsigned int ip;
    if (!dns_resolve(args, &ip)) {
        printf("could not resolve %s\n", args);
        return;
    }
    printf("%s -> %u.%u.%u.%u\n", args,
           (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
}

// Fetch http://<host>/ over TCP port 80 and print the first chunk of the
// response. Proves the TCP client end to end: DNS -> connect -> send -> recv.
static void cmd_http(const char* args) {
    if (!args || !*args) { printf("usage: http <hostname>\n"); return; }

    unsigned int ip;
    if (args[0] >= '0' && args[0] <= '9') {
        ip = parse_ip(args);
        if (!ip) { printf("bad address\n"); return; }
    } else {
        printf("resolving %s ...\n", args);
        if (!dns_resolve(args, &ip)) { printf("http: could not resolve %s\n", args); return; }
        printf("%s -> %u.%u.%u.%u\n", args,
               (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
    }

    printf("connecting to port 80...\n");
    tcp_conn_t* c = tcp_connect(ip, 80);
    if (!c) { printf("http: connect failed\n"); return; }
    printf("connected. sending GET.\n");

    // Build a minimal HTTP/1.0 request. Host header uses the literal arg.
    char req[256];
    int n = 0;
    const char* g = "GET / HTTP/1.0\r\nHost: ";
    for (const char* p = g; *p; p++) req[n++] = *p;
    for (const char* p = args; *p; p++) req[n++] = *p;
    const char* tail = "\r\nConnection: close\r\n\r\n";
    for (const char* p = tail; *p; p++) req[n++] = *p;

    if (tcp_send(c, req, (unsigned int)n) < 0) { printf("http: send failed\n"); tcp_close(c); return; }

    printf("--- response ---\n");
    char buf[1024];
    int total = 0;
    for (;;) {
        int r = tcp_recv(c, buf, sizeof(buf) - 1, 3000);
        if (r > 0) {
            buf[r] = '\0';
            printf("%s", buf);
            total += r;
            if (total > 4000) { printf("\n...(truncated)\n"); break; }
        } else if (r < 0) {
            break;   // peer closed
        } else {
            break;   // timeout
        }
    }
    printf("\n--- done (%d bytes) ---\n", total);
    tcp_close(c);
}

// ---- Bochs VBE framebuffer test ------------------------------------------
// Sets 1024x768x32 and paints a test pattern straight into the linear
// framebuffer: vertical RGB gradient bars and a white border. This proves the
// mode-set and the PCI-derived framebuffer address before we move the whole
// text console onto the framebuffer. NOTE: once this runs, VGA text mode is
// gone, so the shell prompt won't come back until you reboot.
static void cmd_vbe(void) {
    if (!bochs_vbe_available()) {
        printf("Bochs/QEMU VBE adapter not present.\n");
        return;
    }
    printf("Setting 1024x768x32 and drawing a test pattern...\n");
    printf("(VGA text mode will be replaced; reboot to return to the shell.)\n");

    bochs_vbe_mode_t m;
    if (!bochs_vbe_set_mode(1024, 768, &m)) {
        printf("VBE mode-set failed; still in text mode.\n");
        return;
    }

    // Each pixel is a 32-bit little-endian XRGB value: 0x00RRGGBB.
    volatile unsigned int* fb = (volatile unsigned int*)m.virt;
    unsigned int stride = m.pitch / 4;   // pixels per row
    for (unsigned int y = 0; y < m.height; y++) {
        for (unsigned int x = 0; x < m.width; x++) {
            unsigned char r = (unsigned char)(x * 255 / m.width);
            unsigned char g = (unsigned char)(y * 255 / m.height);
            unsigned char b = (unsigned char)((x ^ y) & 0xFF);
            unsigned int px = ((unsigned int)r << 16) |
                              ((unsigned int)g << 8)  |
                               (unsigned int)b;
            // White 2px border so the screen edges are unmistakable.
            if (x < 2 || y < 2 || x >= m.width - 2u || y >= m.height - 2u)
                px = 0x00FFFFFF;
            fb[y * stride + x] = px;
        }
    }
}

static void cmd_kmtest(void) {
    void* a = kmalloc(64);
    void* b = kmalloc(128);
    void* c = kmalloc(256);
    printf("a=0x%llx  b=0x%llx  c=0x%llx\n",
           (uint64_t)a, (uint64_t)b, (uint64_t)c);
    kfree(b);
    void* d = kmalloc(64);
    printf("freed b, new 64-byte alloc at d=0x%llx\n", (uint64_t)d);
    kfree(a); kfree(c); kfree(d);
    printf("all freed. %u bytes free, %u blocks\n",
           kheap_free(), kheap_blocks());
}

// ---- filesystem commands ---------------------------------------------

static void cmd_mount(void) {
    int shown = 0;
    for (int i = 0; ; i++) {
        const vfs_mount_t* m = vfs_mount_at(i);
        if (i >= 8) break;
        if (!m) continue;
        printf("  %-8s on %s\n", m->name, m->prefix);
        shown++;
    }
    if (!shown) printf("(no filesystems mounted)\n");
}

static void cmd_ls(const char* args) {
    while (*args == ' ') args++;

    int flag_l = 0, flag_h = 0, flag_a = 0;
    while (*args == '-') {
        args++;
        while (*args && *args != ' ') {
            if (*args == 'l') flag_l = 1;
            if (*args == 'h') flag_h = 1;
            if (*args == 'a') flag_a = 1;
            args++;
        }
        while (*args == ' ') args++;
    }

    vfs_dirent_t* entries = (vfs_dirent_t*)kmalloc(64 * sizeof(vfs_dirent_t));
    if (!entries) { printf("ls: out of memory\n"); return; }

    int n = vfs_list(args, entries, 64);
    if (n < 0) {
        kfree(entries);
        printf("ls: %s: not a directory\n", (*args ? args : "."));
        return;
    }
    if (n == 0) { kfree(entries); printf("(empty)\n"); return; }

    char size_buf[16];

    if (flag_l) {
        for (int i = 0; i < n; i++) {
            int is_dir = (entries[i].attr & 0x10);
            if (!flag_a && entries[i].name[0] == '.') continue;
            if (flag_h) {
                unsigned int sz = entries[i].size;
                char* sp = size_buf;
                unsigned int sv; char tmp[10]; int tl = 0;
                if (sz >= 1024*1024) { sv = sz/(1024*1024); }
                else if (sz >= 1024) { sv = sz/1024; }
                else                 { sv = sz; }
                if (sv == 0) tmp[tl++] = '0';
                else { unsigned int t=sv; while(t){tmp[tl++]='0'+t%10;t/=10;} }
                for(int ti=tl-1;ti>=0;ti--) *sp++=tmp[ti];
                if      (sz>=1024*1024) *sp++='M';
                else if (sz>=1024)      *sp++='K';
                else                    *sp++='B';
                *sp = '\0';
            } else {
                char* sp = size_buf; unsigned int sz = entries[i].size;
                char tmp[12]; int tl = 0;
                if (sz==0) tmp[tl++]='0';
                else { unsigned int t=sz; while(t){tmp[tl++]='0'+t%10;t/=10;} }
                for(int ti=tl-1;ti>=0;ti--) *sp++=tmp[ti];
                *sp = '\0';
            }
            con_set_color(is_dir ? CON_LIGHT_CYAN : CON_LIGHT_GREY, CON_BLACK);
            printf("%s  %-8s  %s\n", is_dir ? "d" : "-", size_buf, entries[i].name);
        }
        con_set_color(CON_LIGHT_GREY, CON_BLACK);
    } else {
        int cols_avail = con_cols();
        if (cols_avail < 20) cols_avail = 20;
        int max_name = 0;
        for (int i = 0; i < n; i++) {
            if (!flag_a && entries[i].name[0] == '.') continue;
            int len = (int)strlen(entries[i].name);
            if (len > max_name) max_name = len;
        }
        int col_w = max_name + 2;
        int num_cols = cols_avail / col_w;
        if (num_cols < 1) num_cols = 1;
        int col = 0;
        for (int i = 0; i < n; i++) {
            if (!flag_a && entries[i].name[0] == '.') continue;
            int is_dir = (entries[i].attr & 0x10);
            con_set_color(is_dir ? CON_LIGHT_CYAN : CON_LIGHT_GREY, CON_BLACK);
            int len = (int)strlen(entries[i].name);
            printf("%s", entries[i].name);
            if (col < num_cols - 1)
                for (int s = len; s < col_w; s++) printf(" ");
            col++;
            if (col >= num_cols) { printf("\n"); col = 0; }
        }
        if (col > 0) printf("\n");
        con_set_color(CON_LIGHT_GREY, CON_BLACK);
    }
    kfree(entries);
}

static int cat_readline(char* buf, int max) {
    int pos = 0;
    while (pos < max - 1) {
        char c = keyboard_getchar();
        if (c == '\r' || c == '\n') { buf[pos] = '\0'; printf("\n"); return pos; }
        if (c == '\b' || c == 127) { if (pos > 0) { pos--; printf("\b \b"); } continue; }
        buf[pos++] = c; con_putchar(c);
    }
    buf[pos] = '\0'; return pos;
}

static void cmd_cat(const char* args) {
    while (*args == ' ') args++;

    if (args[0] == '<' && args[1] == '<') {
        const char* p = args + 2;
        while (*p == ' ') p++;
        char delim[32]; int di = 0;
        while (*p && *p != ' ' && *p != '>' && di < 31) delim[di++] = *p++;
        delim[di] = '\0';
        while (*p == ' ') p++;
        int append = 0; const char* outfile = 0;
        if (*p == '>') { p++; if (*p == '>') { append = 1; p++; } while (*p == ' ') p++; if (*p) outfile = p; }
        char* collected = (char*)kmalloc(16384);
        if (!collected) { printf("cat: out of memory\n"); return; }
        int clen = 0;
        char line[256];
        while (1) {
            printf("> ");
            int llen = cat_readline(line, sizeof(line));
            if (strcmp(line, delim) == 0) break;
            for (int i = 0; i < llen && clen < 16383; i++) collected[clen++] = line[i];
            if (clen < 16383) collected[clen++] = '\n';
        }
        collected[clen] = '\0';
        if (outfile) {
            if (append) {
                char* existing = (char*)kmalloc(16384);
                int en = existing ? vfs_read_file(outfile, existing, 16383) : -1;
                if (en < 0) en = 0;
                char* combined = (char*)kmalloc(en + clen + 1);
                if (combined) {
                    for (int i = 0; i < en; i++) combined[i] = existing[i];
                    for (int i = 0; i < clen; i++) combined[en+i] = collected[i];
                    vfs_write_file(outfile, combined, (unsigned int)(en + clen));
                    kfree(combined);
                }
                if (existing) kfree(existing);
            } else { vfs_write_file(outfile, collected, (unsigned int)clen); }
        } else { for (int i = 0; i < clen; i++) con_putchar(collected[i]); }
        kfree(collected); return;
    }

    if (*args == '\0') { printf("usage: cat <file> [> outfile]\n"); return; }

    char fname[256]; int fi = 0;
    const char* p = args;
    while (*p && *p != '>' && fi < 255) { fname[fi++] = *p++; }
    while (fi > 0 && fname[fi-1] == ' ') fi--;
    fname[fi] = '\0';
    int append = 0; const char* outfile = 0;
    if (*p == '>') { p++; if (*p == '>') { append = 1; p++; } while (*p == ' ') p++; if (*p) outfile = p; }

    unsigned int cat_max = 1024 * 1024;
    char* buf = (char*)kmalloc(cat_max);
    if (!buf) { printf("cat: out of memory\n"); return; }
    int n = vfs_read_file(fname, buf, cat_max);
    if (n < 0) { printf("cat: %s: not found\n", fname); kfree(buf); return; }

    if (outfile) {
        if (append) {
            char* existing = (char*)kmalloc(cat_max);
            int en = existing ? vfs_read_file(outfile, existing, cat_max-1) : -1;
            if (en < 0) en = 0;
            char* combined = (char*)kmalloc(en + n + 1);
            if (combined) {
                for (int i = 0; i < en; i++) combined[i] = existing[i];
                for (int i = 0; i < n; i++) combined[en+i] = buf[i];
                vfs_write_file(outfile, combined, (unsigned int)(en + n));
                kfree(combined);
            }
            if (existing) kfree(existing);
        } else { vfs_write_file(outfile, buf, (unsigned int)n); }
    } else {
        for (int i = 0; i < n; i++) { char c = buf[i]; if (c == '\r') continue; con_putchar(c); }
        if (n > 0 && buf[n-1] != '\n') printf("\n");
    }
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

    int n = vfs_write_file(fname, buf, len + 1);
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
    if (vfs_delete_file(args) < 0) {
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
    if (vfs_cp(src, dst) < 0) {
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
    if (vfs_mv(src, dst) < 0) {
        printf("mv: failed (missing source, target exists, or invalid path)\n");
        return;
    }
    printf("%s -> %s\n", src, dst);
}

static void cmd_mkdir(const char* args) {
    while (*args == ' ') args++;
    if (*args == '\0') { printf("usage: mkdir <path>\n"); return; }
    if (vfs_mkdir(args) < 0) {
        printf("mkdir: %s: failed (already exists, parent missing, or out of space)\n", args);
        return;
    }
    printf("created %s\n", args);
}

static void cmd_rmdir(const char* args) {
    while (*args == ' ') args++;
    if (*args == '\0') { printf("usage: rmdir <path>\n"); return; }
    if (vfs_rmdir(args) < 0) {
        printf("rmdir: %s: failed (not a directory, not empty, or in use)\n", args);
        return;
    }
    printf("removed %s\n", args);
}

static void cmd_cd(const char* args) {
    while (*args == ' ') args++;
    const char* target = (*args == '\0') ? "/" : args;
    if (vfs_chdir(target) < 0) {
        printf("cd: %s: not a directory\n", target);
    }
}

static void cmd_pwd(void) {
    char buf[128];
    int n = vfs_getcwd(buf, sizeof(buf));
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

    // Block the shell until the program finishes. We poll the specific
    // task id: once the task has exited we collect it (mark its zombie
    // collectable so the reaper frees it) and stop. yield() drives the
    // scheduler so the user task actually gets to run.
    extern task_t* task_find_by_id(int id);
    int code = 0;
    for (;;) {
        task_t* t = task_find_by_id(id);
        if (!t) break;                 // already gone (e.g. orphan-reaped)
        if (t->has_exited) {
            code = t->exit_code;
            t->reaped_by_parent = 1;   // let the reaper free it
            break;
        }
        yield();
    }
    printf("[run] %s finished (exit %d)\n", args, code);
    (void)id;
}

static void cmd_edit(const char* args) {
    while (*args == ' ') args++;
    // Pass null for "no file" so the editor opens an empty buffer and
    // prompts for a filename on save.
    editor_run(args[0] ? args : 0);
    // The editor leaves the screen in whatever state it last drew. Clear
    // it so the shell prompt comes back on a clean screen.
    con_clear();
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
// stats
// =====================================================================

extern unsigned int fat32_free_clusters(void);
extern unsigned int fat32_total_clusters(void);
extern unsigned int fat32_cluster_size(void);

static void print_bar(unsigned int used, unsigned int total, int width) {
    if (total == 0) { printf("[--------------------]"); return; }
    int filled = (int)((unsigned long long)used * width / total);
    printf("[");
    for (int i = 0; i < width; i++)
        printf(i < filled ? "#" : "-");
    printf("]");
}

static void print_mb(unsigned long long bytes) {
    unsigned long long mb = bytes / (1024 * 1024);
    unsigned long long kb = (bytes % (1024 * 1024)) / 1024;
    if (mb >= 1024) {
        printf("%llu GB", mb / 1024);
    } else if (mb > 0) {
        printf("%llu MB", mb);
    } else {
        printf("%llu KB", kb);
    }
}

static void cmd_stats(void) {
    con_set_color(CON_LIGHT_CYAN, CON_BLACK);
    printf("=== System Stats ===\n");
    con_set_color(CON_LIGHT_GREY, CON_BLACK);

    /* --- Date/Time --- */
    {
        rtc_time_t t;
        rtc_read_local(&t);
        static const char* months[] = {
            "Jan","Feb","Mar","Apr","May","Jun",
            "Jul","Aug","Sep","Oct","Nov","Dec"
        };
        const char* mon = (t.month >= 1 && t.month <= 12)
            ? months[t.month - 1] : "???";
        int off = rtc_tz_offset_minutes();
        const char* zone = rtc_tz_name_for_offset(off);
        printf("  Date/Time : %s %u, %u  %u%u:%u%u:%u%u %s\n",
               mon, (unsigned int)t.day, t.year,
               t.hour/10, t.hour%10,
               t.minute/10, t.minute%10,
               t.second/10, t.second%10,
               zone ? zone : "UTC");
    }

    /* --- Uptime --- */
    {
        unsigned int ms = pit_millis();
        unsigned int s  = ms / 1000;
        printf("  Uptime    : %uh %um %us\n",
               s / 3600, (s % 3600) / 60, s % 60);
    }

    /* --- RAM (use memmap total so we count all physical RAM) --- */
    {
        unsigned long long total = memmap_total_usable();
        unsigned long long free  = pmm_free_pages() * 4096ULL;
        unsigned long long used  = (free < total) ? (total - free) : 0;
        printf("  RAM       : ");
        print_bar((unsigned int)(used  / 4096),
                  (unsigned int)(total / 4096), 20);
        printf("  ");
        print_mb(used);
        printf(" / ");
        print_mb(total);
        printf("\n");
    }

    /* --- Kernel Heap --- */
    {
        unsigned long long used  = kheap_used();
        unsigned long long total = kheap_size();
        printf("  Heap      : ");
        print_bar((unsigned int)used, (unsigned int)total, 20);
        printf("  ");
        print_mb(used);
        printf(" / ");
        print_mb(total);
        printf("  (%u blocks)\n", kheap_blocks());
    }

    /* --- Disk (FAT32) --- */
    {
        unsigned int free_c  = fat32_free_clusters();
        unsigned int total_c = fat32_total_clusters();
        unsigned int csz     = fat32_cluster_size();
        unsigned long long free_b  = (unsigned long long)free_c  * csz;
        unsigned long long total_b = (unsigned long long)total_c * csz;
        unsigned long long used_b  = total_b - free_b;
        printf("  Disk      : ");
        print_bar((unsigned int)(used_b  / (csz ? csz : 1)),
                  (unsigned int)(total_b / (csz ? csz : 1)), 20);
        printf("  ");
        print_mb(used_b);
        printf(" / ");
        print_mb(total_b);
        printf("\n");
    }

    /* --- Tasks --- */
    {
        int total = 0, running = 0, ready_c = 0, blocked = 0;
        task_count_states(&total, &running, &ready_c, &blocked);
        printf("  Tasks     : %d total  (%d running, %d ready, %d blocked)\n",
               total, running, ready_c, blocked);
    }

    /* --- CPU (CPUID) --- */
    {
        unsigned int ebx = 0, ecx = 0, edx = 0, eax_max = 0;
        __asm__ volatile (
            "cpuid"
            : "=a"(eax_max), "=b"(ebx), "=c"(ecx), "=d"(edx)
            : "a"(0)
        );
        char vendor[13];
        ((unsigned int*)vendor)[0] = ebx;
        ((unsigned int*)vendor)[1] = edx;
        ((unsigned int*)vendor)[2] = ecx;
        vendor[12] = '\0';

        char brand[49];
        unsigned int* bp = (unsigned int*)brand;
        for (int leaf = 0; leaf < 3; leaf++) {
            unsigned int a, b, c, d;
            __asm__ volatile (
                "cpuid"
                : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                : "a"(0x80000002 + (unsigned int)leaf)
            );
            bp[leaf*4+0] = a; bp[leaf*4+1] = b;
            bp[leaf*4+2] = c; bp[leaf*4+3] = d;
        }
        brand[48] = '\0';
        const char* bs = brand;
        while (*bs == ' ') bs++;

        printf("  CPU       : %s  |  %s\n", vendor, bs);
    }

    con_set_color(CON_LIGHT_CYAN, CON_BLACK);
    printf("====================\n");
    con_set_color(CON_LIGHT_GREY, CON_BLACK);
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

// ---- graphics demo ----------------------------------------------------
// Switch into VGA mode 13h (320x200x256), prove we can address the
// framebuffer, then restore text mode so the shell keeps working.
static void cmd_gfx(void) {
    gfx_init();                       // enter graphics mode, screen -> black

    // One bright-white pixel dead centre. This is the actual "get a pixel
    // to display" goal — everything else here is just so it's easy to see.
    gfx_putpixel(GFX_WIDTH / 2, GFX_HEIGHT / 2, 15);

    // A small red square near the top-left, to confirm the row-major
    // framebuffer math (y*width + x) is right and not mirrored/skewed.
    gfx_fill_rect(20, 20, 16, 16, 4);

    // A line drawn with the real Bresenham routine — any slope, not just
    // the 45-degree special case we hand-rolled before.
    gfx_line(50, 50, 260, 150, 2);

    // Text rendered from the VGA bitmap font, in graphics mode.
    gfx_text(70, 170, "Hello from graphics mode!", 15, -1);
    gfx_text(70, 186, "0123456789 +-*/= () {}", 14, -1);

    // Block until a key is pressed, then go back to text mode. We can't
    // printf to the screen here (we're in graphics mode), so we just wait.
    (void)keyboard_getchar();

    gfx_set_text_mode();
    con_clear();
    printf("Back in text mode. Drew a centre pixel + red square + green line.\n");
}

static void cmd_gfxmouse(void) {
    printf("Entering graphics mode. Move the mouse, hold left button to\n");
    printf("paint, press any key to exit.\n");
    gfx_mouse_demo();                 // blocks until a key is pressed
    con_clear();
    printf("Back in text mode.\n");
}

static void cmd_paint(void) {
    printf("Paint: click a colour swatch, hold LEFT to draw, RIGHT to\n");
    printf("erase. Click QUIT (or press any key) to exit.\n");
    gfx_paint();                      // blocks until QUIT or a keypress
    con_clear();
    printf("Back in text mode.\n");
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
    else if (strcmp(line, "nettest")  == 0) cmd_nettest();
    else if (strcmp(line, "ping")     == 0) cmd_ping(args);
    else if (strcmp(line, "resolve")  == 0) cmd_resolve(args);
    else if (strcmp(line, "http")     == 0) cmd_http(args);
    else if (strcmp(line, "pmemstat") == 0) cmd_pmemstat();
    else if (strcmp(line, "palloc")   == 0) cmd_palloc();
    else if (strcmp(line, "vmap")     == 0) cmd_vmap(args);
    else if (strcmp(line, "kmstat")   == 0) cmd_kmstat();
    else if (strcmp(line, "kmtest")   == 0) cmd_kmtest();
    else if (strcmp(line, "lspci")    == 0) cmd_lspci();
    else if (strcmp(line, "vbe")      == 0) cmd_vbe();
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
    else if (strcmp(line, "mount")    == 0) cmd_mount();
    else if (strcmp(line, "tar")      == 0) cmd_tar(args);
    else if (strcmp(line, "history")  == 0) cmd_history();
    else if (strcmp(line, "date")     == 0) cmd_date();
    else if (strcmp(line, "time")     == 0) cmd_date();
    else if (strcmp(line, "tz")       == 0) cmd_tz(args);
    else if (strcmp(line, "user")     == 0) cmd_user();
    else if (strcmp(line, "run")      == 0) cmd_run(args);
    else if (strcmp(line, "edit")     == 0) cmd_edit(args);
    else if (strcmp(line, "gfx")      == 0) cmd_gfx();
    else if (strcmp(line, "gfxmouse") == 0) cmd_gfxmouse();
    else if (strcmp(line, "paint")    == 0) cmd_paint();
    else if (strcmp(line, "stats")    == 0) cmd_stats();
    else if (strcmp(line, "reboot")   == 0) reboot();
    else if (strcmp(line, "shutdown") == 0) shutdown();
    else {
        con_set_color(CON_LIGHT_RED, CON_BLACK);
        printf("unknown command: %s\n", line);
        con_set_color(CON_LIGHT_GREY, CON_BLACK);
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
        history_push(saved);

        /* Split on \n and execute each sub-line */
        char sub[LINE_MAX]; int si = 0;
        for (int j = 0; j <= i; j++) {
            char ch = line[j];
            if (ch == '\n' || ch == '\0') {
                sub[si] = '\0';
                char* s = sub; while (*s == ' ') s++;
                if (*s) execute(s);
                si = 0;
            } else { if (si < LINE_MAX - 1) sub[si++] = ch; }
        }
    }
}