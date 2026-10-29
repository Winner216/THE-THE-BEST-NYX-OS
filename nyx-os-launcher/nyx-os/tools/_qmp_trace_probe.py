#!/usr/bin/env python3
"""Probe with QEMU tracing: does input-send-event reach ps2_mouse_send_packet?"""
import subprocess, time, socket, json, os, signal

WORK = "/tmp/traceprobe"
os.makedirs(WORK, exist_ok=True)
for f in ("qmp.sock", "serial.log", "trace.log", "events.txt"):
    try: os.unlink(WORK + "/" + f)
    except Exception: pass
open(WORK + "/events.txt", "w").write("qmp_enter_input_send_event\nqmp_exit_input_send_event\nps2_mouse_send_packet\nps2_mouse_fake_event\nps2_read_data\n")

p = subprocess.Popen(["qemu-system-i386","-enable-kvm","-cpu","host",
    "-cdrom","nyx.iso","-m","1G","-boot","d","-display","none",
    "-qmp","unix:" + WORK + "/qmp.sock,server,nowait",
    "-audiodev","none,id=snd","-device","AC97,audiodev=snd",
    "-monitor","stdio","-serial","file:" + WORK + "/serial.log",
    "-trace","events=" + WORK + "/events.txt",
    "-trace","file=" + WORK + "/trace.log"],
    stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
    stderr=subprocess.DEVNULL,
    cwd="/home/winnerbek/Downloads/nyx-os-launcher/nyx-os")
time.sleep(8)
try:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(WORK + "/qmp.sock")
    fh = s.makefile("rw"); fh.readline()
    fh.write('{"execute":"qmp_capabilities"}\n'); fh.flush(); fh.readline()
    def ev(events, label):
        fh.write(json.dumps({"execute":"input-send-event",
                             "arguments":{"events":events}})+"\n"); fh.flush()
        print(label, "->", fh.readline().strip()[:60])
    ev([{"type":"rel","data":{"axis":"x","value":400}}], "rel")
    time.sleep(0.5)
    ev([{"type":"btn","data":{"button":"left","down":True}}], "btn-dn")
    time.sleep(0.5)
    ev([{"type":"btn","data":{"button":"left","down":False}}], "btn-up")
    time.sleep(1)
    s.close()
finally:
    p.send_signal(signal.SIGTERM)
    try: p.wait(timeout=5)
    except Exception: p.kill()

print("=====TRACE=====")
for line in open(WORK + "/trace.log"):
    print(line.rstrip()[:120])
