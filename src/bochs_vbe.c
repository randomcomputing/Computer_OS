#include "bochs_vbe.h"
#include "io.h"
#include "pci.h"
#include "vmm.h"
#include "printf.h"

// DISPI I/O ports.
#define VBE_DISPI_IOPORT_INDEX  0x01CE
#define VBE_DISPI_IOPORT_DATA   0x01CF

// DISPI register indices.
#define VBE_DISPI_INDEX_ID      0x0
#define VBE_DISPI_INDEX_XRES    0x1
#define VBE_DISPI_INDEX_YRES    0x2
#define VBE_DISPI_INDEX_BPP     0x3
#define VBE_DISPI_INDEX_ENABLE  0x4

// ENABLE register flags.
#define VBE_DISPI_DISABLED      0x00
#define VBE_DISPI_ENABLED       0x01
#define VBE_DISPI_LFB_ENABLED   0x40

// Minimum ID that indicates the adapter is present at all.
#define VBE_DISPI_ID0           0xB0C0

// The QEMU stdvga PCI device whose BAR0 is the linear framebuffer.
#define VBE_PCI_VENDOR          0x1234
#define VBE_PCI_DEVICE          0x1111

// Kernel virtual window for the framebuffer. Chosen high in the kernel half,
// clear of the kernel image (~0xC0000000 region), the heap (0xC1000000), and
// user space (< 0xC0000000). 1024x768x32 needs 3 MB = 768 pages, which fits
// in a single 4 MB page-table region starting here.
#define VBE_FB_VIRT_BASE        0xE0000000

static bochs_vbe_mode_t g_mode = { 0, 0, 0, 0, 0, 0, 0 };

static void dispi_write(unsigned short index, unsigned short value) {
    outw(VBE_DISPI_IOPORT_INDEX, index);
    outw(VBE_DISPI_IOPORT_DATA, value);
}

static unsigned short dispi_read(unsigned short index) {
    outw(VBE_DISPI_IOPORT_INDEX, index);
    return inw(VBE_DISPI_IOPORT_DATA);
}

int bochs_vbe_available(void) {
    unsigned short id = dispi_read(VBE_DISPI_INDEX_ID);
    return (id >= VBE_DISPI_ID0 && id <= 0xB0CF);
}

int bochs_vbe_set_mode(unsigned short width, unsigned short height,
                       bochs_vbe_mode_t* out) {
    if (out) out->ok = 0;
    g_mode.ok = 0;

    if (!bochs_vbe_available()) {
        return 0;   // not a Bochs/QEMU DISPI adapter — caller keeps text mode
    }

    // Find the framebuffer's physical address from the display device's BAR0.
    // The PCI scan must have run already (pci_init in kmain). BAR0 for the
    // QEMU stdvga is a 32-bit memory BAR; the low 4 bits are flags, so mask
    // them off to get the base address.
    const pci_device_t* gpu = pci_find(VBE_PCI_VENDOR, VBE_PCI_DEVICE);
    if (!gpu) {
        return 0;
    }
    unsigned int fb_phys = gpu->bar[0] & 0xFFFFFFF0u;
    if (fb_phys == 0) {
        return 0;
    }

    // Program the mode: must disable before changing XRES/YRES/BPP, then
    // re-enable with the linear-framebuffer flag set.
    dispi_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
    dispi_write(VBE_DISPI_INDEX_XRES, width);
    dispi_write(VBE_DISPI_INDEX_YRES, height);
    dispi_write(VBE_DISPI_INDEX_BPP, 32);
    dispi_write(VBE_DISPI_INDEX_ENABLE,
                VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);

    // Verify the adapter accepted our resolution (it clamps/ignores illegal
    // values rather than failing loudly). If it didn't take, disable and bail
    // so the caller can fall back to text mode.
    if (dispi_read(VBE_DISPI_INDEX_XRES) != width ||
        dispi_read(VBE_DISPI_INDEX_YRES) != height) {
        dispi_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
        return 0;
    }

    unsigned int pitch = (unsigned int)width * 4u;      // 32bpp, no padding
    unsigned int bytes = pitch * (unsigned int)height;  // total framebuffer
    unsigned int pages = (bytes + 0xFFF) >> 12;         // round up to pages

    // Map the framebuffer into kernel virtual space, one page at a time.
    // map writes its page tables from PMM low memory (identity-accessible),
    // so mapping a high physical BAR like 0xFD000000 is fine.
    for (unsigned int i = 0; i < pages; i++) {
        unsigned int off = i << 12;
        if (!vmm_map(VBE_FB_VIRT_BASE + off, fb_phys + off,
                     VMM_PRESENT | VMM_WRITE)) {
            // Out of frames for page tables: unwind and fall back to text.
            for (unsigned int j = 0; j < i; j++)
                vmm_unmap(VBE_FB_VIRT_BASE + (j << 12));
            dispi_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
            return 0;
        }
    }

    g_mode.phys   = fb_phys;
    g_mode.virt   = VBE_FB_VIRT_BASE;
    g_mode.pitch  = pitch;
    g_mode.width  = width;
    g_mode.height = height;
    g_mode.bpp    = 32;
    g_mode.ok     = 1;

    if (out) *out = g_mode;
    return 1;
}

const bochs_vbe_mode_t* bochs_vbe_current(void) {
    return &g_mode;
}