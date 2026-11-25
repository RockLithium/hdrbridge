#pragma once

#include "canonical/decoded_image.h"

namespace hdrbridge::detail {

std::vector<uint8_t> encode_jxl(const DecodedImage& decoded, const ConversionOptions& options,
                                const std::vector<uint16_t>& pixels);

std::vector<uint8_t> encode_gainmap_jxl(const DecodedImage& decoded,
                                        const ConversionOptions& options);

Verification decode_jxl(const std::vector<uint8_t>& data, const std::vector<uint16_t>* expected);

DecodedImage decode_jxl_input(const std::filesystem::path& path);

Verification verify_gainmap_jxl(const std::filesystem::path& path, const DecodedImage& decoded,
                                const ConversionOptions& options,
                                std::atomic_bool* cancel);

}  // namespace hdrbridge::detail
