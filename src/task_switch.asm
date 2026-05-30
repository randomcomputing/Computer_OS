; task_switch.asm — 64-bit context switch
;
; void task_switch(uint64_t* old_rsp_ptr, uint64_t new_rsp);
;   rdi = old_rsp_ptr
;   rsi = new_rsp
;
; System V AMD64 callee-saved regs: rbp, rbx, r12–r15.
; We save those plus the return address (implicit in the call) by just
; pushing them; the stack pointer IS the context.

[BITS 64]

global task_switch
task_switch:
    ; Save callee-saved registers.
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15

    ; Stash current RSP into *old_rsp_ptr.
    mov  [rdi], rsp

    ; Load new stack.
    mov  rsp, rsi

    ; Restore callee-saved registers of the incoming task.
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret


; void task_trampoline(void)
; Every new kernel task "returns" into this via the fake frame on its
; initial stack. The entry function pointer is at [rsp].
extern task_exit
global task_trampoline
task_trampoline:
    pop  rax                 ; entry_fn
    sti
    call rax
    call task_exit
.hang:
    hlt
    jmp  .hang


; void enter_user_mode(uint64_t user_rip, uint64_t user_rsp)
;   rdi = user_rip
;   rsi = user_rsp
;
; Build a 64-bit iretq frame and switch to ring 3.
; iretq frame (top of stack = lowest address):
;   rip, cs, rflags, rsp, ss
;
; User CS = 0x23 (GDT selector 4, RPL=3)
; User SS = 0x1B (GDT selector 3, RPL=3)
; (Exact values depend on your GDT layout — adjust if needed.)

global enter_user_mode
enter_user_mode:
    ; rdi = user_rip, rsi = user_rsp — save them before we clobber regs.
    mov  rax, rdi            ; user_rip
    mov  rcx, rsi            ; user_rsp

    push 0x1B                ; user SS   (ring-3 data, RPL=3)
    push rcx                 ; user RSP
    pushfq
    pop  rdx
    or   rdx, 0x200          ; enable interrupts (IF=1) in user RFLAGS
    push rdx                 ; RFLAGS
    push 0x23                ; user CS   (ring-3 code, RPL=3)
    push rax                 ; user RIP
    iretq


; void enter_user_resume(struct registers* regs)
;   rdi = pointer to saved register frame (struct registers)
;
; Restores a full ring-3 context that was saved when the task entered the
; kernel (fork child path). The struct layout must match isr_common's push
; order (see include/isr.h).
global enter_user_resume
enter_user_resume:
    ; Load the iretq frame fields from the struct.
    ; Offsets into struct registers (see include/isr.h):
    ;   r15=0 r14=8 r13=16 r12=24 r11=32 r10=40 r9=48 r8=56
    ;   rdi=64 rsi=72 rbp=80 rbx=88 rdx=96 rcx=104 rax=112
    ;   int_no=120 err_code=128
    ;   rip=136 cs=144 rflags=152 rsp=160 ss=168

    mov  rbp, rdi            ; base pointer into struct

    ; Build iretq frame on stack.
    push qword [rbp + 168]   ; ss
    push qword [rbp + 160]   ; rsp
    push qword [rbp + 152]   ; rflags
    push qword [rbp + 144]   ; cs
    push qword [rbp + 136]   ; rip

    ; Restore GP registers. rdi (rbp+64) must come last since rbp is our
    ; struct pointer — restore rbp second-to-last, rdi absolutely last.
    mov  r15, [rbp + 0]
    mov  r14, [rbp + 8]
    mov  r13, [rbp + 16]
    mov  r12, [rbp + 24]
    mov  r11, [rbp + 32]
    mov  r10, [rbp + 40]
    mov  r9,  [rbp + 48]
    mov  r8,  [rbp + 56]
    mov  rsi, [rbp + 72]
    mov  rbx, [rbp + 88]
    mov  rdx, [rbp + 96]
    mov  rcx, [rbp + 104]
    mov  rax, [rbp + 112]    ; fork sets this to 0 in child's frame
    mov  rdi, [rbp + 64]
    mov  rbp, [rbp + 80]     ; last: restore user rbp

    iretq