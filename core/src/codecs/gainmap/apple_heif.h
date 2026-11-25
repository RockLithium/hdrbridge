#pragma once

#include "codecs/gainmap/apple_probe.h"
#include "codecs/gainmap/reconstructed_hdr.h"
#include "codecs/gainmap/raster.h"

namespace hdrbridge::gainmap {

AppleAuxiliaryProbe probe_apple_heif(const std::filesystem::path& path);
SourceInfo inspect_apple_heif_gainmap(const std::filesystem::path& path);
ReconstructedHdr reconstruct_apple_heif_gainmap(
    const std::filesystem::path& path,
    std::atomic_bool* cancel = nullptr);
GainMapRaster extract_heif_gainmap(const std::filesystem::path& path,
                                   std::atomic_bool* cancel);

}  // namespace hdrbridge::gainmap
