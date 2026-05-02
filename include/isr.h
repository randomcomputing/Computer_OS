#ifndef ISR_H
#define ISR_H

// Snapshot of CPU state at the moment an interrupt fired.
// Built up by the assembly stub in isr.asm — the layout here MUST
// match the order of pushes there, from highest address (top) to lowest.
//
// Stack at entry to the common stub looks like (high addresses first):
//   [pushed by CPU]  ss, useresp (only if privilege change; we ignore)
//                    eflags
//                    cs
//                    eip
//                    error_code  (real or the fake 0 our stub pushed)
//   [pushed by stub] int_no
//   [pusha]          eax, ecx, edx, ebx, esp_dummy, ebp, esi, edi
//   [pushed by stub] ds
//
// Then we push a pointer to this whole block and call the C handler.
// Our struct lists fields in the order they were pushed (last push first),
// so the struct pointer aligns with the top of the pushed data.
struct registers {
    unsigned int ds;                                      // pushed last
    unsigned int edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax; // pusha
    unsigned int int_no, err_code;                        // stub + (real or fake) CPU error
    unsigned int eip, cs, eflags;                         // pushed by CPU
};

// Called from the common assembly stub. Prints a diagnostic and halts.
void isr_handler(struct registers* regs);

#endif