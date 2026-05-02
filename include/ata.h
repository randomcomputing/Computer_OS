#ifndef ATA_H
#define ATA_H

// ATA PIO driver — primary master only.
//
// "PIO" = programmed I/O: the CPU does every byte of the transfer itself
// by reading the data port in a loop. It's slow compared to DMA but it's
// dead simple — no controller setup, no descriptors, no interrupts needed.
// Plenty fast for a hobby kernel reading a few kilobytes.
//
// QEMU's `-hda <file>` exposes the file as a primary-master ATA disk at
// the standard ISA ports 0x1F0..0x1F7 (data + control), which is what we
// drive here.

// Probe the primary-master drive. Returns 1 if a disk is present and
// answered IDENTIFY, 0 if nothing is there. Call this once at boot.
int  ata_init(void);

// Read `count` consecutive 512-byte sectors starting at LBA `lba` into
// `buf`. `buf` must be at least `count * 512` bytes. Returns 0 on success,
// -1 on error (timeout, drive fault, no disk).
//
// LBA = Logical Block Address: sectors are numbered 0, 1, 2, ... from the
// start of the disk. Sector 0 is the boot sector.
int  ata_read(unsigned int lba, unsigned int count, void* buf);

// Write `count` consecutive 512-byte sectors from `buf` into LBA `lba`.
// `buf` must be at least `count * 512` bytes. Returns 0 on success,
// -1 on error. Issues a cache-flush at the end so the data is durable
// even across an unexpected reset.
int  ata_write(unsigned int lba, unsigned int count, const void* buf);

#endif