#ifndef GDT_H
#define GDT_H

// Kernel-side GDT with ring-3 entries and a TSS so we can drop to userspace.
//
// Layout (selectors):
//   0x00  null
//   0x08  ring-0 code   (kernel)
//   0x10  ring-0 data   (kernel)
//   0x1B  ring-3 code   (user)        index 3, RPL 3
//   0x23  ring-3 data   (user)        index 4, RPL 3
//   0x28  TSS                         index 5
//
// The bootloader's GDT only has the first three; this replaces it. Call
// gdt_init() before any int 0x80 from ring 3 can fire, i.e. before we
// ever IRET to userspace.

#define GDT_KCODE  0x08
#define GDT_KDATA  0x10
#define GDT_UCODE  0x1B
#define GDT_UDATA  0x23
#define GDT_TSS    0x28

void gdt_init(void);

// Update the kernel stack pointer the CPU will load on a privilege-level
// transition into ring 0 (e.g. user-mode int 0x80, page fault, IRQ from
// userspace). Called by the scheduler whenever it switches to a task whose
// kernel stack is different from the previous one's.
void tss_set_kernel_stack(unsigned int esp0);

#endif