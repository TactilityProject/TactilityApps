# Third-party code

| Component | Where | Licence | Notes |
|---|---|---|---|
| Adwaita Sans (GNOME [adwaita-fonts](https://github.com/GNOME/adwaita-fonts), derived from Inter by the Inter Project Authors) | `assets/fonts/LabelSans.ttf` | SIL Open Font License 1.1, https://raw.githubusercontent.com/GNOME/adwaita-fonts/refs/heads/main/LICENSE | A subset (U+0020-007E, U+00A0-024F Latin-1 and Latin Extended-A/-B, U+0259, U+02BB-02BC, U+02C6-02DD, U+0300-036F combining marks, U+0370-03FF Greek, U+0400-052F Cyrillic, U+1E00-1EFF Latin Extended Additional, U+2010-2027, U+2030-203A, U+20AC, U+2116, weight 550, no hinting or layout tables) made by `tools/make_label_font.py` from the font in Tactility's repository. The app makes its map label fonts from it on the device. The name table of the file names the licence. |

`main/Source/app/RootCertificates.h` holds the public root certificates of Google Trust Services (GTS Root R1, R4)
and Let's Encrypt (ISRG Root X1, X2), as published at pki.goog and letsencrypt.org, used to verify the HTTPS
connection to OpenFreeMap.

The rasteriser in `main/Source/render/Raster.cpp` is written for this project. It uses the published signed-area
coverage technique (as described for font-rs and stb_truetype), no code was copied.
