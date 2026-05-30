/*
 * kernel.c — 64-bit kernel main entry point (Limine/UEFI).
 */

#include "stdint.h"
#include "stddef.h"

#include "console.h"
#include "printf.h"
#include "gdt.h"
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
#include "fat32.h"
#include "vfs.h"
#include "ramfs.h"
#include "syscall.h"
#include "pci.h"
#include "boot_banner.h"
#include "e1000.h"
#include "acpi.h"
#include "bochs_vbe.h"

void halt(void);

/* ------------------------------------------------------------------ */
/* Limine protocol structures                                          */
/* ------------------------------------------------------------------ */

#define LIMINE_COMMON_MAGIC  0xc7b1dd30df4c8b88ULL, 0x0a82e883a194f07bULL

struct limine_framebuffer {
    void*    address;
    uint64_t width;
    uint64_t height;
    uint64_t pitch;
    uint16_t bpp;
    uint8_t  memory_model;
    uint8_t  red_mask_size,   red_mask_shift;
    uint8_t  green_mask_size, green_mask_shift;
    uint8_t  blue_mask_size,  blue_mask_shift;
    uint8_t  unused[7];
    uint64_t edid_size;
    void*    edid;
    uint64_t mode_count;
    void**   modes;
};

struct limine_framebuffer_response {
    uint64_t revision;
    uint64_t framebuffer_count;
    struct limine_framebuffer** framebuffers;
};

struct limine_framebuffer_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_framebuffer_response* response;
};

struct limine_memmap_entry {
    uint64_t base;
    uint64_t length;
    uint64_t type;
};

struct limine_memmap_response {
    uint64_t revision;
    uint64_t entry_count;
    struct limine_memmap_entry** entries;
};

struct limine_memmap_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_memmap_response* response;
};

struct limine_hhdm_response {
    uint64_t revision;
    uint64_t offset;
};

struct limine_hhdm_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_hhdm_response* response;
};

