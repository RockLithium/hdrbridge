# HDR Bridge Web

HDR Bridge Web is a static, one-file-at-a-time converter for GitHub Pages. The
selected image and converted result stay in the browser. Codec modules load in
a Web Worker only when needed.

## Current conversion path

The first production module handles direct HDR HEIF/HIF:

- HEVC decode through libheif 1.23.1 and libde265 1.0.16;
- Rec.2020 or Display P3 output;
- PQ/ST2084 or BT.2100 HLG output (HLG is Rec.2020 only);
- lossless 16-bit RGB PNG with cICP and full-range signaling;
- native browser preview and Blob download.

PQ/HLG transfer math is compiled from `core/src/hdr_transfer.cpp`. Regression
tests compare decoded 16-bit pixel CRCs against the Windows core for real Canon
PQ and Nikon HLG fixtures. Private fixtures are not included in this repository.

Other output cards remain visible because they define the product UI, but the
page labels unavailable codec paths plainly and disables conversion for them.
They are not substituted with another format.

## Build

Activate Emscripten 4.0.21 and run:

```bash
bash web/scripts/build-wasm.sh
bash web/scripts/assemble-site.sh
```

On the Windows development machine, `web/scripts/build-wasm.ps1` performs the
same pinned build using the existing Emscripten and Ninja installations. Build
caches, generated WASM, private images and `web/dist` are ignored by Git.

The module is single-threaded so deployment does not require cross-origin
isolation headers that GitHub Pages cannot configure. Large images can still
reach a browser's WebAssembly memory limit; this is reported as a conversion
error.
