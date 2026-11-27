#pragma once

#include "canonical/decoded_image.h"
#include <cstdint>
#include <vector>

namespace hdrbridge::detail {

SourceInfo inspect_hsp_input(const std::filesystem::path& path);

DecodedImage decode_hsp_input(const std::filesystem::path& path,
                              const ProgressCallback& progress,
                              std::atomic_bool* cancel);

// Exposed for unit tests exercising malformed inputs, SPS VUI/cropping, and
// EXIF/XMP/orientation preservation directly from memory.
std::vector<uint8_t> build_heif_from_hsp_bytes(const std::vector<uint8_t>& hsp_bytes);
SourceInfo inspect_hsp_bytes(const std::filesystem::path& path, const std::vector<uint8_t>& hsp_bytes);

}  // namespace hdrbridge::detail
