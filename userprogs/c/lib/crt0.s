; crt0.s — C runtime startup for Computer_OS user programs.
;
; Loaded at 0x01000000. The kernel's task_spawn_user() jumps here with:
;   - eip = 0x01000000  (this label _start)
;   - esp = top of a one-page user stack (we don't need to set up a stack)
;   - cs/ds/ss = user-mode segments (from the ring-3 trampoline)
;
; Job: call main(), then SYS_EXIT with main's return value. We don't
; bother with argc/argv yet (always pass 0/NULL); when the loader learns
; how to forward shell arguments we'll wire those up here.
;
; .bss zero-init is unnecessary — the loader zero-fills the code region
; before reading the file in, so any uninitialised globals are already
; zero by the time we get here. We still keep __bss_start/__bss_end
; symbols in the linker script so future tooling can find the bss range.

[BITS 32]

%define SYS_EXIT  0

section .text.start
global _start
extern main

_start:
    ; The loader zeroed the stack page already — nothing to do for stack
    ; setup. Just mark the bottom of the call chain so a stack trace can
    ; tell where to stop.
    xor ebp, ebp

    ; Call into C: int main(int argc, char** argv).
    ; Push args in right-to-left order, cdecl.
    push 0          ; argv = NULL
    push 0          ; argc = 0
    call main
    add esp, 8      ; clean up

    ; main returned — exit with its return value (in eax).
    mov ebx, eax    ; arg1 = exit code
    mov eax, SYS_EXIT
    int 0x80

    ; SYS_EXIT shouldn't return. If it somehow does, hang rather than
    ; falling off the end of the file into garbage.
.hang:
    hlt
    jmp .hang