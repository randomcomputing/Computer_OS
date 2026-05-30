#ifndef VMM_H
#define VMM_H

#include "stdint.h"

/* Forward declaration so vmm_page_fault can reference struct registers
   without a circular include with isr.h. */
struct registers;

#define VMM_PRESENT  (1 << 0)
#define VMM_WRITE    (1 << 1)
#define VMM_USER     (1 << 2)
#define VMM_NX       (1ULL << 63)

#define PAGE_SIZE    4096

void     vmm_init(uint64_t hhdm_offset);
int      vmm_map(uint64_t virt, uint64_t phys, uint64_t flags);
void     vmm_unmap(uint64_t virt);
uint64_t vmm_resolve(uint64_t virt);

uint64_t vmm_new_user_pd(void);
void     vmm_free_user_pd(uint64_t pml4_phys);
int      vmm_map_in(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags);
void     vmm_switch(uint64_t pml4_phys);
uint64_t vmm_current(void);

int      vmm_page_fault(struct registers* regs);

uint64_t vmm_phys_to_virt(uint64_t phys);
uint64_t vmm_virt_to_phys(uint64_t virt);

#endif