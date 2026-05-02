#ifndef USERPROG_H
#define USERPROG_H

// Demo ring-3 program. Just enough to prove the userspace plumbing
// works: writes a string via SYS_WRITE, calls SYS_GETPID, exits cleanly
// via SYS_EXIT. Built once with keystone-engine into a flat byte blob;
// see comments in userprog.c for how to regenerate.

// Map the user program at this virtual address. Must be < 0xC0000000
// and 4 KB aligned. We deliberately pick a "userspace-y" address (4 MB)
// so it doesn't collide with anything the kernel cares about.
#define USERPROG_LOAD_ADDR  0x00400000

// User stack lives just below the 8 MB mark. Single page is plenty.
#define USERPROG_STACK_TOP  0x00800000

// Spawn the demo program as a ring-3 task.
// Maps the code into the address space, copies the program bytes in,
// maps a stack page, and calls task_spawn_user. Returns the new task
// id on success, -1 on failure.
int userprog_run_hello(void);

#endif