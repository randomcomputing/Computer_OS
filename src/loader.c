#include "loader.h"
#include "elf.h"
#include "fat12.h"
#include "vmm.h"
#include "pmm.h"
#include "task.h"
#include "kheap.h"
#include "string.h"
#include "printf.h"

// =====================================================================
// Per-task user resource tracking
// =====================================================================

typedef struct {
    unsigned int virt;
    unsigned int phys;
} mapped_page_t;

#define MAX_USER_PAGES (LOADER_CODE_PAGES + 8)

typedef struct {
    mapped_page_t pages[MAX_USER_PAGES];
    int           npages;
} user_resources_t;

static volatile int busy = 0;

// =====================================================================
// Resource cleanup
//
// Frees the physical frames of all user pages.  The page tables and PD
// are freed separately via vmm_free_user_pd(t->pd_phys) in on_user_exit.
// =====================================================================

static void release_resources(user_resources_t* r) {
    for (int i = 0; i < r->npages; i++) {
        if (r->pages[i].phys) {
            pmm_free(r->pages[i].phys);
            r->pages[i].phys = 0;
        }
    }
}

static void on_user_exit(task_t* t) {
    user_resources_t* r = (user_resources_t*)t->user_data;
    if (r) {
        release_resources(r);
        kfree(r);
        t->user_data = 0;
    }
    // Free the page tables and PD frame for this task's address space.
    if (t->pd_phys) {
        vmm_free_user_pd(t->pd_phys);
        t->pd_phys = 0;
    }
    busy = 0;
}

// =====================================================================
// Page mapping helper
//
// Allocates a physical frame, maps it into the given user PD at `virt`
// with `flags`, and zeroes the frame via its physical address (the
// identity map keeps all PMM frames < 4 MB accessible regardless of
// which CR3 is loaded).  Records the pair for cleanup.
// =====================================================================

static unsigned int alloc_user_page(unsigned int virt, unsigned int flags,
                                    user_resources_t* r, unsigned int pd_phys) {
    if (r->npages >= MAX_USER_PAGES) {
        printf("loader: too many user pages\n");
        return 0;
    }
    unsigned int frame = pmm_alloc();
    if (!frame) { printf("loader: out of physical memory\n"); return 0; }

    if (!vmm_map_pd(pd_phys, virt, frame, flags)) {
        pmm_free(frame);
        printf("loader: vmm_map_pd failed at 0x%x\n", virt);
        return 0;
    }

    // Zero via physical address — the frame is in the identity-mapped
    // low 4 MB, so (void*)frame is always accessible from kernel code.
    memset((void*)frame, 0, 4096);

    r->pages[r->npages].virt = virt;
    r->pages[r->npages].phys = frame;
    r->npages++;
    return frame;
}

// =====================================================================
// ELF loader
//
// Two passes over the PT_LOAD program headers:
//   Pass 1 (kernel PD): allocate frames, map into the user PD, zero
//                        each frame through its physical address.
//   Pass 2 (user PD):   briefly switch CR3 to copy file data into the
//                        now-mapped virtual addresses, then switch back.
//
// BSS (memsz > filesz) needs no explicit zeroing in pass 2 — the
// frames were already zeroed in pass 1.
// =====================================================================

static unsigned int elf_load(const unsigned char* file, int filesz,
                              user_resources_t* r, unsigned int pd_phys) {
    if (filesz < (int)sizeof(Elf32_Ehdr)) {
        printf("loader: ELF too small\n");
        return 0;
    }

    const Elf32_Ehdr* eh = (const Elf32_Ehdr*)file;

    if (eh->e_ident[EI_CLASS] != ELFCLASS32) {
        printf("loader: not a 32-bit ELF\n");
        return 0;
    }
    if (eh->e_ident[EI_DATA] != ELFDATA2LSB) {
        printf("loader: not a little-endian ELF\n");
        return 0;
    }
    if (eh->e_type != ET_EXEC) {
        printf("loader: not an executable ELF (e_type=%d)\n", (int)eh->e_type);
        return 0;
    }
    if (eh->e_machine != EM_386) {
        printf("loader: not an x86 ELF (e_machine=%d)\n", (int)eh->e_machine);
        return 0;
    }
    if (eh->e_phnum == 0 || eh->e_phoff == 0) {
        printf("loader: ELF has no program headers\n");
        return 0;
    }
    if (eh->e_entry == 0) {
        printf("loader: ELF entry point is NULL\n");
        return 0;
    }

    // --- Pass 1: allocate and map pages (no CR3 switch needed) -------

    for (int i = 0; i < (int)eh->e_phnum; i++) {
        unsigned int phdr_off = eh->e_phoff + (unsigned int)i * eh->e_phentsize;
        if (phdr_off + sizeof(Elf32_Phdr) > (unsigned int)filesz) {
            printf("loader: program header %d out of bounds\n", i);
            return 0;
        }

        const Elf32_Phdr* ph = (const Elf32_Phdr*)(file + phdr_off);
        if (ph->p_type != PT_LOAD) continue;
        if (ph->p_memsz == 0) continue;

        if (ph->p_vaddr >= 0xC0000000u ||
            ph->p_vaddr + ph->p_memsz > 0xC0000000u) {
            printf("loader: ELF segment overlaps kernel space (0x%x)\n",
                   ph->p_vaddr);
            return 0;
        }
        if (ph->p_filesz > 0 &&
            ph->p_offset + ph->p_filesz > (unsigned int)filesz) {
            printf("loader: ELF segment data out of bounds\n");
            return 0;
        }

        unsigned int page_flags = VMM_PRESENT | VMM_USER;
        if (ph->p_flags & PF_W) page_flags |= VMM_WRITE;

        unsigned int seg_start = ph->p_vaddr & ~0xFFFu;
        unsigned int seg_end   = (ph->p_vaddr + ph->p_memsz + 0xFFFu) & ~0xFFFu;

        for (unsigned int v = seg_start; v < seg_end; v += 0x1000) {
            if (vmm_resolve_pd(pd_phys, v)) continue;   // already mapped
            if (!alloc_user_page(v, page_flags, r, pd_phys)) return 0;
        }
    }

    // --- Pass 2: switch to user PD, copy file data, switch back ------

    vmm_switch_pd(pd_phys);

    for (int i = 0; i < (int)eh->e_phnum; i++) {
        unsigned int phdr_off = eh->e_phoff + (unsigned int)i * eh->e_phentsize;
        const Elf32_Phdr* ph = (const Elf32_Phdr*)(file + phdr_off);
        if (ph->p_type != PT_LOAD || ph->p_filesz == 0) continue;

        memcpy((void*)ph->p_vaddr, file + ph->p_offset, ph->p_filesz);
        // BSS gap (memsz > filesz) is already zero from pass 1.
    }

    vmm_switch_pd(vmm_kernel_pd_phys());

    return eh->e_entry;
}

