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
#include "task.h"
#include "uaccess.h"
#include "loader.h"
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
#define FD_FLAG_DIR  0x02

typedef struct {
    unsigned char flags;
    char          path[256];   /* resolved absolute path */
    unsigned int  offset;      /* current read/write position */
    unsigned int  size;        /* cached file size */
} fd_entry_t;

/* Stored in task->user_data extended area, or in a global for now */
/* For simplicity, one global fd table (single-process model) */
static fd_entry_t fd_table[FD_MAX];
static int        fd_table_init = 0;

/* Minimal TLS block — just enough for __errno_location to work.
   Layout: [0] = pointer to self (pthread self ptr), [52/0x34] = errno */
static uint64_t minimal_tls[16];  /* 128 bytes, zero-initialized */

static void fdtable_init(void) {
    if (fd_table_init) return;
    for (int i = 0; i < FD_MAX; i++) fd_table[i].flags = 0;
    /* stdin=0, stdout=1, stderr=2 are always open */
    fd_table[0].flags = FD_FLAG_USED;
    fd_table[1].flags = FD_FLAG_USED;
    fd_table[2].flags = FD_FLAG_USED;
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

/* sys_read (0) */
static int64_t sys_linux_read(int fd, char* buf, uint64_t count) {
    if (!fd_valid(fd)) return EBADF;
    if (count == 0) return 0;

    if (fd == 0) {
        /* stdin: read from keyboard */
        if (!user_range_ok(buf, count)) return EFAULT;
        unsigned int i = 0;
        while (i < (unsigned int)count) {
            char c;
            if (keyboard_try_getchar(&c)) {
                if (copy_to_user(buf + i, &c, 1) != 0)
                    return i > 0 ? (int64_t)i : EFAULT;
                i++;
                if (c == '\n') break;
            } else {
                yield();
            }
        }
        return (int64_t)i;
    }

    /* Regular file */
    fd_entry_t* f = &fd_table[fd];
    if (f->flags & FD_FLAG_DIR) return EISDIR;

    unsigned int to_read = (unsigned int)count;
    if (f->offset >= f->size) return 0; /* EOF */
    if (f->offset + to_read > f->size) to_read = f->size - f->offset;

    char* kbuf = (char*)kmalloc(to_read);
    if (!kbuf) return ENOMEM;

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

/* sys_write (1) */
static int64_t sys_linux_write(int fd, const char* buf, uint64_t count) {
    if (!fd_valid(fd)) return EBADF;
    if (count == 0) return 0;

    if (fd == 1 || fd == 2) {
        /* stdout/stderr: print to console */
        char chunk[128];
        uint64_t total = 0;
        while (total < count) {
            unsigned int n = (unsigned int)(count - total);
            if (n > sizeof(chunk)) n = sizeof(chunk);
            if (copy_from_user(chunk, buf + total, n) != 0)
                return total > 0 ? (int64_t)total : EFAULT;
            for (unsigned int i = 0; i < n; i++) con_putchar(chunk[i]);
            total += n;
        }
        return (int64_t)total;
    }

    /* File write — supports offset-based writes */
    fd_entry_t* f = &fd_table[fd];
    char* kbuf = (char*)kmalloc((unsigned int)count);
    if (!kbuf) return ENOMEM;
    if (copy_from_user(kbuf, buf, (unsigned int)count) != 0) {
        kfree(kbuf); return EFAULT;
    }

    /* Read existing content to preserve bytes before offset */
    unsigned int new_size = f->offset + (unsigned int)count;
    if (new_size < f->size) new_size = f->size;
    char* full = (char*)kmalloc(new_size + 1);
    if (!full) { kfree(kbuf); return ENOMEM; }

    if (f->size > 0 && f->offset > 0) {
        int existing = vfs_read_file(f->path, full, f->size);
        if (existing < 0) existing = 0;
        /* Zero any gap */
        for (unsigned int i = (unsigned int)existing; i < f->offset; i++) full[i] = 0;
    }
    /* Copy new data at offset */
    for (unsigned int i = 0; i < (unsigned int)count; i++)
        full[f->offset + i] = kbuf[i];
    kfree(kbuf);

    int r = vfs_write_file(f->path, full, new_size);
    kfree(full);
    if (r < 0) return EIO;
    f->offset += (unsigned int)count;
    if (f->offset > f->size) f->size = f->offset;
    return (int64_t)count;
}

/* sys_open (2) */
static int64_t sys_linux_open(const char* upath, int flags, int mode) {
    (void)mode;
    char path[512];
    if (resolve_user_path(upath, path, sizeof(path)) < 0) return ENOENT;

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

    fd_table[fd].flags  = FD_FLAG_USED | (is_dir ? FD_FLAG_DIR : 0);
    fd_table[fd].offset = 0;
    for (int i = 0; i < 255 && path[i]; i++)
        fd_table[fd].path[i] = path[i];
    fd_table[fd].path[255] = '\0';

    return fd;
}

/* sys_close (3) */
static int64_t sys_linux_close(int fd) {
    if (fd < 3) return 0; /* never close stdin/out/err */
    if (!fd_valid(fd)) return EBADF;
    fd_table[fd].flags = 0;
    return 0;
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
    return fill_stat(path, ustat);
}

static int64_t sys_linux_fstat(int fd, linux_stat_t* ustat) {
    if (!fd_valid(fd)) return EBADF;
    if (fd == 0 || fd == 1 || fd == 2) {
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

        task_t* t = task_current();
        for (unsigned int i = 0; i < pages; i++) {
            uint64_t phys = pmm_alloc();
            if (!phys) return ENOMEM;
            /* Zero the page */
            void* kva = (void*)(phys + 0xffff800000000000ULL);
            memset(kva, 0, 0x1000);
            if (!vmm_map_in(t ? t->pd_phys : 0,
                            base + i * 0x1000, phys,
                            0x07 /* P|W|U */)) {
                pmm_free(phys);
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

static int64_t sys_linux_writev(int fd, const iovec_t* uiov, int iovcnt) {
    if (!fd_valid(fd)) return EBADF;
    int64_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        iovec_t iov;
        if (copy_from_user(&iov, uiov + i, sizeof(iov)) != 0) return EFAULT;
        if (iov.iov_len == 0) continue;
        int64_t r = sys_linux_write(fd, (const char*)iov.iov_base, iov.iov_len);
        if (r < 0) return r;
        total += r;
    }
    return total;
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

/* sys_fork (57) */
static int64_t sys_linux_fork(linux_frame_t* frame) {
    /* Reuse existing loader_fork — needs struct registers format */
    /* For now return ENOSYS until we bridge the frame formats */
    (void)frame;
    return ENOSYS;
}

/* sys_execve (59) */
static int64_t sys_linux_execve(const char* upath, linux_frame_t* frame) {
    char path[256];
    if (copy_str_from_user(path, upath, sizeof(path)) < 0) return EFAULT;
    /* Reuse existing loader_exec — needs struct registers format */
    (void)frame;
    return ENOSYS; /* TODO: bridge frame formats */
}

/* sys_exit (60) and sys_exit_group (231) */
static int64_t sys_linux_exit(int code) {
    /* Mark task as dead and switch to next task — never returns */
    __asm__ volatile ("cli");
    task_exit_code(code);
    /* task_exit_code never returns — it switches to next task directly.
       If somehow we get here, halt. */
    for (;;) __asm__ volatile ("hlt");
    return 0;
}

/* sys_wait4 (61) */
static int64_t sys_linux_wait4(int pid, int* ustatus, int options) {
    (void)pid; (void)options;
    int status = 0;
    int child = task_wait_child(&status);
    if (child < 0) return ECHILD;
    if (ustatus) {
        int wstatus = (status & 0xFF) << 8;
        if (copy_to_user(ustatus, &wstatus, sizeof(int)) != 0) return EFAULT;
    }
    return child;
}

/* sys_uname (63) */
static int64_t sys_linux_uname(linux_utsname_t* ubuf) {
    linux_utsname_t u;
    memset(&u, 0, sizeof(u));
    /* Claim to be Linux so musl/apk accept us */
    memcpy(u.sysname,    "Linux",          6);
    memcpy(u.nodename,   "computer-os",    12);
    memcpy(u.release,    "6.1.0",          6);
    memcpy(u.version,    "#1 Computer OS", 15);
    memcpy(u.machine,    "x86_64",         7);
    memcpy(u.domainname, "(none)",         7);
    if (copy_to_user(ubuf, &u, sizeof(u)) != 0) return EFAULT;
    return 0;
}

/* sys_fcntl (72) */
static int64_t sys_linux_fcntl(int fd, int cmd, uint64_t arg) {
    (void)arg;
    if (!fd_valid(fd)) return EBADF;
    /* F_GETFD=1, F_SETFD=2, F_GETFL=3, F_SETFL=4 */
    if (cmd == 1) return 0;  /* FD_CLOEXEC flag = 0 */
    if (cmd == 2) return 0;
    if (cmd == 3) return 0;  /* O_RDONLY */
    if (cmd == 4) return 0;
    return EINVAL;
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
    /* We don't have symlinks — return EINVAL */
    (void)upath; (void)ubuf; (void)bufsz;
    return EINVAL;
}

/* sys_getuid/geteuid/getgid/getegid (102/107/104/108) */
static int64_t sys_linux_getuid(void)  { return 0; }
static int64_t sys_linux_getgid(void)  { return 0; }

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
    (void)dirfd; /* treat as AT_FDCWD for now */
    return sys_linux_open(upath, flags, mode);
}

/* sys_fstatat/newfstatat (262) */
static int64_t sys_linux_newfstatat(int dirfd, const char* upath,
                                     linux_stat_t* ustat, int flags) {
    (void)dirfd; (void)flags;
    return sys_linux_stat(upath, ustat);
}

/* sys_ioctl (16) — stub for terminal ioctls */
static int64_t sys_linux_ioctl(int fd, uint64_t req, uint64_t arg) {
    (void)fd; (void)arg;
    /* TCGETS/TCSETS — pretend we're a tty */
    if (req == 0x5401 || req == 0x5402) return 0;
    return EINVAL;
}

/* sys_rt_sigaction (13), sys_rt_sigprocmask (14) — stub */
static int64_t sys_linux_rt_sigaction(int sig, void* act, void* oact, uint64_t sz) {
    (void)sig; (void)sz;
    if (oact) {
        /* Zero out old action */
        char zero[32]; memset(zero, 0, sizeof(zero));
        copy_to_user(oact, zero, 32);
    }
    (void)act;
    return 0;
}
static int64_t sys_linux_rt_sigprocmask(int how, void* set, void* oset, uint64_t sz) {
    (void)how; (void)set; (void)sz;
    if (oset) {
        char zero[8]; memset(zero, 0, 8);
        copy_to_user(oset, zero, 8);
    }
    return 0;
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
        case 0:   ret = sys_linux_read   ((int)a1, (char*)a2, a3); break;
        case 1:   ret = sys_linux_write  ((int)a1, (const char*)a2, a3); break;
        case 2:   ret = sys_linux_open   ((const char*)a1, (int)a2, (int)a3); break;
        case 3:   ret = sys_linux_close  ((int)a1); break;
        case 4:   ret = sys_linux_stat   ((const char*)a1, (linux_stat_t*)a2); break;
        case 5:   ret = sys_linux_fstat  ((int)a1, (linux_stat_t*)a2); break;
        case 6:   ret = sys_linux_stat   ((const char*)a1, (linux_stat_t*)a2); break; /* lstat */
        case 8:   ret = sys_linux_lseek  ((int)a1, (int64_t)a2, (int)a3); break;
        case 7: {
            /* poll — musl uses this to get random bytes if AT_RANDOM is bad.
             * Write pseudo-random bytes to the pollfd array (rdi=ptr, rsi=nfds) */
            uint8_t* _buf = (uint8_t*)a1;
            uint64_t _len = a2 * 8;  /* nfds * sizeof(struct pollfd) */
            if (_buf && _len) {
                extern uint64_t g_hhdm_offset;
                /* Just write some bytes — musl only needs entropy, not true random */
                static uint64_t _seed = 0xdeadbeefcafe1234ULL;
                for (uint64_t _i = 0; _i < _len; _i++) {
                    _seed = _seed * 6364136223846793005ULL + 1442695040888963407ULL;
                    _buf[_i] = (uint8_t)(_seed >> 33);
                }
            }
            ret = (int64_t)a2;  /* return nfds = success */
            break;
        }
        case 9:   ret = sys_linux_mmap   (a1, a2, (int)a3, (int)a4, (int)frame->r8, (int64_t)frame->r9); break;
        case 11:  ret = sys_linux_munmap (a1, a2); break;
        case 12:  ret = sys_linux_brk    (a1); break;
        case 13:  ret = sys_linux_rt_sigaction((int)a1, (void*)a2, (void*)a3, a4); break;
        case 14:  ret = sys_linux_rt_sigprocmask((int)a1,(void*)a2,(void*)a3,a4); break;
        case 16:  ret = sys_linux_ioctl  ((int)a1, a2, a3); break;
        case 20:  ret = sys_linux_writev ((int)a1, (const iovec_t*)a2, (int)a3); break;
        case 21:  ret = sys_linux_access ((const char*)a1, (int)a2); break;
        case 39:  ret = sys_linux_getpid (); break;
        case 57:  ret = sys_linux_fork   (frame); break;
        case 59:  ret = sys_linux_execve ((const char*)a1, frame); break;
        case 60:  ret = sys_linux_exit   ((int)a1); break;
        case 61:  ret = sys_linux_wait4  ((int)a1, (int*)a2, (int)a3); break;
        case 63:  ret = sys_linux_uname  ((linux_utsname_t*)a1); break;
        case 72:  ret = sys_linux_fcntl  ((int)a1, (int)a2, a3); break;
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
        /* musl startup syscalls */
        case 218: ret = sys_linux_getpid(); break; /* set_tid_address */
        case 273: ret = 0; break;  /* set_robust_list — stub */
        case 302: ret = 0; break;  /* prlimit64 — stub */
        case 334: ret = 0; break;  /* rseq — stub */
        case 435: ret = 0; break;  /* clone3 — stub */
        case 217: ret = sys_linux_getdents64((int)a1,(void*)a2,(unsigned int)a3); break;
        case 231: ret = sys_linux_exit   ((int)a1); break; /* exit_group */
        case 257: ret = sys_linux_openat ((int)a1,(const char*)a2,(int)a3,(int)a4); break;
        case 262: ret = sys_linux_newfstatat((int)a1,(const char*)a2,(linux_stat_t*)a3,(int)a4); break;
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
        default:
            printf("[linux_syscall] unimplemented %llu (a1=0x%llx)\n", num, a1);
            ret = ENOSYS;
            break;
    }

    frame->rax = (uint64_t)ret;


}

/* ------------------------------------------------------------------ */
/* loader_brk — Linux brk() semantics (set absolute break)             */
/* ------------------------------------------------------------------ */
int64_t loader_brk(uint64_t new_brk) {
    /* brk(0) = return current break; brk(n) = set break to n */
    extern uint64_t loader_sbrk64(int64_t delta);
    uint64_t cur = loader_sbrk64(0);
    if (new_brk == 0) return (int64_t)cur;
    if (new_brk <= cur) return (int64_t)new_brk;
    int64_t delta = (int64_t)(new_brk - cur);
    loader_sbrk64(delta);
    return (int64_t)new_brk;
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