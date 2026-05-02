[BITS 32]

extern irq_handler

; IRQ stubs look a lot like ISR stubs, but:
;   - there's no CPU error code, so we always push a fake 0
;   - the common tail calls irq_handler (not isr_handler)
;   - after handling, we send EOI (done in C) before iret

%macro IRQ 2
    global irq%1
irq%1:
    cli
    push dword 0          ; fake error code (for layout uniformity)
    push dword %2         ; interrupt vector (32..47)
    jmp irq_common
%endmacro

IRQ 0,  32       ; Timer
IRQ 1,  33       ; Keyboard
IRQ 2,  34       ; Cascade (never raised)
IRQ 3,  35       ; COM2
IRQ 4,  36       ; COM1
IRQ 5,  37       ; LPT2
IRQ 6,  38       ; Floppy
IRQ 7,  39       ; LPT1 / spurious
IRQ 8,  40       ; RTC
IRQ 9,  41
IRQ 10, 42
IRQ 11, 43
IRQ 12, 44       ; PS/2 mouse
IRQ 13, 45       ; FPU
IRQ 14, 46       ; Primary ATA
IRQ 15, 47       ; Secondary ATA

irq_common:
    pusha
    mov ax, ds
    push eax
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    mov eax, esp
    push eax
    call irq_handler
    add esp, 4

    pop eax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    popa
    add esp, 8
    sti
    iret
