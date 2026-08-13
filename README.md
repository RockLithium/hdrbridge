# HDR Bridge

HDR Bridge is a local HDR still-image converter for Windows. It understands both direct PQ/HLG images and gain-map formats, reconstructs a high-precision linear HDR master, then writes formats suited to sharing, video, editing, or compact delivery.

The desktop application does not upload images and does not present an SDR simulation as an HDR preview.

## Windows application

Supported input families include:

- direct PQ or HLG HEIF/HIF, AVIF, PNG, TIFF and JPEG XL;
- Apple Adaptive HDR HEIC and JPEG;
- ISO Ultra HDR JPEG, including mono and RGB gain maps;
- gain-map AVIF and gain-map TIFF;
- gain-map JPEG XL (`jhgm`);
- FP16 scRGB JPEG XR.

Available outputs:

- Ultra HDR JPEG (Faithful/Auto);
- 16-bit HDR PNG in PQ or HLG;
- 16-bit PQ HDR TIFF with optional lossless Deflate compression;
- 16-bit JPEG XL in PQ or HLG;
- RGB gain-map JPEG XL;
- FP16 linear scRGB JPEG XR;
- 10-bit 4:4:4 HDR AVIF in PQ or HLG;
- RGB gain-map AVIF;
- packed RGB10 JPEG XR as an experimental option.

The desktop order reflects observed interoperability: Ultra HDR JPEG, HDR PNG, HDR TIFF, JPEG XL, FP16 scRGB JPEG XR, AVIF, then experimental RGB10 JPEG XR. Direct HDR TIFF uses an HDR-aware CICP ICC and an application-compatible large-strip layout. Deflate compression is enabled by default and may be disabled.

The Inspector reports the encoded source representation, metadata, gain-map components and reconstructed HDR separately. The task queue is sequential so large images do not accumulate in memory; tasks retain independent settings, status and progress, and a bad or non-HDR file does not stop the rest of the queue.

Download the unsigned portable Windows x64 build from [GitHub Releases](https://github.com/RockLithium/hdrbridge/releases). Windows may show a SmartScreen or unknown-publisher warning.

## Build on Windows

Requirements and reproducible build commands are in
[docs/BUILDING.md](docs/BUILDING.md). A typical Release build is:

```powershell
.\scripts\bootstrap-vcpkg-local.ps1 -Ref ddd110b8a05cda14b8f1b0333a1d80c4fb6f16cd
cmake --preset windows-release
cmake --build --preset windows-release
ctest --test-dir build-vs -C Release --output-on-failure
```

Run `build-vs\bin\Release\hdrbridge.exe`. The CLI and test executables are developer tools and are not included in the normal Windows release archive.

## Web application

The browser version lives in [`web/`](web/) and is built independently from the desktop application. It runs the portable HDR core in a Web Worker and supports the same principal direct-HDR and gain-map input families. Browser outputs are Ultra HDR JPEG, HDR PNG, direct HDR TIFF, JPEG XL, FP16 scRGB JPEG XR and direct HDR AVIF. Files remain on the device.

## Documentation

- [Compatibility matrix](docs/COMPATIBILITY_MATRIX.md) — observed application behavior
- [Output formats](docs/OUTPUT_FORMATS.md) — representation and controls
- [Color pipeline](docs/COLOR_PIPELINE.md) — PQ, HLG and gain-map processing
- [Conversion fidelity](docs/CONVERSION_FIDELITY.md) — fidelity versus lossless encoding
- [Metadata](docs/METADATA.md) — source inspection and output preservation
- [Architecture](docs/ARCHITECTURE.md) — desktop/core/Web boundaries

## Privacy and fixtures

Conversion is local. Camera originals, private fixtures and generated test outputs are not part of this repository or its releases. The ignore rules also exclude common camera/HDR image extensions as a safeguard.

## License

Project source is licensed under Apache-2.0. Third-party components retain their own licenses; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
