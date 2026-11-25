#pragma once

#include "canonical/decoded_image.h"

namespace hdrbridge::detail {

SourceInfo inspect_heif_input(const std::filesystem::path& path);

DecodedImage decode_direct_hdr(const std::filesystem::path& path,
                               const ProgressCallback& progress,
                               std::atomic_bool* cancel);

}  // namespace hdrbridge::detail
