/*
 * ext2.c — read/write ext2 filesystem driver.
 *
 * See include/ext2.h for the on-disk layout overview and feature list.
 *
 * Design notes
 * ------------
 *  - All metadata I/O (superblock aside) goes through a small write-through
 *    block cache. Write-through keeps things simple and durable: after each
 *    VFS operation returns, everything is on disk, so pulling the plug
 *    never leaves the volume half-updated for longer than one call.
 *  - File data is read/written directly between disk and the caller's
 *    buffer, batching contiguous blocks into one ATA request, so a big
 *    file read doesn't flush the metadata cache.
 *  - bread() returns a pointer into the cache that is only valid until the
 *    next bread()/bwrite(). Code below copies out what it needs right away
 *    and never holds two cache pointers at once.
 *  - Inode structs are passed around by pointer. Functions that modify a
 *    directory (dir_add / dir_remove) write the directory inode back
 *    themselves; callers that also change it (link counts) write it again.
 */

#include "ext2.h"
#include "vfs.h"
#include "ata.h"
#include "kheap.h"
#include "string.h"
#include "printf.h"
#include "rtc.h"
#include "stdint.h"

/* ------------------------------------------------------------------ */
/* On-disk structures                                                  */
/* ------------------------------------------------------------------ */

#define EXT2_MAGIC     0xEF53
#define EXT2_ROOT_INO  2
#define EXT2_NDIR      12              /* direct block pointers */

#define S_IFMT   0170000
#define S_IFREG  0100000
#define S_IFDIR  0040000
#define S_IFLNK  0120000
#define IS_DIR(m) (((m) & S_IFMT) == S_IFDIR)
#define IS_LNK(m) (((m) & S_IFMT) == S_IFLNK)

#define FT_UNKNOWN 0
#define FT_REG     1
#define FT_DIR     2
#define FT_SYMLINK 7

#define COMPAT_HAS_JOURNAL      0x0004

#define INCOMPAT_COMPRESSION    0x0001
#define INCOMPAT_FILETYPE       0x0002
#define INCOMPAT_RECOVER        0x0004
#define INCOMPAT_FLEX_BG        0x0200
#define INCOMPAT_SUPPORTED      (INCOMPAT_FILETYPE | INCOMPAT_FLEX_BG)

#define RO_COMPAT_SPARSE_SUPER  0x0001
#define RO_COMPAT_LARGE_FILE    0x0002
#define RO_COMPAT_DIR_NLINK     0x0020
#define RO_COMPAT_EXTRA_ISIZE   0x0040
/* ro_compat features we keep consistent when writing. Anything else
 * (gdt_csum, metadata_csum, huge_file, ...) => mount read-only. */
#define RO_COMPAT_SAFE (RO_COMPAT_SPARSE_SUPER | RO_COMPAT_LARGE_FILE | \
                        RO_COMPAT_DIR_NLINK | RO_COMPAT_EXTRA_ISIZE)

#define EXT2_INDEX_FL           0x00001000   /* htree-indexed directory */
#define EXT2_EA_MAGIC           0xEA020000

#define STATE_CLEAN             0x0001

typedef struct __attribute__((packed)) {
    uint32_t inodes_count;        /*   0 */
    uint32_t blocks_count;        /*   4 */
    uint32_t r_blocks_count;      /*   8 */
    uint32_t free_blocks_count;   /*  12 */
    uint32_t free_inodes_count;   /*  16 */
    uint32_t first_data_block;    /*  20 */
    uint32_t log_block_size;      /*  24 */
    uint32_t log_frag_size;       /*  28 */
    uint32_t blocks_per_group;    /*  32 */
    uint32_t frags_per_group;     /*  36 */
    uint32_t inodes_per_group;    /*  40 */
    uint32_t mtime;               /*  44 */
    uint32_t wtime;               /*  48 */
    uint16_t mnt_count;           /*  52 */
    uint16_t max_mnt_count;       /*  54 */
    uint16_t magic;               /*  56 */
    uint16_t state;               /*  58 */
    uint16_t errors;              /*  60 */
    uint16_t minor_rev_level;     /*  62 */
    uint32_t lastcheck;           /*  64 */
    uint32_t checkinterval;       /*  68 */
    uint32_t creator_os;          /*  72 */
    uint32_t rev_level;           /*  76 */
    uint16_t def_resuid;          /*  80 */
    uint16_t def_resgid;          /*  82 */
    /* revision 1 (EXT2_DYNAMIC_REV) */
    uint32_t first_ino;           /*  84 */
    uint16_t inode_size;          /*  88 */
    uint16_t block_group_nr;      /*  90 */
    uint32_t feature_compat;      /*  92 */
    uint32_t feature_incompat;    /*  96 */
    uint32_t feature_ro_compat;   /* 100 */
    uint8_t  uuid[16];            /* 104 */
    char     volume_name[16];     /* 120 */
    char     last_mounted[64];    /* 136 */
    uint32_t algo_bitmap;         /* 200 */
    uint8_t  prealloc_blocks;     /* 204 */
    uint8_t  prealloc_dir_blocks; /* 205 */
    uint16_t reserved_gdt_blocks; /* 206 */
} ext2_super_t;

typedef struct __attribute__((packed)) {
    uint32_t block_bitmap;
    uint32_t inode_bitmap;
    uint32_t inode_table;
    uint16_t free_blocks_count;
    uint16_t free_inodes_count;
    uint16_t used_dirs_count;
    uint16_t pad;
    uint32_t reserved[3];
} ext2_gd_t;                       /* 32 bytes */

typedef struct __attribute__((packed)) {
    uint16_t mode;
    uint16_t uid;
    uint32_t size;
    uint32_t atime;
    uint32_t ctime;
    uint32_t mtime;
    uint32_t dtime;
    uint16_t gid;
    uint16_t links_count;
    uint32_t blocks;               /* in 512-byte units, incl. indirect */
    uint32_t flags;
    uint32_t osd1;
    uint32_t block[15];
    uint32_t generation;
    uint32_t file_acl;             /* extended-attribute block */
    uint32_t size_high;            /* dir_acl in rev 0 */
    uint32_t faddr;
    uint8_t  osd2[12];
} ext2_inode_t;                    /* 128 bytes */

/* ------------------------------------------------------------------ */
/* Mount state                                                         */
/* ------------------------------------------------------------------ */

static int          mounted;
static int          writable;
static unsigned int part_lba;         /* partition start, in sectors */

static uint8_t       sb_raw[1024];
static ext2_super_t* sb = (ext2_super_t*)sb_raw;
static int           sb_dirty;

static uint32_t block_size;           /* bytes */
static uint32_t spb;                  /* sectors per block */
static uint32_t ppb;                  /* block pointers per block */
static uint32_t inode_size;           /* on-disk inode record size */
static uint32_t group_count;
static uint32_t gdt_start;            /* first block of the GDT */
static uint32_t gdt_blocks;
static int      has_filetype;
static uint32_t first_ino;           /* first non-reserved inode */

static uint8_t*   gdt_raw;
static ext2_gd_t* gdt;
static int        gdt_dirty_lo = -1;  /* dirty GDT block range */
static int        gdt_dirty_hi = -1;

/* ------------------------------------------------------------------ */
/* Raw block I/O                                                       */
/* ------------------------------------------------------------------ */

static int dev_read(uint32_t blk, uint32_t count, void* buf) {
    return ata_read(part_lba + blk * spb, count * spb, buf);
}

static int dev_write(uint32_t blk, uint32_t count, const void* buf) {
    return ata_write(part_lba + blk * spb, count * spb, buf);
}

