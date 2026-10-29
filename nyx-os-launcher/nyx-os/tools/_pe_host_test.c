// tools/_pe_host_test.c — Host-side test for the PE parser.
// Compiles on the host and validates the PE header parsing logic
// without needing the kernel (no paging, no task_create_user).
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

// ─── PE structures (copied from peload.h) ───────────────────────
#define PE_MZ_MAGIC     0x5A4D
#define PE_SIGNATURE    0x00004550
#define PE_MACHINE_I386 0x014C

typedef struct {
    uint16_t e_magic;
    uint16_t e_cblp, e_cp, e_crlc, e_cparhdr, e_minalloc, e_maxalloc;
    uint16_t e_ss, e_sp, e_csum, e_ip, e_cs, e_lfarlc, e_ovno;
    uint16_t e_res[4];
    uint16_t e_oemid, e_oeminfo;
    uint16_t e_res2[10];
    uint32_t e_lfanew;
} __attribute__((packed)) pe_mz_header_t;

typedef struct {
    uint16_t Machine;
    uint16_t NumberOfSections;
    uint32_t TimeDateStamp;
    uint32_t PointerToSymbolTable;
    uint32_t NumberOfSymbols;
    uint16_t SizeOfOptionalHeader;
    uint16_t Characteristics;
} __attribute__((packed)) pe_coff_header_t;

typedef struct {
    uint16_t Magic;
    uint8_t  MajorLinkerVersion, MinorLinkerVersion;
    uint32_t SizeOfCode, SizeOfInitializedData, SizeOfUninitializedData;
    uint32_t AddressOfEntryPoint, BaseOfCode, BaseOfData;
    uint32_t ImageBase, SectionAlignment, FileAlignment;
    uint16_t MajorOperatingSystemVersion, MinorOperatingSystemVersion;
    uint16_t MajorImageVersion, MinorImageVersion;
    uint16_t MajorSubsystemVersion, MinorSubsystemVersion;
    uint32_t Win32VersionValue, SizeOfImage, SizeOfHeaders, CheckSum;
    uint16_t Subsystem, DllCharacteristics;
    uint32_t SizeOfStackReserve, SizeOfStackCommit;
    uint32_t SizeOfHeapReserve, SizeOfHeapCommit;
    uint32_t LoaderFlags, NumberOfRvaAndSizes;
} __attribute__((packed)) pe_optional_header32_t;

typedef struct {
    char     Name[8];
    uint32_t VirtualSize, VirtualAddress;
    uint32_t SizeOfRawData, PointerToRawData;
    uint32_t PointerToRelocations, PointerToLinenumbers;
    uint16_t NumberOfRelocations, NumberOfLinenumbers;
    uint32_t Characteristics;
} __attribute__((packed)) pe_section_header_t;

// ─── Validation (mirrors peload.c pe_is_valid) ──────────────────
int pe_is_valid(const unsigned char* data, uint32_t len) {
    if (len < sizeof(pe_mz_header_t)) return 0;
    uint16_t mz = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
    if (mz != PE_MZ_MAGIC) return 0;
    const pe_mz_header_t* dos = (const pe_mz_header_t*)data;
    uint32_t pe_off = dos->e_lfanew;
    if (pe_off + 4 + sizeof(pe_coff_header_t) > len) return 0;
    uint32_t sig = (uint32_t)data[pe_off] | ((uint32_t)data[pe_off+1] << 8) |
                   ((uint32_t)data[pe_off+2] << 16) | ((uint32_t)data[pe_off+3] << 24);
    if (sig != PE_SIGNATURE) return 0;
    const pe_coff_header_t* coff = (const pe_coff_header_t*)(data + pe_off + 4);
    if (coff->Machine != PE_MACHINE_I386) return 0;
    return 1;
}

// ─── Test: generate a PE, validate, parse headers ───────────────
static unsigned char* build_test_pe(uint32_t* out_size) {
    uint32_t file_align = 0x200;
    uint32_t sec_align = 0x1000;
    uint32_t text_size = 16;
    uint32_t entry_rva = 0x1000;
    uint32_t image_base = 0x00400000;

    uint32_t header_size = file_align;  // 512 bytes
    uint32_t total = header_size + file_align;  // +1 section body
    unsigned char* pe = calloc(1, total);

    // DOS header
    pe_mz_header_t* dos = (pe_mz_header_t*)pe;
    dos->e_magic = PE_MZ_MAGIC;
    dos->e_lfanew = sizeof(pe_mz_header_t);  // 64

    // PE signature
    uint32_t pe_off = dos->e_lfanew;
    pe[pe_off] = 'P'; pe[pe_off+1] = 'E'; pe[pe_off+2] = 0; pe[pe_off+3] = 0;

    // COFF header
    pe_coff_header_t* coff = (pe_coff_header_t*)(pe + pe_off + 4);
    coff->Machine = PE_MACHINE_I386;
    coff->NumberOfSections = 1;
    coff->SizeOfOptionalHeader = sizeof(pe_optional_header32_t);

    // Optional header
    pe_optional_header32_t* opt = (pe_optional_header32_t*)(pe + pe_off + 4 + sizeof(pe_coff_header_t));
    opt->Magic = 0x10B;
    opt->AddressOfEntryPoint = entry_rva;
    opt->ImageBase = image_base;
    opt->SectionAlignment = sec_align;
    opt->FileAlignment = file_align;
    opt->SizeOfImage = sec_align * 2;
    opt->SizeOfHeaders = file_align;
    opt->Subsystem = 3;  // CONSOLE
    opt->NumberOfRvaAndSizes = 16;

    // Section header
    pe_section_header_t* sec = (pe_section_header_t*)(pe + pe_off + 4 + sizeof(pe_coff_header_t)
                                                      + sizeof(pe_optional_header32_t));
    memcpy(sec->Name, ".text\0\0\0", 8);
    sec->VirtualSize = text_size;
    sec->VirtualAddress = 0x1000;
    sec->SizeOfRawData = text_size;
    sec->PointerToRawData = file_align;
    sec->Characteristics = 0x60000020;  // CODE | EXECUTE | READ

    // Code body: xor eax,eax; ret; nop...
    pe[file_align] = 0x31; pe[file_align+1] = 0xC0;
    pe[file_align+2] = 0xC3;

    *out_size = total;
    return pe;
}

