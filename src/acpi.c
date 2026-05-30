#include "acpi.h"
#include "io.h"
#include "printf.h"
#include "serial.h"
#include "vmm.h"

// ---- ACPI table structures ------------------------------------------------

typedef struct __attribute__((packed)) {
    char     sig[8];        // "RSD PTR "
    unsigned char  checksum;
    char     oem_id[6];
    unsigned char  revision;   // 0 = ACPI 1.0, 2 = ACPI 2.0+
    unsigned int   rsdt_addr;  // physical address of RSDT
    // ACPI 2.0+ fields (revision >= 2):
    unsigned int   length;
    unsigned long long xsdt_addr;
    unsigned char  ext_checksum;
    unsigned char  reserved[3];
} rsdp_t;

typedef struct __attribute__((packed)) {
    char         sig[4];
    unsigned int length;
    unsigned char revision;
    unsigned char checksum;
    char         oem_id[6];
    char         oem_table_id[8];
    unsigned int oem_revision;
    unsigned int creator_id;
    unsigned int creator_revision;
} sdt_header_t;

// Generic Address Structure (GAS)
typedef struct __attribute__((packed)) {
    unsigned char  addr_space;  // 0=memory, 1=I/O, 2=PCI config
    unsigned char  bit_width;
    unsigned char  bit_offset;
    unsigned char  access_size;
    unsigned long long address;
} gas_t;

typedef struct __attribute__((packed)) {
    sdt_header_t header;
    unsigned int firmware_ctrl;
    unsigned int dsdt;
    unsigned char reserved0;
    unsigned char preferred_pm_profile;
    unsigned short sci_int;
    unsigned int smi_cmd;
    unsigned char acpi_enable;
    unsigned char acpi_disable;
    unsigned char s4bios_req;
    unsigned char pstate_cnt;
    unsigned int pm1a_evt_blk;
    unsigned int pm1b_evt_blk;
    unsigned int pm1a_cnt_blk;
    unsigned int pm1b_cnt_blk;
    unsigned int pm2_cnt_blk;
    unsigned int pm_tmr_blk;
    unsigned int gpe0_blk;
    unsigned int gpe1_blk;
    unsigned char pm1_evt_len;
    unsigned char pm1_cnt_len;
    unsigned char pm2_cnt_len;
    unsigned char pm_tmr_len;
    unsigned char gpe0_blk_len;
    unsigned char gpe1_blk_len;
    unsigned char gpe1_base;
    unsigned char cst_cnt;
    unsigned short p_lvl2_lat;
    unsigned short p_lvl3_lat;
    unsigned short flush_size;
    unsigned short flush_stride;
    unsigned char duty_offset;
    unsigned char duty_width;
    unsigned char day_alrm;
    unsigned char mon_alrm;
    unsigned char century;
    unsigned short iapc_boot_arch;
    unsigned char reserved1;
    unsigned int flags;
    gas_t reset_reg;        // GAS for reset register
    unsigned char reset_val; // value to write to reset_reg
} fadt_t;

// ---- state ---------------------------------------------------------------

static fadt_t*        g_fadt    = 0;
static int            g_has_reset_reg = 0;
static unsigned short g_pm1a_cnt = 0;
static unsigned short g_slp_typs5 = 0;
static int            g_has_s5  = 0;

// ---- helpers -------------------------------------------------------------

static int sig_match(const char* a, const char* b, int n) {
    for (int i = 0; i < n; i++)
        if (a[i] != b[i]) return 0;
    return 1;
}

static unsigned char table_checksum(const unsigned char* p, unsigned int len) {
    unsigned char sum = 0;
    for (unsigned int i = 0; i < len; i++) sum += p[i];
    return sum;
}

// ---- RSDP search ---------------------------------------------------------

