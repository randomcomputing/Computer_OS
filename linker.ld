/*
 * 64-bit higher-half kernel linker script.
 *
 * Limine loads our ELF64 kernel and relocates it. We ask for the kernel
 * to be mapped at the standard higher-half base (0xFFFFFFFF80000000),
 * but Limine handles the actual page-table setup and passes us the HHDM
 * offset via the Limine requests API. We just need correct VMA placement
 * and proper section alignment.
 *
 * The physical load address is chosen by Limine (it puts the kernel
 * somewhere sensible above 1 MB). We don't need an AT() trick here
 * because Limine gives us an ELF-aware loader — symbols resolve to the
 * higher-half VMAs and Limine builds the mappings to match.
 */

KERNEL_VMA = 0xFFFFFFFF80000000;

ENTRY(kernel_entry)

SECTIONS
{
    . = KERNEL_VMA;

    .text ALIGN(4096) : {
        *(.text.boot)
        *(.text*)
    }

    .rodata ALIGN(4096) : {
        /* Limine request structures must be in a special section so the
           bootloader can find them by scanning the ELF. */
        KEEP(*(.limine_requests))
        *(.rodata*)
    }

    .data ALIGN(4096) : {
        *(.data*)
    }

    .bss ALIGN(4096) : {
        bss_start = .;
        *(.bss*)
        *(COMMON)
        bss_end = .;
    }

    kernel_end      = .;
    /* Physical end: subtract the VMA base to get physical offset from
       where Limine loaded us. We use this for PMM reservations. */
    kernel_phys_end = . - KERNEL_VMA;

    /DISCARD/ : {
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.note.*)
        *(.comment)
        *(.gnu.build.attributes)
    }
}