#pragma once

#include "codecs/gainmap/reconstructed_hdr.h"
#include "codecs/gainmap/raster.h"

namespace hdrbridge::gainmap {

bool is_adobe_gainmap_tiff(const std::filesystem::path& path);
SourceInfo inspect_adobe_gainmap_tiff(const std::filesystem::path& path);
ReconstructedHdr reconstruct_adobe_gainmap_tiff(
    const std::filesystem::path& path,
    std::atomic_bool* cancel = nullptr);

GainMapRaster extract_tiff_gainmap(const std::filesystem::path& path,
                                   std::atomic_bool* cancel);

}  // namespace hdrbridge::gainmap
