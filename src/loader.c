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

    /* Close every fd the process still had open: flushes buffered file
       writes and drops pipe references, so a parent reading the other
       end of a pipe sees EOF. */
    extern void linux_fdt_release(task_t* t);
    linux_fdt_release(t);

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
 * Where position-independent images go. Static ET_EXEC programs keep their
 * link address (0x400000 / 0x01000000). Must stay clear of the heap
 * (0x02000000..0x12000000), anonymous mmap (from 0x40000000 upward) and
 * the stack (just below 0x7fff0000).
 */
#define LOADER_PIE_BASE     0x0000555555554000ULL   /* main program (ET_DYN) */
#define LOADER_INTERP_BASE  0x00007f0000000000ULL   /* dynamic loader        */

typedef struct {
    uint64_t entry;          /* entry point, load bias applied            */
    uint64_t phdr;           /* program headers in memory, bias applied   */
    uint16_t phnum;
    uint64_t bias;           /* 0 for ET_EXEC                              */
    char     interp[128];    /* PT_INTERP path, "" if statically linked    */
} elf_info_t;

/*
 * elf_map: load an ELF64 image (ET_EXEC, or ET_DYN at dyn_base) by path,
 * reading only a small header buffer then each segment individually.
 * Avoids buffering the whole file in kernel heap. Returns 1 on success.
 */
