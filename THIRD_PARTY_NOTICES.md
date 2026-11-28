# Third-party notices

Windows packages contain dependency licenses under `licenses/`; Android APKs contain them under `assets/licenses/`. The Web build publishes this notice alongside its WASM modules. Library revisions and build inputs are recorded in the manifests, overlays and build scripts.

Primary dependencies:

| Component | Role | License / distribution note |
|---|---|---|
| libheif | HEIF/HIF/AVIF parsing, decode and AVIF encode orchestration | LGPL; Windows DLL, Android static library and Web WASM builds |
| libavif / libyuv | ISO gain-map AVIF parsing/reconstruction and high-bit-depth pixel conversion | BSD-style permissive licenses; exact installed texts bundled |
| libde265 | HEVC decode | LGPL; Windows DLL, Android static library and Web WASM builds |
| libaom | AV1 encode/decode backend for direct PQ AVIF | BSD-style permissive licenses and patent file; exact installed text bundled |
| libpng / zlib | HDR PNG and DEFLATE/CRC support | libpng and zlib permissive licenses |
| libtiff / liblzma | direct HDR TIFF and optional TIFF runtime support | libtiff and public-domain/permissive upstream terms; exact installed texts bundled |
| libjxl | JPEG XL encode/decode/verify | BSD-3-Clause plus upstream patent/IP grant |
| libultrahdr | Ultra HDR encode/decode/verify | MIT and Apache-2.0 dual licensed upstream |
| libjpeg-turbo | JPEG backend for Ultra HDR | BSD-style IJG/libjpeg-turbo licenses |
| Brotli / Highway / Little CMS | libjxl runtime dependencies | permissive licenses; exact texts bundled |
| nlohmann/json | CLI, Android JNI, Web and EXIF-caption JSON handling | MIT; header-only |
| Windows Imaging Component (WIC) | Windows JPEG XR encode/decode | operating-system API on Windows |
| jxrlib | Android and Web JPEG XR encode/decode | BSD-style permissive license; statically linked in Android and Web builds |
| Wikimedia Commons camera/brand logo SVGs | Photo Frame visual assets | Public domain (`PD-textlogo` / simple geometric shapes) on Wikimedia Commons; manufacturer trademarks retained by respective owners (see `assets/frame-logos/SOURCE.md`) |
| Roboto | Photo Frame typography | Apache-2.0; license text retained with bundled assets |
| stb | Photo Frame font rasterization | MIT; license text retained with bundled assets |
| NanoSVG | Photo Frame SVG support | zlib; license text retained with bundled assets |
| Expat | XMP parsing across Windows, Android and Web | MIT; exact installed text bundled |

## Linking

Windows ships libheif and libde265 as replaceable DLLs. Android and Web link these libraries statically; Android source and relinking requirements are described in [Android third-party materials](android/THIRD_PARTY.md), and the Web modules can be rebuilt from the pinned sources and scripts in [web/](web/README.md). x265 is not included.
