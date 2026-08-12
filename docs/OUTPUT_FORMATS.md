# Output formats

The desktop UI orders formats by intended use. Compression controls do not
change the transfer/gamut mathematics described below.

## Ultra HDR JPEG

An SDR JPEG base plus ISO gain map for sharing, Web and mobile use. Faithful/
Auto measures the reconstructed source peak. Gain-map resolution is 1/4, 1/2
(default) or full; channels are mono (default) or true per-channel RGB. Base
and gain-map JPEG quality remain independently controlled.

Ultra HDR is backward compatible with ordinary JPEG readers, which show only
the SDR base. It is not a lossless HDR master format.

## HDR PNG

RGB16 with Rec.2020/PQ by default, optional Display P3/PQ, Rec.2020/HLG or
Display P3/HLG.
PQ PNG writes cICP `9/16/0/1` and the production `Rec.2100 PQ` ICC. HLG writes
cICP `9/18/0/1` without a misleading PQ ICC. PNG Deflate is lossless for the
target RGB16 buffer.

## JPEG XL

RGB16 PQ or HLG for edit/master use. Both transfers support Rec.2020 and
Display P3. Native structured color encoding is used. Lossless mode is
verified against the RGB16 encoder input, while quality modes permit lossy JXL
compression. ISO gain-map output is also available with Rec.709, Display P3 or
Rec.2020 SDR bases. Gain-map JPEG XL uses a true RGB map only and defaults to
half resolution; mono output is intentionally unavailable.

## FP16 scRGB JPEG XR

Linear scRGB FP16, encoded through Windows WIC, with 1.0 representing 80 nits.
It is a Windows-oriented edit/master option and does not expose PQ/HLG or gamut
selectors. Lossless mode is verified against the FP16 encoder representation.

## HDR AVIF

10-bit 4:4:4 PQ or HLG using AV1, with Rec.2020 by default and optional Display
P3. PQ is the default. This is
a compact lossy HDR output whose application support is less universal than
its container and color signaling suggest. ISO gain-map AVIF is an alternate
representation with Rec.709, Display P3 or Rec.2020 SDR bases. It uses a true
RGB gain map only and defaults to half resolution.

## Direct HDR TIFF

RGB16 PQ with Rec.2020 or Display P3, Deflate compression and normalized
metadata. This is an advanced interchange option; tested application behavior
is limited and often requires manual color-space assignment.

## JPEG XR RGB10 — Experimental

Packed `GUID_WICPixelFormat32bppBGR101010`. WIC preserves the packed high-bit-
depth representation, but tested applications often interpret it incorrectly.
It is not included in the Web product.

## Naming and output safety

The format controls the extension. Output defaults beside the source, with an
optional persistent custom folder, editable base name/suffix, auto-numbering or
explicit overwrite. Conversion writes and verifies a temporary file before
committing the final path. Auto-numbering is resolved immediately before each
queued task starts, so duplicate queue entries and reruns cannot collide with
outputs produced earlier in the same or a previous run. The source file itself
is never a valid overwrite target.
