# -----------------------------------------------------------------------
# Computer OS — 64-bit UEFI build (Limine bootloader)
# -----------------------------------------------------------------------

AS      = nasm
CC      = x86_64-elf-gcc
LD      = x86_64-elf-ld
QEMU    = qemu-system-x86_64

OBJDIR  = obj

ASFLAGS = -f elf64
CFLAGS  = -ffreestanding -fno-asynchronous-unwind-tables -fno-pie \
          -fno-stack-protector -nostdlib -nostdinc \
          -mcmodel=kernel -mno-red-zone \
          -mno-sse -mno-sse2 -mno-mmx -mno-80387 \
          -Iinclude -c -std=c11

LDFLAGS = -T linker.ld -nostdlib

LIMINE_DIR ?= limine

KERNEL_ELF  = kernel.elf
DISK_IMG    = disk.img
FAT32_IMG   = fat32disk.img
EXT2_IMG    = ext2disk.img
CFS_IMG     = cfsdisk.img

# Host compiler for build-time tools (mkfs.cfs). Not the cross CC.
HOSTCC ?= cc

# mke2fs with -d (populate from a directory) needs e2fsprogs >= 1.43.
# On macOS: brew install e2fsprogs  (keg-only, so we look for it there).
MKE2FS ?= $(shell command -v mke2fs 2>/dev/null || echo /opt/homebrew/opt/e2fsprogs/sbin/mke2fs)

# -----------------------------------------------------------------------
# Object files
# -----------------------------------------------------------------------

ENTRY_O       = $(OBJDIR)/entry.o
HALT_O        = $(OBJDIR)/halt.o
ISR_ASM_O     = $(OBJDIR)/isr_asm.o
IRQ_ASM_O     = $(OBJDIR)/irq_asm.o
TASK_ASM_O    = $(OBJDIR)/task_switch.o
SYSCALL_ASM_O = $(OBJDIR)/syscall_asm.o

KERNEL_O   = $(OBJDIR)/kernel.o
GDT_O      = $(OBJDIR)/gdt.o
IDT_O      = $(OBJDIR)/idt.o
ISR_O      = $(OBJDIR)/isr.o
PIC_O      = $(OBJDIR)/pic.o
IRQ_O      = $(OBJDIR)/irq.o
KEYBOARD_O = $(OBJDIR)/keyboard.o
MOUSE_O    = $(OBJDIR)/mouse.o
PIT_O      = $(OBJDIR)/pit.o
SERIAL_O   = $(OBJDIR)/serial.o
MEMMAP_O   = $(OBJDIR)/memmap.o
PMM_O      = $(OBJDIR)/pmm.o
VMM_O      = $(OBJDIR)/vmm.o
KHEAP_O    = $(OBJDIR)/kheap.o
TASK_O     = $(OBJDIR)/task.o
ATA_O      = $(OBJDIR)/ata.o
FAT32_O         = $(OBJDIR)/fat32.o
EXT2_O          = $(OBJDIR)/ext2.o
CFS_O           = $(OBJDIR)/cfs.o
INFLATE_O       = $(OBJDIR)/inflate.o
LINUX_SYSCALL_O = $(OBJDIR)/linux_syscall.o
PROCFS_O        = $(OBJDIR)/procfs.o
DEVFS_O         = $(OBJDIR)/devfs.o
VFS_O      = $(OBJDIR)/vfs.o
RAMFS_O    = $(OBJDIR)/ramfs.o
SYSCALL_O  = $(OBJDIR)/syscall.o
USERPROG_O = $(OBJDIR)/userprog.o
LOADER_O   = $(OBJDIR)/loader.o
EDITOR_O   = $(OBJDIR)/editor.o
RTC_O      = $(OBJDIR)/rtc.o
GFX_O      = $(OBJDIR)/gfx.o
FONT_O     = $(OBJDIR)/font8x16.o
UACCESS_O  = $(OBJDIR)/uaccess.o
PCI_O      = $(OBJDIR)/pci.o
E1000_O    = $(OBJDIR)/e1000.o
ARP_O      = $(OBJDIR)/arp.o
NET_O      = $(OBJDIR)/net.o
TCP_O      = $(OBJDIR)/tcp.o
FBCON_O    = $(OBJDIR)/fbcon.o
BVBE_O     = $(OBJDIR)/bochs_vbe.o
CONSOLE_O  = $(OBJDIR)/console.o
STRING_O   = $(OBJDIR)/string.o
PRINTF_O   = $(OBJDIR)/printf.o
REBOOT_O   = $(OBJDIR)/reboot.o
ACPI_O     = $(OBJDIR)/acpi.o
VGA_O      = $(OBJDIR)/vga.o
SHELL_O    = $(OBJDIR)/shell.o
TAR_O      = $(OBJDIR)/tar.o
BANNER_O   = $(OBJDIR)/boot_banner.o

