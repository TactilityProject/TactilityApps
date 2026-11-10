# Third-party code

| Component | Where | Licence | Notes |
|---|---|---|---|
| miniz 3.0.2 (Rich Geldreich, RAD Game Tools, Valve Software) | `host/third_party/miniz/` | MIT, see `host/third_party/miniz/LICENSE` | PC build only: inflate and PNG writing for the host tools and tests. The app uses the `tinfl_decompress` exported by Tactility. |
| Adwaita Sans (GNOME [adwaita-fonts](https://github.com/GNOME/adwaita-fonts), derived from Inter by the Inter Project Authors) | `assets/fonts/LabelSans.ttf` | SIL Open Font License 1.1, https://raw.githubusercontent.com/GNOME/adwaita-fonts/refs/heads/main/LICENSE | A subset (Basic Latin, Latin-1, Latin Extended-A, U+0218-021B, U+0259, Greek U+0386-03CE, Cyrillic U+0400-045F and U+0490-0491, plus the characters listed in `tools/label_codepoints.txt`, which map names use; weight 550, no hinting or layout tables) made by `host/make_label_font.py` from the font in Tactility's repository. The app makes its map label fonts from it on the device. The name table of the file names the licence. |

| [Maki](https://github.com/mapbox/maki) icons (Mapbox) | `main/Source/render/PoiIconData.h` | CC0 1.0 Universal | 89 icons turned into 15 x 15 alpha masks by `host/make_poi_icons.py`. |

`main/Source/app/RootCertificates.h` holds the public root certificates of Google Trust Services (GTS Root R1, R4)
and Let's Encrypt (ISRG Root X1, X2), as published at pki.goog and letsencrypt.org, used to verify the HTTPS
connection to OpenFreeMap.

The rasteriser in `main/Source/render/Raster.cpp` is written for this project. It uses the published signed-area
coverage technique (as described for font-rs and stb_truetype), no code was copied.

# Map data

Test fixtures in `tests/fixtures/` are vector tiles from [OpenFreeMap](https://openfreemap.org/), in the
OpenMapTiles schema. © OpenMapTiles © OpenStreetMap contributors. OpenStreetMap data is available under the
Open Database License (ODbL).
