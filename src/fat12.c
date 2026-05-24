#include "fat12.h"
#include "ata.h"
#include "string.h"
#include "kheap.h"
#include "printf.h"

// ============================================================
// On-disk structures
// ============================================================

typedef struct __attribute__((packed)) {
    unsigned char  jmp[3];
    char           oem[8];
    unsigned short bytes_per_sector;
    unsigned char  sectors_per_cluster;
    unsigned short reserved_sectors;
    unsigned char  num_fats;
    unsigned short root_entries;
    unsigned short total_sectors_16;
    unsigned char  media;
    unsigned short sectors_per_fat;
    unsigned short sectors_per_track;
    unsigned short num_heads;
    unsigned int   hidden_sectors;
    unsigned int   total_sectors_32;
} bpb_t;

typedef struct __attribute__((packed)) {
    char           name[8];
    char           ext[3];
    unsigned char  attr;
    unsigned char  reserved;
    unsigned char  ctime_tenth;
    unsigned short ctime;
    unsigned short cdate;
    unsigned short adate;
    unsigned short cluster_hi;
    unsigned short mtime;
    unsigned short mdate;
    unsigned short cluster_lo;
    unsigned int   size;
} dirent_t;

#define ATTR_READ_ONLY  0x01
#define ATTR_HIDDEN     0x02
#define ATTR_SYSTEM     0x04
#define ATTR_VOLUME_ID  0x08
#define ATTR_DIRECTORY  0x10
#define ATTR_ARCHIVE    0x20
#define ATTR_LFN        0x0F

// ============================================================
// Mount state
// ============================================================

static int            mounted = 0;
static bpb_t          bpb;
static unsigned char* fat = 0;
static unsigned int   fat_start;
static unsigned int   root_start;
static unsigned int   data_start;
static unsigned int   bytes_per_cluster;

static unsigned int   cwd_cluster = 0;   // 0 = root

// ============================================================
// Low-level helpers
// ============================================================

static unsigned int cluster_to_lba(unsigned int cluster) {
    return data_start + (cluster - 2) * bpb.sectors_per_cluster;
}

static unsigned int fat12_get_entry(unsigned int cluster) {
    unsigned int offset = cluster + (cluster / 2);
    unsigned short v = *(unsigned short*)(fat + offset);
    if (cluster & 1) return v >> 4;
    return v & 0x0FFF;
}

static void fat12_set_entry(unsigned int cluster, unsigned int value) {
    value &= 0x0FFF;
    unsigned int offset = cluster + (cluster / 2);
    if (cluster & 1) {
        fat[offset]     = (fat[offset] & 0x0F) | ((value & 0x00F) << 4);
        fat[offset + 1] = (value >> 4) & 0xFF;
    } else {
        fat[offset]     = value & 0xFF;
        fat[offset + 1] = (fat[offset + 1] & 0xF0) | ((value >> 8) & 0x0F);
    }
}

static int is_end_of_chain(unsigned int entry) {
    return entry >= 0x0FF8;
}

static char upper(char c) {
    if (c >= 'a' && c <= 'z') return c - 'a' + 'A';
    return c;
}

// "READMETXT  " -> "README.TXT" (null-terminated). Returns length.
static int format_83(const char* raw, char* out) {
    int n = 0;
    for (int i = 0; i < 8 && raw[i] != ' '; i++) out[n++] = raw[i];
    if (raw[8] != ' ') {
        out[n++] = '.';
        for (int i = 8; i < 11 && raw[i] != ' '; i++) out[n++] = raw[i];
    }
    out[n] = '\0';
    return n;
}

// "readme.txt" -> "README  TXT" (11 bytes). Returns 0 on success.
// Special-cases "." and ".." since they violate the no-leading-space rule
// otherwise, and we need them to round-trip on disk.
static int build_83(const char* name, char out[11]) {
    for (int i = 0; i < 11; i++) out[i] = ' ';

    // "."  -> ".          "
    // ".." -> "..         "
    if (name[0] == '.' && name[1] == '\0') {
        out[0] = '.';
        return 0;
    }
    if (name[0] == '.' && name[1] == '.' && name[2] == '\0') {
        out[0] = '.';
        out[1] = '.';
        return 0;
    }

    int i = 0;
    while (*name && *name != '.') {
        if (i >= 8) return -1;
        out[i++] = upper(*name++);
    }
    if (*name == '.') {
        name++;
        int j = 8;
        while (*name) {
            if (j >= 11) return -1;
            out[j++] = upper(*name++);
        }
    }
    if (out[0] == ' ') return -1;   // empty name
    return 0;
}

static int name_eq_83(const dirent_t* d, const char target[11]) {
    for (int j = 0; j < 8; j++) {
        if (upper(d->name[j]) != target[j]) return 0;
    }
    for (int j = 0; j < 3; j++) {
        if (upper(d->ext[j]) != target[8 + j]) return 0;
    }
    return 1;
}

// ============================================================
// Mount
// ============================================================

