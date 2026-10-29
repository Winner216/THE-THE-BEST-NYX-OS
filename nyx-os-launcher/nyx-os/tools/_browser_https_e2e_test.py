#!/usr/bin/env python3
"""_browser_https_e2e_test.py — in-kernel TLS browsing in Nyx Browser.

Boots the ISO, launches the Browser, opens https://en.wikipedia.org/wiki/Nyx
(the classic "the browser can't do https" site) and verifies:
  TLS OK            serial marker from cpu/tls.c (handshake completed)
  BR HTTP OK 200    serial marker from browser.c br_fetch
  rendered page     screenshot of the htmlview panel
"""
import subprocess, time, os, sys

WORK = "/tmp/browserhttpstest"
os.makedirs(WORK, exist_ok=True)
serial_path = os.path.join(WORK, "serial.log")
open(serial_path, "w").close()

qemu = subprocess.Popen(
    ["qemu-system-i386", "-enable-kvm", "-cpu", "host",
     "-cdrom", "nyx.iso", "-m", "1G", "-boot", "d",
     "-nic", "user,model=e1000,id=net0",
     "-object", "filter-dump,id=f1,netdev=net0,file=/tmp/wiki.pcap",
     "-monitor", "stdio", "-serial", "file:" + serial_path,
     "-display", "none"],
    stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
    stderr=subprocess.DEVNULL)

def send(cmd, wait=0.4):
    qemu.stdin.write((cmd + "\n").encode())
    qemu.stdin.flush()
    time.sleep(wait)

def keys(seq, gap=0.2):
    QKEY = {".": "dot", "/": "slash", "-": "minus", " ": "spc", "=": "equal", ":": "shift-semicolon"}
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
        print("FAIL: guest never reached login")
        sys.exit(1)
    keys(["1", "2", "3", "4"], 0.2)
    send("sendkey ret", 1.0)
    if not wait_serial("DESKTOP UP", 30):
        print("FAIL: desktop never came up")
        sys.exit(1)
    print("    desktop up", flush=True)

    print("[2] launching Browser (F1, `browser`)...", flush=True)
    send("sendkey f1", 1.0)
    keys(list("browser"), 0.18)
    send("sendkey ret", 1.5)
    time.sleep(2)
    send("screendump " + WORK + "/s1_launch.ppm", 0.5)

    print("[3] typing en.wikipedia.org/wiki/nyx...", flush=True)
    # Slow typing: under the WORK/BLINK demo-task serial spam the
    # keyboard ISR drops scancodes at fast sendkey rates.  0.5s gaps
    # keep every character.
    keys(list("en.wikipedia.org/wiki/nyx"), 0.5)
    time.sleep(1)
    send("screendump " + WORK + "/s2_url.ppm", 0.5)

    print("[4] ENTER — DNS + TLS handshake + HTTP + render...", flush=True)
    send("sendkey ret", 1.0)
    tls_ok = wait_serial("TLS OK", 120)
    print(("PASS: in-kernel TLS handshake completed" if tls_ok else
           "FAIL: no TLS OK marker"), flush=True)
    ok = wait_serial("BR HTTP OK 200", 120)
    print(("PASS: HTTP 200 via Browser" if ok else
           "FAIL: no BR HTTP OK marker"), flush=True)
    time.sleep(3)
    send("screendump " + WORK + "/s_page.ppm", 0.5)
    time.sleep(1)

    log = ""
    try:
        with open(serial_path, "r", errors="ignore") as f:
            log = f.read()
    except Exception:
        pass
    for line in log.splitlines():
        if "BR HTTP OK" in line or "TLS OK" in line:
            print("  " + line.strip(), flush=True)

    result = "PASS" if (tls_ok and ok) else "FAIL"

finally:
    qemu.stdin.write(b"quit\n"); qemu.stdin.flush()
    try: qemu.wait(timeout=5)
    except: qemu.kill()

print("RESULT:", result, flush=True)
sys.exit(0 if result == "PASS" else 1)
