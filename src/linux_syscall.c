/*
 * linux_syscall.c — Linux x86-64 syscall compatibility layer.
 *
 * Implements the Linux syscall ABI so real Linux binaries (musl, apk.static)
 * can run on Computer OS. The entry point is linux_syscall_entry in
 * syscall_asm.asm which calls linux_syscall_handler() here.
 *
 * Frame layout passed from assembly:
 *   [rdi+0]   = r15
 *   [rdi+8]   = r14
 *   ...
 *   [rdi+112] = rax  (syscall number)
 *   [rdi+120] = rcx  (user rip)
 *   [rdi+128] = r11  (user rflags)
 *   [rdi+136] = user rsp
 */

#include "linux_syscall.h"
#include "vfs.h"
#include "kheap.h"
#include "string.h"
#include "printf.h"
/* Forward declaration — dns_resolve is implemented in net.c */
int dns_resolve(const char* hostname, unsigned int* out_ip);
#include "task.h"
#include "uaccess.h"
#include "loader.h"
#include "isr.h"
#include "keyboard.h"
#include "console.h"
#include "stdint.h"
#include "pmm.h"
#include "vmm.h"
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* Linux errno values                                                   */
/* ------------------------------------------------------------------ */
#define EPERM    -1
#define ENOENT   -2
#define EINTR    -4
#define EIO      -5
#define EBADF    -9
#define ECHILD   -10
#define EAGAIN   -11
#define ENOMEM   -12
#define EACCES   -13
#define EFAULT   -14
#define EBUSY    -16
#define EEXIST   -17
#define ENODEV   -19
#define ENOTDIR  -20
#define EISDIR   -21
#define EINVAL   -22
#define EMFILE   -24
#define ENOSPC   -28
#define EROFS    -30
#define ENOSYS   -38

/* linux_frame_t defined in linux_syscall.h */

/* ------------------------------------------------------------------ */
/* Per-process file descriptor table                                    */
/* ------------------------------------------------------------------ */
#define FD_MAX       64
#define FD_FLAG_USED 0x01
#define FD_FLAG_DIR    0x02
#define FD_FLAG_TTY    0x04   /* the console: keyboard in, screen out */
#define FD_FLAG_PIPE_R 0x10
#define FD_FLAG_PIPE_W 0x20
#define FD_FLAG_SOCK   0x40
#define FD_FLAG_CLOEXEC 0x80   /* close on execve (O_CLOEXEC / FD_CLOEXEC) */
#define LX_O_CLOEXEC   0x80000 /* 02000000 */

#define SOCK_MAX 8

typedef struct {
    unsigned char flags;
    char          path[256];   /* resolved absolute path */
    unsigned int  offset;      /* current read/write position */
    unsigned int  size;        /* cached file size */
    char*         wbuf;        /* write buffer for sequential writes */
    unsigned int  wbuf_cap;    /* allocated capacity */
} fd_entry_t;

/* Each user process has its own fd table (task->fdt): created on first
   use with stdin/stdout/stderr open, copied on fork, closed on exit.
   boot_fd_table only serves code running outside a user task.
   `fd_table` below expands to the CURRENT process's table, so every
   existing `fd_table[fd]` use is per-process. */
static fd_entry_t boot_fd_table[FD_MAX];
static int        fd_table_init = 0;

static fd_entry_t* fdt_new(void) {
    fd_entry_t* t = (fd_entry_t*)kmalloc(sizeof(fd_entry_t) * FD_MAX);
    if (!t) return 0;
    memset(t, 0, sizeof(fd_entry_t) * FD_MAX);
    t[0].flags = t[1].flags = t[2].flags = FD_FLAG_USED | FD_FLAG_TTY;
    return t;
}

static fd_entry_t* cur_fdt(void) {
    task_t* t = task_current();
    if (t && t->is_user) {
        if (!t->fdt) t->fdt = fdt_new();
        if (t->fdt) return (fd_entry_t*)t->fdt;
    }
    return boot_fd_table;
}
#define fd_table (cur_fdt())

/* Pipe pool — defined fully in the pipe section below; forward-declared
   here so sys_linux_close (which comes earlier) can reference it.     */
#define PIPE_MAX 16
typedef struct {
    uint8_t  buf[4096];
    unsigned head, tail, used;
    int      read_open, write_open;
} pipe_t;
static pipe_t pipe_pool[PIPE_MAX];
static int    pipe_pool_init = 0;

/* Socket table — forward declared so sys_linux_close can use it.
   Fully defined in the socket section below. */
typedef struct sock_entry sock_entry_t;
struct sock_entry {
    int        used;
    int        domain, type, proto;
    void*      tcp;   /* tcp_conn_t* — void* avoids circular include */
    unsigned int  dst_ip;
    unsigned short dst_port;
    char       rxbuf[4096];
    unsigned   rxhead, rxtail;
    int        http_dbg;  /* just sent an HTTP request; log the status line */
    uint64_t   rx_total;  /* debug: total bytes delivered to userspace */
    uint32_t   rx_sum;    /* debug: additive checksum of delivered bytes */
};
static sock_entry_t sock_table[SOCK_MAX];
static int sock_table_init = 0;

#define ENOTCONN -107

/* Minimal TLS block — just enough for __errno_location to work.
   Layout: [0] = pointer to self (pthread self ptr), [52/0x34] = errno */
static uint64_t minimal_tls[16];  /* 128 bytes, zero-initialized */

static void fdtable_init(void) {
    if (fd_table_init) return;
    for (int i = 0; i < FD_MAX; i++) boot_fd_table[i].flags = 0;
    /* stdin=0, stdout=1, stderr=2 are always open */
    boot_fd_table[0].flags = FD_FLAG_USED | FD_FLAG_TTY;
    boot_fd_table[1].flags = FD_FLAG_USED | FD_FLAG_TTY;
    boot_fd_table[2].flags = FD_FLAG_USED | FD_FLAG_TTY;
    fd_table_init = 1;

    /* Set up minimal TLS so %fs:0x0 is valid.
       musl's __errno_location returns %fs:0x34 (for x86-64 musl).
       We point FS base at minimal_tls which is zeroed. */
    minimal_tls[0] = (uint64_t)minimal_tls;  /* self pointer at offset 0 */
    uint64_t tls_addr = (uint64_t)minimal_tls;
    __asm__ volatile (
        "wrmsr"
        : : "c"(0xC0000100UL),
            "a"((uint32_t)tls_addr),
            "d"((uint32_t)(tls_addr >> 32))
    );
}

static int fd_alloc(void) {
    for (int i = 3; i < FD_MAX; i++)
        if (!(fd_table[i].flags & FD_FLAG_USED)) return i;
    return -1;
}

static int fd_valid(int fd) {
    return fd >= 0 && fd < FD_MAX && (fd_table[fd].flags & FD_FLAG_USED);
}

/* ------------------------------------------------------------------ */
/* Linux stat structure (x86-64)                                        */
/* ------------------------------------------------------------------ */
typedef struct {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    unsigned int st_mode;
    unsigned int st_uid;
    unsigned int st_gid;
    unsigned int __pad0;
    uint64_t st_rdev;
    int64_t  st_size;
    int64_t  st_blksize;
    int64_t  st_blocks;
    uint64_t st_atime;
    uint64_t st_atime_nsec;
    uint64_t st_mtime;
    uint64_t st_mtime_nsec;
    uint64_t st_ctime;
    uint64_t st_ctime_nsec;
    int64_t  __unused[3];
} linux_stat_t;

/* ------------------------------------------------------------------ */
/* Linux dirent64 structure                                             */
/* ------------------------------------------------------------------ */
typedef struct {
    uint64_t      d_ino;
    int64_t       d_off;
    unsigned short d_reclen;
    unsigned char  d_type;
    char           d_name[256];
} linux_dirent64_t;

#define DT_UNKNOWN 0
#define DT_REG     8
#define DT_DIR     4

/* ------------------------------------------------------------------ */
/* Linux utsname                                                        */
/* ------------------------------------------------------------------ */
typedef struct {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
} linux_utsname_t;

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

static int copy_str_from_user(char* dst, const char* src, unsigned int max) {
    /* Kernel-space source: one of our own path buffers, e.g. a joined path
     * from the *at() syscall wrappers or a literal like "/etc/hosts".
     * copy_from_user would reject these addresses, so copy directly. */
    if ((uint64_t)src >= 0x0000800000000000ULL) {
        for (unsigned int i = 0; i < max; i++) {
            dst[i] = src[i];
            if (src[i] == '\0') return (int)i;
        }
        dst[max-1] = '\0';
        return (int)(max-1);
    }
    for (unsigned int i = 0; i < max; i++) {
        char c;
        if (copy_from_user(&c, src + i, 1) != 0) return -1;
        dst[i] = c;
        if (c == '\0') return (int)i;
    }
    dst[max-1] = '\0';
    return (int)(max-1);
}

/* Resolve a user-space path to an absolute kernel path */
static int resolve_user_path(const char* upath, char* out, unsigned int outsz) {
    char tmp[512];
    if (copy_str_from_user(tmp, upath, sizeof(tmp)) < 0) return EFAULT;
    if (vfs_resolve(tmp, out, outsz) < 0) return ENOENT;
    return 0;
}

#define AT_FDCWD     (-100)
#define AT_REMOVEDIR 0x200
#define AT_SYMLINK_NOFOLLOW 0x100

/* forward decls (defined with the symlink implementation below) */
static void follow_symlinks(char* path, unsigned int pathsz);
static int  symlink_read_target(const char* path_k, char* tbuf, unsigned int tmax);

/* Resolve a path for the *at() family of syscalls. If the path is absolute
 * or dirfd is AT_FDCWD, behaves like a plain path. Otherwise joins the
 * directory fd's stored path with the relative path. `out` is a kernel
 * string suitable for passing to the non-at syscall handlers. */
static int at_join_path(int dirfd, const char* upath, char* out, unsigned int outsz) {
    char rel[256];
    if (copy_str_from_user(rel, upath, sizeof(rel)) < 0) return EFAULT;
    if (rel[0] == '/' || dirfd == AT_FDCWD) {
        unsigned int i = 0;
        for (; rel[i] && i < outsz - 1; i++) out[i] = rel[i];
        out[i] = '\0';
        return 0;
    }
    if (!fd_valid(dirfd)) return EBADF;
    fd_entry_t* d = &fd_table[dirfd];
    if (!(d->flags & FD_FLAG_DIR)) return ENOTDIR;
    unsigned int o = 0;
    for (unsigned int i = 0; d->path[i] && o < outsz - 1; i++) out[o++] = d->path[i];
    if (o > 0 && out[o-1] != '/' && o < outsz - 1) out[o++] = '/';
    for (unsigned int i = 0; rel[i] && o < outsz - 1; i++) out[o++] = rel[i];
    out[o] = '\0';
    return 0;
}

/* Get file size via vfs_list */
static int get_file_size(const char* abspath) {
    /* Split into parent + basename */
    char parent[256]; parent[0] = '/'; parent[1] = '\0';
    unsigned int sl = 0;
    for (unsigned int i = 0; abspath[i]; i++)
        if (abspath[i] == '/') sl = i;
    if (sl > 0) {
        for (unsigned int i = 0; i < sl && i < 254; i++)
            parent[i] = abspath[i];
        parent[sl] = '\0';
    }
    const char* base = abspath + sl + 1;

    vfs_dirent_t* ents = (vfs_dirent_t*)kmalloc(64 * sizeof(vfs_dirent_t));
    if (!ents) return -1;
    int ne = vfs_list(parent, ents, 64);
    int sz = -1;
    for (int i = 0; i < ne; i++) {
        /* Case-insensitive compare */
        const char* a = ents[i].name;
        const char* b = base;
        int match = 1; int j = 0;
        for (; a[j] && b[j]; j++) {
            char ca = a[j]>='A'&&a[j]<='Z'?a[j]+32:a[j];
            char cb = b[j]>='A'&&b[j]<='Z'?b[j]+32:b[j];
            if (ca != cb) { match = 0; break; }
        }
        if (match && !a[j] && !b[j]) { sz = (int)ents[i].size; break; }
    }
    kfree(ents);
    return sz;
}

/* ------------------------------------------------------------------ */
/* Syscall implementations                                              */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Console terminal (the fds flagged FD_FLAG_TTY)                       */
/* ------------------------------------------------------------------ */
/* Kernel-side termios, as read/written by TCGETS/TCSETS (Linux's 36-byte
 * kernel struct). Defaults: canonical line input (ICANON) + ISIG.
 *
 * ECHO is deliberately NOT reported even though the kernel echoes typed
 * characters in canonical mode: busybox's line editor sees "ICANON without
 * ECHO" and falls back to plain line input (prompt + fgets). Its full
 * editor needs ANSI escape sequences, which this console doesn't draw yet.
 * Once the console understands ESC[...] sequences, report ECHO here too. */
#define LX_ICANON 0x0002
#define LX_ECHO   0x0008
#define LX_ENOTTY (-25)
typedef struct {
    uint32_t c_iflag, c_oflag, c_cflag, c_lflag;
    uint8_t  c_line;
    uint8_t  c_cc[19];
} lx_termios_t;
static lx_termios_t tty_tio = {
    0x0100,          /* ICRNL                  */
    0x0005,          /* OPOST | ONLCR          */
    0x00BF,          /* B38400 | CS8 | CREAD   */
    0x0003,          /* ISIG | ICANON          */
    0,
    { 3, 0x1c, 0x7f, 0x15, 4, 0, 1, 0, 0x11, 0x13, 0x1a, 0, 0x12, 0x0f, 0x17, 0x16, 0, 0, 0 }
};

/* Canonical mode: a whole line is collected (with echo and backspace)
 * before read() returns; leftover bytes are handed out on later reads. */
static char         tty_line[512];
static unsigned int tty_line_len = 0;
static unsigned int tty_line_pos = 0;

/* Defined with the signal code further down. */
static int signal_ready(task_t* t);

/* Block for one key. Returns 0 on success, -1 if a signal became pending
   (so the caller can return EINTR and let it be delivered). */
static int tty_getc_blocking(char* out) {
    while (!keyboard_try_getchar(out)) {
        if (signal_ready(task_current())) return -1;
        yield();
    }
    return 0;
}

/* Terminal special char -> signal. Called from the keyboard IRQ. Returns 1
   if the char was consumed as a signal (ISIG) and must not be buffered. */
#define LX_ISIG 0x0001
void signal_raise(task_t* t, int signo);
int tty_intr_char(char c) {
    if (!(tty_tio.c_lflag & LX_ISIG)) return 0;     /* literal when ISIG off */
    int sig = 0;
    if (c == 0x03)      sig = 2;                     /* Ctrl-C -> SIGINT  */
    else if (c == 0x1c) sig = 3;                     /* Ctrl-\ -> SIGQUIT */
    else return 0;
    task_t* t = task_current();
    if (t && t->is_user) signal_raise(t, sig);
    return 1;
}

static int64_t tty_read(char* ubuf, uint64_t count) {
    if (count == 0) return 0;
    if (!user_range_ok(ubuf, count)) return EFAULT;

    if (!(tty_tio.c_lflag & LX_ICANON)) {
        /* Raw: block for one key, then return whatever else is queued. */
        uint64_t n = 0;
        char c;
        if (tty_getc_blocking(&c) < 0) return EINTR;
        for (;;) {
            if ((unsigned char)c < 0x80) {            /* drop KEY_* codes */
                if (tty_tio.c_lflag & LX_ECHO) con_putchar(c);
                if (copy_to_user(ubuf + n, &c, 1) != 0) return n ? (int64_t)n : EFAULT;
                n++;
            }
            if (n >= count || !keyboard_try_getchar(&c)) break;
        }
        if (n == 0) return tty_read(ubuf, count);    /* only special keys */
        return (int64_t)n;
    }

    if (tty_line_pos >= tty_line_len) {
        /* Collect a new line. */
        tty_line_len = tty_line_pos = 0;
        for (;;) {
            char c;
            if (tty_getc_blocking(&c) < 0) {               /* interrupted */
                tty_line_len = tty_line_pos = 0;           /* discard partial line */
                return EINTR;
            }
            if (c == '\r') c = '\n';                       /* ICRNL */
            if (c == '\b' || c == 0x7f) {                  /* erase */
                if (tty_line_len > 0) { tty_line_len--; con_putchar('\b'); }
                continue;
            }
            if (c == 0x04) {                               /* Ctrl-D */
                if (tty_line_len == 0) return 0;           /* EOF */
                break;
            }
            if ((unsigned char)c >= 0x80) continue;        /* arrows etc. */
            if (tty_line_len < sizeof(tty_line) - 1) {
                tty_line[tty_line_len++] = c;
                con_putchar(c);                            /* echo */
            }
            if (c == '\n') break;
        }
    }
    uint64_t n = tty_line_len - tty_line_pos;
    if (n > count) n = count;
    if (copy_to_user(ubuf, tty_line + tty_line_pos, (unsigned int)n) != 0) return EFAULT;
    tty_line_pos += (unsigned int)n;
    return (int64_t)n;
}

/* sys_read (0) */
static int64_t sys_linux_read(int fd, char* buf, uint64_t count) {
    if (!fd_valid(fd)) return EBADF;
    if (count == 0) return 0;

    if (fd_table[fd].flags & FD_FLAG_TTY) return tty_read(buf, count);

    /* Regular file */
    fd_entry_t* f = &fd_table[fd];
    if (f->flags & FD_FLAG_DIR) return EISDIR;

    unsigned int to_read = (unsigned int)count;
    if (f->offset >= f->size) return 0; /* EOF */
    if (f->offset + to_read > f->size) to_read = f->size - f->offset;

    char* kbuf = (char*)kmalloc(to_read);
    if (!kbuf) return ENOMEM;

    /* If this fd has unflushed buffered writes, read from the buffer —
     * the on-disk copy is stale until close/fsync flushes it. */
    if (f->wbuf) {
        for (unsigned int i = 0; i < to_read; i++)
            kbuf[i] = f->wbuf[f->offset + i];
        if (copy_to_user(buf, kbuf, to_read) != 0) { kfree(kbuf); return EFAULT; }
        kfree(kbuf);
        f->offset += to_read;
        return (int64_t)to_read;
    }

    /* Read at offset */
    char* filebuf = (char*)kmalloc(f->size);
    if (!filebuf) { kfree(kbuf); return ENOMEM; }
    int n = vfs_read_file(f->path, filebuf, f->size);
    if (n < 0) { kfree(filebuf); kfree(kbuf); return EIO; }

    for (unsigned int i = 0; i < to_read; i++)
        kbuf[i] = filebuf[f->offset + i];
    kfree(filebuf);

    if (copy_to_user(buf, kbuf, to_read) != 0) {
        kfree(kbuf); return EFAULT;
    }
    kfree(kbuf);
    f->offset += to_read;
    return (int64_t)to_read;
}

/* Console output with a small ANSI escape interpreter, so programs that
 * colour their output (ls, grep --color, apk) or move the cursor render
 * properly instead of printing "ESC[1;34m". Handles CSI ... m (SGR colours),
 * K / J (erase), H / f (position), A B C D G (cursor moves); anything else
 * is swallowed silently. */
