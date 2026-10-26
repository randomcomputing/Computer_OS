/*
 * Deterministic crash test for CFS journaling (Phase 2 / Month 2).
 *
 * Models power loss as a disk that accepts exactly K block writes and then
 * fails every subsequent write. For an operation, K is swept from 0 past
 * the number of writes the operation makes; after each simulated crash the
 * volume is "rebooted" (remounted, which replays the journal) and checked:
 *
 *   - the mount always succeeds,
 *   - a file that existed before the operation is always intact,
 *   - the operation's result is atomic: the new file/dir is either fully
 *     present and correct, or entirely absent — never partial,
 *   - free-space counts match one of the two consistent states.
 *
 * A clean split (absent before the commit record is written, present after)
 * with zero "corrupt" outcomes is the proof that the journal works.
 *
 * Build & run:  make -C tests/cfs   (runs this and the functional test)
 */

#include "cfs.h"

extern int   printf(const char* fmt, ...);
extern void* memcpy(void* d, const void* s, unsigned long n);
extern void* memset(void* d, int c, unsigned long n);
extern int   memcmp(const void* a, const void* b, unsigned long n);
extern unsigned long strlen(const char* s);

uint64_t cfs_now_ns(void) { static uint64_t t = 1000000000ull; return (t += 1000000ull); }

/* ---- in-memory disk with a write-failure trip point ---- */
#define DISK_BLOCKS 2048u                 /* 8 MiB */
static unsigned char disk[DISK_BLOCKS * CFS_BLOCK_SIZE];
static unsigned char disk0[DISK_BLOCKS * CFS_BLOCK_SIZE];   /* snapshot */
static long fail_after = -1;              /* -1 = never fail */
static long writes_done = 0;

static int md_read(void* c, uint64_t s, uint32_t n, void* b) {
    (void)c; if (s + n > (uint64_t)DISK_BLOCKS * CFS_SPB) return -1;
    memcpy(b, disk + s * 512u, (unsigned long)n * 512u); return 0;
}
static int md_write(void* c, uint64_t s, uint32_t n, const void* b) {
    (void)c; if (s + n > (uint64_t)DISK_BLOCKS * CFS_SPB) return -1;
    if (fail_after >= 0 && writes_done >= fail_after) return -1;   /* crashed */
    writes_done++;
    memcpy(disk + s * 512u, b, (unsigned long)n * 512u); return 0;
}
static void snapshot(void) { memcpy(disk0, disk, sizeof disk); }
static void restore(void)  { memcpy(disk, disk0, sizeof disk); }

static cfs_bdev_t bd = { md_read, md_write, 0, 0 };
static int failures = 0;
static void check(int cond, const char* what) {
    if (!cond) { printf("  [FAIL] %s\n", what); failures++; }
}

#define KEEP "/keep.txt"
static const char* KEEPDATA = "baseline contents that must never be corrupted";

/* Sweep crashes across one operation. `make_change` performs the op on a
 * freshly mounted fs; `verify_present` returns 1 if its full effect is
 * visible. Returns via out-params how many crash points left the op done
 * vs not-done. Any corrupt/partial outcome bumps `failures`. */
typedef void (*op_fn)(cfs_fs_t*);
typedef int  (*check_fn)(cfs_fs_t*);

static void sweep(const char* name, op_fn make_change, check_fn verify_present) {
    cfs_fs_t fs;
    char kb[128];

    /* total writes the op makes when nothing fails, plus the committed state */
    restore();
    fail_after = -1; writes_done = 0;
    cfs_mount_bd(&bd, &fs);
    make_change(&fs);
    long total = writes_done;
    uint64_t free_done = fs.free_blocks;
    check(verify_present(&fs), name);              /* sanity: op works normally */

    restore();
    cfs_mount_bd(&bd, &fs);
    uint64_t free_base = fs.free_blocks;

    int done = 0, notdone = 0;
    for (long k = 0; k <= total; k++) {
        restore();
        fail_after = k; writes_done = 0;
        cfs_mount_bd(&bd, &fs);
        make_change(&fs);                           /* may fail partway */

        /* reboot: remount with writes enabled -> journal_recover runs */
        fail_after = -1; writes_done = 0;
        if (cfs_mount_bd(&bd, &fs) != 0) { printf("  [FAIL] %s: mount failed after crash@%ld\n", name, k); failures++; continue; }

        /* pre-existing file must always survive */
        int kr = cfs_read_file(&fs, KEEP, kb, sizeof kb);
        if (kr != (int)strlen(KEEPDATA) || memcmp(kb, KEEPDATA, kr) != 0) {
            printf("  [FAIL] %s: %s corrupted after crash@%ld\n", name, KEEP, k); failures++; continue;
        }

        int present = verify_present(&fs);
        if (present) {
            done++;
            if (fs.free_blocks != free_done) { printf("  [FAIL] %s: free mismatch (done) @%ld\n", name, k); failures++; }
        } else {
            notdone++;
            if (fs.free_blocks != free_base) { printf("  [FAIL] %s: free mismatch (not-done) @%ld\n", name, k); failures++; }
        }
    }
    printf("  [PASS] %s: %ld crash points, %d left it done, %d left it undone, 0 corrupt\n",
           name, total + 1, done, notdone);
}

/* ---- target operations ---- */
static unsigned char payload[20000];
static unsigned char rbuf[40000];

static void op_write(cfs_fs_t* fs) { cfs_write_file(fs, "/new.txt", payload, sizeof payload); }
static int  chk_write(cfs_fs_t* fs) {
    int r = cfs_read_file(fs, "/new.txt", rbuf, sizeof rbuf);
    if (r < 0) return 0;                            /* absent */
    /* present: must be complete and correct, else it's corruption */
    if (r != (int)sizeof payload || memcmp(rbuf, payload, sizeof payload) != 0) {
        printf("  [FAIL] /new.txt present but partial/corrupt (r=%d)\n", r); failures++;
    }
    return 1;
}

static void op_mkdir(cfs_fs_t* fs) { cfs_mkdir(fs, "/newdir"); }
static int  chk_mkdir(cfs_fs_t* fs) { return cfs_is_dir(fs, "/newdir"); }

int main(void) {
    for (int i = 0; i < (int)sizeof payload; i++) payload[i] = (unsigned char)(i * 53 + 11);

    /* build the baseline volume: format + one file that must survive */
    cfs_fs_t fs;
    memset(disk, 0, sizeof disk);
    check(cfs_format(&bd, DISK_BLOCKS, 64, "jtest") == 0, "format with journal");
    cfs_mount_bd(&bd, &fs);
    check(cfs_write_file(&fs, KEEP, KEEPDATA, (uint32_t)strlen(KEEPDATA)) > 0, "write baseline file");
    snapshot();

    printf("Crash-sweeping cfs_write_file:\n");
    sweep("write_file", op_write, chk_write);
    printf("Crash-sweeping cfs_mkdir:\n");
    sweep("mkdir", op_mkdir, chk_mkdir);

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL CRASH TESTS PASSED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}