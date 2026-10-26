/*
 * Permissions test for CFS (Phase 4 / Month 4).
 *
 * Covers the filesystem side: stat reports mode/uid/gid, chmod changes the
 * permission bits while preserving the file type, chown changes ownership
 * (with -1 meaning "leave unchanged"), and fsck stays clean through it.
 * Also unit-tests the POSIX permission-check logic that the VFS uses to
 * enforce access, including the root (uid 0) bypass.
 *
 * Build & run:  make -C tests/cfs
 */

#include "cfs.h"

extern int   printf(const char* fmt, ...);
extern void* memset(void* d, int c, unsigned long n);

uint64_t cfs_now_ns(void) { static uint64_t t = 1000000000ull; return (t += 1000000ull); }

#define NB 2048u
static unsigned char disk[NB * CFS_BLOCK_SIZE];
static int md_read(void* c, uint64_t s, uint32_t n, void* b) {
    (void)c; __builtin_memcpy(b, disk + s * 512u, (unsigned long)n * 512u); return 0;
}
static int md_write(void* c, uint64_t s, uint32_t n, const void* b) {
    (void)c; __builtin_memcpy(disk + s * 512u, b, (unsigned long)n * 512u); return 0;
}
static cfs_bdev_t bd = { md_read, md_write, 0, 0 };

static int failures = 0;
static void check(int cond, const char* what) {
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what); if (!cond) failures++;
}

/* The exact permission-check logic the VFS enforces (mirrored here so the
 * truth table is tested directly). want bits: 4=read, 2=write, 1=exec. */
static int perm_ok(uint16_t mode, uint32_t fuid, uint32_t fgid,
                   uint32_t uid, uint32_t gid, int want) {
    if (uid == 0) return 1;                      /* root bypass */
    unsigned p;
    if (uid == fuid)       p = (mode >> 6) & 7;  /* owner */
    else if (gid == fgid)  p = (mode >> 3) & 7;  /* group */
    else                   p = mode & 7;         /* other */
    return ((int)p & want) == want;
}

int main(void) {
    cfs_fs_t fs;
    memset(disk, 0, sizeof disk);
    check(cfs_format(&bd, NB, 64, "perms") == 0, "format");
    cfs_mount_bd(&bd, &fs);

    printf("stat / chmod / chown\n");
    cfs_write_file(&fs, "/f", "x", 1);
    cfs_stat_t st;
    check(cfs_stat(&fs, "/f", &st) == 0, "stat a new file");
    check((st.mode & 0x0FFF) == 0644 && st.uid == 0 && st.gid == 0,
          "defaults: mode 0644, uid/gid 0");
    check(CFS_ISREG(st.mode), "type is regular file");

    check(cfs_chmod(&fs, "/f", 0600) == 0, "chmod 0600");
    cfs_stat(&fs, "/f", &st);
    check((st.mode & 0x0FFF) == 0600 && CFS_ISREG(st.mode),
          "mode is 0600 and still a regular file");

    check(cfs_chown(&fs, "/f", 1000, 1000) == 0, "chown 1000:1000");
    cfs_stat(&fs, "/f", &st);
    check(st.uid == 1000 && st.gid == 1000, "uid/gid are 1000");

    check(cfs_chown(&fs, "/f", 1001, (uint32_t)-1) == 0, "chown 1001 (gid unchanged)");
    cfs_stat(&fs, "/f", &st);
    check(st.uid == 1001 && st.gid == 1000, "uid 1001, gid still 1000");

    cfs_mkdir(&fs, "/d");
    cfs_stat(&fs, "/d", &st);
    check(CFS_ISDIR(st.mode) && (st.mode & 0x0FFF) == 0755, "dir is 0755");

    cfs_fsck_result_t fr;
    check(cfs_fsck(&fs, 0, &fr) == 0, "fsck clean after chmod/chown");

    printf("Permission-check truth table\n");
    /* file owned by 1000:1000, mode 0640 (rw- r-- ---) */
    check(perm_ok(0640, 1000, 1000, 0, 0, 7) == 1,        "root may do anything");
    check(perm_ok(0640, 1000, 1000, 1000, 1000, 4) == 1,  "owner may read");
    check(perm_ok(0640, 1000, 1000, 1000, 1000, 2) == 1,  "owner may write");
    check(perm_ok(0640, 1000, 1000, 1000, 1000, 1) == 0,  "owner may not exec");
    check(perm_ok(0640, 1000, 1000, 2000, 1000, 4) == 1,  "group may read");
    check(perm_ok(0640, 1000, 1000, 2000, 1000, 2) == 0,  "group may not write");
    check(perm_ok(0640, 1000, 1000, 2000, 2000, 4) == 0,  "other may not read");
    check(perm_ok(0600, 1000, 1000, 2000, 2000, 4) == 0,  "0600 denies other read");
    check(perm_ok(0755, 1000, 1000, 2000, 2000, 1) == 1,  "0755 allows other exec");

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PERMISSION TESTS PASSED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}