#include "loader.h"
#include "elf.h"
#include "vfs.h"
#include "vmm.h"
#include "pmm.h"
#include "task.h"
#include "kheap.h"
#include "string.h"
#include "printf.h"
#include "isr.h"

// =====================================================================
// Per-task user resource tracking
//
// We need to remember every physical frame mapped into the user's PD so
// we can free them on exit. The original implementation used a fixed
// array sized for code + a few extras; that doesn't scale once the heap
// can grow to thousands of pages. We use a simple linked list of blocks
// instead — each block holds PAGES_PER_BLOCK records, and we allocate a
// new block when the current one fills up.
// =====================================================================

typedef struct {
    unsigned int virt;
    unsigned int phys;
} mapped_page_t;

#define PAGES_PER_BLOCK 64

typedef struct page_block {
    mapped_page_t      pages[PAGES_PER_BLOCK];
    int                used;
    struct page_block* next;
} page_block_t;

typedef struct {
    page_block_t* head;             // singly-linked list, newest at head
    int           total_pages;      // running count, for diagnostics

    // Heap region (sbrk). heap_brk grows in 4 KB increments and is the
    // first byte NOT yet allocated; the user program treats it as the
    // current break.
    unsigned int  heap_base;
    unsigned int  heap_brk;
    unsigned int  heap_max;         // hard cap; heap_brk may not exceed this
    unsigned int  pd_phys;          // saved so sbrk can map without re-deriving
} user_resources_t;

// Forward decl — used by both elf_load/flat_load and loader_sbrk.
static int record_page(user_resources_t* r, unsigned int virt, unsigned int phys);

// =====================================================================
// Resource cleanup
//
// Frees the physical frames of every page the loader (or sbrk) mapped
// for this task, then frees the block list. Page tables and the PD
// itself are freed separately via vmm_free_user_pd() in on_user_exit.
// =====================================================================

static void release_resources(user_resources_t* r) {
    page_block_t* b = r->head;
    while (b) {
        for (int i = 0; i < b->used; i++) {
            if (b->pages[i].phys) {
                pmm_free(b->pages[i].phys);
                b->pages[i].phys = 0;
            }
        }
        page_block_t* next = b->next;
        kfree(b);
        b = next;
    }
    r->head = 0;
    r->total_pages = 0;
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
}

// =====================================================================
// Page-record bookkeeping
//
// Returns 0 on success, -1 if we couldn't allocate a new tracking block.
// =====================================================================

static int record_page(user_resources_t* r, unsigned int virt, unsigned int phys) {
    page_block_t* b = r->head;
    if (!b || b->used >= PAGES_PER_BLOCK) {
        page_block_t* nb = (page_block_t*)kmalloc(sizeof(page_block_t));
        if (!nb) {
            printf("loader: out of kernel heap tracking user pages\n");
            return -1;
        }
        memset(nb, 0, sizeof(*nb));
        nb->next = r->head;
        r->head  = nb;
        b = nb;
    }
    b->pages[b->used].virt = virt;
    b->pages[b->used].phys = phys;
    b->used++;
    r->total_pages++;
    return 0;
}

// =====================================================================
// Page mapping helper
//
// Allocates a physical frame, maps it into the given user PD at `virt`
// with `flags`, zeroes the frame via its physical address (the identity
// map keeps all PMM frames < 4 MB accessible regardless of which CR3 is
// loaded), and records the pair for cleanup.
// =====================================================================

