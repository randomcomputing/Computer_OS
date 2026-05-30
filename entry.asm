; entry.asm — 64-bit Limine kernel entry point
;
; Limine has already:
;   - switched the CPU into long mode (64-bit)
;   - set up a flat 64-bit GDT
;   - enabled paging with the kernel mapped at KERNEL_VMA
;   - enabled the framebuffer (if requested)
;   - passed a pointer to the Limine boot info in rdi (System V ABI)
;
; Our only job here is to set up a proper kernel stack and call kmain().
; Everything else (GDT reload, IDT, PMM, VMM...) is done in C.

[BITS 64]

extern kmain

global kernel_entry
kernel_entry:
    ; Limine gives us a stack, but we want our own so its size and location
    ; are under our control. Switch to it before anything else.
    lea  rsp, [rel _kernel_stack_top]

    ; Clear the frame pointer so backtraces terminate cleanly.
    xor  rbp, rbp

    ; System V AMD64: first arg (Limine boot_info pointer) is already in rdi.
    ; kmain(void) — we ignore the Limine pointer for now; the request
    ; structures let us access everything we need from C.
    call kmain

    ; kmain should never return. If it does, halt.
.hang:
    cli
    hlt
    jmp .hang


; 16 KB kernel stack, 16-byte aligned.
section .bss
align 16
_kernel_stack_bottom:
    resb 16384
_kernel_stack_top: