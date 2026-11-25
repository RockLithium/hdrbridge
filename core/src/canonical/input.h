#pragma once

#include "canonical/decoded_image.h"

namespace hdrbridge::detail {

DecodedImage decode_input_uncached(const std::filesystem::path& path,
                                   const ProgressCallback& progress,
                                   std::atomic_bool* cancel);

SourceInfo inspect_any(const std::filesystem::path& path);

}  // namespace hdrbridge::detail
