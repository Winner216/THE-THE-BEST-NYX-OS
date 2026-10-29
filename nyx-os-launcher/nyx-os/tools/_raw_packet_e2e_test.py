#!/usr/bin/env python3
"""_raw_packet_e2e_test.py — Phase B: craft a raw Ethernet frame,
transmit it from the Nyx guest, and capture it with QEMU's
filter-dump pcap.  Asserts the payload appears on the wire.

Sequence: login (1234) → F1 (Terminal) → `http init` → `rawwrite`.
QEMU writes net.pcap, which we parse to find the frame.
"""
import subprocess, time, os, sys, struct

WORK = "/tmp/rawe2e"
os.makedirs(WORK, exist_ok=True)

PCAP = os.path.join(WORK, "net.pcap")
serial_path = os.path.join(WORK, "serial.log")

# Truncate serial
open(serial_path, "w").close()
for f in (PCAP,):
    try: os.remove(f)
    except FileNotFoundError: pass

qemu = subprocess.Popen(
    ["qemu-system-i386", "-enable-kvm", "-cpu", "host",
     "-cdrom", "nyx.iso", "-m", "1G", "-boot", "d",
     "-netdev", "user,id=net0", "-device", "e1000,netdev=net0",
     "-object", "filter-dump,id=f1,netdev=net0,file=" + PCAP,
     "-monitor", "stdio", "-serial", "file:" + serial_path,
     "-display", "none"],
    stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
    stderr=subprocess.DEVNULL)

def send(cmd, wait=0.4):
    qemu.stdin.write((cmd + "\n").encode())
    qemu.stdin.flush()
    time.sleep(wait)

def keys(seq, gap=0.22):
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

result = "FAIL"
try:
    # ── Phase 1 — wait for login ────────────────────────────────
    print("[1] waiting for LOGIN READY...", flush=True)
    if not wait_serial("LOGIN READY", 90):
        print("FAIL: guest never reached the login screen")
        result = "FAIL"
        sys.exit(1)
    print("    login up", flush=True)

    # ── Phase 2 — log in ────────────────────────────────────────
    print("[2] logging in...", flush=True)
    keys(["1", "2", "3", "4"], 0.2)
    send("sendkey ret", 1.0)
    if not wait_serial("DESKTOP UP", 30):
        print("FAIL: desktop never came up after login")
        sys.exit(1)
    print("    desktop up", flush=True)

    # ── Phase 3 — open Terminal (F1) ────────────────────────────
    print("[3] opening Terminal (F1)...", flush=True)
    send("sendkey f1", 1.0)

    # ── Phase 4 — http init ─────────────────────────────────────
    print("[4] http init...", flush=True)
    keys(list("http init"), 0.18)
    send("sendkey ret", 1.5)
    if not wait_serial("NIC UP", 15):
        print("FAIL: NIC init failed")
        sys.exit(1)
    print("    NIC up", flush=True)

    # ── Phase 5 — rawwrite ─────────────────────────────────────
    print("[5] rawwrite...", flush=True)
    keys(list("rawwrite"), 0.18)
    send("sendkey ret", 1.5)
    if not wait_serial("RAW TX OK", 10):
        print("FAIL: RAW TX OK marker missing")
        sys.exit(1)
    print("    RAW TX OK", flush=True)
    time.sleep(1)  # let QEMU flush the pcap

    # ── Phase 6 — verify the pcap ──────────────────────────────
    print("[6] verifying pcap...", flush=True)
    if not os.path.exists(PCAP):
        print("FAIL: net.pcap not found at", PCAP)
        sys.exit(1)
    pcap_size = os.path.getsize(PCAP)
    print("    pcap size:", pcap_size, "bytes", flush=True)
    if pcap_size < 60:
        print("FAIL: pcap too small (no frame captured)")
        sys.exit(1)

    # Parse the pcap (global header 24 B, then each packet has
    # 16 B record header + frame data)
    with open(PCAP, "rb") as f:
        data = f.read()

    # Verify pcap magic
    magic = struct.unpack("<I", data[0:4])[0]
    if magic != 0xa1b2c3d4 and magic != 0xd4c3b2a1:
        print("FAIL: not a pcap file (magic 0x%08x)" % magic)
        sys.exit(1)

    # Walk records
    off = 24
    frame_no = 0
    found = False
    while off + 16 <= len(data):
        incl_len = struct.unpack("<I", data[off+8:off+12])[0]
        if off + 16 + incl_len > len(data):
            break
        frame = data[off+16 : off+16+incl_len]
        frame_no += 1
        # Check for our payload anywhere in the frame
        if b"NYX-RAW-PACKET-TEST-0001" in frame:
            found = True
            print("    Frame %d: %d bytes, payload found at offset %d" % (
                frame_no, len(frame), frame.find(b"NYX-RAW-PACKET-TEST-0001")), flush=True)
        off += 16 + incl_len

    if found:
        print("PASS: raw Ethernet frame captured and verified in pcap", flush=True)
        result = "PASS"
    else:
        print("FAIL: payload not found in any of %d frames in pcap" % frame_no, flush=True)
        # Dump first two frames for debugging
        off = 24
        for di in range(min(3, frame_no)):
            incl_len = struct.unpack("<I", data[off+8:off+12])[0]
            frame = data[off+16 : off+16+incl_len]
            print("    Frame %d: %d bytes, dst %02x:%02x:%02x:%02x:%02x:%02x type 0x%04x" % (
                di+1, len(frame),
                frame[0], frame[1], frame[2], frame[3], frame[4], frame[5],
                (frame[12] << 8) | frame[13]), flush=True)
            print("      payload hex:", frame[14:min(40, len(frame))].hex(), flush=True)
            off += 16 + incl_len
        sys.exit(1)

finally:
    qemu.stdin.write(b"quit\n"); qemu.stdin.flush()
    try: qemu.wait(timeout=5)
    except: qemu.kill()

print("\nRESULT:", result)
sys.exit(0 if result == "PASS" else 1)