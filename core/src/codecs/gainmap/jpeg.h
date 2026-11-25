#pragma once

#include "codecs/gainmap/apple_probe.h"
#include "codecs/gainmap/reconstructed_hdr.h"
#include "codecs/gainmap/raster.h"

namespace hdrbridge::gainmap {

AppleAuxiliaryProbe probe_apple_jpeg(const std::filesystem::path& path);
SourceInfo inspect_apple_jpeg_gainmap(const std::filesystem::path& path);
ReconstructedHdr reconstruct_apple_jpeg_gainmap(
    const std::filesystem::path& path,
    std::atomic_bool* cancel = nullptr);

GainMapRaster extract_jpeg_gainmap(const std::filesystem::path& path,
                                   std::atomic_bool* cancel,
                                   bool decode_pixels);

}  // namespace hdrbridge::gainmap
