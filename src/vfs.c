#include "vfs.h"
#include "fat12.h"
#include "string.h"

// ---- mount table -------------------------------------------------------
#define VFS_MAX_MOUNTS 8
static vfs_mount_t mounts[VFS_MAX_MOUNTS];

// The VFS-owned current working directory, always a normalized absolute
// path with no trailing slash (except "/" itself).
static char cwd[256] = "/";

void vfs_init(void) {
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        mounts[i].in_use = 0;
        mounts[i].prefix = 0;
        mounts[i].ops    = 0;
        mounts[i].name   = 0;
    }
    cwd[0] = '/';
    cwd[1] = '\0';
}

int vfs_mount(const char* prefix, const vfs_ops_t* ops, const char* name) {
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!mounts[i].in_use) {
            mounts[i].prefix = prefix;
            mounts[i].ops    = ops;
            mounts[i].name   = name;
            mounts[i].in_use = 1;
            return 0;
        }
    }
    return -1;
}

int vfs_mount_count(void) {
    int n = 0;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) if (mounts[i].in_use) n++;
    return n;
}

const vfs_mount_t* vfs_mount_at(int i) {
    if (i < 0 || i >= VFS_MAX_MOUNTS) return 0;
    if (!mounts[i].in_use) return 0;
    return &mounts[i];
}

// ---- path normalization -----------------------------------------------
int vfs_resolve(const char* path, char* out, unsigned int outsz) {
    char buf[256];
    unsigned int len = 0;

    if (path[0] != '/') {
        for (const char* s = cwd; *s; s++) {
            if (len + 1 >= sizeof(buf)) return -1;
            buf[len++] = *s;
        }
    }
    if (len + 1 >= sizeof(buf)) return -1;
    buf[len++] = '/';
    for (const char* s = path; *s; s++) {
        if (len + 1 >= sizeof(buf)) return -1;
        buf[len++] = *s;
    }
    buf[len] = '\0';

    unsigned int op = 0;
    out[0] = '\0';
    unsigned int starts[64];
    int depth = 0;
    unsigned int i = 0;

    while (i <= len) {
        if (buf[i] == '/' || buf[i] == '\0') { i++; continue; }
        unsigned int j = i;
        while (j < len && buf[j] != '/') j++;
        unsigned int clen = j - i;

        if (clen == 1 && buf[i] == '.') {
            // skip
        } else if (clen == 2 && buf[i] == '.' && buf[i+1] == '.') {
            if (depth > 0) { depth--; op = starts[depth]; out[op] = '\0'; }
        } else {
            if (depth >= 64) return -1;
            starts[depth++] = op;
            if (op + 1 + clen + 1 >= outsz) return -1;
            out[op++] = '/';
            for (unsigned int k = 0; k < clen; k++) out[op++] = buf[i+k];
            out[op] = '\0';
        }
        i = j + 1;
    }

    if (op == 0) {
        if (outsz < 2) return -1;
        out[0] = '/'; out[1] = '\0';
    }
    return 0;
}

// ---- mount routing ----------------------------------------------------
static int under(const char* prefix, const char* path) {
    if (strcmp(prefix, "/") == 0) return 1;
    unsigned int pl = strlen(prefix);
    if (strncmp(path, prefix, pl) != 0) return 0;
    return path[pl] == '/' || path[pl] == '\0';
}

static const vfs_mount_t* route(const char* abspath, char* sub, unsigned int subsz) {
    int best = -1; unsigned int bestlen = 0;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!mounts[i].in_use) continue;
        if (under(mounts[i].prefix, abspath)) {
            unsigned int pl = strlen(mounts[i].prefix);
            if (best < 0 || pl > bestlen) { best = i; bestlen = pl; }
        }
    }
    if (best < 0) return 0;

    const char* rest;
    if (strcmp(mounts[best].prefix, "/") == 0) rest = abspath;
    else rest = abspath + strlen(mounts[best].prefix);

    if (rest[0] == '\0') { sub[0] = '/'; sub[1] = '\0'; }
    else {
        unsigned int o = 0;
        if (rest[0] != '/') { if (subsz < 2) return 0; sub[o++] = '/'; }
        for (const char* s = rest; *s; s++) {
            if (o + 1 >= subsz) return 0;
            sub[o++] = *s;
        }
        sub[o] = '\0';
    }
    return &mounts[best];
}

static const vfs_mount_t* resolve_route(const char* path, char* sub, unsigned int subsz) {
    char abspath[256];
    if (vfs_resolve(path, abspath, sizeof(abspath)) < 0) return 0;
    return route(abspath, sub, subsz);
}

// ---- dispatched operations --------------------------------------------
int vfs_list(const char* path, vfs_dirent_t* out, int max) {
    char sub[256];
    const char* p = (path && path[0]) ? path : cwd;
    const vfs_mount_t* m = resolve_route(p, sub, sizeof(sub));
    if (!m || !m->ops->list) return -1;
    return m->ops->list(sub, out, max);
}

int vfs_read_file(const char* path, void* buf, unsigned int max) {
    char sub[256];
    const vfs_mount_t* m = resolve_route(path, sub, sizeof(sub));
    if (!m || !m->ops->read_file) return -1;
    return m->ops->read_file(sub, buf, max);
}

