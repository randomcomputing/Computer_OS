AS = nasm
CC = i686-elf-gcc
LD = i686-elf-ld
DD = dd
MKISOFS = mkisofs
QEMU = qemu-system-x86_64

ASFLAGS_BIN = -f bin
ASFLAGS_ELF = -f elf32
CFLAGS = -ffreestanding -fno-asynchronous-unwind-tables -fno-pie -fno-stack-protector -nostdlib -nostdinc -Iinclude -c
LDFLAGS = -T linker.ld

BOOT_BIN     = boot.bin
KERNEL_BIN   = kernel.bin
ENTRY_O      = entry.o
KERNEL_O     = kernel.o
HALT_O       = halt.o
VGA_O        = vga.o
CONSOLE_O    = console.o
PRINTF_O     = printf.o
IDT_O        = idt.o
ISR_O        = isr.o
ISR_ASM_O    = isr_asm.o
PIC_O        = pic.o
IRQ_O        = irq.o
IRQ_ASM_O    = irq_asm.o
KEYBOARD_O   = keyboard.o
MOUSE_O      = mouse.o
STRING_O     = string.o
SHELL_O      = shell.o
REBOOT_O     = reboot.o
ACPI_O       = acpi.o
PIT_O        = pit.o
SERIAL_O     = serial.o
MEMMAP_O     = memmap.o
PMM_O        = pmm.o
VMM_O        = vmm.o
KHEAP_O      = kheap.o
TASK_O       = task.o
TASK_ASM_O   = task_switch.o
ATA_O        = ata.o
FAT12_O      = fat12.o
VFS_O        = vfs.o
RAMFS_O      = ramfs.o
GDT_O        = gdt.o
SYSCALL_O    = syscall.o
SYSCALL_ASM_O = syscall_asm.o
USERPROG_O   = userprog.o
LOADER_O     = loader.o
EDITOR_O     = editor.o
RTC_O        = rtc.o
GFX_O        = gfx.o
FONT_O       = font8x16.o
DISK_IMG     = disk.img
UACCESS_O    = uaccess.o
PCI_O        = pci.o
E1000_O      = e1000.o
ARP_O        = arp.o
NET_O        = net.o
TCP_O        = tcp.o
BVBE_O       = bochs_vbe.o
FBCON_O      = fbcon.o
BANNER_O     = boot_banner.o

FAT_IMG      = fatdisk.img
ISO_FILE     = Computer_OS.iso

KERNEL_OBJS = $(ENTRY_O) $(KERNEL_O) $(HALT_O) $(VGA_O) $(CONSOLE_O) $(PRINTF_O) \
              $(IDT_O) $(ISR_O) $(ISR_ASM_O) \
              $(PIC_O) $(IRQ_O) $(IRQ_ASM_O) $(KEYBOARD_O) $(MOUSE_O) \
              $(STRING_O) $(SHELL_O) $(REBOOT_O) $(ACPI_O) $(PIT_O) $(SERIAL_O) \
              $(MEMMAP_O) $(PMM_O) $(VMM_O) $(KHEAP_O) \
              $(TASK_O) $(TASK_ASM_O) $(ATA_O) $(FAT12_O) $(VFS_O) $(RAMFS_O) \
              $(GDT_O) $(SYSCALL_O) $(SYSCALL_ASM_O) $(USERPROG_O) \
			  $(LOADER_O) $(EDITOR_O) $(RTC_O) $(UACCESS_O) $(GFX_O) $(FONT_O) $(PCI_O) $(E1000_O) $(ARP_O) $(NET_O) $(TCP_O) $(BVBE_O) $(FBCON_O) $(BANNER_O)
ISO_DIR = isodir

all: $(ISO_FILE)

$(BOOT_BIN): boot.asm
	@echo "Assembling bootloader..."
	$(AS) $(ASFLAGS_BIN) boot.asm -o $(BOOT_BIN)

$(ENTRY_O): entry.asm
	@echo "Assembling kernel entry..."
	$(AS) $(ASFLAGS_ELF) entry.asm -o $(ENTRY_O)

