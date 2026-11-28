#pragma once

#include "codecs/gainmap/gainmap_probe.h"
#include "canonical/decoded_image.h"
#include "codecs/gainmap/raster.h"

namespace hdrbridge::gainmap {

GainMapAuxiliaryProbe probe_apple_jpeg(const std::filesystem::path& path);
SourceInfo inspect_jpeg_asset(const std::filesystem::path& path);
hdrbridge::detail::DecodedImage decode_jpeg_asset(
    const std::filesystem::path& path,
    const hdrbridge::ProgressCallback& progress = {},
    std::atomic_bool* cancel = nullptr);

GainMapRaster extract_jpeg_gainmap(const std::filesystem::path& path,
                                   std::atomic_bool* cancel,
                                   bool decode_pixels);

}  // namespace hdrbridge::gainmap
