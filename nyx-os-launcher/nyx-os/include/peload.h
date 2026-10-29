// include/peload.h  –  PE32 (Windows .exe) loader for ring-3 user programs.
//
// Parses a Windows PE32 executable loaded into memory:
//   1. Validate MZ (DOS) and PE\0\0 signatures
//   2. Read COFF + Optional header (PE32 only, not PE32+)
//   3. Walk section headers: map .text (code), .data, .rdata (read-only data)
//   4. Zero .bss (VirtualSize > SizeOfRawData)
//   5. Stub-import DLL functions (MessageBoxA, ExitProcess, GetProcAddress, etc.)
//   6. Apply base relocations if ImageBase conflicts with our link address
//   7. Set up a user stack, then task_create_user() to ring 3
//
// This mirrors the ELF loader (elfload.h) — same slot, same link address,
// same stack layout — so the two formats are interchangeable from the
// scheduler's perspective.

#ifndef PELOAD_H
#define PELOAD_H

#include <stdint.h>
#include <paging.h>

// ─── PE format constants ─────────────────────────────────────────
#define PE_MZ_MAGIC         0x5A4D          // "MZ"
#define PE_SIGNATURE        0x00004550      // "PE\0\0"
#define PE_MACHINE_I386     0x014C          // IMAGE_FILE_MACHINE_I386
#define PE_MACHINE_AMD64    0x8664          // IMAGE_FILE_MACHINE_AMD64
#define PE_EXECUTABLE_IMAGE 0x0001          // IMAGE_FILE_EXECUTABLE_IMAGE
#define PE_DLL              0x2000          // IMAGE_FILE_DLL

// Section flags
#define PE_SCN_CNT_CODE         0x00000020
#define PE_SCN_CNT_INIT_DATA    0x00000040
#define PE_SCN_CNT_UNINIT_DATA  0x00000080
#define PE_SCN_MEM_EXECUTE      0x20000000
#define PE_SCN_MEM_READ         0x40000000
#define PE_SCN_MEM_WRITE        0x80000000
#define PE_SCN_MEM_DISCARDABLE  0x02000000

// ImageRelocation types
#define PE_REL_BASED_ABSOLUTE  0
#define PE_REL_BASED_HIGHLOW   3

// ─── Minimal PE structures (no <winnt.h> in a freestanding kernel) ──

// DOS Header (at offset 0 of the file)
typedef struct {
    uint16_t e_magic;       // 0x00  MZ
    uint16_t e_cblp;        // 0x02
    uint16_t e_cp;          // 0x04
    uint16_t e_crlc;        // 0x06
    uint16_t e_cparhdr;     // 0x08
    uint16_t e_minalloc;    // 0x0A
    uint16_t e_maxalloc;    // 0x0C
    uint16_t e_ss;          // 0x0E
    uint16_t e_sp;          // 0x10
    uint16_t e_csum;        // 0x12
    uint16_t e_ip;          // 0x14
    uint16_t e_cs;          // 0x16
    uint16_t e_lfarlc;      // 0x18
    uint16_t e_ovno;        // 0x1A
    uint16_t e_res[4];      // 0x1C
    uint16_t e_oemid;       // 0x24
    uint16_t e_oeminfo;     // 0x26
    uint16_t e_res2[10];    // 0x28
    uint32_t e_lfanew;      // 0x3C  offset to PE signature
} __attribute__((packed)) pe_mz_header_t;

// COFF File Header (immediately after PE signature)
typedef struct {
    uint16_t Machine;
    uint16_t NumberOfSections;
    uint32_t TimeDateStamp;
    uint32_t PointerToSymbolTable;
    uint32_t NumberOfSymbols;
    uint16_t SizeOfOptionalHeader;
    uint16_t Characteristics;
} __attribute__((packed)) pe_coff_header_t;

// PE32 Optional Header (standard fields only — we skip data directories)
typedef struct {
    uint16_t Magic;                 // 0x10B = PE32, 0x20B = PE32+
    uint8_t  MajorLinkerVersion;
    uint8_t  MinorLinkerVersion;
    uint32_t SizeOfCode;
    uint32_t SizeOfInitializedData;
    uint32_t SizeOfUninitializedData;
    uint32_t AddressOfEntryPoint;   // RVA of the entry point
    uint32_t BaseOfCode;
    uint32_t BaseOfData;            // PE32 only (absent in PE32+)
    uint32_t ImageBase;
    uint32_t SectionAlignment;
    uint32_t FileAlignment;
    uint16_t MajorOperatingSystemVersion;
    uint16_t MinorOperatingSystemVersion;
    uint16_t MajorImageVersion;
    uint16_t MinorImageVersion;
    uint16_t MajorSubsystemVersion;
    uint16_t MinorSubsystemVersion;
    uint32_t Win32VersionValue;
    uint32_t SizeOfImage;
    uint32_t SizeOfHeaders;
    uint32_t CheckSum;
    uint16_t Subsystem;
    uint16_t DllCharacteristics;
    uint32_t SizeOfStackReserve;
    uint32_t SizeOfStackCommit;
    uint32_t SizeOfHeapReserve;
    uint32_t SizeOfHeapCommit;
    uint32_t LoaderFlags;
    uint32_t NumberOfRvaAndSizes;
} __attribute__((packed)) pe_optional_header32_t;

// Data directory entry (we only need Import and Relocation)
typedef struct {
    uint32_t VirtualAddress;    // RVA
    uint32_t Size;
} __attribute__((packed)) pe_data_dir_t;

// Section header (40 bytes each)
typedef struct {
    char     Name[8];               // null-padded, e.g. ".text\0\0\0"
    uint32_t VirtualSize;           // memory size (Paged 0 → SizeOfRawData)
    uint32_t VirtualAddress;        // RVA
    uint32_t SizeOfRawData;
    uint32_t PointerToRawData;
    uint32_t PointerToRelocations;
    uint32_t PointerToLinenumbers;
    uint16_t NumberOfRelocations;
    uint16_t NumberOfLinenumbers;
    uint32_t Characteristics;       // flags: code, data, executable, etc.
} __attribute__((packed)) pe_section_header_t;

// Import descriptor (one per DLL)
typedef struct {
    uint32_t OriginalFirstThunk;    // RVA to INT (Import Name Table)
    uint32_t TimeDateStamp;
    uint32_t ForwarderChain;
    uint32_t Name;                  // RVA to DLL name string
    uint32_t FirstThunk;            // RVA to IAT (Import Address Table)
} __attribute__((packed)) pe_import_desc_t;

// Import by ordinal (high bit of ordinal hint set)
// Import by name: uint16_t hint + char name[]
typedef struct {
    uint16_t Hint;          // hint index into the export table
    char     Name[1];       // null-terminated name string
} __attribute__((packed)) pe_import_by_name_t;

// Relocation block header (relocation directory entry → block → entries)
typedef struct {
    uint32_t VirtualAddress;
    uint32_t SizeOfBlock;
} __attribute__((packed)) pe_reloc_block_t;

// ─── API ──────────────────────────────────────────────────────────

// Load a PE32 executable from a memory buffer into a ring-3 user task.
// `pe` points to the raw file bytes (starting at MZ), `len` is the total size.
// Returns the task id (>= 0) on success, -1 on error.
int pe_exec(const unsigned char* pe, uint32_t len, const char* name,
            const char* args);

// Detect whether a memory buffer looks like a PE executable
// (checks MZ + PE signature + i386 machine).
int pe_is_valid(const unsigned char* data, uint32_t len);

// Print PE header summary to serial (for diagnostics).
void pe_dump_header(const unsigned char* data, uint32_t len);

#endif
