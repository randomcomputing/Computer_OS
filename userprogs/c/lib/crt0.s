; crt0.s — 64-bit C runtime startup for Computer_OS user programs.

[BITS 64]

%define SYS_EXIT 0

section .text.start
global _start
extern main

_start:
    xor rbp, rbp

    ; int main(int argc, char** argv)
    xor edi, edi        ; argc = 0
    xor esi, esi        ; argv = NULL
    call main

    ; exit(main_return)
    mov rbx, rax
    mov rax, SYS_EXIT
    int 0x80

.hang:
    hlt
    jmp .hang