#include "vga.h"
#include "printf.h"
#include "idt.h"
#include "pic.h"
#include "keyboard.h"
#include "mouse.h"
#include "pit.h"
#include "shell.h"
#include "serial.h"
#include "memmap.h"
#include "pmm.h"
#include "vmm.h"
#include "kheap.h"
#include "task.h"
#include "ata.h"
#include "fat12.h"
#include "vfs.h"
#include "ramfs.h"
#include "gdt.h"
#include "syscall.h"
#include "pci.h"

void halt(void);

void kmain(void) {
    vga_init();
    serial_init();
    serial_write("Serial[OK]\n");


    
    vga_set_color(VGA_WHITE, VGA_BLACK);
    printf("###############################\n");
    printf("###");
    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    printf("       Computer OS       ");
    vga_set_color(VGA_WHITE, VGA_BLACK);
    printf("###\n");
    printf("###############################\n");
    printf("\n");
    vga_set_color(VGA_WHITE, VGA_BLACK);
    printf("[OK] Bootloader  [OK] Protected Mode  [OK] Kernel Running\n");
    printf("[OK] Higher-half kernel at 0xC0000000\n");

    // Replace the bootloader's GDT with our own (kcode/kdata + ucode/udata
    // + TSS) before we touch the IDT — the syscall gate we install later
    // points at a stub that loads 0x10 into ds, which is meaningful only
    // if 0x10 is the kernel data selector in the *currently loaded* GDT.
    gdt_init();
    printf("[OK] GDT loaded (ring-0 + ring-3 + TSS)\n");

    idt_init();
    printf("[OK] IDT loaded\n");

    pic_remap();
    printf("[OK] PIC remapped\n");

    keyboard_init();
    printf("[OK] Keyboard ready\n");

    mouse_init();
    printf("[OK] Mouse ready (scroll = wheel / two-finger)\n");

    pit_init(100);   // 100 Hz = one tick every 10 ms
    printf("[OK] Timer running at 100 Hz\n");

    printf("[OK] Memory map: %d regions, %u MB usable\n",
           memmap_count(), memmap_total_usable() / (1024 * 1024));

    pmm_init();
    printf("[OK] PMM: %u pages free (%u KB)\n",
           pmm_free_pages(), pmm_free_pages() * 4);

    vmm_init(0);     // adopt the bootstrap page directory from entry.asm
    printf("[OK] VMM online (bootstrap PD adopted, PSE demoted)\n");

    // Heap lives in the high half well above the kernel image. 0xC1000000
    // is 16 MB into the high half — kernel image ends around 0xC001_5000,
    // so plenty of headroom.
    kheap_init(0xC1000000, 64, 256);     // 256 KB initial, 1 MB max
    printf("[OK] Kernel heap ready (%u KB)\n", kheap_free() / 1024);

    tasking_init();
    printf("[OK] Tasking online (kmain = task 0)\n");

    syscall_init();
    printf("[OK] Syscall gate (int 0x80) installed\n");

    // Enumerate the PCI bus. This just discovers what hardware is plugged in
    // (storage controllers, the NIC we'll drive later); it doesn't program
    // anything yet. `lspci` at the shell lists what this found.
    {
        int n = pci_init();
        printf("[OK] PCI scan: %d device%s found\n", n, n == 1 ? "" : "s");
    }

    // Storage stack: ATA driver first, then mount FAT12 on top of it.
    // Both are best-effort: if there's no -hda image attached or it's
    // not a FAT12 disk, we just print a warning and the shell still
    // boots — `ls` and `cat` will report no filesystem.
    if (ata_init()) {
        printf("[OK] ATA primary master detected\n");
        if (fat12_mount() == 0) {
            // Bring up the VFS and mount FAT12 as the root filesystem.
            // From here on, the rest of the kernel talks to vfs_* instead
            // of fat12_* directly.
            vfs_init();
            if (vfs_mount("/", fat12_vfs_ops(), "fat12") == 0) {
                printf("[OK] FAT12 mounted at / via VFS\n");
            } else {
                printf("[..] VFS mount table full\n");
            }
            // Mount an in-memory filesystem at /tmp to demonstrate the VFS
            // hosting two filesystems at once. Contents vanish on reboot.
            ramfs_init();
            if (vfs_mount("/tmp", ramfs_vfs_ops(), "ramfs") == 0) {
                printf("[OK] ramfs mounted at /tmp\n");
            }
        } else {
            printf("[..] No FAT12 filesystem found\n");
        }
    } else {
        printf("[..] No ATA disk attached (boot with -hda <image>)\n");
    }
    printf("\n");

    __asm__ volatile ("sti");

    shell_run();

    halt();
}