#include "ata.h"
#include "io.h"
#include "printf.h"
#include "console_backend.h"

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
#define ATA_CMD_READ_SECTORS      0x20   /* 28-bit LBA */
#define ATA_CMD_WRITE_SECTORS     0x30   /* 28-bit LBA */
#define ATA_CMD_READ_SECTORS_EXT  0x24   /* 48-bit LBA */
#define ATA_CMD_WRITE_SECTORS_EXT 0x34   /* 48-bit LBA */
#define ATA_CMD_FLUSH_CACHE       0xE7
#define ATA_CMD_FLUSH_CACHE_EXT   0xEA
#define ATA_CMD_IDENTIFY          0xEC

// Largest sector index addressable by 28-bit LBA; anything at or beyond
// this must use the 48-bit EXT commands.
#define ATA_LBA28_LIMIT  (1ULL << 28)

// LBA48 drive/head select: bit 6 (LBA) set, no address bits in the low
// nibble (all 48 bits go in the LBA registers). 0x40 master, 0x50 slave.
#define ATA_MASTER_LBA48  0x40
#define ATA_SLAVE_LBA48   0x50

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

/*
 * Filled in from the selected drive's IDENTIFY data: whether it supports
 * 48-bit addressing, and its total 512-byte sector count. total_sectors
 * is 0 when no disk is present.
 */
static int      supports_lba48 = 0;
static uint64_t total_sectors  = 0;

static void ata_400ns_delay(void) {
    inb(ATA_CTRL);
    inb(ATA_CTRL);
    inb(ATA_CTRL);
    inb(ATA_CTRL);
}

static void ata_print_ok(const char* msg, const char* name) {
    con_set_color(CON_LIGHT_GREEN, CON_BLACK);
    printf("[OK]");
    con_set_color(CON_WHITE, CON_BLACK);
    printf(" %s: %s\n", msg, name);
}

static void ata_print_fail(const char* msg) {
    con_set_color(CON_LIGHT_RED, CON_BLACK);
    printf("[!!]");
    con_set_color(CON_WHITE, CON_BLACK);
    printf(" %s\n", msg);
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
 * Program the task-file registers for a single-sector 28-bit transfer at
 * `lba`. The caller issues the READ/WRITE SECTORS command afterwards.
 */
static void ata_setup_lba28(unsigned int lba) {
    ata_select_lba(lba);
    outb(ATA_ERR_FEAT, 0);
    outb(ATA_SECCNT, 1);
    outb(ATA_LBA_LO,  (unsigned char)(lba & 0xFF));
    outb(ATA_LBA_MID, (unsigned char)((lba >> 8) & 0xFF));
    outb(ATA_LBA_HI,  (unsigned char)((lba >> 16) & 0xFF));
}

/*
 * Program the task-file registers for a single-sector 48-bit transfer at
 * `lba`. LBA48 writes each register twice — the high byte first, then the
 * low byte — and puts all 48 address bits in the LBA registers, so the
 * drive/head register carries no address bits. Sector count is 1, written
 * as high byte 0 then low byte 1. The caller issues the EXT command after.
 */
static void ata_setup_lba48(uint64_t lba) {
    unsigned char dev = (selected_lba_drive == ATA_SLAVE_LBA)
                      ? ATA_SLAVE_LBA48 : ATA_MASTER_LBA48;
    outb(ATA_DRIVE, dev);
    ata_400ns_delay();

    outb(ATA_ERR_FEAT, 0);

    /* high-order bytes */
    outb(ATA_SECCNT, 0);
    outb(ATA_LBA_LO,  (unsigned char)((lba >> 24) & 0xFF));
    outb(ATA_LBA_MID, (unsigned char)((lba >> 32) & 0xFF));
    outb(ATA_LBA_HI,  (unsigned char)((lba >> 40) & 0xFF));

    /* low-order bytes */
    outb(ATA_SECCNT, 1);
    outb(ATA_LBA_LO,  (unsigned char)(lba & 0xFF));
    outb(ATA_LBA_MID, (unsigned char)((lba >> 8) & 0xFF));
    outb(ATA_LBA_HI,  (unsigned char)((lba >> 16) & 0xFF));
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
        ata_print_fail("ATA stuck BUSY after IDENTIFY");
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
        return 0;
    }

    if (ata_wait_drq() < 0) {
        ata_print_fail("ATA IDENTIFY never produced data");
        return 0;
    }

    /*
     * Drain IDENTIFY data.
     */
    unsigned short identify[256];
    insw(ATA_DATA, identify, 256);

    /*
     * Capabilities from IDENTIFY:
     *   word 83 bit 10  — 48-bit addressing supported
     *   words 60..61    — total sectors, 28-bit (LBA28)
     *   words 100..103  — total sectors, 48-bit (LBA48)
     */
    uint32_t sec28 = (uint32_t)identify[60]
                   | ((uint32_t)identify[61] << 16);
    uint64_t sec48 = (uint64_t)identify[100]
                   | ((uint64_t)identify[101] << 16)
                   | ((uint64_t)identify[102] << 32)
                   | ((uint64_t)identify[103] << 48);

    supports_lba48 = (identify[83] >> 10) & 1;
    total_sectors  = (supports_lba48 && sec48) ? sec48 : sec28;

    selected_lba_drive = lba_drive;
    present = 1;

    ata_print_ok("ATA disk found", name);
    printf("     %s, %llu sectors (%llu MiB)\n",
           supports_lba48 ? "LBA48" : "LBA28",
           (unsigned long long)total_sectors,
           (unsigned long long)((total_sectors * 512ULL) / (1024 * 1024)));

    return 1;
}

