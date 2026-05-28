#include "e1000.h"
#include "pci.h"
#include "vmm.h"
#include "pmm.h"
#include "printf.h"
#include "string.h"

#define E1000_VENDOR_ID 0x8086
#define E1000_DEVICE_ID 0x100E

#define E1000_MMIO_VIRT_BASE 0xE1000000u
#define E1000_MMIO_SIZE      0x20000u

#define E1000_REG_CTRL       0x0000
#define E1000_REG_STATUS     0x0008
#define E1000_REG_MTA        0x5200    // multicast table array (128 regs)

// CTRL bits.
#define CTRL_ASDE            0x00000020  // auto-speed detect enable
#define CTRL_SLU             0x00000040  // set link up
#define E1000_REG_ICR        0x00C0
#define E1000_REG_IMC        0x00D8
#define E1000_REG_RCTL       0x0100
#define E1000_REG_TCTL       0x0400
#define E1000_REG_TIPG       0x0410
#define E1000_REG_RDBAL      0x2800
#define E1000_REG_RDBAH      0x2804
#define E1000_REG_RDLEN      0x2808
#define E1000_REG_RDH        0x2810
#define E1000_REG_RDT        0x2818
#define E1000_REG_TDBAL      0x3800
#define E1000_REG_TDBAH      0x3804
#define E1000_REG_TDLEN      0x3808
#define E1000_REG_TDH        0x3810
#define E1000_REG_TDT        0x3818
#define E1000_REG_RAL        0x5400
#define E1000_REG_RAH        0x5404

#define RCTL_EN              (1 << 1)
#define RCTL_UPE             (1 << 3)    // unicast promiscuous
#define RCTL_MPE             (1 << 4)    // multicast promiscuous
#define RCTL_BAM             (1 << 15)
#define RCTL_SECRC           (1 << 26)

#define TCTL_EN              (1 << 1)
#define TCTL_PSP             (1 << 3)
#define TCTL_CT_SHIFT        4
#define TCTL_COLD_SHIFT      12

#define RXD_STAT_DD          0x01
#define RXD_STAT_EOP         0x02

#define TXD_CMD_EOP          0x01
#define TXD_CMD_IFCS         0x02
#define TXD_CMD_RS           0x08
#define TXD_STAT_DD          0x01

#define NUM_RX_DESC          32
#define NUM_TX_DESC          32
#define RX_BUF_SIZE          2048
#define TX_BUF_SIZE          2048

typedef struct {
    unsigned int  addr_low;
    unsigned int  addr_high;
    unsigned short length;
    unsigned short checksum;
    unsigned char  status;
    unsigned char  errors;
    unsigned short special;
} __attribute__((packed)) rx_desc_t;

typedef struct {
    unsigned int  addr_low;
    unsigned int  addr_high;
    unsigned short length;
    unsigned char  cso;
    unsigned char  cmd;
    unsigned char  status;
    unsigned char  css;
    unsigned short special;
} __attribute__((packed)) tx_desc_t;

static volatile unsigned int* e1000_mmio = 0;
static unsigned char e1000_mac[6];

#define DMA_VIRT_BASE 0xE2000000u
static unsigned int dma_next = 0;

static volatile rx_desc_t* rx_ring;
static volatile tx_desc_t* tx_ring;
static unsigned int rx_buf_phys[NUM_RX_DESC];
static unsigned int tx_buf_phys[NUM_TX_DESC];
static unsigned int rx_buf_virt[NUM_RX_DESC];
static unsigned int tx_buf_virt[NUM_TX_DESC];
static unsigned int rx_cur = 0;
static unsigned int tx_cur = 0;

static unsigned int e1000_read(unsigned int reg) { return e1000_mmio[reg / 4]; }
static void e1000_write(unsigned int reg, unsigned int value) { e1000_mmio[reg / 4] = value; }

static int e1000_map_mmio(unsigned int phys) {
    unsigned int pages = E1000_MMIO_SIZE / PAGE_SIZE;
    for (unsigned int i = 0; i < pages; i++) {
        unsigned int off = i * PAGE_SIZE;
        if (!vmm_map(E1000_MMIO_VIRT_BASE + off, phys + off, VMM_PRESENT | VMM_WRITE)) {
            printf("e1000: failed to map MMIO page %u\n", i);
            return 0;
        }
    }
    e1000_mmio = (volatile unsigned int*)E1000_MMIO_VIRT_BASE;
    return 1;
}

