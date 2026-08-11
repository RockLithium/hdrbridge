# Third-party notices

The Windows package bundles the exact license texts installed with the tested dependencies under `licenses/`. The Web build publishes the libheif and libde265 LGPL texts alongside its WASM module and keeps the complete reproducible source/build scripts in this repository. This summary is not a substitute for those texts.

Primary dependencies:

| Component | Role | License / distribution note |
|---|---|---|
| libheif | HEIF/HIF/AVIF parsing, decode and AVIF encode orchestration | LGPL; avoid static-linking choices that remove users' LGPL replacement rights unless counsel/requirements are satisfied |
| libavif / libyuv | ISO gain-map AVIF parsing/reconstruction and high-bit-depth pixel conversion | BSD-style permissive licenses; exact installed texts bundled |
| libde265 | HEVC decode | LGPL; same care as above |
| libaom | AV1 encode/decode backend for direct PQ AVIF | BSD-style permissive licenses and patent file; exact installed text bundled |
| libpng / zlib | HDR PNG and DEFLATE/CRC support | libpng and zlib permissive licenses |
| libtiff / liblzma | direct HDR TIFF and optional TIFF runtime support | libtiff and public-domain/permissive upstream terms; exact installed texts bundled |
| libjxl | JPEG XL encode/decode/verify | BSD-3-Clause plus upstream patent/IP grant |
| libultrahdr | Ultra HDR encode/decode/verify | MIT and Apache-2.0 dual licensed upstream |
| libjpeg-turbo | JPEG backend for Ultra HDR | BSD-style IJG/libjpeg-turbo licenses |
| Brotli / Highway / Little CMS | libjxl runtime dependencies | permissive licenses; exact texts bundled |
| nlohmann/json | CLI JSON serialization | MIT; compiled header-only into the executables |
| Windows Imaging Component (WIC) | Windows JPEG XR encode/decode | operating-system API; no bundled jxrlib required for the primary path |

Rules:

The Windows package ships `libheif` and `libde265` as replaceable DLLs rather than folding them into the application executable. The Web module is reproducibly built from the pinned upstream sources listed in `web/README.md`; users can rebuild the WASM with modified library sources. x265 is not present in either dependency graph.
