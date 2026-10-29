#!/usr/bin/env python3
"""Generate PE32 executables for testing the PE loader.

Generates three test binaries:
  1. test.exe        — minimal PE with no imports (existing test)
  2. test_imports.exe — PE with imports from user32.dll + relocations
  3. test_reloc.exe   — PE with relocation table entries
"""
import struct, sys, os

def build_pe_basic():
    """Minimal PE: .text only, no imports, no relocations."""
    dos = bytearray(64)
    struct.pack_into('<H', dos, 0, 0x5A4D)
    struct.pack_into('<I', dos, 60, 64)
    pe_sig = b'PE\x00\x00'
    coff = struct.pack('<HHIIIHH', 0x014C, 1, 0, 0, 0, 0xE0, 0x0102)
    opt = struct.pack('<HBBIIIIIIIIIHHHHHHIIIIHHIIIIII',
        0x10B, 0, 0, 16, 0, 0, 0x1000, 0x1000, 0,
        0x00400000, 0x1000, 0x200,
        4, 0, 0, 0, 4, 0, 0, 0x3000, 0x200, 0, 3, 0,
        0x100000, 0x1000, 0x100000, 0x1000, 0, 16)
    data_dirs = b'\x00' * 128
    text_code = bytes([0x31, 0xC0, 0xC3] + [0x90] * 13)
    sec = struct.pack('<8sIIIIIIHHI',
        b'.text\x00\x00\x00', len(text_code), 0x1000, len(text_code),
        0x200, 0, 0, 0, 0, 0x60000020)
    header = dos + pe_sig + coff + opt + data_dirs + sec
    header += b'\x00' * (0x200 - len(header))
    text_body = text_code + b'\x00' * (0x200 - len(text_code))
    return header + text_body