/* ------------------------------------------------------------------ */
/* Write-through metadata block cache                                  */
/* ------------------------------------------------------------------ */

#define CACHE_N 64

typedef struct {
    uint32_t blk;
    uint32_t stamp;    /* last use, for LRU eviction */
    int      valid;
    uint8_t* data;
} cache_ent_t;

static cache_ent_t cache[CACHE_N];
static uint32_t    cache_clock;
static uint8_t*    cache_pool;

static cache_ent_t* cache_find(uint32_t blk) {
    for (int i = 0; i < CACHE_N; i++)
        if (cache[i].valid && cache[i].blk == blk) return &cache[i];
    return 0;
}

static cache_ent_t* cache_victim(void) {
    cache_ent_t* v = &cache[0];
    for (int i = 0; i < CACHE_N; i++) {
        if (!cache[i].valid) return &cache[i];
        if (cache[i].stamp < v->stamp) v = &cache[i];
    }
    return v;
}

/* Read a block through the cache. The pointer is valid until the next
 * bread()/bwrite()/binval() call. Returns 0 on I/O error. */
static uint8_t* bread(uint32_t blk) {
    cache_ent_t* e = cache_find(blk);
    if (!e) {
        e = cache_victim();
        e->valid = 0;
        if (dev_read(blk, 1, e->data) < 0) return 0;
        e->blk = blk;
        e->valid = 1;
    }
    e->stamp = ++cache_clock;
    return e->data;
}

/* Write a full block to disk and keep the cache coherent. `data` may be
 * the pointer bread() returned for this block. */
static int bwrite(uint32_t blk, const void* data) {
    cache_ent_t* e = cache_find(blk);
    if (!e) {
        e = cache_victim();
        e->blk = blk;
    }
    if (e->data != data) memcpy(e->data, data, block_size);
    e->valid = 1;
    e->stamp = ++cache_clock;
    if (dev_write(blk, 1, e->data) < 0) {
        e->valid = 0;
        return -1;
    }
    return 0;
}

static void binval(uint32_t blk) {
    cache_ent_t* e = cache_find(blk);
    if (e) e->valid = 0;
}

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* Seconds since the Unix epoch, from the CMOS clock (assumed UTC). */
static uint32_t now(void) {
    rtc_time_t t;
    rtc_read(&t);
    int y = (int)t.year;
    unsigned m = t.month, d = t.day;
    y -= m <= 2;
    int era = y / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = (int64_t)era * 146097 + (int64_t)doe - 719468;
    return (uint32_t)(days * 86400 + t.hour * 3600 + t.minute * 60 + t.second);
}

static uint32_t dirent_size(uint32_t name_len) { return (8 + name_len + 3) & ~3u; }

static int dirent_name_len(const uint8_t* de) {
    return has_filetype ? de[6] : rd16(de + 6);
}

static uint8_t mode_to_ft(uint16_t mode) {
    if (IS_DIR(mode)) return FT_DIR;
    if (IS_LNK(mode)) return FT_SYMLINK;
    if ((mode & S_IFMT) == S_IFREG) return FT_REG;
    return FT_UNKNOWN;
}

static void dirent_set(uint8_t* de, uint32_t ino, uint32_t rec_len,
                       const char* name, uint32_t nlen, uint8_t ft) {
    wr32(de, ino);
    wr16(de + 4, (uint16_t)rec_len);
    if (has_filetype) { de[6] = (uint8_t)nlen; de[7] = ft; }
    else              { wr16(de + 6, (uint16_t)nlen); }
    memcpy(de + 8, name, nlen);
}

/* ------------------------------------------------------------------ */
/* Superblock / group descriptor bookkeeping                           */
/* ------------------------------------------------------------------ */

static void gdt_mark_dirty(uint32_t g) {
    int b = (int)((g * sizeof(ext2_gd_t)) / block_size);
    if (gdt_dirty_lo < 0 || b < gdt_dirty_lo) gdt_dirty_lo = b;
    if (gdt_dirty_hi < 0 || b > gdt_dirty_hi) gdt_dirty_hi = b;
}

/* Flush the superblock and dirty group descriptors. Called at the end of
 * every mutating VFS operation. Only the primary copies are updated, which
 * is what Linux does too; e2fsck refreshes the backups. */
static int sync_meta(void) {
    int rc = 0;
    if (gdt_dirty_lo >= 0) {
        for (int b = gdt_dirty_lo; b <= gdt_dirty_hi; b++) {
            binval(gdt_start + (uint32_t)b);
            if (dev_write(gdt_start + (uint32_t)b, 1, gdt_raw + (uint32_t)b * block_size) < 0)
                rc = -1;
        }
        gdt_dirty_lo = gdt_dirty_hi = -1;
    }
    if (sb_dirty) {
        sb->wtime = now();
        /* The superblock lives at byte 1024 = sectors 2..3 of the volume. */
        binval(block_size == 1024 ? 1 : 0);
        if (ata_write(part_lba + 2, 2, sb_raw) < 0) rc = -1;
        sb_dirty = 0;
    }
    return rc;
}

static uint32_t group_of_ino(uint32_t ino) { return (ino - 1) / sb->inodes_per_group; }

/* Number of blocks that actually belong to group g (last one is short). */
static uint32_t blocks_in_group(uint32_t g) {
    if (g == group_count - 1)
        return sb->blocks_count - sb->first_data_block - g * sb->blocks_per_group;
    return sb->blocks_per_group;
}

/* ------------------------------------------------------------------ */
/* Block & inode allocation                                            */
/* ------------------------------------------------------------------ */

/* Find and claim a zero bit in a bitmap block. Returns bit index or -1. */
static int bitmap_claim(uint32_t bitmap_blk, uint32_t nbits) {
    uint8_t* bm = bread(bitmap_blk);
    if (!bm) return -1;
    for (uint32_t byte = 0; byte * 8 < nbits; byte++) {
        if (bm[byte] == 0xFF) continue;
        for (uint32_t bit = 0; bit < 8; bit++) {
            uint32_t i = byte * 8 + bit;
            if (i >= nbits) break;
            if (!(bm[byte] & (1u << bit))) {
                bm[byte] |= (uint8_t)(1u << bit);
                if (bwrite(bitmap_blk, bm) < 0) return -1;
                return (int)i;
            }
        }
    }
    return -1;
}

/* Clear a bit. Returns 1 if it was set, 0 if already clear, -1 on error. */
static int bitmap_release(uint32_t bitmap_blk, uint32_t i) {
    uint8_t* bm = bread(bitmap_blk);
    if (!bm) return -1;
    if (!(bm[i >> 3] & (1u << (i & 7)))) return 0;
    bm[i >> 3] &= (uint8_t)~(1u << (i & 7));
    if (bwrite(bitmap_blk, bm) < 0) return -1;
    return 1;
}

/* Allocate one block, preferring group `goal`. Returns 0 if the disk is
 * full. The block's contents are undefined. */
static uint32_t alloc_block(uint32_t goal) {
    if (sb->free_blocks_count == 0) return 0;
    for (uint32_t k = 0; k < group_count; k++) {
        uint32_t g = (goal + k) % group_count;
        if (gdt[g].free_blocks_count == 0) continue;
        int i = bitmap_claim(gdt[g].block_bitmap, blocks_in_group(g));
        if (i < 0) continue;
        gdt[g].free_blocks_count--;
        sb->free_blocks_count--;
        gdt_mark_dirty(g);
        sb_dirty = 1;
        uint32_t blk = sb->first_data_block + g * sb->blocks_per_group + (uint32_t)i;
        binval(blk);
        return blk;
    }
    return 0;
}

