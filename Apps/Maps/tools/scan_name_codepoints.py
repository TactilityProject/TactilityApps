# SPDX-License-Identifier: GPL-3.0-or-later
"""Scan which characters the map names and the app's interface use, and write the label-font codepoint list.

Usage:
    python tools/scan_name_codepoints.py --tiles <dir> [--tiles <dir> ...] --source main/Source --out tools/label_codepoints.txt

    --tiles   folder walked recursively for *.pbf tiles (repeat for more folders)
    --source  folder walked recursively for *.cpp and *.h; their C/C++ string literals are scanned
    --out     output file: '#' header, then one 'U+XXXX char tag' line per codepoint, sorted

Also scanned: tests/fixtures/gpx/*.gpx waypoint names, when that folder exists (counted as interface text).

Which codepoints are kept:
  - printable ASCII U+0020-U+007E and Latin-1 U+00A0-U+00FF, always (tag "base", or "N names" when names use them);
  - characters of interface string literals and GPX waypoint names, without a block filter (tag "ui" when no name uses them);
  - characters of the name and name:latin tags only inside the blocks the label font draws
    (Latin, Greek, Cyrillic, some punctuation and symbols). Other scripts are dropped and counted per script,
    because the app shows name:latin for them (see main/Source/render/LabelLayout.cpp, labelFontCovers).

Standard library only (Python 3.10). Uses tools/mvt_lite.py to read the tiles.
"""

import argparse
import html
import os
import re
import string
import sys
import unicodedata
from collections import Counter, defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from mvt_lite import read_tile  # noqa: E402

FORCED = [(0x0020, 0x007E), (0x00A0, 0x00FF)]
LABEL_BLOCKS = [
    (0x0100, 0x024F),  # Latin Extended-A and -B
    (0x0250, 0x02FF),  # IPA extensions, spacing modifiers
    (0x0300, 0x036F),  # combining diacritical marks
    (0x0370, 0x03FF),  # Greek
    (0x0400, 0x052F),  # Cyrillic and Cyrillic supplement
    (0x1E00, 0x1EFF),  # Latin Extended Additional
    (0x2000, 0x206F),  # general punctuation
    (0x20A0, 0x20CF),  # currency symbols
    (0x2100, 0x214F),  # letterlike symbols
]

# First match wins. Only used to name the scripts that get dropped from names.
SCRIPTS = [
    (0x0000, 0x001F, "Controls"),
    (0x0080, 0x009F, "Controls"),
    (0x0530, 0x058F, "Armenian"),
    (0x0590, 0x05FF, "Hebrew"),
    (0x0600, 0x06FF, "Arabic"),
    (0x0700, 0x074F, "Syriac"),
    (0x0750, 0x077F, "Arabic"),
    (0x0780, 0x07BF, "Thaana"),
    (0x0900, 0x097F, "Devanagari"),
    (0x0980, 0x0DFF, "Other Indic"),
    (0x0E00, 0x0E7F, "Thai"),
    (0x0E80, 0x0FFF, "Lao/Tibetan/other SE Asian"),
    (0x10A0, 0x10FF, "Georgian"),
    (0x1100, 0x11FF, "Hangul"),
    (0x1200, 0x139F, "Ethiopic"),
    (0x1D00, 0x1DFF, "Latin phonetic ext"),
    (0x1F00, 0x1FFF, "Greek extended"),
    (0x2070, 0x209F, "Super/subscripts"),
    (0x20D0, 0x20FF, "Symbol combining marks"),
    (0x2150, 0x218F, "Number forms"),
    (0x2190, 0x2BFF, "Arrows, math, symbols"),
    (0x2C00, 0x2DFF, "Glagolitic/Coptic/Georgian ext"),
    (0x2E00, 0x2FFF, "CJK radicals, punctuation"),
    (0x3000, 0x303F, "CJK symbols"),
    (0x3040, 0x30FF, "Kana"),
    (0x3100, 0x33FF, "CJK misc"),
    (0x3400, 0x9FFF, "CJK ideographs"),
    (0xA000, 0xA4CF, "Yi"),
    (0xAC00, 0xD7FF, "Hangul"),
    (0xE000, 0xF8FF, "Private use"),
    (0xF900, 0xFAFF, "CJK compatibility"),
    (0xFB00, 0xFB4F, "Hebrew/Arabic presentation"),
    (0xFB50, 0xFEFF, "Arabic presentation"),
    (0xFF00, 0xFFEF, "Fullwidth/halfwidth forms"),
    (0x1F000, 0x1FAFF, "Emoji and symbols"),
    (0x20000, 0x3FFFF, "CJK ext"),
]

SIMPLE_ESCAPES = {"\\": "\\", '"': '"', "'": "'", "?": "?"}
EXAMPLES = 5


def in_ranges(cp, ranges):
    return any(lo <= cp <= hi for lo, hi in ranges)


def script_of(cp):
    for lo, hi, name in SCRIPTS:
        if lo <= cp <= hi:
            return name
    return "Other"


