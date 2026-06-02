#ifndef LOADER_H
#define LOADER_H

// User-program loader — supports ELF32 executables and legacy flat binaries.
//
// Reads a file from the FAT12 filesystem, maps its segments into user
// address space, sets up a one-page stack, and spawns a ring-3 task.
//
// ELF32 (preferred): the loader parses PT_LOAD segments and maps each
// one at its p_vaddr with the correct R/W flags.  Entry is taken from
// e_entry.  Build with userprogs/c/Makefile — it produces .elf files
// linked at 0x01000000 via user.ld.
//
// Flat binary (legacy): the file is copied verbatim to LOADER_CODE_VIRT
// and execution begins there.  Build with:
//   nasm -f bin -o hello.bin hello.asm   # [ORG 0x01000000]
//
// Multiple programs may be alive at once.  The reaper's on_exit hook
// unmaps all user pages and frees their frames when each task exits.

// Where user code is mapped. Must match the program's link/ORG address.
#define LOADER_CODE_VIRT  0x01000000

// Maximum code+data size the loader will accept (16 pages = 64 KB).
#define LOADER_CODE_PAGES 16
#define LOADER_CODE_BYTES (LOADER_CODE_PAGES * 4096)

// User stack: placed high in user address space for musl compatibility.
// musl needs ~400KB of stack for init so we give it plenty of room.
#define LOADER_STACK_TOP  0x7fff0000ULL
#define LOADER_STACK_PAGE (LOADER_STACK_TOP - 0x1000)

// Per-process user heap (sbrk).
//
// The heap is mapped on demand by SYS_SBRK. It starts empty (heap_brk ==
// heap_base) and grows upward one page at a time as the user program
// asks for memory. We park it well above the code region (which tops out
// at 0x01100000) and leave a generous gap so future code-size growth
// doesn't collide.
//
// Maximum is 16 MB — enough for any reasonable user program, small
// enough that a runaway malloc loop can't eat all of physical memory.
#define LOADER_HEAP_BASE  0x02000000u
#define LOADER_HEAP_MAX   0x01000000u   // 16 MB cap on heap growth

// Read `path` from FAT12 and run it as a ring-3 program.
// Returns the new task id on success, or -1 on any failure
// (file missing, too big, out of memory). Multiple user programs may
// be alive at once (e.g. a parent that forked a child); the caller
// waits on the returned task id to know when it has finished.
int loader_run(const char* path);

// SYS_SBRK backend: extend the calling task's user heap by `delta` bytes.
// Returns the OLD break on success, or -1 on any failure (no heap, would
// exceed cap, OOM). Negative delta is currently a no-op (returns the
// current break unchanged).
int loader_sbrk(int delta);

// --- fork / exec ------------------------------------------------------
struct registers;

// fork(): clone the calling user task. Builds a new address space, deep-
// copies every page the parent has mapped (code, data, heap, stack),
// duplicates the resource tracker, and spawns a child that resumes from
// `frame` with eax forced to 0. Returns the child's pid to the parent,
// or -1 on failure. `frame` is the syscall register snapshot (with the
// cross-ring useresp/ss appended) so the child can iret back to ring 3.
int loader_fork(struct registers* frame);

// exec(): replace the calling task's image with the program at `path`.
// Tears down the caller's current user pages, loads the new ELF/flat
// binary into a fresh address space, resets the heap, and rewrites
// `frame` so the syscall return path iret's into the new program's
// entry with a clean stack. On success this does not "return" in the
// usual sense — the caller resumes as the new program. Returns -1 on
// failure (file missing, bad image, OOM), in which case the caller's
// original image is left running.
int loader_exec(const char* path, struct registers* frame);

#endif