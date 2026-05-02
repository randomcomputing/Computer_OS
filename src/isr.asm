[BITS 32]

extern isr_handler

; Exceptions 8, 10..14, 17 push an error code onto the stack themselves.
; The others don't — we push a fake 0 so the C handler always sees the
; same stack layout.

%macro ISR_NOERR 1
    global isr%1
isr%1:
    cli
    push dword 0          ; fake error code
    push dword %1         ; interrupt number
    jmp isr_common
%endmacro

%macro ISR_ERR 1
    global isr%1
isr%1:
    cli
    push dword %1         ; interrupt number (error code already on stack)
    jmp isr_common
%endmacro

ISR_NOERR 0     ; Divide-by-zero
ISR_NOERR 1     ; Debug
ISR_NOERR 2     ; NMI
ISR_NOERR 3     ; Breakpoint
ISR_NOERR 4     ; Overflow
ISR_NOERR 5     ; Bound range exceeded
ISR_NOERR 6     ; Invalid opcode
ISR_NOERR 7     ; Device not available
ISR_ERR   8     ; Double fault
ISR_NOERR 9     ; Coprocessor segment overrun (legacy)
ISR_ERR   10    ; Invalid TSS
ISR_ERR   11    ; Segment not present
ISR_ERR   12    ; Stack-segment fault
ISR_ERR   13    ; General protection fault
ISR_ERR   14    ; Page fault
ISR_NOERR 15    ; Reserved
ISR_NOERR 16    ; x87 floating-point exception
ISR_ERR   17    ; Alignment check
ISR_NOERR 18    ; Machine check
ISR_NOERR 19    ; SIMD floating-point exception
ISR_NOERR 20    ; Virtualization exception
ISR_ERR   21    ; Control protection exception
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

; Common handler: save CPU state, call C, restore state, iret.
isr_common:
    pusha                 ; edi,esi,ebp,esp,ebx,edx,ecx,eax
    mov ax, ds
    push eax              ; save data segment

    mov ax, 0x10          ; kernel data segment
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    mov eax, esp          ; pointer to saved state
    push eax              ; pass to C handler
    call isr_handler
    add esp, 4

    pop eax               ; restore data segment
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    popa
    add esp, 8            ; drop int_no and error_code
    sti
    iret