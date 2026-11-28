# Source representation and metadata

Inspector data is read from the source container. The canonical HDR working raster is never substituted for the encoded source representation.

## Input readers

| Input | Representation and metadata read |
|---|---|
| HEIF/HIF direct HDR | HEVC bit depth/chroma, native NCLX CICP/range and embedded ICC as separate signals, resolved color, Exif/XMP items, `irot`/`imir` and Exif Orientation |
| Panasonic HSP | HSP container, HEVC bit depth/chroma, HEVC SPS VUI or HSP BT.2020 HLG default fallback, Exif/GPS, XMP and IFD0 Orientation |
| HEIF/HEIC gain map | SDR base, auxiliary and `tmap` item relationships, Exif/XMP, base ICC/NCLX, `tmap` color profile, `HAGC` or legacy `lumi` reference white and ISO gain-map metadata |
| AVIF gain map | Base, map and `tmap` item relationships, Exif/XMP, base and alternate color signaling and ISO gain-map metadata |
| JPEG / Ultra HDR / Adobe XMP / Apple JPEG | base SOF and MPF/gain-map JPEG independently; APP1 Exif/XMP, APP2 ICC, Orientation, Adobe gain-map XMP and ISO gain-map metadata |
| JPEG XL | integer representation, native structured color or actual embedded ICC, Exif and `xml ` boxes for direct and gain-map inputs, codestream Orientation; native color is labeled `CICP equivalent` |
| PNG | RGB16, literal cICP and iCCP as separate signals, resolved color, eXIf and XMP iTXt |
| TIFF | sample format/bit depth, ICC interpretation, Orientation, XMP and camera Exif from the primary IFD and Exif IFD |
| Direct HDR AVIF | AV1 bit depth/chroma, native NCLX CICP/range and embedded ICC as separate signals, resolved color and Exif/XMP items |
| JPEG XR | exact WIC pixel-format GUID, color-context and metadata-query status |

Exif, XMP, ICC and Orientation report `Present`, `Absent`, `Unsupported` or `Read error`. JPEG range is `Unknown` when there is no authoritative signal.

Direct HDR classification does not require native CICP/NCLX. The resolver may obtain PQ or HLG primaries and transfer from a valid embedded ICC, including its `cicp` tag. Inspector preserves `Native`, `ICC`, `Native + ICC`, `Conflict` or `Unknown` as the signaling source instead of replacing the source representation with the canonical master description.

## Output preservation

| Output                      | Preserved or generated                                                            | Current boundary                                                                        |
| --------------------------- | --------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------- |
| Ultra HDR JPEG              | normalized Exif, SDR-base ICC and ISO gain-map metadata                           | public libultrahdr has no arbitrary source-XMP setter; stale gain-map XMP is not copied |
| HDR PNG                     | normalized eXIf, XMP, cICP and (for PQ) target ICC                                | HLG outputs omit ICC; source color signaling is replaced by the selected output space   |
| Direct HDR TIFF             | XMP, HDR-aware CICP ICC, Orientation 1 and Software tag                           | arbitrary source EXIF IFD cloning is not implemented                                    |
| JPEG XL                     | normalized EXIF/XMP boxes and native PQ/HLG structured color (direct mode)        | direct output uses structured color without ICC; gain-map output currently omits EXIF/XMP boxes |
| AVIF                        | normalized Exif/XMP items plus target NCLX                                        | conflicting source ICC/color signaling is not copied                                    |
| FP16 scRGB JPEG XR          | WIC pixel format and codec properties                                             | current WIC encoder does not clone Exif/XMP/ICC payloads                                |
| Photo Frame UHDR JPEG | generated UHDR gain-map metadata; captions use source EXIF and fill missing camera fields from XMP | source EXIF/XMP are not copied; Lossless, EXIF and XMP are fixed off |
| Photo Frame HDR PNG   | cICP and (for PQ) target ICC; captions use source EXIF and fill missing camera fields from XMP      | source EXIF/XMP are not copied; HLG omits ICC; EXIF/XMP are fixed off and Lossless is fixed on |
| Photo Frame HDR AVIF  | target NCLX; captions use source EXIF and fill missing camera fields from XMP                      | source EXIF/XMP are not copied; EXIF/XMP and Lossless are fixed off                            |

Apple, gain-map-container and Ultra HDR relationship metadata is input reconstruction data. It is removed after reconstruction when it would misdescribe a new direct-HDR output. Camera Exif is retained where the destination encoder supports it.

Converted outputs omit vendor MakerNote (`0x927C`) entries. Other supported camera, lens, exposure, date, GPS and copyright fields are retained, with Orientation normalized to 1. This output policy is shared by Windows, Web and Android; it does not change source files or Inspector metadata. Exif that cannot be parsed safely is omitted rather than copied unchecked.
