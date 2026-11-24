# Building HDR Bridge

## Windows

Requirements: Windows 11 x64, Visual Studio with MSVC x64 and the Windows SDK (`CMakePresets.json` targets `Visual Studio 18 2026`; for earlier versions, configure with `cmake -S . -B build-vs -A x64` and the vcpkg toolchain variables), CMake 3.25+, Git and PowerShell.

From a Developer PowerShell in the repository root:

```powershell
.\scripts\bootstrap-vcpkg-local.ps1
cmake --preset windows-release
cmake --build --preset windows-release
```

The GUI is `build-vs\bin\Release\hdrbridge.exe`. The CLI and test executables are developer targets; they are excluded from the Windows distribution.

To create a portable package:

```powershell
.\scripts\package-windows.ps1
```

The archive contains the GUI, runtime DLLs, resources and license texts.

## Android

Build on a Windows host with JDK 17+, Android SDK platform 36, build-tools 35.0.0, NDK 30.0.16248370 and SDK CMake 3.31.6. Gradle 8.13 is supplied through the repository wrapper.

```powershell
.\scripts\build-android.ps1 -Sdk <sdk-directory> -Java <jdk-directory> -RebuildDependencies
```

Omit `-RebuildDependencies` when using an installed dependency set; `-Gradle` selects an alternative Gradle executable. Native library configuration is documented in [Android dependencies](../android/DEPENDENCIES.md).

The script prepares dependencies and resources, builds the optimized `preview` variant, runs lint and JVM tests, and verifies the APK signature, 16 KB ZIP alignment and packaged assets. Development outputs are `android/dist/HDRBridge-v<version>-Android-arm64-preview.apk` and `SHA256SUMS.txt`.

The preview variant uses a development signing key; `android/.local-signing/debug.keystore` overrides the default debug key. For production builds, invoke `.\android\scripts\build.ps1` directly with `-Configuration Release` and optionally `-SigningProperties <private-properties-file>` (`scripts/build-android.ps1` wraps preview builds only). Production outputs are `HDRBridge-v<version>-Android-arm64.apk`, `SHA256SUMS.txt` and `SHA256SUMS-Android.txt`, verified against the pinned release certificate. See [Android signing](../android/signing/README.md) for key storage and CI configuration (`.github/workflows/android-build.yml` and `.github/workflows/android-release.yml`). SDK paths, private signing files, build trees and caches are ignored by Git.

## Web

Web build commands and module configuration are in [web/README.md](../web/README.md).

## Tests

Windows CTest covers transfer math, orientation, naming, caching, output transactions, format round trips, Photo Frame rendering, preview tiling/mipmap resampling and metadata. Fixtures for these tests are generated at runtime.

```powershell
ctest --test-dir build-vs -C Release --output-on-failure
```

Android option/workspace tests and lint:

```powershell
.\android\gradlew.bat -p android :app:testDebugUnitTest :app:lintPreview
```

Web request-cancellation and version-stamping tests:

```powershell
node --test web/tests/*.test.mjs
```

## Versions and dependencies

The root `VERSION` supplies the CMake project versions, desktop label and file resources, Android package version, assembled Web page version and release links, and output Software tags. Library versions are pinned through the vcpkg baseline, manifests, overlays and the Ultra HDR source revision.

`cmake/PatchUltraHdr.cmake` configures the Ultra HDR dimension limit of 65,500 pixels per axis and checked buffer-size arithmetic. Available memory can impose a lower practical limit.

| Component | Purpose |
| --- | --- |
| libheif + libde265 | HEIF/HIF parsing and HEVC decode |
| libavif + libaom | AVIF decode and encode |
| libjxl | JPEG XL |
| libultrahdr + libjpeg-turbo | Ultra HDR and JPEG |
| libpng + zlib | PNG |
| libtiff + liblzma | TIFF |
| Little CMS | ICC handling |
| WIC / jxrlib | JPEG XR, depending on platform |
| stb_truetype + NanoSVG | Photo Frame font and SVG rasterization |

## Runtime resources and licenses

Windows and Android packages include `assets/fonts/` and `assets/frame-logos/`, including original SVG sources and their notices. Package scripts also collect codec, stb and NanoSVG license texts. See [Third-party notices](../THIRD_PARTY_NOTICES.md) for licenses and [Android third-party materials](../android/THIRD_PARTY.md) for static-library source and linking requirements.
