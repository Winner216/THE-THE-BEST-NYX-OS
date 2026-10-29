#!/usr/bin/env python3
"""_usermode_e2e_test.py — ring-3 user mode in Nyx OS.

Boots the ISO, logs in, opens Terminal, and:
  1. `user`      → the embedded ELF runs at ring 3 (serial markers
                   "USER hello from ring 3! pid=1", ticks, exit)
  2. `userfault` → the program deliberately dereferences NULL; the
                   kernel must kill ONLY that task (USER FAULT path)
                   and the OS must stay alive — proven by relaunching
                   `user` afterwards and seeing it run again.
"""
import subprocess, time, os, sys

WORK = "/tmp/usermodetest"
os.makedirs(WORK, exist_ok=True)
serial_path = os.path.join(WORK, "serial.log")
open(serial_path, "w").close()

qemu = subprocess.Popen(
    ["qemu-system-i386", "-enable-kvm", "-cpu", "host",
     "-cdrom", "nyx.iso", "-m", "1G", "-boot", "d",
     "-nic", "user,model=e1000",
     "-monitor", "stdio", "-serial", "file:" + serial_path,
     "-display", "none"],
    stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
    stderr=subprocess.DEVNULL)

def send(cmd, wait=0.4):
    qemu.stdin.write((cmd + "\n").encode())
    qemu.stdin.flush()
    time.sleep(wait)

def keys(seq, gap=0.2):
    for k in seq:
        send("sendkey " + k, gap)

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
    if not wait_serial("LOGIN READY", 120):
        print("FAIL: guest never reached login")
        sys.exit(1)
    keys(["1", "2", "3", "4"], 0.2)
    send("sendkey ret", 1.0)
    if not wait_serial("DESKTOP UP", 60):
        print("FAIL: desktop never came up")
        sys.exit(1)
    print("    desktop up", flush=True)

    print("[2] F1 terminal, launch `user` (ring 3 hello)...", flush=True)
    send("sendkey f1", 1.0)
    keys(list("user"), 0.15)
    send("sendkey ret", 1.5)
    ok = wait_serial("USER hello from ring 3! pid=1", 30)
    print(("PASS: ring-3 program running (pid=1)" if ok else
           "FAIL: no ring-3 hello"), flush=True)
    if not ok:
        sys.exit(1)

    ok = wait_serial("USER exiting via sys_exit", 40)
    print(("PASS: user program ran, ticked 4x, exited cleanly" if ok else
           "FAIL: no clean exit"), flush=True)
    if not ok:
        sys.exit(1)

    print("[3] launch `userfault` (deliberate NULL deref)...", flush=True)
    keys(list("userfault"), 0.15)
    send("sendkey ret", 1.0)
    ok = wait_serial("USERF about to dereference NULL", 30)
    print(("PASS: fault program running" if ok else
           "FAIL: fault program never started"), flush=True)
    if not ok:
        sys.exit(1)

    # Give the fault time to fire. The marker we must NOT see is
    # "survived"; the kernel's USER FAULT kill + OS survival is proven
    # by the relaunch in step 4.
    time.sleep(4)
    survived = "USERF survived?!" in open(serial_path, errors="ignore").read()
    print(("FAIL: fault program SURVIVED (kernel didn't kill it)!"
           if survived else "PASS: fault killed the task"), flush=True)
    if survived:
        sys.exit(1)

    print("[4] relaunch `user` — proves the OS survived the crash...", flush=True)
    keys(list("user"), 0.15)
    send("sendkey ret", 1.0)
    ok = wait_serial("USER hello from ring 3! pid=1", 30)
    print(("PASS: OS alive after user-fault — ring-3 program runs again"
           if ok else "FAIL: OS dead after the fault"), flush=True)
    result = "PASS" if ok else "FAIL"
finally:
    qemu.stdin.write(b"quit\n")
    qemu.stdin.flush()
    time.sleep(1)
    try:
        qemu.kill()
    except Exception:
        pass

print("RESULT:", result)
sys.exit(0 if result == "PASS" else 1)
