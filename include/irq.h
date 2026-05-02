#ifndef IRQ_H
#define IRQ_H

#include "isr.h"   // struct registers

typedef void (*irq_handler_fn)(struct registers* regs);

// Register a handler for the given IRQ number (0..15).
// Pass 0 to unregister.
void irq_install_handler(int irq, irq_handler_fn handler);

// Called from the common assembly stub (irq_asm.asm).
void irq_handler(struct registers* regs);

#endif