int fat12_mount(void) {
    if (mounted) return 0;

    unsigned char sector[512];
    if (ata_read(0, 1, sector) < 0) {
        printf("fat12: cannot read boot sector\n");
        return -1;
    }
    memcpy(&bpb, sector, sizeof(bpb));

    if (bpb.bytes_per_sector != 512 ||
        bpb.sectors_per_cluster == 0 ||
        bpb.num_fats == 0 ||
        bpb.sectors_per_fat == 0) {
        printf("fat12: BPB looks bogus\n");
        return -1;
    }

    fat_start  = bpb.reserved_sectors;
    root_start = fat_start + bpb.num_fats * bpb.sectors_per_fat;
    unsigned int root_sectors = (bpb.root_entries * 32 + 511) / 512;
    data_start = root_start + root_sectors;
    bytes_per_cluster = bpb.sectors_per_cluster * 512;

    unsigned int fat_bytes = bpb.sectors_per_fat * 512;
    fat = (unsigned char*)kmalloc(fat_bytes);
    if (!fat) {
        printf("fat12: out of heap to cache FAT\n");
        return -1;
    }
    if (ata_read(fat_start, bpb.sectors_per_fat, fat) < 0) {
        printf("fat12: cannot read FAT\n");
        kfree(fat); fat = 0;
        return -1;
    }

    cwd_cluster = 0;
    mounted = 1;
    return 0;
}

// ============================================================
// FAT mutation
// ============================================================

static unsigned int total_clusters(void) {
    unsigned int total_sectors = bpb.total_sectors_16
                                   ? bpb.total_sectors_16
                                   : bpb.total_sectors_32;
    unsigned int root_sectors = (bpb.root_entries * 32 + 511) / 512;
    unsigned int data_sectors = total_sectors
                                   - bpb.reserved_sectors
                                   - bpb.num_fats * bpb.sectors_per_fat
                                   - root_sectors;
    return data_sectors / bpb.sectors_per_cluster;
}

static int fat12_flush_fat(void) {
    for (unsigned int i = 0; i < bpb.num_fats; i++) {
        unsigned int lba = fat_start + i * bpb.sectors_per_fat;
        if (ata_write(lba, bpb.sectors_per_fat, fat) < 0) return -1;
    }
    return 0;
}

static unsigned int fat12_alloc_cluster(void) {
    unsigned int total = total_clusters();
    for (unsigned int c = 2; c < total + 2; c++) {
        if (fat12_get_entry(c) == 0x000) {
            fat12_set_entry(c, 0xFFF);
            return c;
        }
    }
    return 0;
}

static void fat12_free_chain(unsigned int start) {
    unsigned int c = start;
    unsigned int safety = total_clusters() + 2;
    while (c >= 2 && !is_end_of_chain(c) && safety--) {
        unsigned int next = fat12_get_entry(c);
        fat12_set_entry(c, 0x000);
        c = next;
    }
}

// ============================================================
// Directory abstraction.
//
// dir_cluster == 0 means the root directory (fixed location, fixed
// size). Otherwise it's the first cluster of a chain.
//
// Directory I/O is cluster-at-a-time; the root just acts like one big
// "cluster" that's actually `root_sectors` long.
// ============================================================

// Number of 32-byte directory entries in one chunk of `dir_cluster`'s
// storage: bytes_per_cluster / 32 for subdirs, bpb.root_entries for root.
static unsigned int dir_chunk_entries(unsigned int dir_cluster) {
    if (dir_cluster == 0) return bpb.root_entries;
    return bytes_per_cluster / 32;
}

// Read one chunk of a directory into `buf` (which must hold
// dir_chunk_entries(dir_cluster) * 32 bytes).
//   - For root, `chunk_index` must be 0 and `chunk_cluster` is ignored.
//   - For a subdir, `chunk_cluster` is the cluster in the chain to read.
// Returns 0 on success.
static int dir_read_chunk(unsigned int dir_cluster,
                          unsigned int chunk_cluster,
                          unsigned char* buf) {
    if (dir_cluster == 0) {
        unsigned int sectors = (bpb.root_entries * 32 + 511) / 512;
        return ata_read(root_start, sectors, buf);
    } else {
        return ata_read(cluster_to_lba(chunk_cluster),
                        bpb.sectors_per_cluster, buf);
    }
}

static int dir_write_chunk(unsigned int dir_cluster,
                           unsigned int chunk_cluster,
                           const unsigned char* buf) {
    if (dir_cluster == 0) {
        unsigned int sectors = (bpb.root_entries * 32 + 511) / 512;
        return ata_write(root_start, sectors, (void*)buf);
    } else {
        return ata_write(cluster_to_lba(chunk_cluster),
                         bpb.sectors_per_cluster, (void*)buf);
    }
}

// Allocate a buffer big enough to hold one chunk. Caller must kfree().
static unsigned char* dir_alloc_chunk(unsigned int dir_cluster) {
    unsigned int sz;
    if (dir_cluster == 0) sz = ((bpb.root_entries * 32 + 511) / 512) * 512;
    else                  sz = bytes_per_cluster;
    return (unsigned char*)kmalloc(sz);
}

// Iterate over every directory entry. The callback receives:
//   - the entry pointer (mutable, in a heap-resident chunk buffer)
//   - the chunk's owning cluster (0 for root)
//   - the index within the chunk (0..dir_chunk_entries-1)
// If the callback returns nonzero, iteration stops. If it returns 1,
// dir_each() will write the modified chunk back to disk before stopping.
//
// On end-of-directory (a dirent whose first byte is 0x00), iteration
// stops without rewriting unless the callback flagged a modification
// in the same chunk first.
typedef int (*dir_visit_fn)(dirent_t* d,
                            unsigned int chunk_cluster,
                            int idx,
                            void* user);

