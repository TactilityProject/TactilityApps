# Third-party code

| Component | Where | Licence | Notes |
|---|---|---|---|
| miniz 3.0.2 (Rich Geldreich, RAD Game Tools, Valve Software) | `host/third_party/miniz/` | MIT, see `host/third_party/miniz/LICENSE` | PC build only: inflate and PNG writing for the host tools and tests. The app uses the `tinfl_decompress` exported by Tactility. |

`main/Source/app/RootCertificates.h` holds the public root certificates of Google Trust Services (GTS Root R1, R4)
and Let's Encrypt (ISRG Root X1, X2), as published at pki.goog and letsencrypt.org, used to verify the HTTPS
connection to OpenFreeMap.

The rasteriser in `main/Source/render/Raster.cpp` is written for this project. It uses the published signed-area
coverage technique (as described for font-rs and stb_truetype), no code was copied.

# Map data

Test fixtures in `tests/fixtures/` are vector tiles from [OpenFreeMap](https://openfreemap.org/), in the
OpenMapTiles schema. © OpenMapTiles © OpenStreetMap contributors. OpenStreetMap data is available under the
Open Database License (ODbL).
