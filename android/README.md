# HDR Bridge for Android

Native Android interface to the shared HDR Bridge C++ core. Requires ARM64 and Android 8.0 or later. Image conversion is local. Network access is used only to check and download application updates from GitHub.

## Files and tasks

On **FILES**, choose output settings, add images and select **Add to queue**. Each task stores its own settings. File rows show the source format, dimensions and bit depth; **Inspect** opens source metadata and **Preview** opens a supported native HDR preview.

On **TASKS**, **Start all** converts pending tasks sequentially and becomes **Stop** while running. Completed and failed tasks are not repeated automatically. A task menu provides explicit restart, status reset, settings/name editing, reordering, file opening and removal. **Remove all** clears an idle queue.

Non-HDR files are skipped. A decode or conversion error affects only that task. Cancellation takes effect at codec checkpoints. Tasks survive activity recreation but are not restored after the app process exits.

## Output

Output representations and controls are listed in [Output formats](../docs/OUTPUT_FORMATS.md); supported inputs are shared with the core library. Output controls include gamut, PQ/HLG or gain-map representation, peak, gain-map size/channels, quality/compression, EXIF/XMP and filename suffix where applicable.

Photo Frame adds a blurred border, camera logo and caption to an HDR image and exports Ultra HDR JPEG. Source EXIF supplies caption fields but is not copied to the output; source XMP is also omitted. Its Lossless, EXIF and XMP controls are fixed off.

Tap the output path to select a folder. The folder grant, per-format parameters and suffixes are saved. **Reset settings** restores output parameters without clearing the selected folder or queued tasks.

Filenames use the source base name, optional suffix and format-controlled extension. Existing names are automatically numbered. Saving uses Android's document provider; the app cannot assume write access beside an imported document.

## HDR preview

Preview requires Android 14 or later, an HDR display and a gain-map image recognized by the system decoder. It decodes only when opened and supports pinch zoom, pan and double-tap. Other representations remain convertible without preview.

## Platform limitations

JPEG XR uses jxrlib. Some FP16 streams it encodes cannot be read by its own decoder, so those outputs lack the full pixel readback available through Windows WIC.

## Updates

The app quietly checks for a stable Android release at most once a day. Tap the version beside **HDR Bridge** to check manually, download an update or install it. Updates are verified against the release checksum, package identity, version and pinned signing certificate before installation. Output settings and the selected folder remain in place.

Android may ask you to allow installation from HDR Bridge or confirm an update. Installation waits until conversion and inspection are idle; it does not stop an active batch. A missing Android release or a network error does not affect conversion. APKs can also be downloaded and installed manually.

Production keys and release automation are described in [Android signing](signing/README.md).

## Build

See [Building HDR Bridge](../docs/BUILDING.md#android) for toolchain requirements and build commands, [native dependencies](DEPENDENCIES.md) for library configuration, and [third-party materials](THIRD_PARTY.md) for license and source locations.