$(HALT_O): halt.asm
	@echo "Assembling halt helper..."
	$(AS) $(ASFLAGS_ELF) halt.asm -o $(HALT_O)

$(ISR_ASM_O): src/isr.asm
	@echo "Assembling ISR stubs..."
	$(AS) $(ASFLAGS_ELF) src/isr.asm -o $(ISR_ASM_O)

$(IRQ_ASM_O): src/irq_asm.asm
	@echo "Assembling IRQ stubs..."
	$(AS) $(ASFLAGS_ELF) src/irq_asm.asm -o $(IRQ_ASM_O)

$(KERNEL_O): kernel.c include/console.h include/vga.h include/printf.h include/idt.h include/pic.h include/keyboard.h include/mouse.h include/pit.h include/shell.h include/memmap.h include/pmm.h include/vmm.h include/kheap.h include/task.h include/ata.h include/fat12.h include/gdt.h include/syscall.h include/pci.h include/e1000.h
	@echo "Compiling C kernel..."
	$(CC) $(CFLAGS) kernel.c -o $(KERNEL_O)

$(VGA_O): src/vga.c include/vga.h include/io.h
	@echo "Compiling VGA driver..."
	$(CC) $(CFLAGS) src/vga.c -o $(VGA_O)

$(GFX_O): src/gfx.c include/gfx.h include/io.h include/font8x16.h
	@echo "Compiling graphics (mode 13h) driver..."
	$(CC) $(CFLAGS) src/gfx.c -o $(GFX_O)

$(PRINTF_O): src/printf.c include/printf.h include/console.h include/vga.h include/serial.h
	@echo "Compiling printf..."
	$(CC) $(CFLAGS) src/printf.c -o $(PRINTF_O)

$(IDT_O): src/idt.c include/idt.h
	@echo "Compiling IDT..."
	$(CC) $(CFLAGS) src/idt.c -o $(IDT_O)

$(ISR_O): src/isr.c include/isr.h include/console.h include/vga.h include/printf.h include/vmm.h
	@echo "Compiling ISR handler..."
	$(CC) $(CFLAGS) src/isr.c -o $(ISR_O)

$(PIC_O): src/pic.c include/pic.h include/io.h
	@echo "Compiling PIC driver..."
	$(CC) $(CFLAGS) src/pic.c -o $(PIC_O)

$(IRQ_O): src/irq.c include/irq.h include/pic.h include/isr.h
	@echo "Compiling IRQ dispatcher..."
	$(CC) $(CFLAGS) src/irq.c -o $(IRQ_O)

$(KEYBOARD_O): src/keyboard.c include/keyboard.h include/irq.h include/pic.h include/io.h include/isr.h
	@echo "Compiling keyboard driver..."
	$(CC) $(CFLAGS) src/keyboard.c -o $(KEYBOARD_O)

$(MOUSE_O): src/mouse.c include/mouse.h include/irq.h include/pic.h include/io.h include/isr.h include/console.h include/vga.h
	@echo "Compiling mouse driver..."
	$(CC) $(CFLAGS) src/mouse.c -o $(MOUSE_O)

$(STRING_O): src/string.c include/string.h
	@echo "Compiling string utils..."
	$(CC) $(CFLAGS) src/string.c -o $(STRING_O)

$(PIT_O): src/pit.c include/pit.h include/io.h include/irq.h include/pic.h include/isr.h include/task.h
	@echo "Compiling PIT driver..."
	$(CC) $(CFLAGS) src/pit.c -o $(PIT_O)

$(SERIAL_O): src/serial.c include/serial.h include/io.h
	@echo "Compiling serial driver..."
	$(CC) $(CFLAGS) src/serial.c -o $(SERIAL_O)

$(SHELL_O): src/shell.c include/shell.h include/console.h include/vga.h include/printf.h include/keyboard.h include/string.h include/pit.h include/memmap.h include/pmm.h include/vmm.h include/kheap.h include/task.h include/fat12.h include/io.h include/userprog.h include/rtc.h include/loader.h include/editor.h include/pci.h include/e1000.h
	@echo "Compiling shell..."
	$(CC) $(CFLAGS) src/shell.c -o $(SHELL_O)

