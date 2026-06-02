/*
 * devfs.c — /dev virtual filesystem for Computer OS.
 *
 * Provides the minimum /dev entries that musl and apk expect:
 *
 *   /dev/null     → discards all writes, reads return EOF
 *   /dev/zero     → reads return infinite zeroes
 *   /dev/random   → reads return pseudo-random bytes
 *   /dev/urandom  → same as /dev/random (no blocking)
 *   /dev/tty      → current terminal (stdin/stdout)
 *   /dev/stdin    → alias for fd 0
 *   /dev/stdout   → alias for fd 1
 *   /dev/stderr   → alias for fd 2
 *   /dev/full     → writes return ENOSPC, reads return zeroes
 */

#include "devfs.h"
#include "vfs.h"
#include "string.h"
#include "pit.h"

/* ------------------------------------------------------------------ */
/* Simple pseudo-random for /dev/random and /dev/urandom               */
/* ------------------------------------------------------------------ */
static unsigned int prng_state = 0xdeadbeef;

static unsigned char prng_byte(void) {
    /* xorshift32 */
    prng_state ^= prng_state << 13;
    prng_state ^= prng_state >> 17;
    prng_state ^= prng_state << 5;
    return (unsigned char)(prng_state & 0xFF);
}

/* ------------------------------------------------------------------ */
/* VFS ops                                                              */
/* ------------------------------------------------------------------ */

static int devfs_is_dir(const char* path) {
    return (path[0] == '/' && path[1] == '\0') ? 1 : 0;
}

static int devfs_list(const char* path, vfs_dirent_t* out, int max) {
    if (path[0] != '/' || path[1] != '\0') return -1;

    static const struct { const char* name; } devs[] = {
        {"null"}, {"zero"}, {"random"}, {"urandom"},
        {"tty"}, {"stdin"}, {"stdout"}, {"stderr"}, {"full"},
    };
    int n = 0;
    for (unsigned int i = 0;
         i < sizeof(devs)/sizeof(devs[0]) && n < max; i++) {
        int j = 0;
        for (; devs[i].name[j] && j < 255; j++)
            out[n].name[j] = devs[i].name[j];
        out[n].name[j]      = '\0';
        out[n].size         = 0;
        out[n].attr         = 0;
        out[n].first_cluster = 0;
        n++;
    }
    return n;
}

static int devfs_read_file(const char* path, void* buf, unsigned int max) {
    if (max == 0) return 0;

    /* /dev/null → EOF */
    if (strcmp(path, "/null") == 0) return 0;

    /* /dev/full → zeroes (writes return ENOSPC but reads work) */
    /* /dev/zero → zeroes */
    if (strcmp(path, "/zero") == 0 || strcmp(path, "/full") == 0) {
        memset(buf, 0, max);
        return (int)max;
    }

    /* /dev/random, /dev/urandom → pseudo-random bytes */
    if (strcmp(path, "/random")  == 0 ||
        strcmp(path, "/urandom") == 0) {
        /* Seed with uptime for some variance */
        prng_state ^= pit_millis();
        unsigned char* p = (unsigned char*)buf;
        for (unsigned int i = 0; i < max; i++) p[i] = prng_byte();
        return (int)max;
    }

    /* /dev/tty, /dev/stdin, /dev/stdout, /dev/stderr → stub */
    if (strcmp(path, "/tty")    == 0 ||
        strcmp(path, "/stdin")  == 0 ||
        strcmp(path, "/stdout") == 0 ||
        strcmp(path, "/stderr") == 0) {
        /* Return 0 bytes — caller should use fd 0/1/2 directly */
        return 0;
    }

    return -1;
}

static int devfs_write_file(const char* path, const void* data, unsigned int size) {
    /* /dev/null → silently discard */
    if (strcmp(path, "/null")   == 0) return (int)size;
    /* /dev/tty, stdout, stderr → discard (real writes go via syscalls) */
    if (strcmp(path, "/tty")    == 0) return (int)size;
    if (strcmp(path, "/stdout") == 0) return (int)size;
    if (strcmp(path, "/stderr") == 0) return (int)size;
    /* /dev/full → ENOSPC */
    if (strcmp(path, "/full")   == 0) return -28; /* -ENOSPC */
    (void)data;
    return -1;
}

static int devfs_delete_file(const char* p) { (void)p; return -1; }
static int devfs_mkdir(const char* p)       { (void)p; return -1; }
static int devfs_rmdir(const char* p)       { (void)p; return -1; }
static int devfs_cp(const char* s, const char* d) { (void)s;(void)d; return -1; }
static int devfs_mv(const char* s, const char* d) { (void)s;(void)d; return -1; }

static const vfs_ops_t devfs_ops = {
    .list        = devfs_list,
    .is_dir      = devfs_is_dir,
    .read_file   = devfs_read_file,
    .write_file  = devfs_write_file,
    .delete_file = devfs_delete_file,
    .mkdir       = devfs_mkdir,
    .rmdir       = devfs_rmdir,
    .cp          = devfs_cp,
    .mv          = devfs_mv,
};

const vfs_ops_t* devfs_vfs_ops(void) { return &devfs_ops; }

void devfs_init(void) {
    /* Seed PRNG with a fixed value — will get randomized on first read */
    prng_state = 0x13571357;
}