// Walk the directory and call `visit` for each entry, in order.
//   visit returns: 0 = keep going, 1 = stop & write chunk, 2 = stop without write.
// The walk also stops at end-of-directory (0x00).
// Returns 0 on success, -1 on disk error or chain corruption.
static int dir_each(unsigned int dir_cluster, dir_visit_fn visit, void* user) {
    unsigned int per_chunk = dir_chunk_entries(dir_cluster);
    unsigned char* buf = dir_alloc_chunk(dir_cluster);
    if (!buf) return -1;

    unsigned int cluster = dir_cluster;
    unsigned int safety = total_clusters() + 2;

    while (1) {
        if (dir_read_chunk(dir_cluster, cluster, buf) < 0) {
            kfree(buf); return -1;
        }

        int dirty = 0;
        int stop  = 0;
        for (unsigned int i = 0; i < per_chunk; i++) {
            dirent_t* d = (dirent_t*)(buf + i * 32);
            unsigned char first = (unsigned char)d->name[0];

            int r = visit(d, cluster, (int)i, user);
            if (r == 1) { dirty = 1; stop = 1; break; }
            if (r == 2) { stop = 1; break; }

            if (first == 0x00) { stop = 1; break; }
        }

        if (dirty) {
            if (dir_write_chunk(dir_cluster, cluster, buf) < 0) {
                kfree(buf); return -1;
            }
        }
        if (stop) { kfree(buf); return 0; }

        // Advance to next chunk.
        if (dir_cluster == 0) {
            // Root has only one chunk — done.
            kfree(buf); return 0;
        }
        unsigned int next = fat12_get_entry(cluster);
        if (is_end_of_chain(next) || next < 2) { kfree(buf); return 0; }
        cluster = next;
        if (--safety == 0) { kfree(buf); return -1; }
    }
}

// ============================================================
// Searching, listing
// ============================================================

typedef struct {
    char         target[11];
    int          found;
    dirent_t     entry;            // copy of the matched entry
    unsigned int chunk_cluster;    // chunk this entry lives in
    int          idx;              // index within chunk
} find_ctx_t;

static int find_visit(dirent_t* d,
                      unsigned int chunk_cluster,
                      int idx,
                      void* user) {
    find_ctx_t* c = (find_ctx_t*)user;
    unsigned char first = (unsigned char)d->name[0];
    if (first == 0x00) return 2;
    if (first == 0xE5) return 0;
    if ((d->attr & ATTR_LFN) == ATTR_LFN) return 0;

    if (name_eq_83(d, c->target)) {
        c->found = 1;
        c->entry = *d;
        c->chunk_cluster = chunk_cluster;
        c->idx = idx;
        return 2;
    }
    return 0;
}

// Find a child entry of `dir_cluster` by 8.3 raw name.
// On success: *entry_out gets the dirent, *chunk_out the chunk cluster,
// *idx_out the index within the chunk. Returns 0 if found, -1 if not.
static int dir_find(unsigned int dir_cluster, const char target[11],
                    dirent_t* entry_out,
                    unsigned int* chunk_out, int* idx_out) {
    find_ctx_t ctx;
    ctx.found = 0;
    for (int i = 0; i < 11; i++) ctx.target[i] = target[i];
    if (dir_each(dir_cluster, find_visit, &ctx) < 0) return -1;
    if (!ctx.found) return -1;
    if (entry_out) *entry_out = ctx.entry;
    if (chunk_out) *chunk_out = ctx.chunk_cluster;
    if (idx_out)   *idx_out   = ctx.idx;
    return 0;
}

typedef struct {
    fat12_dirent_t* out;
    int max;
    int filled;
} list_ctx_t;

static int list_visit(dirent_t* d,
                      unsigned int chunk_cluster,
                      int idx,
                      void* user) {
    (void)chunk_cluster; (void)idx;
    list_ctx_t* c = (list_ctx_t*)user;
    if (c->filled >= c->max) return 2;

    unsigned char first = (unsigned char)d->name[0];
    if (first == 0x00) return 2;
    if (first == 0xE5) return 0;
    if ((d->attr & ATTR_LFN) == ATTR_LFN) return 0;
    if (d->attr & ATTR_VOLUME_ID) return 0;

    char raw[11];
    memcpy(raw, d->name, 8);
    memcpy(raw + 8, d->ext, 3);
    format_83(raw, c->out[c->filled].name);
    c->out[c->filled].size          = d->size;
    c->out[c->filled].first_cluster = d->cluster_lo;
    c->out[c->filled].attr          = d->attr;
    c->filled++;
    return 0;
}

int fat12_list_dir(unsigned int dir_cluster,
                   fat12_dirent_t* out, int max) {
    if (!mounted) return -1;
    list_ctx_t ctx = { out, max, 0 };
    if (dir_each(dir_cluster, list_visit, &ctx) < 0) return -1;
    return ctx.filled;
}

int fat12_list_root(fat12_dirent_t* out, int max) {
    return fat12_list_dir(cwd_cluster, out, max);
}

// ============================================================
// Path resolution
// ============================================================

// Pull the next '/'-delimited component from *p into out (max capacity
// `cap` including null). Advances *p past the component and any
// trailing slashes. Returns 0 when done, 1 when a component was extracted.
static int next_component(const char** p, char* out, int cap) {
    const char* s = *p;
    while (*s == '/') s++;
    if (*s == '\0') { *p = s; return 0; }
    int n = 0;
    while (*s && *s != '/') {
        if (n < cap - 1) out[n++] = *s;
        s++;
    }
    out[n] = '\0';
    *p = s;
    return 1;
}

