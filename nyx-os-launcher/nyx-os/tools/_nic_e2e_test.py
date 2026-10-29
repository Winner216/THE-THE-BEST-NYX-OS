#!/usr/bin/env python3
"""_nic_e2e_test.py — boot the Nyx ISO in QEMU with an Intel e1000 NIC
(8086:100E, `-nic user,model=e1000`) and verify the IRQ-driven driver
end-to-end.

Sequence: login (1234) → F1 (Terminal) → `http init` → `net status` →
`http get example.com /`. The NIC driver (e1000 or rtl8139 fallback)
prints two COM1 markers:
  E1000 NIC UP irq=N irq-driven   (init + PCI IRQ-line enumeration)
  NET RX IRQ                      (first packet caught by the ISR)
The test asserts both markers appear and captures screenshots of the
terminal for eyeballing the HTTP 200.

Requires host internet (the guest's DNS + HTTP go through QEMU's
user-mode NAT, i.e. the host).
"""
import subprocess, time, os, sys

WORK = "/tmp/nictest"
os.makedirs(WORK, exist_ok=True)

def sh(cmd, **kw):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True, **kw)

# ── serial log: -serial file: APPENDS, so truncate first ──────────
serial_path = os.path.join(WORK, "serial.log")
try:
    open(serial_path, "w").close()
except Exception:
    pass

# ── QEMU with monitor stdin for key injection ─────────────────────
qemu = subprocess.Popen(
    ["qemu-system-i386", "-enable-kvm", "-cpu", "host",
     "-cdrom", "nyx.iso", "-m", "1G", "-boot", "d",
     "-nic", "user,model=e1000",
     "-monitor", "stdio", "-serial", "file:" + WORK + "/serial.log",
     "-display", "none"],
    stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
    stderr=subprocess.DEVNULL)

def send(cmd, wait=0.4):
    qemu.stdin.write((cmd + "\n").encode())
    qemu.stdin.flush()
    time.sleep(wait)

def keys(seq, gap=0.22):
    # QEMU keysym names for characters that aren't plain letters/digits
    QKEY = {" ": "spc", ".": "dot", "/": "slash", "-": "minus", "_": "shift-minus"}
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

try:
    # ── Phase 1 — wait for the login screen ────────────────────────
    print("[1] waiting for LOGIN READY...")
    if not wait_serial("LOGIN READY", 90):
        print("FAIL: guest never reached the login screen")
        sys.exit(1)
    print("    login screen up")
    send("screendump " + WORK + "/s1_login.ppm", 0.5)

    # ── Phase 2 — log in (password 1234) ───────────────────────────
    print("[2] logging in...")
    keys(["1", "2", "3", "4"], 0.2)
    send("sendkey ret", 1.0)
    if not wait_serial("DESKTOP UP", 30):
        print("FAIL: desktop never came up after login")
        sys.exit(1)
    print("    desktop up")

    # ── Phase 3 — open Terminal (F1) ───────────────────────────────
    print("[3] opening Terminal (F1)...")
    send("sendkey f1", 1.0)
    send("screendump " + WORK + "/s2_terminal.ppm", 0.5)

    # ── Phase 4 — http init: NIC init + IRQ handler install ────────
    print("[4] http init...")
    keys(list("http init"), 0.18)
    send("sendkey ret", 1.5)
    if not wait_serial("NIC UP", 15):
        print("FAIL: no NIC UP marker (driver init failed?)")
        sys.exit(1)
    print("    NIC initialized")

    # ── Phase 5 — net status: driver mode + counters on screen ─────
    print("[5] net status...")
    keys(list("net status"), 0.18)
    send("sendkey ret", 1.0)
    send("screendump " + WORK + "/s3_netstatus.ppm", 0.5)

    # ── Phase 6 — real HTTP GET over the NIC ───────────────────────
    print("[6] http get example.com / ...")
    keys(list("http get example.com /"), 0.15)
    send("sendkey ret", 1.0)
    # DNS + TCP handshake + HTTP body under KVM: give it up to 30 s
    time.sleep(30)
    send("screendump " + WORK + "/s4_http.ppm", 0.5)
    time.sleep(2)

    # ── Phase 6b — HTTP outcome via the serial marker ──────────────
    # The terminal prints "HTTP OK <code>" / "HTTP FAIL" / "HTTP DNS
    # FAIL" to COM1, so the outcome is readable from the log.
    http_ok = wait_serial("HTTP OK", 10)
    def read_log():
        try:
            with open(serial_path, "r", errors="ignore") as f:
                return f.read()
        except Exception:
            return ""
    log6 = read_log()
    http_fail = ("HTTP FAIL" in log6 or "HTTP DNS FAIL" in log6)
    if http_ok:
        print("PASS: HTTP request succeeded (marker in serial log)")
    elif http_fail:
        print("FAIL: HTTP request failed (marker in serial log)")
    else:
        print("WARN: no HTTP outcome marker yet")

    # ── Phase 7 — assert the IRQ-driven path via serial markers ────
    log = ""
    try:
        with open(serial_path, "r", errors="ignore") as f:
            log = f.read()
    except Exception:
        pass
    print("\n=== serial markers ===")
    for line in log.splitlines():
        if any(m in line for m in ("NIC UP", "NET RX IRQ", "LOGIN READY",
                                   "DESKTOP UP", "AC97")):
            print("  " + line.strip())
    print("======================\n")

    ok = True
    if "NIC UP" not in log:
        print("FAIL: NIC UP marker missing"); ok = False
    elif "irq=0" in log:
        print("FAIL: NIC has no IRQ line (fell back to polled mode)"); ok = False
    else:
        print("PASS: NIC initialized in IRQ-driven mode")
    if "NET RX IRQ" not in log:
        print("FAIL: no NET RX IRQ marker — packets never reached the ISR"); ok = False
    else:
        print("PASS: ISR caught packet arrivals")
    if http_ok:
        print("PASS: full HTTP fetch over the NIC")
    elif http_fail:
        print("FAIL: HTTP fetch did not succeed"); ok = False

    # Phase 8 — verify the HTTP response pixels on the screenshot
    # (dark text rows under the 'HTTP 200' area; just check the shot
    # exists for eyeballing — the serial markers are the hard proof)
    for shot in ("s2_terminal", "s3_netstatus", "s4_http"):
        p = os.path.join(WORK, shot + ".ppm")
        print(("  " if os.path.exists(p) else "  MISSING ") + shot + ".ppm")

    print("\nRESULT: " + ("PASS" if ok else "FAIL"))
    sys.exit(0 if ok else 1)
finally:
    qemu.stdin.write(b"quit\n"); qemu.stdin.flush()
    try:
        qemu.wait(timeout=5)
    except Exception:
        qemu.kill()
