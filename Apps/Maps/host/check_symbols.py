# SPDX-License-Identifier: GPL-3.0-or-later
"""Lists symbols that an app ELF needs but Tactility does not export to apps.

Usage: python check_symbols.py <app.elf> <Tactility repo> <readelf.exe>
Tactility exports app symbols with DEFINE_MODULE_SYMBOL(...) in Modules/*, TactilityKernel and Tactility.
"""
import os
import re
import subprocess
import sys


OTHER_TARGET = "CONFIG_IDF_TARGET_ESP32P4"


def without_other_targets(text):
    """Drops the lines that are only compiled for the ESP32-P4, e.g. the 64-bit atomics in platform-esp32."""
    kept = []
    stack = []  # per open #if: True when its current branch is P4-only, False when not, None when unrelated
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith("#if"):
            if OTHER_TARGET in stripped and not stripped.startswith("#ifndef") and "!" not in stripped:
                stack.append(True)
            elif stripped.startswith("#ifndef") and OTHER_TARGET in stripped:
                stack.append(False)
            else:
                stack.append(None)
        elif stripped.startswith("#else") and stack:
            if stack[-1] is not None:
                stack[-1] = not stack[-1]
        elif stripped.startswith("#endif") and stack:
            stack.pop()
        elif True not in stack:
            kept.append(line)
    return "\n".join(kept)


def exported_symbols(repo):
    names = set()
    pattern = re.compile(r"(?:DEFINE_MODULE_SYMBOL\w*|ESP_ELFSYM_EXPORT|DEFINE_SYMBOL)\(\s*(\w+)\s*[,)]")
    alias = re.compile(r'\{\s*"(\w+)"\s*,')
    for root, dirs, files in os.walk(repo):
        parts = os.path.relpath(root, repo).split(os.sep)
        if any(part.startswith(("build", "managed_components", ".git", "release")) for part in parts):
            continue
        for file in files:
            if not file.endswith((".c", ".cpp", ".h")):
                continue
            path = os.path.join(root, file)
            try:
                text = open(path, encoding="utf-8", errors="ignore").read()
            except OSError:
                continue
            if "DEFINE_MODULE_SYMBOL" not in text and "ESP_ELFSYM" not in text and "ModuleSymbol" not in text:
                continue
            text = without_other_targets(text)
            names.update(pattern.findall(text))
            if "ModuleSymbol" in text:
                names.update(alias.findall(text))
    return names


def needed_symbols(elf, readelf):
    output = subprocess.run([readelf, "--dyn-syms", "-W", elf], capture_output=True, text=True, check=True).stdout
    names = set()
    for line in output.splitlines():
        parts = line.split()
        if len(parts) >= 8 and parts[6] == "UND" and parts[7]:
            names.add(parts[7].split("@")[0])
    return names


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        return 2
    exported = exported_symbols(sys.argv[2])
    needed = needed_symbols(sys.argv[1], sys.argv[3])
    missing = sorted(n for n in needed if n not in exported)
    print(f"needed: {len(needed)}, exported by Tactility: {len(exported)}, missing: {len(missing)}")
    for name in missing:
        print("  MISSING", name)
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
