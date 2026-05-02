#include "memmap.h"
#include "printf.h"

// These addresses must match MEMMAP_COUNT / MEMMAP_ENTRIES in boot.asm.
// We put them below the bootloader (0x7C00) because the protected-mode
// kernel copy at 0x7E00 -> 0x10000 (32 KB) would trample anything we
// stashed above the bootloader.
#define MEMMAP_COUNT_ADDR    0x500
#define MEMMAP_ENTRIES_ADDR  0x504

static const int*            g_count_ptr   = (const int*)MEMMAP_COUNT_ADDR;
static const memmap_entry_t* g_entries_ptr = (const memmap_entry_t*)MEMMAP_ENTRIES_ADDR;

int memmap_count(void) {
    int n = *g_count_ptr;
    // Sanity: if something went wrong and we got a huge bogus count,
    // clamp so a broken map can't crash the kernel.
    if (n < 0 || n > 128) return 0;
    return n;
}

const memmap_entry_t* memmap_get(int i) {
    if (i < 0 || i >= memmap_count()) return 0;
    return &g_entries_ptr[i];
}

unsigned int memmap_total_usable(void) {
    unsigned int total = 0;
    int n = memmap_count();
    for (int i = 0; i < n; i++) {
        const memmap_entry_t* e = &g_entries_ptr[i];
        if (e->type != MEMMAP_TYPE_USABLE) continue;

        // Anything starting at or above 4 GB is unusable to a 32-bit kernel.
        if (e->base_high != 0) continue;

        // Cap length so we don't overflow adding it in.
        unsigned int len = e->length_low;
        if (e->length_high != 0) {
            // Region extends past 4 GB; clamp to 4 GB - base.
            len = 0xFFFFFFFFu - e->base_low;
        }
        // Clamp again in case base + len would wrap.
        if (len > 0xFFFFFFFFu - e->base_low) {
            len = 0xFFFFFFFFu - e->base_low;
        }
        total += len;
    }
    return total;
}

static const char* type_name(unsigned int type) {
    switch (type) {
        case MEMMAP_TYPE_USABLE:       return "USABLE";
        case MEMMAP_TYPE_RESERVED:     return "RESERVED";
        case MEMMAP_TYPE_ACPI_RECLAIM: return "ACPI_RECLAIM";
        case MEMMAP_TYPE_ACPI_NVS:     return "ACPI_NVS";
        case MEMMAP_TYPE_BAD:          return "BAD";
        default:                       return "UNKNOWN";
    }
}

void memmap_print(void) {
    int n = memmap_count();
    if (n == 0) {
        printf("No memory map available (E820 unsupported).\n");
        return;
    }

    printf("E820 memory map (%d entries):\n", n);
    for (int i = 0; i < n; i++) {
        const memmap_entry_t* e = &g_entries_ptr[i];
        // We print the low 32 bits of base and length. Any high bits being
        // nonzero mean the region touches territory a 32-bit kernel can't
        // address; we flag those.
        printf("  [%d] base=0x%x len=0x%x type=%s",
               i, e->base_low, e->length_low, type_name(e->type));
        if (e->base_high || e->length_high) {
            printf(" (64-bit)");
        }
        printf("\n");
    }
    unsigned int kb = memmap_total_usable() / 1024;
    printf("Total usable: %u KB (%u MB)\n", kb, kb / 1024);
}