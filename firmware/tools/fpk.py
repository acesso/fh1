#!/usr/bin/env python3
"""Build, check and inspect FH1 app packages (.fpk), as described in firmware/PLAN.md ("App packages").

An .fpk (named <id>_<version>.fpk) is an uncompressed ustar archive holding manifest.json, app.elf, SHA256SUMS, and optionally
an icon and an assets/ folder. An asset package ("kind": "assets") has no app.elf: only assets/, installed once to
/sd/assets/<id> ("where": "sd") or /int/assets/<id>, for every app that names it in "uses".

  fpk.py pack <app dir> [-o out.fpk]     app dir holds manifest.json and build/<name>.app.elf (or app.elf)
  fpk.py check <file.fpk>                verify layout, manifest and checksums, as the device would
  fpk.py list <file.fpk>                 show the manifest and the files
  fpk.py upload <file.fpk> --code NNNNNN [--host fh1.local] [--to sd|int]
"""
import argparse
import glob
import hashlib
import io
import json
import os
import re
import sys
import tarfile
import urllib.request

SDK = (0, 8)                    # fh1_sdk.h: FH1_SDK_MAJOR, FH1_SDK_MINOR
CAPS = {"DISPLAY", "BACKLIGHT", "BUTTONS", "STICK_ANALOG", "TOUCH", "AUDIO_OUT", "AUDIO_IN", "SD", "WIFI", "BLE",
        "LORA", "IMU", "RTC", "BATTERY", "BUTTON_LIGHTS"}


def check_manifest(m):
    errors = []
    if "publisher" in m and not re.fullmatch(r"[a-z0-9-]+(\.[a-z0-9-]+)+", str(m["publisher"])):
        errors.append('publisher: reverse-DNS, lowercase ("org.fh1")')
    if not re.fullmatch(r"[a-z][a-z0-9._-]{2,46}", str(m.get("id", ""))):
        errors.append("id: lowercase letters, digits, . _ -, 3 to 47 characters")
    if not m.get("name") or len(m["name"]) > 31:
        errors.append("name: 1 to 31 characters")
    if not m.get("version") or len(m["version"]) > 15:
        errors.append("version: 1 to 15 characters")
    sdk = re.fullmatch(r"(\d+)\.(\d+)", str(m.get("sdk", "")))
    if not sdk:
        errors.append('sdk: "major.minor"')
    elif int(sdk[1]) != SDK[0] or int(sdk[2]) > SDK[1]:
        errors.append(f"sdk {m['sdk']} does not run on SDK {SDK[0]}.{SDK[1]}")
    for key in ("needs", "wants"):
        unknown = [c for c in m.get(key, []) if c not in CAPS]
        if unknown:
            errors.append(f"{key}: unknown capabilities {unknown}")
    if m.get("entry", "app.elf") != "app.elf":
        errors.append("entry must be app.elf")
    if m.get("kind", "app") not in ("app", "assets"):
        errors.append('kind: "app" or "assets"')
    if m.get("where", "int") not in ("int", "sd"):
        errors.append('where: "int" or "sd" (where an asset package is installed)')
    for u in m.get("uses", []):
        if not re.fullmatch(r"[a-z][a-z0-9._-]{2,46}", str(u)):
            errors.append(f"uses: {u!r} is not a package id")
    return errors


def add_bytes(tar, name, data):
    info = tarfile.TarInfo(name)
    info.size = len(data)
    info.mode = 0o644
    tar.addfile(info, io.BytesIO(data))


