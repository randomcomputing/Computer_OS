#include "pic.h"
#include "io.h"

// The 8259 PIC has a master (handles IRQ 0..7) and a slave (IRQ 8..15)
// chained to the master's IRQ 2 line.
#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

#define ICW1_INIT 0x11   // "start initialization + ICW4 will follow"
#define ICW4_8086 0x01   // 8086/88 mode
#define PIC_EOI   0x20   // End-Of-Interrupt command

void pic_remap(void) {
    // Save current masks so we can restore after init.
    unsigned char mask1 = inb(PIC1_DATA);
    unsigned char mask2 = inb(PIC2_DATA);

    // ICW1: start init sequence on both PICs.
    outb(PIC1_CMD, ICW1_INIT); io_wait();
    outb(PIC2_CMD, ICW1_INIT); io_wait();

    // ICW2: set vector offsets.
    outb(PIC1_DATA, PIC1_OFFSET); io_wait();
    outb(PIC2_DATA, PIC2_OFFSET); io_wait();

    // ICW3: tell master that slave is on IRQ2 (bit 2 = 0x04).
    outb(PIC1_DATA, 0x04); io_wait();
    // ICW3: tell slave its cascade identity (2).
    outb(PIC2_DATA, 0x02); io_wait();

    // ICW4: 8086 mode.
    outb(PIC1_DATA, ICW4_8086); io_wait();
    outb(PIC2_DATA, ICW4_8086); io_wait();

    // Restore saved masks. We'll selectively unmask lines we want.
    outb(PIC1_DATA, mask1);
    outb(PIC2_DATA, mask2);

    // Mask everything for a clean start — caller will enable what they want.
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

void pic_enable_irq(unsigned char irq) {
    unsigned short port;
    if (irq < 8) { port = PIC1_DATA; }
    else         { port = PIC2_DATA; irq -= 8; }
    unsigned char mask = inb(port);
    mask &= ~(1 << irq);             // 0 bit = unmasked = enabled
    outb(port, mask);
}

void pic_disable_irq(unsigned char irq) {
    unsigned short port;
    if (irq < 8) { port = PIC1_DATA; }
    else         { port = PIC2_DATA; irq -= 8; }
    unsigned char mask = inb(port);
    mask |= (1 << irq);
    outb(port, mask);
}

void pic_send_eoi(unsigned char irq) {
    if (irq >= 8) outb(PIC2_CMD, PIC_EOI);   // acknowledge slave first
    outb(PIC1_CMD, PIC_EOI);                  // always acknowledge master
}