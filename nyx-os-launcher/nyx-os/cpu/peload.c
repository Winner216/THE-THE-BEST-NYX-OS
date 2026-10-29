// cpu/peload.c  –  PE32 (Windows .exe) loader for Nyx OS.
//
// Loads a PE32 executable into the USER_TASK_SLOT (ring 3), using the
// same address-space layout as the ELF loader:
//   USER_LINK_BASE + section RVA  → mapped pages for .text / .data / .bss
//   USER_LINK_BASE + 0x00F00000  → user stack top (256 KiB below)
//   USER_LINK_BASE + 0x00E00000  → argv page
//
// Imported DLL functions (kernel32.dll, user32.dll, etc.) are resolved
// to stub trampolines that return 0 / nop, so the .exe can link
// without crashing — the stubs just don't do anything useful yet.
// This is enough to load and run simple console PE programs.
//
// Full Win32 API emulation would require a syscall layer and a huge
// amount of work.  The goal here is to demonstrate a correct PE parser
// that can parse headers, map sections, relocate, and hand off to the
// scheduler.

#include <peload.h>
#include <elfload.h>
#include <winapi.h>
#include <task.h>
#include <paging.h>
#include <io.h>
#include <stdint.h>
#include <stddef.h>

// ─── serial debug (COM1 at 0x3F8) ───────────────────────────────
static void pe_serial_putc(char c) {
    int sp = 0;
    while ((inb(0x3F8 + 5) & 0x20) == 0 && sp++ < 200000) {}
    outb(0x3F8, (uint8_t)c);
}
static void pe_serial_puts(const char* s) {
    for (; *s; s++) pe_serial_putc(*s);
}
// Print a hex32 value to serial (no libc needed)
static void pe_hex32(uint32_t v) {
    const char* hex = "0123456789abcdef";
    pe_serial_puts("0x");
    for (int i = 7; i >= 0; i--)
        pe_serial_putc(hex[(v >> (i * 4)) & 0xF]);
}
// Print a decimal number
static void pe_dec(uint32_t v) {
    char buf[12]; int n = 0;
    if (v == 0) { buf[n++] = '0'; }
    else { char t[12]; int q = 0;
        while (v > 0) { t[q++] = (char)('0' + v % 10); v /= 10; }
        while (q > 0) buf[n++] = t[--q]; }
    buf[n] = 0;
    pe_serial_puts(buf);
}
static void pe_newline(void) { pe_serial_putc('\r'); pe_serial_putc('\n'); }

// ─── Validation ──────────────────────────────────────────────────

int pe_is_valid(const unsigned char* data, uint32_t len) {
    if (len < sizeof(pe_mz_header_t)) return 0;

    // Check MZ signature
    uint16_t mz = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
    if (mz != PE_MZ_MAGIC) return 0;

    // Follow e_lfanew to PE signature
    const pe_mz_header_t* dos = (const pe_mz_header_t*)data;
    uint32_t pe_off = dos->e_lfanew;
    if (pe_off + 4 + sizeof(pe_coff_header_t) > len) return 0;

    // Check PE\0\0 signature
    uint32_t sig = (uint32_t)data[pe_off]     | ((uint32_t)data[pe_off+1] << 8) |
                   ((uint32_t)data[pe_off+2] << 16) | ((uint32_t)data[pe_off+3] << 24);
    if (sig != PE_SIGNATURE) return 0;

    // Must be i386 (we're a 32-bit OS)
    const pe_coff_header_t* coff =
        (const pe_coff_header_t*)(data + pe_off + 4);
    if (coff->Machine != PE_MACHINE_I386) return 0;

    return 1;
}

// ─── Header dump (serial diagnostic) ────────────────────────────

