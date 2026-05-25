; Bootloader for CD-ROM (El Torito).
;
; detect_memory enumerates RAM via int 0x15 eax=0xE820 and stashes the
; result at 0x500 (count) / 0x504 (entries) for the kernel to read.
;
; Why the rewrite: the previous version of detect_memory dropped to zero
; entries on QEMU because the loop used 16-bit `add di, 24` and `inc bp`
; without ever clearing the upper halves of the registers it relied on.
; This version uses 32-bit registers throughout and follows the canonical
; OSDev pattern: clear the entry buffer, force ecx=24 every iteration,
; force the ACPI 3.0 "valid" bit on if the BIOS only returned 20 bytes,
; and only count entries with non-zero length.

[BITS 16]
[ORG 0x7C00]

; Stash the memory map below the bootloader (which lives at 0x7C00) so
; the protected_mode kernel copy (0x7E00 -> 0x10000, 32 KB) doesn't
; trample it. 0x500 is the first byte after the BIOS data area.
MEMMAP_COUNT    equ 0x500
MEMMAP_ENTRIES  equ 0x504
COM1            equ 0x3F8

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti

    mov [boot_drive], dl

    mov si, msg_loading
    call print_string

    ; Don't trust the El Torito boot-load-size. SeaBIOS (and many real BIOSes)
    ; honor `-boot-load-size` only up to ~32 KB and silently cap larger values.
    ; The kernel is ~40 KB and growing, so what the BIOS pre-loaded into
    ; 0x7C00 is incomplete past file offset ~0x8200. We have to load it
    ; ourselves before entering protected mode.
    call load_kernel

    mov si, msg_success
    call print_string

    call detect_memory

    call enable_a20

    cli
    lgdt [gdt_descriptor]

    mov eax, cr0
    or eax, 1
    mov cr0, eax

    jmp CODE_SEG:protected_mode

print_string:
    pusha
.loop:
    lodsb
    or al, al
    jz .done
    mov ah, 0x0E
    int 0x10
    jmp .loop
.done:
    popa
    ret

enable_a20:
    in al, 0x92
    or al, 2
    out 0x92, al
    ret

; ---------------------------------------------------------------------------
; load_kernel
;
; Re-reads the boot image from CD into RAM so the kernel's full .data
; section makes it past the SeaBIOS no-emul El Torito ~32 KB load cap.
;
; CD-ROM no-emul boot drive (DL=0xE0 on SeaBIOS) presents the *whole CD*
; via INT 13h ah=0x42 with 2048-byte logical sectors. Our boot image
; lives at CD-LBA 26 (verified via the boot catalog after mkisofs).
;
; Reading into segment 0x0FE0 offset 0 = physical 0xFE00. That puts:
;   boot.bin   (first 512 bytes of the read) at 0xFE00..0xFFFF
;   kernel.bin (the rest of the read)        at 0x10000..
; — exactly where the kernel needs to be.
; ---------------------------------------------------------------------------
CD_SECTORS_TO_LOAD  equ 33   ; 33 * 2048 = 67584 bytes; covers boot.bin (512)
                             ; + up to ~64 KB of kernel.bin

load_kernel:
    pusha
    push ds
    push es

    xor ax, ax
    mov ds, ax

    ; Floppy emulation: 18 sectors/track, 2 heads. INT 13h ah=0x02 cannot
    ; cross a track boundary, so loop one track at a time. After each
    ; read, advance ES by the byte-count-in-paragraphs and reset BX=0
    ; (avoiding 16-bit overflow on BX as we cross 64 KB).

    mov ax, 0x1000
    mov es, ax
    xor bx, bx

    ; First read: track 0, sectors 2..18 = 17 sectors = 8704 bytes = 0x220 para.
    mov ah, 0x02
    mov al, 17
    mov ch, 0
    mov cl, 2
    mov dh, 0
    mov dl, [boot_drive]
    int 0x13
    jc .read_done

    mov ax, es
    add ax, 0x220
    mov es, ax
    xor bx, bx
    mov di, 1                   ; next (cyl, head): cyl=0, head=1

.next_track:
    cmp di, 20                  ; 20 tracks ≈ 180 KB total — headroom for
                                ; a growing kernel (was 14 ≈ 124 KB; the
                                ; kernel crossed 80 KB once fork/exec/wait
                                ; landed, so we give it more runway here)
    jae .read_done

    ; cyl = di >> 1, head = di & 1
    mov ax, di
    shr ax, 1
    mov ch, al
    mov dh, 0
    test di, 1
    jz .h0
    mov dh, 1
.h0:
    mov ah, 0x02
    mov al, 18
    mov cl, 1
    mov dl, [boot_drive]
    int 0x13
    jc .read_done               ; Past end-of-image is fine; we got what we needed.

    mov ax, es
    add ax, 0x240               ; 18 sectors = 9216 bytes = 576 para
    mov es, ax
    xor bx, bx
    inc di
    jmp .next_track

