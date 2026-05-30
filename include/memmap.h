#ifndef MEMMAP_H
#define MEMMAP_H

#include <stdint.h>

/* Memory region types — matches Limine's memmap entry types. */
#define MEMMAP_TYPE_USABLE          1
#define MEMMAP_TYPE_RESERVED        2
#define MEMMAP_TYPE_ACPI_RECLAIM    3
#define MEMMAP_TYPE_ACPI_NVS        4
#define MEMMAP_TYPE_BAD             5
#define MEMMAP_TYPE_BOOTLOADER_RECLAIM 0x1000
#define MEMMAP_TYPE_KERNEL_AND_MODULES 0x1001

typedef struct {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t pad;
} memmap_entry_t;

/* Called from kmain with the Limine memmap response. */
void                  memmap_init(void* limine_memmap_response);

int                   memmap_count(void);
const memmap_entry_t* memmap_get(int i);
uint64_t              memmap_total_usable(void);
void                  memmap_print(void);

#endif