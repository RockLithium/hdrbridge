#pragma once

#include "color/hdr_transfer.h"
#include <array>

namespace hdrbridge::detail {

inline double inverse_pq(double encoded) {
  return transfer::pq_to_nits(encoded);
}

inline double forward_pq(double nits) {
  return transfer::nits_to_pq(nits);
}

const std::array<double, 65536>& pq_nits_lut();

}  // namespace hdrbridge::detail
