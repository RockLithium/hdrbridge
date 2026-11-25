#pragma once

#include "canonical/decoded_image.h"

namespace hdrbridge::detail {

void encode_jxr(const std::filesystem::path& path, uint32_t width, uint32_t height,
                const std::vector<uint16_t>& rgba_half, const ConversionOptions& options);

Verification decode_jxr(const std::filesystem::path& path, const std::vector<uint16_t>* expected);

DecodedImage decode_jxr_input(const std::filesystem::path& path);

}  // namespace hdrbridge::detail
