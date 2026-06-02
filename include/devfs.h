#ifndef DEVFS_H
#define DEVFS_H

#include "vfs.h"

/* Initialize the dev filesystem. */
void devfs_init(void);

/* Return the vfs_ops_t for mounting at /dev. */
const vfs_ops_t* devfs_vfs_ops(void);

#endif /* DEVFS_H */