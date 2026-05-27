#ifndef PCI_H
#define PCI_H

// PCI bus enumeration.
//
// Every PCI device lives at a (bus, device, function) address — often written
// "B:D.F". Each function has a 256-byte "configuration space": a standard block
// of registers describing what the device is (vendor/device ID, class), where
// its memory/IO lives (the Base Address Registers, BARs), and which interrupt
// line it uses. We never touch the device's real registers here — we just read
// config space to find out what's plugged in.
//
// Config space is reached through two 32-bit I/O ports:
//   0xCF8  CONFIG_ADDRESS  — you write the B:D.F + register offset you want
//   0xCFC  CONFIG_DATA     — you then read/write the dword at that location
// This is "mechanism #1", the only one QEMU and essentially all PCs support.
//
// There are up to 256 buses x 32 devices x 8 functions. A brute-force scan of
// all of them is plenty fast at boot and is what we do — no need for the
// recursive bridge walk a real OS uses.
//
// The kernel has no <stdint.h>; on this i686-elf target char/short/int are
// 8/16/32 bits, so we name our own fixed-width aliases the way the rest of the
// tree relies on those widths.

typedef unsigned char  pci_u8;
typedef unsigned short pci_u16;
typedef unsigned int   pci_u32;

// A single discovered PCI function. Just the fields we care about for now;
// BAR decoding (memory vs IO, sizing) comes when we drive a real NIC.
typedef struct {
    pci_u8  bus;
    pci_u8  device;
    pci_u8  function;
    pci_u16 vendor_id;     // e.g. 0x10EC = Realtek, 0x8086 = Intel
    pci_u16 device_id;     // e.g. 0x8139 = RTL8139, 0x100E = e1000
    pci_u8  class_code;    // high-level class, e.g. 0x02 = network controller
    pci_u8  subclass;      // e.g. 0x00 = ethernet within the network class
    pci_u8  prog_if;       // programming interface
    pci_u8  header_type;   // 0 = normal device, 1 = PCI-to-PCI bridge
    pci_u8  irq_line;      // legacy IRQ this function is wired to (0xFF = none)
    pci_u32 bar[6];        // raw Base Address Registers, undecoded
} pci_device_t;

// --- Raw config-space accessors ---------------------------------------------
// Read/write one register at byte `offset` within the config space of the
// function at (bus, dev, func). `offset` is in bytes but must be dword-aligned
// for the 32-bit variant. The 8/16-bit helpers extract the right bytes.
pci_u32 pci_config_read32(pci_u8 bus, pci_u8 dev, pci_u8 func, pci_u8 offset);
pci_u16 pci_config_read16(pci_u8 bus, pci_u8 dev, pci_u8 func, pci_u8 offset);
pci_u8  pci_config_read8 (pci_u8 bus, pci_u8 dev, pci_u8 func, pci_u8 offset);
void    pci_config_write32(pci_u8 bus, pci_u8 dev, pci_u8 func, pci_u8 offset, pci_u32 value);
void    pci_config_write16(pci_u8 bus, pci_u8 dev, pci_u8 func, pci_u8 offset, pci_u16 value);

// Helpers useful for real device drivers.
void    pci_enable_bus_mastering(const pci_device_t* dev);
pci_u32 pci_bar_addr(const pci_device_t* dev, int bar);
int     pci_bar_is_io(const pci_device_t* dev, int bar);

// --- Enumeration -------------------------------------------------------------
// Scan every bus/device/function once and fill an internal table. Returns the
// number of functions found. Call once at boot after the heap is up.
int pci_init(void);

// How many devices the last scan found, and indexed access to them.
int                  pci_device_count(void);
const pci_device_t*  pci_get_device(int index);

// Find the first device matching a vendor/device ID pair (returns 0 if absent).
// Used later as e.g. pci_find(0x10EC, 0x8139) to locate an RTL8139 NIC.
const pci_device_t*  pci_find(pci_u16 vendor_id, pci_u16 device_id);

// Find the first device of a given class/subclass — e.g. (0x02, 0x00) for any
// ethernet controller, regardless of vendor. Returns 0 if none present.
const pci_device_t*  pci_find_class(pci_u8 class_code, pci_u8 subclass);

// Human-readable class name for the `lspci` shell command. Always returns a
// valid string (falls back to "Unknown").
const char* pci_class_name(pci_u8 class_code, pci_u8 subclass);

#endif