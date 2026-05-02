#include "irq.h"
#include "pic.h"

static irq_handler_fn handlers[16] = { 0 };

void irq_install_handler(int irq, irq_handler_fn handler) {
    if (irq < 0 || irq >= 16) return;
    handlers[irq] = handler;
}

void irq_handler(struct registers* regs) {
    // int_no is the remapped vector (32..47). Convert back to IRQ number.
    int irq = regs->int_no - 32;

    // EOI *before* the handler. This matters for the timer case: if the
    // handler decides to preemptively switch tasks, control won't return
    // here to send the EOI, and the PIC would wedge. Sending first is
    // safe: another IRQ can't preempt us anyway — the CPU IF is clear
    // for the duration of this handler (the stub issued cli on entry).
    pic_send_eoi((unsigned char)irq);

    if (irq >= 0 && irq < 16 && handlers[irq]) {
        handlers[irq](regs);
    }
}