/* user/doom/doom.ld — linker script for the DOOM inner image.
 *
 * The image is linked at its FINAL runtime address: Nyx's PE loader
 * (cpu/peload.c) maps sections at USER_LINK_BASE + RVA with
 * USER_LINK_BASE = 0xC1000000, and tools/elf2pe.py puts the flat image
 * at RVA 0x1000.  Linking .text at 0xC1001000 therefore needs NO base
 * relocations — the loader's empty reloc directory is honored.
 *
 * The WAD byte array (nyx_wad.h, ~4 MB) lands in .rodata inside .text.
 * The entry symbol is nyx_pe_entry (nyx_doom.c), placed via its
 * .text.entry section at the very start of .text so the PE entry RVA
 * is exactly 0x1000.
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
    }
}