int vfs_write_file(const char* path, const void* data, unsigned int size) {
    char sub[256];
    const vfs_mount_t* m = resolve_route(path, sub, sizeof(sub));
    if (!m || !m->ops->write_file) return -1;
    return m->ops->write_file(sub, data, size);
}

int vfs_delete_file(const char* path) {
    char sub[256];
    const vfs_mount_t* m = resolve_route(path, sub, sizeof(sub));
    if (!m || !m->ops->delete_file) return -1;
    return m->ops->delete_file(sub);
}

int vfs_mkdir(const char* path) {
    char sub[256];
    const vfs_mount_t* m = resolve_route(path, sub, sizeof(sub));
    if (!m || !m->ops->mkdir) return -1;
    return m->ops->mkdir(sub);
}

int vfs_rmdir(const char* path) {
    char sub[256];
    const vfs_mount_t* m = resolve_route(path, sub, sizeof(sub));
    if (!m || !m->ops->rmdir) return -1;
    return m->ops->rmdir(sub);
}

int vfs_cp(const char* src, const char* dst) {
    char ssub[256], dsub[256];
    const vfs_mount_t* sm = resolve_route(src, ssub, sizeof(ssub));
    const vfs_mount_t* dm = resolve_route(dst, dsub, sizeof(dsub));
    if (!sm || !dm) return -1;
    if (sm == dm) {
        if (!sm->ops->cp) return -1;
        return sm->ops->cp(ssub, dsub);
    }
    static unsigned char xbuf[65536];
    if (!sm->ops->read_file || !dm->ops->write_file) return -1;
    int n = sm->ops->read_file(ssub, xbuf, sizeof(xbuf));
    if (n < 0) return -1;
    return dm->ops->write_file(dsub, xbuf, (unsigned int)n);
}

int vfs_mv(const char* src, const char* dst) {
    char ssub[256], dsub[256];
    const vfs_mount_t* sm = resolve_route(src, ssub, sizeof(ssub));
    const vfs_mount_t* dm = resolve_route(dst, dsub, sizeof(dsub));
    if (!sm || !dm) return -1;
    if (sm == dm) {
        if (!sm->ops->mv) return -1;
        return sm->ops->mv(ssub, dsub);
    }
    if (vfs_cp(src, dst) < 0) return -1;
    if (!sm->ops->delete_file) return -1;
    return sm->ops->delete_file(ssub);
}

// ---- working directory ------------------------------------------------
int vfs_chdir(const char* path) {
    char abspath[256], sub[256];
    if (vfs_resolve(path, abspath, sizeof(abspath)) < 0) return -1;
    const vfs_mount_t* m = route(abspath, sub, sizeof(sub));
    if (!m || !m->ops->is_dir) return -1;
    if (!m->ops->is_dir(sub)) return -1;
    unsigned int i = 0;
    for (; abspath[i] && i < sizeof(cwd) - 1; i++) cwd[i] = abspath[i];
    cwd[i] = '\0';
    return 0;
}

int vfs_getcwd(char* out, unsigned int max) {
    unsigned int i = 0;
    for (; cwd[i] && i < max - 1; i++) out[i] = cwd[i];
    if (i >= max) return -1;
    out[i] = '\0';
    return (int)i;
}

// ---- FAT12 shim --------------------------------------------------------
extern int fat12_vfs_list(const char* path, vfs_dirent_t* out, int max);
extern int fat12_vfs_is_dir(const char* path);

static const vfs_ops_t fat12_ops = {
    .list        = fat12_vfs_list,
    .is_dir      = fat12_vfs_is_dir,
    .read_file   = fat12_read_file,
    .write_file  = fat12_write_file,
    .delete_file = fat12_delete_file,
    .mkdir       = fat12_mkdir,
    .rmdir       = fat12_rmdir,
    .cp          = fat12_cp,
    .mv          = fat12_mv,
};

const vfs_ops_t* fat12_vfs_ops(void) { return &fat12_ops; }

// ---- FAT32 shim --------------------------------------------------------
extern int fat32_read_file(const char* path, void* buf, unsigned int max);
extern int fat32_write_file(const char* path, const void* data, unsigned int size);
extern int fat32_delete_file(const char* path);
extern int fat32_mkdir(const char* path);
extern int fat32_rmdir(const char* path);
extern int fat32_cp(const char* src, const char* dst);
extern int fat32_mv(const char* src, const char* dst);
extern int fat32_vfs_list(const char* path, vfs_dirent_t* out, int max);
extern int fat32_vfs_is_dir(const char* path);

static const vfs_ops_t fat32_ops = {
    .list        = fat32_vfs_list,
    .is_dir      = fat32_vfs_is_dir,
    .read_file   = fat32_read_file,
    .write_file  = fat32_write_file,
    .delete_file = fat32_delete_file,
    .mkdir       = fat32_mkdir,
    .rmdir       = fat32_rmdir,
    .cp          = fat32_cp,
    .mv          = fat32_mv,
};

const vfs_ops_t* fat32_vfs_ops(void) { return &fat32_ops; }