/*
 * cfs.c — CFS filesystem core (Phase 1 / Month 1).
 *
 * Backend-agnostic: all disk access goes through a cfs_bdev_t vtable, so
 * this one file compiles into the kernel driver (ATA backend) and into the
 * host mkfs/test tools (file backend) unchanged. See include/cfs.h for the
 * on-disk format.
 *
 * Design notes
 * ------------
 *  - Metadata I/O is read-modify-write a whole 4 KiB block at a time, into
 *    a small set of dedicated static scratch buffers (one per role, so a
 *    directory walk and an inode update never clobber each other). The FS
 *    is single-threaded, matching the rest of the kernel; there is no block
 *    cache yet (that is Month 5).
 *  - Free counts live in the mount struct and are flushed to the superblock
 *    at the end of each mutating operation (write-through durability).
 *  - A file maps to a list of extents in logical order. extent.start == 0
 *    denotes a hole (sparse); reads of a hole return zeroes. The Month-1
 *    whole-file writer never creates holes, but the format allows them.
 */

#include "cfs.h"
#include "string.h"

/* ------------------------------------------------------------------ */
/* Compile-time layout checks                                          */
/* ------------------------------------------------------------------ */
_Static_assert(sizeof(cfs_extent_t) == 12, "extent must be 12 bytes");
_Static_assert(sizeof(cfs_dirent_t) == 12, "dirent header must be 12 bytes");
_Static_assert(sizeof(cfs_super_t) <= CFS_BLOCK_SIZE, "superblock too big");
_Static_assert(sizeof(cfs_dinode_t) <= CFS_INODE_SIZE, "inode too big");

/* Timestamp hook — the kernel and host each provide a strong definition.
 * The weak default keeps the file linkable if nobody does. (Omitted in the
 * kernel build, which defines a strong cfs_now_ns in the binding below.) */
uint64_t cfs_now_ns(void);
#ifndef CFS_KERNEL
__attribute__((weak)) uint64_t cfs_now_ns(void) { return 0; }
#endif

/* Caller identity hooks — the kernel returns the current credential, the
 * host test a settable stub. Default (weak) is root, so unprivileged code
 * paths and the existing tests behave as before. */
uint32_t cfs_caller_uid(void);
uint32_t cfs_caller_gid(void);
#ifndef CFS_KERNEL
__attribute__((weak)) uint32_t cfs_caller_uid(void) { return 0; }
__attribute__((weak)) uint32_t cfs_caller_gid(void) { return 0; }
#endif

/* rwx masks for may_access(). */
#define CFS_R 4
#define CFS_W 2
#define CFS_X 1

/* POSIX permission check against the caller's identity. want is an rwx
 * mask. uid 0 (root) bypasses all checks. */
static int may_access(uint16_t mode, uint32_t iuid, uint32_t igid, int want) {
    uint32_t uid = cfs_caller_uid();
    if (uid == 0) return 1;
    uint16_t perm;
    if (uid == iuid)                   perm = (uint16_t)((mode >> 6) & 7);
    else if (cfs_caller_gid() == igid) perm = (uint16_t)((mode >> 3) & 7);
    else                               perm = (uint16_t)(mode & 7);
    return ((perm & want) == want);
}

#define BS            CFS_BLOCK_SIZE
#define ALIGN8(x)     (((x) + 7u) & ~7u)

/* Dedicated 4 KiB scratch buffers, 8-byte aligned for struct casts. */
static unsigned char sb_buf[BS]  __attribute__((aligned(8)));
static unsigned char bm_buf[BS]  __attribute__((aligned(8)));
static unsigned char it_buf[BS]  __attribute__((aligned(8)));
static unsigned char dir_buf[BS] __attribute__((aligned(8)));
static unsigned char io_buf[BS]  __attribute__((aligned(8)));

/* ------------------------------------------------------------------ */
/* Block device helpers (4 KiB block <-> 512-byte sectors)             */
/* ------------------------------------------------------------------ */

static int raw_read_block(cfs_fs_t* fs, uint64_t block, void* buf) {
    uint64_t sec = fs->bd->base + block * fs->spb;
    return fs->bd->read(fs->bd->ctx, sec, fs->spb, buf);
}
static int raw_write_block(cfs_fs_t* fs, uint64_t block, const void* buf) {
    uint64_t sec = fs->bd->base + block * fs->spb;
    return fs->bd->write(fs->bd->ctx, sec, fs->spb, buf);
}

/* ------------------------------------------------------------------ */
/* Journaling — write-ahead metadata log (Month 2)                     */
/* ------------------------------------------------------------------ */
/*
 * One committed transaction lives in the journal at a time. A mutating op
 * runs inside a transaction: changes to existing metadata (bitmaps, inode
 * table, directory blocks, superblock) are buffered rather than written to
 * their homes, and reads consult the buffer first so the op sees its own
 * pending changes. At commit, the buffered blocks are written to the
 * journal, a checksummed commit record is made durable, and only then are
 * the blocks checkpointed to their homes. On mount, a committed-but-
 * unfinished transaction is replayed (redo); a torn one is ignored. File
 * data and freshly allocated blocks are written straight to disk (via the
 * raw_* helpers) before commit — ordered mode — so a committed transaction
 * never references stale contents.
 *
 * bd_read_block / bd_write_block are the transaction-aware entry points
 * used for all metadata; with no transaction active they are identical to
 * raw_read_block / raw_write_block, so format and read-only paths are
 * unaffected.
 */

#define CFS_JMAGIC      0x4A534643u   /* 'C','F','S','J' */
#define CFS_JNONE       0u
#define CFS_JCOMMITTED  1u
#define CFS_MAX_TXN     32            /* metadata blocks journaled per op */

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t state;       /* CFS_JNONE / CFS_JCOMMITTED */
    uint64_t seq;         /* transaction sequence number */
    uint32_t nblocks;     /* data blocks in this transaction */
    uint32_t csum;        /* crc32 of descriptor + data blocks */
} cfs_jhdr_t;

/* crc32 (IEEE), chainable: crc32_up(crc32_up(0,a,na),b,nb) == crc of a||b. */
static uint32_t crc32_up(uint32_t crc, const void* data, uint32_t len) {
    const unsigned char* p = (const unsigned char*)data;
    crc = ~crc;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return ~crc;
}

static struct { uint64_t home; unsigned char data[BS]; } txn[CFS_MAX_TXN]
    __attribute__((aligned(8)));
static int      txn_active = 0;
static int      txn_count  = 0;
static uint64_t txn_free_blocks, txn_free_inodes;   /* snapshot for abort */
static uint64_t jseq = 1;
static unsigned char j_buf[BS]  __attribute__((aligned(8)));
static unsigned char j_desc[BS] __attribute__((aligned(8)));

static int txn_find(uint64_t block) {
    for (int i = 0; i < txn_count; i++) if (txn[i].home == block) return i;
    return -1;
}

static int bd_read_block(cfs_fs_t* fs, uint64_t block, void* buf) {
    if (txn_active) {
        int i = txn_find(block);
        if (i >= 0) { memcpy(buf, txn[i].data, BS); return 0; }
    }
    return raw_read_block(fs, block, buf);
}

static int bd_write_block(cfs_fs_t* fs, uint64_t block, const void* buf) {
    if (!txn_active) return raw_write_block(fs, block, buf);
    int i = txn_find(block);
    if (i < 0) {
        if (txn_count >= CFS_MAX_TXN) return -1;   /* transaction too large */
        i = txn_count++;
        txn[i].home = block;
    }
    memcpy(txn[i].data, buf, BS);
    return 0;
}

static void txn_begin(cfs_fs_t* fs) {
    txn_active = 1;
    txn_count = 0;
    txn_free_blocks = fs->free_blocks;
    txn_free_inodes = fs->free_inodes;
}

static void txn_abort(cfs_fs_t* fs) {
    txn_active = 0;
    txn_count = 0;
    fs->free_blocks = txn_free_blocks;      /* undo in-memory alloc counts */
    fs->free_inodes = txn_free_inodes;
}

