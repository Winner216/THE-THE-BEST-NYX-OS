// tools/_pe_mem_test.c — Host-side test for PE memory permission mapping.
// Validates that section characteristics correctly map to page flags.
#include <stdio.h>
#include <stdint.h>
#include <string.h>

// Page flags (mirroring paging.h)
#define PAGING_P 0x001u
#define PAGING_W 0x002u
#define PAGING_U 0x004u

// Section characteristics (mirroring peload.h)
#define PE_SCN_CNT_CODE        0x00000020
#define PE_SCN_CNT_INIT_DATA   0x00000040
#define PE_SCN_CNT_UNINIT_DATA 0x00000080
#define PE_SCN_MEM_EXECUTE     0x20000000
#define PE_SCN_MEM_READ        0x40000000
#define PE_SCN_MEM_WRITE       0x80000000
#define PE_SCN_MEM_DISCARDABLE 0x02000000

// ─── Permission calculation (mirrors pe_load.c logic) ────────────
static uint32_t calc_section_flags(uint32_t characteristics) {
    int is_code = (characteristics & PE_SCN_CNT_CODE) ||
                  (characteristics & PE_SCN_MEM_EXECUTE);
    int is_writable = (characteristics & PE_SCN_MEM_WRITE);
    int is_readonly = !(characteristics & PE_SCN_MEM_WRITE) &&
                      !(characteristics & PE_SCN_CNT_UNINIT_DATA);

    if (is_code && !is_writable)
        return PAGING_U;                // R+U (code)
    else if (is_readonly && !is_code)
        return PAGING_U;                // R+U (rodata)
    else
        return PAGING_W | PAGING_U;     // RW+U (data)
}

// ─── ELF permission calculation (mirrors elfload.c logic) ───────
static uint32_t calc_elf_flags(uint32_t p_flags) {
    // p_flags: PF_X=1 PF_W=2 PF_R=4
    if ((p_flags & 1) && !(p_flags & 2))
        return PAGING_U;                // execute-only → R+U
    return PAGING_W | PAGING_U;         // default: RW+U
}

int main(void) {
    int pass = 0, fail = 0;

    // ─── PE section permission tests ─────────────────────────────
    printf("PE section permissions:\n");

    // .text: code + execute, no write → R+U
    uint32_t text_flags = calc_section_flags(0x60000020);  // CODE|EXECUTE|READ
    if (text_flags == PAGING_U) {
        printf("  [PASS] .text (code|execute|read) → R+U (0x%03X)\n", text_flags); pass++;
    } else {
        printf("  [FAIL] .text → expected R+U (0x%03X), got 0x%03X\n", PAGING_U, text_flags); fail++;
    }

    // .data: init data + write → RW+U
    uint32_t data_flags = calc_section_flags(0xC0000040);  // INIT_DATA|WRITE|READ
    if (data_flags == (PAGING_W | PAGING_U)) {
        printf("  [PASS] .data (init_data|write|read) → RW+U (0x%03X)\n", data_flags); pass++;
    } else {
        printf("  [FAIL] .data → expected RW+U (0x%03X), got 0x%03X\n", PAGING_W|PAGING_U, data_flags); fail++;
    }

    // .rdata: init data, no write → R+U
    uint32_t rdata_flags = calc_section_flags(0x40000040);  // INIT_DATA|READ
    if (rdata_flags == PAGING_U) {
        printf("  [PASS] .rdata (init_data|read) → R+U (0x%03X)\n", rdata_flags); pass++;
    } else {
        printf("  [FAIL] .rdata → expected R+U (0x%03X), got 0x%03X\n", PAGING_U, rdata_flags); fail++;
    }

    // .bss: uninit data + write → RW+U
    uint32_t bss_flags = calc_section_flags(0xC0000080);  // UNINIT_DATA|WRITE|READ
    if (bss_flags == (PAGING_W | PAGING_U)) {
        printf("  [PASS] .bss (uninit_data|write|read) → RW+U (0x%03X)\n", bss_flags); pass++;
    } else {
        printf("  [FAIL] .bss → expected RW+U (0x%03X), got 0x%03X\n", PAGING_W|PAGING_U, bss_flags); fail++;
    }

    // .text+writable: code with write flag → RW+U (self-modifying code)
    uint32_t textw_flags = calc_section_flags(0xE0000020);  // CODE|EXECUTE|READ|WRITE
    if (textw_flags == (PAGING_W | PAGING_U)) {
        printf("  [PASS] .text+writable → RW+U (0x%03X)\n", textw_flags); pass++;
    } else {
        printf("  [FAIL] .text+writable → expected RW+U, got 0x%03X\n", textw_flags); fail++;
    }

    // Discardable section → should be skipped (we test the flag, not the skip)
    uint32_t reloc_flags = calc_section_flags(0x42000040);  // DISCARDABLE|READ|INIT_DATA
    printf("  [INFO] .reloc (discardable) → flags=0x%03X (skipped by loader)\n", reloc_flags);

    // ─── ELF permission tests ────────────────────────────────────
    printf("\nELF segment permissions:\n");

    // PF_R | PF_X (read+execute, no write) → R+U
    uint32_t elf_rx = calc_elf_flags(5);  // PF_R|PF_X
    if (elf_rx == PAGING_U) {
        printf("  [PASS] ELF R+X (flags=5) → R+U (0x%03X)\n", elf_rx); pass++;
    } else {
        printf("  [FAIL] ELF R+X → expected R+U, got 0x%03X\n", elf_rx); fail++;
    }

    // PF_R | PF_W (read+write) → RW+U
    uint32_t elf_rw = calc_elf_flags(6);  // PF_R|PF_W
    if (elf_rw == (PAGING_W | PAGING_U)) {
        printf("  [PASS] ELF R+W (flags=6) → RW+U (0x%03X)\n", elf_rw); pass++;
    } else {
        printf("  [FAIL] ELF R+W → expected RW+U, got 0x%03X\n", elf_rw); fail++;
    }

    // PF_R | PF_W | PF_X (all) → RW+U (self-modifying code allowed)
    uint32_t elf_rwx = calc_elf_flags(7);  // PF_R|PF_W|PF_X
    if (elf_rwx == (PAGING_W | PAGING_U)) {
        printf("  [PASS] ELF R+W+X (flags=7) → RW+U (0x%03X)\n", elf_rwx); pass++;
    } else {
        printf("  [FAIL] ELF R+W+X → expected RW+U, got 0x%03X\n", elf_rwx); fail++;
    }

    // ─── Verify flag bit positions match x86 page table format ───
    printf("\nFlag bit positions (must match x86 PTE):\n");
    if (PAGING_P == 0x001 && PAGING_W == 0x002 && PAGING_U == 0x004) {
        printf("  [PASS] P=0x%03X W=0x%03X U=0x%03X (correct x86 layout)\n",
               PAGING_P, PAGING_W, PAGING_U); pass++;
    } else {
        printf("  [FAIL] Wrong bit positions: P=0x%03X W=0x%03X U=0x%03X\n",
               PAGING_P, PAGING_W, PAGING_U); fail++;
    }

    // ─── Verify combined flags stay within 3-bit range ───────────
    uint32_t max_flags = PAGING_P | PAGING_W | PAGING_U;
    if (max_flags == 0x007 && max_flags < 0xFFF) {
        printf("  [PASS] Max flags (P|W|U) = 0x%03X < 0xFFF (safe for PTE)\n", max_flags); pass++;
    } else {
        printf("  [FAIL] Max flags overflow: 0x%03X\n", max_flags); fail++;
    }

    printf("\n  Results: %d passed, %d failed\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
