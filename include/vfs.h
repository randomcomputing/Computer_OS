#ifndef VFS_H
#define VFS_H

// Virtual File System layer — Phase 3 (multi-mount, VFS-owned cwd).
//
// The VFS owns the current working directory as an absolute string and
// routes every operation to the correct mounted filesystem by longest-
// prefix match. A filesystem mounted at "/tmp" only ever sees paths rooted
// at itself (e.g. "/x" for the user's "/tmp/x"); it doesn't know or care
// where it is mounted.
//
// Path model:
//   - The VFS normalizes user paths (absolute, or relative to its cwd),
//     collapsing "." and "..", before routing.
//   - Each filesystem's ops receive an absolute path *within that mount*.
//   - Listing and cwd are PATH-based (no cluster numbers leak through the
//     interface), so non-FAT filesystems such as ramfs work cleanly.

#include "fat12.h"   // for fat12_dirent_t, reused as the VFS dirent type

typedef fat12_dirent_t vfs_dirent_t;

// Operations a filesystem provides. All paths are absolute *within the
// mount*. A NULL pointer means "unsupported" and the VFS returns -1.
typedef struct vfs_ops {
    int (*list)(const char* path, vfs_dirent_t* out, int max);
    int (*is_dir)(const char* path);
    int (*read_file)(const char* path, void* buf, unsigned int max);
    int (*write_file)(const char* path, const void* data, unsigned int size);
    int (*delete_file)(const char* path);
    int (*mkdir)(const char* path);
    int (*rmdir)(const char* path);
    int (*cp)(const char* src, const char* dst);
    int (*mv)(const char* src, const char* dst);
} vfs_ops_t;

typedef struct vfs_mount {
    const char*      prefix;   // mount point, e.g. "/" or "/tmp"
    const vfs_ops_t* ops;
    const char*      name;
    int              in_use;
} vfs_mount_t;

void vfs_init(void);
int  vfs_mount(const char* prefix, const vfs_ops_t* ops, const char* name);
int                vfs_mount_count(void);
const vfs_mount_t* vfs_mount_at(int i);

// ---- path-based operations (route by normalized absolute path) --------
int  vfs_list(const char* path, vfs_dirent_t* out, int max);   // "" = cwd
int  vfs_read_file(const char* path, void* buf, unsigned int max);
int  vfs_write_file(const char* path, const void* data, unsigned int size);
int  vfs_delete_file(const char* path);
int  vfs_mkdir(const char* path);
int  vfs_rmdir(const char* path);
int  vfs_cp(const char* src, const char* dst);
int  vfs_mv(const char* src, const char* dst);

// ---- working directory (VFS-owned, path-based) ------------------------
int  vfs_chdir(const char* path);
int  vfs_getcwd(char* out, unsigned int max);

// Normalize `path` (relative to the cwd) into an absolute path.
// Returns 0 on success, -1 if out is too small.
int  vfs_resolve(const char* path, char* out, unsigned int outsz);

// ---- filesystem bindings -----------------------------------------------
const vfs_ops_t* fat12_vfs_ops(void);
const vfs_ops_t* fat32_vfs_ops(void);

#endif