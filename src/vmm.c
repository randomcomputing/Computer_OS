#include "vmm.h"
#include "pmm.h"
#include "printf.h"
#include "isr.h"

/*
 * 64-bit VMM using 4-level paging.
 *
 * Limine has already set up paging for us. We adopt its PML4 as the kernel
 * page table and extend it with our own mappings. Each user process gets a
 * fresh PML4 that shares the kernel half (top 256 PML4 entries, covering the
 * upper 128 TB of virtual address space) via copied PML4 entries.
 *
 * Physical frames are accessed through the HHDM:
 *     virtual = physical + g_hhdm_offset
 *
 * This is simpler than the old low-4MB identity map trick.
 */

#define ENTRIES      512
#define PML4_KERNEL  256   /* first PML4 entry covering the upper half */

static uint64_t g_hhdm_offset   = 0;
static uint64_t g_kernel_pml4   = 0;   /* physical addr of kernel PML4 */

/* ---- helpers ---- */

static inline uint64_t* phys_to_virt_ptr(uint64_t phys) {
    return (uint64_t*)(phys + g_hhdm_offset);
}

static inline uint64_t pml4_index(uint64_t v) { return (v >> 39) & 0x1FF; }
static inline uint64_t pdpt_index(uint64_t v) { return (v >> 30) & 0x1FF; }
static inline uint64_t pd_index  (uint64_t v) { return (v >> 21) & 0x1FF; }
static inline uint64_t pt_index  (uint64_t v) { return (v >> 12) & 0x1FF; }

static inline void invlpg(uint64_t v) {
    __asm__ volatile ("invlpg (%0)" : : "r"(v) : "memory");
}

static inline uint64_t read_cr2(void) {
    uint64_t v;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(v));
    return v;
}

/* Allocate a zeroed page-table frame. */
static uint64_t alloc_pt_phys(void) {
    uint64_t phys = pmm_alloc();
    if (!phys) return 0;
    uint64_t* page = phys_to_virt_ptr(phys);
    for (int i = 0; i < ENTRIES; i++) page[i] = 0;
    return phys;
}

/* ---- walk helpers ---- */

/*
 * Return a pointer to the PTE for `virt` in the given PML4 (physical addr),
 * allocating intermediate tables if `alloc` is non-zero.
 * Returns NULL on failure.
 */
static uint64_t* walk_to_pte(uint64_t pml4_phys, uint64_t virt, int alloc) {
    uint64_t* pml4 = phys_to_virt_ptr(pml4_phys);

    /* PML4 -> PDPT */
    uint64_t pml4e = pml4[pml4_index(virt)];
    uint64_t pdpt_phys;
    if (!(pml4e & VMM_PRESENT)) {
        if (!alloc) return 0;
        pdpt_phys = alloc_pt_phys();
        if (!pdpt_phys) return 0;
        pml4[pml4_index(virt)] = pdpt_phys | VMM_PRESENT | VMM_WRITE |
                                  (virt < 0xFFFF800000000000ULL ? VMM_USER : 0);
    } else {
        pdpt_phys = pml4e & ~0xFFFULL;
    }

    /* PDPT -> PD */
    uint64_t* pdpt = phys_to_virt_ptr(pdpt_phys);
    uint64_t pdpte = pdpt[pdpt_index(virt)];
    uint64_t pd_phys;
    if (!(pdpte & VMM_PRESENT)) {
        if (!alloc) return 0;
        pd_phys = alloc_pt_phys();
        if (!pd_phys) return 0;
        pdpt[pdpt_index(virt)] = pd_phys | VMM_PRESENT | VMM_WRITE |
                                  (virt < 0xFFFF800000000000ULL ? VMM_USER : 0);
    } else {
        pd_phys = pdpte & ~0xFFFULL;
    }

    /* PD -> PT */
    uint64_t* pd = phys_to_virt_ptr(pd_phys);
    uint64_t pde = pd[pd_index(virt)];
    uint64_t pt_phys;
    if (!(pde & VMM_PRESENT)) {
        if (!alloc) return 0;
        pt_phys = alloc_pt_phys();
        if (!pt_phys) return 0;
        pd[pd_index(virt)] = pt_phys | VMM_PRESENT | VMM_WRITE |
                              (virt < 0xFFFF800000000000ULL ? VMM_USER : 0);
    } else {
        pt_phys = pde & ~0xFFFULL;
    }

    uint64_t* pt = phys_to_virt_ptr(pt_phys);
    return &pt[pt_index(virt)];
}