/* ------------------------------------------------------------------ */
/* Limine requests                                                     */
/* ------------------------------------------------------------------ */

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request fb_request = {
    .id       = { LIMINE_COMMON_MAGIC,
                  0x9d5827dcd881dd75ULL, 0xa3148604f6fab11bULL },
    .revision = 0,
    .response = NULL,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request mm_request = {
    .id       = { LIMINE_COMMON_MAGIC,
                  0x67cf3d9d378a806fULL, 0xe304acdfc50c3c62ULL },
    .revision = 0,
    .response = NULL,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id       = { LIMINE_COMMON_MAGIC,
                  0x48dcf1cb8ad2b852ULL, 0x63984e959a98244bULL },
    .revision = 0,
    .response = NULL,
};

/* ------------------------------------------------------------------ */
/* Colored status helpers                                              */
/* ------------------------------------------------------------------ */

static void print_status_u64(const char* text, uint64_t value, const char* suffix) {
    con_set_color(CON_LIGHT_GREEN, CON_BLACK);
    printf("[OK]");
    con_set_color(CON_WHITE, CON_BLACK);

    if (suffix) {
        printf(" %s%llu%s\n", text, value, suffix);
    } else {
        printf(" %s%llu\n", text, value);
    }
}

static void print_status_hex64(const char* text, uint64_t value) {
    con_set_color(CON_LIGHT_GREEN, CON_BLACK);
    printf("[OK]");
    con_set_color(CON_WHITE, CON_BLACK);
    printf(" %s0x%llx\n", text, value);
}

/* ------------------------------------------------------------------ */
/* kmain                                                               */
/* ------------------------------------------------------------------ */

void kmain(void) {
    serial_init();
    serial_write("Serial [OK]\n");

    uint64_t hhdm_offset = hhdm_request.response
                         ? hhdm_request.response->offset
                         : 0xFFFF800000000000ULL;

    /* Memory subsystem. */
    memmap_init(mm_request.response);

    extern void pmm_set_hhdm_offset(uint64_t);
    pmm_set_hhdm_offset(hhdm_offset);
    pmm_init();

    vmm_init(hhdm_offset);

    kheap_init(0xFFFFFFFF40000000ULL, 64, 1024);

    /* Console: try the Limine framebuffer, fall back to VGA text. */
    con_init();
    if (fb_request.response && fb_request.response->framebuffer_count > 0) {
        struct limine_framebuffer* fb = fb_request.response->framebuffers[0];

        bochs_vbe_set_from_limine(fb->address,
                                  (unsigned int)fb->width,
                                  (unsigned int)fb->height,
                                  (unsigned int)fb->pitch,
                                  (unsigned char)fb->bpp);

        if (con_use_framebuffer()) {
            serial_write("[fb] framebuffer console active\n");
        } else {
            serial_write("[fb] con_use_framebuffer failed\n");
        }
    } else {
        serial_write("[fb] no Limine framebuffer response\n");
    }

    boot_banner();

    print_status(1, "Limine UEFI boot");
    print_status(1, "Long mode");
    print_status(1, "Kernel running");
    print_status(1, "Higher-half kernel");
    print_status_hex64("HHDM offset: ", hhdm_offset);

    con_set_color(CON_LIGHT_GREEN, CON_BLACK);
    printf("[OK]");
    con_set_color(CON_WHITE, CON_BLACK);
    printf(" Memory map: %d regions, %llu MB usable\n",
           memmap_count(), memmap_total_usable() / (1024 * 1024));

    print_status_u64("PMM: ", pmm_free_pages(), " pages free");
    print_status(1, "VMM online 4-level paging");
    print_status(1, "Kernel heap ready");

    gdt_init();
    print_status(1, "GDT loaded");

    idt_init();
    print_status(1, "IDT loaded");

    pic_remap();
    print_status(1, "PIC remapped");

    keyboard_init();
    print_status(1, "Keyboard ready");

    mouse_init();
    print_status(1, "Mouse ready");

    pit_init(100);
    print_status(1, "Timer at 100 Hz");

    acpi_init();

    tasking_init();
    print_status(1, "Tasking online");

    syscall_init();
    print_status(1, "Syscall gate installed");

    {
        int n = pci_init();

        con_set_color(CON_LIGHT_GREEN, CON_BLACK);
        printf("[OK]");
        con_set_color(CON_WHITE, CON_BLACK);
        printf(" PCI: %d device%s\n", n, n == 1 ? "" : "s");

        if (e1000_init()) {
            print_status(1, "e1000 NIC initialized");
        } else {
            print_status(0, "e1000 NIC not available");
        }
    }

    int g_fs_ok = 0;
    if (ata_init()) {
        print_status(1, "ATA primary master detected");

        vfs_init();

        // Try FAT32 first; fall back to FAT12 for the 1.44 MB floppy image.
        if (fat32_mount() == 0) {
            if (vfs_mount("/", fat32_vfs_ops(), "fat32") == 0) {
                print_status(1, "FAT32 mounted at /");
                g_fs_ok = 1;
            } else {
                print_status(0, "FAT32 mount failed");
            }
        } else if (fat12_mount() == 0) {
            if (vfs_mount("/", fat12_vfs_ops(), "fat12") == 0) {
                print_status(1, "FAT12 mounted at /");
                g_fs_ok = 1;
            } else {
                print_status(0, "FAT12 mount failed");
            }
        } else {
            print_status(0, "No FAT filesystem");
        }

        ramfs_init();
        if (vfs_mount("/tmp", ramfs_vfs_ops(), "ramfs") == 0) {
            print_status(1, "ramfs mounted at /tmp");
        } else {
            print_status(0, "ramfs mount failed");
        }
    } else {
        print_status(0, "No ATA disk");
    }
    (void)g_fs_ok;

    printf("\n");
    __asm__ volatile ("sti");
    shell_run();
    halt();
}