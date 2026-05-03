#ifndef UACCESS_H
#define UACCESS_H

// Safe copy primitives for moving data across the user/kernel boundary.
//
// Why this exists: a syscall runs in ring 0 inside the kernel, but its
// arguments often point into ring-3 memory. If the user passes a bogus
// pointer (unmapped page, kernel address, off the end of a buffer) we
// must NOT panic — we must return -EFAULT and let the user program
// continue. These helpers cooperate with vmm_page_fault() to do that.
//
// Mechanism (setjmp-style, no compiler fixup tables required):
//   1. Helper sets current->in_user_access = 1 and stashes a recovery
//      EIP (via GCC labels-as-values).
//   2. Helper does the copy byte-by-byte.
//   3. If a page fault fires inside the copy, vmm_page_fault sees the
//      flag, sets current->uaccess_faulted = 1, and rewrites the saved
//      EIP to the recovery label so we bail out cleanly.
//   4. Helper checks uaccess_faulted on the way out: if set, returns -1.
//
// Validates: pointer must lie entirely in the user half (< KERNEL_VMA).
// Anything in the kernel half is rejected before we even try the copy.

// Returns 0 on success, -1 on fault or invalid pointer.
int copy_from_user(void* dst, const void* user_src, unsigned int len);

// Returns 0 on success, -1 on fault or invalid pointer.
int copy_to_user(void* user_dst, const void* src, unsigned int len);

// Returns the length of a NUL-terminated user string (excluding the NUL),
// or -1 on fault or invalid pointer or if no NUL is found within max.
int strnlen_user(const char* user_str, unsigned int max);

// True if the entire range [p, p+len) is in user space (< 0xC0000000)
// and doesn't wrap. Cheap range check — does NOT verify pages are mapped.
int user_range_ok(const void* p, unsigned int len);

#endif