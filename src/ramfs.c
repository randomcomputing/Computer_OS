#include "ramfs.h"
#include "vfs.h"
#include "kheap.h"
#include "string.h"

// Flat in-memory filesystem mounted at /tmp.
// Names support up to 255 chars (LFN-compatible).
// File data is heap-allocated so we don't waste BSS for unused slots.

#define RAMFS_MAX_FILES  64
#define RAMFS_MAX_SIZE   (4 * 1024 * 1024)  // 4 MB per file hard cap
#define RAMFS_NAME_MAX   256

typedef struct {
    int           used;
    char          name[RAMFS_NAME_MAX];
    unsigned int  size;
    unsigned char* data;   // heap-allocated, NULL when unused
} ramfs_file_t;

static ramfs_file_t files[RAMFS_MAX_FILES];

void ramfs_init(void) {
    for (int i = 0; i < RAMFS_MAX_FILES; i++) {
        files[i].used = 0;
        files[i].data = 0;
    }
}

static const char* bare(const char* path) {
    if (path[0] == '/') return path + 1;
    return path;
}

static int is_root(const char* path) {
    return bare(path)[0] == '\0';
}

static ramfs_file_t* find(const char* name) {
    for (int i = 0; i < RAMFS_MAX_FILES; i++)
        if (files[i].used && strcmp(files[i].name, name) == 0)
            return &files[i];
    return 0;
}

static int ramfs_list(const char* path, vfs_dirent_t* out, int max) {
    if (!is_root(path)) return -1;
    int n = 0;
    for (int i = 0; i < RAMFS_MAX_FILES && n < max; i++) {
        if (!files[i].used) continue;
        unsigned int k = 0;
        for (; files[i].name[k] && k < sizeof(out[n].name) - 1; k++)
            out[n].name[k] = files[i].name[k];
        out[n].name[k] = '\0';
        out[n].size          = files[i].size;
        out[n].first_cluster = 0;
        out[n].attr          = 0;
        n++;
    }
    return n;
}

static int ramfs_is_dir(const char* path) {
    return is_root(path) ? 1 : 0;
}

static int ramfs_read_file(const char* path, void* buf, unsigned int max) {
    if (is_root(path)) return -1;
    ramfs_file_t* f = find(bare(path));
    if (!f || !f->data) return -1;
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
        if (!f) return -1;
        f->used = 1;
        f->data = 0;
        f->size = 0;
        unsigned int k = 0;
        for (; name[k]; k++) f->name[k] = name[k];
        f->name[k] = '\0';
    }

    /* Realloc: free old buffer if size changed */
    if (f->data) { kfree(f->data); f->data = 0; }
    f->data = (unsigned char*)kmalloc(size ? size : 1);
    if (!f->data) { f->used = 0; return -1; }

    memcpy(f->data, data, size);
    f->size = size;
    return (int)size;
}

static int ramfs_delete_file(const char* path) {
    if (is_root(path)) return -1;
    ramfs_file_t* f = find(bare(path));
    if (!f) return -1;
    if (f->data) { kfree(f->data); f->data = 0; }
    f->used = 0;
    return 0;
}

static int ramfs_mkdir(const char* path) { (void)path; return -1; }
static int ramfs_rmdir(const char* path) { (void)path; return -1; }

static int ramfs_cp(const char* src, const char* dst) {
    ramfs_file_t* f = find(bare(src));
    if (!f || !f->data) return -1;
    return ramfs_write_file(dst, f->data, f->size);
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