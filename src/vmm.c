#include "vmm.h"
#include "pmm.h"
#include "printf.h"
#include "vga.h"
#include "isr.h"

#define ENTRIES    1024
#define KERNEL_VMA 0xC0000000u

extern unsigned int boot_page_directory[];

static unsigned int* page_directory      = 0;
static unsigned int  page_directory_phys = 0;

static inline unsigned int kvirt_to_phys(unsigned int v) { return v - KERNEL_VMA; }
static inline unsigned int pd_index(unsigned int v) { return (v >> 22) & 0x3FF; }
static inline unsigned int pt_index(unsigned int v) { return (v >> 12) & 0x3FF; }

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
    for (int i = 0; i < ENTRIES; i++) pg[i] = 0;
}

// All PMM frames are below 4 MB, which is covered by the identity map in
// PDE[0].  That PDE is copied into every user PD at creation time, so this
// trick works regardless of which CR3 is currently loaded.
static inline unsigned int* phys_to_accessible(unsigned int phys) {
    return (unsigned int*)phys;
}

// =====================================================================
// Internal per-PD helpers
// =====================================================================

static int map_in_pd(unsigned int* pd, unsigned int virt,
                     unsigned int phys, unsigned int flags) {
    unsigned int pdi = pd_index(virt);
    unsigned int pti = pt_index(virt);

    unsigned int pde = pd[pdi];
    unsigned int* pt;

    if (!(pde & VMM_PRESENT)) {
        unsigned int pt_phys = pmm_alloc();
        if (!pt_phys) return 0;
        pt = phys_to_accessible(pt_phys);
        zero_page(pt);
        pd[pdi] = pt_phys | VMM_PRESENT | VMM_WRITE | (flags & VMM_USER);
    } else {
        if (pde & 0x80) {
            printf("[vmm] map into PSE region @ 0x%x not supported\n", virt);
            return 0;
        }
        pt = phys_to_accessible(pde & 0xFFFFF000);
    }

    pt[pti] = (phys & 0xFFFFF000) | (flags & 0xFFF) | VMM_PRESENT;
    invlpg(virt);
    return 1;
}

static void unmap_in_pd(unsigned int* pd, unsigned int virt) {
    unsigned int pde = pd[pd_index(virt)];
    if (!(pde & VMM_PRESENT)) return;
    if (pde & 0x80) return;

    unsigned int* pt = phys_to_accessible(pde & 0xFFFFF000);
    pt[pt_index(virt)] = 0;
    invlpg(virt);
}

static unsigned int resolve_in_pd(unsigned int* pd, unsigned int virt) {
    unsigned int pde = pd[pd_index(virt)];
    if (!(pde & VMM_PRESENT)) return 0;

    if (pde & 0x80) {
        return (pde & 0xFFC00000) | (virt & 0x003FFFFF);
    }

    unsigned int* pt = phys_to_accessible(pde & 0xFFFFF000);
    unsigned int pte = pt[pt_index(virt)];
    if (!(pte & VMM_PRESENT)) return 0;

    return (pte & 0xFFFFF000) | (virt & 0xFFF);
}

// =====================================================================
// PSE demotion (kernel-PD only, called once during vmm_init)
// =====================================================================

static int demote_pse(unsigned int pd_idx) {
    unsigned int old = page_directory[pd_idx];
    if (!(old & VMM_PRESENT) || !(old & 0x80)) return 1;

    unsigned int base_phys = old & 0xFFC00000;
    unsigned int flags     = old & 0xFFF & ~0x80;

    unsigned int pt_phys = pmm_alloc();
    if (!pt_phys) return 0;
    unsigned int* pt = phys_to_accessible(pt_phys);
    for (int i = 0; i < ENTRIES; i++) {
        pt[i] = (base_phys + (unsigned int)i * 4096) | (flags & 0xFFF) | VMM_PRESENT;
    }

    page_directory[pd_idx] = pt_phys | VMM_PRESENT | VMM_WRITE;

    for (unsigned int v = pd_idx * 0x400000u;
         v < (pd_idx + 1) * 0x400000u;
         v += 4096) {
        invlpg(v);
    }
    return 1;
}

