# HDR color pipeline

HDR Bridge reads actual source signaling rather than assuming a camera model or
file extension implies a transfer function. CICP transfer 16 is PQ/ST 2084;
transfer 18 is HLG/BT.2100.

## Canonical HDR master

Direct PQ is decoded to absolute linear light. HLG is decoded with the inverse
HLG OETF and BT.2100 OOTF in Rec.2020 linear light. When a source does not carry
a more specific display model, HLG uses the BT.2100 1000-nit reference display
and system gamma 1.2.

Color-gamut conversion is performed in linear light. The canonical raster is
oriented once; base and gain map receive the same transform, and output
Orientation is 1. Working precision is at least RGB16 or floating point. No
production conversion uses an 8-bit UI, Canvas ImageData, screenshot, or SDR
temporary file.

## PQ and HLG output

PQ output encodes absolute luminance with ST 2084. HLG output applies the
inverse BT.2100 OOTF and HLG OETF; it is not a metadata-only transfer change.
Rec.2020 is the default video gamut. Display P3 is available for supported PQ
and HLG outputs. HLG remains a BT.2100 transfer but may be encoded with either
Rec.2020 or Display P3 primaries where the selected output exposes that option.

## Ultra HDR and gain maps

Gain-map inputs reconstruct HDR from an SDR base, gain map, item relationship
and family-specific metadata. ISO Ultra HDR JPEG, Apple auxiliary/MPF,
gain-map AVIF `tmap`, Adobe TIFF SubIFD and gain-map JPEG XL (`jhgm`) remain
separate adapters. Mono and RGB input maps are decoded according to their
actual channel layout; the SDR base is linearized in its own signaled gamut
before gain application and conversion to the canonical master.

Ultra HDR derives an SDR base and ISO gain map from the linear HDR master. Its
Faithful/Auto mode uses the measured content peak instead of the 10000-nit PQ
code-space ceiling. It supports mono or true per-channel RGB maps and defaults
to a half-resolution mono map. Gain-map JPEG XL and AVIF instead use a true
per-channel RGB map only and default to half resolution; Rec.709, Display P3
and Rec.2020 SDR base gamuts are available, with Display P3 as the default.
These are reconstructed HDR representations, not PQ/HLG metadata aliases.

FP16 JPEG XR converts the master to linear scRGB using 1.0 = 80 nits.

## Presentation

The Windows application omits image preview because it has no verified native
HDR presentation path. The Web application may use the browser's native
`<img>` presentation for supported Blob formats. It does not tone-map an SDR
preview and label it as HDR.
