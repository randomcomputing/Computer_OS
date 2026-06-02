#ifndef PROCFS_H
#define PROCFS_H

#include "vfs.h"

/* Initialize the proc filesystem (no-op — content generated on the fly). */
void procfs_init(void);

/* Return the vfs_ops_t for mounting at /proc. */
const vfs_ops_t* procfs_vfs_ops(void);

#endif /* PROCFS_H */