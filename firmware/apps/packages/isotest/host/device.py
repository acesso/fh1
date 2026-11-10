#!/usr/bin/env python3
"""Runs a host-style script on the FH1 itself, over its dev console: keys, the game's development commands,
screenshots. One connection for the whole script, so the timing stays close to the script's.

  FH1_CODE=NNNNNN host/device.py SCRIPT OUTDIR [--host fh1.local]

Script lines: key A|B|X|Y|UP|DOWN|LEFT|RIGHT|START|SELECT|MENU, sleep SECONDS, shot NAME (OUTDIR/NAME.png),
dev TEXT (the game's dev_command: place, act, counter, gear, ranged, xp, hp, win, lose, log, nosave...),
op OP [PATH] [ID] (a console op: start, stop...). Lines starting with # are comments.
"""
import asyncio
import io
import json
import os
import shlex
import sys
import urllib.request

import websockets
from PIL import Image


async def main(script, outdir, host):
    code = os.environ["FH1_CODE"]
    os.makedirs(outdir, exist_ok=True)
    async with websockets.connect(f"ws://{host}/ws", max_size=None) as ws:
        await ws.send(json.dumps({"t": "auth", "token": code}))
        await asyncio.sleep(1.0)

        # The console pushes stats all the time: read everything, keep the replies.
        replies = asyncio.Queue()

        async def reader():
            async for raw in ws:
                try:
                    m = json.loads(raw)
                except ValueError:
                    continue
                if m.get("t") == "reply":
                    await replies.put(m.get("msg", m))

        reading = asyncio.create_task(reader())

        async def op(name, path="", id_=""):
            await ws.send(json.dumps({"t": "app", "op": name, "path": path, "id": id_}))
            try:
                return await asyncio.wait_for(replies.get(), 25)
            except asyncio.TimeoutError:
                return None

        for raw in open(script):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            cmd, _, rest = line.partition(" ")
            if cmd == "key":
                k = rest.strip().lower()
                await ws.send(json.dumps({"t": "key", "k": k, "p": True}))
                await asyncio.sleep(0.07)
                await ws.send(json.dumps({"t": "key", "k": k, "p": False}))
                await asyncio.sleep(0.12)
            elif cmd == "sleep":
                await asyncio.sleep(float(rest))
            elif cmd == "shot":
                data = await asyncio.to_thread(
                    lambda: urllib.request.urlopen(f"http://{host}/shot.bmp?code={code}", timeout=20).read())
                Image.open(io.BytesIO(data)).save(os.path.join(outdir, rest.strip() + ".png"))
                print("shot", rest.strip())
            elif cmd == "dev":
                print("dev", rest, "->", await op("dev", rest))
            elif cmd == "op":
                args = shlex.split(rest)
                reply = await op(*args)
                print("op", rest, "->", reply)
                # Someone may be using the FH1 (a game of theirs on screen): keys would go into it. Stop.
                if args[0] == "start" and "started" not in str(reply):
                    print("the game did not start: stopping before any key is sent")
                    break
            else:
                print("?", line)
        reading.cancel()


if __name__ == "__main__":
    host = "fh1.local"
    if "--host" in sys.argv:
        host = sys.argv[sys.argv.index("--host") + 1]
    asyncio.run(main(sys.argv[1], sys.argv[2], host))
