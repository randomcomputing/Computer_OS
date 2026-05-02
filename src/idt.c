#include "idt.h"

struct idt_entry {
    unsigned short offset_low;
    unsigned short selector;
    unsigned char  zero;
    unsigned char  type_attr;
    unsigned short offset_high;
} __attribute__((packed));

struct idt_ptr {
    unsigned short limit;
    unsigned int   base;
} __attribute__((packed));

static struct idt_entry idt[256];
static struct idt_ptr   idtp;

// Exception stubs (from isr.asm).
extern void isr0(void);  extern void isr1(void);  extern void isr2(void);  extern void isr3(void);
extern void isr4(void);  extern void isr5(void);  extern void isr6(void);  extern void isr7(void);
extern void isr8(void);  extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void); extern void isr15(void);
extern void isr16(void); extern void isr17(void); extern void isr18(void); extern void isr19(void);
extern void isr20(void); extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void); extern void isr27(void);
extern void isr28(void); extern void isr29(void); extern void isr30(void); extern void isr31(void);

// IRQ stubs (from irq_asm.asm).
extern void irq0(void);  extern void irq1(void);  extern void irq2(void);  extern void irq3(void);
extern void irq4(void);  extern void irq5(void);  extern void irq6(void);  extern void irq7(void);
extern void irq8(void);  extern void irq9(void);  extern void irq10(void); extern void irq11(void);
extern void irq12(void); extern void irq13(void); extern void irq14(void); extern void irq15(void);

static void set_gate(int num, unsigned int handler) {
    idt[num].offset_low  = handler & 0xFFFF;
    idt[num].offset_high = (handler >> 16) & 0xFFFF;
    idt[num].selector    = 0x08;
    idt[num].zero        = 0;
    idt[num].type_attr   = 0x8E;
}

// Like set_gate but DPL=3, so a ring-3 `int $N` won't fault with a GP.
// Used for the syscall vector (0x80). Type byte is the same shape:
//   bit 7   = present
//   bits 6-5 = DPL
//   bit 4   = 0 (system gate)
//   bits 3-0 = gate type (0xE = 32-bit interrupt gate)
// 0xEE = present, DPL=3, 32-bit interrupt gate.
void idt_set_user_gate(int num, unsigned int handler) {
    idt[num].offset_low  = handler & 0xFFFF;
    idt[num].offset_high = (handler >> 16) & 0xFFFF;
    idt[num].selector    = 0x08;
    idt[num].zero        = 0;
    idt[num].type_attr   = 0xEE;
}

static void idt_load(void) {
    __asm__ volatile ("lidt (%0)" : : "r"(&idtp));
}

void idt_init(void) {
    idtp.limit = sizeof(idt) - 1;
    idtp.base  = (unsigned int)&idt;

    unsigned char* p = (unsigned char*)idt;
    for (unsigned int i = 0; i < sizeof(idt); i++) p[i] = 0;

    // CPU exceptions (vectors 0..31)
    set_gate(0,  (unsigned int)isr0);   set_gate(1,  (unsigned int)isr1);
    set_gate(2,  (unsigned int)isr2);   set_gate(3,  (unsigned int)isr3);
    set_gate(4,  (unsigned int)isr4);   set_gate(5,  (unsigned int)isr5);
    set_gate(6,  (unsigned int)isr6);   set_gate(7,  (unsigned int)isr7);
    set_gate(8,  (unsigned int)isr8);   set_gate(9,  (unsigned int)isr9);
    set_gate(10, (unsigned int)isr10);  set_gate(11, (unsigned int)isr11);
    set_gate(12, (unsigned int)isr12);  set_gate(13, (unsigned int)isr13);
    set_gate(14, (unsigned int)isr14);  set_gate(15, (unsigned int)isr15);
    set_gate(16, (unsigned int)isr16);  set_gate(17, (unsigned int)isr17);
    set_gate(18, (unsigned int)isr18);  set_gate(19, (unsigned int)isr19);
    set_gate(20, (unsigned int)isr20);  set_gate(21, (unsigned int)isr21);
    set_gate(22, (unsigned int)isr22);  set_gate(23, (unsigned int)isr23);
    set_gate(24, (unsigned int)isr24);  set_gate(25, (unsigned int)isr25);
    set_gate(26, (unsigned int)isr26);  set_gate(27, (unsigned int)isr27);
    set_gate(28, (unsigned int)isr28);  set_gate(29, (unsigned int)isr29);
    set_gate(30, (unsigned int)isr30);  set_gate(31, (unsigned int)isr31);

    // Hardware IRQs remapped to vectors 32..47 (matches pic_remap).
    set_gate(32, (unsigned int)irq0);   set_gate(33, (unsigned int)irq1);
    set_gate(34, (unsigned int)irq2);   set_gate(35, (unsigned int)irq3);
    set_gate(36, (unsigned int)irq4);   set_gate(37, (unsigned int)irq5);
    set_gate(38, (unsigned int)irq6);   set_gate(39, (unsigned int)irq7);
    set_gate(40, (unsigned int)irq8);   set_gate(41, (unsigned int)irq9);
    set_gate(42, (unsigned int)irq10);  set_gate(43, (unsigned int)irq11);
    set_gate(44, (unsigned int)irq12);  set_gate(45, (unsigned int)irq13);
    set_gate(46, (unsigned int)irq14);  set_gate(47, (unsigned int)irq15);

    idt_load();
}