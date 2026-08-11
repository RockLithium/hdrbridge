# HDR Bridge final compatibility matrix

Updated: 2026-08-12. This table records observed application behavior only.
Standards conformance, successful local decoding, or nominal HDR signaling is
not treated as application compatibility. Untested combinations remain `NT`.

## Status

- **AUTO HDR**: opens/imports and presents correctly as HDR without a per-file override.
- **CONFIG HDR**: correct HDR after enabling a tested application preference or import route.
- **MANUAL HDR**: correct HDR after manually assigning the intended input color space.
- **OPEN / WRONG HDR**: opens, but brightness or HDR interpretation is materially wrong.
- **SDR**: only the SDR/base rendition is used.
- **REJECT**: cannot open/import in the tested route.
- **NT**: not tested or not characterized sufficiently.
- **INPUT OK**: HDR Bridge reconstructs/decodes the source into its canonical HDR master.

## PQ and gain-map results

| Asset / preset | Windows Photos | Chrome | Android | Apple devices | Photoshop | Adobe Camera Raw | Premiere Pro 2024 | DaVinci Resolve 20 | HDR Bridge |
|---|---|---|---|---|---|---|---|---|---|
| Ultra HDR JPEG — Faithful/Auto | SDR | AUTO HDR | AUTO HDR | AUTO HDR | SDR | SDR | SDR | SDR | INPUT OK |
| HDR PNG RGB16 Rec.2020/PQ, current `Rec.2100 PQ` ICC | OPEN / WRONG HDR — overexposed | AUTO HDR | NT | NT | OPEN / WRONG HDR — very dark | REJECT | MANUAL HDR — assign Rec.2100 PQ | AUTO HDR | INPUT OK |
| HDR PNG RGB16 P3/PQ | OPEN / WRONG HDR | AUTO HDR | NT | NT | OPEN / WRONG HDR | REJECT | MANUAL HDR | AUTO HDR | INPUT OK |
| Direct HDR TIFF RGB16 PQ | OPEN / WRONG HDR | REJECT | NT | NT | OPEN / WRONG HDR | REJECT | REJECT | MANUAL HDR | INPUT OK |
| Direct PQ AVIF 10-bit 4:4:4 | AUTO HDR | AUTO HDR | NT | OPEN / WRONG HDR — SDR/overexposed in tested Apple route | REJECT | CONFIG HDR | REJECT | REJECT | INPUT OK |
| JPEG XL RGB16 PQ | AUTO HDR | REJECT | NT | NT | REJECT | CONFIG HDR | REJECT | REJECT | INPUT OK |
| JPEG XR FP16 linear scRGB | AUTO HDR | REJECT | NT | REJECT | REJECT | CONFIG HDR | REJECT | REJECT | INPUT OK |
| JPEG XR packed RGB10 — Experimental | OPEN / WRONG HDR — gray/flat | REJECT | NT | REJECT | NT | OPEN / WRONG HDR — gray/flat | REJECT | REJECT | INPUT OK |
| Adobe gain-map AVIF / `tmap` AVIF | SDR | AUTO HDR | NT | SDR | REJECT | CONFIG HDR | REJECT | REJECT | INPUT OK |
| Adobe gain-map TIFF | SDR | REJECT | NT | NT | OPEN / WRONG HDR | CONFIG HDR | REJECT | SDR | INPUT OK |
| iPhone Apple HDR gain-map HEIC | NT | NT | NT | NT | NT | NT | NT | NT | INPUT OK |
| iPhone Apple HDR gain-map JPEG | NT | NT | NT | NT | NT | NT | NT | NT | INPUT OK |

### PNG ICC observations retained from manual testing

- Old custom-named ICC: Premiere Pro 2024 and 2025 identify it as
  `HDR Bridge Rec.2100 PQ` and require manual reassignment to Rec.2100 PQ;
  Resolve 20 identifies HDR automatically.
- No ICC, standard cICP only: Premiere Pro 2024 defaults to a gray SDR result;
  Premiere Pro 2025 can be manually assigned Rec.2100 PQ; Resolve 20 identifies
  HDR automatically.
- The production PNG now uses ICC identification `Rec.2100 PQ`, preserves cICP
  `9/16/0/1`, and does not change pixels or color mathematics.

## HLG results

Only the cells below were tested. No result is inferred for another HLG format
or application.

| HLG output | Windows Photos | Chrome | Android | Apple devices | Photoshop / ACR | Premiere Pro | DaVinci Resolve |
|---|---|---|---|---|---|---|---|
| PNG RGB16 Rec.2020/HLG | NT | AUTO HDR — same experience as PQ PNG | NT | NT | NT | OPEN / WRONG HDR — opens, not recognized correctly as HLG HDR | AUTO HDR |
| AVIF 10-bit 4:4:4 Rec.2020/HLG | OPEN / WRONG HDR — visibly overexposed | AUTO HDR — same experience as PQ AVIF | NT | NT | NT | NT | NT |
| JPEG XL RGB16 Rec.2020/HLG | OPEN / WRONG HDR — visibly underexposed | NT | NT | NT | NT | NT | NT |

## Product conclusions based on these tests

The desktop output order remains purpose-based:

1. Share — Ultra HDR JPEG
2. Video — HDR PNG
3. Edit / Master — JPEG XL
4. Edit / Master — JPEG XR FP16
5. Compact — Direct HDR AVIF
6. Advanced — Direct HDR TIFF
7. Experimental — JPEG XR RGB10

Rec.2020/PQ is the Video/NLE default. Display P3/PQ remains a normal option
where implemented. HLG output is Rec.2020/HLG only. FP16 JXR remains linear
scRGB and Ultra HDR remains SDR base plus gain map; neither uses the PQ/HLG
selector.

No listed format is universal. Ultra HDR is the strongest tested share/web
route; PQ PNG is the strongest tested NLE still route; JXL and FP16 JXR are
co-equal edit/master choices with different decoder availability; PQ AVIF is a
compact Windows/Chrome/ACR route; TIFF is an advanced fallback; packed RGB10
JXR remains experimental.
