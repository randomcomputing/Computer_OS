#include "serial.h"
#include "io.h"

#define COM1 0x3F8

int serial_init(void) {
    outb(COM1 + 1, 0x00);    /* disable all interrupts                    */
    outb(COM1 + 3, 0x80);    /* enable DLAB (set baud rate divisor)       */
    outb(COM1 + 0, 0x03);    /* divisor low byte: 3 -> 38400 baud         */
    outb(COM1 + 1, 0x00);    /* divisor high byte                         */
    outb(COM1 + 3, 0x03);    /* 8 bits, no parity, one stop bit; clr DLAB */
    outb(COM1 + 2, 0xC7);    /* enable FIFO, clear them, 14-byte thresh.  */
    outb(COM1 + 4, 0x0B);    /* IRQs enabled, RTS/DSR set                 */
    outb(COM1 + 4, 0x1E);    /* loopback mode for self-test               */
    outb(COM1 + 0, 0xAE);    /* send a test byte                          */

    if (inb(COM1 + 0) != 0xAE) {
        return 1;            /* chip is broken / not present              */
    }

    outb(COM1 + 4, 0x0F);    /* back to normal operation                  */
    return 0;
}

static int tx_ready(void) {
    return inb(COM1 + 5) & 0x20;   /* line status bit 5 = THR empty       */
}

void serial_putc(char c) {
    if (c == '\n') {
        while (!tx_ready()) {}
        outb(COM1, '\r');          /* most terminals want CRLF            */
    }
    while (!tx_ready()) {}
    outb(COM1, c);
}

void serial_write(const char *s) {
    while (*s) serial_putc(*s++);
}

int serial_received(void) {
    return inb(COM1 + 5) & 1;      /* line status bit 0 = data ready      */
}

char serial_getc(void) {
    while (!serial_received()) {}
    return inb(COM1);
}