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

static int bd_read_block(cfs_fs_t* fs, uint64_t block, void* buf) {
    uint64_t sec = fs->bd->base + block * fs->spb;
    return fs->bd->read(fs->bd->ctx, sec, fs->spb, buf);
}
static int bd_write_block(cfs_fs_t* fs, uint64_t block, const void* buf) {
    uint64_t sec = fs->bd->base + block * fs->spb;
    return fs->bd->write(fs->bd->ctx, sec, fs->spb, buf);
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
    if (bd_write_block(fs, b, dir_buf) < 0) return -1;
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
    fs->mounted = 1;
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

int cfs_list(cfs_fs_t* fs, const char* path, vfs_dirent_t* out, int max) {
    uint64_t ino = resolve_ino(fs, path);
    if (ino == 0) return -1;
    cfs_dinode_t din;
    if (read_dinode(fs, ino, &din) < 0) return -1;
    if (!CFS_ISDIR(din.mode)) return -1;

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
    if (CFS_ISDIR(din.mode)) return -1;
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
            if (bd_read_block(fs, pb, io_buf) < 0) return -1;
            memcpy(out + done, io_buf + within, chunk);
        }
        done += chunk;
    }
    return (int)done;
}

int cfs_read_file(cfs_fs_t* fs, const char* path, void* buf, uint32_t max) {
    return cfs_read_at(fs, path, buf, 0, max);
}

int cfs_write_file(cfs_fs_t* fs, const char* path, const void* data, uint32_t size) {
    uint64_t parent; char leaf[256]; uint32_t nlen;
    if (resolve_parent(fs, path, &parent, leaf, &nlen) < 0) return -1;

    cfs_dinode_t pdin;
    if (read_dinode(fs, parent, &pdin) < 0) return -1;

    uint64_t ino = dir_lookup(fs, &pdin, leaf, nlen);
    cfs_dinode_t din;
    uint64_t now = cfs_now_ns();

    if (ino != 0) {
        if (read_dinode(fs, ino, &din) < 0) return -1;
        if (CFS_ISDIR(din.mode)) return -1;          /* path is a directory */
        inode_free_data(fs, &din);                   /* truncate to empty */
    } else {
        ino = alloc_inode(fs);
        if (ino == 0) return -1;
        memset(&din, 0, sizeof din);
        din.mode = CFS_S_IFREG | 0644;
        din.links = 1;
        din.crtime_ns = now;
        if (dir_add(fs, &pdin, leaf, nlen, ino, CFS_FT_REG) < 0) {
            free_inode(fs, ino);
            return -1;
        }
        if (write_dinode(fs, parent, &pdin) < 0) return -1;
    }

    const unsigned char* src = (const unsigned char*)data;
    uint64_t nblk = (size + BS - 1) / BS;
    for (uint64_t fb = 0; fb < nblk; fb++) {
        uint64_t b = alloc_block(fs);
        if (b == 0) { sync_super(fs); return -1; }   /* out of space */
        uint32_t off = (uint32_t)(fb * BS);
        uint32_t chunk = (size - off < BS) ? (size - off) : BS;
        memset(io_buf, 0, BS);
        memcpy(io_buf, src + off, chunk);
        if (bd_write_block(fs, b, io_buf) < 0) return -1;
        if (inode_push_extent(&din, b, 1) < 0) { sync_super(fs); return -1; }
    }

    din.size = size;
    din.mtime_ns = din.ctime_ns = now;
    if (din.atime_ns == 0) din.atime_ns = now;
    if (write_dinode(fs, ino, &din) < 0) return -1;
    sync_super(fs);
    return (int)size;
}

int cfs_mkdir(cfs_fs_t* fs, const char* path) {
    uint64_t parent; char leaf[256]; uint32_t nlen;
    if (resolve_parent(fs, path, &parent, leaf, &nlen) < 0) return -1;

    cfs_dinode_t pdin;
    if (read_dinode(fs, parent, &pdin) < 0) return -1;
    if (dir_lookup(fs, &pdin, leaf, nlen) != 0) return -1;   /* exists */

    uint64_t ino = alloc_inode(fs);
    if (ino == 0) return -1;
    uint64_t b = alloc_block(fs);
    if (b == 0) { free_inode(fs, ino); return -1; }

    dir_init_block(dir_buf, ino, parent);
    if (bd_write_block(fs, b, dir_buf) < 0) return -1;

    cfs_dinode_t din;
    memset(&din, 0, sizeof din);
    uint64_t now = cfs_now_ns();
    din.mode = CFS_S_IFDIR | 0755;
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

int cfs_rmdir(cfs_fs_t* fs, const char* path) {
    uint64_t parent; char leaf[256]; uint32_t nlen;
    if (resolve_parent(fs, path, &parent, leaf, &nlen) < 0) return -1;

    cfs_dinode_t pdin;
    if (read_dinode(fs, parent, &pdin) < 0) return -1;
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

int cfs_unlink(cfs_fs_t* fs, const char* path) {
    uint64_t parent; char leaf[256]; uint32_t nlen;
    if (resolve_parent(fs, path, &parent, leaf, &nlen) < 0) return -1;

    cfs_dinode_t pdin;
    if (read_dinode(fs, parent, &pdin) < 0) return -1;
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
};
const vfs_ops_t* cfs_vfs_ops(void) { return &cfs_ops; }

#endif /* CFS_KERNEL */