#ifndef PMM_H
#define PMM_H

#define PAGE_SIZE 4096

// Initialize the physical memory manager. Must be called after memmap is
// readable (i.e. after the kernel is running — the bootloader already
// populated the E820 data).
void pmm_init(void);

// Allocate a single 4 KB physical page. Returns the physical address of
// the page, or 0 if out of memory. The returned page's contents are
// undefined — zero it yourself if you need that.
unsigned int pmm_alloc(void);

// Mark the page starting at `addr` as free. `addr` must be page-aligned
// and must have been returned by pmm_alloc(). Freeing an already-free
// page is a bug but we don't detect it.
void pmm_free(unsigned int addr);

// Stats.
unsigned int pmm_free_pages(void);
unsigned int pmm_total_pages(void);

// Debug: print a summary of PMM state.
void pmm_print(void);

#endif