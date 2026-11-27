# HDR Bridge

HDR Bridge is a local HDR still-image converter for Windows, Android and the Web. It understands both direct PQ/HLG images and gain-map formats, normalizes them to a high-precision Rec.2020/PQ RGB16 working image, then writes formats suited to sharing, video, editing, or compact delivery. HDR reconstruction and color transforms are calculated in linear light.

Files are processed on the device. HDR previews use native display paths on supported systems.

## Supported inputs

All platforms share the C++ core and support these principal HDR input families:

- direct PQ or HLG HEIF/HIF, Panasonic HSP, AVIF, PNG, TIFF and JPEG XL;
- Apple Adaptive HDR HEIC/JPEG and ISO `tmap` HEIF/HEIC;
- ISO Ultra HDR JPEG, including mono and RGB gain maps;
- gain-map AVIF and gain-map TIFF;
- gain-map JPEG XL (`jhgm`);
- FP16 scRGB JPEG XR.

## Windows application

Available outputs:

- Ultra HDR JPEG;
- 16-bit HDR PNG in PQ or HLG;
- 16-bit PQ HDR TIFF with optional lossless Deflate compression;
- 16-bit JPEG XL in PQ or HLG;
- RGB gain-map JPEG XL;
- FP16 linear scRGB JPEG XR;
- 10-bit 4:4:4 HDR AVIF in PQ or HLG;
- RGB gain-map AVIF;
- embedded gain-map extraction to the original payload where possible, or PNG, TIFF and JPEG images;
- Photo Frame: Ultra HDR JPEG or 16-bit PQ/HLG HDR PNG with a blurred border, camera logo and caption.

See [output formats](docs/OUTPUT_FORMATS.md) for controls and defaults, and the [compatibility matrix](docs/COMPATIBILITY_MATRIX.md) for application support.

The Inspector reports the encoded source representation, metadata, gain-map components and reconstructed HDR separately. The task queue is sequential so large images do not accumulate in memory; tasks retain independent settings, status and progress, and a bad or non-HDR file does not stop the rest of the queue.

Download the Windows x64 installer or portable ZIP from [GitHub Releases](https://github.com/RockLithium/hdrbridge/releases). The installer supports in-place upgrades. Both builds are unsigned; Windows may show a SmartScreen or unknown-publisher warning.

Click **HDR Bridge** in the header to switch the interface between English and Simplified Chinese. The desktop checks for updates in the background; click the version beside the title to check manually. Updates are downloaded and verified before opening the installer, with confirmation before download and installation. Updates never interrupt active conversions: if a conversion queue is running, installation waits until you click the version label after the queue finishes. Updating from a portable copy runs the standard installer rather than replacing the portable folder in place.

## Web application

The browser version lives in [`web/`](web/) and is built independently from the desktop application. It runs the portable HDR core in a Web Worker, includes links to the Windows and Android releases, and supports the same principal direct-HDR and gain-map input families. Browser outputs are Ultra HDR JPEG, HDR PNG, direct HDR TIFF, JPEG XL, FP16 scRGB JPEG XR and HDR AVIF. JPEG XL and AVIF support direct PQ/HLG and ISO gain-map output. Files remain on the device.

## Android application

The native [Android application](android/README.md) brings the shared C++ core to ARM64 devices running Android 8.0 or later, with an English/Simplified Chinese interface, a file library, per-task output settings and a sequential conversion queue. Signed release APKs are published on [GitHub Releases](https://github.com/RockLithium/hdrbridge/releases); tap the version beside the title to check for verified Android updates. Native HDR preview uses the shared decoder and requires Android 14 or later, an HDR display and a compatible graphics surface. See the Android guide for usage and platform limitations.

## Build on Windows

Requirements and reproducible build commands are in [docs/BUILDING.md](docs/BUILDING.md). A typical Release build is:

```powershell
.\scripts\bootstrap-vcpkg-local.ps1
cmake --preset windows-release
cmake --build --preset windows-release
ctest --test-dir build-vs -C Release --output-on-failure
```

Run `build-vs\bin\Release\hdrbridge.exe`. The CLI and test executables are developer tools and are not included in the normal Windows release archive.

## Documentation

- [Compatibility matrix](docs/COMPATIBILITY_MATRIX.md) — observed application behavior
- [Output formats](docs/OUTPUT_FORMATS.md) — representation and controls
- [Color pipeline](docs/COLOR_PIPELINE.md) — PQ, HLG and gain-map processing
- [Conversion fidelity](docs/CONVERSION_FIDELITY.md) — fidelity versus lossless encoding
- [Metadata](docs/METADATA.md) — source inspection and output preservation
- [Architecture](docs/ARCHITECTURE.md) — desktop/core/Web/Android boundaries

## Privacy and fixtures

Conversion is local. Camera originals, private fixtures and generated test outputs are not part of this repository or its releases. The ignore rules also exclude common camera/HDR image extensions as a safeguard.

## License

Project source is licensed under Apache-2.0. Third-party components retain their own licenses; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
