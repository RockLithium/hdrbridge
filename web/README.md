# HDR Bridge Web

HDR Bridge Web is the static, one-file-at-a-time browser edition of HDR Bridge.
Selected images and converted results stay on the device; the page has no upload
or server-side conversion path.

The WebAssembly module compiles the desktop project's HDR core and portable codec
libraries. It supports direct PQ/HLG HEIF/HIF, Apple Adaptive HDR HEIC/JPEG,
Ultra HDR JPEG, Adobe gain-map AVIF/TIFF, direct HDR AVIF/PNG/TIFF, JPEG XL and
FP16 scRGB JPEG XR input. Outputs are Ultra HDR JPEG, HDR PNG, JPEG XL, FP16
scRGB JPEG XR, direct HDR AVIF and direct HDR TIFF.

Native browser HDR presentation is used for JPEG, PNG and AVIF previews. If the
browser cannot display an output format, conversion and download remain available
and the preview reports that it is unavailable. No SDR simulation is generated.

## Build

The Windows build uses the repository's pinned vcpkg setup and an active
Emscripten SDK:

```powershell
pwsh web/scripts/build-wasm.ps1
pwsh web/scripts/assemble-site.ps1
```

The module is single-threaded so GitHub Pages does not need cross-origin
isolation headers. Large images are processed in a worker, but browser and device
WebAssembly memory limits still apply.