static int  ansi_state = 0;            /* 0 normal, 1 after ESC, 2 in CSI */
static int  ansi_params[8];
static int  ansi_nparams = 0;
static int  ansi_fg = CON_LIGHT_GREY, ansi_bg = CON_BLACK, ansi_bold = 0;
/* DECSC/DECRC (ESC 7 / ESC 8) saved cursor + rendition. apk's progress bar
   saves the cursor, draws the bar, then restores to the line start and
   clears, so each frame redraws in place instead of on a new line. */
static int  ansi_saved_row = 0, ansi_saved_col = 0;
static int  ansi_saved_fg = CON_LIGHT_GREY, ansi_saved_bg = CON_BLACK, ansi_saved_bold = 0;

static void ansi_apply_color(void) {
    int fg = ansi_fg;
    if (ansi_bold && fg < 8) fg += 8;
    con_set_color((enum con_color)fg, (enum con_color)ansi_bg);
}

static void ansi_erase_cells(int row, int from_col, int to_col) {
    int cr, cc;
    con_get_cursor(&cr, &cc);
    for (int c = from_col; c < to_col; c++) {
        con_set_cursor(row, c);
        con_putchar_at_cursor(' ');
    }
    con_set_cursor(cr, cc);
}

static void ansi_csi(char final) {
    static const int ansi_to_vga[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };
    int p0 = ansi_nparams > 0 ? ansi_params[0] : 0;
    int row, col;
    con_get_cursor(&row, &col);
    int rows = con_rows(), cols = con_cols();
    int n = p0 > 0 ? p0 : 1;

    switch (final) {
    case 'm':
        if (ansi_nparams == 0) { ansi_params[0] = 0; ansi_nparams = 1; }
        for (int i = 0; i < ansi_nparams; i++) {
            int v = ansi_params[i];
            if (v == 0)                   { ansi_fg = CON_LIGHT_GREY; ansi_bg = CON_BLACK; ansi_bold = 0; }
            else if (v == 1)              ansi_bold = 1;
            else if (v == 22)             ansi_bold = 0;
            else if (v >= 30 && v <= 37)  ansi_fg = ansi_to_vga[v - 30];
            else if (v == 39)             ansi_fg = CON_LIGHT_GREY;
            else if (v >= 40 && v <= 47)  ansi_bg = ansi_to_vga[v - 40];
            else if (v == 49)             ansi_bg = CON_BLACK;
            else if (v >= 90 && v <= 97)  ansi_fg = ansi_to_vga[v - 90] + 8;
            else if (v >= 100 && v <= 107) ansi_bg = ansi_to_vga[v - 100] + 8;
        }
        ansi_apply_color();
        break;
    case 'K':                                     /* erase in line */
        if (p0 == 0)      ansi_erase_cells(row, col, cols);
        else if (p0 == 1) ansi_erase_cells(row, 0, col + 1);
        else              ansi_erase_cells(row, 0, cols);
        break;
    case 'J':                                     /* erase in display */
        if (p0 == 2 || p0 == 3) { con_clear(); con_set_cursor(0, 0); }
        else if (p0 == 0) {
            ansi_erase_cells(row, col, cols);
            for (int r = row + 1; r < rows; r++) ansi_erase_cells(r, 0, cols);
        }
        break;
    case 'H': case 'f': {
        int r = (ansi_nparams > 0 && ansi_params[0] > 0) ? ansi_params[0] - 1 : 0;
        int c = (ansi_nparams > 1 && ansi_params[1] > 0) ? ansi_params[1] - 1 : 0;
        con_set_cursor(r, c);
        break;
    }
    case 'A': con_set_cursor(row - n < 0 ? 0 : row - n, col); break;
    case 'B': con_set_cursor(row + n >= rows ? rows - 1 : row + n, col); break;
    case 'C': con_set_cursor(row, col + n >= cols ? cols - 1 : col + n); break;
    case 'D': con_set_cursor(row, col - n < 0 ? 0 : col - n); break;
    case 'G': con_set_cursor(row, n - 1 >= cols ? cols - 1 : n - 1); break;
    default: break;                               /* unsupported: ignore */
    }
}

static void tty_putc(char c) {
    if (ansi_state == 0) {
        if (c == 0x1b) { ansi_state = 1; return; }
        con_putchar(c);
        return;
    }
    if (ansi_state == 1) {
        if (c == '[') { ansi_state = 2; ansi_nparams = 0; ansi_params[0] = 0; return; }
        if (c == '7') {                           /* DECSC: save cursor + attrs */
            con_get_cursor(&ansi_saved_row, &ansi_saved_col);
            ansi_saved_fg = ansi_fg; ansi_saved_bg = ansi_bg; ansi_saved_bold = ansi_bold;
            ansi_state = 0; return;
        }
        if (c == '8') {                           /* DECRC: restore cursor + attrs */
            con_set_cursor(ansi_saved_row, ansi_saved_col);
            ansi_fg = ansi_saved_fg; ansi_bg = ansi_saved_bg; ansi_bold = ansi_saved_bold;
            ansi_apply_color();
            ansi_state = 0; return;
        }
        ansi_state = 0;                           /* ESC x: ignore both */
        return;
    }
    /* CSI: digits ; digits ... final byte (0x40..0x7e) */
    if (c >= '0' && c <= '9') {
        if (ansi_nparams == 0) ansi_nparams = 1;
        if (ansi_nparams <= 8)
            ansi_params[ansi_nparams - 1] = ansi_params[ansi_nparams - 1] * 10 + (c - '0');
        return;
    }
    if (c == ';') {
        if (ansi_nparams == 0) ansi_nparams = 1;
        if (ansi_nparams < 8) ansi_params[ansi_nparams++] = 0;
        return;
    }
    if (c == '?' || c == '>' || c == '=') return; /* private-mode prefix */
    if ((unsigned char)c >= 0x40 && (unsigned char)c <= 0x7e) {
        if (ansi_nparams > 8) ansi_nparams = 8;
        ansi_csi(c);
        ansi_state = 0;
    }
}

/* sys_write (1) */
static int64_t sys_linux_write(int fd, const char* buf, uint64_t count) {
    if (!fd_valid(fd)) return EBADF;
    if (count == 0) return 0;

    if (fd_table[fd].flags & FD_FLAG_TTY) {
        char chunk[128];
        uint64_t total = 0;
        while (total < count) {
            unsigned int n = (unsigned int)(count - total);
            if (n > sizeof(chunk)) n = sizeof(chunk);
            if (copy_from_user(chunk, buf + total, n) != 0)
                return total > 0 ? (int64_t)total : EFAULT;
            for (unsigned int i = 0; i < n; i++) tty_putc(chunk[i]);
            total += n;
        }
        return (int64_t)total;
    }

    fd_entry_t* f = &fd_table[fd];
    unsigned int n = (unsigned int)count;

    /* Grow write buffer as needed */
    unsigned int need = f->offset + n;
    if (need > f->wbuf_cap) {
        unsigned int newcap = need * 2;
        if (newcap < 4096) newcap = 4096;
        char* nb = (char*)kmalloc(newcap);
        if (!nb) return ENOMEM;
        if (f->wbuf && f->size > 0) {
            for (unsigned int i = 0; i < f->size; i++) nb[i] = f->wbuf[i];
        } else if (!f->wbuf && f->size > 0) {
            /* Load existing content into buffer */
            int r = vfs_read_file(f->path, nb, f->size);
            if (r < 0) { kfree(nb); return EIO; }
        }
        if (f->wbuf) kfree(f->wbuf);
        f->wbuf = nb;
        f->wbuf_cap = newcap;
    }

    if (!f->wbuf) {
        f->wbuf = (char*)kmalloc(4096);
        if (!f->wbuf) return ENOMEM;
        f->wbuf_cap = 4096;
    }

    /* Copy user data into write buffer */
    if (copy_from_user(f->wbuf + f->offset, buf, n) != 0) return EFAULT;
    f->offset += n;
    if (f->offset > f->size) f->size = f->offset;
    return (int64_t)count;
}

static void fd_flush(fd_entry_t* f) {
    if (f->wbuf && f->size > 0 && f->path[0]) {
        vfs_write_file(f->path, f->wbuf, f->size);
        kfree(f->wbuf);
        f->wbuf = 0;
        f->wbuf_cap = 0;
    }
}

/* sys_open (2) */
static int64_t sys_linux_open(const char* upath, int flags, int mode) {
    (void)mode;
    char path[512];

    /* Intercept /etc/hosts — inject CDN IP so glibc resolves without DNS */
    {
        char raw[64];
        if (copy_str_from_user(raw, upath, sizeof(raw)) >= 0) {

            if (strcmp(raw, "/etc/hosts") == 0 || strcmp(raw, "etc/hosts") == 0 ||
                strcmp(raw, "hosts") == 0) {
                /* Use kernel DNS to resolve, with hardcoded fallback.
                 * Use a static cached IP so we don't re-enter on every call. */
                static unsigned int cached_ip = 0;
                if (!cached_ip) {
                    /* First call: try to resolve, cache result */
                    dns_resolve("dl-cdn.alpinelinux.org", &cached_ip);
                    if (!cached_ip) cached_ip = 0x97650284UL; /* 151.101.2.132 */
                }
                printf("[hosts] using %u.%u.%u.%u\n",
                       (cached_ip>>24)&0xFF,(cached_ip>>16)&0xFF,
                       (cached_ip>>8)&0xFF,cached_ip&0xFF);
                char buf[256]; int n = 0;
                const char* lo = "127.0.0.1 localhost\n";
                while (*lo) buf[n++] = *lo++;
                unsigned int o[4]={(cached_ip>>24)&0xFF,(cached_ip>>16)&0xFF,(cached_ip>>8)&0xFF,cached_ip&0xFF};
                for (int i=0;i<4;i++){unsigned int v=o[i];if(v>=100)buf[n++]='0'+v/100;if(v>=10)buf[n++]='0'+(v/10)%10;buf[n++]='0'+v%10;buf[n++]=(i<3)?'.':' ';}
                const char* cdn = "dl-cdn.alpinelinux.org\n";
                while (*cdn) buf[n++] = *cdn++;
                vfs_write_file("/tmp/.vhosts", buf, (unsigned int)n);
                if (resolve_user_path("/tmp/.vhosts", path, sizeof(path)) < 0) return ENOENT;
                goto open_path;
            }
        }
    }

    if (resolve_user_path(upath, path, sizeof(path)) < 0) return ENOENT;
    follow_symlinks(path, sizeof(path));
open_path:;

    int fd = fd_alloc();
    if (fd < 0) return EMFILE;

    /* Check if it's a directory */
    vfs_dirent_t* ents = (vfs_dirent_t*)kmalloc(2 * sizeof(vfs_dirent_t));
    int is_dir = 0;
    if (ents) {
        int n = vfs_list(path, ents, 1);
        if (n >= 0) is_dir = 1; /* list succeeded = it's a directory */
        kfree(ents);
    }

    /* O_CREAT=0x40, O_WRONLY=1, O_RDWR=2, O_TRUNC=0x200 */
    int writable = (flags & 1) || (flags & 2);
    int o_creat  = (flags & 0x40);
    int o_trunc  = (flags & 0x200);

    if (!is_dir && !writable && !o_creat) {
        /* Read-only open — file must exist */
        int sz = get_file_size(path);
        if (sz < 0) return ENOENT;
        fd_table[fd].size = (unsigned int)sz;
    } else if (!is_dir) {
        /* Writable or O_CREAT — create file if it doesn't exist */
        int sz = get_file_size(path);
        if (sz < 0 && o_creat) {
            /* Create empty file with a null byte */
            char empty = 0;
            int cr = vfs_write_file(path, &empty, 1);
            if (cr < 0) { fd_table[fd].flags = 0; return EIO; }
            sz = 0;
        } else if (sz < 0) {
            return ENOENT;
        }
        if (o_trunc) {
            vfs_write_file(path, "", 0);
            sz = 0;
        }
        fd_table[fd].size = (unsigned int)sz;
    }

    fd_table[fd].flags  = FD_FLAG_USED | (is_dir ? FD_FLAG_DIR : 0)
                        | ((flags & LX_O_CLOEXEC) ? FD_FLAG_CLOEXEC : 0);
    fd_table[fd].offset = 0;
    fd_table[fd].wbuf     = 0;
    fd_table[fd].wbuf_cap = 0;
    {
        int pi = 0;
        for (; pi < 255 && path[pi]; pi++)
            fd_table[fd].path[pi] = path[pi];
        fd_table[fd].path[pi] = '\0';   /* terminate at actual length —
            fd slots are reused, so a shorter path must not inherit the
            tail of the previous fd's longer path */
    }

    return fd;
}

/* sys_close (3) */
/* Release one open fd entry (any process's table). */
static void fd_close_entry(fd_entry_t* e) {
    if (!(e->flags & FD_FLAG_USED)) return;
    if (e->flags & FD_FLAG_PIPE_R) {
        int pi = (int)e->offset;
        if (pi >= 0 && pi < PIPE_MAX)
            if (--pipe_pool[pi].read_open < 0) pipe_pool[pi].read_open = 0;
    } else if (e->flags & FD_FLAG_PIPE_W) {
        int pi = (int)e->offset;
        if (pi >= 0 && pi < PIPE_MAX)
            if (--pipe_pool[pi].write_open < 0) pipe_pool[pi].write_open = 0;
    } else if (e->flags & FD_FLAG_SOCK) {
        int si = (int)e->offset;
        if (si >= 0 && si < SOCK_MAX && sock_table[si].used) {
            /* tcp_close defined later — call via function pointer workaround.
               We'll just NULL the pointer; tcp_close is called in shutdown. */
            sock_table[si].tcp  = 0;
            sock_table[si].used = 0;
        }
    }
    e->flags = 0;
    /* Flush any buffered writes to disk */
    fd_flush(e);
    e->wbuf = 0;
    e->wbuf_cap = 0;
}

static int64_t sys_linux_close(int fd) {
    /* 0/1/2 are ordinary entries now: a shell closes and re-dups them to
       build pipelines and redirections. */
    if (!fd_valid(fd)) return EBADF;
    fd_close_entry(&fd_table[fd]);
    return 0;
}

/* fork(): child gets a copy of the parent's table. Pipe ends gain a
   reference; a pending write buffer is duplicated so the two processes
   don't free the same buffer. Called from task_clone_user(). */
void linux_fdt_clone(task_t* parent, task_t* child) {
    child->fdt = 0;
    if (!parent || !parent->fdt) return;   /* child starts with fresh 0/1/2 */
    fd_entry_t* src = (fd_entry_t*)parent->fdt;
    fd_entry_t* dst = (fd_entry_t*)kmalloc(sizeof(fd_entry_t) * FD_MAX);
    if (!dst) return;
    memcpy(dst, src, sizeof(fd_entry_t) * FD_MAX);
    for (int i = 0; i < FD_MAX; i++) {
        fd_entry_t* e = &dst[i];
        if (!(e->flags & FD_FLAG_USED)) continue;
        if (e->flags & FD_FLAG_PIPE_R) pipe_pool[e->offset].read_open++;
        if (e->flags & FD_FLAG_PIPE_W) pipe_pool[e->offset].write_open++;
        if (e->wbuf) {
            char* nb = (char*)kmalloc(e->wbuf_cap ? e->wbuf_cap : 1);
            if (nb) memcpy(nb, e->wbuf, e->wbuf_cap);
            e->wbuf = nb;
            if (!nb) e->wbuf_cap = 0;
        }
    }
    child->fdt = dst;
}

/* Process exit: close everything it still had open, free the table. */
void linux_fdt_release(task_t* t) {
    if (!t || !t->fdt) return;
    fd_entry_t* tab = (fd_entry_t*)t->fdt;
    for (int i = 0; i < FD_MAX; i++) fd_close_entry(&tab[i]);
    t->fdt = 0;
    kfree(tab);
}

/* execve(): close every fd marked close-on-exec. */
static void fdt_close_on_exec(void) {
    fd_entry_t* tab = fd_table;
    for (int i = 3; i < FD_MAX; i++)
        if ((tab[i].flags & FD_FLAG_USED) && (tab[i].flags & FD_FLAG_CLOEXEC))
            fd_close_entry(&tab[i]);
}

/* sys_stat (4) and sys_fstat (5) */
static int64_t fill_stat(const char* path, linux_stat_t* ustat) {
    linux_stat_t s;
    memset(&s, 0, sizeof(s));

    /* Check if directory */
    vfs_dirent_t* ents = (vfs_dirent_t*)kmalloc(sizeof(vfs_dirent_t));
    int is_dir = 0;
    if (ents) {
        if (vfs_list(path, ents, 1) >= 0) is_dir = 1;
        kfree(ents);
    }

    if (is_dir) {
        s.st_mode = 0040755; /* directory */
        s.st_size = 4096;
    } else {
        int sz = get_file_size(path);
        if (sz < 0) return ENOENT;
        s.st_mode    = 0100644; /* regular file */
        s.st_size    = sz;
        s.st_blocks  = (sz + 511) / 512;
    }
    s.st_blksize = 512;
    s.st_nlink   = 1;
    s.st_dev     = 1;
    s.st_ino     = 1;

    if (copy_to_user(ustat, &s, sizeof(s)) != 0) return EFAULT;
    return 0;
}

static int64_t sys_linux_stat(const char* upath, linux_stat_t* ustat) {
    char path[512];
    if (resolve_user_path(upath, path, sizeof(path)) < 0) return ENOENT;
    follow_symlinks(path, sizeof(path));
    return fill_stat(path, ustat);
}

static int64_t sys_linux_fstat(int fd, linux_stat_t* ustat) {
    if (!fd_valid(fd)) return EBADF;
    if (fd_table[fd].flags & FD_FLAG_TTY) {
        linux_stat_t s; memset(&s, 0, sizeof(s));
        s.st_mode = 0020666; /* character device (tty) */
        s.st_rdev = 0x0501;  /* /dev/tty */
        if (copy_to_user(ustat, &s, sizeof(s)) != 0) return EFAULT;
        return 0;
    }
    return fill_stat(fd_table[fd].path, ustat);
}

/* sys_lseek (8) */
static int64_t sys_linux_lseek(int fd, int64_t offset, int whence) {
    if (!fd_valid(fd)) return EBADF;
    fd_entry_t* f = &fd_table[fd];
    int64_t new_off;
    if      (whence == 0) new_off = offset;               /* SEEK_SET */
    else if (whence == 1) new_off = f->offset + offset;   /* SEEK_CUR */
    else if (whence == 2) new_off = f->size + offset;     /* SEEK_END */
    else return EINVAL;
    if (new_off < 0) return EINVAL;
    f->offset = (unsigned int)new_off;
    return new_off;
}

/* sys_mmap (9) */
static int64_t sys_linux_mmap(uint64_t addr, uint64_t length,
                               int prot, int flags,
                               int fd, int64_t off) {
    (void)prot; (void)addr; (void)off;
    /* MAP_ANONYMOUS (fd=-1): allocate zeroed pages */
    if (fd == -1 || (flags & 0x20)) {
        unsigned int pages = (unsigned int)((length + 0xFFF) / 0x1000);
        /* Find a free virtual range above 0x40000000 */
        static uint64_t mmap_next = 0x40000000ULL;
        uint64_t base = mmap_next;
        mmap_next += (uint64_t)pages * 0x1000;

        extern int loader_map_anon_page(uint64_t virt);
        for (unsigned int i = 0; i < pages; i++) {
            if (!loader_map_anon_page(base + i * 0x1000)) {
                printf("[mmap] OOM mapping %u pages at 0x%llx\n",
                       pages, (unsigned long long)base);
                return ENOMEM;
            }
        }
        return (int64_t)base;
    }
    return EINVAL;
}

