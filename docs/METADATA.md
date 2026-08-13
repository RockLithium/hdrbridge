# Source representation and metadata

Inspector data is read from the source container. The canonical HDR working raster is never substituted for the encoded source representation.

## Input readers

| Input | Representation and metadata read |
|---|---|
| HEIF/HIF direct HDR | HEVC bit depth/chroma, NCLX CICP/range, Exif/XMP items, ICC presence, `irot`/`imir` and Exif Orientation |
| HEIC/AVIF gain map | SDR base and auxiliary/base-map-tmap graph, Exif/XMP/ICC/CICP, auxiliary type and ISO gain-map metadata |
| JPEG / Ultra HDR / Apple JPEG | base SOF and MPF/gain-map JPEG independently; APP1 Exif/XMP, APP2 ICC, Orientation and ISO gain-map metadata |
| JPEG XL | integer representation, native structured color, Exif and `xml ` boxes, codestream Orientation; color is labeled `CICP equivalent` |
| PNG | RGB16, literal cICP, eXIf, XMP iTXt and iCCP |
| TIFF | sample format/bit depth, ICC interpretation, Orientation, XMP and Exif/main camera tags |
| Direct HDR AVIF | AV1 bit depth/chroma, NCLX CICP/range, Exif/XMP items and ICC presence |
| JPEG XR | exact WIC pixel-format GUID, color-context and metadata-query status |

Exif, XMP, ICC and Orientation report `Present`, `Absent`, `Unsupported` or `Read error`. JPEG range is `Unknown` when there is no authoritative signal.

## Output preservation

| Output             | Preserved or generated                                       | Current boundary                                                                        |
| ------------------ | ------------------------------------------------------------ | --------------------------------------------------------------------------------------- |
| Ultra HDR JPEG     | normalized Exif, SDR-base ICC and ISO gain-map metadata      | public libultrahdr has no arbitrary source-XMP setter; stale gain-map XMP is not copied |
| HDR PNG            | normalized eXIf, descriptive XMP, target ICC and cICP        | source color signaling is replaced by the selected output space                         |
| Direct HDR TIFF    | XMP, HDR-aware CICP ICC, Orientation 1 and Software tag       | arbitrary source Exif IFD cloning is not implemented                                    |
| JPEG XL            | normalized Exif/XMP boxes and native PQ/HLG structured color | generated output correctly reports no ICC when structured color is used                 |
| AVIF               | normalized Exif/XMP items plus target NCLX                   | conflicting source ICC/color signaling is not copied                                    |
| FP16/RGB10 JPEG XR | WIC pixel format and codec properties                        | current WIC encoder does not clone Exif/XMP/ICC payloads                                |

Apple, gain-map-container and Ultra HDR relationship metadata is input reconstruction data. It is removed after reconstruction when it would misdescribe a new direct-HDR output. Camera Exif is retained where the destination encoder supports it.