KERNEL_OBJS = \
    $(ENTRY_O) $(HALT_O) \
    $(ISR_ASM_O) $(IRQ_ASM_O) $(TASK_ASM_O) $(SYSCALL_ASM_O) \
    $(KERNEL_O) $(GDT_O) $(IDT_O) $(ISR_O) $(PIC_O) $(IRQ_O) \
    $(KEYBOARD_O) $(MOUSE_O) $(PIT_O) $(SERIAL_O) \
    $(MEMMAP_O) $(PMM_O) $(VMM_O) $(KHEAP_O) \
    $(TASK_O) $(ATA_O) $(FAT32_O) $(EXT2_O) $(CFS_O) $(VFS_O) $(RAMFS_O) \
    $(INFLATE_O) $(LINUX_SYSCALL_O) $(PROCFS_O) $(DEVFS_O) \
    $(SYSCALL_O) $(USERPROG_O) $(LOADER_O) $(EDITOR_O) \
    $(RTC_O) $(GFX_O) $(FONT_O) $(UACCESS_O) \
    $(PCI_O) $(E1000_O) $(ARP_O) $(NET_O) $(TCP_O) \
    $(FBCON_O) $(BVBE_O) $(CONSOLE_O) $(STRING_O) $(PRINTF_O) \
    $(REBOOT_O) $(ACPI_O) $(VGA_O) $(SHELL_O) $(TAR_O) $(BANNER_O)

# -----------------------------------------------------------------------
# Build rules
# -----------------------------------------------------------------------

all: $(DISK_IMG)

$(OBJDIR):
	mkdir -p $(OBJDIR)

$(ENTRY_O): entry.asm | $(OBJDIR)
	@echo "Assembling entry..."
	$(AS) $(ASFLAGS) entry.asm -o $@

$(HALT_O): halt.asm | $(OBJDIR)
	$(AS) $(ASFLAGS) halt.asm -o $@

$(ISR_ASM_O): src/isr.asm | $(OBJDIR)
	$(AS) $(ASFLAGS) src/isr.asm -o $@

$(IRQ_ASM_O): src/irq_asm.asm | $(OBJDIR)
	$(AS) $(ASFLAGS) src/irq_asm.asm -o $@

$(TASK_ASM_O): src/task_switch.asm | $(OBJDIR)
	$(AS) $(ASFLAGS) src/task_switch.asm -o $@

$(SYSCALL_ASM_O): src/syscall_asm.asm | $(OBJDIR)
	$(AS) $(ASFLAGS) src/syscall_asm.asm -o $@

$(KERNEL_O): kernel.c | $(OBJDIR)
	@echo "Compiling kernel..."
	$(CC) $(CFLAGS) kernel.c -o $@

