#include "pci.h"
#include "io.h"
#include "printf.h"

// The two magic ports. Writing a 32-bit "address" to CONFIG_ADDRESS selects
// which dword of which function's config space appears at CONFIG_DATA.
#define PCI_CONFIG_ADDRESS  0xCF8
#define PCI_CONFIG_DATA     0xCFC

// Standard config-space register offsets (byte offsets into the 256-byte space).
#define PCI_OFF_VENDOR        0x00   // u16 vendor, u16 device at 0x02
#define PCI_OFF_CLASS         0x08   // u8 revision, prog_if, subclass, class
#define PCI_OFF_HEADER_TYPE   0x0E   // u8; bit 7 set => multi-function device
#define PCI_OFF_BAR0          0x10   // BAR0..BAR5 at 0x10,0x14,...,0x24
#define PCI_OFF_IRQ_LINE      0x3C   // u8 interrupt line

#define PCI_VENDOR_NONE       0xFFFF // reads as all-ones when nothing is there

// A bus can have at most 256 devices, but in practice QEMU presents a handful.
// A fixed table avoids needing the heap and keeps enumeration self-contained.
#define PCI_MAX_DEVICES 64

static pci_device_t g_devices[PCI_MAX_DEVICES];
static int          g_count = 0;

// Build the CONFIG_ADDRESS dword: bit 31 = enable, then bus/device/function,
// then the dword-aligned register offset. The low two bits of offset are
// forced to zero because config reads are always 32-bit aligned.
static pci_u32 pci_address(pci_u8 bus, pci_u8 dev, pci_u8 func, pci_u8 offset) {
    return (pci_u32)((1u << 31)
                   | ((pci_u32)bus  << 16)
                   | ((pci_u32)dev  << 11)
                   | ((pci_u32)func << 8)
                   | ((pci_u32)offset & 0xFC));
}

pci_u32 pci_config_read32(pci_u8 bus, pci_u8 dev, pci_u8 func, pci_u8 offset) {
    outl(PCI_CONFIG_ADDRESS, pci_address(bus, dev, func, offset));
    return inl(PCI_CONFIG_DATA);
}

pci_u16 pci_config_read16(pci_u8 bus, pci_u8 dev, pci_u8 func, pci_u8 offset) {
    // Read the aligned dword, then pick the right 16-bit half. (offset & 2)*8
    // is either 0 or 16 — the bit position of the wanted word.
    pci_u32 dword = pci_config_read32(bus, dev, func, offset & 0xFC);
    return (pci_u16)((dword >> ((offset & 2) * 8)) & 0xFFFF);
}

pci_u8 pci_config_read8(pci_u8 bus, pci_u8 dev, pci_u8 func, pci_u8 offset) {
    pci_u32 dword = pci_config_read32(bus, dev, func, offset & 0xFC);
    return (pci_u8)((dword >> ((offset & 3) * 8)) & 0xFF);
}

void pci_config_write32(pci_u8 bus, pci_u8 dev, pci_u8 func, pci_u8 offset, pci_u32 value) {
    outl(PCI_CONFIG_ADDRESS, pci_address(bus, dev, func, offset));
    outl(PCI_CONFIG_DATA, value);
}

void pci_config_write16(pci_u8 bus, pci_u8 dev, pci_u8 func, pci_u8 offset, pci_u16 value) {
    pci_u32 old = pci_config_read32(bus, dev, func, offset & 0xFC);
    int shift = (offset & 2) * 8;

    old &= ~((pci_u32)0xFFFF << shift);
    old |= ((pci_u32)value << shift);

    pci_config_write32(bus, dev, func, offset & 0xFC, old);
}

// Read one function's identifying fields into the table. Caller has already
// confirmed the function exists (vendor != 0xFFFF).
static void pci_record(pci_u8 bus, pci_u8 dev, pci_u8 func) {
    if (g_count >= PCI_MAX_DEVICES) return;

    pci_device_t* d = &g_devices[g_count];
    d->bus = bus; d->device = dev; d->function = func;

    pci_u32 ids = pci_config_read32(bus, dev, func, PCI_OFF_VENDOR);
    d->vendor_id = (pci_u16)(ids & 0xFFFF);
    d->device_id = (pci_u16)(ids >> 16);

    pci_u32 cls = pci_config_read32(bus, dev, func, PCI_OFF_CLASS);
    d->prog_if    = (pci_u8)((cls >> 8)  & 0xFF);
    d->subclass   = (pci_u8)((cls >> 16) & 0xFF);
    d->class_code = (pci_u8)((cls >> 24) & 0xFF);

    d->header_type = pci_config_read8(bus, dev, func, PCI_OFF_HEADER_TYPE);
    d->irq_line    = pci_config_read8(bus, dev, func, PCI_OFF_IRQ_LINE);

    for (int b = 0; b < 6; b++)
        d->bar[b] = pci_config_read32(bus, dev, func, (pci_u8)(PCI_OFF_BAR0 + b * 4));

    g_count++;
}

