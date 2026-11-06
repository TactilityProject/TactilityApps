# SPDX-License-Identifier: GPL-3.0-or-later
"""Make assets/fonts/LabelSans.ttf, the font the map's place labels are drawn in.

Usage:
    python tools/make_label_font.py [AdwaitaSans-Regular.ttf] [LabelSans.ttf]

The defaults are reference/Tactility/Tactility/Fonts/AdwaitaSans-Regular.ttf and assets/fonts/LabelSans.ttf.
Needs fontTools (pip install --user fonttools).

Tactility's own fonts leave out Latin Extended-A and more, so labels lose letters such as S with caron, l with caron,
L with stroke, S with acute, O with double acute, and all Greek and Cyrillic. The app generates its label fonts from this subset on the device, with
the binfont-module's binfont_generate(). The subset keeps the text characters common on maps: Basic Latin, Latin-1, Latin
Extended-A/-B and Additional, schwa, spacing accents, combining marks, Greek, Cyrillic and some punctuation. The generator reads no kerning (only the glyph outlines, advances and the
character map), so hinting and all layout features are dropped to keep the file small. The name table is kept and gets
a copyright and a SIL Open Font License entry, which the source lacks.

Adwaita Sans: GNOME adwaita-fonts (https://github.com/GNOME/adwaita-fonts), SIL Open Font License 1.1.
"""

import os
import sys

from fontTools import subset
from fontTools.varLib import instancer
from fontTools.ttLib import TTFont

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
DEFAULT_SOURCE = os.path.join(ROOT, "reference", "Tactility", "Tactility", "Fonts", "AdwaitaSans-Regular.ttf")
DEFAULT_OUTPUT = os.path.join(ROOT, "assets", "fonts", "LabelSans.ttf")

RANGES = [
    (0x0020, 0x007E),  # Basic Latin
    (0x00A0, 0x024F),  # Latin-1 Supplement, Latin Extended-A and -B
    (0x0259, 0x0259),  # schwa (Azerbaijani)
    (0x02BB, 0x02BC),  # modifier letters turned comma and apostrophe
    (0x02C6, 0x02DD),  # modifier letters, spacing accents
    (0x0300, 0x036F),  # combining marks
    (0x0370, 0x03FF),  # Greek and Coptic
    (0x0400, 0x052F),  # Cyrillic and Cyrillic Supplement
    (0x1E00, 0x1EFF),  # Latin Extended Additional (Vietnamese)
    (0x2010, 0x2027),  # dashes, quotes, bullet, ellipsis
    (0x2030, 0x203A),  # per mille, primes, guillemets
    (0x20AC, 0x20AC),  # euro
    (0x2116, 0x2116),  # numero sign
]


def main(argv):
    source = argv[1] if len(argv) > 1 else DEFAULT_SOURCE
    output = argv[2] if len(argv) > 2 else DEFAULT_OUTPUT
    codepoints = [c for first, last in RANGES for c in range(first, last + 1)]

    options = subset.Options()
    options.layout_features = []  # no GSUB/GPOS features
    options.hinting = False
    options.notdef_outline = True
    options.name_IDs = ["*"]  # copyright, licence and the rest of the name table
    options.name_legacy = True
    options.name_languages = ["*"]
    options.glyph_names = False
    options.drop_tables += ["kern", "GDEF", "GPOS", "GSUB", "DSIG", "STAT", "gasp", "prep", "fpgm", "cvt ", "hdmx", "LTSH", "VDMX"]
    options.legacy_kern = False

    font = TTFont(source)
    if "fvar" in font:
        # The generator reads the default glyph outlines only: pin the variable font to the weight Tactility's own text font has (550)
        font = instancer.instantiateVariableFont(font, {"opsz": 14, "wght": 550}, inplace=False)
    subsetter = subset.Subsetter(options)
    subsetter.populate(unicodes=codepoints)
    subsetter.subset(font)

    # The source's name table has no copyright or licence entries: say whose font this is and under what licence
    names = font["name"]
    names.setName("Adwaita Sans (GNOME adwaita-fonts, derived from Inter by the Inter Project Authors), subset for Tactility Maps", 0, 3, 1, 0x409)
    names.setName("This Font Software is licensed under the SIL Open Font License, Version 1.1.", 13, 3, 1, 0x409)
    names.setName("https://openfontlicense.org", 14, 3, 1, 0x409)

    present = set(font.getBestCmap().keys())
    missing = [c for c in codepoints if c not in present]
    if missing:
        print("Not in the source font: " + " ".join("U+%04X" % c for c in missing))

    os.makedirs(os.path.dirname(os.path.abspath(output)), exist_ok=True)
    font.save(output)
    print("%s: %d bytes, %d codepoints" % (output, os.path.getsize(output), len(present)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
