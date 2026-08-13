# HDR Bridge final compatibility matrix

Updated: 2026-08-13. This table records observed application behavior only.
Standards conformance, successful local decoding, or nominal HDR signaling is not treated as application compatibility. Untested combinations remain `NT`.

## Status

- **AUTO HDR**: opens/imports and presents correctly as HDR without a per-file override.
- **CONFIG HDR**: correct HDR after enabling a tested application preference or import route.
- **MANUAL HDR**: correct HDR after manually assigning the intended input color space.
- **OPEN / WRONG HDR**: opens, but brightness or HDR interpretation is materially wrong.
- **SDR**: only the SDR/base rendition is used, or a usable SDR interpretation is shown.
- **REJECT**: cannot open/import in the tested route.
- **NT**: not tested or not characterized sufficiently.
- **INPUT OK**: HDR Bridge reconstructs/decodes the source into its canonical HDR master.

## HDR compatibility results

The Photoshop, Camera Raw, Premiere Pro, and DaVinci Resolve results were tested on
Windows; equivalent behavior was also observed on macOS where checked. Chrome results below specifically refer to **Chrome on Windows**.

| HDR representation / asset   | Windows Photos                 | Chrome (Windows) | Photoshop 2026                                  | Camera Raw 18.5                                                        | DaVinci Resolve 20                                    | Premiere Pro 2026                                    | Xiaomi Gallery (HyperOS 3)               | OPPO Gallery (ColorOS 16)                | Apple Photos | Final Cut Pro | HDR Bridge |
| ---------------------------- | ------------------------------ | ---------------- | ----------------------------------------------- | ---------------------------------------------------------------------- | ----------------------------------------------------- | ---------------------------------------------------- | ---------------------------------------- | ---------------------------------------- | ------------ | ------------- | ---------- |
| **Gain map**                 |                                |                  |                                                 |                                                                        |                                                       |                                                      |                                          |                                          |              |               |            |
| JPEG / Ultra HDR             | SDR                            | AUTO HDR         | SDR                                             | AUTO HDR                                                               | SDR                                                   | SDR                                                  | AUTO HDR                                 | AUTO HDR                                 | AUTO HDR     | SDR           | INPUT OK   |
| AVIF gain map                | SDR                            | AUTO HDR         | REJECT                                          | AUTO HDR                                                               | REJECT                                                | REJECT                                               | REJECT                                   | AUTO HDR                                 | AUTO HDR     | SDR           | INPUT OK   |
| JPEG XL ISO gain map         | SDR                            | REJECT           | REJECT                                          | AUTO HDR                                                               | REJECT                                                | REJECT                                               | REJECT                                   | REJECT                                   | SDR          | SDR           | INPUT OK   |
| Apple HDR HEIF/HEIC gain map | SDR                            | REJECT           | SDR                                             | REJECT                                                                 | SDR                                                   | SDR                                                  | SDR                                      | SDR                                      | AUTO HDR     | SDR           | INPUT OK   |
| TIFF gain map                | SDR                            | REJECT           | SDR                                             | AUTO HDR                                                               | SDR                                                   | SDR                                                  | REJECT                                   | REJECT                                   | SDR          | SDR           | INPUT OK   |
| **PQ / ST 2084**             |                                |                  |                                                 |                                                                        |                                                       |                                                      |                                          |                                          |              |               |            |
| Direct AVIF 10-bit           | AUTO HDR                       | AUTO HDR         | REJECT                                          | AUTO HDR                                                               | REJECT                                                | REJECT                                               | REJECT                                   | REJECT                                   | AUTO HDR     | AUTO HDR      | INPUT OK   |
| Direct JPEG XL RGB16         | AUTO HDR                       | REJECT           | REJECT                                          | AUTO HDR                                                               | REJECT                                                | REJECT                                               | REJECT                                   | REJECT                                   | AUTO HDR     | AUTO HDR      | INPUT OK   |
| Direct HIF/HEIF 10-bit       | AUTO HDR                       | REJECT           | REJECT — renaming to `.heic` opens as SDR       | REJECT                                                                 | AUTO HDR                                              | REJECT — renaming to `.heic` opens as SDR            | REJECT                                   | REJECT                                   | AUTO HDR     | AUTO HDR      | INPUT OK   |
| HDR PNG RGB16                | OPEN / WRONG HDR — SDR/gray    | AUTO HDR         | AUTO HDR                                        | AUTO HDR                                                               | MANUAL HDR — defaults to Rec.2020; assign Rec.2100 PQ | AUTO HDR                                             | OPEN / WRONG HDR — SDR/severely underexposed | OPEN / WRONG HDR — SDR/severely underexposed | AUTO HDR     | AUTO HDR      | INPUT OK   |
| Direct HDR TIFF RGB16        | OPEN / WRONG HDR — SDR/gray    | REJECT           | AUTO HDR                                        | AUTO HDR                                                               | MANUAL HDR — defaults to Rec.709; assign Rec.2100 PQ  | MANUAL HDR — defaults to Rec.709; assign Rec.2100 PQ | REJECT                                   | REJECT                                   | AUTO HDR     | AUTO HDR      | INPUT OK   |
| **HLG / BT.2100**            |                                |                  |                                                 |                                                                        |                                                       |                                                      |                                          |                                          |              |               |            |
| Direct AVIF 10-bit           | OPEN / WRONG HDR — overexposed | AUTO HDR         | REJECT                                          | AUTO HDR                                                               | REJECT                                                | REJECT                                               | REJECT                                   | REJECT                                   | AUTO HDR     | AUTO HDR      | INPUT OK   |
| Direct JPEG XL RGB16         | SDR                            | REJECT           | REJECT                                          | AUTO HDR                                                               | REJECT                                                | REJECT                                               | REJECT                                   | REJECT                                   | AUTO HDR     | AUTO HDR      | INPUT OK   |
| Direct HIF/HEIF 10-bit       | OPEN / WRONG HDR — overexposed | REJECT           | REJECT — renaming to `.heic` opens as SDR       | REJECT                                                                 | MANUAL HDR — defaults to Rec.2020; assign Rec.2100 PQ | REJECT — renaming to `.heic` opens as SDR            | REJECT                                   | REJECT                                   | AUTO HDR     | AUTO HDR      | INPUT OK   |
| HDR PNG RGB16                | SDR                            | AUTO HDR         | AUTO HDR                                        | AUTO HDR                                                               | AUTO HDR                                              | AUTO HDR                                             | SDR                                      | SDR                                      | AUTO HDR     | AUTO HDR      | INPUT OK   |
| **scRGB**                    |                                |                  |                                                 |                                                                        |                                                       |                                                      |                                          |                                          |              |               |            |
| FP16 scRGB JPEG XR           | AUTO HDR                       | REJECT           | CONFIG HDR — Microsoft JPEG XR plug-in required | CONFIG HDR — Microsoft JPEG XR plug-in plus Photoshop/Camera Raw route | REJECT                                                | REJECT                                               | REJECT                                   | REJECT                                   | REJECT       | REJECT        | INPUT OK   |

## Product conclusions based on these tests

The desktop output order remains purpose-based:

1. Share / Web — Ultra HDR JPEG
2. Interchange — HDR PNG
3. Interchange — HDR TIFF
4. Master — JPEG XL
5. Windows — FP16 scRGB JPEG XR
6. Compact — AVIF
7. Experimental — JPEG XR RGB10

Rec.2020 is the Video/NLE default. Display P3 is a normal color-space option for both
PQ and HLG where implemented. FP16 scRGB JXR remains linear scRGB and Ultra HDR
remains SDR base plus gain map; neither uses the PQ/HLG selector.

No listed format is universal. Ultra HDR is the strongest share/web route; HDR PNG is the strongest broadly tested direct-HDR interchange route; TIFF is a strong professional edit/interchange format when encoded with compatible HDR signaling; JXL is a modern edit/master format with direct-HDR and gain-map representations; FP16 scRGB JXR remains particularly useful in Windows/Adobe workflows; AVIF provides compact HDR delivery.
