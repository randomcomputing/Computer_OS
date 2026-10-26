/*
 * mkfs_cfs — host tool that formats a CFS disk image and optionally seeds
 * it with a directory tree (like `mke2fs -d`).
 *
 *   mkfs_cfs <image> <blocks> [label] [stage-dir]
 *
 *     image      output image file (created/truncated to blocks * 4096)
 *     blocks     volume size in 4 KiB blocks
 *     label      volume label (default "cfs")
 *     stage-dir  host directory whose contents are copied into the image
 *
 * It links against the shared core (src/cfs.c). To stay a normal hosted
 * program (it needs <dirent.h>, <stdio.h>, ...), it declares the few CFS
 * symbols it uses itself rather than including cfs.h, whose kernel stdint.h
 * would clash with the system headers. The struct layouts match cfs.h and
 * every width is 64-bit, so the ABI is identical.
 */

#define _POSIX_C_SOURCE 200809L   /* pread/pwrite/ftruncate/lstat */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>

/* ---- minimal CFS API (matches include/cfs.h) ---------------------- */
#define CFS_BS 4096u

typedef struct cfs_fs cfs_fs_t;                 /* opaque here */
typedef struct cfs_bdev {
    int   (*read)(void*, uint64_t, uint32_t, void*);
    int   (*write)(void*, uint64_t, uint32_t, const void*);
    void*    ctx;
    uint64_t base;
} cfs_bdev_t;

extern int cfs_format(cfs_bdev_t*, uint64_t total_blocks,
                      uint64_t journal_blocks, const char* label);
extern int cfs_mount_bd(cfs_bdev_t*, cfs_fs_t*);
extern int cfs_mkdir(cfs_fs_t*, const char*);
extern int cfs_write_file(cfs_fs_t*, const char*, const void*, uint32_t);

/* The core asks the environment for the time. */
uint64_t cfs_now_ns(void) { return (uint64_t)time(NULL) * 1000000000ull; }

/* Storage for the opaque mount struct (cfs_fs_t is well under this). */
static _Alignas(16) unsigned char fsmem[512];

/* ---- file-backed block device ------------------------------------- */
static int g_fd = -1;

static int file_read(void* ctx, uint64_t sector, uint32_t count, void* buf) {
    (void)ctx;
    ssize_t n = pread(g_fd, buf, (size_t)count * 512, (off_t)sector * 512);
    return (n == (ssize_t)count * 512) ? 0 : -1;
}
static int file_write(void* ctx, uint64_t sector, uint32_t count, const void* buf) {
    (void)ctx;
    ssize_t n = pwrite(g_fd, buf, (size_t)count * 512, (off_t)sector * 512);
    return (n == (ssize_t)count * 512) ? 0 : -1;
}

/* ---- seed a staging directory into the image ---------------------- */
static int copy_file(cfs_fs_t* fs, const char* hostpath, const char* cfspath) {
    FILE* f = fopen(hostpath, "rb");
    if (!f) { fprintf(stderr, "  skip (open): %s\n", hostpath); return -1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    void* buf = sz > 0 ? malloc((size_t)sz) : malloc(1);
    if (!buf) { fclose(f); return -1; }
    if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f); free(buf); return -1;
    }
    fclose(f);
    int rc = cfs_write_file(fs, cfspath, buf, (uint32_t)sz);
    free(buf);
    if (rc < 0) { fprintf(stderr, "  write failed: %s\n", cfspath); return -1; }
    printf("  + %-28s (%ld bytes)\n", cfspath, sz);
    return 0;
}

static void seed_dir(cfs_fs_t* fs, const char* hostdir, const char* cfsdir) {
    DIR* d = opendir(hostdir);
    if (!d) return;
    struct dirent* de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char hp[4096], cp[4096];
        snprintf(hp, sizeof hp, "%s/%s", hostdir, de->d_name);
        snprintf(cp, sizeof cp, "%s%s%s",
                 cfsdir, (strcmp(cfsdir, "/") ? "/" : ""), de->d_name);
        struct stat st;
        if (lstat(hp, &st) < 0) continue;
        if (S_ISLNK(st.st_mode)) continue;              /* no symlinks yet */
        if (S_ISDIR(st.st_mode)) {
            if (cfs_mkdir(fs, cp) == 0) printf("  d %s\n", cp);
            seed_dir(fs, hp, cp);
        } else if (S_ISREG(st.st_mode)) {
            copy_file(fs, hp, cp);
        }
    }
    closedir(d);
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <image> <blocks> [label] [stage-dir]\n", argv[0]);
        return 2;
    }
    const char* image = argv[1];
    uint64_t blocks = strtoull(argv[2], NULL, 0);
    const char* label = (argc >= 4) ? argv[3] : "cfs";
    const char* stage = (argc >= 5) ? argv[4] : NULL;

    g_fd = open(image, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (g_fd < 0) { perror("open"); return 1; }
    if (ftruncate(g_fd, (off_t)blocks * CFS_BS) < 0) { perror("ftruncate"); return 1; }

    cfs_bdev_t bd = { file_read, file_write, NULL, 0 };
    cfs_fs_t* fs = (cfs_fs_t*)fsmem;

    /* Reserve a journal: ~1/32 of the volume, clamped to a sane range. */
    uint64_t jblocks = blocks / 32;
    if (jblocks < 34)   jblocks = 34;
    if (jblocks > 1024) jblocks = 1024;
    if (cfs_format(&bd, blocks, jblocks, label) != 0) {
        fprintf(stderr, "cfs_format failed\n"); return 1;
    }
    if (cfs_mount_bd(&bd, fs) != 0) {
        fprintf(stderr, "cfs_mount_bd failed\n"); return 1;
    }
    printf("mkfs.cfs: formatted %s — %llu blocks (%llu MiB), label \"%s\"\n",
           image, (unsigned long long)blocks,
           (unsigned long long)(blocks * CFS_BS / (1024 * 1024)), label);

    if (stage) {
        printf("Seeding from %s:\n", stage);
        seed_dir(fs, stage, "/");
    }
    close(g_fd);
    printf("mkfs.cfs: done.\n");
    return 0;
}