$(GDT_O):     src/gdt.c | $(OBJDIR);      $(CC) $(CFLAGS) $< -o $@
$(IDT_O):     src/idt.c | $(OBJDIR);      $(CC) $(CFLAGS) $< -o $@
$(ISR_O):     src/isr.c | $(OBJDIR);      $(CC) $(CFLAGS) $< -o $@
$(PIC_O):     src/pic.c | $(OBJDIR);      $(CC) $(CFLAGS) $< -o $@
$(IRQ_O):     src/irq.c | $(OBJDIR);      $(CC) $(CFLAGS) $< -o $@
$(KEYBOARD_O):src/keyboard.c | $(OBJDIR); $(CC) $(CFLAGS) $< -o $@
$(MOUSE_O):   src/mouse.c | $(OBJDIR);    $(CC) $(CFLAGS) $< -o $@
$(PIT_O):     src/pit.c | $(OBJDIR);      $(CC) $(CFLAGS) $< -o $@
$(SERIAL_O):  src/serial.c | $(OBJDIR);   $(CC) $(CFLAGS) $< -o $@
$(MEMMAP_O):  src/memmap.c | $(OBJDIR);   $(CC) $(CFLAGS) $< -o $@
$(PMM_O):     src/pmm.c | $(OBJDIR);      $(CC) $(CFLAGS) $< -o $@
$(VMM_O):     src/vmm.c | $(OBJDIR);      $(CC) $(CFLAGS) $< -o $@
$(KHEAP_O):   src/kheap.c | $(OBJDIR);    $(CC) $(CFLAGS) $< -o $@
$(TASK_O):    src/task.c | $(OBJDIR);     $(CC) $(CFLAGS) $< -o $@
$(ATA_O):     src/ata.c | $(OBJDIR);      $(CC) $(CFLAGS) $< -o $@
$(FAT32_O):         src/fat32.c | $(OBJDIR);         $(CC) $(CFLAGS) $< -o $@
$(EXT2_O):          src/ext2.c | $(OBJDIR);          $(CC) $(CFLAGS) $< -o $@
$(CFS_O):           src/cfs.c | $(OBJDIR);           $(CC) $(CFLAGS) -DCFS_KERNEL $< -o $@
$(INFLATE_O):       src/inflate.c | $(OBJDIR);       $(CC) $(CFLAGS) $< -o $@
$(LINUX_SYSCALL_O): src/linux_syscall.c | $(OBJDIR); $(CC) $(CFLAGS) $< -o $@
$(PROCFS_O):        src/procfs.c | $(OBJDIR);        $(CC) $(CFLAGS) $< -o $@
$(DEVFS_O):         src/devfs.c | $(OBJDIR);         $(CC) $(CFLAGS) $< -o $@
$(VFS_O):     src/vfs.c | $(OBJDIR);      $(CC) $(CFLAGS) $< -o $@
$(RAMFS_O):   src/ramfs.c | $(OBJDIR);    $(CC) $(CFLAGS) $< -o $@
$(SYSCALL_O): src/syscall.c | $(OBJDIR);  $(CC) $(CFLAGS) $< -o $@
$(USERPROG_O):src/userprog.c | $(OBJDIR); $(CC) $(CFLAGS) $< -o $@
$(LOADER_O):  src/loader.c | $(OBJDIR);   $(CC) $(CFLAGS) $< -o $@
$(EDITOR_O):  src/editor.c | $(OBJDIR);   $(CC) $(CFLAGS) $< -o $@
$(RTC_O):     src/rtc.c | $(OBJDIR);      $(CC) $(CFLAGS) $< -o $@
$(GFX_O):     src/gfx.c | $(OBJDIR);      $(CC) $(CFLAGS) $< -o $@
$(FONT_O):    src/font8x16.c | $(OBJDIR);  $(CC) $(CFLAGS) $< -o $@
$(UACCESS_O): src/uaccess.c | $(OBJDIR);  $(CC) $(CFLAGS) $< -o $@
$(PCI_O):     src/pci.c | $(OBJDIR);      $(CC) $(CFLAGS) $< -o $@
$(E1000_O):   src/e1000.c | $(OBJDIR);    $(CC) $(CFLAGS) $< -o $@
$(ARP_O):     src/arp.c | $(OBJDIR);      $(CC) $(CFLAGS) $< -o $@
$(NET_O):     src/net.c | $(OBJDIR);      $(CC) $(CFLAGS) $< -o $@
$(TCP_O):     src/tcp.c | $(OBJDIR);      $(CC) $(CFLAGS) $< -o $@
$(FBCON_O):   src/fbcon.c | $(OBJDIR);    $(CC) $(CFLAGS) $< -o $@
$(BVBE_O):    src/bochs_vbe.c | $(OBJDIR); $(CC) $(CFLAGS) $< -o $@
$(CONSOLE_O): src/console.c | $(OBJDIR);  $(CC) $(CFLAGS) $< -o $@
$(STRING_O):  src/string.c | $(OBJDIR);   $(CC) $(CFLAGS) $< -o $@
$(PRINTF_O):  src/printf.c | $(OBJDIR);   $(CC) $(CFLAGS) $< -o $@
$(REBOOT_O):  src/reboot.c | $(OBJDIR);   $(CC) $(CFLAGS) $< -o $@
$(ACPI_O):    src/acpi.c | $(OBJDIR);     $(CC) $(CFLAGS) $< -o $@
$(VGA_O):     src/vga.c | $(OBJDIR);      $(CC) $(CFLAGS) $< -o $@
$(SHELL_O):   src/shell.c | $(OBJDIR);    $(CC) $(CFLAGS) $< -o $@
$(TAR_O):     src/tar.c   | $(OBJDIR);    $(CC) $(CFLAGS) $< -o $@
$(BANNER_O):  src/boot_banner.c | $(OBJDIR); $(CC) $(CFLAGS) $< -o $@

