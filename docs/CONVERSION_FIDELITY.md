# Conversion fidelity and lossless encoding

HDR fidelity and lossless compression are different claims.

**Fidelity** means preserving intended HDR luminance, dynamic range, hue and gamut relationships to the precision and gamut of the destination.
**Lossless** means a codec reproduces the exact discrete buffer supplied to its encoder. A lossless output codec does not make the entire source-to-output operation bit-exact: source decode, chroma reconstruction, gamut conversion, transfer conversion and quantization may occur first.

## Input reconstruction

| Input                            | Path to the canonical HDR master                                                                  | Characterization                                                                          |
| -------------------------------- | ------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------- |
| Ultra HDR JPEG                   | SDR base plus ISO mono/RGB gain map reconstruction                                                | Preserves HDR intent within JPEG, map resolution and metadata precision; not lossless     |
| Apple HDR HEIC/JPEG              | Apple auxiliary or MPF gain map and metadata reconstructed in the correct base gamut              | High-precision reconstruction, not a bit-exact recovery of an unavailable original master |
| Gain-map AVIF                    | `tmap` item graph plus ISO metadata                                                               | High-precision metadata-defined reconstruction; source encoding limits remain             |
| Gain-map JPEG XL (`jhgm`)        | SDR base plus `jhgm` gain-map codestream and ISO metadata; this is not the AVIF `tmap` item graph | Metadata-defined HDR reconstruction; base/map codec loss and resampling remain            |
| Gain-map TIFF                    | TIFF SubIFD plus ISO metadata                                                                     | High-precision metadata-defined reconstruction; source encoding limits remain             |
| Direct PQ HEIF/AVIF/PNG/TIFF/JXL | ST 2084 to absolute linear light; optional linear gamut conversion                                | Mathematically defined and high-fidelity; source codec losses cannot be recovered         |
| Direct HLG HEIF/PNG/AVIF/JXL     | inverse HLG OETF and BT.2100 OOTF using the signaled or 1000-nit reference model                  | High-fidelity for the same display model, not code-value lossless                         |
| FP16 scRGB JXR                   | linear scRGB at 80 nit reference white to the canonical gamut                                     | Preserves extended and negative values to FP16 precision                                  |

An ordinary SDR file without gain-map, PQ or HLG data is rejected as an HDR source rather than silently promoted.

## Output characterization

| Output                   | Fidelity                                                                                               | Lossless scope                                                                           |
| ------------------------ | ------------------------------------------------------------------------------------------------------ | ---------------------------------------------------------------------------------------- |
| Ultra HDR JPEG           | Display-adaptive HDR reconstruction; map scale, channel choice and JPEG compression are approximations | Not lossless                                                                             |
| RGB16 PQ/HLG PNG         | High precision within the selected gamut and transfer                                                  | Deflate is exact for the target RGB16 buffer                                             |
| RGB16 PQ TIFF            | High precision with HDR-aware CICP ICC signaling and an application-compatible large-strip layout      | Deflate and uncompressed modes are both exact for the target RGB16 buffer                |
| RGB16 PQ/HLG JPEG XL     | High precision; optional lossy modes affect it                                                         | Lossless mode is exact for the target RGB16 buffer                                       |
| RGB ISO gain-map JPEG XL | Display-adaptive HDR with an SDR base and half-resolution RGB map by default                           | Not lossless; RGB is required because the mono path is not offered as a validated output |
| FP16 scRGB JPEG XR       | High-fidelity linear edit/master representation within FP16 range                                      | Lossless mode is exact for the target FP16 representation                                |
| 10-bit PQ/HLG AVIF       | Compact HDR with 10-bit quantization and AV1 compression                                               | Not lossless                                                                             |
| RGB ISO gain-map AVIF    | Display-adaptive HDR with an SDR base and half-resolution RGB map by default                           | Not lossless; RGB is required because the mono path is not offered as a validated output |
| packed RGB10 JPEG XR     | Quantized and experimentally interpreted by applications                                               | May preserve the packed target buffer; the full conversion is not lossless               |

Each encoder is followed by a format-appropriate readback check. Verification covers structure, color signaling, dimensions, numerical range and, where applicable, exact quality with the encoder input buffer.
