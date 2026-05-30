#include "memmap.h"
#include "printf.h"
#include <stddef.h>

/*
 * Limine memmap response layout (from the Limine spec).
 * We reproduce the relevant structs here so we don't need
 * the full Limine header in every file.
 */
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

#define MAX_ENTRIES 128

static memmap_entry_t  g_entries[MAX_ENTRIES];
static int             g_count = 0;

void memmap_init(void* resp_ptr) {
    struct limine_memmap_response* resp =
        (struct limine_memmap_response*)resp_ptr;

    if (!resp) return;

    uint64_t n = resp->entry_count;
    if (n > MAX_ENTRIES) n = MAX_ENTRIES;

    for (uint64_t i = 0; i < n; i++) {
        struct limine_memmap_entry* e = resp->entries[i];
        g_entries[g_count].base   = e->base;
        g_entries[g_count].length = e->length;
        g_entries[g_count].type   = (uint32_t)e->type;
        g_entries[g_count].pad    = 0;
        g_count++;
    }
}

int memmap_count(void) { return g_count; }

const memmap_entry_t* memmap_get(int i) {
    if (i < 0 || i >= g_count) return 0;
    return &g_entries[i];
}

uint64_t memmap_total_usable(void) {
    uint64_t total = 0;
    for (int i = 0; i < g_count; i++) {
        if (g_entries[i].type == MEMMAP_TYPE_USABLE)
            total += g_entries[i].length;
    }
    return total;
}

static const char* type_name(uint32_t t) {
    switch (t) {
        case MEMMAP_TYPE_USABLE:               return "USABLE";
        case MEMMAP_TYPE_RESERVED:             return "RESERVED";
        case MEMMAP_TYPE_ACPI_RECLAIM:         return "ACPI_RECLAIM";
        case MEMMAP_TYPE_ACPI_NVS:             return "ACPI_NVS";
        case MEMMAP_TYPE_BAD:                  return "BAD";
        case MEMMAP_TYPE_BOOTLOADER_RECLAIM:   return "BOOTLOADER_RECLAIM";
        case MEMMAP_TYPE_KERNEL_AND_MODULES:   return "KERNEL_AND_MODULES";
        default:                               return "UNKNOWN";
    }
}

void memmap_print(void) {
    printf("Memory map (%d entries):\n", g_count);
    for (int i = 0; i < g_count; i++) {
        printf("  [%d] base=0x%llx len=0x%llx type=%s\n",
               i, g_entries[i].base, g_entries[i].length,
               type_name(g_entries[i].type));
    }
    uint64_t mb = memmap_total_usable() / (1024 * 1024);
    printf("Total usable: %llu MB\n", mb);
}