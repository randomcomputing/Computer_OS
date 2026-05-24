#ifndef RAMFS_H
#define RAMFS_H

#include "vfs.h"

// A tiny in-memory filesystem: a flat set of files (no subdirectories)
// held in RAM. Mount it somewhere like "/tmp". Contents are lost on reboot.
// This exists mainly to prove the VFS can host more than one filesystem
// at once, alongside FAT12.

void              ramfs_init(void);          // clear all files
const vfs_ops_t*  ramfs_vfs_ops(void);       // ops table to hand to vfs_mount

#endif