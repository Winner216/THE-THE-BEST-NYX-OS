#!/usr/bin/env python3
"""Probe: do QMP rel events move the DESKTOP cursor? (screendump diff)"""
import subprocess, time, socket, json, os, sys

WORK = "/tmp/qmpprobe"
os.makedirs(WORK, exist_ok=True)
for f in (WORK + "/qmp.sock", WORK + "/serial.log"):
    try: os.unlink(f)
    except Exception: pass

p = subprocess.Popen(["qemu-system-i386","-enable-kvm","-cpu","host",
    "-cdrom","nyx.iso","-m","1G","-boot","d","-display","none",
    "-qmp","unix:" + WORK + "/qmp.sock,server,nowait",
    "-audiodev","none,id=snd","-device","AC97,audiodev=snd",
    "-monitor","stdio","-serial","file:" + WORK + "/serial.log"],
    stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
    stderr=subprocess.DEVNULL,
    cwd="/home/winnerbek/Downloads/nyx-os-launcher/nyx-os")

def hmp(cmd, wait=0.4):
    p.stdin.write((cmd+"\n").encode()); p.stdin.flush(); time.sleep(wait)

def readlog():
    try: return open(WORK+"/serial.log","rb").read().decode("latin1","replace")
    except Exception: return ""

def wait_log(sub, timeout=90):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if sub in readlog(): return True
        time.sleep(1)
    return False

try:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    while True:
        try:
            s.connect(WORK + "/qmp.sock"); break
        except Exception:
            time.sleep(0.5)
    fh = s.makefile("rw"); fh.readline()
    fh.write('{"execute":"qmp_capabilities"}\n'); fh.flush(); fh.readline()
    def ev(events):
        fh.write(json.dumps({"execute":"input-send-event",
                             "arguments":{"events":events}})+"\n"); fh.flush()
        return fh.readline().strip()[:80]

    wait_log("LOGIN READY", 90)
    for k in "1 2 3 4".split(): hmp("sendkey " + k, 0.25)
    hmp("sendkey ret", 0.4)
    wait_log("DESKTOP UP", 120)
    time.sleep(5)

    hmp("screendump " + WORK + "/a.ppm", 0.8)
    print("rel ack:", ev([{"type":"rel","data":{"axis":"x","value":400}},
                          {"type":"rel","data":{"axis":"y","value":300}}]))
    time.sleep(0.5)
    hmp("screendump " + WORK + "/b.ppm", 0.8)
    print("rel ack2:", ev([{"type":"btn","data":{"button":"left","down":True}}]))
    time.sleep(0.2)
    print("rel ack3:", ev([{"type":"btn","data":{"button":"left","down":False}}]))
    time.sleep(0.5)
    hmp("screendump " + WORK + "/c.ppm", 0.8)
finally:
    hmp("quit", 0.4)
    try: p.wait(timeout=8)
    except Exception: p.kill()

def diff(a, b):
    da = open(a,"rb").read().split(b"\n",3)[3]
    db = open(b,"rb").read().split(b"\n",3)[3]
    return sum(1 for i in range(0,len(da),3) if da[i:i+3]!=db[i:i+3])
print("a->b (rel 400,300):", diff(WORK+"/a.ppm", WORK+"/b.ppm"))
print("b->c (click):      ", diff(WORK+"/b.ppm", WORK+"/c.ppm"))
