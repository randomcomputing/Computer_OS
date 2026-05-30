#ifndef BOCHS_VBE_H
#define BOCHS_VBE_H

#include "stdint.h"

// Bochs / QEMU "DISPI" VBE interface.
//
// QEMU's standard VGA device (PCI 1234:1111 — the one the PCI scan reports as
// the display controller) implements the Bochs Graphics Adapter register set.
// Unlike real VESA BIOS calls, this is driven entirely from protected mode
// through two I/O ports, so the kernel can set a high-resolution linear
// framebuffer mode without any real-mode/BIOS gymnastics.
//
// Protocol (see OSDev "Bochs VBE Extensions"):
//   1. write VBE_DISPI_DISABLED to the ENABLE register
//   2. write desired XRES, YRES, BPP
//   3. write VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED to ENABLE
// The linear framebuffer then lives at the physical address in BAR0 of the
// 1234:1111 PCI device — which our pci.c already captured during enumeration.

// Result of bringing up a graphics mode. phys/virt are the framebuffer base
// (physical, and where we mapped it in kernel virtual space). One pixel is a
// 32-bit XRGB value; rows are `pitch` bytes apart (pitch == width*4 here).
typedef struct {
    unsigned int  phys;     // framebuffer physical base (from PCI BAR0)
    uint64_t      virt;     // where vmm mapped it for the kernel to write
    unsigned int  pitch;    // bytes per scanline
    unsigned short width;
    unsigned short height;
    unsigned char  bpp;
    unsigned char  ok;      // 1 = framebuffer ready, 0 = unavailable
} bochs_vbe_mode_t;

// True if the Bochs/QEMU DISPI adapter is present (ID register >= 0xB0C0).
int bochs_vbe_available(void);

// Set `width` x `height` x 32bpp linear-framebuffer mode, locate the
// framebuffer via PCI BAR0, and map it into kernel virtual memory. On
// success fills *out and returns 1; on any failure returns 0 and leaves the
// adapter untouched so the caller can stay in VGA text mode.
int bochs_vbe_set_mode(unsigned short width, unsigned short height,
                       bochs_vbe_mode_t* out);

// The mode set by the last successful bochs_vbe_set_mode (ok=0 if none).
const bochs_vbe_mode_t* bochs_vbe_current(void);

// Populate the mode struct from a Limine framebuffer (bypasses DISPI hardware).
void bochs_vbe_set_from_limine(void* fb_addr, unsigned int width,
                               unsigned int height, unsigned int pitch,
                               unsigned char bpp);

#endif