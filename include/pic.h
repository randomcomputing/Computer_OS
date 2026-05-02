#ifndef PIC_H
#define PIC_H

// After remap: IRQ 0 -> vector 32, IRQ 1 -> 33, ..., IRQ 15 -> 47.
#define PIC1_OFFSET 32
#define PIC2_OFFSET 40

// Remap the master+slave PICs and mask all IRQs by default.
void pic_remap(void);

// Unmask (enable) or mask (disable) a specific IRQ line (0..15).
void pic_enable_irq(unsigned char irq);
void pic_disable_irq(unsigned char irq);

// Send End-Of-Interrupt to the PIC after handling an IRQ.
// Call this from the IRQ handler before iret.
void pic_send_eoi(unsigned char irq);

#endif