#!/usr/bin/env python3
"""_files_f2_rename_e2e_test.py — physical F2 starts inline rename in Files.

Regression test: the global F1-F8 quick-launch block in keyboard.c used to
intercept F2 (scancode 0x3C) and re-launch the Files app, so the Files
window's advertised "F2 rename" never fired.  Flow:

  login -> F2 (launches Files app) -> B x5 (This PC -> Local Disk tile)
  -> ENTER (drive root rows) -> F2 on selected row (must enter rename mode,
  serial "RNRENAMEMODE") -> backspace-clear + type "zz.txt" -> ENTER
  (commit, serial "RNRENAME zz.txt").
"""
import os
import subprocess
import sys
import time

os.chdir(os.path.dirname(os.path.abspath(__file__)) + "/..")

WORK = "/tmp/f2rename"
os.makedirs(WORK, exist_ok=True)
SERIAL = os.path.join(WORK, "serial.log")
open(SERIAL, "w").close()

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


def keys(seq, gap=0.16):
    for k in seq:
        send("sendkey " + k, gap)


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


ok = False
try:
    if not wait_serial("LOGIN READY", 90):
        print("FAIL: no login screen"); sys.exit(1)
    print("[1] login screen up")
    keys(["1", "2", "3", "4"], 0.2)
    send("sendkey ret", 1.0)
    if not wait_serial("DESKTOP UP", 30):
        print("FAIL: desktop never came up"); sys.exit(1)
    print("[2] desktop up")

    # F2 from the desktop = quick-launch Files app.
    send("sendkey f2", 1.2)
    time.sleep(0.6)
    # This PC home: B x5 moves the highlight to the Local Disk tile,
    # ENTER opens the drive root listing (rows of real files).
    for _ in range(5):
        send("sendkey down", 0.22)
    send("sendkey ret", 1.0)
    time.sleep(0.5)

    # F2 on the selected row → rename mode must engage.
    send("sendkey f2", 0.8)
    if not wait_serial("RNRENAMEMODE", 15):
        print("FAIL: F2 did not enter rename mode")
        # dump tail for diagnosis
        with open(SERIAL, errors="ignore") as f:
            print(f.read()[-1500:])
        sys.exit(1)
    print("[3] F2 entered inline rename (RNRENAMEMODE)")

    # Clear the prefilled name with Backspace, type a fresh short
    # name, ENTER commits.
    for _ in range(60):
        send("sendkey backspace", 0.08)
    send("sendkey z", 0.12)
    send("sendkey z", 0.12)
    send("sendkey dot", 0.12)
    send("sendkey t", 0.12)
    send("sendkey x", 0.12)
    send("sendkey t", 0.12)
    send("sendkey ret", 1.0)
    if not wait_serial("RNRENAME ", 15):
        print("FAIL: rename never committed")
        sys.exit(1)
    with open(SERIAL, errors="ignore") as f:
        log = f.read()
    ok_line = [l for l in log.splitlines() if l.startswith("RNRENAME ")]
    if not ok_line:
        print("FAIL: no RNRENAME line"); sys.exit(1)
    print("[4] committed:", ok_line[-1])
    if ok_line[-1].strip() != "RNRENAME zz.txt":
        print("FAIL: expected RNRENAME zz.txt"); sys.exit(1)
    print("PASS: F2 rename works — name committed as 'zz.txt'")
    ok = True
finally:
    qemu.stdin.write("quit\n".encode()); qemu.stdin.flush()
    try:
        qemu.wait(timeout=5)
    except Exception:
        qemu.kill()
sys.exit(0 if ok else 1)