def printable(cp):
    ch = chr(cp)
    cat = unicodedata.category(ch)
    if cat == "Mn" or cat == "Me":
        return "◌" + ch  # dotted circle, so a lone mark is visible
    if cat[0] in "CZ":
        return "."
    return ch


def load_codepoints(path):
    """Reads an output file of this script; returns the codepoints in file order (sorted)."""
    cps = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            if not line.strip() or line.startswith("#"):
                continue
            token = line.split(None, 1)[0]
            if token.startswith("U+"):
                cps.append(int(token[2:], 16))
    return cps


def scan_tiles(folders):
    """Returns (name_freq, tile_count, error_count): name_freq maps each name string to how many features use it."""
    name_freq = Counter()
    tiles = 0
    errors = 0
    for folder in folders:
        if not os.path.isdir(folder):
            print("skipping missing tile folder: %s" % folder)
            continue
        for dirpath, dirnames, filenames in os.walk(folder):
            dirnames.sort()
            for filename in sorted(filenames):
                if not filename.lower().endswith(".pbf"):
                    continue
                tiles += 1
                path = os.path.join(dirpath, filename)
                try:
                    layers = read_tile(path)
                except Exception as exc:  # a broken tile must not stop the scan
                    errors += 1
                    if errors <= 5:
                        print("cannot read %s: %s" % (path, exc))
                    continue
                for layer in layers:
                    for feature in layer.features:
                        for key in ("name", "name:latin"):
                            value = feature.tags.get(key)
                            if isinstance(value, str) and value:
                                name_freq[value] += 1
    return name_freq, tiles, errors


def c_string_literals(text):
    """Yields the decoded contents of the C/C++ string literals in text. Skips comments, char literals and #include lines."""
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
            continue
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        if c == "#":
            line_end = text.find("\n", i)
            if line_end < 0:
                line_end = n
            if text[i:line_end].lstrip("# \t").startswith("include"):
                i = line_end
            else:
                i += 1
            continue
        if c == "'":
            k = i - 1
            while k >= 0 and text[k] in " \t":
                k -= 1
            if k >= 0 and text[k].isdigit():  # digit separator such as 1'000
                i += 1
                continue
            j = i + 1
            while j < n and text[j] not in "'\n":
                j += 2 if text[j] == "\\" else 1
            i = j + 1
            continue
        if c == '"':
            if i > 0 and text[i - 1] == "R":  # raw string R"delim(...)delim"
                paren = text.find("(", i + 1)
                if paren < 0:
                    break
                marker = ")" + text[i + 1:paren] + '"'
                j = text.find(marker, paren + 1)
                if j < 0:
                    j = n
                out.append(text[paren + 1:j])
                i = j + len(marker)
                continue
            buf = []
            j = i + 1
            while j < n:
                ch = text[j]
                if ch == '"' or ch == "\n":
                    break
                if ch == "\\" and j + 1 < n:
                    e = text[j + 1]
                    if e in "uU":
                        width = 4 if e == "u" else 8
                        digits = text[j + 2:j + 2 + width]
                        if len(digits) == width and all(d in string.hexdigits for d in digits):
                            buf.append(chr(int(digits, 16)))
                        j += 2 + width
                        continue
                    if e == "x":
                        k = j + 2
                        while k < n and text[k] in string.hexdigits:
                            k += 1
                        j = k
                        continue
                    buf.append(SIMPLE_ESCAPES.get(e, ""))
                    j += 2
                    continue
                buf.append(ch)
                j += 1
            out.append("".join(buf))
            i = j + 1
            continue
        i += 1
    return out


def scan_source(folder):
    """Returns (codepoints, file_count, literal_count) for the string literals of *.cpp and *.h under folder."""
    cps = set()
    files = 0
    literals = 0
    if not os.path.isdir(folder):
        print("missing source folder: %s" % folder)
        return cps, files, literals
    for dirpath, dirnames, filenames in os.walk(folder):
        dirnames.sort()
        for filename in sorted(filenames):
            if not filename.lower().endswith((".cpp", ".h")):
                continue
            files += 1
            with open(os.path.join(dirpath, filename), encoding="utf-8", errors="replace") as f:
                text = f.read().lstrip("﻿")
            for literal in c_string_literals(text):
                literals += 1
                cps.update(ord(ch) for ch in literal)
    return cps, files, literals


def scan_gpx(folder):
    """Returns (codepoints, file_count, name_count) for the waypoint names in tests/fixtures/gpx/*.gpx."""
    cps = set()
    files = 0
    names = 0
    if not os.path.isdir(folder):
        return cps, files, names
    pattern = re.compile(r"<name>(.*?)</name>", re.S)
    for filename in sorted(os.listdir(folder)):
        if not filename.lower().endswith(".gpx"):
            continue
        files += 1
        with open(os.path.join(folder, filename), encoding="utf-8", errors="replace") as f:
            text = f.read()
        for match in pattern.finditer(text):
            name = html.unescape(match.group(1))
            names += 1
            cps.update(ord(ch) for ch in name)
    return cps, files, names


