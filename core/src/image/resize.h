#pragma once

#include "canonical/decoded_image.h"

namespace hdrbridge::detail {

DecodedImage resize_linear_hdr_to_fit(const DecodedImage& source,
                                      uint32_t max_dimension);

std::vector<uint16_t> resize_preview_scrgb(const DecodedImage& source,
                                          uint32_t width, uint32_t height,
                                          uint32_t max_dimension,
                                          const ProgressCallback& progress,
                                          std::atomic_bool* cancel);

}  // namespace hdrbridge::detail