static void free_block(uint32_t blk) {
    if (blk < sb->first_data_block || blk >= sb->blocks_count) {
        printf("ext2: refusing to free out-of-range block %u\n", blk);
        return;
    }
    uint32_t rel = blk - sb->first_data_block;
    uint32_t g = rel / sb->blocks_per_group;
    binval(blk);
    int r = bitmap_release(gdt[g].block_bitmap, rel % sb->blocks_per_group);
    if (r == 1) {
        gdt[g].free_blocks_count++;
        sb->free_blocks_count++;
        gdt_mark_dirty(g);
        sb_dirty = 1;
    } else if (r == 0) {
        printf("ext2: block %u was already free\n", blk);
    }
}

/* Allocate an inode number, preferring group `goal`. Returns 0 if none. */
static uint32_t alloc_inode(uint32_t goal, int is_dir) {
    if (sb->free_inodes_count == 0) return 0;
    for (uint32_t k = 0; k < group_count; k++) {
        uint32_t g = (goal + k) % group_count;
        if (gdt[g].free_inodes_count == 0) continue;
        int i = bitmap_claim(gdt[g].inode_bitmap, sb->inodes_per_group);
        if (i < 0) continue;
        uint32_t ino = g * sb->inodes_per_group + (uint32_t)i + 1;
        if (ino < first_ino || ino > sb->inodes_count) {
            /* Reserved inode with a clear bit (shouldn't happen): leave it
             * marked used so we never hand it out, and keep looking. */
            continue;
        }
        gdt[g].free_inodes_count--;
        if (is_dir) gdt[g].used_dirs_count++;
        sb->free_inodes_count--;
        gdt_mark_dirty(g);
        sb_dirty = 1;
        return ino;
    }
    return 0;
}

static void free_inode_bit(uint32_t ino, int is_dir) {
    uint32_t g = group_of_ino(ino);
    int r = bitmap_release(gdt[g].inode_bitmap, (ino - 1) % sb->inodes_per_group);
    if (r == 1) {
        gdt[g].free_inodes_count++;
        if (is_dir && gdt[g].used_dirs_count) gdt[g].used_dirs_count--;
        sb->free_inodes_count++;
        gdt_mark_dirty(g);
        sb_dirty = 1;
    }
}

/* ------------------------------------------------------------------ */
/* Inodes                                                              */
/* ------------------------------------------------------------------ */

static int inode_loc(uint32_t ino, uint32_t* blk, uint32_t* off) {
    if (ino == 0 || ino > sb->inodes_count) return -1;
    uint32_t g   = group_of_ino(ino);
    uint32_t idx = (ino - 1) % sb->inodes_per_group;
    uint32_t byte = idx * inode_size;
    *blk = gdt[g].inode_table + byte / block_size;
    *off = byte % block_size;
    return 0;
}

static int read_inode(uint32_t ino, ext2_inode_t* out) {
    uint32_t blk, off;
    if (inode_loc(ino, &blk, &off) < 0) return -1;
    uint8_t* p = bread(blk);
    if (!p) return -1;
    memcpy(out, p + off, sizeof(*out));
    return 0;
}

/* Write the 128-byte base inode. If `fresh`, the whole on-disk record
 * (including any extra space past 128 bytes) is reset first. */
static int write_inode_ex(uint32_t ino, const ext2_inode_t* in, int fresh) {
    uint32_t blk, off;
    if (inode_loc(ino, &blk, &off) < 0) return -1;
    uint8_t* p = bread(blk);
    if (!p) return -1;
    if (fresh) {
        memset(p + off, 0, inode_size);
        /* Large inodes: declare the standard 32 bytes of extra fields
         * (as mke2fs/Linux do) so e2fsck sees a sane i_extra_isize. */
        if (inode_size >= 160) wr16(p + off + 128, 32);
    }
    memcpy(p + off, in, sizeof(*in));
    return bwrite(blk, p);
}

static int write_inode(uint32_t ino, const ext2_inode_t* in) {
    return write_inode_ex(ino, in, 0);
}

/* Fast symlinks keep the target inside i_block[] and own no data blocks. */
static int is_fast_symlink(const ext2_inode_t* in) {
    if (!IS_LNK(in->mode)) return 0;
    uint32_t ea = in->file_acl ? spb : 0;
    return in->blocks - ea == 0;
}

/* ------------------------------------------------------------------ */
/* Logical -> physical block mapping                                   */
/* ------------------------------------------------------------------ */

static uint64_t span(int depth) {
    uint64_t s = 1;
    while (depth-- > 0) s *= ppb;
    return s;
}

/* Allocate a block for inode `in` and account for it in i_blocks. */
static uint32_t inode_new_block(ext2_inode_t* in, uint32_t goal, int zero) {
    uint32_t b = alloc_block(goal);
    if (!b) return 0;
    in->blocks += spb;
    if (zero) {
        cache_ent_t* e = cache_victim();
        e->valid = 0;
        memset(e->data, 0, block_size);
        if (bwrite(b, e->data) < 0) return 0;
    }
    return b;
}

/* Map logical block `lblk` of inode `in` to a physical block. Returns 0 for
 * a hole. With `alloc`, missing data and indirect blocks are allocated
 * (indirect blocks zeroed); the caller must write the inode back. */
static uint32_t bmap(ext2_inode_t* in, uint32_t goal, uint32_t lblk, int alloc) {
    if (lblk < EXT2_NDIR) {
        if (!in->block[lblk] && alloc)
            in->block[lblk] = inode_new_block(in, goal, 0);
        return in->block[lblk];
    }

    uint64_t rel = lblk - EXT2_NDIR;
    int depth;
    if (rel < span(1)) depth = 1;
    else {
        rel -= span(1);
        if (rel < span(2)) depth = 2;
        else {
            rel -= span(2);
            if (rel >= span(3)) return 0;
            depth = 3;
        }
    }
    int root = 11 + depth;               /* i_block[12], [13] or [14] */

    uint32_t blk = in->block[root];
    if (!blk) {
        if (!alloc) return 0;
        blk = inode_new_block(in, goal, 1);
        if (!blk) return 0;
        in->block[root] = blk;
    }

    for (int level = depth; level > 0; level--) {
        uint32_t idx = (uint32_t)((rel / span(level - 1)) % ppb);
        uint8_t* p = bread(blk);
        if (!p) return 0;
        uint32_t next = rd32(p + idx * 4);
        if (!next) {
            if (!alloc) return 0;
            next = inode_new_block(in, goal, level > 1);
            if (!next) return 0;
            p = bread(blk);              /* inode_new_block may evict */
            if (!p) return 0;
            wr32(p + idx * 4, next);
            if (bwrite(blk, p) < 0) return 0;
        }
        blk = next;
    }
    return blk;
}

/* Free every block of the subtree rooted at `blk` (depth 0 = data block)
 * that maps logical blocks >= cutoff. The subtree covers logical blocks
 * [base, base + span(depth)). The block itself is freed iff base >= cutoff,
 * in which case the caller clears its pointer. */
static void trunc_tree(ext2_inode_t* in, uint32_t blk, int depth,
                       uint64_t base, uint64_t cutoff) {
    if (!blk) return;
    if (base + span(depth) <= cutoff) return;     /* entirely kept */

    if (depth > 0) {
        uint64_t cs = span(depth - 1);
        uint32_t* tab = (uint32_t*)kmalloc(block_size);
        if (!tab) return;
        uint8_t* p = bread(blk);
        if (!p) { kfree(tab); return; }
        memcpy(tab, p, block_size);
        int changed = 0;
        for (uint32_t i = 0; i < ppb; i++) {
            uint64_t cb = base + (uint64_t)i * cs;
            if (!tab[i] || cb + cs <= cutoff) continue;
            trunc_tree(in, tab[i], depth - 1, cb, cutoff);
            if (cb >= cutoff) { tab[i] = 0; changed = 1; }
        }
        if (base < cutoff && changed) bwrite(blk, tab);
        kfree(tab);
    }

    if (base >= cutoff) {
        free_block(blk);
        in->blocks -= spb;
    }
}

