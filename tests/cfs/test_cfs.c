/*
 * Host test harness for src/cfs.c — Phase 1 (core read/write filesystem).
 *
 * Runs the real CFS core against an in-memory disk behind the block-device
 * vtable, so format + mount + every file/directory operation can be
 * exercised without QEMU. Checks: format/mount, nested directory creation
 * and path resolution, long (255-char) filenames, single- and multi-block
 * file round-trips, positional reads, overwrite-as-truncate, directories
 * that grow past one block, rmdir (empty vs non-empty), unlink, and that
 * freed blocks/inodes return to the superblock counts.
 *
 * Build & run:  make -C tests/cfs
 */

#include "cfs.h"

/* libc, declared directly (no system header, to avoid clashing with the
 * kernel stdint.h that cfs.h pulls in). */
extern int   printf(const char* fmt, ...);
extern void* memcpy(void* d, const void* s, unsigned long n);
extern void* memset(void* d, int c, unsigned long n);
extern int   memcmp(const void* a, const void* b, unsigned long n);
extern unsigned long strlen(const char* s);

/* The core asks the environment for the time; give it a monotonic stub. */
static uint64_t clock_ns = 1000000000ull;
uint64_t cfs_now_ns(void) { return (clock_ns += 1000000ull); }

/* ------------------------------------------------------------------ */
/* In-memory disk behind the block-device vtable                       */
/* ------------------------------------------------------------------ */

typedef struct { unsigned char* data; uint64_t sectors; } memdisk_t;

static int md_read(void* ctx, uint64_t sector, uint32_t count, void* buf) {
    memdisk_t* d = (memdisk_t*)ctx;
    if (sector + count > d->sectors) return -1;
    memcpy(buf, d->data + sector * 512u, (unsigned long)count * 512u);
    return 0;
}
static int md_write(void* ctx, uint64_t sector, uint32_t count, const void* buf) {
    memdisk_t* d = (memdisk_t*)ctx;
    if (sector + count > d->sectors) return -1;
    memcpy(d->data + sector * 512u, buf, (unsigned long)count * 512u);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Test scaffolding                                                    */
/* ------------------------------------------------------------------ */

static int failures = 0;
static void check(int cond, const char* what) {
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) failures++;
}

#define DISK_BLOCKS 4096u                 /* 16 MiB */
static unsigned char disk[DISK_BLOCKS * CFS_BLOCK_SIZE];