static int elf_map(const char* path, uint64_t dyn_base,
                   user_resources_t* r, uint64_t pd_phys, elf_info_t* out) {
    memset(out, 0, sizeof(*out));

    /* Read just enough for ELF header + program headers */
    unsigned int hdr_buf_sz = 4096;
    unsigned char* hbuf = (unsigned char*)kmalloc(hdr_buf_sz);
    if (!hbuf) { printf("loader: OOM (hdr)\n"); return 0; }

    int n = vfs_read_file(path, hbuf, hdr_buf_sz);
    if (n < (int)sizeof(Elf64_Ehdr)) {
        printf("loader: %s: too small\n", path);
        kfree(hbuf); return 0;
    }

    Elf64_Ehdr eh_copy = *(const Elf64_Ehdr*)hbuf;

    if (eh_copy.e_ident[EI_CLASS] != ELFCLASS64 ||
        eh_copy.e_ident[EI_DATA]  != ELFDATA2LSB ||
        (eh_copy.e_type != ET_EXEC && eh_copy.e_type != ET_DYN) ||
        eh_copy.e_machine         != EM_X86_64    ||
        eh_copy.e_phnum == 0 || eh_copy.e_phoff == 0) {
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
    for (int i = 0; i < nph; i++)
        saved_phdrs[i] = *(const Elf64_Phdr*)
            (hbuf + eh_copy.e_phoff + (uint64_t)i * eh_copy.e_phentsize);
    kfree(hbuf);

    /* Load bias: ET_DYN images are linked at ~0 and placed at dyn_base. */
    uint64_t min_vaddr = ~0ULL;
    for (int i = 0; i < nph; i++)
        if (saved_phdrs[i].p_type == PT_LOAD && saved_phdrs[i].p_vaddr < min_vaddr)
            min_vaddr = saved_phdrs[i].p_vaddr;
    if (min_vaddr == ~0ULL) { printf("loader: %s: no PT_LOAD\n", path); return 0; }
    uint64_t bias = (eh_copy.e_type == ET_DYN) ? dyn_base - (min_vaddr & ~0xFFFULL) : 0;

    uint64_t phdr_vaddr = 0;
    for (int i = 0; i < nph; i++) {
        const Elf64_Phdr* ph = &saved_phdrs[i];
        if (ph->p_type == PT_PHDR) phdr_vaddr = ph->p_vaddr;
        if (ph->p_type == PT_LOAD && !phdr_vaddr &&
            ph->p_offset <= eh_copy.e_phoff &&
            eh_copy.e_phoff < ph->p_offset + ph->p_filesz)
            phdr_vaddr = ph->p_vaddr + (eh_copy.e_phoff - ph->p_offset);
        if (ph->p_type == PT_INTERP) {
            unsigned int len = (unsigned int)ph->p_filesz;
            if (len == 0 || len >= sizeof(out->interp)) {
                printf("loader: %s: bad PT_INTERP\n", path); return 0;
            }
            if (vfs_read_at(path, out->interp, (unsigned int)ph->p_offset, len) < (int)len) {
                printf("loader: %s: can't read PT_INTERP\n", path); return 0;
            }
            out->interp[len] = '\0';
        }
    }

    out->entry = eh_copy.e_entry + bias;
    out->phdr  = phdr_vaddr ? phdr_vaddr + bias : 0;
    out->phnum = (uint16_t)nph;
    out->bias  = bias;

    /* Map all PT_LOAD pages. Adjacent segments often share a page (end of
     * text / start of data): map each page only once. */
    for (int i = 0; i < nph; i++) {
        const Elf64_Phdr* ph = &saved_phdrs[i];
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;
        if (ph->p_filesz > ph->p_memsz) {
            printf("loader: bad segment %d\n", i); return 0;
        }
        uint64_t seg_start = (ph->p_vaddr + bias) & ~0xFFFULL;
        uint64_t seg_end   = (ph->p_vaddr + bias + ph->p_memsz + 0xFFFULL) & ~0xFFFULL;
        for (uint64_t v = seg_start; v < seg_end; v += 0x1000) {
            if (vmm_resolve_in(pd_phys, v)) continue;
            if (!alloc_user_page(v, VMM_PRESENT|VMM_WRITE|VMM_USER, r, pd_phys))
                return 0;
        }
    }

    /* Load each segment using vfs_read_at — reads directly from the
     * FAT32 cluster chain at the right offset, using only a 1-cluster
     * (~4KB) kernel buffer inside the driver.  No large allocations. */
    uint64_t old_cr3 = vmm_current();
    vmm_switch(pd_phys);

    for (int i = 0; i < nph; i++) {
        const Elf64_Phdr* ph = &saved_phdrs[i];
        if (ph->p_type != PT_LOAD || ph->p_filesz == 0) continue;

        int nr = vfs_read_at(path,
                             (void*)(ph->p_vaddr + bias),
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
            uint64_t bss_start = ph->p_vaddr + bias + ph->p_filesz;
            uint64_t bss_len   = ph->p_memsz - ph->p_filesz;
            memset((void*)bss_start, 0, (unsigned int)bss_len);
        }
    }

    vmm_switch(old_cr3);
    return 1;
}

/* Implemented in linux_syscall.c: resolve fake-symlink files in place. */
extern void linux_follow_symlinks(char* path, unsigned int pathsz);

/*
 * elf_load_program: load a program and, if it is dynamically linked,
 * its PT_INTERP loader (e.g. /lib/ld-musl-x86_64.so.1) too.
 *   returns   the address to start executing (interpreter entry for
 *             dynamic programs, program entry for static ones), 0 on error
 *   at_*      values for the auxiliary vector: AT_PHDR/AT_PHNUM/AT_ENTRY
 *             always describe the MAIN program; AT_BASE is the
 *             interpreter's load address (0 if static).
 */
static uint64_t elf_load_program(const char* path,
                                 user_resources_t* r, uint64_t pd_phys,
                                 uint64_t* at_phdr, uint16_t* at_phnum,
                                 uint64_t* at_entry, uint64_t* at_base) {
    elf_info_t mainp;
    if (!elf_map(path, LOADER_PIE_BASE, r, pd_phys, &mainp)) return 0;

    *at_phdr  = mainp.phdr;
    *at_phnum = mainp.phnum;
    *at_entry = mainp.entry;
    *at_base  = 0;

    if (!mainp.interp[0]) return mainp.entry;     /* statically linked */

    char ipath[256];
    unsigned int i = 0;
    for (; mainp.interp[i] && i < sizeof(ipath) - 1; i++) ipath[i] = mainp.interp[i];
    ipath[i] = '\0';
    linux_follow_symlinks(ipath, sizeof(ipath));

    elf_info_t interp;
    if (!elf_map(ipath, LOADER_INTERP_BASE, r, pd_phys, &interp)) {
        printf("loader: %s: interpreter %s not loadable\n", path, mainp.interp);
        return 0;
    }
    *at_base = interp.bias;
    return interp.entry;
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

/* =====================================================================
 * Linux initial process stack
 * =====================================================================
 * Builds   argc | argv[] NULL | envp[] NULL | auxv pairs AT_NULL | strings
 * in the CURRENTLY ACTIVE address space, ending at LOADER_STACK_TOP.
 * The stack pages must already be mapped (see map_user_stack).
 * Returns the initial rsp, or 0 if the arguments don't fit. */
#define LOADER_STACK_PAGES   8              /* + 1 page at LOADER_STACK_TOP */
#define LOADER_MAX_ARGS      256
#define LOADER_MAX_ARGBYTES  (16 * 1024)

static int map_user_stack(user_resources_t* r, uint64_t pd_phys) {
    for (int i = -1; i < LOADER_STACK_PAGES; i++) {
        if (!alloc_user_page(LOADER_STACK_PAGE - (uint64_t)(i * 0x1000),
                             VMM_PRESENT | VMM_WRITE | VMM_USER, r, pd_phys))
            return 0;
    }
    return 1;
}

static uint64_t build_linux_stack(const char* const* argv, int argc,
                                  const char* const* envp, int envc,
                                  uint64_t entry, uint64_t phdr_vaddr,
                                  uint16_t phnum, uint64_t at_base) {
    if (argc < 0 || argc > LOADER_MAX_ARGS) return 0;
    if (envc < 0 || envc > LOADER_MAX_ARGS) return 0;

    unsigned int total = 0;
    for (int i = 0; i < argc; i++) total += (unsigned int)strlen(argv[i]) + 1;
    for (int i = 0; i < envc; i++) total += (unsigned int)strlen(envp[i]) + 1;
    if (total > LOADER_MAX_ARGBYTES) return 0;

    uint8_t* top = (uint8_t*)LOADER_STACK_TOP;

    /* Strings, copied top-down. Remember where each one lands. */
    uint64_t argp[LOADER_MAX_ARGS];
    uint64_t envpp[LOADER_MAX_ARGS];
    for (int i = envc - 1; i >= 0; i--) {
        unsigned int n = (unsigned int)strlen(envp[i]) + 1;
        top -= n; memcpy(top, envp[i], n); envpp[i] = (uint64_t)top;
    }
    for (int i = argc - 1; i >= 0; i--) {
        unsigned int n = (unsigned int)strlen(argv[i]) + 1;
        top -= n; memcpy(top, argv[i], n); argp[i] = (uint64_t)top;
    }

    /* AT_RANDOM: 16 bytes. Zero keeps pointer-guard values predictable. */
    top = (uint8_t*)((uint64_t)top & ~15ULL);
    top -= 16;
    memset(top, 0, 16);
    uint64_t rand_ptr = (uint64_t)top;

    uint64_t* sp = (uint64_t*)((uint64_t)top & ~15ULL);

    /* Words pushed below: 13 auxv pairs (26) + envp NULL + envc
       + argv NULL + argc slots + argc word. rsp must be 16-aligned at
       entry with argc at [rsp], so pad above the aux vector if needed. */
    uint64_t words = 26 + 1 + (uint64_t)envc + 1 + (uint64_t)argc + 1;
    if (((uint64_t)sp - words * 8) & 15) *(--sp) = 0;

    sp -= 2; sp[0] = 0;  sp[1] = 0;                  /* AT_NULL   */
    sp -= 2; sp[0] = 6;  sp[1] = 4096;               /* AT_PAGESZ */
    sp -= 2; sp[0] = 25; sp[1] = rand_ptr;           /* AT_RANDOM */
    sp -= 2; sp[0] = 23; sp[1] = 0;                  /* AT_SECURE */
    sp -= 2; sp[0] = 14; sp[1] = 0;                  /* AT_EGID   */
    sp -= 2; sp[0] = 13; sp[1] = 0;                  /* AT_GID    */
    sp -= 2; sp[0] = 12; sp[1] = 0;                  /* AT_EUID   */
    sp -= 2; sp[0] = 11; sp[1] = 0;                  /* AT_UID    */
    sp -= 2; sp[0] = 7;  sp[1] = at_base;            /* AT_BASE: dynamic loader */
    sp -= 2; sp[0] = 9;  sp[1] = entry;              /* AT_ENTRY: main program  */
    sp -= 2; sp[0] = 4;  sp[1] = sizeof(Elf64_Phdr); /* AT_PHENT  */
    sp -= 2; sp[0] = 5;  sp[1] = phnum;              /* AT_PHNUM  */
    sp -= 2; sp[0] = 3;  sp[1] = phdr_vaddr;         /* AT_PHDR   */

    *(--sp) = 0;                                     /* envp NULL */
    for (int i = envc - 1; i >= 0; i--) *(--sp) = envpp[i];
    *(--sp) = 0;                                     /* argv NULL */
    for (int i = argc - 1; i >= 0; i--) *(--sp) = argp[i];
    *(--sp) = (uint64_t)argc;

    return (uint64_t)sp;
}

int loader_run_args(const char* path, const char* extra_args);

int loader_run(const char* path) {
    return loader_run_args(path, "");
}

int loader_run_args(const char* path, const char* extra_args) {
    /* `path` may be a symlink (busybox installs /bin/sh -> /bin/busybox).
       Load the real file, but keep the name the user typed as argv[0]:
       busybox picks the applet to run from argv[0]. */
    const char* argv0 = path;
    char real_path[256];
    {
        unsigned int i = 0;
        for (; path[i] && i < sizeof(real_path) - 1; i++) real_path[i] = path[i];
        real_path[i] = '\0';
        linux_follow_symlinks(real_path, sizeof(real_path));
        path = real_path;
    }

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
    uint64_t at_entry = 0, at_base = 0;

    int is_elf = (nm >= 4 && magic[0] == 0x7f && magic[1] == 'E' &&
                              magic[2] == 'L'  && magic[3] == 'F');

    if (is_elf) {
        /* Streaming ELF load (plus dynamic loader if PT_INTERP) */
        entry = elf_load_program(path, r, pd_phys, &elf_phdr_vaddr, &elf_phnum,
                                 &at_entry, &at_base);
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

    if (!map_user_stack(r, pd_phys)) {
        release_resources(r);
        kfree(r);
        vmm_free_user_pd(pd_phys);
        return -1;
    }

    /* argv = { path, extra_args split on spaces... } */
    #define MAX_EXTRA_ARGS 16
    char argbuf[512];
    const char* argv[MAX_EXTRA_ARGS + 1];
    int argc = 0;
    argv[argc++] = argv0;
    if (extra_args) {
        unsigned int o = 0;
        const char* p = extra_args;
        while (*p && argc < MAX_EXTRA_ARGS + 1) {
            while (*p == ' ') p++;
            if (!*p) break;
            argv[argc++] = &argbuf[o];
            while (*p && *p != ' ' && o < sizeof(argbuf) - 1) argbuf[o++] = *p++;
            argbuf[o++] = '\0';
            if (o >= sizeof(argbuf) - 1) break;
        }
    }

    uint64_t old_cr3_s = vmm_current();
    vmm_switch(pd_phys);
    uint64_t initial_rsp = build_linux_stack(argv, argc, 0, 0,
                                             at_entry ? at_entry : entry,
                                             elf_phdr_vaddr, elf_phnum, at_base);
    vmm_switch(old_cr3_s);
    if (!initial_rsp) {
        release_resources(r);
        kfree(r);
        vmm_free_user_pd(pd_phys);
        return -1;
    }


    int id = task_spawn_user(entry, initial_rsp, pd_phys, argv0);

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

/* Shared exec implementation.
 *   linux_abi = 1: Linux execve — builds argc/argv/envp/auxv, maps a full
 *                  stack, and refuses non-ELF files (returns -2 = ENOEXEC)
 *                  so the caller can handle "#!" scripts.
 *   linux_abi = 0: legacy int 0x80 exec — old behaviour (flat binaries
 *                  allowed, bare one-page stack). */
static int do_exec(const char* path,
                   const char* const* argv, int argc,
                   const char* const* envp, int envc,
                   int linux_abi, struct registers* frame) {
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
    uint64_t at_entry = 0, at_base = 0;

    int is_elf = (nm >= 4 && magic[0] == 0x7f && magic[1] == 'E' &&
                              magic[2] == 'L'  && magic[3] == 'F');
    if (is_elf) {
        entry = elf_load_program(path, nr, new_pd, &elf_phdr_vaddr, &elf_phnum,
                                 &at_entry, &at_base);
    } else if (linux_abi) {
        /* Not an ELF: never run arbitrary bytes as code. */
        kfree(nr); vmm_free_user_pd(new_pd); return -2;
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

    int stack_ok = linux_abi
        ? map_user_stack(nr, new_pd)
        : alloc_user_page(LOADER_STACK_PAGE, VMM_PRESENT | VMM_WRITE | VMM_USER,
                          nr, new_pd);
    if (!stack_ok) {
        release_resources(nr);
        kfree(nr);
        vmm_free_user_pd(new_pd);
        return -1;
    }

    /* Build the Linux stack BEFORE committing, so a failure leaves the
       caller's image intact. argv/envp must be kernel copies: the old
       address space is about to go away. */
    uint64_t initial_rsp = LOADER_STACK_TOP;
    if (linux_abi) {
        uint64_t prev_cr3 = vmm_current();
        vmm_switch(new_pd);
        initial_rsp = build_linux_stack(argv, argc, envp, envc,
                                        at_entry ? at_entry : entry,
                                        elf_phdr_vaddr, elf_phnum, at_base);
        vmm_switch(prev_cr3);
        if (!initial_rsp) {
            release_resources(nr);
            kfree(nr);
            vmm_free_user_pd(new_pd);
            return -7;   /* E2BIG */
        }
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
    frame->rsp    = initial_rsp;
    frame->ss     = 0x1B;

    frame->rax = 0;
    frame->rbx = 0;
    frame->rcx = 0;
    frame->rdx = 0;
    frame->rsi = 0;
    frame->rdi = 0;
    frame->rbp = 0;
    frame->r8 = frame->r9 = frame->r10 = frame->r11 = 0;
    frame->r12 = frame->r13 = frame->r14 = frame->r15 = 0;

    /* Fresh image: no TLS until its libc sets one up via arch_prctl. */
    t->fs_base = 0;
    __asm__ volatile ("wrmsr" : : "c"(0xC0000100U), "a"(0), "d"(0));

    return 0;
}

int loader_exec(const char* path, struct registers* frame) {
    return do_exec(path, 0, 0, 0, 0, 0, frame);
}

int loader_exec_argv(const char* path,
                     const char* const* argv, int argc,
                     const char* const* envp, int envc,
                     struct registers* frame) {
    return do_exec(path, argv, argc, envp, envc, 1, frame);
}