$(KERNEL_ELF): $(KERNEL_OBJS) linker.ld
	@echo "Linking kernel ELF..."
	$(LD) $(LDFLAGS) -o $@ $(KERNEL_OBJS)

# -----------------------------------------------------------------------
# Disk image
# -----------------------------------------------------------------------

# Partition layout (512-byte sectors):
#   1  ESP    2048   .. 67583    32 MB  Limine + kernel.elf
#   2  FAT32  67584  .. 198655   64 MB  fat32disk.img  (mounted at /)
#   3  ext2   198656 .. 313343   56 MB  ext2disk.img   (mounted at /mnt)
#   4  CFS    313344 .. 327646  ~7 MB   cfsdisk.img    (mounted at /cfs)
$(DISK_IMG): $(KERNEL_ELF) $(FAT32_IMG) $(EXT2_IMG) $(CFS_IMG) limine.conf
	@echo "Creating UEFI disk image..."
	dd if=/dev/zero of=$(DISK_IMG) bs=1M count=160 2>/dev/null
	sgdisk -n 1:2048:67583    -t 1:ef00 \
	       -n 2:67584:198655  -t 2:0700 \
	       -n 3:198656:313343 -t 3:8300 \
	       -n 4:313344:327646 -t 4:8300 \
	       $(DISK_IMG)
	mformat -i $(DISK_IMG)@@1M -F -v EFI ::
	mmd    -i $(DISK_IMG)@@1M ::/EFI ::/EFI/BOOT
	mcopy  -i $(DISK_IMG)@@1M $(LIMINE_DIR)/BOOTX64.EFI   ::/EFI/BOOT/
	mcopy  -i $(DISK_IMG)@@1M limine.conf                 ::/
	mcopy  -i $(DISK_IMG)@@1M $(KERNEL_ELF)               ::/
	@# mount points so the ext2 and CFS volumes show up in `ls /`
	@mdir -i $(FAT32_IMG) ::mnt >/dev/null 2>&1 || mmd -i $(FAT32_IMG) ::mnt
	@mdir -i $(FAT32_IMG) ::cfs >/dev/null 2>&1 || mmd -i $(FAT32_IMG) ::cfs
	dd if=$(FAT32_IMG) of=$(DISK_IMG) bs=512 seek=67584  conv=notrunc 2>/dev/null
	dd if=$(EXT2_IMG)  of=$(DISK_IMG) bs=512 seek=198656 conv=notrunc 2>/dev/null
	dd if=$(CFS_IMG)   of=$(DISK_IMG) bs=512 seek=313344 conv=notrunc 2>/dev/null
	@echo "Disk image ready: $(DISK_IMG)"