$(REBOOT_O): src/reboot.c include/reboot.h include/console.h include/serial.h include/io.h
	@echo "Compiling reboot..."
	$(CC) $(CFLAGS) src/reboot.c -o $(REBOOT_O)

$(ACPI_O): src/acpi.c include/acpi.h include/io.h include/printf.h include/serial.h include/vmm.h
	@echo "Compiling ACPI..."
	$(CC) $(CFLAGS) src/acpi.c -o $(ACPI_O)

$(MEMMAP_O): src/memmap.c include/memmap.h include/printf.h
	@echo "Compiling memory map..."
	$(CC) $(CFLAGS) src/memmap.c -o $(MEMMAP_O)

$(PMM_O): src/pmm.c include/pmm.h include/memmap.h include/printf.h
	@echo "Compiling physical memory manager..."
	$(CC) $(CFLAGS) src/pmm.c -o $(PMM_O)

$(VMM_O): src/vmm.c include/vmm.h include/pmm.h include/printf.h include/console.h include/vga.h include/isr.h include/task.h
	@echo "Compiling virtual memory manager..."
	$(CC) $(CFLAGS) src/vmm.c -o $(VMM_O)

$(KHEAP_O): src/kheap.c include/kheap.h include/vmm.h include/pmm.h include/printf.h
	@echo "Compiling kernel heap..."
	$(CC) $(CFLAGS) src/kheap.c -o $(KHEAP_O)

$(TASK_O): src/task.c include/task.h include/kheap.h include/printf.h include/string.h include/gdt.h include/vmm.h
	@echo "Compiling scheduler..."
	$(CC) $(CFLAGS) src/task.c -o $(TASK_O)

$(TASK_ASM_O): src/task_switch.asm
	@echo "Assembling context switch..."
	$(AS) $(ASFLAGS_ELF) src/task_switch.asm -o $(TASK_ASM_O)

$(GDT_O): src/gdt.c include/gdt.h
	@echo "Compiling GDT/TSS..."
	$(CC) $(CFLAGS) src/gdt.c -o $(GDT_O)

$(SYSCALL_O): src/syscall.c include/syscall.h include/idt.h include/console.h include/vga.h include/printf.h include/task.h include/keyboard.h include/isr.h include/uaccess.h include/loader.h
	@echo "Compiling syscall dispatcher..."
	$(CC) $(CFLAGS) src/syscall.c -o $(SYSCALL_O)

$(SYSCALL_ASM_O): src/syscall_asm.asm
	@echo "Assembling syscall stub..."
	$(AS) $(ASFLAGS_ELF) src/syscall_asm.asm -o $(SYSCALL_ASM_O)

$(USERPROG_O): src/userprog.c include/userprog.h include/vmm.h include/pmm.h include/task.h include/printf.h include/string.h
	@echo "Compiling userprog launcher..."
	$(CC) $(CFLAGS) src/userprog.c -o $(USERPROG_O)

$(LOADER_O): src/loader.c include/loader.h include/elf.h include/fat12.h include/vmm.h include/pmm.h include/task.h include/kheap.h include/string.h include/printf.h
	@echo "Compiling user-program loader..."
	$(CC) $(CFLAGS) src/loader.c -o $(LOADER_O)

$(EDITOR_O): src/editor.c include/editor.h include/console.h include/vga.h include/keyboard.h include/kheap.h include/string.h include/printf.h include/fat12.h
	@echo "Compiling text editor..."
	$(CC) $(CFLAGS) src/editor.c -o $(EDITOR_O)

$(RTC_O): src/rtc.c include/rtc.h include/io.h
	@echo "Compiling RTC driver..."
	$(CC) $(CFLAGS) src/rtc.c -o $(RTC_O)

$(UACCESS_O): src/uaccess.c include/uaccess.h include/task.h
	@echo "Compiling user-access helpers..."
	$(CC) $(CFLAGS) src/uaccess.c -o $(UACCESS_O)

$(ATA_O): src/ata.c include/ata.h include/io.h include/printf.h
	@echo "Compiling ATA driver..."
	$(CC) $(CFLAGS) src/ata.c -o $(ATA_O)

