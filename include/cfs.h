#ifndef CFS_H
#define CFS_H

#include "stdint.h"
#include "vfs.h"

/*
 * CFS — Computer OS filesystem (Phase 1 / Month 1).
 *
 * An original, extent-based, 64-bit filesystem. Design goals: simpler than
 * ext2 where it can be (no block groups, no indirect-block chains — files
 * map to a small list of extents), but with the on-disk room for the later
 * phases reserved from day one so the format never has to change:
 *   - a journal region (Month 2),
 *   - uid/gid/mode permission fields (Month 4),
 *   - an xattr block pointer (Month 4),
 *   - a superblock/inode checksum slot (Month 5).
 *
 * Everything is little-endian and sized in fixed-width types, so the same
 * image is byte-identical whether written by the in-kernel driver or the
 * host mkfs tool. The driver and mkfs share one core (cfs.c); only the
 * block-device backend differs (ATA in the kernel, a file on the host).
 *
 * On-disk layout, in 4 KiB blocks:
 *
 *   block 0            superblock
 *   block 1..          journal           (reserved in Month 1, size in sb)
 *   ...                block bitmap       (1 bit per block, 0 = free)
 *   ...                inode bitmap       (1 bit per inode, 0 = free)
 *   ...                inode table        (inode_size bytes per inode)
 *   data_start..end    data blocks        (file + directory contents)
 *
 * Inode 0 is reserved (means "no inode"); inode 1 is the root directory.
 * A file's data is described by up to CFS_INLINE_EXTENTS extents stored in
 * the inode; each extent is a run of contiguous blocks (start, count). A
 * hole (sparse file) is simply a gap not covered by any extent. Directories
 * are files whose data is a sequence of variable-length records.
 */

#define CFS_MAGIC          0x31534643u   /* 'C','F','S','1' little-endian   */
#define CFS_VERSION        1
#define CFS_BLOCK_SIZE     4096u
#define CFS_SECTOR_SIZE    512u
#define CFS_SPB            (CFS_BLOCK_SIZE / CFS_SECTOR_SIZE)  /* 8 */
#define CFS_INODE_SIZE     256u
#define CFS_ROOT_INO       1u
#define CFS_NAME_MAX       255u
#define CFS_INLINE_EXTENTS 12

/* superblock state */
#define CFS_STATE_CLEAN    1u
#define CFS_STATE_DIRTY    0u

/* inode mode: type in the high bits, permission bits in the low 12 (POSIX) */
#define CFS_S_IFMT   0xF000u
#define CFS_S_IFREG  0x8000u
#define CFS_S_IFDIR  0x4000u
#define CFS_S_IFLNK  0xA000u
#define CFS_ISDIR(m) (((m) & CFS_S_IFMT) == CFS_S_IFDIR)
#define CFS_ISREG(m) (((m) & CFS_S_IFMT) == CFS_S_IFREG)
#define CFS_ISLNK(m) (((m) & CFS_S_IFMT) == CFS_S_IFLNK)

/* directory entry type hints */
#define CFS_FT_NONE  0
#define CFS_FT_REG   1
#define CFS_FT_DIR   2
#define CFS_FT_LNK   7

/* ------------------------------------------------------------------ */
/* On-disk structures (packed, little-endian)                          */
/* ------------------------------------------------------------------ */

typedef struct __attribute__((packed)) {
    uint64_t start;   /* first block of the run */
    uint32_t count;   /* number of blocks in the run */
} cfs_extent_t;       /* 12 bytes */

typedef struct __attribute__((packed)) {
    uint32_t magic;               /*   0  CFS_MAGIC */
    uint32_t version;             /*   4  on-disk format version */
    uint32_t block_size;          /*   8  bytes per block (4096) */
    uint32_t inode_size;          /*  12  bytes per inode (256) */
    uint64_t total_blocks;        /*  16  volume size in blocks */
    uint64_t total_inodes;        /*  24  inode count */
    uint64_t free_blocks;         /*  32 */
    uint64_t free_inodes;         /*  40 */
    uint64_t block_bitmap_start;  /*  48 */
    uint64_t block_bitmap_blocks; /*  56 */
    uint64_t inode_bitmap_start;  /*  64 */
    uint64_t inode_bitmap_blocks; /*  72 */
    uint64_t inode_table_start;   /*  80 */
    uint64_t inode_table_blocks;  /*  88 */
    uint64_t data_start;          /*  96  first data block */
    uint64_t root_inode;          /* 104  = 1 */
    uint64_t journal_start;       /* 112  reserved (Month 2); 0 if none */
    uint64_t journal_blocks;      /* 120  reserved */
    uint32_t state;               /* 128  clean/dirty */
    uint32_t flags;               /* 132  feature flags (reserved) */
    uint32_t mkfs_time;           /* 136  creation time (unix seconds) */
    uint32_t sb_csum;             /* 140  superblock checksum (Month 5; 0) */
    uint8_t  uuid[16];            /* 144 */
    char     label[32];           /* 160 */
    /* 192..4095 reserved (zero) */
} cfs_super_t;

