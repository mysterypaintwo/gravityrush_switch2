#!/usr/bin/env python3
"""Scripted movement test for any host (Windows PC port, Mac ARM64 host).

Runs the game with GOW3_MOVE_LOG=fast and GOW3_PAD_FILE (src/runtime_pad.c reads scripted
input from that file), presses cross on the title screen until a save's area has loaded (the
game prints "SetPlayerLimit"), waits 10 s, then pushes the left stick up, down, left and right
for 2 s each, with 2 s at rest between pushes, twice; no buttons in that part, so Kat stays put.
Each output line is prefixed with seconds since start, as the analysis scripts expect.

  python drive.py --log run.log -- <host command and arguments>

Then: python diag.py run.log  and  python dirs.py run.log
"""
import argparse
import os
import subprocess
import sys
import tempfile
import threading
import time

PUSHES = ["lx=128 ly=0", "lx=128 ly=255", "lx=0 ly=128", "lx=255 ly=128"]  # up, down, left, right
REST = "lx=128 ly=128"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", required=True)
    ap.add_argument("--duration", type=float, default=300)
    ap.add_argument("command", nargs=argparse.REMAINDER)
    a = ap.parse_args()
    cmd = a.command[1:] if a.command and a.command[0] == "--" else a.command
    if not cmd:
        sys.exit("give the host command after --")

    pad = os.path.join(tempfile.gettempdir(), "gr_pad.txt")
    open(pad, "w").close()
    env = dict(os.environ, GOW3_MOVE_LOG="fast", GOW3_PAD_FILE=pad)
    t0 = time.time()
    loaded = threading.Event()
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env,
                            bufsize=1, text=True, errors="replace")

    def pump():
        with open(a.log, "w", encoding="utf-8") as out:
            for line in proc.stdout:
                if "SetPlayerLimit" in line:
                    loaded.set()
                out.write("%7.1f %s" % (time.time() - t0, line))
                out.flush()

    reader = threading.Thread(target=pump, daemon=True)
    reader.start()

    def press(text):
        with open(pad, "w") as f:
            f.write(text + "\n")

    play = None
    while time.time() - t0 < a.duration and proc.poll() is None:
        time.sleep(1)
        t = time.time() - t0
        if play is None:
            if loaded.is_set():
                play = t
                print("area loaded at %.0f s" % t)
            elif t > 20:
                press("cross" if int(t) % 2 else "")
            continue
        p = int(t - play) - 10
        if p < 0:
            press(REST)
            continue
        if p >= 32:
            break
        c = p % 16
        press(REST if c % 4 < 2 else PUSHES[c // 4])
    proc.terminate()
    try:
        proc.wait(10)
    except subprocess.TimeoutExpired:
        proc.kill()
    reader.join(5)
    print("log:", a.log)


if __name__ == "__main__":
    main()
