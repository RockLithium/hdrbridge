# HDR Bridge Web

HDR Bridge Web is the static, one-file-at-a-time browser edition of HDR Bridge. Selected images and converted results stay on the device; the page has no upload or server-side conversion path.

The WebAssembly module compiles the desktop project's HDR core and portable codec libraries. It supports direct PQ/HLG HEIF/HIF, Apple Adaptive HDR HEIC/JPEG, Ultra HDR JPEG, gain-map AVIF, gain-map TIFF, direct HDR AVIF/PNG/TIFF, JPEG XL and FP16 scRGB JPEG XR input, including ISO gain-map JPEG XL. Outputs are Ultra HDR JPEG, HDR PNG, direct HDR TIFF, JPEG XL, FP16 scRGB JPEG XR and HDR AVIF. JPEG XL and AVIF can be written as direct PQ/HLG or ISO gain-map HDR. Their gain-map outputs use RGB maps only and default to full resolution; Ultra HDR JPEG also defaults to a full-resolution RGB map while retaining its Mono/RGB choice.

Direct HDR TIFF uses the same HDR-aware CICP ICC and large-strip layout as the Windows application. Deflate compression is the default; uncompressed output is available from the format controls.

Native browser `<img>` presentation is used for JPEG, PNG, AVIF and JPEG XL previews when supported by the browser. If the browser cannot display an output format, conversion and download remain available and the preview reports that it is unavailable. No SDR simulation is generated.

## Build

On Windows, pass your local Emscripten and Ninja paths if they differ from the script defaults (`-EmsdkRoot`, `-NinjaPath`, and optionally `-VcpkgRoot`):

```powershell
pwsh web/scripts/build-wasm.ps1 -EmsdkRoot <emsdk-dir> -NinjaPath <ninja.exe>
pwsh web/scripts/assemble-site.ps1
```

On Linux (as used in CI), activate Emscripten and run `web/scripts/build-wasm.sh` followed by `web/scripts/assemble-site.sh`.

The build produces separate scalar and pthread modules, including the threaded module's worker files. A same-origin service worker attempts cross-origin isolation on GitHub Pages and other static hosts, with at most one automatic reload before user interaction. The worker loads the threaded module when isolation and SharedArrayBuffer are available, otherwise it uses the scalar module. Threaded initialization failures also fall back to scalar. The thread budget uses the detected logical thread count, or four if unavailable. Browser and device WebAssembly memory limits still apply.

The assembled site also contains `/bundled/`, a compatibility entry whose HTML embeds both WebAssembly binaries. The standard page offers this entry when the separate core download fails or times out. `assemble-site` generates the Base64 payloads automatically from the compiled modules and stamps the root `VERSION` into both pages and their Windows/Android download links; neither the payloads nor the stamped links are maintained by hand. The staged source and its canonical raster are reused for repeated conversions of the same file; changed input bytes invalidate them.
