# HDR color pipeline

HDR Bridge reads actual source signaling rather than assuming a camera model or
file extension implies a transfer function. CICP transfer 16 is PQ/ST 2084;
transfer 18 is HLG/BT.2100.

## Canonical HDR master

Direct PQ is decoded to absolute linear light. HLG is decoded with the inverse
HLG OETF and BT.2100 OOTF in Rec.2020 linear light. When a source does not carry
a more specific display model, HLG uses the BT.2100 1000-nit reference display
and system gamma 1.2.

Gain-map inputs reconstruct HDR from an SDR base, gain map, item relationship
and family-specific metadata. ISO Ultra HDR, Apple auxiliary/MPF and Adobe
`tmap`/SubIFD metadata remain separate adapters. Mono and RGB gain maps are
handled according to their actual channel layout.

Color-gamut conversion is performed in linear light. The canonical raster is
oriented once; base and gain map receive the same transform, and output
Orientation is 1. Working precision is at least RGB16 or floating point. No
production conversion uses an 8-bit UI, Canvas ImageData, screenshot, or SDR
temporary file.

## PQ and HLG output

PQ output encodes absolute luminance with ST 2084. HLG output applies the
inverse BT.2100 OOTF and HLG OETF; it is not a metadata-only transfer change.
Rec.2020 is the default video gamut. Display P3 is available for supported PQ
outputs, while HLG is Rec.2020 only.

Ultra HDR derives an SDR base and ISO gain map from the linear HDR master. Its
Faithful/Auto mode uses the measured content peak instead of the 10000-nit PQ
code-space ceiling. FP16 JPEG XR converts the master to linear scRGB using
1.0 = 80 nits.

## Presentation

The Windows application omits image preview because it has no verified native
HDR presentation path. The Web application may use the browser's native
`<img>` presentation for supported Blob formats. It does not tone-map an SDR
preview and label it as HDR.