/* Release all data/indirect blocks past `new_size` bytes. */
static void truncate_blocks(ext2_inode_t* in, uint32_t new_size) {
    if (is_fast_symlink(in)) return;
    uint64_t cutoff = ((uint64_t)new_size + block_size - 1) / block_size;
    for (uint32_t i = 0; i < EXT2_NDIR; i++) {
        if (i >= cutoff && in->block[i]) {
            free_block(in->block[i]);
            in->blocks -= spb;
            in->block[i] = 0;
        }
    }
    uint64_t base = EXT2_NDIR;
    for (int d = 1; d <= 3; d++) {
        trunc_tree(in, in->block[11 + d], d, base, cutoff);
        if (base >= cutoff) in->block[11 + d] = 0;
        base += span(d);
    }
}

/* ------------------------------------------------------------------ */
/* File content                                                        */
/* ------------------------------------------------------------------ */

/* Read up to `len` bytes at `off`. Holes read as zeros. */
static int read_range(ext2_inode_t* in, uint8_t* out, uint32_t off, uint32_t len) {
    if (off >= in->size) return 0;
    if (len > in->size - off) len = in->size - off;

    uint32_t done = 0;
    while (done < len) {
        uint32_t pos   = off + done;
        uint32_t lb    = pos / block_size;
        uint32_t boff  = pos % block_size;
        uint32_t chunk = block_size - boff;
        if (chunk > len - done) chunk = len - done;

        uint32_t pb = bmap(in, 0, lb, 0);
        if (!pb) {
            memset(out + done, 0, chunk);
        } else if (boff == 0 && chunk == block_size) {
            /* Whole blocks: batch a physically contiguous run, read it
             * straight into the caller's buffer. */
            uint32_t run = 1;
            while (run < 128 && done + (run + 1) * block_size <= len &&
                   bmap(in, 0, lb + run, 0) == pb + run)
                run++;
            if (dev_read(pb, run, out + done) < 0) return -1;
            chunk = run * block_size;
        } else {
            uint8_t* p = bread(pb);
            if (!p) return -1;
            memcpy(out + done, p + boff, chunk);
        }
        done += chunk;
    }
    return (int)len;
}

/* Replace the whole content of a regular file with data[0..size). */
static int write_content(uint32_t ino, ext2_inode_t* in, const uint8_t* data, uint32_t size) {
    uint32_t goal = group_of_ino(ino);
    truncate_blocks(in, size);

    uint32_t nblocks = (size + block_size - 1) / block_size;
    uint32_t run_start = 0, run_len = 0, run_lb = 0;
    uint32_t committed = 0;          /* leading blocks known to be on disk */
    int rc = 0;

    for (uint32_t lb = 0; lb < nblocks; lb++) {
        uint32_t pb = bmap(in, goal, lb, 1);
        if (!pb) { rc = -1; break; }
        int full = (lb + 1) * block_size <= size;

        if (full && run_len && pb == run_start + run_len && run_len < 128) {
            run_len++;
            continue;
        }
        if (run_len) {
            for (uint32_t k = 0; k < run_len; k++) binval(run_start + k);
            if (dev_write(run_start, run_len, data + run_lb * block_size) < 0) { rc = -1; break; }
            committed = run_lb + run_len;
            run_len = 0;
        }
        if (full) {
            run_start = pb; run_len = 1; run_lb = lb;
        } else {
            /* Partial last block: zero-pad so the slack reads as zeros. */
            cache_ent_t* e = cache_victim();
            e->valid = 0;
            memset(e->data, 0, block_size);
            memcpy(e->data, data + lb * block_size, size - lb * block_size);
            if (bwrite(pb, e->data) < 0) { rc = -1; break; }
            committed = lb + 1;
        }
    }
    if (rc == 0 && run_len) {
        for (uint32_t k = 0; k < run_len; k++) binval(run_start + k);
        if (dev_write(run_start, run_len, data + run_lb * block_size) < 0) rc = -1;
    }

    if (rc < 0) {
        /* Out of space or I/O error: keep what made it, drop the rest. */
        uint32_t kept = committed * block_size;
        if (kept > size) kept = size;
        truncate_blocks(in, kept);
        in->size = kept;
    } else {
        in->size = size;
    }
    in->mtime = in->ctime = now();
    write_inode(ino, in);
    return rc;
}

static int read_symlink(ext2_inode_t* in, char* buf, uint32_t max) {
    uint32_t n = in->size;
    if (n >= max) return -1;
    if (is_fast_symlink(in)) {
        memcpy(buf, (const uint8_t*)in->block, n);
    } else {
        uint32_t pb = bmap(in, 0, 0, 0);
        if (!pb) return -1;
        uint8_t* p = bread(pb);
        if (!p) return -1;
        memcpy(buf, p, n);
    }
    buf[n] = '\0';
    return (int)n;
}

/* ------------------------------------------------------------------ */
/* Directories                                                         */
/* ------------------------------------------------------------------ */

static int dir_lookup(ext2_inode_t* dir, const char* name, uint32_t* ino_out) {
    uint32_t nlen = strlen(name);
    uint32_t nblocks = dir->size / block_size;
    for (uint32_t lb = 0; lb < nblocks; lb++) {
        uint32_t pb = bmap(dir, 0, lb, 0);
        if (!pb) continue;
        uint8_t* p = bread(pb);
        if (!p) return -1;
        uint32_t off = 0;
        while (off + 8 <= block_size) {
            uint8_t* de = p + off;
            uint32_t rec = rd16(de + 4);
            if (rec < 8 || off + rec > block_size) break;
            uint32_t ino = rd32(de);
            if (ino && (uint32_t)dirent_name_len(de) == nlen &&
                memcmp(de + 8, name, nlen) == 0) {
                *ino_out = ino;
                return 0;
            }
            off += rec;
        }
    }
    return -1;
}

static void dir_touch(uint32_t dir_ino, ext2_inode_t* dir) {
    dir->mtime = dir->ctime = now();
    /* We don't maintain htree indexes; dropping the flag turns the
     * directory back into a valid plain linear one. */
    dir->flags &= ~EXT2_INDEX_FL;
    write_inode(dir_ino, dir);
}

/* Add a (name -> ino) entry to a directory. */
static int dir_add(uint32_t dir_ino, ext2_inode_t* dir, const char* name,
                   uint32_t ino, uint8_t ft) {
    uint32_t nlen = strlen(name);
    if (nlen == 0 || nlen > 255) return -1;
    uint32_t need = dirent_size(nlen);
    uint32_t nblocks = dir->size / block_size;

    for (uint32_t lb = 0; lb < nblocks; lb++) {
        uint32_t pb = bmap(dir, 0, lb, 0);
        if (!pb) continue;
        uint8_t* p = bread(pb);
        if (!p) return -1;
        uint32_t off = 0;
        while (off + 8 <= block_size) {
            uint8_t* de = p + off;
            uint32_t rec = rd16(de + 4);
            if (rec < 8 || off + rec > block_size) break;
            uint32_t used = rd32(de) ? dirent_size((uint32_t)dirent_name_len(de)) : 0;
            if (rec >= used + need) {
                if (used) {
                    wr16(de + 4, (uint16_t)used);
                    dirent_set(de + used, ino, rec - used, name, nlen, ft);
                } else {
                    dirent_set(de, ino, rec, name, nlen, ft);
                }
                if (bwrite(pb, p) < 0) return -1;
                dir_touch(dir_ino, dir);
                return 0;
            }
            off += rec;
        }
    }

    /* No room: append a fresh block holding just this entry. */
    uint32_t pb = bmap(dir, group_of_ino(dir_ino), nblocks, 1);
    if (!pb) return -1;
    cache_ent_t* e = cache_victim();
    e->valid = 0;
    memset(e->data, 0, block_size);
    dirent_set(e->data, ino, block_size, name, nlen, ft);
    if (bwrite(pb, e->data) < 0) return -1;
    dir->size += block_size;
    dir_touch(dir_ino, dir);
    return 0;
}