/* sys_munmap (11) */
static int64_t sys_linux_munmap(uint64_t addr, uint64_t length) {
    (void)addr; (void)length;
    /* Stub — just return success for now */
    return 0;
}

/* sys_brk (12) */
static int64_t sys_linux_brk(uint64_t new_brk) {
    task_t* t = task_current();
    if (!t || !t->is_user || !t->user_data) return EINVAL;

    /* Use loader's existing heap management */
    extern int64_t loader_brk(uint64_t new_brk);
    return loader_brk(new_brk);
}

/* sys_writev (20) */
typedef struct { uint64_t iov_base; uint64_t iov_len; } iovec_t;

/* One entry point per direction for every kind of fd (console, pipe,
   socket, file). read/write/readv/writev/pread/pwrite all go through these,
   so a redirected stdout (dup2 of a pipe onto fd 1) works no matter which
   call the program happens to use. Defined after the pipe/socket code. */
static int64_t fd_read_any(int fd, char* buf, uint64_t count);
static int64_t fd_write_any(int fd, const char* buf, uint64_t count);

static int64_t sys_linux_writev(int fd, const iovec_t* uiov, int iovcnt) {
    if (!fd_valid(fd)) return EBADF;
    int64_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        iovec_t iov;
        if (copy_from_user(&iov, uiov + i, sizeof(iov)) != 0) return EFAULT;
        if (iov.iov_len == 0) continue;
        int64_t r = fd_write_any(fd, (const char*)iov.iov_base, iov.iov_len);
        if (r < 0) return total > 0 ? total : r;
        total += r;
        if ((uint64_t)r < iov.iov_len) break;     /* short write */
    }
    return total;
}

/* sys_readv (19) — musl's stdio reads through this. Stops at the first
   short read, like Linux (a pipe/console read returns what's available). */
static int64_t sys_linux_readv(int fd, const iovec_t* uiov, int iovcnt) {
    if (!fd_valid(fd)) return EBADF;
    int64_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        iovec_t iov;
        if (copy_from_user(&iov, uiov + i, sizeof(iov)) != 0) return EFAULT;
        if (iov.iov_len == 0) continue;
        int64_t r = fd_read_any(fd, (char*)iov.iov_base, iov.iov_len);
        if (r < 0) return total > 0 ? total : r;
        total += r;
        if ((uint64_t)r < iov.iov_len) break;
    }
    return total;
}

/* sys_pread64 (17) / sys_pwrite64 (18): file I/O at an offset without
   moving the file position. */
static int64_t sys_linux_pread(int fd, char* buf, uint64_t count, int64_t off,
                               int is_write) {
    if (!fd_valid(fd)) return EBADF;
    fd_entry_t* f = &fd_table[fd];
    if (f->flags & (FD_FLAG_TTY | FD_FLAG_PIPE_R | FD_FLAG_PIPE_W | FD_FLAG_SOCK))
        return -29; /* ESPIPE */
    if (off < 0) return -22; /* EINVAL */
    unsigned int saved = f->offset;
    f->offset = (unsigned int)off;
    int64_t r = is_write ? sys_linux_write(fd, buf, count)
                         : sys_linux_read(fd, buf, count);
    fd_table[fd].offset = saved;
    return r;
}

/* sys_access (21) */
static int64_t sys_linux_access(const char* upath, int mode) {
    (void)mode;
    char path[512];
    if (resolve_user_path(upath, path, sizeof(path)) < 0) return ENOENT;
    int sz = get_file_size(path);
    if (sz < 0) {
        /* Try as directory */
        vfs_dirent_t* ents = (vfs_dirent_t*)kmalloc(sizeof(vfs_dirent_t));
        int is_dir = ents && vfs_list(path, ents, 1) >= 0;
        if (ents) kfree(ents);
        if (!is_dir) return ENOENT;
    }
    return 0;
}

/* sys_getpid (39) */
static int64_t sys_linux_getpid(void) {
    task_t* t = task_current();
    return t ? t->id : 1;
}

/* ------------------------------------------------------------------ */
/* linux_frame_t <-> struct registers bridge                           */
/*                                                                     */
/* loader_fork / loader_exec take struct registers* (the iretq layout  */
/* from include/isr.h).  The linux syscall path gives us linux_frame_t */
/* instead.  We build a synthetic struct registers on the stack, call  */
/* the loader, then (for exec) read the rewritten fields back.         */
/* ------------------------------------------------------------------ */

static void lframe_to_regs(const linux_frame_t* lf, struct registers* r) {
    memset(r, 0, sizeof(*r));
    /* GP regs */
    r->rax = lf->rax;
    r->rbx = lf->rbx;
    r->rcx = lf->rcx;
    r->rdx = lf->rdx;
    r->rsi = lf->rsi;
    r->rdi = lf->rdi;
    r->r8  = lf->r8;
    r->r9  = lf->r9;
    r->r10 = lf->r10;
    /* Callee-saved user registers. linux_syscall_entry pushes
       rbp, rbx, r12, r13, r14, r15 directly *below* the frame, and they
       still hold the user's values at that point. A forked child must
       get them back, or code like musl's fork() that keeps a pointer in
       r12-r15 dereferences NULL in the child. */
    const uint64_t* below = (const uint64_t*)lf;
    r->rbp = below[-1];
    r->r12 = below[-3];
    r->r13 = below[-4];
    r->r14 = below[-5];
    r->r15 = below[-6];
    /* iretq-style CPU fields */
    r->rip    = lf->rcx;        /* rcx = saved user RIP on syscall entry */
    r->rsp    = lf->user_rsp;
    r->rflags = lf->r11;        /* r11 = saved user RFLAGS */
    r->cs     = 0x23;
    r->ss     = 0x1B;
    r->int_no = 0;
    r->err_code = 0;
}

static void regs_to_lframe(const struct registers* r, linux_frame_t* lf) {
    lf->rax      = r->rax;
    lf->rbx      = r->rbx;
    lf->rcx      = r->rip;      /* rip → rcx so sysretq lands there */
    lf->rdx      = r->rdx;
    lf->rsi      = r->rsi;
    lf->rdi      = r->rdi;
    lf->r8       = r->r8;
    lf->r9       = r->r9;
    lf->r10      = r->r10;
    lf->r11      = r->rflags;   /* rflags → r11 so sysretq restores them */
    lf->user_rsp = r->rsp;
}

/* sys_fork (57) */
static int64_t sys_linux_fork(linux_frame_t* frame) {
    struct registers regs;
    lframe_to_regs(frame, &regs);
    /* Child gets rax=0; parent gets child pid returned below */
    regs.rax = 0;
    int child_pid = loader_fork(&regs);
    return (int64_t)child_pid;  /* -1 on failure */
}

/* sys_execve (59)
 *
 * 1. Copy path, argv[] and envp[] out of user memory into one kernel arena
 *    (the caller's address space is destroyed on success).
 * 2. Resolve the path against the cwd and follow symlinks
 *    (busybox installs /bin/sh etc. as symlinks to /bin/busybox).
 * 3. If the file starts with "#!", run the interpreter instead:
 *      argv = { interp, [optional arg], script-path, argv[1..] }
 * 4. Load the ELF and build a real argc/argv/envp/auxv stack. */
#define EXEC_ARENA     (32 * 1024)
#define EXEC_MAX_ARGS  256
#define EXEC_MAX_SHEBANG 4

typedef struct {
    char*        buf;      /* string storage */
    unsigned int used;
    const char*  argv[EXEC_MAX_ARGS + 4];
    int          argc;
    const char*  envp[EXEC_MAX_ARGS];
    int          envc;
} exec_args_t;

static const char* exec_store(exec_args_t* a, const char* s, unsigned int n) {
    if (a->used + n + 1 > EXEC_ARENA) return 0;
    char* d = a->buf + a->used;
    memcpy(d, s, n); d[n] = '\0';
    a->used += n + 1;
    return d;
}

/* Copy a NULL-terminated user array of strings. */
static int exec_copy_vec(exec_args_t* a, const char* const* uvec,
                         const char** out, int max, int* count) {
    *count = 0;
    if (!uvec) return 0;
    for (int i = 0; ; i++) {
        uint64_t up;
        if (copy_from_user(&up, &uvec[i], sizeof(up)) != 0) return EFAULT;
        if (!up) break;
        if (i >= max) return -7; /* E2BIG */
        char tmp[4096];
        if (copy_str_from_user(tmp, (const char*)up, sizeof(tmp)) < 0) return EFAULT;
        const char* k = exec_store(a, tmp, (unsigned int)strlen(tmp));
        if (!k) return -7;
        out[i] = k;
        *count = i + 1;
    }
    return 0;
}

/* cwd-relative -> absolute, then follow symlinks. */
static int exec_resolve(const char* in, char* out, unsigned int outsz) {
    if (vfs_resolve(in, out, outsz) < 0) return ENOENT;
    follow_symlinks(out, outsz);
    return 0;
}

static int64_t sys_linux_execve(const char* upath, const char* const* uargv,
                                const char* const* uenvp, linux_frame_t* frame) {
    exec_args_t* a = (exec_args_t*)kmalloc(sizeof(exec_args_t));
    if (!a) return ENOMEM;
    memset(a, 0, sizeof(*a));
    a->buf = (char*)kmalloc(EXEC_ARENA);
    if (!a->buf) { kfree(a); return ENOMEM; }

    int64_t ret;
    char upath_k[256];
    char path[512];
    if (copy_str_from_user(upath_k, upath, sizeof(upath_k)) < 0) { ret = EFAULT; goto out; }

    ret = exec_copy_vec(a, uargv, a->argv, EXEC_MAX_ARGS, &a->argc);
    if (ret) goto out;
    ret = exec_copy_vec(a, uenvp, a->envp, EXEC_MAX_ARGS, &a->envc);
    if (ret) goto out;
    if (a->argc == 0) {                      /* argv may legally be empty */
        a->argv[0] = exec_store(a, upath_k, (unsigned int)strlen(upath_k));
        a->argc = 1;
    }

    if (exec_resolve(upath_k, path, sizeof(path)) < 0) { ret = ENOENT; goto out; }

    for (int depth = 0; ; depth++) {
        struct registers regs;
        lframe_to_regs(frame, &regs);
        int r = loader_exec_argv(path, a->argv, a->argc, a->envp, a->envc, &regs);
        if (r == 0) {
            fdt_close_on_exec();
            /* loader rewrote rip/rsp/rflags/regs: hand them to sysret. */
            regs_to_lframe(&regs, frame);
            ret = 0;
            goto out;
        }
        if (r == -7) { ret = -7; goto out; }        /* E2BIG */
        if (r != -2) { ret = ENOENT; goto out; }    /* missing / bad ELF */

        /* Not an ELF: is it a "#!" script? */
        char hdr[128];
        int n = vfs_read_file(path, hdr, sizeof(hdr) - 1);
        if (n < 2 || hdr[0] != '#' || hdr[1] != '!' || depth >= EXEC_MAX_SHEBANG) {
            ret = -8;   /* ENOEXEC */
            goto out;
        }
        hdr[n] = '\0';
        char* p = hdr + 2;
        while (*p == ' ' || *p == '\t') p++;
        char* interp = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
        char* optarg = 0;
        if (*p == ' ' || *p == '\t') {
            *p++ = '\0';
            while (*p == ' ' || *p == '\t') p++;
            if (*p && *p != '\n' && *p != '\r') {
                optarg = p;                     /* Linux: rest of line = one arg */
                while (*p && *p != '\n' && *p != '\r') p++;
                while (p > optarg && (p[-1] == ' ' || p[-1] == '\t')) p--;
            }
        }
        *p = '\0';
        if (!*interp) { ret = -8; goto out; }

        /* New argv: interp [optarg] script argv[1..] */
        const char* nargv[EXEC_MAX_ARGS + 4];
        int nc = 0;
        nargv[nc++] = exec_store(a, interp, (unsigned int)strlen(interp));
        if (optarg) nargv[nc++] = exec_store(a, optarg, (unsigned int)strlen(optarg));
        nargv[nc++] = exec_store(a, path, (unsigned int)strlen(path));
        for (int i = 1; i < a->argc && nc < EXEC_MAX_ARGS; i++) nargv[nc++] = a->argv[i];
        for (int i = 0; i < nc; i++) if (!nargv[i]) { ret = -7; goto out; }
        for (int i = 0; i < nc; i++) a->argv[i] = nargv[i];
        a->argc = nc;

        if (exec_resolve(interp, path, sizeof(path)) < 0) { ret = ENOENT; goto out; }
    }

out:
    kfree(a->buf);
    kfree(a);
    return ret;
}

/* sys_exit (60) and sys_exit_group (231) */
static int64_t sys_linux_exit(int code) {
    __asm__ volatile ("cli");
    task_exit_code(code & 0xFF);   /* Linux keeps only the low 8 bits; negative
                                      codes are reserved for "killed by signal" */
    /* task_exit_code switches away and never returns here.
       If it somehow does (shouldn't happen), halt this CPU. */
    __asm__ volatile ("cli; hlt");
    for (;;) __asm__ volatile ("hlt");
    __builtin_unreachable();
}

/* sys_wait4 (61) */
#define LX_WNOHANG 1
static int64_t sys_linux_wait4(int pid, int* ustatus, int options) {
    int status = 0;
    int child = task_wait_child_ex(pid, (options & LX_WNOHANG) != 0, &status);
    if (child < 0) return ECHILD;
    if (child == 0) return 0;            /* WNOHANG: children still running */
    if (ustatus) {
        /* Linux wait status: normal exit = code << 8; killed by a signal
           = signal number in the low 7 bits (we record kills as -signo). */
        int wstatus = (status < 0) ? ((-status) & 0x7F) : ((status & 0xFF) << 8);
        if (copy_to_user(ustatus, &wstatus, sizeof(int)) != 0) return EFAULT;
    }
    return child;
}

/* sys_uname (63) */
static int64_t sys_linux_uname(linux_utsname_t* ubuf) {
    linux_utsname_t u;
    memset(&u, 0, sizeof(u));
    /* Binary compatibility comes from the syscall ABI, not this string,
       so we can brand it freely. `release` stays a valid modern version
       in case a glibc binary (which checks it) ever runs. */
    memcpy(u.sysname,    "Computer OS",       12);
    memcpy(u.nodename,   "computer-os",       12);
    memcpy(u.release,    "6.1.0-computeros",  17);
    memcpy(u.version,    "#1 Computer OS",     15);
    memcpy(u.machine,    "x86_64",         7);
    memcpy(u.domainname, "(none)",         7);
    if (copy_to_user(ubuf, &u, sizeof(u)) != 0) return EFAULT;
    return 0;
}

/* sys_fcntl (72) */
static int64_t sys_linux_fcntl(int fd, int cmd, uint64_t arg) {
    if (!fd_valid(fd)) return EBADF;
    fd_entry_t* tab = fd_table;
    switch (cmd) {
    case 0:      /* F_DUPFD: lowest free fd >= arg */
    case 1030: { /* F_DUPFD_CLOEXEC */
        if (arg >= FD_MAX) return EINVAL;
        int nfd = -1;
        for (int i = (int)arg; i < FD_MAX; i++)
            if (!(tab[i].flags & FD_FLAG_USED)) { nfd = i; break; }
        if (nfd < 0) return EMFILE;
        tab[nfd] = tab[fd];
        tab[nfd].flags &= (unsigned char)~FD_FLAG_CLOEXEC;
        if (cmd == 1030) tab[nfd].flags |= FD_FLAG_CLOEXEC;
        if (tab[fd].flags & FD_FLAG_PIPE_R) pipe_pool[tab[fd].offset].read_open++;
        if (tab[fd].flags & FD_FLAG_PIPE_W) pipe_pool[tab[fd].offset].write_open++;
        return nfd;
    }
    case 1:      /* F_GETFD */
        return (tab[fd].flags & FD_FLAG_CLOEXEC) ? 1 : 0;
    case 2:      /* F_SETFD */
        if (arg & 1) tab[fd].flags |= FD_FLAG_CLOEXEC;
        else         tab[fd].flags &= (unsigned char)~FD_FLAG_CLOEXEC;
        return 0;
    case 3:      /* F_GETFL */
        return 2;                     /* O_RDWR */
    case 4:      /* F_SETFL (O_NONBLOCK etc.): accepted, ignored */
        return 0;
    case 5: case 6: case 7:   /* F_GETLK / F_SETLK / F_SETLKW: one user, no contention */
    case 36: case 37: case 38: /* OFD locks */
        return 0;
    default:
        return EINVAL;
    }
}

/* sys_getdents64 (217) */
static int64_t sys_linux_getdents64(int fd, void* ubuf, unsigned int count) {
    if (!fd_valid(fd)) return EBADF;
    fd_entry_t* f = &fd_table[fd];
    if (!(f->flags & FD_FLAG_DIR)) return ENOTDIR;

    vfs_dirent_t* ents = (vfs_dirent_t*)kmalloc(64 * sizeof(vfs_dirent_t));
    if (!ents) return ENOMEM;
    int n = vfs_list(f->path, ents, 64);
    if (n < 0) { kfree(ents); return EIO; }

    unsigned int written = 0;
    for (int i = (int)f->offset; i < n; i++) {
        linux_dirent64_t d;
        memset(&d, 0, sizeof(d));
        d.d_ino    = (uint64_t)(i + 1);
        d.d_off    = i + 1;
        d.d_type   = (ents[i].attr & 0x10) ? DT_DIR : DT_REG;
        unsigned int nlen = (unsigned int)strlen(ents[i].name);
        /* offsetof(linux_dirent64_t, d_name) = 8+8+2+1 = 19 */
        unsigned short reclen = (unsigned short)(19 + nlen + 1);
        reclen = (reclen + 7) & ~7; /* align to 8 */
        d.d_reclen = reclen;

        if (written + reclen > count) break;

        /* Copy header */
        if (copy_to_user((char*)ubuf + written, &d, 19) != 0) {
            kfree(ents); return EFAULT;
        }
        /* Copy name */
        if (copy_to_user((char*)ubuf + written + 19,
            ents[i].name, nlen + 1) != 0) {
            kfree(ents); return EFAULT;
        }
        written += reclen;
        f->offset++;
    }

    kfree(ents);
    return (int64_t)written;
}

/* sys_chdir (80) */
static int64_t sys_linux_chdir(const char* upath) {
    char path[512];
    if (resolve_user_path(upath, path, sizeof(path)) < 0) return ENOENT;
    if (vfs_chdir(path) < 0) return ENOENT;
    return 0;
}

/* sys_mkdir (83) */
static int64_t sys_linux_mkdir(const char* upath, int mode) {
    (void)mode;
    char path[512];
    if (resolve_user_path(upath, path, sizeof(path)) < 0) return EFAULT;
    if (vfs_mkdir(path) < 0) return EEXIST;
    return 0;
}

/* sys_unlink (87) */
static int64_t sys_linux_unlink(const char* upath) {
    char path[512];
    if (resolve_user_path(upath, path, sizeof(path)) < 0) return ENOENT;
    if (vfs_delete_file(path) < 0) return ENOENT;
    return 0;
}

