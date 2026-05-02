OUTPUT_FORMAT("binary")

/*
 * Higher-half kernel layout.
 *
 * The bootloader loads the kernel image as a flat binary at physical 0x10000.
 * But we want kernel code and data to live at virtual 0xC0010000 once paging
 * is on. The trick: AT(...) sets the *load* address (where the image goes in
 * the binary file / where the bootloader places it in RAM), while `. = ...`
 * sets the *virtual* address the linker uses when resolving symbols and
 * fixing up absolute references.
 *
 * So every call/jump in the kernel resolves to a 0xC00..... address, but the
 * bytes themselves sit at physical 0x10000. Before we turn paging on, any
 * code that executes must not depend on those symbols being reachable — that
 * is entry.asm's job: it runs with paging off, builds a page directory that
 * maps 0xC0000000..0xC0400000 -> 0x00000000..0x00400000 (and also keeps the
 * low region identity-mapped so the EIP remains valid the instant after PG
 * is set), enables paging, then jumps to a high-half label.
 */

KERNEL_VMA = 0xC0000000;
KERNEL_LMA = 0x00010000;

SECTIONS
{
    . = KERNEL_VMA + KERNEL_LMA;

    .text : AT(KERNEL_LMA) {
        *(.text.boot)       /* entry.asm's bootstrap — runs before paging */
        *(.text*)
    }

    .rodata : {
        *(.rodata*)
    }

    .data : {
        *(.data*)
    }

    .bss : {
        bss_start = .;
        *(.bss*)
        *(COMMON)
        bss_end = .;
    }

    /* End of the kernel image as seen by the PMM when placing its bitmap.
       Virtual form for code that wants to write to kernel_end (the bitmap
       lives right after the kernel). Physical form for code that wants to
       reserve the kernel's physical footprint. */
    kernel_end      = .;
    kernel_phys_end = . - KERNEL_VMA;

    /* Drop sections that don't belong in a flat binary. Without this,
       OUTPUT_FORMAT("binary") will lay them out at file offsets matching
       their VMAs, but they sit between .rodata and .data — pushing .data
       in the on-disk image out of sync with where the linker says .data
       lives in memory. The .eh_frame section is the worst offender:
       GCC emits it for exception unwinding (which a kernel doesn't use)
       and it's typically a few KB. -fno-asynchronous-unwind-tables in
       CFLAGS suppresses most of these at compile time; this rule is the
       safety net for any that slip through. */
    /DISCARD/ : {
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.note.*)
        *(.comment)
        *(.gnu.build.attributes)
    }
}