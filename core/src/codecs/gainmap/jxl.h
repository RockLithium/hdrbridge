#pragma once

#include "codecs/gainmap/reconstructed_hdr.h"
#include "codecs/gainmap/raster.h"

namespace hdrbridge::gainmap {

bool is_iso_gainmap_jxl(const std::filesystem::path& path);
SourceInfo inspect_iso_gainmap_jxl(const std::filesystem::path& path);
ReconstructedHdr reconstruct_iso_gainmap_jxl(
    const std::filesystem::path& path,
    std::atomic_bool* cancel = nullptr);

GainMapRaster extract_jxl_gainmap(const std::filesystem::path& path,
                                  std::atomic_bool* cancel,
                                  bool decode_pixels);

}  // namespace hdrbridge::gainmap
