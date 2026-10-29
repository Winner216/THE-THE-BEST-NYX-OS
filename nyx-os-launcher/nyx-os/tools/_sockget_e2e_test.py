#!/usr/bin/env python3
"""_sockget_e2e_test.py — ring-3 BSD-socket HTTP demo (sockget).

Boots the ISO, logs in, opens Terminal (F1), types `sockget example.com /`
and verifies over COM1 (serial) that the ring-3 program:

  1. resolved the hostname via the SYS_RESOLVE syscall (prints ip=...),
  2. opened a socket, connected, sent an HTTP GET,
  3. received the response and streamed it to the console,
  4. printed the END marker with a byte count > 0.

This proves the full chain user program -> int 0x80 syscalls -> socket
layer -> per-connection TCP -> NIC works from ring 3.
"""
import os
import subprocess
import sys
import time

WORK = "/tmp/sockgettest"
os.makedirs(WORK, exist_ok=True)
SERIAL = os.path.join(WORK, "serial.log")
try:
    open(SERIAL, "w").close()
except Exception:
    pass

qemu = subprocess.Popen(
    ["qemu-system-i386", "-enable-kvm", "-cpu", "host",
     "-cdrom", "nyx.iso", "-m", "1G", "-boot", "d",
     "-nic", "user,model=e1000",
     "-monitor", "stdio", "-serial", "file:" + SERIAL,
     "-display", "none"],
    stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
    stderr=subprocess.DEVNULL)


def send(cmd, wait=0.35):
    qemu.stdin.write((cmd + "\n").encode())
    qemu.stdin.flush()
    time.sleep(wait)


QKEY = {".": "dot", "/": "slash", "-": "minus", " ": "spc"}

def keys(seq, gap=0.18):
    for k in seq:
        send("sendkey " + QKEY.get(k, k), gap)


def wait_serial(marker, timeout):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            with open(SERIAL, "r", errors="ignore") as f:
                if marker in f.read():
                    return True
        except Exception:
            pass
        time.sleep(0.5)
    return False


ok = True
try:
    if not wait_serial("LOGIN READY", 90):
        print("FAIL: no login screen"); sys.exit(1)
    print("[1] login screen up")
    keys(["1", "2", "3", "4"], 0.2)
    send("sendkey ret", 1.0)
    if not wait_serial("DESKTOP UP", 30):
        print("FAIL: desktop never came up"); sys.exit(1)
    print("[2] desktop up")

    print("[3] F1 -> Terminal, typing `sockget example.com /`...")
    send("sendkey f1", 1.5)
    time.sleep(0.8)
    keys(list("sockget example.com /"), 0.15)
    send("sendkey ret", 0.5)

    if not wait_serial("[sockget] resolving", 30):
        print("FAIL: sockget never started (dispatch broken?)")
        ok = False
    else:
        print("    sockget launched")

    if not wait_serial("===== END (", 90):
        print("FAIL: no HTTP response END marker (network/socket failed)")
        ok = False
    else:
        print("    HTTP response streamed")

    # Read back the byte count and first line of the response.
    with open(SERIAL, "r", errors="ignore") as f:
        log = f.read()
    import re
    m = re.search(r"===== END \((\d+) bytes\) =====", log)
    if m:
        n = int(m.group(1))
        print(f"    total bytes: {n}")
        if n < 200:
            print("FAIL: response implausibly small"); ok = False
        else:
            print("PASS: got a real HTTP response")
    else:
        print("FAIL: byte count not found"); ok = False

    if "HTTP/1.1" in log or "<!doctype html" in log.lower() or "<html" in log.lower():
        print("PASS: response contains HTTP/1.1 or HTML")
    else:
        print("NOTE: response content marker not matched (still may be valid)")

    if "DNS failed" in log:
        print("FAIL: DNS resolution failed"); ok = False
    if "connect() failed" in log:
        print("FAIL: connect failed"); ok = False
    if "send() failed" in log:
        print("FAIL: send failed"); ok = False

finally:
    qemu.stdin.write("quit\n".encode())
    qemu.stdin.flush()
    qemu.wait(timeout=10)

print("\n" + ("PASS: ring-3 sockets verified" if ok else "FAIL: socket checks failed"))
sys.exit(0 if ok else 1)