int main(void) {
    int pass = 0, fail = 0;

    // Test 1: valid PE detected
    uint32_t pe_size;
    unsigned char* pe = build_test_pe(&pe_size);
    if (pe_is_valid(pe, pe_size)) {
        printf("  [PASS] Test 1: PE validation succeeded\n");
        pass++;
    } else {
        printf("  [FAIL] Test 1: PE validation failed\n");
        fail++;
    }

    // Test 2: wrong magic → rejected
    pe[0] = 'X';
    if (!pe_is_valid(pe, pe_size)) {
        printf("  [PASS] Test 2: Bad MZ magic rejected\n");
        pass++;
    } else {
        printf("  [FAIL] Test 2: Bad MZ magic accepted\n");
        fail++;
    }
    pe[0] = 'M';

    // Test 3: too short → rejected
    if (!pe_is_valid(pe, 4)) {
        printf("  [PASS] Test 3: Short buffer rejected\n");
        pass++;
    } else {
        printf("  [FAIL] Test 3: Short buffer accepted\n");
        fail++;
    }

    // Test 4: corrupt PE sig → rejected
    uint32_t pe_off = ((pe_mz_header_t*)pe)->e_lfanew;
    pe[pe_off] = 'X';
    if (!pe_is_valid(pe, pe_size)) {
        printf("  [PASS] Test 4: Bad PE sig rejected\n");
        pass++;
    } else {
        printf("  [FAIL] Test 4: Bad PE sig accepted\n");
        fail++;
    }
    pe[pe_off] = 'P';

    // Test 5: wrong machine → rejected
    pe_coff_header_t* coff = (pe_coff_header_t*)(pe + pe_off + 4);
    uint16_t saved_machine = coff->Machine;
    coff->Machine = 0x8664;  // AMD64
    if (!pe_is_valid(pe, pe_size)) {
        printf("  [PASS] Test 5: AMD64 machine rejected\n");
        pass++;
    } else {
        printf("  [FAIL] Test 5: AMD64 machine accepted\n");
        fail++;
    }
    coff->Machine = saved_machine;

    // Test 6: parse headers
    pe_optional_header32_t* opt = (pe_optional_header32_t*)(pe + pe_off + 4 + sizeof(pe_coff_header_t));
    pe_section_header_t* sec = (pe_section_header_t*)(pe + pe_off + 4 + sizeof(pe_coff_header_t)
                                                      + sizeof(pe_optional_header32_t));
    if (opt->Magic == 0x10B && opt->AddressOfEntryPoint == 0x1000 &&
        opt->ImageBase == 0x00400000 && opt->Subsystem == 3) {
        printf("  [PASS] Test 6: Optional header fields correct\n");
        printf("           Entry=0x%08X  ImageBase=0x%08X  Subsystem=%d\n",
               opt->AddressOfEntryPoint, opt->ImageBase, opt->Subsystem);
        pass++;
    } else {
        printf("  [FAIL] Test 6: Optional header mismatch\n");
        fail++;
    }

    // Test 7: section header
    if (sec->VirtualAddress == 0x1000 && sec->VirtualSize == 16 &&
        sec->SizeOfRawData == 16 && sec->PointerToRawData == 0x200 &&
        (sec->Characteristics & 0x60000020) == 0x60000020) {
        printf("  [PASS] Test 7: .text section header correct\n");
        printf("           VA=0x%08X VSize=%d RawSize=%d Flags=0x%08X\n",
               sec->VirtualAddress, sec->VirtualSize, sec->SizeOfRawData,
               sec->Characteristics);
        pass++;
    } else {
        printf("  [FAIL] Test 7: .text section mismatch\n");
        fail++;
    }

    // Test 8: verify code bytes
    uint8_t* code = pe + sec->PointerToRawData;
    if (code[0] == 0x31 && code[1] == 0xC0 && code[2] == 0xC3) {
        printf("  [PASS] Test 8: Code bytes correct (xor eax,eax; ret)\n");
        pass++;
    } else {
        printf("  [FAIL] Test 8: Code bytes wrong (0x%02X 0x%02X 0x%02X)\n",
               code[0], code[1], code[2]);
        fail++;
    }

    // Test 9: test.exe from the build
    FILE* f = fopen("/tmp/test.exe", "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        unsigned char* buf = malloc(sz);
        fread(buf, 1, sz, f);
        fclose(f);
        if (pe_is_valid(buf, (uint32_t)sz)) {
            pe_coff_header_t* c2 = (pe_coff_header_t*)(buf + ((pe_mz_header_t*)buf)->e_lfanew + 4);
            pe_optional_header32_t* o2 = (pe_optional_header32_t*)((unsigned char*)c2 + sizeof(pe_coff_header_t));
            printf("  [PASS] Test 9: /tmp/test.exe validated\n");
            printf("           Machine=0x%04X Sections=%d Entry=0x%08X\n",
                   c2->Machine, c2->NumberOfSections, o2->AddressOfEntryPoint);
            pass++;
        } else {
            printf("  [FAIL] Test 9: /tmp/test.exe failed validation\n");
            fail++;
        }
        free(buf);
    }

    free(pe);
    printf("\n  Results: %d passed, %d failed\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
