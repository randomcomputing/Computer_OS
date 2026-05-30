#ifndef PMM_H
#define PMM_H

#include <stdint.h>

#define PAGE_SIZE 4096

void     pmm_init(void);
uint64_t pmm_alloc(void);        /* returns physical address or 0 on failure */
void     pmm_free(uint64_t phys);
uint64_t pmm_free_pages(void);
uint64_t pmm_total_pages(void);

#endif