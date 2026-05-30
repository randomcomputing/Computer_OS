; isr.asm — 64-bit exception stubs
;
; In 64-bit mode:
;   - pushad/popad are GONE; we push/pop each register individually
;   - The CPU always pushes SS:RSP on an interrupt (no more conditional
;     on privilege level — the stack frame is always 40 bytes)
;   - Segment registers DS/ES/FS/GS are mostly ignored in 64-bit mode
;     (CS and SS still matter for privilege, but DS/ES are flat zero-base)
;   - We use the System V AMD64 calling convention: first arg in rdi
;
; Stack frame on entry to the common stub (top = low address):
;   [CPU]    rip, cs, rflags, rsp, ss   (always 5 * 8 = 40 bytes)
;   [stub]   int_no (8 bytes)
;   [stub]   err_code (real or fake 0) — ALREADY pushed before int_no
;            on ERR variants, CPU pushes it before we push int_no
;   [us]     r15..rax (15 GP regs * 8 = 120 bytes)
;
; We then call isr_handler(struct registers*)  with rdi = rsp.

[BITS 64]

extern isr_handler

%macro ISR_NOERR 1
    global isr%1
isr%1:
    push qword 0          ; fake error code
    push qword %1         ; interrupt number
    jmp isr_common
%endmacro

%macro ISR_ERR 1
    global isr%1
isr%1:
    push qword %1         ; interrupt number (error code already on stack)
    jmp isr_common
%endmacro

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_NOERR 30
ISR_NOERR 31

isr_common:
    ; Save all general-purpose registers. Order must match struct registers.
    push rax
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; rdi = pointer to the saved state (struct registers*)
    mov  rdi, rsp
    call isr_handler

    ; Restore registers in reverse order.
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx
    pop rax

    add rsp, 16           ; drop int_no and err_code
    iretq