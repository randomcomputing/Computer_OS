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
          -Iinclude -c -std=c11

LDFLAGS = -T linker.ld -nostdlib

LIMINE_DIR ?= limine

KERNEL_ELF  = kernel.elf
DISK_IMG    = disk.img
FAT_IMG     = fatdisk.img

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
FAT12_O    = $(OBJDIR)/fat12.o
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
BANNER_O   = $(OBJDIR)/boot_banner.o

KERNEL_OBJS = \
    $(ENTRY_O) $(HALT_O) \
    $(ISR_ASM_O) $(IRQ_ASM_O) $(TASK_ASM_O) $(SYSCALL_ASM_O) \
    $(KERNEL_O) $(GDT_O) $(IDT_O) $(ISR_O) $(PIC_O) $(IRQ_O) \
    $(KEYBOARD_O) $(MOUSE_O) $(PIT_O) $(SERIAL_O) \
    $(MEMMAP_O) $(PMM_O) $(VMM_O) $(KHEAP_O) \
    $(TASK_O) $(ATA_O) $(FAT12_O) $(VFS_O) $(RAMFS_O) \
    $(SYSCALL_O) $(USERPROG_O) $(LOADER_O) $(EDITOR_O) \
    $(RTC_O) $(GFX_O) $(FONT_O) $(UACCESS_O) \
    $(PCI_O) $(E1000_O) $(ARP_O) $(NET_O) $(TCP_O) \
    $(FBCON_O) $(BVBE_O) $(CONSOLE_O) $(STRING_O) $(PRINTF_O) \
    $(REBOOT_O) $(ACPI_O) $(VGA_O) $(SHELL_O) $(BANNER_O)

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
$(FAT12_O):   src/fat12.c | $(OBJDIR);    $(CC) $(CFLAGS) $< -o $@
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
$(BANNER_O):  src/boot_banner.c | $(OBJDIR); $(CC) $(CFLAGS) $< -o $@

$(KERNEL_ELF): $(KERNEL_OBJS) linker.ld
	@echo "Linking kernel ELF..."
	$(LD) $(LDFLAGS) -o $@ $(KERNEL_OBJS)

# -----------------------------------------------------------------------
# Disk image
# -----------------------------------------------------------------------

$(DISK_IMG): $(KERNEL_ELF) $(FAT_IMG) limine.conf
	@echo "Creating UEFI disk image..."
	dd if=/dev/zero of=$(DISK_IMG) bs=1M count=128 2>/dev/null
	sgdisk -n 1:2048:67583   -t 1:ef00 \
	       -n 2:67584:0      -t 2:0700 \
	       $(DISK_IMG)
	mformat -i $(DISK_IMG)@@1M -F -v EFI ::
	mmd    -i $(DISK_IMG)@@1M ::/EFI ::/EFI/BOOT
	mcopy  -i $(DISK_IMG)@@1M $(LIMINE_DIR)/BOOTX64.EFI   ::/EFI/BOOT/
	mcopy  -i $(DISK_IMG)@@1M limine.conf                 ::/
	mcopy  -i $(DISK_IMG)@@1M $(KERNEL_ELF)               ::/
	dd if=$(FAT_IMG) of=$(DISK_IMG) bs=512 seek=67584 conv=notrunc 2>/dev/null
	@echo "Disk image ready: $(DISK_IMG)"

# -----------------------------------------------------------------------
# FAT12 data disk
# -----------------------------------------------------------------------

$(FAT_IMG): userprogs/hello.asm userprogs/count.asm \
            userprogs/c/testc.c userprogs/c/echo.c \
            userprogs/c/lib/stdio.c userprogs/c/lib/string.c \
            userprogs/c/lib/crt0.s userprogs/c/user.ld \
            userprogs/c/Makefile
	@echo "Building FAT12 data disk..."
	dd if=/dev/zero of=$(FAT_IMG) bs=512 count=2880 2>/dev/null
	mformat -i $(FAT_IMG) -f 1440 -v COMPUTEROS ::
	@echo "Hello World" > /tmp/_helloworld.txt
	@printf "Computer_OS\nA 64-bit operating system.\n" > /tmp/_readme.txt
	$(AS) -f bin userprogs/hello.asm -o /tmp/_hello.bin
	$(AS) -f bin userprogs/count.asm -o /tmp/_count.bin
	$(MAKE) -C userprogs/c all
	mcopy -i $(FAT_IMG) /tmp/_helloworld.txt    ::HELLO.TXT
	mcopy -i $(FAT_IMG) /tmp/_readme.txt         ::README.TXT
	mcopy -i $(FAT_IMG) /tmp/_hello.bin          ::HELLO.BIN
	mcopy -i $(FAT_IMG) /tmp/_count.bin          ::COUNT.BIN
	mcopy -i $(FAT_IMG) userprogs/c/testc.elf    ::TESTC.ELF
	mcopy -i $(FAT_IMG) userprogs/c/echo.elf     ::ECHO.ELF
	mcopy -i $(FAT_IMG) userprogs/c/maltest.elf  ::MALTEST.ELF
	mcopy -i $(FAT_IMG) userprogs/c/colors.elf   ::COLORS.ELF
	mcopy -i $(FAT_IMG) userprogs/c/game.elf     ::GAME.ELF
	mcopy -i $(FAT_IMG) userprogs/c/calc.elf     ::CALC.ELF
	mcopy -i $(FAT_IMG) userprogs/c/forktest.elf ::FORKTEST.ELF
	@rm -f /tmp/_helloworld.txt /tmp/_readme.txt /tmp/_hello.bin /tmp/_count.bin
	@echo "FAT12 disk ready: $(FAT_IMG)"

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
	    -m 256M \
	    -accel tcg \
	    -display cocoa,zoom-to-fit=on

debug: $(DISK_IMG)
	@echo "Booting in QEMU (UEFI, debug)..."
	$(QEMU) \
	    -drive if=pflash,format=raw,readonly=on,file=$(OVMF) \
	    -drive file=$(DISK_IMG),format=raw \
	    -netdev user,id=net0 \
	    -device e1000,netdev=net0 \
	    -m 256M \
	    -serial stdio \
	    -no-reboot \
	    -accel tcg \
	    -display cocoa,zoom-to-fit=on

clean:
	rm -rf $(OBJDIR) *.elf $(DISK_IMG) $(FAT_IMG)
	$(MAKE) -C userprogs/c clean