; hello.asm — sample user program for the `run` command.
;
; Build:  nasm -f bin -o hello.bin hello.asm
;
; Drop the resulting hello.bin onto the FAT12 disk image alongside
; HELLO.TXT and friends, then from the shell:
;
;   > run hello.bin
;
; Calls SYS_WRITE to print a message, then SYS_EXIT to clean up.

[BITS 32]
[ORG 0x01000000]    ; must match LOADER_CODE_VIRT in include/loader.h

%define SYS_EXIT  0
%define SYS_WRITE 1

_start:
    mov eax, SYS_WRITE
    mov ebx, 1                    ; fd = stdout
    mov ecx, msg
    mov edx, msg_len
    int 0x80

    mov eax, SYS_EXIT
    mov ebx, 0                    ; exit code
    int 0x80

    ; Should never reach here, but if SYS_EXIT ever fails to terminate
    ; the task we'd return into garbage on the stack and triple-fault.
    ; Spin instead.
.hang:
    jmp .hang

msg:    db 'Hello World', 10
msg_len equ $ - msg