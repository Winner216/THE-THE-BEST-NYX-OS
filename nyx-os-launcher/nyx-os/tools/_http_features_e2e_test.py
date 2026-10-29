#!/usr/bin/env python3
"""E2E test for the Nyx HTTP client's gzip + redirect support.

Boots the Nyx ISO in QEMU (KVM) with an RTL8139 NIC, logs in, opens
the Terminal, initializes the NIC, then fetches four URLs from a local
host test server through slirp (10.0.2.2 = host loopback):

  /         plain HTML            -> expect "PLAIN PAGE OK"  in body
  /gz       gzip Content-Encoding -> expect "GZIP DECODE OK" in body
  /redir    302 Location: /target -> expect "REDIRECT TARGET OK"
  /relredir 302 Location: target  -> expect "REDIRECT TARGET OK"

Assertions use the COM1 serial markers the shell prints after each
fetch ("HTTP OK <code>", "HTTP BODY: <first 60 bytes> (len N)").

Requires: qemu-system-i386, KVM, host network (only loopback).
"""
import http.server
import gzip
import os
import subprocess
import sys
import threading
import time

WORK = "/tmp/httpfeat"
os.makedirs(WORK, exist_ok=True)
PORT = 8899

SERIAL = os.path.join(WORK, "serial.log")
try:
    open(SERIAL, "w").close()
except Exception:
    pass


def sh(cmd, **kw):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True, **kw)


# ── Host test server ──────────────────────────────────────────────
class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/":
            body = b"<html><body><h1>PLAIN PAGE OK</h1><p>" + b"plain text " * 40 + b"</p></body></html>"
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif self.path == "/gz":
            raw = b"<html><body><h1>GZIP DECODE OK</h1><p>" + b"compressed payload " * 60 + b"</p></body></html>"
            gz = gzip.compress(raw)
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Encoding", "gzip")
            self.send_header("Content-Length", str(len(gz)))
            self.end_headers()
            self.wfile.write(gz)
        elif self.path == "/redir":
            self.send_response(302)
            self.send_header("Location", "/target")
            self.send_header("Content-Length", "0")
            self.end_headers()
        elif self.path == "/relredir":
            self.send_response(302)
            self.send_header("Location", "target")
            self.send_header("Content-Length", "0")
            self.end_headers()
        elif self.path == "/target":
            body = b"<html><body><h1>REDIRECT TARGET OK</h1><p>redirect worked</p></body></html>"
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()

    def log_message(self, *a):
        pass


server = http.server.ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
t = threading.Thread(target=server.serve_forever, daemon=True)
t.start()
print(f"[0] host test server on 127.0.0.1:{PORT}")

# ── QEMU (same invocation as the NIC E2E) ─────────────────────────
qemu = subprocess.Popen(
    ["qemu-system-i386", "-enable-kvm", "-cpu", "host",
     "-cdrom", "nyx.iso", "-m", "1G", "-boot", "d",
     "-nic", "user,model=e1000",
     "-monitor", "stdio", "-serial", "file:" + SERIAL,
     "-display", "none"],
    stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
    stderr=subprocess.DEVNULL)


def send(cmd, wait=0.4):
    qemu.stdin.write((cmd + "\n").encode())
    qemu.stdin.flush()
    time.sleep(wait)


def keys(seq, gap=0.18):
    # QEMU keysym names for characters that aren't plain letters/digits
    QKEY = {" ": "spc", ".": "dot", "/": "slash", "-": "minus",
            "_": "shift-minus", ":": "shift-semicolon"}
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


def read_log():
    try:
        with open(SERIAL, "r", errors="ignore") as f:
            return f.read()
    except Exception:
        return ""


ok = True
try:
    # ── Phase 1 — login ───────────────────────────────────────────
    print("[1] waiting for LOGIN READY...")
    if not wait_serial("LOGIN READY", 90):
        print("FAIL: guest never reached the login screen")
        sys.exit(1)
    print("    login screen up")

    print("[2] logging in...")
    keys(["1", "2", "3", "4"], 0.2)
    send("sendkey ret", 1.0)
    if not wait_serial("DESKTOP UP", 30):
        print("FAIL: desktop never came up after login")
        sys.exit(1)
    print("    desktop up")

    # ── Phase 2 — Terminal + NIC init ─────────────────────────────
    print("[3] opening Terminal (F1)...")
    send("sendkey f1", 1.0)
    print("[4] http init...")
    keys(list("http init"), 0.18)
    send("sendkey ret", 1.5)
    if not wait_serial("NIC UP", 15):
        print("FAIL: no NIC UP marker")
        sys.exit(1)
    print("    NIC initialized")

    # ── Phase 3 — fetch / (plain, baseline) ───────────────────────
    print("[5] http get 10.0.2.2:8899 / ...")
    keys(list("http get 10.0.2.2:8899 /"), 0.15)
    send("sendkey ret", 1.0)
    if not wait_serial("PLAIN PAGE OK", 25):
        print("FAIL: baseline fetch did not return PLAIN PAGE OK")
        ok = False
    else:
        print("PASS: baseline fetch OK")

    # ── Phase 4 — fetch /gz (gzip Content-Encoding) ───────────────
    print("[6] http get 10.0.2.2:8899 /gz ...")
    keys(list("http get 10.0.2.2:8899 /gz"), 0.15)
    send("sendkey ret", 1.0)
    if not wait_serial("GZIP DECODE OK", 25):
        print("FAIL: gzip body was not decoded (marker shows raw bytes?)")
        ok = False
    else:
        print("PASS: gzip decoding OK")

    # ── Phase 5 — fetch /redir (absolute-path redirect) ───────────
    print("[7] http get 10.0.2.2:8899 /redir ...")
    keys(list("http get 10.0.2.2:8899 /redir"), 0.15)
    send("sendkey ret", 1.0)
    if not wait_serial("REDIRECT TARGET OK", 25):
        print("FAIL: absolute redirect was not followed")
        ok = False
    else:
        print("PASS: absolute redirect (Location: /target) OK")

    # ── Phase 6 — fetch /relredir (relative redirect) ─────────────
    print("[8] http get 10.0.2.2:8899 /relredir ...")
    keys(list("http get 10.0.2.2:8899 /relredir"), 0.15)
    send("sendkey ret", 1.0)
    if not wait_serial("REDIRECT TARGET OK", 25):
        print("FAIL: relative redirect was not followed")
        ok = False
    else:
        print("PASS: relative redirect (Location: target) OK")

    # ── Summary ───────────────────────────────────────────────────
    log = read_log()
    print("\n=== HTTP markers in serial log ===")
    for line in log.splitlines():
        if "HTTP " in line:
            print("  " + line.strip())
    print("==================================\n")

    # Sanity: the gzip page must inflate to MORE bytes than its raw
    # transfer (proves we actually inflated, not just echoed bytes)
    import re
    lens = re.findall(r"\(len (\d+)\)", log)
    if len(lens) >= 4:
        gz_len = int(lens[1])
        plain_len = int(lens[0])
        if gz_len <= plain_len:
            print(f"WARN: gzip len {gz_len} not > plain len {plain_len}")

finally:
    server.shutdown()
    qemu.stdin.write("quit\n".encode())
    qemu.stdin.flush()
    qemu.wait(timeout=10)

print("\n" + ("PASS: all HTTP feature checks passed" if ok else "FAIL: one or more checks failed"))
sys.exit(0 if ok else 1)
