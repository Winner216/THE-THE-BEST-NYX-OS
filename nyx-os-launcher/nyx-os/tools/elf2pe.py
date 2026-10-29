#!/usr/bin/env python3
"""Wrap a flat i386 image into a PE32 executable for pe_exec.

Usage: python3 tools/elf2pe.py <in.bin> <out.exe> <link_base>

Nyx's PE loader (cpu/peload.c) maps sections at USER_LINK_BASE + RVA
(0xC1000000), so the DOOM image is linked directly at its final runtime
base (0xC1001000 for .text) — NO base relocations are needed and the
relocation data directory is emitted empty (VA=0, Size=0), which the
loader treats as "no relocs".

Inputs
  <in.bin>    flat binary: objcopy -O binary of the static ELF.  Offset 0
              corresponds to the ELF's lowest loadable VA (the link base
              of .text); .data/.bss gaps are zero-filled by objcopy.
  <link_base> the absolute VA that offset 0 of in.bin maps to.  The entry
              RVA is computed from the ELF header's e_entry, so the ELF
              path must also be given via --elf (optional; without it the
              entry RVA defaults to 0x1000).

Pipeline (Makefile target `doomexe`):
  gcc -m32 -nostdlib ... -o doom.elf        (static, link base 0xC1001000)
  objcopy -O binary doom.elf doom.bin
  python3 elf2pe.py doom.bin doom.exe 0xC1001000 --elf doom.elf
"""
import struct, sys, optparse

PAGE = 4096
def align(n, a): return (n + a - 1) & ~(a - 1)

def main():
    p = optparse.OptionParser()
    p.add_option("--elf", help="source ELF (for e_entry)")
    (opts, args) = p.parse_args()
    if len(args) < 3:
        sys.exit(__doc__)
    src, dst, base = args[0], args[1], int(args[2], 0)

    data = open(src, "rb").read()
    size = len(data)

    # Entry RVA from the ELF header (e_entry at offset 0x18, ELF32).
    entry_rva = 0x1000
    if opts.elf:
        e = open(opts.elf, "rb").read(52)
        if e[:4] != b"\x7fELF":
            sys.exit(f"{opts.elf}: not an ELF file")
        e_entry = struct.unpack_from("<I", e, 0x18)[0]
        entry_rva = e_entry - (base & ~0xFFF) - 0x1000 + 0x1000
        # base is the VA of offset 0 of the image; USER_LINK_BASE is that
        # page-aligned region start (base - 0x1000 when .text sits at
        # +0x1000).  Compute directly: RVA = e_entry - USER_LINK_BASE.
        # We pass link_base as the VA of image offset 0, and the loader's
        # LINK_BASE is (link_base & ~0xFFF) minus the .text RVA offset.
        # For our layout: .text at 0xC1001000 == image offset 0, so
        # USER_LINK_BASE = 0xC1000000 = (base - 0x1000).
        entry_rva = e_entry - (base - 0x1000)
    if not (0x1000 <= entry_rva < 0x1000 + size):
        sys.exit(f"entry RVA 0x{entry_rva:x} outside image (size {size})")

    # ── Layout ────────────────────────────────────────────────────
    dos_stub_size = 0x80
    pe_off = dos_stub_size                     # PE header right after stub
    coff_off = pe_off + 4
    opt_off = coff_off + 20
    opt_size = 224                             # PE32 optional header (96 fixed + 16*8 dirs)
    sec_table_off = opt_off + opt_size
    sect_al = align(size, 0x200)               # file-aligned raw section size
    size_headers = 0x400
    size_image = align(0x1000 + sect_al, 0x1000)
    user_link_base = base - 0x1000             # loader's LINK_BASE

    # ── DOS stub ──────────────────────────────────────────────────
    dos = bytearray(b"\x00" * dos_stub_size)
    dos[0:2] = b"MZ"
    struct.pack_into("<I", dos, 0x3C, pe_off)

    # ── PE signature + COFF ───────────────────────────────────────
    out = bytearray()
    out += dos
    out += b"PE\x00\x00"
    machine = 0x014C                     # i386
    nsections = 1                        # single .flat carrying the image
    out += struct.pack("<HHIIIHH", machine, nsections,
                       0,                # timestamp
                       0, 0,             # symtab, nsyms
                       opt_size,
                       0x0102)           # EXECUTABLE_IMAGE | 32BIT

    # ── Optional header (PE32), 96 bytes + 16 data dirs ───────────
    # <HBB + 9I + 6H + 4I + 2H + 6I — exactly the loader's packed struct.
    out += struct.pack(
        "<HBBIIIIIIIIIHHHHHHIIIIHHIIIIII",
        0x10B,                            # Magic: PE32
        3, 9,                             # linker maj/min (cosmetic)
        size,                             # SizeOfCode (cosmetic)
        0, 0,                             # SizeOfInitialized/Uninitialized
        entry_rva,                        # AddressOfEntryPoint
        0x1000,                           # BaseOfCode
        0x1000,                           # BaseOfData (PE32 only)
        user_link_base,                   # ImageBase (== where we load)
        0x1000,                           # SectionAlignment
        0x200,                            # FileAlignment
        4, 0,                             # OS ver maj/min
        0, 0,                             # Image ver maj/min
        4, 0,                             # Subsystem ver maj/min
        0,                                # Win32VersionValue
        size_image,                       # SizeOfImage
        size_headers,                     # SizeOfHeaders
        0,                                # CheckSum
        2,                                # Subsystem: Windows GUI
        0,                                # DllCharacteristics
        0x100000, 0x1000,                 # Stack reserve/commit
        0x100000, 0x1000,                 # Heap reserve/commit
        0,                                # LoaderFlags
        16)                               # NumberOfRvaAndSizes
    # Data directories (16): all empty — no imports, no relocations
    # (the image is linked at its final runtime base).
    for _ in range(16):
        out += struct.pack("<II", 0, 0)

    # ── Section header: one .flat, RWX, not discardable ───────────
    def sec_hdr(name, vaddr, vsize, raw_ptr, raw_size, flags):
        return struct.pack("<8sIIIIIIHHI",
                           name.encode().ljust(8, b"\x00")[:8],
                           vsize, vaddr, vsize, raw_ptr, raw_size,
                           0, 0, 0, flags)
    # code|execute|read|write|initialized-data (loader maps RW during
    # load and the game writes .data/.bss in place).
    flags_flat = 0xE0000020
    out += sec_hdr(".flat", 0x1000, sect_al, size_headers, sect_al, flags_flat)

    # Pad headers to size_headers
    assert len(out) <= size_headers, (len(out), size_headers)
    out += b"\x00" * (size_headers - len(out))
    # Section raw data (the flat image)
    out += data
    out += b"\x00" * (sect_al - len(data))

    open(dst, "wb").write(bytes(out))
    print(f"wrote {dst}: {len(out)} bytes (image {size}, "
          f"entry RVA 0x{entry_rva:x}, base 0x{user_link_base:x})")

if __name__ == "__main__":
    main()
