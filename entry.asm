[BITS 32]

;; -----------------------------------------------------------------------
;; Higher-half bootstrap.
;;
;; When the bootloader hands us control, paging is off, we're at physical
;; EIP = 0x10000, and the linker has already fixed up absolute symbols
;; assuming we're running at 0xC0010000. So while we're in this bootstrap
;; we must NOT reference any symbol by its linked (virtual) address as a
;; pointer we actually dereference or jump to — we'd chase an address that
;; isn't mapped yet and triple-fault.
;;
;; The trick used throughout: compute physical addresses on the fly by
;; subtracting KERNEL_VMA = 0xC0000000 from any symbol whose low-half
;; physical address we need. Once paging is on and the high half is
;; mapped, we do a single jmp to a high-half label and the rest of the
;; kernel runs with virtual addresses normally.
;; -----------------------------------------------------------------------

KERNEL_VMA    equ 0xC0000000
PAGE_PRESENT  equ 0x1
PAGE_WRITE    equ 0x2
PAGE_SIZE_BIT equ 0x80         ; CR4.PSE needed; sets 4 MB page in PDE

global _start
global boot_page_directory      ; vmm.c will take this over
extern kmain
extern halt
extern bss_start                ; from linker.ld
extern bss_end                  ; from linker.ld

;; .text.boot runs BEFORE paging is enabled. It's placed first in .text
;; by linker.ld so _start ends up at physical 0x10000 where boot.asm
;; jumps. Inside here, never dereference a high-half address.
section .text.boot
_start:
    ;; segment registers were set by boot.asm, but set them again
    ;; defensively (boot.asm used gdt_data = 0x10).
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ;; temporary low-half stack (same as before — boot.asm set 0x90000)
    mov esp, 0x90000

    ;; checkpoint: 'E' in top-left of VGA so we know we got here
    mov word [0xB8000], 0x0F45

    ;; ---- zero .bss ----------------------------------------------------
    ;;
    ;; In a freestanding environment, nobody zeros .bss for us. On a cold
    ;; boot it's *usually* zero (DRAM tends to read as zero after power-on
    ;; on most chipsets, and QEMU explicitly zeroes it), so the kernel
    ;; appears to work. On a warm reboot via the keyboard controller,
    ;; though, RAM is preserved — every static variable still holds its
    ;; value from the previous boot, including ring-buffer head/tail
    ;; pointers, scrollback indices, mouse packet state, the boot page
    ;; directory itself, etc. That's a recipe for a crash on the second
    ;; boot. Zero it explicitly here so warm and cold boots look the same.
    ;;
    ;; bss_start / bss_end are linked at 0xC00xxxxx; we want their
    ;; physical addresses (paging is still off here). Subtract KERNEL_VMA.

    mov edi, bss_start - KERNEL_VMA
    mov ecx, bss_end - KERNEL_VMA
    sub ecx, edi                        ; byte count
    xor eax, eax
    cld
    rep stosb

    ;; ---- build the bootstrap page directory ---------------------------
    ;;
    ;; Two 4 MB mappings using PSE:
    ;;   PDE[0]:   0x00000000 -> 0x00000000   (identity, so EIP stays valid
    ;;                                         the instant PG is set, and
    ;;                                         low-half symbols still work
    ;;                                         through the rest of boot)
    ;;   PDE[768]: 0xC0000000 -> 0x00000000   (the high-half kernel mapping
    ;;                                         we want going forward;
    ;;                                         768 = 0xC0000000 >> 22)
    ;;
    ;; boot_page_directory lives in .bss, so its physical address is
    ;; (linked VMA) - KERNEL_VMA. We poke the two PDEs directly.

    mov edi, boot_page_directory - KERNEL_VMA
    mov ecx, 1024
    xor eax, eax
    rep stosd                           ; zero all 1024 PDEs

    mov edi, boot_page_directory - KERNEL_VMA
    mov dword [edi + 0*4],   0x00000000 | PAGE_PRESENT | PAGE_WRITE | PAGE_SIZE_BIT
    mov dword [edi + 768*4], 0x00000000 | PAGE_PRESENT | PAGE_WRITE | PAGE_SIZE_BIT

    ;; ---- enable PSE then paging --------------------------------------
    ;;
    ;; Order matters: CR4.PSE (bit 4) before CR0.PG (bit 31), otherwise
    ;; the PDEs we just wrote are interpreted as 4 KB-page PDEs and the
    ;; CPU dereferences bits 31..12 as a page-table pointer -> garbage.

    mov eax, cr4
    or  eax, 0x00000010          ; CR4.PSE
    mov cr4, eax

    mov eax, boot_page_directory - KERNEL_VMA
    mov cr3, eax

    mov eax, cr0
    or  eax, 0x80000000          ; CR0.PG
    mov cr0, eax

    ;; checkpoint: 'P' (paging on)
    mov word [0xB8002], 0x0F50

    ;; ---- jump to the high half ---------------------------------------
    ;;
    ;; EIP is still low because the instruction we're executing was fetched
    ;; from a low physical address. An absolute `jmp` to higher_half_entry
    ;; uses its linked (high) address, which the CPU now can translate via
    ;; the PDE at index 768. From the target instruction on, EIP is high.

    lea eax, [higher_half_entry]
    jmp eax


;; -----------------------------------------------------------------------
;; From here on, paging is on and EIP is a high-half address. We can
;; reference symbols by their linked virtual addresses normally.
;; -----------------------------------------------------------------------
section .text
higher_half_entry:
    ;; move the stack into the high half too, so backtraces are sane and
    ;; later unmapping of the low identity region won't pull the rug out.
    mov esp, kernel_stack_top

    ;; checkpoint: 'H' (higher half)
    mov word [0xB8004], 0x0F48

    call kmain

    ;; kmain should never return; if it does, halt.
    mov word [0xB8006], 0x0F58        ; 'X'
    call halt
.hang:
    hlt
    jmp .hang


section .bss
align 4096
boot_page_directory:
    resb 4096

align 16
kernel_stack_bottom:
    resb 16384                         ; 16 KB kernel stack
kernel_stack_top: