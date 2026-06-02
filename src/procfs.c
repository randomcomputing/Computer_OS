/*
 * procfs.c — /proc virtual filesystem for Computer OS.
 *
 * Provides the minimum /proc entries that musl and apk expect:
 *
 *   /proc/self/exe        → symlink to current executable path
 *   /proc/self/maps       → memory map (stub)
 *   /proc/self/status     → process status
 *   /proc/self/cmdline    → process command line
 *   /proc/self/fd/        → open file descriptors (stub)
 *   /proc/mounts          → mounted filesystems
 *   /proc/version         → kernel version string
 *   /proc/cpuinfo         → CPU info
 *   /proc/meminfo         → memory info
 *   /proc/uptime          → uptime in seconds
 */

#include "procfs.h"
#include "vfs.h"
#include "kheap.h"
#include "string.h"
#include "printf.h"
#include "task.h"
#include "pmm.h"
#include "pit.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

static int streq(const char* a, const char* b) {
    return strcmp(a, b) == 0;
}

static int startswith(const char* s, const char* prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

/* Write a decimal number into buf, return length */
static int u64_to_str(char* buf, unsigned long long v) {
    if (v == 0) { buf[0] = '0'; buf[1] = '\0'; return 1; }
    char tmp[32]; int tl = 0;
    while (v > 0) { tmp[tl++] = '0' + (int)(v % 10); v /= 10; }
    for (int i = 0; i < tl; i++) buf[i] = tmp[tl - 1 - i];
    buf[tl] = '\0';
    return tl;
}

/* ------------------------------------------------------------------ */
/* File content generators                                              */
/* ------------------------------------------------------------------ */

static int gen_version(char* buf, unsigned int max) {
    const char* ver =
        "Linux version 6.1.0 (Computer OS) "
        "(gcc version 13.0) #1 SMP\n";
    unsigned int len = (unsigned int)strlen(ver);
    if (len >= max) len = max - 1;
    memcpy(buf, ver, len);
    buf[len] = '\0';
    return (int)len;
}

static int gen_uptime(char* buf, unsigned int max) {
    unsigned int ms = pit_millis();
    unsigned int secs = ms / 1000;
    char tmp[64];
    int n = 0;
    /* uptime idle_time */
    char sv[16]; u64_to_str(sv, secs);
    for (int i = 0; sv[i]; i++) tmp[n++] = sv[i];
    /* .xx fraction */
    tmp[n++] = '.';
    tmp[n++] = '0' + (int)((ms % 1000) / 100);
    tmp[n++] = '0' + (int)((ms % 100) / 10);
    tmp[n++] = ' ';
    /* idle = same as uptime for single-core stub */
    for (int i = 0; sv[i]; i++) tmp[n++] = sv[i];
    tmp[n++] = '.';
    tmp[n++] = '0' + (int)((ms % 1000) / 100);
    tmp[n++] = '0' + (int)((ms % 100) / 10);
    tmp[n++] = '\n';
    tmp[n] = '\0';
    if ((unsigned int)n >= max) n = (int)max - 1;
    memcpy(buf, tmp, (unsigned int)n + 1);
    return n;
}

static int gen_cpuinfo(char* buf, unsigned int max) {
    /* CPUID brand string */
    char brand[49];
    unsigned int* bp = (unsigned int*)brand;
    for (int leaf = 0; leaf < 3; leaf++) {
        unsigned int a, b, c, d;
        __asm__ volatile ("cpuid"
            : "=a"(a),"=b"(b),"=c"(c),"=d"(d)
            : "a"(0x80000002 + (unsigned int)leaf));
        bp[leaf*4+0]=a; bp[leaf*4+1]=b; bp[leaf*4+2]=c; bp[leaf*4+3]=d;
    }
    brand[48] = '\0';
    const char* bs = brand;
    while (*bs == ' ') bs++;

    char tmp[512];
    int n = 0;
    #define APPEND(s) do { \
        const char* _s = (s); \
        while (*_s && n < 511) tmp[n++] = *_s++; \
    } while(0)

    APPEND("processor\t: 0\n");
    APPEND("vendor_id\t: GenuineIntel\n");
    APPEND("model name\t: "); APPEND(bs); APPEND("\n");
    APPEND("cpu MHz\t\t: 1000.000\n");
    APPEND("flags\t\t: fpu vme de pse tsc msr pae mce cx8 apic\n");
    APPEND("\n");
    #undef APPEND

    tmp[n] = '\0';
    if ((unsigned int)n >= max) n = (int)max - 1;
    memcpy(buf, tmp, (unsigned int)n + 1);
    return n;
}

