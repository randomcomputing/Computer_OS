#include "pmm.h"
#include "memmap.h"
#include "printf.h"

// Linker symbols.
//   kernel_end      - virtual address one past the last byte of the kernel.
//                     We use this as a pointer (writing the bitmap goes here).
//   kernel_phys_end - PHYSICAL address one past the last byte of the kernel
//                     in RAM. We use this to know how much physical memory
//                     to reserve for the kernel image. Needed because after
//                     higher-half relocation, kernel_end is a 0xC0...
//                     address and can't be used as a physical extent.
extern unsigned char kernel_end[];
extern unsigned char kernel_phys_end[];

// Physical load address of the kernel image (set by bootloader: see boot.asm
// which copies the image to 0x10000).
#define KERNEL_PHYS_BASE 0x10000u

// Bitmap: one bit per 4 KB frame. bit=1 means "in use".
static unsigned char* g_bitmap     = 0;
static unsigned int   g_total_pages = 0;
static unsigned int   g_free_pages  = 0;

// Where we can start scanning from. Advances over time as an optimization so
// we don't re-scan the first N pages every alloc — not strictly needed for
// correctness since pmm_free() will reset it if we free something earlier.
static unsigned int   g_next_scan  = 0;

static void bit_set(unsigned int i) {
    g_bitmap[i / 8] |=  (1 << (i & 7));
}

static void bit_clear(unsigned int i) {
    g_bitmap[i / 8] &= ~(1 << (i & 7));
}

static int bit_test(unsigned int i) {
    return (g_bitmap[i / 8] >> (i & 7)) & 1;
}

// Mark a physical address range as "in use". `base` and `size` are in
// bytes; we conservatively round `base` down and `(base+size)` up so any
// page the range touches becomes reserved.
static void mark_used_range(unsigned int base, unsigned int size) {
    unsigned int start = base / PAGE_SIZE;
    unsigned int end   = (base + size + PAGE_SIZE - 1) / PAGE_SIZE;
    for (unsigned int i = start; i < end && i < g_total_pages; i++) {
        if (!bit_test(i)) {
            bit_set(i);
            if (g_free_pages) g_free_pages--;
        }
    }
}

// Mark a page FREE. Used during init to open up usable E820 regions.
static void mark_free_page(unsigned int i) {
    if (i >= g_total_pages) return;
    if (bit_test(i)) {
        bit_clear(i);
        g_free_pages++;
    }
}

void pmm_init(void) {
    // Walk E820 to find the highest physical address we need to cover.
    // We only care about usable regions for sizing, but we also want to
    // know the top of *any* region so we don't hand out pages above RAM.
    unsigned int top = 0;
    int n = memmap_count();
    for (int i = 0; i < n; i++) {
        const memmap_entry_t* e = memmap_get(i);
        if (!e) continue;
        if (e->base_high || e->length_high) continue;   // skip > 4 GB
        unsigned int end = e->base_low + e->length_low;
        if (end > top) top = end;
    }

    // Cap at 4 GB - one page to keep math sane.
    if (top > 0xFFFFF000u) top = 0xFFFFF000u;

    g_total_pages = top / PAGE_SIZE;

    // Place the bitmap right after the kernel image in memory. Round up
    // to a page boundary so we don't share a page with kernel code/data.
    // We use the VIRTUAL kernel_end here because we actually dereference
    // this pointer to write bitmap bytes, and the high-half kernel mapping
    // covers this range.
    unsigned int bitmap_addr = ((unsigned int)kernel_end + PAGE_SIZE - 1)
                               & ~(PAGE_SIZE - 1);
    g_bitmap = (unsigned char*)bitmap_addr;

    unsigned int bitmap_bytes = (g_total_pages + 7) / 8;

    // Mark EVERYTHING as in use first. We'll selectively free USABLE
    // regions below.
    for (unsigned int i = 0; i < bitmap_bytes; i++) {
        g_bitmap[i] = 0xFF;
    }
    g_free_pages = 0;

    // Walk E820 again and free the usable regions.
    for (int i = 0; i < n; i++) {
        const memmap_entry_t* e = memmap_get(i);
        if (!e) continue;
        if (e->type != MEMMAP_TYPE_USABLE) continue;
        if (e->base_high) continue;

        unsigned int base = e->base_low;
        unsigned int len  = e->length_low;
        if (e->length_high) {
            // Clamp any region that extends past 4 GB.
            len = 0xFFFFFFFFu - base;
        }

        // Page-align inward: start at the first full page, end at the
        // last full page. Partial pages at the edges are not usable.
        unsigned int start = (base + PAGE_SIZE - 1) / PAGE_SIZE;
        unsigned int end   = (base + len) / PAGE_SIZE;
        for (unsigned int p = start; p < end && p < g_total_pages; p++) {
            mark_free_page(p);
        }
    }

    // Now re-reserve the things we just accidentally freed:

    // 1) The first page (0x0 - 0x1000). Null pointer traps, BIOS data,
    //    our memory map stash at 0x500. Always off-limits.
    mark_used_range(0x0, PAGE_SIZE);

    // 2) The bootloader area. The bootloader itself is at 0x7C00 and
    //    isn't needed anymore, but the BIOS may still reference low
    //    memory below 0x10000, so reserve 0x7000 - 0x10000 conservatively.
    mark_used_range(0x7000, 0x9000);

    // 3) The kernel image. Use kernel_phys_end (physical) because we're
    //    computing a physical extent starting at KERNEL_PHYS_BASE.
    unsigned int ke_phys = (unsigned int)kernel_phys_end;
    mark_used_range(KERNEL_PHYS_BASE, ke_phys - KERNEL_PHYS_BASE);

    // 4) The bitmap itself. g_bitmap is a high-half virtual address; the
    //    backing physical address is bitmap_addr - KERNEL_VMA (the bitmap
    //    sits in the high-half mapping, backed by physical memory right
    //    after the kernel image).
    unsigned int bitmap_phys = bitmap_addr - 0xC0000000u;
    mark_used_range(bitmap_phys, bitmap_bytes);

    g_next_scan = 0;
}

unsigned int pmm_alloc(void) {
    if (g_free_pages == 0) return 0;

    // Linear scan from g_next_scan. Simple and fine for now.
    for (unsigned int i = g_next_scan; i < g_total_pages; i++) {
        if (!bit_test(i)) {
            bit_set(i);
            g_free_pages--;
            g_next_scan = i + 1;
            return i * PAGE_SIZE;
        }
    }
    // Wrap around.
    for (unsigned int i = 0; i < g_next_scan; i++) {
        if (!bit_test(i)) {
            bit_set(i);
            g_free_pages--;
            g_next_scan = i + 1;
            return i * PAGE_SIZE;
        }
    }
    return 0;
}

void pmm_free(unsigned int addr) {
    unsigned int i = addr / PAGE_SIZE;
    if (i >= g_total_pages) return;
    if (!bit_test(i)) return;        // already free; ignore rather than corrupt stats
    bit_clear(i);
    g_free_pages++;
    if (i < g_next_scan) g_next_scan = i;
}

unsigned int pmm_free_pages(void) {
    return g_free_pages;
}

unsigned int pmm_total_pages(void) {
    return g_total_pages;
}

void pmm_print(void) {
    printf("PMM: %u free / %u total pages (%u KB free of %u KB)\n",
           g_free_pages, g_total_pages,
           g_free_pages * 4, g_total_pages * 4);
    printf("     bitmap at 0x%x, kernel_end at 0x%x (phys_end 0x%x)\n",
           (unsigned int)g_bitmap,
           (unsigned int)kernel_end,
           (unsigned int)kernel_phys_end);
}