#!/usr/bin/env python3
"""_browser_links_e2e_test.py — relative link resolution in the Nyx Browser.

Runs a tiny HTTP server on the host, boots the guest, opens the Browser
at http://10.0.2.2:PORT/ (QEMU user-mode NAT → host loopback), then
follows links:

  /            → page with <a href="/wiki/Page"> and <a href="sub/page.html">
  /wiki/Page   → body "WIKI PAGE OK"
  /sub/page.html → body "SUB PAGE OK"

The BR HTTP OK serial marker now includes the URL + first body bytes, so
the test asserts the EXACT page that loaded after each link follow:
  BR HTTP OK 200 10.0.2.2:PORT/wiki/Page | WIKI PAGE OK
"""
import subprocess, time, os, sys, threading, socket

PORT = 18081
WORK = "/tmp/browserlinks"
os.makedirs(WORK, exist_ok=True)
serial_path = os.path.join(WORK, "serial.log")
open(serial_path, "w").close()

# ── Tiny HTTP server ─────────────────────────────────────────────
INDEX = ("<html><head><title>Link Test</title></head><body>"
         "<h1>Link Test Home</h1>"
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
            body = "<html><body>WIKI PAGE OK</body></html>"
        elif path.startswith("/sub/page.html"):
            body = "<html><body>SUB PAGE OK</body></html>"
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

# ── QEMU ─────────────────────────────────────────────────────────
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

def keys(seq, gap=0.18):
    QKEY = {".": "dot", "/": "slash", ":": "shift-semicolon", "-": "minus"}
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
    print("[1] boot + login...", flush=True)
    if not wait_serial("LOGIN READY", 90):
        print("FAIL: no login screen"); sys.exit(1)
    keys(["1", "2", "3", "4"], 0.2)
    send("sendkey ret", 1.0)
    if not wait_serial("DESKTOP UP", 30):
        print("FAIL: no desktop"); sys.exit(1)

    print("[2] launch Browser...", flush=True)
    send("sendkey f1", 1.0)
    keys(list("browser"), 0.18)
    send("sendkey ret", 1.5)
    time.sleep(2)

    print("[3] navigate to 10.0.2.2:%d/ ..." % PORT, flush=True)
    keys(list("10.0.2.2:%d/" % PORT), 0.18)
    send("sendkey ret", 1.0)
    ok = wait_serial("BR HTTP OK 200 10.0.2.2:%d/ | <html><head><title>Link Test" % PORT, 90)
    print(("PASS: index page loaded" if ok else "FAIL: index page"), flush=True)
    if not ok: sys.exit(1)

    print("[4] TAB + ENTER to follow /wiki/Page...", flush=True)
    send("sendkey tab", 1.0)
    send("sendkey ret", 1.0)
    ok = wait_serial("BR HTTP OK 200 10.0.2.2:%d/wiki/Page " % PORT, 90) and \
         wait_serial("WIKI PAGE OK", 5)
    print(("PASS: root-relative /wiki/Page resolved to host" if ok else
           "FAIL: /wiki/Page link"), flush=True)
    if not ok: sys.exit(1)

    print("[5] Z (back) to /, follow relative sub/page.html...", flush=True)
    send("sendkey z", 1.0)       # browser back → index
    if not wait_serial("BR HTTP OK 200 10.0.2.2:%d/ " % PORT, 60):
        print("FAIL: could not go back to index"); sys.exit(1)
    # First TAB focuses root-rel; second TAB focuses sub/page.html
    send("sendkey tab", 1.0)
    send("sendkey tab", 1.0)
    send("sendkey ret", 1.0)
    ok = wait_serial("BR HTTP OK 200 10.0.2.2:%d/sub/page.html " % PORT, 90) and \
         wait_serial("SUB PAGE OK", 5)
    print(("PASS: relative sub/page.html resolved against current dir" if ok else
           "FAIL: sub/page.html link"), flush=True)
    if not ok: sys.exit(1)

    result = "PASS"

finally:
    qemu.stdin.write(b"quit\n"); qemu.stdin.flush()
    try: qemu.wait(timeout=5)
    except: qemu.kill()

print("\nRESULT:", result, flush=True)
sys.exit(0 if result == "PASS" else 1)