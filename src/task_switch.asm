[BITS 32]

; void task_switch(unsigned int* old_esp_ptr, unsigned int new_esp);
;
;   [esp+4] = old_esp_ptr
;   [esp+8] = new_esp
;
; Save callee-saved regs, stash esp into *old_esp_ptr, load new esp,
; restore callee-saved regs, ret.
global task_switch
task_switch:
    push ebp
    push ebx
    push esi
    push edi

    mov  eax, [esp + 20]     ; old_esp_ptr  (4 saved regs + ret addr = 20)
    mov  [eax], esp
    mov  esp, [esp + 24]     ; new_esp

    pop  edi
    pop  esi
    pop  ebx
    pop  ebp
    ret


; void task_trampoline(void);
;
; Every new task "returns into" this via the fake frame built by
; task_init_stack. At entry, the top of our stack holds [entry_fn].
; We pop it, enable interrupts, call it, then call task_exit if it
; ever returns. Hand-written in asm so there's no C prologue
; touching esp before we pop.
extern task_exit
global task_trampoline
task_trampoline:
    pop  eax                 ; entry_fn
    sti
    call eax
    call task_exit
.hang:
    hlt
    jmp  .hang


; void enter_user_mode(unsigned int user_eip, unsigned int user_esp);
;
; Build the iret frame the CPU expects on a cross-privilege return and
; iret to ring 3. Used as the *entry function* of a user task — when the
; scheduler first switches to a user task, task_trampoline calls into
; this with (user_eip, user_esp) and it never returns: iret transfers
; control to ring 3.
;
; Frame the CPU pops on iret (top of stack at iret time, low addr first):
;
;     [eip]            <- ring-3 EIP we want to start at
;     [cs]             <- 0x1B = ring-3 code selector (RPL=3)
;     [eflags]         <- with IF=1 so interrupts come on after iret
;     [esp]            <- ring-3 stack pointer
;     [ss]             <- 0x23 = ring-3 data selector (RPL=3)

global enter_user_mode
enter_user_mode:
    mov  eax, [esp + 4]      ; user_eip
    mov  ecx, [esp + 8]      ; user_esp

    ; Reload data segs to ring-3 data. ss is reloaded by the iret itself.
    mov  bx, 0x23
    mov  ds, bx
    mov  es, bx
    mov  fs, bx
    mov  gs, bx

    push 0x23                ; user SS
    push ecx                 ; user ESP
    pushf
    pop  edx
    or   edx, 0x200          ; IF=1
    push edx                 ; EFLAGS
    push 0x1B                ; user CS
    push eax                 ; user EIP
    iret