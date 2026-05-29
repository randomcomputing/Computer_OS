#include "vmm.h"
#include "pmm.h"
#include "printf.h"
#include "console.h"
#include "isr.h"
#include "task.h"

#define ENTRIES    1024
#define KERNEL_VMA 0xC0000000u
#define PDE_PS     0x80u

/*
 * Page-table pages allocated through PMM must be below 4 MB because this
 * simple kernel directly dereferences physical addresses through the low
 * identity map.
 */
#define VMM_META_MAX_PHYS 0x00400000u

extern unsigned int boot_page_directory[];

static unsigned int* page_directory      = 0;
static unsigned int  page_directory_phys = 0;

/*
 * Static page tables for demoting the original bootstrap 4 MB PSE mappings:
 *
 *   PDE[0]   maps 0x00000000 -> 0x00000000
 *   PDE[768] maps 0xC0000000 -> 0x00000000
 */
static unsigned int demote_pt_low[1024]  __attribute__((aligned(4096)));
static unsigned int demote_pt_high[1024] __attribute__((aligned(4096)));

/*
 * Static kernel page-table pool.
 *
 * The earlier value, 128, was too small for your current kernel boot path.
 * Your OS now maps heap, PCI/e1000 MMIO, user-program structures, and then
 * the framebuffer at 0xE0000000. By the time VBE starts, the pool is already
 * exhausted.
 *
 * 512 page tables = 512 * 4 KB = 2 MB of page-table memory.
 * Each page table maps 4 MB, so this can cover up to 2 GB of virtual space.
 */
#define BOOT_PT_POOL_COUNT 512

static unsigned int boot_pt_pool[BOOT_PT_POOL_COUNT][1024]
    __attribute__((aligned(4096)));

static unsigned int boot_pt_pool_used = 0;

static inline unsigned int kvirt_to_phys(unsigned int v) {
    return v - KERNEL_VMA;
}

static inline unsigned int pd_index(unsigned int v) {
    return (v >> 22) & 0x3FFu;
}

static inline unsigned int pt_index(unsigned int v) {
    return (v >> 12) & 0x3FFu;
}

static inline unsigned int read_cr2(void) {
    unsigned int v;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(v));
    return v;
}

static inline void load_cr3(unsigned int pd_phys) {
    __asm__ volatile ("mov %0, %%cr3" : : "r"(pd_phys) : "memory");
}

static inline void invlpg(unsigned int v) {
    __asm__ volatile ("invlpg (%0)" : : "r"(v) : "memory");
}

static void zero_page(unsigned int* pg) {
    for (int i = 0; i < ENTRIES; i++) {
        pg[i] = 0;
    }
}

/*
 * Physical addresses below 4 MB are directly accessible because PDE[0]
 * keeps low memory identity-mapped.
 */
static inline unsigned int* phys_to_accessible(unsigned int phys) {
    return (unsigned int*)phys;
}

/*
 * Allocate one page table for the kernel page directory.
 *
 * First try the static boot pool. If that is exhausted, fall back to PMM
 * below 4 MB. This keeps early boot stable but avoids breaking VBE when the
 * static pool runs out.
 */
static unsigned int alloc_kernel_pt_page(void) {
    if (boot_pt_pool_used < BOOT_PT_POOL_COUNT) {
        unsigned int* pt = boot_pt_pool[boot_pt_pool_used++];

        zero_page(pt);

        return kvirt_to_phys((unsigned int)pt);
    }

    printf("[vmm] kernel boot page-table pool exhausted, using PMM fallback\n");

    unsigned int pt_phys = pmm_alloc_below(VMM_META_MAX_PHYS);

    if (!pt_phys) {
        printf("[vmm] PMM fallback failed for kernel page table\n");
        return 0;
    }

    unsigned int* pt = phys_to_accessible(pt_phys);
    zero_page(pt);

    return pt_phys;
}

/*
 * User page tables should come from PMM so they can be released later.
 */
static unsigned int alloc_user_pt_page(void) {
    unsigned int pt_phys = pmm_alloc_below(VMM_META_MAX_PHYS);

    if (!pt_phys) {
        printf("[vmm] failed to allocate user page table below 4 MB\n");
        return 0;
    }

    unsigned int* pt = phys_to_accessible(pt_phys);
    zero_page(pt);

    return pt_phys;
}

