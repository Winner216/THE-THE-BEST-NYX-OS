#!/usr/bin/env python3
"""Generate a PE32 that imports kernel32.dll functions and calls them.

The generated code:
  1. Pushes args for WriteConsoleA(GetStdHandle(-11), "Hello from PE!\r\n", 16, &written, 0)
  2. Calls GetStdHandle(-11) → returns stdout handle
  3. Calls WriteConsoleA(handle, msg, 16, &written, 0)
  4. Calls ExitProcess(0)
  5. Ret (unreachable)

This tests the full pipeline: PE parse → section map → import resolution
→ WinAPI stub → terminal output.
"""
import struct, sys, os

def build_winapi_test_pe():
    file_align = 0x200
    sec_align  = 0x1000
    image_base = 0x00400000
    entry_rva  = 0x1000
    data_rva   = 0x2000
    idata_rva  = 0x3000

    # ─── .text section code ──────────────────────────────────────
    # WinAPI calling convention (stdcall): args pushed right-to-left,
    # callee cleans the stack.
    #
    # GetStdHandle(STD_OUTPUT_HANDLE = -11):
    #   push -11
    #   call [IAT_GetStdHandle]
    #   ; eax = stdout handle
    #
    # WriteConsoleA(handle, msg, 16, &written, 0):
    #   push 0              ; lpReserved
    #   push written_va     ; lpNumberOfCharsWritten
    #   push 16             ; nNumberOfCharsToWrite
    #   push msg_va         ; lpBuffer
    #   push eax            ; hConsoleOutput (from GetStdHandle)
    #   call [IAT_WriteConsoleA]
    #
    # ExitProcess(0):
    #   push 0
    #   call [IAT_ExitProcess]

    # IAT offsets from image_base:
    # IAT_GetStdHandle    = idata_rva + 0x30  (offset 0x30 in .idata)
    # IAT_WriteConsoleA   = idata_rva + 0x34
    # IAT_ExitProcess     = idata_rva + 0x38

    iat_ghi   = image_base + idata_rva + 0x30   # GetStdHandle
    iat_wca   = image_base + idata_rva + 0x34   # WriteConsoleA
    iat_ep    = image_base + idata_rva + 0x38   # ExitProcess
    msg_va    = image_base + data_rva + 0       # "Hello from PE!\r\n"
    written_va = image_base + data_rva + 0x20   # DWORD for written count

    code = bytearray()

    # GetStdHandle(STD_OUTPUT_HANDLE)
    code += bytes([0x68])                        # push imm32
    code += struct.pack('<i', -11)               # STD_OUTPUT_HANDLE = -11
    code += bytes([0xFF, 0x15])                  # call [abs]
    code += struct.pack('<I', iat_ghi)           # address of IAT entry

    # WriteConsoleA(handle, msg, 16, &written, 0)
    # stdcall: push args right-to-left, callee cleans
    code += bytes([0x6A, 0x00])                  # push 0 (lpReserved)
    code += bytes([0x68])                        # push imm32
    code += struct.pack('<I', written_va)        # &written
    code += bytes([0x6A, 0x10])                  # push 16 (nChars)
    code += bytes([0x68])                        # push imm32
    code += struct.pack('<I', msg_va)            # msg
    code += bytes([0x50])                        # push eax (handle)
    code += bytes([0xFF, 0x15])                  # call [abs]
    code += struct.pack('<I', iat_wca)           # address of IAT entry

    # ExitProcess(0)
    code += bytes([0x6A, 0x00])                  # push 0
    code += bytes([0xFF, 0x15])                  # call [abs]
    code += struct.pack('<I', iat_ep)            # address of IAT entry

    # padding
    while len(code) < 16:
        code.append(0x90)  # nop

    # ─── .data section ───────────────────────────────────────────
    msg = b"Hello from PE!\r\n\x00"
    data = bytearray(64)
    data[0:len(msg)] = msg
    # written count at offset 0x20
    struct.pack_into('<I', data, 0x20, 0)

    # ─── .idata section ──────────────────────────────────────────
    # Layout:
    #   0x00: Import Dir Entry 1 (kernel32.dll, 20 bytes)
    #   0x14: Import Dir Entry 2 (null terminator, 20 bytes)
    #   0x28: ILT entries (4 bytes each) + null
    #   0x30: IAT entries (4 bytes each) + null
    #   0x38: DLL name + func names

    idata = bytearray(256)

    # DLL name at offset 0x60
    dll_name_off = 0x60
    dll_name = b"kernel32.dll\x00"
    idata[dll_name_off:dll_name_off+len(dll_name)] = dll_name

    # Function names
    fn_ghi = b"GetStdHandle\x00"
    fn_wca = b"WriteConsoleA\x00"
    fn_ep  = b"ExitProcess\x00"

    fn_ghi_off = dll_name_off + len(dll_name)
    fn_wca_off = fn_ghi_off + len(fn_ghi)
    fn_ep_off  = fn_wca_off + len(fn_wca)

    idata[fn_ghi_off:fn_ghi_off+len(fn_ghi)] = fn_ghi
    idata[fn_wca_off:fn_wca_off+len(fn_wca)] = fn_wca
    idata[fn_ep_off:fn_ep_off+len(fn_ep)] = fn_ep

    # Hint+Name entries (2-byte hint + name string)
    hn_ghi_off = fn_ep_off + len(fn_ep)
    hn_wca_off = hn_ghi_off + 2 + len(fn_ghi)
    hn_ep_off  = hn_wca_off + 2 + len(fn_wca)

    struct.pack_into('<H', idata, hn_ghi_off, 0)  # hint
    idata[hn_ghi_off+2:hn_ghi_off+2+len(fn_ghi)] = fn_ghi
    struct.pack_into('<H', idata, hn_wca_off, 0)
    idata[hn_wca_off+2:hn_wca_off+2+len(fn_wca)] = fn_wca
    struct.pack_into('<H', idata, hn_ep_off, 0)
    idata[hn_ep_off+2:hn_ep_off+2+len(fn_ep)] = fn_ep

    # ILT entries (offset 0x28)
    struct.pack_into('<I', idata, 0x28, idata_rva + hn_ghi_off)
    struct.pack_into('<I', idata, 0x2C, idata_rva + hn_wca_off)
    struct.pack_into('<I', idata, 0x30, idata_rva + hn_ep_off)
    # ILT null at 0x34 (already zero)

    # IAT entries (offset 0x38) — initially zero, loader fills them
    # IAT null at 0x48 (already zero)

    # Import Directory Entry for kernel32.dll
    struct.pack_into('<I', idata, 0x00, idata_rva + 0x28)   # OriginalFirstThunk → ILT
    struct.pack_into('<I', idata, 0x04, 0)                   # TimeDateStamp
    struct.pack_into('<I', idata, 0x08, 0)                   # ForwarderChain
    struct.pack_into('<I', idata, 0x0C, idata_rva + dll_name_off)  # Name
    struct.pack_into('<I', idata, 0x10, idata_rva + 0x38)   # FirstThunk → IAT

    idata = bytes(idata)

    # ─── Build headers ───────────────────────────────────────────
    total_text = len(code)
    total_data = len(data)
    total_idata = len(idata)
    size_of_image = sec_align * 5

    opt = struct.pack('<HBBIIIIIIIIIHHHHHHIIIIHHIIIIII',
        0x10B, 0, 0, total_text, total_data, 0,
        entry_rva, 0x1000, 0,
        image_base, sec_align, file_align,
        4, 0, 0, 0, 4, 0, 0, size_of_image, file_align,
        0, 3, 0, 0x100000, 0x1000, 0x100000, 0x1000, 0, 16)

    data_dirs = bytearray(128)
    struct.pack_into('<II', data_dirs, 8, idata_rva, 40)  # Import Dir
    data_dirs = bytes(data_dirs)

    coff = struct.pack('<HHIIIHH', 0x014C, 3, 0, 0, 0, 0xE0, 0x0102)

    sec_text  = struct.pack('<8sIIIIIIHHI',
        b'.text\x00\x00\x00', total_text, 0x1000, total_text,
        file_align, 0, 0, 0, 0, 0x60000020)
    sec_data  = struct.pack('<8sIIIIIIHHI',
        b'.data\x00\x00\x00', total_data, data_rva, total_data,
        file_align * 2, 0, 0, 0, 0, 0xC0000040)
    sec_idata = struct.pack('<8sIIIIIIHHI',
        b'.idata\x00\x00', total_idata, idata_rva, total_idata,
        file_align * 3, 0, 0, 0, 0, 0xC0000040)

    dos = bytearray(64)
    struct.pack_into('<H', dos, 0, 0x5A4D)
    struct.pack_into('<I', dos, 60, 64)

    pe_sig = b'PE\x00\x00'
    header = bytes(dos) + pe_sig + coff + opt + data_dirs + sec_text + sec_data + sec_idata
    header += b'\x00' * (file_align - len(header))

    text_body = bytes(code) + b'\x00' * (file_align - len(code))
    data_body = bytes(data) + b'\x00' * (file_align - len(data))
    idata_body = bytes(idata) + b'\x00' * (file_align - len(idata))

    return header + text_body + data_body + idata_body


if __name__ == '__main__':
    outdir = sys.argv[1] if len(sys.argv) > 1 else '/tmp'
    pe = build_winapi_test_pe()
    path = os.path.join(outdir, 'winapi_test.exe')
    with open(path, 'wb') as f:
        f.write(pe)
    print(f"Generated {len(pe)}-byte PE32 → {path}")

    # Validate
    assert pe[:2] == b'MZ'
    pe_off = struct.unpack_from('<I', pe, 60)[0]
    assert pe[pe_off:pe_off+4] == b'PE\x00\x00'
    coff_off = pe_off + 4
    nsec = struct.unpack_from('<H', pe, coff_off + 2)[0]
    opt_off = coff_off + 20
    entry = struct.unpack_from('<I', pe, opt_off + 16)[0]
    ibase = struct.unpack_from('<I', pe, opt_off + 28)[0]
    dd_off = opt_off + 96
    imp_va = struct.unpack_from('<I', pe, dd_off + 8)[0]
    print(f"  Machine=0x014C  Sections={nsec}  Entry=0x{entry:08X}  Base=0x{ibase:08X}")
    print(f"  Import Dir VA=0x{imp_va:08X}")
    print("  ✓ PE validation passed")
