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
// One program at a time.  While a task is alive loader_run() returns -1.
// The reaper's on_exit hook unmaps all user pages and frees their frames.

// Where user code is mapped. Must match the program's link/ORG address.
#define LOADER_CODE_VIRT  0x01000000

// Maximum code+data size the loader will accept (16 pages = 64 KB).
#define LOADER_CODE_PAGES 16
#define LOADER_CODE_BYTES (LOADER_CODE_PAGES * 4096)

// User stack: one page, sitting just below LOADER_CODE_VIRT + 1 MB.
// The initial esp is the top of this page (LOADER_STACK_TOP); the
// first push will write to (top - 4) which is inside the page.
#define LOADER_STACK_TOP  (LOADER_CODE_VIRT + 0x100000)
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
// (file missing, too big, out of memory, another program running).
int loader_run(const char* path);

// Is a loaded program currently running?
int loader_is_busy(void);

// SYS_SBRK backend: extend the calling task's user heap by `delta` bytes.
// Returns the OLD break on success, or -1 on any failure (no heap, would
// exceed cap, OOM). Negative delta is currently a no-op (returns the
// current break unchanged).
int loader_sbrk(int delta);

#endif