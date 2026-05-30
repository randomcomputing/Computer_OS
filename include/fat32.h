#ifndef FAT32_H
#define FAT32_H

// Read/write FAT32 driver.
//
// FAT32 layout (sector 0 onward):
//
//   [boot sector + BPB]      sector 0
//   [FSInfo sector]          usually sector 1
//   [reserved sectors]       BPB.reserved_sectors total (often 32)
//   [FAT 1]                  cluster allocation table (32-bit entries)
//   [FAT 2]                  redundant copy
//   [data area]              cluster 2 onward; root dir is just a normal
//                            cluster chain starting at BPB.root_cluster
//
// Unlike FAT12/16 there is no fixed-size root directory region. The root
// is a regular directory whose first cluster is stored in the BPB. Every
// directory cluster holds 512/32 = 16 entries per sector.
//
// FAT entries are 28 bits (upper 4 bits reserved). End-of-chain is
// >= 0x0FFFFFF8. Free cluster = 0x00000000.

// One row of `ls`. Same layout as fat12_dirent_t so the VFS can share
// vfs_dirent_t across both drivers without any casting.
typedef struct {
    char          name[13];        // "FOO     TXT" -> "FOO.TXT", null-terminated
    unsigned int  size;            // file size in bytes (0 for directories)
    unsigned int  first_cluster;   // starting cluster in the FAT chain
    unsigned char attr;            // FAT attribute byte (0x10 = directory, etc.)
} fat32_dirent_t;

// Mount the FAT32 filesystem on the primary ATA disk.
// Returns 0 on success, -1 if the disk has no FAT32 BPB or is unreadable.
int fat32_mount(void);

// ---- Directory listing & path resolution --------------------------------

// List a directory by cluster. Pass dir_cluster == 0 to list the root.
// Skips deleted, volume-label, and LFN entries.
// Returns the number of entries written, or -1 on error.
int fat32_list_dir(unsigned int dir_cluster, fat32_dirent_t* out, int max);

// List the current working directory.
int fat32_list_root(fat32_dirent_t* out, int max);

// Resolve a path string to a directory cluster. Absolute paths start with
// '/'; relative paths are resolved against the cwd. "" and "." -> cwd.
// On success writes the resolved cluster (0 = root alias) to *out and
// returns 0. Returns -1 if any component is missing or not a directory.
int fat32_resolve_dir(const char* path, unsigned int* out);

// Split a path into (parent dir cluster, basename).
// "/foo/bar.txt" -> parent = cluster of "/foo", basename = "bar.txt".
// "bar.txt"      -> parent = cwd,               basename = "bar.txt".
// basename_out must be at least 64 bytes.
// Returns 0 on success, -1 if the parent directory doesn't exist.
int fat32_resolve_parent(const char* path,
                         unsigned int* parent_out,
                         char* basename_out);

// ---- Working directory --------------------------------------------------

// Cluster of the current working directory. 0 = root (an alias for the
// real root_cluster stored in the BPB).
unsigned int fat32_cwd(void);

// Change the working directory. Absolute or relative, including "..".
// Returns 0 on success, -1 on error.
int fat32_chdir(const char* path);

// Write the absolute path of the cwd into `out`. Always begins with '/'.
// Returns bytes written (excluding null), or -1 if out is too small.
int fat32_getcwd(char* out, unsigned int max);

// ---- File operations ---------------------------------------------------
//
// All four accept paths with '/' separators, absolute or relative to cwd.

// Read a file by path. Returns bytes read, or -1.
int fat32_read_file(const char* path, void* buf, unsigned int max);

// Create or overwrite a file. Returns size on success, -1 on error.
int fat32_write_file(const char* path, const void* data, unsigned int size);

// Delete a regular file. Returns 0 on success, -1 if missing or a dir.
int fat32_delete_file(const char* path);

// ---- Directory operations ----------------------------------------------

// Create a directory. Parents must already exist.
// Returns 0 on success, -1 on error.
int fat32_mkdir(const char* path);

// Remove an *empty* directory. Refuses "." and "..".
// Returns 0 on success, -1 on error.
int fat32_rmdir(const char* path);

// ---- Copy / move --------------------------------------------------------

// Both treat the destination as a file path unless it resolves to an
// existing directory, in which case the source basename is used inside it.
// Refuse to copy/move directories (mkdir + cp + rm for that).
int fat32_cp(const char* src, const char* dst);
int fat32_mv(const char* src, const char* dst);

// ---- VFS adapters (path-based wrappers used by the VFS layer) ----------
int fat32_vfs_list(const char* path, fat32_dirent_t* out, int max);
int fat32_vfs_is_dir(const char* path);

#endif