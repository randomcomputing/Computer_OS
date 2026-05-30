#include "fat32.h"
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
    unsigned short sectors_per_fat_16;
    unsigned short sectors_per_track;
    unsigned short num_heads;
    unsigned int   hidden_sectors;
    unsigned int   total_sectors_32;
} bpb_t;

typedef struct __attribute__((packed)) {
    unsigned int   sectors_per_fat;
    unsigned short ext_flags;
    unsigned short fs_version;
    unsigned int   root_cluster;
    unsigned short fs_info;
    unsigned short backup_boot;
    unsigned char  reserved[12];
    unsigned char  drive_number;
    unsigned char  reserved1;
    unsigned char  boot_sig;
    unsigned int   volume_id;
    char           volume_label[11];
    char           fs_type[8];
} bpb32_ext_t;

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

typedef struct __attribute__((packed)) {
    unsigned char  order;
    unsigned short name1[5];
    unsigned char  attr;
    unsigned char  type;
    unsigned char  checksum;
    unsigned short name2[6];
    unsigned short cluster;
    unsigned short name3[2];
} lfn_entry_t;

#define FAT32_EOC       0x0FFFFFF8u
#define FAT32_FREE      0x00000000u
#define FAT32_BAD       0x0FFFFFF7u
#define FAT32_MASK      0x0FFFFFFFu

// ============================================================
// Mount state
// ============================================================

static int           mounted          = 0;
static bpb_t         bpb;
static bpb32_ext_t   bpb32;
static unsigned int* fat              = 0;
static unsigned int  fat_entries      = 0;
static unsigned int  fat_start;
static unsigned int  data_start;
static unsigned int  root_cluster;
static unsigned int  bytes_per_cluster;
static unsigned int  sectors_per_fat;
static unsigned int  total_clusters_count;
static unsigned int  part_lba         = 0;

static unsigned int  cwd_cluster      = 0;

// ============================================================
// Low-level disk helpers
// ============================================================

static int fat_read(unsigned int lba, unsigned int count, void* buf) {
    return ata_read(part_lba + lba, count, buf);
}

static int fat_write(unsigned int lba, unsigned int count, const void* buf) {
    return ata_write(part_lba + lba, count, buf);
}

static unsigned int cluster_to_lba(unsigned int cluster) {
    return data_start + (cluster - 2) * bpb.sectors_per_cluster;
}

// ============================================================
// FAT helpers
// ============================================================

static unsigned int fat32_get_entry(unsigned int cluster) {
    if (cluster >= fat_entries) return FAT32_EOC;
    return fat[cluster] & FAT32_MASK;
}

static void fat32_set_entry(unsigned int cluster, unsigned int value) {
    if (cluster >= fat_entries) return;
    fat[cluster] = (fat[cluster] & 0xF0000000u) | (value & FAT32_MASK);
}

static int is_end_of_chain(unsigned int entry) {
    return (entry & FAT32_MASK) >= FAT32_EOC;
}

static int fat32_flush_fat(void) {
    for (unsigned int i = 0; i < bpb.num_fats; i++) {
        unsigned int lba = fat_start + i * sectors_per_fat;
        if (fat_write(lba, sectors_per_fat, fat) < 0) return -1;
    }

    return 0;
}

static unsigned int fat32_alloc_cluster(void) {
    for (unsigned int c = 2; c < fat_entries; c++) {
        if ((fat[c] & FAT32_MASK) == FAT32_FREE) {
            fat32_set_entry(c, FAT32_EOC);
            return c;
        }
    }

    return 0;
}

static void fat32_free_chain(unsigned int start) {
    unsigned int c = start;
    unsigned int safety = total_clusters_count + 2;

    while (c >= 2 && !is_end_of_chain(c) && safety--) {
        unsigned int next = fat32_get_entry(c);
        fat32_set_entry(c, FAT32_FREE);
        c = next;
    }

    if (c >= 2 && is_end_of_chain(c)) {
        fat32_set_entry(c, FAT32_FREE);
    }
}

static unsigned int fat32_extend_chain(unsigned int last) {
    unsigned int c = fat32_alloc_cluster();

    if (!c) return 0;

    fat32_set_entry(last, c);
    fat32_set_entry(c, FAT32_EOC);

    return c;
}

// ============================================================
// Name helpers
// ============================================================

static char upper(char c) {
    if (c >= 'a' && c <= 'z') return c - 'a' + 'A';
    return c;
}

static int format_83(const char* raw, char* out) {
    int n = 0;

    for (int i = 0; i < 8 && raw[i] != ' '; i++) {
        out[n++] = raw[i];
    }

    if (raw[8] != ' ') {
        out[n++] = '.';

        for (int i = 8; i < 11 && raw[i] != ' '; i++) {
            out[n++] = raw[i];
        }
    }

    out[n] = '\0';
    return n;
}

