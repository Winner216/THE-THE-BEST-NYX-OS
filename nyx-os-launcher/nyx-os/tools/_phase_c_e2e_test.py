#!/usr/bin/env python3
"""_phase_c_e2e_test.py — Phase C: the full network stack, in order.

Exercises every layer the user asked for, top to bottom:
  1. ARP      — http init triggers ARP exchange with the slirp gateway
  2. ICMP     — net ping 10.0.2.2 (QEMU gateway replies)
  3. UDP/DNS  — net ping example.com (UDP query to 10.0.2.3:53)
  4. TCP      — http get example.com / (3-way handshake to :80)
  5. HTML     — response body printed to the terminal + serial log

Each layer is proven by its own serial marker:
  NIC UP          ARP/gateway stage completed
  PING REPLY OK   ICMP echo reply from 10.0.2.2
  PING DNS OK     UDP DNS query resolved example.com
  HTTP OK 200     TCP handshake + HTTP GET succeeded
  HTTP BODY: ...  raw HTML printed to the console
"""
import subprocess, time, os, sys

WORK = "/tmp/phasec"
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
    QKEY = {" ": "spc", ".": "dot", "/": "slash", "-": "minus"}
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

def read_log():
    try:
        with open(serial_path, "r", errors="ignore") as f:
            return f.read()
    except Exception:
        return ""

checks = []
def check(name, ok):
    checks.append((name, ok))
    print(("PASS: " if ok else "FAIL: ") + name, flush=True)

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

    print("[2] opening Terminal (F1)...", flush=True)
    send("sendkey f1", 1.0)

    # ── Stage 1: ARP (http init resolves the gateway MAC) ────────
    print("[3] http init (ARP)...", flush=True)
    keys(list("http init"), 0.18)
    send("sendkey ret", 1.5)
    ok = wait_serial("NIC UP", 15)
    check("ARP: NIC up, gateway MAC resolved", ok)
    if not ok:
        sys.exit(1)

    # ── Stage 2: ICMP ping 10.0.2.2 ─────────────────────────────
    print("[4] net ping 10.0.2.2 (ICMP)...", flush=True)
    keys(list("net ping 10.0.2.2"), 0.15)
    send("sendkey ret", 1.0)
    ok = wait_serial("PING REPLY OK", 30)
    check("ICMP: echo reply from 10.0.2.2", ok)
    if not ok:
        sys.exit(1)

    # ── Stage 3: UDP/DNS example.com ─────────────────────────────
    # (slirp does NOT forward ICMP to external hosts, so the ping
    # itself times out — the DNS layer is what we're proving here.
    # Wait for the DNS marker AND for the ping loop to finish so the
    # next command's keystrokes are consumed cleanly.)
    print("[5] net ping example.com (UDP DNS)...", flush=True)
    keys(list("net ping example.com"), 0.15)
    send("sendkey ret", 1.0)
    ok_dns = wait_serial("PING DNS OK", 30)
    check("UDP/DNS: example.com resolved via 10.0.2.3", ok_dns)
    if not ok_dns:
        sys.exit(1)
    # 3 pings × ~1.5 s timeout each; wait for the loop to drain.
    time.sleep(8)

    # ── Stage 4+5: TCP handshake + HTTP GET + HTML output ───────
    print("[6] http get example.com / (TCP + HTML)...", flush=True)
    keys(list("http get example.com /"), 0.15)
    send("sendkey ret", 1.0)
    ok = wait_serial("HTTP OK 200", 90)
    check("TCP: 3-way handshake + HTTP 200", ok)
    log = read_log()
    if "HTTP BODY:" in log and ("<html" in log.lower()):
        check("HTML: response body printed to console", True)
    else:
        check("HTML: response body printed to console", False)
    if not ok:
        sys.exit(1)

    # ── Final report ─────────────────────────────────────────────
    print("\n=== Phase C serial markers (in order) ===", flush=True)
    for line in log.splitlines():
        if any(m in line for m in ("NIC UP", "PING", "HTTP", "RAW")):
            print("  " + line.strip(), flush=True)
    print("====================================\n", flush=True)

    failed = [n for n, ok in checks if not ok]
    result = "PASS" if not failed else "FAIL"
    print("RESULT: " + result, flush=True)

finally:
    qemu.stdin.write(b"quit\n"); qemu.stdin.flush()
    try: qemu.wait(timeout=5)
    except: qemu.kill()

sys.exit(0 if result == "PASS" else 1)