/*
 * Allocate a new page table for the target page directory.
 *
 * Kernel PD uses the static boot pool, with PMM fallback.
 * User PDs use PMM pages below 4 MB.
 */
static unsigned int alloc_pt_for_pd(unsigned int* pd) {
    if (pd == page_directory) {
        return alloc_kernel_pt_page();
    }

    return alloc_user_pt_page();
}

static int map_in_pd(unsigned int* pd,
                     unsigned int virt,
                     unsigned int phys,
                     unsigned int flags) {
    unsigned int pdi = pd_index(virt);
    unsigned int pti = pt_index(virt);

    unsigned int pde = pd[pdi];
    unsigned int* pt;

    if (!(pde & VMM_PRESENT)) {
        unsigned int pt_phys = alloc_pt_for_pd(pd);

        if (!pt_phys) {
            printf("[vmm] map failed: no page table for virt=0x%x\n", virt);
            return 0;
        }

        pt = phys_to_accessible(pt_phys);

        pd[pdi] = pt_phys | VMM_PRESENT | VMM_WRITE | (flags & VMM_USER);
    } else {
        if (pde & PDE_PS) {
            printf("[vmm] map failed: PSE PDE still present for virt=0x%x\n",
                   virt);
            return 0;
        }

        pt = phys_to_accessible(pde & 0xFFFFF000u);
    }

    pt[pti] = (phys & 0xFFFFF000u)
            | (flags & 0xFFFu)
            | VMM_PRESENT;

    invlpg(virt);

    return 1;
}

static void unmap_in_pd(unsigned int* pd, unsigned int virt) {
    unsigned int pde = pd[pd_index(virt)];

    if (!(pde & VMM_PRESENT)) {
        return;
    }

    if (pde & PDE_PS) {
        return;
    }

    unsigned int* pt = phys_to_accessible(pde & 0xFFFFF000u);
    pt[pt_index(virt)] = 0;

    invlpg(virt);
}

static unsigned int resolve_in_pd(unsigned int* pd, unsigned int virt) {
    unsigned int pde = pd[pd_index(virt)];

    if (!(pde & VMM_PRESENT)) {
        return 0;
    }

    if (pde & PDE_PS) {
        return (pde & 0xFFC00000u) | (virt & 0x003FFFFFu);
    }

    unsigned int* pt = phys_to_accessible(pde & 0xFFFFF000u);
    unsigned int pte = pt[pt_index(virt)];

    if (!(pte & VMM_PRESENT)) {
        return 0;
    }

    return (pte & 0xFFFFF000u) | (virt & 0xFFFu);
}

static int demote_pse(unsigned int pd_idx) {
    unsigned int old = page_directory[pd_idx];

    if (!(old & VMM_PRESENT)) {
        return 1;
    }

    if (!(old & PDE_PS)) {
        return 1;
    }

    unsigned int base_phys = old & 0xFFC00000u;
    unsigned int flags     = (old & 0xFFFu) & ~PDE_PS;

    unsigned int* pt;
    unsigned int pt_phys;

    if (pd_idx == 0) {
        pt = demote_pt_low;
        pt_phys = kvirt_to_phys((unsigned int)demote_pt_low);
    } else if (pd_idx == 768) {
        pt = demote_pt_high;
        pt_phys = kvirt_to_phys((unsigned int)demote_pt_high);
    } else {
        printf("[vmm] demote_pse(%u): unsupported PDE\n", pd_idx);
        return 0;
    }

    for (int i = 0; i < ENTRIES; i++) {
        pt[i] = (base_phys + (unsigned int)i * PAGE_SIZE)
              | flags
              | VMM_PRESENT;
    }

    page_directory[pd_idx] = pt_phys | VMM_PRESENT | VMM_WRITE;

    unsigned int start = pd_idx * 0x400000u;
    unsigned int end   = start + 0x400000u;

    for (unsigned int v = start; v < end; v += PAGE_SIZE) {
        invlpg(v);
    }

    return 1;
}

int vmm_map(unsigned int virt, unsigned int phys, unsigned int flags) {
    return map_in_pd(page_directory, virt, phys, flags);
}