// Probe a single (bus, dev) slot. Function 0 must exist for the slot to be
// populated; if its header-type bit 7 is set the device is multi-function and
// we probe functions 1..7 as well.
static void pci_scan_slot(pci_u8 bus, pci_u8 dev) {
    if (pci_config_read16(bus, dev, 0, PCI_OFF_VENDOR) == PCI_VENDOR_NONE)
        return;

    pci_record(bus, dev, 0);

    pci_u8 header = pci_config_read8(bus, dev, 0, PCI_OFF_HEADER_TYPE);
    if (header & 0x80) {
        for (pci_u8 func = 1; func < 8; func++) {
            if (pci_config_read16(bus, dev, func, PCI_OFF_VENDOR) != PCI_VENDOR_NONE)
                pci_record(bus, dev, func);
        }
    }
}

int pci_init(void) {
    g_count = 0;
    // Brute force: every bus, every device. QEMU's PIIX3 host bridge answers on
    // bus 0; we sweep all 256 buses anyway since it's cheap and correct.
    for (int bus = 0; bus < 256; bus++)
        for (int dev = 0; dev < 32; dev++)
            pci_scan_slot((pci_u8)bus, (pci_u8)dev);
    return g_count;
}

int pci_device_count(void) { return g_count; }

const pci_device_t* pci_get_device(int index) {
    if (index < 0 || index >= g_count) return 0;
    return &g_devices[index];
}

const pci_device_t* pci_find(pci_u16 vendor_id, pci_u16 device_id) {
    for (int i = 0; i < g_count; i++)
        if (g_devices[i].vendor_id == vendor_id && g_devices[i].device_id == device_id)
            return &g_devices[i];
    return 0;
}

const pci_device_t* pci_find_class(pci_u8 class_code, pci_u8 subclass) {
    for (int i = 0; i < g_count; i++)
        if (g_devices[i].class_code == class_code && g_devices[i].subclass == subclass)
            return &g_devices[i];
    return 0;
}


#define PCI_COMMAND_REG        0x04
#define PCI_COMMAND_IO_SPACE   0x1
#define PCI_COMMAND_MEM_SPACE  0x2
#define PCI_COMMAND_BUS_MASTER 0x4

void pci_enable_bus_mastering(const pci_device_t* dev) {
    pci_u16 cmd = pci_config_read16(dev->bus,
                                    dev->device,
                                    dev->function,
                                    PCI_COMMAND_REG);

    cmd |= PCI_COMMAND_IO_SPACE;
    cmd |= PCI_COMMAND_MEM_SPACE;
    cmd |= PCI_COMMAND_BUS_MASTER;

    pci_config_write16(dev->bus,
                       dev->device,
                       dev->function,
                       PCI_COMMAND_REG,
                       cmd);
}

pci_u32 pci_bar_addr(const pci_device_t* dev, int bar) {
    if (bar < 0 || bar >= 6) return 0;

    pci_u32 raw = dev->bar[bar];

    if (raw & 1) {
        // I/O BAR: low 2 bits are flags.
        return raw & 0xFFFFFFFCu;
    }

    // Memory BAR: low 4 bits are flags.
    return raw & 0xFFFFFFF0u;
}

int pci_bar_is_io(const pci_device_t* dev, int bar) {
    if (bar < 0 || bar >= 6) return 0;
    return (dev->bar[bar] & 1) != 0;
}

// Just the handful of classes we're likely to see under QEMU. The networking
// work to come cares about class 0x02; the rest is so `lspci` reads nicely.
const char* pci_class_name(pci_u8 class_code, pci_u8 subclass) {
    switch (class_code) {
        case 0x00: return "Unclassified";
        case 0x01:
            switch (subclass) {
                case 0x01: return "IDE controller";
                case 0x06: return "SATA controller";
                default:   return "Mass storage controller";
            }
        case 0x02: return "Network controller";   // 0x02/0x00 = ethernet
        case 0x03: return "Display controller";
        case 0x04: return "Multimedia controller";
        case 0x06:
            switch (subclass) {
                case 0x00: return "Host bridge";
                case 0x01: return "ISA bridge";
                case 0x04: return "PCI-to-PCI bridge";
                default:   return "Bridge device";
            }
        case 0x0C: return "Serial bus controller";
        default:   return "Unknown";
    }
}