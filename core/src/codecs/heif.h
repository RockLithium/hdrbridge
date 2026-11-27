#pragma once

#include "canonical/decoded_image.h"

namespace hdrbridge::detail {

SourceInfo inspect_heif_input(const std::filesystem::path& path);

SourceInfo inspect_heif_bytes(const std::filesystem::path& path, std::vector<uint8_t> bytes);

DecodedImage decode_direct_hdr(const std::filesystem::path& path,
                               const ProgressCallback& progress,
                               std::atomic_bool* cancel);

DecodedImage decode_direct_hdr_bytes(const std::filesystem::path& path,
                                     std::vector<uint8_t> bytes,
                                     const ProgressCallback& progress,
                                     std::atomic_bool* cancel);

}  // namespace hdrbridge::detail
