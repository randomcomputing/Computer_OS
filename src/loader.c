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
    if (!r) return;

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
    if (!t) return;

    user_resources_t* r = (user_resources_t*)t->user_data;

    if (r) {
        release_resources(r);
        kfree(r);
        t->user_data = 0;
    }

    if (t->pd_phys) {
        vmm_free_user_pd(t->pd_phys);
        t->pd_phys = 0;
    }
}

/* =====================================================================
 * Page-record bookkeeping
 * ===================================================================== */

static int record_page(user_resources_t* r, uint64_t virt, uint64_t phys) {
    page_block_t* b = r->head;

    if (!b || b->used >= PAGES_PER_BLOCK) {
        page_block_t* nb = (page_block_t*)kmalloc(sizeof(page_block_t));

        if (!nb) {
            printf("loader: OOM tracking user pages\n");
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

/* =====================================================================
 * Page mapping helper
 * ===================================================================== */

static uint64_t alloc_user_page(uint64_t virt,
                                uint64_t flags,
                                user_resources_t* r,
                                uint64_t pd_phys) {
    uint64_t frame = pmm_alloc();

    if (!frame) {
        printf("loader: OOM (pmm_alloc failed, %llu pages free)\n",
               (unsigned long long)pmm_free_pages());
        return 0;
    }

    if (!vmm_map_in(pd_phys, virt, frame, flags)) {
        pmm_free(frame);
        printf("loader: vmm_map_in failed at 0x%llx\n", virt);
        return 0;
    }

    /*
     * Zero the physical frame through the higher-half/direct map.
     * This works even when the user page table is not active.
     */
    memset((void*)vmm_phys_to_virt(frame), 0, 4096);

    if (record_page(r, virt, frame) != 0) {
        pmm_free(frame);
        return 0;
    }

    return frame;
}

/* =====================================================================
 * ELF64 loader
 * ===================================================================== */

/*
 * elf_load_streaming: load an ELF64 executable by path, reading only
 * a small header buffer then each segment individually from VFS.
 * Avoids buffering the whole file in kernel heap.
 */
static uint64_t elf_load_streaming(const char* path,
                                    user_resources_t* r,
                                    uint64_t pd_phys,
                                    uint64_t* out_phdr_vaddr,
                                    uint16_t* out_phnum) {
    /* Read just enough for ELF header + program headers */
    unsigned int hdr_buf_sz = 4096;
    unsigned char* hbuf = (unsigned char*)kmalloc(hdr_buf_sz);
    if (!hbuf) { printf("loader: OOM (hdr)\n"); return 0; }

    int n = vfs_read_file(path, hbuf, hdr_buf_sz);
    if (n < (int)sizeof(Elf64_Ehdr)) {
        printf("loader: %s: too small\n", path);
        kfree(hbuf); return 0;
    }

    Elf64_Ehdr eh_copy;
    const Elf64_Ehdr* eh = (const Elf64_Ehdr*)hbuf;
    eh_copy = *eh;

    if (eh_copy.e_ident[EI_CLASS] != ELFCLASS64 ||
        eh_copy.e_ident[EI_DATA]  != ELFDATA2LSB ||
        eh_copy.e_type            != ET_EXEC      ||
        eh_copy.e_machine         != EM_X86_64    ||
        eh_copy.e_phnum == 0 || eh_copy.e_phoff == 0 || eh_copy.e_entry == 0) {
        printf("loader: %s: invalid ELF header\n", path);
        kfree(hbuf); return 0;
    }

    /* Expand header buffer if phdrs extend past 4KB */
    uint64_t phdrs_end = (uint64_t)eh_copy.e_phoff +
                         (uint64_t)eh_copy.e_phnum * (uint64_t)eh_copy.e_phentsize;
    if (phdrs_end > hdr_buf_sz) {
        kfree(hbuf);
        hdr_buf_sz = (unsigned int)((phdrs_end + 4095) & ~4095ULL);
        hbuf = (unsigned char*)kmalloc(hdr_buf_sz);
        if (!hbuf) { printf("loader: OOM (phdrs)\n"); return 0; }
        n = vfs_read_file(path, hbuf, hdr_buf_sz);
        if (n < (int)phdrs_end) {
            printf("loader: %s: can't read phdrs\n", path);
            kfree(hbuf); return 0;
        }
    }

    /* Save program headers before freeing hbuf */
#define MAX_PHDRS 32
    Elf64_Phdr saved_phdrs[MAX_PHDRS];
    int nph = (eh_copy.e_phnum < MAX_PHDRS) ? eh_copy.e_phnum : MAX_PHDRS;
    uint64_t elf_phdr_vaddr = 0;

    for (int i = 0; i < nph; i++) {
        const Elf64_Phdr* ph = (const Elf64_Phdr*)
            (hbuf + eh_copy.e_phoff + (uint64_t)i * eh_copy.e_phentsize);
        saved_phdrs[i] = *ph;
        if (ph->p_type == 1 && elf_phdr_vaddr == 0) {
            uint64_t load_base = ph->p_vaddr - ph->p_offset;
            elf_phdr_vaddr = load_base + eh_copy.e_phoff;
        }
    }
    kfree(hbuf);

    if (out_phdr_vaddr) *out_phdr_vaddr = elf_phdr_vaddr;
    if (out_phnum)      *out_phnum      = (uint16_t)nph;

    /* Map all PT_LOAD pages */
    for (int i = 0; i < nph; i++) {
        const Elf64_Phdr* ph = &saved_phdrs[i];
        if (ph->p_type != 1 || ph->p_memsz == 0) continue;
        if (ph->p_filesz > ph->p_memsz) {
            printf("loader: bad segment %d\n", i); return 0;
        }
        uint64_t seg_start = ph->p_vaddr & ~0xFFFULL;
        uint64_t seg_end   = (ph->p_vaddr + ph->p_memsz + 0xFFFULL) & ~0xFFFULL;
        for (uint64_t v = seg_start; v < seg_end; v += 0x1000)
            if (!alloc_user_page(v, VMM_PRESENT|VMM_WRITE|VMM_USER, r, pd_phys))
                return 0;
    }

    /* Load each segment using vfs_read_at — reads directly from the
     * FAT32 cluster chain at the right offset, using only a 1-cluster
     * (~4KB) kernel buffer inside the driver.  No large allocations. */
    uint64_t old_cr3 = vmm_current();
    vmm_switch(pd_phys);

    for (int i = 0; i < nph; i++) {
        const Elf64_Phdr* ph = &saved_phdrs[i];
        if (ph->p_type != 1 || ph->p_filesz == 0) continue;

        int nr = vfs_read_at(path,
                             (void*)(uint64_t)ph->p_vaddr,
                             (unsigned int)ph->p_offset,
                             (unsigned int)ph->p_filesz);
        if (nr < (int)ph->p_filesz) {
            vmm_switch(old_cr3);
            printf("loader: segment %d short read (%d/%llu)\n",
                   i, nr, (unsigned long long)ph->p_filesz);
            return 0;
        }

        /* Zero the BSS tail (memsz > filesz) */
        if (ph->p_memsz > ph->p_filesz) {
            uint64_t bss_start = ph->p_vaddr + ph->p_filesz;
            uint64_t bss_len   = ph->p_memsz - ph->p_filesz;
            memset((void*)bss_start, 0, (unsigned int)bss_len);
        }
    }

    vmm_switch(old_cr3);
    return eh_copy.e_entry;
}

/* elf_load: load from an already-buffered ELF file (small binaries) */
static uint64_t elf_load(const unsigned char* file,
                         int filesz,
                         user_resources_t* r,
                         uint64_t pd_phys) {
    if (filesz < (int)sizeof(Elf64_Ehdr)) { printf("loader: ELF too small\n"); return 0; }
    const Elf64_Ehdr* eh = (const Elf64_Ehdr*)file;
    if (eh->e_ident[EI_CLASS] != ELFCLASS64 || eh->e_ident[EI_DATA] != ELFDATA2LSB ||
        eh->e_type != ET_EXEC || eh->e_machine != EM_X86_64 ||
        eh->e_phnum == 0 || eh->e_phoff == 0 || eh->e_entry == 0) {
        printf("loader: invalid ELF\n"); return 0;
    }
    if ((uint64_t)eh->e_phoff + (uint64_t)eh->e_phnum * eh->e_phentsize > (uint64_t)filesz) {
        printf("loader: program headers out of bounds\n"); return 0;
    }
    for (int i = 0; i < (int)eh->e_phnum; i++) {
        const Elf64_Phdr* ph = (const Elf64_Phdr*)
            (file + eh->e_phoff + (uint64_t)i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;
        if (ph->p_filesz > ph->p_memsz ||
            (uint64_t)ph->p_offset + ph->p_filesz > (uint64_t)filesz) {
            printf("loader: bad segment %d\n", i); return 0;
        }
        uint64_t seg_start = (uint64_t)ph->p_vaddr & ~0xFFFULL;
        uint64_t seg_end   = ((uint64_t)ph->p_vaddr + ph->p_memsz + 0xFFFULL) & ~0xFFFULL;
        for (uint64_t v = seg_start; v < seg_end; v += 0x1000)
            if (!alloc_user_page(v, VMM_PRESENT|VMM_WRITE|VMM_USER, r, pd_phys)) return 0;
    }
    uint64_t old_cr3 = vmm_current();
    vmm_switch(pd_phys);
    for (int i = 0; i < (int)eh->e_phnum; i++) {
        const Elf64_Phdr* ph = (const Elf64_Phdr*)
            (file + eh->e_phoff + (uint64_t)i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD || ph->p_filesz == 0) continue;
        memcpy((void*)(uint64_t)ph->p_vaddr, file + ph->p_offset, (unsigned int)ph->p_filesz);
        if (ph->p_memsz > ph->p_filesz)
            memset((void*)(uint64_t)(ph->p_vaddr + ph->p_filesz), 0,
                   (unsigned int)(ph->p_memsz - ph->p_filesz));
    }
    vmm_switch(old_cr3);
    return (uint64_t)eh->e_entry;
}

/* =====================================================================
 * Flat-binary loader
 * ===================================================================== */

static uint64_t flat_load(const unsigned char* buf,
                          int filesz,
                          user_resources_t* r,
                          uint64_t pd_phys) {
    if (filesz > LOADER_CODE_BYTES) {
        printf("loader: flat binary too large\n");
        return 0;
    }

    uint64_t flags = VMM_PRESENT | VMM_WRITE | VMM_USER;

    for (int i = 0; i < LOADER_CODE_PAGES; i++) {
        uint64_t v = LOADER_CODE_VIRT + (uint64_t)i * 4096;

        if (!alloc_user_page(v, flags, r, pd_phys)) {
            return 0;
        }
    }

    uint64_t old_cr3 = vmm_current();
    vmm_switch(pd_phys);

    memcpy((void*)(uint64_t)LOADER_CODE_VIRT, buf, (unsigned int)filesz);

    vmm_switch(old_cr3);

    return LOADER_CODE_VIRT;
}

/* =====================================================================
 * Public API
 * ===================================================================== */

int loader_run_args(const char* path, const char* extra_args);

int loader_run(const char* path) {
    return loader_run_args(path, "");
}

int loader_run_args(const char* path, const char* extra_args) {
    uint64_t pd_phys = vmm_new_user_pd();
    if (!pd_phys) { printf("loader: failed to create PD\n"); return -1; }

    user_resources_t* r = (user_resources_t*)kmalloc(sizeof(*r));
    if (!r) { vmm_free_user_pd(pd_phys); return -1; }
    memset(r, 0, sizeof(*r));
    r->pd_phys   = pd_phys;
    r->heap_base = LOADER_HEAP_BASE;
    r->heap_brk  = LOADER_HEAP_BASE;
    r->heap_max  = LOADER_HEAP_BASE + LOADER_HEAP_MAX;

    /* Probe: read just the first 8 bytes to detect ELF vs flat */
    unsigned char magic[8];
    int nm = vfs_read_file(path, magic, sizeof(magic));
    if (nm <= 0) {
        printf("loader: %s: not found\n", path);
        kfree(r); vmm_free_user_pd(pd_phys); return -1;
    }

    uint64_t entry = 0;
    uint64_t elf_phdr_vaddr = 0;
    uint16_t elf_phnum = 0;

    int is_elf = (nm >= 4 && magic[0] == 0x7f && magic[1] == 'E' &&
                              magic[2] == 'L'  && magic[3] == 'F');

    if (is_elf) {
        /* Streaming ELF load — never buffers the whole file */
        entry = elf_load_streaming(path, r, pd_phys, &elf_phdr_vaddr, &elf_phnum);
    } else {
        /* Flat binary — small, buffer the whole thing */
        unsigned char* buf = (unsigned char*)kmalloc(LOADER_CODE_BYTES);
        if (!buf) {
            printf("loader: OOM\n");
            kfree(r); vmm_free_user_pd(pd_phys); return -1;
        }
        int n = vfs_read_file(path, buf, LOADER_CODE_BYTES);
        if (n <= 0) {
            printf("loader: %s: not found\n", path);
            kfree(buf); kfree(r); vmm_free_user_pd(pd_phys); return -1;
        }
        entry = flat_load(buf, n, r, pd_phys);
        kfree(buf);
    }

    if (!entry) {
        vmm_free_user_pd(pd_phys);
        return -1;
    }

    /* Allocate 8 stack pages (32KB) — enough for musl startup.
       Pages run from LOADER_STACK_PAGE down to LOADER_STACK_PAGE - 7*0x1000.
       We also map the page AT LOADER_STACK_TOP so [rsp] is accessible. */
    for (int _si = -1; _si < 8; _si++) {
        if (!alloc_user_page(LOADER_STACK_PAGE - _si * 0x1000,
                             VMM_PRESENT | VMM_WRITE | VMM_USER,
                             r, pd_phys)) {
            release_resources(r);
            kfree(r);
            vmm_free_user_pd(pd_phys);
            return -1;
        }
    }

    /*
     * Set up the initial stack for musl/Linux ABI.
     * musl's _start reads: [rsp] = argc, argv[], NULL, envp[], NULL, auxv[], AT_NULL
     * We push this onto the user stack before spawning.
     */
    uint64_t old_cr3_s = vmm_current();
    vmm_switch(pd_phys);

    /*
     * Build Linux ABI initial stack in user memory.
     * Layout from rsp upward:
     *   argc, argv[0], NULL, envp[0], NULL, auxv pairs..., AT_NULL
     * Data area above aux vector: AT_RANDOM bytes, argv0 string.
     *
     * We start from LOADER_STACK_TOP and work downward.
     * Use a large gap so __init_libc's sub rsp, 0x158 doesn't
     * stomp on our TLS-related stack frames.
     */

    /* Start well below stack top to leave room for musl's init frames */
    uint8_t* stk_top = (uint8_t*)LOADER_STACK_TOP;

    /* Program name at top */
    const char* _pname = path ? path : "hello2";
    int _pname_len = 0;
    while (_pname[_pname_len]) _pname_len++;
    _pname_len++;
    stk_top -= _pname_len;
    for (int _pi2 = 0; _pi2 < _pname_len; _pi2++) stk_top[_pi2] = ((const uint8_t*)_pname)[_pi2];
    uint8_t* _argv0_str = stk_top;

    /* AT_RANDOM 16 bytes, 16-byte aligned.
     * Use all-zero bytes so glibc's pointer guard = 0 and
     * PTR_MANGLE/DEMANGLE is just ror/rol(17), always consistent. */
    stk_top = (uint8_t*)((uint64_t)stk_top & ~15ULL);
    stk_top -= 16;
    for (int _ri2 = 0; _ri2 < 16; _ri2++)
        stk_top[_ri2] = 0;
    uint8_t* _rand_ptr = stk_top;

    /* Extra args: write arg strings into the data area (above aux vector),
     * right after _rand_ptr. We write them top-down via stk_top BEFORE
     * building the aux/argv stack with sp. */
    #define MAX_EXTRA_ARGS 16
    uint8_t* arg_strs[MAX_EXTRA_ARGS];
    int nargs = 0;
    if (extra_args && extra_args[0]) {
        const char* p = extra_args;
        while (*p && nargs < MAX_EXTRA_ARGS) {
            while (*p == ' ') p++;
            if (!*p) break;
            const char* start = p;
            while (*p && *p != ' ') p++;
            int alen = (int)(p - start) + 1;
            stk_top -= alen;
            for (int ai = 0; ai < alen - 1; ai++) stk_top[ai] = ((const uint8_t*)start)[ai];
            stk_top[alen-1] = 0;
            arg_strs[nargs++] = stk_top;
        }
    }

    /* Align to 8 bytes */
    stk_top = (uint8_t*)((uint64_t)stk_top & ~7ULL);

    uint64_t* sp = (uint64_t*)stk_top;

    /* The ABI requires rsp % 16 == 0 at process entry with argc exactly
     * at [rsp]. We are about to push a fixed number of words:
     *   12 auxv pairs (24) + envp NULL (1) + argv NULL (1)
     *   + nargs argv slots + argv0 (1) + argc (1)  =  28 + nargs.
     * If that would leave rsp misaligned, insert one padding word HERE,
     * above the aux vector — never below argc, which must stay at [rsp]. */
    {
        int total_words = 28 + nargs;
        if (((uint64_t)sp - (uint64_t)total_words * 8) & 15)
            *(--sp) = 0;
    }

    /* Aux vector */
    sp -= 2; sp[0] = 0;  sp[1] = 0;              /* AT_NULL */
    sp -= 2; sp[0] = 6;  sp[1] = 4096;            /* AT_PAGESZ */
    sp -= 2; sp[0] = 25; sp[1] = (uint64_t)_rand_ptr; /* AT_RANDOM */
    sp -= 2; sp[0] = 23; sp[1] = 0;               /* AT_SECURE */
    sp -= 2; sp[0] = 14; sp[1] = 0;               /* AT_EGID */
    sp -= 2; sp[0] = 13; sp[1] = 0;               /* AT_GID */
    sp -= 2; sp[0] = 12; sp[1] = 0;               /* AT_EUID */
    sp -= 2; sp[0] = 11; sp[1] = 0;               /* AT_UID */
    sp -= 2; sp[0] = 9;  sp[1] = (uint64_t)entry; /* AT_ENTRY */
    sp -= 2; sp[0] = 4;  sp[1] = sizeof(Elf64_Phdr); /* AT_PHENT */
    sp -= 2; sp[0] = 5;  sp[1] = elf_phnum;       /* AT_PHNUM */
    sp -= 2; sp[0] = 3;  sp[1] = elf_phdr_vaddr;  /* AT_PHDR */

    /* NULL envp terminator */
    *(--sp) = 0;
    /* NULL argv terminator */
    *(--sp) = 0;

    /* argv[1..N] in reverse order */
    for (int ai = nargs - 1; ai >= 0; ai--)
        *(--sp) = (uint64_t)arg_strs[ai];

    /* argv[0] */
    *(--sp) = (uint64_t)_argv0_str;
    /* argc — must be exactly at [rsp] at entry; alignment was
     * handled by the padding word above the aux vector. */
    *(--sp) = (uint64_t)(1 + nargs);

    uint64_t initial_rsp = (uint64_t)sp;
    vmm_switch(old_cr3_s);

    int id = task_spawn_user(entry, initial_rsp, pd_phys, path);

    if (id < 0) {
        printf("loader: task_spawn_user failed\n");
        release_resources(r);
        kfree(r);
        vmm_free_user_pd(pd_phys);
        return -1;
    }

    task_t* t = task_find_by_id(id);

    if (t) {
        t->user_data = r;
        t->on_exit   = on_user_exit;
        t->parent    = task_current();
    }

    return id;
}

/* =====================================================================
 * SYS_SBRK backend
 * ===================================================================== */

uint64_t loader_sbrk64(int64_t delta) {
    task_t* t = task_current();
    if (!t || !t->is_user || !t->user_data) return 0;
    user_resources_t* r = (user_resources_t*)t->user_data;
    uint64_t old_brk = r->heap_brk;
    if (delta <= 0) return old_brk;
    uint64_t new_brk = old_brk + (uint64_t)delta;
    if (new_brk > r->heap_max) return old_brk; /* fail = return old */
    uint64_t first_page = (old_brk + 0xFFFULL) & ~0xFFFULL;
    uint64_t last_page  = (new_brk + 0xFFFULL) & ~0xFFFULL;
    for (uint64_t v = first_page; v < last_page; v += 0x1000) {
        /* Skip pages already mapped (e.g. left over from a previous
         * partially-failed grow, or below a shrunken break). */
        if (vmm_resolve_in(t->pd_phys, v)) continue;
        if (!alloc_user_page(v, VMM_PRESENT|VMM_WRITE|VMM_USER, r, t->pd_phys)) {
            /* Out of pages mid-grow. Do NOT advance the break — a break
             * that covers unmapped pages makes malloc hand out memory
             * that faults on first touch. The pages mapped so far stay
             * mapped and tracked; they'll back a smaller future grow. */
            return old_brk;
        }
    }
    r->heap_brk = new_brk;
    return old_brk;
}

/* Move the break down without unmapping (pages are reused on regrowth
 * and released at task exit). */
void loader_set_brk(uint64_t brk) {
    task_t* t = task_current();
    if (!t || !t->is_user || !t->user_data) return;
    user_resources_t* r = (user_resources_t*)t->user_data;
    if (brk >= r->heap_base && brk <= r->heap_brk)
        r->heap_brk = brk;
}

/* Map one zeroed anonymous user page at virt in the current task's
 * address space, tracked so it is freed at task exit. Used by mmap. */
int loader_map_anon_page(uint64_t virt) {
    task_t* t = task_current();
    if (!t || !t->is_user || !t->user_data) return 0;
    user_resources_t* r = (user_resources_t*)t->user_data;
    if (vmm_resolve_in(t->pd_phys, virt)) return 1; /* already mapped */
    return alloc_user_page(virt, VMM_PRESENT|VMM_WRITE|VMM_USER,
                           r, t->pd_phys) ? 1 : 0;
}

int loader_sbrk(int delta) {
    task_t* t = task_current();

    if (!t || !t->is_user || !t->user_data) {
        return -1;
    }

    user_resources_t* r = (user_resources_t*)t->user_data;
    uint64_t old_brk = r->heap_brk;

    if (delta <= 0) {
        return (int)old_brk;
    }

    uint64_t new_brk = old_brk + (uint64_t)delta;

    if (new_brk > r->heap_max) {
        return -1;
    }

    uint64_t first_page = (old_brk + 0xFFFULL) & ~0xFFFULL;
    uint64_t last_page  = (new_brk + 0xFFFULL) & ~0xFFFULL;

    for (uint64_t v = first_page; v < last_page; v += 0x1000) {
        if (!alloc_user_page(v,
                             VMM_PRESENT | VMM_WRITE | VMM_USER,
                             r,
                             r->pd_phys)) {
            return -1;
        }
    }

    r->heap_brk = new_brk;

    return (int)old_brk;
}

/* =====================================================================
 * fork()
 * ===================================================================== */

static int clone_resources(user_resources_t* dst,
                           user_resources_t* src,
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

            if (!parent_phys) {
                continue;
            }

            /*
             * Clone all user pages writable for now.
             * Later you can preserve exact page flags.
             */
            uint64_t flags = VMM_PRESENT | VMM_WRITE | VMM_USER;

            uint64_t frame = pmm_alloc();

            if (!frame) {
                printf("fork: OOM\n");
                return -1;
            }

            memcpy((void*)vmm_phys_to_virt(frame),
                   (void*)vmm_phys_to_virt(parent_phys),
                   4096);

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
        printf("fork: caller is not a user task\n");
        return -1;
    }

    user_resources_t* pr = (user_resources_t*)parent->user_data;

    uint64_t child_pd = vmm_new_user_pd();

    if (!child_pd) {
        printf("fork: cannot create child PD\n");
        return -1;
    }

    user_resources_t* cr = (user_resources_t*)kmalloc(sizeof(*cr));

    if (!cr) {
        vmm_free_user_pd(child_pd);
        return -1;
    }

    memset(cr, 0, sizeof(*cr));

    if (clone_resources(cr, pr, child_pd) != 0) {
        release_resources(cr);
        kfree(cr);
        vmm_free_user_pd(child_pd);
        return -1;
    }

    /*
     * Build child frame with rax = 0 so fork() returns 0 in child.
     */
    struct registers child_frame_storage;
    memcpy(&child_frame_storage, frame, sizeof(struct registers));
    child_frame_storage.rax = 0;

    int cid = task_clone_user(&child_frame_storage, child_pd, parent->name);

    if (cid < 0) {
        release_resources(cr);
        kfree(cr);
        vmm_free_user_pd(child_pd);
        return -1;
    }

    task_t* child = task_find_by_id(cid);

    if (!child) {
        release_resources(cr);
        kfree(cr);
        return cid;
    }

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
        printf("exec: caller is not a user task\n");
        return -1;
    }

    uint64_t new_pd = vmm_new_user_pd();
    if (!new_pd) return -1;

    user_resources_t* nr = (user_resources_t*)kmalloc(sizeof(*nr));
    if (!nr) { vmm_free_user_pd(new_pd); return -1; }
    memset(nr, 0, sizeof(*nr));
    nr->pd_phys   = new_pd;
    nr->heap_base = LOADER_HEAP_BASE;
    nr->heap_brk  = LOADER_HEAP_BASE;
    nr->heap_max  = LOADER_HEAP_BASE + LOADER_HEAP_MAX;

    /* Probe magic bytes */
    unsigned char magic[8];
    int nm = vfs_read_file(path, magic, sizeof(magic));
    if (nm <= 0) {
        printf("exec: %s: not found\n", path);
        kfree(nr); vmm_free_user_pd(new_pd); return -1;
    }

    uint64_t entry = 0;
    uint64_t elf_phdr_vaddr = 0;
    uint16_t elf_phnum = 0;

    int is_elf = (nm >= 4 && magic[0] == 0x7f && magic[1] == 'E' &&
                              magic[2] == 'L'  && magic[3] == 'F');
    if (is_elf) {
        entry = elf_load_streaming(path, nr, new_pd, &elf_phdr_vaddr, &elf_phnum);
    } else {
        unsigned char* buf = (unsigned char*)kmalloc(LOADER_CODE_BYTES);
        if (!buf) { kfree(nr); vmm_free_user_pd(new_pd); return -1; }
        int n = vfs_read_file(path, buf, LOADER_CODE_BYTES);
        if (n > 0) entry = flat_load(buf, n, nr, new_pd);
        kfree(buf);
    }

    if (!entry) {
        release_resources(nr);
        kfree(nr);
        vmm_free_user_pd(new_pd);
        return -1;
    }

    if (!alloc_user_page(LOADER_STACK_PAGE,
                         VMM_PRESENT | VMM_WRITE | VMM_USER,
                         nr,
                         new_pd)) {
        release_resources(nr);
        kfree(nr);
        vmm_free_user_pd(new_pd);
        return -1;
    }

    /*
     * Commit:
     * Replace the current process address space.
     */
    user_resources_t* old = (user_resources_t*)t->user_data;
    uint64_t old_pd = t->pd_phys;

    t->pd_phys   = new_pd;
    t->user_data = nr;

    vmm_switch(new_pd);

    if (old) {
        release_resources(old);
        kfree(old);
    }

    if (old_pd) {
        vmm_free_user_pd(old_pd);
    }

    /*
     * Rewrite the syscall trap frame so iretq enters the new program.
     */
    frame->rip    = entry;
    frame->cs     = 0x23;
    frame->rflags = 0x202;
    frame->rsp    = LOADER_STACK_TOP;
    frame->ss     = 0x1B;

    frame->rax = 0;
    frame->rbx = 0;
    frame->rcx = 0;
    frame->rdx = 0;
    frame->rsi = 0;
    frame->rdi = 0;
    frame->rbp = 0;

    return 0;
}