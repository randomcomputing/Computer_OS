# Computer OS

A 32-bit hobby operating system for the x86 architecture, written from scratch in C
and assembly. It boots from a CD-ROM via a custom El Torito bootloader, brings up a
**higher-half kernel** at `0xC0000000`, and drops you into a fully interactive shell
with around 40 commands, a real filesystem, ring-3 user programs, a VGA graphics
mode with a paint app, and a built-in text editor.

It runs under QEMU and is intended as a learning project — every subsystem (paging,
the scheduler, the syscall gate, the filesystem) is hand-rolled and reasonably
commented rather than pulled from a library.

```
###############################
###       Computer OS       ###
###############################

[OK] Bootloader  [OK] Protected Mode  [OK] Kernel Running
[OK] Higher-half kernel at 0xC0000000
[OK] GDT loaded (ring-0 + ring-3 + TSS)
[OK] IDT loaded
...
[OK] FAT12 mounted at / via VFS
[OK] ramfs mounted at /tmp

>
```

---

## Table of contents

1. [Quick start](#quick-start)
2. [What it can do](#what-it-can-do)
3. [The boot story](#the-boot-story-how-a-cd-rom-becomes-a-shell)
4. [Subsystems in depth](#subsystems-in-depth)
5. [The shell](#the-shell)
6. [Writing user programs](#writing-user-programs)
7. [System calls](#system-calls)
8. [Build system & toolchain](#build-system--toolchain)
9. [Source layout](#source-layout)
10. [Known limitations](#known-limitations)

---

## Quick start

You need a 32-bit `i686-elf` cross-toolchain, `nasm`, QEMU, and `mtools`.

```bash
# macOS
brew install nasm qemu cdrtools mtools
brew install i686-elf-binutils i686-elf-gcc
make run

# Linux (Debian/Ubuntu)
sudo apt-get install nasm qemu-system-x86 genisoimage mtools
# build the i686-elf cross-toolchain from source (see below), then:
make run
```

`make run` assembles the bootloader, compiles and links the kernel into an ISO,
builds a FAT12 disk image full of test files and user programs, and boots the
whole thing in QEMU. Once at the `>` prompt, try:

```
help            # list every command
ls              # browse the FAT12 disk
run GAME.ELF    # play the number-guessing game in ring 3
edit notes.txt  # open the text editor
paint           # launch the VGA paint program
about           # version / build banner
```

---

## What it can do

- **Boots itself** from a custom 512-byte bootloader through protected-mode entry
  into a higher-half C kernel — no GRUB, no existing bootloader.
- **Manages memory properly**: parses the BIOS memory map, runs a bitmap physical
  allocator, a two-level paging virtual memory manager, and a growable kernel heap.
- **Runs real user programs** in ring 3 from ELF32 files on disk, each in its own
  isolated address space, talking to the kernel only through `int 0x80`.
- **Multitasks**: a 100 Hz preemptive round-robin scheduler with per-process page
  directories and full context switching (including `CR3` swaps).
- **Has a filesystem**: a read/write FAT12 implementation with subdirectories,
  fronted by a small VFS so a RAM filesystem can be mounted alongside it at `/tmp`.
- **Talks to hardware**: PS/2 keyboard and mouse, PIT timer, CMOS real-time clock,
  ATA PIO disk, COM1 serial, and VGA in both text and 320×200 graphics mode.
- **Includes apps**: an interactive shell, a text editor, a paint program, and a
  handful of ring-3 programs (a calculator, a guessing game, a palette viewer, etc.).

---

## The boot story (how a CD-ROM becomes a shell)

It's worth walking the boot chain once, because the higher-half jump is the trickiest
part of the whole system.

1. **`boot.asm`** — The BIOS loads our 512-byte boot sector (El Torito CD boot). Still
   in 16-bit real mode, it enumerates RAM with `int 0x15, eax=0xE820` and stashes the
   memory map at physical `0x500` for the kernel to read later. It then loads the
   kernel image off the disk, sets up a flat GDT, enables the A20 line, and switches
   the CPU into 32-bit protected mode.

2. **`entry.asm`** — Runs with paging *off* at its physical load address. Its job is to
   build a bootstrap page directory that maps the high half: it points
   `0xC0000000–0xC0400000` at physical `0x00000000–0x00400000`, **and** identity-maps
   the same low 4 MB so the instruction right after enabling paging doesn't fault.
   It loads `CR3`, sets the paging bit in `CR0`, then does a long jump into the high
   half and calls `kmain`.

3. **`kernel.c` → `kmain`** — Initializes every subsystem in a careful order (the
   comments in the file explain *why* the order matters — e.g. the GDT must be
   replaced before the syscall gate is installed). The sequence is roughly:

   ```
   vga → serial → gdt → idt → pic → keyboard → mouse → pit →
   memmap → pmm → vmm → kheap → tasking → syscall → ata → fat12 → vfs → ramfs
   ```

   Each step prints an `[OK]` line. Storage is best-effort: if no disk is attached
   the shell still boots, it just reports "no filesystem." Finally it runs `sti`,
   enters the shell, and `halt`s if the shell ever returns.

The linker script (`linker.ld`) is what makes the high half work: it sets the
load address (LMA) low but the virtual address (VMA) at `0xC0010000`, so every
symbol resolves to a `0xC00xxxxx` address even though the bytes physically live
near `0x10000`.

---

## Subsystems in depth

### CPU & interrupts
- **GDT + TSS** (`gdt.c`) — ring-0 code/data, ring-3 code/data, and a task-state
  segment. `esp0` in the TSS is updated on every ring-3 entry so traps from user
  mode land on a valid kernel stack.
- **IDT, ISRs, IRQs** (`idt.c`, `isr.c/.asm`, `irq.c/.asm`) — full exception table
  plus hardware IRQ dispatch.
- **8259 PIC** (`pic.c`) — remapped away from the reserved CPU exception vectors.

### Memory
- **Memory map** (`memmap.c`) — reads the E820 map the bootloader saved at `0x500`.
- **Physical memory manager** (`pmm.c`) — bitmap allocator over usable physical pages.
- **Virtual memory manager** (`vmm.c`) — two-level x86 paging, 4 KB pages.
  **Each user task gets its own page directory**, cloned from the kernel at spawn and
  freed on exit. Includes **page-fault recovery** so a faulting user program is
  killed cleanly instead of taking down the kernel.
- **Kernel heap** (`kheap.c`) — first-fit allocator with free-block coalescing,
  growable up to 1 MB, living high in the address space at `0xC1000000`.

### Timers & clock
- **PIT** (`pit.c`) — programmed to 100 Hz (one tick per 10 ms); drives both
  `pit_sleep` and the preemptive scheduler.
- **RTC** (`rtc.c`) — reads date/time from the CMOS, with timezone support.

### Tasking
- **Scheduler** (`task.c`, `task_switch.asm`) — cooperative *and* preemptive
  round-robin. Context switches save/restore callee-saved registers and the stack
  pointer, and swap `CR3` so each task sees its own address space.

### Storage
- **ATA PIO driver** (`ata.c`) — primary master, polled.
- **FAT12** (`fat12.c`) — read **and** write, with subdirectory support. The largest
  single source file in the project.
- **VFS** (`vfs.c`) — a thin virtual filesystem layer so multiple filesystems can be
  mounted at once. FAT12 mounts at `/`.
- **ramfs** (`ramfs.c`) — an in-memory filesystem mounted at `/tmp` to demonstrate
  the VFS hosting two backends simultaneously. Its contents vanish on reboot.

### Display & input
- **VGA text mode** (`vga.c`) — 16-color text, scrollback, hardware cursor.
- **VGA graphics** (`gfx.c`) — 320×200×256 (mode 13h): pixels, lines, rectangles,
  text rendering, a mouse demo, and a full interactive paint program.
- **Keyboard** (`keyboard.c`) — PS/2, scancode→ASCII, feeds the shell's line editor.
- **Mouse** (`mouse.c`) — PS/2 with scroll-wheel / two-finger support.
- **Serial** (`serial.c`) — COM1 output for debug logging under `make debug`.

### User space
- **ELF32 loader** (`loader.c`) — parses `PT_LOAD` segments, maps them with the
  correct read/write flags, and jumps to `e_entry`. Legacy flat `.bin` files still
  work — the loader sniffs the ELF magic and falls back if it's absent.
- **Syscall gate** (`syscall.c`, `syscall_asm.asm`) — `int 0x80` with DPL=3.
- **User-access helpers** (`uaccess.c`) — safe copying of buffers across the
  user/kernel boundary so a bad pointer from ring 3 can't crash the kernel.
- **User libc** — a tiny freestanding C library (`stdio`, `string`, `malloc`) plus
  a `crt0.s` startup stub, archived into `libuser.a` and linked into every program.

---

## The shell

The shell (`shell.c`) provides line editing (Left/Right cursor, Backspace),
**Tab completion**, and **Up/Down command history**.

| Category | Commands |
|---|---|
| General | `help`, `clear`, `echo`, `about`, `uptime`, `sleep`, `reboot`, `shutdown` |
| Memory | `meminfo`, `pmemstat`, `palloc`, `vmap`, `kmstat`, `kmtest` |
| Tasks | `ps`, `spawn`, `yield`, `preempt`, `user` |
| Filesystem | `ls`, `cat`, `write`, `rm`, `cp`, `mv`, `mkdir`, `rmdir`, `cd`, `pwd`, `mount` |
| Programs | `run <file>` (ELF/flat), built-in demos |
| Editor | `edit <file>` |
| Graphics | `gfx`, `gfxmouse`, `paint` |
| Time | `date`, `time`, `tz` |
| Interrupts | `cli`, `sti`, `hlt` |
| Shell UX | `history`, Tab-complete, Up/Down history, Left/Right cursor |

> Tip: boot with `make debug` to also get the kernel log on `-serial stdio`,
> which is invaluable when something faults before the shell comes up.

---

## Writing user programs

User programs are ELF32 executables linked at `0x01000000` via `userprogs/c/user.ld`.
The build compiles every `.c` in `userprogs/c/`, links it against `crt0.s` and
`libuser.a`, and copies the resulting `.elf` onto the FAT12 disk.

**Add a new program:**

```c
// userprogs/c/myprog.c
#include "stdio.h"

int main(int argc, char** argv) {
    printf("Hello from user space!\n");
    return 0;
}
```

```bash
make fatdisk.img      # rebuild the disk with your program on it
# then, in the OS shell:
run MYPROG.ELF
```

**Programs shipped on the disk:**

| File | What it does |
|---|---|
| `TESTC.ELF` | User-space test suite — exercises `.text`, `.data`, `.bss`, and the stack |
| `ECHO.ELF` | Reads a line from stdin and echoes it back |
| `CAT.ELF` | Interactive line-echo (empty line exits) |
| `CALC.ELF` | Tiny integer calculator: `<num> <op> <num>` with `+ - * / %` |
| `GAME.ELF` | Number-guessing game with higher/lower hints |
| `COLORS.ELF` | Prints the 16-color VGA palette |
| `MEMSTAT.ELF` | Heap probe — grows `malloc` allocations until they fail |
| `MALTEST.ELF` | Stress-tests `malloc` with many small allocations |
| `FORKTEST.ELF` | Demonstrates `fork` / `wait` / `exec` (see system calls) |
| `HELLO.BIN` / `COUNT.BIN` | Legacy flat-binary demos (assembled with `nasm -f bin`) |

Legacy flat binaries (`nasm -f bin` with `[ORG 0x01000000]`) still run — the loader
detects the ELF magic and falls back to the flat path if it's missing.

---

## System calls

Invoked via `int 0x80`, gate DPL=3. Syscall number in `eax`, arguments in the usual
registers; return value comes back in `eax`.

| # | Name | Arguments | Returns |
|---|---|---|---|
| 0 | `exit` | exit code | — (does not return) |
| 1 | `write` | fd, buf, len | bytes written |
| 2 | `read` | fd, buf, len | bytes read |
| 3 | `getpid` | — | task id |
| 4 | `yield` | — | 0 |
| 5 | `sbrk` | increment | previous break (grows the user heap) |
| 6 | `setcolor` | fg (0–15), bg (0–15) | 0 |
| 7 | `fork` | — | child pid to parent, 0 to child, -1 on error |
| 8 | `exec` | path | does not return on success; -1 on error |
| 9 | `wait` | int\* status | exited child's pid (status written through ptr), -1 if no children |

File descriptors: `0` = stdin (keyboard), `1` = stdout (VGA), `2` = stderr (VGA).
The user-space `malloc` is built on top of `sbrk`.

### Process control: fork / exec / wait

These three give user programs real Unix-style process control on top of the
existing per-task address spaces.

- **`fork()`** clones the calling process. The kernel builds a fresh page
  directory for the child and **deep-copies every page the parent has mapped**
  — code, data, heap, and stack — into new physical frames, preserving each
  page's original read/write/user flags. The child is a separate process with
  its own address space: writes in one are invisible to the other. The child
  re-emerges from the same `int 0x80` the parent called, except its `eax` (the
  return value) is forced to `0`, so the classic idiom works:

  ```c
  int pid = fork();
  if (pid == 0) {
      // child
  } else if (pid > 0) {
      // parent; pid is the child's id
  } else {
      // fork failed
  }
  ```

- **`exec(path)`** replaces the calling process's image with the program at
  `path` (e.g. `"ECHO.ELF"`). The new image is loaded into a *fresh* address
  space first; only once that fully succeeds does the kernel tear down the old
  pages, switch the live task onto the new address space, and rewrite the trap
  frame so the syscall return lands at the new program's entry on a clean
  stack. On success `exec` does not return. On failure (missing file, bad
  image, OOM) it returns `-1` and the original program keeps running.

- **`wait(&status)`** blocks the caller until one of its children exits, writes
  the child's exit code through `status`, reaps the zombie, and returns the
  child's pid. It returns `-1` immediately if the caller has no children. The
  scheduler marks a waiting parent `BLOCKED` and wakes it from `task_exit` when
  a child dies, so a blocked parent consumes no CPU.

The fork machinery lives in `task.c` (`task_clone_user` plus the
`enter_user_resume` stub in `task_switch.asm`, which `iret`s into a copied
register frame) and `loader.c` (`loader_fork` / `loader_exec`, which own the
address-space copy and image swap). `FORKTEST.ELF` exercises all three:

```
run FORKTEST.ELF
```

It forks a child that exits with a known status (proving the parent/child split
and the `wait` status hand-off), confirms the child's memory writes don't leak
into the parent (proving the copy is real, not shared), then forks a second
child that `exec`s `ECHO.ELF` in place.

---

## Build system & toolchain

### Tools

| Tool | Purpose |
|---|---|
| `nasm` | Assembler (bootloader + kernel stubs + user flat binaries) |
| `i686-elf-gcc` / `i686-elf-ld` / `i686-elf-ar` | 32-bit ELF cross-toolchain |
| `qemu-system-x86_64` | Emulator |
| `mkisofs` / `genisoimage` | ISO creation |
| `mtools` (`mformat`, `mcopy`) | Populating the FAT12 image without root |

### Make targets

```bash
make run          # build everything and boot in QEMU
make debug        # same, plus -serial stdio kernel log and -no-reboot
make fatdisk.img  # rebuild only the FAT12 disk image
make clean        # remove all build artifacts (kernel + user programs)
```

The default target builds `Computer_OS.iso`. `make run` boots from that ISO
(`-boot d`) and attaches the FAT12 image as the primary IDE disk.

### Getting the cross-toolchain on Linux

There's no apt package for `i686-elf-gcc`; build it from source following the OSDev
"GCC Cross-Compiler" guide (build `binutils` first, then `gcc` configured with
`--target=i686-elf`). On macOS the Homebrew formulas above handle it for you.

---

## Source layout

```
boot.asm            512-byte El Torito boot sector; E820 memory probe; enter protected mode
entry.asm           Bootstrap paging (higher-half), long-jump to kmain
linker.ld           Splits load address (low) from virtual address (0xC0010000)
kernel.c            kmain — initialises every subsystem in dependency order
Makefile            Builds kernel ISO + FAT12 disk; run/debug/clean targets

include/            Public headers for every subsystem
src/
  gdt.c             GDT + TSS (ring-0, ring-3, esp0 updates)
  idt.c             Interrupt descriptor table
  isr.c/.asm        CPU exception handlers + stubs
  pic.c             8259 PIC remap
  irq.c/.asm        Hardware IRQ dispatch + stubs
  pit.c             100 Hz timer, pit_sleep, scheduler tick
  rtc.c             CMOS real-time clock + timezone
  keyboard.c        PS/2 keyboard, scancode → ASCII
  mouse.c           PS/2 mouse, scroll wheel
  vga.c             VGA text mode: colour, scrollback, cursor
  gfx.c             VGA mode 13h: pixels, lines, rects, text, paint program
  serial.c          COM1 debug output
  memmap.c          Multiboot/E820 memory map parser
  pmm.c             Physical memory manager (bitmap)
  vmm.c             Virtual memory manager, per-process page dirs, fault recovery
  kheap.c           Kernel heap (first-fit + coalescing, growable)
  task.c            Scheduler, ring-3 spawn, context switch + CR3 swap
  task_switch.asm   Low-level register/stack save & restore
  ata.c             ATA PIO disk driver
  fat12.c           FAT12 read/write filesystem with subdirectories
  vfs.c             Virtual filesystem layer (mount table)
  ramfs.c           In-memory filesystem (mounted at /tmp)
  loader.c          ELF32 + flat-binary user-program loader
  syscall.c         int 0x80 dispatcher
  syscall_asm.asm   Syscall entry stub
  uaccess.c         Safe user/kernel buffer copies
  userprog.c        Hardcoded ring-3 demo launcher
  editor.c          Built-in text editor
  string.c          Freestanding string helpers
  printf.c          Kernel printf (VGA + serial)

userprogs/
  hello.asm, count.asm        Flat-binary demos
  c/                          C user programs (compiled to ELF32)
    *.c                       Programs (calc, game, colors, cat, echo, memstat, ...)
    lib/                      User libc: stdio, string, malloc, crt0.s
    include/                  User-space headers
    user.ld                   Links programs at 0x01000000
    Makefile                  Builds each .c into a .elf
```

---

## Known limitations

This is a learning OS, not a production one. Notably:

- Single-core, 32-bit only; no SMP, no long mode.
- ATA driver is polled PIO (no DMA, no interrupts), primary master only.
- The scheduler is plain round-robin — no priorities, no blocking/sleeping queues
  beyond `pit_sleep`.
- FAT12 only (1.44 MB floppy-style images); no FAT16/32, no journaling.
- No networking, no dynamic linking, no signals. `fork`/`exec`/`wait` exist
  (see the system-calls section) but there is no `kill`/signal delivery, no
  process groups, and `wait` collects any child rather than a specific pid.
- A small, bounded amount of physical memory (a few pages) is not reclaimed at
  the end of each user-program lifecycle. It does not grow without bound during
  a single program and does not threaten stability at this scale, but a future
  pass on the loader's page-table teardown (`vmm_free_user_pd` /
  `release_resources`) could tighten it up.
- `ramfs` contents are lost on reboot by design.

These are the natural next things to extend if you want to keep hacking on it.