/* sys_readlink (89) */
static int64_t sys_linux_readlink(const char* upath, char* ubuf, uint64_t bufsz) {
    char path[512];
    if (copy_str_from_user(path, upath, sizeof(path)) < 0) return EFAULT;
    char target[300];
    int tl = symlink_read_target(path, target, sizeof(target));
    if (tl < 0) return EINVAL;   /* not a symlink */
    if ((uint64_t)tl > bufsz) tl = (int)bufsz;
    if (copy_to_user(ubuf, target, (unsigned)tl) != 0) return EFAULT;
    return tl;   /* POSIX: no NUL terminator, return length */
}

/* sys_getuid/geteuid/getgid/getegid (102/107/104/108) */
static int64_t sys_linux_getuid(void)  { return 0; }
static int64_t sys_linux_getgid(void)  { return 0; }

/* ------------------------------------------------------------------ */
/* Pipe / dup / dup2                                                    */
/* ------------------------------------------------------------------ */

#define PIPE_BUF_SIZE 4096

/* pipe index stored in fd_table[fd].offset when FD_FLAG_PIPE_* set */

static void pipe_pool_ensure(void) {
    if (pipe_pool_init) return;
    memset(pipe_pool, 0, sizeof(pipe_pool));
    pipe_pool_init = 1;
}

static int pipe_alloc(void) {
    pipe_pool_ensure();
    for (int i = 0; i < PIPE_MAX; i++) {
        if (!pipe_pool[i].read_open && !pipe_pool[i].write_open) {
            memset(&pipe_pool[i], 0, sizeof(pipe_pool[i]));
            pipe_pool[i].read_open  = 1;
            pipe_pool[i].write_open = 1;
            return i;
        }
    }
    return -1;
}

/* Override read/write for pipe fds */
static int fd_is_pipe(int fd) {
    return fd_valid(fd) &&
           (fd_table[fd].flags & (FD_FLAG_PIPE_R | FD_FLAG_PIPE_W));
}

/* Linux pipe semantics:
 *   read : block only while the pipe is empty and a writer still exists;
 *          once any bytes are available, return them (short read is fine).
 *          Empty + no writers = EOF (0).
 *   write: block while full; if no reader is left, fail with EPIPE
 *          instead of waiting forever (e.g. `ls | head` after head exits). */
#define LX_EPIPE (-32)
static int64_t pipe_read(int fd, char* buf, uint64_t count) {
    int pi = (int)fd_table[fd].offset;
    if (pi < 0 || pi >= PIPE_MAX) return EBADF;
    pipe_t* p = &pipe_pool[pi];
    if (count == 0) return 0;
    while (p->used == 0) {
        if (!p->write_open) return 0;     /* EOF: all writers closed */
        yield();                          /* wait for data */
    }
    uint64_t i = 0;
    while (i < count && p->used > 0) {
        char c = (char)p->buf[p->head];
        p->head = (p->head + 1) % PIPE_BUF_SIZE;
        p->used--;
        if (copy_to_user(buf + i, &c, 1) != 0) return i > 0 ? (int64_t)i : EFAULT;
        i++;
    }
    return (int64_t)i;
}

static int64_t pipe_write(int fd, const char* buf, uint64_t count) {
    int pi = (int)fd_table[fd].offset;
    if (pi < 0 || pi >= PIPE_MAX) return EBADF;
    pipe_t* p = &pipe_pool[pi];
    uint64_t i = 0;
    while (i < count) {
        if (!p->read_open) return i > 0 ? (int64_t)i : LX_EPIPE;
        if (p->used == PIPE_BUF_SIZE) { yield(); continue; }   /* wait for space */
        char c;
        if (copy_from_user(&c, buf + i, 1) != 0) return i > 0 ? (int64_t)i : EFAULT;
        p->buf[p->tail] = (uint8_t)c;
        p->tail = (p->tail + 1) % PIPE_BUF_SIZE;
        p->used++;
        i++;
    }
    return (int64_t)count;
}

/* sys_pipe (22) / sys_pipe2 (293) */
static int64_t sys_linux_pipe(int* upipefd, int flags) {
    unsigned char cx = (flags & LX_O_CLOEXEC) ? FD_FLAG_CLOEXEC : 0;
    int pi = pipe_alloc();
    if (pi < 0) return ENOMEM;

    int rfd = fd_alloc();
    if (rfd < 0) { pipe_pool[pi].read_open = pipe_pool[pi].write_open = 0; return EMFILE; }
    fd_table[rfd].flags  = FD_FLAG_USED | FD_FLAG_PIPE_R | cx;
    fd_table[rfd].offset = (unsigned int)pi;
    fd_table[rfd].size   = 0;

    int wfd = fd_alloc();
    if (wfd < 0) {
        fd_table[rfd].flags = 0;
        pipe_pool[pi].read_open = pipe_pool[pi].write_open = 0;
        return EMFILE;
    }
    fd_table[wfd].flags  = FD_FLAG_USED | FD_FLAG_PIPE_W | cx;
    fd_table[wfd].offset = (unsigned int)pi;
    fd_table[wfd].size   = 0;

    int pair[2] = { rfd, wfd };
    if (copy_to_user(upipefd, pair, sizeof(pair)) != 0) {
        fd_table[rfd].flags = fd_table[wfd].flags = 0;
        pipe_pool[pi].read_open = pipe_pool[pi].write_open = 0;
        return EFAULT;
    }
    return 0;
}

/* sys_dup (32) */
static int64_t sys_linux_dup(int oldfd) {
    if (!fd_valid(oldfd)) return EBADF;
    int nfd = fd_alloc();
    if (nfd < 0) return EMFILE;
    fd_table[nfd] = fd_table[oldfd];
    fd_table[nfd].flags &= (unsigned char)~FD_FLAG_CLOEXEC;
    /* if it's a pipe, bump open count */
    if (fd_table[oldfd].flags & (FD_FLAG_PIPE_R | FD_FLAG_PIPE_W)) {
        int pi = (int)fd_table[oldfd].offset;
        if (fd_table[oldfd].flags & FD_FLAG_PIPE_R) pipe_pool[pi].read_open++;
        else                                          pipe_pool[pi].write_open++;
    }
    return nfd;
}

/* sys_dup2 (33) */
static int64_t sys_linux_dup2(int oldfd, int newfd) {
    if (!fd_valid(oldfd)) return EBADF;
    if (newfd < 0 || newfd >= FD_MAX) return EBADF;
    if (newfd == oldfd) return newfd;
    /* close newfd if open */
    if (fd_table[newfd].flags & FD_FLAG_USED) {
        if (fd_table[newfd].flags & FD_FLAG_PIPE_R) {
            int pi = (int)fd_table[newfd].offset;
            if (--pipe_pool[pi].read_open <= 0) pipe_pool[pi].read_open = 0;
        } else if (fd_table[newfd].flags & FD_FLAG_PIPE_W) {
            int pi = (int)fd_table[newfd].offset;
            if (--pipe_pool[pi].write_open <= 0) pipe_pool[pi].write_open = 0;
        }
    }
    /* flush/release whatever newfd was (files too, not just pipes) */
    if (fd_table[newfd].flags & FD_FLAG_USED) {
        fd_entry_t* e = &fd_table[newfd];
        e->flags &= (unsigned char)~(FD_FLAG_PIPE_R | FD_FLAG_PIPE_W);
        fd_flush(e);
        e->flags = 0;
    }
    fd_table[newfd] = fd_table[oldfd];
    fd_table[newfd].flags &= (unsigned char)~FD_FLAG_CLOEXEC;
    if (fd_table[oldfd].flags & (FD_FLAG_PIPE_R | FD_FLAG_PIPE_W)) {
        int pi = (int)fd_table[oldfd].offset;
        if (fd_table[oldfd].flags & FD_FLAG_PIPE_R) pipe_pool[pi].read_open++;
        else                                          pipe_pool[pi].write_open++;
    }
    return newfd;
}

/* ------------------------------------------------------------------ */
/* mprotect (10), clone (56), rename (82), symlink (88),               */
/* chmod (90), fchmod (91), rmdir (84), link (86), readlinkat (267)    */
/* ------------------------------------------------------------------ */

/* sys_mprotect (10) — we don't enforce page permissions strictly yet,
   just return success so musl/apk don't bail */
static int64_t sys_linux_mprotect(uint64_t addr, uint64_t len, int prot) {
    (void)addr; (void)len; (void)prot;
    return 0;
}

/* sys_clone (56) — apk uses clone(CLONE_VM|CLONE_VFORK) for some paths.
   For now treat as fork (copy-on-write not needed for correctness of
   single-threaded apk runs).  Threads (CLONE_THREAD) return ENOSYS. */
#define CLONE_THREAD  0x00010000
#define CLONE_VM      0x00000100
static int64_t sys_linux_clone(uint64_t flags, uint64_t child_sp,
                                linux_frame_t* frame) {
    if (flags & CLONE_THREAD) return ENOSYS;  /* no threading yet */

    struct registers regs;
    lframe_to_regs(frame, &regs);
    regs.rax = 0;
    /* If caller provided a child stack, use it */
    if (child_sp) regs.rsp = child_sp;

    int pid = loader_fork(&regs);
    return (int64_t)pid;
}

/* sys_rename (82) */
static int64_t sys_linux_rename(const char* uold, const char* unew) {
    char oldp[512], newp[512];
    if (resolve_user_path(uold, oldp, sizeof(oldp)) < 0) return ENOENT;
    if (resolve_user_path(unew, newp, sizeof(newp)) < 0) return EFAULT;
    /* Read old file */
    int sz = get_file_size(oldp);
    if (sz < 0) return ENOENT;
    char* buf = (char*)kmalloc((unsigned int)sz + 1);
    if (!buf) return ENOMEM;
    int r = vfs_read_file(oldp, buf, (unsigned int)sz);
    if (r < 0) { kfree(buf); return EIO; }
    if (vfs_write_file(newp, buf, (unsigned int)sz) < 0) { kfree(buf); return EIO; }
    kfree(buf);
    vfs_delete_file(oldp);
    return 0;
}

/* sys_renameat2 (316) / renameat (264) */
static int64_t sys_linux_renameat(int olddirfd, const char* uold,
                                   int newdirfd, const char* unew) {
    char fullold[512], fullnew[512];
    int jr = at_join_path(olddirfd, uold, fullold, sizeof(fullold));
    if (jr < 0) return jr;
    jr = at_join_path(newdirfd, unew, fullnew, sizeof(fullnew));
    if (jr < 0) return jr;
    return sys_linux_rename(fullold, fullnew);
}

/* sys_unlinkat (263) */
static int64_t sys_linux_rmdir(const char* upath);  /* defined below */
static int64_t sys_linux_unlinkat(int dirfd, const char* upath, int flags) {
    char full[512];
    int jr = at_join_path(dirfd, upath, full, sizeof(full));
    if (jr < 0) return jr;
    if (flags & AT_REMOVEDIR) return sys_linux_rmdir(full);
    return sys_linux_unlink(full);
}

/* sys_mkdirat (258) */
static int64_t sys_linux_mkdirat(int dirfd, const char* upath, int mode) {
    char full[512];
    int jr = at_join_path(dirfd, upath, full, sizeof(full));
    if (jr < 0) return jr;
    return sys_linux_mkdir(full, mode);
}

/* sys_faccessat (269) */
static int64_t sys_linux_faccessat(int dirfd, const char* upath, int mode, int flags) {
    (void)flags;
    char full[512];
    int jr = at_join_path(dirfd, upath, full, sizeof(full));
    if (jr < 0) return jr;
    return sys_linux_access(full, mode);
}

/* sys_symlink (88) — pretend success; apk uses for /lib -> /usr/lib etc */
/* Symlinks on FAT32: stored as regular files whose content is
 * SYMLINK_MAGIC followed by the target path. Path resolution follows
 * them (final component only) with a depth limit. */
#define SYMLINK_MAGIC     "!<symlink>"
#define SYMLINK_MAGIC_LEN 10

static int64_t symlink_create(const char* target_k, const char* linkpath_k) {
    char content[300];
    int tl = 0;
    for (; target_k[tl] && tl < 280; tl++) ;
    if (target_k[tl]) return -36; /* ENAMETOOLONG */
    memcpy(content, SYMLINK_MAGIC, SYMLINK_MAGIC_LEN);
    memcpy(content + SYMLINK_MAGIC_LEN, target_k, tl);
    char lp[512];
    if (vfs_resolve(linkpath_k, lp, sizeof(lp)) < 0) return ENOENT;
    if (vfs_write_file(lp, content, SYMLINK_MAGIC_LEN + (unsigned)tl) < 0)
        return EIO;
    return 0;
}

static int64_t sys_linux_symlink(const char* utarget, const char* ulinkpath) {
    char target[280], linkp[512];
    if (copy_str_from_user(target, utarget, sizeof(target)) < 0) return EFAULT;
    if (copy_str_from_user(linkp, ulinkpath, sizeof(linkp)) < 0) return EFAULT;
    return symlink_create(target, linkp);
}

/* sys_symlinkat (266) */
static int64_t sys_linux_symlinkat(const char* utarget, int newdirfd,
                                   const char* ulinkpath) {
    char target[280], linkp[512];
    if (copy_str_from_user(target, utarget, sizeof(target)) < 0) return EFAULT;
    int jr = at_join_path(newdirfd, ulinkpath, linkp, sizeof(linkp));
    if (jr < 0) return jr;
    return symlink_create(target, linkp);
}

/* Read a symlink's target into tbuf. Returns target length, or <0. */
static int symlink_read_target(const char* path_k, char* tbuf, unsigned int tmax) {
    char rp[512];
    if (vfs_resolve(path_k, rp, sizeof(rp)) < 0) return -1;
    char head[300 + SYMLINK_MAGIC_LEN];
    int n = vfs_read_file(rp, head, sizeof(head) - 1);
    if (n < SYMLINK_MAGIC_LEN) return -1;
    if (memcmp(head, SYMLINK_MAGIC, SYMLINK_MAGIC_LEN) != 0) return -1;
    int tl = n - SYMLINK_MAGIC_LEN;
    if ((unsigned)tl >= tmax) tl = (int)tmax - 1;
    memcpy(tbuf, head + SYMLINK_MAGIC_LEN, tl);
    tbuf[tl] = '\0';
    return tl;
}

/* Resolve marker-file symlinks on the FINAL path component, in place.
 * `path` is a kernel buffer of size pathsz holding an absolute-ish path. */
static void follow_symlinks(char* path, unsigned int pathsz) {
    for (int depth = 0; depth < 8; depth++) {
        char target[300];
        int tl = symlink_read_target(path, target, sizeof(target));
        if (tl < 0) return;                    /* not a symlink: done */
        char next[512];
        if (target[0] == '/') {
            /* absolute target */
            unsigned int i = 0;
            for (; target[i] && i < sizeof(next) - 1; i++) next[i] = target[i];
            next[i] = '\0';
        } else {
            /* relative to the symlink's directory */
            int slash = -1;
            for (int i = 0; path[i]; i++) if (path[i] == '/') slash = i;
            unsigned int o = 0;
            for (int i = 0; i <= slash && o < sizeof(next) - 1; i++)
                next[o++] = path[i];
            for (int i = 0; target[i] && o < sizeof(next) - 1; i++)
                next[o++] = target[i];
            next[o] = '\0';
        }
        /* normalize (handles "..") */
        if (vfs_resolve(next, path, pathsz) < 0) return;
    }
}

/* Exported for the ELF loader (PT_INTERP paths such as
   /lib/ld-musl-x86_64.so.1 may be symlinks). */
void linux_follow_symlinks(char* path, unsigned int pathsz) {
    follow_symlinks(path, pathsz);
}

/* sys_link (86) — hard link: copy the file */
static int64_t sys_linux_link(const char* uold, const char* unew) {
    char oldp[512], newp[512];
    if (resolve_user_path(uold, oldp, sizeof(oldp)) < 0) return ENOENT;
    if (copy_str_from_user(newp, unew, sizeof(newp)) < 0) return EFAULT;
    char newr[512];
    if (vfs_resolve(newp, newr, sizeof(newr)) < 0) return ENOENT;
    int sz = get_file_size(oldp);
    if (sz < 0) return ENOENT;
    char* buf = (char*)kmalloc(sz > 0 ? (unsigned)sz : 1);
    if (!buf) return ENOMEM;
    if (sz > 0 && vfs_read_file(oldp, buf, (unsigned)sz) < 0) {
        kfree(buf); return EIO;
    }
    int wr = vfs_write_file(newr, buf, (unsigned)sz);
    kfree(buf);
    return wr < 0 ? EIO : 0;
}

/* sys_chmod / fchmod (90/91) — no permission enforcement */
static int64_t sys_linux_chmod(const char* upath, int mode) {
    (void)upath; (void)mode; return 0;
}
static int64_t sys_linux_fchmod(int fd, int mode) {
    (void)fd; (void)mode; return 0;
}

/* sys_rmdir (84) */
static int64_t sys_linux_rmdir(const char* upath) {
    char path[512];
    if (resolve_user_path(upath, path, sizeof(path)) < 0) return ENOENT;
    if (vfs_rmdir(path) < 0) return ENOENT;
    return 0;
}

/* sys_readlinkat (267) */
static int64_t sys_linux_readlinkat(int dirfd, const char* upath,
                                     char* ubuf, uint64_t bufsz) {
    /* Check for /proc/self/exe — apk queries this */
    char tmp[512];
    if (copy_str_from_user(tmp, upath, sizeof(tmp)) < 0) return EFAULT;
    if (strcmp(tmp, "/proc/self/exe") == 0) {
        const char* exe = "/apk.static";
        unsigned int len = (unsigned int)strlen(exe);
        if (len > bufsz) len = (unsigned int)bufsz;
        if (copy_to_user(ubuf, exe, len) != 0) return EFAULT;
        return (int64_t)len;
    }
    char full[512];
    int jr = at_join_path(dirfd, upath, full, sizeof(full));
    if (jr < 0) return jr;
    char target[300];
    int tl = symlink_read_target(full, target, sizeof(target));
    if (tl < 0) return EINVAL;   /* not a symlink */
    if ((uint64_t)tl > bufsz) tl = (int)bufsz;
    if (copy_to_user(ubuf, target, (unsigned)tl) != 0) return EFAULT;
    return tl;
}

/* ------------------------------------------------------------------ */
/* Time syscalls                                                        */
/* ------------------------------------------------------------------ */

/* sys_nanosleep (35) — just yield a few times then return */
static int64_t sys_linux_nanosleep(const void* req, void* rem) {
    (void)req;
    if (rem) {
        uint64_t zero[2] = {0, 0};
        copy_to_user(rem, zero, 16);
    }
    yield();
    return 0;
}

/* sys_clock_gettime (228) */
typedef struct { int64_t tv_sec; int64_t tv_nsec; } linux_timespec_t;

static int64_t sys_linux_clock_gettime(int clkid, linux_timespec_t* utp) {
    (void)clkid;
    /* Return a monotonically increasing fake time based on a counter */
    static uint64_t fake_ns = 0;
    fake_ns += 1000000; /* advance 1ms per call */
    linux_timespec_t ts;
    ts.tv_sec  = (int64_t)(fake_ns / 1000000000ULL);
    ts.tv_nsec = (int64_t)(fake_ns % 1000000000ULL);
    if (copy_to_user(utp, &ts, sizeof(ts)) != 0) return EFAULT;
    return 0;
}

/* sys_gettimeofday (96) */
typedef struct { int64_t tv_sec; int64_t tv_usec; } linux_timeval_t;

