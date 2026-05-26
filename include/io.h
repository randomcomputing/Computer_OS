#ifndef IO_H
#define IO_H

// x86 has a separate I/O address space accessed with `in` / `out` instructions.
// These are the standard wrappers. `volatile` keeps GCC from reordering them.

static inline unsigned char inb(unsigned short port) {
    unsigned char val;
    __asm__ volatile ("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline void outb(unsigned short port, unsigned char val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

// A tiny delay used after writing to old ISA hardware (like the PIC).
// Writing to unused port 0x80 takes ~1μs on real hardware and is harmless.
static inline void io_wait(void) {
    outb(0x80, 0);
}

// 16-bit port I/O. ATA's data register is 16 bits wide, so we need these
// to actually move sector contents. The ATA controller hands you exactly
// 256 words per sector through port 0x1F0.

static inline unsigned short inw(unsigned short port) {
    unsigned short val;
    __asm__ volatile ("inw %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline void outw(unsigned short port, unsigned short val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

// Bulk read of 16-bit words from a port into memory. Implemented as
// `rep insw` — one instruction reads `count` words from `port` to `dst`.
// This is the fast path for pulling a sector off the ATA data port.
static inline void insw(unsigned short port, void* dst, unsigned int count) {
    __asm__ volatile ("rep insw"
                      : "+D"(dst), "+c"(count)
                      : "d"(port)
                      : "memory");
}

// Bulk write of 16-bit words from memory to a port. The mirror of insw.
// Used to push a sector's worth of data into the ATA data register.
static inline void outsw(unsigned short port, const void* src, unsigned int count) {
    __asm__ volatile ("rep outsw"
                      : "+S"(src), "+c"(count)
                      : "d"(port));
}

// 32-bit ("long") port I/O. PCI configuration space is read and written one
// 32-bit dword at a time through the config-address (0xCF8) and config-data
// (0xCFC) ports, so the PCI driver needs these. NIC registers later will too.
static inline unsigned int inl(unsigned short port) {
    unsigned int val;
    __asm__ volatile ("inl %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline void outl(unsigned short port, unsigned int val) {
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}

#endif