int fat12_resolve_dir(const char* path, unsigned int* out) {
    if (!mounted) return -1;

    unsigned int dir;
    if (path[0] == '/') dir = 0;
    else                dir = cwd_cluster;

    const char* p = path;
    char comp[64];

    while (next_component(&p, comp, sizeof(comp))) {
        if (comp[0] == '\0' || (comp[0] == '.' && comp[1] == '\0')) {
            continue;
        }
        // ".." from root stays at root.
        if (comp[0] == '.' && comp[1] == '.' && comp[2] == '\0') {
            if (dir == 0) continue;
            // Look up ".." entry in current dir; its cluster_lo is parent.
            char dotdot[11];
            build_83("..", dotdot);
            dirent_t e;
            if (dir_find(dir, dotdot, &e, 0, 0) < 0) return -1;
            // FAT convention: ".." in a child of root has cluster_lo = 0.
            dir = e.cluster_lo;
            continue;
        }

        char raw[11];
        if (build_83(comp, raw) < 0) return -1;
        dirent_t e;
        if (dir_find(dir, raw, &e, 0, 0) < 0) return -1;
        if (!(e.attr & ATTR_DIRECTORY)) return -1;
        dir = e.cluster_lo;
    }

    if (out) *out = dir;
    return 0;
}

int fat12_resolve_parent(const char* path,
                         unsigned int* parent_out,
                         char* basename_out) {
    if (!mounted) return -1;

    // Find the last '/' in path. Everything before is the parent's
    // path; everything after is the basename.
    int len = 0;
    while (path[len]) len++;
    int last_slash = -1;
    for (int i = 0; i < len; i++) if (path[i] == '/') last_slash = i;

    const char* base;
    char parent_path[128];

    if (last_slash < 0) {
        // No slash at all -> parent is cwd, basename is the whole path.
        if (parent_out) *parent_out = cwd_cluster;
        base = path;
    } else {
        // Copy the prefix (including all trailing slashes' worth of '/'s)
        // into parent_path. If last_slash == 0 the parent is root ("/").
        int copy_len = last_slash == 0 ? 1 : last_slash;
        if (copy_len >= (int)sizeof(parent_path)) return -1;
        for (int i = 0; i < copy_len; i++) parent_path[i] = path[i];
        parent_path[copy_len] = '\0';
        unsigned int p;
        if (fat12_resolve_dir(parent_path, &p) < 0) return -1;
        if (parent_out) *parent_out = p;
        base = path + last_slash + 1;
    }

    if (basename_out) {
        int i = 0;
        while (base[i] && i < 63) { basename_out[i] = base[i]; i++; }
        basename_out[i] = '\0';
        if (i == 0) return -1;   // trailing-slash path with no basename
    }
    return 0;
}

// ============================================================
// Working directory
// ============================================================

unsigned int fat12_cwd(void) { return cwd_cluster; }

int fat12_chdir(const char* path) {
    if (!mounted) return -1;
    unsigned int c;
    if (fat12_resolve_dir(path, &c) < 0) return -1;
    cwd_cluster = c;
    return 0;
}

// Find `child_cluster`'s name inside `parent_cluster`. Writes the
// formatted "NAME.EXT" form to `out` (must be at least 13 bytes).
// Returns 0 on success, -1 if not found.
typedef struct {
    unsigned int target_cluster;
    char         name_out[13];
    int          found;
} childname_ctx_t;

static int childname_visit(dirent_t* d,
                           unsigned int chunk_cluster,
                           int idx, void* user) {
    (void)chunk_cluster; (void)idx;
    childname_ctx_t* c = (childname_ctx_t*)user;
    unsigned char first = (unsigned char)d->name[0];
    if (first == 0x00) return 2;
    if (first == 0xE5) return 0;
    if ((d->attr & ATTR_LFN) == ATTR_LFN) return 0;
    if (!(d->attr & ATTR_DIRECTORY)) return 0;
    // Skip "." and ".." themselves.
    if (d->name[0] == '.') return 0;
    if (d->cluster_lo == c->target_cluster) {
        char raw[11];
        memcpy(raw, d->name, 8);
        memcpy(raw + 8, d->ext, 3);
        format_83(raw, c->name_out);
        c->found = 1;
        return 2;
    }
    return 0;
}

static int find_child_name(unsigned int parent_cluster,
                           unsigned int child_cluster,
                           char out[13]) {
    childname_ctx_t ctx;
    ctx.target_cluster = child_cluster;
    ctx.found = 0;
    if (dir_each(parent_cluster, childname_visit, &ctx) < 0) return -1;
    if (!ctx.found) return -1;
    int i = 0;
    while (ctx.name_out[i]) { out[i] = ctx.name_out[i]; i++; }
    out[i] = '\0';
    return 0;
}

int fat12_getcwd(char* out, unsigned int max) {
    if (!mounted || max < 2) return -1;

    if (cwd_cluster == 0) {
        out[0] = '/'; out[1] = '\0';
        return 1;
    }

    // Walk up via "..", collecting names from the leaf toward root.
    // We build into a temp buffer and then reverse-assemble at the end.
    // Up to 8 levels of nesting is plenty for a 1.44 MB floppy.
    char names[8][13];
    int  depth = 0;

    unsigned int cur = cwd_cluster;
    unsigned int safety = 8;
    while (cur != 0 && safety--) {
        // Find parent via this dir's ".." entry.
        char dotdot[11];
        build_83("..", dotdot);
        dirent_t e;
        if (dir_find(cur, dotdot, &e, 0, 0) < 0) return -1;
        unsigned int parent = e.cluster_lo;   // 0 means root

        // Look up cur's name in parent.
        if (depth >= 8) return -1;
        if (find_child_name(parent, cur, names[depth]) < 0) return -1;
        depth++;
        cur = parent;
    }
    if (cur != 0) return -1;   // didn't reach root within safety bound

    // Assemble: /name0/name1/...
    unsigned int pos = 0;
    for (int i = depth - 1; i >= 0; i--) {
        if (pos + 1 >= max) return -1;
        out[pos++] = '/';
        for (int j = 0; names[i][j]; j++) {
            if (pos + 1 >= max) return -1;
            out[pos++] = names[i][j];
        }
    }
    out[pos] = '\0';
    return (int)pos;
}

