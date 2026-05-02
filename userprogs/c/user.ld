/*
 * user.ld — linker script for Computer_OS ELF32 user programs.
 *
 * Lays out .text, .rodata, .data, and .bss starting at 0x01000000
 * (LOADER_CODE_VIRT in include/loader.h).  The kernel ELF loader
 * parses the resulting PT_LOAD segments and maps each one at its
 * p_vaddr with the appropriate R/W permissions.
 *
 * Key invariants:
 *  - Entry symbol _start lives at 0x01000000.  crt0.s places _start
 *    in .text.start, which we emit before the rest of .text.
 *  - .bss is flagged in the PT_LOAD segment as memsz > filesz.  The
 *    loader zeros the gap, so uninitialized globals are zero.
 *  - Total program size should stay under LOADER_CODE_BYTES (64 KB).
 *    Bump LOADER_CODE_PAGES in loader.h if you need more.
 */

ENTRY(_start)

SECTIONS
{
    . = 0x01000000;

    /* crt0 puts the entry point in .text.start so it lands at offset 0. */
    .text : ALIGN(4) {
        *(.text.start)
        *(.text)
        *(.text.*)
    }

    .rodata : ALIGN(4) {
        *(.rodata)
        *(.rodata.*)
    }

    .data : ALIGN(4) {
        *(.data)
        *(.data.*)
    }

    /* .bss is NOT included in the file image (memsz > filesz in the
     * PT_LOAD segment).  The ELF loader zeros the gap explicitly. */
    .bss : ALIGN(4) {
        __bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        __bss_end = .;
    }

    /* Drop everything the loader doesn't map (debug info, eh_frame, etc.). */
    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame)
        *(.eh_frame_hdr)
    }
}