static unsigned int alloc_user_page(unsigned int virt, unsigned int flags,
                                    user_resources_t* r, unsigned int pd_phys) {
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

    if (record_page(r, virt, frame) != 0) {
        // Tracking failed; unmap and free the frame so we don't leak it.
        vmm_unmap_pd(pd_phys, virt);
        pmm_free(frame);
        return 0;
    }
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

int loader_run(const char* path) {
    unsigned char* buf = (unsigned char*)kmalloc(LOADER_CODE_BYTES);
    if (!buf) { printf("loader: out of kernel heap\n"); return -1; }

    int n = vfs_read_file(path, buf, LOADER_CODE_BYTES);
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
    r->pd_phys   = pd_phys;
    r->heap_base = LOADER_HEAP_BASE;
    r->heap_brk  = LOADER_HEAP_BASE;
    r->heap_max  = LOADER_HEAP_BASE + LOADER_HEAP_MAX;

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

    int id = task_spawn_user(entry, LOADER_STACK_TOP, pd_phys, path);
    if (id < 0) {
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
    t->parent    = task_current();

    return id;
}

// =====================================================================
// SYS_SBRK backend
//
// Grow the calling task's user heap by `delta` bytes, rounded up to a
// page boundary. Returns the OLD break (so a user-space malloc gets
// "here's the chunk you just allocated"). On failure, returns -1.
//
// Negative delta is accepted but treated as a no-op for now — shrinking
// the heap would require unmapping pages and updating the tracker mid-
// list, which we don't need yet.
// =====================================================================

int loader_sbrk(int delta) {
    task_t* t = task_current();
    if (!t || !t->is_user || !t->user_data) return -1;

    user_resources_t* r = (user_resources_t*)t->user_data;
    unsigned int old_brk = r->heap_brk;

    if (delta == 0) return (int)old_brk;
    if (delta < 0)  return (int)old_brk;   // shrink not implemented

    unsigned int d       = (unsigned int)delta;
    unsigned int new_brk = old_brk + d;

    // Overflow / cap check.
    if (new_brk < old_brk)         return -1;     // wraparound
    if (new_brk > r->heap_max)     return -1;     // exceeds cap

    // Allocate every fresh page between old_brk and new_brk. Existing
    // pages (when old_brk wasn't page-aligned) are left alone.
    unsigned int first_page = (old_brk + 0xFFFu) & ~0xFFFu;
    unsigned int last_page  = (new_brk + 0xFFFu) & ~0xFFFu;

    for (unsigned int v = first_page; v < last_page; v += 0x1000) {
        if (vmm_resolve_pd(r->pd_phys, v)) continue;   // already mapped
        if (!alloc_user_page(v,
                             VMM_PRESENT | VMM_WRITE | VMM_USER,
                             r, r->pd_phys)) {
            // Out of memory or tracking failed. Pages we already mapped
            // on this call stay mapped (tracked, freed on exit), but
            // the user sees -1 and doesn't get any of this delta.
            return -1;
        }
    }

    r->heap_brk = new_brk;
    return (int)old_brk;
}

// =====================================================================
// fork()
//
// Clone the calling user task into a brand-new address space. We walk
// the parent's resource tracker (which records every page the loader or
// sbrk mapped: code, data, heap, stack), and for each one:
//   - allocate a fresh physical frame
//   - copy the parent's page contents into it (both frames are in the
//     identity-mapped low region, so we copy physical->physical with no
//     CR3 switch)
//   - map it into the child PD at the same virtual address with the
//     parent's exact flags
//   - record it in the child's tracker so it's freed on the child's exit
//
// Then we spawn the child via task_clone_user, handing it the syscall
// register frame with eax forced to 0. The parent gets the child's pid.
// =====================================================================

static int clone_resources(user_resources_t* dst, user_resources_t* src,
                           unsigned int child_pd) {
    // Copy heap bookkeeping verbatim — the actual heap pages are copied
    // below along with everything else in the page list.
    dst->heap_base = src->heap_base;
    dst->heap_brk  = src->heap_brk;
    dst->heap_max  = src->heap_max;
    dst->pd_phys   = child_pd;
    dst->head      = 0;
    dst->total_pages = 0;

    for (page_block_t* b = src->head; b; b = b->next) {
        for (int i = 0; i < b->used; i++) {
            unsigned int virt = b->pages[i].virt;

            // Get the parent's PTE so we replicate its flags exactly.
            unsigned int pte = vmm_resolve_pd_flags(src->pd_phys, virt);
            if (!pte) continue;                 // stale entry; skip
            unsigned int parent_phys = pte & 0xFFFFF000;
            unsigned int flags       = pte & 0xFFF;

            unsigned int frame = pmm_alloc();
            if (!frame) {
                printf("fork: out of physical memory\n");
                return -1;
            }
            // Copy page contents physical->physical (identity-mapped).
            memcpy((void*)frame, (void*)parent_phys, 4096);

            if (!vmm_map_pd(child_pd, virt, frame, flags)) {
                pmm_free(frame);
                printf("fork: vmm_map_pd failed at 0x%x\n", virt);
                return -1;
            }
            if (record_page(dst, virt, frame) != 0) {
                vmm_unmap_pd(child_pd, virt);
                pmm_free(frame);
                return -1;
            }
        }
    }
    return 0;
}

int loader_fork(struct registers* frame) {
    task_t* parent = task_current();
    if (!parent || !parent->is_user || !parent->user_data) {
        printf("fork: caller is not a user task\n");
        return -1;
    }
    user_resources_t* pr = (user_resources_t*)parent->user_data;

    // 1. Fresh address space for the child.
    unsigned int child_pd = vmm_create_user_pd();
    if (!child_pd) { printf("fork: cannot create child PD\n"); return -1; }

    // 2. Child resource tracker.
    user_resources_t* cr = (user_resources_t*)kmalloc(sizeof(*cr));
    if (!cr) { vmm_free_user_pd(child_pd); return -1; }
    memset(cr, 0, sizeof(*cr));

    // 3. Deep-copy every parent page into the child.
    if (clone_resources(cr, pr, child_pd) != 0) {
        release_resources(cr);
        kfree(cr);
        vmm_free_user_pd(child_pd);
        return -1;
    }

    // 4. Build the child's resume frame: identical to the parent's trap
    //    frame, but with eax = 0 so fork() returns 0 in the child. We
    //    pass the parent's frame to task_clone_user, which copies it onto
    //    the child's kernel stack; tweak a local copy's eax first.
    struct registers child_frame_storage;
    // struct registers is 14 words; the cross-ring useresp/ss live just
    // past it in the same contiguous block the asm stub built. Copy all
    // 16 words so the child can iret across rings.
    for (int i = 0; i < 16; i++)
        ((unsigned int*)&child_frame_storage)[i] = ((unsigned int*)frame)[i];
    child_frame_storage.eax = 0;   // child sees fork() == 0

    int cid = task_clone_user(&child_frame_storage, child_pd, parent->name);
    if (cid < 0) {
        release_resources(cr);
        kfree(cr);
        vmm_free_user_pd(child_pd);
        return -1;
    }

    // 5. Wire up the child's PCB: resource tracker, cleanup hook, parent.
    task_t* child = task_find_by_id(cid);
    if (!child) {
        // Extremely unlikely (we just created it). Leak-safe: the reaper
        // will free the task; resources are already attached below only
        // if we found it, so free them here.
        release_resources(cr);
        kfree(cr);
        return cid;
    }
    child->user_data = cr;
    child->on_exit   = on_user_exit;
    child->parent    = parent;

    // Parent's return value: the child's pid.
    return cid;
}

// =====================================================================
// exec()
//
// Replace the calling task's program image in place. We load the new
// program into a *fresh* PD, and only once that has fully succeeded do
// we tear down the old address space and switch the live task over to
// the new one. This keeps exec atomic-ish: a failure to load the new
// image leaves the caller's current program intact and running.
//
// On success we rewrite `frame` so that when the syscall stub iret's, it
// lands at the new program's entry point on a fresh user stack. We also
// swap the task's pd_phys / user_data and reload CR3, because the rest
// of this syscall (and the iret) execute with the *current* CR3 — which
// must become the new address space before we return to ring 3.
// =====================================================================

int loader_exec(const char* path, struct registers* frame) {
    task_t* t = task_current();
    if (!t || !t->is_user || !t->user_data) {
        printf("exec: caller is not a user task\n");
        return -1;
    }

    // --- Load the new image into a fresh PD (don't touch the old one) --
    unsigned char* buf = (unsigned char*)kmalloc(LOADER_CODE_BYTES);
    if (!buf) { printf("exec: out of kernel heap\n"); return -1; }

    int n = vfs_read_file(path, buf, LOADER_CODE_BYTES);
    if (n <= 0) {
        printf("exec: %s: not found\n", path);
        kfree(buf);
        return -1;
    }

    unsigned int new_pd = vmm_create_user_pd();
    if (!new_pd) { kfree(buf); return -1; }

    user_resources_t* nr = (user_resources_t*)kmalloc(sizeof(*nr));
    if (!nr) { vmm_free_user_pd(new_pd); kfree(buf); return -1; }
    memset(nr, 0, sizeof(*nr));
    nr->pd_phys   = new_pd;
    nr->heap_base = LOADER_HEAP_BASE;
    nr->heap_brk  = LOADER_HEAP_BASE;
    nr->heap_max  = LOADER_HEAP_BASE + LOADER_HEAP_MAX;

    unsigned int entry;
    if (elf32_valid(buf, n)) entry = elf_load(buf, n, nr, new_pd);
    else                     entry = flat_load(buf, n, nr, new_pd);
    kfree(buf);

    if (!entry) {
        release_resources(nr);
        kfree(nr);
        vmm_free_user_pd(new_pd);
        return -1;
    }

    // New user stack.
    if (!alloc_user_page(LOADER_STACK_PAGE,
                         VMM_PRESENT | VMM_WRITE | VMM_USER, nr, new_pd)) {
        release_resources(nr);
        kfree(nr);
        vmm_free_user_pd(new_pd);
        return -1;
    }

    // --- Commit: swap the live task onto the new address space ---------
    // Free the OLD resources + PD. We're currently running on the old
    // CR3, but the kernel half is identical across all user PDs and the
    // identity-mapped low region (where page tables live) is shared, so
    // it's safe to free the old user pages and switch CR3 right after.
    user_resources_t* old = (user_resources_t*)t->user_data;
    unsigned int old_pd = t->pd_phys;

    t->pd_phys   = new_pd;
    t->user_data = nr;
    // on_exit stays on_user_exit (already set when the task was created).

    // Switch CR3 to the new address space so the upcoming iret resolves
    // the new program's pages.
    vmm_switch_pd(new_pd);

    // Now that we're off the old PD, release it.
    if (old) {
        release_resources(old);
        kfree(old);
    }
    if (old_pd) vmm_free_user_pd(old_pd);

    // --- Rewrite the trap frame to enter the new program --------------
    // Clean ring-3 state: entry point, fresh stack top, ring-3 segments,
    // IF=1. Zero the GP regs the way a fresh process expects.
    struct user_trap_frame* uf = (struct user_trap_frame*)frame;
    uf->eip     = entry;
    uf->cs      = 0x1B;            // ring-3 code
    uf->eflags  = 0x202;          // IF=1, reserved bit
    uf->useresp = LOADER_STACK_TOP;
    uf->ss      = 0x23;           // ring-3 data
    uf->ds      = 0x23;
    uf->eax = uf->ebx = uf->ecx = uf->edx = 0;
    uf->esi = uf->edi = uf->ebp = 0;

    // The syscall stub will iret into the new program. Returning 0 here
    // is irrelevant (eax is overwritten above), but keep it tidy.
    return 0;
}