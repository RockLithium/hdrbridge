# Output formats

Compression controls affect encoding size and quality. Gamut and transfer settings determine the target color representation.

## Ultra HDR JPEG

An SDR JPEG base plus ISO gain map for sharing, Web and mobile use. The target peak is estimated automatically from the reconstructed source. Gain-map resolution is 1/4, 1/2 or full (default); channels are mono or true per-channel RGB (default). The quality slider controls both JPEG encoders and defaults to 95. Ordinary JPEG readers display the SDR base. Ultra HDR is not a lossless HDR master format.

## HDR PNG

RGB16 (or RGBA16 with alpha) with Rec.2020/PQ by default, optional Display P3/PQ, Rec.2020/HLG or Display P3/HLG. PQ PNG writes cICP `9/16/0/1` and the production `Rec.2100 PQ` ICC; Display P3/PQ uses the corresponding primaries. HLG writes cICP `9/18/0/1` or `12/18/0/1` without a PQ ICC. PNG Deflate is lossless for the target RGB16/RGBA16 buffer.

## Direct HDR TIFF

RGB16 PQ with Rec.2020 or Display P3, Deflate compression and normalized metadata. Its HDR-aware ICC carries CICP `9/16/0/1` for Rec.2020/PQ or `12/16/0/1` for Display P3/PQ. The image is stored in an application-compatible large-strip layout. Deflate without prediction is the default and is lossless for the target RGB16 buffer; uncompressed output is optional. Tested professional applications either recognize HDR automatically or accept manual Rec.2100 PQ assignment as recorded in the compatibility matrix.

## JPEG XL

RGB16 PQ or HLG for edit/master use. Both transfers support Rec.2020 and Display P3. Native structured color encoding is used. Lossless mode is verified against the RGB16 encoder input, while quality modes permit lossy JXL compression. ISO gain-map output is also available with Display P3 (default) or Rec.2020 SDR bases. Gain-map JPEG XL uses a true RGB map only and defaults to full resolution; mono output is intentionally unavailable.

## FP16 scRGB JPEG XR

Linear scRGB FP16, encoded through Windows WIC on Windows (or jxrlib on Web and Android), with 1.0 representing 80 nits. It is a Windows-oriented option and does not expose PQ/HLG or gamut selectors. On Windows, lossless mode is verified against the FP16 encoder representation.

## HDR AVIF

10-bit 4:4:4 PQ or HLG using AV1, with Rec.2020 by default and optional Display P3. PQ is the default. This is a compact lossy HDR output. ISO gain-map AVIF is an alternate representation with Display P3 (default) or Rec.2020 SDR bases. It uses a true RGB gain map only and defaults to full resolution.

## Gain Map extraction

Exports the embedded gain-map image without reconstructing the HDR master. Original is the default: JPEG keeps its compressed scan data without re-encoding while removing parent-container metadata, JPEG XL is copied byte-for-byte, TIFF remains TIFF, and auxiliary AVIF/HEIC items that are not independently usable fall back to lossless PNG. PNG and TIFF preserve decoded 16-bit samples; JPEG is an optional lossy 8-bit export. A three-channel gain map remains one RGB image; a mono map remains single-channel.

## Photo Frame

Adds a blurred border, camera logo and caption to an HDR image and exports **Ultra HDR JPEG**, **PNG** or **HDR AVIF**. Ultra HDR JPEG defaults to Display P3, a full-resolution RGB gain map and JPEG quality 95; gain-map resolution, channels and quality are configurable. PNG exports a lossless 16-bit RGB HDR PNG defaulting to Rec.2020, PQ/ST2084 and compression level 4; Display P3, HLG and compression level (1–9) are configurable. HDR AVIF exports a 10-bit 4:4:4 AVIF defaulting to Rec.2020, PQ/ST2084 and quality 95; Display P3, HLG and quality (1–100) are configurable, with Lossless fixed off.

The caption uses source EXIF and fills missing camera fields from XMP. A matching camera or device logo replaces the manufacturer name, and repeated brand prefixes are removed from the model; otherwise the manufacturer is prepended when missing from the model. Missing fields and their units are omitted. Source EXIF/XMP are not copied and EXIF/XMP controls are fixed off; Ultra HDR JPEG generates new HDR gain-map metadata, PNG writes HDR cICP signaling (and target ICC for PQ), and AVIF writes target NCLX signaling.

## Naming and output safety

The format controls the extension. Windows and Android store output parameters independently per format and preserve direct (`PQ`/`HLG`) and gain-map color gamuts separately across toggles; default suffixes update automatically when switching output formats or transfer/gain-map options, while manually edited suffixes are preserved across option changes. On Windows, output defaults beside the source with an optional persistent custom folder, editable base name/suffix, auto-numbering or explicit overwrite; Android writes to a user-selected document folder with auto-numbering, and Web downloads the converted file. Local file outputs write and verify a temporary file before committing the final path. Auto-numbering is resolved immediately before each queued task starts, so duplicate queue entries and reruns cannot collide with outputs produced earlier in the same or a previous run. The source file itself is never a valid overwrite target.