def build_pe_imports():
    """PE with .text + .data + .idata (imports from user32.dll).

    The code does:
        mov eax, [IAT_entry]   ; load function pointer from IAT
        call eax               ; call it (will hit the stub)
        xor eax, eax
        ret

    The .data section contains a string "Hello from PE!\r\n$".
    The import table imports MessageBoxA from user32.dll.
    A relocation entry patches the IAT pointer reference.
    """
    file_align = 0x200
    sec_align  = 0x1000
    image_base = 0x00400000
    entry_rva  = 0x1000    # .text at RVA 0x1000
    data_rva   = 0x2000    # .data at RVA 0x2000
    idata_rva  = 0x3000    # .idata at RVA 0x3000

    # ─── .text section code ──────────────────────────────────────
    # mov eax, [0x403000+0x10]  → load MessageBoxA pointer from IAT
    #   8B 05 10 30 40 00        (mov eax, [addr])
    # call eax                   → FF D0
    # xor eax, eax               → 31 C0
    # ret                        → C3
    # nop padding
    text_code = bytes([
        0x8B, 0x05,                       # mov eax, [addr]
        0x00, 0x30, 0x40, 0x00,          # addr = image_base + idata_rva + 0x10 (IAT entry 4)
        0xFF, 0xD0,                       # call eax
        0x31, 0xC0,                       # xor eax, eax
        0xC3,                             # ret
    ] + [0x90] * 5)  # nop padding to 16 bytes

    # ─── .data section content ───────────────────────────────────
    data_content = b"Hello from PE!\r\n\x00"
    data_content += b"\x00" * (16 - len(data_content))  # pad to 16

    # ─── .idata section (Import Directory + IAT + ILT + DLL name) ──
    # Layout within .idata (all RVAs relative to idata_rva):
    #   offset 0x00: Import Directory Table entry for user32.dll (20 bytes)
    #   offset 0x14: Null terminator entry (20 bytes)
    #   offset 0x28: ILT entry (4 bytes) — hint/name RVAs
    #   offset 0x2C: ILT null terminator (4 bytes)
    #   offset 0x30: IAT entry (4 bytes) — filled by loader
    #   offset 0x34: IAT null terminator (4 bytes)
    #   offset 0x38: DLL name string "user32.dll\0"
    #   offset 0x43: Hint+Name for MessageBoxA

    dll_name_offset = 0x38
    hint_name_offset = 0x43

    idata = bytearray(256)

    # Import Directory Entry for user32.dll
    # OriginalFirstThunk → ILT (offset 0x28 from idata start)
    struct.pack_into('<I', idata, 0x00, idata_rva + 0x28)
    struct.pack_into('<I', idata, 0x04, 0)  # TimeDateStamp
    struct.pack_into('<I', idata, 0x08, 0)  # ForwarderChain
    struct.pack_into('<I', idata, 0x0C, idata_rva + dll_name_offset)  # Name
    struct.pack_into('<I', idata, 0x10, idata_rva + 0x30)  # FirstThunk → IAT

    # Null terminator entry
    # (already zero from bytearray)

    # ILT (Import Lookup Table) — one entry + null
    struct.pack_into('<I', idata, 0x28, idata_rva + hint_name_offset)  # Hint/Name RVA
    # ILT null terminator at offset 0x2C (already zero)

    # IAT (Import Address Table) — one entry + null
    # The loader fills this with the stub address
    # Initially zero (the loader overwrites it)
    # IAT null terminator at offset 0x34 (already zero)

    # DLL name string
    dll_name = b"user32.dll\x00"
    idata[dll_name_offset:dll_name_offset+len(dll_name)] = dll_name

    # Hint + Name for MessageBoxA
    struct.pack_into('<H', idata, hint_name_offset, 0)  # Hint (unused)
    func_name = b"MessageBoxA\x00"
    idata[hint_name_offset+2:hint_name_offset+2+len(func_name)] = func_name

    idata = bytes(idata)

    # ─── Build Optional Header ───────────────────────────────────
    # NumberOfRvaAndSizes = 16, data directories at offset 96
    # Import Directory = index 1, Relocation = index 5

    total_idata = len(idata)
    total_text = len(text_code)
    total_data = len(data_content)
    size_of_image = sec_align * 5  # headers + .text + .data + .idata + spare

    opt = struct.pack('<HBBIIIIIIIIIHHHHHHIIIIHHIIIIII',
        0x10B,                  # Magic = PE32
        0, 0,                   # Linker version
        total_text,             # SizeOfCode
        total_data,             # SizeOfInitializedData
        0,                      # SizeOfUninitializedData
        entry_rva,              # AddressOfEntryPoint
        0x1000,                 # BaseOfCode
        0,                      # BaseOfData
        image_base,             # ImageBase
        sec_align,              # SectionAlignment
        file_align,             # FileAlignment
        4, 0,                   # OS version
        0, 0,                   # Image version
        4, 0,                   # Subsystem version
        0,                      # Win32VersionValue
        size_of_image,          # SizeOfImage
        file_align,             # SizeOfHeaders
        0,                      # CheckSum
        3,                      # Subsystem = CONSOLE
        0x8160,                 # DllCharacteristics (DYNAMIC_BASE | NX_COMPAT)
        0x100000,               # StackReserve
        0x1000,                 # StackCommit
        0x100000,               # HeapReserve
        0x1000,                 # HeapCommit
        0,                      # LoaderFlags
        16,                     # NumberOfRvaAndSizes
    )

    # Data directories (16 × 8 = 128 bytes)
    data_dirs = bytearray(128)
    # Import Directory (index 1)
    struct.pack_into('<II', data_dirs, 8, idata_rva, 40)  # VA, Size
    # No relocation directory — this test has no relocations
    data_dirs = bytes(data_dirs)

    # ─── COFF Header ─────────────────────────────────────────────
    coff = struct.pack('<HHIIIHH',
        0x014C,     # Machine = i386
        3,          # NumberOfSections (.text, .data, .idata)
        0, 0, 0,    # TimeDateStamp, SymbolTable, NumSymbols
        0xE0,       # SizeOfOptionalHeader
        0x0102,     # Characteristics = EXECUTABLE_IMAGE | 32BIT_MACHINE
    )

    # ─── Section Headers (40 bytes each) ────────────────────────
    def make_sec(name, vsize, va, rsize, raw_off, chars):
        return struct.pack('<8sIIIIIIHHI',
            name, vsize, va, rsize, raw_off, 0, 0, 0, 0, chars)

    sec_text  = make_sec(b'.text\x00\x00\x00', total_text, 0x1000,
                         total_text, file_align, 0x60000020)
    sec_data  = make_sec(b'.data\x00\x00\x00', total_data, data_rva,
                         total_data, file_align * 2, 0xC0000040)
    sec_idata = make_sec(b'.idata\x00\x00', total_idata, idata_rva,
                         total_idata, file_align * 3, 0xC0000040)

    # ─── Assemble ────────────────────────────────────────────────
    header = bytearray()
    header += bytearray(64)  # DOS header placeholder
    dos = bytearray(header)
    struct.pack_into('<H', dos, 0, 0x5A4D)
    struct.pack_into('<I', dos, 60, 64)
    header = bytes(dos)

    pe_sig = b'PE\x00\x00'
    pe_off = len(header)
    header += pe_sig + coff + opt + data_dirs
    header += sec_text + sec_data + sec_idata

    # Pad header to file_align
    header += b'\x00' * (file_align - len(header))

    # Section bodies
    text_body = text_code + b'\x00' * (file_align - len(text_code))
    data_body = data_content + b'\x00' * (file_align - len(data_content))
    idata_body = idata + b'\x00' * (file_align - len(idata))

    return header + text_body + data_body + idata_body


