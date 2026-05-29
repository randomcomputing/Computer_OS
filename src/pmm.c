#include "pmm.h"
#include "memmap.h"
#include "printf.h"

extern unsigned char kernel_end[];
extern unsigned char kernel_phys_end[];

#define KERNEL_PHYS_BASE 0x00010000u
#define KERNEL_VMA       0xC0000000u
#define LOW_RESERVED_END 0x00100000u

static unsigned char* g_bitmap      = 0;
static unsigned int   g_total_pages = 0;
static unsigned int   g_free_pages  = 0;
static unsigned int   g_next_scan   = 0;

static void bit_set(unsigned int i) {
    g_bitmap[i / 8] |= (1u << (i & 7));
}

static void bit_clear(unsigned int i) {
    g_bitmap[i / 8] &= ~(1u << (i & 7));
}

static int bit_test(unsigned int i) {
    return (g_bitmap[i / 8] >> (i & 7)) & 1u;
}

static void mark_used_range(unsigned int base, unsigned int size) {
    if (size == 0) {
        return;
    }

    unsigned int start = base / PAGE_SIZE;
    unsigned int end   = (base + size + PAGE_SIZE - 1u) / PAGE_SIZE;

    if (end > g_total_pages) {
        end = g_total_pages;
    }

    for (unsigned int i = start; i < end; i++) {
        if (!bit_test(i)) {
            bit_set(i);

            if (g_free_pages) {
                g_free_pages--;
            }
        }
    }
}

static void mark_free_page(unsigned int i) {
    if (i >= g_total_pages) {
        return;
    }

    if (bit_test(i)) {
        bit_clear(i);
        g_free_pages++;
    }
}

void pmm_init(void) {
    unsigned int top = 0;
    int n = memmap_count();

    for (int i = 0; i < n; i++) {
        const memmap_entry_t* e = memmap_get(i);

        if (!e) {
            continue;
        }

        if (e->base_high || e->length_high) {
            continue;
        }

        unsigned int end = e->base_low + e->length_low;

        if (end > top) {
            top = end;
        }
    }

    if (top > 0xFFFFF000u) {
        top = 0xFFFFF000u;
    }

    g_total_pages = top / PAGE_SIZE;

    unsigned int bitmap_addr =
        ((unsigned int)kernel_end + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u);

    g_bitmap = (unsigned char*)bitmap_addr;

    unsigned int bitmap_bytes = (g_total_pages + 7u) / 8u;

    for (unsigned int i = 0; i < bitmap_bytes; i++) {
        g_bitmap[i] = 0xFFu;
    }

    g_free_pages = 0;

    /*
     * Free usable E820 pages.
     */
    for (int i = 0; i < n; i++) {
        const memmap_entry_t* e = memmap_get(i);

        if (!e) {
            continue;
        }

        if (e->type != MEMMAP_TYPE_USABLE) {
            continue;
        }

        if (e->base_high) {
            continue;
        }

        unsigned int base = e->base_low;
        unsigned int len  = e->length_low;

        if (e->length_high) {
            len = 0xFFFFFFFFu - base;
        }

        unsigned int start = (base + PAGE_SIZE - 1u) / PAGE_SIZE;
        unsigned int end   = (base + len) / PAGE_SIZE;

        if (end > g_total_pages) {
            end = g_total_pages;
        }

        for (unsigned int p = start; p < end; p++) {
            mark_free_page(p);
        }
    }

    /*
     * Never allocate low memory.
     *
     * This avoids BIOS data, IVT, BDA, EBDA, VGA holes, bootloader data,
     * and the confusing physical address 0 return value.
     */
    mark_used_range(0x00000000u, LOW_RESERVED_END);

    /*
     * Reserve kernel physical image.
     */
    unsigned int ke_phys = (unsigned int)kernel_phys_end;

    if (ke_phys > KERNEL_PHYS_BASE) {
        mark_used_range(KERNEL_PHYS_BASE, ke_phys - KERNEL_PHYS_BASE);
    }

    /*
     * Reserve PMM bitmap.
     */
    unsigned int bitmap_phys = bitmap_addr - KERNEL_VMA;
    mark_used_range(bitmap_phys, bitmap_bytes);

    g_next_scan = LOW_RESERVED_END / PAGE_SIZE;
}

unsigned int pmm_alloc(void) {
    if (g_free_pages == 0) {
        return 0;
    }

    unsigned int min_i = LOW_RESERVED_END / PAGE_SIZE;

    if (g_next_scan < min_i) {
        g_next_scan = min_i;
    }

    for (unsigned int i = g_next_scan; i < g_total_pages; i++) {
        if (!bit_test(i)) {
            bit_set(i);
            g_free_pages--;
            g_next_scan = i + 1u;
            return i * PAGE_SIZE;
        }
    }

    for (unsigned int i = min_i; i < g_next_scan && i < g_total_pages; i++) {
        if (!bit_test(i)) {
            bit_set(i);
            g_free_pages--;
            g_next_scan = i + 1u;
            return i * PAGE_SIZE;
        }
    }

    return 0;
}

unsigned int pmm_alloc_below(unsigned int max_phys) {
    if (g_free_pages == 0) {
        return 0;
    }

    unsigned int start_i = LOW_RESERVED_END / PAGE_SIZE;
    unsigned int end_i   = max_phys / PAGE_SIZE;

    if (end_i > g_total_pages) {
        end_i = g_total_pages;
    }

    if (end_i <= start_i) {
        return 0;
    }

    for (unsigned int i = start_i; i < end_i; i++) {
        if (!bit_test(i)) {
            bit_set(i);
            g_free_pages--;

            if (g_next_scan <= i) {
                g_next_scan = i + 1u;
            }

            return i * PAGE_SIZE;
        }
    }

    return 0;
}

void pmm_free(unsigned int addr) {
    if (addr < LOW_RESERVED_END) {
        return;
    }

    unsigned int i = addr / PAGE_SIZE;

    if (i >= g_total_pages) {
        return;
    }

    if (!bit_test(i)) {
        return;
    }

    bit_clear(i);
    g_free_pages++;

    if (i < g_next_scan) {
        g_next_scan = i;
    }
}

unsigned int pmm_free_pages(void) {
    return g_free_pages;
}

unsigned int pmm_total_pages(void) {
    return g_total_pages;
}

void pmm_print(void) {
    printf("PMM: %u free / %u total pages (%u KB free of %u KB)\n",
           g_free_pages,
           g_total_pages,
           g_free_pages * 4u,
           g_total_pages * 4u);

    printf("     bitmap virt=0x%x kernel_end=0x%x kernel_phys_end=0x%x\n",
           (unsigned int)g_bitmap,
           (unsigned int)kernel_end,
           (unsigned int)kernel_phys_end);
}