static unsigned int dma_alloc_page(unsigned int* phys_out) {
    unsigned int phys = pmm_alloc();
    if (!phys) return 0;
    unsigned int virt = DMA_VIRT_BASE + dma_next;
    if (!vmm_map(virt, phys, VMM_PRESENT | VMM_WRITE)) return 0;
    dma_next += PAGE_SIZE;
    memset((void*)virt, 0, PAGE_SIZE);
    if (phys_out) *phys_out = phys;
    return virt;
}

static int e1000_setup_rx(void) {
    unsigned int ring_phys, ring_virt;
    ring_virt = dma_alloc_page(&ring_phys);
    if (!ring_virt) return 0;
    rx_ring = (volatile rx_desc_t*)ring_virt;

    for (int i = 0; i < NUM_RX_DESC; i += 2) {
        unsigned int p, v;
        v = dma_alloc_page(&p);
        if (!v) return 0;
        rx_buf_virt[i] = v; rx_buf_phys[i] = p;
        rx_ring[i].addr_low = p; rx_ring[i].addr_high = 0; rx_ring[i].status = 0;
        if (i + 1 < NUM_RX_DESC) {
            rx_buf_virt[i+1] = v + RX_BUF_SIZE; rx_buf_phys[i+1] = p + RX_BUF_SIZE;
            rx_ring[i+1].addr_low = p + RX_BUF_SIZE; rx_ring[i+1].addr_high = 0; rx_ring[i+1].status = 0;
        }
    }

    e1000_write(E1000_REG_RDBAL, ring_phys);
    e1000_write(E1000_REG_RDBAH, 0);
    e1000_write(E1000_REG_RDLEN, NUM_RX_DESC * sizeof(rx_desc_t));
    e1000_write(E1000_REG_RDH, 0);
    e1000_write(E1000_REG_RDT, NUM_RX_DESC - 1);
    rx_cur = 0;
    e1000_write(E1000_REG_RCTL, RCTL_EN | RCTL_UPE | RCTL_MPE | RCTL_BAM | RCTL_SECRC);
    return 1;
}

static int e1000_setup_tx(void) {
    unsigned int ring_phys, ring_virt;
    ring_virt = dma_alloc_page(&ring_phys);
    if (!ring_virt) return 0;
    tx_ring = (volatile tx_desc_t*)ring_virt;

    for (int i = 0; i < NUM_TX_DESC; i += 2) {
        unsigned int p, v;
        v = dma_alloc_page(&p);
        if (!v) return 0;
        tx_buf_virt[i] = v; tx_buf_phys[i] = p;
        tx_ring[i].addr_low = p; tx_ring[i].addr_high = 0;
        tx_ring[i].status = TXD_STAT_DD; tx_ring[i].cmd = 0;
        if (i + 1 < NUM_TX_DESC) {
            tx_buf_virt[i+1] = v + TX_BUF_SIZE; tx_buf_phys[i+1] = p + TX_BUF_SIZE;
            tx_ring[i+1].addr_low = p + TX_BUF_SIZE; tx_ring[i+1].addr_high = 0;
            tx_ring[i+1].status = TXD_STAT_DD; tx_ring[i+1].cmd = 0;
        }
    }

    e1000_write(E1000_REG_TDBAL, ring_phys);
    e1000_write(E1000_REG_TDBAH, 0);
    e1000_write(E1000_REG_TDLEN, NUM_TX_DESC * sizeof(tx_desc_t));
    e1000_write(E1000_REG_TDH, 0);
    e1000_write(E1000_REG_TDT, 0);
    tx_cur = 0;
    e1000_write(E1000_REG_TIPG, 10 | (8 << 10) | (6 << 20));
    e1000_write(E1000_REG_TCTL, TCTL_EN | TCTL_PSP | (0x0F << TCTL_CT_SHIFT) | (0x40 << TCTL_COLD_SHIFT));
    return 1;
}