int ata_init(void) {
    present = 0;
    selected_lba_drive = ATA_MASTER_LBA;
    supports_lba48 = 0;
    total_sectors  = 0;

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

    ata_print_fail("No primary ATA disk found");

    present = 0;
    return 0;
}

uint64_t ata_total_sectors(void) {
    return total_sectors;
}

int ata_lba48_supported(void) {
    return supports_lba48;
}

/*
 * Decide whether a request must use 48-bit addressing, and reject it if it
 * runs off the end of the disk. Returns 1 for LBA48, 0 for LBA28, or -1 if
 * the request is out of range (or needs LBA48 on a drive without it).
 */
static int ata_pick_mode(uint64_t lba, unsigned int count) {
    uint64_t end = lba + count;              /* one past the last sector */

    if (total_sectors && end > total_sectors) {
        return -1;
    }

    if (end > ATA_LBA28_LIMIT) {
        return supports_lba48 ? 1 : -1;
    }

    return 0;
}

int ata_read(uint64_t lba, unsigned int count, void* buf) {
    if (!present) {
        return -1;
    }

    if (count == 0) {
        return 0;
    }

    int use48 = ata_pick_mode(lba, count);
    if (use48 < 0) {
        return -1;
    }

    unsigned char* out = (unsigned char*)buf;

    for (unsigned int i = 0; i < count; i++) {
        uint64_t this_lba = lba + i;

        if (ata_wait_not_busy(0) < 0) {
            return -1;
        }

        if (use48) {
            ata_setup_lba48(this_lba);
            outb(ATA_CMD, ATA_CMD_READ_SECTORS_EXT);
        } else {
            ata_setup_lba28((unsigned int)this_lba);
            outb(ATA_CMD, ATA_CMD_READ_SECTORS);
        }

        if (ata_wait_drq() < 0) {
            return -1;
        }

        insw(ATA_DATA, out, 256);
        out += 512;
    }

    return 0;
}

int ata_write(uint64_t lba, unsigned int count, const void* buf) {
    if (!present) {
        return -1;
    }

    if (count == 0) {
        return 0;
    }

    int use48 = ata_pick_mode(lba, count);
    if (use48 < 0) {
        return -1;
    }

    const unsigned char* in = (const unsigned char*)buf;

    for (unsigned int i = 0; i < count; i++) {
        uint64_t this_lba = lba + i;

        if (ata_wait_not_busy(0) < 0) {
            return -1;
        }

        if (use48) {
            ata_setup_lba48(this_lba);
            outb(ATA_CMD, ATA_CMD_WRITE_SECTORS_EXT);
        } else {
            ata_setup_lba28((unsigned int)this_lba);
            outb(ATA_CMD, ATA_CMD_WRITE_SECTORS);
        }

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

    outb(ATA_CMD, use48 ? ATA_CMD_FLUSH_CACHE_EXT : ATA_CMD_FLUSH_CACHE);

    if (ata_wait_not_busy(0) < 0) {
        return -1;
    }

    return 0;
}