// ============================================================
// Slot allocation in a directory
// ============================================================

// Find a free entry slot in `dir_cluster`. If the directory needs to
// grow (subdirs only), allocate a new cluster, append it to the chain,
// zero it, and use its first slot.
//
// On success: *chunk_cluster_out is the cluster where the slot lives,
// *idx_out is its index within that chunk's per_chunk entries.
// Returns 0 on success, -1 on failure (root full, out of clusters).

typedef struct {
    int          first_free_idx;       // -1 if none in this chunk
    unsigned int first_free_cluster;
    int          done;
    unsigned int last_cluster;         // last chunk cluster we visited
} find_free_ctx_t;

static int find_free_visit(dirent_t* d,
                           unsigned int chunk_cluster,
                           int idx, void* user) {
    find_free_ctx_t* c = (find_free_ctx_t*)user;
    unsigned char first = (unsigned char)d->name[0];

    c->last_cluster = chunk_cluster;

    if (first == 0x00 || first == 0xE5) {
        if (c->first_free_idx < 0) {
            c->first_free_idx = idx;
            c->first_free_cluster = chunk_cluster;
        }
        if (first == 0x00) {
            // We can stop walking. Past here all slots are free; we
            // already have our first hit recorded.
            return 2;
        }
    }
    return 0;
}

static int dir_alloc_slot(unsigned int dir_cluster,
                          unsigned int* chunk_cluster_out,
                          int* idx_out) {
    find_free_ctx_t ctx;
    ctx.first_free_idx = -1;
    ctx.first_free_cluster = 0;
    ctx.done = 0;
    ctx.last_cluster = dir_cluster;
    if (dir_each(dir_cluster, find_free_visit, &ctx) < 0) return -1;

    if (ctx.first_free_idx >= 0) {
        *chunk_cluster_out = ctx.first_free_cluster;
        *idx_out = ctx.first_free_idx;
        return 0;
    }

    // No free slot. Root can't grow; subdirs can.
    if (dir_cluster == 0) return -1;

    unsigned int new_c = fat12_alloc_cluster();
    if (new_c == 0) return -1;
    fat12_set_entry(ctx.last_cluster, new_c);

    // Zero the new cluster on disk so dir entries there are properly
    // marked empty (0x00).
    unsigned char* zero = (unsigned char*)kmalloc(bytes_per_cluster);
    if (!zero) { fat12_free_chain(new_c); return -1; }
    memset(zero, 0, bytes_per_cluster);
    if (ata_write(cluster_to_lba(new_c), bpb.sectors_per_cluster, zero) < 0) {
        kfree(zero); fat12_free_chain(new_c); return -1;
    }
    kfree(zero);

    *chunk_cluster_out = new_c;
    *idx_out = 0;
    return 0;
}

// Write `entry` into the given slot.
static int dir_put_entry(unsigned int dir_cluster,
                         unsigned int chunk_cluster, int idx,
                         const dirent_t* entry) {
    unsigned char* buf = dir_alloc_chunk(dir_cluster);
    if (!buf) return -1;
    if (dir_read_chunk(dir_cluster, chunk_cluster, buf) < 0) {
        kfree(buf); return -1;
    }
    *(dirent_t*)(buf + idx * 32) = *entry;
    int rc = dir_write_chunk(dir_cluster, chunk_cluster, buf);
    kfree(buf);
    return rc;
}

// Mark a slot deleted (0xE5).
static int dir_delete_slot(unsigned int dir_cluster,
                           unsigned int chunk_cluster, int idx) {
    unsigned char* buf = dir_alloc_chunk(dir_cluster);
    if (!buf) return -1;
    if (dir_read_chunk(dir_cluster, chunk_cluster, buf) < 0) {
        kfree(buf); return -1;
    }
    dirent_t* d = (dirent_t*)(buf + idx * 32);
    d->name[0] = (char)0xE5;
    int rc = dir_write_chunk(dir_cluster, chunk_cluster, buf);
    kfree(buf);
    return rc;
}

// Build a fresh directory entry.
static void make_dirent(dirent_t* d,
                        const char raw[11],
                        unsigned char attr,
                        unsigned int first_cluster,
                        unsigned int size) {
    for (unsigned int i = 0; i < sizeof(dirent_t); i++) ((unsigned char*)d)[i] = 0;
    for (int i = 0; i < 8; i++) d->name[i] = raw[i];
    for (int i = 0; i < 3; i++) d->ext[i]  = raw[8 + i];
    d->attr       = attr;
    d->cluster_lo = (unsigned short)first_cluster;
    d->cluster_hi = 0;
    d->size       = size;
}

// ============================================================
// File read
// ============================================================

int fat12_read_file(const char* path, void* buf, unsigned int max) {
    if (!mounted) return -1;

    unsigned int parent;
    char base[64];
    if (fat12_resolve_parent(path, &parent, base) < 0) return -1;

    char target[11];
    if (build_83(base, target) < 0) return -1;

    dirent_t e;
    if (dir_find(parent, target, &e, 0, 0) < 0) return -1;
    if (e.attr & (ATTR_VOLUME_ID | ATTR_DIRECTORY)) return -1;

    unsigned char* cluster_buf = (unsigned char*)kmalloc(bytes_per_cluster);
    if (!cluster_buf) return -1;

    unsigned int   cluster   = e.cluster_lo;
    unsigned int   remaining = e.size;
    if (remaining > max) remaining = max;
    unsigned int   wrote     = 0;
    unsigned char* out       = (unsigned char*)buf;

    while (remaining > 0 && cluster >= 2 && !is_end_of_chain(cluster)) {
        if (ata_read(cluster_to_lba(cluster), bpb.sectors_per_cluster,
                     cluster_buf) < 0) {
            kfree(cluster_buf); return -1;
        }
        unsigned int chunk = bytes_per_cluster;
        if (chunk > remaining) chunk = remaining;
        memcpy(out + wrote, cluster_buf, chunk);
        wrote     += chunk;
        remaining -= chunk;
        cluster = fat12_get_entry(cluster);
    }

    kfree(cluster_buf);
    return (int)wrote;
}

