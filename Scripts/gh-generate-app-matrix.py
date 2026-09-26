#!/usr/bin/env python3
"""Discover app directories under Apps/ and emit a GitHub Actions matrix.

Used by .github/workflows/main.yml's GenerateAppMatrix job so new apps are
picked up automatically instead of needing to be added to a hardcoded list.
"""

import json
import os
import sys
from pathlib import Path

APPS_DIR = Path(__file__).resolve().parent.parent / "Apps"
MANIFEST = "manifest.properties"


def get_app_names():
    names = []
    for entry in sorted(os.listdir(APPS_DIR)):
        app_dir = APPS_DIR / entry
        if app_dir.is_dir() and (app_dir / MANIFEST).is_file():
            names.append(entry)
    return names


def main():
    matrix_json = json.dumps({"app_name": get_app_names()})

    github_output = os.environ.get("GITHUB_OUTPUT")
    if github_output:
        with open(github_output, "a") as file:
            file.write(f"matrix={matrix_json}\n")
    else:
        print(matrix_json)


if __name__ == "__main__":
    main()