void vmm_unmap(unsigned int virt) {
    unmap_in_pd(page_directory, virt);
}

unsigned int vmm_resolve(unsigned int virt) {
    return resolve_in_pd(page_directory, virt);
}

int vmm_map_pd(unsigned int pd_phys,
               unsigned int virt,
               unsigned int phys,
               unsigned int flags) {
    return map_in_pd(phys_to_accessible(pd_phys), virt, phys, flags);
}

void vmm_unmap_pd(unsigned int pd_phys, unsigned int virt) {
    unmap_in_pd(phys_to_accessible(pd_phys), virt);
}

unsigned int vmm_resolve_pd(unsigned int pd_phys, unsigned int virt) {
    return resolve_in_pd(phys_to_accessible(pd_phys), virt);
}

unsigned int vmm_resolve_pd_flags(unsigned int pd_phys, unsigned int virt) {
    unsigned int* pd = phys_to_accessible(pd_phys);
    unsigned int pde = pd[pd_index(virt)];

    if (!(pde & VMM_PRESENT)) {
        return 0;
    }

    if (pde & PDE_PS) {
        unsigned int phys = (pde & 0xFFC00000u)
                          | (virt & 0x003FF000u);

        return phys | ((pde & 0xFFFu) & ~PDE_PS);
    }

    unsigned int* pt = phys_to_accessible(pde & 0xFFFFF000u);
    unsigned int pte = pt[pt_index(virt)];

    if (!(pte & VMM_PRESENT)) {
        return 0;
    }

    return pte;
}

unsigned int vmm_create_user_pd(void) {
    unsigned int pd_phys = pmm_alloc_below(VMM_META_MAX_PHYS);

    if (!pd_phys) {
        printf("[vmm] failed to allocate user page directory below 4 MB\n");
        return 0;
    }

    unsigned int* pd = phys_to_accessible(pd_phys);

    for (int i = 0; i < ENTRIES; i++) {
        pd[i] = page_directory[i];
    }

    return pd_phys;
}

void vmm_free_user_pd(unsigned int pd_phys) {
    unsigned int* pd = phys_to_accessible(pd_phys);

    /*
     * Free only page tables added by the user address space.
     *
     * PDEs equal to page_directory[i] are shared kernel/identity mappings.
     */
    for (int i = 0; i < 768; i++) {
        unsigned int pde = pd[i];

        if ((pde & VMM_PRESENT) &&
            !(pde & PDE_PS) &&
            pde != page_directory[i]) {
            pmm_free(pde & 0xFFFFF000u);
        }
    }

    pmm_free(pd_phys);
}

void vmm_switch_pd(unsigned int pd_phys) {
    load_cr3(pd_phys);
}

unsigned int vmm_kernel_pd_phys(void) {
    return page_directory_phys;
}

void vmm_init(unsigned int identity_mb) {
    (void)identity_mb;

    page_directory      = boot_page_directory;
    page_directory_phys = kvirt_to_phys((unsigned int)boot_page_directory);

    if (!demote_pse(0)) {
        printf("[vmm] demote_pse(0) failed\n");
    }

    if (!demote_pse(768)) {
        printf("[vmm] demote_pse(768) failed\n");
    }

    load_cr3(page_directory_phys);
}

int vmm_page_fault(struct registers* regs) {
    unsigned int cr2 = read_cr2();
    unsigned int err = regs->err_code;

    task_t* cur = task_current();

    if (cur && cur->in_user_access && cur->fault_recovery_eip) {
        cur->uaccess_faulted = 1;
        regs->eip = cur->fault_recovery_eip;
        return 1;
    }

    con_set_color(CON_LIGHT_RED, CON_BLACK);
    printf("\n*** PAGE FAULT ***\n");

    con_set_color(CON_WHITE, CON_BLACK);
    printf("  fault addr (CR2): 0x%x\n", cr2);
    printf("  EIP:              0x%x\n", regs->eip);

    printf("  cause: %s, %s, %s%s\n",
           (err & 1) ? "protection violation" : "non-present page",
           (err & 2) ? "write" : "read",
           (err & 4) ? "user mode" : "kernel mode",
           (err & 16) ? ", instruction fetch" : "");

    return 0;
}