/* ---- public API ---- */

void vmm_init(uint64_t hhdm_offset) {
    g_hhdm_offset = hhdm_offset;

    /* Read Limine's PML4 from CR3. */
    __asm__ volatile ("mov %%cr3, %0" : "=r"(g_kernel_pml4));
    g_kernel_pml4 &= ~0xFFFULL;    /* strip flags */
}

int vmm_map(uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t* pte = walk_to_pte(g_kernel_pml4, virt, 1);
    if (!pte) return 0;
    *pte = (phys & ~0xFFFULL) | (flags & 0xFFF) | VMM_PRESENT;
    invlpg(virt);
    return 1;
}

void vmm_unmap(uint64_t virt) {
    uint64_t* pte = walk_to_pte(g_kernel_pml4, virt, 0);
    if (!pte) return;
    *pte = 0;
    invlpg(virt);
}

uint64_t vmm_resolve(uint64_t virt) {
    uint64_t* pte = walk_to_pte(g_kernel_pml4, virt, 0);
    if (!pte || !(*pte & VMM_PRESENT)) return 0;
    return (*pte & ~0xFFFULL) | (virt & 0xFFF);
}

uint64_t vmm_phys_to_virt(uint64_t phys) {
    return phys + g_hhdm_offset;
}

uint64_t vmm_virt_to_phys(uint64_t virt) {
    /* For kernel addresses in the HHDM region: */
    if (virt >= g_hhdm_offset) return virt - g_hhdm_offset;
    return vmm_resolve(virt);
}

void vmm_switch(uint64_t pml4_phys) {
    __asm__ volatile ("mov %0, %%cr3" : : "r"(pml4_phys) : "memory");
}

uint64_t vmm_current(void) {
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    return cr3 & ~0xFFFULL;
}

/*
 * Create a new user PML4.
 *
 * The upper 256 entries (kernel half) are copied from the kernel PML4 so
 * the kernel is always visible regardless of which CR3 is loaded. The lower
 * 256 entries (user half) start empty.
 */
uint64_t vmm_new_user_pd(void) {
    uint64_t new_phys = alloc_pt_phys();
    if (!new_phys) return 0;

    uint64_t* new_pml4    = phys_to_virt_ptr(new_phys);
    uint64_t* kernel_pml4 = phys_to_virt_ptr(g_kernel_pml4);

    for (int i = 0; i < 256; i++)  new_pml4[i] = 0;                   /* user half: empty */
    for (int i = 256; i < 512; i++) new_pml4[i] = kernel_pml4[i];     /* kernel half: shared */

    return new_phys;
}

/*
 * Free all user-half page tables rooted at pml4_phys.
 * Does NOT free the physical frames that were mapped — the caller must
 * have freed those already (or they're shared).
 */
void vmm_free_user_pd(uint64_t pml4_phys) {
    uint64_t* pml4 = phys_to_virt_ptr(pml4_phys);

    for (int i = 0; i < 256; i++) {
        if (!(pml4[i] & VMM_PRESENT)) continue;
        uint64_t* pdpt = phys_to_virt_ptr(pml4[i] & ~0xFFFULL);

        for (int j = 0; j < ENTRIES; j++) {
            if (!(pdpt[j] & VMM_PRESENT)) continue;
            uint64_t* pd = phys_to_virt_ptr(pdpt[j] & ~0xFFFULL);

            for (int k = 0; k < ENTRIES; k++) {
                if (pd[k] & VMM_PRESENT)
                    pmm_free(pd[k] & ~0xFFFULL);   /* free PT frame */
            }
            pmm_free(pdpt[j] & ~0xFFFULL);         /* free PD frame */
        }
        pmm_free(pml4[i] & ~0xFFFULL);             /* free PDPT frame */
    }
    pmm_free(pml4_phys);                           /* free PML4 frame itself */
}

int vmm_map_in(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t* pte = walk_to_pte(pml4_phys, virt, 1);
    if (!pte) return 0;
    *pte = (phys & ~0xFFFULL) | (flags & 0xFFF) | VMM_PRESENT;
    /* No invlpg needed: this PML4 isn't in CR3 yet. */
    return 1;
}

int vmm_page_fault(struct registers* regs) {
    uint64_t fault_addr = read_cr2();
    (void)regs;

    /* TODO: add copy_from_user recovery label support here. */
    printf("[vmm] page fault at 0x%llx (rip=0x%llx err=0x%llx)\n",
           fault_addr, regs->rip, regs->err_code);
    return 0;   /* not recoverable yet */
}