void pe_dump_header(const unsigned char* data, uint32_t len) {
    if (!pe_is_valid(data, len)) {
        pe_serial_puts("PE: invalid header");
        pe_newline();
        return;
    }
    const pe_mz_header_t* dos = (const pe_mz_header_t*)data;
    uint32_t pe_off = dos->e_lfanew;
    const pe_coff_header_t* coff =
        (const pe_coff_header_t*)(data + pe_off + 4);
    const pe_optional_header32_t* opt =
        (const pe_optional_header32_t*)(data + pe_off + 4 + sizeof(pe_coff_header_t));

    pe_serial_puts("PE: machine=");
    pe_hex32(coff->Machine);
    pe_serial_puts(" sections=");
    pe_dec(coff->NumberOfSections);
    pe_serial_puts(" entry=");
    pe_hex32(opt->AddressOfEntryPoint);
    pe_serial_puts(" imageBase=");
    pe_hex32(opt->ImageBase);
    pe_serial_puts(" subsystem=");
    pe_dec(opt->Subsystem);
    pe_newline();

    // Walk sections
    const unsigned char* sec_base = data + pe_off + 4 + sizeof(pe_coff_header_t)
                                    + coff->SizeOfOptionalHeader;
    for (uint16_t i = 0; i < coff->NumberOfSections; i++) {
        if ((uint32_t)((sec_base - data) + (i + 1) * 40) > len) break;
        const pe_section_header_t* sec =
            (const pe_section_header_t*)(sec_base + i * 40);
        pe_serial_puts("  [");
        // Print section name (up to 8 chars, no null terminator guaranteed)
        for (int c = 0; c < 8 && sec->Name[c]; c++)
            pe_serial_putc(sec->Name[c]);
        pe_serial_puts("] VA=");
        pe_hex32(sec->VirtualAddress);
        pe_serial_puts(" VSize=");
        pe_hex32(sec->VirtualSize);
        pe_serial_puts(" RawOff=");
        pe_hex32(sec->PointerToRawData);
        pe_serial_puts(" RawSize=");
        pe_hex32(sec->SizeOfRawData);
        pe_serial_puts(" Flags=");
        pe_hex32(sec->Characteristics);
        pe_newline();
    }
}

// ─── Stub import resolver ────────────────────────────────────────
// Imported DLL functions get a pointer to this stub.  It returns 0
// (NULL for handles, 0 for BOOL, no-op for void) — enough to not
// crash on the first call.  Each stub is a plain `xor eax,eax; ret`.

// Compiler barrier to prevent the optimizer from removing stub_func
static void __attribute__((used)) stub_func(void) {
    asm volatile("xor %%eax, %%eax\n\t"
                 "ret"
                 ::: "eax");
}

static uint32_t stub_addr(void) {
    return (uint32_t)&stub_func;
}

// ─── Section name helpers ────────────────────────────────────────

static int sec_name_matches(const pe_section_header_t* sec, const char* name) {
    for (int i = 0; i < 8 && name[i]; i++) {
        if (sec->Name[i] != name[i]) return 0;
    }
    // The name in the section may not be null-terminated if 8 chars.
    // Ensure the name string also has no more chars.
    int nlen = 0;
    while (name[nlen]) nlen++;
    if (nlen < 8 && sec->Name[nlen] != '\0') return 0;
    return 1;
}

static int sec_is_code(const pe_section_header_t* sec) {
    return (sec->Characteristics & PE_SCN_CNT_CODE) != 0 ||
           (sec->Characteristics & PE_SCN_MEM_EXECUTE) != 0;
}

static int sec_is_data(const pe_section_header_t* sec) {
    return (sec->Characteristics & PE_SCN_CNT_INIT_DATA) != 0;
}

// ─── Loader copy helper ──────────────────────────────────────────
// Copy bytes under the user task's CR3 (same pattern as elfload.c).

static void pe_loader_copy(uint32_t dst, const unsigned char* src,
                           uint32_t n, uint32_t z) {
    for (uint32_t i = 0; i < n; i++)
        *(volatile uint8_t*)(dst + i) = src[i];
    for (uint32_t i = 0; i < z; i++)
        *(volatile uint8_t*)(dst + n + i) = 0;
}

// ─── User-memory read/write under CR3 ──────────────────────────
// The user task's pages are only visible under its page directory.
// These helpers switch CR3, do the access, then restore.

static uint32_t pe_read32_user(uint32_t va) {
    uint32_t saved_cr3, val;
    asm volatile("mov %%cr3, %0" : "=r" (saved_cr3));
    asm volatile("cli");
    asm volatile("mov %0, %%cr3" :: "r" (paging_task_pd(TASK_USER_SLOT)));
    val = *(volatile uint32_t*)va;
    asm volatile("mov %0, %%cr3" :: "r" (saved_cr3));
    asm volatile("sti");
    return val;
}

static void pe_write32_user(uint32_t va, uint32_t val) {
    uint32_t saved_cr3;
    asm volatile("mov %%cr3, %0" : "=r" (saved_cr3));
    asm volatile("cli");
    asm volatile("mov %0, %%cr3" :: "r" (paging_task_pd(TASK_USER_SLOT)));
    *(volatile uint32_t*)va = val;
    asm volatile("mov %0, %%cr3" :: "r" (saved_cr3));
    asm volatile("sti");
}