def pack(args):
    d = args.dir
    with open(os.path.join(d, "manifest.json"), "rb") as f:
        manifest_raw = f.read()
    manifest = json.loads(manifest_raw)
    errors = check_manifest(manifest)
    if errors:
        sys.exit("manifest.json:\n  " + "\n  ".join(errors))
    files = {"manifest.json": manifest_raw}
    if manifest.get("kind", "app") == "app":               # an asset package has no code
        elf = os.path.join(d, "app.elf")
        if not os.path.exists(elf):
            found = glob.glob(os.path.join(d, "build", "*.app.elf"))
            if len(found) != 1:
                sys.exit("no app.elf: build the app first (idf.py build), or put app.elf next to manifest.json")
            elf = found[0]
        files["app.elf"] = open(elf, "rb").read()
    for extra in ("icon.png",):
        p = os.path.join(d, extra)
        if os.path.exists(p):
            files[extra] = open(p, "rb").read()
    assets = os.path.join(d, "assets")
    for root, _, names in os.walk(assets):
        for n in sorted(names):
            p = os.path.join(root, n)
            files[os.path.relpath(p, d).replace(os.sep, "/")] = open(p, "rb").read()
    sums = "".join(f"{hashlib.sha256(data).hexdigest()}  {name}\n" for name, data in files.items())
    out = args.output or f"{manifest['id']}_{manifest['version']}.fpk"
    with tarfile.open(out, "w", format=tarfile.USTAR_FORMAT) as tar:
        for name, data in files.items():
            add_bytes(tar, name, data)
        add_bytes(tar, "SHA256SUMS", sums.encode())
    print(f"{out}: {manifest['id']} {manifest['version']}, {len(files) + 1} files, {os.path.getsize(out)} bytes")


def read(path):
    with tarfile.open(path, "r:") as tar:
        if tar.format != tarfile.USTAR_FORMAT and any(m.pax_headers for m in tar.getmembers()):
            raise SystemExit("not plain ustar (PAX headers found)")
        return {m.name.removeprefix("./"): tar.extractfile(m).read() for m in tar.getmembers() if m.isfile()}


def check(args):
    files = read(args.file)
    errors = []
    if "manifest.json" not in files:
        errors.append("no manifest.json")
    else:
        errors += check_manifest(json.loads(files["manifest.json"]))
    kind = json.loads(files["manifest.json"]).get("kind", "app") if "manifest.json" in files else "app"
    if kind == "assets":
        pass
    elif "app.elf" not in files:
        errors.append("no app.elf")
    elif files["app.elf"][:4] != b"\x7fELF":
        errors.append("app.elf is not an ELF file")
    sums = {}
    for line in files.get("SHA256SUMS", b"").decode().splitlines():
        h, _, name = line.partition("  ")
        sums[name.lstrip("*")] = h
    if not sums:
        errors.append("no SHA256SUMS")
    for name, data in files.items():
        if name == "SHA256SUMS":
            continue
        if name not in sums:
            errors.append(f"not in SHA256SUMS: {name}")
        elif hashlib.sha256(data).hexdigest() != sums[name]:
            errors.append(f"checksum mismatch: {name}")
    for name in sums:
        if name not in files:
            errors.append(f"listed but missing: {name}")
    if errors:
        sys.exit(f"{args.file}:\n  " + "\n  ".join(errors))
    print(f"{args.file}: OK")


def listing(args):
    files = read(args.file)
    print(json.dumps(json.loads(files.get("manifest.json", b"{}")), indent=2))
    for name, data in files.items():
        print(f"  {len(data):8}  {name}")


def upload(args):
    name = os.path.basename(args.file)
    req = urllib.request.Request(f"http://{args.host}/upload?name={name}&dest={args.to}", data=open(args.file, "rb").read(),
                                 headers={"X-FH1-Code": args.code}, method="POST")
    with urllib.request.urlopen(req, timeout=60) as r:
        print(f"{name}: {r.read().decode()}")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("pack"); s.add_argument("dir"); s.add_argument("-o", "--output"); s.set_defaults(fn=pack)
    s = sub.add_parser("check"); s.add_argument("file"); s.set_defaults(fn=check)
    s = sub.add_parser("list"); s.add_argument("file"); s.set_defaults(fn=listing)
    s = sub.add_parser("upload"); s.add_argument("file"); s.add_argument("--code", required=True)
    s.add_argument("--host", default="fh1.local"); s.add_argument("--to", choices=["sd", "int"], default="sd")
    s.set_defaults(fn=upload)
    args = p.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
