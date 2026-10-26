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

// Symlink representation at the VFS boundary. A read of a symlink returns
// this prefix followed by the target path; writing content with this
// prefix creates a symlink. FAT32 and ramfs store it literally (they have
// no symlinks); ext2 translates it to/from native symlinks. Must match
// SYMLINK_MAGIC in linux_syscall.c.
#define VFS_SYMLINK_MAGIC     "!<symlink>"
#define VFS_SYMLINK_MAGIC_LEN 10

// One directory entry returned by vfs_list().
typedef struct {
    char          name[256];      // LFN-capable name, null-terminated
    unsigned int  size;           // file size in bytes (0 for directories)
    unsigned int  first_cluster;  // starting cluster (filesystem-internal)
    unsigned char attr;           // FAT-style attribute byte (0x10 = directory)
} vfs_dirent_t;

// Operations a filesystem provides. All paths are absolute *within the
// mount*. A NULL pointer means "unsupported" and the VFS returns -1.
typedef struct vfs_ops {
    int (*list)(const char* path, vfs_dirent_t* out, int max);
    int (*is_dir)(const char* path);
    int (*read_file)(const char* path, void* buf, unsigned int max);
    int (*read_file_at)(const char* path, void* buf, unsigned int offset, unsigned int len);
    int (*write_file)(const char* path, const void* data, unsigned int size);
    int (*delete_file)(const char* path);
    int (*mkdir)(const char* path);
    int (*rmdir)(const char* path);
    int (*cp)(const char* src, const char* dst);
    int (*mv)(const char* src, const char* dst);
    int (*link)(const char* oldpath, const char* newpath);  /* hard link */
    int (*chmod)(const char* path, unsigned int mode);
    int (*chown)(const char* path, unsigned int uid, unsigned int gid);
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
int  vfs_read_at  (const char* path, void* buf, unsigned int offset, unsigned int len);
int  vfs_write_file(const char* path, const void* data, unsigned int size);
int  vfs_delete_file(const char* path);
int  vfs_mkdir(const char* path);
int  vfs_rmdir(const char* path);
int  vfs_cp(const char* src, const char* dst);
int  vfs_mv(const char* src, const char* dst);
int  vfs_link(const char* oldpath, const char* newpath);  /* same mount only */
int  vfs_chmod(const char* path, unsigned int mode);
int  vfs_chown(const char* path, unsigned int uid, unsigned int gid);

// ---- working directory (VFS-owned, path-based) ------------------------
int  vfs_chdir(const char* path);
int  vfs_getcwd(char* out, unsigned int max);

// Normalize `path` (relative to the cwd) into an absolute path.
// Returns 0 on success, -1 if out is too small.
int  vfs_resolve(const char* path, char* out, unsigned int outsz);

// ---- filesystem bindings -----------------------------------------------
const vfs_ops_t* fat32_vfs_ops(void);

#endif