$(FAT12_O): src/fat12.c include/fat12.h include/ata.h include/string.h include/kheap.h include/printf.h
	@echo "Compiling FAT12 driver..."
	$(CC) $(CFLAGS) src/fat12.c -o $(FAT12_O)

$(VFS_O): src/vfs.c include/vfs.h include/fat12.h
	@echo "Compiling VFS layer..."
	$(CC) $(CFLAGS) src/vfs.c -o $(VFS_O)

$(RAMFS_O): src/ramfs.c include/ramfs.h include/vfs.h
	@echo "Compiling ramfs..."
	$(CC) $(CFLAGS) src/ramfs.c -o $(RAMFS_O)

$(KERNEL_BIN): $(KERNEL_OBJS) linker.ld
	@echo "Linking kernel..."
	$(LD) $(LDFLAGS) -o $(KERNEL_BIN) $(KERNEL_OBJS)

$(DISK_IMG): $(BOOT_BIN) $(KERNEL_BIN)
	@echo "Creating disk image..."
	$(DD) if=/dev/zero of=$(DISK_IMG) bs=512 count=2880 2>/dev/null
	$(DD) if=$(BOOT_BIN) of=$(DISK_IMG) conv=notrunc bs=512 seek=0 2>/dev/null
	$(DD) if=$(KERNEL_BIN) of=$(DISK_IMG) conv=notrunc bs=512 seek=1 2>/dev/null

$(ISO_FILE): $(DISK_IMG)
	@echo "Creating ISO..."
	mkdir -p $(ISO_DIR)
	cp $(DISK_IMG) $(ISO_DIR)/boot.img
	$(MKISOFS) -quiet -o $(ISO_FILE) \
	           -b boot.img \
	           $(ISO_DIR)
	@echo "Build complete! ISO created: $(ISO_FILE)"

# 1.44 MB FAT12 floppy image populated with test files. macOS uses
# `newfs_msdos` (built-in) to format and `mcopy` (from `mtools`, via
# `brew install mtools`) to drop files in without needing root.
$(FAT_IMG): userprogs/hello.asm userprogs/count.asm \
            userprogs/c/testc.c userprogs/c/echo.c \
            userprogs/c/lib/stdio.c userprogs/c/lib/string.c \
            userprogs/c/lib/crt0.s userprogs/c/user.ld \
            userprogs/c/Makefile
	@echo "Building FAT12 disk image..."
	$(DD) if=/dev/zero of=$(FAT_IMG) bs=512 count=2880 2>/dev/null
	mformat -i $(FAT_IMG) -f 1440 -v COMPUTEROS ::
	@echo "Hello World" > /tmp/_helloworld.txt
	@printf "Computer_OS\nA 32bit operating system.\n" > /tmp/_readme.txt
	$(AS) $(ASFLAGS_BIN) userprogs/hello.asm -o /tmp/_hello.bin
	$(AS) $(ASFLAGS_BIN) userprogs/count.asm -o /tmp/_count.bin
	@echo "Building C user programs (ELF)..."
	$(MAKE) -C userprogs/c all
	mcopy -i $(FAT_IMG) /tmp/_helloworld.txt   ::HELLO.TXT
	mcopy -i $(FAT_IMG) /tmp/_readme.txt        ::README.TXT
	mcopy -i $(FAT_IMG) /tmp/_hello.bin         ::HELLO.BIN
	mcopy -i $(FAT_IMG) /tmp/_count.bin         ::COUNT.BIN
	mcopy -i $(FAT_IMG) userprogs/c/testc.elf   ::TESTC.ELF
	mcopy -i $(FAT_IMG) userprogs/c/echo.elf    ::ECHO.ELF
	mcopy -i $(FAT_IMG) userprogs/c/maltest.elf ::MALTEST.ELF
	mcopy -i $(FAT_IMG) userprogs/c/colors.elf  ::COLORS.ELF
	mcopy -i $(FAT_IMG) userprogs/c/game.elf    ::GAME.ELF
	mcopy -i $(FAT_IMG) userprogs/c/calc.elf    ::CALC.ELF
	mcopy -i $(FAT_IMG) userprogs/c/forktest.elf ::FORKTEST.ELF



	@rm -f /tmp/_helloworld.txt /tmp/_readme.txt /tmp/_hello.bin /tmp/_count.bin
	@echo "FAT12 disk ready: $(FAT_IMG)"

