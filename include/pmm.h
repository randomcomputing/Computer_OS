#ifndef PMM_H
#define PMM_H

#define PAGE_SIZE 4096

void pmm_init(void);

/*
 * Allocate one 4 KB physical page.
 * Returns physical address, or 0 on failure.
 *
 * This allocator never returns physical addresses below 1 MB.
 */
unsigned int pmm_alloc(void);

/*
 * Allocate one 4 KB physical page below max_phys.
 * Returns physical address, or 0 on failure.
 *
 * This is mostly kept for compatibility. The VMM now has a static
 * boot page-table pool, so early VMM metadata does not depend on this.
 */
unsigned int pmm_alloc_below(unsigned int max_phys);

void pmm_free(unsigned int addr);

unsigned int pmm_free_pages(void);
unsigned int pmm_total_pages(void);

void pmm_print(void);

#endif