#!/usr/bin/env python3
"""Nyx OS HTTPS bridge (host side).

The Nyx kernel browser has no TLS stack, so https:// sites (wikipedia,
google, ...) can't be fetched directly.  This tiny bridge runs on the
HOST and speaks plain HTTP to the guest: the browser fetches

    http://10.0.2.2:8443/fetch?u=<absolute-url>

and the bridge performs the real (possibly https) fetch with Python's
urllib — following redirects, doing TLS, and handling gzip — then
returns the final status code and body over plain HTTP.

The kernel treats 10.0.2.2 as "the host" (QEMU slirp gateway); URLs
that name 10.0.2.2 are rewritten to 127.0.0.1 here so a guest-visible
redirect target resolves from the host's side too.

Usage:
    python3 tools/https_proxy.py [port]      (default 8443)
"""
import sys
import ssl
import urllib.request

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8443

# The guest can't verify certificates anyway, so don't verify on the
# host side either — this also lets the E2E use a self-signed test cert.
_CTX = ssl.create_default_context()
_CTX.check_hostname = False
_CTX.verify_mode = ssl.CERT_NONE

UA = "Mozilla/5.0 (NyxOS; bridge/1.0)"


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        target = None
        if self.path.startswith("/fetch?u="):
            target = self.path[len("/fetch?u="):]
        if not target:
            self.send_response(400)
            body = b"missing ?u="
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        # Guest gateway address -> host loopback.
        target = target.replace("10.0.2.2", "127.0.0.1")

        status = 200
        ctype = "text/html"
        body = b""
        try:
            req = urllib.request.Request(target, headers={"User-Agent": UA})
            with urllib.request.urlopen(req, timeout=30, context=_CTX) as r:
                body = r.read()
                status = r.status
                ctype = r.headers.get("Content-Type", "text/html")
        except Exception as e:  # noqa: BLE001 — report any failure to the guest
            status = 502
            msg = "<html><body><h2>Bridge error</h2><pre>%s</pre></body></html>" % e
            body = msg.encode("utf-8", "replace")

        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        try:
            self.wfile.write(body)
        except OSError:
            pass

    def log_message(self, *a):  # keep stdout clean
        pass


if __name__ == "__main__":
    print("https bridge listening on 127.0.0.1:%d" % PORT, flush=True)
    ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
