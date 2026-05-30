#include "reboot.h"
#include "console.h"
#include "acpi.h"
#include "io.h"

#define VBE_DISPI_IOPORT_INDEX  0x01CE
#define VBE_DISPI_IOPORT_DATA   0x01CF
#define VBE_DISPI_INDEX_ENABLE  0x04
#define VBE_DISPI_DISABLED      0x00

static void vbe_disable(void) {
    /* Just disable -- do not zero XRES/YRES/BPP first.
       Zeroing resolution while enabled collapses the display to a
       broken state that survives the reset. Simply clearing the
       ENABLE bit is enough to return the adapter to VGA text mode. */
    outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_ENABLE);
    outw(VBE_DISPI_IOPORT_DATA,  VBE_DISPI_DISABLED);
    /* No spin or hlt needed: the ACPI reset via FADT fully reinitialises
       the VBE hardware, so as long as the disable write reaches the
       adapter before the reset fires we are fine. Port I/O is
       synchronous in QEMU TCG -- the write completes before the next
       instruction, so the reset sees a clean adapter state. */
}

void shutdown(void) {
    vbe_disable();
    acpi_poweroff();
    con_puts("Shutdown not supported on this hardware.\n");
    con_puts("You can safely power off the machine.\n");
}

void reboot(void) {
    con_puts("Rebooting...\n");
    vbe_disable();
    __asm__ volatile ("cli");
    acpi_reboot();
}