// ============================================================
// File write
// ============================================================

int fat12_write_file(const char* path, const void* data, unsigned int size) {
    if (!mounted) return -1;

    unsigned int parent;
    char base[64];
    if (fat12_resolve_parent(path, &parent, base) < 0) return -1;

    char target[11];
    if (build_83(base, target) < 0) return -1;
    if ((unsigned char)target[0] == 0x00 ||
        (unsigned char)target[0] == 0xE5 ||
        (unsigned char)target[0] == ' ') return -1;
    // No overwriting "." or ".." through here.
    if (target[0] == '.' && (target[1] == ' ' ||
        (target[1] == '.' && target[2] == ' '))) return -1;

    // Existing entry?
    dirent_t existing;
    unsigned int ex_chunk = 0;
    int ex_idx = 0;
    int has_existing = (dir_find(parent, target,
                                 &existing, &ex_chunk, &ex_idx) == 0);

    if (has_existing) {
        if (existing.attr & (ATTR_DIRECTORY | ATTR_VOLUME_ID | ATTR_READ_ONLY))
            return -1;
        fat12_free_chain(existing.cluster_lo);
    }

    // Allocate cluster chain.
    unsigned int first_cluster = 0;
    if (size > 0) {
        unsigned int needed = (size + bytes_per_cluster - 1) / bytes_per_cluster;
        unsigned int prev = 0;
        for (unsigned int i = 0; i < needed; i++) {
            unsigned int c = fat12_alloc_cluster();
            if (c == 0) {
                fat12_free_chain(first_cluster);
                return -1;
            }
            if (i == 0) first_cluster = c;
            else        fat12_set_entry(prev, c);
            prev = c;
        }
    }

    // Write data.
    if (size > 0) {
        unsigned char* cb = (unsigned char*)kmalloc(bytes_per_cluster);
        if (!cb) { fat12_free_chain(first_cluster); return -1; }
        const unsigned char* in = (const unsigned char*)data;
        unsigned int remaining = size;
        unsigned int cluster = first_cluster;
        while (remaining > 0) {
            unsigned int chunk = remaining < bytes_per_cluster
                                   ? remaining : bytes_per_cluster;
            for (unsigned int i = 0; i < chunk; i++) cb[i] = in[i];
            for (unsigned int i = chunk; i < bytes_per_cluster; i++) cb[i] = 0;
            if (ata_write(cluster_to_lba(cluster),
                          bpb.sectors_per_cluster, cb) < 0) {
                kfree(cb); fat12_free_chain(first_cluster); return -1;
            }
            in        += chunk;
            remaining -= chunk;
            if (remaining > 0) cluster = fat12_get_entry(cluster);
        }
        kfree(cb);
    }

    // Place the directory entry.
    dirent_t e;
    make_dirent(&e, target, ATTR_ARCHIVE, first_cluster, size);

    unsigned int slot_chunk;
    int slot_idx;
    if (has_existing) {
        slot_chunk = ex_chunk;
        slot_idx   = ex_idx;
    } else {
        if (dir_alloc_slot(parent, &slot_chunk, &slot_idx) < 0) {
            fat12_free_chain(first_cluster);
            return -1;
        }
    }
    if (dir_put_entry(parent, slot_chunk, slot_idx, &e) < 0) {
        fat12_free_chain(first_cluster);
        return -1;
    }

    if (fat12_flush_fat() < 0) return -1;
    return (int)size;
}

// ============================================================
// File delete
// ============================================================

int fat12_delete_file(const char* path) {
    if (!mounted) return -1;

    unsigned int parent;
    char base[64];
    if (fat12_resolve_parent(path, &parent, base) < 0) return -1;

    char target[11];
    if (build_83(base, target) < 0) return -1;

    dirent_t e;
    unsigned int chunk;
    int idx;
    if (dir_find(parent, target, &e, &chunk, &idx) < 0) return -1;
    if (e.attr & (ATTR_DIRECTORY | ATTR_VOLUME_ID)) return -1;

    fat12_free_chain(e.cluster_lo);
    if (dir_delete_slot(parent, chunk, idx) < 0) return -1;
    if (fat12_flush_fat() < 0) return -1;
    return 0;
}

// ============================================================
// mkdir
// ============================================================

