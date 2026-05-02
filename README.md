# Computer OS - Working Build

This is the fixed source tree. It boots to a VGA welcome screen in QEMU.

## What was changed from the original

1. **boot.asm** — removed the `int 0x13` disk-read block (BIOS already loads
   the kernel via El Torito). Added a `rep movsd` in the 32-bit
   `protected_mode` section that copies the kernel from 0x7E00 to 0x10000
   before jumping to it.

2. **Makefile** — `-boot-load-size 4` → `-boot-load-size 8` so the BIOS loads
   4 KB (bootloader + kernel) from the CD in one shot.

## Building on Linux

Works as-is:

```
sudo apt-get install nasm gcc-multilib genisoimage qemu-system-x86
make run
```

## Building on macOS

macOS's system `ld` is Apple's linker, not GNU ld — it doesn't understand
`-m elf_i386` or `-T linker.ld`. You need a cross-toolchain:

```
brew install nasm qemu cdrtools
brew install i686-elf-binutils i686-elf-gcc
```

Then edit the Makefile and change:

```make
CC = gcc
LD = ld
CFLAGS = -m32 -ffreestanding -fno-pie -fno-stack-protector -nostdlib -nostdinc -c
```

to:

```make
CC = i686-elf-gcc
LD = i686-elf-ld
CFLAGS = -ffreestanding -fno-pie -fno-stack-protector -nostdlib -nostdinc -c
```

(`i686-elf-gcc` is 32-bit by default, so `-m32` is unnecessary and rejected.)

Then:

```
make clean
make run
```

## Known-good ISO

`Computer_OS.iso` in this archive is the ISO I built and tested in QEMU on Linux.
If your local build produces something different, diff against this one to
narrow down whether it's a toolchain issue or a source issue.

## Expected output

Top of screen, three lines:

```
Welcome to Computer OS!     (green)
[OK] Bootloader  [OK] Protected Mode  [OK] Kernel Running   (white)
System Status: Halted                              (yellow)
```