static int gen_meminfo(char* buf, unsigned int max) {
    unsigned long long total_kb = (unsigned long long)pmm_total_pages() * 4;
    unsigned long long free_kb  = (unsigned long long)pmm_free_pages()  * 4;

    char tmp[512]; int n = 0;
    char sv[32];

    #define LINE(name, val_kb) do { \
        const char* _n = name; \
        while (*_n) tmp[n++] = *_n++; \
        u64_to_str(sv, val_kb); \
        for (int _i = 0; sv[_i]; _i++) tmp[n++] = sv[_i]; \
        tmp[n++] = ' '; tmp[n++] = 'k'; tmp[n++] = 'B'; \
        tmp[n++] = '\n'; \
    } while(0)

    LINE("MemTotal:       ", total_kb);
    LINE("MemFree:        ", free_kb);
    LINE("MemAvailable:   ", free_kb);
    LINE("Buffers:        ", 0ULL);
    LINE("Cached:         ", 0ULL);
    LINE("SwapTotal:      ", 0ULL);
    LINE("SwapFree:       ", 0ULL);
    #undef LINE

    tmp[n] = '\0';
    if ((unsigned int)n >= max) n = (int)max - 1;
    memcpy(buf, tmp, (unsigned int)n + 1);
    return n;
}

static int gen_mounts(char* buf, unsigned int max) {
    const char* content =
        "rootfs / rootfs rw 0 0\n"
        "fat32 / fat32 rw 0 0\n"
        "tmpfs /tmp tmpfs rw 0 0\n"
        "proc /proc proc ro 0 0\n"
        "devfs /dev devfs ro 0 0\n";
    unsigned int len = (unsigned int)strlen(content);
    if (len >= max) len = max - 1;
    memcpy(buf, content, len);
    buf[len] = '\0';
    return (int)len;
}

static int gen_self_status(char* buf, unsigned int max) {
    task_t* t = task_current();
    int pid = t ? t->id : 1;
    char pidstr[16]; u64_to_str(pidstr, (unsigned long long)pid);

    char tmp[512]; int n = 0;
    #define APPEND(s) do { \
        const char* _s = (s); \
        while (*_s && n < 511) tmp[n++] = *_s++; \
    } while(0)

    APPEND("Name:\t"); APPEND(t ? t->name : "kernel"); APPEND("\n");
    APPEND("State:\tR (running)\n");
    APPEND("Pid:\t"); APPEND(pidstr); APPEND("\n");
    APPEND("PPid:\t1\n");
    APPEND("Uid:\t0\t0\t0\t0\n");
    APPEND("Gid:\t0\t0\t0\t0\n");
    APPEND("VmRSS:\t4096 kB\n");
    APPEND("VmSize:\t8192 kB\n");
    APPEND("Threads:\t1\n");
    #undef APPEND

    tmp[n] = '\0';
    if ((unsigned int)n >= max) n = (int)max - 1;
    memcpy(buf, tmp, (unsigned int)n + 1);
    return n;
}

static int gen_self_cmdline(char* buf, unsigned int max) {
    const char* cmd = "computer_os\0";
    unsigned int len = 12;
    if (len >= max) len = max - 1;
    memcpy(buf, cmd, len);
    return (int)len;
}

static int gen_self_exe(char* buf, unsigned int max) {
    /* Return the path of the current executable */
    task_t* t = task_current();
    const char* path = "/";
    if (t && t->name) path = t->name;
    unsigned int len = (unsigned int)strlen(path);
    if (len >= max) len = max - 1;
    memcpy(buf, path, len);
    buf[len] = '\0';
    return (int)len;
}

