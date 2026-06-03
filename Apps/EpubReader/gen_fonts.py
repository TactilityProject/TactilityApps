"""
gen_fonts.py - regenerate all LVGL binary font files for EpubReader.

Prerequisites:
    node / npm must be on PATH
    npm install -g lv_font_conv

Usage (from any directory):
    python Apps/EpubReader/gen_fonts.py

Output: .bin files written to Apps/EpubReader/assets/

Output font filenames are font-family-agnostic: font_<size>pt_<variant>.bin
The app loader expects exactly these names - swap in any 4-weight typeface by
editing the FONTS dict and placing the .ttf files in Apps/EpubReader/fonts/.

Default: Noto Serif (4 weights)  https://fonts.google.com/noto/specimen/Noto+Serif
  NotoSerif-Medium.ttf        → regular
  NotoSerif-MediumItalic.ttf  → italic
  NotoSerif-Bold.ttf          → bold
  NotoSerif-BoldItalic.ttf    → bold italic
"""

import shutil
import subprocess
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).parent

# ── Latin content fonts (4 weights) ───────────────────────────────────────────
FONTS = {
    "regular":     SCRIPT_DIR / "fonts" / "NotoSerif-Medium.ttf",
    "italic":      SCRIPT_DIR / "fonts" / "NotoSerif-MediumItalic.ttf",
    "bold":        SCRIPT_DIR / "fonts" / "NotoSerif-Bold.ttf",
    "bold_italic": SCRIPT_DIR / "fonts" / "NotoSerif-BoldItalic.ttf",
}

# ── Unicode ranges ─────────────────────────────────────────────────────────────

# Unicode coverage - adjust if your chosen font supports different blocks:
LATIN_TEXT_RANGES = [
    "-r", "0x20-0x7E",        # Basic Latin
    "-r", "0xA0-0x024F",      # Latin-1 Supplement + Latin Extended-A/B
    "-r", "0x0370-0x03FF",    # Greek (π, Δ, Σ, Ω etc.)
    "-r", "0x1E9E",           # ẞ (capital sharp-s, German)
    "-r", "0x2010-0x2026",    # General Punctuation (hyphens, dashes, ellipsis)
    "-r", "0x2070-0x209F",    # Superscripts and Subscripts (⁰⁴⁵⁶⁷⁸⁹ ₀₁₂₃₄₅₆₇₈₉)
    "-r", "0x20A0-0x20CF",    # Currency Symbols (€ ₹ ₽ ₩ ₪ ₴ etc.)
    "-r", "0x2100-0x214F",    # Letterlike Symbols (™ ℃ ℉ etc.)
    "-r", "0x2150-0x218F",    # Number Forms (⅓ ⅔ ⅛ ⅜ ⅝ ⅞ etc.)
]

# ── Sizes to generate ──────────────────────────────────────────────────────────
SIZES = [16, 20, 28, 36]

# ── Rendering options ──────────────────────────────────────────────────────────
BPP = 2          # 2-bit anti-aliasing; use 4 for smoother rendering (~2× larger)
FORMAT = "bin"   # "lvgl" = C struct output; "bin" = binary blob
COMPRESS = False # True = enable RLE compression (smaller binary, slightly slower)

# ── Output directory ───────────────────────────────────────────────────────────
OUT_DIR = SCRIPT_DIR / "assets"
OUT_DIR.mkdir(parents=True, exist_ok=True)

# ── Locate lv_font_conv (handles Windows .cmd npm wrappers) ───────────────────
def _find_tool() -> str:
    for name in ("lv_font_conv", "lv_font_conv.cmd"):
        found = shutil.which(name)
        if found:
            return found
    print("ERROR: lv_font_conv not found. Install with: npm install -g lv_font_conv", file=sys.stderr)
    sys.exit(1)

LV_FONT_CONV = _find_tool()
USE_SHELL = sys.platform == "win32"

def _run(cmd: list) -> bool:
    if USE_SHELL:
        result = subprocess.run(subprocess.list2cmdline(cmd), capture_output=True, text=True, shell=True)
    else:
        result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"  ERROR: {result.stderr.strip()}", file=sys.stderr)
        return False
    return True


def generate_font(font_path: Path, ranges: list, size: int, symbol: str) -> bool:
    """Convert a single .ttf to an LVGL binary font file."""
    if not font_path.exists():
        print(f"  SKIP  {symbol}  (font not found: {font_path.name})")
        return False

    out = OUT_DIR / f"{symbol}.bin"
    cmd = [LV_FONT_CONV,
           "--font", str(font_path)] + ranges + [
        "--size",         str(size),
        "--bpp",          str(BPP),
        "--format",       FORMAT,
        "--lv-font-name", symbol,
        "-o",             str(out),
    ]
    if not COMPRESS:
        cmd.append("--no-compress")

    print(f"  {symbol:<36}  size={size}  ← {font_path.name}")
    return _run(cmd)


def main():
    print(f"lv_font_conv  →  {OUT_DIR}\n")
    generated = 0
    skipped = 0

    # ── Content font variants (Latin text + symbols) ───────────────────────────
    for size in SIZES:
        print(f"── {size}pt {'─' * 52}")
        for font_key in FONTS.keys():
            symbol = f"font_{size}pt_{font_key}"
            ok = generate_font(FONTS[font_key], LATIN_TEXT_RANGES, size, symbol)
            if ok: generated += 1
            else:  skipped  += 1

    print(f"\nDone.  {generated} generated, {skipped} skipped.")


if __name__ == "__main__":
    main()
