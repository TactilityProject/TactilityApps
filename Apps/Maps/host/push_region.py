# SPDX-License-Identifier: GPL-3.0-or-later
"""Copies a downloaded region to a device through its development server.

Usage: python host/push_region.py <device ip> <local root> <region>
<local root> holds maps/<region>/... as written by fetch_tiles.py. Files go to the app's user data folder.
push creates missing folders on the device. The shell's mkdir is avoided, it overflowed its stack on Tactility 0e276da2.
"""
import os
import subprocess
import sys

TOOLS = os.path.join(os.path.dirname(__file__), "..", "reference", "Tactility", "Buildscripts", "TactilitySDK", "Tools")
DEVICE_ROOT = "/sdcard/tactility/user/app/tactility.maps"


def push(host, local, remote):
    return subprocess.run([sys.executable, os.path.join(TOOLS, "push.py"), "-h", host, local, remote], capture_output=True, text=True)


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        return 2
    host, root, region = sys.argv[1:]
    local_region = os.path.join(root, "maps", region)
    files = []
    for folder, _, names in os.walk(local_region):
        for name in names:
            if not name.endswith(".tmp"):
                files.append(os.path.join(folder, name))
    files.sort()
    for index, path in enumerate(files):
        relative = os.path.relpath(path, root).replace(os.sep, "/")
        remote = f"{DEVICE_ROOT}/{relative}"
        result = push(host, path, remote)
        status = "ok" if result.returncode == 0 else ("FAILED " + (result.stderr or result.stdout).strip())
        print(f"[{index + 1}/{len(files)}] {relative} {status}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
