#pragma once

#include "canonical/decoded_image.h"

namespace hdrbridge::detail {

std::vector<uint16_t> pq2020_to_scrgb_half(const DecodedImage& decoded,
                                            const ProgressCallback& progress,
                                            std::atomic_bool* cancel);

}  // namespace hdrbridge::detail
