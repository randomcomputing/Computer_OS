#ifndef IDT_H
#define IDT_H

// Build the 256-entry IDT, fill in the 32 CPU exception handlers,
// and load it with the `lidt` instruction. Call once from kmain.
void idt_init(void);

#endif