#pragma once

#include "canonical/decoded_image.h"
#include "image/hdr_stats.h"

namespace hdrbridge::detail {

std::vector<uint8_t> encode_ultrahdr(const DecodedImage& decoded, const ConversionOptions& options,
                                     const HdrStats& stats);

Verification decode_ultrahdr(const std::vector<uint8_t>& bytes,
                             const DecodedImage* expected = nullptr,
                             const HdrStats* expected_stats = nullptr,
                             const ConversionOptions* expected_options = nullptr);

SourceInfo inspect_jpeg(const std::filesystem::path& path);

DecodedImage decode_ultrahdr_input(const std::filesystem::path& path);

}  // namespace hdrbridge::detail