static int build_83(const char* name, char out[11]) {
    for (int i = 0; i < 11; i++) out[i] = ' ';

    if (name[0] == '.' && name[1] == '\0') {
        out[0] = '.';
        return 0;
    }

    if (name[0] == '.' && name[1] == '.' && name[2] == '\0') {
        out[0] = '.';
        out[1] = '.';
        return 0;
    }

    int dot = -1;

    for (int i = 0; name[i]; i++) {
        if (name[i] == '.') dot = i;
    }

    int end = (dot >= 0) ? dot : (int)strlen(name);
    if (end > 8) end = 8;

    for (int i = 0; i < end; i++) {
        out[i] = upper(name[i]);
    }

    if (dot >= 0) {
        int elen = (int)strlen(name) - dot - 1;
        if (elen > 3) elen = 3;

        for (int i = 0; i < elen; i++) {
            out[8 + i] = upper(name[dot + 1 + i]);
        }
    }

    return 0;
}

// ============================================================
// LFN helpers
// ============================================================

static unsigned char lfn_checksum(const char name83[11]) {
    unsigned char sum = 0;

    for (int i = 0; i < 11; i++) {
        sum = (unsigned char)(((sum & 1) << 7) +
                              (sum >> 1) +
                              (unsigned char)name83[i]);
    }

    return sum;
}

static int lfn_entry_count(int len) {
    return (len + 12) / 13;
}

static void lfn_write_entries(unsigned char* chunk_buf,
                              int start_idx,
                              int n_lfn,
                              const char* name,
                              unsigned char checksum,
                              unsigned int epc) {
    int namelen = (int)strlen(name);

    for (int seg = n_lfn - 1; seg >= 0; seg--) {
        int slot = start_idx + (n_lfn - 1 - seg);

        if ((unsigned int)slot >= epc) break;

        lfn_entry_t* le = (lfn_entry_t*)(chunk_buf + slot * 32);

        memset(le, 0xFF, 32);

        le->attr     = ATTR_LFN;
        le->type     = 0;
        le->cluster  = 0;
        le->checksum = checksum;
        le->order    = (unsigned char)(seg + 1);

        if (seg == n_lfn - 1) {
            le->order |= 0x40;
        }

        int base = seg * 13;
        unsigned short chars[13];

        for (int i = 0; i < 13; i++) {
            int ci = base + i;

            if (ci < namelen) {
                chars[i] = (unsigned short)(unsigned char)name[ci];
            } else if (ci == namelen) {
                chars[i] = 0x0000;
            } else {
                chars[i] = 0xFFFF;
            }
        }

        for (int i = 0; i < 5; i++) le->name1[i] = chars[i];
        for (int i = 0; i < 6; i++) le->name2[i] = chars[5 + i];
        for (int i = 0; i < 2; i++) le->name3[i] = chars[11 + i];
    }
}

static void lfn_reconstruct(dirent_t* entries, int n, char* out, int outmax) {
    int pos = 0;

    for (int e = n - 1; e >= 0 && pos < outmax - 1; e--) {
        lfn_entry_t* le = (lfn_entry_t*)&entries[e];

        unsigned short chars[13];

        for (int i = 0; i < 5; i++) chars[i] = le->name1[i];
        for (int i = 0; i < 6; i++) chars[5 + i] = le->name2[i];
        for (int i = 0; i < 2; i++) chars[11 + i] = le->name3[i];

        for (int i = 0; i < 13 && pos < outmax - 1; i++) {
            if (chars[i] == 0x0000) goto done_lfn;
            if (chars[i] == 0xFFFF) continue;

            out[pos++] = (char)(chars[i] & 0x7F);
        }
    }

done_lfn:
    out[pos] = '\0';
}

// ============================================================
// Directory helpers
// ============================================================

static unsigned int real_cluster(unsigned int alias) {
    return (alias == 0) ? root_cluster : alias;
}

static unsigned int dir_entries_per_cluster(void) {
    return bytes_per_cluster / 32;
}

static int dir_read_chunk(unsigned int cluster, unsigned char* buf) {
    return fat_read(cluster_to_lba(real_cluster(cluster)),
                    bpb.sectors_per_cluster,
                    buf);
}

static int dir_write_chunk(unsigned int cluster, const unsigned char* buf) {
    return fat_write(cluster_to_lba(real_cluster(cluster)),
                     bpb.sectors_per_cluster,
                     buf);
}

static unsigned char* dir_alloc_chunk(void) {
    return (unsigned char*)kmalloc(bytes_per_cluster);
}

typedef int (*dir_cb)(dirent_t* e,
                      unsigned int chunk_cluster,
                      int idx,
                      void* ctx);

