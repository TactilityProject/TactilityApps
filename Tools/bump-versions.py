#!/usr/bin/env python3
"""Bump app.version.name and app.version.code in all app manifests.

Usage:
    python Tools/bump-versions.py major   # 0.10.0 -> 1.0.0
    python Tools/bump-versions.py minor   # 0.10.0 -> 0.11.0
    python Tools/bump-versions.py patch   # 0.10.0 -> 0.10.1

Regardless of the argument, app.version.code is incremented by 1.
"""

import sys
from pathlib import Path

APPS_DIR = Path(__file__).resolve().parent.parent / "Apps"
MANIFEST = "manifest.properties"
VERSION_NAME_KEY = "app.version.name"
VERSION_CODE_KEY = "app.version.code"


def parse_version(name: str) -> list[int]:
    parts = name.strip().split(".")
    return [int(p) for p in parts]


def bump_version(name: str, component: str) -> str:
    parts = parse_version(name)
    while len(parts) < 3:
        parts.append(0)

    if component == "major":
        parts[0] += 1
        parts[1] = 0
        parts[2] = 0
    elif component == "minor":
        parts[1] += 1
        parts[2] = 0
    elif component == "patch":
        parts[2] += 1

    return ".".join(str(p) for p in parts)


def process_manifest(path: Path, component: str) -> None:
    lines = path.read_text().splitlines(keepends=True)
    changed = False
    new_lines = []

    for line in lines:
        stripped = line.strip()
        if stripped.startswith(VERSION_NAME_KEY + "="):
            old_value = stripped.split("=", 1)[1].strip()
            new_value = bump_version(old_value, component)
            new_lines.append(line.replace(old_value, new_value))
            changed = True
        elif stripped.startswith(VERSION_CODE_KEY + "="):
            old_code = int(stripped.split("=", 1)[1].strip())
            new_code = old_code + 1
            new_lines.append(line.replace(str(old_code), str(new_code), 1))
            changed = True
        else:
            new_lines.append(line)

    if changed:
        path.write_text("".join(new_lines))
        print(f"  {path.parent.name}: bumped")


def main() -> int:
    if len(sys.argv) != 2 or sys.argv[1] not in ("major", "minor", "patch"):
        print(f"Usage: {sys.argv[0]} major|minor|patch", file=sys.stderr)
        return 1

    component = sys.argv[1]

    manifests = sorted(APPS_DIR.glob(f"*/{MANIFEST}"))
    if not manifests:
        print(f"No manifests found in {APPS_DIR}", file=sys.stderr)
        return 1

    print(f"Bumping {component} (+ version.code) in {len(manifests)} apps:\n")
    for manifest in manifests:
        process_manifest(manifest, component)

    print("\nDone.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
