# Third-party notices

The portable demo bundles the exact license texts installed with the tested dependencies under `licenses/`. This summary is not a substitute for those texts.

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

`libheif` and `libde265` are shipped as replaceable DLLs rather than folded into the application executable. x265 is not present in the dependency graph. Exact tested versions are recorded in `docs/THIRD_PARTY_LOCK.md`.
