# HDR Bridge for Android

Native Android interface to the shared HDR Bridge C++ core. Requires ARM64 and Android 8.0 or later. Image conversion is local. Network access is used only to check and download application updates from GitHub.

## Files and tasks

On **FILES**, choose output settings, add images and select **Add to queue**. Each task stores its own settings. File rows show the source format, dimensions and bit depth; **Preview** opens a supported native HDR preview, **Inspect** opens source metadata and **Delete** removes that file from the list.

On **TASKS**, **Start all** converts pending tasks sequentially and becomes **Stop** while running. Completed and failed tasks are not repeated automatically. Each task card includes a direct remove button (`×`), and its menu provides explicit restart, status reset, settings/name editing, reordering, file opening and removal. **Remove all** clears an idle queue.

Non-HDR files are skipped. A decode or conversion error affects only that task. Cancellation takes effect at codec checkpoints. Tasks survive activity recreation but are not restored after the app process exits.

## Output

Output representations and controls are listed in [Output formats](../docs/OUTPUT_FORMATS.md); supported inputs are shared with the core library. Output controls include gamut, PQ/HLG or gain-map representation, gain-map size/channels, quality/compression, EXIF/XMP and filename suffix where applicable.

Photo Frame adds a blurred border, camera logo and caption to an HDR image and exports Ultra HDR JPEG or lossless 16-bit HDR PNG. Captions use source EXIF and fill missing camera fields from XMP, without copying source EXIF or XMP to the output; EXIF and XMP are fixed off, while Lossless is fixed off for JPEG and fixed on for PNG.

Tap the output path to select a folder. The folder grant, per-format parameters and suffixes are saved. Default suffixes update automatically when switching options, while manually edited suffixes are preserved until **Reset settings** or **Reset format** restores defaults. On **FILES**, **Reset settings** restores output parameters without clearing the selected folder or queued tasks. Tap **HDR Bridge** in the header to switch between English and Simplified Chinese.

Filenames use the source base name, optional suffix and format-controlled extension. Existing names are automatically numbered. Saving uses Android's document provider; the app cannot assume write access beside an imported document.

## HDR preview

Preview uses the shared HDR decoder and requires Android 14 or later, an HDR display and a compatible native graphics surface. It opens full-screen, decodes only when opened, preserves the decoded session and framing across rotation and configuration changes, and supports pinch zoom, clamped panning and double-tap zoom/reset. Images larger than a single GPU texture use tiled `RGBA16F` textures with 1/2-stepped mipmaps when memory permits, falling back to the full-resolution level under memory pressure. Unsupported devices do not receive an SDR simulation.

## Platform limitations

JPEG XR uses jxrlib. Some FP16 streams it encodes cannot be read by its own decoder, so those outputs lack the full pixel readback available through Windows WIC.

## Updates

The app checks for a stable Android release in the background while open, reusing successful results for up to an hour. Tap the version beside **HDR Bridge** to check manually, download an update or install it. Updates are verified against the release checksum, package identity, version and pinned signing certificate before opening the system installer. Output settings and the selected folder remain in place.

Android may ask you to allow installation from HDR Bridge or confirm an update. Installation waits until conversion and inspection are idle; it does not stop an active batch. A missing Android release or a network error does not affect conversion. APKs can also be downloaded and installed manually.

Production keys and release automation are described in [Android signing](signing/README.md).

## Build

See [Building HDR Bridge](../docs/BUILDING.md#android) for toolchain requirements and build commands, [native dependencies](DEPENDENCIES.md) for library configuration, and [third-party materials](THIRD_PARTY.md) for license and source locations.
