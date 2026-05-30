; syscall_asm.asm — 64-bit syscall entry
;
; We keep int 0x80 for now (same as 32-bit), which keeps the user-space
; ABI unchanged and means syscall.c needs no changes. A later pass can
; swap to the SYSCALL/SYSRET fast path.
;
; On entry from ring 3 (int 0x80, DPL=3 gate):
;   CPU pushes: ss, rsp, rflags, cs, rip  (40 bytes — always in 64-bit)
;   We push: err_code (fake 0), int_no (0x80), then all GP regs.
;
; The C handler receives a pointer to struct registers (rdi = rsp).
; It can write the return value into regs->rax; we restore rax from there.

[BITS 64]

extern syscall_handler

global syscall_stub
syscall_stub:
    push qword 0          ; fake error code
    push qword 0x80       ; vector number

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

    mov  rdi, rsp         ; first arg = pointer to saved state
    call syscall_handler

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
    pop rax               ; syscall return value (written by C handler)

    add rsp, 16           ; drop int_no and err_code
    iretq