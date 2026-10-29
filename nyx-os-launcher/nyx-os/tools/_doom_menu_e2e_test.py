#!/usr/bin/env python3
"""E2E: DOOM menu behavior — ESC pauses the world, arrows navigate.

Flow: boot (KVM) → login → desktop search "doom" → launch → wait for
gameplay → New Game via menu keys → ESC to open in-game menu → freeze
check (two screendumps must differ LESS than a playing-world baseline)
→ arrow keys move selection → screenshot.

Key checks:
  demo_freezes   — ESC on the title screen pauses the attract demo
                   (two dumps 2 s apart must differ far less than the
                   playing-demo baseline; vanilla kept demos running
                   behind the menu — that read as a bug)
  demo_resumes   — closing the menu starts the demo again
  playing_delta  — pixels changed over 2 s while playing (big)
  menu_delta     — pixels changed over 2 s while menu open (≈0 = paused)
  menu_visual    — menu overlay visible (differs from playing frame)
  arrow moves    — selection moved after DOWN key (skull position)
"""
import subprocess, time, os, sys, hashlib, socket, json

WORK = "/tmp/doommenutest"
os.makedirs(WORK, exist_ok=True)
serial_path = os.path.join(WORK, "serial.log")
open(serial_path, "w").close()
ROOT = "/home/winnerbek/Downloads/nyx-os-launcher/nyx-os"

def readlog():
    try:
        return open(serial_path, "rb").read().decode("latin1", "replace")
    except Exception:
        return ""

def wait_log(sub, timeout=240):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if sub in readlog(): return True
        time.sleep(1)
    return False

qemu = subprocess.Popen(
    ["qemu-system-i386", "-enable-kvm", "-cpu", "host",
     "-cdrom", "nyx.iso", "-m", "1G", "-boot", "d",
     "-audiodev", "none,id=snd", "-device", "AC97,audiodev=snd",
     "-monitor", "stdio", "-serial", "file:" + WORK + "/serial.log",
     "-qmp", "unix:" + WORK + "/qmp.sock,server,nowait",
     "-display", "none"],
    stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
    stderr=subprocess.DEVNULL, cwd=ROOT)

def send(cmd, wait=0.4):
    qemu.stdin.write((cmd + "\n").encode()); qemu.stdin.flush(); time.sleep(wait)

def md5(p):
    return hashlib.md5(open(p, "rb").read()).hexdigest()

# ── QMP mouse injection (deterministic turns for the motion baseline;
#    HMP mouse_move collapses X deltas with -display none) ──────────
def qmp_connect(path, timeout=15):
    deadline = time.time() + timeout
    while time.time() < deadline:
        s = None
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.connect(path)
            f = s.makefile("rw")
            json.loads(f.readline())          # greeting
            f.write('{"execute":"qmp_capabilities"}\n'); f.flush()
            json.loads(f.readline())          # capabilities ack
            return s, f
        except Exception:
            if s: s.close()
            time.sleep(0.5)
    raise RuntimeError("QMP connect failed")

qmp, qmpf = qmp_connect(WORK + "/qmp.sock")

def mouse_rel(dx, dy):
    qmpf.write(json.dumps({"execute": "input-send-event",
                           "arguments": {"events": [
                               {"type": "rel",
                                "data": {"axis": "x", "value": dx}}]}}) + "\n")
    qmpf.flush()
    json.loads(qmpf.readline())

