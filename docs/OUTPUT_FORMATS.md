# Output formats

The desktop UI orders formats by intended use. Compression controls do not change the transfer/gamut mathematics described below.

## Ultra HDR JPEG

An SDR JPEG base plus ISO gain map for sharing, Web and mobile use. Faithful/Auto measures the reconstructed source peak. Gain-map resolution is 1/4, 1/2 (default) or full; channels are mono (default) or true per-channel RGB. Base and gain-map JPEG quality remain independently controlled.
Ultra HDR is backward compatible with ordinary JPEG readers, which show only the SDR base. It is not a lossless HDR master format.

## HDR PNG

RGB16 with Rec.2020/PQ by default, optional Display P3/PQ, Rec.2020/HLG or Display P3/HLG.
PQ PNG writes cICP `9/16/0/1` and the production `Rec.2100 PQ` ICC; Display P3/PQ uses the corresponding primaries. HLG writes cICP `9/18/0/1` or `12/18/0/1` without a PQ ICC. PNG Deflate is lossless for the target RGB16 buffer.

## Direct HDR TIFF

RGB16 PQ with Rec.2020 or Display P3, Deflate compression and normalized metadata. Its HDR-aware ICC carries CICP `9/16/0/1` for Rec.2020/PQ or `12/16/0/1` for Display P3/PQ. The image is stored in an application-compatible large-strip layout. Deflate without prediction is the default and is lossless for the target RGB16 buffer; uncompressed output is optional. Tested professional applications either recognize HDR automatically or accept manual Rec.2100 PQ assignment as recorded in the compatibility matrix.

## JPEG XL

RGB16 PQ or HLG for edit/master use. Both transfers support Rec.2020 and Display P3. Native structured color encoding is used. Lossless mode is verified against the RGB16 encoder input, while quality modes permit lossy JXL compression. ISO gain-map output is also available with Rec.709, Display P3 or Rec.2020 SDR bases. Gain-map JPEG XL uses a true RGB map only and defaults to half resolution; mono output is intentionally unavailable.

## FP16 scRGB JPEG XR

Linear scRGB FP16, encoded through Windows WIC, with 1.0 representing 80 nits. It is a Windows-oriented option and does not expose PQ/HLG or gamut selectors. Lossless mode is verified against the FP16 encoder representation.

## HDR AVIF

10-bit 4:4:4 PQ or HLG using AV1, with Rec.2020 by default and optional Display P3. PQ is the default. This is a compact lossy HDR output whose application support is less universal than its container and color signaling suggest. ISO gain-map AVIF is an alternate representation with Rec.709, Display P3 or Rec.2020 SDR bases. It uses a true RGB gain map only and defaults to half resolution.

## Gain Map extraction

Exports the embedded gain-map image without reconstructing the HDR master. Original is the default: JPEG keeps its compressed scan data without re-encoding while removing parent-container metadata, JPEG XL is copied byte-for-byte, TIFF remains TIFF, and auxiliary AVIF/HEIC items that are not independently usable fall back to lossless PNG. PNG and TIFF preserve decoded 16-bit samples; JPEG is an optional lossy 8-bit export. A three-channel gain map remains one RGB image; a mono map remains single-channel.

## Naming and output safety

The format controls the extension. Output defaults beside the source, with an optional persistent custom folder, editable base name/suffix, auto-numbering or explicit overwrite. Conversion writes and verifies a temporary file before committing the final path. Auto-numbering is resolved immediately before each queued task starts, so duplicate queue entries and reruns cannot collide with outputs produced earlier in the same or a previous run. The source file itself is never a valid overwrite target.