static int txn_commit(cfs_fs_t* fs) {
    if (!txn_active) return 0;
    int n = txn_count;
    if (n == 0) { txn_active = 0; return 0; }

    uint64_t j = fs->journal_start;
    if (j == 0 || (uint64_t)(n + 2) > fs->journal_blocks) {
        /* No journal, or op too big to log: fall back to write-through. */
        for (int i = 0; i < n; i++)
            if (raw_write_block(fs, txn[i].home, txn[i].data) < 0) { txn_active = 0; return -1; }
        txn_active = 0; txn_count = 0;
        return 0;
    }

    /* 1. descriptor: home block numbers. */
    memset(j_desc, 0, BS);
    uint64_t* homes = (uint64_t*)j_desc;
    for (int i = 0; i < n; i++) homes[i] = txn[i].home;
    if (raw_write_block(fs, j + 1, j_desc) < 0) return -1;

    /* 2. data blocks. */
    for (int i = 0; i < n; i++)
        if (raw_write_block(fs, j + 2 + i, txn[i].data) < 0) return -1;

    /* 3. checksum over descriptor + data. */
    uint32_t csum = crc32_up(0, j_desc, BS);
    for (int i = 0; i < n; i++) csum = crc32_up(csum, txn[i].data, BS);

    /* 4. commit record — the atomic point. */
    memset(j_buf, 0, BS);
    cfs_jhdr_t* h = (cfs_jhdr_t*)j_buf;
    h->magic = CFS_JMAGIC; h->state = CFS_JCOMMITTED;
    h->seq = jseq; h->nblocks = (uint32_t)n; h->csum = csum;
    if (raw_write_block(fs, j, j_buf) < 0) return -1;

    /* 5. checkpoint to home locations. */
    for (int i = 0; i < n; i++)
        if (raw_write_block(fs, txn[i].home, txn[i].data) < 0) return -1;

    /* 6. release the journal. */
    memset(j_buf, 0, BS);
    ((cfs_jhdr_t*)j_buf)->magic = CFS_JMAGIC;
    ((cfs_jhdr_t*)j_buf)->state = CFS_JNONE;
    ((cfs_jhdr_t*)j_buf)->seq = jseq;
    if (raw_write_block(fs, j, j_buf) < 0) return -1;

    jseq++;
    txn_active = 0; txn_count = 0;
    return 0;
}

/* Replay a committed transaction left behind by a crash, then clear it. */
static void journal_recover(cfs_fs_t* fs) {
    uint64_t j = fs->journal_start;
    if (j == 0) return;
    if (raw_read_block(fs, j, j_buf) < 0) return;
    cfs_jhdr_t h = *(cfs_jhdr_t*)j_buf;
    if (h.magic != CFS_JMAGIC || h.state != CFS_JCOMMITTED) return;
    if (h.nblocks == 0 || (uint64_t)(h.nblocks + 2) > fs->journal_blocks) return;

    if (raw_read_block(fs, j + 1, j_desc) < 0) return;
    uint32_t csum = crc32_up(0, j_desc, BS);
    for (uint32_t i = 0; i < h.nblocks; i++) {
        if (raw_read_block(fs, j + 2 + i, j_buf) < 0) return;
        csum = crc32_up(csum, j_buf, BS);
    }
    if (csum != h.csum) return;                 /* torn journal: don't replay */

    uint64_t* homes = (uint64_t*)j_desc;
    for (uint32_t i = 0; i < h.nblocks; i++) {
        if (raw_read_block(fs, j + 2 + i, j_buf) < 0) return;
        raw_write_block(fs, homes[i], j_buf);   /* redo (idempotent) */
    }
    if (h.seq >= jseq) jseq = h.seq + 1;

    memset(j_buf, 0, BS);
    ((cfs_jhdr_t*)j_buf)->magic = CFS_JMAGIC;
    ((cfs_jhdr_t*)j_buf)->state = CFS_JNONE;
    raw_write_block(fs, j, j_buf);
}

/* ------------------------------------------------------------------ */
/* Bitmaps                                                             */
/* ------------------------------------------------------------------ */

static int bit_set(cfs_fs_t* fs, uint64_t region, uint64_t idx, int val) {
    uint64_t blk = region + idx / (BS * 8);
    uint32_t byte = (uint32_t)((idx / 8) % BS);
    if (bd_read_block(fs, blk, bm_buf) < 0) return -1;
    unsigned char m = (unsigned char)(1u << (idx & 7));
    if (val) bm_buf[byte] |= m; else bm_buf[byte] &= (unsigned char)~m;
    return bd_write_block(fs, blk, bm_buf);
}

/* First free block at or after data_start; marks it used. 0 = full. */
static uint64_t alloc_block(cfs_fs_t* fs) {
    uint64_t nbits = fs->total_blocks;
    uint64_t bmblocks = (nbits + BS * 8 - 1) / (BS * 8);
    for (uint64_t bb = 0; bb < bmblocks; bb++) {
        if (bd_read_block(fs, fs->block_bitmap_start + bb, bm_buf) < 0) return 0;
        uint64_t base = bb * BS * 8;
        for (uint32_t i = 0; i < BS; i++) {
            if (bm_buf[i] == 0xFF) continue;
            for (int j = 0; j < 8; j++) {
                uint64_t b = base + (uint64_t)i * 8 + j;
                if (b >= fs->total_blocks) return 0;
                if (b < fs->data_start) continue;
                if (!((bm_buf[i] >> j) & 1)) {
                    bm_buf[i] |= (unsigned char)(1u << j);
                    if (bd_write_block(fs, fs->block_bitmap_start + bb, bm_buf) < 0)
                        return 0;
                    fs->free_blocks--;
                    return b;
                }
            }
        }
    }
    return 0;
}
static void free_block(cfs_fs_t* fs, uint64_t b) {
    if (b < fs->data_start || b >= fs->total_blocks) return;
    if (bit_set(fs, fs->block_bitmap_start, b, 0) == 0) fs->free_blocks++;
}

/* First free inode at or after 2 (0 reserved, 1 root). 0 = none. */
static uint64_t alloc_inode(cfs_fs_t* fs) {
    uint64_t bmblocks = (fs->total_inodes + BS * 8 - 1) / (BS * 8);
    for (uint64_t bb = 0; bb < bmblocks; bb++) {
        if (bd_read_block(fs, fs->inode_bitmap_start + bb, bm_buf) < 0) return 0;
        uint64_t base = bb * BS * 8;
        for (uint32_t i = 0; i < BS; i++) {
            if (bm_buf[i] == 0xFF) continue;
            for (int j = 0; j < 8; j++) {
                uint64_t n = base + (uint64_t)i * 8 + j;
                if (n >= fs->total_inodes) return 0;
                if (n < 2) continue;
                if (!((bm_buf[i] >> j) & 1)) {
                    bm_buf[i] |= (unsigned char)(1u << j);
                    if (bd_write_block(fs, fs->inode_bitmap_start + bb, bm_buf) < 0)
                        return 0;
                    fs->free_inodes--;
                    return n;
                }
            }
        }
    }
    return 0;
}
static void free_inode(cfs_fs_t* fs, uint64_t n) {
    if (n < 2 || n >= fs->total_inodes) return;
    if (bit_set(fs, fs->inode_bitmap_start, n, 0) == 0) fs->free_inodes++;
}

/* ------------------------------------------------------------------ */
/* Inodes                                                              */
/* ------------------------------------------------------------------ */

static int read_dinode(cfs_fs_t* fs, uint64_t ino, cfs_dinode_t* out) {
    if (ino == 0 || ino >= fs->total_inodes) return -1;
    uint64_t byteoff = ino * fs->inode_size;
    uint64_t blk = fs->inode_table_start + byteoff / BS;
    uint32_t off = (uint32_t)(byteoff % BS);
    if (bd_read_block(fs, blk, it_buf) < 0) return -1;
    memcpy(out, it_buf + off, sizeof(cfs_dinode_t));
    return 0;
}
static int write_dinode(cfs_fs_t* fs, uint64_t ino, const cfs_dinode_t* in) {
    if (ino == 0 || ino >= fs->total_inodes) return -1;
    uint64_t byteoff = ino * fs->inode_size;
    uint64_t blk = fs->inode_table_start + byteoff / BS;
    uint32_t off = (uint32_t)(byteoff % BS);
    if (bd_read_block(fs, blk, it_buf) < 0) return -1;
    memcpy(it_buf + off, in, sizeof(cfs_dinode_t));
    return bd_write_block(fs, blk, it_buf);
}

static void sync_super(cfs_fs_t* fs) {
    if (bd_read_block(fs, 0, sb_buf) < 0) return;
    cfs_super_t* sb = (cfs_super_t*)sb_buf;
    sb->free_blocks = fs->free_blocks;
    sb->free_inodes = fs->free_inodes;
    sb->state = CFS_STATE_CLEAN;
    bd_write_block(fs, 0, sb_buf);
}

/* Map a file-relative block index to a device block. Returns the physical
 * block, 0 for a hole, or 0 when past the mapped range (caller zero-fills). */
static uint64_t map_fileblock(const cfs_dinode_t* in, uint64_t fileblk) {
    uint64_t logical = 0;
    for (uint32_t e = 0; e < in->nextents; e++) {
        uint64_t cnt = in->extent[e].count;
        if (fileblk < logical + cnt) {
            if (in->extent[e].start == 0) return 0;        /* hole */
            return in->extent[e].start + (fileblk - logical);
        }
        logical += cnt;
    }
    return 0;
}

/* Append `count` blocks starting at `start` to an inode's extent list,
 * coalescing with the last extent when physically contiguous. Returns -1
 * if the inline extent array is full. */
