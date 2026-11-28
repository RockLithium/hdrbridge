# Android native dependencies

The dependency manifest is `android/vcpkg.json`; it pins the vcpkg baseline. `android/ports-overlay/libjxl` supplies the JPEG XL port, `ports-overlay/aom` supplies the shared libaom overlay, and `android/triplets/arm64-android-hdr.cmake` defines the target.

The triplet builds static, position-independent libraries for Android API 26 and arm64-v8a using the NDK shared C++ runtime. The dependency build scripts target a Windows host.

| Component | Role |
| --- | --- |
| libheif + libde265 | HEIF/HIF parsing and HEVC decode |
| libavif + libaom | AVIF decode and encode |
| libjxl | JPEG XL |
| libultrahdr + libjpeg-turbo | Ultra HDR and JPEG |
| libpng + zlib | PNG |
| libtiff + liblzma | TIFF |
| Little CMS | ICC interpretation and generation |
| Expat | XMP parsing |
| jxrlib | FP16 scRGB JPEG XR |
| nlohmann/json | JNI data exchange |

libheif enables libde265 and AOM; x265 is disabled. Ultra HDR is built by the application CMake configuration from its pinned revision and repository patch.

From the repository root:

```powershell
.\scripts\bootstrap-vcpkg-local.ps1
.\android\scripts\build-deps.ps1 -Ndk <sdk-directory>\ndk\30.0.16248370
```

`-Vcpkg` overrides the vcpkg executable. Installed libraries are under `android/vcpkg_installed`; build sources are under `android/build-deps`. Downloads default to `.tools/vcpkg/downloads`, or the location set by `VCPKG_DOWNLOADS`.

Use [the Android build entry point](../docs/BUILDING.md#android) to prepare dependencies, resources and notices and build the APK.
