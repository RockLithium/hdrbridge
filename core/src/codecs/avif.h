#pragma once

#include "canonical/decoded_image.h"

namespace hdrbridge::detail {

void encode_avif(const std::filesystem::path& path, const DecodedImage& decoded,
                 const ConversionOptions& options, const std::vector<uint16_t>& pixels);

void encode_gainmap_avif(const std::filesystem::path& path,
                         const DecodedImage& decoded,
                         const ConversionOptions& options);

Verification decode_avif(const std::filesystem::path& path,
                         const std::vector<uint16_t>* expected = nullptr,
                         bool expect_icc_only = false);

Verification verify_gainmap_avif(const std::filesystem::path& path, const DecodedImage& decoded,
                                std::atomic_bool* cancel);

}  // namespace hdrbridge::detail