static int dir_each(unsigned int dir_cluster, dir_cb cb, void* ctx) {
    unsigned char* buf = dir_alloc_chunk();

    if (!buf) return -1;

    unsigned int epc = dir_entries_per_cluster();
    unsigned int chunk = dir_cluster;
    int ret = 0;

    while (1) {
        if (dir_read_chunk(chunk, buf) < 0) {
            ret = -1;
            break;
        }

        dirent_t* entries = (dirent_t*)buf;
        int dirty = 0;

        for (unsigned int i = 0; i < epc; i++) {
            if ((unsigned char)entries[i].name[0] == 0x00) {
                ret = 0;
                goto done;
            }

            int r = cb(&entries[i], chunk, (int)i, ctx);

            if (r != 0) {
                if (r == 1) {
                    dirty = 1;
                    ret = 1;
                } else {
                    ret = r;
                }

                if (dirty) dir_write_chunk(chunk, buf);
                goto done;
            }
        }

        if (dirty) dir_write_chunk(chunk, buf);

        unsigned int next = fat32_get_entry(real_cluster(chunk));

        if (is_end_of_chain(next)) break;

        chunk = next;
    }

done:
    kfree(buf);
    return ret;
}

// ============================================================
// Directory find / alloc / delete
// ============================================================

typedef struct {
    char         raw83[11];
    dirent_t     found;
    unsigned int found_chunk;
    int          found_idx;
    int          was_found;
} find_ctx;

static int find_cb(dirent_t* e, unsigned int chunk, int idx, void* ctx) {
    if ((unsigned char)e->name[0] == 0xE5) return 0;
    if (e->attr == ATTR_LFN) return 0;

    find_ctx* fc = (find_ctx*)ctx;

    if (memcmp(e->name, fc->raw83, 11) == 0) {
        fc->found = *e;
        fc->found_chunk = chunk;
        fc->found_idx = idx;
        fc->was_found = 1;
        return -1;
    }

    return 0;
}

static int dir_find(unsigned int dir_cluster,
                    const char raw83[11],
                    dirent_t* out,
                    unsigned int* found_chunk_out,
                    int* found_idx_out) {
    find_ctx fc;

    memset(&fc, 0, sizeof(fc));
    memcpy(fc.raw83, raw83, 11);

    dir_each(dir_cluster, find_cb, &fc);

    if (!fc.was_found) return -1;

    if (out) *out = fc.found;
    if (found_chunk_out) *found_chunk_out = fc.found_chunk;
    if (found_idx_out) *found_idx_out = fc.found_idx;

    return 0;
}

/*
 * IMPORTANT FIX:
 *
 * The old version used dir_each() to find a free slot.
 * That was wrong because dir_each() stops at 0x00, which means
 * "end of directory".
 *
 * For listing and searching, stopping at 0x00 is correct.
 * For creating a new file, 0x00 is exactly the free slot we want.
 *
 * If we skip the 0x00 slot, the filesystem extends the directory chain,
 * writes the new file entry into the new cluster, and then `ls` never
 * reaches it because `ls` stops at the old 0x00 marker.
 */
static int dir_alloc_slot(unsigned int dir_cluster,
                          unsigned int* chunk_out,
                          int* idx_out) {
    unsigned char* buf = dir_alloc_chunk();

    if (!buf) return -1;

    unsigned int epc = dir_entries_per_cluster();
    unsigned int chunk = dir_cluster;

    while (1) {
        if (dir_read_chunk(chunk, buf) < 0) {
            kfree(buf);
            return -1;
        }

        dirent_t* entries = (dirent_t*)buf;

        for (unsigned int i = 0; i < epc; i++) {
            unsigned char first = (unsigned char)entries[i].name[0];

            if (first == 0xE5 || first == 0x00) {
                *chunk_out = chunk;
                *idx_out = (int)i;
                kfree(buf);
                return 0;
            }
        }

        unsigned int real = real_cluster(chunk);
        unsigned int next = fat32_get_entry(real);

        if (is_end_of_chain(next)) break;

        chunk = next;
    }

    unsigned int last = real_cluster(chunk);
    unsigned int new_c = fat32_extend_chain(last);

    if (!new_c) {
        kfree(buf);
        return -1;
    }

    memset(buf, 0, bytes_per_cluster);

    if (fat_write(cluster_to_lba(new_c), bpb.sectors_per_cluster, buf) < 0) {
        kfree(buf);
        return -1;
    }

    kfree(buf);

    *chunk_out = new_c;
    *idx_out = 0;

    return 0;
}

static int dir_put_entry(unsigned int dir_cluster,
                         unsigned int chunk,
                         int idx,
                         const dirent_t* e) {
    (void)dir_cluster;

    unsigned char* buf = dir_alloc_chunk();

    if (!buf) return -1;

    if (dir_read_chunk(chunk, buf) < 0) {
        kfree(buf);
        return -1;
    }

    ((dirent_t*)buf)[idx] = *e;

    int r = dir_write_chunk(chunk, buf);

    kfree(buf);

    return r;
}

static int dir_delete_slot(unsigned int chunk, int idx) {
    unsigned char* buf = dir_alloc_chunk();

    if (!buf) return -1;

    if (dir_read_chunk(chunk, buf) < 0) {
        kfree(buf);
        return -1;
    }

    buf[idx * 32] = 0xE5;

    int r = dir_write_chunk(chunk, buf);

    kfree(buf);

    return r;
}

// ============================================================
// Mount
// ============================================================

