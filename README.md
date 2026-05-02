# Computer OS

A 32-bit hobby operating system for x86, running in QEMU. Boots into a
higher-half kernel with a full interactive shell.

## Features

### Kernel
- Higher-half kernel mapped at `0xC0000000` via bootstrap page directory
- Custom GDT with ring-0/ring-3 segments and TSS
- IDT with ISR/IRQ handlers, PIC remapping
- PIT at 100 Hz — cooperative and preemptive round-robin scheduler
- PS/2 keyboard driver with tab-completion and command history
- PS/2 mouse driver (scroll wheel / two-finger)
- VGA text-mode driver with colour, scrollback, and cursor
- Serial output (QEMU `-serial stdio` for debug logging)
- RTC driver for date/time

### Memory
- Multiboot memory map parser
- Physical memory manager (bitmap allocator)
- Virtual memory manager — two-level x86 paging, 4 KB pages
- **Per-process page tables** — each user task gets its own page directory,
  cloned from the kernel at spawn and freed on exit
- Kernel heap (first-fit allocator with coalescing, growable up to 1 MB)

### Storage
- ATA PIO driver (primary master)
- FAT12 read/write filesystem with subdirectory support

### User space
- Ring-3 task execution via `iret` trampoline
- **ELF32 loader** — parses PT_LOAD segments, maps with correct R/W flags,
  entry point from `e_entry`; flat-binary (`.bin`) files still work
- `int 0x80` syscall gate (DPL=3): `exit`, `write`, `read`, `getpid`,
  `yield`, `setcolor`
- Tiny user-space libc: `printf`, `puts`, `getchar`, `gets_safe`,
  `strlen`, `strcmp`, `memset`, `memcpy`, and more

### Shell commands
| Category | Commands |
|---|---|
| General | `help`, `clear`, `echo`, `about`, `uptime`, `sleep`, `reboot`, `shutdown` |
| Memory | `meminfo`, `pmemstat`, `palloc`, `vmap`, `kmstat`, `kmtest` |
| Tasks | `ps`, `spawn`, `yield`, `preempt` |
| Filesystem | `ls`, `cat`, `write`, `rm`, `cp`, `mv`, `mkdir`, `rmdir`, `cd`, `pwd` |
| Programs | `user`, `run <file>` |
| Time | `date`, `time`, `tz` |
| Shell | `history`, Tab-complete, Up/Down history, Left/Right cursor |

---

## Building

### Dependencies

| Tool | Purpose |
|---|---|
| `nasm` | Assembler |
| `i686-elf-gcc` / `i686-elf-ld` | Cross-compiler (32-bit ELF) |
| `qemu-system-x86_64` | Emulator |
| `mkisofs` / `genisoimage` | ISO creation |
| `mtools` (`mformat`, `mcopy`) | FAT12 image creation |

### macOS
```
brew install nasm qemu cdrtools mtools
brew install i686-elf-binutils i686-elf-gcc
make run
```

### Linux
```
sudo apt-get install nasm qemu-system-x86 genisoimage mtools
# install i686-elf cross-toolchain from source or a pre-built package
make run
```

### Targets
```
make run          # build everything and boot in QEMU
make debug        # same, with -serial stdio for kernel log
make fatdisk.img  # rebuild the FAT12 disk image only
make clean        # remove all build artefacts
```

---

## Writing user programs

User programs are ELF32 executables linked at `0x01000000` via
`userprogs/c/user.ld`. The build system compiles every `.c` file in
`userprogs/c/` and copies the resulting `.elf` to the FAT12 disk.

### Add a new program
```c
// userprogs/c/myprog.c
#include "stdio.h"
int main(int argc, char** argv) {
    printf("Hello from user space!\n");
    return 0;
}
```
```
make fatdisk.img
# in the shell:
run MYPROG.ELF
```

### Available syscalls
| Number | Name | Args | Returns |
|---|---|---|---|
| 0 | `exit` | exit code | — |
| 1 | `write` | fd, buf, len | bytes written |
| 2 | `read` | fd, buf, len | bytes read |
| 3 | `getpid` | — | task id |
| 4 | `yield` | — | 0 |
| 6 | `setcolor` | fg (0-15), bg (0-15) | 0 |

fd=1 is stdout (VGA), fd=2 is stderr (VGA), fd=0 is stdin (keyboard).

Legacy flat binaries (`nasm -f bin` with `[ORG 0x01000000]`) also work —
the loader detects the ELF magic and falls back to the flat path if absent.

---

## Architecture overview

```
boot.asm          El Torito CD boot, enters 32-bit protected mode
entry.asm         Bootstrap paging (higher-half), jumps to kmain
kernel.c          kmain — initialises all subsystems in order
src/
  gdt.c           GDT + TSS (ring-0, ring-3, task state)
  idt.c / isr.c   IDT, exception handlers
  pic.c / irq.c   8259 PIC, IRQ dispatch
  pit.c           PIT 100 Hz timer, pit_sleep
  keyboard.c      PS/2 keyboard, scancode->ASCII
  mouse.c         PS/2 mouse, scroll wheel
  vga.c           VGA text mode, colour, scrollback
  serial.c        COM1 debug output
  rtc.c           CMOS real-time clock
  memmap.c        Multiboot memory map
  pmm.c           Physical memory manager (bitmap)
  vmm.c           Virtual memory manager, per-process page directories
  kheap.c         Kernel heap allocator
  task.c          Scheduler, ring-3 task spawn, context switch + CR3 swap
  task_switch.asm Context switch (save/restore callee-saved regs + esp)
  gdt.c           TSS esp0 update on ring-3 entry
  ata.c           ATA PIO driver
  fat12.c         FAT12 read/write filesystem
  loader.c        ELF32 + flat-binary user program loader
  userprog.c      Hardcoded ring-3 demo blob
  syscall.c       int 0x80 handler (write, read, exit, getpid, yield, setcolor)
  shell.c         Interactive shell with ~30 commands
```