int main(void) {
    memdisk_t md = { disk, (uint64_t)DISK_BLOCKS * CFS_SPB };
    cfs_bdev_t bd = { md_read, md_write, &md, 0 };
    cfs_fs_t fs;

    printf("CFS core: format + mount\n");
    check(cfs_format(&bd, DISK_BLOCKS, 64, "testfs") == 0, "format a 16 MiB volume");
    check(cfs_mount_bd(&bd, &fs) == 0,                      "mount the volume");
    uint64_t free_at_start = fs.free_blocks;

    vfs_dirent_t ents[300];
    check(cfs_list(&fs, "/", ents, 300) == 0, "fresh root is empty");
    check(cfs_is_dir(&fs, "/") == 1,          "root is a directory");

    printf("Directories + path resolution\n");
    check(cfs_mkdir(&fs, "/home") == 0,              "mkdir /home");
    check(cfs_mkdir(&fs, "/home/user") == 0,         "mkdir /home/user");
    check(cfs_mkdir(&fs, "/home/user") == -1,        "mkdir existing path fails");
    check(cfs_mkdir(&fs, "/nope/deep") == -1,        "mkdir with missing parent fails");
    check(cfs_is_dir(&fs, "/home/user") == 1,        "/home/user is a directory");
    check(cfs_list(&fs, "/", ents, 300) == 1,        "root now has one entry");

    printf("Files: small round-trip\n");
    const char* msg = "Hello, CFS! Extent-based and journaled-to-be.";
    check(cfs_write_file(&fs, "/home/user/notes.txt", msg,
                         (uint32_t)strlen(msg)) == (int)strlen(msg),
          "write /home/user/notes.txt");
    char rb[256];
    int n = cfs_read_file(&fs, "/home/user/notes.txt", rb, sizeof rb);
    check(n == (int)strlen(msg) && memcmp(rb, msg, (unsigned long)n) == 0,
          "read it back byte-for-byte");
    check(cfs_is_dir(&fs, "/home/user/notes.txt") == 0, "a file is not a directory");

    printf("Files: multi-block + positional read\n");
    static unsigned char big[100000], got[100000];
    for (int i = 0; i < (int)sizeof big; i++) big[i] = (unsigned char)(i * 31 + 7);
    check(cfs_write_file(&fs, "/big.bin", big, sizeof big) == (int)sizeof big,
          "write a 100000-byte file (spans many blocks)");
    check(cfs_read_file(&fs, "/big.bin", got, sizeof got) == (int)sizeof big
          && memcmp(big, got, sizeof big) == 0, "read the whole file back");
    int w = cfs_read_at(&fs, "/big.bin", got, 40000, 5000);
    check(w == 5000 && memcmp(got, big + 40000, 5000) == 0,
          "positional read of a 5000-byte window at offset 40000");

    printf("Files: overwrite truncates then rewrites\n");
    const char* small = "tiny";
    check(cfs_write_file(&fs, "/big.bin", small, 4) == 4, "overwrite big.bin with 4 bytes");
    check(cfs_read_file(&fs, "/big.bin", rb, sizeof rb) == 4
          && memcmp(rb, small, 4) == 0, "read back the new short content");

    printf("Long filenames (255 chars)\n");
    char lfn[300]; lfn[0] = '/';
    for (int i = 1; i <= 255; i++) lfn[i] = (char)('a' + (i % 26));
    lfn[256] = '\0';
    check(cfs_write_file(&fs, lfn, "x", 1) == 1, "write a 255-char filename");
    check(cfs_read_file(&fs, lfn, rb, sizeof rb) == 1 && rb[0] == 'x',
          "read it back by its long name");

    printf("Directory growth: 200 entries in one directory\n");
    check(cfs_mkdir(&fs, "/many") == 0, "mkdir /many");
    int made = 0;
    for (int i = 0; i < 200; i++) {
        char p[64]; int k = 0;
        const char* pre = "/many/file";
        for (const char* s = pre; *s; s++) p[k++] = *s;
        /* append a 3-digit number */
        p[k++] = (char)('0' + (i / 100) % 10);
        p[k++] = (char)('0' + (i / 10) % 10);
        p[k++] = (char)('0' + i % 10);
        p[k] = '\0';
        if (cfs_write_file(&fs, p, "hi", 2) == 2) made++;
    }
    check(made == 200, "created 200 files");
    check(cfs_list(&fs, "/many", ents, 300) == 200, "all 200 listed (dir spans blocks)");
    check(cfs_read_file(&fs, "/many/file137", rb, sizeof rb) == 2
          && rb[0] == 'h' && rb[1] == 'i', "look one up by name");

    printf("Native symlinks\n");
    {
        int ml = (int)strlen(VFS_SYMLINK_MAGIC);
        const char* tgt = "/home/user/notes.txt";
        int tl = (int)strlen(tgt);
        char lb[320]; memcpy(lb, VFS_SYMLINK_MAGIC, ml); memcpy(lb + ml, tgt, tl);
        check(cfs_write_file(&fs, "/mylink", lb, ml + tl) == ml + tl, "create a symlink");
        char rr[320];
        int r = cfs_read_file(&fs, "/mylink", rr, sizeof rr);
        check(r == ml + tl && memcmp(rr, VFS_SYMLINK_MAGIC, ml) == 0
              && memcmp(rr + ml, tgt, tl) == 0, "reads back as magic + target");
        check(cfs_is_dir(&fs, "/mylink") == 0, "symlink is not a directory");

        char big[420]; for (int i = 0; i < 380; i++) big[i] = (char)('a' + i % 26);
        char lb2[440]; memcpy(lb2, VFS_SYMLINK_MAGIC, ml); memcpy(lb2 + ml, big, 380);
        check(cfs_write_file(&fs, "/longlink", lb2, ml + 380) == ml + 380,
              "create a long (slow) symlink");
        char rr2[440];
        check(cfs_read_file(&fs, "/longlink", rr2, sizeof rr2) == ml + 380
              && memcmp(rr2 + ml, big, 380) == 0, "long symlink reads back");
    }

    printf("Hard links\n");
    {
        check(cfs_write_file(&fs, "/f1", "shared", 6) == 6, "write /f1");
        check(cfs_link(&fs, "/f1", "/f2") == 0, "link /f2 -> /f1");
        char rb[16];
        check(cfs_read_file(&fs, "/f2", rb, sizeof rb) == 6
              && memcmp(rb, "shared", 6) == 0, "/f2 reads the same data");
        check(cfs_link(&fs, "/f1", "/f2") == -1, "linking over an existing name fails");
        check(cfs_link(&fs, "/home", "/dlink") == -1, "hard link to a directory fails");
        check(cfs_unlink(&fs, "/f1") == 0, "unlink /f1 (one of two names)");
        check(cfs_read_file(&fs, "/f2", rb, sizeof rb) == 6, "/f2 still readable");
        cfs_fsck_result_t fr;
        check(cfs_fsck(&fs, 0, &fr) == 0, "fsck clean after link churn");
        check(cfs_unlink(&fs, "/f2") == 0, "unlink /f2 (last link)");
        check(cfs_read_file(&fs, "/f2", rb, sizeof rb) < 0, "/f2 now gone");
    }

    printf("Removal: rmdir + unlink + free-space accounting\n");
    check(cfs_rmdir(&fs, "/home/user") == -1, "rmdir a non-empty dir fails");
    check(cfs_unlink(&fs, "/home/user/notes.txt") == 0, "unlink the file inside it");
    check(cfs_rmdir(&fs, "/home/user") == 0, "rmdir the now-empty dir");
    check(cfs_rmdir(&fs, "/home") == 0,      "rmdir /home");
    check(cfs_unlink(&fs, "/home") == -1,    "unlink refuses a directory");

    uint64_t before = fs.free_blocks;
    static unsigned char blob[60000];
    memset(blob, 0x5a, sizeof blob);
    check(cfs_write_file(&fs, "/scratch.bin", blob, sizeof blob) == (int)sizeof blob,
          "write a 60000-byte file");
    uint64_t during = fs.free_blocks;
    check(during < before, "free blocks dropped while the file existed");
    check(cfs_unlink(&fs, "/scratch.bin") == 0, "unlink it");
    check(fs.free_blocks == before, "its data blocks returned to the free pool");

    printf("Remount: superblock survives\n");
    cfs_fs_t fs2;
    check(cfs_mount_bd(&bd, &fs2) == 0, "re-mount the same image");
    check(cfs_read_file(&fs2, "/many/file042", rb, sizeof rb) == 2,
          "data written earlier is still readable after remount");
    (void)free_at_start;

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL TESTS PASSED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}