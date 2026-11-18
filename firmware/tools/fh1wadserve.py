#!/usr/bin/env python3
"""Serves WAD files (or any file) to FH1 units over HTTP, a piece at a time: Doom on the FH1 reads its WAD from here
while it plays, so a unit with no SD card plays a 28 MB WAD (apps/packages/doom, its wadsrc.c).

  fh1wadserve.py [DIR] [--port 8077]     serve the files in DIR (default: the current folder)

Then on the FH1, Doom's dev command:  wad http://<this computer's IP>:8077/freedoom1.wad
(the address is printed at the start). Range requests (bytes=a-b) and kept-alive connections; each request logged.
"""
import argparse
import http.server
import os
import re
import socket
import socketserver
import sys
import time


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"              # keep-alive: the unit reuses its connection
    root = "."

    def log_message(self, fmt, *args):
        sys.stderr.write("%s %s\n" % (time.strftime("%H:%M:%S"), fmt % args))

    def _file(self):
        name = os.path.basename(self.path.split("?", 1)[0])
        p = os.path.join(self.root, name)
        return p if name and os.path.isfile(p) else None

    def do_HEAD(self):
        p = self._file()
        if not p:
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Length", str(os.path.getsize(p)))
        self.send_header("Accept-Ranges", "bytes")
        self.end_headers()

    def do_GET(self):
        p = self._file()
        if not p:
            self.send_error(404)
            return
        size = os.path.getsize(p)
        start, end = 0, size - 1
        m = re.match(r"bytes=(\d+)-(\d*)", self.headers.get("Range", ""))
        if m:
            start = int(m.group(1))
            end = min(int(m.group(2)) if m.group(2) else size - 1, size - 1)
            if start > end:
                self.send_error(416)
                return
            self.send_response(206)
            self.send_header("Content-Range", "bytes %d-%d/%d" % (start, end, size))
        else:
            self.send_response(200)
        self.send_header("Content-Length", str(end - start + 1))
        self.send_header("Accept-Ranges", "bytes")
        self.end_headers()
        with open(p, "rb") as f:
            f.seek(start)
            self.wfile.write(f.read(end - start + 1))


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def my_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("10.255.255.255", 1))           # no packet sent: only the route's source address
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


def main():
    a = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    a.add_argument("dir", nargs="?", default=".")
    a.add_argument("--port", type=int, default=8077)
    args = a.parse_args()
    Handler.root = os.path.abspath(args.dir)
    files = [f for f in sorted(os.listdir(Handler.root)) if f.lower().endswith(".wad")]
    print("Serving %s on port %d" % (Handler.root, args.port))
    for f in files:
        print("  wad http://%s:%d/%s" % (my_ip(), args.port, f))
    Server(("", args.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