static int dir_remove(uint32_t dir_ino, ext2_inode_t* dir, const char* name) {
    uint32_t nlen = strlen(name);
    uint32_t nblocks = dir->size / block_size;
    for (uint32_t lb = 0; lb < nblocks; lb++) {
        uint32_t pb = bmap(dir, 0, lb, 0);
        if (!pb) continue;
        uint8_t* p = bread(pb);
        if (!p) return -1;
        uint32_t off = 0, prev = 0xFFFFFFFFu;
        while (off + 8 <= block_size) {
            uint8_t* de = p + off;
            uint32_t rec = rd16(de + 4);
            if (rec < 8 || off + rec > block_size) break;
            if (rd32(de) && (uint32_t)dirent_name_len(de) == nlen &&
                memcmp(de + 8, name, nlen) == 0) {
                if (prev != 0xFFFFFFFFu) {
                    uint8_t* pd = p + prev;           /* merge into previous */
                    wr16(pd + 4, (uint16_t)(rd16(pd + 4) + rec));
                } else {
                    wr32(de, 0);                      /* first in block */
                }
                if (bwrite(pb, p) < 0) return -1;
                dir_touch(dir_ino, dir);
                return 0;
            }
            prev = off;
            off += rec;
        }
    }
    return -1;
}

/* 1 if the directory holds nothing but "." and "..". */
static int dir_is_empty(ext2_inode_t* dir) {
    uint32_t nblocks = dir->size / block_size;
    for (uint32_t lb = 0; lb < nblocks; lb++) {
        uint32_t pb = bmap(dir, 0, lb, 0);
        if (!pb) continue;
        uint8_t* p = bread(pb);
        if (!p) return 0;
        uint32_t off = 0;
        while (off + 8 <= block_size) {
            uint8_t* de = p + off;
            uint32_t rec = rd16(de + 4);
            if (rec < 8 || off + rec > block_size) break;
            if (rd32(de)) {
                int nl = dirent_name_len(de);
                int dot = (nl == 1 && de[8] == '.') ||
                          (nl == 2 && de[8] == '.' && de[9] == '.');
                if (!dot) return 0;
            }
            off += rec;
        }
    }
    return 1;
}

/* Point a directory's ".." entry at a new parent. */
static int dir_set_dotdot(uint32_t ino, uint32_t parent) {
    ext2_inode_t in;
    if (read_inode(ino, &in) < 0) return -1;
    uint32_t pb = bmap(&in, 0, 0, 0);
    if (!pb) return -1;
    uint8_t* p = bread(pb);
    if (!p) return -1;
    uint32_t rec = rd16(p + 4);                 /* skip "." */
    if (rec < 12 || rec + 12 > block_size) return -1;
    uint8_t* dd = p + rec;
    if (dirent_name_len(dd) != 2 || dd[8] != '.' || dd[9] != '.') return -1;
    wr32(dd, parent);
    return bwrite(pb, p);
}

/* ------------------------------------------------------------------ */
/* Path resolution                                                     */
/* ------------------------------------------------------------------ */

#define MAX_SYMLINKS 8

/* Resolve a path (absolute within this mount) to an inode number.
 * Symlinks in the middle of the path are always followed; the final
 * component is followed only if `follow_last`. Absolute symlink targets
 * are resolved from this volume's root (correct when ext2 is mounted at
 * "/"). Returns 0 if not found. */
static uint32_t namei(const char* path, int follow_last) {
    char work[512];
    uint32_t n = 0;
    for (; path[n] && n < sizeof(work) - 1; n++) work[n] = path[n];
    if (path[n]) return 0;
    work[n] = '\0';

    uint32_t cur = EXT2_ROOT_INO;
    int links = 0;
    char* p = work;

    for (;;) {
        while (*p == '/') p++;
        if (!*p) return cur;

        char comp[256];
        uint32_t cl = 0;
        while (*p && *p != '/') {
            if (cl >= 255) return 0;
            comp[cl++] = *p++;
        }
        comp[cl] = '\0';
        while (*p == '/') p++;
        int last = (*p == '\0');

        if (cl == 1 && comp[0] == '.') continue;

        ext2_inode_t din;
        if (read_inode(cur, &din) < 0 || !IS_DIR(din.mode)) return 0;
        uint32_t child;
        if (dir_lookup(&din, comp, &child) < 0) return 0;

        ext2_inode_t cin;
        if (read_inode(child, &cin) < 0) return 0;
        if (IS_LNK(cin.mode) && (!last || follow_last)) {
            if (++links > MAX_SYMLINKS) return 0;
            char tgt[256];
            if (read_symlink(&cin, tgt, sizeof(tgt)) < 0) return 0;
            char next[512];
            uint32_t o = 0;
            for (uint32_t i = 0; tgt[i]; i++) { if (o >= sizeof(next) - 2) return 0; next[o++] = tgt[i]; }
            next[o++] = '/';
            for (uint32_t i = 0; p[i]; i++) { if (o >= sizeof(next) - 1) return 0; next[o++] = p[i]; }
            next[o] = '\0';
            memcpy(work, next, o + 1);
            p = work;
            if (tgt[0] == '/') cur = EXT2_ROOT_INO;   /* else stay in link's dir */
            continue;
        }
        cur = child;
    }
}

