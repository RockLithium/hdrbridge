#pragma once

#include "canonical/decoded_image.h"
#include "codecs/gainmap/raster.h"

namespace hdrbridge::detail {

void encode_gainmap_tiff(const std::filesystem::path& path,
                         const gainmap::GainMapRaster& raster, uint32_t);

void encode_tiff(const std::filesystem::path& path, const DecodedImage& decoded,
                 const ConversionOptions& options, const std::vector<uint16_t>& pixels);

Verification decode_tiff(const std::filesystem::path& path,
                         const std::vector<uint16_t>* expected = nullptr);

SourceInfo inspect_tiff_metadata(const std::filesystem::path& path,
                                 std::vector<uint8_t>* exif = nullptr);

DecodedImage decode_tiff_input(const std::filesystem::path& path);

void verify_gainmap_tiff(const std::filesystem::path& path,
                        const gainmap::GainMapRaster& raster, size_t channels);

}  // namespace hdrbridge::detail
