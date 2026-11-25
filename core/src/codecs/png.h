#pragma once

#include "canonical/decoded_image.h"
#include "codecs/gainmap/raster.h"
#include <array>

namespace hdrbridge::detail {

std::vector<uint8_t> encode_png(const DecodedImage& decoded, const ConversionOptions& options,
                                const std::vector<uint16_t>& pixels);

std::vector<uint8_t> encode_gainmap_png(const gainmap::GainMapRaster& raster,
                                        uint32_t);

bool find_png_cicp(const std::vector<uint8_t>& bytes, std::array<uint8_t, 4>& cicp);

Verification decode_png(const std::vector<uint8_t>& bytes,
                        const std::vector<uint16_t>* expected = nullptr,
                        bool expect_icc = true,
                        bool expect_icc_only = false,
                        const std::vector<uint16_t>* expected_alpha = nullptr);

DecodedImage decode_png_input(const std::filesystem::path& path);

void verify_gainmap_png(const std::vector<uint8_t>& actual,
                        const gainmap::GainMapRaster& raster, size_t channels);

}  // namespace hdrbridge::detail
