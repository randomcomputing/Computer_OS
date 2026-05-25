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


; void enter_user_resume(struct registers* regs);
;
; Resume a ring-3 task from a *complete* saved register frame, rather
; than from a fresh (eip, esp) pair. This is what a forked child uses:
; the child must re-emerge from `int 0x80` at exactly the instruction
; the parent did, with identical register state EXCEPT eax (which the
; fork syscall sets to 0 in the child's copy of the frame before this
; runs). enter_user_mode can't do that — it only knows an entry point.
;
; The `struct registers` layout (see include/isr.h) from low address up:
;     ds, edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax,
;     int_no, err_code, eip, cs, eflags, [useresp, ss]
;
; The last two (useresp, ss) are present because the original int 0x80
; came from ring 3, so the CPU pushed them on the privilege change. The
; fork path captures them too (the frame we're handed is 16 bytes longer
; than a same-ring frame). We rebuild the exact iret frame by hand.
;
; We do NOT pusha/popa here; we load each register explicitly from the
; struct so we have full control, then iret. Interrupts are off on entry
; (the shim runs with cli) and come back on via the restored EFLAGS.
global enter_user_resume
enter_user_resume:
    mov  eax, [esp + 4]      ; regs pointer (struct registers*)
    mov  ebp, eax            ; keep base pointer to the struct in ebp

    ; Reload ring-3 data segments. (The saved ds in the struct is the
    ; user ds = 0x23; load it directly to be faithful.)
    mov  bx, [ebp + 0]       ; regs->ds  (low 16 bits)
    mov  ds, bx
    mov  es, bx
    mov  fs, bx
    mov  gs, bx

    ; Build the iret frame: ss, useresp, eflags, cs, eip (low addr last).
    ; Offsets into struct registers:
    ;   eip=44 cs=48 eflags=52 useresp=56 ss=60
    push dword [ebp + 60]    ; user SS
    push dword [ebp + 56]    ; user ESP (useresp)
    push dword [ebp + 52]    ; EFLAGS (already has IF=1 from original trap)
    push dword [ebp + 48]    ; user CS
    push dword [ebp + 44]    ; user EIP

    ; Restore general-purpose registers from the struct. Load eax LAST
    ; via a scratch so we don't clobber ebp (our struct pointer) early.
    ; Offsets: edi=4 esi=8 ebp=12 ebx=20 edx=24 ecx=28 eax=32
    mov  edi, [ebp + 4]
    mov  esi, [ebp + 8]
    mov  ebx, [ebp + 20]
    mov  edx, [ebp + 24]
    mov  ecx, [ebp + 28]
    mov  eax, [ebp + 32]     ; child's eax (fork set this to 0)
    mov  ebp, [ebp + 12]     ; finally restore user ebp

    iret