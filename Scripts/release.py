import json
import subprocess
import tarfile
import os
import tempfile
import sys
from datetime import datetime, UTC

def read_properties_file(path):
    properties = {}
    with open(path, "r") as file:
        for line in file:
            stripped = line.strip()
            if not stripped or stripped.startswith("#"):
                continue
            key, sep, value = line.partition("=")
            if not sep:
                continue
            properties[key.strip()] = value.strip()
    return properties

def get_manifest(appPath):
    """Extract only the file named 'manifest.properties' from the given tar/tar.gz
    archive into the directory specified by outputPath.

    Returns True on success, False on failure.
    """
    print(f"Unpacking manifest.properties from {appPath}")
    if not os.path.isfile(appPath):
        print(f"File not found: {appPath}")
        return False

    try:
        # Auto-detect compression (tar, tar.gz, tar.bz2, etc.)
        with tarfile.open(appPath, mode="r:*") as tf:
            # Find a member whose base name is exactly 'manifest.properties'
            member = None
            for m in tf.getmembers():
                # Skip directories and non-files
                if not m.isfile():
                    continue
                if os.path.basename(m.name) == "manifest.properties":
                    member = m
                    break

            if member is None:
                print("manifest.properties not found in archive")
                return None

            # Read the member's content and write it directly into outputPath
            # Create a temporary directory that persists after this function returns.
            temp_dir = tempfile.mkdtemp(prefix="tactility_app_")
            dest_file = os.path.join(temp_dir, "manifest.properties")
            with tf.extractfile(member) as src, open(dest_file, "wb") as dst:
                if src is None:
                    print("Failed to open manifest.properties from the archive")
                    return False
                while True:
                    chunk = src.read(1024 * 1024)
                    if not chunk:
                        break
                    dst.write(chunk)
            properties = read_properties_file(dest_file)
            os.remove(dest_file)
            os.rmdir(temp_dir)
            return properties
    except tarfile.ReadError as e:
        print(f"Not a valid tar archive: {appPath} ({e})")
    except Exception as e:
        print(f"Failed to extract manifest.properties from {appPath}: {e}")
    return None

def get_icons(appPath):
    """Read icon.svg and icon_mono.svg from the package. Returns a dict of the ones that exist."""
    icons = {}
    with tarfile.open(appPath, mode="r:*") as tf:
        for m in tf.getmembers():
            if m.isfile() and m.name.lstrip("./") in ("icon.svg", "icon_mono.svg"):
                icons[os.path.basename(m.name)] = tf.extractfile(m).read()
    return icons

def get_versioned_file_name(manifest):
    app_id = manifest["id"]
    version_code = manifest["version.code"]
    return f"{app_id}-{version_code}.app"

def get_os_version(manifest):
    sdk = manifest["target.sdk"]
    # Remove trailing hyphen suffix if present
    if "-" in sdk:
        return sdk.rsplit("-", 1)[0].strip()
    else:
        return sdk

def check_and_get_sdk_version(manifest_map):
    """Ensure all apps target the same (simplified) SDK version and return it."""
    versions = {get_os_version(manifest) for manifest in manifest_map.values()}
    if len(versions) != 1:
        print(f"ERROR: Apps target multiple SDK versions: {sorted(versions)}. All apps must target the same SDK version.")
        sys.exit(1)
    return next(iter(versions))

def get_git_commit_hash():
    return subprocess.check_output(['git', 'rev-parse', 'HEAD']).decode('ascii').strip()

SHORT_DESCRIPTION_MAX_LENGTH = 32

def get_requires_ram(manifest):
    """Parse "requires.ram" (whole megabytes with an "M" suffix, e.g. "2M") into megabytes. Absent means 0."""
    value = manifest.get("requires.ram")
    if value is None:
        return 0
    value = value.strip()
    if len(value) < 2 or not value.endswith("M") or not value[:-1].isdigit() or int(value[:-1]) > 255:
        print(f"ERROR: Invalid requires.ram \"{value}\" in {manifest.get('id', '?')}: expected whole megabytes with an M suffix (e.g. 2M), at most 255M")
        sys.exit(1)
    return int(value[:-1])