run: $(ISO_FILE) $(FAT_IMG)
	@echo "Booting OS in QEMU..."
	$(QEMU) \
	        -drive file=$(FAT_IMG),format=raw,if=ide,bus=0,unit=0,media=disk \
	        -drive file=$(ISO_FILE),format=raw,if=ide,bus=1,unit=0,media=cdrom \
	        -netdev user,id=net0 \
	        -device e1000,netdev=net0 \
	        -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
	        -boot d \
	        -accel tcg \
	        -display cocoa,zoom-to-fit=on

debug: $(ISO_FILE) $(FAT_IMG)
	@echo "Booting OS in QEMU (debug mode)..."
	$(QEMU) \
	        -drive file=$(FAT_IMG),format=raw,if=ide,bus=0,unit=0,media=disk \
	        -drive file=$(ISO_FILE),format=raw,if=ide,bus=1,unit=0,media=cdrom \
	        -netdev user,id=net0 \
	        -device e1000,netdev=net0 \
	        -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
	        -boot d \
	        -serial stdio \
	        -accel tcg \
	        -display cocoa,zoom-to-fit=on

clean:
	rm -f *.o *.bin *.iso disk.img $(FAT_IMG)
	$(MAKE) -C userprogs/c clean
	rm -rf $(ISO_DIR)
$(PCI_O): src/pci.c include/pci.h include/io.h include/printf.h
	@echo "Compiling PCI bus driver..."
	$(CC) $(CFLAGS) src/pci.c -o $(PCI_O)

$(E1000_O): src/e1000.c include/e1000.h include/pci.h include/vmm.h include/pmm.h include/string.h include/printf.h
	@echo "Compiling e1000 network driver..."
	$(CC) $(CFLAGS) src/e1000.c -o $(E1000_O)

$(CONSOLE_O): src/console.c include/console.h include/vga.h include/console_backend.h include/fbcon.h include/bochs_vbe.h
	@echo "Compiling console abstraction..."
	$(CC) $(CFLAGS) src/console.c -o $(CONSOLE_O)

$(FONT_O): src/font8x16.c include/font8x16.h
	@echo "Compiling embedded 8x16 font..."
	$(CC) $(CFLAGS) src/font8x16.c -o $(FONT_O)

$(BVBE_O): src/bochs_vbe.c include/bochs_vbe.h include/io.h include/pci.h include/vmm.h include/printf.h
	@echo "Compiling Bochs VBE driver..."
	$(CC) $(CFLAGS) src/bochs_vbe.c -o $(BVBE_O)

$(FBCON_O): src/fbcon.c include/fbcon.h include/console_backend.h include/bochs_vbe.h include/font8x16.h include/string.h
	@echo "Compiling framebuffer console..."
	$(CC) $(CFLAGS) src/fbcon.c -o $(FBCON_O)

$(BANNER_O): src/boot_banner.c include/boot_banner.h include/console.h include/printf.h
	@echo "Compiling boot banner..."
	$(CC) $(CFLAGS) src/boot_banner.c -o $(BANNER_O)
$(ARP_O): src/arp.c include/arp.h include/e1000.h include/string.h
	@echo "Compiling ARP..."
	$(CC) $(CFLAGS) src/arp.c -o $(ARP_O)

$(NET_O): src/net.c include/net.h include/net_internal.h include/arp.h include/e1000.h include/string.h include/pit.h include/printf.h
	@echo "Compiling IP/ICMP..."
	$(CC) $(CFLAGS) src/net.c -o $(NET_O)

$(TCP_O): src/tcp.c include/tcp.h include/net.h include/net_internal.h include/e1000.h include/string.h include/pit.h include/printf.h
	@echo "Compiling TCP..."
	$(CC) $(CFLAGS) src/tcp.c -o $(TCP_O)