typedef struct __attribute__((packed)) {
    uint16_t mode;                /*   0  type + permission bits */
    uint16_t links;               /*   2  hard link count */
    uint32_t uid;                 /*   4 */
    uint32_t gid;                 /*   8 */
    uint64_t size;                /*  12  logical size in bytes */
    uint64_t atime_ns;            /*  20  nanosecond timestamps */
    uint64_t mtime_ns;            /*  28 */
    uint64_t ctime_ns;            /*  36 */
    uint64_t crtime_ns;           /*  44  birth time */
    uint32_t flags;               /*  52  immutable/append/... (reserved) */
    uint32_t nextents;            /*  56  valid entries in extent[] */
    uint64_t xattr_block;         /*  60  xattr block (Month 4); 0 if none */
    uint64_t extent_index;        /*  68  overflow extent block (reserved) */
    cfs_extent_t extent[CFS_INLINE_EXTENTS]; /* 76.. (12 * 12 = 144) */
    /* 220..255 reserved (zero) */
} cfs_dinode_t;

typedef struct __attribute__((packed)) {
    uint64_t inode;    /* target inode; 0 marks a free/deleted slot */
    uint16_t rec_len;  /* total record length, multiple of 8 */
    uint8_t  name_len; /* bytes of name that follow */
    uint8_t  type;     /* CFS_FT_* hint */
    /* name_len bytes of name follow (not null-terminated), then padding */
} cfs_dirent_t;        /* 12-byte header */

#define CFS_DIRENT_HDR  12

/* ------------------------------------------------------------------ */
/* Block device backend                                                */
/* ------------------------------------------------------------------ */

/*
 * The core speaks in 512-byte sectors through this vtable. `base` is the
 * partition's first sector, added to every access, so the filesystem knows
 * nothing about where on the disk it lives. read/write return 0 on success.
 */
typedef struct cfs_bdev {
    int   (*read)(void* ctx, uint64_t sector, uint32_t count, void* buf);
    int   (*write)(void* ctx, uint64_t sector, uint32_t count, const void* buf);
    void*    ctx;
    uint64_t base;
} cfs_bdev_t;

/* In-memory mount state (one volume). Caller owns the storage. */
typedef struct cfs_fs {
    cfs_bdev_t* bd;
    int         mounted;
    uint32_t    block_size;
    uint32_t    spb;
    uint32_t    inode_size;
    uint64_t    total_blocks;
    uint64_t    total_inodes;
    uint64_t    free_blocks;
    uint64_t    free_inodes;
    uint64_t    block_bitmap_start;
    uint64_t    inode_bitmap_start;
    uint64_t    inode_table_start;
    uint64_t    data_start;
    uint64_t    root_ino;
} cfs_fs_t;

/* ------------------------------------------------------------------ */
/* Core API (explicit fs pointer; usable from kernel and host)         */
/* ------------------------------------------------------------------ */

/* Format `total_blocks` 4 KiB blocks on `bd` as an empty CFS volume with a
 * root directory. `journal_blocks` reserves the Month-2 journal region
 * (pass 0 for none). Returns 0 on success. */
int cfs_format(cfs_bdev_t* bd, uint64_t total_blocks,
               uint64_t journal_blocks, const char* label);

/* Read and validate the superblock into `fs`. Returns 0 on success. */
int cfs_mount_bd(cfs_bdev_t* bd, cfs_fs_t* fs);

/* Path operations. Paths are absolute within the volume ("/", "/a/b"). */
int cfs_list(cfs_fs_t* fs, const char* path, vfs_dirent_t* out, int max);
int cfs_is_dir(cfs_fs_t* fs, const char* path);
int cfs_read_file(cfs_fs_t* fs, const char* path, void* buf, uint32_t max);
int cfs_read_at(cfs_fs_t* fs, const char* path, void* buf,
                uint32_t offset, uint32_t len);
int cfs_write_file(cfs_fs_t* fs, const char* path, const void* data,
                   uint32_t size);
int cfs_mkdir(cfs_fs_t* fs, const char* path);
int cfs_rmdir(cfs_fs_t* fs, const char* path);
int cfs_unlink(cfs_fs_t* fs, const char* path);

/* ------------------------------------------------------------------ */
/* Kernel bindings (single global volume on the ATA disk)              */
/* ------------------------------------------------------------------ */

/* Probe the ATA disk for a CFS volume and mount it. Returns 0 on success. */
int cfs_mount(void);
int cfs_mounted(void);
void cfs_print_info(void);

/* Ops table to hand to vfs_mount(). */
const vfs_ops_t* cfs_vfs_ops(void);

#endif