// =====================================================================
// Public kernel-PD API (wrappers around the internal helpers)
// =====================================================================

int vmm_map(unsigned int virt, unsigned int phys, unsigned int flags) {
    return map_in_pd(page_directory, virt, phys, flags);
}

void vmm_unmap(unsigned int virt) {
    unmap_in_pd(page_directory, virt);
}

unsigned int vmm_resolve(unsigned int virt) {
    return resolve_in_pd(page_directory, virt);
}

// =====================================================================
// Per-PD public API
// =====================================================================

int vmm_map_pd(unsigned int pd_phys, unsigned int virt,
               unsigned int phys, unsigned int flags) {
    return map_in_pd(phys_to_accessible(pd_phys), virt, phys, flags);
}

void vmm_unmap_pd(unsigned int pd_phys, unsigned int virt) {
    unmap_in_pd(phys_to_accessible(pd_phys), virt);
}

unsigned int vmm_resolve_pd(unsigned int pd_phys, unsigned int virt) {
    return resolve_in_pd(phys_to_accessible(pd_phys), virt);
}

unsigned int vmm_create_user_pd(void) {
    unsigned int pd_phys = pmm_alloc();
    if (!pd_phys) return 0;

    unsigned int* pd = phys_to_accessible(pd_phys);

    // Copy all 1024 PDEs from the kernel PD.  This gives the new PD:
    //   PDE[0]     — identity map of low 4 MB (ring-0 only, no VMM_USER)
    //                needed so phys_to_accessible() works during syscalls
    //   PDEs[1-767]— empty (user programs will populate these)
    //   PDEs[768+] — kernel high half (ring-0 only)
    for (int i = 0; i < ENTRIES; i++) pd[i] = page_directory[i];

    return pd_phys;
}

void vmm_free_user_pd(unsigned int pd_phys) {
    unsigned int* pd = phys_to_accessible(pd_phys);

    // Free only page tables the user task ADDED — PDEs whose value
    // differs from the kernel PD's copy.  PDEs that match the kernel PD
    // (PDE[0] identity map, any other pre-existing kernel tables) are
    // SHARED and must not be freed here.
    for (int i = 0; i < 768; i++) {
        unsigned int pde = pd[i];
        if ((pde & VMM_PRESENT) && !(pde & 0x80) &&
            pde != page_directory[i]) {
            pmm_free(pde & 0xFFFFF000);
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

// =====================================================================
// Initialisation
// =====================================================================

void vmm_init(unsigned int identity_mb) {
    (void)identity_mb;

    page_directory      = boot_page_directory;
    page_directory_phys = kvirt_to_phys((unsigned int)boot_page_directory);

    if (!demote_pse(0))   printf("[vmm] demote_pse(0) failed\n");
    if (!demote_pse(768)) printf("[vmm] demote_pse(768) failed\n");

    load_cr3(page_directory_phys);
}

// =====================================================================
// Page-fault handler
// =====================================================================

void vmm_page_fault(struct registers* regs) {
    unsigned int cr2 = read_cr2();
    unsigned int err = regs->err_code;

    vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
    printf("\n*** PAGE FAULT ***\n");
    vga_set_color(VGA_WHITE, VGA_BLACK);
    printf("  fault addr (CR2): 0x%x\n", cr2);
    printf("  EIP:              0x%x\n", regs->eip);
    printf("  cause: %s, %s, %s%s\n",
           (err & 1) ? "protection violation" : "non-present page",
           (err & 2) ? "write"                : "read",
           (err & 4) ? "user mode"            : "kernel mode",
           (err & 16) ? ", instruction fetch" : "");
}