static int64_t sys_linux_gettimeofday(linux_timeval_t* utv, void* utz) {
    (void)utz;
    static uint64_t fake_us = 0;
    fake_us += 1000;
    linux_timeval_t tv;
    tv.tv_sec  = (int64_t)(fake_us / 1000000ULL);
    tv.tv_usec = (int64_t)(fake_us % 1000000ULL);
    if (utv && copy_to_user(utv, &tv, sizeof(tv)) != 0) return EFAULT;
    return 0;
}

/* sys_clock_getres (229) */
static int64_t sys_linux_clock_getres(int clkid, linux_timespec_t* utp) {
    (void)clkid;
    if (utp) {
        linux_timespec_t ts = {0, 1000000}; /* 1ms resolution */
        if (copy_to_user(utp, &ts, sizeof(ts)) != 0) return EFAULT;
    }
    return 0;
}

/* sys_alarm (37) — stub */
static int64_t sys_linux_alarm(unsigned int secs) { (void)secs; return 0; }

/* ------------------------------------------------------------------ */
/* Process / signal extras                                              */
/* ------------------------------------------------------------------ */

static int64_t sys_linux_getppid(void) {
    task_t* t = task_current();
    return (t && t->parent) ? t->parent->id : 1;
}

static int64_t sys_linux_setsid(void)           { return sys_linux_getpid(); }
static int64_t sys_linux_setpgid(int p, int g)  { (void)p; (void)g; return 0; }
static int64_t sys_linux_getpgid(int p)         { (void)p; return 1; }
static int64_t sys_linux_getsid(int p)          { (void)p; return 1; }
static int64_t sys_linux_getpgrp(void)          { return 1; }
static int64_t sys_linux_setuid(int u)          { (void)u; return 0; }
static int64_t sys_linux_setgid(int g)          { (void)g; return 0; }
static int64_t sys_linux_setresuid(int r,int e,int s) { (void)r;(void)e;(void)s; return 0; }
static int64_t sys_linux_setresgid(int r,int e,int s) { (void)r;(void)e;(void)s; return 0; }
static int64_t sys_linux_getgroups(int sz, int* ulist) {
    (void)ulist;
    if (sz == 0) return 1; /* we have 1 supplementary group (root) */
    if (sz >= 1) {
        int g = 0;
        copy_to_user(ulist, &g, sizeof(int));
    }
    return 1;
}
static int64_t sys_linux_setgroups(int sz, const int* ulist) { (void)sz;(void)ulist; return 0; }

/* ================================================================== */
/* POSIX signals                                                       */
/* ================================================================== */
#define SIG_DFL_H  0
#define SIG_IGN_H  1
#define LX_SIGKILL 9
#define LX_SIGSTOP 19
#define LX_SIGINT  2
#define LX_SIGQUIT 3
#define LX_SIGCHLD 17

/* Default action is to ignore (not terminate) for these. */
static int sig_default_ignored(int s) {
    return s == 17 || s == 18 || s == 23 || s == 28 ||     /* CHLD CONT URG WINCH */
           s == 19 || s == 20 || s == 21 || s == 22;        /* STOP TSTP TTIN TTOU */
}

/* Make `signo` pending on `t`. Wakes a blocked task so it can return EINTR
   from whatever it was waiting on and then take delivery. Non-static:
   keyboard.c calls this for Ctrl-C. */
void signal_raise(task_t* t, int signo) {
    if (!t || signo <= 0 || signo >= 32) return;
    /* SIGKILL / SIGSTOP can neither be caught nor blocked. */
    if (signo == LX_SIGKILL) {
        if (t->is_user) { t->sig_pending |= (1u << signo); }
    }
    t->sig_pending |= (1u << signo);
    if (t->state == TASK_BLOCKED) t->state = TASK_READY;
}

/* Does this task have a signal ready to deliver right now? */
static int signal_ready(task_t* t) {
    if (!t) return 0;
    uint32_t deliverable = t->sig_pending & ~t->sig_blocked;
    /* SIGKILL/SIGSTOP ignore the block mask. */
    deliverable |= t->sig_pending & ((1u << LX_SIGKILL) | (1u << LX_SIGSTOP));
    return deliverable != 0;
}

static int64_t sys_linux_kill(int pid, int sig) {
    if (sig == 0) return 0;                 /* existence check — say yes */
    if (sig < 0 || sig >= 32) return EINVAL;
    task_t* self = task_current();
    /* pid <= 0 means "process group / all"; we have one group, so target self
       (matches how the shell signals its own group). */
    task_t* target = (pid > 0) ? task_find_by_id(pid) : self;
    if (!target) return -3;                 /* ESRCH */
    signal_raise(target, sig);
    return 0;
}

/* sys_prctl (157) */
static int64_t sys_linux_prctl(int op, uint64_t a2, uint64_t a3,
                                uint64_t a4, uint64_t a5) {
    (void)a2;(void)a3;(void)a4;(void)a5;
    /* PR_SET_NAME=15, PR_GET_NAME=16 — apk names its threads */
    if (op == 15) return 0;  /* PR_SET_NAME */
    if (op == 16) {
        const char* n = "apk";
        copy_to_user((void*)a2, n, 4);
        return 0;
    }
    return 0; /* be permissive */
}

/* ------------------------------------------------------------------ */
/* File extras                                                          */
/* ------------------------------------------------------------------ */

/* sys_ftruncate (77) */
static int64_t sys_linux_ftruncate(int fd, int64_t length) {
    if (!fd_valid(fd)) return EBADF;
    fd_entry_t* f = &fd_table[fd];
    if (length < 0) return EINVAL;
    unsigned int newlen = (unsigned int)length;
    char* buf = (char*)kmalloc(newlen + 1);
    if (!buf) return ENOMEM;
    memset(buf, 0, newlen + 1);
    if (f->size > 0 && newlen > 0) {
        unsigned int copy = f->size < newlen ? f->size : newlen;
        char* old = (char*)kmalloc(f->size);
        if (old) {
            vfs_read_file(f->path, old, f->size);
            memcpy(buf, old, copy);
            kfree(old);
        }
    }
    vfs_write_file(f->path, buf, newlen);
    kfree(buf);
    f->size = newlen;
    return 0;
}

/* sys_truncate (76) */
static int64_t sys_linux_truncate(const char* upath, int64_t length) {
    char path[512];
    if (resolve_user_path(upath, path, sizeof(path)) < 0) return ENOENT;
    if (length == 0) {
        vfs_write_file(path, "", 0);
        return 0;
    }
    return 0;
}

/* sys_fsync (74) / fdatasync (75) — we write through, so no-op */
static int64_t sys_linux_fsync(int fd) {
    /* apk fsyncs cache/db files before renaming them into place.
     * Flush the buffered writes to disk but keep the buffer so
     * subsequent reads/writes on this fd still work. */
    if (!fd_valid(fd)) return EBADF;
    fd_entry_t* f = &fd_table[fd];
    if (f->wbuf && f->size > 0 && f->path[0])
        vfs_write_file(f->path, f->wbuf, f->size);
    return 0;
}

/* sys_fchdir (81) */
static int64_t sys_linux_fchdir(int fd) {
    if (!fd_valid(fd)) return EBADF;
    fd_entry_t* d = &fd_table[fd];
    if (!(d->flags & FD_FLAG_DIR)) return ENOTDIR;
    /* The fd already holds a resolved absolute path; chdir to it directly.
       (Used by apk before running package scripts: fchdir(root_fd).) */
    if (vfs_chdir(d->path) < 0) return ENOENT;
    return 0;
}

/* sys_chroot (161) — there is only one root here. apk calls chroot(".")
   after fchdir(root_fd); when "." is already "/" that is a no-op, so accept
   it. Anything else is unsupported. */
static int64_t sys_linux_chroot(const char* upath) {
    char path[512];
    if (resolve_user_path(upath, path, sizeof(path)) < 0) return ENOENT;
    if (path[0] == '/' && path[1] == '\0') return 0;
    return -1; /* EPERM */
}

/* sys_getdents (78) — redirect to getdents64 */
static int64_t sys_linux_getdents(int fd, void* ubuf, unsigned int count) {
    return sys_linux_getdents64(fd, ubuf, count);
}

/* sys_sendfile (40) — copy between fds in kernel */
static int64_t sys_linux_sendfile(int outfd, int infd, int64_t* uoffset,
                                   uint64_t count) {
    if (!fd_valid(outfd) || !fd_valid(infd)) return EBADF;
    uint64_t offset = 0;
    if (uoffset) {
        int64_t tmp;
        if (copy_from_user(&tmp, uoffset, sizeof(tmp)) != 0) return EFAULT;
        if (tmp >= 0) offset = (uint64_t)tmp;
    }
    char* buf = (char*)kmalloc((unsigned int)count);
    if (!buf) return ENOMEM;
    fd_entry_t* fin = &fd_table[infd];
    unsigned int saved = fin->offset;
    fin->offset = (unsigned int)offset;
    int64_t nr = sys_linux_read(infd, buf, count);
    if (uoffset) {
        int64_t new_off = (int64_t)fin->offset;
        copy_to_user(uoffset, &new_off, sizeof(new_off));
    } else {
        fin->offset = saved + (nr > 0 ? (unsigned int)nr : 0);
    }
    if (nr > 0) {
        int64_t nw = sys_linux_write(outfd, buf, (uint64_t)nr);
        kfree(buf);
        return nw;
    }
    kfree(buf);
    return nr;
}

/* sys_umask (95) */
static int64_t sys_linux_umask(int mask) { (void)mask; return 022; }

/* sys_chown / lchown / fchown (92/94/93) — no-op */
static int64_t sys_linux_chown(const char* p, int u, int g)  { (void)p;(void)u;(void)g; return 0; }
static int64_t sys_linux_fchown(int f, int u, int g)         { (void)f;(void)u;(void)g; return 0; }
static int64_t sys_linux_lchown(const char* p, int u, int g) { (void)p;(void)u;(void)g; return 0; }

/* sys_utimensat (280) / utime (132) / utimes (235) */
static int64_t sys_linux_utimensat(int dfd, const char* upath,
                                    const void* times, int flags) {
    (void)dfd;(void)upath;(void)times;(void)flags;
    return 0;
}

/* sys_statfs (137) / fstatfs (138) */
typedef struct {
    int64_t f_type, f_bsize, f_blocks, f_bfree, f_bavail;
    int64_t f_files, f_ffree, f_fsid[2], f_namelen, f_frsize, f_flags;
    int64_t f_spare[4];
} linux_statfs_t;

static int64_t sys_linux_statfs(const char* upath, linux_statfs_t* ubuf) {
    (void)upath;
    linux_statfs_t s;
    memset(&s, 0, sizeof(s));
    s.f_type    = 0x4d44;    /* MSDOS_SUPER_MAGIC (close enough) */
    s.f_bsize   = 512;
    s.f_blocks  = 131072;    /* ~64MB */
    s.f_bfree   = 65536;
    s.f_bavail  = 65536;
    s.f_namelen = 255;
    if (copy_to_user(ubuf, &s, sizeof(s)) != 0) return EFAULT;
    return 0;
}
static int64_t sys_linux_fstatfs(int fd, linux_statfs_t* ubuf) {
    (void)fd; return sys_linux_statfs((const char*)0, ubuf);
}

/* sys_getrlimit / setrlimit / prlimit (97/160/302) */
typedef struct { uint64_t rlim_cur, rlim_max; } linux_rlimit_t;
#define RLIM_INFINITY (~0ULL)

static int64_t sys_linux_getrlimit(int res, linux_rlimit_t* ubuf) {
    linux_rlimit_t r = { RLIM_INFINITY, RLIM_INFINITY };
    if (res == 7) { r.rlim_cur = 8192; r.rlim_max = 8192; } /* RLIMIT_NOFILE */
    if (copy_to_user(ubuf, &r, sizeof(r)) != 0) return EFAULT;
    return 0;
}
static int64_t sys_linux_setrlimit(int res, const linux_rlimit_t* ubuf) {
    (void)res;(void)ubuf; return 0;
}
static int64_t sys_linux_prlimit(int pid, int res,
                                  const linux_rlimit_t* unew,
                                  linux_rlimit_t* uold) {
    (void)pid;(void)unew;
    if (uold) return sys_linux_getrlimit(res, uold);
    return 0;
}

/* sys_sysinfo (99) */
typedef struct {
    int64_t  uptime;
    uint64_t loads[3];
    uint64_t totalram, freeram, sharedram, bufferram;
    uint64_t totalswap, freeswap;
    uint16_t procs;
    uint64_t totalhigh, freehigh;
    uint32_t mem_unit;
    char     _pad[20];
} linux_sysinfo_t;

static int64_t sys_linux_sysinfo(linux_sysinfo_t* ubuf) {
    linux_sysinfo_t s;
    memset(&s, 0, sizeof(s));
    s.uptime   = 42;
    s.totalram = 8ULL * 1024 * 1024 * 1024;
    s.freeram  = 4ULL * 1024 * 1024 * 1024;
    s.procs    = 2;
    s.mem_unit = 1;
    if (copy_to_user(ubuf, &s, sizeof(s)) != 0) return EFAULT;
    return 0;
}

/* sys_sched_yield (24) */
static int64_t sys_linux_sched_yield(void) { yield(); return 0; }

/* sys_sched_getaffinity (204). Single CPU: report cpu 0 online.
   Returns the number of bytes written to the mask, which is what musl's
   sched_getaffinity / get_nprocs (used by fastfetch for the core count)
   expects. */
static int64_t sys_linux_sched_getaffinity(int pid, unsigned int len, void* umask) {
    (void)pid;
    if (!umask || len == 0) return EINVAL;
    unsigned int n = len < 128 ? len : 128;   /* cap the zero-fill */
    unsigned char mask[128];
    memset(mask, 0, n);
    mask[0] = 0x01;                            /* cpu 0 present */
    if (copy_to_user(umask, mask, n) != 0) return EFAULT;
    return (int64_t)n;
}

/* sys_madvise (28), msync (26), mincore (27) — stubs */
static int64_t sys_linux_madvise(uint64_t a, uint64_t l, int adv) {
    (void)a;(void)l;(void)adv; return 0;
}
static int64_t sys_linux_msync(uint64_t a, uint64_t l, int f) {
    (void)a;(void)l;(void)f; return 0;
}

/* sys_splice (275) — pipe-to-fd or fd-to-pipe copy */
static int64_t sys_linux_splice(int fdin, int64_t* ofin, int fdout,
                                 int64_t* ofout, uint64_t len, int flags) {
    (void)ofin;(void)ofout;(void)flags;
    /* Simple: read from fdin into temp buf, write to fdout */
    if (len == 0) return 0;
    unsigned int n = (unsigned int)(len > 65536 ? 65536 : len);
    char* buf = (char*)kmalloc(n);
    if (!buf) return ENOMEM;
    int64_t nr;
    if (fd_is_pipe(fdin))
        nr = pipe_read(fdin, buf, n);
    else
        nr = sys_linux_read(fdin, buf, n);
    if (nr <= 0) { kfree(buf); return nr; }
    int64_t nw;
    if (fd_is_pipe(fdout))
        nw = pipe_write(fdout, buf, (uint64_t)nr);
    else
        nw = sys_linux_write(fdout, buf, (uint64_t)nr);
    kfree(buf);
    return nw;
}