int fat12_mkdir(const char* path) {
    if (!mounted) return -1;

    unsigned int parent;
    char base[64];
    if (fat12_resolve_parent(path, &parent, base) < 0) return -1;
    if (base[0] == '.' && (base[1] == '\0' ||
        (base[1] == '.' && base[2] == '\0'))) return -1;

    char target[11];
    if (build_83(base, target) < 0) return -1;

    // Already exists?
    dirent_t e;
    if (dir_find(parent, target, &e, 0, 0) == 0) return -1;

    // Allocate the new directory's first cluster.
    unsigned int new_c = fat12_alloc_cluster();
    if (new_c == 0) return -1;

    // Write "." and ".." as the only two entries; rest of the cluster
    // is zero ("end of directory").
    unsigned char* buf = (unsigned char*)kmalloc(bytes_per_cluster);
    if (!buf) { fat12_free_chain(new_c); return -1; }
    memset(buf, 0, bytes_per_cluster);

    char dot11[11], dotdot11[11];
    build_83(".",  dot11);
    build_83("..", dotdot11);

    dirent_t* dot    = (dirent_t*)(buf);
    dirent_t* dotdot = (dirent_t*)(buf + 32);
    make_dirent(dot,    dot11,    ATTR_DIRECTORY, new_c,  0);
    // ".." cluster_lo is parent; FAT convention: 0 if parent is root.
    make_dirent(dotdot, dotdot11, ATTR_DIRECTORY, parent, 0);

    if (ata_write(cluster_to_lba(new_c),
                  bpb.sectors_per_cluster, buf) < 0) {
        kfree(buf); fat12_free_chain(new_c); return -1;
    }
    kfree(buf);

    // Add the directory entry to the parent.
    unsigned int slot_chunk;
    int slot_idx;
    if (dir_alloc_slot(parent, &slot_chunk, &slot_idx) < 0) {
        fat12_free_chain(new_c); return -1;
    }
    dirent_t parent_entry;
    make_dirent(&parent_entry, target, ATTR_DIRECTORY, new_c, 0);
    if (dir_put_entry(parent, slot_chunk, slot_idx, &parent_entry) < 0) {
        fat12_free_chain(new_c); return -1;
    }

    if (fat12_flush_fat() < 0) return -1;
    return 0;
}

// ============================================================
// rmdir
// ============================================================

typedef struct { int has_real_entry; } empty_ctx_t;
static int empty_visit(dirent_t* d,
                       unsigned int chunk_cluster, int idx, void* user) {
    (void)chunk_cluster; (void)idx;
    empty_ctx_t* c = (empty_ctx_t*)user;
    unsigned char first = (unsigned char)d->name[0];
    if (first == 0x00) return 2;
    if (first == 0xE5) return 0;
    if ((d->attr & ATTR_LFN) == ATTR_LFN) return 0;
    // Skip "." and ".."
    if (d->name[0] == '.' &&
        (d->name[1] == ' ' || (d->name[1] == '.' && d->name[2] == ' ')))
        return 0;
    c->has_real_entry = 1;
    return 2;
}

int fat12_rmdir(const char* path) {
    if (!mounted) return -1;

    unsigned int parent;
    char base[64];
    if (fat12_resolve_parent(path, &parent, base) < 0) return -1;
    if (base[0] == '.' && (base[1] == '\0' ||
        (base[1] == '.' && base[2] == '\0'))) return -1;

    char target[11];
    if (build_83(base, target) < 0) return -1;

    dirent_t e;
    unsigned int chunk;
    int idx;
    if (dir_find(parent, target, &e, &chunk, &idx) < 0) return -1;
    if (!(e.attr & ATTR_DIRECTORY)) return -1;
    if (e.cluster_lo == 0) return -1;        // can't remove root
    if (e.cluster_lo == cwd_cluster) return -1;  // can't remove cwd

    // Verify empty (no entries other than "." and "..").
    empty_ctx_t ec = { 0 };
    if (dir_each(e.cluster_lo, empty_visit, &ec) < 0) return -1;
    if (ec.has_real_entry) return -1;

    // Free the directory's clusters and remove the entry.
    fat12_free_chain(e.cluster_lo);
    if (dir_delete_slot(parent, chunk, idx) < 0) return -1;
    if (fat12_flush_fat() < 0) return -1;
    return 0;
}

// ============================================================
// cp
// ============================================================

// If `dst` resolves to an existing directory, return that directory's
// cluster in *dir_out and 1; otherwise return 0 (treat dst as a file path).
// On error returns -1.
static int dst_is_existing_dir(const char* dst, unsigned int* dir_out) {
    unsigned int c;
    if (fat12_resolve_dir(dst, &c) == 0) {
        if (dir_out) *dir_out = c;
        return 1;
    }
    return 0;
}

// Strip the basename out of a path. e.g. "/foo/bar.txt" -> "bar.txt".
// Result points into `path` (no copy). For paths ending in '/', returns "".
static const char* path_basename(const char* path) {
    int len = 0; while (path[len]) len++;
    int last = -1;
    for (int i = 0; i < len; i++) if (path[i] == '/') last = i;
    return path + last + 1;
}

int fat12_cp(const char* src, const char* dst) {
    if (!mounted) return -1;

    // Refuse to copy a directory.
    {
        unsigned int sp; char sb[64];
        if (fat12_resolve_parent(src, &sp, sb) < 0) return -1;
        char raw[11];
        if (build_83(sb, raw) < 0) return -1;
        dirent_t e;
        if (dir_find(sp, raw, &e, 0, 0) < 0) return -1;
        if (e.attr & ATTR_DIRECTORY) return -1;
    }

    // Read src in full.
    enum { CP_MAX = 65536 };
    unsigned char* buf = (unsigned char*)kmalloc(CP_MAX);
    if (!buf) return -1;
    int n = fat12_read_file(src, buf, CP_MAX);
    if (n < 0) { kfree(buf); return -1; }

    // Build the real dst path: if dst is a directory, append src's basename.
    char effective[160];
    unsigned int dst_dir;
    if (dst_is_existing_dir(dst, &dst_dir) == 1) {
        const char* sb = path_basename(src);
        // effective = dst + "/" + sb
        int i = 0;
        while (dst[i] && i < (int)sizeof(effective) - 2) {
            effective[i] = dst[i]; i++;
        }
        // Trim a trailing slash.
        if (i > 0 && effective[i - 1] == '/') i--;
        if (i >= (int)sizeof(effective) - 1) { kfree(buf); return -1; }
        effective[i++] = '/';
        for (int j = 0; sb[j] && i < (int)sizeof(effective) - 1; j++)
            effective[i++] = sb[j];
        effective[i] = '\0';
    } else {
        int i = 0;
        while (dst[i] && i < (int)sizeof(effective) - 1) {
            effective[i] = dst[i]; i++;
        }
        effective[i] = '\0';
    }

    int rc = fat12_write_file(effective, buf, (unsigned int)n);
    kfree(buf);
    return rc < 0 ? -1 : 0;
}