def build_pe_reloc():
    """PE with .text + relocation entries (HIGHLOW patches).

    The .text section contains two 32-bit pointers that need relocation:
      1. A direct address reference (call/jmp target)
      2. A data pointer
    The relocation table has one block with two HIGHLOW entries.
    """
    file_align = 0x200
    sec_align  = 0x1000
    image_base = 0x00400000
    entry_rva  = 0x1000
    reloc_rva  = 0x2000

    # ─── .text section code ──────────────────────────────────────
    # The code contains two absolute addresses that need relocation:
    #   addr1 = 0x00401020 (data reference in .text itself)
    #   addr2 = 0x00402000 (data pointer to .data section)
    text_code = bytes([
        # addr1: a 32-bit absolute address (to be relocated)
        0x20, 0x10, 0x40, 0x00,   # 0x00401020
        # addr2: another 32-bit absolute address
        0x00, 0x20, 0x40, 0x00,   # 0x00402000
        # xor eax, eax; ret
        0x31, 0xC0, 0xC3,
    ] + [0x90] * 5)  # pad to 16 bytes

    # ─── Relocation table ────────────────────────────────────────
    # One block covering the page at RVA 0x1000:
    #   VirtualAddress = 0x1000
    #   SizeOfBlock   = 12 (8 header + 2 entries × 2 bytes)
    #   Entry 0: type=3 (HIGHLOW), offset=0x000 → patch at RVA 0x1000
    #   Entry 1: type=3 (HIGHLOW), offset=0x004 → patch at RVA 0x1004
    reloc_block = bytearray()
    reloc_block += struct.pack('<II', 0x1000, 12)  # VirtualAddress, SizeOfBlock
    reloc_block += struct.pack('<H', (3 << 12) | 0x000)  # HIGHLOW @ offset 0
    reloc_block += struct.pack('<H', (3 << 12) | 0x004)  # HIGHLOW @ offset 4
    reloc_block = bytes(reloc_block)

    # ─── .reloc section ──────────────────────────────────────────
    reloc_section = reloc_block
    # Pad to file_align
    reloc_section += b'\x00' * (file_align - len(reloc_section))

    # ─── Build headers ───────────────────────────────────────────
    total_text = len(text_code)
    total_reloc = len(reloc_block)
    size_of_image = sec_align * 4

    opt = struct.pack('<HBBIIIIIIIIIHHHHHHIIIIHHIIIIII',
        0x10B, 0, 0, total_text, 0, 0,
        entry_rva, 0x1000, 0,
        image_base, sec_align, file_align,
        4, 0, 0, 0, 4, 0, 0, size_of_image, file_align,
        0, 3, 0, 0x100000, 0x1000, 0x100000, 0x1000, 0, 16)

    data_dirs = bytearray(128)
    struct.pack_into('<II', data_dirs, 40, reloc_rva, total_reloc)  # Relocation = index 5
    data_dirs = bytes(data_dirs)

    coff = struct.pack('<HHIIIHH', 0x014C, 2, 0, 0, 0, 0xE0, 0x0102)

    sec_text  = struct.pack('<8sIIIIIIHHI',
        b'.text\x00\x00\x00', total_text, 0x1000, total_text,
        file_align, 0, 0, 0, 0, 0x60000020)
    sec_reloc = struct.pack('<8sIIIIIIHHI',
        b'.reloc\x00\x00', total_reloc, reloc_rva, total_reloc,
        file_align * 2, 0, 0, 0, 0, 0x42000040)

    dos = bytearray(64)
    struct.pack_into('<H', dos, 0, 0x5A4D)
    struct.pack_into('<I', dos, 60, 64)

    header = bytes(dos) + b'PE\x00\x00' + coff + opt + data_dirs + sec_text + sec_reloc
    header += b'\x00' * (file_align - len(header))

    text_body = text_code + b'\x00' * (file_align - len(text_code))

    return header + text_body + reloc_section


