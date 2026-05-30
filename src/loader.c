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
#include "stdint.h"

/* =====================================================================
 * Per-task user resource tracking
 * ===================================================================== */

typedef struct {
    uint64_t virt;
    uint64_t phys;
} mapped_page_t;

#define PAGES_PER_BLOCK 64

typedef struct page_block {
    mapped_page_t      pages[PAGES_PER_BLOCK];
    int                used;
    struct page_block* next;
} page_block_t;

typedef struct {
    page_block_t* head;
    int           total_pages;
    uint64_t      heap_base;
    uint64_t      heap_brk;
    uint64_t      heap_max;
    uint64_t      pd_phys;
} user_resources_t;

static int record_page(user_resources_t* r, uint64_t virt, uint64_t phys);

/* =====================================================================
 * Resource cleanup
 * ===================================================================== */

static void release_resources(user_resources_t* r) {
    page_block_t* b = r->head;
    while (b) {
        for (int i = 0; i < b->used; i++)
            if (b->pages[i].phys) { pmm_free(b->pages[i].phys); b->pages[i].phys = 0; }
        page_block_t* next = b->next;
        kfree(b);
        b = next;
    }
    r->head = 0;
    r->total_pages = 0;
}

static void on_user_exit(task_t* t) {
    user_resources_t* r = (user_resources_t*)t->user_data;
    if (r) { release_resources(r); kfree(r); t->user_data = 0; }
    if (t->pd_phys) { vmm_free_user_pd(t->pd_phys); t->pd_phys = 0; }
}

/* =====================================================================
 * Page-record bookkeeping
 * ===================================================================== */

