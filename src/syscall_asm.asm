; syscall_asm.asm — dual syscall entry points

[BITS 64]

extern syscall_handler
extern linux_syscall_handler

; -----------------------------------------------------------------------
; int 0x80 path (legacy Computer OS userprogs)
; -----------------------------------------------------------------------
global syscall_stub
syscall_stub:
    push qword 0
    push qword 0x80
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
    mov  rdi, rsp
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
    pop rax
    add rsp, 16
    iretq

; -----------------------------------------------------------------------
; syscall fast path (Linux x86-64)
;
; Frame layout (rsp = r9 slot after pushes):
;   [rsp+ 0] = r9   (arg6)
;   [rsp+ 8] = r8   (arg5)
;   [rsp+16] = r10  (arg4)
;   [rsp+24] = rdx  (arg3)
;   [rsp+32] = rsi  (arg2)
;   [rsp+40] = rdi  (arg1)
;   [rsp+48] = rax  (syscall# → retval)
;   [rsp+56] = rcx  (user rip)
;   [rsp+64] = r11  (user rflags)
;   [rsp+72] = rbx  (user rbx)   ← NEW: saved here for explicit restore
;   [rsp+80] = user_rsp
; -----------------------------------------------------------------------
global linux_syscall_entry
global g_syscall_kernel_rsp

section .data
align 8
g_syscall_kernel_rsp: dq 0
g_syscall_user_rsp:   dq 0
g_syscall_num:        dq 0

section .text

linux_syscall_entry:
    mov  [rel g_syscall_user_rsp], rsp
    mov  rsp, [rel g_syscall_kernel_rsp]
    mov  [rel g_syscall_num], rax

    ; Push frame (linux_frame_t) — rbx added at +72, user_rsp moved to +80
    push qword [rel g_syscall_user_rsp]  ; user rsp  [+80]
    push rbx                             ; user rbx  [+72]  ← save user rbx
    push r11                             ; user rflags[+64]
    push rcx                             ; user rip   [+56]
    push rax                             ; syscall num[+48]
    push rdi                             ; arg1       [+40]
    push rsi                             ; arg2       [+32]
    push rdx                             ; arg3       [+24]
    push r10                             ; arg4       [+16]
    push r8                              ; arg5       [+8]
    push r9                              ; arg6       [+0]

    ; Save kernel callee-saved regs (NOT in frame)
    push rbp
    push rbx                             ; kernel rbx (not user rbx)
    push r12
    push r13
    push r14
    push r15

    ; frame pointer = rsp + 48 (skip 6 callee regs)
    lea  rdi, [rsp + 48]
    call linux_syscall_handler

    ; Restore kernel callee-saved regs
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx                              ; restore kernel rbx
    pop rbp

    ; Load return value
    mov  rax, [rsp + 48]

    ; Check for exit
    mov  r11, [rel g_syscall_num]
    cmp  r11, 60
    je   linux_do_exit_halt
    cmp  r11, 231
    je   linux_do_exit_halt

    ; Pop arg regs
    pop r9
    pop r8
    pop r10
    pop rdx
    pop rsi
    pop rdi
    add rsp, 8           ; skip rax
    pop rcx              ; user rip  → sysretq uses rcx as rip
    pop r11              ; user rflags → sysretq uses r11 as rflags
    pop rbx              ; user rbx
    pop rsp              ; user rsp  → restore user stack directly!

    ; sysretq: returns to rcx (rip), restoring r11→rflags
    ; All other registers (rbx, etc.) are preserved as-is
    o64 sysret

linux_do_exit_halt:
    sti
    hlt
    jmp  linux_do_exit_halt