// Search a memory range for the RSDP signature "RSD PTR ".
static rsdp_t* find_rsdp_in(unsigned int base, unsigned int end) {
    for (unsigned int p = base; p < end; p += 16) {
        rsdp_t* r = (rsdp_t*)p;
        if (!sig_match(r->sig, "RSD PTR ", 8)) continue;
        // Verify ACPI 1.0 checksum (first 20 bytes).
        if (table_checksum((unsigned char*)r, 20) != 0) continue;
        return r;
    }
    return 0;
}

static rsdp_t* find_rsdp(void) {
    // QEMU always places the RSDP in the BIOS ROM area 0xE0000-0xFFFFF.
    // This range is within the 4MB identity map so no vmm_map needed.
    // Skip EBDA scanning -- EBDA can be above 4MB on some configurations
    // and would require mapping before access.
    return find_rsdp_in(0xE0000, 0x100000);
}

// ---- S5 sleep type extraction --------------------------------------------
// Walk DSDT looking for the _S5_ package to find the SLP_TYP value for S5.
// This is a minimal pattern scan -- not a full AML interpreter.
static void extract_s5(unsigned int dsdt_phys) {
    sdt_header_t* dsdt = (sdt_header_t*)dsdt_phys;
    if (!sig_match(dsdt->sig, "DSDT", 4)) return;

    unsigned char* aml     = (unsigned char*)(dsdt_phys + sizeof(sdt_header_t));
    unsigned int   aml_len = dsdt->length - sizeof(sdt_header_t);

    // Look for the byte sequence: '_', 'S', '5', '_'
    for (unsigned int i = 0; i + 7 < aml_len; i++) {
        if (aml[i]   != '_' || aml[i+1] != 'S' ||
            aml[i+2] != '5' || aml[i+3] != '_') continue;

        // Expected: PackageOp (0x12), PkgLength, NumElements, BytePrefix (0x0A), val, ...
        // Skip past the name (4 bytes already consumed), then check for package.
        if (aml[i+4] != 0x12) continue;  // PackageOp
        // SLP_TYPa is at offset +6 or +7 depending on encoding -- try both.
        unsigned char slp_a = 0;
        if (aml[i+6] == 0x0A) {
            slp_a = aml[i+7];
        } else if (aml[i+7] == 0x0A) {
            slp_a = aml[i+8];
        } else {
            slp_a = aml[i+6];
        }
        g_slp_typs5 = (unsigned short)((slp_a & 0x7) << 10);
        g_has_s5 = 1;
        serial_write("[acpi] found _S5_ SLP_TYP\n");
        return;
    }
    serial_write("[acpi] _S5_ not found in DSDT\n");
}

// ---- public API ----------------------------------------------------------

