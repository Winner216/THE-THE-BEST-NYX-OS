/* linker64.ld — 64-bit linker script for Nyx OS
 *
 * The 64-bit kernel is linked at physical 0x200000, above the
 * 32-bit Multiboot stub (which lives at 0x100000).  The boot stub
 * copies the kernel binary to 0x200000 and jumps to the entry point.
 */

ENTRY(kernel_main64)

SECTIONS {
    /* 64-bit kernel starts at physical 0x200000 */
    . = 0x200000;

    .text BLOCK(4K) : ALIGN(4K) {
        *(.text)
    }

    .rodata BLOCK(4K) : ALIGN(4K) {
        *(.rodata)
    }

    .data BLOCK(4K) : ALIGN(4K) {
        *(.data)
    }

    .bss BLOCK(4K) : ALIGN(4K) {
        *(COMMON)
        *(.bss)
    }

    . = ALIGN(4K);
    _end = .;
}
