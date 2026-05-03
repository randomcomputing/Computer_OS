#include "isr.h"
#include "vga.h"
#include "printf.h"
#include "vmm.h"

void halt(void);

// Human-readable names for the 32 CPU exceptions.
static const char* exception_names[32] = {
    "Divide-by-zero",
    "Debug",
    "Non-Maskable Interrupt",
    "Breakpoint",
    "Overflow",
    "Bound Range Exceeded",
    "Invalid Opcode",
    "Device Not Available",
    "Double Fault",
    "Coprocessor Segment Overrun",
    "Invalid TSS",
    "Segment Not Present",
    "Stack-Segment Fault",
    "General Protection Fault",
    "Page Fault",
    "Reserved",
    "x87 Floating-Point Exception",
    "Alignment Check",
    "Machine Check",
    "SIMD Floating-Point Exception",
    "Virtualization Exception",
    "Control Protection Exception",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved",
};

void isr_handler(struct registers* regs) {
    // Page fault gets its own handler — it needs CR2 and a decoded error code.
    // It returns 1 if recoverable (e.g. copy_from_user landed on a bad
    // user pointer); we return immediately so the iret resumes at the
    // helper's recovery label.
    if (regs->int_no == 14) {
        if (vmm_page_fault(regs)) return;
        halt();
    }

    vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
    printf("\n*** EXCEPTION %d: %s ***\n",
           regs->int_no,
           regs->int_no < 32 ? exception_names[regs->int_no] : "Unknown");

    vga_set_color(VGA_WHITE, VGA_BLACK);
    printf("  error code: 0x%x\n", regs->err_code);
    printf("  EIP:        0x%x\n", regs->eip);
    printf("  CS:         0x%x\n", regs->cs);
    printf("  EFLAGS:     0x%x\n", regs->eflags);
    printf("  EAX=0x%x  EBX=0x%x  ECX=0x%x  EDX=0x%x\n",
           regs->eax, regs->ebx, regs->ecx, regs->edx);
    printf("  ESI=0x%x  EDI=0x%x  EBP=0x%x\n",
           regs->esi, regs->edi, regs->ebp);

    vga_set_color(VGA_YELLOW, VGA_BLACK);
    printf("\nSystem halted.\n");

    halt();
}