#include "metadata/source_metadata.h"

namespace hdrbridge::detail {

const char* metadata_status(bool present) {
  return present ? "present" : "absent";
}

void finalize_source_metadata(SourceInfo& info) {
  const bool wic_jxr = info.format == "JPEG XR";
  if (!wic_jxr) {
    if (info.exif_status == "unsupported") info.exif_status = metadata_status(info.exif_present);
    if (info.xmp_status == "unsupported") info.xmp_status = metadata_status(info.xmp_present);
    if (info.icc_status == "unsupported") info.icc_status = metadata_status(info.icc_present);
    if (info.orientation_status == "unsupported") {
      info.orientation_status = info.original_orientation != 1 ? "present" : "absent";
    }
  }
  if (info.pixel_format.empty()) info.pixel_format = info.chroma;
}

}  // namespace hdrbridge::detail