def main():
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except AttributeError:
        pass

    parser = argparse.ArgumentParser(description="Scan map names and interface strings for label-font codepoints.")
    parser.add_argument("--tiles", action="append", required=True, help="tile folder (repeatable)")
    parser.add_argument("--source", required=True, help="source folder with *.cpp and *.h")
    parser.add_argument("--out", required=True, help="output codepoint file")
    args = parser.parse_args()

    name_freq, tile_count, tile_errors = scan_tiles(args.tiles)
    name_count = len(name_freq)
    feature_names = sum(name_freq.values())

    # Distinct names per codepoint, occurrences per codepoint, and up to EXAMPLES names per codepoint.
    distinct = Counter()
    occurrences = Counter()
    examples = defaultdict(list)
    for name, uses in name_freq.items():
        for ch in name:
            occurrences[ord(ch)] += uses
        for cp in set(ord(ch) for ch in name):
            distinct[cp] += 1
            if len(examples[cp]) < EXAMPLES:
                examples[cp].append(name)

    ui_cps, source_files, literal_count = scan_source(args.source)
    gpx_cps, gpx_files, gpx_names = scan_gpx(os.path.join(ROOT, "tests", "fixtures", "gpx"))
    ui_cps = ui_cps | gpx_cps

    forced_cps = {cp for cp in range(0, 0x10000) if in_ranges(cp, FORCED)}
    name_ok = {cp for cp in distinct if in_ranges(cp, LABEL_BLOCKS)}
    kept = forced_cps | ui_cps | name_ok | {cp for cp in distinct if in_ranges(cp, FORCED)}

    # Characters from names that are neither drawn by the label font nor used by the interface.
    dropped = [cp for cp in distinct if cp not in kept]
    dropped_by_script = defaultdict(lambda: [0, 0])  # script -> [distinct codepoints, occurrences]
    for cp in dropped:
        entry = dropped_by_script[script_of(cp)]
        entry[0] += 1
        entry[1] += occurrences[cp]

    tags = {}
    for cp in kept:
        if distinct[cp] > 0:
            tags[cp] = "%d names" % distinct[cp]
        elif in_ranges(cp, FORCED):
            tags[cp] = "base"
        else:
            tags[cp] = "ui"
    kept_sorted = sorted(kept)

    from_names = sum(1 for cp in kept_sorted if tags[cp].endswith("names"))
    ui_only = sum(1 for cp in kept_sorted if tags[cp] == "ui")
    base_only = sum(1 for cp in kept_sorted if tags[cp] == "base")

    header = [
        "# Label-font codepoints, written by tools/scan_name_codepoints.py. Do not edit by hand.",
        "# Tile folders scanned: %s" % ", ".join(args.tiles),
        "# Tiles: %d (unreadable: %d), features with names: %d, distinct names: %d"
        % (tile_count, tile_errors, feature_names, name_count),
        "# Interface source: %s (%d files, %d string literals), gpx waypoint names: %d"
        % (args.source, source_files, literal_count, gpx_names),
        "# Kept codepoints: %d (from names %d, interface only %d, base only %d)"
        % (len(kept_sorted), from_names, ui_only, base_only),
        "# Columns: U+code, the character, then the distinct names using it, or ui (interface only) or base (ASCII/Latin-1)",
    ]
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        for line in header:
            f.write(line + "\n")
        for cp in kept_sorted:
            f.write("U+%04X %s %s\n" % (cp, printable(cp), tags[cp]))

    print("Tile folders: %s" % ", ".join(args.tiles))
    print("Tiles: %d (unreadable %d), features with names: %d, distinct names: %d"
          % (tile_count, tile_errors, feature_names, name_count))
    print("Interface: %d source files, %d string literals; gpx waypoint names: %d from %d files"
          % (source_files, literal_count, gpx_names, gpx_files))
    print("")
    print("Codepoints kept: %d" % len(kept_sorted))
    print("  used by names: %d" % from_names)
    print("  only from the interface: %d" % ui_only)
    print("  only base (ASCII/Latin-1, no names): %d" % base_only)
    print("Dropped from names (script: distinct codepoints, occurrences):")
    if not dropped_by_script:
        print("  none")
    for script, (count, uses) in sorted(dropped_by_script.items(), key=lambda item: -item[1][1]):
        print("  %-32s %5d chars %9d occurrences" % (script, count, uses))
    print("")
    for title, ranges in (("U+0300-U+036F combining marks", [(0x0300, 0x036F)]),
                          ("U+02B0-U+02FF modifier letters", [(0x02B0, 0x02FF)])):
        print("Kept in %s:" % title)
        found = [cp for cp in kept_sorted if in_ranges(cp, ranges)]
        if not found:
            print("  none")
        for cp in found:
            names = " | ".join(examples[cp]) if examples[cp] else "(no names)"
            print("  U+%04X %s %s: %s" % (cp, printable(cp), tags[cp], names))
    print("")
    print("Wrote %d codepoints to %s" % (len(kept_sorted), args.out))


if __name__ == "__main__":
    main()
