#ifndef GDT_H
#define GDT_H

#include <stdint.h>

/*
 * GDT selectors (64-bit flat model).
 *
 * Index 0: null
 * Index 1: kernel code  (0x08) — 64-bit, DPL=0
 * Index 2: kernel data  (0x10) — 64-bit, DPL=0
 * Index 3: user data    (0x18) — 64-bit, DPL=3  (must come before user code for SYSRET)
 * Index 4: user code    (0x20) — 64-bit, DPL=3
 * Index 5: TSS low      (0x28) — 16-byte system descriptor
 * Index 6: TSS high     (0x30) — upper 8 bytes of 64-bit TSS descriptor
 */
#define GDT_NULL    0x00
#define GDT_KCODE   0x08
#define GDT_KDATA   0x10
#define GDT_UDATA   0x18
#define GDT_UCODE   0x20
#define GDT_TSS     0x28

/* User selectors with RPL=3 set (for iretq frames). */
#define GDT_UCODE_RPL3  (GDT_UCODE | 3)   /* 0x23 */
#define GDT_UDATA_RPL3  (GDT_UDATA | 3)   /* 0x1B */

void     gdt_init(void);
void     tss_set_kernel_stack(uint64_t rsp0);

#endif