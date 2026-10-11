#!/usr/bin/env python3
"""Check that apps changed on the current branch have bumped version code and version name.

Diff the current branch against main: for each app directory under Apps/ that has
changes, verify that both `app.version.code` and `app.version.name` in its
manifest.properties differ from main.

Exit code:
  0  – all changed apps have bumped versions (or no apps changed)
  1  – one or more apps are missing version bumps
"""

import subprocess
import sys
from pathlib import Path

APPS_DIR = "Apps"
MANIFEST = "manifest.properties"
VERSION_CODE_KEY = "app.version.code"
VERSION_NAME_KEY = "app.version.name"


def run(cmd: list[str], **kwargs) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, capture_output=True, text=True, check=True, **kwargs)


def parse_properties(text: str) -> dict[str, str]:
    props: dict[str, str] = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if "=" in line:
            key, _, value = line.partition("=")
            props[key.strip()] = value.strip()
    return props


def get_changed_apps(base_ref: str) -> set[str]:
    """Return set of app names that have file changes vs base_ref."""
    result = run(["git", "diff", base_ref, "--name-only", "--", APPS_DIR])
    apps: set[str] = set()
    for line in result.stdout.splitlines():
        parts = line.split("/")
        if len(parts) >= 2 and parts[0] == APPS_DIR:
            apps.add(parts[1])
    return apps


def get_file_at_ref(ref: str, path: str) -> str | None:
    """Return file contents at a git ref, or None if it doesn't exist."""
    try:
        result = run(["git", "show", f"{ref}:{path}"])
        return result.stdout
    except subprocess.CalledProcessError:
        return None


def main() -> int:
    base_ref = "main"

    # Make sure base ref exists
    try:
        run(["git", "rev-parse", "--verify", base_ref])
    except subprocess.CalledProcessError:
        print(f"Error: cannot resolve ref '{base_ref}'", file=sys.stderr)
        return 1

    changed_apps = get_changed_apps(base_ref)
    if not changed_apps:
        print("No app changes detected.")
        return 0

    print(f"Changed apps: {', '.join(sorted(changed_apps))}\n")

    failures: list[str] = []

    for app_name in sorted(changed_apps):
        manifest_path = f"{APPS_DIR}/{app_name}/{MANIFEST}"

        base_content = get_file_at_ref(base_ref, manifest_path)
        head_content = get_file_at_ref("HEAD", manifest_path)

        if head_content is None:
            failures.append(f"{app_name}: manifest.properties missing on HEAD")
            continue

        if base_content is None:
            # New app – no check needed
            print(f"  {app_name}: new app (no base manifest) – OK")
            continue

        base_props = parse_properties(base_content)
        head_props = parse_properties(head_content)

        base_code = base_props.get(VERSION_CODE_KEY, "")
        head_code = head_props.get(VERSION_CODE_KEY, "")
        base_name = base_props.get(VERSION_NAME_KEY, "")
        head_name = head_props.get(VERSION_NAME_KEY, "")

        code_bumped = base_code != head_code
        name_bumped = base_name != head_name

        if code_bumped and name_bumped:
            print(f"  {app_name}: version bumped ({base_name}→{head_name}, {base_code}→{head_code}) – OK")
        else:
            problems: list[str] = []
            if not code_bumped:
                problems.append(f"version.code unchanged ({head_code})")
            if not name_bumped:
                problems.append(f"version.name unchanged ({head_name})")
            failures.append(f"{app_name}: {', '.join(problems)}")

    print()

    if failures:
        print("FAIL – missing version bump:")
        for f in failures:
            print(f"  ✗ {f}")
        return 1

    print("PASS – all changed apps have bumped versions.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