/* Split "/a/b/c" into parent inode (of "/a/b") and basename "c". */
static int split_parent(const char* path, uint32_t* parent, char* base) {
    int len = (int)strlen(path);
    while (len > 1 && path[len - 1] == '/') len--;   /* trailing slashes */
    int slash = -1;
    for (int i = 0; i < len; i++) if (path[i] == '/') slash = i;

    int bl = len - (slash + 1);
    if (bl <= 0 || bl > 255) return -1;
    memcpy(base, path + slash + 1, (uint32_t)bl);
    base[bl] = '\0';
    if ((bl == 1 && base[0] == '.') || (bl == 2 && base[0] == '.' && base[1] == '.'))
        return -1;

    char ppath[512];
    if (slash <= 0) { ppath[0] = '/'; ppath[1] = '\0'; }
    else {
        if (slash >= (int)sizeof(ppath)) return -1;
        memcpy(ppath, path, (uint32_t)slash);
        ppath[slash] = '\0';
    }
    uint32_t pino = namei(ppath, 1);
    if (!pino) return -1;
    ext2_inode_t pin;
    if (read_inode(pino, &pin) < 0 || !IS_DIR(pin.mode)) return -1;
    *parent = pino;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Inode lifetime                                                      */
/* ------------------------------------------------------------------ */

/* Drop an inode whose link count reached zero: free its blocks, its
 * extended-attribute block, and its inode-bitmap bit. */
static void release_inode(uint32_t ino, ext2_inode_t* in) {
    int is_dir = IS_DIR(in->mode);
    truncate_blocks(in, 0);
    if (in->file_acl) {
        uint8_t* p = bread(in->file_acl);
        if (p && rd32(p) == EXT2_EA_MAGIC && rd32(p + 4) > 1) {
            wr32(p + 4, rd32(p + 4) - 1);       /* shared: drop a ref */
            bwrite(in->file_acl, p);
        } else {
            free_block(in->file_acl);
        }
        in->blocks -= spb;
        in->file_acl = 0;
    }
    if (is_fast_symlink(in)) memset(in->block, 0, sizeof(in->block));
    in->links_count = 0;
    in->size = 0;
    in->dtime = now();
    write_inode(ino, in);
    free_inode_bit(ino, is_dir);
}

/* Remove `base` from directory `pino`. want_dir selects unlink vs rmdir. */
static int do_unlink(uint32_t pino, ext2_inode_t* pin, const char* base, int want_dir) {
    uint32_t ino;
    if (dir_lookup(pin, base, &ino) < 0) return -1;
    ext2_inode_t in;
    if (read_inode(ino, &in) < 0) return -1;
    if (IS_DIR(in.mode) != (want_dir != 0)) return -1;
    if (want_dir && !dir_is_empty(&in)) return -1;

    if (dir_remove(pino, pin, base) < 0) return -1;

    if (want_dir) {
        if (pin->links_count > 2) pin->links_count--;   /* lost its ".." */
        write_inode(pino, pin);
        in.links_count = 0;
    } else if (in.links_count) {
        in.links_count--;
    }

    if (in.links_count == 0) release_inode(ino, &in);
    else { in.ctime = now(); write_inode(ino, &in); }
    return 0;
}

/* Create a new inode of the given mode and link it into the parent. */
static uint32_t create_node(uint32_t pino, ext2_inode_t* pin, const char* base,
                            uint16_t mode, ext2_inode_t* in) {
    uint32_t ino = alloc_inode(group_of_ino(pino), IS_DIR(mode));
    if (!ino) return 0;
    memset(in, 0, sizeof(*in));
    in->mode = mode;
    in->links_count = IS_DIR(mode) ? 2 : 1;
    in->atime = in->ctime = in->mtime = now();
    if (write_inode_ex(ino, in, 1) < 0 ||
        dir_add(pino, pin, base, ino, mode_to_ft(mode)) < 0) {
        free_inode_bit(ino, IS_DIR(mode));
        return 0;
    }
    return ino;
}

/* ------------------------------------------------------------------ */
/* VFS operations                                                      */
/* ------------------------------------------------------------------ */

static int fill_dirents(uint32_t dino, vfs_dirent_t* out, int max) {
    ext2_inode_t dir;
    if (read_inode(dino, &dir) < 0 || !IS_DIR(dir.mode)) return -1;

    /* Pass 1: names and inode numbers (needs the dir blocks in cache). */
    int n = 0;
    uint32_t nblocks = dir.size / block_size;
    for (uint32_t lb = 0; lb < nblocks && n < max; lb++) {
        uint32_t pb = bmap(&dir, 0, lb, 0);
        if (!pb) continue;
        uint8_t* p = bread(pb);
        if (!p) return -1;
        uint32_t off = 0;
        while (off + 8 <= block_size && n < max) {
            uint8_t* de = p + off;
            uint32_t rec = rd16(de + 4);
            if (rec < 8 || off + rec > block_size) break;
            uint32_t ino = rd32(de);
            int nl = dirent_name_len(de);
            int dot = (nl == 1 && de[8] == '.') ||
                      (nl == 2 && de[8] == '.' && de[9] == '.');
            if (ino && nl > 0 && !dot) {
                memcpy(out[n].name, de + 8, (uint32_t)nl);
                out[n].name[nl] = '\0';
                out[n].first_cluster = ino;
                n++;
            }
            off += rec;
        }
    }

    /* Pass 2: sizes and types from the inodes. */
    for (int i = 0; i < n; i++) {
        ext2_inode_t in;
        if (read_inode(out[i].first_cluster, &in) < 0) {
            out[i].size = 0; out[i].attr = 0; continue;
        }
        if (IS_DIR(in.mode))      { out[i].size = 0; out[i].attr = 0x10; }
        else if (IS_LNK(in.mode)) { out[i].size = VFS_SYMLINK_MAGIC_LEN + in.size; out[i].attr = 0; }
        else                      { out[i].size = in.size; out[i].attr = 0; }
    }
    return n;
}

static int ext2_list(const char* path, vfs_dirent_t* out, int max) {
    if (!mounted) return -1;
    uint32_t ino = namei(path, 1);
    if (!ino) return -1;
    return fill_dirents(ino, out, max);
}

static int ext2_is_dir(const char* path) {
    if (!mounted) return 0;
    uint32_t ino = namei(path, 1);
    if (!ino) return 0;
    ext2_inode_t in;
    if (read_inode(ino, &in) < 0) return 0;
    return IS_DIR(in.mode) ? 1 : 0;
}

/* Symlinks read back as VFS_SYMLINK_MAGIC + target (not followed). */
static int read_link_as_file(ext2_inode_t* in, uint8_t* buf, uint32_t off, uint32_t len) {
    uint32_t cap = VFS_SYMLINK_MAGIC_LEN + block_size + 1;
    char* content = (char*)kmalloc(cap);
    if (!content) return -1;
    memcpy(content, VFS_SYMLINK_MAGIC, VFS_SYMLINK_MAGIC_LEN);
    int tl = read_symlink(in, content + VFS_SYMLINK_MAGIC_LEN, cap - VFS_SYMLINK_MAGIC_LEN);
    int rc = -1;
    if (tl >= 0) {
        uint32_t total = VFS_SYMLINK_MAGIC_LEN + (uint32_t)tl;
        if (off >= total) len = 0;
        else if (len > total - off) len = total - off;
        if (len) memcpy(buf, content + off, len);
        rc = (int)len;
    }
    kfree(content);
    return rc;
}

static int ext2_read_file_at(const char* path, void* buf, unsigned int offset, unsigned int len) {
    if (!mounted) return -1;
    uint32_t ino = namei(path, 0);
    if (!ino) return -1;
    ext2_inode_t in;
    if (read_inode(ino, &in) < 0 || IS_DIR(in.mode)) return -1;
    if (IS_LNK(in.mode)) return read_link_as_file(&in, (uint8_t*)buf, offset, len);
    return read_range(&in, (uint8_t*)buf, offset, len);
}

static int ext2_read_file(const char* path, void* buf, unsigned int max) {
    return ext2_read_file_at(path, buf, 0, max);
}

static int ext2_write_file(const char* path, const void* data, unsigned int size) {
    if (!mounted || !writable) return -1;
    const uint8_t* d = (const uint8_t*)data;
    int is_link = size >= VFS_SYMLINK_MAGIC_LEN &&
                  memcmp(d, VFS_SYMLINK_MAGIC, VFS_SYMLINK_MAGIC_LEN) == 0;

    uint32_t pino;
    char base[256];
    if (split_parent(path, &pino, base) < 0) return -1;
    ext2_inode_t pin;
    if (read_inode(pino, &pin) < 0) return -1;

    ext2_inode_t in;
    uint32_t ino = 0;
    if (dir_lookup(&pin, base, &ino) == 0) {
        if (read_inode(ino, &in) < 0) return -1;
        if (IS_DIR(in.mode)) return -1;
        if (is_link || IS_LNK(in.mode)) {
            /* Changing between file and symlink: replace the inode. */
            if (do_unlink(pino, &pin, base, 0) < 0) { sync_meta(); return -1; }
            ino = 0;
        }
    } else {
        ino = 0;
    }

    int rc = 0;
    if (is_link) {
        uint32_t tl = size - VFS_SYMLINK_MAGIC_LEN;
        if (tl == 0 || tl >= block_size) { sync_meta(); return -1; }
        ino = create_node(pino, &pin, base, S_IFLNK | 0777, &in);
        if (!ino) { sync_meta(); return -1; }
        if (tl < sizeof(in.block)) {
            memcpy(in.block, d + VFS_SYMLINK_MAGIC_LEN, tl);   /* fast symlink */
        } else {
            uint32_t pb = bmap(&in, group_of_ino(ino), 0, 1);
            if (!pb) rc = -1;
            else {
                cache_ent_t* e = cache_victim();
                e->valid = 0;
                memset(e->data, 0, block_size);
                memcpy(e->data, d + VFS_SYMLINK_MAGIC_LEN, tl);
                if (bwrite(pb, e->data) < 0) rc = -1;
            }
        }
        in.size = rc == 0 ? tl : 0;
        write_inode(ino, &in);
    } else {
        if (!ino) {
            ino = create_node(pino, &pin, base, S_IFREG | 0644, &in);
            if (!ino) { sync_meta(); return -1; }
        }
        rc = write_content(ino, &in, d, size);
    }

    if (sync_meta() < 0) rc = -1;
    return rc < 0 ? -1 : (int)size;
}

static int ext2_delete_file(const char* path) {
    if (!mounted || !writable) return -1;
    uint32_t pino;
    char base[256];
    if (split_parent(path, &pino, base) < 0) return -1;
    ext2_inode_t pin;
    if (read_inode(pino, &pin) < 0) return -1;
    int rc = do_unlink(pino, &pin, base, 0);
    if (sync_meta() < 0) rc = -1;
    return rc;
}

static int ext2_mkdir(const char* path) {
    if (!mounted || !writable) return -1;
    uint32_t pino;
    char base[256];
    if (split_parent(path, &pino, base) < 0) return -1;
    ext2_inode_t pin;
    if (read_inode(pino, &pin) < 0) return -1;
    uint32_t existing;
    if (dir_lookup(&pin, base, &existing) == 0) return -1;

    uint32_t ino = alloc_inode(group_of_ino(pino), 1);
    if (!ino) return -1;

    ext2_inode_t in;
    memset(&in, 0, sizeof(in));
    in.mode = S_IFDIR | 0755;
    in.links_count = 2;                       /* "." + parent's entry */
    in.atime = in.ctime = in.mtime = now();

    uint32_t pb = bmap(&in, group_of_ino(ino), 0, 1);
    if (!pb) { free_inode_bit(ino, 1); sync_meta(); return -1; }
    cache_ent_t* e = cache_victim();
    e->valid = 0;
    memset(e->data, 0, block_size);
    dirent_set(e->data,      ino,  12,              ".",  1, FT_DIR);
    dirent_set(e->data + 12, pino, block_size - 12, "..", 2, FT_DIR);
    int rc = bwrite(pb, e->data);
    in.size = block_size;

    if (rc == 0) rc = write_inode_ex(ino, &in, 1);
    if (rc == 0) rc = dir_add(pino, &pin, base, ino, FT_DIR);
    if (rc < 0) {
        truncate_blocks(&in, 0);
        free_inode_bit(ino, 1);
    } else {
        pin.links_count++;                    /* new child's ".." */
        write_inode(pino, &pin);
    }
    if (sync_meta() < 0) rc = -1;
    return rc;
}

static int ext2_rmdir(const char* path) {
    if (!mounted || !writable) return -1;
    uint32_t pino;
    char base[256];
    if (split_parent(path, &pino, base) < 0) return -1;
    ext2_inode_t pin;
    if (read_inode(pino, &pin) < 0) return -1;
    int rc = do_unlink(pino, &pin, base, 1);
    if (sync_meta() < 0) rc = -1;
    return rc;
}

/* If `dst` names an existing directory, append the basename of `src`. */
static int target_path(const char* src, const char* dst, char* out, uint32_t outsz) {
    uint32_t dl = strlen(dst);
    if (dl >= outsz) return -1;
    memcpy(out, dst, dl + 1);
    uint32_t dino = namei(dst, 1);
    if (!dino) return 0;
    ext2_inode_t din;
    if (read_inode(dino, &din) < 0 || !IS_DIR(din.mode)) return 0;
    const char* b = src;
    for (const char* s = src; *s; s++) if (*s == '/' && s[1]) b = s + 1;
    uint32_t bl = 0;
    while (b[bl] && b[bl] != '/') bl++;
    if (dl + 1 + bl + 1 > outsz) return -1;
    uint32_t o = dl;
    if (o == 0 || out[o - 1] != '/') out[o++] = '/';
    memcpy(out + o, b, bl);
    out[o + bl] = '\0';
    return 0;
}

static int ext2_cp(const char* src, const char* dst) {
    if (!mounted || !writable) return -1;
    uint32_t sino = namei(src, 0);
    if (!sino) return -1;
    ext2_inode_t sin;
    if (read_inode(sino, &sin) < 0 || IS_DIR(sin.mode)) return -1;

    char dpath[512];
    if (target_path(src, dst, dpath, sizeof(dpath)) < 0) return -1;

    uint32_t sz = IS_LNK(sin.mode) ? VFS_SYMLINK_MAGIC_LEN + sin.size : sin.size;
    uint8_t* buf = (uint8_t*)kmalloc(sz ? sz : 1);
    if (!buf) return -1;
    int n = ext2_read_file_at(src, buf, 0, sz);
    int rc = n < 0 ? -1 : ext2_write_file(dpath, buf, (unsigned int)n);
    kfree(buf);
    return rc < 0 ? -1 : 0;
}

static int ext2_mv(const char* src, const char* dst) {
    if (!mounted || !writable) return -1;

    uint32_t spino;
    char sbase[256];
    if (split_parent(src, &spino, sbase) < 0) return -1;
    ext2_inode_t spin;
    if (read_inode(spino, &spin) < 0) return -1;
    uint32_t sino;
    if (dir_lookup(&spin, sbase, &sino) < 0) return -1;
    ext2_inode_t sin;
    if (read_inode(sino, &sin) < 0) return -1;
    int moving_dir = IS_DIR(sin.mode);

    char dpath[512];
    if (target_path(src, dst, dpath, sizeof(dpath)) < 0) return -1;
    uint32_t dpino;
    char dbase[256];
    if (split_parent(dpath, &dpino, dbase) < 0) return -1;

    /* One struct when source and destination share a parent, so updates
     * made through one pointer are seen through the other. */
    ext2_inode_t dpin_store;
    ext2_inode_t* sp = &spin;
    ext2_inode_t* dp = &spin;
    if (dpino != spino) {
        if (read_inode(dpino, &dpin_store) < 0) return -1;
        dp = &dpin_store;
    }

    if (dpino == spino && strcmp(sbase, dbase) == 0) return 0;

    /* A directory can't move into its own subtree. */
    if (moving_dir) {
        uint32_t walk = dpino;
        for (int guard = 0; guard < 4096; guard++) {
            if (walk == sino) return -1;
            if (walk == EXT2_ROOT_INO) break;
            ext2_inode_t w;
            uint32_t up;
            if (read_inode(walk, &w) < 0 || dir_lookup(&w, "..", &up) < 0) return -1;
            walk = up;
        }
    }

    int rc = 0;
    uint32_t tino;
    if (dir_lookup(dp, dbase, &tino) == 0) {
        if (tino == sino) return 0;
        ext2_inode_t tin;
        if (read_inode(tino, &tin) < 0) return -1;
        if (IS_DIR(tin.mode) != moving_dir) return -1;
        if (do_unlink(dpino, dp, dbase, moving_dir) < 0) { sync_meta(); return -1; }
    }

    if (dir_add(dpino, dp, dbase, sino, mode_to_ft(sin.mode)) < 0) { sync_meta(); return -1; }
    if (dir_remove(spino, sp, sbase) < 0) rc = -1;

    if (moving_dir && dpino != spino) {
        if (dir_set_dotdot(sino, dpino) < 0) rc = -1;
        dp->links_count++;
        if (sp->links_count > 2) sp->links_count--;
        write_inode(dpino, dp);
        write_inode(spino, sp);
    }
    if (read_inode(sino, &sin) == 0) { sin.ctime = now(); write_inode(sino, &sin); }

    if (sync_meta() < 0) rc = -1;
    return rc;
}

static const vfs_ops_t ext2_ops = {
    .list         = ext2_list,
    .is_dir       = ext2_is_dir,
    .read_file    = ext2_read_file,
    .read_file_at = ext2_read_file_at,
    .write_file   = ext2_write_file,
    .delete_file  = ext2_delete_file,
    .mkdir        = ext2_mkdir,
    .rmdir        = ext2_rmdir,
    .cp           = ext2_cp,
    .mv           = ext2_mv,
};

const vfs_ops_t* ext2_vfs_ops(void) { return &ext2_ops; }

/* ------------------------------------------------------------------ */
/* Mount                                                               */
/* ------------------------------------------------------------------ */

/* Collect partition start LBAs from the GPT. Returns how many. */
static int gpt_partitions(unsigned int* out, int max) {
    uint8_t hdr[512];
    if (ata_read(1, 1, hdr) < 0) return 0;
    if (memcmp(hdr, "EFI PART", 8) != 0) return 0;

    uint32_t entries_lba = rd32(hdr + 72);
    uint32_t num_entries = rd32(hdr + 80);
    uint32_t entry_size  = rd32(hdr + 84);
    if (entry_size < 128 || entry_size > 512 || (512 % entry_size) != 0) return 0;
    if (num_entries > 256) num_entries = 256;

    int n = 0;
    uint32_t per_sector = 512 / entry_size;
    uint8_t sec[512];
    for (uint32_t i = 0; i < num_entries && n < max; i++) {
        if (i % per_sector == 0 && ata_read(entries_lba + i / per_sector, 1, sec) < 0) break;
        uint8_t* e = sec + (i % per_sector) * entry_size;
        int empty = 1;
        for (int b = 0; b < 16; b++) if (e[b]) { empty = 0; break; }
        if (empty) continue;
        uint32_t start = rd32(e + 32);       /* low half of 64-bit first LBA */
        if (start) out[n++] = start;
    }
    return n;
}

static int probe_at(unsigned int lba) {
    if (ata_read(lba + 2, 2, sb_raw) < 0) return 0;
    return sb->magic == EXT2_MAGIC;
}

int ext2_mount(void) {
    if (mounted) return 0;

    unsigned int cands[16];
    int nc = gpt_partitions(cands, 16);
    if (nc == 0) cands[nc++] = 0;            /* no GPT: try the whole disk */

    int found = 0;
    for (int i = 0; i < nc && !found; i++) {
        if (probe_at(cands[i])) { part_lba = cands[i]; found = 1; }
    }
    if (!found) return -1;

    if (sb->log_block_size > 2) {
        printf("ext2: block size %u unsupported\n", 1024u << sb->log_block_size);
        return -1;
    }
    if (sb->blocks_per_group == 0 || sb->inodes_per_group == 0 ||
        sb->blocks_count <= sb->first_data_block) {
        printf("ext2: superblock looks corrupt\n");
        return -1;
    }

    block_size = 1024u << sb->log_block_size;
    spb        = block_size / 512;
    ppb        = block_size / 4;

    uint32_t incompat = 0, ro_compat = 0;
    if (sb->rev_level == 0) {
        inode_size = 128;                    /* rev 0: fixed values, no features */
        first_ino  = 11;
    } else {
        inode_size = sb->inode_size;
        first_ino  = sb->first_ino;
        incompat   = sb->feature_incompat;
        ro_compat  = sb->feature_ro_compat;
        if (inode_size < 128 || inode_size > block_size || (inode_size & (inode_size - 1))) {
            printf("ext2: bad inode size %u\n", inode_size);
            return -1;
        }
    }

    if (incompat & INCOMPAT_RECOVER) {
        printf("ext2: journal needs recovery - run e2fsck first\n");
        return -1;
    }
    if (incompat & ~INCOMPAT_SUPPORTED) {
        printf("ext2: unsupported features 0x%x (ext4?)\n", incompat & ~INCOMPAT_SUPPORTED);
        return -1;
    }
    has_filetype = (incompat & INCOMPAT_FILETYPE) != 0;

    writable = 1;
    if (ro_compat & ~RO_COMPAT_SAFE) {
        printf("ext2: ro_compat features 0x%x, mounting read-only\n",
               ro_compat & ~RO_COMPAT_SAFE);
        writable = 0;
    }
    if (!(sb->state & STATE_CLEAN) || (sb->state & 0x2)) {
        printf("ext2: volume not cleanly unmounted, mounting read-only\n");
        writable = 0;
    }

    group_count = (sb->blocks_count - sb->first_data_block + sb->blocks_per_group - 1)
                / sb->blocks_per_group;
    gdt_start  = sb->first_data_block + 1;
    gdt_blocks = (group_count * (uint32_t)sizeof(ext2_gd_t) + block_size - 1) / block_size;

    gdt_raw = (uint8_t*)kmalloc(gdt_blocks * block_size);
    cache_pool = (uint8_t*)kmalloc(CACHE_N * block_size);
    if (!gdt_raw || !cache_pool) {
        printf("ext2: out of memory\n");
        if (gdt_raw) kfree(gdt_raw);
        if (cache_pool) kfree(cache_pool);
        gdt_raw = cache_pool = 0;
        return -1;
    }
    for (int i = 0; i < CACHE_N; i++) {
        cache[i].valid = 0;
        cache[i].stamp = 0;
        cache[i].data  = cache_pool + (uint32_t)i * block_size;
    }
    if (dev_read(gdt_start, gdt_blocks, gdt_raw) < 0) {
        printf("ext2: can't read group descriptors\n");
        kfree(gdt_raw); kfree(cache_pool);
        gdt_raw = cache_pool = 0;
        return -1;
    }
    gdt = (ext2_gd_t*)gdt_raw;

    ext2_inode_t root;
    mounted = 1;
    if (read_inode(EXT2_ROOT_INO, &root) < 0 || !IS_DIR(root.mode)) {
        printf("ext2: root inode is not a directory\n");
        mounted = 0;
        return -1;
    }
    return 0;
}

int ext2_writable(void) { return mounted && writable; }

void ext2_print_info(void) {
    if (!mounted) { printf("ext2: not mounted\n"); return; }
    char label[17];
    memcpy(label, sb->volume_name, 16);
    label[16] = '\0';
    uint32_t kb_per_block = block_size / 1024;
    printf("ext2: \"%s\" %uK blocks, %u groups, %u/%u MB free, %u/%u inodes free, %s\n",
           label[0] ? label : "(no label)", kb_per_block, group_count,
           (sb->free_blocks_count / 1024) * kb_per_block,
           (sb->blocks_count / 1024) * kb_per_block,
           sb->free_inodes_count, sb->inodes_count,
           writable ? "read-write" : "read-only");
}