static unsigned int gpt_find_data_partition(void) {
    unsigned char buf[512];

    if (ata_read(1, 1, buf) < 0) return 0;

    if (buf[0] != 'E' || buf[1] != 'F' || buf[2] != 'I' || buf[3] != ' ' ||
        buf[4] != 'P' || buf[5] != 'A' || buf[6] != 'R' || buf[7] != 'T') {
        return 0;
    }

    unsigned int entries_lba = *(unsigned int*)(buf + 72);
    unsigned int num_entries = *(unsigned int*)(buf + 80);
    unsigned int entry_size  = *(unsigned int*)(buf + 84);

    if (entry_size == 0) return 0;

    const unsigned char esp0[4] = { 0x28, 0x73, 0x2A, 0xC1 };

    unsigned int checked = 0;
    unsigned int lba = entries_lba;

    while (checked < num_entries) {
        unsigned char ebuf[512];

        if (ata_read(lba, 1, ebuf) < 0) break;

        unsigned int per_sector = 512 / entry_size;

        for (unsigned int i = 0;
             i < per_sector && checked < num_entries;
             i++, checked++) {
            unsigned char* e = ebuf + i * entry_size;

            int all_zero = 1;

            for (int b = 0; b < 16; b++) {
                if (e[b]) {
                    all_zero = 0;
                    break;
                }
            }

            if (all_zero) continue;

            if (e[0] == esp0[0] &&
                e[1] == esp0[1] &&
                e[2] == esp0[2] &&
                e[3] == esp0[3]) {
                continue;
            }

            unsigned int start = *(unsigned int*)(e + 32);

            if (start > 0) return start;
        }

        lba++;
    }

    return 0;
}

int fat32_mount(void) {
    if (mounted) return 0;

    unsigned int candidates[2];

    candidates[0] = gpt_find_data_partition();
    candidates[1] = (candidates[0] != 0) ? 0 : 0xFFFFFFFFu;

    unsigned char sector[512];
    int found = 0;

    for (int t = 0; t < 2 && !found; t++) {
        if (candidates[t] == 0xFFFFFFFFu) break;

        part_lba = candidates[t];

        if (ata_read(part_lba, 1, sector) < 0) continue;

        memcpy(&bpb, sector, sizeof(bpb));
        memcpy(&bpb32, sector + sizeof(bpb), sizeof(bpb32));

        if (bpb.bytes_per_sector != 512) continue;
        if (bpb.sectors_per_cluster == 0) continue;
        if (bpb.num_fats == 0) continue;
        if (bpb32.sectors_per_fat == 0) continue;

        if (bpb32.fs_type[0] == 'F' &&
            bpb32.fs_type[1] == 'A' &&
            bpb32.fs_type[2] == 'T' &&
            bpb32.fs_type[3] == '3' &&
            bpb32.fs_type[4] == '2') {
            found = 1;
        }
    }

    if (!found) {
        printf("fat32: BPB does not look like FAT32\n");
        return -1;
    }

    sectors_per_fat = bpb32.sectors_per_fat;
    fat_start = bpb.reserved_sectors;
    data_start = fat_start + bpb.num_fats * sectors_per_fat;
    root_cluster = bpb32.root_cluster;
    bytes_per_cluster = bpb.sectors_per_cluster * 512u;

    unsigned int total_sectors = bpb.total_sectors_16
                               ? bpb.total_sectors_16
                               : bpb.total_sectors_32;

    unsigned int data_sectors = total_sectors - data_start;

    total_clusters_count = data_sectors / bpb.sectors_per_cluster;

    fat_entries = sectors_per_fat * (512 / 4);

    unsigned int fat_bytes = sectors_per_fat * 512;

    fat = (unsigned int*)kmalloc(fat_bytes);

    if (!fat) {
        printf("fat32: out of heap to cache FAT\n");
        return -1;
    }

    if (fat_read(fat_start, sectors_per_fat, fat) < 0) {
        printf("fat32: cannot read FAT\n");
        kfree(fat);
        fat = 0;
        return -1;
    }

    cwd_cluster = 0;
    mounted = 1;

    return 0;
}

// ============================================================
// Path resolution
// ============================================================

static int is_dir_entry(const dirent_t* e) {
    return (e->attr & ATTR_DIRECTORY) && e->name[0] != '.';
}

static unsigned int entry_cluster(const dirent_t* e) {
    return ((unsigned int)e->cluster_hi << 16) | e->cluster_lo;
}

static unsigned int resolve_component(unsigned int dir, const char* comp) {
    if (comp[0] == '\0' ||
        (comp[0] == '.' && comp[1] == '\0')) {
        return dir;
    }

    if (comp[0] == '.' && comp[1] == '.' && comp[2] == '\0') {
        char dotdot[11];

        build_83("..", dotdot);

        dirent_t e;

        if (dir_find(dir, dotdot, &e, 0, 0) < 0) {
            return 0;
        }

        unsigned int c = entry_cluster(&e);

        return (c == root_cluster || c == 0) ? 0 : c;
    }

    char raw83[11];

    if (build_83(comp, raw83) < 0) {
        return 0xFFFFFFFFu;
    }

    dirent_t e;

    if (dir_find(dir, raw83, &e, 0, 0) < 0) {
        return 0xFFFFFFFFu;
    }

    if (!(e.attr & ATTR_DIRECTORY)) {
        return 0xFFFFFFFFu;
    }

    unsigned int c = entry_cluster(&e);

    return (c == root_cluster) ? 0 : c;
}