static int record_page(user_resources_t* r, uint64_t virt, uint64_t phys) {
    page_block_t* b = r->head;
    if (!b || b->used >= PAGES_PER_BLOCK) {
        page_block_t* nb = (page_block_t*)kmalloc(sizeof(page_block_t));
        if (!nb) { printf("loader: OOM tracking user pages\n"); return -1; }
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

/* =====================================================================
 * Page mapping helper
 * ===================================================================== */

static uint64_t alloc_user_page(uint64_t virt, uint64_t flags,
                                 user_resources_t* r, uint64_t pd_phys) {
    uint64_t frame = pmm_alloc();
    if (!frame) { printf("loader: OOM\n"); return 0; }

    if (!vmm_map_in(pd_phys, virt, frame, flags)) {
        pmm_free(frame);
        printf("loader: vmm_map_in failed at 0x%llx\n", virt);
        return 0;
    }

    /* Zero via HHDM. */
    memset((void*)vmm_phys_to_virt(frame), 0, 4096);

    if (record_page(r, virt, frame) != 0) {
        vmm_unmap(virt);
        pmm_free(frame);
        return 0;
    }
    return frame;
}

/* =====================================================================
 * ELF loader
 * ===================================================================== */

static uint64_t elf_load(const unsigned char* file, int filesz,
                          user_resources_t* r, uint64_t pd_phys) {
    if (filesz < (int)sizeof(Elf32_Ehdr)) { printf("loader: ELF too small\n"); return 0; }

    const Elf32_Ehdr* eh = (const Elf32_Ehdr*)file;

    if (eh->e_ident[EI_CLASS]  != ELFCLASS32)  { printf("loader: not ELF32\n");  return 0; }
    if (eh->e_ident[EI_DATA]   != ELFDATA2LSB) { printf("loader: not LE ELF\n"); return 0; }
    if (eh->e_type    != ET_EXEC)              { printf("loader: not executable\n"); return 0; }
    if (eh->e_machine != EM_386)               { printf("loader: not x86 ELF\n");   return 0; }
    if (eh->e_phnum == 0 || eh->e_phoff == 0)  { printf("loader: no phdrs\n");      return 0; }
    if (eh->e_entry == 0)                      { printf("loader: null entry\n");     return 0; }

    /* Pass 1: allocate and map pages. */
    for (int i = 0; i < (int)eh->e_phnum; i++) {
        unsigned int phdr_off = eh->e_phoff + (unsigned int)i * eh->e_phentsize;
        if (phdr_off + sizeof(Elf32_Phdr) > (unsigned int)filesz) {
            printf("loader: phdr %d out of bounds\n", i); return 0;
        }
        const Elf32_Phdr* ph = (const Elf32_Phdr*)(file + phdr_off);
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;

        uint64_t page_flags = VMM_PRESENT | VMM_USER;
        if (ph->p_flags & PF_W) page_flags |= VMM_WRITE;

        uint64_t seg_start = ph->p_vaddr & ~0xFFFULL;
        uint64_t seg_end   = (ph->p_vaddr + ph->p_memsz + 0xFFF) & ~0xFFFULL;

        for (uint64_t v = seg_start; v < seg_end; v += 0x1000) {
            if (vmm_resolve(v)) continue;   /* already mapped */
            if (!alloc_user_page(v, page_flags, r, pd_phys)) return 0;
        }
    }

    /* Pass 2: switch to user PD, copy file data, switch back. */
    vmm_switch(pd_phys);

    for (int i = 0; i < (int)eh->e_phnum; i++) {
        unsigned int phdr_off = eh->e_phoff + (unsigned int)i * eh->e_phentsize;
        const Elf32_Phdr* ph = (const Elf32_Phdr*)(file + phdr_off);
        if (ph->p_type != PT_LOAD || ph->p_filesz == 0) continue;
        memcpy((void*)(uint64_t)ph->p_vaddr, file + ph->p_offset, ph->p_filesz);
    }

    vmm_switch(vmm_current());   /* switch back to kernel CR3 */

    return eh->e_entry;
}

/* =====================================================================
 * Flat-binary loader
 * ===================================================================== */

static uint64_t flat_load(const unsigned char* buf, int filesz,
                           user_resources_t* r, uint64_t pd_phys) {
    if (filesz > LOADER_CODE_BYTES) {
        printf("loader: flat binary too large\n"); return 0;
    }
    uint64_t flags = VMM_PRESENT | VMM_WRITE | VMM_USER;
    for (int i = 0; i < LOADER_CODE_PAGES; i++) {
        uint64_t v = LOADER_CODE_VIRT + (uint64_t)i * 4096;
        if (!alloc_user_page(v, flags, r, pd_phys)) return 0;
    }
    vmm_switch(pd_phys);
    memcpy((void*)(uint64_t)LOADER_CODE_VIRT, buf, (unsigned int)filesz);
    vmm_switch(vmm_current());
    return LOADER_CODE_VIRT;
}

/* =====================================================================
 * Public API
 * ===================================================================== */

int loader_run(const char* path) {
    unsigned char* buf = (unsigned char*)kmalloc(LOADER_CODE_BYTES);
    if (!buf) { printf("loader: OOM\n"); return -1; }

    int n = vfs_read_file(path, buf, LOADER_CODE_BYTES);
    if (n <= 0) { printf("loader: %s: not found\n", path); kfree(buf); return -1; }

    uint64_t pd_phys = vmm_new_user_pd();
    if (!pd_phys) { printf("loader: failed to create PD\n"); kfree(buf); return -1; }

    user_resources_t* r = (user_resources_t*)kmalloc(sizeof(*r));
    if (!r) { vmm_free_user_pd(pd_phys); kfree(buf); return -1; }
    memset(r, 0, sizeof(*r));
    r->pd_phys   = pd_phys;
    r->heap_base = LOADER_HEAP_BASE;
    r->heap_brk  = LOADER_HEAP_BASE;
    r->heap_max  = LOADER_HEAP_BASE + LOADER_HEAP_MAX;

    uint64_t entry = elf32_valid(buf, n) ? elf_load(buf, n, r, pd_phys)
                                         : flat_load(buf, n, r, pd_phys);
    kfree(buf);

    if (!entry) { release_resources(r); kfree(r); vmm_free_user_pd(pd_phys); return -1; }

    if (!alloc_user_page(LOADER_STACK_PAGE, VMM_PRESENT | VMM_WRITE | VMM_USER, r, pd_phys)) {
        release_resources(r); kfree(r); vmm_free_user_pd(pd_phys); return -1;
    }

    int id = task_spawn_user(entry, LOADER_STACK_TOP, pd_phys, path);
    if (id < 0) {
        printf("loader: task_spawn_user failed\n");
        release_resources(r); kfree(r); vmm_free_user_pd(pd_phys); return -1;
    }

    task_t* t = task_find_by_id(id);
    if (t) { t->user_data = r; t->on_exit = on_user_exit; t->parent = task_current(); }
    return id;
}

/* =====================================================================
 * SYS_SBRK backend
 * ===================================================================== */

int loader_sbrk(int delta) {
    task_t* t = task_current();
    if (!t || !t->is_user || !t->user_data) return -1;

    user_resources_t* r = (user_resources_t*)t->user_data;
    uint64_t old_brk = r->heap_brk;

    if (delta <= 0) return (int)old_brk;

    uint64_t new_brk = old_brk + (uint64_t)delta;
    if (new_brk > r->heap_max) return -1;

    uint64_t first_page = (old_brk + 0xFFF) & ~0xFFFULL;
    uint64_t last_page  = (new_brk + 0xFFF) & ~0xFFFULL;

    for (uint64_t v = first_page; v < last_page; v += 0x1000) {
        if (vmm_resolve(v)) continue;
        if (!alloc_user_page(v, VMM_PRESENT | VMM_WRITE | VMM_USER, r, r->pd_phys))
            return -1;
    }

    r->heap_brk = new_brk;
    return (int)old_brk;
}

/* =====================================================================
 * fork()
 * ===================================================================== */

static int clone_resources(user_resources_t* dst, user_resources_t* src,
                            uint64_t child_pd) {
    dst->heap_base   = src->heap_base;
    dst->heap_brk    = src->heap_brk;
    dst->heap_max    = src->heap_max;
    dst->pd_phys     = child_pd;
    dst->head        = 0;
    dst->total_pages = 0;

    for (page_block_t* b = src->head; b; b = b->next) {
        for (int i = 0; i < b->used; i++) {
            uint64_t virt = b->pages[i].virt;
            uint64_t parent_phys = b->pages[i].phys;
            if (!parent_phys) continue;

            /* Resolve flags from the parent's PD. */
            uint64_t pte = vmm_resolve(virt);   /* returns phys | flags */
            uint64_t flags = pte ? (pte & 0xFFF) : (VMM_PRESENT | VMM_WRITE | VMM_USER);

            uint64_t frame = pmm_alloc();
            if (!frame) { printf("fork: OOM\n"); return -1; }

            /* Copy page contents via HHDM. */
            memcpy((void*)vmm_phys_to_virt(frame),
                   (void*)vmm_phys_to_virt(parent_phys), 4096);

            if (!vmm_map_in(child_pd, virt, frame, flags)) {
                pmm_free(frame);
                printf("fork: vmm_map_in failed at 0x%llx\n", virt);
                return -1;
            }
            if (record_page(dst, virt, frame) != 0) {
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
        printf("fork: caller is not a user task\n"); return -1;
    }
    user_resources_t* pr = (user_resources_t*)parent->user_data;

    uint64_t child_pd = vmm_new_user_pd();
    if (!child_pd) { printf("fork: cannot create child PD\n"); return -1; }

    user_resources_t* cr = (user_resources_t*)kmalloc(sizeof(*cr));
    if (!cr) { vmm_free_user_pd(child_pd); return -1; }
    memset(cr, 0, sizeof(*cr));

    if (clone_resources(cr, pr, child_pd) != 0) {
        release_resources(cr); kfree(cr); vmm_free_user_pd(child_pd); return -1;
    }

    /* Build child frame with rax = 0 so fork() returns 0 in child. */
    struct registers child_frame_storage;
    memcpy(&child_frame_storage, frame, sizeof(struct registers));
    child_frame_storage.rax = 0;

    int cid = task_clone_user(&child_frame_storage, child_pd, parent->name);
    if (cid < 0) {
        release_resources(cr); kfree(cr); vmm_free_user_pd(child_pd); return -1;
    }

    task_t* child = task_find_by_id(cid);
    if (!child) { release_resources(cr); kfree(cr); return cid; }
    child->user_data = cr;
    child->on_exit   = on_user_exit;
    child->parent    = parent;

    return cid;
}

/* =====================================================================
 * exec()
 * ===================================================================== */

int loader_exec(const char* path, struct registers* frame) {
    task_t* t = task_current();
    if (!t || !t->is_user || !t->user_data) {
        printf("exec: caller is not a user task\n"); return -1;
    }

    unsigned char* buf = (unsigned char*)kmalloc(LOADER_CODE_BYTES);
    if (!buf) { printf("exec: OOM\n"); return -1; }

    int n = vfs_read_file(path, buf, LOADER_CODE_BYTES);
    if (n <= 0) { printf("exec: %s: not found\n", path); kfree(buf); return -1; }

    uint64_t new_pd = vmm_new_user_pd();
    if (!new_pd) { kfree(buf); return -1; }

    user_resources_t* nr = (user_resources_t*)kmalloc(sizeof(*nr));
    if (!nr) { vmm_free_user_pd(new_pd); kfree(buf); return -1; }
    memset(nr, 0, sizeof(*nr));
    nr->pd_phys   = new_pd;
    nr->heap_base = LOADER_HEAP_BASE;
    nr->heap_brk  = LOADER_HEAP_BASE;
    nr->heap_max  = LOADER_HEAP_BASE + LOADER_HEAP_MAX;

    uint64_t entry = elf32_valid(buf, n) ? elf_load(buf, n, nr, new_pd)
                                         : flat_load(buf, n, nr, new_pd);
    kfree(buf);

    if (!entry) { release_resources(nr); kfree(nr); vmm_free_user_pd(new_pd); return -1; }

    if (!alloc_user_page(LOADER_STACK_PAGE, VMM_PRESENT | VMM_WRITE | VMM_USER, nr, new_pd)) {
        release_resources(nr); kfree(nr); vmm_free_user_pd(new_pd); return -1;
    }

    /* Commit: swap live task onto new address space. */
    user_resources_t* old = (user_resources_t*)t->user_data;
    uint64_t old_pd = t->pd_phys;

    t->pd_phys   = new_pd;
    t->user_data = nr;

    vmm_switch(new_pd);

    if (old) { release_resources(old); kfree(old); }
    if (old_pd) vmm_free_user_pd(old_pd);

    /* Rewrite the trap frame to enter the new program.
     * In 64-bit: rip, cs, rflags, rsp, ss are the iretq frame fields. */
    frame->rip    = entry;
    frame->cs     = 0x23;           /* GDT_UCODE_RPL3 */
    frame->rflags = 0x202;          /* IF=1 */
    frame->rsp    = LOADER_STACK_TOP;
    frame->ss     = 0x1B;           /* GDT_UDATA_RPL3 */
    frame->rax    = 0;
    frame->rbx    = 0;
    frame->rcx    = 0;
    frame->rdx    = 0;
    frame->rsi    = 0;
    frame->rdi    = 0;
    frame->rbp    = 0;

    return 0;
}