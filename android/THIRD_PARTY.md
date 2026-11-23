# Android third-party materials

The APK contains dependency license texts in `assets/licenses/`, the Roboto font and its license in `assets/fonts/`, and the original camera-logo SVGs with their license and source notice in `assets/frame-logos/`. The complete dependency list is in [Third-party notices](../THIRD_PARTY_NOTICES.md).

Regenerate the packaged resources from the repository root:

```powershell
.\android\scripts\prepare-notices.ps1
```

The script reads installed vcpkg copyright files, the pinned libultrahdr license files and repository assets. Missing notices or source assets fail the build.

## Source and linking

`android/vcpkg.json` records library versions through its baseline and features. Overlays are under `android/ports-overlay` and `ports-overlay/aom`; extracted package sources are in `android/build-deps/buildtrees`. `android/app/src/main/cpp/CMakeLists.txt` records the Ultra HDR revision and shared-core build configuration.

libheif and libde265 are statically linked LGPL libraries. Distribution must satisfy their source and relinking requirements, including the corresponding library sources and application code suitable for rebuilding the combined work; see [LGPL-3.0 section 4](https://www.gnu.org/licenses/lgpl-3.0.en.html#section4). License texts alone do not supply those build materials.
