; count.asm — counts from 0 to 9 with a syscall per digit, then exits.
;
; Demonstrates: (1) a simple loop in user space, (2) repeated syscalls
; back-to-back, (3) computing a buffer address relative to the link base.

[BITS 32]
[ORG 0x01000000]

%define SYS_EXIT  0
%define SYS_WRITE 1

_start:
    mov ebx, 1                    ; fd = stdout (kept across syscalls
                                  ; because the kernel only touches eax)
    mov esi, 0                    ; counter

.loop:
    cmp esi, 10
    jge .done

    ; Render the digit into our 1-byte buffer.
    mov eax, esi
    add eax, '0'
    mov [digit], al

    mov eax, SYS_WRITE
    mov ecx, digit
    mov edx, 1
    int 0x80

    inc esi
    jmp .loop

.done:
    ; Trailing newline.
    mov eax, SYS_WRITE
    mov ebx, 1
    mov ecx, newline
    mov edx, 1
    int 0x80

    mov eax, SYS_EXIT
    mov ebx, 0
    int 0x80

.hang:
    jmp .hang

digit:   db 0
newline: db 10