static int inode_push_extent(cfs_dinode_t* in, uint64_t start, uint32_t count) {
    if (in->nextents > 0) {
        cfs_extent_t* last = &in->extent[in->nextents - 1];
        if (last->start != 0 && last->start + last->count == start) {
            last->count += count;
            return 0;
        }
    }
    if (in->nextents >= CFS_INLINE_EXTENTS) return -1;
    in->extent[in->nextents].start = start;
    in->extent[in->nextents].count = count;
    in->nextents++;
    return 0;
}

static void inode_free_data(cfs_fs_t* fs, cfs_dinode_t* in) {
    for (uint32_t e = 0; e < in->nextents; e++) {
        if (in->extent[e].start == 0) continue;           /* hole */
        for (uint32_t k = 0; k < in->extent[e].count; k++)
            free_block(fs, in->extent[e].start + k);
    }
    in->nextents = 0;
    in->size = 0;
}

/* ------------------------------------------------------------------ */
/* Directories                                                         */
/* ------------------------------------------------------------------ */

static int name_eq(const cfs_dirent_t* de, const unsigned char* block,
                   uint32_t off, const char* name, uint32_t nlen) {
    if (de->inode == 0 || de->name_len != nlen) return 0;
    return memcmp(block + off + CFS_DIRENT_HDR, name, nlen) == 0;
}

/* Find `name` in directory `dino`; returns child inode or 0. */
static uint64_t dir_lookup(cfs_fs_t* fs, const cfs_dinode_t* dino,
                           const char* name, uint32_t nlen) {
    uint64_t nblk = dino->size / BS;
    for (uint64_t fb = 0; fb < nblk; fb++) {
        uint64_t pb = map_fileblock(dino, fb);
        if (pb == 0) continue;
        if (bd_read_block(fs, pb, dir_buf) < 0) return 0;
        uint32_t off = 0;
        while (off + CFS_DIRENT_HDR <= BS) {
            cfs_dirent_t* de = (cfs_dirent_t*)(dir_buf + off);
            if (de->rec_len < CFS_DIRENT_HDR || off + de->rec_len > BS) break;
            if (name_eq(de, dir_buf, off, name, nlen)) return de->inode;
            off += de->rec_len;
        }
    }
    return 0;
}

/* Write a fresh directory block containing exactly `.` and `..`. */
static void dir_init_block(unsigned char* blk, uint64_t self, uint64_t parent) {
    memset(blk, 0, BS);
    cfs_dirent_t* dot = (cfs_dirent_t*)blk;
    dot->inode = self; dot->name_len = 1; dot->type = CFS_FT_DIR;
    dot->rec_len = (uint16_t)ALIGN8(CFS_DIRENT_HDR + 1);
    blk[CFS_DIRENT_HDR] = '.';
    cfs_dirent_t* dd = (cfs_dirent_t*)(blk + dot->rec_len);
    dd->inode = parent; dd->name_len = 2; dd->type = CFS_FT_DIR;
    dd->rec_len = (uint16_t)(BS - dot->rec_len);
    blk[dot->rec_len + CFS_DIRENT_HDR] = '.';
    blk[dot->rec_len + CFS_DIRENT_HDR + 1] = '.';
}

/* Insert an entry into directory inode `dino` (ino `dino_no`). */
static int dir_add(cfs_fs_t* fs, cfs_dinode_t* dino,
                   const char* name, uint32_t nlen, uint64_t child, uint8_t type) {
    uint32_t need = ALIGN8(CFS_DIRENT_HDR + nlen);
    uint64_t nblk = dino->size / BS;

    for (uint64_t fb = 0; fb < nblk; fb++) {
        uint64_t pb = map_fileblock(dino, fb);
        if (pb == 0) continue;
        if (bd_read_block(fs, pb, dir_buf) < 0) return -1;
        uint32_t off = 0;
        while (off + CFS_DIRENT_HDR <= BS) {
            cfs_dirent_t* de = (cfs_dirent_t*)(dir_buf + off);
            if (de->rec_len < CFS_DIRENT_HDR || off + de->rec_len > BS) break;

            if (de->inode == 0 && de->rec_len >= need) {
                de->inode = child; de->name_len = (uint8_t)nlen; de->type = type;
                memcpy(dir_buf + off + CFS_DIRENT_HDR, name, nlen);
                return bd_write_block(fs, pb, dir_buf);
            }
            uint32_t used = (de->inode != 0)
                          ? ALIGN8(CFS_DIRENT_HDR + de->name_len) : 0;
            if (de->inode != 0 && de->rec_len - used >= need) {
                uint16_t newlen = (uint16_t)(de->rec_len - used);
                de->rec_len = (uint16_t)used;
                cfs_dirent_t* nd = (cfs_dirent_t*)(dir_buf + off + used);
                nd->inode = child; nd->rec_len = newlen;
                nd->name_len = (uint8_t)nlen; nd->type = type;
                memcpy(dir_buf + off + used + CFS_DIRENT_HDR, name, nlen);
                return bd_write_block(fs, pb, dir_buf);
            }
            off += de->rec_len;
        }
    }

    /* No room: append a fresh directory block holding just this entry. */
    uint64_t b = alloc_block(fs);
    if (b == 0) return -1;
    memset(dir_buf, 0, BS);
    cfs_dirent_t* de = (cfs_dirent_t*)dir_buf;
    de->inode = child; de->rec_len = (uint16_t)BS;
    de->name_len = (uint8_t)nlen; de->type = type;
    memcpy(dir_buf + CFS_DIRENT_HDR, name, nlen);
    if (raw_write_block(fs, b, dir_buf) < 0) return -1;   /* new block: ordered */
    if (inode_push_extent(dino, b, 1) < 0) { free_block(fs, b); return -1; }
    dino->size += BS;
    return 0;
}

/* Remove `name` from directory `dino`; coalesces freed space backward. */
static int dir_remove(cfs_fs_t* fs, const cfs_dinode_t* dino,
                      const char* name, uint32_t nlen) {
    uint64_t nblk = dino->size / BS;
    for (uint64_t fb = 0; fb < nblk; fb++) {
        uint64_t pb = map_fileblock(dino, fb);
        if (pb == 0) continue;
        if (bd_read_block(fs, pb, dir_buf) < 0) return -1;
        uint32_t off = 0, prev = 0; int have_prev = 0;
        while (off + CFS_DIRENT_HDR <= BS) {
            cfs_dirent_t* de = (cfs_dirent_t*)(dir_buf + off);
            if (de->rec_len < CFS_DIRENT_HDR || off + de->rec_len > BS) break;
            if (name_eq(de, dir_buf, off, name, nlen)) {
                if (have_prev) {
                    cfs_dirent_t* pd = (cfs_dirent_t*)(dir_buf + prev);
                    pd->rec_len = (uint16_t)(pd->rec_len + de->rec_len);
                } else {
                    de->inode = 0;
                }
                return bd_write_block(fs, pb, dir_buf);
            }
            prev = off; have_prev = 1;
            off += de->rec_len;
        }
    }
    return -1;
}

