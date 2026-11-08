#!/usr/bin/env python3
"""A pretend FH1 on the PC, to test users between units (firmware/docs/users.md, U4 and U5).

It has its own unit key and one user (Ed25519 keys kept in a JSON file), signs profiles and trust statements
the way the firmware does (canonical CBOR, components/fh1_users), and talks to FH1s over HTTP:

  fh1peer.py users [HOST]                 the FH1's users, their signatures checked here (default fh1.local)
  fh1peer.py serve [--name ana] [--port 8080] [--accept]
                                          serve /fh1/users and /fh1/trust, announced by mDNS (avahi-publish-service)
                                          as _fh1._tcp, so the FH1 lists this user in People nearby. --accept
                                          answers a friend request with a friend statement back.
  fh1peer.py befriend USER_ID [HOST] [--level 2]
                                          send our user's statement about one of the FH1's users (2 friend, 0 none)

State: --state FILE (default ~/.fh1peer.json): the keys and the revisions used. Needs: python3-cryptography.
"""
import argparse
import hashlib
import json
import os
import struct
import subprocess
import sys
import urllib.request
from http.server import BaseHTTPRequestHandler, HTTPServer

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey, Ed25519PublicKey
from cryptography.hazmat.primitives import serialization

B32 = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"


# ── Canonical CBOR, the subset the firmware writes ──

def head(major, v):
    if v < 24:
        return bytes([major << 5 | v])
    if v < 256:
        return bytes([major << 5 | 24, v])
    if v < 65536:
        return bytes([major << 5 | 25]) + struct.pack(">H", v)
    return bytes([major << 5 | 26]) + struct.pack(">I", v)


def cbytes(b):
    return head(2, len(b)) + b


def ctext(s):
    b = s.encode()
    return head(3, len(b)) + b


def cmap(items):
    """items: [(key, encoded value)] in key order."""
    return head(5, len(items)) + b"".join(head(0, k) + v for k, v in items)


def parse(b):
    """A flat map of uint keys to uint / bytes / text; returns {key: value}."""
    i = 0

    def h():
        nonlocal i
        x = b[i]
        i += 1
        major, ai = x >> 5, x & 31
        if ai < 24:
            return major, ai
        k = {24: 1, 25: 2, 26: 4}[ai]
        v = int.from_bytes(b[i:i + k], "big")
        i += k
        return major, v

    major, n = h()
    assert major == 5
    out = {}
    for _ in range(n):
        _, key = h()
        major, v = h()
        if major == 0:
            out[key] = v
        else:
            out[key] = b[i:i + v] if major == 2 else b[i:i + v].decode()
            i += v
    assert i == len(b)
    return out


# ── Ids, profiles, statements ──

def id_of(pk):
    h = hashlib.sha256(pk).digest()
    bits = int.from_bytes(h[:10], "big")
    return "".join(B32[(bits >> (75 - 5 * i)) & 31] for i in range(16)), h[:10]


def pub(sk):
    return sk.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)


def record(sk, name, rev, unit_raw, avatar=b""):
    pk = pub(sk)
    fields = [(0, head(0, 1)), (1, cbytes(pk)), (2, ctext(name)), (3, cbytes(avatar)), (4, head(0, rev)), (5, cbytes(unit_raw))]
    sig = sk.sign(cmap(fields))
    return cmap(fields + [(6, cbytes(sig))])


def check_record(rec):
    m = parse(rec)
    fields = [(0, head(0, m[0])), (1, cbytes(m[1])), (2, ctext(m[2])), (3, cbytes(m[3])), (4, head(0, m[4])), (5, cbytes(m[5]))]
    assert cmap(fields + [(6, cbytes(m[6]))]) == rec, "not canonical"
    Ed25519PublicKey.from_public_bytes(m[1]).verify(m[6], cmap(fields))
    return m


def statement(sk, to_id, level, rev):
    fields = [(0, head(0, 1)), (1, cbytes(pub(sk))), (2, ctext(to_id)), (3, head(0, level)), (4, head(0, rev))]
    return cmap(fields + [(5, cbytes(sk.sign(cmap(fields))))])


def check_statement(st):
    m = parse(st)
    fields = [(0, head(0, m[0])), (1, cbytes(m[1])), (2, ctext(m[2])), (3, head(0, m[3])), (4, head(0, m[4]))]
    assert cmap(fields + [(5, cbytes(m[5]))]) == st, "not canonical"
    Ed25519PublicKey.from_public_bytes(m[1]).verify(m[5], cmap(fields))
    return m


def message(rec, st):
    return b"FH1T\x01" + struct.pack(">H", len(rec)) + rec + struct.pack(">H", len(st)) + st


# ── State ──

