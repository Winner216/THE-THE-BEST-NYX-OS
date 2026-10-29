#!/usr/bin/env python3
"""_browser_tour.py — visual tour of the Nyx Browser in QEMU.

Boots nyx.iso, logs in, launches the Browser, opens example.com, follows
its link, then opens a host-served demo page and follows a link there.
Captures PPM screenshots at each step into /tmp/browsertour/.

Run from the nyx-os directory:  python3 tools/_browser_tour.py
"""
import subprocess, time, os, sys, threading, socket

PORT = 18091
WORK = "/tmp/browsertour"
os.makedirs(WORK, exist_ok=True)
serial_path = os.path.join(WORK, "serial.log")
open(serial_path, "w").close()

# ── Tiny HTTP server on the host (guest reaches it at 10.0.2.2:PORT) ──
INDEX = ("<html><head><title>Link Test</title></head><body>"
         "<h1>Link Test Home</h1>"
         "<p>This page is served by a tiny HTTP server on the host, "
         "reached through QEMU user-mode NAT.</p>"
         "<a href=\"/wiki/Page\">root-rel</a><br>"
         "<a href=\"sub/page.html\">rel</a><br>"
         "</body></html>")

def handle(conn):
    try:
        conn.settimeout(5)
        data = conn.recv(4096)
        req = data.split(b"\r\n", 1)[0].decode("latin1", "replace")
        parts = req.split(" ")
        path = parts[1] if len(parts) > 1 else "/"
        if path.startswith("/wiki/Page"):
            body = "<html><body><h1>WIKI PAGE OK</h1><p>You followed the root-rel link.</p></body></html>"
        elif path.startswith("/sub/page.html"):
            body = "<html><body><h1>SUB PAGE OK</h1><p>You followed the rel link.</p></body></html>"
        elif path == "/":
            body = INDEX
        else:
            body = "<html><body>NOT FOUND</body></html>"
        resp = ("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
                "Content-Length: %d\r\nConnection: close\r\n\r\n%s"
                % (len(body), body)).encode()
        conn.sendall(resp)
    except Exception:
        pass
    finally:
        conn.close()

def serve():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", PORT))
    s.listen(8)
    while True:
        conn, _ = s.accept()
        threading.Thread(target=handle, args=(conn,), daemon=True).start()

threading.Thread(target=serve, daemon=True).start()
print("host server on port %d" % PORT, flush=True)

# ── QEMU ─────────────────────────────────────────────────────────────
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
    QKEY = {":": "shift-semicolon", ".": "dot", "/": "slash",
            " ": "spc", "?": "shift-slash", "(": "shift-9"}
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

def shot(name):
    send("screendump " + os.path.join(WORK, name), 0.7)
    print("    shot: " + name, flush=True)

try:
    print("[1] boot + login...", flush=True)
    if not wait_serial("LOGIN READY", 90):
        print("FAIL: guest never reached login"); sys.exit(1)
    keys(["1", "2", "3", "4"], 0.2)
    send("sendkey ret", 1.0)
    if not wait_serial("DESKTOP UP", 30):
        print("FAIL: desktop never came up"); sys.exit(1)
    print("    desktop up", flush=True)

    print("[2] launch Browser (F1 -> `browser`)...", flush=True)
    send("sendkey f1", 1.0)
    keys(list("browser"), 0.18)
    send("sendkey ret", 1.5)
    time.sleep(2)
    shot("s1_welcome.ppm")

    print("[3] open example.com...", flush=True)
    keys(list("example.com"), 0.18)
    time.sleep(1)
    shot("s2_url_typed.ppm")
    send("sendkey ret", 1.0)
    ok = wait_serial("BR HTTP OK 200 example.com", 90)
    print("    example.com loaded: %s" % ok, flush=True)
    time.sleep(2)
    shot("s3_example.ppm")

    print("[4] follow the page link (TAB, ENTER)...", flush=True)
    send("sendkey tab", 0.6)   # focus content / cycle to link
    send("sendkey ret", 1.0)   # follow (example.com -> https://www.iana.org/...)
    time.sleep(12)
    shot("s4_followed.ppm")

    print("[5] open host demo page (ESC, type URL)...", flush=True)
    send("sendkey esc", 0.6)   # back to address bar
    keys(list("10.0.2.2:%d/" % PORT), 0.18)
    send("sendkey ret", 1.0)
    ok2 = wait_serial("BR HTTP OK 200 10.0.2.2:%d/" % PORT, 60)
    print("    demo page loaded: %s" % ok2, flush=True)
    time.sleep(2)
    shot("s5_hostpage.ppm")

    print("[6] follow the root-rel link...", flush=True)
    send("sendkey tab", 0.6)
    send("sendkey ret", 1.0)
    ok3 = wait_serial("/wiki/Page", 60)
    print("    link followed: %s" % ok3, flush=True)
    time.sleep(2)
    shot("s6_followed2.ppm")

    print("[7] back (Z)...", flush=True)
    send("sendkey z", 1.0)
    ok4 = wait_serial("BR HTTP OK 200 10.0.2.2:%d/" % PORT, 60)
    print("    back: %s" % ok4, flush=True)
    time.sleep(2)
    shot("s7_back.ppm")

    print("===== serial markers =====", flush=True)
    with open(serial_path, "r", errors="ignore") as f:
        for line in f.read().splitlines():
            if "BR HTTP OK" in line or "NIC UP" in line or "LOGIN" in line:
                print("  " + line.strip(), flush=True)

finally:
    qemu.stdin.write(b"quit\n"); qemu.stdin.flush()
    try: qemu.wait(timeout=5)
    except Exception: qemu.kill()

print("DONE — screenshots in %s" % WORK, flush=True)
