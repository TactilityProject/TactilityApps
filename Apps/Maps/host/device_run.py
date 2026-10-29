# SPDX-License-Identifier: GPL-3.0-or-later
"""Runs the app on a device with a debug script, records the serial log and fetches the screenshots.

Usage: python host/device_run.py <device ip> <serial port> <script.txt> <out folder> [seconds]
The script is copied to the app's folder as debug.txt (see main/Source/app/Debug.h for its commands).
Every "shot <name>" in it is fetched as <out folder>/<name>.png. The debug.txt is removed afterwards.
"""
import os
import re
import struct
import subprocess
import sys
import threading
import time
import zlib

import serial

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
TOOLS = os.path.join(REPO, "reference", "Tactility", "Buildscripts", "TactilitySDK", "Tools")
DEVICE_ROOT = "/sdcard/tactility/user/app/tactility.maps"
APP_ID = "tactility.maps"


def tool(name, *arguments):
    return subprocess.run([sys.executable, os.path.join(TOOLS, name), *arguments], capture_output=True, text=True)


def bmp_to_png(bmp_path, png_path):
    data = open(bmp_path, "rb").read()
    offset, = struct.unpack_from("<I", data, 10)
    width, height = struct.unpack_from("<ii", data, 18)
    row_bytes = (width * 3 + 3) & ~3
    raw = bytearray()
    for y in range(abs(height)):
        source_row = (abs(height) - 1 - y) if height > 0 else y
        row = data[offset + source_row * row_bytes: offset + source_row * row_bytes + width * 3]
        raw.append(0)
        for x in range(width):
            b, g, r = row[x * 3: x * 3 + 3]
            raw += bytes((r, g, b))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, abs(height), 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b"")
    open(png_path, "wb").write(png)


def record(port, seconds, path, stop):
    connection = serial.Serial()
    connection.port = port
    connection.baudrate = 115200
    connection.timeout = 0.2
    connection.dtr = False
    connection.rts = False
    connection.open()
    end = time.time() + seconds
    with open(path, "wb") as log:
        while time.time() < end and not stop.is_set():
            data = connection.read(4096)
            if data:
                log.write(data)
                log.flush()
    connection.close()


def main():
    if len(sys.argv) < 5:
        print(__doc__)
        return 2
    host, port, script, out = sys.argv[1:5]
    seconds = float(sys.argv[5]) if len(sys.argv) > 5 else 60
    os.makedirs(out, exist_ok=True)
    shots = re.findall(r"^\s*shot\s+(\S+)", open(script).read(), re.MULTILINE)
    result = tool("push.py", "-h", host, script, f"{DEVICE_ROOT}/debug.txt")
    if result.returncode != 0:
        print("push failed:", result.stderr or result.stdout)
        return 1
    log_path = os.path.join(out, "serial.log")
    stop = threading.Event()
    recorder = threading.Thread(target=record, args=(port, seconds, log_path, stop))
    recorder.start()
    time.sleep(0.5)
    env = dict(os.environ)
    env["PYTHONIOENCODING"] = "utf-8"
    run = subprocess.run([sys.executable, os.path.join(REPO, "reference", "Tactility", "tt.py"), "app", "run", "--host", host],
                         cwd=REPO, capture_output=True, text=True, env=env)
    print("run:", (run.stdout + run.stderr).strip().splitlines()[-1:] or "")
    # The app writes "LVGL started" once it has left the map
    deadline = time.time() + seconds
    while time.time() < deadline:
        time.sleep(1)
        try:
            if b"LVGL started" in open(log_path, "rb").read():
                break
        except OSError:
            pass
    time.sleep(1)
    stop.set()
    recorder.join()
    for name in shots:
        local_bmp = os.path.join(out, name + ".bmp")
        result = tool("pull.py", "-h", host, f"{DEVICE_ROOT}/{name}.bmp", local_bmp)
        if result.returncode == 0 and os.path.exists(local_bmp):
            bmp_to_png(local_bmp, os.path.join(out, name + ".png"))
            os.remove(local_bmp)
            print("shot", name + ".png")
        else:
            print("shot", name, "not fetched:", (result.stderr or result.stdout).strip())
    tool("shell.py", "-h", host, f"rm {DEVICE_ROOT}/debug.txt")
    text = open(log_path, "rb").read().decode("utf-8", "replace")
    for line in text.splitlines():
        if "Maps:" in line or "Maps" in line.split(":")[0] or "Guru" in line or "abort" in line or "assert" in line.lower():
            print(line.strip())
    return 0


if __name__ == "__main__":
    sys.exit(main())