static int gen_self_maps(char* buf, unsigned int max) {
    /* Stub — return a minimal maps entry */
    const char* maps =
        "00400000-00401000 r-xp 00000000 00:00 0\n"
        "7fff00000000-7fff00001000 rwxp 00000000 00:00 0 [stack]\n";
    unsigned int len = (unsigned int)strlen(maps);
    if (len >= max) len = max - 1;
    memcpy(buf, maps, len);
    buf[len] = '\0';
    return (int)len;
}

/* ------------------------------------------------------------------ */
/* VFS ops                                                              */
/* ------------------------------------------------------------------ */

static int procfs_is_dir(const char* path) {
    if (streq(path, "/"))           return 1;
    if (streq(path, "/self"))       return 1;
    if (streq(path, "/self/fd"))    return 1;
    return 0;
}

static int procfs_list(const char* path, vfs_dirent_t* out, int max) {
    int n = 0;

    #define ENTRY(nm, sz, isdir) do { \
        if (n < max) { \
            int _i = 0; \
            for (; (nm)[_i] && _i < 255; _i++) out[n].name[_i] = (nm)[_i]; \
            out[n].name[_i] = '\0'; \
            out[n].size = (sz); \
            out[n].attr = (isdir) ? 0x10 : 0; \
            out[n].first_cluster = 0; \
            n++; \
        } \
    } while(0)

    if (streq(path, "/")) {
        ENTRY("self",    0, 1);
        ENTRY("version", 128, 0);
        ENTRY("uptime",  32, 0);
        ENTRY("cpuinfo", 512, 0);
        ENTRY("meminfo", 256, 0);
        ENTRY("mounts",  256, 0);
    } else if (streq(path, "/self")) {
        ENTRY("exe",     64, 0);
        ENTRY("maps",    256, 0);
        ENTRY("status",  256, 0);
        ENTRY("cmdline", 64, 0);
        ENTRY("fd",      0, 1);
    } else if (streq(path, "/self/fd")) {
        ENTRY("0", 0, 0);
        ENTRY("1", 0, 0);
        ENTRY("2", 0, 0);
    }
    #undef ENTRY

    return n;
}

static int procfs_read_file(const char* path, void* buf, unsigned int max) {
    char* out = (char*)buf;

    if (streq(path, "/version"))      return gen_version(out, max);
    if (streq(path, "/uptime"))       return gen_uptime(out, max);
    if (streq(path, "/cpuinfo"))      return gen_cpuinfo(out, max);
    if (streq(path, "/meminfo"))      return gen_meminfo(out, max);
    if (streq(path, "/mounts"))       return gen_mounts(out, max);
    if (streq(path, "/self/status"))  return gen_self_status(out, max);
    if (streq(path, "/self/cmdline")) return gen_self_cmdline(out, max);
    if (streq(path, "/self/exe"))     return gen_self_exe(out, max);
    if (streq(path, "/self/maps"))    return gen_self_maps(out, max);

    /* /proc/self/fd/N */
    if (startswith(path, "/self/fd/")) {
        /* Return a stub path for any fd */
        const char* p = "/dev/tty";
        unsigned int len = (unsigned int)strlen(p);
        if (len >= max) len = max - 1;
        memcpy(out, p, len);
        out[len] = '\0';
        return (int)len;
    }

    return -1;
}

/* Everything else is read-only */
static int procfs_write_file(const char* path, const void* data, unsigned int size) {
    (void)path; (void)data; (void)size;
    return -1;
}
static int procfs_delete_file(const char* path) { (void)path; return -1; }
static int procfs_mkdir(const char* path)       { (void)path; return -1; }
static int procfs_rmdir(const char* path)       { (void)path; return -1; }
static int procfs_cp(const char* s, const char* d) { (void)s;(void)d; return -1; }
static int procfs_mv(const char* s, const char* d) { (void)s;(void)d; return -1; }

static const vfs_ops_t procfs_ops = {
    .list        = procfs_list,
    .is_dir      = procfs_is_dir,
    .read_file   = procfs_read_file,
    .write_file  = procfs_write_file,
    .delete_file = procfs_delete_file,
    .mkdir       = procfs_mkdir,
    .rmdir       = procfs_rmdir,
    .cp          = procfs_cp,
    .mv          = procfs_mv,
};

const vfs_ops_t* procfs_vfs_ops(void) { return &procfs_ops; }

void procfs_init(void) {
    /* Nothing to initialize — all content is generated on the fly */
}