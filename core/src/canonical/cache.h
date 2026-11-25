#pragma once

#include "canonical/decoded_image.h"
#include <memory>

namespace hdrbridge::detail {

std::shared_ptr<const DecodedImage> decode_input(
    const std::filesystem::path& path,
    const ProgressCallback& progress,
    std::atomic_bool* cancel,
    TimingDiagnostics& timings);

}  // namespace hdrbridge::detail
