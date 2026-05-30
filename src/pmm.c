#include "pmm.h"
#include "memmap.h"
#include "printf.h"

static uint64_t g_hhdm_offset = 0;
static uint8_t* g_bitmap      = 0;
static uint64_t g_total_pages = 0;
static uint64_t g_free_pages  = 0;
static uint64_t g_next_scan   = 0;

void pmm_set_hhdm_offset(uint64_t offset) {
    g_hhdm_offset = offset;
}

static void bit_set(uint64_t i)   { g_bitmap[i / 8] |=  (1 << (i & 7)); }
static void bit_clear(uint64_t i) { g_bitmap[i / 8] &= ~(1 << (i & 7)); }
static int  bit_test(uint64_t i)  { return (g_bitmap[i / 8] >> (i & 7)) & 1; }

static void mark_used(uint64_t base, uint64_t size) {
    uint64_t start = base / PAGE_SIZE;
    uint64_t end   = (base + size + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint64_t i = start; i < end && i < g_total_pages; i++) {
        if (!bit_test(i)) { bit_set(i); if (g_free_pages) g_free_pages--; }
    }
}

static void mark_free_page(uint64_t i) {
    if (i >= g_total_pages) return;
    if (bit_test(i)) { bit_clear(i); g_free_pages++; }
}

void pmm_init(void) {
    int n = memmap_count();

    /* Find the highest physical address that is actually reachable
       (skip regions above 4 GB on first pass to size the bitmap
       conservatively, then we'll handle > 4 GB separately if needed).
       For now cap at the highest address below 4 GB. */
    uint64_t top = 0;
    for (int i = 0; i < n; i++) {
        const memmap_entry_t* e = memmap_get(i);
        /* Only count regions that start below 4 GB. */
        if (e->base >= 0x100000000ULL) continue;
        uint64_t end = e->base + e->length;
        if (end > 0x100000000ULL) end = 0x100000000ULL;
        if (end > top) top = end;
    }

    if (top == 0) top = 0x10000000ULL; /* fallback: 256 MB */

    g_total_pages = top / PAGE_SIZE;
    uint64_t bitmap_bytes = (g_total_pages + 7) / 8;

    /* Find first USABLE region above 1 MB large enough for the bitmap. */
    uint64_t bitmap_phys = 0;
    int found = 0;
    for (int i = 0; i < n; i++) {
        const memmap_entry_t* e = memmap_get(i);
        if (e->type != MEMMAP_TYPE_USABLE) continue;
        uint64_t base = e->base < 0x100000 ? 0x100000 : e->base;
        if (base >= e->base + e->length) continue;
        uint64_t avail = (e->base + e->length) - base;
        if (avail >= bitmap_bytes) {
            bitmap_phys = base;
            found = 1;
            break;
        }
    }

    if (!found) {
        printf("[pmm] FATAL: no region for bitmap\n");
        for (;;) __asm__ volatile ("hlt");
    }

    g_bitmap = (uint8_t*)(bitmap_phys + g_hhdm_offset);

    /* Mark everything used first. */
    for (uint64_t i = 0; i < bitmap_bytes; i++) g_bitmap[i] = 0xFF;
    g_free_pages = 0;

    /* Free all USABLE pages below 4 GB. */
    for (int i = 0; i < n; i++) {
        const memmap_entry_t* e = memmap_get(i);
        if (e->type != MEMMAP_TYPE_USABLE) continue;
        if (e->base >= 0x100000000ULL) continue;
        uint64_t start = (e->base + PAGE_SIZE - 1) / PAGE_SIZE;
        uint64_t end   = (e->base + e->length) / PAGE_SIZE;
        if (end > g_total_pages) end = g_total_pages;
        for (uint64_t p = start; p < end; p++) mark_free_page(p);
    }

    /* Also reclaim bootloader pages (safe after Limine hands off). */
    for (int i = 0; i < n; i++) {
        const memmap_entry_t* e = memmap_get(i);
        if (e->type != MEMMAP_TYPE_BOOTLOADER_RECLAIM) continue;
        if (e->base >= 0x100000000ULL) continue;
        uint64_t start = (e->base + PAGE_SIZE - 1) / PAGE_SIZE;
        uint64_t end   = (e->base + e->length) / PAGE_SIZE;
        if (end > g_total_pages) end = g_total_pages;
        for (uint64_t p = start; p < end; p++) mark_free_page(p);
    }

    /* Reserve the bitmap itself and page 0. */
    mark_used(bitmap_phys, bitmap_bytes);
    mark_used(0, PAGE_SIZE);
}

uint64_t pmm_alloc(void) {
    for (uint64_t i = g_next_scan; i < g_total_pages; i++) {
        if (!bit_test(i)) {
            bit_set(i); g_free_pages--;
            g_next_scan = i + 1;
            return i * PAGE_SIZE;
        }
    }
    for (uint64_t i = 1; i < g_next_scan; i++) {
        if (!bit_test(i)) {
            bit_set(i); g_free_pages--;
            g_next_scan = i + 1;
            return i * PAGE_SIZE;
        }
    }
    return 0;
}

void pmm_free(uint64_t phys) {
    uint64_t i = phys / PAGE_SIZE;
    if (i >= g_total_pages) return;
    if (!bit_test(i)) return;
    bit_clear(i); g_free_pages++;
    if (i < g_next_scan) g_next_scan = i;
}

uint64_t pmm_free_pages(void)  { return g_free_pages; }
uint64_t pmm_total_pages(void) { return g_total_pages; }