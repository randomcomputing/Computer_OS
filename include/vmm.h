#ifndef VMM_H
#define VMM_H

// 4 KB pages, 1024 entries per page dir / page table.
// We use classic 32-bit x86 two-level paging (no PAE).

// Page flags for vmm_map()
#define VMM_PRESENT  0x1
#define VMM_WRITE    0x2
#define VMM_USER     0x4

// Initialize paging: build a page directory, identity-map the first
// `identity_mb` megabytes (so the kernel, VGA buffer at 0xB8000, and all
// code/data we've already touched keep working), load CR3, enable PG.
// After this call, every virtual address <= identity_mb*MB maps 1:1 to
// the same physical address. Call this AFTER pmm_init() — we allocate
// page tables from the PMM.
void vmm_init(unsigned int identity_mb);

// Map / unmap / resolve in the KERNEL page directory (current CR3).
int          vmm_map(unsigned int virt, unsigned int phys, unsigned int flags);
void         vmm_unmap(unsigned int virt);
unsigned int vmm_resolve(unsigned int virt);

// Per-PD variants — operate on an arbitrary page directory identified by
// its PHYSICAL address (which is also its identity-mapped virtual address
// since all PMM frames live in the low 4 MB).  Use these when building a
// user task's address space before it has ever been loaded into CR3.
int          vmm_map_pd(unsigned int pd_phys, unsigned int virt,
                        unsigned int phys, unsigned int flags);
void         vmm_unmap_pd(unsigned int pd_phys, unsigned int virt);
unsigned int vmm_resolve_pd(unsigned int pd_phys, unsigned int virt);

// Allocate a fresh page directory for a user task.  Copies all 1024 PDEs
// from the current kernel PD so the kernel half is immediately accessible
// (syscalls / interrupts execute kernel code with the user PD in CR3).
// User-space PDEs not yet touched are zero.  Returns the PHYSICAL address
// of the new PD, or 0 on allocation failure.
unsigned int vmm_create_user_pd(void);

// Free all user-half (PDEs 0-767) page tables in a user PD, then free
// the PD frame itself.  Does NOT free the physical pages pointed to by
// the PTEs — those are owned by the loader's resource tracker.
void vmm_free_user_pd(unsigned int pd_phys);

// Load a page directory by its physical address into CR3.
void vmm_switch_pd(unsigned int pd_phys);

// Physical address of the global kernel page directory.
unsigned int vmm_kernel_pd_phys(void);

// Page-fault handler, called from isr.c when int_no == 14.
struct registers;
int vmm_page_fault(struct registers* regs);

#endif