if __name__ == '__main__':
    outdir = sys.argv[1] if len(sys.argv) > 1 else '/tmp'

    # 1. Basic test.exe
    pe = build_pe_basic()
    path = os.path.join(outdir, 'test.exe')
    with open(path, 'wb') as f:
        f.write(pe)
    print(f"  test.exe: {len(pe)} bytes (basic, no imports)")

    # 2. test_imports.exe
    pe = build_pe_imports()
    path = os.path.join(outdir, 'test_imports.exe')
    with open(path, 'wb') as f:
        f.write(pe)
    print(f"  test_imports.exe: {len(pe)} bytes (imports from user32.dll)")

    # 3. test_reloc.exe
    pe = build_pe_reloc()
    path = os.path.join(outdir, 'test_reloc.exe')
    with open(path, 'wb') as f:
        f.write(pe)
    print(f"  test_reloc.exe: {len(pe)} bytes (relocation entries)")

    # Validate all three
    for name in ['test.exe', 'test_imports.exe', 'test_reloc.exe']:
        path = os.path.join(outdir, name)
        with open(path, 'rb') as f:
            data = f.read()
        assert data[:2] == b'MZ', f"{name}: bad MZ"
        pe_off = struct.unpack_from('<I', data, 60)[0]
        assert data[pe_off:pe_off+4] == b'PE\x00\x00', f"{name}: bad PE sig"
        coff_off = pe_off + 4
        machine = struct.unpack_from('<H', data, coff_off)[0]
        nsec = struct.unpack_from('<H', data, coff_off + 2)[0]
        opt_off = coff_off + 20
        entry = struct.unpack_from('<I', data, opt_off + 16)[0]
        ibase = struct.unpack_from('<I', data, opt_off + 28)[0]
        # Check data directories
        dd_off = opt_off + 96
        imp_va = struct.unpack_from('<I', data, dd_off + 8)[0]
        imp_sz = struct.unpack_from('<I', data, dd_off + 12)[0]
        rel_va = struct.unpack_from('<I', data, dd_off + 40)[0]
        rel_sz = struct.unpack_from('<I', data, dd_off + 44)[0]
        print(f"  {name}: Machine=0x{machine:04X} Sec={nsec} Entry=0x{entry:08X} "
              f"Base=0x{ibase:08X} Imp=0x{imp_va:08X}/{imp_sz} Reloc=0x{rel_va:08X}/{rel_sz}")

    print("  All PE files validated OK")
