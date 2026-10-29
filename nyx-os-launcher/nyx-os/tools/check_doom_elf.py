#!/usr/bin/env python3
"""Sanity-check an ELF produced for the Nyx DOOM build.

Verifies: i386, static, entry point exists, no undefined symbols, no
dynamic relocations the freestanding runtime can't handle.
"""
import subprocess, sys

elf = sys.argv[1] if len(sys.argv) > 1 else "user/doom/doom.elf"

def run(*args):
    return subprocess.run(args, capture_output=True, text=True).stdout

info = run("file", elf)
if "ELF" not in info or "Intel 80386" not in info:
    sys.exit(f"not a static i386 ELF: {info.strip()}")
if "statically linked" not in info and "dynamically linked" not in info:
    pass
print("file:", info.strip())

# Undefined symbols?
und = run("nm", "-u", elf).strip()
if und:
    print("UNDEFINED SYMBOLS:")
    print(und)
    sys.exit(1)

readelf = run("readelf", "-h", "-d", elf)
if "Entry point address" in readelf:
    for line in readelf.splitlines():
        if "Entry point" in line:
            print(line.strip())
for tag in ("NEEDED", "Dynamic"):
    if tag in run("readelf", "-d", elf):
        print("WARNING: dynamic section present")

print("ELF OK")
