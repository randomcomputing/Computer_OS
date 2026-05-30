#include "idt.h"
#include "stdint.h"

/*
 * 64-bit IDT.
 *
 * A 64-bit interrupt gate descriptor is 16 bytes:
 *   offset_low   [15:0]
 *   selector     [31:16]
 *   ist + zero   [39:32]
 *   type_attr    [47:40]
 *   offset_mid   [63:48]
 *   offset_high  [95:64]
 *   reserved     [127:96]
 */
struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;           /* interrupt stack table index (0 = none) */
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct idt_entry idt[256];
static struct idt_ptr   idtp;

/* Exception stubs (isr.asm). */
extern void isr0(void);  extern void isr1(void);  extern void isr2(void);  extern void isr3(void);
extern void isr4(void);  extern void isr5(void);  extern void isr6(void);  extern void isr7(void);
extern void isr8(void);  extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void); extern void isr15(void);
extern void isr16(void); extern void isr17(void); extern void isr18(void); extern void isr19(void);
extern void isr20(void); extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void); extern void isr27(void);
extern void isr28(void); extern void isr29(void); extern void isr30(void); extern void isr31(void);

/* IRQ stubs (irq_asm.asm). */
extern void irq0(void);  extern void irq1(void);  extern void irq2(void);  extern void irq3(void);
extern void irq4(void);  extern void irq5(void);  extern void irq6(void);  extern void irq7(void);
extern void irq8(void);  extern void irq9(void);  extern void irq10(void); extern void irq11(void);
extern void irq12(void); extern void irq13(void); extern void irq14(void); extern void irq15(void);

static void set_gate(int num, uint64_t handler, uint8_t type_attr) {
    idt[num].offset_low  = (uint16_t)(handler & 0xFFFF);
    idt[num].offset_mid  = (uint16_t)((handler >> 16) & 0xFFFF);
    idt[num].offset_high = (uint32_t)((handler >> 32) & 0xFFFFFFFF);
    idt[num].selector    = 0x08;    /* kernel code selector */
    idt[num].ist         = 0;
    idt[num].type_attr   = type_attr;
    idt[num].reserved    = 0;
}

/* DPL=0 interrupt gate (kernel only). */
static void set_kgate(int num, uint64_t handler) {
    set_gate(num, handler, 0x8E);   /* P=1, DPL=0, type=0xE (64-bit intr gate) */
}

/* DPL=3 interrupt gate (callable from ring 3, e.g. int 0x80). */
void idt_set_user_gate(int num, uint64_t handler) {
    set_gate(num, handler, 0xEE);   /* P=1, DPL=3, type=0xE */
}

static void idt_load(void) {
    __asm__ volatile ("lidt (%0)" : : "r"(&idtp));
}

void idt_init(void) {
    idtp.limit = sizeof(idt) - 1;
    idtp.base  = (uint64_t)&idt;

    uint8_t* p = (uint8_t*)idt;
    for (uint32_t i = 0; i < sizeof(idt); i++) p[i] = 0;

    set_kgate(0,  (uint64_t)isr0);  set_kgate(1,  (uint64_t)isr1);
    set_kgate(2,  (uint64_t)isr2);  set_kgate(3,  (uint64_t)isr3);
    set_kgate(4,  (uint64_t)isr4);  set_kgate(5,  (uint64_t)isr5);
    set_kgate(6,  (uint64_t)isr6);  set_kgate(7,  (uint64_t)isr7);
    set_kgate(8,  (uint64_t)isr8);  set_kgate(9,  (uint64_t)isr9);
    set_kgate(10, (uint64_t)isr10); set_kgate(11, (uint64_t)isr11);
    set_kgate(12, (uint64_t)isr12); set_kgate(13, (uint64_t)isr13);
    set_kgate(14, (uint64_t)isr14); set_kgate(15, (uint64_t)isr15);
    set_kgate(16, (uint64_t)isr16); set_kgate(17, (uint64_t)isr17);
    set_kgate(18, (uint64_t)isr18); set_kgate(19, (uint64_t)isr19);
    set_kgate(20, (uint64_t)isr20); set_kgate(21, (uint64_t)isr21);
    set_kgate(22, (uint64_t)isr22); set_kgate(23, (uint64_t)isr23);
    set_kgate(24, (uint64_t)isr24); set_kgate(25, (uint64_t)isr25);
    set_kgate(26, (uint64_t)isr26); set_kgate(27, (uint64_t)isr27);
    set_kgate(28, (uint64_t)isr28); set_kgate(29, (uint64_t)isr29);
    set_kgate(30, (uint64_t)isr30); set_kgate(31, (uint64_t)isr31);

    set_kgate(32, (uint64_t)irq0);  set_kgate(33, (uint64_t)irq1);
    set_kgate(34, (uint64_t)irq2);  set_kgate(35, (uint64_t)irq3);
    set_kgate(36, (uint64_t)irq4);  set_kgate(37, (uint64_t)irq5);
    set_kgate(38, (uint64_t)irq6);  set_kgate(39, (uint64_t)irq7);
    set_kgate(40, (uint64_t)irq8);  set_kgate(41, (uint64_t)irq9);
    set_kgate(42, (uint64_t)irq10); set_kgate(43, (uint64_t)irq11);
    set_kgate(44, (uint64_t)irq12); set_kgate(45, (uint64_t)irq13);
    set_kgate(46, (uint64_t)irq14); set_kgate(47, (uint64_t)irq15);

    idt_load();
}