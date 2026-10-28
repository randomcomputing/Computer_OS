/*
 * fsck test for CFS (Phase 3 / Month 3).
 *
 * First proves that a freshly built, exercised volume is already clean
 * (which cross-checks that Months 1-2 keep the on-disk bitmaps, link counts
 * and free counts consistent). Then injects specific corruptions straight
 * into the disk image and checks that cfs_fsck detects each one and that
 * repair restores a clean volume.
 *
 * Build & run:  make -C tests/cfs
 */

#include "cfs.h"

extern int   printf(const char* fmt, ...);
extern void* memcpy(void* d, const void* s, unsigned long n);
extern void* memset(void* d, int c, unsigned long n);
extern int   memcmp(const void* a, const void* b, unsigned long n);
extern unsigned long strlen(const char* s);

uint64_t cfs_now_ns(void) { static uint64_t t = 1000000000ull; return (t += 1000000ull); }

#define DISK_BLOCKS 2048u
static unsigned char disk[DISK_BLOCKS * CFS_BLOCK_SIZE];

static int md_read(void* c, uint64_t s, uint32_t n, void* b) {
    (void)c; if (s + n > (uint64_t)DISK_BLOCKS * CFS_SPB) return -1;
    memcpy(b, disk + s * 512u, (unsigned long)n * 512u); return 0;
}
static int md_write(void* c, uint64_t s, uint32_t n, const void* b) {
    (void)c; if (s + n > (uint64_t)DISK_BLOCKS * CFS_SPB) return -1;
    memcpy(disk + s * 512u, b, (unsigned long)n * 512u); return 0;
}
static cfs_bdev_t bd = { md_read, md_write, 0, 0 };

static int failures = 0;
static void check(int cond, const char* what) {
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what); if (!cond) failures++;
}

/* Direct views into the raw image (cfs.h gives us the on-disk layout). */
static cfs_super_t* SB(void) { return (cfs_super_t*)disk; }
static unsigned char* block_ptr(uint64_t b) { return disk + b * CFS_BLOCK_SIZE; }
static cfs_dinode_t* inode_ptr(uint64_t ino) {
    cfs_super_t* sb = SB();
    uint64_t off = ino * sb->inode_size;
    return (cfs_dinode_t*)(block_ptr(sb->inode_table_start) + off);
}
static void flip_block_bit(uint64_t b) {
    cfs_super_t* sb = SB();
    unsigned char* bm = block_ptr(sb->block_bitmap_start);
    bm[b >> 3] ^= (unsigned char)(1u << (b & 7));
}
static void set_inode_bit(uint64_t n) {
    cfs_super_t* sb = SB();
    unsigned char* bm = block_ptr(sb->inode_bitmap_start);
    bm[n >> 3] |= (unsigned char)(1u << (n & 7));
}

static int fsck_total(int repair) {
    cfs_fs_t fs; cfs_fsck_result_t r;
    cfs_mount_bd(&bd, &fs);
    cfs_fsck(&fs, repair, &r);
    return r.total;
}

int main(void) {
    cfs_fs_t fs;
    memset(disk, 0, sizeof disk);
    check(cfs_format(&bd, DISK_BLOCKS, 64, "fscktest") == 0, "format");
    cfs_mount_bd(&bd, &fs);
    /* build a small tree */
    cfs_mkdir(&fs, "/a");
    cfs_mkdir(&fs, "/a/b");
    cfs_write_file(&fs, "/a/b/file.txt", "hello fsck", 10);
    static unsigned char blob[20000];
    for (int i = 0; i < (int)sizeof blob; i++) blob[i] = (unsigned char)i;
    cfs_write_file(&fs, "/big.bin", blob, sizeof blob);
    cfs_mkdir(&fs, "/empty");

    printf("Clean volume:\n");
    check(fsck_total(0) == 0, "freshly built volume is clean");

    printf("Corruption: block bitmap leak (free block marked used)\n");
    {
        /* mark a definitely-free block used */
        uint64_t b = SB()->data_start + 500;
        flip_block_bit(b);                          /* now used-but-unreferenced */
        cfs_fsck_result_t r; cfs_mount_bd(&bd, &fs); cfs_fsck(&fs, 0, &r);
        check(r.block_leaks >= 1 && r.total >= 1, "detects a leaked block");
        check(fsck_total(1) >= 1 && fsck_total(0) == 0, "repairs it clean");
    }

    printf("Corruption: inode link count\n");
    {
        inode_ptr(CFS_ROOT_INO)->links += 5;        /* wrong root link count */
        cfs_fsck_result_t r; cfs_mount_bd(&bd, &fs); cfs_fsck(&fs, 0, &r);
        check(r.linkcount_bad >= 1, "detects a bad link count");
        check(fsck_total(1) >= 1 && fsck_total(0) == 0, "repairs it clean");
    }

    printf("Corruption: superblock free counts\n");
    {
        SB()->free_blocks += 123;
        SB()->free_inodes += 7;
        cfs_fsck_result_t r; cfs_mount_bd(&bd, &fs); cfs_fsck(&fs, 0, &r);
        check(r.freecount_bad >= 1, "detects wrong free counts");
        check(fsck_total(1) >= 1 && fsck_total(0) == 0, "repairs them clean");
    }

    printf("Corruption: orphan inode (allocated, unreferenced)\n");
    {
        /* find a free inode number and mark it allocated with no dir entry */
        uint64_t orphan = 50;                       /* well past root/used ones */
        set_inode_bit(orphan);
        cfs_fsck_result_t r; cfs_mount_bd(&bd, &fs); cfs_fsck(&fs, 0, &r);
        check(r.inode_leaks >= 1, "detects an orphan inode");
        check(fsck_total(1) >= 1 && fsck_total(0) == 0, "reclaims it clean");
    }

    printf("Corruption: inode checksum (flip a byte in an inode)\n");
    {
        inode_ptr(CFS_ROOT_INO)->mtime_ns ^= 0xFFull;   /* corrupt, csum now stale */
        cfs_fsck_result_t r; cfs_mount_bd(&bd, &fs); cfs_fsck(&fs, 0, &r);
        check(r.inode_csum_bad >= 1, "detects a corrupted inode via checksum");
        check(fsck_total(1) >= 1 && fsck_total(0) == 0, "repairs it clean");
    }

    printf("Corruption: superblock checksum\n");
    {
        SB()->label[0] ^= 0xFF;                         /* corrupt the superblock */
        cfs_fsck_result_t r; cfs_mount_bd(&bd, &fs); cfs_fsck(&fs, 0, &r);
        check(r.sb_csum_bad >= 1, "detects a corrupted superblock via checksum");
        check(fsck_total(1) >= 1 && fsck_total(0) == 0, "repairs it clean");
    }

    printf("Data intact after all repairs:\n");
    {
        cfs_mount_bd(&bd, &fs);
        char rb[32];
        int n = cfs_read_file(&fs, "/a/b/file.txt", rb, sizeof rb);
        check(n == 10 && memcmp(rb, "hello fsck", 10) == 0, "nested file still reads");
        static unsigned char got[20000];
        check(cfs_read_file(&fs, "/big.bin", got, sizeof got) == (int)sizeof blob
              && memcmp(got, blob, sizeof blob) == 0, "20000-byte file still intact");
        check(cfs_is_dir(&fs, "/empty") == 1, "empty dir still present");
    }

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL FSCK TESTS PASSED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}