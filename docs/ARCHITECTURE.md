# Architecture

HDR Bridge has a shared core and independent platform interfaces:

```text
desktop/       native Windows UI
core/          C++ HDR inspection, reconstruction, conversion and verification
web/           browser UI, Worker and WASM adapters
android/       native Android UI, JNI and platform adapters
```

The desktop UI calls the C++ core directly without an intermediate 8-bit UI buffer. The developer CLI exposes the same inspection and conversion operations as JSON for regression tests.

Photo Frame uses the canonical HDR raster to compose a border, camera logo and caption, then encodes Ultra HDR JPEG or 16-bit HDR PNG. Desktop and Android share the compositor, font rasterizer and SVG renderer. Fonts and logo sources are packaged as runtime assets.

The Web UI calls the portable core through a C/WASM boundary:

```text
Web UI -> Web Worker -> codec adapter -> Rec.2020/PQ RGB16 working image -> encoder
```

The worker and WebAssembly core are loaded only after the user selects a file. Separate single-threaded and pthread modules are built. A same-origin service worker establishes cross-origin isolation on static hosting when supported; it may reload the page once before user interaction. Isolated browsers use the threaded module, with a single-threaded fallback if isolation or threaded initialization is unavailable.

The native Android application builds the existing C++ core through JNI, independently of the Windows desktop and Web interfaces. Android uses a user-selected document tree for output and an app-private intermediate for conversion; it streams verified results to the document provider. Its optional HDR preview uses the shared decoder and native HDR surfaces on Android 14 or later. A dedicated graphics thread owns surface resources; decoding begins when the preview activity opens, while configuration changes preserve the decoded session and re-bind display HDR headroom listeners when the active display changes. Update checking, SHA-256 and certificate-pin verification, and `PackageInstaller` handoff run in the Java layer outside the conversion core. The Android app targets ARM64 and Android 8.0 or later. See the [Android guide](../android/README.md) for build and platform details.

## Core model

`inspect` reads the encoded source representation and metadata. `convert` decodes or reconstructs HDR in linear light where needed, then caches a correctly oriented Rec.2020/PQ RGB16 image. Output adapters decode PQ to linear light when they need another gamut, transfer or linear output representation. `verify` reopens the result and checks its target representation.

The cache holds one source raster. Inspector paths that require full PNG/JXL/JXR decoding share it with preview and conversion; concurrent cache misses are serialized. Windows invalidation checks source path, size, write time, file identity and filesystem change time; it avoids reuse when a reliable revision is unavailable. Removing or clearing sources discards matching cache entries and prevents in-flight decodes from refilling them; Android also deletes unreferenced staged source files and skips queued inspections after removal. The Web adapter keeps the staged input across operations, compares its actual bytes and invalidates the cache before replacing it. Different output settings reuse the same source raster.

Worker budgets use the detected logical thread count, falling back to four when detection is unavailable. Small pixel jobs can use fewer workers; scalar WASM stays on its calling thread. Tiled HEIF decoding shares the budget across tiles instead of starting a full codec pool inside every tile. Threaded WASM also divides the AVIF decoding budget across the live base, alpha, gain-map or tile codec contexts. Single-image AVIF encoding releases codec workers between items and uses the detected budget. HLG processing retains full-pixel transfer-error checks with per-worker reductions.

Parallel execution preserves the per-pixel formulas, precision and quantization. Diagnostic floating-point sums can differ in their least-significant digits because of reduction order, and codec scheduling can change compressed bitstream bytes. Thread budgets apply where the codec exposes a control; PNG/TIFF Deflate and the Windows JPEG XR codec retain their existing threading behavior and compression settings. FP16 scRGB conversion reuses a double-precision PQ lookup table. Bounded preview resizing combines linear-light interpolation and alpha compositing directly into the FP16 display buffer, avoiding an intermediate PQ16 raster. Native Windows and Android previews retain full resolution with 1/2-stepped linear-light mipmap levels, fall back to the base level when extra mip allocation or upload fails, split images larger than a single GPU texture into tiles with one-texel seam overlaps, and blend adjacent levels in linear light for trilinear filtering.

Direct PQ, direct HLG, ISO Ultra HDR, Apple Adaptive HDR, gain-map AVIF, gain-map TIFF and gain-map JPEG XL (`jhgm`) assets have separate input adapters. They share color and HDR mathematics only after reconstruction. Camera brand is not used as a transfer-function proxy.

For core filesystem outputs, each output reserves its own temporary sibling file, verifies it, then commits on the same filesystem. Overwriting replaces the existing file without deleting it first; no-overwrite commits refuse an existing target. Failed or cancelled operations before commit preserve the previous target and remove only their own temporary file. Cancellation is checked again immediately before commit.

JPEG XL gain-map verification reuses its existing reconstructed raster to check map dimensions and RGB channels and measure linear-light error at up to 4,096 sampled pixels. Extraction verifies the written bytes and image header without another full decode.

## Desktop shell

The desktop posts each completed task's verification and timing to Activity immediately; the end-of-batch message contains the counts. Update checking and download run outside the conversion core. A downloaded installer is checked against the Release SHA-256 and checked again before launch. Updates never interrupt active conversions: if a conversion queue is running, installation waits until you click the version label after the queue finishes.

## Platform boundaries

HEIF/AVIF, JPEG/JPEG XL, PNG, TIFF, ICC and Ultra HDR processing use portable libraries. Windows JPEG XR uses WIC; Android and Web use jxrlib for FP16 linear scRGB encoding. Platform-specific code handles file access, presentation and task scheduling.

## Testing

Synthetic end-to-end tests independently decode exported pixels and check lossless equality or format-specific error tolerances, including orientation, ICC and gain-map cases.