// ============================================================
// mv  (rename, possibly across directories)
// ============================================================

int fat12_mv(const char* src, const char* dst) {
    if (!mounted) return -1;

    // Resolve src.
    unsigned int s_parent;
    char s_base[64];
    if (fat12_resolve_parent(src, &s_parent, s_base) < 0) return -1;
    char s_raw[11];
    if (build_83(s_base, s_raw) < 0) return -1;
    if (s_base[0] == '.' && (s_base[1] == '\0' ||
        (s_base[1] == '.' && s_base[2] == '\0'))) return -1;

    dirent_t src_entry;
    unsigned int s_chunk; int s_idx;
    if (dir_find(s_parent, s_raw, &src_entry, &s_chunk, &s_idx) < 0)
        return -1;
    if (src_entry.attr & ATTR_VOLUME_ID) return -1;
    // We allow moving directories — but only as a rename, not into themselves.

    // Figure out destination directory and basename.
    unsigned int d_parent;
    char d_base[64];
    unsigned int existing_dir;
    if (dst_is_existing_dir(dst, &existing_dir) == 1) {
        d_parent = existing_dir;
        // basename of src goes into the target dir.
        int i = 0;
        while (s_base[i] && i < 63) { d_base[i] = s_base[i]; i++; }
        d_base[i] = '\0';
    } else {
        if (fat12_resolve_parent(dst, &d_parent, d_base) < 0) return -1;
    }
    if (d_base[0] == '.' && (d_base[1] == '\0' ||
        (d_base[1] == '.' && d_base[2] == '\0'))) return -1;

    char d_raw[11];
    if (build_83(d_base, d_raw) < 0) return -1;

    // No-op if same parent and same name.
    int same = (s_parent == d_parent);
    if (same) {
        int eq = 1;
        for (int i = 0; i < 11; i++) if (s_raw[i] != d_raw[i]) { eq = 0; break; }
        if (eq) return 0;
    }

    // If destination exists, refuse.
    dirent_t already;
    if (dir_find(d_parent, d_raw, &already, 0, 0) == 0) return -1;

    // Refuse to move a directory into itself or a descendant.
    if (src_entry.attr & ATTR_DIRECTORY) {
        unsigned int p = d_parent;
        unsigned int safety = 16;
        while (p != 0 && safety--) {
            if (p == src_entry.cluster_lo) return -1;
            char dotdot[11]; build_83("..", dotdot);
            dirent_t pe;
            if (dir_find(p, dotdot, &pe, 0, 0) < 0) break;
            p = pe.cluster_lo;
        }
    }

    // Build new entry: same metadata, possibly new name.
    dirent_t new_entry = src_entry;
    for (int i = 0; i < 8; i++) new_entry.name[i] = d_raw[i];
    for (int i = 0; i < 3; i++) new_entry.ext[i]  = d_raw[8 + i];

    // Insert into destination first; then delete from source. If insert
    // fails we leave src untouched.
    unsigned int dst_chunk; int dst_idx;
    if (dir_alloc_slot(d_parent, &dst_chunk, &dst_idx) < 0) return -1;
    if (dir_put_entry(d_parent, dst_chunk, dst_idx, &new_entry) < 0)
        return -1;

    if (dir_delete_slot(s_parent, s_chunk, s_idx) < 0) {
        // Best effort: if this somehow fails the file ends up in both
        // places. We've at least flushed nothing yet that conflicts.
        return -1;
    }

    // If we moved a directory across parents, fix up its ".." entry.
    if ((src_entry.attr & ATTR_DIRECTORY) && !same) {
        char dotdot[11]; build_83("..", dotdot);
        dirent_t dde;
        unsigned int dd_chunk; int dd_idx;
        if (dir_find(src_entry.cluster_lo, dotdot,
                     &dde, &dd_chunk, &dd_idx) == 0) {
            dde.cluster_lo = (unsigned short)d_parent;   // 0 if root
            dir_put_entry(src_entry.cluster_lo, dd_chunk, dd_idx, &dde);
        }
    }

    if (fat12_flush_fat() < 0) return -1;
    return 0;
}

// ---- VFS adapters ------------------------------------------------------
// Path-based wrappers so the VFS can treat FAT12 through a uniform,
// cluster-free interface. These use only the public fat12_* API. The VFS
// always passes absolute paths (rooted at this mount), which FAT12 resolves
// from its root independent of its internal cwd.

// List a directory given by path. Returns entry count or -1.
int fat12_vfs_list(const char* path, fat12_dirent_t* out, int max) {
    unsigned int dir;
    if (fat12_resolve_dir(path, &dir) < 0) return -1;
    return fat12_list_dir(dir, out, max);
}

// Return 1 if `path` names a directory, else 0. fat12_resolve_dir succeeds
// only for directories, so this is a direct test.
int fat12_vfs_is_dir(const char* path) {
    unsigned int dir;
    return (fat12_resolve_dir(path, &dir) == 0) ? 1 : 0;
}