/* 1 if directory holds nothing but `.` and `..`. */
static int dir_is_empty(cfs_fs_t* fs, const cfs_dinode_t* dino) {
    uint64_t nblk = dino->size / BS;
    for (uint64_t fb = 0; fb < nblk; fb++) {
        uint64_t pb = map_fileblock(dino, fb);
        if (pb == 0) continue;
        if (bd_read_block(fs, pb, dir_buf) < 0) return 0;
        uint32_t off = 0;
        while (off + CFS_DIRENT_HDR <= BS) {
            cfs_dirent_t* de = (cfs_dirent_t*)(dir_buf + off);
            if (de->rec_len < CFS_DIRENT_HDR || off + de->rec_len > BS) break;
            if (de->inode != 0) {
                const char* nm = (const char*)(dir_buf + off + CFS_DIRENT_HDR);
                int dot  = (de->name_len == 1 && nm[0] == '.');
                int ddot = (de->name_len == 2 && nm[0] == '.' && nm[1] == '.');
                if (!dot && !ddot) return 0;
            }
            off += de->rec_len;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Path resolution                                                     */
/* ------------------------------------------------------------------ */

static uint64_t resolve_ino(cfs_fs_t* fs, const char* path) {
    uint64_t ino = fs->root_ino;
    const char* p = path;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char* s = p;
        while (*p && *p != '/') p++;
        uint32_t nlen = (uint32_t)(p - s);
        if (nlen > CFS_NAME_MAX) return 0;
        cfs_dinode_t din;
        if (read_dinode(fs, ino, &din) < 0) return 0;
        if (!CFS_ISDIR(din.mode)) return 0;
        ino = dir_lookup(fs, &din, s, nlen);
        if (ino == 0) return 0;
    }
    return ino;
}

/* Split `path` into its parent directory inode and the final component. */
static int resolve_parent(cfs_fs_t* fs, const char* path,
                          uint64_t* parent, char* leaf, uint32_t* leaflen) {
    const char* slash = 0;
    for (const char* q = path; *q; q++) if (*q == '/') slash = q;
    const char* name = slash ? slash + 1 : path;
    uint32_t nlen = (uint32_t)strlen(name);
    if (nlen == 0 || nlen > CFS_NAME_MAX) return -1;

    char pbuf[256];
    if (!slash || slash == path) {
        pbuf[0] = '/'; pbuf[1] = '\0';
    } else {
        uint32_t plen = (uint32_t)(slash - path);
        if (plen >= sizeof(pbuf)) return -1;
        memcpy(pbuf, path, plen); pbuf[plen] = '\0';
    }
    uint64_t pino = resolve_ino(fs, pbuf);
    if (pino == 0) return -1;
    cfs_dinode_t din;
    if (read_dinode(fs, pino, &din) < 0 || !CFS_ISDIR(din.mode)) return -1;

    *parent = pino;
    memcpy(leaf, name, nlen); leaf[nlen] = '\0';
    *leaflen = nlen;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Mount / format                                                      */
/* ------------------------------------------------------------------ */

int cfs_mount_bd(cfs_bdev_t* bd, cfs_fs_t* fs) {
    txn_active = 0; txn_count = 0;        /* a fresh mount has no in-flight txn */
    memset(fs, 0, sizeof(*fs));
    fs->bd = bd;
    fs->spb = CFS_SPB;
    if (bd->read(bd->ctx, bd->base, CFS_SPB, sb_buf) < 0) return -1;
    cfs_super_t* sb = (cfs_super_t*)sb_buf;
    if (sb->magic != CFS_MAGIC) return -1;
    if (sb->version != CFS_VERSION) return -1;
    if (sb->block_size != BS) return -1;

    fs->block_size         = sb->block_size;
    fs->inode_size         = sb->inode_size;
    fs->total_blocks       = sb->total_blocks;
    fs->total_inodes       = sb->total_inodes;
    fs->free_blocks        = sb->free_blocks;
    fs->free_inodes        = sb->free_inodes;
    fs->block_bitmap_start = sb->block_bitmap_start;
    fs->inode_bitmap_start = sb->inode_bitmap_start;
    fs->inode_table_start  = sb->inode_table_start;
    fs->data_start         = sb->data_start;
    fs->root_ino           = sb->root_inode;
    fs->journal_start      = sb->journal_start;
    fs->journal_blocks     = sb->journal_blocks;
    fs->mounted = 1;

    /* Recover a transaction interrupted by a crash, then reload the
     * superblock counts (replay may have rewritten block 0). */
    journal_recover(fs);
    if (raw_read_block(fs, 0, sb_buf) == 0) {
        fs->free_blocks = ((cfs_super_t*)sb_buf)->free_blocks;
        fs->free_inodes = ((cfs_super_t*)sb_buf)->free_inodes;
    }
    return 0;
}

int cfs_format(cfs_bdev_t* bd, uint64_t total_blocks,
               uint64_t journal_blocks, const char* label) {
    if (total_blocks < 64) return -1;

    uint64_t total_inodes = total_blocks / 8;
    if (total_inodes < 64) total_inodes = 64;

    uint64_t bbm_bytes = (total_blocks + 7) / 8;
    uint64_t bbm_blocks = (bbm_bytes + BS - 1) / BS;
    uint64_t ibm_bytes = (total_inodes + 7) / 8;
    uint64_t ibm_blocks = (ibm_bytes + BS - 1) / BS;
    uint64_t it_bytes = total_inodes * CFS_INODE_SIZE;
    uint64_t it_blocks = (it_bytes + BS - 1) / BS;

    uint64_t bbm_start = 1 + journal_blocks;
    uint64_t ibm_start = bbm_start + bbm_blocks;
    uint64_t it_start  = ibm_start + ibm_blocks;
    uint64_t data_start = it_start + it_blocks;
    if (data_start + 1 > total_blocks) return -1;   /* need a root data block */

    cfs_fs_t fs;
    memset(&fs, 0, sizeof fs);
    fs.bd = bd; fs.spb = CFS_SPB; fs.block_size = BS; fs.inode_size = CFS_INODE_SIZE;
    fs.total_blocks = total_blocks; fs.total_inodes = total_inodes;
    fs.block_bitmap_start = bbm_start; fs.inode_bitmap_start = ibm_start;
    fs.inode_table_start = it_start; fs.data_start = data_start;
    fs.root_ino = CFS_ROOT_INO;
    fs.journal_start = journal_blocks ? 1 : 0;
    fs.journal_blocks = journal_blocks;

    /* Zero every metadata region (sb, journal, bitmaps, inode table). */
    memset(io_buf, 0, BS);
    for (uint64_t b = 0; b < data_start; b++)
        if (bd_write_block(&fs, b, io_buf) < 0) return -1;

    /* Block bitmap: mark metadata blocks [0,data_start), the root's data
     * block, and every bit past total_blocks, as used. */
    uint64_t root_data = data_start;             /* first data block */
    for (uint64_t b = 0; b <= data_start; b++)   /* through the root block */
        if (bit_set(&fs, bbm_start, b, 1) < 0) return -1;
    for (uint64_t b = total_blocks; b < bbm_blocks * BS * 8; b++)
        if (bit_set(&fs, bbm_start, b, 1) < 0) return -1;

    /* Inode bitmap: inode 0 (reserved) and inode 1 (root) used, plus the
     * tail past total_inodes. */
    if (bit_set(&fs, ibm_start, 0, 1) < 0) return -1;
    if (bit_set(&fs, ibm_start, 1, 1) < 0) return -1;
    for (uint64_t n = total_inodes; n < ibm_blocks * BS * 8; n++)
        if (bit_set(&fs, ibm_start, n, 1) < 0) return -1;

    /* Root directory inode. */
    cfs_dinode_t root;
    memset(&root, 0, sizeof root);
    root.mode = CFS_S_IFDIR | 0755;
    root.links = 2;                               /* "." and ".." */
    uint64_t now = cfs_now_ns();
    root.atime_ns = root.mtime_ns = root.ctime_ns = root.crtime_ns = now;
    root.size = BS;
    root.nextents = 1;
    root.extent[0].start = root_data;
    root.extent[0].count = 1;
    if (write_dinode(&fs, CFS_ROOT_INO, &root) < 0) return -1;

    dir_init_block(dir_buf, CFS_ROOT_INO, CFS_ROOT_INO);
    if (bd_write_block(&fs, root_data, dir_buf) < 0) return -1;

    /* Superblock. */
    memset(sb_buf, 0, BS);
    cfs_super_t* sb = (cfs_super_t*)sb_buf;
    sb->magic = CFS_MAGIC; sb->version = CFS_VERSION;
    sb->block_size = BS; sb->inode_size = CFS_INODE_SIZE;
    sb->total_blocks = total_blocks; sb->total_inodes = total_inodes;
    sb->free_blocks = total_blocks - (data_start + 1);
    sb->free_inodes = total_inodes - 2;
    sb->block_bitmap_start = bbm_start; sb->block_bitmap_blocks = bbm_blocks;
    sb->inode_bitmap_start = ibm_start; sb->inode_bitmap_blocks = ibm_blocks;
    sb->inode_table_start = it_start;   sb->inode_table_blocks = it_blocks;
    sb->data_start = data_start; sb->root_inode = CFS_ROOT_INO;
    sb->journal_start = journal_blocks ? 1 : 0;
    sb->journal_blocks = journal_blocks;
    sb->state = CFS_STATE_CLEAN;
    sb->mkfs_time = (uint32_t)(now / 1000000000ull);
    if (label) {
        uint32_t i = 0;
        for (; label[i] && i < sizeof(sb->label) - 1; i++) sb->label[i] = label[i];
        sb->label[i] = '\0';
    }
    if (bd_write_block(&fs, 0, sb_buf) < 0) return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public path operations                                              */
/* ------------------------------------------------------------------ */

int cfs_is_dir(cfs_fs_t* fs, const char* path) {
    uint64_t ino = resolve_ino(fs, path);
    if (ino == 0) return 0;
    cfs_dinode_t din;
    if (read_dinode(fs, ino, &din) < 0) return 0;
    return CFS_ISDIR(din.mode) ? 1 : 0;
}

int cfs_stat(cfs_fs_t* fs, const char* path, cfs_stat_t* out) {
    uint64_t ino = resolve_ino(fs, path);
    if (ino == 0) return -1;
    cfs_dinode_t din;
    if (read_dinode(fs, ino, &din) < 0) return -1;
    out->mode  = din.mode;
    out->uid   = din.uid;
    out->gid   = din.gid;
    out->size  = din.size;
    out->flags = din.flags;
    return 0;
}

int cfs_list(cfs_fs_t* fs, const char* path, vfs_dirent_t* out, int max) {
    uint64_t ino = resolve_ino(fs, path);
    if (ino == 0) return -1;
    cfs_dinode_t din;
    if (read_dinode(fs, ino, &din) < 0) return -1;
    if (!CFS_ISDIR(din.mode)) return -1;
    if (!may_access(din.mode, din.uid, din.gid, CFS_R)) return -1;

    int n = 0;
    uint64_t nblk = din.size / BS;
    for (uint64_t fb = 0; fb < nblk && n < max; fb++) {
        uint64_t pb = map_fileblock(&din, fb);
        if (pb == 0) continue;
        if (bd_read_block(fs, pb, dir_buf) < 0) return -1;
        uint32_t off = 0;
        while (off + CFS_DIRENT_HDR <= BS && n < max) {
            cfs_dirent_t* de = (cfs_dirent_t*)(dir_buf + off);
            if (de->rec_len < CFS_DIRENT_HDR || off + de->rec_len > BS) break;
            if (de->inode != 0) {
                const char* nm = (const char*)(dir_buf + off + CFS_DIRENT_HDR);
                int dot  = (de->name_len == 1 && nm[0] == '.');
                int ddot = (de->name_len == 2 && nm[0] == '.' && nm[1] == '.');
                if (!dot && !ddot) {
                    uint32_t k = de->name_len;
                    if (k > sizeof(out[n].name) - 1) k = sizeof(out[n].name) - 1;
                    memcpy(out[n].name, nm, k);
                    out[n].name[k] = '\0';
                    cfs_dinode_t cd;
                    out[n].size = 0; out[n].attr = 0; out[n].first_cluster = 0;
                    if (read_dinode(fs, de->inode, &cd) == 0) {
                        out[n].size = (unsigned int)cd.size;
                        if (CFS_ISDIR(cd.mode)) out[n].attr = 0x10;
                    }
                    n++;
                }
            }
            off += de->rec_len;
        }
    }
    return n;
}

int cfs_read_at(cfs_fs_t* fs, const char* path, void* buf,
                uint32_t offset, uint32_t len) {
    uint64_t ino = resolve_ino(fs, path);
    if (ino == 0) return -1;
    cfs_dinode_t din;
    if (read_dinode(fs, ino, &din) < 0) return -1;

    /* A symlink reads back as the VFS magic followed by its target. */
    if (CFS_ISLNK(din.mode)) {
        uint32_t tlen = (uint32_t)din.size;
        const unsigned char* tgt;
        if (din.nextents == 0) {
            tgt = (const unsigned char*)din.extent;      /* fast symlink */
        } else {
            if (raw_read_block(fs, din.extent[0].start, io_buf) < 0) return -1;
            tgt = io_buf;                                 /* slow symlink */
        }
        uint32_t total = VFS_SYMLINK_MAGIC_LEN + tlen;
        if (offset >= total) return 0;
        if (len > total - offset) len = total - offset;
        unsigned char* o = (unsigned char*)buf;
        for (uint32_t i = 0; i < len; i++) {
            uint32_t pos = offset + i;
            o[i] = (pos < VFS_SYMLINK_MAGIC_LEN)
                 ? (unsigned char)VFS_SYMLINK_MAGIC[pos]
                 : tgt[pos - VFS_SYMLINK_MAGIC_LEN];
        }
        return (int)len;
    }

    if (CFS_ISDIR(din.mode)) return -1;
    if (!may_access(din.mode, din.uid, din.gid, CFS_R)) return -1;
    if (offset >= din.size) return 0;
    uint64_t avail = din.size - offset;
    if (len > avail) len = (uint32_t)avail;

    unsigned char* out = (unsigned char*)buf;
    uint32_t done = 0;
    while (done < len) {
        uint64_t pos = offset + done;
        uint64_t fb = pos / BS;
        uint32_t within = (uint32_t)(pos % BS);
        uint32_t chunk = BS - within;
        if (chunk > len - done) chunk = len - done;
        uint64_t pb = map_fileblock(&din, fb);
        if (pb == 0) {
            memset(out + done, 0, chunk);            /* hole */
        } else {
            if (raw_read_block(fs, pb, io_buf) < 0) return -1;   /* file data */
            memcpy(out + done, io_buf + within, chunk);
        }
        done += chunk;
    }
    return (int)done;
}

int cfs_read_file(cfs_fs_t* fs, const char* path, void* buf, uint32_t max) {
    return cfs_read_at(fs, path, buf, 0, max);
}

static int do_write_file(cfs_fs_t* fs, const char* path, const void* data, uint32_t size) {
    uint64_t parent; char leaf[256]; uint32_t nlen;
    if (resolve_parent(fs, path, &parent, leaf, &nlen) < 0) return -1;

    cfs_dinode_t pdin;
    if (read_dinode(fs, parent, &pdin) < 0) return -1;

    /* A write carrying the VFS symlink magic creates a native symlink. */
    int is_link = (size >= VFS_SYMLINK_MAGIC_LEN &&
                   memcmp(data, VFS_SYMLINK_MAGIC, VFS_SYMLINK_MAGIC_LEN) == 0);
    const unsigned char* target = (const unsigned char*)data + VFS_SYMLINK_MAGIC_LEN;
    uint32_t tlen = is_link ? size - VFS_SYMLINK_MAGIC_LEN : 0;
    if (is_link && tlen > BS - 1) return -1;          /* target too long */

    uint16_t mode  = is_link ? (CFS_S_IFLNK | 0777) : (CFS_S_IFREG | 0644);
    uint8_t  ftype = is_link ? CFS_FT_LNK : CFS_FT_REG;

    uint64_t ino = dir_lookup(fs, &pdin, leaf, nlen);
    cfs_dinode_t din;
    uint64_t now = cfs_now_ns();

    if (ino != 0) {
        if (read_dinode(fs, ino, &din) < 0) return -1;
        if (CFS_ISDIR(din.mode)) return -1;          /* path is a directory */
        if (!may_access(din.mode, din.uid, din.gid, CFS_W)) return -1;
        inode_free_data(fs, &din);                   /* truncate to empty */
        din.mode = mode;                             /* reg<->symlink on rewrite */
    } else {
        if (!may_access(pdin.mode, pdin.uid, pdin.gid, CFS_W | CFS_X)) return -1;
        ino = alloc_inode(fs);
        if (ino == 0) return -1;
        memset(&din, 0, sizeof din);
        din.mode = mode;
        din.uid = cfs_caller_uid();                  /* new file owned by creator */
        din.gid = cfs_caller_gid();
        din.links = 1;
        din.crtime_ns = now;
        if (dir_add(fs, &pdin, leaf, nlen, ino, ftype) < 0) {
            free_inode(fs, ino);
            return -1;
        }
        if (write_dinode(fs, parent, &pdin) < 0) return -1;
    }

    if (is_link) {
        if (tlen <= sizeof din.extent) {
            memset(din.extent, 0, sizeof din.extent);
            memcpy((unsigned char*)din.extent, target, tlen);   /* fast symlink */
            din.nextents = 0;
        } else {
            uint64_t b = alloc_block(fs);
            if (b == 0) return -1;
            memset(io_buf, 0, BS);
            memcpy(io_buf, target, tlen);
            if (raw_write_block(fs, b, io_buf) < 0) return -1;   /* slow symlink */
            din.nextents = 1; din.extent[0].start = b; din.extent[0].count = 1;
        }
        din.size = tlen;
        din.mtime_ns = din.ctime_ns = now;
        if (din.atime_ns == 0) din.atime_ns = now;
        if (write_dinode(fs, ino, &din) < 0) return -1;
        sync_super(fs);
        return (int)size;
    }

    const unsigned char* src = (const unsigned char*)data;
    uint64_t nblk = (size + BS - 1) / BS;
    for (uint64_t fb = 0; fb < nblk; fb++) {
        uint64_t b = alloc_block(fs);
        if (b == 0) return -1;                        /* out of space */
        uint32_t off = (uint32_t)(fb * BS);
        uint32_t chunk = (size - off < BS) ? (size - off) : BS;
        memset(io_buf, 0, BS);
        memcpy(io_buf, src + off, chunk);
        if (raw_write_block(fs, b, io_buf) < 0) return -1;   /* data: ordered */
        if (inode_push_extent(&din, b, 1) < 0) { return -1; }
    }

    din.size = size;
    din.mtime_ns = din.ctime_ns = now;
    if (din.atime_ns == 0) din.atime_ns = now;
    if (write_dinode(fs, ino, &din) < 0) return -1;
    sync_super(fs);
    return (int)size;
}

static int do_mkdir(cfs_fs_t* fs, const char* path) {
    uint64_t parent; char leaf[256]; uint32_t nlen;
    if (resolve_parent(fs, path, &parent, leaf, &nlen) < 0) return -1;

    cfs_dinode_t pdin;
    if (read_dinode(fs, parent, &pdin) < 0) return -1;
    if (!may_access(pdin.mode, pdin.uid, pdin.gid, CFS_W | CFS_X)) return -1;
    if (dir_lookup(fs, &pdin, leaf, nlen) != 0) return -1;   /* exists */

    uint64_t ino = alloc_inode(fs);
    if (ino == 0) return -1;
    uint64_t b = alloc_block(fs);
    if (b == 0) { free_inode(fs, ino); return -1; }

    dir_init_block(dir_buf, ino, parent);
    if (raw_write_block(fs, b, dir_buf) < 0) return -1;   /* new block: ordered */

    cfs_dinode_t din;
    memset(&din, 0, sizeof din);
    uint64_t now = cfs_now_ns();
    din.mode = CFS_S_IFDIR | 0755;
    din.uid = cfs_caller_uid();                           /* owned by creator */
    din.gid = cfs_caller_gid();
    din.links = 2;
    din.atime_ns = din.mtime_ns = din.ctime_ns = din.crtime_ns = now;
    din.size = BS;
    din.nextents = 1; din.extent[0].start = b; din.extent[0].count = 1;
    if (write_dinode(fs, ino, &din) < 0) return -1;

    if (dir_add(fs, &pdin, leaf, nlen, ino, CFS_FT_DIR) < 0) {
        free_block(fs, b); free_inode(fs, ino);
        return -1;
    }
    pdin.links++;                                    /* child's ".." */
    pdin.mtime_ns = pdin.ctime_ns = now;
    if (write_dinode(fs, parent, &pdin) < 0) return -1;
    sync_super(fs);
    return 0;
}

static int do_rmdir(cfs_fs_t* fs, const char* path) {
    uint64_t parent; char leaf[256]; uint32_t nlen;
    if (resolve_parent(fs, path, &parent, leaf, &nlen) < 0) return -1;

    cfs_dinode_t pdin;
    if (read_dinode(fs, parent, &pdin) < 0) return -1;
    if (!may_access(pdin.mode, pdin.uid, pdin.gid, CFS_W | CFS_X)) return -1;
    uint64_t ino = dir_lookup(fs, &pdin, leaf, nlen);
    if (ino == 0) return -1;

    cfs_dinode_t din;
    if (read_dinode(fs, ino, &din) < 0) return -1;
    if (!CFS_ISDIR(din.mode)) return -1;
    if (!dir_is_empty(fs, &din)) return -1;

    if (dir_remove(fs, &pdin, leaf, nlen) < 0) return -1;
    if (pdin.links > 0) pdin.links--;                /* lost child's ".." */
    pdin.mtime_ns = pdin.ctime_ns = cfs_now_ns();
    if (write_dinode(fs, parent, &pdin) < 0) return -1;

    inode_free_data(fs, &din);
    free_inode(fs, ino);
    sync_super(fs);
    return 0;
}

static int do_unlink(cfs_fs_t* fs, const char* path) {
    uint64_t parent; char leaf[256]; uint32_t nlen;
    if (resolve_parent(fs, path, &parent, leaf, &nlen) < 0) return -1;

    cfs_dinode_t pdin;
    if (read_dinode(fs, parent, &pdin) < 0) return -1;
    if (!may_access(pdin.mode, pdin.uid, pdin.gid, CFS_W | CFS_X)) return -1;
    uint64_t ino = dir_lookup(fs, &pdin, leaf, nlen);
    if (ino == 0) return -1;

    cfs_dinode_t din;
    if (read_dinode(fs, ino, &din) < 0) return -1;
    if (CFS_ISDIR(din.mode)) return -1;              /* use rmdir */

    if (dir_remove(fs, &pdin, leaf, nlen) < 0) return -1;
    pdin.mtime_ns = pdin.ctime_ns = cfs_now_ns();
    if (write_dinode(fs, parent, &pdin) < 0) return -1;

    if (din.links > 0) din.links--;
    if (din.links == 0) {
        inode_free_data(fs, &din);
        free_inode(fs, ino);
    } else {
        write_dinode(fs, ino, &din);
    }
    sync_super(fs);
    return 0;
}

/* Create a hard link `newpath` to the inode of `oldpath` (not a directory). */
static int do_link(cfs_fs_t* fs, const char* oldpath, const char* newpath) {
    uint64_t oino = resolve_ino(fs, oldpath);
    if (oino == 0) return -1;
    cfs_dinode_t od;
    if (read_dinode(fs, oino, &od) < 0) return -1;
    if (CFS_ISDIR(od.mode)) return -1;               /* no hard links to dirs */

    uint64_t parent; char leaf[256]; uint32_t nlen;
    if (resolve_parent(fs, newpath, &parent, leaf, &nlen) < 0) return -1;
    cfs_dinode_t pd;
    if (read_dinode(fs, parent, &pd) < 0) return -1;
    if (!may_access(pd.mode, pd.uid, pd.gid, CFS_W | CFS_X)) return -1;
    if (dir_lookup(fs, &pd, leaf, nlen) != 0) return -1;   /* new name exists */

    uint8_t ft = CFS_ISLNK(od.mode) ? CFS_FT_LNK : CFS_FT_REG;
    if (dir_add(fs, &pd, leaf, nlen, oino, ft) < 0) return -1;
    if (write_dinode(fs, parent, &pd) < 0) return -1;

    od.links++;
    od.ctime_ns = cfs_now_ns();
    if (write_dinode(fs, oino, &od) < 0) return -1;
    return 0;
}

/* Change permission bits (incl. setuid/setgid/sticky). Owner or root. */
static int do_chmod(cfs_fs_t* fs, const char* path, uint16_t newperm) {
    uint64_t ino = resolve_ino(fs, path);
    if (ino == 0) return -1;
    cfs_dinode_t d;
    if (read_dinode(fs, ino, &d) < 0) return -1;
    uint32_t uid = cfs_caller_uid();
    if (uid != 0 && uid != d.uid) return -1;          /* only owner or root */
    d.mode = (uint16_t)((d.mode & CFS_S_IFMT) | (newperm & 07777));
    d.ctime_ns = cfs_now_ns();
    return write_dinode(fs, ino, &d);
}

/* Change owner/group. Requires root. uid/gid == (uint32_t)-1 leaves it. */
static int do_chown(cfs_fs_t* fs, const char* path, uint32_t uid, uint32_t gid) {
    if (cfs_caller_uid() != 0) return -1;             /* chown needs root */
    uint64_t ino = resolve_ino(fs, path);
    if (ino == 0) return -1;
    cfs_dinode_t d;
    if (read_dinode(fs, ino, &d) < 0) return -1;
    if (uid != (uint32_t)-1) d.uid = uid;
    if (gid != (uint32_t)-1) d.gid = gid;
    d.ctime_ns = cfs_now_ns();
    return write_dinode(fs, ino, &d);
}

/* ------------------------------------------------------------------ */
/* Public mutating ops — each runs as one journaled transaction.       */
/* On success the transaction is committed atomically; on any failure  */
/* it is aborted, leaving the filesystem exactly as it was.            */
/* ------------------------------------------------------------------ */

int cfs_write_file(cfs_fs_t* fs, const char* path, const void* data, uint32_t size) {
    txn_begin(fs);
    int rc = do_write_file(fs, path, data, size);
    if (rc < 0) { txn_abort(fs); return -1; }
    if (txn_commit(fs) < 0) return -1;
    return rc;
}
int cfs_mkdir(cfs_fs_t* fs, const char* path) {
    txn_begin(fs);
    int rc = do_mkdir(fs, path);
    if (rc < 0) { txn_abort(fs); return -1; }
    if (txn_commit(fs) < 0) return -1;
    return rc;
}
int cfs_rmdir(cfs_fs_t* fs, const char* path) {
    txn_begin(fs);
    int rc = do_rmdir(fs, path);
    if (rc < 0) { txn_abort(fs); return -1; }
    if (txn_commit(fs) < 0) return -1;
    return rc;
}
int cfs_unlink(cfs_fs_t* fs, const char* path) {
    txn_begin(fs);
    int rc = do_unlink(fs, path);
    if (rc < 0) { txn_abort(fs); return -1; }
    if (txn_commit(fs) < 0) return -1;
    return rc;
}
int cfs_link(cfs_fs_t* fs, const char* oldpath, const char* newpath) {
    txn_begin(fs);
    int rc = do_link(fs, oldpath, newpath);
    if (rc < 0) { txn_abort(fs); return -1; }
    if (txn_commit(fs) < 0) return -1;
    return rc;
}
int cfs_chmod(cfs_fs_t* fs, const char* path, uint16_t perm) {
    txn_begin(fs);
    int rc = do_chmod(fs, path, perm);
    if (rc < 0) { txn_abort(fs); return -1; }
    if (txn_commit(fs) < 0) return -1;
    return rc;
}
int cfs_chown(cfs_fs_t* fs, const char* path, uint32_t uid, uint32_t gid) {
    txn_begin(fs);
    int rc = do_chown(fs, path, uid, gid);
    if (rc < 0) { txn_abort(fs); return -1; }
    if (txn_commit(fs) < 0) return -1;
    return rc;
}

/* ------------------------------------------------------------------ */
/* fsck — offline consistency check and repair                         */
/* ------------------------------------------------------------------ */
/*
 * Rebuilds, from the directory tree alone, what the block and inode bitmaps
 * and link counts *should* be, then compares that against what is on disk.
 * It reaches everything from the root, counts every directory reference to
 * each inode (which equals that inode's correct link count), and records
 * which blocks each reachable inode's extents cover. Bounded static working
 * sets keep it allocation-free; volumes beyond the caps report sb_bad.
 */

#define CFS_FSCK_MAX_BLOCKS 65536u     /* up to 256 MiB at 4 KiB blocks */
#define CFS_FSCK_MAX_INODES 16384u
#define CFS_FSCK_MAX_STACK  2048

static unsigned char fsck_bbm[CFS_FSCK_MAX_BLOCKS / 8];   /* computed blocks */
static unsigned char fsck_ibm[CFS_FSCK_MAX_INODES / 8];   /* reachable inodes */
static uint16_t      fsck_refs[CFS_FSCK_MAX_INODES];      /* dir refs per inode */
static uint64_t      fsck_stack[CFS_FSCK_MAX_STACK];      /* dirs to visit */

#define FSCK_GET(bm, i) (((bm)[(i) >> 3] >> ((i) & 7)) & 1u)
#define FSCK_SET(bm, i) ((bm)[(i) >> 3] |= (unsigned char)(1u << ((i) & 7)))

/* Record the data blocks an inode's extents cover; flag bad/duplicate ones. */
static void fsck_mark_blocks(cfs_fs_t* fs, const cfs_dinode_t* in,
                             cfs_fsck_result_t* out) {
    for (uint32_t e = 0; e < in->nextents; e++) {
        if (in->extent[e].start == 0) continue;            /* hole */
        for (uint32_t k = 0; k < in->extent[e].count; k++) {
            uint64_t b = in->extent[e].start + k;
            if (b < fs->data_start || b >= fs->total_blocks) { out->block_badref++; continue; }
            if (FSCK_GET(fsck_bbm, b)) out->block_dup++;
            else FSCK_SET(fsck_bbm, b);
        }
    }
}

int cfs_fsck(cfs_fs_t* fs, int repair, cfs_fsck_result_t* out) {
    memset(out, 0, sizeof *out);
    if (fs->total_blocks > CFS_FSCK_MAX_BLOCKS ||
        fs->total_inodes > CFS_FSCK_MAX_INODES) {
        out->sb_bad = 1; out->total = 1; return -1;
    }
    memset(fsck_bbm, 0, sizeof fsck_bbm);
    memset(fsck_ibm, 0, sizeof fsck_ibm);
    memset(fsck_refs, 0, sizeof fsck_refs);

    /* Metadata region is always in use. */
    for (uint64_t b = 0; b < fs->data_start; b++) FSCK_SET(fsck_bbm, b);

    /* Walk the directory tree: mark reachable inodes + their blocks, and
     * count every directory reference (== the inode's correct link count). */
    int sp = 0;
    fsck_stack[sp++] = fs->root_ino;
    FSCK_SET(fsck_ibm, fs->root_ino);

    while (sp > 0) {
        uint64_t dno = fsck_stack[--sp];
        cfs_dinode_t d;
        if (read_dinode(fs, dno, &d) < 0) continue;
        fsck_mark_blocks(fs, &d, out);

        uint64_t nblk = d.size / BS;
        for (uint64_t fb = 0; fb < nblk; fb++) {
            uint64_t pb = map_fileblock(&d, fb);
            if (pb == 0) continue;
            if (raw_read_block(fs, pb, dir_buf) < 0) continue;
            uint32_t off = 0;
            while (off + CFS_DIRENT_HDR <= BS) {
                cfs_dirent_t* de = (cfs_dirent_t*)(dir_buf + off);
                if (de->rec_len < CFS_DIRENT_HDR || off + de->rec_len > BS) break;
                if (de->inode != 0) {
                    uint64_t c = de->inode;
                    if (c < CFS_FSCK_MAX_INODES) fsck_refs[c]++;
                    const char* nm = (const char*)(dir_buf + off + CFS_DIRENT_HDR);
                    int dot  = (de->name_len == 1 && nm[0] == '.');
                    int ddot = (de->name_len == 2 && nm[0] == '.' && nm[1] == '.');
                    if (!dot && !ddot) {
                        if (c >= fs->total_inodes) {
                            out->inode_badref++;
                        } else if (!FSCK_GET(fsck_ibm, c)) {
                            FSCK_SET(fsck_ibm, c);
                            cfs_dinode_t cd;
                            if (read_dinode(fs, c, &cd) == 0) {
                                if (CFS_ISDIR(cd.mode)) {
                                    if (sp < CFS_FSCK_MAX_STACK) fsck_stack[sp++] = c;
                                    else out->sb_bad++;   /* tree too deep/wide */
                                } else {
                                    fsck_mark_blocks(fs, &cd, out);
                                }
                            }
                        }
                    }
                }
                off += de->rec_len;
            }
        }
    }

    /* Compare block bitmap, correcting it when repairing; count used blocks. */
    uint64_t used_blocks = 0;
    uint64_t bbm_blocks = (fs->total_blocks + BS * 8 - 1) / (BS * 8);
    for (uint64_t bb = 0; bb < bbm_blocks; bb++) {
        if (raw_read_block(fs, fs->block_bitmap_start + bb, io_buf) < 0) break;
        int dirty = 0;
        uint64_t base = bb * BS * 8;
        for (uint32_t i = 0; i < BS * 8; i++) {
            uint64_t b = base + i;
            int disk = (io_buf[i >> 3] >> (i & 7)) & 1;
            int want = (b < fs->total_blocks) ? (int)FSCK_GET(fsck_bbm, b) : 1;
            if (want) used_blocks++;
            if (b < fs->total_blocks) {
                if (want && !disk) out->block_badref++;
                else if (!want && disk) out->block_leaks++;
            }
            if (repair && want != disk) {
                if (want) io_buf[i >> 3] |= (unsigned char)(1u << (i & 7));
                else      io_buf[i >> 3] &= (unsigned char)~(1u << (i & 7));
                dirty = 1;
            }
        }
        if (repair && dirty) raw_write_block(fs, fs->block_bitmap_start + bb, io_buf);
    }
    used_blocks = 0;                                   /* recount within volume */
    for (uint64_t b = 0; b < fs->total_blocks; b++)
        if (b < fs->data_start || FSCK_GET(fsck_bbm, b)) used_blocks++;

    /* Compare inode bitmap; count used inodes. */
    uint64_t used_inodes = 0;
    uint64_t ibm_blocks = (fs->total_inodes + BS * 8 - 1) / (BS * 8);
    for (uint64_t bb = 0; bb < ibm_blocks; bb++) {
        if (raw_read_block(fs, fs->inode_bitmap_start + bb, io_buf) < 0) break;
        int dirty = 0;
        uint64_t base = bb * BS * 8;
        for (uint32_t i = 0; i < BS * 8; i++) {
            uint64_t n = base + i;
            int disk = (io_buf[i >> 3] >> (i & 7)) & 1;
            int want = (n == 0) ? 1
                     : (n < fs->total_inodes) ? (int)FSCK_GET(fsck_ibm, n) : 1;
            if (want) used_inodes++;
            if (n < fs->total_inodes && n > 0) {
                if (want && !disk) out->inode_badref++;
                else if (!want && disk) out->inode_leaks++;    /* orphan */
            }
            if (repair && want != disk) {
                if (want) io_buf[i >> 3] |= (unsigned char)(1u << (i & 7));
                else      io_buf[i >> 3] &= (unsigned char)~(1u << (i & 7));
                dirty = 1;
            }
        }
        if (repair && dirty) raw_write_block(fs, fs->inode_bitmap_start + bb, io_buf);
    }
    used_inodes = 0;                                   /* recount within volume */
    for (uint64_t n = 0; n < fs->total_inodes; n++)
        if (n == 0 || FSCK_GET(fsck_ibm, n)) used_inodes++;

    /* Link counts of reachable inodes. */
    for (uint64_t n = 1; n < fs->total_inodes; n++) {
        if (!FSCK_GET(fsck_ibm, n)) continue;
        cfs_dinode_t d;
        if (read_dinode(fs, n, &d) < 0) continue;
        if (d.links != fsck_refs[n]) {
            out->linkcount_bad++;
            if (repair) { d.links = fsck_refs[n]; write_dinode(fs, n, &d); }
        }
    }

    /* Free counts. */
    uint64_t want_fb = fs->total_blocks - used_blocks;
    uint64_t want_fi = fs->total_inodes - used_inodes;
    if (raw_read_block(fs, 0, sb_buf) == 0) {
        cfs_super_t* sb = (cfs_super_t*)sb_buf;
        if (sb->free_blocks != want_fb || sb->free_inodes != want_fi) {
            out->freecount_bad++;
            if (repair) {
                sb->free_blocks = want_fb; sb->free_inodes = want_fi;
                sb->state = CFS_STATE_CLEAN;
                raw_write_block(fs, 0, sb_buf);
                fs->free_blocks = want_fb; fs->free_inodes = want_fi;
            }
        }
    }

    out->total = out->sb_bad + out->block_badref + out->block_leaks +
                 out->block_dup + out->inode_badref + out->inode_leaks +
                 out->linkcount_bad + out->freecount_bad;
    return out->total;
}

/* ================================================================== */
/* Kernel binding — compiled only into the kernel (-DCFS_KERNEL).      */
/* ATA block device, GPT partition discovery, RTC time source, and the */
/* vfs_ops table that plugs CFS into the VFS at a mount point.          */
/* ================================================================== */
#ifdef CFS_KERNEL

#include "ata.h"
#include "rtc.h"
#include "printf.h"
#include "console_backend.h"
#include "cred.h"

/* Permission checks use the system's current credential. */
uint32_t cfs_caller_uid(void) { return cred_uid(); }
uint32_t cfs_caller_gid(void) { return cred_gid(); }

/* --- time source: RTC wall-clock -> Unix nanoseconds --------------- */
static int64_t cfs_days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= (m <= 2);
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
    unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + (int64_t)doe - 719468;
}
uint64_t cfs_now_ns(void) {
    rtc_time_t t;
    rtc_read(&t);
    int64_t days = cfs_days_from_civil(t.year, t.month, t.day);
    int64_t secs = days * 86400 + t.hour * 3600 + t.minute * 60 + t.second;
    if (secs < 0) secs = 0;
    return (uint64_t)secs * 1000000000ull;
}

/* --- ATA-backed block device --------------------------------------- */
static int ata_bd_read(void* ctx, uint64_t sector, uint32_t count, void* buf) {
    (void)ctx; return ata_read(sector, count, buf);
}
static int ata_bd_write(void* ctx, uint64_t sector, uint32_t count, const void* buf) {
    (void)ctx; return ata_write(sector, count, buf);
}
static cfs_bdev_t g_bd = { ata_bd_read, ata_bd_write, 0, 0 };
static cfs_fs_t   g_cfs;
static int        g_cfs_ok = 0;

/* --- GPT partition discovery (first-LBA of each partition) --------- */
static uint32_t rd32(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static int gpt_partitions(unsigned int* out, int max) {
    unsigned char hdr[512];
    if (ata_read(1, 1, hdr) < 0) return 0;
    if (memcmp(hdr, "EFI PART", 8) != 0) return 0;
    uint32_t entries_lba = rd32(hdr + 72);
    uint32_t num_entries = rd32(hdr + 80);
    uint32_t entry_size  = rd32(hdr + 84);
    if (entry_size < 128 || entry_size > 512 || (512 % entry_size) != 0) return 0;
    if (num_entries > 256) num_entries = 256;

    int n = 0;
    uint32_t per_sector = 512 / entry_size;
    unsigned char sec[512];
    for (uint32_t i = 0; i < num_entries && n < max; i++) {
        if (i % per_sector == 0 &&
            ata_read(entries_lba + i / per_sector, 1, sec) < 0) break;
        unsigned char* e = sec + (i % per_sector) * entry_size;
        int empty = 1;
        for (int b = 0; b < 16; b++) if (e[b]) { empty = 0; break; }
        if (empty) continue;
        uint32_t start = rd32(e + 32);          /* first LBA (low 32 bits) */
        if (start) out[n++] = start;
    }
    return n;
}

int cfs_mount(void) {
    if (g_cfs_ok) return 0;
    unsigned int cands[16];
    int nc = gpt_partitions(cands, 16);
    if (nc == 0) cands[nc++] = 0;               /* no GPT: try whole disk */
    for (int i = 0; i < nc; i++) {
        g_bd.base = cands[i];
        if (cfs_mount_bd(&g_bd, &g_cfs) == 0) { g_cfs_ok = 1; return 0; }
    }
    return -1;
}
int cfs_mounted(void) { return g_cfs_ok; }

void cfs_print_info(void) {
    if (!g_cfs_ok) { printf("cfs: not mounted\n"); return; }
    printf("label=%s blocks=%llu free=%llu inodes=%llu free=%llu",
           ((cfs_super_t*)sb_buf)->label,     /* sb_buf holds the last sb read */
           (unsigned long long)g_cfs.total_blocks,
           (unsigned long long)g_cfs.free_blocks,
           (unsigned long long)g_cfs.total_inodes,
           (unsigned long long)g_cfs.free_inodes);
    printf("\n");
}

void cfs_fsck_print(int repair) {
    if (!g_cfs_ok) { printf("fsck: CFS not mounted\n"); return; }
    cfs_fsck_result_t r;
    int n = cfs_fsck(&g_cfs, repair, &r);
    if (n < 0) { printf("fsck: volume too large for the built-in checker\n"); return; }
    if (n == 0) { printf("fsck: /cfs is clean\n"); return; }
    printf("fsck: %d problem%s%s:\n", n, n == 1 ? "" : "s",
           repair ? " (repaired)" : "");
    if (r.block_badref)  printf("  blocks referenced but free:   %d\n", r.block_badref);
    if (r.block_leaks)   printf("  blocks used but unreferenced: %d\n", r.block_leaks);
    if (r.block_dup)     printf("  blocks referenced twice:      %d\n", r.block_dup);
    if (r.inode_badref)  printf("  inodes referenced but free:   %d\n", r.inode_badref);
    if (r.inode_leaks)   printf("  orphaned inodes:              %d\n", r.inode_leaks);
    if (r.linkcount_bad) printf("  wrong link counts:            %d\n", r.linkcount_bad);
    if (r.freecount_bad) printf("  free-count corrections:       %d\n", r.freecount_bad);
    if (!repair) printf("  run `fsck -r` to repair\n");
}

/* --- vfs_ops wrappers (single global volume) ----------------------- */
static unsigned char cp_buf[65536];

static int vop_list(const char* p, vfs_dirent_t* o, int m) { return cfs_list(&g_cfs, p, o, m); }
static int vop_is_dir(const char* p)                       { return cfs_is_dir(&g_cfs, p); }
static int vop_read(const char* p, void* b, unsigned int m){ return cfs_read_file(&g_cfs, p, b, m); }
static int vop_read_at(const char* p, void* b, unsigned int o, unsigned int l)
                                                           { return cfs_read_at(&g_cfs, p, b, o, l); }
static int vop_write(const char* p, const void* d, unsigned int s)
                                                           { return cfs_write_file(&g_cfs, p, d, s); }
static int vop_delete(const char* p)                       { return cfs_unlink(&g_cfs, p); }
static int vop_mkdir(const char* p)                        { return cfs_mkdir(&g_cfs, p); }
static int vop_rmdir(const char* p)                        { return cfs_rmdir(&g_cfs, p); }
static int vop_cp(const char* s, const char* d) {
    int n = cfs_read_file(&g_cfs, s, cp_buf, sizeof cp_buf);
    if (n < 0) return -1;
    return cfs_write_file(&g_cfs, d, cp_buf, (unsigned int)n) < 0 ? -1 : 0;
}
static int vop_mv(const char* s, const char* d) {
    if (vop_cp(s, d) < 0) return -1;
    return cfs_unlink(&g_cfs, s);
}
static int vop_link(const char* o, const char* n) { return cfs_link(&g_cfs, o, n); }
static int vop_chmod(const char* p, unsigned int mode) {
    return cfs_chmod(&g_cfs, p, (uint16_t)mode);
}
static int vop_chown(const char* p, unsigned int uid, unsigned int gid) {
    return cfs_chown(&g_cfs, p, uid, gid);
}

static const vfs_ops_t cfs_ops = {
    .list         = vop_list,
    .is_dir       = vop_is_dir,
    .read_file    = vop_read,
    .read_file_at = vop_read_at,
    .write_file   = vop_write,
    .delete_file  = vop_delete,
    .mkdir        = vop_mkdir,
    .rmdir        = vop_rmdir,
    .cp           = vop_cp,
    .mv           = vop_mv,
    .link         = vop_link,
    .chmod        = vop_chmod,
    .chown        = vop_chown,
};
const vfs_ops_t* cfs_vfs_ops(void) { return &cfs_ops; }

#endif /* CFS_KERNEL */