#ifndef FAT12_H
#define FAT12_H

// Read/write FAT12 driver with subdirectory support.
//
// FAT12 layout (sector 0 onward):
//
//   [boot sector + BPB]   sector 0
//   [reserved sectors]    usually just the boot sector
//   [FAT 1]               cluster allocation table
//   [FAT 2]               redundant copy
//   [root directory]      fixed size, fixed location
//   [data area]           addressed in clusters of N sectors
//
// A "cluster" is N consecutive sectors. Each FAT entry is 12 bits and
// stores the *next* cluster of a chain, or >= 0xFF8 for end-of-chain.
//
// The root directory is special: it lives at a fixed LBA with a fixed
// entry count (no cluster chain). Subdirectories are normal files whose
// contents happen to be 32-byte directory entries; they grow by adding
// clusters to their chain. We expose a single API that handles both:
// throughout, a directory cluster of 0 means "the root directory".

// One row of `ls`. Pre-parsed 8.3 name + size + starting cluster + attrs
// so callers don't need to know the on-disk layout.
typedef struct {
    char         name[13];          // "FOO     TXT" -> "FOO.TXT", null-terminated
    unsigned int size;              // file size in bytes (0 for directories)
    unsigned int first_cluster;     // starting cluster in the FAT chain
    unsigned char attr;             // FAT attribute byte (0x10 = directory, etc.)
} fat12_dirent_t;

// Mount the FAT12 filesystem on the primary ATA disk.
int fat12_mount(void);

// ---- Directory listing & path resolution -------------------------------

// List a directory. Pass dir_cluster == 0 to list the root.
// Skips deleted, volume-label, and long-name entries.
// Returns the number of entries written, or -1 on error.
int fat12_list_dir(unsigned int dir_cluster, fat12_dirent_t* out, int max);

// List the *current* working directory. Source-compatible with the
// original API; equivalent to fat12_list_dir(fat12_cwd()).
int fat12_list_root(fat12_dirent_t* out, int max);

// Resolve a path string to a directory cluster.
// Absolute paths start with '/'. Relative paths are resolved against the
// current working directory. The empty string and "." resolve to the cwd.
// On success writes the resolved directory cluster (0 = root) to *out and
// returns 0. Returns -1 if any component is missing or not a directory.
int fat12_resolve_dir(const char* path, unsigned int* out);

// Split a path into (parent directory cluster, basename).
// "/foo/bar.txt" -> parent = cluster of "/foo", basename = "bar.txt".
// "bar.txt"      -> parent = cwd,                basename = "bar.txt".
// basename_out must be at least 64 bytes.
// Returns 0 on success, -1 if the parent directory doesn't exist.
int fat12_resolve_parent(const char* path,
                         unsigned int* parent_out,
                         char* basename_out);

// ---- Working directory -------------------------------------------------

// Cluster number of the current working directory. 0 = root.
unsigned int fat12_cwd(void);

// Change the working directory. Accepts absolute or relative paths,
// including "..". Returns 0 on success, -1 on error.
int fat12_chdir(const char* path);

// Write the absolute path of the cwd into `out`. Always begins with '/'.
// Returns the number of bytes written (excluding the null), or -1 if the
// buffer is too small or the directory chain is corrupt.
int fat12_getcwd(char* out, unsigned int max);

// ---- File operations ---------------------------------------------------
//
// All four accept paths with '/' separators, absolute or relative to
// cwd. e.g. "foo.txt", "/notes/foo.txt", "../docs/readme.txt".

// Read a file by path. Returns bytes read, or -1.
int fat12_read_file(const char* path, void* buf, unsigned int max);

// Create or overwrite a file. Returns size on success, -1 on error.
int fat12_write_file(const char* path, const void* data, unsigned int size);

// Delete a regular file. Returns 0 on success, -1 if missing or a directory.
int fat12_delete_file(const char* path);

// ---- Directory operations ---------------------------------------------

// Create a directory. Parents must already exist (no -p semantics).
// Returns 0 on success, -1 on error (parent missing, name exists,
// out of space, name doesn't fit 8.3, ...).
int fat12_mkdir(const char* path);

// Remove an *empty* directory. Refuses to remove "." or "..".
// Returns 0 on success, -1 on error.
int fat12_rmdir(const char* path);

// ---- Copy / move -------------------------------------------------------
//
// Both treat the destination as a *file path* unless it resolves to an
// existing directory, in which case the source's basename is used inside
// it. Refuse to copy/move directories (use mkdir + cp + rm for that).

int fat12_cp(const char* src, const char* dst);
int fat12_mv(const char* src, const char* dst);

#endif