static void e1000_read_mac_from_registers(void) {
    unsigned int ral = e1000_read(E1000_REG_RAL);
    unsigned int rah = e1000_read(E1000_REG_RAH);
    e1000_mac[0] = (unsigned char)(ral & 0xFF);
    e1000_mac[1] = (unsigned char)((ral >> 8) & 0xFF);
    e1000_mac[2] = (unsigned char)((ral >> 16) & 0xFF);
    e1000_mac[3] = (unsigned char)((ral >> 24) & 0xFF);
    e1000_mac[4] = (unsigned char)(rah & 0xFF);
    e1000_mac[5] = (unsigned char)((rah >> 8) & 0xFF);
}

int e1000_send(const void* data, unsigned int len) {
    if (!e1000_mmio || len == 0 || len > TX_BUF_SIZE) return 0;
    volatile tx_desc_t* d = &tx_ring[tx_cur];
    if (!(d->status & TXD_STAT_DD)) return 0;
    memcpy((void*)tx_buf_virt[tx_cur], data, len);
    d->length = (unsigned short)len;
    d->cmd = TXD_CMD_EOP | TXD_CMD_IFCS | TXD_CMD_RS;
    d->status = 0;
    tx_cur = (tx_cur + 1) % NUM_TX_DESC;
    e1000_write(E1000_REG_TDT, tx_cur);
    return 1;
}

unsigned int e1000_receive_poll(void* out, unsigned int max) {
    if (!e1000_mmio) return 0;
    volatile rx_desc_t* d = &rx_ring[rx_cur];
    if (!(d->status & RXD_STAT_DD)) return 0;
    unsigned int len = d->length;
    if (len > max) len = max;
    memcpy(out, (void*)rx_buf_virt[rx_cur], len);
    d->status = 0;
    e1000_write(E1000_REG_RDT, rx_cur);
    rx_cur = (rx_cur + 1) % NUM_RX_DESC;
    return len;
}

void e1000_get_mac(unsigned char mac_out[6]) {
    for (int i = 0; i < 6; i++) mac_out[i] = e1000_mac[i];
}

int e1000_init(void) {
    const pci_device_t* dev = pci_find(E1000_VENDOR_ID, E1000_DEVICE_ID);
    if (!dev) { printf("e1000: not found\n"); return 0; }
    printf("e1000: found at %u:%u:%u irq %u\n", dev->bus, dev->device, dev->function, dev->irq_line);

    pci_enable_bus_mastering(dev);
    if (pci_bar_is_io(dev, 0)) { printf("e1000: BAR0 is I/O, expected MMIO\n"); return 0; }
    unsigned int bar0_phys = pci_bar_addr(dev, 0);
    if (!bar0_phys) { printf("e1000: BAR0 is zero\n"); return 0; }
    if (!e1000_map_mmio(bar0_phys)) { printf("e1000: MMIO map failed\n"); return 0; }

    e1000_write(E1000_REG_IMC, 0xFFFFFFFF);
    e1000_read(E1000_REG_ICR);

    e1000_read_mac_from_registers();
    printf("e1000: MAC = %x:%x:%x:%x:%x:%x\n",
           e1000_mac[0], e1000_mac[1], e1000_mac[2], e1000_mac[3], e1000_mac[4], e1000_mac[5]);

    // Bring the link up BEFORE enabling the receiver/transmitter. Without
    // CTRL.SLU the PHY link never comes up, so frames silently go nowhere and
    // none are ever received — set link up + auto-speed-detect first.
    {
        unsigned int ctrl = e1000_read(E1000_REG_CTRL);
        ctrl |= CTRL_SLU | CTRL_ASDE;
        e1000_write(E1000_REG_CTRL, ctrl);
    }

    // Clear the multicast table array (128 registers) — it holds garbage at
    // reset and can otherwise interfere with receive filtering.
    for (int i = 0; i < 128; i++)
        e1000_write(E1000_REG_MTA + i * 4, 0);

    dma_next = 0;
    if (!e1000_setup_rx()) { printf("e1000: RX setup failed\n"); return 0; }
    if (!e1000_setup_tx()) { printf("e1000: TX setup failed\n"); return 0; }

    printf("e1000: RX/TX rings ready (%d/%d desc)\n", NUM_RX_DESC, NUM_TX_DESC);
    return 1;
}