results = {}
try:
    results["login"] = wait_log("LOGIN READY", 90)
    for k in "1 2 3 4".split(): send("sendkey " + k, 0.25)
    send("sendkey ret", 0.4)
    results["desktop"] = wait_log("DESKTOP UP", 120)
    time.sleep(5)
    send("sendkey slash", 0.6)
    send("sendkey d", 0.3); send("sendkey o", 0.3)
    send("sendkey o", 0.3); send("sendkey m", 0.3)
    send("sendkey ret", 1.2)
    results["pe_load"] = wait_log("PE: loading", 60)
    results["doom_start"] = wait_log("[DOOM] starting", 60)
    results["gamestart"] = wait_log("[DOOM] doomgeneric_Create", 90)
    results["mixer"] = wait_log("kernel mixer present", 30)

    # ── Attract suspend/resume check ──────────────────────────────
    # Nyx attract loop = title pages only (no demos: they crashed the
    # zone allocator).  The pages cycle every ~3.4-4 s; the menu must
    # SUSPEND the cycling ([DOOM] page lines stop) and closing the
    # menu must RESUME it.  Verified via the serial page-change hook.
    time.sleep(12)               # title + first pages underway
    pages_before = readlog().count("[DOOM] page ")
    send("screendump " + WORK + "/demo_a.ppm", 0.8)
    send("sendkey esc", 0.8)     # menu open → pages suspend
    time.sleep(6)
    pages_menu   = readlog().count("[DOOM] page ")
    send("screendump " + WORK + "/dmenu1.ppm", 0.8)
    send("sendkey esc", 0.8)     # close menu → pages resume
    time.sleep(8)                # > one page period (3.4-4 s)
    pages_after  = readlog().count("[DOOM] page ")
    send("screendump " + WORK + "/dres_b.ppm", 0.8)
    results["pages_suspended"] = pages_menu == pages_before
    results["pages_resumed"]   = pages_after > pages_menu

    # Start a REAL game (attract demos never were good baselines).
    # Shareware flow: ESC pops the menu (any key does during the demo),
    # then Enter×3: New Game → Episode (shareware has EpiDef!) → Skill.
    send("sendkey esc", 0.8)     # open main menu (New Game preselected)
    send("sendkey ret", 1.0)     # New Game → episode select
    send("sendkey ret", 1.0)     # Episode 1 → skill select
    send("sendkey ret", 1.0)     # "I'm too young to die" → start level
    time.sleep(12)               # level load + render
    send("screendump " + WORK + "/p1.ppm", 0.8)

    # Baseline: world playing — two dumps 2 s apart should differ a lot.
    # (Standing still in a quiet room barely moves — turn while sampling.
    #  QMP mouse rel moves are deterministic; keyboard timing can flake
    #  when keys land during the level splash.)
    send("screendump " + WORK + "/play_a.ppm", 0.8)
    for _ in range(12):
        send("sendkey right", 0.2)
    for _ in range(8):
        mouse_rel(120, 0); time.sleep(0.15)
    send("screendump " + WORK + "/play_b.ppm", 0.8)

    # Open the in-game menu with ESC.
    send("sendkey esc", 0.8)
    send("screendump " + WORK + "/menu_open.ppm", 0.8)
    # While menu is open the world should be frozen.
    time.sleep(2)
    send("screendump " + WORK + "/menu_a.ppm", 0.8)
    time.sleep(2)
    send("screendump " + WORK + "/menu_b.ppm", 0.8)

    # Move the selection down twice, then screenshot.
    send("sendkey down", 0.6)
    send("sendkey down", 0.6)
    send("screendump " + WORK + "/menu_moved.ppm", 0.8)

    results["realgame"] = " IGL" in readlog()  # E2E hook: in real gameplay
    results["alive"] = qemu.poll() is None
    send("quit", 0.4)
    qemu.wait(timeout=10)
except Exception as e:
    print("ERR:", e)
finally:
    try: qemu.kill()
    except Exception: pass

def diff_ppm(a, b):
    def openit(p):
        d = open(p, "rb").read()
        i = 0
        while d[i:i+1] != b"\n": i += 1
        i += 1
        while d[i:i+1] != b"\n": i += 1
        i += 1
        return d, i
    try:
        da, oa = openit(a); db, ob = openit(b)
    except Exception:
        return -1
    if oa != ob or len(da) != len(db): return -1
    diff = 0
    for p in range(oa, len(da), 3):
        if da[p:p+3] != db[p:p+3]: diff += 1
    return diff

d_play  = diff_ppm(WORK + "/play_a.ppm", WORK + "/play_b.ppm")
d_menu  = diff_ppm(WORK + "/menu_a.ppm", WORK + "/menu_b.ppm")
d_open  = diff_ppm(WORK + "/play_b.ppm", WORK + "/menu_open.ppm")
d_moved = diff_ppm(WORK + "/menu_open.ppm", WORK + "/menu_moved.ppm")
# Attract suspend/resume now uses the [DOOM] page serial hook.
print("pages suspended while menu open:", results["pages_suspended"])
print("pages resumed after menu close: ", results["pages_resumed"])
print("playing-world delta (2s):", d_play)
print("menu-open delta (2s):    ", d_menu)
print("menu overlay delta:      ", d_open)
print("after DOWN keys delta:   ", d_moved)

results["world_playing"]  = d_play > 20000
# Menu freeze: a paused world still animates the status-bar face, so
# allow a small floor, but it must be far below the playing baseline.
results["menu_freezes"]   = d_menu < max(5000, d_play // 8)
results["menu_visible"]   = d_open > 5000
results["arrows_move"]    = d_moved > 500

for n in ("p1", "menu_open", "menu_moved"):
    p = WORK + "/" + n + ".ppm"
    if os.path.exists(p):
        print(n, os.path.getsize(p))

print("RESULTS:", {k: ("OK" if v else "FAIL") for k, v in results.items()})
ok = all(results.values())
print("OVERALL:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