int fat32_resolve_dir(const char* path, unsigned int* out) {
    unsigned int cur = (*path == '/') ? 0 : cwd_cluster;

    if (*path == '/') path++;

    char comp[64];

    while (*path) {
        int i = 0;

        while (*path && *path != '/') {
            if (i < 63) comp[i++] = *path;
            path++;
        }

        comp[i] = '\0';

        if (*path == '/') path++;

        if (comp[0] == '\0') continue;

        cur = resolve_component(cur, comp);

        if (cur == 0xFFFFFFFFu) return -1;
    }

    *out = cur;
    return 0;
}

int fat32_resolve_parent(const char* path,
                         unsigned int* parent_out,
                         char* basename_out) {
    int slash = -1;

    for (int i = 0; path[i]; i++) {
        if (path[i] == '/') slash = i;
    }

    if (slash < 0) {
        *parent_out = cwd_cluster;

        int i = 0;

        while (path[i] && i < 63) {
            basename_out[i] = path[i];
            i++;
        }

        basename_out[i] = '\0';

        return 0;
    }

    char parent_path[256];

    int plen = slash > 0 ? slash : 1;

    if (plen >= 256) return -1;

    memcpy(parent_path, path, plen);
    parent_path[plen] = '\0';

    int i = 0;
    const char* bn = path + slash + 1;

    while (*bn && i < 63) {
        basename_out[i++] = *bn++;
    }

    basename_out[i] = '\0';

    return fat32_resolve_dir(parent_path, parent_out);
}

// ============================================================
// Working directory
// ============================================================

unsigned int fat32_cwd(void) {
    return cwd_cluster;
}

int fat32_chdir(const char* path) {
    unsigned int c;

    if (fat32_resolve_dir(path, &c) < 0) return -1;

    cwd_cluster = c;

    return 0;
}

int fat32_getcwd(char* out, unsigned int max) {
    if (cwd_cluster == 0) {
        if (max < 2) return -1;

        out[0] = '/';
        out[1] = '\0';

        return 1;
    }

#define MAX_DEPTH 32

    char components[MAX_DEPTH][64];
    int depth = 0;
    unsigned int cur = cwd_cluster;

    while (cur != 0 && cur != root_cluster) {
        if (depth >= MAX_DEPTH) return -1;

        char dotdot[11];

        build_83("..", dotdot);

        dirent_t dd_entry;

        if (dir_find(cur, dotdot, &dd_entry, 0, 0) < 0) {
            return -1;
        }

        unsigned int parent = entry_cluster(&dd_entry);

        if (parent == root_cluster) parent = 0;

        unsigned char* buf = dir_alloc_chunk();

        if (!buf) return -1;

        unsigned int epc = dir_entries_per_cluster();
        unsigned int chunk = parent;
        int found = 0;
        char comp_name[13];

        while (!found) {
            if (dir_read_chunk(chunk, buf) < 0) break;

            dirent_t* entries = (dirent_t*)buf;

            for (unsigned int i = 0; i < epc; i++) {
                unsigned char first = (unsigned char)entries[i].name[0];

                if (first == 0x00) goto done_scan;
                if (first == 0xE5) continue;
                if (entries[i].attr == ATTR_LFN) continue;
                if (!(entries[i].attr & ATTR_DIRECTORY)) continue;
                if (entries[i].name[0] == '.') continue;

                unsigned int ec = entry_cluster(&entries[i]);

                if (ec == cur || (ec == 0 && cur == root_cluster)) {
                    format_83(entries[i].name, comp_name);
                    found = 1;
                    break;
                }
            }

            unsigned int next = fat32_get_entry(real_cluster(chunk));

            if (is_end_of_chain(next)) break;

            chunk = next;
        }

done_scan:
        kfree(buf);

        if (!found) return -1;

        int ci = 0;

        while (comp_name[ci] && ci < 63) {
            components[depth][ci] = comp_name[ci];
            ci++;
        }

        components[depth][ci] = '\0';

        depth++;
        cur = parent;
    }

    unsigned int pos = 0;

    out[pos++] = '/';

    for (int d = depth - 1; d >= 0; d--) {
        int ci = 0;

        while (components[d][ci]) {
            if (pos + 1 >= max) return -1;
            out[pos++] = components[d][ci++];
        }

        if (d > 0) {
            if (pos + 1 >= max) return -1;
            out[pos++] = '/';
        }
    }

    out[pos] = '\0';

    return (int)pos;
}

// ============================================================
// Directory listing
// ============================================================

typedef struct {
    fat32_dirent_t* out;
    int             max;
    int             count;
} list_ctx;

