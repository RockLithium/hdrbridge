# HDR color pipeline

HDR Bridge reads actual source signaling rather than assuming a camera model or file extension implies a transfer function. The input resolver records container-native signaling, embedded ICC information and format-specific HDR representations independently, then resolves the source primaries and transfer. Native signaling includes HEIF/AVIF NCLX, PNG cICP and JPEG XL structured color encoding. A valid ICC profile can also identify a direct-HDR input when native signaling is absent; matching native and ICC signals are reported together, while conflicts remain visible in Inspector. In either source, CICP transfer 16 is PQ/ST 2084 and transfer 18 is HLG/BT.2100.

## Canonical working representation

The cached HDR image is an oriented, unsigned RGB16 raster encoded as Rec.2020/PQ (ST 2084). It is not a persistent linear floating-point image. Direct Rec.2020/PQ input can retain its decoded PQ samples. Other gamuts and transfers are converted through linear-light calculations before the result is quantized to PQ16. Source signaling remains separate in Inspector; an optional straight-alpha plane is stored separately from RGB.

HLG is reconstructed with the inverse HLG OETF and BT.2100 OOTF. When a source does not carry a more specific display model, HLG uses the BT.2100 1000-nit reference display and system gamma 1.2. Gain-map reconstruction also operates in linear light. Base and gain map receive the same orientation transform, and output Orientation is 1. PQ16 quantization and its nonnegative, 10000-nit range limit the working representation; a lossless output codec does not undo those limits. No production conversion uses an 8-bit UI, Canvas ImageData, screenshot, or SDR temporary file.

## PQ and HLG output

PQ output encodes absolute luminance with ST 2084. HLG output applies the inverse BT.2100 OOTF and HLG OETF; it is not a metadata-only transfer change. Rec.2020 is the default video gamut. Display P3 is available for supported PQ and HLG outputs. HLG remains a BT.2100 transfer but may be encoded with either Rec.2020 or Display P3 primaries where the selected output exposes that option.

## Ultra HDR and gain maps

Gain-map inputs reconstruct HDR from an SDR base, gain map, item relationship and family-specific metadata. ISO Ultra HDR JPEG, Apple auxiliary/MPF, gain-map AVIF `tmap`, gain-map TIFF SubIFD and gain-map JPEG XL (`jhgm`) remain separate adapters. Mono and RGB input maps are decoded according to their actual channel layout; the SDR base is linearized in its own signaled gamut before gain application. The reconstructed result is converted to the Rec.2020/PQ RGB16 working image.

Ultra HDR decodes the working PQ16 image to linear light to derive an SDR base and ISO gain map. Its Faithful/Auto mode uses the measured content peak instead of the 10000-nit PQ code-space ceiling. It supports mono or true per-channel RGB maps and defaults to a half-resolution mono map. Gain-map JPEG XL and AVIF instead use a true per-channel RGB map only and default to half resolution; Rec.709, Display P3 and Rec.2020 SDR base gamuts are available, with Display P3 as the default. These are reconstructed HDR representations, not PQ/HLG metadata aliases.

## scRGB output

FP16 JPEG XR decodes the Rec.2020/PQ RGB16 image to linear light and converts it to scRGB using 1.0 = 80 nits.

## Presentation

The Windows application offers an optional native HDR preview through a linear FP16 swap chain. The Web application may use the browser's native `<img>` presentation for supported Blob formats. Neither path tone-maps an SDR preview and labels it as HDR.
