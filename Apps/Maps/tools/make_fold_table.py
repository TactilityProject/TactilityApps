# SPDX-License-Identifier: GPL-3.0-or-later
"""Writes main/Source/search/FoldTable.h: the character tables of fold.py as C++ arrays, made with Python's own
unicodedata so the device folds exactly as the study did.

Usage: python tools/make_fold_table.py
Tables (each sorted by code point):
  FOLD_LATIN: code point -> folded UTF-8 text, for every letter or digit up to U+24F and U+1E00..U+1EFF whose
              fold_latin() result differs from itself (lowercasing, marks, SPECIAL letters), and for Greek and
              Cyrillic capitals (lowercased; fold_latin keeps those scripts)
  FOLD_ORIGINAL: code point -> text for Greek and Cyrillic in fold_orig() (lowercase, tonos removed, final sigma,
              ё -> е, ґ -> г)
  TRANSLIT_RU / _UK / _SR: lowercase Greek and Cyrillic letter -> Latin text, as translit()
"""
import os
import sys
import unicodedata

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import fold  # noqa: E402

OUT = os.path.join(HERE, "..", "main", "Source", "search", "FoldTable.h")
RANGES = [(0x41, 0x5A), (0xC0, 0xABFF), (0xD7A4, 0xFFFF)]


def c_string(text):
    return '"' + "".join("\\x%02X" % b for b in text.encode("utf-8")) + '"'


def table(name, pairs):
    lines = [f"constexpr FoldEntry {name}[] = {{"]
    for cp, text in pairs:
        lines.append(f"    {{ 0x{cp:04X}, {c_string(text)} }},")
    lines.append("};")
    return "\n".join(lines)


def main():
    latin = []
    original = []
    for start, end in RANGES:
        for cp in range(start, end + 1):
            ch = chr(cp)
            if not ch.isalnum():
                continue
            # fold_latin works on lowercased text; a single letter's lowercase may be 1 char
            folded = fold.fold_latin(ch)
            if folded != ch and len(folded) <= 3:
                latin.append((cp, folded))
            if 0x370 <= cp <= 0x52F:
                o = fold.fold_orig(ch)
                if o != ch and len(o) <= 2:
                    original.append((cp, o))
    # SPECIAL letters that are not alphanumeric by Python's rules are still letters there
    for ch, text in fold.SPECIAL.items():
        if ord(ch) not in {cp for cp, _ in latin}:
            latin.append((ord(ch), text))
    latin.sort()
    original.sort()
    # Plain lowercasing for fold_orig of Latin letters (marks kept there)
    lower = []
    for start, end in RANGES:
        for cp in range(start, end + 1):
            ch = chr(cp)
            low = ch.lower()
            if low != ch and len(low) == 1:
                lower.append((cp, low))
            elif len(low) == 2 and unicodedata.category(low[1]) == "Mn":
                # İ -> i + combining dot; the folds drop the dot
                lower.append((cp, low[0]))
    lower.sort()
    # Code points U+0080..U+FFFF that are neither letters nor digits for Python (isalnum) and no combining marks:
    # the folds turn them into spaces. Stored as ranges.
    separators = []
    start = None
    for cp in range(0x80, 0x10000):
        ch = chr(cp)
        is_sep = not ch.isalnum() and unicodedata.category(ch) != "Mn"
        if is_sep and start is None:
            start = cp
        elif not is_sep and start is not None:
            separators.append((start, cp - 1))
            start = None
    if start is not None:
        separators.append((start, 0xFFFF))
    translit = {}
    for profile in ("ru", "uk", "sr"):
        rows = []
        letters = set(fold.GREEK) | set(fold.PROFILES[profile])
        table_ = fold.PROFILES[profile]
        for ch in sorted(letters):
            # The letter's own value, not its word-start form (ї at a word start is handled in code)
            raw = fold.GREEK.get(ch) if ch in fold.GREEK else table_.get(ch, "")
            rows.append((ord(ch), fold.fold_latin(raw) if raw else ""))
        translit[profile] = sorted(rows)
    parts = [
        "// SPDX-License-Identifier: GPL-3.0-or-later",
        "// Made by tools/make_fold_table.py from fold.py and Python's unicodedata " + unicodedata.unidata_version + "; don't edit.",
        "#pragma once",
        "",
        "#include <cstdint>",
        "",
        "namespace maps {",
        "",
        "struct FoldEntry {",
        "    uint32_t codePoint;",
        "    const char* text;",
        "};",
        "",
        table("FOLD_LATIN", latin),
        "",
        table("FOLD_ORIGINAL", original),
        "",
        table("FOLD_LOWER", lower),
        "",
        table("TRANSLIT_RU", translit["ru"]),
        "",
        table("TRANSLIT_UK", translit["uk"]),
        "",
        table("TRANSLIT_SR", translit["sr"]),
        "",
        "/** Ranges of U+0080..U+FFFF that fold to a space (not letters, digits or combining marks), sorted. */",
        "constexpr uint32_t FOLD_SEPARATORS[][2] = {",
        *[f"    {{ 0x{a:04X}, 0x{b:04X} }}," for a, b in separators],
        "};",
        "",
        "/** Apostrophes, dropped by both folds. */",
        "constexpr uint32_t FOLD_APOSTROPHES[] = { " + ", ".join("0x%04X" % ord(c) for c in sorted(fold.APOS)) + " };",
        "",
        "} // namespace maps",
        "",
    ]
    with open(OUT, "w", encoding="utf-8", newline="\n") as out:
        out.write("\n".join(parts))
    print(f"{OUT}: separators {len(separators)} ranges, latin {len(latin)}, original {len(original)}, translit {[len(v) for v in translit.values()]}")


if __name__ == "__main__":
    main()