static int list_cb(dirent_t* e, unsigned int chunk, int idx, void* ctx) {
    (void)chunk;
    (void)idx;

    unsigned char first = (unsigned char)e->name[0];

    if (first == 0xE5) return 0;
    if (e->attr == ATTR_LFN) return 0;
    if (e->attr & ATTR_VOLUME_ID) return 0;

    list_ctx* lc = (list_ctx*)ctx;

    if (lc->count >= lc->max) return -1;

    fat32_dirent_t* d = &lc->out[lc->count++];

    format_83(e->name, d->name);

    d->size = e->size;
    d->attr = e->attr;

    unsigned int c = entry_cluster(e);

    d->first_cluster = (c == root_cluster) ? 0 : c;

    return 0;
}

int fat32_list_dir(unsigned int dir_cluster, fat32_dirent_t* out, int max) {
    list_ctx lc = { out, max, 0 };

    dir_each(dir_cluster, list_cb, &lc);

    return lc.count;
}

int fat32_list_root(fat32_dirent_t* out, int max) {
    return fat32_list_dir(cwd_cluster, out, max);
}

// ============================================================
// File read
// ============================================================

int fat32_read_file(const char* path, void* buf, unsigned int max) {
    unsigned int parent;
    char basename[64];

    if (fat32_resolve_parent(path, &parent, basename) < 0) {
        return -1;
    }

    char raw83[11];

    if (build_83(basename, raw83) < 0) {
        return -1;
    }

    dirent_t e;

    if (dir_find(parent, raw83, &e, 0, 0) < 0) {
        return -1;
    }

    if (e.attr & ATTR_DIRECTORY) {
        return -1;
    }

    unsigned int remaining = (e.size < max) ? e.size : max;
    unsigned int cluster = entry_cluster(&e);
    unsigned char* dst = (unsigned char*)buf;
    unsigned int read_so_far = 0;

    unsigned char* cbuf = (unsigned char*)kmalloc(bytes_per_cluster);

    if (!cbuf) return -1;

    while (remaining > 0 && cluster >= 2 && !is_end_of_chain(cluster)) {
        if (fat_read(cluster_to_lba(cluster),
                     bpb.sectors_per_cluster,
                     cbuf) < 0) {
            break;
        }

        unsigned int n = (remaining < bytes_per_cluster)
                       ? remaining
                       : bytes_per_cluster;

        memcpy(dst + read_so_far, cbuf, n);

        read_so_far += n;
        remaining -= n;

        cluster = fat32_get_entry(cluster);
    }

    kfree(cbuf);

    return (int)read_so_far;
}

// ============================================================
// File write
// ============================================================

int fat32_write_file(const char* path, const void* data, unsigned int size) {
    unsigned int parent;
    char basename[64];

    if (fat32_resolve_parent(path, &parent, basename) < 0) {
        return -1;
    }

    char raw83[11];

    if (build_83(basename, raw83) < 0) {
        return -1;
    }

    dirent_t existing;
    unsigned int ex_chunk;
    int ex_idx;

    int exists = (dir_find(parent,
                           raw83,
                           &existing,
                           &ex_chunk,
                           &ex_idx) == 0);

    if (exists && (existing.attr & ATTR_DIRECTORY)) {
        return -1;
    }

    if (exists) {
        unsigned int ec = entry_cluster(&existing);

        if (ec >= 2) {
            fat32_free_chain(ec);
        }
    }

    unsigned int first = 0;
    unsigned int last = 0;
    unsigned int written = 0;

    const unsigned char* src = (const unsigned char*)data;

    unsigned char* cbuf = (unsigned char*)kmalloc(bytes_per_cluster);

    if (!cbuf) return -1;

    while (written < size) {
        unsigned int c = fat32_alloc_cluster();

        if (!c) {
            kfree(cbuf);
            return -1;
        }

        if (!first) {
            first = c;
            last = c;
        } else {
            fat32_set_entry(last, c);
            last = c;
        }

        fat32_set_entry(c, FAT32_EOC);

        unsigned int n = size - written;

        if (n > bytes_per_cluster) n = bytes_per_cluster;

        memcpy(cbuf, src + written, n);

        if (n < bytes_per_cluster) {
            memset(cbuf + n, 0, bytes_per_cluster - n);
        }

        if (fat_write(cluster_to_lba(c),
                      bpb.sectors_per_cluster,
                      cbuf) < 0) {
            kfree(cbuf);
            return -1;
        }

        written += n;
    }

    kfree(cbuf);

    dirent_t ne;

    memset(&ne, 0, sizeof(ne));
    memcpy(ne.name, raw83, 11);

    ne.attr = ATTR_ARCHIVE;
    ne.size = size;
    ne.cluster_lo = first & 0xFFFF;
    ne.cluster_hi = (first >> 16) & 0xFFFF;

    if (exists) {
        if (dir_put_entry(parent, ex_chunk, ex_idx, &ne) < 0) {
            return -1;
        }
    } else {
        unsigned int slot_chunk;
        int slot_idx;

        if (dir_alloc_slot(parent, &slot_chunk, &slot_idx) < 0) {
            return -1;
        }

        if (dir_put_entry(parent, slot_chunk, slot_idx, &ne) < 0) {
            return -1;
        }
    }

    if (fat32_flush_fat() < 0) return -1;

    return (int)size;
}