/* sys_fallocate (285) — just extend the file */
static int64_t sys_linux_fallocate(int fd, int mode, int64_t offset, int64_t len) {
    (void)mode;
    if (!fd_valid(fd)) return EBADF;
    int64_t end = offset + len;
    if (end > (int64_t)fd_table[fd].size)
        return sys_linux_ftruncate(fd, end);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Socket syscalls — backed by the kernel TCP stack                    */
/* ------------------------------------------------------------------ */

#include "tcp.h"
#include "net.h"
int dns_resolve(const char* hostname, unsigned int* out_ip); /* forward decl */

#define AF_UNIX     1
#define AF_INET     2
#define AF_INET6    10
#define AF_NETLINK  16
#define SOCK_NONBLOCK 0x800
#define SOCK_STREAM 1
#define SOCK_DGRAM  2

/* Redefine sock_entry with proper tcp_conn_t type now that tcp.h is included */
/* (the forward declaration used void* — patch the tcp field access via cast) */

static void sock_table_ensure(void) {
    if (sock_table_init) return;
    memset(sock_table, 0, sizeof(sock_table));
    sock_table_init = 1;
}

static int sock_alloc(void) {
    sock_table_ensure();
    for (int i = 0; i < SOCK_MAX; i++)
        if (!sock_table[i].used) return i;
    return -1;
}

static int fd_is_sock(int fd) {
    return fd_valid(fd) && (fd_table[fd].flags & FD_FLAG_SOCK);
}

static sock_entry_t* fd_to_sock(int fd) {
    if (!fd_is_sock(fd)) return 0;
    int si = (int)fd_table[fd].offset;
    if (si < 0 || si >= SOCK_MAX) return 0;
    return &sock_table[si];
}

/* sockaddr_in layout (Linux) */
typedef struct {
    uint16_t sin_family;
    uint16_t sin_port;      /* network byte order */
    uint32_t sin_addr;      /* network byte order */
    uint8_t  sin_zero[8];
} linux_sockaddr_in_t;

static unsigned short bswap16(unsigned short v) {
    return (unsigned short)((v >> 8) | (v << 8));
}
static unsigned int bswap32(unsigned int v) {
    return ((v & 0xFF) << 24) | (((v >> 8) & 0xFF) << 16) |
           (((v >> 16) & 0xFF) << 8) | ((v >> 24) & 0xFF);
}

/* sys_socket (41) */
static int64_t sys_linux_socket(int domain, int type, int proto) {
    (void)proto;
    /* Unsupported families */
    if (domain == AF_UNIX)   return ENOENT;   /* nscd not available */
    if (domain == AF_INET6)  return -97;      /* EAFNOSUPPORT */
    if (domain == AF_NETLINK) {
        /* Allocate a dummy socket for netlink — connect/send/recv do nothing */
        int si = sock_alloc();
        if (si < 0) return ENOMEM;
        int fd = fd_alloc();
        if (fd < 0) return EMFILE;
        sock_table[si].used   = 1;
        sock_table[si].domain = domain;
    sock_table[si].type   = type & ~(0x800 | 0x80000); /* strip SOCK_NONBLOCK|SOCK_CLOEXEC */
        sock_table[si].proto  = proto;
        sock_table[si].tcp    = 0;
        fd_table[fd].flags  = FD_FLAG_USED | FD_FLAG_SOCK;
        fd_table[fd].offset = (unsigned int)si;
        fd_table[fd].size   = 0;
        return fd;
    }

    int si = sock_alloc();
    if (si < 0) return ENOMEM;
    int fd = fd_alloc();
    if (fd < 0) return EMFILE;

    sock_table[si].used   = 1;
    sock_table[si].domain = domain;
    sock_table[si].type   = type & ~(0x800 | 0x80000); /* strip SOCK_NONBLOCK|SOCK_CLOEXEC */
    sock_table[si].proto  = proto;
    sock_table[si].tcp    = 0;
    sock_table[si].rxhead = 0;
    sock_table[si].rxtail = 0;

    fd_table[fd].flags  = FD_FLAG_USED | FD_FLAG_SOCK;
    fd_table[fd].offset = (unsigned int)si;
    fd_table[fd].size   = 0;
    return fd;
}

/* sys_connect (42) */
static int64_t sys_linux_connect(int fd, const void* uaddr, uint32_t addrlen) {
    (void)addrlen;
    sock_entry_t* s = fd_to_sock(fd);
    if (!s) return EBADF;

    linux_sockaddr_in_t sa;
    if (copy_from_user(&sa, uaddr, sizeof(sa)) != 0) return EFAULT;

    /* AF_UNSPEC = disconnect */
    if (sa.sin_family == 0) {
        s->dst_ip = 0; s->dst_port = 0;
        return 0;
    }

    unsigned int   dst_ip   = bswap32(sa.sin_addr);
    unsigned short dst_port = bswap16(sa.sin_port);

    /* DGRAM sockets: just record destination, no real connection */
    if (s->type == SOCK_DGRAM) {
        s->dst_ip   = dst_ip;
        s->dst_port = dst_port;
        return 0;
    }

    /* Netlink / unknown domain: no-op */
    if (s->domain != AF_INET) return 0;

    /* STREAM sockets: check domain then do real TCP connect */
    if (s->type != SOCK_STREAM) return EINVAL;

    tcp_conn_t* c = tcp_connect(dst_ip, dst_port);
    if (!c) return -111; /* ECONNREFUSED */
    s->tcp      = (void*)c;
    s->dst_ip   = dst_ip;
    s->dst_port = dst_port;
    s->rx_total = 0; s->rx_sum = 0;
    return 0;
}

/* sys_send / sendto (44) */
/* Minimal DNS response builder.
 * Copies the query ID and question, then appends one A record answer. */
static int dns_make_response(const uint8_t* query, int qlen,
                              uint32_t ip, uint8_t* out, int outmax) {
    if (qlen < 12 || outmax < 12) return 0;
    /* Copy header, set QR=1 AA=1 RA=1 */
    out[0] = query[0]; out[1] = query[1]; /* ID */
    out[2] = 0x81; out[3] = 0x80;         /* QR=1, RA=1 */
    out[4] = query[4]; out[5] = query[5]; /* QDCOUNT */
    out[6] = 0x00; out[7] = 0x01;         /* ANCOUNT=1 */
    out[8] = 0x00; out[9] = 0x00;         /* NSCOUNT */
    out[10]= 0x00; out[11]= 0x00;         /* ARCOUNT */
    /* Copy question section */
    int qi = 12;
    int oi = 12;
    while (qi < qlen && oi < outmax - 16) {
        out[oi++] = query[qi];
        if (query[qi] == 0) { qi++; break; }
        qi++;
    }
    /* QTYPE + QCLASS (4 bytes) */
    if (qi + 4 <= qlen && oi + 4 <= outmax) {
        out[oi++] = query[qi++]; out[oi++] = query[qi++];
        out[oi++] = query[qi++]; out[oi++] = query[qi++];
    }
    /* Answer: pointer to name (0xC00C), TYPE A, CLASS IN, TTL, RDLEN=4, IP */
    if (oi + 16 <= outmax) {
        out[oi++] = 0xC0; out[oi++] = 0x0C; /* name ptr to offset 12 */
        out[oi++] = 0x00; out[oi++] = 0x01; /* TYPE A */
        out[oi++] = 0x00; out[oi++] = 0x01; /* CLASS IN */
        out[oi++] = 0x00; out[oi++] = 0x00; out[oi++] = 0x00; out[oi++] = 0x3C; /* TTL=60 */
        out[oi++] = 0x00; out[oi++] = 0x04; /* RDLEN */
        out[oi++] = (ip >> 24) & 0xFF;
        out[oi++] = (ip >> 16) & 0xFF;
        out[oi++] = (ip >>  8) & 0xFF;
        out[oi++] = (ip      ) & 0xFF;
    }
    return oi;
}

/* DNS hostname table — hardcode CDN IPs so we don't need a real DNS server */
static uint32_t dns_resolve_hardcoded(const uint8_t* query, int qlen) {
    /* Extract hostname from DNS question (skip header, decode labels) */
    char hostname[256];
    int hi = 0;
    int qi = 12;
    while (qi < qlen && query[qi] != 0 && hi < 255) {
        int label_len = query[qi++];
        if (label_len >= 0x40) break; /* pointer or invalid */
        for (int i = 0; i < label_len && qi < qlen && hi < 255; i++)
            hostname[hi++] = (char)query[qi++];
        if (query[qi] != 0) hostname[hi++] = '.';
    }
    hostname[hi] = '\0';
    printf("[dns] query for: '%s'\n", hostname);
    if (strcmp(hostname, "dl-cdn.alpinelinux.org") == 0 ||
        strcmp(hostname, "dl-cdn.alpinelinux.org.") == 0)
        return 0x97650284UL; /* 151.101.2.132 */

    /* Default: return a loopback to make it fail gracefully */
    return 0x7F000001UL; /* 127.0.0.1 */
}

static int64_t sys_linux_sendto(int fd, const void* ubuf, uint64_t len,
                                 int flags, const void* uaddr, uint32_t addrlen) {
    (void)flags;
    sock_entry_t* s = fd_to_sock(fd);
    if (!s) return EBADF;

    /* Handle DNS UDP: DGRAM socket sending to port 53 */
    if (s->type == SOCK_DGRAM) {
        /* Read the destination address if provided */
        uint16_t dst_port = s->dst_port;
        if (uaddr) {
            linux_sockaddr_in_t sa;
            if (copy_from_user(&sa, uaddr, sizeof(sa)) == 0)
                dst_port = bswap16(sa.sin_port);
        }
        if (dst_port == 53) {
            /* DNS query — extract hostname, resolve via real network, build response */
            uint8_t* kbuf = (uint8_t*)kmalloc((unsigned int)len);
            if (!kbuf) return ENOMEM;
            copy_from_user(kbuf, ubuf, (unsigned int)len);
            printf("[dns-intercept] got query len=%u\n", (unsigned)len);

            /* Extract hostname from DNS question */
            char hostname[256];
            int hi = 0, qi = 12;
            while (qi < (int)len && kbuf[qi] != 0 && hi < 255) {
                int label_len = kbuf[qi++];
                if (label_len >= 0x40) break;
                for (int li = 0; li < label_len && qi < (int)len && hi < 254; li++)
                    hostname[hi++] = (char)kbuf[qi++];
                if (kbuf[qi] != 0) hostname[hi++] = '.';
            }
            hostname[hi] = '\0';
            printf("[dns] resolving '%s' via real network...\n", hostname);

            /* Try real DNS first, fall back to hardcoded table */
            unsigned int resolved_ip = 0x7F000001UL; /* 127.0.0.1 fallback */
            if (dns_resolve(hostname, &resolved_ip)) {
                printf("[dns] resolved to %u.%u.%u.%u\n",
                       (resolved_ip>>24)&0xFF, (resolved_ip>>16)&0xFF,
                       (resolved_ip>>8)&0xFF, resolved_ip&0xFF);
            } else if (strcmp(hostname, "dl-cdn.alpinelinux.org") == 0) {
                resolved_ip = 0x97650284UL; /* 151.101.2.132 hardcoded fallback */
                printf("[dns] using hardcoded IP for %s\n", hostname);
            }

            int rlen = dns_make_response(kbuf, (int)len, resolved_ip,
                                         (uint8_t*)s->rxbuf, (int)sizeof(s->rxbuf));
            kfree(kbuf);
            s->rxhead = 0;
            s->rxtail = (unsigned)rlen;
            return (int64_t)len;
        }
        /* Non-DNS UDP: just pretend it succeeded */
        return (int64_t)len;
    }

    /* TCP path */
    if (!s->tcp && uaddr && s->type == SOCK_STREAM) {
        int64_t r = sys_linux_connect(fd, uaddr, addrlen);
        if (r < 0) return r;
    }
    /* Keep-alive reuse: apk/libfetch reuses one socket for several HTTP
     * requests. If the peer closed the connection between requests (FIN/RST),
     * transparently reconnect instead of sending into a dead connection. */
    if (s->tcp && !tcp_alive((tcp_conn_t*)s->tcp) && s->dst_ip) {
        printf("[tcp] connection was closed by peer - reconnecting\n");
        tcp_conn_t* nc = tcp_connect(s->dst_ip, s->dst_port);
        if (!nc) return -111; /* ECONNREFUSED */
        s->tcp = (void*)nc;
        s->rx_total = 0; s->rx_sum = 0;
    }
    if (!s->tcp) return ENOTCONN;

    char* kbuf = (char*)kmalloc((unsigned int)len + 64);
    if (!kbuf) return ENOMEM;
    if (copy_from_user(kbuf, ubuf, (unsigned int)len) != 0) { kfree(kbuf); return EFAULT; }

    /* Rewrite Host: header for Alpine CDN IPs — CDN needs hostname not IP */
    unsigned int send_len = (unsigned int)len;
    if (s->tcp && send_len > 4) {
        /* Rewrite Host: header — works on any chunk containing it */
        char* p = kbuf;
        char* end = kbuf + send_len;
        while (p < end - 6) {
            if (p[0]=='H' && p[1]=='o' && p[2]=='s' && p[3]=='t' &&
                p[4]==':' && p[5]==' ') {
                char* val = p + 6;
                if (val < end && val[0] >= '0' && val[0] <= '9') {
                    const char* newhost = "dl-cdn.alpinelinux.org";
                    int nhlen = 22;
                    char* eol = val;
                    while (eol < end && *eol != '\r' && *eol != '\n') eol++;
                    int oldlen = (int)(eol - val);
                    int diff = nhlen - oldlen;
                    if (diff != 0 && (int)send_len + diff > 0 &&
                        (int)send_len + diff < (int)len + 64) {
                        if (diff > 0) {
                            for (int i = (int)(end-eol)-1; i >= 0; i--)
                                eol[diff+i] = eol[i];
                        } else {
                            for (int i = 0; i < (int)(end-eol); i++)
                                eol[diff+i] = eol[i];
                        }
                        send_len = (unsigned int)((int)send_len + diff);
                    }
                    for (int i = 0; i < nhlen; i++) val[i] = newhost[i];
                }
                break;
            }
            p++;
        }
    }

    /* Debug: log the HTTP request line so failures are visible on console */
    if (send_len >= 4 &&
        ((kbuf[0]=='G'&&kbuf[1]=='E'&&kbuf[2]=='T'&&kbuf[3]==' ') ||
         (kbuf[0]=='H'&&kbuf[1]=='E'&&kbuf[2]=='A'&&kbuf[3]=='D') ||
         (kbuf[0]=='P'&&kbuf[1]=='O'&&kbuf[2]=='S'&&kbuf[3]=='T'))) {
        char line[100]; unsigned int li = 0;
        while (li < send_len && li < sizeof(line)-1 &&
               kbuf[li] != '\r' && kbuf[li] != '\n') { line[li] = kbuf[li]; li++; }
        line[li] = '\0';
        printf("[http] > %s\n", line);
        s->http_dbg = 1;   /* log the status line of the next response */
    }

    int r = tcp_send((tcp_conn_t*)s->tcp, kbuf, send_len);
    kfree(kbuf);
    return r < 0 ? EIO : (int64_t)len;
}

/* sys_recv / recvfrom (45) */
static int64_t sys_linux_recvfrom(int fd, void* ubuf, uint64_t len,
                                   int flags, void* uaddr, uint32_t* uaddrlen) {
    (void)flags;
    sock_entry_t* s = fd_to_sock(fd);
    if (!s) return EBADF;

    /* UDP path: serve from rxbuf (DNS response or nothing) */
    if (s->type == SOCK_DGRAM) {
        unsigned int avail = s->rxtail - s->rxhead;
        if (avail == 0) return -11; /* EAGAIN — no data */
        unsigned int copy = avail < (unsigned int)len ? avail : (unsigned int)len;
        if (copy_to_user(ubuf, s->rxbuf + s->rxhead, copy) != 0) return EFAULT;
        /* Fill in sender address if requested */
        if (uaddr && uaddrlen) {
            linux_sockaddr_in_t sa;
            memset(&sa, 0, sizeof(sa));
            sa.sin_family = AF_INET;
            sa.sin_port   = bswap16(53);
            sa.sin_addr   = bswap32(0x08080808); /* 8.8.8.8 */
            uint32_t slen = sizeof(sa);
            copy_to_user(uaddr, &sa, sizeof(sa));
            copy_to_user(uaddrlen, &slen, sizeof(slen));
        }
        s->rxhead += copy;
        return (int64_t)copy;
    }

    /* TCP path */
    if (!s->tcp) return ENOTCONN;
    char* kbuf = (char*)kmalloc((unsigned int)len);
    if (!kbuf) return ENOMEM;
    int r = tcp_recv((tcp_conn_t*)s->tcp, kbuf, (unsigned int)len, 120000);
    if (r < 0)  {
        printf("[tcp] rx done: total=%d sum=0x%x\n",
               (int)s->rx_total, (unsigned int)s->rx_sum);
        kfree(kbuf); return 0;                  /* FIN/RST => real EOF */
    }
    if (r == 0) {
        printf("[tcp] recv TIMEOUT after %d bytes!\n", (int)s->rx_total);
        kfree(kbuf); return EAGAIN;             /* timeout: NOT EOF */
    }
    s->rx_total += (uint64_t)r;
    for (int di = 0; di < r; di++) s->rx_sum += (unsigned char)kbuf[di];
    /* Debug: log the HTTP status line of the response */
    if (s->http_dbg) {
        s->http_dbg = 0;
        char line[100]; int li = 0;
        while (li < r && li < (int)sizeof(line)-1 &&
               kbuf[li] != '\r' && kbuf[li] != '\n') { line[li] = kbuf[li]; li++; }
        line[li] = '\0';
        printf("[http] < %s\n", line);
    }
    if (copy_to_user(ubuf, kbuf, (unsigned int)r) != 0) { kfree(kbuf); return EFAULT; }
    kfree(kbuf);
    return (int64_t)r;
}

/* sys_bind (49), listen (50), accept (43) — server stubs */
static int64_t sys_linux_bind(int fd, const void* uaddr, uint32_t len) {
    (void)fd;(void)uaddr;(void)len; return 0;
}
static int64_t sys_linux_listen(int fd, int backlog) { (void)fd;(void)backlog; return 0; }
static int64_t sys_linux_accept(int fd, void* uaddr, uint32_t* ulen) {
    (void)fd;(void)uaddr;(void)ulen; return -EINVAL;
}
static int64_t sys_linux_accept4(int fd, void* uaddr, uint32_t* ulen, int fl) {
    (void)fl; return sys_linux_accept(fd, uaddr, ulen);
}

/* sys_getsockname (51) / getpeername (52) */
static int64_t sys_linux_getsockname(int fd, void* uaddr, uint32_t* ulen) {
    sock_entry_t* s = fd_to_sock(fd);
    if (!s) return EBADF;
    linux_sockaddr_in_t sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr   = bswap32(0x0A00020FU); /* 10.0.2.15 */
    sa.sin_port   = 0;
    uint32_t slen = sizeof(sa);
    copy_to_user(uaddr, &sa, sizeof(sa));
    copy_to_user(ulen, &slen, sizeof(slen));
    return 0;
}
static int64_t sys_linux_getpeername(int fd, void* uaddr, uint32_t* ulen) {
    sock_entry_t* s = fd_to_sock(fd);
    if (!s) return EBADF;
    linux_sockaddr_in_t sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr   = bswap32(s->dst_ip);
    sa.sin_port   = bswap16(s->dst_port);
    uint32_t slen = sizeof(sa);
    copy_to_user(uaddr, &sa, sizeof(sa));
    copy_to_user(ulen, &slen, sizeof(slen));
    return 0;
}

/* sys_setsockopt (54) / getsockopt (55) */
static int64_t sys_linux_setsockopt(int fd, int lv, int opt,
                                     const void* uval, uint32_t len) {
    (void)fd;(void)lv;(void)opt;(void)uval;(void)len; return 0;
}
static int64_t sys_linux_getsockopt(int fd, int lv, int opt,
                                     void* uval, uint32_t* ulen) {
    (void)fd;(void)lv;
    /* SO_ERROR (4) — return 0 meaning no error (connect succeeded) */
    if (opt == 4 && uval) {
        int zero = 0;
        copy_to_user(uval, &zero, sizeof(zero));
        if (ulen) { uint32_t sz = 4; copy_to_user(ulen, &sz, sizeof(sz)); }
    }
    return 0;
}

/* sys_shutdown (48) */
static int64_t sys_linux_shutdown(int fd, int how) {
    (void)how;
    sock_entry_t* s = fd_to_sock(fd);
    if (!s) return EBADF;
    if (s->tcp) { tcp_close((tcp_conn_t*)s->tcp); s->tcp = 0; }
    return 0;
}

/* Socket read/write wrappers (used in dispatcher) */
static int64_t sock_read(int fd, char* buf, uint64_t len) {
    return sys_linux_recvfrom(fd, buf, len, 0, 0, 0);
}
static int64_t sock_write(int fd, const char* buf, uint64_t len) {
    return sys_linux_sendto(fd, buf, len, 0, 0, 0);
}

/* ------------------------------------------------------------------ */
/* Epoll stubs (apk uses epoll for async I/O on some paths)            */
/* ------------------------------------------------------------------ */
static int64_t sys_linux_epoll_create(int s) { (void)s; return sys_linux_socket(AF_INET, 0, 0); }
static int64_t sys_linux_epoll_ctl(int ep, int op, int fd, void* ev) {
    (void)ep;(void)op;(void)fd;(void)ev; return 0;
}
static int64_t sys_linux_epoll_wait(int ep, void* ev, int mx, int to) {
    (void)ep;(void)ev;(void)mx;(void)to; return 0;
}
static int64_t sys_linux_epoll_pwait(int ep, void* ev, int mx, int to, const void* sig) {
    (void)sig; return sys_linux_epoll_wait(ep, ev, mx, to);
}

/* sys_eventfd / eventfd2 (290/323) — create a simple pipe-backed fd */
static int64_t sys_linux_eventfd(unsigned int initval, int flags) {
    (void)initval;(void)flags;
    /* allocate a single pipe slot and return the write-end fd as the eventfd */
    int pi = pipe_alloc();
    if (pi < 0) return ENOMEM;
    int fd = fd_alloc();
    if (fd < 0) { pipe_pool[pi].read_open = pipe_pool[pi].write_open = 0; return EMFILE; }
    fd_table[fd].flags  = FD_FLAG_USED | FD_FLAG_PIPE_R | FD_FLAG_PIPE_W;
    fd_table[fd].offset = (unsigned int)pi;
    fd_table[fd].size   = 0;
    return fd;
}

/* sys_arch_prctl (158) */
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003
#define ARCH_SET_GS 0x1001

static int64_t sys_linux_arch_prctl(int code, uint64_t addr) {
    if (code == ARCH_SET_FS) {
        __asm__ volatile (
            "wrmsr"
            : : "c"(0xC0000100UL),
                "a"((uint32_t)addr),
                "d"((uint32_t)(addr >> 32))
        );
        task_t* ct = task_current();
        if (ct) ct->fs_base = addr;   /* remember it for task switches */
        return 0;
    }
    if (code == ARCH_GET_FS) {
        uint32_t lo, hi;
        __asm__ volatile (
            "rdmsr"
            : "=a"(lo), "=d"(hi)
            : "c"(0xC0000100UL)
        );
        uint64_t val = ((uint64_t)hi << 32) | lo;
        if (copy_to_user((void*)addr, &val, 8) != 0) return EFAULT;
        return 0;
    }
    if (code == ARCH_SET_GS) {
        __asm__ volatile (
            "wrmsr"
            : : "c"(0xC0000101UL),
                "a"((uint32_t)addr),
                "d"((uint32_t)(addr >> 32))
        );
        return 0;
    }
    return EINVAL;
}

/* sys_getcwd (79) */
static int64_t sys_linux_getcwd(char* ubuf, uint64_t size) {
    char tmp[512];
    if (vfs_getcwd(tmp, sizeof(tmp)) < 0) return EIO;
    unsigned int len = (unsigned int)strlen(tmp) + 1;
    if (len > (unsigned int)size) return EINVAL;
    if (copy_to_user(ubuf, tmp, len) != 0) return EFAULT;
    return (int64_t)(uint64_t)ubuf;
}

/* sys_openat (257) — common in modern libc */
static int64_t sys_linux_openat(int dirfd, const char* upath, int flags, int mode) {
    char full[512];
    int jr = at_join_path(dirfd, upath, full, sizeof(full));
    if (jr < 0) return jr;
    /* For /etc/hosts specifically, also intercept when opened via openat */
    if (strcmp(full, "hosts") == 0 || strcmp(full, "/etc/hosts") == 0)
        return sys_linux_open("/etc/hosts", flags, mode);
    return sys_linux_open(full, flags, mode);
}

/* sys_fstatat/newfstatat (262) */
static int64_t sys_linux_newfstatat(int dirfd, const char* upath,
                                     linux_stat_t* ustat, int flags) {
    char full[512];
    int jr = at_join_path(dirfd, upath, full, sizeof(full));
    if (jr < 0) return jr;
    char path[512];
    if (resolve_user_path(full, path, sizeof(path)) < 0) return ENOENT;
    if (!(flags & AT_SYMLINK_NOFOLLOW))
        follow_symlinks(path, sizeof(path));
    return fill_stat(path, ustat);
}

/* sys_ioctl (16) — stub for terminal ioctls */
static int64_t sys_linux_ioctl(int fd, uint64_t req, uint64_t arg) {
    if (!fd_valid(fd)) return EBADF;
    int is_tty = (fd_table[fd].flags & FD_FLAG_TTY) != 0;

    switch (req) {
    case 0x5401:                    /* TCGETS */
        if (!is_tty) return LX_ENOTTY;
        if (copy_to_user((void*)arg, &tty_tio, sizeof(tty_tio)) != 0) return EFAULT;
        return 0;
    case 0x5402: case 0x5403: case 0x5404: {   /* TCSETS / TCSETSW / TCSETSF */
        if (!is_tty) return LX_ENOTTY;
        lx_termios_t t;
        if (copy_from_user(&t, (const void*)arg, sizeof(t)) != 0) return EFAULT;
        tty_tio = t;
        if (req == 0x5404) tty_line_len = tty_line_pos = 0;   /* flush input */
        return 0;
    }
    case 0x5413: {                  /* TIOCGWINSZ — this is how musl's isatty() asks */
        if (!is_tty) return LX_ENOTTY;
        uint16_t ws[4] = { (uint16_t)con_rows(), (uint16_t)con_cols(), 0, 0 };
        if (copy_to_user((void*)arg, ws, sizeof(ws)) != 0) return EFAULT;
        return 0;
    }
    case 0x5414:                    /* TIOCSWINSZ */
        return is_tty ? 0 : LX_ENOTTY;
    case 0x540F: {                  /* TIOCGPGRP: one session; foreground = getpgrp() */
        if (!is_tty) return LX_ENOTTY;
        int pg = 1;
        if (copy_to_user((void*)arg, &pg, sizeof(pg)) != 0) return EFAULT;
        return 0;
    }
    case 0x5410:                    /* TIOCSPGRP */
    case 0x540E:                    /* TIOCSCTTY */
        return is_tty ? 0 : LX_ENOTTY;
    case 0x541B: {                  /* FIONREAD: bytes ready without blocking */
        int n = 0;
        if (is_tty) n = (int)(tty_line_len - tty_line_pos);
        if (copy_to_user((void*)arg, &n, sizeof(n)) != 0) return EFAULT;
        return 0;
    }
    case 0x5421:                    /* FIONBIO */
    case 0x5451: case 0x5450:       /* FIOCLEX / FIONCLEX */
        return 0;
    default:
        return is_tty ? EINVAL : LX_ENOTTY;
    }
}

/* sys_rt_sigaction (13), sys_rt_sigprocmask (14) — stub */
/* struct kernel_sigaction (x86_64): handler@0, flags@8, restorer@16, mask@24 */
typedef struct { uint64_t handler, flags, restorer, mask; } lx_sigaction_t;

static int64_t sys_linux_rt_sigaction(int sig, void* act, void* oact, uint64_t sz) {
    (void)sz;
    task_t* t = task_current();
    if (!t || sig <= 0 || sig >= 32) return EINVAL;
    if (oact) {
        lx_sigaction_t old = {0};
        old.handler  = t->sig_handlers[sig];
        old.restorer = t->sig_restorer;
        if (copy_to_user(oact, &old, sizeof(old)) != 0) return EFAULT;
    }
    if (act) {
        lx_sigaction_t na;
        if (copy_from_user(&na, act, sizeof(na)) != 0) return EFAULT;
        /* SIGKILL and SIGSTOP cannot have their disposition changed. */
        if (sig != LX_SIGKILL && sig != LX_SIGSTOP) {
            t->sig_handlers[sig] = na.handler;
            if (na.restorer) t->sig_restorer = na.restorer;
        }
    }
    return 0;
}

static int64_t sys_linux_rt_sigprocmask(int how, void* set, void* oset, uint64_t sz) {
    (void)sz;
    task_t* t = task_current();
    if (!t) return EINVAL;
    if (oset) {
        uint64_t cur = t->sig_blocked;
        if (copy_to_user(oset, &cur, 8) != 0) return EFAULT;
    }
    if (set) {
        uint64_t v;
        if (copy_from_user(&v, set, 8) != 0) return EFAULT;
        uint32_t m = (uint32_t)v;
        if (how == 0)      t->sig_blocked |= m;        /* SIG_BLOCK   */
        else if (how == 1) t->sig_blocked &= ~m;       /* SIG_UNBLOCK */
        else if (how == 2) t->sig_blocked = m;         /* SIG_SETMASK */
        /* Never block the uncatchable signals. */
        t->sig_blocked &= ~((1u << LX_SIGKILL) | (1u << LX_SIGSTOP));
    }
    return 0;
}

/* Signal frame pushed onto the user stack when invoking a custom handler.
   Layout is private to this kernel: deliver pushes it, rt_sigreturn reads
   it back. `ret_addr` sits at the lowest address so the handler's final
   `ret` pops it and lands in the restorer, which calls rt_sigreturn(15). */
typedef struct {
    uint64_t ret_addr;                                  /* = sa_restorer      */
    uint64_t r8,r9,r10,r11,r12,r13,r14,r15;
    uint64_t rdi,rsi,rbp,rbx,rdx,rax,rcx;
    uint64_t rip, rflags, rsp;
    uint64_t blocked;                                   /* mask to restore    */
    uint64_t signo;
} lx_sigframe_t;

/* The six callee-saved user registers are pushed by linux_syscall_entry just
   below the linux_frame_t; index them the same way loader_fork does. */
static inline uint64_t* csaved(linux_frame_t* f) { return (uint64_t*)f; }

/* Deliver at most one pending signal by rewriting `frame` so sysret enters
   either the handler (custom) or, for SIG_DFL/terminate, the task dies here.
   Called at the end of every syscall, after frame->rax is set. */
void deliver_signals(linux_frame_t* frame) {
    task_t* t = task_current();
    if (!t || !t->is_user) return;
    if (!signal_ready(t)) return;

    uint32_t ready = (t->sig_pending & ~t->sig_blocked)
                   | (t->sig_pending & ((1u<<LX_SIGKILL)|(1u<<LX_SIGSTOP)));
    int sig = 0;
    for (int i = 1; i < 32; i++) if (ready & (1u << i)) { sig = i; break; }
    if (!sig) return;
    t->sig_pending &= ~(1u << sig);

    uint64_t h = t->sig_handlers[sig];

    if (h == SIG_IGN_H || (h == SIG_DFL_H && sig_default_ignored(sig)))
        return;                                         /* ignore */

    if (h == SIG_DFL_H || sig == LX_SIGKILL) {
        /* Default action: terminate. Exit status = 128 + signo (shell view). */
        __asm__ volatile ("cli");
        task_exit_code(128 + sig);
        for (;;) __asm__ volatile ("hlt");              /* never returns */
    }

    /* Custom handler: build a signal frame on the user stack. */
    uint64_t* cs = csaved(frame);
    uint64_t sp = frame->user_rsp;
    sp -= 128;                                          /* skip the red zone */
    sp &= ~15ULL;
    sp -= sizeof(lx_sigframe_t);
    /* Entry ABI: on `call`, (rsp % 16) == 0 after the return addr is pushed,
       i.e. rsp points at ret_addr and (rsp+8) % 16 == 0. */
    if (((sp + 8) & 15) != 0) sp -= 8;

    lx_sigframe_t sf;
    sf.ret_addr = t->sig_restorer;
    sf.rip = frame->rcx; sf.rflags = frame->r11; sf.rsp = frame->user_rsp;
    sf.rax = frame->rax; sf.rbx = frame->rbx; sf.rcx = frame->rcx;
    sf.rdx = frame->rdx; sf.rsi = frame->rsi; sf.rdi = frame->rdi;
    sf.r8 = frame->r8; sf.r9 = frame->r9; sf.r10 = frame->r10; sf.r11 = frame->r11;
    sf.rbp = cs[-1]; sf.r12 = cs[-3]; sf.r13 = cs[-4]; sf.r14 = cs[-5]; sf.r15 = cs[-6];
    sf.blocked = t->sig_blocked;
    sf.signo = sig;

    if (copy_to_user((void*)sp, &sf, sizeof(sf)) != 0) {
        /* Can't push the frame (bad stack) — fall back to killing the task. */
        __asm__ volatile ("cli");
        task_exit_code(128 + sig);
        for (;;) __asm__ volatile ("hlt");
    }

    /* Block this signal while its handler runs (classic POSIX behaviour). */
    t->sig_blocked |= (1u << sig);

    /* Enter the handler: rip=handler, rdi=signo, rsp at the frame. */
    frame->rcx      = h;             /* sysret uses rcx as the new rip        */
    frame->rdi      = (uint64_t)sig; /* handler arg1                          */
    frame->user_rsp = sp;
    frame->r11      = frame->r11 | 0x200;   /* ensure IF set in handler       */
}

/* sys_rt_sigreturn (15): undo deliver_signals — restore the interrupted
   context from the signal frame at the top of the user stack. */
static int64_t sys_linux_rt_sigreturn(linux_frame_t* frame) {
    task_t* t = task_current();
    /* After the handler's `ret` popped ret_addr, user_rsp points just past
       it, at the saved-register area. Read the frame from one word back. */
    uint64_t sp = frame->user_rsp - 8;
    lx_sigframe_t sf;
    if (copy_from_user(&sf, (void*)sp, sizeof(sf)) != 0) return EFAULT;

    frame->rcx = sf.rip;            /* resume rip   */
    frame->r11 = sf.rflags;         /* resume flags */
    frame->user_rsp = sf.rsp;       /* resume rsp   */
    frame->rax = sf.rax; frame->rbx = sf.rbx;
    frame->rdx = sf.rdx; frame->rsi = sf.rsi; frame->rdi = sf.rdi;
    frame->r8 = sf.r8; frame->r9 = sf.r9; frame->r10 = sf.r10;
    uint64_t* cs = csaved(frame);
    cs[-1] = sf.rbp; cs[-3] = sf.r12; cs[-4] = sf.r13; cs[-5] = sf.r14; cs[-6] = sf.r15;
    if (t) t->sig_blocked = (uint32_t)sf.blocked;
    return (int64_t)sf.rax;         /* frame->rax already set; keep it        */
}

static int64_t fd_read_any(int fd, char* buf, uint64_t count) {
    if (fd_is_pipe(fd)) return pipe_read(fd, buf, count);
    if (fd_is_sock(fd)) return sock_read(fd, buf, count);
    return sys_linux_read(fd, buf, count);
}

static int64_t fd_write_any(int fd, const char* buf, uint64_t count) {
    if (fd_is_pipe(fd)) return pipe_write(fd, buf, count);
    if (fd_is_sock(fd)) return sock_write(fd, buf, count);
    return sys_linux_write(fd, buf, count);
}

/* ------------------------------------------------------------------ */
/* Main dispatcher                                                      */
/* ------------------------------------------------------------------ */

void linux_syscall_handler(linux_frame_t* frame) {
    fdtable_init();

    uint64_t num  = frame->rax;
    uint64_t a1   = frame->rdi;
    uint64_t a2   = frame->rsi;
    uint64_t a3   = frame->rdx;
    uint64_t a4   = frame->r10;

    /* uint64_t a5 = frame->r8; */
    /* uint64_t a6 = frame->r9; */

    int64_t ret = ENOSYS;
    switch (num) {
        case 0:   ret = fd_read_any ((int)a1, (char*)a2, a3); break;
        case 1:   ret = fd_write_any((int)a1, (const char*)a2, a3); break;
        case 17:  ret = sys_linux_pread((int)a1, (char*)a2, a3, (int64_t)a4, 0); break; /* pread64 */
        case 18:  ret = sys_linux_pread((int)a1, (char*)a2, a3, (int64_t)a4, 1); break; /* pwrite64 */
        case 19:  ret = sys_linux_readv((int)a1, (const iovec_t*)a2, (int)a3); break;
        case 2:   ret = sys_linux_open   ((const char*)a1, (int)a2, (int)a3); break;
        case 3:   ret = sys_linux_close  ((int)a1); break;
        case 4:   ret = sys_linux_stat   ((const char*)a1, (linux_stat_t*)a2); break;
        case 5:   ret = sys_linux_fstat  ((int)a1, (linux_stat_t*)a2); break;
        case 6:   ret = sys_linux_stat   ((const char*)a1, (linux_stat_t*)a2); break; /* lstat */
        case 8:   ret = sys_linux_lseek  ((int)a1, (int64_t)a2, (int)a3); break;
        case 9:   ret = sys_linux_mmap(a1,a2,(int)a3,(int)a4,(int)frame->r8,(int64_t)frame->r9); break;
        case 7: {
            /* poll(struct pollfd* fds, nfds_t nfds, int timeout) */
            typedef struct { int fd; short events; short revents; } pollfd_t;
            pollfd_t* pfds = (pollfd_t*)a1;
            int nfds = (int)a2;
            int nready = 0;
            for (int _pi = 0; _pi < nfds; _pi++) {
                pollfd_t pfd;
                if (copy_from_user(&pfd, &pfds[_pi], sizeof(pfd)) != 0) continue;
                pfd.revents = 0;
                if (pfd.fd >= 0) {
                    if (fd_is_sock(pfd.fd)) {
                        /* Sockets: always ready for read+write (we're single-threaded) */
                        if (pfd.events & 0x0004) pfd.revents |= 0x0004; /* POLLOUT */
                        if (pfd.events & 0x0001) pfd.revents |= 0x0001; /* POLLIN */
                    } else if (fd_valid(pfd.fd)) {
                        /* Regular files: always readable */
                        if (pfd.events & 0x0001) pfd.revents |= 0x0001; /* POLLIN */
                    }
                    if (pfd.revents) nready++;
                }
                copy_to_user(&pfds[_pi], &pfd, sizeof(pfd));
            }
            ret = nready;
            break;
        }
        case 10:  ret = sys_linux_mprotect(a1, a2, (int)a3); break;
        case 22:  ret = sys_linux_pipe    ((int*)a1, 0); break;
        case 32:  ret = sys_linux_dup     ((int)a1); break;
        case 33:  ret = sys_linux_dup2    ((int)a1, (int)a2); break;
        case 292: { /* dup3(old, new, flags) */
            if ((int)a1 == (int)a2) { ret = EINVAL; break; }
            ret = sys_linux_dup2((int)a1, (int)a2);
            if (ret >= 0 && (a3 & LX_O_CLOEXEC)) fd_table[ret].flags |= FD_FLAG_CLOEXEC;
            break;
        }
        case 11:  ret = sys_linux_munmap (a1, a2); break;
        case 12:  ret = sys_linux_brk(a1); break;
        case 13:  ret = sys_linux_rt_sigaction((int)a1, (void*)a2, (void*)a3, a4); break;
        case 15:  ret = sys_linux_rt_sigreturn(frame); break;  /* undo deliver_signals */
        case 14:  ret = sys_linux_rt_sigprocmask((int)a1,(void*)a2,(void*)a3,a4); break;
        case 16:  ret = sys_linux_ioctl  ((int)a1, a2, a3); break;
        case 20:  ret = sys_linux_writev ((int)a1, (const iovec_t*)a2, (int)a3); break;
        case 21:  ret = sys_linux_access ((const char*)a1, (int)a2); break;
        case 39:  ret = sys_linux_getpid (); break;
        case 57:  ret = sys_linux_fork   (frame); break;
        case 56:  ret = sys_linux_clone  (a1, a2, frame); break;
        case 59:  ret = sys_linux_execve ((const char*)a1, (const char* const*)a2, (const char* const*)a3, frame); break;
        case 82:  ret = sys_linux_rename ((const char*)a1, (const char*)a2); break;
        case 84:  ret = sys_linux_rmdir  ((const char*)a1); break;
        case 86:  ret = sys_linux_link   ((const char*)a1, (const char*)a2); break;
        case 88:  ret = sys_linux_symlink((const char*)a1, (const char*)a2); break;
        case 90:  ret = sys_linux_chmod  ((const char*)a1, (int)a2); break;
        case 91:  ret = sys_linux_fchmod ((int)a1, (int)a2); break;
        case 264: ret = sys_linux_renameat((int)a1,(const char*)a2,(int)a3,(const char*)a4); break;
        case 267: ret = sys_linux_readlinkat((int)a1,(const char*)a2,(char*)a3,a4); break;
        case 60:  ret = sys_linux_exit   ((int)a1); break;
        case 61:  ret = sys_linux_wait4  ((int)a1, (int*)a2, (int)a3); break;
        case 63:  ret = sys_linux_uname  ((linux_utsname_t*)a1); break;
        case 72:  ret = sys_linux_fcntl  ((int)a1, (int)a2, a3); break;
        case 73:  ret = 0; break; /* flock — stub */
        case 165: ret = sys_linux_getdents64((int)a1,(void*)a2,(unsigned int)a3); break; /* old getdents */
        case 166: ret = 0; break; /* umount2 — stub */
        case 201: { /* time(time_t*) */
            static uint64_t _fake_time = 1700000000ULL;
            _fake_time++;
            if (a1) copy_to_user((void*)a1, &_fake_time, 8);
            ret = (int64_t)_fake_time;
            break;
        }
        case 263: ret = sys_linux_unlinkat((int)a1,(const char*)a2,(int)a3); break; /* unlinkat */
        case 260: ret = 0; break; /* fchownat — no ownership on FAT32 */
        case 268: ret = 0; break; /* fchmodat — no permission bits on FAT32 */
        case 266: ret = sys_linux_symlinkat((const char*)a1,(int)a2,(const char*)a3); break;
        case 79:  ret = sys_linux_getcwd ((char*)a1, a2); break;
        case 80:  ret = sys_linux_chdir  ((const char*)a1); break;
        case 83:  ret = sys_linux_mkdir  ((const char*)a1, (int)a2); break;
        case 87:  ret = sys_linux_unlink ((const char*)a1); break;
        case 89:  ret = sys_linux_readlink((const char*)a1,(char*)a2,a3); break;
        case 102: ret = sys_linux_getuid (); break;
        case 104: ret = sys_linux_getgid (); break;
        case 107: ret = sys_linux_getuid (); break; /* geteuid */
        case 108: ret = sys_linux_getgid (); break; /* getegid */
        case 158: ret = sys_linux_arch_prctl((int)a1, a2); break;
        case 217: ret = sys_linux_getdents64((int)a1,(void*)a2,(unsigned int)a3); break;
        case 218: ret = sys_linux_getpid(); break; /* set_tid_address */
        case 273: ret = 0; break;  /* set_robust_list — stub */
        case 274: ret = 0; break;  /* get_robust_list — stub */
        case 231: ret = sys_linux_exit   ((int)a1); break; /* exit_group */
        case 257: ret = sys_linux_openat ((int)a1,(const char*)a2,(int)a3,(int)a4); break;
        case 262: ret = sys_linux_newfstatat((int)a1,(const char*)a2,(linux_stat_t*)a3,(int)a4); break;
        case 202: {
            /* futex(uaddr, op, val, timeout, uaddr2, val3)
             * Single-threaded: WAIT checks val and returns EAGAIN if mismatch,
             * else returns 0 (immediately "woken"). WAKE returns 0. */
            #define FUTEX_WAIT         0
            #define FUTEX_WAKE         1
            #define FUTEX_PRIVATE_FLAG 0x80
            #define FUTEX_WAIT_PRIVATE (FUTEX_WAIT|FUTEX_PRIVATE_FLAG)
            #define FUTEX_WAKE_PRIVATE (FUTEX_WAKE|FUTEX_PRIVATE_FLAG)
            int futex_op  = (int)a2 & ~FUTEX_PRIVATE_FLAG;
            uint32_t* futex_addr = (uint32_t*)(uintptr_t)a1;
            uint32_t  futex_val  = (uint32_t)a3;
            if (futex_op == FUTEX_WAIT) {
                /* Read the value at uaddr; if != val return EAGAIN */
                uint32_t cur = 0;
                if (copy_from_user(&cur, futex_addr, sizeof(cur)) != 0) { ret = EFAULT; break; }
                if (cur != futex_val) { ret = -11; break; } /* EAGAIN = -11 */
                /* Single-threaded: nobody can wake us, return 0 (spurious wakeup) */
                ret = 0;
            } else {
                ret = 0; /* WAKE: nothing to wake */
            }
            break;
        }
        case 258: ret = sys_linux_mkdirat((int)a1,(const char*)a2,(int)a3); break; /* mkdirat */
        case 334: ret = 0; break;  /* rseq — stub */
        case 435: ret = 0; break;  /* clone3 — stub */
        case 24:  ret = sys_linux_sched_yield(); break;
        case 203: ret = 0; break; /* sched_setaffinity — accept */
        case 204: ret = sys_linux_sched_getaffinity((int)a1,(unsigned int)a2,(void*)a3); break;
        case 26:  ret = sys_linux_msync(a1, a2, (int)a3); break;
        case 28:  ret = sys_linux_madvise(a1, a2, (int)a3); break;
        case 35:  ret = sys_linux_nanosleep((const void*)a1, (void*)a2); break;
        case 37:  ret = sys_linux_alarm((unsigned int)a1); break;
        case 40:  ret = sys_linux_sendfile((int)a1,(int)a2,(int64_t*)a3,a4); break;
        case 41:  ret = sys_linux_socket((int)a1,(int)a2,(int)a3); break;
        case 42:  ret = sys_linux_connect((int)a1,(const void*)a2,(uint32_t)a3); break;
        case 43:  ret = sys_linux_accept((int)a1,(void*)a2,(uint32_t*)a3); break;
        case 44:  ret = sys_linux_sendto((int)a1,(const void*)a2,a3,(int)a4,(const void*)frame->r8,(uint32_t)frame->r9); break;
        case 45:  ret = sys_linux_recvfrom((int)a1,(void*)a2,a3,(int)a4,(void*)frame->r8,(uint32_t*)frame->r9); break;
        case 46: { /* sendmsg(fd, msghdr*, flags) */
            /* struct msghdr: msg_name(8), msg_namelen(4), pad(4),
               msg_iov(8), msg_iovlen(8), msg_control(8), msg_controllen(8), flags(4) */
            typedef struct { uint64_t name; uint32_t namelen; uint32_t pad;
                             uint64_t iov;  uint64_t iovlen; } msghdr_t;
            typedef struct { uint64_t base; uint64_t len; } iovec_t2;
            msghdr_t mh;
            if (copy_from_user(&mh, (void*)a2, sizeof(mh)) != 0) { ret = EFAULT; break; }
            /* Read first iov entry for the data */
            iovec_t2 iov0;
            if (mh.iovlen == 0 || copy_from_user(&iov0, (void*)mh.iov, sizeof(iov0)) != 0)
                { ret = EFAULT; break; }
            void* name = mh.namelen > 0 ? (void*)mh.name : 0;
            ret = sys_linux_sendto((int)a1, (void*)iov0.base, iov0.len,
                                   (int)a4, name, mh.namelen);
            break;
        }
        case 47: { /* recvmsg(fd, msghdr*, flags) */
            typedef struct { uint64_t name; uint32_t namelen; uint32_t pad;
                             uint64_t iov;  uint64_t iovlen; } msghdr_t;
            typedef struct { uint64_t base; uint64_t len; } iovec_t2;
            msghdr_t mh;
            if (copy_from_user(&mh, (void*)a2, sizeof(mh)) != 0) { ret = EFAULT; break; }
            iovec_t2 iov0;
            if (mh.iovlen == 0 || copy_from_user(&iov0, (void*)mh.iov, sizeof(iov0)) != 0)
                { ret = EFAULT; break; }
            ret = sys_linux_recvfrom((int)a1, (void*)iov0.base, iov0.len,
                                     (int)a4, (void*)mh.name,
                                     mh.namelen ? (uint32_t*)&mh.namelen : 0);
            break;
        }
        case 48:  ret = sys_linux_shutdown((int)a1,(int)a2); break;
        case 49:  ret = sys_linux_bind((int)a1,(const void*)a2,(uint32_t)a3); break;
        case 50:  ret = sys_linux_listen((int)a1,(int)a2); break;
        case 51:  ret = sys_linux_getsockname((int)a1,(void*)a2,(uint32_t*)a3); break;
        case 52:  ret = sys_linux_getpeername((int)a1,(void*)a2,(uint32_t*)a3); break;
        case 54:  ret = sys_linux_setsockopt((int)a1,(int)a2,(int)a3,(const void*)a4,(uint32_t)frame->r8); break;
        case 55:  ret = sys_linux_getsockopt((int)a1,(int)a2,(int)a3,(void*)a4,(uint32_t*)frame->r8); break;
        case 62:  ret = sys_linux_kill((int)a1,(int)a2); break;
        case 74:  ret = sys_linux_fsync((int)a1); break;
        case 75:  ret = sys_linux_fsync((int)a1); break;  /* fdatasync */
        case 76:  ret = sys_linux_truncate((const char*)a1,(int64_t)a2); break;
        case 77:  ret = sys_linux_ftruncate((int)a1,(int64_t)a2); break;
        case 78:  ret = sys_linux_getdents((int)a1,(void*)a2,(unsigned int)a3); break;
        case 81:  ret = sys_linux_fchdir((int)a1); break;
        case 161: ret = sys_linux_chroot((const char*)a1); break;
        case 92:  ret = sys_linux_chown((const char*)a1,(int)a2,(int)a3); break;
        case 93:  ret = sys_linux_fchown((int)a1,(int)a2,(int)a3); break;
        case 94:  ret = sys_linux_lchown((const char*)a1,(int)a2,(int)a3); break;
        case 95:  ret = sys_linux_umask((int)a1); break;
        case 96:  ret = sys_linux_gettimeofday((linux_timeval_t*)a1,(void*)a2); break;
        case 97:  ret = sys_linux_getrlimit((int)a1,(linux_rlimit_t*)a2); break;
        case 99:  ret = sys_linux_sysinfo((linux_sysinfo_t*)a1); break;
        case 109: ret = sys_linux_setpgid((int)a1,(int)a2); break;
        case 110: ret = sys_linux_getppid(); break;
        case 111: ret = sys_linux_getpgrp(); break;
        case 112: ret = sys_linux_setsid(); break;
        case 121: ret = sys_linux_getpgid((int)a1); break;
        case 124: ret = sys_linux_getsid((int)a1); break;
        case 132: ret = 0; break;  /* utime — stub */
        case 137: ret = sys_linux_statfs((const char*)a1,(linux_statfs_t*)a2); break;
        case 138: ret = sys_linux_fstatfs((int)a1,(linux_statfs_t*)a2); break;
        case 157: ret = sys_linux_prctl((int)a1,a2,a3,a4,frame->r8); break;
        case 160: ret = sys_linux_setrlimit((int)a1,(const linux_rlimit_t*)a2); break;
        case 105: ret = sys_linux_setuid((int)a1); break;
        case 106: ret = sys_linux_setgid((int)a1); break;
        case 113: ret = sys_linux_setresuid((int)a1,(int)a2,(int)a3); break;
        case 114: ret = 0; break;  /* getresuid stub */
        case 116: ret = sys_linux_setresgid((int)a1,(int)a2,(int)a3); break;
        case 117: ret = 0; break;  /* getresgid stub */
        case 115: ret = sys_linux_getgroups((int)a1,(int*)a2); break;
        case 200: ret = sys_linux_kill((int)a1,(int)a2); break;  /* tkill */
        case 228: ret = sys_linux_clock_gettime((int)a1,(linux_timespec_t*)a2); break;
        case 229: ret = sys_linux_clock_getres((int)a1,(linux_timespec_t*)a2); break;
        case 230: ret = sys_linux_nanosleep((const void*)a1,(void*)a2); break; /* clock_nanosleep */
        case 232: ret = sys_linux_epoll_create((int)a1); break;
        case 233: ret = sys_linux_epoll_ctl((int)a1,(int)a2,(int)a3,(void*)a4); break;
        case 234: ret = sys_linux_kill((int)a1,(int)a3); break;  /* tgkill */
        case 235: ret = 0; break;  /* utimes stub */
        case 269: ret = sys_linux_faccessat((int)a1,(const char*)a2,(int)a3,(int)a4); break; /* faccessat */
        case 186: { task_t* ct = task_current(); ret = ct ? ct->id : 1; break; } /* gettid */
        case 439: ret = sys_linux_faccessat((int)a1,(const char*)a2,(int)a3,(int)a4); break; /* faccessat2 */
        case 270: {
            /* pselect6(nfds, readfds, writefds, exceptfds, timeout, sigmask)
             * Used by libfetch to wait for non-blocking connect to complete.
             * For connected STREAM sockets, mark them writable immediately. */
            int nfds = (int)a1;
            uint64_t* wfds = (uint64_t*)a3; /* writefds */
            if (wfds && nfds > 0) {
                /* Check which fds in writefds are connected sockets -> mark ready */
                int words = (nfds + 63) / 64;
                for (int w = 0; w < words; w++) {
                    uint64_t mask = 0;
                    uint64_t bits;
                    if (copy_from_user(&bits, &wfds[w], 8) != 0) continue;
                    for (int b = 0; b < 64; b++) {
                        int fd = w * 64 + b;
                        if (fd >= nfds) break;
                        if (!(bits & (1ULL << b))) continue;
                        if (fd_is_sock(fd)) mask |= (1ULL << b);
                    }
                    copy_to_user(&wfds[w], &mask, 8);
                }
            }
            /* Zero readfds and exceptfds */
            if (a2) { uint64_t z = 0; copy_to_user((void*)a2, &z, 8); }
            if (a4) { uint64_t z = 0; copy_to_user((void*)a4, &z, 8); }
            ret = 1; /* 1 fd ready */
            break;
        }
        case 281: ret = sys_linux_epoll_wait((int)a1,(void*)a2,(int)a3,(int)a4); break;
        case 275: ret = sys_linux_splice((int)a1,(int64_t*)a2,(int)a3,(int64_t*)a4,(uint64_t)frame->r8,(int)frame->r9); break;
        case 280: ret = sys_linux_utimensat((int)a1,(const char*)a2,(const void*)a3,(int)a4); break;
        case 285: ret = sys_linux_fallocate((int)a1,(int)a2,(int64_t)a3,(int64_t)a4); break;
        case 290: ret = sys_linux_eventfd((unsigned int)a1,0); break;
        case 293: ret = sys_linux_pipe((int*)a1,(int)a2); break; /* pipe2 */
        case 302: ret = sys_linux_prlimit((int)a1,(int)a2,(const linux_rlimit_t*)a3,(linux_rlimit_t*)a4); break;
        case 316: ret = sys_linux_renameat((int)a1,(const char*)a2,(int)a3,(const char*)a4); break;
        case 318: {
            /* getrandom — write random bytes to buffer */
            uint8_t* _gbuf = (uint8_t*)a1;
            uint64_t _glen = a2;
            if (_gbuf && _glen) {
                static uint64_t _gseed = 0x123456789abcdef0ULL;
                for (uint64_t _gi = 0; _gi < _glen; _gi++) {
                    _gseed = _gseed * 6364136223846793005ULL + 1442695040888963407ULL;
                    _gbuf[_gi] = (uint8_t)(_gseed >> 33);
                }
            }
            ret = (int64_t)a2;
            break;
        }
        case 25: { /* mremap(old_addr, old_size, new_size, flags) */
            uint64_t old_addr = a1, old_size = a2, new_size = a3;
            if (new_size <= old_size) { ret = (int64_t)old_addr; break; }
            int64_t nb = sys_linux_mmap(0, new_size, 3 /*RW*/,
                                        0x22 /*MAP_PRIVATE|ANON*/, -1, 0);
            if (nb < 0) { ret = ENOMEM; break; }
            /* Both ranges are user memory in the current address space */
            memcpy((void*)(uint64_t)nb, (void*)old_addr, old_size);
            ret = nb;   /* old region reclaimed at task exit */
            break;
        }
        default:
            printf("[linux_syscall] unimplemented %llu (a1=0x%llx)\n", num, a1);
            ret = ENOSYS;
            break;
    }

    frame->rax = (uint64_t)ret;

    /* Take delivery of any pending signal before returning to user mode. */
    deliver_signals(frame);
}

/* ------------------------------------------------------------------ */
/* loader_brk — Linux brk() semantics (set absolute break)             */
/* ------------------------------------------------------------------ */
int64_t loader_brk(uint64_t new_brk) {
    /* brk(0) = return current break; brk(n) = set break to n.
     * Linux semantics: on failure return the CURRENT break unchanged.
     * musl checks (brk(p) == p) and falls back to mmap when it fails,
     * so reporting an honest failure here is essential — lying about
     * success makes malloc hand out unmapped memory (=> segfault). */
    extern uint64_t loader_sbrk64(int64_t delta);
    uint64_t cur = loader_sbrk64(0);
    if (new_brk == 0) return (int64_t)cur;
    if (new_brk <= cur) {
        /* Shrink: keep the pages mapped (they'll be reused on regrowth
         * and freed at exit), just move the break down. */
        extern void loader_set_brk(uint64_t brk);
        loader_set_brk(new_brk);
        return (int64_t)new_brk;
    }
    int64_t delta = (int64_t)(new_brk - cur);
    loader_sbrk64(delta);
    uint64_t got = loader_sbrk64(0);
    return (int64_t)got;   /* == new_brk on success, == cur on failure */
}

/* ------------------------------------------------------------------ */
/* MSR setup for syscall/sysret                                         */
/* ------------------------------------------------------------------ */

/* MSR numbers */
#define MSR_EFER        0xC0000080
#define MSR_STAR        0xC0000081
#define MSR_LSTAR       0xC0000082
#define MSR_SFMASK      0xC0000084

static void wrmsr(uint32_t msr, uint64_t val) {
    __asm__ volatile (
        "wrmsr"
        : : "c"(msr), "a"((uint32_t)val), "d"((uint32_t)(val >> 32))
    );
}

static uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

extern void linux_syscall_entry(void);
extern uint64_t g_syscall_kernel_rsp;

/* A dedicated kernel stack for syscall entry (8KB) */
static uint8_t syscall_kstack[8192] __attribute__((aligned(16)));

void linux_syscall_init(void) {
    /* Point the kernel syscall stack to the top of our dedicated stack */
    g_syscall_kernel_rsp = (uint64_t)(syscall_kstack + sizeof(syscall_kstack));

    /* Enable SCE (System Call Extensions) bit in EFER */
    uint64_t efer = rdmsr(MSR_EFER);
    efer |= 1; /* SCE bit */
    wrmsr(MSR_EFER, efer);

    /*
     * STAR MSR layout:
     *   bits 63:48 = SYSRET CS/SS  (user code selector - 16, user data selector - 8)
     *   bits 47:32 = SYSCALL CS/SS (kernel code selector)
     *
     * On SYSCALL: CS = STAR[47:32], SS = STAR[47:32]+8
     * On SYSRET:  CS = STAR[63:48]+16, SS = STAR[63:48]+8
     *
     * Our GDT: kcode=0x08, kdata=0x10, udata=0x18, ucode=0x20
     * For SYSRET to ucode(0x20): STAR[63:48] = 0x20-16 = 0x10... 
     * Actually: SYSRET sets CS=STAR[63:48]+16|3, SS=STAR[63:48]+8|3
     * So we want ucode=0x20: STAR[63:48] = 0x20-16 = 0x10... no.
     * STAR[63:48] should be the user data selector - 8.
     * udata=0x18, so STAR[63:48] = 0x18-8 = 0x10... 
     *
     * Correct formula from Intel manual:
     *   SYSRET 64-bit: CS = STAR[63:48]+16, SS = STAR[63:48]+8
     *   We want CS=0x23 (ucode|3=0x20|3) and SS=0x1B (udata|3=0x18|3)
     *   So STAR[63:48]+16 = 0x20 => STAR[63:48] = 0x10
     *   And STAR[63:48]+8  = 0x18 => STAR[63:48] = 0x10 ✓
     *
     * SYSCALL: CS = STAR[47:32], SS = STAR[47:32]+8
     *   We want CS=0x08 (kcode), SS=0x10 (kdata)
     *   So STAR[47:32] = 0x08 ✓
     */
    uint64_t star = ((uint64_t)0x0010 << 48) | ((uint64_t)0x0008 << 32);
    wrmsr(MSR_STAR, star);

    /* LSTAR = entry point for syscall instruction */
    wrmsr(MSR_LSTAR, (uint64_t)linux_syscall_entry);

    /* SFMASK = flags to clear on syscall entry (clear IF = disable interrupts) */
    wrmsr(MSR_SFMASK, 0x200); /* clear IF */

    extern void print_status(int ok, const char* msg);
    print_status(1, "Linux syscall/sysret path ready");
}