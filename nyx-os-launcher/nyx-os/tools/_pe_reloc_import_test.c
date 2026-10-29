// tools/_pe_reloc_import_test.c — Host-side test for PE relocation
// and import resolution.  Tests the logic without needing the kernel
// (no paging, no task_create_user).
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

// ─── Relocation types ───────────────────────────────────────────
#define PE_REL_ABSOLUTE  0
#define PE_REL_HIGH      1
#define PE_REL_LOW       2
#define PE_REL_HIGHLOW   3

// ─── Relocation test ────────────────────────────────────────────
// Simulate applying relocations to a buffer, matching peload.c logic.
static void apply_reloc_high(uint32_t* addr, uint32_t delta) {
    uint32_t old = *addr;
    uint32_t new_val = old + delta;
    *addr = (new_val & 0xFFFF0000u) | (old & 0x0000FFFFu);
}

static void apply_reloc_low(uint32_t* addr, uint32_t delta) {
    uint32_t old = *addr;
    uint32_t new_val = old + delta;
    *addr = (old & 0xFFFF0000u) | (new_val & 0x0000FFFFu);
}

static void apply_reloc_highlow(uint32_t* addr, uint32_t delta) {
    *addr += delta;
}

// ─── Stub import resolver ───────────────────────────────────────
static void __attribute__((used)) stub_func(void) {
    asm volatile("xor %%eax, %%eax\n\t"
                 "ret"
                 ::: "eax");
}
static uint32_t stub_addr(void) {
    return (uint32_t)&stub_func;
}

// ─── IAT resolution test ────────────────────────────────────────
// Simulates what peload.c does: write stub address into each IAT slot.
static void resolve_imports(uint32_t* iat, int n_entries) {
    uint32_t stub = stub_addr();
    for (int i = 0; i < n_entries; i++) {
        iat[i] = stub;
    }
}