int acpi_init(void) {
    rsdp_t* rsdp = find_rsdp();
    if (!rsdp) {
        serial_write("[acpi] RSDP not found\n");
        return 0;
    }
    serial_write("[acpi] RSDP found\n");

    // Map RSDT physical address into kernel virtual space before accessing.
    // ACPI tables may be above the 4MB identity map, so we must vmm_map them.
    unsigned int rsdt_phys = rsdp->rsdt_addr;
    // Map 2 pages covering the RSDT header + entry array.
    unsigned int rsdt_virt = 0xCFF00000u;
    vmm_map(rsdt_virt,        rsdt_phys & 0xFFFFF000u, VMM_PRESENT);
    vmm_map(rsdt_virt + 4096, (rsdt_phys & 0xFFFFF000u) + 4096, VMM_PRESENT);
    unsigned int rsdt_off = rsdt_phys & 0xFFFu;
    sdt_header_t* rsdt    = (sdt_header_t*)(rsdt_virt + rsdt_off);
    unsigned int  entries = (rsdt->length - sizeof(sdt_header_t)) / 4;
    unsigned int* ptrs    = (unsigned int*)((unsigned char*)rsdt + sizeof(sdt_header_t));

    for (unsigned int i = 0; i < entries; i++) {
        // Map each RSDT entry before reading it -- physical address may be > 4MB.
        unsigned int fadt_phys = ptrs[i];
        unsigned int fadt_virt = 0xCFF02000u;
        vmm_map(fadt_virt,        fadt_phys & 0xFFFFF000u, VMM_PRESENT);
        vmm_map(fadt_virt + 4096, (fadt_phys & 0xFFFFF000u) + 4096, VMM_PRESENT);
        sdt_header_t* h = (sdt_header_t*)(fadt_virt + (fadt_phys & 0xFFFu));
        if (!sig_match(h->sig, "FACP", 4)) continue;

        g_fadt = (fadt_t*)h;
        serial_write("[acpi] FADT found\n");

        // Check ACPI 2.0+ reset register support (bit 10 of flags).
        if (g_fadt->header.revision >= 2 && (g_fadt->flags & (1u << 10))) {
            g_has_reset_reg = 1;
            serial_write("[acpi] reset register supported\n");
        }

        // PM1a control block for S5 sleep.
        g_pm1a_cnt = (unsigned short)g_fadt->pm1a_cnt_blk;

        // Map DSDT pages (DSDT can be large -- map 4 pages).
        unsigned int dsdt_phys = g_fadt->dsdt;
        unsigned int dsdt_virt = 0xCFF04000u;
        for (int pg = 0; pg < 8; pg++)
            vmm_map(dsdt_virt + (unsigned int)pg*4096,
                    (dsdt_phys & 0xFFFFF000u) + (unsigned int)pg*4096,
                    VMM_PRESENT);
        // Extract S5 sleep type from DSDT.
        extract_s5(dsdt_virt + (dsdt_phys & 0xFFFu));
        return 1;
    }

    serial_write("[acpi] FADT not found in RSDT\n");
    return 0;
}

void acpi_reboot(void) {
    if (g_has_reset_reg && g_fadt) {
        gas_t* reg = &g_fadt->reset_reg;
        serial_write("[acpi] using FADT reset register\n");

        if (reg->addr_space == 1) {
            // I/O port -- most common on QEMU i440fx (port 0xCF9, value 0x06).
            outb((unsigned short)reg->address, g_fadt->reset_val);
        } else if (reg->addr_space == 0) {
            // Memory-mapped.
            volatile unsigned char* p = (volatile unsigned char*)(unsigned int)reg->address;
            *p = g_fadt->reset_val;
        }
        // Short spin to let the reset propagate.
        for (volatile int i = 0; i < 100000; i++) {}
    }

    // Fallback: KBC reset line.
    serial_write("[acpi] fallback: KBC reset\n");
    for (int i = 0; i < 16; i++) {
        if ((inb(0x64) & 0x01) == 0) break;
        (void)inb(0x60);
    }
    for (volatile int i = 0; i < 100000; i++) {
        if ((inb(0x64) & 0x02) == 0) break;
    }
    outb(0x64, 0xFE);
    for (volatile int i = 0; i < 1000000; i++) {}

    // Triple fault last resort.
    serial_write("[acpi] fallback: triple fault\n");
    struct { unsigned short limit; unsigned int base; }
        __attribute__((packed)) null_idt = { 0, 0 };
    __asm__ volatile (
        "lidt (%0)\n\t"
        "sti\n\t"
        "int $0x03\n\t"
        : : "r"(&null_idt) : "memory"
    );
    for (;;) __asm__ volatile ("hlt");
}

void acpi_poweroff(void) {
    if (g_has_s5 && g_pm1a_cnt) {
        serial_write("[acpi] ACPI S5 poweroff\n");
        __asm__ volatile ("cli");
        outw(g_pm1a_cnt, g_slp_typs5 | 0x2000);  // SLP_TYP | SLP_EN
        for (volatile int i = 0; i < 100000; i++) {}
    }
    // Legacy QEMU fallback.
    serial_write("[acpi] fallback: legacy poweroff\n");
    outw(0x604,  0x2000);
    outw(0xB004, 0x2000);
    __asm__ volatile ("sti");
}