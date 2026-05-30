#include "gdt.h"
#include <stdint.h>

/*
 * 64-bit GDT.
 *
 * In long mode, most of the base/limit fields are ignored for code and data
 * segments — the CPU treats everything as flat 64-bit. The access byte and
 * the L (long-mode) flag in the granularity byte are what matter.
 *
 * The TSS is an exception: it needs a real base address (the address of our
 * tss_entry struct) and it uses a 16-byte descriptor (two GDT slots).
 */

/* Standard 8-byte GDT entry. */
struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;   /* limit_high[3:0] | flags[7:4] */
    uint8_t  base_high;
} __attribute__((packed));

/* Upper 8 bytes of the 16-byte 64-bit TSS descriptor. */
struct gdt_entry_high {
    uint32_t base_upper;    /* bits 63:32 of TSS base */
    uint32_t reserved;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/*
 * 64-bit TSS.  We only use rsp0 (kernel stack on ring-3 → ring-0 transition)
 * and iomap_base. Everything else stays zero.
 */
struct tss_entry {
    uint32_t reserved0;
    uint64_t rsp0;          /* kernel stack for ring-3 → ring-0 */
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];        /* interrupt stack table — leave zero */
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

/* 7 entries: null, kcode, kdata, udata, ucode, tss-low, tss-high */
#define GDT_ENTRIES 7
static struct gdt_entry     gdt[GDT_ENTRIES];
static struct gdt_ptr       gdtp;
static struct tss_entry     tss;

static void gdt_set(int i, uint32_t base, uint32_t limit,
                    uint8_t access, uint8_t gran) {
    gdt[i].base_low    =  base        & 0xFFFF;
    gdt[i].base_mid    = (base >> 16) & 0xFF;
    gdt[i].base_high   = (base >> 24) & 0xFF;
    gdt[i].limit_low   =  limit       & 0xFFFF;
    gdt[i].granularity = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    gdt[i].access      = access;
}

/*
 * The 64-bit TSS descriptor is 16 bytes.  The lower 8 bytes look like a
 * normal GDT entry (with type=0x9 = available 64-bit TSS), but the upper
 * 8 bytes hold bits 63:32 of the base address plus a reserved dword.
 * We alias slots 5 and 6 to a struct gdt_entry_high to write the upper half.
 */
static void gdt_set_tss(uint64_t base, uint32_t limit) {
    /* Lower 8 bytes: normal descriptor, type 0x9, DPL=0, present. */
    gdt[5].limit_low   =  limit       & 0xFFFF;
    gdt[5].base_low    =  base        & 0xFFFF;
    gdt[5].base_mid    = (base >> 16) & 0xFF;
    gdt[5].access      = 0x89;         /* P=1, DPL=0, Type=9 (64-bit TSS avail) */
    gdt[5].granularity = ((limit >> 16) & 0x0F);
    gdt[5].base_high   = (base >> 24) & 0xFF;

    /* Upper 8 bytes: bits 63:32 of base + reserved. */
    struct gdt_entry_high* hi = (struct gdt_entry_high*)&gdt[6];
    hi->base_upper = (uint32_t)(base >> 32);
    hi->reserved   = 0;
}

/*
 * Reload CS via a far return (lretq) and reload the other segment registers.
 * We can't ljmp in 64-bit inline asm easily, so we push the new CS and the
 * return address, then lretq — that's the standard long-mode way to reload CS.
 *
 * In 64-bit mode DS/ES/FS/GS are mostly vestigial (base=0, ignored by the
 * CPU for most purposes), but we load GDT_KDATA into them anyway for
 * correctness and to keep the null selector out of them.
 */
static void gdt_flush(struct gdt_ptr* p) {
    __asm__ volatile (
        "lgdt (%0)              \n"
        /* Push new CS and the address of the label '1f', then lretq. */
        "lea  1f(%%rip), %%rax  \n"
        "push %1                \n"   /* new CS = GDT_KCODE */
        "push %%rax             \n"
        "lretq                  \n"
        "1:                     \n"
        "mov  %2, %%ax          \n"
        "mov  %%ax, %%ds        \n"
        "mov  %%ax, %%es        \n"
        "mov  %%ax, %%fs        \n"
        "mov  %%ax, %%gs        \n"
        "mov  %%ax, %%ss        \n"
        :
        : "r"(p), "i"((uint64_t)GDT_KCODE), "i"((uint64_t)GDT_KDATA)
        : "rax", "memory"
    );
}

static void tss_flush(void) {
    __asm__ volatile ("ltr %%ax" : : "a"((uint16_t)GDT_TSS));
}

void gdt_init(void) {
    /* Zero TSS. */
    uint8_t* p = (uint8_t*)&tss;
    for (uint32_t i = 0; i < sizeof(tss); i++) p[i] = 0;
    tss.iomap_base = sizeof(tss);   /* no I/O permission map */

    /*
     * Access byte for 64-bit code/data:
     *   P=1, S=1 (non-system), DPL=0 or 3
     *   Code: Type=0xA (exec+read), L=1 (64-bit), D=0  -> gran=0xA0
     *   Data: Type=0x2 (read/write)                    -> gran=0x00
     *         (limit/base ignored in 64-bit flat mode)
     *
     * For code segments the L bit (bit 5 of the granularity byte) MUST be 1.
     * For data segments L=0, but the CPU doesn't care in 64-bit mode.
     */
    gdt_set(0, 0, 0,       0x00, 0x00);   /* null */
    gdt_set(1, 0, 0xFFFFF, 0x9A, 0xA0);   /* kernel code, DPL=0, L=1 */
    gdt_set(2, 0, 0xFFFFF, 0x92, 0x00);   /* kernel data, DPL=0 */
    gdt_set(3, 0, 0xFFFFF, 0xF2, 0x00);   /* user data,   DPL=3 */
    gdt_set(4, 0, 0xFFFFF, 0xFA, 0xA0);   /* user code,   DPL=3, L=1 */
    gdt_set_tss((uint64_t)&tss, sizeof(tss) - 1);

    gdtp.limit = sizeof(gdt) - 1;
    gdtp.base  = (uint64_t)&gdt;

    gdt_flush(&gdtp);
    tss_flush();
}

void tss_set_kernel_stack(uint64_t rsp0) {
    tss.rsp0 = rsp0;
}