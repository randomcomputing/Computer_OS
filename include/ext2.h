#ifndef EXT2_H
#define EXT2_H

#include "vfs.h"

// Read/write ext2 driver (also mounts clean ext3 volumes as plain ext2).
//
// On-disk layout, in blocks of 1024 << s_log_block_size bytes:
//
//   [boot area: 1024 bytes]
//   [superblock: 1024 bytes]        always at byte offset 1024
//   [block group 0] [block group 1] ... [block group N-1]
//
// Each block group holds (optionally) a superblock backup and a copy of
// the group-descriptor table, then a block bitmap, an inode bitmap, an
// inode table, and data blocks. The group-descriptor table says where the
// bitmaps and inode table of every group live.
//
// A file is an inode: mode, size, timestamps, link count, and 15 block
// pointers: 12 direct, then single-, double- and triple-indirect.
// A directory is a file whose data is a list of variable-length records
// (inode, rec_len, name_len, file_type, name). Inode 2 is the root.
//
// Supported: block sizes 1K/2K/4K, revision 0 and 1, any inode size,
// sparse_super, large_file, filetype, resize_inode, dir_index (treated as
// a plain linear directory), ext_attr blocks (freed on delete), and fast
// and slow symlinks.
//
// Symlinks are presented to the VFS the same way the Linux syscall layer
// already fakes them on FAT32: read_file() on a symlink returns
// VFS_SYMLINK_MAGIC followed by the target, and write_file() with that
// prefix creates a real ext2 symlink. So readlink/symlink/follow logic
// works unchanged, but on ext2 the links are native and visible to Linux.
//
// Features that would make a write corrupt the volume (metadata_csum,
// uninit_bg/gdt_csum, huge_file, ...) cause a read-only mount. Features we
// can't even read (extents, 64bit, meta_bg, compression, a journal that
// needs recovery) refuse the mount.

// Probe the primary ATA disk for an ext2 volume: every GPT partition is
// checked for the ext2 magic, and if there is no GPT the whole disk is
// tried. Returns 0 on success (volume mounted), -1 if none was found.
int ext2_mount(void);

// 1 if ext2_mount() succeeded and the volume is writable.
int ext2_writable(void);

// Print a short summary (label, block size, free space) to the console.
void ext2_print_info(void);

// Ops table to hand to vfs_mount().
const vfs_ops_t* ext2_vfs_ops(void);

#endif