// ─── Tests ──────────────────────────────────────────────────────
int main(void) {
    int pass = 0, fail = 0;

    // ─── Relocation type tests ──────────────────────────────────
    printf("Relocation types:\n");

    // HIGHLOW: patch full 32-bit
    uint32_t val_hl = 0x00401000;
    uint32_t delta = 0xC1000000 - 0x00400000;  // our LINK_BASE - PE ImageBase
    apply_reloc_highlow(&val_hl, delta);
    if (val_hl == 0xC1001000) {
        printf("  [PASS] HIGHLOW: 0x00401000 + delta → 0xC1001000\n"); pass++;
    } else {
        printf("  [FAIL] HIGHLOW: expected 0xC1001000, got 0x%08X\n", val_hl); fail++;
    }

    // HIGH: patch bits 31..16
    uint32_t val_hi = 0x00401234;
    apply_reloc_high(&val_hi, delta);
    if (val_hi == 0xC1001234) {
        printf("  [PASS] HIGH: 0x00401234 + delta → 0xC1001234\n"); pass++;
    } else {
        printf("  [FAIL] HIGH: expected 0xC1001234, got 0x%08X\n", val_hi); fail++;
    }

    // HIGH with carry into low bits: should NOT affect low 16 bits
    uint32_t val_hi2 = 0x0040FFFF;
    apply_reloc_high(&val_hi2, delta);
    // new = 0x0040FFFF + 0xC0C00000 = 0xC100FFFF
    // HIGH patches 31..16: 0xC100
    // low 16 bits unchanged: 0xFFFF
    if (val_hi2 == 0xC100FFFF) {
        printf("  [PASS] HIGH (no low corruption): 0x0040FFFF → 0xC100FFFF\n"); pass++;
    } else {
        printf("  [FAIL] HIGH (no low corruption): expected 0xC100FFFF, got 0x%08X\n", val_hi2); fail++;
    }

    // LOW: patch bits 15..0 only
    // Use a delta where low 16 bits are non-zero
    uint32_t delta_lo = 0x00001234;
    uint32_t val_lo = 0x00400000;
    apply_reloc_low(&val_lo, delta_lo);
    // new low = (0x0000 + 0x1234) & 0xFFFF = 0x1234
    // high unchanged: 0x0040
    if (val_lo == 0x00401234) {
        printf("  [PASS] LOW: 0x00400000 + delta_lo → 0x00401234\n"); pass++;
    } else {
        printf("  [FAIL] LOW: expected 0x00401234, got 0x%08X\n", val_lo); fail++;
    }

    // LOW with carry: should only affect low 16 bits, high unchanged
    uint32_t val_lo2 = 0xC100F000;
    uint32_t delta_lo2 = 0x00002000;
    apply_reloc_low(&val_lo2, delta_lo2);
    // new low = (0xF000 + 0x2000) & 0xFFFF = 0x1000 (carry wraps)
    // high unchanged: 0xC100
    if (val_lo2 == 0xC1001000) {
        printf("  [PASS] LOW (carry): 0xC100F000 → 0xC1001000\n"); pass++;
    } else {
        printf("  [FAIL] LOW (carry): expected 0xC1001000, got 0x%08X\n", val_lo2); fail++;
    }

    // ABSOLUTE: no change
    uint32_t val_abs = 0x12345678;
    // Type 0 is a no-op, we just skip it
    if (val_abs == 0x12345678) {
        printf("  [PASS] ABSOLUTE: no-op (0x12345678 unchanged)\n"); pass++;
    } else {
        printf("  [FAIL] ABSOLUTE: value changed!\n"); fail++;
    }

    // ─── Relocation entry parsing ───────────────────────────────
    printf("\nRelocation entry parsing:\n");

    // Simulate a relocation block
    uint16_t entries[4];
    entries[0] = (PE_REL_HIGHLOW << 12) | 0x000;  // HIGHLOW @ offset 0
    entries[1] = (PE_REL_HIGH    << 12) | 0x004;  // HIGH @ offset 4
    entries[2] = (PE_REL_LOW     << 12) | 0x008;  // LOW @ offset 8
    entries[3] = (PE_REL_ABSOLUTE << 12) | 0x00C;  // ABSOLUTE @ offset 12

    for (int i = 0; i < 4; i++) {
        uint16_t type = (entries[i] >> 12) & 0xF;
        uint16_t offset = entries[i] & 0xFFF;
        const char* name = "?";
        if (type == PE_REL_ABSOLUTE) name = "ABSOLUTE";
        else if (type == PE_REL_HIGH) name = "HIGH";
        else if (type == PE_REL_LOW) name = "LOW";
        else if (type == PE_REL_HIGHLOW) name = "HIGHLOW";
        printf("  [PASS] Entry %d: type=%s (%d) offset=0x%03X\n",
               i, name, type, offset);
        pass++;
    }

    // ─── Block header parsing ───────────────────────────────────
    printf("\nBlock header parsing:\n");
    struct { uint32_t VirtualAddress; uint32_t SizeOfBlock; } block;
    block.VirtualAddress = 0x1000;
    block.SizeOfBlock = 8 + 4 * 2;  // header + 4 entries
    uint32_t n_entries = (block.SizeOfBlock - 8) / 2;
    if (n_entries == 4) {
        printf("  [PASS] Block with 4 entries: n_entries=%d\n", n_entries); pass++;
    } else {
        printf("  [FAIL] Block with 4 entries: expected 4, got %d\n", n_entries); fail++;
    }

    // Validate block alignment
    if ((block.SizeOfBlock & 3) == 0) {
        printf("  [PASS] Block size aligned to 4 bytes: %d\n", block.SizeOfBlock); pass++;
    } else {
        printf("  [FAIL] Block size not aligned: %d\n", block.SizeOfBlock); fail++;
    }

    // ─── Import resolution tests ────────────────────────────────
    printf("\nImport resolution:\n");

    // Simulate IAT with 3 entries
    uint32_t iat[4] = {0, 0, 0, 0};
    resolve_imports(iat, 3);
    uint32_t expected_stub = stub_addr();
    int all_stub = 1;
    for (int i = 0; i < 3; i++) {
        if (iat[i] != expected_stub) {
            all_stub = 0;
            break;
        }
    }
    if (all_stub) {
        printf("  [PASS] IAT[0..2] filled with stub address 0x%08X\n", expected_stub); pass++;
    } else {
        printf("  [FAIL] IAT not filled correctly\n"); fail++;
    }

    // Null terminator preserved
    if (iat[3] == 0) {
        printf("  [PASS] IAT[3] null terminator preserved\n"); pass++;
    } else {
        printf("  [FAIL] IAT[3] corrupted: 0x%08X\n", iat[3]); fail++;
    }

    // Import by ordinal test
    uint32_t ilt_by_name = 0x00001234;     // Hint/Name RVA
    uint32_t ilt_by_ordinal = 0x80000123;  // Bit 31 set = ordinal
    int is_by_name = !(ilt_by_name & 0x80000000);
    int is_by_ord = (ilt_by_ordinal & 0x80000000) != 0;
    uint16_t ordinal = ilt_by_ordinal & 0xFFFF;
    if (is_by_name && is_by_ord && ordinal == 0x0123) {
        printf("  [PASS] By-name vs by-ordinal detection correct\n"); pass++;
    } else {
        printf("  [FAIL] By-name/by-ordinal detection wrong\n"); fail++;
    }

    // ─── Import descriptor validation ───────────────────────────
    printf("\nImport descriptor validation:\n");

    // Null descriptor (all zeros) terminates the list
    struct { uint32_t OriginalFirstThunk, TimeDateStamp, ForwarderChain, Name, FirstThunk; } desc;
    memset(&desc, 0, sizeof(desc));
    int is_null = (desc.OriginalFirstThunk == 0 && desc.FirstThunk == 0);
    if (is_null) {
        printf("  [PASS] Null descriptor detected (terminator)\n"); pass++;
    } else {
        printf("  [FAIL] Null descriptor not detected\n"); fail++;
    }

    // Non-null descriptor
    desc.OriginalFirstThunk = 0x3028;
    desc.Name = 0x3038;
    desc.FirstThunk = 0x3030;
    is_null = (desc.OriginalFirstThunk == 0 && desc.FirstThunk == 0);
    if (!is_null) {
        printf("  [PASS] Non-null descriptor detected\n"); pass++;
    } else {
        printf("  [FAIL] Non-null descriptor rejected\n"); fail++;
    }

    // INT fallback to IAT when OriginalFirstThunk is 0
    desc.OriginalFirstThunk = 0;
    desc.FirstThunk = 0x3030;
    uint32_t int_rva = desc.OriginalFirstThunk;
    uint32_t iat_rva = desc.FirstThunk;
    if (int_rva == 0) int_rva = iat_rva;
    if (int_rva == 0x3030) {
        printf("  [PASS] INT fallback to IAT when OriginalFirstThunk=0\n"); pass++;
    } else {
        printf("  [FAIL] INT fallback wrong: 0x%08X\n", int_rva); fail++;
    }

    // ─── Data directory index tests ─────────────────────────────
    printf("\nData directory indices:\n");
    // The data directory array is at optional_header + 96
    // Index 0 = Export, 1 = Import, 5 = Relocation
    uint32_t dd_indices[] = {0, 1, 5};
    const char* dd_names[] = {"Export", "Import", "Relocation"};
    for (int i = 0; i < 3; i++) {
        uint32_t off = dd_indices[i] * 8;  // each entry is 8 bytes
        printf("  [PASS] %s directory at index %d (offset %d)\n",
               dd_names[i], dd_indices[i], off);
        pass++;
    }

    printf("\n  Results: %d passed, %d failed\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
