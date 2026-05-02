[BITS 32]

extern syscall_handler

; int 0x80 entry point.
;
; On entry from ring 3, the CPU has already:
;   - switched ESP to tss.esp0 (kernel stack)
;   - pushed (in order): user SS, user ESP, EFLAGS, user CS, return EIP
;
; We then push a fake error code and the int number, pusha, save DS,
; load kernel data segments, hand the C handler a pointer to the saved
; state, then unwind exactly the way ISR/IRQ stubs do. We deliberately
; reuse the same `struct registers` layout used by ISRs so the syscall
; handler can read args from regs->ebx, regs->ecx, etc., and write its
; return value back into regs->eax — popa restores eax from the stack
; we wrote, so the user program sees the return value when it resumes.

global syscall_stub
syscall_stub:
    cli
    push dword 0          ; fake error code (layout uniformity)
    push dword 0x80       ; vector number

    pusha                 ; edi,esi,ebp,esp_dummy,ebx,edx,ecx,eax
    mov ax, ds
    push eax              ; saved DS

    mov ax, 0x10          ; kernel data segment (GDT_KDATA)
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    mov eax, esp          ; pointer to registers struct
    push eax
    call syscall_handler
    add esp, 4

    pop eax               ; restore caller's DS
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    popa                  ; restores eax with whatever C handler wrote
    add esp, 8            ; drop int_no and error_code
    iret                  ; back to userspace; CPU pops EIP/CS/EFLAGS/ESP/SS