// =====================================================================
// Flat-binary loader  (legacy path — keeps .bin programs working)
// =====================================================================

static unsigned int flat_load(const unsigned char* buf, int filesz,
                               user_resources_t* r, unsigned int pd_phys) {
    if (filesz > LOADER_CODE_BYTES) {
        printf("loader: flat binary too large (%d > %d bytes)\n",
               filesz, LOADER_CODE_BYTES);
        return 0;
    }

    unsigned int flags = VMM_PRESENT | VMM_WRITE | VMM_USER;
    for (int i = 0; i < LOADER_CODE_PAGES; i++) {
        unsigned int v = LOADER_CODE_VIRT + (unsigned int)i * 4096;
        if (!alloc_user_page(v, flags, r, pd_phys)) return 0;
    }

    // Pages are zeroed in alloc_user_page; switch to user PD to copy.
    vmm_switch_pd(pd_phys);
    memcpy((void*)LOADER_CODE_VIRT, buf, (unsigned int)filesz);
    vmm_switch_pd(vmm_kernel_pd_phys());

    return LOADER_CODE_VIRT;
}

// =====================================================================
// Public API
// =====================================================================

int loader_is_busy(void) { return busy; }

int loader_run(const char* path) {
    if (busy) {
        printf("loader: another program is still running\n");
        return -1;
    }

    unsigned char* buf = (unsigned char*)kmalloc(LOADER_CODE_BYTES);
    if (!buf) { printf("loader: out of kernel heap\n"); return -1; }

    int n = fat12_read_file(path, buf, LOADER_CODE_BYTES);
    if (n < 0) {
        printf("loader: %s: not found\n", path);
        kfree(buf);
        return -1;
    }
    if (n == 0) {
        printf("loader: %s: empty file\n", path);
        kfree(buf);
        return -1;
    }
    if (n == LOADER_CODE_BYTES) {
        printf("loader: warning: %s filled the read buffer; "
               "it may have been truncated\n", path);
    }

    // Create a fresh page directory for this task.
    unsigned int pd_phys = vmm_create_user_pd();
    if (!pd_phys) {
        printf("loader: failed to create user page directory\n");
        kfree(buf);
        return -1;
    }

    user_resources_t* r = (user_resources_t*)kmalloc(sizeof(*r));
    if (!r) { vmm_free_user_pd(pd_phys); kfree(buf); return -1; }
    memset(r, 0, sizeof(*r));

    unsigned int entry;
    if (elf32_valid(buf, n)) {
        entry = elf_load(buf, n, r, pd_phys);
    } else {
        entry = flat_load(buf, n, r, pd_phys);
    }

    kfree(buf);

    if (!entry) {
        release_resources(r);
        kfree(r);
        vmm_free_user_pd(pd_phys);
        return -1;
    }

    // Map the user stack (zeroed via physical address in alloc_user_page).
    if (!alloc_user_page(LOADER_STACK_PAGE,
                         VMM_PRESENT | VMM_WRITE | VMM_USER, r, pd_phys)) {
        release_resources(r);
        kfree(r);
        vmm_free_user_pd(pd_phys);
        return -1;
    }

    busy = 1;

    int id = task_spawn_user(entry, LOADER_STACK_TOP, pd_phys, path);
    if (id < 0) {
        busy = 0;
        printf("loader: task_spawn_user failed\n");
        release_resources(r);
        kfree(r);
        vmm_free_user_pd(pd_phys);
        return -1;
    }

    task_t* t = task_find_by_id(id);
    if (!t) {
        printf("loader: lost track of task %d\n", id);
        return id;
    }
    t->user_data = r;
    t->on_exit   = on_user_exit;

    return id;
}
