#!/usr/bin/env python3
"""Host-side engine selftest harness: compiles user/chess/*.c with the host
gcc (32-bit, freestanding — the same flags as the PE build) and runs
perft/mate/tactic/selfplay checks.  Test driver lives in _chess_selftest.c."""

import subprocess, sys, os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC  = os.path.join(ROOT, "user/chess")
OUT  = "/tmp/chess_selftest"
os.makedirs(OUT, exist_ok=True)

CC = "gcc"
CFLAGS = ["-m32", "-O2", "-fno-pie", "-fno-strict-aliasing", "-Wno-main",
          "-Wno-unused-function", "-Wno-builtin", "-I" + SRC,
          "-idirafter", "/usr/lib/gcc/x86_64-linux-gnu/13/include"]

def run():
    exe = os.path.join(OUT, "selftest")
    cmd = [CC] + CFLAGS + [
        os.path.join(SRC, "chess_engine.c"),
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "_chess_selftest.c"),
        "-o", exe]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print("COMPILE FAIL:\n" + r.stdout + r.stderr)
        return 1
    r = subprocess.run([exe], capture_output=True, text=True)
    print(r.stdout)
    if r.stderr: print(r.stderr)
    return r.returncode

if __name__ == "__main__":
    sys.exit(run())
