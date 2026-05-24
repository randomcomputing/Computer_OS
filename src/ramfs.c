#include "ramfs.h"
#include "vfs.h"
#include "string.h"

// Flat, fixed-capacity in-memory filesystem. Paths handed in by the VFS are
// absolute within this mount, e.g. "/" (the root dir) or "/foo.txt". We
// support no subdirectories: every file lives directly in the root.

#define RAMFS_MAX_FILES 32
#define RAMFS_MAX_SIZE  8192      // per-file byte cap
#define RAMFS_NAME_MAX  13        // 8.3 + dot + null, matches dirent

typedef struct {
    int          used;
    char         name[RAMFS_NAME_MAX];   // bare name, no leading slash
    unsigned int size;
    unsigned char data[RAMFS_MAX_SIZE];
} ramfs_file_t;

static ramfs_file_t files[RAMFS_MAX_FILES];

void ramfs_init(void) {
    for (int i = 0; i < RAMFS_MAX_FILES; i++) files[i].used = 0;
}

// Strip the leading '/' from an in-mount path to get a bare filename.
// "/foo.txt" -> "foo.txt". "/" -> "" (the root directory itself).
static const char* bare(const char* path) {
    if (path[0] == '/') return path + 1;
    return path;
}

// Is this the root directory path ("/" or "")?
static int is_root(const char* path) {
    const char* b = bare(path);
    return b[0] == '\0';
}

static ramfs_file_t* find(const char* name) {
    for (int i = 0; i < RAMFS_MAX_FILES; i++)
        if (files[i].used && strcmp(files[i].name, name) == 0)
            return &files[i];
    return 0;
}

// ---- vfs_ops implementations ------------------------------------------

static int ramfs_list(const char* path, vfs_dirent_t* out, int max) {
    if (!is_root(path)) return -1;     // only the root dir exists
    int n = 0;
    for (int i = 0; i < RAMFS_MAX_FILES && n < max; i++) {
        if (!files[i].used) continue;
        // Copy name into the dirent.
        unsigned int k = 0;
        for (; files[i].name[k] && k < sizeof(out[n].name) - 1; k++)
            out[n].name[k] = files[i].name[k];
        out[n].name[k] = '\0';
        out[n].size = files[i].size;
        out[n].first_cluster = 0;
        out[n].attr = 0;               // regular file
        n++;
    }
    return n;
}

static int ramfs_is_dir(const char* path) {
    return is_root(path) ? 1 : 0;      // only the root is a directory
}

static int ramfs_read_file(const char* path, void* buf, unsigned int max) {
    if (is_root(path)) return -1;
    ramfs_file_t* f = find(bare(path));
    if (!f) return -1;
    unsigned int n = f->size < max ? f->size : max;
    memcpy(buf, f->data, n);
    return (int)n;
}

static int ramfs_write_file(const char* path, const void* data, unsigned int size) {
    if (is_root(path)) return -1;
    if (size > RAMFS_MAX_SIZE) return -1;
    const char* name = bare(path);
    if (strlen(name) >= RAMFS_NAME_MAX) return -1;

    ramfs_file_t* f = find(name);
    if (!f) {
        for (int i = 0; i < RAMFS_MAX_FILES; i++) {
            if (!files[i].used) { f = &files[i]; break; }
        }
        if (!f) return -1;             // filesystem full
        f->used = 1;
        unsigned int k = 0;
        for (; name[k]; k++) f->name[k] = name[k];
        f->name[k] = '\0';
    }
    memcpy(f->data, data, size);
    f->size = size;
    return (int)size;
}

static int ramfs_delete_file(const char* path) {
    if (is_root(path)) return -1;
    ramfs_file_t* f = find(bare(path));
    if (!f) return -1;
    f->used = 0;
    return 0;
}

// No subdirectories, so mkdir/rmdir are unsupported.
static int ramfs_mkdir(const char* path) { (void)path; return -1; }
static int ramfs_rmdir(const char* path) { (void)path; return -1; }

// Same-mount cp/mv implemented in terms of read/write/delete.
static int ramfs_cp(const char* src, const char* dst) {
    static unsigned char tmp[RAMFS_MAX_SIZE];
    int n = ramfs_read_file(src, tmp, sizeof(tmp));
    if (n < 0) return -1;
    return ramfs_write_file(dst, tmp, (unsigned int)n);
}
static int ramfs_mv(const char* src, const char* dst) {
    if (ramfs_cp(src, dst) < 0) return -1;
    return ramfs_delete_file(src);
}

static const vfs_ops_t ramfs_ops = {
    .list        = ramfs_list,
    .is_dir      = ramfs_is_dir,
    .read_file   = ramfs_read_file,
    .write_file  = ramfs_write_file,
    .delete_file = ramfs_delete_file,
    .mkdir       = ramfs_mkdir,
    .rmdir       = ramfs_rmdir,
    .cp          = ramfs_cp,
    .mv          = ramfs_mv,
};

const vfs_ops_t* ramfs_vfs_ops(void) { return &ramfs_ops; }