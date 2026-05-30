# Computer OS

Computer OS is a 64-bit x86_64 hobby operating system written from scratch in C and assembly.
It now boots through **Limine UEFI**, enters long mode, starts a higher-half kernel, initializes hardware, and drops into an interactive shell.

The current build uses a framebuffer/VBE-style console. Boot status lines are color coded:

```text
[OK]  green = working
[!!]  red   = missing, broken, or unavailable
```

It runs best in QEMU while developing, but the Limine UEFI disk image can also be tried in other virtual machines once the image is stable.

---

## Quick start

Required tools on macOS:

```bash
brew install nasm qemu mtools xorriso
brew install x86_64-elf-binutils x86_64-elf-gcc
```

Build and run:

```bash
make clean
make run
```

Debug run with serial output:

```bash
make debug
```

Useful QEMU development flags are already in the Makefile, including serial output for debug mode.

---

## Boot layout

The 64-bit boot path is:

```text
Limine UEFI bootloader
        ↓
kernel.elf loaded from boot():/kernel.elf
        ↓
x86_64 long mode
        ↓
higher-half kernel
        ↓
GDT / IDT / IRQ / PIT / memory / heap / disk / filesystem / shell
```

The Limine config is simple:

```conf
timeout: 0

/Computer OS (64-bit)
    protocol: limine
    kernel_path: boot():/kernel.elf
```

Use `timeout: 0` to boot immediately, or `timeout: -1` to keep the boot menu forever.

---

## What works now

- 64-bit x86_64 kernel
- Limine UEFI boot
- Higher-half kernel mapping
- 4-level paging
- GDT, IDT, ISR, IRQ, PIC
- PIT timer at 100 Hz
- PS/2 keyboard and mouse
- Serial debug output
- Limine framebuffer / VBE console
- Colored boot status: green `[OK]`, red `[!!]`
- Physical memory manager
- Virtual memory manager
- Kernel heap
- Basic tasking and scheduling
- Syscall path
- ATA PIO disk probing
- FAT12 root filesystem
- ramfs mounted at `/tmp`
- VFS layer
- PCI scanning
- e1000 network driver experiments
- ARP/IP/TCP experiments
- Interactive shell
- Text editor
- Graphics demos and paint
- User program loading experiments

---

## Shell commands

At the prompt, run:

```text
help
```

Main commands:

```text
General:
  help                 show help
  about                show OS information
  clear                clear screen
  echo <text>          print text
  uptime               show time since boot
  sleep <ms>           pause
  reboot               restart machine
  shutdown             power off

Files:
  mount                show mounts
  ls [path]            list directory
  cat <file>           print file
  write <file> <text>  write file
  rm <file>            delete file
  cp <src> <dst>       copy file
  mv <src> <dst>       rename/move
  mkdir <path>         make directory
  rmdir <path>         remove empty directory
  cd [path]            change directory
  pwd                  print current directory

Programs/tasks:
  run <file>           run ELF64 or flat binary
  user                 ring-3 demo
  ps                   list tasks
  spawn <kind>         counter or spinner task
  yield                yield once
  preempt <on|off>     toggle preemption

Memory/debug:
  meminfo              memory map summary
  pmemstat             PMM stats
  palloc               allocate page
  vmap <virt>          virtual to physical lookup
  kmstat               heap stats
  kmtest               heap test

Hardware/network:
  lspci                list PCI devices
  nettest              network state
  ping <ip>            ping/ARP test
  resolve <host>       resolver test
  http <host/path>     tiny HTTP request

Graphics/editor:
  vbe                  framebuffer test
  gfx                  graphics demo
  gfxmouse             mouse graphics demo
  paint                paint program
  edit [file]          text editor

Time:
  date                 show date/time
  time                 same as date
  tz [name|offset]     timezone
```

Shell editing:

```text
Tab        autocomplete
Up/Down    command history
Left/Right move cursor
Home/End   line start/end
Delete     delete character
PgUp/PgDn  scroll output history
```

---

## Source layout

```text
kernel.c              kernel entry and boot initialization order
entry.asm             64-bit entry/bootstrap assembly
linker.ld             kernel linker script
limine.conf           Limine boot menu config
Makefile              build, disk image, and QEMU run rules

include/              kernel headers
src/gdt.c             GDT/TSS setup
src/idt.c             IDT setup
src/isr.c/.asm        CPU exception handlers
src/irq.c/.asm        hardware IRQ handlers
src/pic.c             8259 PIC
src/pit.c             timer
src/keyboard.c        PS/2 keyboard
src/mouse.c           PS/2 mouse
src/memmap.c          Limine memory map handling
src/pmm.c             physical page allocator
src/vmm.c             virtual memory manager
src/kheap.c           kernel heap
src/task.c            tasking/scheduler
src/syscall.c/.asm    syscall handling
src/ata.c             ATA PIO disk
src/fat12.c           FAT12 filesystem
src/vfs.c             virtual filesystem layer
src/ramfs.c           RAM filesystem
src/pci.c             PCI scan
src/e1000.c           Intel e1000 NIC experiments
src/net.c             network layer experiments
src/arp.c             ARP
src/tcp.c             TCP experiments
src/fbcon.c           framebuffer console
src/bochs_vbe.c       Bochs/QEMU VBE support
src/gfx.c             graphics demos
src/editor.c          text editor
src/shell.c           interactive shell
src/reboot.c          reboot/shutdown helpers
```

---

## Notes

This is still a hobby OS, so QEMU is the main target while debugging. Parallels or other VMs may boot later, but QEMU gives better serial logs and lower-level debugging.

For OS development, prefer small changes and rebuild often:

```bash
make clean
make run
```

