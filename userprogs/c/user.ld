/*
 * user.ld — 64-bit user program linker script.
 *
 * User programs are loaded at 0x01000000 and run in long mode ring 3.
 */

ENTRY(_start)

SECTIONS
{
    . = 0x01000000;

    .text : ALIGN(0x1000) {
        *(.text.start)
        *(.text)
        *(.text.*)
    }

    .rodata : ALIGN(0x1000) {
        *(.rodata)
        *(.rodata.*)
    }

    .data : ALIGN(0x1000) {
        *(.data)
        *(.data.*)
    }

    .bss : ALIGN(0x1000) {
        __bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        __bss_end = .;
    }

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame)
        *(.eh_frame_hdr)
    }
}