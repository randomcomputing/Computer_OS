#include "ata.h"
#include "io.h"
#include "printf.h"

// Primary ATA bus I/O ports. These are fixed by the PC architecture —
// every IDE/ATA controller answers here for the primary channel.
#define ATA_DATA        0x1F0   // 16-bit data register (read/write sector contents)
#define ATA_ERR_FEAT    0x1F1   // error (read) / features (write)
#define ATA_SECCNT      0x1F2   // sector count
#define ATA_LBA_LO      0x1F3   // LBA bits  0..7
#define ATA_LBA_MID     0x1F4   // LBA bits  8..15
#define ATA_LBA_HI      0x1F5   // LBA bits 16..23
#define ATA_DRIVE       0x1F6   // drive select + LBA bits 24..27 + LBA mode flag
#define ATA_CMD         0x1F7   // command (write); same port as status (read)
#define ATA_STATUS      0x1F7   // alias for clarity at read sites
#define ATA_CTRL        0x3F6   // control register (alt status, reset, nIEN)

// Status-register bits (read from 0x1F7).
#define ATA_SR_BSY      0x80    // drive is busy executing a command
#define ATA_SR_DRDY     0x40    // drive is ready to accept commands
#define ATA_SR_DF       0x20    // drive fault
#define ATA_SR_DRQ      0x08    // drive has data ready (or wants data)
#define ATA_SR_ERR      0x01    // command failed; check ATA_ERR_FEAT

// Commands.
#define ATA_CMD_READ_SECTORS    0x20
#define ATA_CMD_WRITE_SECTORS   0x30
#define ATA_CMD_FLUSH_CACHE     0xE7
#define ATA_CMD_IDENTIFY        0xEC

static int present = 0;     // set by ata_init() if a drive answered

// Read alt-status four times. Each read of the alt-status register
// (0x3F6) is documented to take ~100 ns on real hardware, giving the
// drive the 400 ns settle time it needs after a command write before
// status bits are guaranteed valid. Reading alt-status (vs the regular
// status) doesn't acknowledge interrupts, which is what we want here.
static void ata_400ns_delay(void) {
    inb(ATA_CTRL); inb(ATA_CTRL); inb(ATA_CTRL); inb(ATA_CTRL);
}

// Spin until the drive isn't busy, then return its status. Bounded by
// a tick counter so a missing drive doesn't hang the kernel forever.
static int ata_wait_not_busy(unsigned char* out_status) {
    for (unsigned int i = 0; i < 1000000; i++) {
        unsigned char s = inb(ATA_STATUS);
        if (!(s & ATA_SR_BSY)) {
            if (out_status) *out_status = s;
            return 0;
        }
    }
    return -1;
}

// Spin until DRQ is set (drive has data for us) and BSY is clear.
// If ERR or DF comes up we bail out — the command failed.
static int ata_wait_drq(void) {
    for (unsigned int i = 0; i < 1000000; i++) {
        unsigned char s = inb(ATA_STATUS);
        if (s & (ATA_SR_ERR | ATA_SR_DF)) return -1;
        if (!(s & ATA_SR_BSY) && (s & ATA_SR_DRQ)) return 0;
    }
    return -1;
}

// Issue a soft reset on the primary channel. Setting bit 2 (SRST) of
// the control register holds both drives in reset; clearing it lets
// them come back up. Some emulators (and a lot of real BIOSes) leave
// the channel in a weird state, and this gives us a known starting
// point before we probe.
static void ata_soft_reset(void) {
    outb(ATA_CTRL, 0x04);   // SRST = 1, nIEN = 0
    ata_400ns_delay();
    outb(ATA_CTRL, 0x02);   // SRST = 0, nIEN = 1 (mask IRQs while we poll)
    ata_400ns_delay();
}

int ata_init(void) {
    // Start from a known state. Without the reset, a half-initialized
    // controller can leave BSY stuck high and we'd time out below.
    ata_soft_reset();

    // Floating-bus check. If nothing is wired to the primary channel,
    // reads from the status port return 0xFF (all pull-ups, no driver
    // pulling the lines low). QEMU does this faithfully when there's
    // no -drive on index=0. The original code only checked for 0x00,
    // which is why a missing disk slipped through and caused the
    // confusing "no filesystem" error downstream.
    unsigned char floating = inb(ATA_STATUS);
    if (floating == 0xFF) {
        printf("ata: primary channel floating (no drive attached)\n");
        present = 0;
        return 0;
    }

    // Select primary master. 0xA0 = drive 0, CHS mode (we'll switch to
    // LBA per-command). Wait the obligatory 400 ns for the selection
    // to take effect — the drive needs that long before status reads
    // reflect the newly-selected device.
    outb(ATA_DRIVE, 0xA0);
    ata_400ns_delay();

    // IDENTIFY: ask the drive to describe itself. We don't actually need
    // its model string or geometry — we just want to know it's there.
    outb(ATA_SECCNT, 0);
    outb(ATA_LBA_LO, 0);
    outb(ATA_LBA_MID, 0);
    outb(ATA_LBA_HI, 0);
    outb(ATA_CMD, ATA_CMD_IDENTIFY);

    unsigned char status = inb(ATA_STATUS);
    if (status == 0 || status == 0xFF) {
        // 0x00 = controller saw no drive. 0xFF = floating bus.
        // Either way, there's nothing to talk to.
        printf("ata: IDENTIFY status=0x%x, no drive\n", status);
        present = 0;
        return 0;
    }

    if (ata_wait_not_busy(&status) < 0) {
        printf("ata: drive stuck BUSY after IDENTIFY\n");
        present = 0;
        return 0;
    }

    // ATAPI/SATA devices put non-zero values in LBA_MID/HI here to signal
    // "I'm not a plain ATA disk." Plain ATA leaves them zero. CD-ROMs
    // (ATAPI) put 0x14/0xEB; SATA bridges put 0x3C/0xC3.
    unsigned char mid = inb(ATA_LBA_MID);
    unsigned char hi  = inb(ATA_LBA_HI);
    if (mid != 0 || hi != 0) {
        printf("ata: non-ATA device on primary master (sig=%x:%x)\n", hi, mid);
        present = 0;
        return 0;
    }

    if (ata_wait_drq() < 0) {
        printf("ata: IDENTIFY never produced data\n");
        present = 0;
        return 0;
    }

    // Drain the 256-word IDENTIFY result. We discard it, but the drive
    // won't accept new commands until we've read it all.
    unsigned short scratch[256];
    insw(ATA_DATA, scratch, 256);

    present = 1;
    return 1;
}

int ata_read(unsigned int lba, unsigned int count, void* buf) {
    if (!present) return -1;
    if (count == 0) return 0;

    // 28-bit LBA caps at 2^28 sectors = 128 GB. Plenty for us.
    if ((lba + count) >= (1u << 28)) return -1;

    unsigned char* out = (unsigned char*)buf;

    // One command per sector. The READ SECTORS command takes a sector
    // count in a single byte where 0 means 256, but looping keeps the
    // logic obvious — for 1 KB / 4 KB reads this is fine.
    for (unsigned int i = 0; i < count; i++) {
        unsigned int this_lba = lba + i;

        if (ata_wait_not_busy(0) < 0) return -1;

        // Drive select + top 4 LBA bits + LBA mode flag (bit 6).
        // 0xE0 = 1110_0000 = LBA, drive 0, plus the high nibble of LBA.
        outb(ATA_DRIVE, 0xE0 | ((this_lba >> 24) & 0x0F));
        // 400 ns settle after switching the drive-select register —
        // matters on real hardware even if QEMU doesn't care.
        ata_400ns_delay();

        outb(ATA_ERR_FEAT, 0);
        outb(ATA_SECCNT, 1);
        outb(ATA_LBA_LO,  (unsigned char)(this_lba       & 0xFF));
        outb(ATA_LBA_MID, (unsigned char)((this_lba >> 8)  & 0xFF));
        outb(ATA_LBA_HI,  (unsigned char)((this_lba >> 16) & 0xFF));
        outb(ATA_CMD, ATA_CMD_READ_SECTORS);

        if (ata_wait_drq() < 0) return -1;

        // 512 bytes = 256 16-bit words from the data port.
        insw(ATA_DATA, out, 256);
        out += 512;
    }

    return 0;
}

int ata_write(unsigned int lba, unsigned int count, const void* buf) {
    if (!present) return -1;
    if (count == 0) return 0;
    if ((lba + count) >= (1u << 28)) return -1;

    const unsigned char* in = (const unsigned char*)buf;

    // One sector per command, mirroring ata_read. The handshake here is
    // subtly different: after we issue WRITE SECTORS and the drive sets
    // DRQ, *we* push 256 words into the data port; then the drive sets
    // BSY while it commits to media; then we must wait for BSY to clear
    // (and check ERR) before we can issue the next command.
    for (unsigned int i = 0; i < count; i++) {
        unsigned int this_lba = lba + i;

        if (ata_wait_not_busy(0) < 0) return -1;

        outb(ATA_DRIVE, 0xE0 | ((this_lba >> 24) & 0x0F));
        ata_400ns_delay();

        outb(ATA_ERR_FEAT, 0);
        outb(ATA_SECCNT, 1);
        outb(ATA_LBA_LO,  (unsigned char)(this_lba       & 0xFF));
        outb(ATA_LBA_MID, (unsigned char)((this_lba >> 8)  & 0xFF));
        outb(ATA_LBA_HI,  (unsigned char)((this_lba >> 16) & 0xFF));
        outb(ATA_CMD, ATA_CMD_WRITE_SECTORS);

        if (ata_wait_drq() < 0) return -1;

        // Push 512 bytes (256 words) into the data register.
        outsw(ATA_DATA, in, 256);
        in += 512;

        // After writing, the drive sets BSY while it commits to its
        // media. Wait it out and check for an error.
        unsigned char st;
        if (ata_wait_not_busy(&st) < 0) return -1;
        if (st & (ATA_SR_ERR | ATA_SR_DF)) return -1;
    }

    // Tell the drive to push any internal cache to media. Cheap insurance
    // — without this, an unexpected reset can lose recently-written data.
    if (ata_wait_not_busy(0) < 0) return -1;
    outb(ATA_CMD, ATA_CMD_FLUSH_CACHE);
    if (ata_wait_not_busy(0) < 0) return -1;

    return 0;
}