# Building HDR Bridge for Windows

## Requirements

- Windows 11 x64
- Visual Studio with the current MSVC x64 workload and Windows SDK
- CMake 3.25 or newer
- Git and PowerShell

Dependencies are resolved through the pinned vcpkg revision in the bootstrap
command. The build uses dynamic libheif/libde265 libraries and does not include
x265.

## Configure and build

From a Developer PowerShell in the repository root:

```powershell
.\scripts\bootstrap-vcpkg-local.ps1 -Ref ddd110b8a05cda14b8f1b0333a1d80c4fb6f16cd
cmake --preset windows-release
cmake --build --preset windows-release
ctest --test-dir build-vs -C Release --output-on-failure
```

The GUI is `build-vs\bin\Release\hdrbridge.exe`. Developer-only CLI and test
targets are built alongside it when `BUILD_TESTING` is enabled.

## Portable package

```powershell
.\scripts\package-windows.ps1
```

The archive contains the GUI, required runtime DLLs, license texts and public
documentation. It does not include the CLI, test executables, build tree,
fixtures or generated test output.

## Main dependencies

| Component | Purpose |
|---|---|
| libheif + libde265 | HEIF/HIF parsing and HEVC decode |
| libavif + libaom | AVIF item parsing, decode and encode |
| libjxl | JPEG XL decode, encode and verification |
| libultrahdr + libjpeg-turbo | Ultra HDR decode, reconstruction and encode |
| libpng + zlib | HDR PNG |
| libtiff + liblzma | HDR and gain-map TIFF |
| Little CMS | ICC parsing and profile generation |
| Windows Imaging Component | FP16 and experimental RGB10 JPEG XR |

Exact license texts are collected into the portable archive. The tested
dependency versions are recorded in `vcpkg.json`, the vcpkg lock revision and
the libjxl overlay.