def manifest_config_to_flat_json(manifest):
    """Convert a flat (V2) manifest dict into a flat JSON-like dict.

    Expected keys:
        id -> appId
        version.name -> appVersionName
        version.code -> appVersionCode (int)
        name -> appName
        description -> appDescription (optional; default "")
        target.sdk -> targetSdk
        target.platforms -> targetPlatforms (comma-separated list)
        requires.device.id -> requiresDeviceId (comma-separated list, optional; default [])
        requires.ram -> requiresRam (megabytes, optional; default 0)
        description.short -> shortDescription (required)

    Unknown/missing values fall back to sensible defaults per requirements.
    """
    # Map values
    app_id = manifest.get("id", "")
    app_version_name = manifest.get("version.name", "")
    app_version_code_raw = manifest.get("version.code", "0")
    app_name = manifest.get("name", "")
    app_description = manifest.get("description", "") or ""
    short_description = manifest.get("description.short", "")
    if not short_description:
        print(f"ERROR: Missing description.short in {app_id}")
        sys.exit(1)
    if len(short_description) > SHORT_DESCRIPTION_MAX_LENGTH:
        print(f"ERROR: description.short of {app_id} is {len(short_description)} characters, at most {SHORT_DESCRIPTION_MAX_LENGTH} are allowed")
        sys.exit(1)

    # Coerce version code to int safely
    try:
        app_version_code = int(str(app_version_code_raw).strip())
    except Exception:
        app_version_code = 0

    target_sdk = manifest.get("target.sdk", "")
    platforms_raw = manifest.get("target.platforms", "")
    target_platforms = [p.strip() for p in str(platforms_raw).split(",") if p.strip()] if platforms_raw is not None else []
    # An empty list means every platform
    if target_platforms == ["all"]:
        target_platforms = []

    requires_device_id_raw = manifest.get("requires.device.id", "")
    requires_device_id = [d.strip() for d in str(requires_device_id_raw).split(",") if d.strip()]
    requires_ram = get_requires_ram(manifest)

    filename = get_versioned_file_name(manifest)
    return {
        "appId": app_id,
        "appVersionName": app_version_name,
        "appVersionCode": app_version_code,
        "appName": app_name,
        "appDescription": app_description,
        "shortDescription": short_description,
        "targetSdk": target_sdk,
        "targetPlatforms": target_platforms,
        "requiresDeviceId": requires_device_id,
        "requiresRam": requires_ram,
        "file": filename
    }

PAGE_SIZE = 20
SIMULATOR_PLATFORMS = ["linux-x86_64", "linux-aarch64", "macos-arm64", "macos-x86_64"]

def get_list_file_name(page, arch=None):
    """All apps: index.json, list_2.json, list_3.json and so on. One architecture: list-[arch]-1.json, list-[arch]-2.json and so on"""
    if arch is not None:
        return f"list-{arch}-{page}.json"
    return "index.json" if page == 1 else f"list_{page}.json"

def get_details_file_name(app_id):
    return f"{app_id}.json"

def get_icon_file_name(app_id, version_code, mono):
    """The version code is in the name, so a new version of an app has a new icon file, which a cache can't confuse with the old one"""
    return f"{app_id}-{version_code}-icon-mono.svg" if mono else f"{app_id}-{version_code}-icon.svg"

