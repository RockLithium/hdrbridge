# Conversion fidelity and lossless encoding

HDR fidelity and lossless compression are different claims.

**Fidelity** means preserving intended HDR luminance, dynamic range, hue and gamut relationships to the precision and gamut of the destination.

**Lossless** means a codec reproduces the exact discrete buffer supplied to its encoder. A lossless output codec does not make the entire source-to-output operation bit-exact: source decode, chroma reconstruction, gamut conversion, transfer conversion and quantization may occur first.

## Input reconstruction

Accepted HDR inputs become an oriented Rec.2020/PQ RGB16 working image. Linear-light values are used during reconstruction and color conversion, then encoded to PQ16. Those conversions can introduce quantization even when the source and destination codecs are lossless.

| Input                            | Path to the working HDR image                                                                     | Characterization                                                                          |
| -------------------------------- | ------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------- |
| Ultra HDR / Adobe XMP JPEG       | SDR base plus ISO or Adobe hdrgm XMP gain-map reconstruction                                      | Preserves HDR intent within JPEG, map resolution and metadata precision; not lossless     |
| Apple Adaptive HDR HEIC/JPEG | Apple auxiliary or MPF gain map reconstructed using its color and gain-map metadata | High-precision reconstruction within source encoding limits |
| ISO gain-map HEIF/HEIC (`tmap`) | ISO gain map reconstructed in its signaled base or alternate gamut | High-precision reconstruction within source encoding limits |
| Gain-map AVIF                    | `tmap` item graph plus ISO metadata                                                               | High-precision metadata-defined reconstruction; source encoding limits remain             |
| Gain-map JPEG XL (`jhgm`)        | SDR base plus `jhgm` gain-map box and ISO metadata                                                | Metadata-defined HDR reconstruction; base/map codec loss and resampling remain            |
| Gain-map TIFF                    | TIFF SubIFD plus ISO metadata                                                                     | High-precision metadata-defined reconstruction; source encoding limits remain             |
| Direct PQ HEIF/AVIF/PNG/TIFF/JXL | Rec.2020/PQ samples retained where possible; other gamuts transformed in linear light to PQ16   | Mathematically defined and high-fidelity; source codec losses cannot be recovered         |
| Direct HLG HEIF/HSP/PNG/AVIF/JXL | inverse HLG OETF and BT.2100 OOTF, then Rec.2020/PQ RGB16 using the reference display model       | High-fidelity for the same display model, not code-value lossless                         |
| FP16 scRGB JXR                   | linear scRGB at 80 nit reference white, converted to Rec.2020/PQ RGB16                            | Negative or out-of-range Rec.2020 values are clipped; source FP16 is not retained         |

An ordinary SDR file without gain-map, PQ or HLG data is rejected as an HDR source rather than silently promoted.

## Output characterization

| Output                      | Fidelity                                                                                                       | Lossless scope                                                                                                                                                      |
| --------------------------- | -------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Ultra HDR JPEG              | Display-adaptive HDR reconstruction; map scale, channel choice and JPEG compression are approximations         | Not lossless                                                                                                                                                        |
| RGB16 PQ/HLG PNG            | High precision within the selected gamut and transfer                                                          | Deflate is exact for the target RGB16 buffer                                                                                                                        |
| RGB16 PQ TIFF               | High precision with HDR-aware CICP ICC signaling and an application-compatible large-strip layout              | Deflate and uncompressed modes are both exact for the target RGB16 buffer                                                                                           |
| RGB16 PQ/HLG JPEG XL        | High precision; optional lossy modes affect it                                                                 | Lossless mode is exact for the target RGB16 buffer                                                                                                                  |
| RGB ISO gain-map JPEG XL    | Display-adaptive HDR with an SDR base and full-resolution RGB map by default                                   | Not end-to-end lossless                                                                                                                                             |
| FP16 scRGB JPEG XR          | High-fidelity linear representation of the PQ16 working image within FP16 range                                | Lossless mode is exact for the target FP16 representation                                                                                                           |
| 10-bit PQ/HLG AVIF          | Compact HDR with 10-bit quantization and AV1 compression                                                       | Not lossless                                                                                                                                                        |
| RGB ISO gain-map AVIF       | Display-adaptive HDR with an SDR base and full-resolution RGB map by default                                   | Not lossless                                                                                                                                                        |
| Gain-map extraction         | Exposes the encoded map rather than reconstructing an HDR image                                                | Original JXL is byte-exact; original JPEG preserves compressed scan data while removing parent metadata; PNG/TIFF preserve decoded samples; JPEG re-encode is lossy |
| Photo Frame UHDR JPEG | Opaque photograph samples are retained before encoding; the border and rounded edges are newly composed | Not lossless; JPEG quality and gain-map encoding are lossy                                                                                                          |
| Photo Frame HDR PNG   | Opaque photograph samples remain in the framed RGB16 raster within the selected gamut and transfer     | Deflate is exact for the framed RGB16 buffer                                                                                                                       |
| Photo Frame HDR AVIF  | Opaque photograph samples remain in the framed RGB16 raster before 10-bit 4:4:4 AV1 encoding           | Not lossless; AV1 compression and 10-bit quantization are lossy                                                                                                    |

Each encoder is followed by a format-appropriate readback check, except for portable jxrlib JPEG XR builds on Web and Android. Verification covers structure, color signaling, dimensions, numerical range and, where applicable, exact equality with the encoder input buffer.