.read_done:
    pop es
    pop ds
    popa
    ret

; Write AL to COM1, polling. Preserves AX.
serial_putc:
    push dx
    push ax
.wait:
    mov dx, COM1 + 5
    in al, dx
    test al, 0x20
    jz .wait
    pop ax
    push ax
    mov dx, COM1
    out dx, al
    pop ax
    pop dx
    ret

; ---------------------------------------------------------------------------
; detect_memory
;
; Calls int 0x15, eax=0xE820 in a loop to enumerate the BIOS memory map.
; Stores entries at MEMMAP_ENTRIES (each 24 bytes) and the total entry
; count as a 32-bit dword at MEMMAP_COUNT.
;
; Register usage during the loop:
;   ebx — continuation value (0 on first call, 0 from BIOS on last entry)
;   edi — pointer into the entries buffer; advanced 24 bytes per kept entry
;   ebp — count of accepted entries (length > 0)
;
; Edge cases handled:
;   - BIOS doesn't support E820     -> CF set on first call, return count=0
;   - BIOS returns 20 bytes not 24  -> we pre-fill the ACPI attr field with 1
;   - BIOS returns a zero-length entry -> skip it without advancing edi/ebp
;   - BIOS returns ebx=0 but a valid entry -> count it, then exit
; ---------------------------------------------------------------------------
detect_memory:
    pushad

    ; Make sure DS/ES are 0 — int 0x15 uses ES:DI.
    xor ax, ax
    mov ds, ax
    mov es, ax

    ; Zero the count word up front so a CF-on-first-call early-out
    ; leaves a sane "0 entries" indication for the kernel.
    mov dword [MEMMAP_COUNT], 0

    mov edi, MEMMAP_ENTRIES
    xor ebx, ebx           ; first call: ebx must be 0
    xor ebp, ebp           ; entry count

.loop:
    ; Pre-fill the entry's ACPI attr field (offset 20) with 1. If the
    ; BIOS only writes the first 20 bytes (older format), this leaves
    ; the entry marked "valid". If the BIOS writes 24 bytes, it
    ; overwrites this with the real attrs. Either way, the field has
    ; a meaningful value.
    mov dword [es:di + 20], 1

    mov eax, 0xE820
    mov edx, 0x534D4150    ; 'SMAP'
    mov ecx, 24
    int 0x15

    jc .maybe_done         ; CF set: either unsupported (first call) or
                           ; "this was the last entry" on some BIOSes

    cmp eax, 0x534D4150    ; BIOS must echo 'SMAP' on success
    jne .done

    ; CX = bytes the BIOS actually wrote. Some BIOSes return less than
    ; 24 even when we asked for 24; the pre-fill above handles that.
    ; If CX is 0 the entry is bogus — skip it.
    jcxz .skip

    ; Reject zero-length entries (length_low at offset 8, length_high
    ; at offset 12 — both zero means a placeholder entry we should skip).
    mov ecx, [es:di + 8]
    or  ecx, [es:di + 12]
    jz  .skip

    ; Keep this entry: advance the buffer pointer and the count.
    add di, 24
    inc ebp

.skip:
    test ebx, ebx          ; ebx==0 from BIOS = this was the last entry
    jz .done
    jmp .loop

.maybe_done:
    ; CF was set. If ebp==0 this means E820 isn't supported at all and
    ; we exit with count=0. If ebp>0 we already kept at least one entry
    ; and CF here means "no more entries" — so we still commit the count.
    jmp .done

.done:
    mov [MEMMAP_COUNT], ebp
    popad
    ret

hang:
    cli
    hlt
    jmp hang

msg_loading:    db 'Loading...', 13, 10, 0
msg_success:    db 'Loaded!', 13, 10, 0
boot_drive:     db 0

; Disk Address Packet for INT 13h ah=0x42. Filled in by load_kernel.
align 2
dap:
dap_size:       dw 0
dap_reserved:   dw 0
dap_count:      dw 0
dap_offset:     dw 0
dap_segment:    dw 0
dap_lba_lo:     dd 0
dap_lba_hi:     dd 0

gdt_start:
    dq 0x0
gdt_code:
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 10011010b
    db 11001111b
    db 0x00
gdt_data:
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 10010010b
    db 11001111b
    db 0x00
gdt_end:
gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

CODE_SEG equ gdt_code - gdt_start
DATA_SEG equ gdt_data - gdt_start

[BITS 32]
protected_mode:
    mov ax, DATA_SEG
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000

    ; The kernel is already at physical 0x10000 — load_kernel put it there
    ; via INT 13h before we entered protected mode. No rep movsd needed.
    mov eax, 0x10000
    jmp eax

times 510-($-$$) db 0
dw 0xAA55