#include "ata.h"
#include "io.h"
#include "printf.h"

/*
 * Simple polling ATA PIO driver for the primary IDE channel.
 *
 * Important fix:
 *   The old driver only checked primary master.
 *
 * But when QEMU boots from a CD-ROM/ISO and you also attach fatdisk.img,
 * the FAT disk may not always be primary master depending on the QEMU
 * command line/device ordering. Sometimes the CD-ROM/ATAPI device sits in
 * the place your code probes first.
 *
 * This version scans:
 *   - primary master
 *   - primary slave
 *
 * It skips ATAPI devices and chooses the first real ATA disk.
 */

// Primary ATA channel I/O ports.
#define ATA_DATA        0x1F0
#define ATA_ERR_FEAT    0x1F1
#define ATA_SECCNT      0x1F2
#define ATA_LBA_LO      0x1F3
#define ATA_LBA_MID     0x1F4
#define ATA_LBA_HI      0x1F5
#define ATA_DRIVE       0x1F6
#define ATA_CMD         0x1F7
#define ATA_STATUS      0x1F7
#define ATA_CTRL        0x3F6

// Status bits.
#define ATA_SR_BSY      0x80
#define ATA_SR_DRDY     0x40
#define ATA_SR_DF       0x20
#define ATA_SR_DRQ      0x08
#define ATA_SR_ERR      0x01

// Commands.
#define ATA_CMD_READ_SECTORS    0x20
#define ATA_CMD_WRITE_SECTORS   0x30
#define ATA_CMD_FLUSH_CACHE     0xE7
#define ATA_CMD_IDENTIFY        0xEC

// Drive select values.
#define ATA_MASTER_CHS  0xA0
#define ATA_SLAVE_CHS   0xB0
#define ATA_MASTER_LBA  0xE0
#define ATA_SLAVE_LBA   0xF0

static int present = 0;

/*
 * This is either 0xE0 for primary master or 0xF0 for primary slave.
 * ata_read/ata_write use this when selecting the drive for LBA commands.
 */
static unsigned char selected_lba_drive = ATA_MASTER_LBA;

static void ata_400ns_delay(void) {
    inb(ATA_CTRL);
    inb(ATA_CTRL);
    inb(ATA_CTRL);
    inb(ATA_CTRL);
}

static int ata_wait_not_busy(unsigned char* out_status) {
    for (unsigned int i = 0; i < 1000000; i++) {
        unsigned char s = inb(ATA_STATUS);

        if (!(s & ATA_SR_BSY)) {
            if (out_status) {
                *out_status = s;
            }

            return 0;
        }
    }

    return -1;
}

static int ata_wait_drq(void) {
    for (unsigned int i = 0; i < 1000000; i++) {
        unsigned char s = inb(ATA_STATUS);

        if (s & (ATA_SR_ERR | ATA_SR_DF)) {
            return -1;
        }

        if (!(s & ATA_SR_BSY) && (s & ATA_SR_DRQ)) {
            return 0;
        }
    }

    return -1;
}

static void ata_soft_reset(void) {
    /*
     * SRST reset on primary channel.
     * nIEN = 1 disables ATA IRQs since this driver polls.
     */
    outb(ATA_CTRL, 0x04);
    ata_400ns_delay();

    outb(ATA_CTRL, 0x02);
    ata_400ns_delay();

    /*
     * Give the channel some extra time after reset.
     */
    for (unsigned int i = 0; i < 10000; i++) {
        inb(ATA_STATUS);
    }
}

static void ata_select_chs(unsigned char chs_drive) {
    outb(ATA_DRIVE, chs_drive);
    ata_400ns_delay();
}

static void ata_select_lba(unsigned int lba) {
    outb(ATA_DRIVE,
         selected_lba_drive | (unsigned char)((lba >> 24) & 0x0F));
    ata_400ns_delay();
}

/*
 * Try IDENTIFY on one drive.
 *
 * chs_drive:
 *   0xA0 = primary master
 *   0xB0 = primary slave
 *
 * lba_drive:
 *   0xE0 = primary master LBA
 *   0xF0 = primary slave LBA
 */
static int ata_probe_one(unsigned char chs_drive,
                         unsigned char lba_drive,
                         const char* name) {
    ata_select_chs(chs_drive);

    unsigned char status = inb(ATA_STATUS);

    if (status == 0xFF) {
        return 0;
    }

    /*
     * Send IDENTIFY.
     */
    outb(ATA_SECCNT, 0);
    outb(ATA_LBA_LO, 0);
    outb(ATA_LBA_MID, 0);
    outb(ATA_LBA_HI, 0);
    outb(ATA_CMD, ATA_CMD_IDENTIFY);

    ata_400ns_delay();

    status = inb(ATA_STATUS);

    if (status == 0x00 || status == 0xFF) {
        return 0;
    }

    if (ata_wait_not_busy(&status) < 0) {
        printf("ata: %s stuck BUSY after IDENTIFY\n", name);
        return 0;
    }

    /*
     * ATAPI devices, like CD-ROM drives, report a signature in LBA_MID/HI.
     * FAT disk must be a plain ATA device, so skip non-ATA devices.
     *
     * Common signatures:
     *   ATAPI: 0x14 / 0xEB
     *   SATA : 0x3C / 0xC3
     */
    unsigned char mid = inb(ATA_LBA_MID);
    unsigned char hi  = inb(ATA_LBA_HI);

    if (mid != 0 || hi != 0) {
        printf("ata: %s is not ATA disk, sig=%x:%x\n", name, hi, mid);
        return 0;
    }

    if (ata_wait_drq() < 0) {
        printf("ata: %s IDENTIFY never produced data\n", name);
        return 0;
    }

    /*
     * Drain IDENTIFY data.
     */
    unsigned short identify[256];
    insw(ATA_DATA, identify, 256);

    selected_lba_drive = lba_drive;
    present = 1;

    printf("[OK] ATA disk found: %s\n", name);

    return 1;
}

int ata_init(void) {
    present = 0;
    selected_lba_drive = ATA_MASTER_LBA;

    ata_soft_reset();

    /*
     * Check both primary master and primary slave.
     *
     * This fixes the "filesystem worked yesterday" problem caused by
     * QEMU drive ordering changing after modifying -cdrom / -drive args.
     */
    if (ata_probe_one(ATA_MASTER_CHS,
                      ATA_MASTER_LBA,
                      "primary master")) {
        return 1;
    }

    if (ata_probe_one(ATA_SLAVE_CHS,
                      ATA_SLAVE_LBA,
                      "primary slave")) {
        return 1;
    }

    printf("ata: no primary ATA disk found\n");

    present = 0;
    return 0;
}

int ata_read(unsigned int lba, unsigned int count, void* buf) {
    if (!present) {
        return -1;
    }

    if (count == 0) {
        return 0;
    }

    if ((lba + count) >= (1u << 28)) {
        return -1;
    }

    unsigned char* out = (unsigned char*)buf;

    for (unsigned int i = 0; i < count; i++) {
        unsigned int this_lba = lba + i;

        if (ata_wait_not_busy(0) < 0) {
            return -1;
        }

        ata_select_lba(this_lba);

        outb(ATA_ERR_FEAT, 0);
        outb(ATA_SECCNT, 1);
        outb(ATA_LBA_LO,  (unsigned char)(this_lba & 0xFF));
        outb(ATA_LBA_MID, (unsigned char)((this_lba >> 8) & 0xFF));
        outb(ATA_LBA_HI,  (unsigned char)((this_lba >> 16) & 0xFF));
        outb(ATA_CMD, ATA_CMD_READ_SECTORS);

        if (ata_wait_drq() < 0) {
            return -1;
        }

        insw(ATA_DATA, out, 256);
        out += 512;
    }

    return 0;
}

int ata_write(unsigned int lba, unsigned int count, const void* buf) {
    if (!present) {
        return -1;
    }

    if (count == 0) {
        return 0;
    }

    if ((lba + count) >= (1u << 28)) {
        return -1;
    }

    const unsigned char* in = (const unsigned char*)buf;

    for (unsigned int i = 0; i < count; i++) {
        unsigned int this_lba = lba + i;

        if (ata_wait_not_busy(0) < 0) {
            return -1;
        }

        ata_select_lba(this_lba);

        outb(ATA_ERR_FEAT, 0);
        outb(ATA_SECCNT, 1);
        outb(ATA_LBA_LO,  (unsigned char)(this_lba & 0xFF));
        outb(ATA_LBA_MID, (unsigned char)((this_lba >> 8) & 0xFF));
        outb(ATA_LBA_HI,  (unsigned char)((this_lba >> 16) & 0xFF));
        outb(ATA_CMD, ATA_CMD_WRITE_SECTORS);

        if (ata_wait_drq() < 0) {
            return -1;
        }

        outsw(ATA_DATA, in, 256);
        in += 512;

        unsigned char st;

        if (ata_wait_not_busy(&st) < 0) {
            return -1;
        }

        if (st & (ATA_SR_ERR | ATA_SR_DF)) {
            return -1;
        }
    }

    if (ata_wait_not_busy(0) < 0) {
        return -1;
    }

    outb(ATA_CMD, ATA_CMD_FLUSH_CACHE);

    if (ata_wait_not_busy(0) < 0) {
        return -1;
    }

    return 0;
}