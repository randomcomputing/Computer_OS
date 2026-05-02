#ifndef MEMMAP_H
#define MEMMAP_H

// E820 memory map, populated by the bootloader in real mode and read by
// the kernel after boot. See boot.asm for the stash addresses.

// Region types as defined by the ACPI spec / E820.
#define MEMMAP_TYPE_USABLE        1
#define MEMMAP_TYPE_RESERVED      2
#define MEMMAP_TYPE_ACPI_RECLAIM  3
#define MEMMAP_TYPE_ACPI_NVS      4
#define MEMMAP_TYPE_BAD           5

// A single E820 entry. Note the 64-bit fields — BIOSes can report memory
// above the 4 GB line on PAE/64-bit systems. We're a 32-bit kernel so
// anything beyond 4 GB is unusable to us, but we still parse the full values.
typedef struct {
    unsigned int base_low;
    unsigned int base_high;
    unsigned int length_low;
    unsigned int length_high;
    unsigned int type;
    unsigned int acpi_attrs;   // ACPI 3.0 extended attribute field
} __attribute__((packed)) memmap_entry_t;

// Total number of entries the bootloader discovered. Zero means the BIOS
// didn't support E820 (very unlikely on modern hardware / QEMU).
int memmap_count(void);

// Returns a pointer to entry `i`, or 0 if out of range.
const memmap_entry_t* memmap_get(int i);

// Total usable RAM (type 1 regions only), in bytes, capped at the 4 GB line.
unsigned int memmap_total_usable(void);

// Print the full map to the console via printf. Used by the `meminfo` shell
// command.
void memmap_print(void);

#endif