# -----------------------------------------------------------------------
# ext2 data disk (56 MB, populated from a staging directory)
# -----------------------------------------------------------------------

EXT2_STAGE = $(OBJDIR)/ext2root

$(EXT2_IMG): Makefile | $(OBJDIR)
	@echo "Building ext2 data disk..."
	rm -rf $(EXT2_STAGE) && mkdir -p $(EXT2_STAGE)/etc $(EXT2_STAGE)/bin $(EXT2_STAGE)/home/user
	@printf "Hello from ext2!\n" > $(EXT2_STAGE)/hello.txt
	@printf 'NAME="Computer OS"\nPRETTY_NAME="Computer OS (64-bit)"\nID=computeros\nVERSION_ID=0.1\n' > $(EXT2_STAGE)/etc/os-release
	@printf "This file lives on an ext2 partition.\n" > $(EXT2_STAGE)/home/user/notes.txt
	ln -s hello.txt $(EXT2_STAGE)/greeting
	-cp userprogs/c/*.elf $(EXT2_STAGE)/bin/ 2>/dev/null
	rm -f $(EXT2_IMG)
	$(MKE2FS) -q -F -t ext2 -b 4096 -m 0 -L computeros \
	    -O ^metadata_csum,^uninit_bg -E root_owner=0:0 \
	    -d $(EXT2_STAGE) $(EXT2_IMG) 56M
	@echo "ext2 disk ready: $(EXT2_IMG)"

# -----------------------------------------------------------------------
# CFS data disk (~7 MB, built by our own mkfs.cfs host tool)
# -----------------------------------------------------------------------
# Partition 4 is sectors 313344..327646 = 14303 sectors; at 8 sectors/block
# that is 1787 CFS blocks (14296 sectors, fits with room to spare).

MKFS_CFS  = $(OBJDIR)/mkfs_cfs
CFS_STAGE = $(OBJDIR)/cfsroot
CFS_BLOCKS = 1787

# The host tool shares the filesystem core (src/cfs.c), compiled WITHOUT
# -DCFS_KERNEL so the ATA binding is excluded. Both halves use the host cc.
$(MKFS_CFS): src/mkfs_cfs.c src/cfs.c include/cfs.h | $(OBJDIR)
	@echo "Building mkfs.cfs (host tool)..."
	$(HOSTCC) -O2 -std=c11 -fno-builtin -Iinclude -c src/cfs.c -o $(OBJDIR)/cfs_host.o
	$(HOSTCC) -O2 -std=c11 -c src/mkfs_cfs.c -o $(OBJDIR)/mkfs_cfs.o
	$(HOSTCC) $(OBJDIR)/cfs_host.o $(OBJDIR)/mkfs_cfs.o -o $(MKFS_CFS)

$(CFS_IMG): $(MKFS_CFS) Makefile | $(OBJDIR)
	@echo "Building CFS data disk..."
	rm -rf $(CFS_STAGE) && mkdir -p $(CFS_STAGE)/home/user $(CFS_STAGE)/etc
	@printf "Hello from CFS!\nExtent-based, long filenames, journaled-to-be.\n" > $(CFS_STAGE)/hello.txt
	@printf 'NAME="Computer OS"\nFS=cfs\n' > $(CFS_STAGE)/etc/os-release
	@printf "This file lives on a native CFS partition.\n" > $(CFS_STAGE)/home/user/notes.txt
	$(MKFS_CFS) $(CFS_IMG) $(CFS_BLOCKS) computeros $(CFS_STAGE)
	@echo "CFS disk ready: $(CFS_IMG)"

# -----------------------------------------------------------------------
# FAT32 data disk
# -----------------------------------------------------------------------

$(FAT32_IMG): userprogs/hello.asm userprogs/count.asm \
              userprogs/c/testc.c userprogs/c/echo.c \
              userprogs/c/lib/stdio.c userprogs/c/lib/string.c \
              userprogs/c/lib/crt0.s userprogs/c/user.ld \
              userprogs/c/Makefile \
              apk.static
	@echo "Building FAT32 data disk..."
	dd if=/dev/zero of=$(FAT32_IMG) bs=1M count=64 2>/dev/null
	mkfs.fat -F 32 -n COMPUTEROS $(FAT32_IMG)
	@echo "Hello World" > /tmp/_helloworld.txt
	@printf "Computer_OS\nA 64-bit operating system.\n" > /tmp/_readme.txt
	$(AS) -f bin userprogs/hello.asm -o /tmp/_hello.bin
	$(AS) -f bin userprogs/count.asm -o /tmp/_count.bin
	$(MAKE) -C userprogs/c all
	@printf "hello world!\n" > /tmp/_helloworld2.txt
	@printf "this is file 2 used for testing\n" > /tmp/_file2_testing.txt
	COPYFILE_DISABLE=1 tar -cf /tmp/_test.tar -C /tmp _helloworld2.txt _file2_testing.txt
	COPYFILE_DISABLE=1 tar -czf /tmp/_tarball.tar.gz -C /tmp _helloworld2.txt _file2_testing.txt
	@printf '#include <unistd.h>\n#include <fcntl.h>\nvoid _start(void){write(1,"before open\\n",12);int fd=open("/tmp/test2.txt",O_WRONLY|O_CREAT|O_TRUNC,0644);write(1,"after open\\n",11);if(fd<0){write(1,"open failed\\n",12);_exit(1);}write(1,"before write\\n",13);write(fd,"hello from musl\\n",17);write(1,"after write\\n",12);close(fd);write(1,"done\\n",5);_exit(0);}\n' > /tmp/_fileio2.c
	x86_64-linux-musl-gcc -static -nostartfiles -o /tmp/_fileio2 /tmp/_fileio2.c
	@printf '#include <unistd.h>\nvoid _start(void){const char msg[]="Hello from musl!\\n";write(1,msg,sizeof(msg)-1);_exit(0);}\n' > /tmp/_hello_musl.c
	x86_64-linux-musl-gcc -static -nostartfiles -o /tmp/_hello_musl /tmp/_hello_musl.c
	mcopy -i $(FAT32_IMG) /tmp/_helloworld.txt       ::HELLO.TXT
	mcopy -i $(FAT32_IMG) /tmp/_readme.txt            ::README.TXT
	mcopy -i $(FAT32_IMG) /tmp/_hello.bin             ::HELLO.BIN
	mcopy -i $(FAT32_IMG) /tmp/_count.bin             ::COUNT.BIN
	mcopy -i $(FAT32_IMG) userprogs/c/testc.elf       ::TESTC.ELF
	mcopy -i $(FAT32_IMG) userprogs/c/echo.elf        ::ECHO.ELF
	mcopy -i $(FAT32_IMG) userprogs/c/maltest.elf     ::MALTEST.ELF
	mcopy -i $(FAT32_IMG) userprogs/c/colors.elf      ::COLORS.ELF
	mcopy -i $(FAT32_IMG) userprogs/c/game.elf        ::GAME.ELF
	mcopy -i $(FAT32_IMG) userprogs/c/calc.elf        ::CALC.ELF
	mcopy -i $(FAT32_IMG) userprogs/c/forktest.elf    ::FORKTEST.ELF
	mcopy -i $(FAT32_IMG) /tmp/_test.tar              ::test.tar
	mcopy -i $(FAT32_IMG) /tmp/_tarball.tar.gz        ::tarball.tar.gz
	mcopy -i $(FAT32_IMG) /tmp/_fileio2               ::fileio2
	mcopy -i $(FAT32_IMG) /tmp/_hello_musl            ::hello
	@# --- apk filesystem structure ---
	mmd -i $(FAT32_IMG) ::etc
	mmd -i $(FAT32_IMG) ::etc/apk
	mmd -i $(FAT32_IMG) ::etc/apk/keys
	mmd -i $(FAT32_IMG) ::etc/apk/protected_paths.d
	mmd -i $(FAT32_IMG) ::lib
	mmd -i $(FAT32_IMG) ::lib/apk
	mmd -i $(FAT32_IMG) ::lib/apk/db
	mmd -i $(FAT32_IMG) ::var
	mmd -i $(FAT32_IMG) ::var/cache
	mmd -i $(FAT32_IMG) ::var/cache/apk
	mmd -i $(FAT32_IMG) ::usr
	mmd -i $(FAT32_IMG) ::usr/lib
	@printf "x86_64\n"  > /tmp/_apk_arch
	@echo "http://151.101.2.132/alpine/v3.20/main" > /tmp/_apk_repos
	@echo "http://151.101.2.132/alpine/v3.20/community" >> /tmp/_apk_repos
	@printf ""           > /tmp/_apk_world
	@printf ""           > /tmp/_apk_installed
	mcopy -i $(FAT32_IMG) /tmp/_apk_arch        ::etc/apk/arch
	mcopy -i $(FAT32_IMG) /tmp/_apk_repos       ::etc/apk/repositories
	mcopy -i $(FAT32_IMG) /tmp/_apk_world       ::etc/apk/world
	mcopy -i $(FAT32_IMG) /tmp/_apk_installed   ::lib/apk/db/installed
	@# Alpine signing key (works across releases)
	@if [ -f /tmp/alpine-devel@lists.alpinelinux.org-6165ee59.rsa.pub ]; then \
	  mcopy -i $(FAT32_IMG) "/tmp/alpine-devel@lists.alpinelinux.org-6165ee59.rsa.pub" \
	    "::etc/apk/keys/alpine-devel@lists.alpinelinux.org-6165ee59.rsa.pub"; \
	fi
	@# /etc/resolv.conf — point to QEMU SLIRP DNS (10.0.2.3)
	@echo "nameserver 10.0.2.3" > /tmp/_resolv.conf
	@echo "127.0.0.1 localhost" > /tmp/_hosts
	@echo "151.101.2.132 dl-cdn.alpinelinux.org" >> /tmp/_hosts
	mcopy -i $(FAT32_IMG) /tmp/_resolv.conf     ::etc/resolv.conf
	mcopy -i $(FAT32_IMG) /tmp/_hosts           ::etc/hosts
	@# /etc/os-release — lets fastfetch and friends show the OS name
	@printf 'NAME="Computer OS"\nPRETTY_NAME="Computer OS (64-bit)"\nID=computeros\nVERSION_ID=0.1\n' > /tmp/_osrel
	mcopy -i $(FAT32_IMG) /tmp/_osrel            ::etc/os-release
	@echo "hosts: files" > /tmp/_nsswitch.conf
	@echo "networks: files" >> /tmp/_nsswitch.conf
	mcopy -i $(FAT32_IMG) /tmp/_nsswitch.conf   ::etc/nsswitch.conf
	@rm -f /tmp/_apk_arch /tmp/_apk_repos /tmp/_apk_world /tmp/_apk_installed /tmp/_resolv.conf /tmp/_hosts /tmp/_osrel
	@# --- apk.static binary ---
	mcopy -i $(FAT32_IMG) apk.static           ::apk.static
	@rm -f /tmp/_helloworld.txt /tmp/_readme.txt /tmp/_hello.bin /tmp/_count.bin
	@rm -f /tmp/_helloworld2.txt /tmp/_file2_testing.txt /tmp/_test.tar /tmp/_tarball.tar.gz
	@rm -f /tmp/_fileio2.c /tmp/_fileio2 /tmp/_hello_musl.c /tmp/_hello_musl
	@echo "FAT32 disk ready: $(FAT32_IMG)"

# -----------------------------------------------------------------------
# QEMU
# -----------------------------------------------------------------------

OVMF ?= /opt/homebrew/share/qemu/edk2-x86_64-code.fd

run: $(DISK_IMG)
	@echo "Booting in QEMU (UEFI)..."
	$(QEMU) \
	    -drive if=pflash,format=raw,readonly=on,file=$(OVMF) \
	    -drive file=$(DISK_IMG),format=raw \
	    -netdev user,id=net0 \
	    -device e1000,netdev=net0 \
	    -m 8G \
	    -cpu max \
	    -accel tcg \
	    -display cocoa,zoom-to-fit=on

debug: $(DISK_IMG)
	@rm -f /tmp/serial.log && touch /tmp/serial.log
	@tail -f /tmp/serial.log & \
	$(QEMU) \
	    -drive if=pflash,format=raw,readonly=on,file=$(OVMF) \
	    -drive file=$(DISK_IMG),format=raw \
	    -netdev user,id=net0 \
	    -device e1000,netdev=net0 \
	    -m 8G \
	    -cpu max \
	    -accel tcg \
	    -serial file:/tmp/serial.log \
	    -no-reboot \
	    -d cpu_reset,guest_errors \
	    -D /tmp/qemu_debug.log \
	    -display cocoa,zoom-to-fit=on; \
	kill %% 2>/dev/null; true

# Set up apk filesystem on existing fat32disk.img without full rebuild
apk-fs:
	@echo "Setting up apk filesystem structure on $(FAT32_IMG)..."
	-mmd -i $(FAT32_IMG) ::etc
	-mmd -i $(FAT32_IMG) ::etc/apk
	-mmd -i $(FAT32_IMG) ::etc/apk/keys
	-mmd -i $(FAT32_IMG) ::etc/apk/protected_paths.d
	-mmd -i $(FAT32_IMG) ::lib
	-mmd -i $(FAT32_IMG) ::lib/apk
	-mmd -i $(FAT32_IMG) ::lib/apk/db
	-mmd -i $(FAT32_IMG) ::var
	-mmd -i $(FAT32_IMG) ::var/cache
	-mmd -i $(FAT32_IMG) ::var/cache/apk
	-mmd -i $(FAT32_IMG) ::usr
	-mmd -i $(FAT32_IMG) ::usr/lib
	@printf "x86_64\n" > /tmp/_apk_arch
	@echo "http://151.101.2.132/alpine/v3.20/main" > /tmp/_apk_repos
	@echo "http://151.101.2.132/alpine/v3.20/community" >> /tmp/_apk_repos
	@printf "" > /tmp/_apk_world
	@printf "" > /tmp/_apk_installed
	-mcopy -oi $(FAT32_IMG) /tmp/_apk_arch      ::etc/apk/arch
	-mcopy -oi $(FAT32_IMG) /tmp/_apk_repos     ::etc/apk/repositories
	-mcopy -oi $(FAT32_IMG) /tmp/_apk_world     ::etc/apk/world
	-mcopy -oi $(FAT32_IMG) /tmp/_apk_installed ::lib/apk/db/installed
	@echo "nameserver 10.0.2.3" > /tmp/_resolv.conf
	@echo "127.0.0.1 localhost" > /tmp/_hosts
	@echo "151.101.2.132 dl-cdn.alpinelinux.org" >> /tmp/_hosts
	-mcopy -oi $(FAT32_IMG) /tmp/_resolv.conf   ::etc/resolv.conf
	-mcopy -oi $(FAT32_IMG) /tmp/_hosts         ::etc/hosts
	@echo "hosts: files" > /tmp/_nsswitch.conf
	@echo "networks: files" >> /tmp/_nsswitch.conf
	-mcopy -oi $(FAT32_IMG) /tmp/_nsswitch.conf ::etc/nsswitch.conf
	@rm -f /tmp/_apk_arch /tmp/_apk_repos /tmp/_apk_world /tmp/_apk_installed /tmp/_resolv.conf /tmp/_hosts /tmp/_osrel /tmp/_nsswitch.conf
	mcopy -oi $(FAT32_IMG) apk.static          ::apk.static
	@echo "Done. Run 'make' to rebuild disk.img with the new FAT32 image."

clean:
	rm -rf $(OBJDIR) *.elf $(DISK_IMG) $(FAT32_IMG) $(EXT2_IMG) $(CFS_IMG)
	-$(MAKE) -C userprogs/c clean 2>/dev/null || true