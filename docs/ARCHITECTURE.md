# Architecture

HDR Bridge has three independent layers:

```text
desktop/       native Windows UI
core/          C++ HDR inspection, reconstruction, conversion and verification
web/           browser UI, Worker and WASM adapters
```

The desktop UI calls the C++ core directly. Full-resolution pixels never pass
through a JavaScript or 8-bit UI buffer. The developer CLI exposes the same
inspection and conversion operations as JSON for regression tests.

The Web application does not move or wrap the desktop UI. It uses the same
conversion model through a narrow portable C/WASM boundary:

```text
Web UI -> Web Worker -> codec adapter -> canonical linear HDR master -> encoder
```

Codec modules are loaded on demand. The GitHub Pages build remains
single-thread compatible because cross-origin isolation headers are not
available there; deployments with suitable headers may add threaded modules.

## Core model

`inspect` reads the encoded source representation and metadata. `convert`
decodes or reconstructs a canonical, correctly oriented, high-precision linear
HDR master and sends it to the selected output adapter. `verify` reopens the
result and checks its target representation.

Direct PQ, direct HLG, ISO Ultra HDR, Apple Adaptive HDR and Adobe gain-map
assets have separate input adapters. They share color and HDR mathematics only
after reconstruction. Camera brand is not used as a transfer-function proxy.

Outputs are written to a temporary file, verified, then renamed to the final
path. A failed or cancelled operation does not leave a partial final output.

## Platform boundaries

HEIF/AVIF, JPEG/JPEG XL, PNG, TIFF, ICC and Ultra HDR processing use portable
libraries where practical. Windows JPEG XR uses WIC and is isolated from the
portable core. The Web build therefore requires a separate portable JPEG XR
codec with equivalent FP16 scRGB semantics; it cannot compile the WIC adapter.
