#pragma once

#include "codecs/gainmap/gainmap_probe.h"
#include "codecs/gainmap/reconstructed_hdr.h"
#include "codecs/gainmap/raster.h"

namespace hdrbridge::gainmap {

GainMapAuxiliaryProbe probe_heif_gainmap(const std::filesystem::path& path);
SourceInfo inspect_heif_gainmap(const std::filesystem::path& path);
ReconstructedHdr reconstruct_heif_gainmap(
    const std::filesystem::path& path,
    std::atomic_bool* cancel = nullptr);
GainMapRaster extract_heif_gainmap(const std::filesystem::path& path,
                                   std::atomic_bool* cancel);

}  // namespace hdrbridge::gainmap
