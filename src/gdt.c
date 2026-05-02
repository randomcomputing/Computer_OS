#include "gdt.h"

// --- GDT entry layout -------------------------------------------------
// Same packed-bitfield layout as IDT entries. A "segment descriptor" in
// 32-bit protected mode is 8 bytes describing a base+limit+access. For
// flat memory we set base=0 and limit=4 GB, varying only the access byte
// to distinguish kcode / kdata / ucode / udata. The TSS is a system
// descriptor with a real base (the TSS struct address) and limit (sizeof).

struct gdt_entry {
    unsigned short limit_low;
    unsigned short base_low;
    unsigned char  base_mid;
    unsigned char  access;
    unsigned char  granularity;   // limit_high (low nibble) | flags (high nibble)
    unsigned char  base_high;
} __attribute__((packed));

struct gdt_ptr {
    unsigned short limit;
    unsigned int   base;
} __attribute__((packed));

// 6 entries: null, kcode, kdata, ucode, udata, tss.
#define GDT_ENTRIES 6
static struct gdt_entry gdt[GDT_ENTRIES];
static struct gdt_ptr   gdtp;

// --- TSS layout -------------------------------------------------------
// We only ever use ss0/esp0 (kernel stack on ring transition). Everything
// else is here because the CPU expects 104 bytes; we leave them zero.
struct tss_entry {
    unsigned int  prev_tss;
    unsigned int  esp0;     // kernel stack pointer used on int from ring 3
    unsigned int  ss0;      // kernel stack segment used on int from ring 3
    unsigned int  esp1, ss1, esp2, ss2;
    unsigned int  cr3;
    unsigned int  eip, eflags;
    unsigned int  eax, ecx, edx, ebx;
    unsigned int  esp, ebp, esi, edi;
    unsigned int  es, cs, ss, ds, fs, gs;
    unsigned int  ldt;
    unsigned short trap, iomap_base;
} __attribute__((packed));

static struct tss_entry tss;

static void gdt_set(int i, unsigned int base, unsigned int limit,
                    unsigned char access, unsigned char gran) {
    gdt[i].base_low    =  base        & 0xFFFF;
    gdt[i].base_mid    = (base >> 16) & 0xFF;
    gdt[i].base_high   = (base >> 24) & 0xFF;
    gdt[i].limit_low   =  limit       & 0xFFFF;
    gdt[i].granularity = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    gdt[i].access      = access;
}

// Reload all six segment registers and the GDTR. Order matters:
//   1. lgdt loads the new GDT, but the segment registers still hold
//      "shadow" descriptors cached from the *old* GDT.
//   2. We can't reload CS with a plain mov — only a far jump (or far
//      call/ret) reloads it. So we lgdt, reload the data segs (which
//      reads new descriptors from the new GDT into their shadows),
//      then ljmp to reload CS.
//
// Both the bootloader GDT and our new GDT happen to put kernel data
// at selector 0x10, so even the in-between window is harmless — but
// doing it in the right order means we don't depend on that.
static void gdt_flush(struct gdt_ptr* p) {
    __asm__ volatile (
        "lgdt (%0)               \n"
        "ljmp $0x08, $1f         \n"   // reload CS via far jump
        "1:                      \n"
        "mov $0x10, %%ax         \n"
        "mov %%ax, %%ds          \n"
        "mov %%ax, %%es          \n"
        "mov %%ax, %%fs          \n"
        "mov %%ax, %%gs          \n"
        "mov %%ax, %%ss          \n"
        : : "r"(p) : "ax", "memory"
    );
}

// Load the task register with our TSS selector. ltr is privileged but
// we're in ring 0 here. Once loaded, the CPU consults tss.esp0 on every
// transition from ring 3 to ring 0.
static void tss_flush(void) {
    __asm__ volatile ("ltr %%ax" : : "a"(GDT_TSS));
}

void gdt_init(void) {
    // Zero the TSS up front, then point ss0 at the kernel data segment.
    // esp0 is patched in by tss_set_kernel_stack() — it must be set
    // before the first IRET to ring 3, but we don't have a kernel stack
    // pointer to put there yet.
    unsigned char* tssp = (unsigned char*)&tss;
    for (unsigned int i = 0; i < sizeof(tss); i++) tssp[i] = 0;
    tss.ss0        = GDT_KDATA;
    tss.esp0       = 0;
    tss.iomap_base = sizeof(tss);   // no I/O permission map

    // Access byte breakdown (high to low bits):
    //   P  | DPL[1:0] | S | Type[3:0]
    //   P=1 (present), S=1 (code/data, not system), DPL=0 or 3.
    //   Type 0xA = code, executable+readable; 0x2 = data, writable.
    // Granularity byte: G=1 (4 KB), D/B=1 (32-bit), L=0, AVL=0 -> 0xC.
    //
    // For the TSS, S=0 and Type=0x9 (available 32-bit TSS); G=0 (limit
    // in bytes, fine for 104 bytes); D/B=0 -> granularity nibble 0x0.

    gdt_set(0, 0, 0,           0x00, 0x00);       // null
    gdt_set(1, 0, 0xFFFFF,     0x9A, 0xCF);       // kernel code, DPL=0
    gdt_set(2, 0, 0xFFFFF,     0x92, 0xCF);       // kernel data, DPL=0
    gdt_set(3, 0, 0xFFFFF,     0xFA, 0xCF);       // user code,   DPL=3
    gdt_set(4, 0, 0xFFFFF,     0xF2, 0xCF);       // user data,   DPL=3
    gdt_set(5, (unsigned int)&tss,
               sizeof(tss) - 1, 0x89, 0x00);      // TSS, available 32-bit

    gdtp.limit = sizeof(gdt) - 1;
    gdtp.base  = (unsigned int)&gdt;

    gdt_flush(&gdtp);
    tss_flush();
}

void tss_set_kernel_stack(unsigned int esp0) {
    tss.esp0 = esp0;
}