// ============================================================
// File delete
// ============================================================

int fat32_delete_file(const char* path) {
    unsigned int parent;
    char basename[64];

    if (fat32_resolve_parent(path, &parent, basename) < 0) {
        return -1;
    }

    char raw83[11];

    if (build_83(basename, raw83) < 0) {
        return -1;
    }

    dirent_t e;
    unsigned int chunk;
    int idx;

    if (dir_find(parent, raw83, &e, &chunk, &idx) < 0) {
        return -1;
    }

    if (e.attr & ATTR_DIRECTORY) {
        return -1;
    }

    unsigned int c = entry_cluster(&e);

    if (c >= 2) {
        fat32_free_chain(c);
    }

    dir_delete_slot(chunk, idx);

    if (fat32_flush_fat() < 0) return -1;

    return 0;
}

// ============================================================
// Directory create / remove
// ============================================================

int fat32_mkdir(const char* path) {
    unsigned int parent;
    char basename[64];

    if (fat32_resolve_parent(path, &parent, basename) < 0) {
        return -1;
    }

    char raw83[11];

    if (build_83(basename, raw83) < 0) {
        return -1;
    }

    dirent_t dummy;

    if (dir_find(parent, raw83, &dummy, 0, 0) == 0) {
        return -1;
    }

    unsigned int c = fat32_alloc_cluster();

    if (!c) return -1;

    unsigned char* buf = dir_alloc_chunk();

    if (!buf) return -1;

    memset(buf, 0, bytes_per_cluster);

    dirent_t* entries = (dirent_t*)buf;

    memset(entries[0].name, ' ', 11);
    entries[0].name[0] = '.';
    entries[0].attr = ATTR_DIRECTORY;
    entries[0].cluster_lo = c & 0xFFFF;
    entries[0].cluster_hi = (c >> 16) & 0xFFFF;

    memset(entries[1].name, ' ', 11);
    entries[1].name[0] = '.';
    entries[1].name[1] = '.';
    entries[1].attr = ATTR_DIRECTORY;

    unsigned int pp = (parent == 0) ? root_cluster : parent;

    entries[1].cluster_lo = pp & 0xFFFF;
    entries[1].cluster_hi = (pp >> 16) & 0xFFFF;

    int r = fat_write(cluster_to_lba(c),
                      bpb.sectors_per_cluster,
                      buf);

    kfree(buf);

    if (r < 0) {
        fat32_set_entry(c, FAT32_FREE);
        return -1;
    }

    dirent_t ne;

    memset(&ne, 0, sizeof(ne));
    memcpy(ne.name, raw83, 11);

    ne.attr = ATTR_DIRECTORY;
    ne.cluster_lo = c & 0xFFFF;
    ne.cluster_hi = (c >> 16) & 0xFFFF;

    unsigned int slot_chunk;
    int slot_idx;

    if (dir_alloc_slot(parent, &slot_chunk, &slot_idx) < 0) {
        fat32_set_entry(c, FAT32_FREE);
        return -1;
    }

    if (dir_put_entry(parent, slot_chunk, slot_idx, &ne) < 0) {
        fat32_set_entry(c, FAT32_FREE);
        return -1;
    }

    if (fat32_flush_fat() < 0) return -1;

    return 0;
}

typedef struct {
    int count;
} rmdir_count_ctx;

static int rmdir_count_cb(dirent_t* e,
                          unsigned int chunk,
                          int idx,
                          void* ctx) {
    (void)chunk;
    (void)idx;

    unsigned char first = (unsigned char)e->name[0];

    if (first == 0xE5 || first == 0x00) return 0;
    if (e->attr == ATTR_LFN) return 0;
    if (e->name[0] == '.') return 0;

    rmdir_count_ctx* rc = (rmdir_count_ctx*)ctx;

    rc->count++;

    return -1;
}

int fat32_rmdir(const char* path) {
    unsigned int parent;
    char basename[64];

    if (fat32_resolve_parent(path, &parent, basename) < 0) {
        return -1;
    }

    if (basename[0] == '.' &&
        (basename[1] == '\0' ||
        (basename[1] == '.' && basename[2] == '\0'))) {
        return -1;
    }

    char raw83[11];

    if (build_83(basename, raw83) < 0) {
        return -1;
    }

    dirent_t e;
    unsigned int chunk;
    int idx;

    if (dir_find(parent, raw83, &e, &chunk, &idx) < 0) {
        return -1;
    }

    if (!(e.attr & ATTR_DIRECTORY)) {
        return -1;
    }

    unsigned int dir_c = entry_cluster(&e);

    rmdir_count_ctx rc = { 0 };

    dir_each(dir_c, rmdir_count_cb, &rc);

    if (rc.count > 0) return -1;

    fat32_free_chain(dir_c);
    dir_delete_slot(chunk, idx);

    if (fat32_flush_fat() < 0) return -1;

    return 0;
}

