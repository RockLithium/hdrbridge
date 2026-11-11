# Architecture

HDR Bridge has three independent layers:

```text
desktop/       native Windows UI
core/          C++ HDR inspection, reconstruction, conversion and verification
web/           browser UI, Worker and WASM adapters
```

The desktop UI calls the C++ core directly. Full-resolution pixels never pass through a JavaScript or 8-bit UI buffer. The developer CLI exposes the same inspection and conversion operations as JSON for regression tests.

The Web application does not move or wrap the desktop UI. It uses the same conversion model through a narrow portable C/WASM boundary:

```text
Web UI -> Web Worker -> codec adapter -> Rec.2020/PQ RGB16 working image -> encoder
```

The worker and WebAssembly core are loaded only after the user selects a file. Separate single-threaded and pthread modules are built. A same-origin service worker establishes cross-origin isolation on static hosting when supported; it may reload the page once before user interaction. Isolated browsers use the threaded module, with a single-threaded fallback if isolation or threaded initialization is unavailable.

## Core model

`inspect` reads the encoded source representation and metadata. `convert` decodes or reconstructs HDR in linear light where needed, then caches a correctly oriented Rec.2020/PQ RGB16 image. Output adapters decode PQ to linear light when they need another gamut, transfer or linear output representation. `verify` reopens the result and checks its target representation.

The cache holds one source raster. Inspector paths that require full PNG/JXL/JXR decoding share it with preview and conversion; concurrent cache misses are serialized. Windows invalidation checks source path, size, write time, file identity and filesystem change time; it avoids reuse when a reliable revision is unavailable. The Web adapter keeps the staged input across operations, compares its actual bytes and invalidates the cache before replacing it. Different output settings reuse the same source raster.

Worker budgets use the detected logical thread count, falling back to four when detection is unavailable. Small pixel jobs can use fewer workers; scalar WASM stays on its calling thread. Tiled HEIF decoding shares the budget across tiles instead of starting a full codec pool inside every tile. Threaded WASM also divides the AVIF decoding budget across the live base, alpha, gain-map or tile codec contexts. Single-image AVIF encoding releases codec workers between items and uses the detected budget. HLG processing retains full-pixel transfer-error checks with per-worker reductions.

Parallel execution preserves the per-pixel formulas, precision and quantization. Diagnostic floating-point sums can differ in their least-significant digits because of reduction order, and codec scheduling can change compressed bitstream bytes. Thread budgets apply where the codec exposes a control; PNG/TIFF Deflate and the Windows JPEG XR codec retain their existing threading behavior and compression settings.

Direct PQ, direct HLG, ISO Ultra HDR, Apple Adaptive HDR, gain-map AVIF, gain-map TIFF and gain-map JPEG XL (`jhgm`) assets have separate input adapters. They share color and HDR mathematics only after reconstruction. Camera brand is not used as a transfer-function proxy.

Outputs are written to a temporary file, verified, then renamed to the final path. A failed or cancelled operation does not leave a partial final output.

## Platform boundaries

HEIF/AVIF, JPEG/JPEG XL, PNG, TIFF, ICC and Ultra HDR processing use portable libraries where practical. Windows JPEG XR uses WIC. The Web build substitutes jxrlib for the container codec while preserving the same FP16 linear scRGB output semantics; Windows WIC compatibility is covered by release regression.
