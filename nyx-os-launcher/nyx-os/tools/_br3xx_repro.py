#!/usr/bin/env python3
"""Repro: browser 3xx-https path — capture status bar + content pixels.

Robust serial-aware driver: polls the serial log for the boot/login
milestones before sending each input batch, so boot timing drift can't
scramble the key sequence. Serves a local page that 301-redirects to
https:// and screenshots the browser at several moments.
"""
import os, sys, subprocess, time, threading, http.server, socketserver

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WORK = "/tmp/br3xx"
os.makedirs(WORK, exist_ok=True)

class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = b"<html><body>PLACEHOLDER</body></html>"
        self.send_response(301)
        self.send_header("Location", "https://wikipedia.com/")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
    def log_message(self, *a): pass

class Srv(socketserver.TCPServer):
    allow_reuse_address = True
srv = Srv(("127.0.0.1", 0), H)
PORT = srv.server_address[1]
threading.Thread(target=srv.serve_forever, daemon=True).start()

QKEY = {".": "dot", ":": "shift-semicolon"}
URL = "10.0.2.2:%d" % PORT

p = subprocess.Popen(
    ["qemu-system-i386", "-enable-kvm", "-cpu", "host", "-cdrom", ROOT + "/nyx.iso",
     "-m", "1G", "-boot", "d", "-display", "none",
     "-serial", "file:" + WORK + "/serial.log", "-monitor", "stdio",
     "-netdev", "user,id=net0", "-device", "e1000,netdev=net0"],
    stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

ser_path = WORK + "/serial.log"
last = 0

def wait_serial(marker, timeout=120, tail=0):
    """Wait until marker appears in the serial log (grows via file)."""
    global last
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            with open(ser_path, "rb") as f:
                f.seek(max(0, last - tail))
                data = f.read()
            if marker.encode() in data:
                last = os.path.getsize(ser_path)
                return True
        except FileNotFoundError:
            pass
        time.sleep(0.2)
    return False

def send(cmd, gap=0.15):
    p.stdin.write(("sendkey %s\n" % cmd).encode()); p.stdin.flush(); time.sleep(gap)

def shot(name):
    p.stdin.write(("screendump %s/%s.ppm\n" % (WORK, name)).encode())
    p.stdin.flush(); time.sleep(0.4)

try:
    if not wait_serial("LOGIN READY"): print("NO LOGIN READY"); sys.exit(1)
    time.sleep(1)
    for k in "1234": send(k, 0.3)
    send("ret", 1.0)
    if not wait_serial("DESKTOP UP"): print("NO DESKTOP"); sys.exit(1)
    time.sleep(2)
    send("f1", 1.2)                       # open terminal
    for k in list("browser"): send(k, 0.18)
    send("ret", 1.6)                      # launch browser
    time.sleep(2); shot("launch")
    time.sleep(3)
    for k in URL: send(QKEY.get(k, k), 0.13)
    send("ret", 0.3)                      # navigate (deferred fetch)
    shot("enter")
    time.sleep(2); shot("t2")
    time.sleep(6); shot("t9")
    time.sleep(6); shot("t15")
    time.sleep(2); shot("t17")
finally:
    try:
        p.stdin.write(b"quit\n"); p.stdin.flush()
        p.wait(timeout=5)
    except Exception:
        p.kill()
    srv.shutdown()

print("shots:", sorted(f for f in os.listdir(WORK) if f.endswith(".ppm")))