class Peer:
    def __init__(self, path, name):
        self.path = path
        st = json.load(open(path)) if os.path.exists(path) else {}
        if not st:
            st = {"unit": Ed25519PrivateKey.generate().private_bytes(serialization.Encoding.Raw, serialization.PrivateFormat.Raw,
                                                                      serialization.NoEncryption()).hex(),
                  "user": Ed25519PrivateKey.generate().private_bytes(serialization.Encoding.Raw, serialization.PrivateFormat.Raw,
                                                                      serialization.NoEncryption()).hex(),
                  "name": name or "ana", "rev": 1, "said": {}}
        if name and name != st["name"]:
            st["name"], st["rev"] = name, st["rev"] + 1
        self.st = st
        self.unit_sk = Ed25519PrivateKey.from_private_bytes(bytes.fromhex(st["unit"]))
        self.sk = Ed25519PrivateKey.from_private_bytes(bytes.fromhex(st["user"]))
        self.unit_id, self.unit_raw = id_of(pub(self.unit_sk))
        self.id, _ = id_of(pub(self.sk))
        self.save()

    def save(self):
        json.dump(self.st, open(self.path, "w"), indent=1)

    def record(self):
        return record(self.sk, self.st["name"], self.st["rev"], self.unit_raw)

    def say(self, to_id, level):
        rev = self.st["said"].get(to_id, 0) + 1
        self.st["said"][to_id] = rev
        self.save()
        return message(self.record(), statement(self.sk, to_id, level, rev))


def fetch_users(host):
    body = urllib.request.urlopen(f"http://{host}/fh1/users", timeout=8).read()
    assert body[:5] == b"FH1R\x01", body[:16]
    unit, count, i, out = body[5:21].decode(), body[21], 22, []
    for _ in range(count):
        n = struct.unpack(">H", body[i:i + 2])[0]
        out.append(check_record(body[i + 2:i + 2 + n]))
        i += 2 + n
    return unit, out


def post(host, msg):
    req = urllib.request.Request(f"http://{host}/fh1/trust", data=msg, method="POST")
    return urllib.request.urlopen(req, timeout=8).status


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["users", "serve", "befriend"])
    ap.add_argument("args", nargs="*")
    ap.add_argument("--state", default=os.path.expanduser("~/.fh1peer.json"))
    ap.add_argument("--name")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--level", type=int, default=2)
    ap.add_argument("--accept", action="store_true")
    a = ap.parse_args()

    if a.cmd == "users":
        unit, users = fetch_users(a.args[0] if a.args else "fh1.local")
        print(f"unit {unit}: {len(users)} users, every signature checked here")
        for m in users:
            uid, _ = id_of(m[1])
            print(f"  {m[2]}#{uid[:4]}  id {uid}  rev {m[4]}  avatar {m[3].hex() or '-'}")
        return

    peer = Peer(a.state, a.name)
    print(f"peer unit {peer.unit_id}, user {peer.st['name']}#{peer.id[:4]} ({peer.id})")

    if a.cmd == "befriend":
        to = a.args[0]
        host = a.args[1] if len(a.args) > 1 else "fh1.local"
        print("sent:", post(host, peer.say(to, a.level)))
        return

    class H(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_GET(self):
            if self.path != "/fh1/users":
                self.send_error(404)
                return
            rec = peer.record()
            body = b"FH1R\x01" + peer.unit_id.encode() + bytes([1]) + struct.pack(">H", len(rec)) + rec
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            print(f"{self.client_address[0]} took our profile")

        def do_POST(self):
            n = int(self.headers.get("Content-Length", 0))
            msg = self.rfile.read(n)
            try:
                assert msg[:5] == b"FH1T\x01"
                rn = struct.unpack(">H", msg[5:7])[0]
                rec = check_record(msg[7:7 + rn])
                st = check_statement(msg[9 + rn:])
                assert st[1] == rec[1], "signed by someone else"
            except Exception as e:
                print("refused:", e)
                self.send_error(400)
                return
            uid, _ = id_of(rec[1])
            print(f"{rec[2]}#{uid[:4]} says level {st[3]} (rev {st[4]}) about {st[2]}")
            self.send_response(202)
            self.send_header("Content-Length", "2")
            self.end_headers()
            self.wfile.write(b"ok")
            if a.accept and st[2] == peer.id and st[3] >= 2 and peer.st["said"].get(uid, 0) == 0:
                print("  accepting:", post(self.client_address[0], peer.say(uid, 2)))

    avahi = subprocess.Popen(["avahi-publish-service", f"fh1-peer-{peer.unit_id[:4]}", "_fh1._tcp", str(a.port), f"unit={peer.unit_id}"],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    print(f"serving on port {a.port}, announced as _fh1._tcp")
    try:
        HTTPServer(("", a.port), H).serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        avahi.terminate()


if __name__ == "__main__":
    main()
