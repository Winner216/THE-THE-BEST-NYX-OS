/* user/chess/chess.ld — linker script for the CHESS inner image.
 *
 * Same layout contract as user/doom/doom.ld: the image is linked at
 * its FINAL runtime address (Nyx's PE loader maps sections at
 * USER_LINK_BASE = 0xC1000000 + RVA, and elf2pe.py puts the flat image
 * at RVA 0x1000).  Linking .text at 0xC1001000 needs no base relocs.
 *
 * Entry is nyx_pe_entry in section .text.entry, placed first so the PE
 * entry RVA is exactly 0x1000.
 */
ENTRY(nyx_pe_entry)

SECTIONS {
    . = 0xC1001000;

    .text : {
        KEEP(*(.text.entry))
        *(.text*)
        *(.rodata*)
    }
    .data : {
        *(.data*)
        *(.bss*)
        *(COMMON)
    }
    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
        *(.rel*)
    }
}
