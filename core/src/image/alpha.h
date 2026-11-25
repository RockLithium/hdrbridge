#pragma once

#include "canonical/decoded_image.h"

namespace hdrbridge::detail {

void composite_straight_alpha_over_black(DecodedImage& decoded);

}  // namespace hdrbridge::detail