// ============================================================
// Copy / Move
// ============================================================

int fat32_cp(const char* src, const char* dst) {
    unsigned int s_parent;
    char s_base[64];

    if (fat32_resolve_parent(src, &s_parent, s_base) < 0) {
        return -1;
    }

    char s_raw[11];

    if (build_83(s_base, s_raw) < 0) {
        return -1;
    }

    dirent_t se;

    if (dir_find(s_parent, s_raw, &se, 0, 0) < 0) {
        return -1;
    }

    if (se.attr & ATTR_DIRECTORY) {
        return -1;
    }

    unsigned char* buf = (unsigned char*)kmalloc(se.size ? se.size : 1);

    if (!buf) return -1;

    int n = fat32_read_file(src, buf, se.size);

    if (n < 0) {
        kfree(buf);
        return -1;
    }

    char dst_path[256];
    unsigned int dst_dir;

    if (fat32_resolve_dir(dst, &dst_dir) == 0) {
        unsigned int dlen = strlen(dst);
        unsigned int blen = strlen(s_base);

        if (dlen + 1 + blen >= 256) {
            kfree(buf);
            return -1;
        }

        memcpy(dst_path, dst, dlen);
        dst_path[dlen] = '/';
        memcpy(dst_path + dlen + 1, s_base, blen + 1);
    } else {
        unsigned int dlen = strlen(dst);

        if (dlen >= 256) {
            kfree(buf);
            return -1;
        }

        memcpy(dst_path, dst, dlen + 1);
    }

    int r = fat32_write_file(dst_path, buf, (unsigned int)n);

    kfree(buf);

    return (r < 0) ? -1 : 0;
}

int fat32_mv(const char* src, const char* dst) {
    unsigned int s_parent;
    char s_base[64];

    if (fat32_resolve_parent(src, &s_parent, s_base) < 0) {
        return -1;
    }

    char s_raw[11];

    if (build_83(s_base, s_raw) < 0) {
        return -1;
    }

    dirent_t se;
    unsigned int s_chunk;
    int s_idx;

    if (dir_find(s_parent, s_raw, &se, &s_chunk, &s_idx) < 0) {
        return -1;
    }

    if (se.attr & ATTR_DIRECTORY) {
        return -1;
    }

    char dst_final[256];
    unsigned int dst_dir_check;

    if (fat32_resolve_dir(dst, &dst_dir_check) == 0) {
        unsigned int dlen = strlen(dst);
        unsigned int blen = strlen(s_base);

        if (dlen + 1 + blen >= 256) {
            return -1;
        }

        memcpy(dst_final, dst, dlen);
        dst_final[dlen] = '/';
        memcpy(dst_final + dlen + 1, s_base, blen + 1);
    } else {
        unsigned int dlen = strlen(dst);

        if (dlen >= 256) {
            return -1;
        }

        memcpy(dst_final, dst, dlen + 1);
    }

    unsigned int d_parent;
    char d_base[64];

    if (fat32_resolve_parent(dst_final, &d_parent, d_base) < 0) {
        return -1;
    }

    char d_raw[11];

    if (build_83(d_base, d_raw) < 0) {
        return -1;
    }

    dirent_t ne = se;

    memcpy(ne.name, d_raw, 11);

    unsigned int slot_chunk;
    int slot_idx;

    if (dir_alloc_slot(d_parent, &slot_chunk, &slot_idx) < 0) {
        return -1;
    }

    if (dir_put_entry(d_parent, slot_chunk, slot_idx, &ne) < 0) {
        return -1;
    }

    if (dir_delete_slot(s_chunk, s_idx) < 0) {
        return -1;
    }

    if (se.attr & ATTR_DIRECTORY) {
        char dotdot[11];

        build_83("..", dotdot);

        dirent_t dde;
        unsigned int dd_chunk;
        int dd_idx;
        unsigned int dir_c = entry_cluster(&se);

        if (dir_find(dir_c, dotdot, &dde, &dd_chunk, &dd_idx) == 0) {
            dde.cluster_lo = d_parent & 0xFFFF;
            dde.cluster_hi = (d_parent >> 16) & 0xFFFF;
            dir_put_entry(dir_c, dd_chunk, dd_idx, &dde);
        }
    }

    if (fat32_flush_fat() < 0) return -1;

    return 0;
}

// ============================================================
// VFS adapters
// ============================================================

int fat32_vfs_list(const char* path, fat32_dirent_t* out, int max) {
    unsigned int dir;

    if (fat32_resolve_dir(path, &dir) < 0) {
        return -1;
    }

    return fat32_list_dir(dir, out, max);
}

int fat32_vfs_is_dir(const char* path) {
    unsigned int dir;

    return (fat32_resolve_dir(path, &dir) == 0) ? 1 : 0;
}