static uint16_t pe_read16_user(uint32_t va) {
    uint32_t saved_cr3;
    uint16_t val;
    asm volatile("mov %%cr3, %0" : "=r" (saved_cr3));
    asm volatile("cli");
    asm volatile("mov %0, %%cr3" :: "r" (paging_task_pd(TASK_USER_SLOT)));
    val = *(volatile uint16_t*)va;
    asm volatile("mov %0, %%cr3" :: "r" (saved_cr3));
    asm volatile("sti");
    return val;
}

static void pe_write16_user(uint32_t va, uint16_t val) {
    uint32_t saved_cr3;
    asm volatile("mov %%cr3, %0" : "=r" (saved_cr3));
    asm volatile("cli");
    asm volatile("mov %0, %%cr3" :: "r" (paging_task_pd(TASK_USER_SLOT)));
    *(volatile uint16_t*)va = val;
    asm volatile("mov %0, %%cr3" :: "r" (saved_cr3));
    asm volatile("sti");
}

static void pe_read_buf_user(uint32_t va, void* dst, uint32_t n) {
    uint32_t saved_cr3;
    asm volatile("mov %%cr3, %0" : "=r" (saved_cr3));
    asm volatile("cli");
    asm volatile("mov %0, %%cr3" :: "r" (paging_task_pd(TASK_USER_SLOT)));
    const uint8_t* src = (const uint8_t*)va;
    uint8_t* d = (uint8_t*)dst;
    for (uint32_t i = 0; i < n; i++) d[i] = src[i];
    asm volatile("mov %0, %%cr3" :: "r" (saved_cr3));
    asm volatile("sti");
}

// ─── Relocation types ───────────────────────────────────────────
// x86-32 PE relocations (from winnt.h):
//   0 = IMAGE_REL_BASED_ABSOLUTE   — padding, skip
//   1 = IMAGE_REL_BASED_HIGH       — patch bits 31..16 of a 32-bit addr
//   2 = IMAGE_REL_BASED_LOW        — patch bits 15..0  of a 32-bit addr
//   3 = IMAGE_REL_BASED_HIGHLOW    — patch full 32-bit addr (most common)
#define PE_REL_ABSOLUTE   0
#define PE_REL_HIGH       1
#define PE_REL_LOW        2
#define PE_REL_HIGHLOW    3

// ─── Main loader entry ───────────────────────────────────────────

