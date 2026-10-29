#!/usr/bin/env python3
"""tools/pkg_store.py — Nyx App Store repository server (host side).

The Nyx kernel package manager (cpu/pkg.c) downloads apps over plain
HTTP from the HOST through QEMU's slirp gateway (10.0.2.2).  This tiny
server publishes the package catalog and the prebuilt ELF32 packages.

A "package" is a ring-3 ELF32 built from user/*.c (same ABI as the
embedded `user` demos).  The Makefile `pkgrepo` target compiles the
sources and copies the ELFs into pkgrepo/ alongside optional
`<name>.meta` files describing each app.

Routes:
    GET /                  small HTML index (handy from the browser)
    GET /pkg/manifest      text catalog, one app per line:
                           name|version|author|size|desc
    GET /pkg/<name>.elf    the app binary

Usage:
    python3 tools/pkg_store.py [repodir] [port]
        (defaults: pkgrepo/ relative to this file, port 8123)
"""
import os
import sys
import time

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
# Optional request log (append one line per GET) — handy for E2E tests.
# Set PKGSTORE_LOG=/path/to/file to enable.
REQLOG = os.environ.get("PKGSTORE_LOG") or None
REPO = os.path.normpath(sys.argv[1]) if len(sys.argv) > 1 else \
    os.path.normpath(os.path.join(HERE, "..", "pkgrepo"))
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8123


def _load_meta(name: str) -> dict:
    """Optional '<name>.meta' next to the ELF overrides the defaults."""
    meta = {"version": "1.0", "author": "Nyx Store", "desc": ""}
    path = os.path.join(REPO, name + ".meta")
    try:
        with open(path, "r", errors="replace") as fh:
            for line in fh:
                line = line.strip()
                if not line or ":" not in line:
                    continue
                k, _, v = line.partition(":")
                k, v = k.strip().lower(), v.strip()
                if k in ("version", "author", "desc") and v:
                    meta[k] = v
    except OSError:
        pass
    return meta


def _catalog():
    """List of (name, version, author, size, desc) for every *.elf."""
    out = []
    try:
        entries = sorted(os.listdir(REPO))
    except OSError:
        return out
    for fn in entries:
        if not fn.endswith(".elf"):
            continue
        name = fn[:-4]
        size = os.path.getsize(os.path.join(REPO, fn))
        meta = _load_meta(name)
        if not meta["desc"]:
            meta["desc"] = "Ring-3 Nyx package (%d bytes)" % size
        out.append((name, meta["version"], meta["author"], size, meta["desc"]))
    return out


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def _reply(self, code, ctype, body):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        try:
            self.wfile.write(body)
        except OSError:
            pass

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path == "/":
            rows = "".join(
                "<li><b>%s</b> v%s — %s <i>(%d bytes)</i></li>" % (n, v, d, s)
                for n, v, _, s, d in _catalog())
            self._reply(200, "text/html",
                        ("<html><head><title>Nyx App Store</title></head>"
                         "<body><h1>Nyx App Store repository</h1><ul>%s</ul>"
                         "</body></html>" % rows).encode("utf-8"))
            return
        if path == "/pkg/manifest":
            lines = "\n".join("%s|%s|%s|%d|%s" % row for row in _catalog())
            self._reply(200, "text/plain", (lines + "\n").encode("utf-8"))
            return
        if path.startswith("/pkg/") and path.endswith(".elf"):
            name = path[len("/pkg/"):-len(".elf")]
            # Only serve names we actually have — never path traversal.
            if "/" not in name and ".." not in name and all(
                    c.isalnum() or c in "._-" for c in name):
                fp = os.path.join(REPO, name + ".elf")
                if os.path.isfile(fp):
                    with open(fp, "rb") as fh:
                        self._reply(200, "application/octet-stream",
                                    fh.read())
                    return
            self._reply(404, "text/plain", b"not found")
            return
        self._reply(404, "text/plain", b"unknown route")

    def log_message(self, *a):  # keep stdout clean by default
        if REQLOG:
            try:
                with open(REQLOG, "a") as fh:
                    fh.write("%s %s %s\n" % (
                        time.strftime("%H:%M:%S"), self.command,
                        self.path))
            except OSError:
                pass


if __name__ == "__main__":
    print("==> Nyx App Store repo serving %s on port %d" % (REPO, PORT))
    ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()
