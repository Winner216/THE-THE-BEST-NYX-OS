/* user/user.ld – linker script for ring-3 user programs.
 *
 * Links every program at USER_LINK_BASE (0xC1000000) — the virtual
 * address the kernel's ELF loader (cpu/elfload.c) maps PT_LOAD
 * segments at.  Sections are laid out contiguously so the single
 * LOAD segment starts at the base; _start is the entry point (the
 * user programs are freestanding, no crt0). */
ENTRY(_start)

SECTIONS {
    . = 0xC1000000;

    .text : {
        *(.text*)
    }
    .rodata : {
        *(.rodata*)
    }
    .data : {
        *(.data*)
    }
    .bss : {
        *(.bss*)
        *(COMMON)
    }
}