int pe_exec(const unsigned char* pe, uint32_t len, const char* name,
            const char* args) {
    pe_serial_puts("PE: loading ");
    pe_serial_puts(name ? name : "?");
    pe_newline();

    if (!pe_is_valid(pe, len)) {
        pe_serial_puts("PE: header invalid");
        pe_newline();
        return -1;
    }

    const pe_mz_header_t* dos = (const pe_mz_header_t*)pe;
    uint32_t pe_off = dos->e_lfanew;
    const pe_coff_header_t* coff =
        (const pe_coff_header_t*)(pe + pe_off + 4);
    const pe_optional_header32_t* opt =
        (const pe_optional_header32_t*)(pe + pe_off + 4 + sizeof(pe_coff_header_t));

    // Only support PE32 (not PE32+ which is 64-bit)
    if (opt->Magic != 0x10B) {
        pe_serial_puts("PE: not PE32 (magic=");
        pe_hex32(opt->Magic);
        pe_serial_puts(")");
        pe_newline();
        return -1;
    }

    // Only support i386
    if (coff->Machine != PE_MACHINE_I386) {
        pe_serial_puts("PE: not i386");
        pe_newline();
        return -1;
    }

    // Don't load DLLs (only executables)
    if (coff->Characteristics & PE_DLL) {
        pe_serial_puts("PE: is a DLL, skipping");
        pe_newline();
        return -1;
    }

    uint32_t entry_rva = opt->AddressOfEntryPoint;
    uint32_t image_base_pe = opt->ImageBase;  // original preferred base
    uint32_t sec_align = opt->SectionAlignment;
    if (sec_align < 0x1000) sec_align = 0x1000;

    pe_serial_puts("PE: entry=");
    pe_hex32(entry_rva);
    pe_serial_puts(" imgBase=");
    pe_hex32(image_base_pe);
    pe_serial_puts(" nsec=");
    pe_dec(coff->NumberOfSections);
    pe_newline();

    // Relaunch: kill any previous user task
    task_kill(TASK_USER_SLOT);

    // Our link address for the user slot
    const uint32_t LINK_BASE = USER_LINK_BASE;

    // The PE is loaded at LINK_BASE + section RVA.
    // If the PE's ImageBase conflicts, we apply base relocations
    // (always, since our LINK_BASE differs from Windows' addresses).
    int need_reloc = 1;  // always relocate to our base

    // Section table starts right after the optional header
    const unsigned char* sec_table = pe + pe_off + 4 + sizeof(pe_coff_header_t)
                                     + coff->SizeOfOptionalHeader;

    // Find the import directory and relocation directory from the
    // data directories (array starts right after the fixed optional
    // header fields).  The data directory array index:
    //   0 = Export
    //   1 = Import
    //   5 = Relocation (Base Relocation Table)
    uint32_t num_rva = opt->NumberOfRvaAndSizes;
    const pe_data_dir_t* data_dirs =
        (const pe_data_dir_t*)(pe + pe_off + 4 + sizeof(pe_coff_header_t)
                               - sizeof(pe_data_dir_t));  // right after NumberOfRvaAndSizes
    // Data directories start immediately after the 96-byte fixed
    // fields of the optional header (right after NumberOfRvaAndSizes).
    data_dirs = (const pe_data_dir_t*)(pe + pe_off + 4 + sizeof(pe_coff_header_t) + 96);

    pe_data_dir_t import_dir = {0, 0};
    pe_data_dir_t reloc_dir  = {0, 0};
    if (num_rva > 1) import_dir = data_dirs[1];  // Import Directory
    if (num_rva > 5) reloc_dir  = data_dirs[5];  // Relocation Table

    pe_serial_puts("PE: importDir VA=");
    pe_hex32(import_dir.VirtualAddress);
    pe_serial_puts(" size=");
    pe_dec(import_dir.Size);
    pe_serial_puts(" relocDir VA=");
    pe_hex32(reloc_dir.VirtualAddress);
    pe_serial_puts(" size=");
    pe_dec(reloc_dir.Size);
    pe_newline();

    // ─── Step 1: Map each section ────────────────────────────────
    for (uint16_t i = 0; i < coff->NumberOfSections; i++) {
        uint32_t sec_off = (uint32_t)(sec_table - pe) + (uint32_t)i * 40;
        if (sec_off + 40 > len) break;
        const pe_section_header_t* sec =
            (const pe_section_header_t*)(pe + sec_off);

        uint32_t vsize = sec->VirtualSize;
        uint32_t rsize = sec->SizeOfRawData;
        if (vsize == 0) vsize = rsize;
        if (vsize == 0) continue;  // empty section (discardable)

        // Skip discardable sections (like .reloc, .pdata)
        if (sec->Characteristics & PE_SCN_MEM_DISCARDABLE) {
            pe_serial_puts("PE: skip discardable section [");
            for (int c = 0; c < 8 && sec->Name[c]; c++)
                pe_serial_putc(sec->Name[c]);
            pe_serial_puts("]");
            pe_newline();
            continue;
        }

        uint32_t va = LINK_BASE + sec->VirtualAddress;
        uint32_t va_page = va & ~(PAGE_SIZE - 1);
        uint32_t end     = va + vsize;
        uint32_t pages   = (end - va_page + PAGE_SIZE - 1) / PAGE_SIZE;

        pe_serial_puts("PE: map [");
        for (int c = 0; c < 8 && sec->Name[c]; c++)
            pe_serial_putc(sec->Name[c]);
        pe_serial_puts("] VA=");
        pe_hex32(va);
        pe_serial_puts(" pages=");
        pe_dec(pages);
        pe_newline();

        if (!paging_map_user_va(TASK_USER_SLOT, va_page, (int)pages)) {
            pe_serial_puts("PE: paging_map_user_va failed!");
            pe_newline();
            return -1;
        }

        // Calculate how much to copy vs zero
        uint32_t copy_n = rsize;
        uint32_t zero_n = (vsize > rsize) ? (vsize - rsize) : 0;
        // Don't copy past end of file
        if (sec->PointerToRawData + copy_n > len)
            copy_n = (len > sec->PointerToRawData) ? (len - sec->PointerToRawData) : 0;

        const unsigned char* src = (copy_n > 0) ? (pe + sec->PointerToRawData) : 0;

        // Copy under user task's CR3
        uint32_t saved_cr3;
        asm volatile("mov %%cr3, %0" : "=r" (saved_cr3));
        asm volatile("cli");
        asm volatile("mov %0, %%cr3" :: "r" (paging_task_pd(TASK_USER_SLOT)));
        pe_loader_copy(va, src, copy_n, zero_n);
        asm volatile("mov %0, %%cr3" :: "r" (saved_cr3));
        asm volatile("sti");
    }

    // ─── Step 2: Apply base relocations ──────────────────────────
    // The PE was compiled for image_base_pe but we load at LINK_BASE.
    // The relocation table contains blocks of entries that tell us
    // which addresses to patch.  Each block covers a 4 KiB page and
    // contains 16-bit entries: top 4 bits = type, bottom 12 = offset.
    //
    // Supported types:
    //   0 (ABSOLUTE) — padding, skip
    //   1 (HIGH)     — patch bits 31..16 of a 32-bit value
    //   2 (LOW)      — patch bits 15..0  of a 32-bit value
    //   3 (HIGHLOW)  — patch full 32-bit value (most common)
    //
    // Types 4-10 are ARM/MIPS/RISC-V-specific and skipped on x86.
    uint32_t n_high = 0, n_low = 0, n_highlow = 0, n_skipped = 0;
    if (need_reloc && reloc_dir.VirtualAddress != 0 && reloc_dir.Size != 0) {
        pe_serial_puts("PE: applying base relocations (delta=");
        pe_hex32(LINK_BASE - image_base_pe);
        pe_serial_puts(")");
        pe_newline();

        uint32_t reloc_offset = reloc_dir.VirtualAddress;  // RVA
        uint32_t reloc_size   = reloc_dir.Size;
        uint32_t delta = LINK_BASE - image_base_pe;

        // Walk relocation blocks
        uint32_t rva = 0;
        while (rva < reloc_size) {
            uint32_t blk_rva = reloc_offset + rva;
            if (blk_rva + 8 > len) break;

            // Read block header (VirtualAddress + SizeOfBlock) from
            // the user-mapped memory under its CR3.
            pe_reloc_block_t blk;
            blk.VirtualAddress = pe_read32_user(LINK_BASE + blk_rva);
            blk.SizeOfBlock    = pe_read32_user(LINK_BASE + blk_rva + 4);

            // Sanity: SizeOfBlock must be >= 8 (header) and block must
            // fit within the relocation directory.
            if (blk.SizeOfBlock < 8) {
                pe_serial_puts("PE: reloc block too small (");
                pe_dec(blk.SizeOfBlock);
                pe_serial_puts("), stopping");
                pe_newline();
                break;
            }
            if (rva + blk.SizeOfBlock > reloc_size) {
                pe_serial_puts("PE: reloc block extends past dir, stopping");
                pe_newline();
                break;
            }
            // SizeOfBlock must be 4-byte aligned
            if (blk.SizeOfBlock & 3) {
                pe_serial_puts("PE: reloc block not aligned, stopping");
                pe_newline();
                break;
            }

            uint32_t n_entries = (blk.SizeOfBlock - 8) / 2;
            for (uint32_t e = 0; e < n_entries; e++) {
                uint16_t ent = pe_read16_user(LINK_BASE + blk_rva + 8 + e * 2);

                uint16_t type   = (ent >> 12) & 0xF;
                uint16_t offset = ent & 0xFFF;
                uint32_t patch_rva = blk.VirtualAddress + offset;
                uint32_t patch_va  = LINK_BASE + patch_rva;

                switch (type) {
                case PE_REL_ABSOLUTE:
                    // Padding entry — no patch needed
                    n_skipped++;
                    break;

                case PE_REL_HIGH: {
                    // Patch bits 31..16 of the 32-bit value at patch_va.
                    // new_high = (old + delta) >> 16
                    uint32_t old32 = pe_read32_user(patch_va);
                    uint32_t new32 = old32 + delta;
                    // Keep low 16 bits, replace high 16 bits
                    uint32_t patched = (new32 & 0xFFFF0000u) | (old32 & 0x0000FFFFu);
                    pe_write32_user(patch_va, patched);
                    n_high++;
                    break;
                }

                case PE_REL_LOW: {
                    // Patch bits 15..0 of the 32-bit value at patch_va.
                    // new_low = (old + delta) & 0xFFFF
                    uint32_t old32 = pe_read32_user(patch_va);
                    uint32_t new32 = old32 + delta;
                    // Keep high 16 bits, replace low 16 bits
                    uint32_t patched = (old32 & 0xFFFF0000u) | (new32 & 0x0000FFFFu);
                    pe_write32_user(patch_va, patched);
                    n_low++;
                    break;
                }

                case PE_REL_HIGHLOW: {
                    // Patch full 32-bit address (the common case)
                    uint32_t old32 = pe_read32_user(patch_va);
                    pe_write32_user(patch_va, old32 + delta);
                    n_highlow++;
                    break;
                }

                default:
                    // Unknown relocation type (ARM, MIPS, RISC-V specific)
                    // Skip silently — these are harmless on x86.
                    n_skipped++;
                    break;
                }
            }

            rva += blk.SizeOfBlock;
        }
        pe_serial_puts("PE: reloc done: HIGH="); pe_dec(n_high);
        pe_serial_puts(" LOW="); pe_dec(n_low);
        pe_serial_puts(" HIGHLOW="); pe_dec(n_highlow);
        pe_serial_puts(" skip="); pe_dec(n_skipped);
        pe_newline();
    }

    // ─── Step 2.5: Tighten page permissions ─────────────────────
    // All sections were mapped RW during Step 1 so relocations and
    // BSS zeroing could write to code pages.  Now that relocation
    // is done, walk the sections again and lock down permissions:
    //   .text / code sections    → read-only + user (PAGING_U, no PAGING_W)
    //   .data / init data       → read-write + user (PAGING_W | PAGING_U)
    //   .rdata / read-only data → read-only + user
    //   .bss / uninit data      → read-write + user
    // x86-32 has no hardware NX bit, so "read-only" is the best we
    // can do for code — it prevents user-mode writes (catching buffer
    // overflows in code pages) while still allowing execution.
    pe_serial_puts("PE: setting page permissions");
    pe_newline();
    for (uint16_t i = 0; i < coff->NumberOfSections; i++) {
        uint32_t sec_off2 = (uint32_t)(sec_table - pe) + (uint32_t)i * 40;
        if (sec_off2 + 40 > len) break;
        const pe_section_header_t* sec2 =
            (const pe_section_header_t*)(pe + sec_off2);
        uint32_t vsize2 = sec2->VirtualSize;
        uint32_t rsize2 = sec2->SizeOfRawData;
        if (vsize2 == 0) vsize2 = rsize2;
        if (vsize2 == 0) continue;
        if (sec2->Characteristics & PE_SCN_MEM_DISCARDABLE) continue;

        uint32_t va2 = LINK_BASE + sec2->VirtualAddress;
        uint32_t va2_page = va2 & ~(PAGE_SIZE - 1);
        uint32_t end2     = va2 + vsize2;
        uint32_t pages2   = (end2 - va2_page + PAGE_SIZE - 1) / PAGE_SIZE;

        // Determine permissions from section characteristics
        uint32_t sec_flags;
        int is_code = (sec2->Characteristics & PE_SCN_CNT_CODE) ||
                      (sec2->Characteristics & PE_SCN_MEM_EXECUTE);
        int is_writable = (sec2->Characteristics & PE_SCN_MEM_WRITE);
        int is_readonly = !(sec2->Characteristics & PE_SCN_MEM_WRITE) &&
                          !(sec2->Characteristics & PE_SCN_CNT_UNINIT_DATA);

        if (is_code && !is_writable) {
            // Code section: read-only + user (prevent self-modifying code)
            sec_flags = PAGING_U;
            pe_serial_puts("  [");
            for (int c = 0; c < 8 && sec2->Name[c]; c++)
                pe_serial_putc(sec2->Name[c]);
            pe_serial_puts("] → R+U (code)");
            pe_newline();
        } else if (is_readonly && !is_code) {
            // Read-only data: read-only + user
            sec_flags = PAGING_U;
            pe_serial_puts("  [");
            for (int c = 0; c < 8 && sec2->Name[c]; c++)
                pe_serial_putc(sec2->Name[c]);
            pe_serial_puts("] → R+U (rodata)");
            pe_newline();
        } else {
            // Writable data (.data, .bss): read-write + user
            sec_flags = PAGING_W | PAGING_U;
            pe_serial_puts("  [");
            for (int c = 0; c < 8 && sec2->Name[c]; c++)
                pe_serial_putc(sec2->Name[c]);
            pe_serial_puts("] → RW+U (data)");
            pe_newline();
        }

        paging_set_user_va_flags(TASK_USER_SLOT, va2_page, (int)pages2, sec_flags);
    }
    pe_serial_puts("PE: permissions set");
    pe_newline();

    // ─── Step 3: Resolve imports ─────────────────────────────────
    if (import_dir.VirtualAddress != 0 && import_dir.Size != 0) {
        pe_serial_puts("PE: resolving imports");
        pe_newline();

        uint32_t imp_offset = import_dir.VirtualAddress;
        uint32_t imp_desc_size = sizeof(pe_import_desc_t);
        uint32_t idx = 0;

        while (1) {
            uint32_t desc_rva = imp_offset + idx * imp_desc_size;
            if (desc_rva + imp_desc_size > len) break;

            pe_import_desc_t idesc;
            // Read descriptor under user CR3 using the helper
            pe_read_buf_user(LINK_BASE + desc_rva, &idesc, imp_desc_size);

            // Null terminator (all zeros)
            if (idesc.OriginalFirstThunk == 0 && idesc.FirstThunk == 0) break;

            // Get DLL name
            char dll_name[64];
            if (idesc.Name != 0 && idesc.Name < len) {
                pe_read_buf_user(LINK_BASE + idesc.Name, dll_name, 63);
                dll_name[63] = 0;
            } else {
                dll_name[0] = '?';
                dll_name[1] = 0;
            }

            pe_serial_puts("PE:   DLL=");
            pe_serial_puts(dll_name);
            pe_newline();

            // Walk the Import Name Table (INT) / Import Address Table (IAT)
            // OriginalFirstThunk → INT, FirstThunk → IAT
            uint32_t int_rva = idesc.OriginalFirstThunk;
            uint32_t iat_rva = idesc.FirstThunk;
            if (int_rva == 0) int_rva = iat_rva;  // some linkers omit INT

            for (uint32_t imp_idx = 0; ; imp_idx++) {
                uint32_t ilt_entry_rva = int_rva + imp_idx * 4;
                if (ilt_entry_rva + 4 > len) break;

                uint32_t ilt_entry = pe_read32_user(LINK_BASE + ilt_entry_rva);

                if (ilt_entry == 0) break;  // null terminator

                // Extract function name (by-name) or ordinal number
                char func_name[128];
                func_name[0] = 0;
                uint16_t ordinal = 0;
                int is_ordinal = 0;

                if (ilt_entry & 0x80000000) {
                    // Import by ordinal (bit 31 set)
                    ordinal = ilt_entry & 0xFFFF;
                    is_ordinal = 1;
                } else {
                    // Import by name: hint (2 bytes) + name string
                    uint32_t hint_name_rva = ilt_entry;
                    if (hint_name_rva + 2 < len) {
                        uint16_t hint = pe_read16_user(LINK_BASE + hint_name_rva);
                        (void)hint;
                        pe_read_buf_user(LINK_BASE + hint_name_rva + 2, func_name, 126);
                        func_name[126] = 0;
                    } else {
                        func_name[0] = '?';
                        func_name[1] = 0;
                    }
                }

                // Resolve: try winapi_lookup first, fall back to stub
                uint32_t impl = 0;
                if (func_name[0]) {
                    impl = winapi_lookup(dll_name, func_name);
                }
                if (impl) {
                    pe_serial_puts("PE:     ");
                    pe_serial_puts(func_name);
                    pe_serial_puts(" → winapi");
                    pe_newline();
                } else {
                    if (is_ordinal) {
                        pe_serial_puts("PE:     ordinal=");
                        pe_dec(ordinal);
                        pe_serial_puts(" → stub");
                        pe_newline();
                    } else {
                        pe_serial_puts("PE:     ");
                        pe_serial_puts(func_name);
                        pe_serial_puts(" → stub");
                        pe_newline();
                    }
                    impl = (uint32_t)stub_addr;
                }

                // Write resolved address into the IAT
                pe_write32_user(LINK_BASE + iat_rva + imp_idx * 4, impl);
            }

            idx++;
        }
        pe_serial_puts("PE: imports resolved");
        pe_newline();
    }

    // ─── Step 4: Map user stack ──────────────────────────────────
    // The stack is placed DIRECTLY ABOVE the highest mapped section
    // (page-aligned, minus a guard page) instead of a fixed 15 MiB
    // offset: large images (DOOM.EXE ≈ 9 MiB with a baked-in WAD)
    // would otherwise collide with the fixed location, and small ones
    // waste nothing.  The region ABOVE the stack stays free for the
    // task's own paging_alloc_private() arena (SYS_ALLOC_PAGE) —
    // paging_alloc_private bumps from the region start, and
    // paging_map_user_va raises priv_next past any explicit map, so
    // runtime pages land after the stack and grow toward the 16 MiB
    // region end.
    uint32_t max_end = LINK_BASE;
    for (uint16_t i = 0; i < coff->NumberOfSections; i++) {
        uint32_t sec_off = (uint32_t)(sec_table - pe) + (uint32_t)i * 40;
        if (sec_off + 40 > len) break;
        const pe_section_header_t* sec =
            (const pe_section_header_t*)(pe + sec_off);
        uint32_t vs = sec->VirtualSize ? sec->VirtualSize : sec->SizeOfRawData;
        if (vs == 0) continue;
        uint32_t end = LINK_BASE + sec->VirtualAddress + vs;
        if (end > max_end) max_end = end;
    }
    uint32_t stack_top = (max_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    stack_top += 0x00100000u;                    // 1 MiB above the image
    const uint32_t stack_pages = 64;             // 256 KiB
    uint32_t stack_va = stack_top - stack_pages * PAGE_SIZE;
    pe_serial_puts("PE: stack VA=");
    pe_hex32(stack_va);
    pe_serial_puts(" top=");
    pe_hex32(stack_top);
    pe_serial_puts(" pages=");
    pe_dec(stack_pages);
    pe_newline();
    if (!paging_map_user_va(TASK_USER_SLOT, stack_va, (int)stack_pages)) {
        pe_serial_puts("PE: stack map failed!");
        pe_newline();
        return -1;
    }
    // priv_next was advanced past the whole image AND stack by the
    // explicit maps above (paging_map_user_va bumps it); SYS_ALLOC_PAGE
    // now hands out fresh pages above the stack, up to the region end.
    // Zero the stack so the PE starts with a clean environment
    {
        uint32_t saved_cr3;
        asm volatile("mov %%cr3, %0" : "=r" (saved_cr3));
        asm volatile("cli");
        asm volatile("mov %0, %%cr3" :: "r" (paging_task_pd(TASK_USER_SLOT)));
        for (uint32_t i = 0; i < stack_pages * PAGE_SIZE; i += 4)
            *(volatile uint32_t*)(stack_va + i) = 0;
        asm volatile("mov %0, %%cr3" :: "r" (saved_cr3));
        asm volatile("sti");
    }

    // ─── Step 5: Map argv page ───────────────────────────────────
    // Copy command-line arguments to a page DIRECTLY ABOVE the stack.
    // (Historically this was the fixed USER_LINK_BASE+14 MiB mark, but
    // that pushed the SYS_ALLOC_PAGE bump frontier to page 3585 of the
    // 4096-page region, leaving only ~2 MiB for runtime pages — a
    // large game like DOOM.EXE then failed its zone allocation.  The
    // stack is already placed image-size-dependently, so tucking the
    // argv page just above it keeps the bump allocator contiguous:
    // image → stack → argv → SYS_ALLOC_PAGE growth.)
    uint32_t argv_va = stack_top;
    pe_serial_puts("PE: argv page=");
    pe_hex32(argv_va);
    pe_newline();
    if (paging_map_user_va(TASK_USER_SLOT, argv_va, 1)) {
        int n = 0;
        if (args) while (args[n] && n < USER_ARGS_MAX - 1) n++;
        // Write args under user CR3 using the helper
        uint32_t saved_cr3;
        asm volatile("mov %%cr3, %0" : "=r" (saved_cr3));
        asm volatile("cli");
        asm volatile("mov %0, %%cr3" :: "r" (paging_task_pd(TASK_USER_SLOT)));
        char* dst = (char*)argv_va;
        for (int i = 0; i < n; i++) dst[i] = args[i];
        dst[n] = '\0';
        asm volatile("mov %0, %%cr3" :: "r" (saved_cr3));
        asm volatile("sti");
    }

    // ─── Step 6: Create user task ────────────────────────────────
    // task_create_user builds a ring-3 interrupt frame on the kernel
    // stack so the first context switch iret's into the PE at ring 3:
    //   CS  = 0x1B (user code segment, DPL 3)
    //   SS  = 0x23 (user data segment, DPL 3)
    //   EIP = entry point (LINK_BASE + entry_rva)
    //   ESP = stack_top
    //   EFLAGS = 0x202 (IF set, IOPL 0)
    //   All GPRs zeroed, DS = 0x23
    uint32_t entry_va = LINK_BASE + entry_rva;
    pe_serial_puts("PE: creating ring-3 task");
    pe_newline();
    pe_serial_puts("  EIP="); pe_hex32(entry_va);
    pe_serial_puts("  ESP="); pe_hex32(stack_top);
    pe_serial_puts("  CS=0x1B SS=0x23 EFLAGS=0x202");
    pe_newline();

    int id = task_create_user(name, entry_va, stack_top);
    if (id < 0) {
        pe_serial_puts("PE: task_create_user FAILED");
        pe_newline();
        return -1;
    }

    pe_serial_puts("PE: loaded successfully (pid=");
    pe_dec(id);
    pe_serial_puts(")");
    pe_newline();
    pe_serial_puts("PE: execution environment ready");
    pe_newline();
    pe_serial_puts("  User code:  0x1B (DPL3, ring3)");
    pe_newline();
    pe_serial_puts("  User data:  0x23 (DPL3, ring3)");
    pe_newline();
    pe_serial_puts("  Kernel stk: task_stacks[1] (16 KiB)");
    pe_newline();
    pe_serial_puts("  TSS esp0:   set on context switch");
    pe_newline();
    return id;
}
