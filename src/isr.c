#include "isr.h"
#include "console.h"
#include "printf.h"
#include "vmm.h"

void halt(void);

static const char* exception_names[32] = {
    "Divide-by-zero",        "Debug",
    "Non-Maskable Interrupt", "Breakpoint",
    "Overflow",              "Bound Range Exceeded",
    "Invalid Opcode",        "Device Not Available",
    "Double Fault",          "Coprocessor Segment Overrun",
    "Invalid TSS",           "Segment Not Present",
    "Stack-Segment Fault",   "General Protection Fault",
    "Page Fault",            "Reserved",
    "x87 FP Exception",      "Alignment Check",
    "Machine Check",         "SIMD FP Exception",
    "Virtualization Exception", "Control Protection Exception",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved",
};

void isr_handler(struct registers* regs) {
    if (regs->int_no == 14) {
        if (vmm_page_fault(regs)) return;
        halt();
    }

    con_set_color(CON_LIGHT_RED, CON_BLACK);
    printf("\n*** EXCEPTION %llu: %s ***\n",
           regs->int_no,
           regs->int_no < 32 ? exception_names[regs->int_no] : "Unknown");

    con_set_color(CON_WHITE, CON_BLACK);
    printf("  err_code: 0x%llx\n", regs->err_code);
    printf("  RIP:      0x%llx\n", regs->rip);
    printf("  CS:       0x%llx\n", regs->cs);
    printf("  RFLAGS:   0x%llx\n", regs->rflags);
    printf("  RAX=0x%llx  RBX=0x%llx  RCX=0x%llx  RDX=0x%llx\n",
           regs->rax, regs->rbx, regs->rcx, regs->rdx);
    printf("  RSI=0x%llx  RDI=0x%llx  RBP=0x%llx\n",
           regs->rsi, regs->rdi, regs->rbp);
    printf("  RSP=0x%llx\n", regs->rsp);

    con_set_color(CON_YELLOW, CON_BLACK);
    printf("\nSystem halted.\n");

    halt();
}