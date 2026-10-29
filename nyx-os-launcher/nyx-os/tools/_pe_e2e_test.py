#!/usr/bin/env python3
"""_pe_e2e_test.py — Test the PE (.exe) loader in QEMU.

Boots the ISO, logs in, opens Terminal (F1), types 'pe test.exe',
and captures serial output showing PE loader diagnostics.
"""
import subprocess, time, os, sys

WORK = "/tmp/pe_e2e"
os.makedirs(WORK, exist_ok=True)
serial_path = os.path.join(WORK, "serial.log")
open(serial_path, "w").close()

qemu = subprocess.Popen(
    ["qemu-system-i386", "-enable-kvm", "-cpu", "host",
     "-cdrom", "nyx.iso", "-m", "1G", "-boot", "d",
     "-monitor", "stdio", "-serial", "file:" + serial_path,
     "-display", "none"],
    stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
    stderr=subprocess.DEVNULL)

def send(cmd, wait=0.4):
    qemu.stdin.write((cmd + "\n").encode())
    qemu.stdin.flush()
    time.sleep(wait)

def keys(seq, gap=0.15):
    QKEY = {".": "dot", "/": "slash", " ": "spc"}
    for k in seq:
        send("sendkey " + QKEY.get(k, k), gap)

def wait_serial(marker, timeout):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            with open(serial_path, "r", errors="ignore") as f:
                if marker in f.read():
                    return True
        except Exception:
            pass
        time.sleep(0.5)
    return False

result = "FAIL"
try:
    print("[1] waiting for LOGIN READY...", flush=True)
    if not wait_serial("LOGIN READY", 90):
        print("  LOGIN READY not found in 90s")
        sys.exit(1)

    print("[2] typing password 1234...", flush=True)
    for k in ["1", "2", "3", "4"]:
        send(f"sendkey {k}", 0.3)
    send("sendkey ret", 3.0)

    print("[3] waiting for DESKTOP UP...", flush=True)
    if not wait_serial("DESKTOP UP", 30):
        print("  DESKTOP UP not found")
        sys.exit(1)
    time.sleep(2)

    print("[4] opening Terminal (F1)...", flush=True)
    send("sendkey f1", 3.0)

    print("[5] typing 'pe test.exe'...", flush=True)
    keys("pe test.exe", gap=0.1)
    time.sleep(0.5)
    send("sendkey ret", 5.0)

    print("[6] checking serial for PE loader output...", flush=True)
    time.sleep(3)

    with open(serial_path, "r", errors="ignore") as f:
        log = f.read()

    # Look for PE loader markers
    pe_lines = [l for l in log.split("\n") if "PE:" in l]
    if pe_lines:
        print("  PE loader output found:")
        for l in pe_lines[:15]:
            print(f"    {l}")
        if any("loaded successfully" in l for l in pe_lines):
            result = "PASS"
            print("  >>> PE LOADER SUCCESS!")
        elif any("header invalid" in l or "failed" in l.lower() for l in pe_lines):
            result = "PARTIAL"
            print("  >>> PE header parsed but load failed")
        else:
            result = "PARTIAL"
            print("  >>> PE loader ran but unclear result")
    else:
        # Check if command was even received
        if "pe test" in log:
            print("  'pe test' command was received")
        else:
            print("  No PE output and no command trace")
        # Print last few lines for debugging
        lines = [l for l in log.split("\n") if l.strip()]
        for l in lines[-10:]:
            print(f"    {l}")

    send("screendump /tmp/pe_e2e/s_final.ppm", 1.0)

finally:
    send("quit", 1.0)
    try:
        qemu.wait(timeout=5)
    except Exception:
        qemu.kill()

print(f"\nRESULT: {result}")
sys.exit(0 if result == "PASS" else 1)