def write_list_pages(app_directory, header, app_jsons, apps_without_icon, arch=None):
    """Write the pages of a list. Items only have what a list needs, with short keys to keep it small.
    A page has no links: its number is page and the last one is pageCount, and the file names follow from those.
    With an arch, the list has the apps that support it. Apps without target platforms support every one.
    An app has "icon": false when it has no icon. Without the key, an app is assumed to have one."""
    items = []
    for app in app_jsons:
        if arch is not None and app["targetPlatforms"] and arch not in app["targetPlatforms"]:
            continue
        item = {
            "id": app["appId"],
            "name": app["appName"],
            "version": app["appVersionCode"],
            "description": app["shortDescription"],
        }
        if app["appId"] in apps_without_icon:
            item["icon"] = False
        items.append(item)
    items.sort(key=lambda item: item["name"].lower())
    page_count = max(1, -(-len(items) // PAGE_SIZE))
    for page in range(1, page_count + 1):
        page_items = items[(page - 1) * PAGE_SIZE:page * PAGE_SIZE]
        write_json(os.path.join(app_directory, get_list_file_name(page, arch)), {
            **header,
            **({"arch": arch} if arch is not None else {}),
            "page": page,
            "pageCount": page_count,
            "appCount": len(items),
            "apps": page_items,
        })

def write_details(app_directory, app_json, icons):
    """Write [appid].json with the full manifest data, and the icons next to it"""
    app_id = app_json["appId"]
    for mono, name in ((False, "icon.svg"), (True, "icon_mono.svg")):
        if name in icons:
            with open(os.path.join(app_directory, get_icon_file_name(app_id, app_json["appVersionCode"], mono)), "wb") as f:
                f.write(icons[name])
    write_json(os.path.join(app_directory, get_details_file_name(app_id)), app_json)

def write_json(filepath, data):
    text = json.dumps(data)
    with open(filepath, 'w') as f:
        f.write(text)

if __name__ == "__main__":
    print("Tactility app release script")
    if len(sys.argv) != 2:
        print("Missing argument")
        sys.exit()
    app_directory = sys.argv[1]
    manifest_map = {}
    any_manifest = None
    if os.path.exists(app_directory):
        for file in os.listdir(app_directory):
            if file.endswith(".app"):
                file_path = os.path.join(app_directory, file)
                manifest_map[file_path] = get_manifest(file_path)
    # All bundled apps must target the same SDK version; this becomes the CDN path segment
    sdk_version = check_and_get_sdk_version(manifest_map)
    with open("sdk_version.txt", "w") as f:
        f.write(sdk_version)
    print(f"SDK version: {sdk_version}")
    output_json = {
        "sdkVersion": sdk_version,
        "created": datetime.now(UTC).strftime('%Y-%m-%dT%H:%M:%SZ'),
        "gitCommit": get_git_commit_hash(),
        "apps": []
    }
    apps_without_icon = set()
    # Rename files and collect manifest data into output json object
    for file_path in manifest_map.keys():
        print(f"Processing {file_path}: {manifest_map[file_path]}")
        manifest = manifest_map[file_path]
        manifest_json = manifest_config_to_flat_json(manifest)
        output_json["apps"].append(manifest_json)
        icons = get_icons(file_path)
        if not icons:
            apps_without_icon.add(manifest_json["appId"])
        write_details(app_directory, manifest_json, icons)
        release_file_name = get_versioned_file_name(manifest)
        renamed_file_path = os.path.join(app_directory, release_file_name)
        os.rename(file_path, renamed_file_path)
        print(f"Renamed {file_path} to {renamed_file_path}")
        any_manifest = manifest
    # Write JSON
    output_json_path = os.path.join(app_directory, "apps.json")
    write_json(output_json_path, {**output_json, "apps": [{**{k: v for k, v in app.items() if k != "shortDescription"}, "appDescription": app["shortDescription"]} for app in output_json["apps"]]})
    list_header = {key: value for key, value in output_json.items() if key != "apps"}
    write_list_pages(app_directory, list_header, output_json["apps"], apps_without_icon)
    architectures = sorted({platform for app in output_json["apps"] for platform in app["targetPlatforms"]} | set(SIMULATOR_PLATFORMS))
    for arch in architectures:
        write_list_pages(app_directory, list_header, output_json["apps"], apps_without_icon, arch)
